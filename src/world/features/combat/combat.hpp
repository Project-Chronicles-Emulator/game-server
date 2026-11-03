#pragma once

#include "common/world_metrics.hpp"
#include "persistence/character_repository.hpp"
#include "world/core/quest_events.hpp"
#include "world/features/combat/combat_rules.hpp"
#include "world/features/combat/combat_timeline.hpp"
#include "world/features/npc/npc_ai.hpp"
#include "world/features/quests/quest_intent_ledger.hpp"
#include "world/features/tutorial/tutorial.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace chronicles {

// CANDIDATE: milliseconds between a monster's death and the unit-remove notification
// (PS2C_UnitRemoveNoti, opcode 11), anchored to death. It is the median of the seven normal
// tutorial kills in the full-tutorial capture (native session 4d5c42e5...; Demise state to the
// first remove naming the unit: 11045, 11128, 11142, 11236, 11309, 11321, 11543 ms). The catalog
// Npc.corpseRemainMillis is 5000 for these NPCs and contradicts the captures; the extra ~6 s is
// unexplained, and whether the anchor is death or the auto-loot about one second later is
// unresolved. Outliers (5001015, 5001529, 5001082, boss 5001527) use the same default.
inline constexpr std::int64_t kCorpseRemovalDelayMs = 11236;
inline constexpr std::size_t kMaximumPendingWorldDefeats = 128;

// One adapted-chase attack motion block of an action stage, as exported by the combat data
// generator. Parsed and validated at load only; no server code moves a unit from it yet.
// Sample times are absolute montage milliseconds (not relative to window_start_ms).
struct RootMotionSample {
  std::int64_t elapsed_ms{};
  double x_mm{};
  double y_mm{};
  double z_mm{};
};

struct RootMotionWindow {
  std::string type;
  int window_start_ms{};
  int window_end_ms{};
  double max_move_distance_mm{};
  double offset_distance_mm{};
  std::string offset_basis_pos_type;
  std::string offset_basis_dir_type;
  double offset_direction_degrees{};
  std::string look_at_dir_type;
  double rotation_angular_speed_degrees{};
  bool using_scale{};
  bool ground_move{};
  bool ignore_collision{};
  bool end_ground_move{};
  std::string time_base;
  std::vector<RootMotionSample> samples;
};

struct UnitCollisionRadii {
  double radius_mm{};
  double half_height_mm{};
};

// One catalog stat modifier that a status effect holds on its target while it is active.
// `stat_type` is the resolved EX5StatType value, `add` the flat MaintainStat.statAdd and `mul`
// the MaintainStat.statMul multiplier in 1/1000 percent (see combat_rules::stat_multiplier_delta).
// An active status stores the resolved change in `add` with `mul` zero, so removing the status
// reverts exactly what applying it added.
struct StatusStatModifier {
  int stat_type{};
  int add{};
  int mul{};
};

// One AOEIntervalHit skill object (SkillObject row named by a spawnAreaOfEffect effect): a
// cylinder that applies `interval_effect_id` to the enemies inside it every interval.
struct AreaDefinition {
  int duration_ms{};
  int interval_ms{};
  int interval_effect_id{};
  // SkillObject.aoeSpawnSkillEffectId, applied once to the enemies inside the area at its
  // created tick (the elemental bursts' damage); zero when the area has none.
  int spawn_effect_id{};
  // SkillObject.FK_lifeTimeDespawnSkillEffect, applied once when the area's lifetime ends (the
  // lightning storm's bolts); it selects targets with its own shape around the area. Zero when
  // the area has none.
  int despawn_effect_id{};
  int radius_mm{};
  int half_height_mm{};
  // SkillObject.maxTargetCount; zero leaves the effect's own target limit.
  int max_targets{};
  std::string shape{"cylinder"};
  int extent_x_mm{};
  int extent_y_mm{};
};

// The server-side path of one NPC attack lunge: a straight line from the caster's start pose to
// the clamped destination, travelled along the authored position samples (absolute montage
// milliseconds, piecewise linear, scaled so the last sample reaches the destination). The window
// points into the immutable CombatData.
struct LungeMotion {
  const RootMotionWindow *window{};
  WorldPosition start;
  WorldPosition destination;
  // Sent to the client: displacement / authored path length (the last sample's forward distance).
  double scale{1.0};
  std::int64_t start_tick{};
  std::int64_t end_tick{};
  bool end_sent{};

  WorldPosition pose_at(std::int64_t ticks) const;
};

// An executable NPC attack: an NPC skill of catalog type Normal whose first stage executes an
// offensive effect. The range values are copied from the skill row once at load, so NPC AI
// ticks over a whole map population never re-walk the skill and effect JSON.
struct NpcAttackOption {
  int skill_id{};
  // Skill.minDistance and Skill.maxDistance (catalog cm x10).
  std::int64_t min_range_mm{};
  std::int64_t range_mm{};
  // A projectile or ranged-distance effect keeps the full catalog range when casting; melee
  // attacks are also limited by the NPC template's combat distance.
  bool ranged{};
  // Attacks whose every damage path is a caster-centred shape: the furthest caster-to-player
  // centre distance at which one of those shapes still touches a standing player's hit capsule
  // (plus a supported lunge's travel). Catalog maxDistance is often wider than the authored
  // shape, and a swing started beyond this distance plays its animation but cannot land. Empty
  // for projectile, area and reference-target attacks.
  std::optional<std::int64_t> melee_reach_mm;
};

// One Immune table row (catalog EX5ImmuneType names). Dodge and Damage cancel a whole hit; the
// reaction categories (Sway, Knockback, KnockDown, ...) only suppress the stagger of a hit that
// still lands. Other names (abnormal types, Immortal, ...) are kept but not interpreted here.
struct ImmunityDefinition {
  bool dodge{};
  bool damage{};
  std::set<std::string> names;
};

// One Shield table row named by an addShield effect. The absorbed amount is `amount` plus
// per-mille shares of the holder's maximum and current health; immune_id (0 for none) names the
// Immune row the holder keeps while the shield lasts.
struct ShieldDefinition {
  int duration_ms{};
  int amount{};
  int hp_per_remain_milli{};
  int hp_per_total_milli{};
  int immune_id{};
};

// Loaded once with the tutorial profile. All IDs here are CMS definitions;
// runtime units and projectile instances are allocated by the local server.
class CombatData {
public:
  explicit CombatData(const nlohmann::json &document);

  // Executable attacks of one NPC template in npc_skills catalog order; empty when it has none.
  const std::vector<NpcAttackOption> &npc_attack_options(int npc_cms_id) const;

  std::map<int, nlohmann::json> skills;
  nlohmann::json effects;
  nlohmann::json projectiles;
  nlohmann::json npc_stats;
  nlohmann::json npc_skills;
  // Complete catalog locomotion blocks for private GM-spawned encounters.
  std::map<int, NpcAiDefinition> npc_locomotion;
  nlohmann::json weapon_items;
  nlohmann::json recoveries;
  // Optional profile key: status effect id -> {duration_ms, ...}. Absent in older profiles.
  nlohmann::json status_effects;
  // Item CMS id -> the Item-type skill that a StartSkillReq{use_item} for it must name. Empty
  // for profiles exported before item skills; item use then stays on the tutorial path.
  std::map<int, int> item_skills;
  // Stat modifiers per status effect id, from the optional `stat_modifiers` rows of
  // status_effects.
  std::map<int, std::vector<StatusStatModifier>> status_stat_modifiers;
  // Abnormal status types each status effect makes its holder immune to (Immune table).
  std::map<int, std::set<combat_rules::AbnormalType>> status_abnormal_immunities;
  // Immune row id -> its protections, for stage immune windows and shields. Optional profile
  // key; empty in older profiles, which then grant no hit immunity.
  std::map<int, ImmunityDefinition> immunities;
  // Shield row id -> definition. Optional profile key; addShield does nothing without it.
  std::map<int, ShieldDefinition> shields;
  // Optional profile keys added with item effects; empty in older profiles.
  std::map<int, AreaDefinition> areas;
  std::optional<combat_rules::AbnormalGaugeRules> abnormal_gauges;
  // Item CMS id -> Item.FK_skillEffectList, for consumables used through PC2S_ItemUseReq.
  std::map<int, std::vector<int>> item_effect_lists;
  // Attack root motion windows keyed by (skill id, layer index, stage index) and the unit hit
  // capsules (game-hit radius) keyed by NPC cms id / player model collision id. Stored only.
  std::map<std::tuple<int, int, int>, RootMotionWindow> root_motion;
  std::map<int, UnitCollisionRadii> npc_collision;
  std::map<int, UnitCollisionRadii> player_collision;
  // NPC CMS id -> executable attacks derived from npc_skills at load (see NpcAttackOption).
  std::map<int, std::vector<NpcAttackOption>> npc_attacks;

private:
  void build_npc_attack_options();
};

// A monster that reached zero health through server-owned damage.
struct MonsterDefeat {
  std::uint64_t unit_id{};
  int npc_cms_id{};
  // The caller supplies the durable character ID before handing this to persistence.
  std::int64_t killer_character_id{};
  // Where the corpse lies.
  WorldPosition position;
  std::int64_t ticks{};
  // World encounters use a server-only receipt, independent of transient 32-bit actor IDs.
  std::uint64_t reward_credit_id{};
  // Stable 128-bit outbox event identity for this monster life, encoded as lowercase hex.
  std::string reward_event_id;
};

struct MonsterRewardCapture {
  // Immutable encounter script context captured with the lethal hit, before publication.
  std::optional<BossScriptIntent> boss_script;
  std::vector<MonsterRewardReceipt> recipients;
  std::vector<std::shared_ptr<quests::QuestIntentLedger::CaptureLease>> quest_captures;
  std::vector<int> relevant_quest_ids;
};

// What a defeat observer returns to combat.
struct MonsterDefeatReaction {
  // Sent ahead of the Demise state and death, matching the captured order (loot flag first).
  std::vector<GameplayPacket> packets;
  // True when the observer announces the corpse's PS2C_UnitRemoveNoti itself because it keeps
  // the corpse longer (for looting). Combat then does not arm its default removal timer, so the
  // client never receives two removals for one unit.
  bool manages_corpse_removal{};
  // Shared deaths activate staged corpse rights only once their public journal is ready.
  std::function<void(std::int64_t)> on_published;
};

// A lethal shared-world hit remains unpublished until its whole reward batch is durable.
struct PendingWorldDefeat {
  MonsterDefeat defeat;
  std::uint64_t caster{};
  MonsterRewardCapture capture;
  GameplayPacket damage;
  std::optional<MonsterDefeatReaction> loot_reaction;
  std::optional<std::vector<GameplayPacket>> final_packets;
  std::shared_ptr<const std::vector<GameplayPacket>> delivery_packets;
  bool death_state_applied{};
  bool finalized{};
  // Staged in the caller's output; this does not prove a recipient received or queued it.
  bool owner_packets_staged{};
};

// Called synchronously when a monster dies, before its death packets are queued.
// The observer runs inside combat resolution, so it must not block or touch storage.
using MonsterDefeatObserver = std::function<MonsterDefeatReaction(const MonsterDefeat &)>;

// The durable stack inventory that item skills spend. Both callbacks run on the world
// session's strand. consume_one commits one spent item before it returns the client update and
// throws when the item is missing or storage refuses the commit; the world connection then
// closes like after any other failed commit, so no effect of that use reaches the client.
struct ItemInventoryAccess {
  std::function<std::int64_t(int item_cms_id)> quantity;
  std::function<std::vector<GameplayPacket>(int item_cms_id)> consume_one;
};

// Player vitals and optional private NPC simulation, on the world session's worker strand.
// A null TutorialData starts with player vitals only; private GM encounters may be added later.
// Client effect/projectile reports never apply damage or advance deadlines. The only effect of a
// client projectile report on combat state is that a reported miss withdraws that flight's hit.
struct CombatSessionTestAccess;
class CombatSession;

class CombatMilestoneOwner {
public:
  virtual ~CombatMilestoneOwner() = default;
  virtual const CombatSession *owned_combat_session() const noexcept = 0;
};

class CombatSession {
public:
  struct PlayerMotionOutcome {
    std::vector<GameplayPacket> own;
    std::vector<GameplayPacket> observers;
  };

  PlayerMotionOutcome receive_player_motion(std::uint16_t opcode, const nlohmann::json &request,
                                            std::uint32_t sequence, const WorldPosition &position,
                                            std::int64_t ticks);

  // Flight reports may synchronize a validated visual trajectory, never award damage.
  std::vector<GameplayPacket> receive_projectile_report(std::uint16_t opcode,
                                                        const nlohmann::json &request,
                                                        const WorldPosition &position,
                                                        std::int64_t ticks);

  struct WorldMobs;

  static std::shared_ptr<WorldMobs> create_world_mobs(
      const nlohmann::json &map, std::shared_ptr<const CombatData> data);
  void attach_world_mobs(std::shared_ptr<WorldMobs> world, bool instance_copy = false,
                         std::int64_t initial_tick = 0);
  void detach_world_mobs(std::optional<std::int64_t> ticks = std::nullopt);
  bool has_world_mobs() const;
  // Call on the session's owner strand. The returned shared owner is scoped to the current
  // world population or instance copy and remains valid after this session detaches.
  // No work is ordered until all ingress and milestone producers use this timeline.
  std::shared_ptr<combat::CombatTimeline> world_combat_timeline() const;
  bool is_visible_world_monster(std::uint64_t unit_id) const;
  std::vector<MonsterDefeat> take_world_defeats();
  void set_world_defeat_capture(std::function<MonsterRewardCapture(const MonsterDefeat &)> capture);
  const std::deque<PendingWorldDefeat> &pending_world_defeats() const noexcept;
  std::vector<GameplayPacket> finalize_world_defeat(const std::string &event_id,
                                                    std::int64_t ticks);
  // Stages the retained immutable final batch once; failure leaves it retryable by event ID.
  bool stage_finalized_world_defeat(const std::string &event_id,
                                   std::vector<GameplayPacket> &destination);
  std::shared_ptr<const std::vector<GameplayPacket>>
  finalized_world_defeat_batch(const std::string &event_id);
  // The caller must first confirm every required recipient's ordered enqueue or departure.
  // The event ID and immutable batch identity must both match the retained defeat.
  bool acknowledge_delivered_world_defeat(
      const std::string &event_id,
      const std::shared_ptr<const std::vector<GameplayPacket>> &batch);
  void set_world_defeat_stage_hook_for_test(std::function<void()> hook);
  void set_projectile_preflight_hook_for_test(std::function<void()> hook);
  // Injects failures at named detach checkpoints while the world lock is held.
  void set_world_detach_hook_for_test(std::function<void(std::string_view)> hook);
  void set_world_journal_hook_for_test(std::function<void()> hook);
  void publish_world_journal_for_test(const std::vector<GameplayPacket> &packets);
  nlohmann::json world_detach_state_for_test() const;
  void seed_world_detach_journal_for_test(std::size_t count);
  bool acknowledge_world_defeat(const std::string &event_id);

  CombatSession(std::shared_ptr<const CombatData> data,
                std::shared_ptr<const TutorialData> tutorial_data, const Character &character,
                nlohmann::json stats, const WorldProgress &progress = {});
  ~CombatSession();

  bool handles(std::uint16_t opcode, const nlohmann::json &request) const;
  bool busy() const;
  bool decorate_spawn(GameplayPacket &packet) const;
  std::optional<std::int64_t> next_action_tick() const;
  std::vector<GameplayPacket> receive(std::uint16_t opcode, const nlohmann::json &request,
                                      const WorldPosition &position, std::int64_t ticks);
  std::vector<GameplayPacket> advance(std::int64_t ticks, const WorldPosition &position,
                                      TutorialSession &tutorial);

  // GM-spawned units are visible only to their issuing connection, even on a shared map.
  // They reuse combat without a TutorialSession and cannot grant tutorial quest credit.
  bool private_encounter_active() const;
  // True while an active enemy is engaged with this player.
  bool player_in_combat() const;
  std::vector<GameplayPacket> advance_private_encounter(std::int64_t ticks,
                                                        const WorldPosition &position);

  struct ProjectileWork {
    std::uint64_t life_id{};
    std::uint64_t caster{};
    std::uint64_t instance{};
    std::int64_t deadline{};

    bool operator==(const ProjectileWork &) const = default;
  };

  // Owner-strand inspection only. The caller must pair this per-session identity with the
  // timeline owner generation; selecting work does not resolve or reserve the flight.
  std::optional<ProjectileWork> earliest_projectile_work() const;

  struct WorldMilestoneAdvance {
    std::int64_t tick{};
    std::vector<GameplayPacket> packets;
    std::optional<std::int64_t> next_tick;
  };

  enum class WorldMilestoneOutcomeState { executing, packets_retained, ready, uncertain };

  struct RetainedWorldMilestone {
    combat::CombatTimeline::Token token;
    WorldMilestoneOutcomeState state{WorldMilestoneOutcomeState::executing};
    WorldMilestoneAdvance result;
    std::shared_ptr<CombatMilestoneOwner> enclosing_owner;
  };

  // The lifetime must be the enclosing session, already registered with an independent
  // supervisor. Owner generations are unique within this population until detached.
  std::optional<combat::CombatTimeline::Owner> bind_world_milestone_owner(
      std::uint64_t owner_id, std::shared_ptr<CombatMilestoneOwner> enclosing_owner);
  // Called on the owner strand only after claiming that tick from the shared timeline.
  // A late poll advances no farther than the next scheduled tick, so another owner's
  // earlier milestone can run before later effects from this session.
  std::optional<WorldMilestoneAdvance> advance_next_world_milestone(
      const combat::CombatTimeline::Token &token, std::int64_t now_tick,
      const WorldPosition &position);
  // Owner-strand APIs. The session and claimed population must remain fenced until a ready
  // outcome is settled, or an uncertain execution is reconciled by a future recovery path.
  std::shared_ptr<const RetainedWorldMilestone> retained_world_milestone(
      const combat::CombatTimeline::Token &token) const;
  bool finish_retained_world_milestone(const combat::CombatTimeline::Token &token);
  bool acknowledge_retained_world_milestone(
      const combat::CombatTimeline::Token &token,
      const std::shared_ptr<const RetainedWorldMilestone> &outcome);
  void set_milestone_retention_hook_for_test(std::function<void()> hook);
  void set_action_copy_hook_for_test(std::function<void()> hook);

  const nlohmann::json &player_stats() const;
  bool player_dead() const;

  // True while the player holds the stun ailment status that a full stun gauge applies
  // (GameVariable AbnormalStatusStunStatusEffectId). This is the only crowd-control state the
  // server models; other authored control effects are not classified yet.
  bool player_stunned(std::int64_t ticks) const;
  // Server-calculated environmental impact; bypasses attack/defence and cannot heal.
  // The world session must persist lethal results before publishing these packets.
  std::vector<GameplayPacket> apply_fall_damage(int damage, const WorldPosition &position,
                                                std::int64_t ticks);

  std::vector<GameplayPacket> apply_drowning_damage(int damage, const WorldPosition &position,
                                                    std::int64_t ticks);
  std::vector<GameplayPacket> change_movement_stamina(int delta, std::string_view source,
                                                      std::int64_t ticks, bool announce = true);
  std::optional<std::pair<std::uint64_t, GameplayPacket>>
  begin_swimming_stamina_flow(double rate_per_second, std::int64_t ticks);
  GameplayPacket end_swimming_stamina_flow(std::uint64_t instance, std::int64_t ticks,
                                          bool block_regeneration);
  bool movement_stamina_unlimited() const;
  bool can_apply_stat_recovery(int effect_id) const;
  void set_movement_recovery_blocked(bool stamina, bool health, std::int64_t ticks);

  // SP-costed interactions such as gathering. begin announces the drain the client simulates
  // (StatSyncNoti with an SCS_InteractionFlow flow and a paused regen range) and returns its
  // packets; nullopt when the living player lacks `cost` stamina. spend deducts the announced
  // cost once without a packet; cancel stops an unspent drain and returns its correction.
  // Wire ticks use the client's nanosecond clock; `ticks` is the internal server tick.
  std::optional<std::vector<GameplayPacket>>
  begin_interaction_stamina(int cost, std::int64_t wire_start_ns, int execute_ms,
                            int regen_resume_ms, std::int64_t ticks);
  void spend_interaction_stamina(std::int64_t ticks);
  std::optional<GameplayPacket> cancel_interaction_stamina(std::int64_t ticks);
  // Shared maps need vital/status deadlines without private-instance NPC simulation.
  std::vector<GameplayPacket> advance_player_vitals(std::int64_t ticks);

  // Candidate in-place revival: nothing is committed or persisted (the revisioned death-to-life
  // transition is not implemented), so the durable dead flag is left to the next progress commit.
  std::vector<GameplayPacket> revive(std::int64_t ticks);
  // Same as revive, but a living player changes nothing and yields no packets.
  std::vector<GameplayPacket> revive_if_dead(std::int64_t ticks);

  // Replaces cached player stats after a persisted equipment change.
  void update_player_stats(nlohmann::json stats);
  void update_player_weapon(const Character &character);

  // True for a known player skill whose catalog skillType is SwapWeapon.
  bool is_swap_weapon_skill(int skill_id) const;
  // A living player's level-up restores HP and SP to their maxima (observed: the official
  // level-up ChangeStatNoti reports full HP/SP). Returns false, changing nothing, when dead.
  bool refill_player_vitals();
  int monster_hp(std::uint64_t unit_id) const;

  // Replaces the observer notified when the player defeats a monster.
  void set_defeat_observer(MonsterDefeatObserver observer);

  // Player actions since the last call that advance quest objectives: accepted skill starts,
  // executed skill effects, statuses applied to the player and spent items.
  QuestEvents take_quest_events();
  const QuestEvents &quest_events() const noexcept;

  // Lets item skills (StartSkillReq with use_item) spend stackable items. Until both callbacks
  // are set, and for profiles without item skills, item use is left to the tutorial session.
  void set_item_inventory(ItemInventoryAccess inventory);
  bool owns_item_use() const;

  // The authenticated handshake binds the player's map independently of NPC populations.
  // Item skills still need validation and timer advancement in an otherwise empty field.
  void set_player_map(std::optional<int> map_id);

  // The Item.FK_skillEffectList of a consumable used through PC2S_ItemUseReq, or nullptr when
  // the profile has none for the item.
  const std::vector<int> *item_effect_list(int item_cms_id) const;

  // Applies a consumable's effect list to the living player (caster and target) at `ticks`.
  std::vector<GameplayPacket> apply_item_effects(const std::vector<int> &effect_ids,
                                                 const WorldPosition &position,
                                                 std::int64_t ticks);

  // The current abnormal gauge of the player or of a monster (zero when none is held).
  int abnormal_gauge(std::uint64_t unit_id, combat_rules::AbnormalType type,
                     std::int64_t ticks) const;

  // The current value of one of a monster's stats (status modifiers included).
  int monster_stat(std::uint64_t unit_id, int stat_type) const;

  // Game master protections, set by the world session only for a game-master handoff.
  // invulnerable: no damage from monsters, falls or drowning. hidden: monsters do not notice
  // the player (a monster the player attacks still fights back). infinite_stamina: sprint and
  // swim stamina costs are ignored.
  void set_gm_protection(bool invulnerable, bool hidden, bool infinite_stamina);

  // GM heal: HP and stamina to their maxima, reported with ChangeStatNoti. Empty when dead.
  std::vector<GameplayPacket> gm_restore_vitals(std::int64_t ticks);

  // Adds a private GM-spawned monster with catalog stats and complete locomotion.
  // Missing stats, attack definitions or locomotion keep the unit visual-only.
  bool add_gm_monster(std::uint64_t unit_id, int npc_cms_id, const WorldPosition &position);

  // Cancels owned effects before the caller removes the GM unit; releases its retained state.
  std::vector<GameplayPacket> remove_gm_monster(std::uint64_t unit_id, std::int64_t ticks);

  // GM suicide (GmCmd.suicide): environmental damage equal to the current HP. Unlike falls and
  // drowning it also applies while GM protection is on. Empty when the player is already dead.
  // The world session must persist the death before publishing these packets.
  std::vector<GameplayPacket> gm_kill_player(const WorldPosition &position, std::int64_t ticks);

  // Test-only: switches the idle-mob early-out off (the original path is then the oracle) for
  // every session in the process. Default on.
  static void set_idle_early_out_enabled_for_test(bool enabled);
  // Test-only: every monster's server state (position, aggro, damage flag, owner, AI state and
  // deadline, attack resolution, cooldown maps) as one string, for before/after comparisons.
  std::string monster_state_fingerprint_for_test() const;
  bool set_monster_status_sequence_for_test(std::uint64_t unit_id, std::uint64_t next_instance);

  // Read-only state of one server-simulated monster for the GM debug watch.
  struct MonsterDebugView {
    WorldPosition position;
    bool alive{};
    // Present for monsters with complete locomotion (NpcAi).
    std::optional<NpcAiState> ai_state;
    // Endpoint of the route the monster is currently running, if any.
    std::optional<WorldPosition> route_end;
  };

  // Nothing for a unit this combat session does not simulate. Changes nothing.
  std::optional<MonsterDebugView> monster_debug_view(std::uint64_t unit_id,
                                                     std::int64_t ticks) const;

  // GM combat commands. Each returns the notifications to send; an empty result means the
  // target is not a live unit this session simulates (or the change had no effect).
  //
  // Damage from the GM's character to one monster (GmCmd.damage_monster / monster_kill). A
  // lethal hit runs the normal defeat path: kill credit, loot and corpse removal.
  std::vector<GameplayPacket> gm_damage_monster(std::uint64_t unit_id, int amount,
                                                std::int64_t ticks);
  // Current HP of a live monster, or nothing.
  std::optional<int> live_monster_health(std::uint64_t unit_id) const;
  // Catalog status effect on the player or a monster, cast by the player (GmCmd.add_status_effect).
  std::vector<GameplayPacket> gm_add_status_effect(std::uint64_t unit_id, int status_effect_id,
                                                   std::int64_t ticks);
  // Removes every active instance of one status effect (GmCmd.remove_status_effect).
  std::vector<GameplayPacket> gm_remove_status_effect(std::uint64_t unit_id, int status_effect_id,
                                                      std::int64_t ticks);
  // Adds to one ailment gauge (GmCmd.add_abnormal_status_gauge); a full gauge applies its status.
  std::vector<GameplayPacket> gm_add_abnormal_gauge(std::uint64_t unit_id,
                                                    combat_rules::AbnormalType type, int amount,
                                                    std::int64_t ticks);
  // Ends the running skill of the player or a monster (GmCmd.cancel_skill).
  std::vector<GameplayPacket> gm_cancel_skill(std::uint64_t unit_id, const WorldPosition &position,
                                              std::int64_t ticks);
  // Clears the server's skill cooldowns for the player and every monster (GmCmd.cooldown).
  // Returns how many cooldown entries were cleared. The client keeps its own cooldown display.
  std::size_t gm_reset_cooldowns();
  // Pauses or resumes one monster's AI (GmCmd.npc_ai_off / npc_ai_on). A paused monster stays
  // where it is and does not acquire or attack; it can still be hit. Returns false for an
  // unknown or non-AI monster.
  // Pauses/resumes the route and running action, returning the cancellation/stop packets.
  std::vector<GameplayPacket> gm_toggle_monster_ai(std::uint64_t unit_id, bool enabled,
                                                   std::int64_t ticks);
  // Sets one player stat to `value` until the next stat recalculation (GmCmd.set_gm_stat).
  // HP and stamina are clamped to their maxima. Reported with ChangeStatNoti.
  std::vector<GameplayPacket> gm_set_player_stat(int stat_type, int value, std::int64_t ticks);
  // Executes only a bounded, known self-effect chain (healing/status/recovery/gauge changes),
  // never an arbitrary attack, projectile, item consumption or unsupported conditional effect.
  std::vector<GameplayPacket> gm_execute_self_effect(int effect_id, const WorldPosition &position,
                                                     std::int64_t ticks);

  // Teleports a GM monster's simulated position and announces it stopped there. AI monsters
  // keep their home; a paused AI stays at the new point. Returns nothing for unknown units.
  std::vector<GameplayPacket>
  gm_place_monster(std::uint64_t unit_id, const WorldPosition &destination, std::int64_t ticks);

private:
  friend struct CombatSessionTestAccess;
  // Falls and drowning respect GM protection; the GM suicide command ignores it.
  enum class GmProtectionRule { respected, ignored };

  std::vector<GameplayPacket> advance_encounter(std::int64_t ticks, const WorldPosition &position,
                                                TutorialSession *tutorial);
  bool owns_encounter_map(int map_id) const;
  void cancel_npc_projectiles(std::uint64_t unit_id, const WorldPosition &position,
                              std::int64_t ticks, std::vector<GameplayPacket> &packets);
  std::vector<GameplayPacket>
  apply_environmental_damage(int damage, const WorldPosition &position, std::int64_t ticks,
                             std::string_view origin,
                             GmProtectionRule protection = GmProtectionRule::respected);
  void finish_player_death(std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // A monster's HP just reached zero: kill credit and loot (defeat observer), the Demise state,
  // DeathNoti, cleared statuses and the corpse removal deadline.
  void defeat_monster(std::uint64_t target, std::uint64_t caster, std::int64_t ticks,
                      std::vector<GameplayPacket> &packets, bool outbox_committed = false);
  // Removes every active instance of one status effect from the player or a monster.
  void remove_statuses_by_id(std::uint64_t target, int status_effect_id, std::int64_t ticks,
                             std::vector<GameplayPacket> &packets);

  // A timed status effect the server applied to the player or to a monster.
  struct ActiveStatus {
    int status_effect_id{};
    std::uint32_t instance{};
    std::int64_t start_tick{};
    std::int64_t expire_tick{};
    std::uint64_t caster{};
    // The changes added to the holder's stats when the status was applied, so that removal
    // reverts exactly what was added.
    std::vector<StatusStatModifier> modifiers;
  };

  // An absorbing shield from an addShield effect. Its remaining amount is the holder's stat 71,
  // as in the captured official grants (ChangeStatNoti) and absorbed hits (DamageNoti).
  struct ActiveShield {
    int shield_id{};
    std::int64_t expire_tick{};
  };

  // What protects a unit from one hit at a given tick: the immune windows of its running action
  // stage and the immunity its active shield grants.
  struct HitImmunity {
    bool dodge{};
    bool damage{};
    std::set<std::string> names;
  };

  struct MonsterLifeIdentity {};

  struct NpcProjectileCaster {
    int cms_id{};
    nlohmann::json stats;
    std::shared_ptr<const MonsterLifeIdentity> life;
  };

  struct Action {
    int skill_id{};
    std::uint32_t instance{};
    int layer{};
    int stage{};
    std::int64_t started{};
    std::uint64_t target{};
    float yaw{};
    std::set<int> executed_events;
    std::set<std::pair<int, int>> client_projectile_events;
    bool player_motion_received{};
    std::set<int> adapted_rotation_windows;
    // Present only for an NPC attack whose window the server moves (see begin_npc_lunge).
    std::optional<LungeMotion> lunge;
    // The stage-0 root-motion window was evaluated (moved or not); a delayed window is only
    // evaluated once its start time is reached.
    bool lunge_window_checked{};
    // The stackable item an item skill spends at its useItem event; 0 for ordinary skills.
    int item_cms_id{};
    bool item_spent{};
    int effect_unique_id{};
    // Where the client's PC2S_ExecuteSkillEffectReq placed one stage event (keyed by the
    // event's unique effect id): the first unit it named and its aim point. Only targeted and
    // aimed area placements read it.
    struct PlacementHint {
      std::uint64_t target{};
      std::optional<WorldPosition> aim;
    };
    std::map<int, PlacementHint> placement_hints;
    // A flight can resolve after its caster has been removed. Keep its launch-time
    // damage inputs and life identity for that case, including chained area effects.
    std::shared_ptr<const NpcProjectileCaster> npc_projectile_caster;
  };

  struct ProjectileInterruption {
    std::optional<std::int64_t> tick;
  };

  struct Monster {
    int cms_id{};
    nlohmann::json stats;
    WorldPosition position;
    bool active{};
    bool aggro{};
    std::int64_t next_attack{};
    std::optional<Action> action;
    // Present for a complete authored spawner or private GM locomotion definition.
    // Older authored monsters without it retain their legacy damage-triggered attack path.
    std::optional<NpcAi> ai;
    bool hostile{};
    bool damaged{};
    // Server tick at which the dead monster's unit is removed from the client; unset while alive
    // and again once the removal has been sent.
    std::optional<std::int64_t> corpse_deadline;
    // First executable captured NPC skill (points into the immutable CombatData) and the
    // distance, in mm, at which both pursuit stops and the attack gate opens. AI monsters only.
    const nlohmann::json *attack_skill{};
    // True once the lazy attack-skill initialisation has run for a world mob. For a mob with no
    // usable attack attack_skill stays null, so the early-out cannot use it as "initialised".
    bool attack_resolved{};
    std::int64_t engagement_range_mm{};
    // Earliest next cast tick per skill, so catalog cooldowns are respected per skill.
    std::map<int, std::int64_t> skill_ready_ticks;
    // Least recently used eligible attacks rotate without imposing a global attack delay.
    std::map<int, std::int64_t> skill_last_used_ticks;
    // Whether the client was last told that this monster is running (PS2C_MoveNoti Run), and
    // the internal tick of the last announced position.
    bool move_announced{};
    std::int64_t last_move_tick{};
    // Authoritative position at the last announced moving frame, to measure the displacement since.
    WorldPosition last_move_position;
    // Statuses the player applied to this monster (debuffs, ailments) and its ailment gauges.
    std::vector<ActiveStatus> statuses;
    // Instance IDs belong to this monster life, not to the player applying a status.
    std::uint64_t next_status_instance{1};
    combat_rules::AbnormalGauges gauges;
    // A shield the monster raised on itself (boss guard skills); its amount is stats["71"].
    std::optional<ActiveShield> shield;
    // GmCmd.npc_ai_off: no AI steps and no attacks until npc_ai_on.
    bool gm_ai_paused{};
    // Shared world AI/action ownership. Zero means no player is engaged. Only the owning
    // session resolves this monster's attacks against its player vitals.
    std::uint64_t simulation_owner{};
    // A departed instance target left a live encounter that needs a nearby replacement.
    bool awaiting_target_transfer{};
    std::uint64_t private_owner{};
    bool proactive{true};
    bool attackable{true};
    // Server-only identity for durable world-quest kill receipts; never sent as an actor ID.
    std::uint64_t reward_credit_id{};
    std::string reward_event_id;
    // Late observers must see the pre-hit health until the reward batch is durable.
    std::optional<int> pending_defeat_visible_hp;
    std::shared_ptr<const MonsterLifeIdentity> life_identity =
        std::make_shared<MonsterLifeIdentity>();
    // NPC flights belong to the target's session. This shared interruption epoch lets
    // another session cancel those flights without losing their original hit ordering.
    std::shared_ptr<ProjectileInterruption> projectile_interruption =
        std::make_shared<ProjectileInterruption>();
  };

  bool monster_stunned(const Monster &monster, std::int64_t ticks) const;
  void record_world_monster_engagement(std::uint64_t unit_id);
  const NpcProjectileCaster *retired_projectile_caster(const Action &action,
                                                       std::uint64_t caster) const;
  void interrupt_monster_action(std::uint64_t unit_id, Monster &monster, std::int64_t ticks,
                                  std::int64_t resume_tick, std::vector<GameplayPacket> &packets);

  // A spawned area of effect (an AOEIntervalHit skill object) announced as its own unit.
  struct ActiveArea {
    std::uint64_t unit_id{};
    int cms_id{};
    const AreaDefinition *definition{};
    std::uint64_t caster{};
    WorldPosition position;
    Action action;
    std::int64_t next_hit_tick{};
    std::int64_t end_tick{};
    // Set once the lifetime despawn was announced; the unit removal follows this tick.
    std::optional<std::int64_t> removal_tick;
  };

  struct Projectile {
    std::uint64_t instance{};
    int cms_id{};
    std::uint64_t caster{};
    std::uint64_t target{};
    Action action;
    std::int64_t deadline{};
    // True when the player's client spawned the flight (PC2S_ExecuteSpawnProjectileSkillEffectReq)
    // under its own instance ID. The server still times the hit, but the client reports the
    // despawn, so the server never announces one for it.
    bool client_spawned{};
    WorldPosition launch;
    WorldPosition destination;
    bool linear{};
    std::int64_t launched{};
    std::int64_t last_report_tick{};
    bool hit_reported{};
    // Set only for NPC flights; player projectiles have their own interruption rules.
    std::shared_ptr<const ProjectileInterruption> npc_interruption;
    // Distinguishes reused client instance IDs during this session's lifetime.
    std::uint64_t work_life_id{};
    // Keep elapsed player-stage immunity until this particular impact is settled.
    HitImmunity retired_player_immunity;
  };

  enum class ProjectileBatchResult { unsupported, completed, deferred };

  // Only plain, nonlethal NPC hits without other authored effects can be rolled back here.
  ProjectileBatchResult advance_simple_npc_projectiles(
      std::int64_t ticks, const WorldPosition &position, std::vector<GameplayPacket> &packets);
  void defer_projectile_retry(std::int64_t ticks) noexcept;

  // A timed HP or SP gain from a statRecovery effect. The client simulates it as a continuous
  // flow announced at its start; the server accrues the same gain silently, in whole points,
  // proportionally to the elapsed time.
  struct Recovery {
    std::uint64_t instance{};
    int stat{};
    // Gain per interval, after the potion heal rate when the catalog row uses it.
    int amount{};
    int interval_ms{};
    std::int64_t start_tick{};
    std::int64_t end_tick{};
    // Whole points already added to the stat.
    std::int64_t applied{};
    // Catalog cancelType EndSkillStage: the gain stops when the player's casting action ends.
    std::optional<std::uint32_t> cancelled_with_action;
  };

  const nlohmann::json &stage(const Action &action) const;

  // Returns nullptr when the action's layer has no stage with this index.
  const nlohmann::json *find_stage(const Action &action, int stage_index) const;

  // Restarts the stage clock; every event of the entered stage runs once again.
  void enter_stage(Action &action, int stage_index, std::int64_t ticks);

  std::vector<std::uint64_t> targets(const nlohmann::json &effect, const Action &action,
                                     std::uint64_t caster, const WorldPosition &origin,
                                     const WorldPosition &player_position,
                                     const nlohmann::json *event_shape = nullptr) const;
  void start(const nlohmann::json &request, const WorldPosition &position, std::int64_t ticks,
             std::vector<GameplayPacket> &packets);

  // Handles PC2S_ChangeSkillStageReq. Accepted transitions are silent; a refused
  // transition of the current action also ends it, so it cannot stay in use.
  void change_stage(const nlohmann::json &request, const WorldPosition &position,
                    std::int64_t ticks, std::vector<GameplayPacket> &packets);

  // Sends PS2C_ChangeSkillStageRes with the error. When the instance is the
  // current action, also ends it so that later casts are not refused.
  void refuse_stage_change(std::uint32_t instance, const char *error_code, const char *end_reason,
                           const WorldPosition &position, std::int64_t ticks,
                           std::vector<GameplayPacket> &packets);

  void finish(std::optional<Action> &action, std::uint64_t caster, const WorldPosition &position,
              std::int64_t ticks, std::vector<GameplayPacket> &packets, const char *reason);
  // projectile_instance is set when a projectile hit applies the effect; its damage then names
  // the flight and the Projectile origin, as the captured official hits do. area_unit is set
  // when an area's interval hit applies it; its damage then names the area unit as caster with
  // the AreaOfEffect origin, as the captured official area hits do.
  void apply_effect(int effect_id, const Action &action, std::uint64_t caster,
                    std::uint64_t explicit_target, const WorldPosition &origin,
                    const WorldPosition &player_position, std::int64_t ticks,
                    std::vector<GameplayPacket> &packets, int depth = 0,
                    const nlohmann::json *event_shape = nullptr,
                    std::optional<std::uint64_t> projectile_instance = std::nullopt,
                    std::optional<std::uint64_t> area_unit = std::nullopt,
                    const HitImmunity *retired_player_immunity = nullptr);
  // Damage of one hit by `caster` on `target` (fitted official formulas when the caster has
  // attack ratings, the earlier local model otherwise).
  int hit_damage(const nlohmann::json &parameters, const Action &action, std::uint64_t caster,
                 std::uint64_t target) const;
  // Fields that DamageNoti and ImmuneNoti share for one hit: caster, target, skill, cast and
  // effect identity, the flight instance of a projectile hit and the area unit as caster of an
  // area hit.
  nlohmann::json hit_header(int effect_id, const Action &action, std::uint64_t caster,
                            std::uint64_t target, std::int64_t ticks,
                            std::optional<std::uint64_t> projectile_instance,
                            std::optional<std::uint64_t> area_unit) const;
  // Starts a server-driven flight from the caster's catalog spawn offset toward the target and
  // schedules its hit from distance and speed. A player projectile that the client drives is
  // skipped here: the client spawns it itself through spawn_client_projectiles. An aimed
  // destination (the client's ground marker) replaces the target: the flight lands there, or
  // where its lifetime ends when the point lies beyond its reach.
  void spawn_server_projectile(const nlohmann::json &parameters, const Action &action,
                               std::uint64_t caster, std::uint64_t target,
                               const WorldPosition &origin, const WorldPosition &player_position,
                               std::int64_t ticks, std::vector<GameplayPacket> &packets,
                               std::optional<WorldPosition> aimed_destination = std::nullopt);
  // Handles PC2S_ExecuteSpawnProjectileSkillEffectReq for a client-driven player projectile:
  // echoes PS2C_SpawnProjectileNoti under the client's instance IDs so the client confirms its
  // local flight, and schedules the hit on a valid hostile target. Throws CombatError when the
  // report is refused; the caller then answers PS2C_SpawnClientProjectileFailNoti.
  void spawn_client_projectiles(const nlohmann::json &request, const WorldPosition &position,
                                std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Closest living enemy crossed by the reported flight, never an arbitrary client hit ID.
  std::uint64_t projectile_target(const nlohmann::json &definition, const WorldPosition &launch,
                                  const WorldPosition &destination,
                                  std::uint64_t requested_target) const;
  // Handles PC2S_DespawnClientProjectileReq: echoes PS2C_DespawnProjectileNoti for a flight the
  // client spawned. A miss report withdraws that flight's pending hit; nothing else changes.
  void despawn_client_projectile(const nlohmann::json &request,
                                 std::vector<GameplayPacket> &packets);
  // Applies a due projectile's hit effects and announces PS2C_ProjectileHitNoti, then the
  // server-side despawn for a server-driven flight.
  void resolve_projectile_hit(const Projectile &projectile, const WorldPosition &player_position,
                              std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // The effect a conditional hit (ConditionalSkillEffect export) applies to `target`: the first
  // branch whose conditions all hold (target health ratio, active status), else its default.
  int conditional_effect_for(const nlohmann::json &conditional, std::uint64_t target) const;
  // Living enemies a piercing flight's path crosses, nearest to the launch first, limited to
  // the projectile's maxTargetCount; the flight's own target always counts.
  std::vector<std::uint64_t> penetrated_units(const nlohmann::json &definition,
                                              const Projectile &projectile) const;
  // True for a monster that a player projectile may hit: active, alive and listed in this
  // private instance.
  bool is_live_monster(std::uint64_t unit) const;
  void advance_action(std::optional<Action> &action, std::uint64_t caster,
                      const WorldPosition &origin, const WorldPosition &player_position,
                      std::int64_t ticks, std::vector<GameplayPacket> &packets,
                      int transitions = 0);
  // Starts the data-driven attack lunge for the NPC action that was just announced (opcode 14):
  // samples the caster and its target once, emits the skill-bound 64 and stores the motion in the
  // action. Unsupported or zero-displacement windows leave the attack unchanged.
  void begin_npc_lunge(std::uint64_t id, Monster &monster, const WorldPosition &target,
                       std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Puts the monster (and its AI) on the lunge pose of the given tick.
  void sync_npc_lunge_pose(Monster &monster, std::int64_t ticks);
  void emit_lunge_end(std::uint64_t caster, Action &action, const WorldPosition &pose,
                      std::int64_t end_tick, std::vector<GameplayPacket> &packets);
  void change_stat(int stat, int delta, std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Adds delta to HP or SP, clamped to [0, maximum], without notifying the client. Returns the
  // new value.
  int adjust_vital(int stat, std::int64_t delta);
  // Applies an addStatusEffect effect to `target` (the player or a monster) when the catalog
  // gives the status a positive duration; otherwise applies nothing.
  void add_status_effect(const nlohmann::json &parameters, std::uint64_t target,
                         std::uint64_t caster, std::int64_t ticks,
                         std::vector<GameplayPacket> &packets);
  // Applies one status to the player or a monster. A ResetDuration status that is already
  // active is refreshed instead of added again.
  void apply_status(std::uint64_t target, std::uint64_t caster, int status_id,
                    std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Applies a removeStatusEffect effect: removes the target's active statuses of its id.
  void remove_status_effect(const nlohmann::json &parameters, std::uint64_t target,
                            std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Removes statuses whose expiry has passed and reports each removal once.
  void expire_statuses(std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Removes every active status when the player dies and reports each removal once.
  void clear_statuses_on_death(std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Reverts a status's stat modifiers, then reports its removal.
  void remove_status(std::uint64_t holder, const ActiveStatus &status, std::int64_t tick,
                     std::vector<GameplayPacket> &packets);
  void append_status_removal(std::uint64_t holder, const ActiveStatus &status,
                             std::int64_t tick, std::vector<GameplayPacket> &packets) const;
  // The catalog modifiers of a status resolved against the holder's current stats.
  std::vector<StatusStatModifier> resolve_status_modifiers(int status_id,
                                                           const nlohmann::json &stats) const;
  // Adds (sign +1) or reverts (sign -1) the player's stat modifiers and reports every changed
  // stat in one ChangeStatNoti; nothing is sent when there is no modifier.
  void apply_stat_modifiers(const std::vector<StatusStatModifier> &modifiers, int sign,
                            std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Monster stat changes are not reported: the client shows no NPC stats.
  static void apply_monster_modifiers(Monster &monster,
                                      const std::vector<StatusStatModifier> &modifiers, int sign);

  // Abnormal status gauges. A positive change is blocked while the holder is immune to the
  // type; reaching the maximum resets the gauge and applies the type's ailment status.
  void change_abnormal_gauge(std::uint64_t unit, combat_rules::AbnormalType type, int change,
                             std::uint64_t caster, std::int64_t ticks,
                             std::vector<GameplayPacket> &packets);
  bool immune_to(std::uint64_t unit, combat_rules::AbnormalType type) const;
  const combat_rules::AbnormalGauges *gauges_of(std::uint64_t unit) const;

  // Hit immunity of the player or a monster at `ticks` (see HitImmunity).
  HitImmunity hit_immunity(std::uint64_t unit, std::int64_t ticks) const;
  void grant_hit_immunity(int immunity_id, HitImmunity &immunity) const;
  HitImmunity action_immunity(const Action &action, std::int64_t ticks) const;
  void retain_player_projectile_immunity(const Action &action, std::int64_t end_tick);
  // Applies an addShield effect to its holder: sets stat 71 to the shield amount, reports it
  // for the player and attaches the effect's status. A new shield replaces an older one.
  void add_shield(const nlohmann::json &effect, std::uint64_t holder, std::uint64_t caster,
                  std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Removes the holder's shield (dispelShield or expiry); the player is told stat 71 is 0.
  void clear_shield(std::uint64_t holder, std::int64_t ticks,
                    std::vector<GameplayPacket> &packets);
  // Clears every shield whose lifetime has passed.
  void expire_shields(std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Spends a hit on the target's shield first. Returns the absorbed amount and appends the
  // stat-71 change; the caller applies the remainder to health.
  int absorb_with_shield(std::uint64_t target, int damage, nlohmann::json &stat_changes);
  // Executes a changeWeaponSkillSlot or skillCooldownReset effect on the player.
  void apply_skill_slot_effect(int effect_id, const nlohmann::json &effect, std::int64_t ticks,
                               std::vector<GameplayPacket> &packets);

  // Areas of effect: spawned by a spawnAreaOfEffect effect, they hit on their own interval.
  // at_impact marks an area left by a projectile: it stays on the struck unit or impact point.
  void spawn_area(const nlohmann::json &effect, const nlohmann::json &parameters,
                  const Action &action, std::uint64_t caster, std::uint64_t target,
                  const WorldPosition &origin, const WorldPosition &player_position,
                  std::int64_t ticks, std::vector<GameplayPacket> &packets,
                  bool at_impact = false);
  // The client's reported aim point for the action's current event, when it lies within the
  // skill's range plus the report tolerance of `origin`.
  std::optional<WorldPosition> reported_aim_point(const Action &action,
                                                  const WorldPosition &origin) const;
  // Where a MainTarget or Aim area of a player cast is placed: on its target, at the
  // client-reported target or aim point, or ahead of the caster at the skill's range.
  WorldPosition targeted_area_position(const std::string &position_type, const Action &action,
                                       std::uint64_t target, const WorldPosition &origin,
                                       const WorldPosition &player_position) const;
  // True for a player area event that takes its placement from the client's report: a
  // MainTarget area without a live cast target, or any Aim area.
  bool needs_placement_report(const Action &action, std::uint64_t caster,
                              const nlohmann::json &event) const;
  // Records the client's reported target and aim point for the event the report names.
  void record_placement_hint(const nlohmann::json &report, const WorldPosition &position);
  void advance_areas(std::int64_t ticks, const WorldPosition &player_position,
                     std::vector<GameplayPacket> &packets);
  // Applies `effect_id` once from the area (its unit as caster) to the enemies inside it, as
  // the interval pulses and the spawn hit do.
  void apply_area_effect(const ActiveArea &area, int effect_id, std::int64_t ticks,
                         const WorldPosition &player_position,
                         std::vector<GameplayPacket> &packets);
  // The next free 32-bit skill-object unit id (see kAreaUnitIdBase).
  std::uint64_t allocate_area_unit_id();
  // The live enemies of the area's caster inside its cylinder, nearest first.
  std::vector<std::uint64_t> area_targets(const ActiveArea &area, int effect_limit,
                                          const WorldPosition &player_position) const;

  // Item use: the item a StartSkillReq spends (0 for an ordinary skill), validated against the
  // item-skill table and the durable inventory.
  int used_item(const nlohmann::json &request, int skill_id, const nlohmann::json &skill) const;
  // Spends the item of the player's current item action at its useItem event, once per cast.
  // Returns false when the action no longer owns an item to spend; it has then ended and no
  // chained effect of the event may apply.
  bool spend_action_item(const Action &action, const WorldPosition &position, std::int64_t ticks,
                         std::vector<GameplayPacket> &packets);

  // statRecovery: announces the flow that the client simulates; settle_recoveries accrues it.
  void start_stat_recovery(int effect_id, const nlohmann::json &parameters, const Action &action,
                           std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Adds each recovery's gain due by `ticks`, silently, and drops finished recoveries.
  void settle_recoveries(std::int64_t ticks);
  // Stops the EndSkillStage recoveries started by the given player action.
  void cancel_action_recoveries(std::uint32_t action_instance, std::int64_t ticks,
                                std::vector<GameplayPacket> &packets);
  // Drives one monster that has an NPC AI definition and emits its aggro, state, phase and
  // ordinary movement packets. Skill lunges retain their own root-motion lifecycle.
  void advance_npc_ai(std::uint64_t id, Monster &monster, const WorldPosition &player_position,
                      std::int64_t ticks, std::vector<GameplayPacket> &packets);
  // Sends one PS2C_MoveNoti Run (with pred_milli and the route goal) or Stop for the monster at
  // its current authoritative position and records what the client was told.
  void announce_npc_move(std::uint64_t id, Monster &monster, bool moving, std::int64_t ticks,
                         std::vector<GameplayPacket> &packets);

  std::shared_ptr<const CombatData> data_;
  std::shared_ptr<const TutorialData> tutorial_data_;
  std::optional<int> private_encounter_map_;
  std::optional<int> player_map_;
  std::uint64_t player_id_{};
  int class_id_{};
  std::string weapon_;
  int active_weapon_slot_{};
  nlohmann::json stats_;
  std::map<std::uint64_t, Monster> local_monsters_;
  std::shared_ptr<WorldMobs> world_mobs_;
  std::optional<combat::CombatTimeline::Token> executed_world_milestone_;
  std::optional<combat::CombatTimeline::Owner> world_milestone_owner_;
  std::shared_ptr<CombatMilestoneOwner> world_milestone_enclosing_owner_;
  bool world_milestone_detach_pending_{};
  bool world_milestone_timeline_retired_{};
  std::shared_ptr<RetainedWorldMilestone> retained_world_milestone_;
  std::function<void()> milestone_retention_hook_for_test_;
  std::function<void()> action_copy_hook_for_test_;
  std::uint64_t world_journal_cursor_{};
  std::int64_t projectile_report_window_{};
  unsigned projectile_reports_in_window_{};
  std::int64_t world_refresh_tick_{};
  std::set<std::uint64_t> visible_world_mobs_;
  // A shared monster can target only one session, but every player who damages it participates
  // in combat while that monster remains engaged and visible to them.
  std::set<std::uint64_t> engaged_world_mobs_;
  std::vector<MonsterDefeat> world_defeats_;
  std::deque<PendingWorldDefeat> pending_world_defeats_;
  bool pending_world_defeat_capacity_reported_{};
  std::function<MonsterRewardCapture(const MonsterDefeat &)> world_defeat_capture_;
  std::function<void()> world_defeat_stage_hook_for_test_;
  bool world_mobs_are_instance_copy_{};

  std::map<std::uint64_t, Monster> &monsters();
  const std::map<std::uint64_t, Monster> &monsters() const;
  std::unique_lock<world_metrics::RecursiveTimedMutex> lock_world_mobs() const;
  // True when advance_npc_ai would change nothing for this monster at this tick (see
  // NpcAi::idle_and_not_due); the cheap tests come first and the HP lookup last.
  bool idle_mob_not_due(std::uint64_t id, const Monster &monster, int player_map_id,
                        std::int64_t ticks) const;
  int world_map_id() const;
  void world_monster_defeated(std::uint64_t id, std::int64_t ticks);
  std::vector<GameplayPacket> receive_impl(std::uint16_t opcode,
                                         const nlohmann::json &request,
                                         const WorldPosition &position, std::int64_t ticks);
  void publish_world_packets(const std::vector<GameplayPacket> &packets);
  std::vector<GameplayPacket> refresh_world_mobs(const WorldPosition &position,
                                               std::int64_t ticks);
  std::optional<Action> action_;
  std::map<int, std::int64_t> cooldowns_;
  std::vector<Projectile> projectiles_;
  std::optional<std::int64_t> projectile_retry_not_before_;
  std::function<void()> projectile_preflight_hook_for_test_;
  std::function<void(std::string_view)> world_detach_hook_for_test_;
  std::function<void()> world_journal_hook_for_test_;
  bool world_detach_journal_transaction_{};
  std::size_t world_detach_journal_start_{};
  std::uint64_t next_projectile_work_life_id_{1};
  // Client-spawned flight instances whose despawn report has not been echoed yet, oldest first.
  // Bounded by kMaximumProjectiles; the oldest entry is dropped when a client never reports one.
  std::deque<std::uint64_t> client_projectiles_awaiting_despawn_;
  std::vector<Recovery> recoveries_;
  std::uint64_t next_recovery_instance_{1};
  // The announced, not yet deducted SP drain of an open interaction animation.
  struct InteractionStamina {
    std::uint64_t instance{};
    int cost{};
    std::int64_t regen_resume_tick{};
  };
  std::optional<InteractionStamina> interaction_stamina_;
  std::vector<ActiveStatus> statuses_;
  combat_rules::AbnormalGauges gauges_;
  // The player's active shield; its remaining amount is stats_["71"].
  std::optional<ActiveShield> player_shield_;
  std::vector<ActiveArea> areas_;
  // Sequence of the next area unit id inside the 32-bit skill-object block; wraps.
  std::uint64_t next_area_serial_{1};
  std::uint32_t next_status_instance_{1};
  std::vector<std::uint64_t> defeated_;
  MonsterDefeatObserver defeat_observer_;
  // Quest-relevant player actions not yet handed to the quest service.
  QuestEvents quest_events_;
  ItemInventoryAccess item_inventory_;
  // Damage effects that carry an overlap group hit each target once per cast. State is keyed by
  // (caster, cast instance), so overlapping casts, late projectiles and alternating casts never
  // reset each other. An entry is dropped once its cast can no longer hit: the action is over and
  // no projectile of that cast is in flight (release_finished_overlap_hits).
  struct OverlapHits {
    std::set<std::pair<std::string, std::uint64_t>> hits;
  };
  // Area pulses have their own contact scope; the same enemy can be hit on the next pulse.
  std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint64_t, std::int64_t>, OverlapHits>
      overlap_hits_;
  void release_finished_overlap_hits();
  std::uint32_t last_instance_{};
  // The client keys flights by (caster unit, low 32 bits of the instance) and numbers its own
  // projectiles from 1 upward. Server-allocated instances start far above that counter, so a
  // server flight never replaces the player's own flight in the client table.
  static constexpr std::uint64_t kFirstServerProjectileInstance = 0x40000000;
  std::uint64_t next_projectile_{kFirstServerProjectileInstance};
  std::uint32_t next_npc_instance_{1};
  std::optional<std::int64_t> regeneration_tick_;
  std::int64_t stamina_resume_tick_{};
  bool movement_stamina_recovery_blocked_{};
  bool movement_health_recovery_blocked_{};
  std::int64_t request_window_{};
  unsigned requests_in_window_{};
  bool gm_invulnerable_{};
  bool gm_hidden_{};
  bool gm_infinite_stamina_{};
};

// Attack skill an AI monster of the given NPC would start at this player distance: the
// catalog-driven selection described in combat.cpp (Normal type, Skill min/max distance,
// per-skill cooldown), or nullopt when no skill is eligible. Exposed for unit tests.
std::optional<int> select_npc_attack_skill(const CombatData &data, int cms_id,
                                           const std::map<int, std::int64_t> &ready_ticks,
                                           double distance_mm, std::int64_t ticks,
                                           const std::map<int, std::int64_t> &last_used_ticks = {});

} // namespace chronicles
