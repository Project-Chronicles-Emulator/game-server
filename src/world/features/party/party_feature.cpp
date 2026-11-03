#include "world/features/party/party_feature.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

namespace chronicles::party {
namespace {

// Party tick period and the live-vitals stat keys (current and maximum HP).
constexpr auto kPartyTickInterval = std::chrono::seconds(1);
constexpr char kHpStatKey[] = "69";
constexpr char kMaxHpStatKey[] = "18";

PartyWorldConfig load_config(ClientCatalog *catalog) {
  auto config = catalog ? load_party_world_config(*catalog) : PartyWorldConfig{};
  spdlog::info("Party service: max_members={} invite_timeout_ms={} unlock_quest_id={}",
               config.service.max_members, config.service.invite_ttl.count(),
               config.unlock_quest_id);
  return config;
}

} // namespace

PartyFeature::PartyFeature(boost::asio::io_context &io_context, ClientCatalog *catalog)
    : PartyFeature(io_context, load_config(catalog)) {
}

PartyFeature::PartyFeature(boost::asio::io_context &io_context, PartyWorldConfig config)
    : world_(std::make_shared<PartyWorld>(std::move(config))),
      strand_(boost::asio::make_strand(io_context)), timer_(strand_) {
}

std::unique_ptr<PartySession> PartyFeature::create_session(PartySessionContext &context) {
  return std::make_unique<PartySession>(*this, context);
}

const std::shared_ptr<PartyWorld> &PartyFeature::world() const noexcept {
  return world_;
}

boost::asio::any_io_executor PartyFeature::executor() const {
  return strand_;
}

void PartyFeature::set_delivery(std::function<void(const PartyUpdate &)> delivery) {
  delivery_ = std::move(delivery);
}

void PartyFeature::deliver_tick_update(const PartyUpdate &update) {
  if (delivery_) {
    delivery_(update);
    return;
  }

  const bool has_content = !update.packets.empty() || !update.unit_changes.empty();
  if (has_content && !dropped_delivery_reported_) {
    dropped_delivery_reported_ = true;
    spdlog::error("Party tick update dropped because no delivery is installed: {} packets, "
                  "{} unit changes",
                  update.packets.size(), update.unit_changes.size());
  }
}

void PartyFeature::schedule_tick() {
  std::weak_ptr<PartyFeature> weak_feature = weak_from_this();
  boost::asio::post(strand_, [weak_feature] {
    const auto feature = weak_feature.lock();
    if (!feature || feature->tick_armed_ || !feature->world_->active()) {
      return;
    }

    feature->tick_armed_ = true;
    feature->timer_.expires_after(kPartyTickInterval);
    feature->timer_.async_wait(boost::asio::bind_executor(
        feature->strand_, [weak_feature](boost::system::error_code error) {
          const auto ticking_feature = weak_feature.lock();
          if (!ticking_feature) {
            return;
          }

          ticking_feature->tick_armed_ = false;
          if (error) {
            return;
          }

          try {
            const auto update = ticking_feature->world_->tick(Clock::now());
            ticking_feature->deliver_tick_update(update);
          } catch (const std::exception &failure) {
            spdlog::error("Party tick failed: {}", failure.what());
          }
          ticking_feature->schedule_tick();
        }));
  });
}

PartySession::PartySession(PartyFeature &feature, PartySessionContext &context)
    : feature_(feature), context_(context) {
}

void PartySession::select_character(const Character &character,
                                    const std::optional<WorldProgress> &stored_progress) {
  MemberPresence identity;
  identity.char_id = character.id;
  identity.account_id = character.account_id;
  identity.world_id = character.world_id;
  identity.connection_id = context_.party_connection_id();
  identity.name = character.name;
  identity.level = character.level;
  identity.char_class = character.class_id;
  identity.channel_key =
      context_.party_has_tutorial() ? static_cast<std::uint64_t>(character.id) : 0;
  if (const auto stats = context_.party_character_stats(character)) {
    identity.max_hp = stats->value(kMaxHpStatKey, 0);
    identity.hp = stats->value(kHpStatKey, identity.max_hp);
  }
  if (stored_progress) {
    identity.party_unlocked = party_unlocked(*stored_progress, feature_.world()->unlock_quest_id());
  }

  identity_ = std::move(identity);
}

const MemberPresence *PartySession::identity() const noexcept {
  return identity_ ? &*identity_ : nullptr;
}

void PartySession::update_level(int level) {
  identity_->level = level;
}

void PartySession::update_unlock(const WorldProgress &progress) {
  identity_->party_unlocked = party_unlocked(progress, feature_.world()->unlock_quest_id());
}

void PartySession::on_tutorial_graduation(const WorldProgress &progress) {
  identity_->channel_key = 0;
  update_unlock(progress);
}

void PartySession::set_instance_channel(std::uint64_t channel_key) {
  if (!identity_ || context_.party_has_tutorial()) {
    return;
  }
  identity_->channel_key = channel_key;
}

void PartySession::set_quest_ledger(std::shared_ptr<quests::QuestIntentLedger> ledger) {
  if (identity_) {
    identity_->quest_ledger = std::move(ledger);
  }
}

MemberPresence PartySession::presence(const WorldPosition &position) const {
  auto presence = *identity_;
  presence.position = position;
  if (const auto *stats = context_.party_combat_stats()) {
    presence.max_hp = stats->value(kMaxHpStatKey, presence.max_hp);
    presence.hp = stats->value(kHpStatKey, presence.max_hp);
  }
  if (const auto *progress = context_.party_tutorial_progress()) {
    presence.party_unlocked = party_unlocked(*progress, feature_.world()->unlock_quest_id());
  }
  return presence;
}

PartyUpdate PartySession::on_enter_or_refresh(const std::optional<WorldPosition> &position,
                                              bool entered_world) {
  if (!identity_ || !position || context_.party_closed()) {
    return {};
  }

  const auto current_presence = presence(*position);
  if (attached_) {
    feature_.world()->refresh(current_presence);
    return {};
  }
  if (!entered_world) {
    return {};
  }

  attached_ = true;
  auto update = feature_.world()->attach(current_presence, context_.party_now());
  if (context_.party_closed()) {
    // An I/O close may have detached before the worker's attach completed.
    update.append(feature_.world()->detach(current_presence.char_id, current_presence.connection_id,
                                           context_.party_now()));
  }
  return update;
}

PartyUpdate PartySession::receive(const WorldGameplayRequest &request) {
  const CharacterId requester = identity_ ? identity_->char_id : 0;
  return feature_.world()->receive(requester, context_.party_connection_id(), request.opcode,
                                   request.message, request.sequence, context_.party_now());
}

bool PartySession::attached() const noexcept {
  return attached_;
}

PartyUpdate PartySession::on_leave_world() {
  return on_leave_world_staged({});
}

PartyUpdate PartySession::on_leave_world_staged(
    const std::function<void(const PartyUpdate &)> &before_commit) {
  return feature_.world()->detach_staged(identity_->char_id, context_.party_connection_id(),
                                         context_.party_now(), before_commit);
}

} // namespace chronicles::party
