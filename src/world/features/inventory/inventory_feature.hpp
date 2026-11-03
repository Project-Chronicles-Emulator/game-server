#pragma once

#include "world/core/frame_context.hpp"
#include "world/core/progress_access.hpp"
#include "world/features/inventory/inventory_capacity.hpp"
#include "world/features/inventory/inventory_encumbrance.hpp"
#include "world/features/inventory/item_polish_catalog.hpp"

#include <map>
#include <memory>
#include <optional>
#include <random>
#include <unordered_set>
#include <vector>

namespace chronicles::inventory {

class InventorySession;
class InventorySessionContext;

class InventoryFeature final {
public:
  // The utility index loads here. The capacity, encumbrance and polish tables load later,
  // through the explicit load calls below.
  explicit InventoryFeature(std::shared_ptr<ClientCatalog> catalog);
  void load_capacity_and_encumbrance();
  void load_polish_catalog();

  std::unique_ptr<InventorySession> create_session(ProgressAccess &progress,
                                                   InventorySessionContext &context);
  bool has_capacity() const noexcept;
  CarriedWeight measure_weight(const Character &character, const WorldProgress &progress) const;

private:
  friend class InventorySession;

  std::shared_ptr<ClientCatalog> catalog_;
  std::shared_ptr<const std::unordered_set<int>> utility_items_;
  std::optional<InventoryCapacity> capacity_;
  std::shared_ptr<const EncumbranceRules> encumbrance_rules_;
  std::shared_ptr<const ItemPolishCatalog> polish_catalog_;
};

// Storage operations are explicit, result-bearing commands over the authenticated owner.
// Borrowed combat/tutorial views remain valid only during the synchronous worker call.
class InventorySessionContext {
public:
  virtual ~InventorySessionContext() = default;

  virtual bool inventory_has_handoff() const = 0;
  virtual std::int64_t inventory_character_id() const = 0;
  virtual std::uint64_t inventory_connection_id() const = 0;
  virtual std::uint64_t inventory_player_unit_id(std::int64_t character_id) const = 0;
  virtual std::int64_t inventory_monotonic_ticks() const = 0;
  virtual bool inventory_has_tutorial() const = 0;
  virtual const WorldProgress *inventory_tutorial_progress() const = 0;
  virtual bool inventory_has_character_stats() const = 0;
  virtual nlohmann::json inventory_character_stats(const Character &character) const = 0;
  virtual bool inventory_has_combat() const = 0;
  virtual bool inventory_player_in_combat() const {
    return false;
  }
  virtual bool inventory_action_busy() const = 0;
  virtual const nlohmann::json *inventory_combat_stats() const = 0;

  // The returned live snapshot is used immediately by the following cache/diff operation.
  // The value parameter preserves a move for equip/weight and the original copy for polish.
  virtual const nlohmann::json &inventory_update_combat_stats(nlohmann::json stats) = 0;
  virtual bool inventory_update_items(std::int64_t revision,
                                      const std::vector<CharacterItem> &mutations) = 0;
  virtual bool inventory_discard_items(std::int64_t revision,
                                       const std::vector<std::int64_t> &item_ids,
                                       const std::map<int, int> &stackable_counts) = 0;
  virtual bool inventory_apply_polish(std::int64_t revision, const ItemPolishChange &change) = 0;
  virtual bool inventory_save_progress(const WorldProgress &before, const WorldProgress &after) = 0;

  virtual void inventory_publish_equipment_revision(std::int64_t revision) = 0;
  virtual void inventory_publish_world_loot_progress(const WorldProgress &progress) = 0;
  virtual void inventory_publish_weapon_selection(const WorldProgress &progress) = 0;
  virtual void inventory_publish_snapshot(const Character &character,
                                          const nlohmann::json *prepared_stats) = 0;

  // Existing snapshot sink. The caller preserves its original codec lock scope; this
  // capability must not acquire a lock or recalculate stats (including Market and Mail).
  virtual void inventory_refresh_snapshot(const Character &character,
                                          const nlohmann::json *prepared_stats) = 0;
};

class InventorySession final {
public:
  InventorySession(InventoryFeature &feature, ProgressAccess &progress,
                   InventorySessionContext &context);

  // Initializes the session once its connection has been admitted.
  void on_connection_open();
  bool handle_equip(FrameContext &frame);
  bool handle_discard(FrameContext &frame);
  bool handle_polish(FrameContext &frame);
  bool handle_utility_bag(FrameContext &frame);
  void observe_utility_bag_snapshot(FrameContext &frame);

  std::int64_t stack_quantity(int item_cms_id);
  std::vector<GameplayPacket> consume_skill_item(int item_cms_id);
  // Inactive preparation primitive for a future retained combat owner.
  SkillItemUsePlan prepare_skill_item_use(const std::string &nonce, int item_cms_id);

  // Official weapon switches are SwapWeapon skills whose StartSkillReq names the destination
  // `equip_slot` (absent = Weapon_1). Commits that slot as the active weapon before combat
  // validates the skill, and answers WeaponSlotActivationNoti (172) plus the stat change.
  // Returns no packets without a selected character.
  std::vector<GameplayPacket> select_weapon_for_skill(const nlohmann::json &slot);

  nlohmann::json session_stats(const Character &character) const;
  void append_encumbrance_changes(std::vector<GameplayPacket> &packets, bool entering_world);
  std::vector<GameplayPacket> tool_wear_updates(const CharacterItem &tool);
  GameplayPacket equipment_stat_notification(std::int64_t unit_id, const nlohmann::json &before,
                                             const nlohmann::json &after,
                                             const nlohmann::json &current) const;

  // These own the existing ordered inventory event sinks, used by Shop/GM/Market/Mail.
  void on_inventory_stats_changed(const Character &character);
  void on_inventory_snapshot_changed(const Character &character,
                                     const nlohmann::json *prepared_stats);
  static GameplayPacket equip_error(const WorldGameplayRequest &request, x5::ec::EErrorCode code);

private:
  struct PolishRequest {
    std::int64_t item_id{};
    int additive_cms_id{};
  };

  static std::optional<int> parse_equip_slot(const nlohmann::json &raw_slot);
  static bool is_bare_unequip_request(const nlohmann::json &message);
  std::vector<GameplayPacket> unequip_slot(const WorldGameplayRequest &request,
                                           const Character &character);
  std::vector<GameplayPacket> equip_items(const WorldGameplayRequest &request);
  std::vector<GameplayPacket> activate_weapon(const WorldGameplayRequest &request);
  // Shared weapon-slot selection. `refuse_when_busy` keeps the explicit 20078 request idle-only.
  std::vector<GameplayPacket> select_weapon_slot(const Character &character,
                                                 const nlohmann::json &raw_slot,
                                                 bool refuse_when_busy);
  std::vector<GameplayPacket> commit_equipment_change(const WorldGameplayRequest &request,
                                                      const Character &character,
                                                      const std::vector<CharacterItem> &mutations,
                                                      std::optional<int> cleared_visual_slot);
  std::vector<GameplayPacket> discard_items(const WorldGameplayRequest &request);
  static bool changes_inventory(const std::vector<GameplayPacket> &packets);
  bool utility_bag_request(const WorldGameplayRequest *request) const;
  std::vector<GameplayPacket> receive_utility_bag(const WorldGameplayRequest &request);
  static std::optional<PolishRequest> parse_polish_request(const nlohmann::json &message);
  static x5::ec::EErrorCode polish_plan_error(PolishPlanStatus status);
  static const char *polish_outcome_name(PolishOutcome outcome);
  int roll_polish();
  static GameplayPacket polish_items_update(const CharacterItem &item, const PolishPlan &plan,
                                            PolishOutcome outcome);
  std::vector<GameplayPacket> polish_item(const WorldGameplayRequest &request);
  void append_polish_success_updates(const CharacterItem &item, const Character &polished,
                                     const std::optional<nlohmann::json> &stats_before,
                                     const std::optional<nlohmann::json> &stats_after,
                                     std::vector<GameplayPacket> &packets);

  InventoryFeature &feature_;
  ProgressAccess &progress_;
  InventorySessionContext &context_;
  std::optional<EncumbranceState> encumbrance_;
  // Constructed at the original polish-engine member position, before the unit registry.
  std::mt19937 polish_random_{std::random_device{}()};
  bool utility_bag_snapshot_sent_{};
};

} // namespace chronicles::inventory
