#pragma once

#include "protocol/protocol.hpp"
#include "world/core/frame_context.hpp"
#include "world/core/instance_copies.hpp"
#include "world/core/progress_access.hpp"
#include "world/core/session_event_bus.hpp"
#include "world/features/gathering/gathering.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <span>

namespace chronicles {
class GatheringRecoveryOperation;
}

namespace chronicles::gathering {

struct GatheringPeer {
  std::optional<int> map_id;
  // InstanceCopy::serial of the peer's instance or dungeon copy; 0 on shared maps.
  std::uint64_t channel{};
  std::function<void(Frame)> post;
  // The adapter checks readiness for the captured view without taking the source
  // publication lock, because visitation already holds the codec lock.
  std::function<bool(std::uint32_t)> sector_ready;
};

class GatheringWorldContext {
public:
  virtual ~GatheringWorldContext() = default;

  virtual Frame gathering_encode(const GameplayPacket &packet) = 0;
  // The adapter holds the codec lock during visitation. Exclusion precedes weak lookup
  // and map access; each post callback retains its selected session owner.
  virtual void gathering_each_peer_locked(std::uint64_t excluded_connection,
                                          const std::function<void(GatheringPeer)> &visitor) = 0;
};

class GatheringSessionContext {
public:
  virtual ~GatheringSessionContext() = default;

  // Install a detached owner before its first write. The world rejects actor entry/admin
  // admission while this operation remains unresolved.
  virtual bool gathering_register_recovery(std::shared_ptr<GatheringRecoveryOperation>) {
    return false;
  }
  virtual void gathering_recovery_updated(std::shared_ptr<GatheringRecoveryOperation>) {
    throw std::runtime_error("Gathering recovery owner is unavailable");
  }
  virtual void gathering_force_reconnect() {
    throw std::runtime_error("Gathering reconnect is unavailable");
  }
  virtual GatheringActorIdentity gathering_actor_identity() const {
    throw std::runtime_error("Gathering actor identity is unavailable");
  }
  virtual GatheringPreparedQuest gathering_prepare_quest(
      const QuestEvents &, const WorldPosition &, const WorldProgress &, const WorldProgress &,
      const ItemTrade &) {
    throw std::runtime_error("Gathering quest planning is unavailable");
  }
  // Called on the live session strand only after a confirmed combined commit. Detached
  // retries use fresh world-entry snapshots and must never invoke this callback.
  virtual std::vector<GameplayPacket> gathering_publish_committed(
      const WorldProgress &, const WorldProgress &, const std::vector<CharacterItem> &) {
    throw std::runtime_error("Gathering progress publication is unavailable");
  }

  virtual bool gathering_has_tutorial() const = 0;
  virtual bool gathering_sector_streaming() const = 0;
  virtual bool gathering_other_action_busy() const = 0;
  virtual std::int64_t gathering_monotonic_ticks() const = 0;
  virtual bool gathering_player_dead() const = 0;
  // Resolve the player's due combat before an active interaction can award a resource.
  // Damage callbacks may cancel the interaction on this same session strand.
  virtual std::vector<GameplayPacket> gathering_advance_combat(const WorldPosition &position,
                                                              std::int64_t ticks) {
    static_cast<void>(position);
    static_cast<void>(ticks);
    return {};
  }
  virtual bool gathering_conditions_met(const nlohmann::json &, const WorldPosition &) {
    return false;
  }
  virtual std::optional<std::vector<GameplayPacket>>
  gathering_begin_stamina(const GatheringStaminaFlow &flow) = 0;
  // True means current combat spent the flow and its existing cache must be refreshed.
  virtual bool gathering_spend_stamina(std::int64_t ticks) = 0;
  virtual std::optional<GameplayPacket> gathering_cancel_stamina(std::int64_t ticks) = 0;
  virtual bool gathering_effect_available(int effect_id) const {
    static_cast<void>(effect_id);
    return false;
  }
  virtual std::vector<GameplayPacket> gathering_apply_effect(int effect_id,
                                                             const WorldPosition &position,
                                                             std::int64_t ticks) {
    static_cast<void>(effect_id);
    static_cast<void>(position);
    static_cast<void>(ticks);
    return {};
  }
  virtual std::vector<GameplayPacket> gathering_tool_wear_updates(const CharacterItem &tool) = 0;
  virtual void gathering_deliver_observers(const std::vector<GameplayPacket> &packets) = 0;
  virtual std::vector<GameplayPacket> gathering_play_cut_scene(int cut_scene_id,
                                                              const WorldPosition &position) {
    static_cast<void>(cut_scene_id);
    static_cast<void>(position);
    return {};
  }
  // A used portal doodad: move the player to `destination` after this frame's replies. Contexts
  // without map travel ignore it.
  virtual void gathering_travel(const WorldPosition &destination) {
    static_cast<void>(destination);
  }
};

struct GatheringSharedSourceStore {
  std::function<std::vector<SharedGatheringSourceRecord>(
      const std::optional<SharedGatheringSourceKey> &, std::size_t)> list;
  std::function<SharedGatheringSourceUpdateResult(
      const SharedGatheringSourceTransition &, std::int64_t, bool)> transition;
  std::function<SharedGatheringSourceUpdateResult(
      const SharedGatheringSourceTransition &)> fence;
  std::function<void(std::function<void()>)> post_worker;
};

// Detached owner of the complete local boss target set. A caller must retain this lease
// across uncertain storage outcomes; abort_before_write is legal only before the first attempt.
class BossScriptSourceLease final {
public:
  BossScriptSourceLease(std::shared_ptr<GatheringNodes> nodes, std::string event_id,
                        std::vector<BossSourceTarget> targets,
                        std::vector<std::uint64_t> tokens);

  bool mark_write_started();
  std::optional<std::vector<GatheringStateChange>> install(
      const BossScriptSettlement &settlement, std::int64_t epoch_ticks,
      std::int64_t monotonic_ticks);
  std::optional<std::vector<GatheringStateChange>> install_private(std::int64_t monotonic_ticks);
  // Release only after the actor and peer updates are enqueued in source order.
  bool acknowledge_publication();
  bool abort_before_write(std::int64_t monotonic_ticks);

private:
  std::shared_ptr<GatheringNodes> nodes_;
  const std::string event_id_;
  std::vector<BossSourceTarget> targets_;
  std::vector<GatheringNodeKey> node_keys_;
  std::vector<std::uint64_t> tokens_;
  std::mutex mutex_;
  bool finished_{};
  bool write_started_{};
  bool installed_{};
  std::vector<GatheringStateChange> pending_changes_;
};

// Shared node state, respawn scheduling and map/sector packet policy have one owner.
// Existing GatheringSession objects continue to own each player's pending action and budget.
// Shared maps use one node set; every instance or dungeon copy (InstanceCopy) owns its own,
// identified on the wire-delivery side by the copy's serial ("channel", 0 for shared maps).
class GatheringFeature final : public std::enable_shared_from_this<GatheringFeature> {
public:
  // One scripted volume doodad phase (LevelTriggerTask ChangeDoodadPhase).
  struct VolumeDoodadPhase {
    std::uint64_t unit_id{};
    int phase{};
  };

  GatheringFeature(boost::asio::io_context &io_context, std::shared_ptr<const GatheringData> data);

  bool enabled() const noexcept;
  void set_shared_source_store(GatheringSharedSourceStore store);
  void hydrate_shared_sources();
  // Reserve before the first DB attempt; retain the lease while its outcome is unknown.
  std::shared_ptr<BossScriptSourceLease> reserve_boss_script_sources(
      const BossScriptIntent &intent, InstanceCopy *copy = nullptr);
  bool has_map(int map_id) const;
  // True when the profile has a butchering reward for this monster.
  bool butcherable(int npc_cms_id) const;

  // The node set of a copy (created with it), or the shared node set when `copy` is null.
  std::shared_ptr<GatheringNodes> nodes_for(InstanceCopy *copy);

  // Current phase of a profiled spot doodad on `map_id` (gathering or quest doodad); nullopt for
  // other units. Shared state: every session on the map (or in the copy) sees the same phase.
  std::optional<int> doodad_phase(int map_id, std::uint64_t unit_id, InstanceCopy *copy);
  std::vector<GatheringStateChange> on_monster_defeated(int map_id, int npc_cms_id,
                                                         std::int64_t now_ticks,
                                                         InstanceCopy *copy = nullptr);

  // Records doodad phases set by a scripted volume in the state of the map (or copy) so later
  // arrivals see them, and returns the changes the actor and the other players there must
  // receive. With `restoring`, phases are applied only to doodads without recorded state (a
  // character's saved phases never override the live map).
  std::vector<GatheringStateChange>
  record_volume_doodad_phases(int map_id, InstanceCopy *copy,
                              const std::vector<VolumeDoodadPhase> &phases,
                              std::int64_t now_ticks, bool restoring = false);

  std::unique_ptr<GatheringSession> create_session(std::uint64_t player_unit_id,
                                                   std::shared_ptr<ProgressAccess> progress,
                                                   GatheringSessionContext &context,
                                                   SessionEventBus &events,
                                                   std::optional<std::uint32_t> random_seed = {});
  bool handle(GatheringSession *session, FrameContext &frame, GatheringSessionContext &context,
              std::vector<GatheringStateChange> &broadcasts);
  // The world supplies sectors entering its 5 x 5 sight window. SectorUpdateNoti creates the
  // client's sector record/unit and carries depleted placements and changed doodads, as on
  // official map-load/movement captures; default-phase doodads are omitted.
  std::vector<GameplayPacket> sector_snapshots(int map_id,
                                               std::span<const std::uint32_t> sector_ids,
                                               InstanceCopy *copy = nullptr);
  // Sends changes to the players on the changed map in the same copy (`channel`).
  void deliver_changes(const std::vector<GatheringStateChange> &changes, std::uint64_t channel,
                       std::uint64_t excluded_connection, GatheringWorldContext &context);

  // Installed before accepting players; the composition captures only weak world state. The
  // delivery receives each node set's respawns with its channel.
  void set_respawn_delivery(
      std::function<void(const std::vector<GatheringStateChange> &, std::uint64_t channel)>
          delivery);
  void schedule_respawn();
  // Schedules detached recovery publication without retaining a world session.
  void publish_recovered_sources(std::vector<GatheringStateChange> changes,
                                  std::uint64_t channel);

  // Hands respawned nodes to the installed delivery. Without one the changes are dropped and
  // reported once, so a composition that forgot set_respawn_delivery shows up in the log.
  // Call on the feature's strand (the respawn timer's executor): the drop flag is confined to it
  // and not synchronized.
  void deliver_respawn_changes(const std::vector<GatheringStateChange> &changes,
                               std::uint64_t channel = 0);

private:
  struct ChannelNodes {
    std::uint64_t channel{};
    std::shared_ptr<GatheringNodes> nodes;
  };

  GatheringVitals vitals(GatheringSessionContext &context, SessionEventBus &events);
  SharedGatheringSourceRecord advance_shared_source(SharedGatheringSourceRecord record,
                                                     std::int64_t now_epoch_ticks);
  void process_due_shared_sources(std::vector<SharedGatheringSourceRecord> records);

  // The shared node set and every live copy node set, pruning released copies.
  std::vector<ChannelNodes> live_nodes();

  const std::shared_ptr<const GatheringData> data_;
  const std::shared_ptr<GatheringNodes> nodes_;
  GatheringSharedSourceStore shared_source_store_;
  std::mutex copy_nodes_mutex_;
  std::vector<std::pair<std::uint64_t, std::weak_ptr<GatheringNodes>>> copy_nodes_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::steady_timer timer_;
  std::optional<std::int64_t> scheduled_respawn_tick_;
  std::function<void(const std::vector<GatheringStateChange> &, std::uint64_t)> respawn_delivery_;
  bool dropped_delivery_reported_{};
  bool shared_transition_inflight_{};
  std::optional<std::int64_t> shared_retry_not_before_;
  std::optional<std::int64_t> shared_last_retry_log_;
  std::optional<GatheringNodeKey> shared_due_cursor_;
};

} // namespace chronicles::gathering
