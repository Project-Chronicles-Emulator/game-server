#pragma once

#include "persistence/character_state_types.hpp"
#include "persistence/skill_item_use.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/features/inventory/inventory_bootstrap.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace chronicles {

class Progression;

// Immutable indexes shared by all instances. Load/validate before accepting clients.
class TutorialData {
public:
  static std::shared_ptr<const TutorialData> load(const std::filesystem::path &path);
  explicit TutorialData(nlohmann::json document);

  const nlohmann::json *object(std::uint64_t unit_id) const;
  const nlohmann::json *doodad(std::uint64_t unit_id) const;
  const nlohmann::json *quest(int cms_id) const;
  // Level triggers are keyed by their wire GUID pair ("x:y"), as sent by the client.
  const nlohmann::json *level_trigger(const std::string &wire_guid) const;
  const nlohmann::json *cut_scene(int cut_scene_id) const;
  const nlohmann::json *boss_defeat(int npc_cms_id) const;
  int map_id() const;
  std::shared_ptr<const TutorialData> followup(int map_id) const;

  nlohmann::json document;
  std::unordered_map<std::uint64_t, nlohmann::json> objects;
  std::unordered_map<std::uint64_t, nlohmann::json> doodads;
  std::unordered_map<int, nlohmann::json> quests;
  std::unordered_map<std::string, nlohmann::json> level_triggers;
  std::unordered_map<int, nlohmann::json> cut_scenes;
  std::unordered_map<int, nlohmann::json> boss_defeats;
  std::unordered_map<std::uint64_t, nlohmann::json> civilians;
  std::unordered_map<std::uint64_t, nlohmann::json> npcs;
  std::unordered_set<int> utility_items;
  std::unordered_map<int, nlohmann::json> potions;
  std::unordered_map<int, std::shared_ptr<const TutorialData>> followup_profiles;
};

// A map change requested by a completed cutscene's end point.
struct TutorialMapMove {
  int map_id{};
  WorldPosition spawn;
};

// One character's private tutorial instance. Calls are serialized by the session;
// persistence runs on its worker and completes before packets are returned.
class TutorialSession {
public:
  using Commit = std::function<bool(const WorldProgress &, const WorldProgress &)>;

  enum class InstanceCommitStatus {
    committed,
    // No free inventory slot for every granted row; nothing was written.
    inventory_full,
    // Revision, ownership or slot conflict; nothing was written.
    conflict,
  };

  struct InstanceCommitResult {
    InstanceCommitStatus status{InstanceCommitStatus::conflict};
    // On success, the granted rows as stored, with their ids, slots and creation time.
    std::vector<CharacterItem> stored;
  };

  // Atomically commits `after` with new unequipped item instances (equipment rewards such as
  // the tutorial treasure chest's necklace). The owner assigns inventory slots and creation
  // time; `granted` carries the catalog CMS id, grade and durability of each row.
  using InstanceCommit = std::function<InstanceCommitResult(
      const WorldProgress &before, const WorldProgress &after,
      const std::vector<CharacterItem> &granted)>;

  // Without `progression` (focused tests without a CharacterLevel table) EXP accumulates but
  // the level never advances and no kill EXP is awarded.
  TutorialSession(std::shared_ptr<const TutorialData> data, std::int64_t character_id,
                  WorldProgress progress, Commit commit,
                  std::shared_ptr<const Progression> progression = {});

  // Enables item-instance rewards. Without it, an object that grants an instance is rejected
  // with Interaction_ConditionsNotMatch and stays unactivated.
  void set_instance_commit(InstanceCommit commit);

  // Shared-world objects use the same activation transaction with server-rolled rewards.
  // The resolver runs only for an object that has not already been collected.
  using ObjectRewards = std::function<nlohmann::json(int, const WorldProgress &)>;
  void set_object_rewards(ObjectRewards resolver);
  void refresh_shared_progress(WorldProgress committed);

  std::vector<GameplayPacket> snapshot() const;
  std::vector<GameplayPacket> sectors() const;
  // Cosmetic phase-reset timers do not occupy the character's action slot.
  bool busy() const;
  bool has_cut_scene(int cut_scene_id) const noexcept;
  std::optional<std::int64_t> next_action_tick() const;
  std::vector<GameplayPacket> advance(std::int64_t server_ticks, const WorldPosition &position);
  std::vector<GameplayPacket> receive(std::uint16_t opcode, const nlohmann::json &request,
                                      std::uint32_t sequence, const WorldPosition &position,
                                      std::int64_t server_ticks);

  // Called only by authoritative combat after a monster enters its dead state.
  // This is deliberately separate from the client packet dispatch path.
  std::vector<GameplayPacket> record_monster_defeat(std::uint64_t unit_id,
                                                    const WorldPosition &position,
                                                    std::int64_t server_ticks = 0);

  // Server-authored relocations produced by a completed cutscene. The owning world
  // session applies them: a finished teleport moves the authoritative player position;
  // a map move starts the client's map-change handshake.
  std::optional<WorldPosition> take_completed_teleport();
  std::optional<TutorialMapMove> take_map_move();

  // Rebinds this session to the instance profile of the map the character moved into.
  // Transient actions belong to the previous map and are discarded.
  void enter_map(std::shared_ptr<const TutorialData> data);
  const TutorialData &data() const;

  const WorldProgress &progress() const;

  // Quantity of one stackable item in the committed inventory (0 when absent).
  std::int64_t stack_quantity(int item_cms_id) const;
  // Spends one stackable item for an item skill: commits the decremented stack, and the first
  // utility-bag slot that holds the item, then returns the ItemsUpdateNoti (utility-bag reason)
  // observed after every official item use. Throws when the item is missing or the commit fails.
  std::vector<GameplayPacket> consume_used_item(int item_cms_id);
  // Inactive preparation primitive for a future retained combat owner.
  SkillItemUsePlan prepare_skill_item_use(const std::string &nonce, int item_cms_id) const;
  // CMS ID of the NPC the character opened a dialogue with (DialogueStartReq); nullopt when no
  // dialogue is open. Company dialogue functions are valid only during such a dialogue.
  std::optional<int> dialogue_npc_cms_id() const;

  // Accepts progress that another session service (the NPC shop) committed from this
  // session's current revision, so later tutorial commits compare against stored state.
  void adopt_committed_progress(WorldProgress committed);
  // A server-owned death/revival commit already advanced the durable revision.
  void accept_combat_progress(WorldProgress committed);
  void accept_equipment_revision(std::int64_t committed_revision);

private:
  struct PendingInteraction {
    std::uint64_t unit_id{};
    int animation_id{};
    std::int64_t started_ticks{};
    bool executed{};
  };

  struct PendingPotion {
    int item_cms_id{};
    std::uint32_t instance_id{};
    std::int64_t started_ticks{};
    bool consumed{};
    bool finished{};
  };

  // Rewards that are item instances are appended to granted_instances; the caller commits them
  // with the progress through commit_progress_with_items.
  void activate(WorldProgress &next, std::uint64_t unit_id, int interaction_id,
                const WorldPosition &position, std::vector<GameplayPacket> &packets,
                std::vector<CharacterItem> &granted_instances, bool animated = false) const;
  void accept(WorldProgress &next, int quest_id, std::vector<GameplayPacket> &packets) const;
  void reward(WorldProgress &next, const nlohmann::json &definition,
              std::vector<GameplayPacket> &packets) const;
  void execute(WorldProgress &next, const WorldPosition &position, std::int64_t ticks,
               std::vector<GameplayPacket> &packets,
               std::vector<CharacterItem> &granted_instances);
  void refresh_doodad_phases(WorldProgress &next, std::int64_t ticks,
                             std::vector<GameplayPacket> &packets) const;
  void interact_doodad(WorldProgress &next, std::uint64_t unit_id, const WorldPosition &position,
                       std::int64_t ticks, std::vector<GameplayPacket> &packets) const;
  void start_potion(const nlohmann::json &request, const WorldPosition &position,
                    std::int64_t ticks, std::vector<GameplayPacket> &packets);
  void validate_potion_effect(const nlohmann::json &request, const WorldPosition &position,
                              std::int64_t ticks) const;
  void fire_level_trigger(WorldProgress &next, const nlohmann::json &request,
                          const WorldPosition &position, std::int64_t ticks,
                          std::vector<GameplayPacket> &packets) const;
  void play_cut_scene(int cut_scene_id, const WorldPosition &position,
                      std::vector<GameplayPacket> &packets) const;
  void end_cut_scene(int cut_scene_id, std::int64_t ticks, std::vector<GameplayPacket> &packets);
  void apply_boss_defeat(WorldProgress &next, int npc_cms_id, const WorldPosition &position,
                         std::int64_t ticks, std::vector<GameplayPacket> &packets) const;
  void remember_started_cut_scene(const std::vector<GameplayPacket> &packets);
  void commit_progress(WorldProgress next);
  // Commits progress together with new item instances; returns the stored rows with their ids.
  // A full inventory throws ItemSlotLimit and a storage conflict throws std::runtime_error;
  // either way nothing is written and progress_ is unchanged.
  std::vector<CharacterItem> commit_progress_with_items(WorldProgress next,
                                                        const std::vector<CharacterItem> &granted);
  // Applies a reward's CharBuildPoint entries ({"type", "value"} with EX5CharBuildPointType).
  void grant_build_points(WorldProgress &next, const nlohmann::json &entries,
                          std::vector<GameplayPacket> &packets) const;

  std::shared_ptr<const TutorialData> data_;
  std::int64_t character_id_{};
  WorldProgress progress_;
  Commit commit_;
  InstanceCommit instance_commit_;
  ObjectRewards object_rewards_;
  std::shared_ptr<const Progression> progression_;
  std::optional<PendingInteraction> pending_;
  std::map<std::uint64_t, std::int64_t> doodad_reset_ticks_;
  std::optional<std::uint64_t> dialogue_target_;
  std::optional<PendingPotion> pending_potion_;
  std::unordered_set<std::uint32_t> potion_instances_;
  // The server-played cutscene the client is watching. Its end request applies the
  // cutscene's follow-up (teleport, NPC phase binding or map move) exactly once.
  std::optional<int> active_cut_scene_;
  std::optional<WorldPosition> pending_teleport_;
  std::optional<WorldPosition> completed_teleport_;
  std::optional<TutorialMapMove> pending_map_move_;
  std::int64_t request_window_ticks_{};
  unsigned requests_in_window_{};
};

} // namespace chronicles
