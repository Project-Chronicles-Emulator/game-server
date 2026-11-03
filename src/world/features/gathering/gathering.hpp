#pragma once

#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"
#include "persistence/quest_service_effects.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/core/corpse_handling.hpp"
#include "world/core/quest_events.hpp"
#include "world/core/world_request_opcodes.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chronicles {

// Recovered world opcodes used by the gathering flow (build 40367e24...).
inline constexpr std::uint16_t kGatheringStartInteractionRequest =
    world_request::kInteractionStartRequest;
inline constexpr std::uint16_t kGatheringExecuteInteractionRequest = 20070;
inline constexpr std::uint16_t kGatheringEndInteractionRequest = 20071;
// PC2S_InteractUnitReq / PS2C_InteractUnitRes: instant quest doodad interactions.
inline constexpr std::uint16_t kGatheringInteractUnitRequest =
    world_request::kInteractionUnitRequest;
inline constexpr std::uint16_t kInteractUnitResponse = 163;
inline constexpr std::uint16_t kDoodadPhaseChangedNotification = 194;
inline constexpr std::uint16_t kGatheringStateChangedNotification = 196;

// A doodad whose phase graph reaches this phase disappears until it respawns.
inline constexpr int kDespawnedDoodadPhase = 0;

struct GatheringAnimation {
  int id{};
  int sp_cost{};
  // CMS EX5EquipmentType name, or "None" when no tool is required.
  std::string tool_equipment_type;
  int execute_time_ms{};
  int duration_ms{};
};

struct GatheringLootItem {
  int item_cms_id{};
  int min_quantity{};
  int max_quantity{};
  int weight{};
};

struct GatheringLootGroup {
  double rate_percent{};
  std::vector<GatheringLootItem> items;
};

struct GatheringLoot {
  double pack_rate_percent{};
  std::vector<GatheringLootGroup> groups;
};

// A placement from ArcheWorld/gathering.json (herbs, trees, ore veins, ...).
struct GatheringDefinition {
  int id{};
  int animation_id{};
  int deactivate_delay_ms{};
  int activate_delay_min_ms{};
  int activate_delay_max_ms{};
  GatheringLoot loot;
};

struct GatheringPoint {
  std::uint32_t sector_id{};
  std::uint32_t index{};
  int gathering_id{};
  int x_mm{};
  int y_mm{};
  int z_mm{};
};

// How a doodad's reward is reported in ItemsUpdateNoti. Captured: LootingPack tasks use the
// looting reason naming the doodad (Roseberry Bush 74); ExpertiseReward tasks use the system
// reason (Dark Vein 90).
enum class DoodadRewardReason { Looting, System };

struct DoodadPhaseChoice {
  int phase_id{};
  int weight{};
};

// One DoodadPhaseFlow row. A gathering phase waits for its interaction; a timed phase advances
// by itself after duration_ms.
struct DoodadPhase {
  int phase_id{};
  std::optional<int> interaction_id;
  int animation_id{};
  // The interaction has no animation: the client uses it at once with PC2S_InteractUnitReq
  // (quest doodads only).
  bool instant{};
  // A portal interaction (Interaction.playerTask MapMove) moves the player here.
  std::optional<WorldPosition> map_move;
  std::optional<int> skill_effect_id;
  GatheringLoot loot;
  DoodadRewardReason reason{DoodadRewardReason::System};
  int duration_ms{};
  std::vector<DoodadPhaseChoice> next;
};

struct DoodadDefinition {
  int id{};
  int initial_phase{};
  int respawn_delay_min_ms{};
  int respawn_delay_max_ms{};
  // A quest doodad (no harvesting reward of its own); its interactions advance quests.
  bool quest{};
  std::vector<DoodadPhase> phases;

  const DoodadPhase *phase(int phase_id) const;
};

// An authored spot doodad. Its wire unit ID is sector << 32 | local_id << 12 | 6. Unit ids are
// only unique within one world asset, so `world` indexes the profile's world list (0 is the main
// ArcheWorld; dungeon and story worlds follow).
struct DoodadPoint {
  std::uint32_t sector_id{};
  std::uint32_t local_id{};
  int doodad_id{};
  int x_mm{};
  int y_mm{};
  int z_mm{};
  int world{};
  // The authored spot is absent until an evidenced server script activates it.
  bool initially_deactivated{};
};

struct GatheringMapArea {
  int map_id{};
  std::array<int, 3> min_mm{};
  std::array<int, 3> max_mm{};
  // Index into the profile's world list; maps of one world share its coordinates.
  int world{};
};

// Corpse butchering ("handling loot"): the knife animation and, per monster, the
// Npc.FK_expertiseRewardId loot pack rolled when the butchering executes.
struct GatheringHandling {
  int animation_id{};
  std::unordered_map<int, GatheringLoot> rewards;
};

// Immutable placement and CMS indexes built by tools/build_gathering_data.py.
// Load and validate once at startup; lookups never parse JSON.
class GatheringData {
public:
  static std::shared_ptr<const GatheringData> load(const std::filesystem::path &path);

  explicit GatheringData(const nlohmann::json &document);

  bool has_map(int map_id) const;

  bool map_contains(int map_id, const std::array<int, 3> &position_mm) const;

  const GatheringPoint *point(std::uint32_t sector_id, std::uint32_t index) const;

  const GatheringDefinition *definition(int gathering_id) const;

  // Placements whose ID exists in the client table but has no item reward.
  bool is_unsupported(int gathering_id) const;

  const GatheringAnimation *animation(int animation_id) const;

  bool is_tool(int item_cms_id, const std::string &equipment_type) const;

  const DoodadPoint *doodad_point(std::uint32_t sector_id, std::uint32_t local_id) const;

  // The spot doodad of the map's world at `sector_id`/`local_id`; null otherwise.
  const DoodadPoint *doodad_point(int map_id, std::uint32_t sector_id,
                                  std::uint32_t local_id) const;

  // The spot doodad a wire unit id (sector << 32 | local_id << 12 | 6) names in the world of
  // `map_id`; null otherwise.
  const DoodadPoint *doodad_point_of_unit(int map_id, std::uint64_t unit_id) const;

  // True when some world of the profile places a quest doodad under this wire unit id.
  bool names_quest_doodad(std::uint64_t unit_id) const;

  const DoodadDefinition *doodad(int doodad_id) const;

  // True for interactions that start gathering on a doodad (for example 20001-20003).
  bool is_doodad_interaction(int interaction_id) const;

  // True for interactions of quest doodads (profile entries marked `quest`). Unlike the
  // dedicated gathering interactions these ids may also name other systems' targets, so a
  // request is only answered here when its target is a profiled quest doodad.
  bool is_quest_doodad_interaction(int interaction_id) const;

  // The butchering reward of a monster, or null when its corpse cannot be butchered.
  const GatheringLoot *handling_reward(int npc_cms_id) const;

  // The butchering animation (1000 in this build); zero when the profile has no butchering.
  int handling_animation_id() const;

  // GameVariable ToolDurabiliyDecreaseValue: durability a tool loses per gather.
  int tool_durability_loss() const;

  // GameVariable SPStatRegenDelayMilli: SP regeneration resumes this long after the animation.
  int sp_regen_delay_ms() const;

  std::size_t point_count() const;

  std::size_t definition_count() const;

  std::size_t doodad_point_count() const;

private:
  void parse_handling(const nlohmann::json &handling);

  std::vector<GatheringMapArea> maps_;
  std::unordered_map<std::uint64_t, GatheringPoint> points_;
  std::unordered_map<int, GatheringDefinition> definitions_;
  std::unordered_set<int> unsupported_;
  std::unordered_map<int, GatheringAnimation> animations_;
  std::unordered_map<std::string, std::unordered_set<int>> tool_items_;
  // Doodad spots per world index (0 = main world), keyed by sector and local id.
  std::vector<std::unordered_map<std::uint64_t, DoodadPoint>> doodad_points_;
  std::unordered_map<int, DoodadDefinition> doodads_;
  std::unordered_set<int> doodad_interactions_;
  std::unordered_set<int> quest_doodad_interactions_;
  GatheringHandling handling_;
  int tool_durability_loss_{};
  int sp_regen_delay_ms_{};
};

enum class GatheringNodeKind : std::uint8_t { Placement, Doodad };

struct GatheringNodeKey {
  int map_id{};
  GatheringNodeKind kind{GatheringNodeKind::Placement};
  std::uint32_t sector_id{};
  // Placement index within the sector, or the doodad's spot local ID.
  std::uint32_t index{};

  auto operator<=>(const GatheringNodeKey &) const = default;
};

std::uint64_t doodad_unit_id(std::uint32_t sector_id, std::uint32_t local_id);

// The doodad node a spot doodad's wire unit id names on `map_id`; nullopt for other units.
std::optional<GatheringNodeKey> doodad_node_of_unit(int map_id, std::uint64_t unit_id);

struct GatheringStateChange {
  GatheringNodeKey node;
  // Placements: whether the node can be gathered again.
  bool active{};
  // Doodads: the new phase, or nullopt when the doodad respawned in its authored initial phase
  // (official respawn ChangeDoodadPhaseNoti omits phase_id).
  std::optional<int> phase;
  // Internal 100 ns tick at which the doodad phase began.
  std::int64_t tick{};
  // Local-only version minted while the node state changes; never sent on the wire.
  std::uint64_t revision{};
};

struct BossSourceTarget;

// Node state shared by every session on one emulator process. Each map ID is a separate
// channel, so nodes on maps 9011 and 9012 change independently. All methods are
// thread-safe; callers deliver returned changes themselves.
class GatheringNodes {
public:
  explicit GatheringNodes(std::shared_ptr<const GatheringData> data = {},
                          std::uint32_t random_seed = std::random_device{}());

  // Install a fenced shared-source snapshot, optionally resolving the exact local owner.
  // Pending receipts are never exposed as an available source.
  bool install_shared_source(const GatheringNodeKey &node,
                             const StoredSharedGatheringSource &source,
                             std::int64_t now_epoch_ticks, std::int64_t now_monotonic_ticks,
                             std::optional<std::uint64_t> owner_token = {},
                             bool retain_owner = false, std::uint64_t *revision = nullptr);
  // Install both confirmed snapshots under one lock, releasing neither owner on failure.
  bool install_shared_source_pair(const GatheringNodeKey &first,
                                  const StoredSharedGatheringSource &first_source,
                                  std::uint64_t first_token,
                                  const GatheringNodeKey &second,
                                  const StoredSharedGatheringSource &second_source,
                                  std::uint64_t second_token,
                                  std::int64_t now_epoch_ticks,
                                  std::int64_t now_monotonic_ticks,
                                  bool retain_owners = false,
                                  std::uint64_t *first_revision = nullptr,
                                  std::uint64_t *second_revision = nullptr);
  bool release_published_reservations(const std::vector<GatheringNodeKey> &nodes,
                                      const std::vector<std::uint64_t> &tokens);
  // Reserves the complete boss target set before any storage attempt. Unpersisted
  // progressed local phases are refused rather than reset to an initial script phase.
  std::optional<std::vector<std::uint64_t>> reserve_boss_source_batch(
      const std::vector<BossSourceTarget> &targets, bool require_shared_initial = true);
  std::optional<std::vector<GatheringStateChange>> install_private_boss_sources(
      const std::vector<BossSourceTarget> &targets, const std::vector<std::uint64_t> &tokens,
      std::int64_t now_ticks);
  std::optional<StoredSharedGatheringSource> shared_source(const GatheringNodeKey &node) const;
  bool is_current_revision(const GatheringNodeKey &node, std::uint64_t revision) const;
  // Orders local source changes with recipient enqueue and sight-readiness marking.
  std::recursive_mutex &publication_mutex() noexcept;
  std::vector<SharedGatheringSourceRecord> due_shared_sources(
      std::int64_t now_ticks, std::size_t limit,
      std::optional<GatheringNodeKey> after = {}) const;

  // A reservation excludes competing claims without publishing a depleted/changed node.
  // It remains in flight until its exact owner completes or aborts it.
  std::optional<std::uint64_t> reserve_placement(const GatheringNodeKey &node,
                                                  std::int64_t now_ticks,
                                                  std::int64_t respawn_delay_ticks);
  std::optional<std::uint64_t> reserve_doodad(const GatheringNodeKey &node,
                                              const DoodadDefinition &definition,
                                              int expected_phase, int next_phase,
                                              std::optional<std::int64_t> next_delay_ticks);
  // Only the evidenced Cliffs cannon barrier is currently supported as an unprofiled script.
  std::optional<std::uint64_t> reserve_cliffs_cannon_barrier(const GatheringNodeKey &node);
  bool finish_reservation(const GatheringNodeKey &node, std::uint64_t token,
                          bool committed, std::int64_t now_ticks,
                          std::optional<std::int64_t> remaining_ticks = {},
                          std::uint64_t *revision = nullptr);

  bool finish_reservation_pair(const GatheringNodeKey &first, std::uint64_t first_token,
                               const GatheringNodeKey &second, std::uint64_t second_token,
                               bool committed, std::int64_t now_ticks,
                               std::uint64_t *first_revision = nullptr,
                               std::uint64_t *second_revision = nullptr);

  // Claims an active placement until respawn_tick. Returns false when already depleted.
  bool try_deplete(const GatheringNodeKey &node, std::int64_t now_ticks, std::int64_t respawn_tick,
                   std::uint64_t *revision = nullptr);

  // Undoes a claim whose reward commit failed, so the node is not lost.
  void restore(const GatheringNodeKey &node);

  bool is_active(const GatheringNodeKey &node, std::int64_t now_ticks) const;

  // The doodad's current phase; doodads without recorded state are in their initial phase.
  int doodad_phase(const GatheringNodeKey &node, const DoodadDefinition &definition) const;

  // Moves a doodad from expected_phase to next_phase (kDespawnedDoodadPhase despawns it) when
  // no other player changed it first. next_tick schedules the respawn or timed transition.
  bool try_advance_doodad(const GatheringNodeKey &node, const DoodadDefinition &definition,
                          int expected_phase, int next_phase, std::int64_t now_ticks,
                          std::optional<std::int64_t> next_tick,
                          std::uint64_t *revision = nullptr);

  // Records a server-scripted phase so later sector snapshots include the changed doodad.
  void set_scripted_doodad_phase(const GatheringNodeKey &node, int doodad_id, int phase,
                                 std::int64_t now_ticks, std::uint64_t *revision = nullptr);

  // Restore an encounter object only once; never reset a phase already used by another player.
  bool initialize_scripted_doodad_phase(const GatheringNodeKey &node, int doodad_id, int phase,
                                        std::int64_t now_ticks,
                                        std::uint64_t *revision = nullptr);

  // A scripted volume's doodad phase (LevelTriggerTask ChangeDoodadPhase). Returns true when the
  // recorded state changed: false when the doodad already shows `phase`, or when
  // `only_if_unrecorded` and some phase is already recorded (a saved phase never overrides the
  // live state of the map).
  bool record_volume_doodad_phase(const GatheringNodeKey &node, int phase, std::int64_t now_ticks,
                                  bool only_if_unrecorded, std::uint64_t *revision = nullptr);

  std::vector<GatheringStateChange> cliffs_monster_defeated(int map_id, int npc_cms_id,
                                                            std::int64_t now_ticks);
  std::vector<GatheringStateChange> restore_cliffs_progress(int map_id,
                                                             const WorldProgress &progress,
                                                             std::int64_t now_ticks);

  // Collects respawned placements and due doodad transitions.
  std::vector<GatheringStateChange> collect_respawns(std::int64_t now_ticks);

  std::optional<std::int64_t> next_respawn_tick(bool exclude_shared = false) const;
  std::optional<std::int64_t> next_shared_respawn_tick() const;

  // Bounded, atomic gathering state for one sector; throws rather than truncating.
  std::pair<std::vector<GatheringStateChange>, std::vector<GatheringStateChange>>
  sector_state(int map_id, std::uint32_t sector_id,
               const std::function<void()> &before_append = {}) const;

  // Depleted gathering placements on the map (SectorUpdateNoti/ChangeGatheringStateNoti).
  std::vector<GatheringStateChange> depleted_on_map(
      int map_id, const std::function<void()> &before_append = {}) const;

  // Doodads on the map whose phase differs from their authored initial state.
  std::vector<GatheringStateChange> doodads_on_map(
      int map_id, const std::function<void()> &before_append = {}) const;

private:
  struct NodeState {
    int doodad_id{};
    // A durable fence may preserve an unknown client baseline without inventing a phase.
    bool unknown_baseline{};
    // Placements: depleted until due_tick. Doodads: phase (0 = despawned) since begin_tick,
    // with due_tick scheduling the respawn or timed transition.
    int phase{};
    std::int64_t begin_tick{};
    std::optional<std::int64_t> due_tick;
    // The doodad state before the last claim, restored when that claim's commit fails.
    bool previous_recorded{};
    int previous_phase{};
    std::int64_t previous_begin_tick{};
  };

  struct DueIndexChange {
    GatheringNodeKey node;
    std::optional<std::int64_t> previous_tick;
    bool previously_shared{};
    std::optional<std::int64_t> next_tick;
    bool next_shared{};
  };

  // Stages allocations before changing any existing index entries. Called under mutex_.
  void replace_due_indexes_locked(std::initializer_list<DueIndexChange> changes);
  std::uint64_t replace_state_locked(const GatheringNodeKey &node,
                                     std::optional<NodeState> replacement);
  void prepare_revision_locked(const GatheringNodeKey &node);
  std::uint64_t publish_revision_locked(const GatheringNodeKey &node) noexcept;

  struct Reservation {
    std::uint64_t token{};
    bool previous_recorded{};
    NodeState previous;
    NodeState committed;
    std::optional<std::int64_t> delay_ticks;
  };

  std::optional<NodeState> prepare_shared_source(const GatheringNodeKey &node,
                                                 const StoredSharedGatheringSource &source,
                                                 std::int64_t now_epoch_ticks,
                                                 std::int64_t now_monotonic_ticks) const;

  std::optional<std::uint64_t> reserve_locked(const GatheringNodeKey &node,
                                               NodeState committed,
                                               std::optional<std::int64_t> delay_ticks);

  // Applies one due transition. Sets erase when the node returned to its initial state.
  std::optional<GatheringStateChange> advance_due(const GatheringNodeKey &node, NodeState &state,
                                                  bool &erase);

  std::shared_ptr<const GatheringData> data_;
  mutable std::recursive_mutex publication_mutex_;
  mutable std::mutex mutex_;
  std::mt19937 random_;
  std::map<GatheringNodeKey, NodeState> states_;
  std::map<GatheringNodeKey, StoredSharedGatheringSource> shared_sources_;
  std::map<GatheringNodeKey, Reservation> reservations_;
  std::map<GatheringNodeKey, std::uint64_t> revisions_;
  std::uint64_t next_revision_{1};
  using DueKey = std::pair<std::int64_t, GatheringNodeKey>;
  std::set<DueKey> shared_due_by_time_;
  std::set<DueKey> local_due_by_time_;
  std::set<GatheringNodeKey> shared_due_by_node_;
  std::uint64_t next_reservation_token_{1};
};

struct BossSourceTarget {
  GatheringNodeKey node;
  int initial_phase{};
};

struct GatheringActorIdentity {
  std::int64_t account_id{};
  std::int64_t character_id{};
  int world_id{};
  std::uint64_t channel{};
  // Shared world has a fixed scope; private copies use a fresh process/copy incarnation.
  std::string source_scope;
};

// Prepared on the actor strand, with no persistence or packet publication. The finalizer
// owns the shared quest intent, not the session, so detached reconciliation can finish it.
struct GatheringPreparedQuest {
  WorldProgress after;
  ItemTrade trade;
  QuestServiceEffects effects;
  std::function<void(bool)> finalize_intent;
  std::function<std::vector<GameplayPacket>(const std::vector<CharacterItem> &)> packets;
};

struct GatheringDurableRequest {
  GatheringNodeKey node;
  std::optional<std::uint64_t> corpse_unit_id;
  std::shared_ptr<CorpseHandlingState> corpse_handling;
  std::optional<std::uint64_t> corpse_reservation;
  std::optional<std::uint64_t> reservation;
  std::optional<GatheringNodeKey> linked_node;
  std::optional<std::uint64_t> linked_reservation;
  std::shared_ptr<GatheringNodes> nodes;
  int expected_phase{};
  int next_phase{};
  std::optional<std::int64_t> delay_ticks;
  std::optional<int> timed_successor_phase;
  QuestEvents events;
  WorldPosition position;
  WorldProgress before;
  WorldProgress after;
  ItemTrade trade;
};

struct GatheringDurableResult {
  bool committed{};
  bool unresolved{};
  WorldProgress after;
  ItemTrade trade;
  std::vector<GameplayPacket> packets;
  std::vector<GatheringStateChange> source_changes;
};

// Storage callbacks for the authenticated character. They run on the calling
// session's worker strand and must not hold the shared protocol codec lock.
struct GatheringPersistence {
  std::function<std::optional<Character>()> load_character;
  std::function<WorldProgress()> load_progress;
  // Evaluate authored quest conditions through the owning quest service.
  std::function<bool(const nlohmann::json &, const WorldPosition &)> conditions_met;
  // Revision-checked commit of the progress transition together with item-row changes
  // (tool durability). Returns false when another writer won.
  std::function<bool(const WorldProgress &, const WorldProgress &, const ItemTrade &)> commit;
  std::function<GatheringDurableResult(const GatheringDurableRequest &)> commit_durable;
};

// The SP drain announced for one interaction. Wire ticks use the client's nanosecond clock.
struct GatheringStaminaFlow {
  int cost{};
  std::int64_t wire_start_ns{};
  int execute_ms{};
  // SP regeneration resumes after the animation plus SPStatRegenDelayMilli.
  int regen_resume_ms{};
  std::int64_t server_ticks{};
};

// Player vitals owned by the world session. Every callback is optional: a session without
// simulated vitals gathers without SP checks, drains or equipment follow-ups.
struct GatheringVitals {
  std::function<bool()> dead;
  // Returns the packets announcing the drain (possibly none), or nullopt when the player lacks
  // the SP.
  std::function<std::optional<std::vector<GameplayPacket>>(const GatheringStaminaFlow &)>
      begin_stamina;
  // Deducts the announced cost once at execution; the client already simulated the drain.
  std::function<void(std::int64_t server_ticks)> spend_stamina;
  // Stops an unexecuted drain and returns the correcting StatSyncNoti, if one is needed.
  std::function<std::optional<GameplayPacket>(std::int64_t server_ticks)> cancel_stamina;
  // ChangeStatNoti (SCS_Equip) and EquipmentsUpdateNoti after a committed tool wear.
  std::function<std::vector<GameplayPacket>(const CharacterItem &tool)> tool_worn;
  std::function<bool(int effect_id)> effect_available;
  std::function<std::vector<GameplayPacket>(int effect_id, const WorldPosition &position,
                                             std::int64_t server_ticks)> apply_effect;
};

struct GatheringOutcome {
  // Ordered replies for the requesting client, including its own state change.
  std::vector<GameplayPacket> packets;
  // The gatherer's animation packets that nearby players must also receive.
  std::vector<GameplayPacket> observers;
  // Node changes that other sessions on the same map must also receive.
  std::vector<GatheringStateChange> broadcasts;
  // A used portal: the session moves the player to this map position after the replies.
  std::optional<WorldPosition> travel;
  std::optional<int> cut_scene_id;
};

// A corpse the player may butcher, resolved by the loot service that owns corpse rights.
struct ButcheringTarget {
  int npc_cms_id{};
  int map_id{};
  std::array<int, 3> position_mm{};
  std::shared_ptr<CorpseHandlingState> handling;
};

// Corpse rights for butchering. Both callbacks are optional; without them no corpse resolves.
struct GatheringCorpses {
  // The player's own unexpired, not yet butchered corpse with this wire unit ID.
  std::function<std::optional<ButcheringTarget>(std::uint64_t unit_id, std::int64_t server_ticks)>
      find;
  // Called once after the butchering reward committed. Returns the corpse's flag update.
  std::function<std::vector<GameplayPacket>(std::uint64_t unit_id, std::int64_t server_ticks)>
      butchered;
};

// One character's gathering actions. Calls are serialized by the world session.
// Execution is accepted only after the server-side animation deadline; the item
// grant, tool wear and node change succeed together or none of them is visible.
class GatheringSession {
public:
  GatheringSession(std::shared_ptr<const GatheringData> data, std::shared_ptr<GatheringNodes> nodes,
                   std::uint64_t player_unit_id, GatheringPersistence persistence,
                   std::uint32_t random_seed, GatheringVitals vitals = {});

  // True for gathering starts (placement indices, doodad gathering interactions or corpse
  // butchering), and for execute/end while a gathering action is open.
  bool handles(std::uint16_t opcode, const nlohmann::json &request) const;

  // True for a butchering start and for execute/end of an open butchering. Corpses lie wherever
  // monsters die, so these requests do not depend on the gathering map areas.
  bool corpse_request(std::uint16_t opcode, const nlohmann::json &request) const;

  void set_corpses(GatheringCorpses corpses);

  // other_action_busy: another service (skill, statue, craft) has an open action.
  GatheringOutcome receive(std::uint16_t opcode, const nlohmann::json &request,
                           std::uint32_t sequence, const WorldPosition &position,
                           std::int64_t server_ticks, bool other_action_busy = false);

  bool busy() const;

  // A damaging hit cancels the current interaction without granting an unfinished reward.
  GatheringOutcome interrupt(std::int64_t server_ticks);

  // Node state of the map (or instance copy) the character occupies. An open action keeps the
  // nodes it started on until it ends, so switching never hands a claim to another copy.
  void set_nodes(std::shared_ptr<GatheringNodes> nodes);
  const std::shared_ptr<GatheringNodes> &nodes() const noexcept;

  // Doodad interactions committed since the last call, for quest objectives.
  QuestEvents take_quest_events();

  WorldProgress load_progress() const;

private:
  struct PendingGathering {
    GatheringNodeKey node;
    int animation_id{};
    std::int64_t started_ticks{};
    bool executed{};
    // Doodads: the gathering phase the action started in.
    int doodad_phase{};
    bool stamina_flow{};
    // Butchering: the corpse's wire unit ID.
    std::optional<std::uint64_t> corpse_unit_id;
  };

  struct ResolvedTarget {
    GatheringNodeKey node;
    std::array<int, 3> position_mm{};
    int animation_id{};
    const GatheringDefinition *placement{};
    const DoodadDefinition *doodad{};
    const DoodadPhase *doodad_phase{};
    std::optional<std::uint64_t> corpse_unit_id;
  };

  GatheringOutcome start(const nlohmann::json &request, std::uint32_t sequence,
                         const WorldPosition &position, std::int64_t server_ticks,
                         bool other_action_busy);

  GatheringOutcome execute(const nlohmann::json &request, const WorldPosition &position,
                           std::int64_t server_ticks);

  // PC2S_InteractUnitReq on a quest doodad whose interaction has no animation: the phase
  // advances (with any authored reward) at once and InteractUnitRes answers the request.
  GatheringOutcome instant(const nlohmann::json &request, std::uint32_t sequence,
                           const WorldPosition &position, std::int64_t server_ticks,
                           bool other_action_busy);

  GatheringOutcome end(const nlohmann::json &request, std::int64_t server_ticks);

  ResolvedTarget resolve_placement(const nlohmann::json &request,
                                   const WorldPosition &position) const;

  ResolvedTarget resolve_doodad(const nlohmann::json &request,
                                const WorldPosition &position) const;

  ResolvedTarget resolve_corpse(const nlohmann::json &request, const WorldPosition &position,
                                std::int64_t server_ticks) const;

  std::optional<CharacterItem> usable_tool(const GatheringAnimation &animation) const;

  bool butchering_available() const;

  // The request's target_unit_id names a profiled quest doodad spot.
  bool names_quest_doodad(const nlohmann::json &request) const;

  std::map<int, int> roll_loot(const GatheringLoot &loot);

  int choose_next_phase(const DoodadPhase &phase);

  std::optional<std::int64_t> doodad_next_tick(const DoodadDefinition &definition,
                                               int next_phase, std::int64_t server_ticks);

  std::int64_t respawn_tick(const GatheringDefinition &definition, std::int64_t server_ticks);

  bool pending_expired(std::int64_t server_ticks) const;

  bool within_request_limit(std::int64_t server_ticks);

  // Drops the open action, stopping its SP drain; returns the correcting packets.
  std::vector<GameplayPacket> abandon_pending(std::int64_t server_ticks);

  GameplayPacket animation_result() const;

  std::shared_ptr<const GatheringData> data_;
  std::shared_ptr<GatheringNodes> nodes_;
  std::uint64_t player_unit_id_{};
  GatheringPersistence persistence_;
  GatheringVitals vitals_;
  GatheringCorpses corpses_;
  std::mt19937 random_;
  std::optional<PendingGathering> pending_;
  QuestEvents quest_events_;
  std::int64_t request_window_ticks_{};
  unsigned requests_in_window_{};
};

// Builds ChangeGatheringStateNoti (one per sector and direction) for placements and
// ChangeDoodadPhaseNoti for doodads.
std::vector<GameplayPacket>
gathering_state_packets(const std::vector<GatheringStateChange> &changes,
                        std::shared_ptr<GatheringNodes> nodes = {}, std::uint64_t channel = 0);

std::shared_ptr<const LocalPacketPlan> gathering_sector_plan(
    std::shared_ptr<GatheringNodes> nodes, int map_id, std::uint64_t channel,
    std::uint32_t sector_id, const std::vector<GatheringStateChange> &original_doodads);

// SectorUpdateNoti.doodads entries (DoodadSimpleNub) for one sector's changed doodads. The
// official server listed only doodads outside their initial phase; a despawned doodad carried
// its id and phase_begin_tick, with the zero phase omitted.
nlohmann::json sector_doodad_nubs(const std::vector<GatheringStateChange> &doodads);

} // namespace chronicles
