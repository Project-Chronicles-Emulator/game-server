#pragma once

#include "world/core/world_gameplay_request.hpp"
#include "world/features/combat/combat_feature.hpp"

#include <functional>
#include <stdexcept>

namespace chronicles::combat {

// Capabilities over this one live player. Every model/map/tutorial query resolves current
// state when called; storage commands keep their original durable transaction boundaries.
class CombatPlayerContext : public CombatEncounterAdmission {
public:
  virtual bool combat_has_handoff() const = 0;
  virtual std::int64_t combat_character_id() const = 0;
  virtual std::uint64_t combat_connection_id() const = 0;
  virtual std::uint64_t combat_wire_player_id(std::int64_t character_id) const = 0;
  virtual std::int64_t combat_monotonic_ticks() const = 0;
  virtual bool combat_closed() const = 0;
  virtual bool combat_services_entered() const = 0;
  virtual bool combat_in_housing() const = 0;
  virtual bool combat_riding() const = 0;
  virtual bool combat_other_action_busy() const = 0;

  virtual bool combat_has_tutorial() const = 0;
  virtual const WorldProgress &combat_tutorial_progress() const = 0;
  virtual const std::shared_ptr<const TutorialData> &combat_base_tutorial_data() const = 0;
  virtual void combat_accept_tutorial_progress(WorldProgress &&progress) = 0;
  virtual std::vector<GameplayPacket> combat_advance_tutorial(CombatSession &simulation,
                                                              std::int64_t ticks,
                                                              const WorldPosition &position) = 0;
  virtual ItemInventoryAccess combat_item_inventory() = 0;

  virtual bool combat_has_character_stats() const = 0;
  virtual nlohmann::json combat_character_stats(const Character &character) const = 0;
  virtual std::optional<Character> combat_character() const = 0;
  virtual WorldProgress combat_stored_progress() const = 0;
  virtual std::optional<WorldPosition> combat_last_position() const = 0;
  virtual bool combat_commit_death(std::int64_t revision,
                                   const std::vector<EquippedDurabilityChange> &changes) = 0;
  virtual std::optional<DeathReconciliationSnapshot> combat_fence_death_reconciliation() {
    throw std::runtime_error("Player death reconciliation is unavailable");
  }
  // A successful registration owns this character until the outcome is settled or an
  // authoritative reconnect can safely take over. Failure means no owner was installed.
  virtual bool combat_register_pending_death() noexcept {
    return false;
  }
  virtual void combat_pending_death_updated() noexcept {}
  virtual bool combat_forget_pending_death() noexcept {
    return false;
  }
  virtual bool combat_commit_revival(std::int64_t revision, const WorldPosition &position) = 0;
  virtual bool combat_commit_shrine_revival(const WorldProgress &before, const WorldProgress &after,
                                            const WorldPosition &position) = 0;
  virtual bool combat_has_revival_supplies() const = 0;
  virtual std::vector<GameplayPacket> combat_revival_supplies(int task_id, WorldProgress &progress,
                                                              const Character &character) const = 0;
  virtual std::vector<GameplayPacket>
  combat_revive_at_shrine(const WorldGameplayRequest &request) = 0;

  virtual void combat_attach_world_loot() = 0;
  virtual bool combat_uses_ordered_schedule() const {
    return false;
  }
  virtual void combat_settle_quest_events(CombatSession &, const WorldPosition &, std::int64_t,
                                          std::vector<GameplayPacket> &) {}
  virtual bool combat_uses_durable_monster_rewards() const {
    return false;
  }
  virtual MonsterRewardCapture combat_snapshot_monster_rewards(const MonsterDefeat &defeat) = 0;
  virtual bool combat_enqueue_monster_rewards(const std::vector<MonsterRewardReceipt> &receipts) = 0;
  virtual bool combat_enqueue_monster_rewards_with_script(
      const std::vector<MonsterRewardReceipt> &receipts,
      const std::optional<BossScriptIntent> &script) {
    if (script) {
      throw std::runtime_error("Durable boss script enqueue is unavailable");
    }
    return combat_enqueue_monster_rewards(receipts);
  }
  virtual bool combat_mark_monster_reward_ready(const std::string &event_id) = 0;
  // Returns true only after the captured script has been installed, its required
  // local notifications have been queued or their views departed, and the durable
  // publication acknowledgement has succeeded. False retains the defeat for retry.
  virtual bool combat_publish_boss_script(const BossScriptIntent &) {
    return false;
  }
  virtual void combat_retire_boss_script(const BossScriptIntent &) noexcept {
  }
  virtual void combat_notify_monster_rewards(const std::vector<MonsterRewardReceipt> &receipts) = 0;
  virtual void combat_schedule_monster_reward_retry() = 0;
  virtual void combat_record_monster_reward(const WorldPosition &position,
                                            const MonsterDefeat &defeat,
                                            std::vector<GameplayPacket> &packets) = 0;
  virtual void combat_sync_protection() = 0;
  virtual std::optional<std::int64_t> combat_next_gameplay_tick() const = 0;
  virtual void combat_publish_snapshot() = 0;
  virtual void combat_publish_committed_life(bool dead, const nlohmann::json &stats) = 0;
  virtual void combat_send_owner(const GameplayPacket &packet) = 0;
  virtual void combat_send_observers(const std::vector<GameplayPacket> &packets) = 0;

  virtual std::vector<GameplayPacket> combat_interrupt_gathering(std::int64_t ticks) {
    static_cast<void>(ticks);
    return {};
  }

  // Commits the weapon slot named by a SwapWeapon skill (the inventory owns the selection)
  // and returns its WeaponSlotActivationNoti and stat packets. Sessions without an inventory
  // owner select nothing.
  virtual std::vector<GameplayPacket>
  combat_select_weapon_for_skill([[maybe_unused]] const nlohmann::json &slot) {
    return {};
  }
};

} // namespace chronicles::combat
