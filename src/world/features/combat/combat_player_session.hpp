#pragma once

#include "world/core/frame_context.hpp"
#include "world/features/combat/combat_player_context.hpp"
#include "world/features/combat/gliding.hpp"

namespace chronicles::combat {

struct PvpPresenceUpdate {
  // Engaged even when registration failed; the generic lifecycle still sees that result.
  std::optional<bool> registration;
  std::optional<GameplayPacket> notification;
};

class CombatPlayerSession final {
public:
  CombatPlayerSession(CombatFeature &feature, CombatPlayerContext &context);

  // Borrow only in Native composition's named capabilities, never retain or pass to another
  // feature. All Combat algorithms and player state belong to this concrete owner.
  CombatSession *current_simulation() noexcept;
  const CombatSession *current_simulation() const noexcept;
  bool has_simulation() const noexcept;
  bool has_world_mobs() const;
  bool busy() const;
  bool gliding() const noexcept;
  bool handles(const WorldGameplayRequest &request) const;
  bool decorate_spawn(GameplayPacket &packet) const;
  std::vector<GameplayPacket> receive(const WorldGameplayRequest &request,
                                      const WorldPosition &position, std::int64_t ticks);
  std::optional<std::int64_t> next_action_tick() const;

  void ensure_player_vitals(const Character &character, const WorldProgress &progress);
  bool start_tutorial_combat(const Character &character, std::int64_t wire_id, bool has_followup);
  void end_tutorial_combat();
  void on_map_departure(bool preserve_simulation, bool preserve_movement = false);
  void detach_world_mobs();
  bool has_pending_world_defeats() const;
  void retry_pending_world_monster_rewards(std::vector<GameplayPacket> &packets);
  void ensure_world_mobs(const WorldPosition &position);
  void reset_movement_vitals();
  void synchronize_player_state();
  void publish_committed_player_life();
  void persist_player_death(std::vector<GameplayPacket> &packets);
  enum class DeathRecovery { none, committed_reconnect, retry, conflict };
  // Call only while the old character admission is retained. A committed result still needs
  // an authoritative reconnect; it does not publish or release the pending timeline head.
  DeathRecovery reconcile_pending_death();
  bool has_pending_death() const noexcept;
  // Called only after the old session is sealed and its timeline/observer work is settled.
  bool acknowledge_reconciled_death() noexcept;
  std::optional<WorldPosition> revival_position() const;
  std::vector<GameplayPacket>
  persist_player_revival(std::optional<WorldPosition> destination = std::nullopt,
                         int revive_task_id = 0);
  std::vector<GameplayPacket> revive_player();

  void player_pvp_command(std::optional<bool> requested,
                          const std::function<void(std::string)> &completion);
  // `presence_map` is the housing channel's or instance copy's negative presence map, else 0.
  PvpPresenceUpdate update_pvp_presence(bool entered_world, bool services_entered,
                                        const WorldPosition &position, int presence_map,
                                        bool in_tutorial);

  void observe_map_load_vitals(FrameContext &frame, bool has_response);
  void observe_shared_map_mobs(FrameContext &frame, bool has_response);
  void observe_movement_vitals(FrameContext &frame,
                               const std::optional<nlohmann::json> &movement_to_broadcast);
  void observe_riding_vitals_reset();
  bool handle_shared_revival(FrameContext &frame);
  bool handle_private_combat(FrameContext &frame);
  std::vector<GameplayPacket> advance_tutorial(std::int64_t ticks, const WorldPosition &position);
  std::vector<GameplayPacket> advance_session_combat(std::int64_t ticks,
                                                     const WorldPosition &position);
  // Inspect this player's authoritative damage results, not observer broadcasts. Any
  // producer that can damage this player must invoke this before completing an action.
  void interrupt_gathering_on_damage(std::vector<GameplayPacket> &packets, std::int64_t ticks);
  void advance_gameplay_tick(std::int64_t ticks, const WorldPosition &position,
                             std::vector<GameplayPacket> &packets);

private:
  struct DeathCommitPlan {
    WorldProgress before;
    std::int64_t attempt_revision{};
    std::vector<CharacterItem> equipped_before;
    std::vector<EquippedDurabilityChange> changes;
    nlohmann::json changed_items = nlohmann::json::array();
    bool reconciled_committed{};
  };

  DeathCommitPlan prepare_death_commit_plan() const;
  void commit_world_monster_rewards(const WorldPosition &position,
                                    std::vector<GameplayPacket> &packets);
  // Official weapon switches are SwapWeapon skills carrying the destination equip_slot
  // (absent = Weapon_1). Selects that slot before the skill is validated against the weapon.
  std::vector<GameplayPacket> select_weapon_for_skill(const WorldGameplayRequest &request);

  CombatFeature &feature_;
  CombatPlayerContext &context_;
  std::unique_ptr<CombatSession> simulation_;
  FallTracker fall_tracker_;
  MovementVitals movement_vitals_;
  GlidingSession gliding_;
  bool region_handoff_pending_{};
  std::vector<GameplayPacket> pending_movement_reset_packets_;
  // Retained across an uncertain acknowledgement so reconciliation can fence and inspect
  // the exact transition instead of applying durability loss a second time.
  bool death_preparation_pending_{};
  bool death_owner_registered_{};
  std::optional<DeathCommitPlan> pending_death_;
};

} // namespace chronicles::combat
