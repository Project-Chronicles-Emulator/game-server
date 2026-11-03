#include "world/features/loot/loot_feature.hpp"

#include "common/clock_ticks.hpp"
#include "world/features/party/party_world.hpp"

#include <iterator>
#include <random>
#include <utility>

namespace chronicles::loot {
namespace {

class PartyLootAudience final : public LootAudience {
public:
  using ChannelKey = std::function<std::uint64_t()>;
  using Deliver = std::function<void(const party::PartyUpdate &)>;

  PartyLootAudience(std::shared_ptr<party::PartyWorld> party_world, std::int64_t character_id,
                    ChannelKey channel_key, Deliver deliver)
      : party_world_(std::move(party_world)), character_id_(character_id),
        channel_key_(std::move(channel_key)), deliver_(std::move(deliver)) {
  }

  std::vector<std::int64_t> party_members_near(const MonsterDefeat &defeat,
                                               double range_mm) override {
    std::vector<std::int64_t> members;
    const auto nearby = party_world_->nearby_party_members(character_id_, defeat.position,
                                                           channel_key_(), range_mm);
    for (const auto &member : nearby) {
      members.push_back(member.char_id);
    }
    return members;
  }

  void notify(std::int64_t character_id, std::vector<GameplayPacket> packets) override {
    const auto connection = party_world_->connection_of(character_id);
    if (!connection) {
      return;
    }

    party::PartyUpdate update;
    for (auto &packet : packets) {
      update.packets.push_back({*connection, packet.opcode, std::move(packet.message)});
    }
    deliver_(update);
  }

private:
  std::shared_ptr<party::PartyWorld> party_world_;
  std::int64_t character_id_{};
  ChannelKey channel_key_;
  Deliver deliver_;
};

} // namespace

LootFeature::LootFeature(std::shared_ptr<const LootData> data) : data_(std::move(data)) {
}

std::unique_ptr<LootPlayerSession> LootFeature::create_session(LootSessionContext &context) {
  return std::make_unique<LootPlayerSession>(*this, context);
}

LootPlayerSession::LootPlayerSession(LootFeature &feature, LootSessionContext &context)
    : feature_(feature), context_(context) {
}

void LootPlayerSession::create_tutorial(std::int64_t character_id, const Handoff &entry) {
  if (!feature_.data_) {
    return;
  }

  auto persistence = context_.loot_tutorial_persistence(entry);
  persistence.force_reconnect = [this] {
    context_.loot_force_reconnect();
  };
  persistence.epoch_ticks = [] {
    return unix_epoch_ticks_now();
  };
  auto audience = std::make_shared<PartyLootAudience>(context_.loot_party_world(), character_id,
                                                      context_.loot_party_channel_key(),
                                                      context_.loot_party_delivery());

  std::random_device entropy;
  session_ = std::make_unique<LootSession>(feature_.data_, character_id, std::move(persistence),
                                           std::make_unique<SeededLootRandom>(entropy()),
                                           std::move(audience), feature_.board_);
  session_->set_butchering(butcherable_);
  context_.loot_install_defeat_observer(
      [loot = session_.get(), character_id](const MonsterDefeat &defeat) {
        auto owned_defeat = defeat;
        owned_defeat.killer_character_id = character_id;
        return loot->monster_defeated(owned_defeat);
      });
}

void LootPlayerSession::attach_world_loot() {
  world_progress_ = context_.loot_stored_progress();
  if (!feature_.data_) {
    return;
  }

  LootPersistence persistence;
  persistence.force_reconnect = [this] {
    context_.loot_force_reconnect();
  };
  persistence.load_character = [this] {
    return context_.loot_stored_character();
  };
  persistence.load_progress = [this] {
    return world_progress_;
  };
  persistence.epoch_ticks = [] {
    return unix_epoch_ticks_now();
  };
  persistence.lookup_claim = [this](const std::string &receipt_id,
                                    const std::string &corpse_fingerprint) {
    auto result = context_.loot_lookup_claim(receipt_id, corpse_fingerprint);
    if (result.status == LootClaimStatus::already_claimed_by_same_character) {
      world_progress_ = context_.loot_stored_progress();
      if (const auto character = context_.loot_stored_character()) {
        context_.loot_publish_snapshot(*character, &context_.loot_current_combat_stats());
      }
    }
    return result;
  };
  persistence.commit_once = [this](const LootCommit &commit) {
    auto result = context_.loot_commit_items(commit);
    if (result.status == LootClaimStatus::committed ||
        result.status == LootClaimStatus::already_claimed_by_same_character) {
      world_progress_ = context_.loot_stored_progress();
      if (const auto character = context_.loot_stored_character()) {
        context_.loot_publish_snapshot(*character, &context_.loot_current_combat_stats());
      }
    }
    return result;
  };
  std::random_device entropy;
  session_ = std::make_unique<LootSession>(feature_.data_, context_.loot_character_id(),
                                           std::move(persistence),
                                           std::make_unique<SeededLootRandom>(entropy()));
  session_->set_butchering(butcherable_);
  context_.loot_install_defeat_observer([this](const MonsterDefeat &defeat) {
    auto owned = defeat;
    owned.killer_character_id = context_.loot_character_id();
    return session_->monster_defeated(owned);
  });
}

void LootPlayerSession::reset() {
  session_.reset();
}

bool LootPlayerSession::has_session() const noexcept {
  return session_ != nullptr;
}

bool LootPlayerSession::handles(std::uint16_t opcode) const {
  return session_ && session_->handles(opcode);
}

bool LootPlayerSession::handle_shared(FrameContext &frame) {
  const auto *request = frame.request();
  if (request && session_ && context_.loot_has_combat() && context_.loot_has_world_mobs() &&
      frame.gameplay_position && session_->handles(request->opcode)) {
    world_progress_ = context_.loot_stored_progress();
    auto packets = session_->receive(request->message, *frame.gameplay_position,
                                     context_.loot_monotonic_ticks());
    frame.session_packets.insert(frame.session_packets.end(),
                                 std::make_move_iterator(packets.begin()),
                                 std::make_move_iterator(packets.end()));
    return true;
  }

  return false;
}

std::vector<GameplayPacket> LootPlayerSession::receive(const nlohmann::json &request,
                                                       const WorldPosition &position,
                                                       std::int64_t now) {
  return session_->receive(request, position, now);
}

std::vector<GameplayPacket> LootPlayerSession::advance(std::int64_t now) {
  return session_ ? session_->advance(now) : std::vector<GameplayPacket>{};
}

std::optional<std::int64_t> LootPlayerSession::next_action_tick() const {
  return session_ ? session_->next_action_tick() : std::nullopt;
}

void LootPlayerSession::on_world_progress_committed(const WorldProgress &progress) {
  if (context_.loot_has_combat() && context_.loot_has_world_mobs()) {
    world_progress_ = progress;
  }
}

void LootPlayerSession::set_butchering(std::function<bool(int npc_cms_id)> butcherable) {
  butcherable_ = std::move(butcherable);
  if (session_) {
    session_->set_butchering(butcherable_);
  }
}

std::optional<ButcherableCorpse> LootPlayerSession::butcherable_corpse(std::uint64_t unit_id,
                                                                       std::int64_t now) const {
  if (!session_) {
    return std::nullopt;
  }
  return session_->butcherable_corpse(unit_id, now);
}

std::vector<GameplayPacket> LootPlayerSession::corpse_butchered(std::uint64_t unit_id,
                                                                std::int64_t now) {
  if (!session_) {
    return {};
  }
  return session_->corpse_butchered(unit_id, now);
}

} // namespace chronicles::loot
