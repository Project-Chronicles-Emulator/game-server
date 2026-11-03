#include "world/features/processing/processing_feature.hpp"

#include <utility>

namespace chronicles::processing {

std::optional<ProcessingPlacement>
ProcessingSessionContext::processing_placed_station(std::uint64_t, int) const {
  return std::nullopt;
}

ProcessingFeature::ProcessingFeature(std::shared_ptr<const ProcessingData> data)
    : data_(std::move(data)) {
}

GameplayPacket ProcessingFeature::expertise_entry_snapshot() {
  return {kWorldCharExpertiseSkillsNotification, expertise_skills_entry_snapshot()};
}

GameplayPacket ProcessingFeature::crafts_entry_snapshot(const std::set<int> &learned_crafts) {
  // Non-update snapshots replace the client's learned-recipe cache. Automatic recipes come
  // from ItemCraft.autoRegistration and must not be duplicated in this list.
  auto message = nlohmann::json::object();
  if (!learned_crafts.empty()) {
    message["craft_ids"] = learned_crafts;
  }
  return {kWorldCharItemCraftsNotification, std::move(message)};
}

bool ProcessingFeature::enabled() const noexcept {
  return static_cast<bool>(data_);
}

std::unique_ptr<ProcessingSession>
ProcessingFeature::create_session(std::uint64_t player_unit_id,
                                  std::shared_ptr<ProgressAccess> progress,
                                  ProcessingSessionContext &context, SessionEventBus &events) {
  if (!enabled()) {
    return {};
  }

  ProcessingPersistence persistence;
  persistence.load_character = [progress] {
    return progress->character();
  };
  persistence.load_progress = [progress, &context] {
    if (const auto *cached = context.processing_tutorial_progress()) {
      return *cached;
    }
    return progress->load();
  };
  persistence.commit = make_item_grant_commit(
      progress,
      ItemGrantCacheAccess{[&context] {
                             return context.processing_tutorial_progress() != nullptr;
                           },
                           [&context] {
                             return context.processing_player_stats();
                           }},
      events);

  // The context outlives its session: both belong to the same world session.
  ProcessingStationLocator placed_stations = [&context](std::uint64_t unit_id, int map_id) {
    return context.processing_placed_station(unit_id, map_id);
  };

  return std::make_unique<ProcessingSession>(data_, player_unit_id, std::move(persistence),
                                             std::move(placed_stations));
}

bool ProcessingFeature::handle(ProcessingSession *session, FrameContext &frame,
                               ProcessingSessionContext &context) {
  const auto *request = frame.request();
  if (!request || !frame.gameplay_position || !session ||
      !session->handles(request->opcode, request->message, frame.gameplay_position)) {
    return false;
  }

  frame.session_packets = session->receive(
      request->opcode, request->message, request->sequence, *frame.gameplay_position,
      context.processing_monotonic_ticks(), context.processing_other_action_busy());
  return true;
}

} // namespace chronicles::processing
