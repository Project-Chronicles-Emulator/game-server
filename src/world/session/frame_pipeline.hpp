#pragma once

#include "protocol/protocol.hpp"
#include "world/core/frame_context.hpp"
#include "world/core/session_event_bus.hpp"
#include "world/features/companion/companion_session.hpp"
#include "world/features/events/raid_channel.hpp"
#include "world/features/gathering/gathering.hpp"
#include "world/features/guild/guild_world.hpp"
#include "world/features/party/party_world.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chronicles {

// Named callbacks keep feature-local capability ports separate. Only this composition
// creates subscriber lists; features cannot self-register or supply opaque ordered lists.
struct SessionEventObservers {
  // These five effects were always present. Empty callbacks fail bus validation.
  SessionEventBus::Subscriber<PlayerProtectionChanged> protection;
  SessionEventBus::Subscriber<PlayerVisibilityChanged> visibility;
  SessionEventBus::Subscriber<PlayerCombatSnapshotChanged> combat_snapshot;
  SessionEventBus::Subscriber<CompanyMembershipChanged> company_membership;
  SessionEventBus::Subscriber<PlayerProgressCommitted> progress_committed;

  // Remaining feature effects retain their existing optional registration.

  SessionEventBus::Subscriber<CharacterActivityExperienceGained> activity_experience;
  SessionEventBus::Subscriber<PlayerInventoryStatsChanged> inventory_stats;
  SessionEventBus::Subscriber<PlayerInventorySnapshotChanged> inventory_snapshot;
  SessionEventBus::Subscriber<PlayerEquipmentRevisionCommitted> equipment_revision;
  SessionEventBus::Subscriber<PlayerWorldLootProgressCommitted> world_loot_progress;
  SessionEventBus::Subscriber<PlayerProgressionSnapshotChanged> progression_snapshot;
  SessionEventBus::Subscriber<PlayerLevelChanged> player_level;
  SessionEventBus::Subscriber<PlayerLifeCommitted> player_life;
};

SessionEventBus compose_session_events(SessionEventObservers observers);

struct WorldPacketMetadata;

struct PreparedFrameInput {
  std::optional<WorldGameplayRequest> request;
  std::optional<WorldPosition> gameplay_position;
  std::optional<WorldPosition> party_position;
};

// One worker invocation owns these accumulators. Only encoded results leave that invocation;
// feature delivery aggregates retain their distinct ordering and failure boundaries.
struct SessionFrameState {
  explicit SessionFrameState(WorldPacketMetadata &packet_metadata) : metadata(packet_metadata) {
  }

  WorldPacketMetadata &metadata;
  std::optional<Frame> response;
  std::optional<WorldPosition> position_to_checkpoint;
  std::optional<nlohmann::json> movement_to_broadcast;
  std::vector<Frame> pending_notifications;
  // Tutorial replies are encoded by their owner before the shared guide observer runs.
  std::vector<GameplayPacket> guide_action_packets;
  std::optional<int> completed_guide_cut_scene;
  std::vector<GatheringStateChange> gathering_broadcasts;
  party::PartyUpdate party_update;
  raid::RaidUpdate raid_update;
  guild_world::GuildWorldUpdate guild_update;
  companion::SessionResult companion_result;
};

// The private session pipeline always supplies this concrete type to its core router.
// Core itself remains independent of the session's feature-specific accumulators.
class SessionFrameContext final : public FrameContext {
public:
  SessionFrameContext(std::uint16_t incoming_opcode, PreparedFrameInput input,
                      SessionFrameState &session_state)
      : FrameContext(incoming_opcode, std::move(input.request)), state(session_state) {
    gameplay_position = std::move(input.gameplay_position);
    party_position = std::move(input.party_position);
  }

  SessionFrameState &state;
};

struct FramePipelineOutcome {
  std::optional<Frame> response;
  std::vector<Frame> pending_notifications;
  // Monotonic deadline in the same 100 ns ticks as the session's next_gameplay_tick().
  std::optional<std::int64_t> action_deadline;
  bool rejected{};
};

// Transitional Phase 3a boundary. WorldSession implements these operations with its existing
// feature methods; the pipeline owns order, while each operation preserves its original guard.
class SessionFrameOperations {
public:
  virtual ~SessionFrameOperations() = default;

  virtual PreparedFrameInput prepare_frame(SessionFrameState &state) = 0;

  virtual void observe_movement_mode(SessionFrameContext &frame) = 0;
  virtual void observe_map_load_reset(SessionFrameContext &frame) = 0;
  virtual void observe_frame_metrics(SessionFrameContext &frame) = 0;
  virtual void observe_patrol_position(SessionFrameContext &frame) = 0;
  virtual void observe_map_load_vitals(SessionFrameContext &frame) = 0;
  virtual void observe_shared_map_mobs(SessionFrameContext &frame) = 0;
  virtual void observe_movement_vitals(SessionFrameContext &frame) = 0;
  virtual void observe_server_map_completion(SessionFrameContext &frame) = 0;
  virtual void observe_raid_arrival(SessionFrameContext &frame) = 0;
  virtual void observe_companion_own_packets(SessionFrameContext &frame) = 0;
  virtual void observe_tear_snapshot(SessionFrameContext &frame) = 0;
  virtual void observe_adventure_lifecycle(SessionFrameContext &frame) = 0;
  virtual void observe_quest_snapshot(SessionFrameContext &frame) = 0;
  virtual void observe_utility_bag_snapshot(SessionFrameContext &frame) = 0;
  virtual void observe_shared_population(SessionFrameContext &frame) = 0;
  virtual void observe_sector_sight(SessionFrameContext &frame) = 0;
  virtual void observe_npc_sight(SessionFrameContext &frame) = 0;
  virtual void observe_unhandled_interaction(SessionFrameContext &frame) = 0;
  virtual void observe_start_playing_patrol(SessionFrameContext &frame) = 0;
  virtual void observe_encumbrance(SessionFrameContext &frame) = 0;
  virtual void observe_session_encoding(SessionFrameContext &frame) = 0;
  virtual void observe_riding_vitals_reset(SessionFrameContext &frame) = 0;
  virtual void observe_final_deadline(SessionFrameContext &frame) = 0;

  virtual bool handle_shared_loot(SessionFrameContext &frame) = 0;
  virtual bool handle_shared_revival(SessionFrameContext &frame) = 0;
  virtual bool handle_party(SessionFrameContext &frame) = 0;
  virtual bool handle_guild(SessionFrameContext &frame) = 0;
  virtual bool handle_housing_request(SessionFrameContext &frame) = 0;
  virtual bool handle_housing_toggle(SessionFrameContext &frame) = 0;
  virtual bool handle_housing_departure(SessionFrameContext &frame) = 0;
  virtual bool handle_zone_channel_move(SessionFrameContext &frame) = 0;
  virtual bool handle_map_travel(SessionFrameContext &frame) = 0;
  virtual bool handle_market(SessionFrameContext &frame) = 0;
  virtual bool handle_events(SessionFrameContext &frame) = 0;
  virtual bool handle_world_chat(SessionFrameContext &frame) = 0;
  virtual bool handle_companion_trade(SessionFrameContext &frame) = 0;
  virtual bool handle_statue(SessionFrameContext &frame) = 0;
  virtual bool handle_statue_busy_skill(SessionFrameContext &frame) = 0;
  virtual bool handle_item_use(SessionFrameContext &frame) = 0;
  virtual bool handle_company(SessionFrameContext &frame) = 0;
  virtual bool handle_social_gm(SessionFrameContext &frame) = 0;
  virtual bool handle_generic_gm(SessionFrameContext &frame) = 0;
  virtual bool handle_equip(SessionFrameContext &frame) = 0;
  virtual bool handle_discard(SessionFrameContext &frame) = 0;
  virtual bool handle_polish(SessionFrameContext &frame) = 0;
  virtual bool handle_progression(SessionFrameContext &frame) = 0;
  virtual bool handle_npc_threat(SessionFrameContext &frame) = 0;
  virtual bool handle_escape(SessionFrameContext &frame) = 0;
  virtual bool handle_expression(SessionFrameContext &frame) = 0;
  virtual bool handle_merchant(SessionFrameContext &frame) = 0;
  virtual bool handle_processing(SessionFrameContext &frame) = 0;
  virtual bool handle_gathering(SessionFrameContext &frame) = 0;
  virtual bool handle_tears(SessionFrameContext &frame) = 0;
  virtual bool handle_adventure(SessionFrameContext &frame) = 0;
  virtual bool handle_utility_bag(SessionFrameContext &frame) = 0;
  virtual bool handle_world_quests(SessionFrameContext &frame) = 0;
  virtual bool handle_private_combat(SessionFrameContext &frame) = 0;
  virtual bool handle_tutorial_tail(SessionFrameContext &frame) = 0;

  virtual void post_movement_presence(SessionFrameContext &frame) = 0;
  virtual void post_companion_entry(SessionFrameContext &frame) = 0;
  virtual void post_companion_delivery(SessionFrameContext &frame) = 0;
  virtual void post_gathering_changes(SessionFrameContext &frame) = 0;
  virtual void post_player_services(SessionFrameContext &frame) = 0;
  virtual void post_party_update(SessionFrameContext &frame) = 0;
  virtual void post_raid_update(SessionFrameContext &frame) = 0;
  virtual void post_emotion_snapshot(SessionFrameContext &frame) = 0;
  virtual void post_guild_update(SessionFrameContext &frame) = 0;
  virtual void post_housing_snapshot(SessionFrameContext &frame) = 0;
  virtual void post_progress_checkpoint(SessionFrameContext &frame) = 0;

  virtual void report_frame_rejection(std::uint16_t incoming_opcode, std::string_view step,
                                      const std::exception &error) = 0;
  virtual void log_frame_result(std::uint16_t incoming_opcode, const SessionFrameState &state,
                                bool rejected) = 0;
};

class SessionFramePipeline {
public:
  explicit SessionFramePipeline(SessionFrameOperations &operations);
  ~SessionFramePipeline();

  SessionFramePipeline(const SessionFramePipeline &) = delete;
  SessionFramePipeline &operator=(const SessionFramePipeline &) = delete;
  SessionFramePipeline(SessionFramePipeline &&) = delete;
  SessionFramePipeline &operator=(SessionFramePipeline &&) = delete;

  FramePipelineOutcome run(std::uint16_t incoming_opcode, WorldPacketMetadata &metadata);

  // Step names in composition order; these are the labels used in rejection logs.
  std::vector<std::string> step_names() const;

private:
  struct Impl;
  std::unique_ptr<Impl> implementation_;
};

} // namespace chronicles
