#include "world/features/nuia/nuia_feature.hpp"

#include "world/core/world_request_opcodes.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace chronicles::nuia {
namespace {

// The statue handler keeps its broad request guard and lazy construction.
// These recovered world opcodes do not restrict which requests reach it.
constexpr std::uint16_t kTimeSyncRequest = 20001;
constexpr std::uint16_t kCommonErrorResponse = 2;
using world_request::kActivateAdventureObjectRequest;
using world_request::kStartSkillRequest;
constexpr std::uint16_t kSkillExecutionResponse = 13;
constexpr std::uint16_t kReviveResponse = 24;
constexpr std::uint16_t kInvincibilityAttribute = 49;
constexpr std::uint16_t kInvincibilityState = 51;
constexpr std::int64_t kTicksPerSecond = 10'000'000;
constexpr unsigned kTearRequestsPerWindow = 32;

} // namespace

NuiaSession::NuiaSession(NuiaFeature &feature, ProgressAccess &progress,
                         NuiaSessionContext &context)
    : feature_(feature), progress_(progress), context_(context) {
}

bool NuiaSession::has_statue_session() const noexcept {
  return statue_ != nullptr;
}

bool NuiaSession::busy() const {
  return statue_ && statue_->busy();
}

void NuiaSession::cancel_action() {
  if (statue_) {
    statue_->cancel_action();
  }
}

std::optional<std::int64_t> NuiaSession::next_action_tick() const {
  return statue_ ? statue_->next_action_tick() : std::nullopt;
}

std::vector<GameplayPacket> NuiaSession::advance(std::int64_t now, const WorldPosition &position) {
  return statue_ ? statue_->advance(now, position) : std::vector<GameplayPacket>{};
}

NuiStatueSession &NuiaSession::statue_session() {
  if (!statue_) {
    statue_ = std::make_unique<NuiStatueSession>(
        feature_.statues_, context_.nuia_player_unit_id(),
        [this](const WorldProgress &before, const WorldProgress &after) {
          // Resolve the stat model before spending tears, so an unsupported build cannot
          // commit a permanent reward that this session is unable to calculate.
          if (context_.nuia_has_character_stats() &&
              before.nui_blessing_level != after.nui_blessing_level) {
            auto character = progress_.character();
            if (!character) {
              return false;
            }
            character->nui_blessing_level = after.nui_blessing_level;
            character->dedicated_nui_tears = after.dedicated_nui_tears;
            context_.nuia_character_stats(*character);
          }
          return context_.nuia_save_progress(before, after);
        });
  }
  return *statue_;
}

std::vector<GameplayPacket> NuiaSession::receive_statue(const WorldGameplayRequest &request,
                                                        const WorldPosition &position) {
  auto progress = progress_.load();
  const auto previous = progress;
  const auto previous_character = progress_.character();
  const bool blocked = context_.nuia_other_action_busy() || context_.nuia_processing_busy() ||
                       context_.nuia_riding() || context_.nuia_teleport_pending();
  auto packets = statue_session().receive(
      request.opcode, request.message, request.sequence, position, context_.nuia_monotonic_ticks(),
      progress, blocked, previous_character ? &*previous_character : nullptr);
  if (context_.nuia_has_tutorial() && progress != previous) {
    context_.nuia_publish_progress(progress);
  }
  if (previous.nui_blessing_level != progress.nui_blessing_level && previous_character &&
      context_.nuia_has_character_stats()) {
    const auto character = progress_.character();
    if (!character) {
      throw std::runtime_error("Nui blessing committed but character ownership was lost");
    }
    const auto before_stats = context_.nuia_character_stats(*previous_character);
    const auto after_stats = context_.nuia_character_stats(*character);
    const auto &current_stats =
        context_.nuia_has_combat() ? context_.nuia_adopt_blessing_stats(after_stats) : after_stats;
    context_.nuia_publish_inventory_snapshot_locked(*character, &current_stats);
    auto notification =
        context_.nuia_growth_notification(character->id, before_stats, after_stats, current_stats);
    notification.message["default_source"] = "SCS_Growth";
    packets.push_back(std::move(notification));
  }
  return packets;
}

std::vector<GameplayPacket> NuiaSession::receive_nuia_tear(const WorldGameplayRequest &request,
                                                           const WorldPosition &position) {
  // Bound storage work independently of valid/invalid spot IDs. This shares the session's
  // worker strand with all other inventory/progress writes.
  const auto now = context_.nuia_monotonic_ticks();
  if (now - tear_window_start_ticks_ >= kTicksPerSecond) {
    tear_window_start_ticks_ = now;
    tear_requests_in_window_ = 0;
  }
  if (++tear_requests_in_window_ > kTearRequestsPerWindow) {
    return {{kCommonErrorResponse,
             {{"ec", "BadRequest"}, {"packet_seq", {{"seq", std::to_string(request.sequence)}}}}}};
  }

  const auto current = progress_.load();
  auto result = feature_.tears_->receive(request.opcode, request.message, request.sequence,
                                         position, current);
  if (result.next) {
    if (!context_.nuia_save_progress(current, *result.next)) {
      // Closing a stale session prevents its cached revision from overwriting newer rewards.
      // No planned success or item update has been queued yet.
      throw std::runtime_error("Nuia Tear progress commit conflict");
    }
    if (context_.nuia_has_tutorial()) {
      context_.nuia_publish_progress(*result.next);
    }
  }
  return std::move(result.packets);
}

std::vector<GameplayPacket> NuiaSession::revive_at_shrine(const WorldGameplayRequest &request) {
  if (!feature_.statues_) {
    return context_.nuia_legacy_revive();
  }
  if (!context_.nuia_has_combat() || !context_.nuia_player_dead() || pending_shrine_revival_) {
    return {};
  }
  const auto current = context_.nuia_revival_position();
  if (!current || context_.nuia_server_teleport_pending()) {
    return {{kReviveResponse, {{"ec", "BadRequest"}}}};
  }
  const auto progress = progress_.load();
  const bool to_flame = request.message.value("to_nui_flame", false);
  const auto *shrine = feature_.statues_->revival_shrine(*current, progress, to_flame);
  if (!shrine) {
    return {{kReviveResponse, {{"ec", "BadRequest"}}}};
  }
  auto outcome =
      context_.nuia_request_server_teleport(shrine->spawn, context_.nuia_monotonic_ticks());
  if (!outcome) {
    return {{kReviveResponse, {{"ec", "BadRequest"}}}};
  }
  pending_shrine_revival_ = *shrine;
  return std::move(*outcome);
}

bool NuiaSession::finish_escape_revival(std::vector<GameplayPacket> &packets) {
  if (pending_shrine_revival_) {
    const auto shrine = *pending_shrine_revival_;
    pending_shrine_revival_.reset();
    std::vector<GameplayPacket> supplies;
    try {
      supplies = context_.nuia_persist_revival(shrine.spawn, shrine.revive_task_id);
    } catch (const std::exception &error) {
      spdlog::warn("Native world shrine revival refused: {}", error.what());
      return false;
    }
    auto revived = context_.nuia_revive_combat(context_.nuia_monotonic_ticks());
    // Reuse the teleport protection instance rather than announcing a second one.
    std::erase_if(revived, [](const GameplayPacket &packet) {
      return packet.opcode == kInvincibilityAttribute || packet.opcode == kInvincibilityState;
    });
    const auto insertion = packets.empty() ? packets.begin() : std::next(packets.begin());
    packets.insert(insertion, std::make_move_iterator(revived.begin()),
                   std::make_move_iterator(revived.end()));
    packets.insert(packets.end(), std::make_move_iterator(supplies.begin()),
                   std::make_move_iterator(supplies.end()));
    context_.nuia_publish_combat_snapshot();
    context_.nuia_publish_player_life();
  }
  return true;
}

bool NuiaSession::handle_statue(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &gameplay_position = frame.gameplay_position;
  auto &session_packets = frame.session_packets;

  // Statues own their animations and targeted tear offerings before generic item,
  // processing, gathering and adventure-object handlers can consume those requests.
  if (gameplay_request && gameplay_position && context_.nuia_has_handoff() && feature_.statues_ &&
      statue_session().handles(gameplay_request->opcode, gameplay_request->message,
                               *gameplay_position)) {
    auto statue_packets = receive_statue(*gameplay_request, *gameplay_position);
    session_packets.insert(session_packets.end(), std::make_move_iterator(statue_packets.begin()),
                           std::make_move_iterator(statue_packets.end()));
    return true;
  }

  return false;
}

bool NuiaSession::handle_statue_busy_skill(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;

  if (gameplay_request && statue_ && statue_->busy() &&
      gameplay_request->opcode == kStartSkillRequest) {
    const auto &start = gameplay_request->message.at("start_skill");
    session_packets.push_back({kSkillExecutionResponse,
                               {{"ec", "Skill_UsingSkill"},
                                {"skill_id", start.at("skill_id")},
                                {"instance_id", start.at("instance_id")}}});
    return true;
  }

  return false;
}

bool NuiaSession::handle_tears(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &gameplay_position = frame.gameplay_position;
  auto &session_packets = frame.session_packets;

  if (gameplay_request && gameplay_position && context_.nuia_has_handoff() && feature_.tears_ &&
      feature_.tears_->handles(gameplay_request->opcode, gameplay_request->message,
                               *gameplay_position) &&
      (!context_.nuia_has_tutorial() ||
       gameplay_request->opcode == kActivateAdventureObjectRequest)) {
    auto tear_packets = receive_nuia_tear(*gameplay_request, *gameplay_position);
    session_packets.insert(session_packets.end(), std::make_move_iterator(tear_packets.begin()),
                           std::make_move_iterator(tear_packets.end()));
    return true;
  }

  return false;
}

void NuiaSession::observe_tear_snapshot(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;

  // Restore consumed tears before the client resumes proximity collection after login.
  // TutorialSession already includes every saved adventure object in its own snapshot.
  if (gameplay_request && gameplay_request->opcode == kTimeSyncRequest &&
      (feature_.tears_ || feature_.statues_) && !context_.nuia_has_tutorial() &&
      context_.nuia_has_handoff() && !tear_snapshot_sent_) {
    session_packets.push_back(NuiaTears::snapshot(progress_.load()));
    tear_snapshot_sent_ = true;
  }
}

} // namespace chronicles::nuia
