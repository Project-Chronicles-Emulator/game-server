#pragma once

#include "data/client_catalog.hpp"
#include "ec.generated.pb.h"
#include "persistence/character_state_types.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace chronicles {

// Server-authoritative character progression: cumulative EXP and levels, build points,
// allocated attribute bonuses and earned perk (trait) nodes. Every operation mutates a
// WorldProgress copy that the caller commits as one revisioned transition; no operation trusts
// a client-supplied EXP amount, level or point balance.
//
// Evidence (protocol/feature-coverage.json "Level, EXP, stats, perks and achievements"):
//   Observed  PS2C_ChangeCharacterBaseNoti (36). `exp` is the cumulative total: the tutorial
//             capture reports exp=130 at level 1, then exp=630 with level=2 after a 500 EXP
//             quest reward, and every level-up frame (L2..L8) lies between consecutive
//             CharacterLevel.nextLevelExp values. nextLevelExp is therefore the cumulative total
//             at which the next level is reached. EXP gains carry only `exp`; a level-up adds
//             `level` and the LevelUpPoint balance; a WeaponPoint grant carries only that
//             balance.
//   Observed  Each reached level adds CharacterLevel.charBuildPoint (3) LevelUpPoints; unspent
//             points carry over (6 after skipping one allocation).
//   Observed  PC2S_UpdateStatBonusReq (20060) values are deltas (Int 30 -> 33 -> 36 after two
//             value-3 requests). PS2C_ChangeStatBonusNoti (120) returns the LevelUpPoint balance.
//   Observed  PC2S_ChangePerkReq (20053) -> PS2C_ChangePerkRes (99) with both balances; perks
//             cost PerkNode.cost WeaponPoints (1 and 2 observed).
//   Static    EErrorCode PerkNode_* / StatBonus_* names define the refusal reasons used here,
//             including deactivation with level 0 and exclusive node groups.
//   Candidate Prerequisites (PerkNode.FK_unlockNodeIds) are any-of: multi-entry lists name the
//             members of one exclusive group. Deactivation refunds the node cost.
//   Candidate Monster kill EXP = round_half_up((5 + npc_level / 4) * grade_multiplier) with
//             Weak 0.4, Normal 1, Strong 1.6, Elite 16 and Boss_B 60, fitted to every captured
//             kill (player levels 1-8, NPC levels 2-8). No client table carries kill EXP.
//             Unobserved Boss_A/Boss_C/Boss_S grades use the Boss_B multiplier (emulator
//             policy).

// x5.cms.EX5CharBuildPointType values, also the remaining_char_build_points map keys.
inline constexpr int kLevelUpPointType = 1;
inline constexpr int kWeaponPointType = 2;

// Recovered opcodes (protocol/opcodes.json).
inline constexpr std::uint16_t kChangeCharacterBaseNotification = 36;
inline constexpr std::uint16_t kChangeCharacterPublicNotification = 37;
inline constexpr std::uint16_t kChangePerkResponse = 99;
inline constexpr std::uint16_t kResetPerkNotification = 100;
inline constexpr std::uint16_t kUnitPerkNotification = 101;
inline constexpr std::uint16_t kChangeStatBonusNotification = 120;
inline constexpr std::uint16_t kChangePerkRequest = 20053;
inline constexpr std::uint16_t kResetPerkRequest = 20054;
inline constexpr std::uint16_t kUpdateStatBonusRequest = 20060;

// Emulator request bounds, not recovered official limits.
inline constexpr std::size_t kMaximumPerkUpdatesPerRequest = 64;
inline constexpr std::size_t kMaximumStatBonusEntriesPerRequest = 6;

// One allocation entry of UpdateStatBonusReq: `points` LevelUpPoints added to `stat_type`.
struct StatBonusAllocation {
  int stat_type{};
  int points{};
};

// One entry of ChangePerkReq.updated_perks. Level 0 deactivates a node.
struct PerkUpdate {
  int cms_id{};
  int level{};
};

// What one EXP award or level change did to a WorldProgress.
struct ExperienceChange {
  std::int64_t experience_added{};
  int previous_level{};
  int levels_gained{};
  int level_up_points_awarded{};

  bool changed() const {
    return experience_added != 0 || levels_gained != 0;
  }
};

class Progression {
public:
  // Loads CharacterLevel, PerkNode, PerkTheme, Npc and NpcTemplate once at startup; the
  // result is immutable and shared by every session. Throws on a malformed catalog.
  static std::shared_ptr<const Progression> load(ClientCatalog &catalog);

  int max_level() const {
    return max_level_;
  }

  // Cumulative EXP at which `level` advances to level + 1; nullopt for the cap or an
  // unknown level.
  std::optional<std::int64_t> next_level_experience(int level) const;

  // Cumulative EXP at which `level` is first reached (0 for level 1).
  std::int64_t level_floor_experience(int level) const;

  // Highest level a cumulative EXP total reaches, bounded by max_level().
  int level_for_experience(std::int64_t experience) const;

  // LevelUpPoints granted on reaching `level` (CharacterLevel.charBuildPoint).
  int level_up_points_on_reach(int level) const;

  // Adds a server-derived EXP amount, levels up through every crossed threshold and grants the
  // LevelUpPoints of each reached level. Throws std::invalid_argument for a negative amount.
  // EXP beyond the cap row's sentinel threshold is discarded.
  ExperienceChange award_experience(WorldProgress &progress, std::int64_t amount) const;

  // Applies levels that the stored EXP already reached (characters saved before automatic
  // level-up existed). Does nothing for an up-to-date character.
  ExperienceChange reconcile_level(WorldProgress &progress) const;

  // Game-master level change: sets the level and its EXP floor. Raising the level grants the
  // skipped levels' LevelUpPoints; lowering it keeps every point and allocation.
  ExperienceChange set_level(WorldProgress &progress, int level) const;

  // EXP for killing one NPC CMS type; 0 for unknown or non-monster NPCs.
  std::int64_t monster_experience(int npc_cms_id) const;

  // Adds (or with a negative amount removes, never below zero) build points of one type.
  x5::ec::EErrorCode grant_build_points(WorldProgress &progress, int point_type,
                                        int amount) const;

  // Spends LevelUpPoints on Con..Tal attribute bonuses. Nothing changes on a refusal.
  x5::ec::EErrorCode allocate_stat_bonus(WorldProgress &progress,
                                         const std::vector<StatBonusAllocation> &entries) const;

  // Returns every allocated attribute bonus to the LevelUpPoint balance.
  void reset_stat_bonus(WorldProgress &progress) const;

  // Game-master stat bonus (GmCmd.set_stat_bonus / add_stat_bonus): sets one attribute's
  // allocated bonus to `value` (0..kMaximumStatBonus) without spending LevelUpPoints.
  x5::ec::EErrorCode gm_set_stat_bonus(WorldProgress &progress, int stat_type, int value) const;

  // Current allocated bonus of one attribute (0 when none).
  static int stat_bonus(const WorldProgress &progress, int stat_type);

  // Game-master perk grant (GmCmd.learn_all_perks): activates every earned node of the class at
  // its maximum level without charging WeaponPoints. Nodes are taken in CMS id order; an
  // exclusive group keeps its first active member and a node waits until a prerequisite is
  // active, so the result always passes the normal tree validation. Returns the activated nodes.
  std::vector<PerkUpdate> gm_learn_all_perks(WorldProgress &progress, int class_id) const;

  // Activates/deactivates earned perk nodes of the character's class, charging or refunding
  // WeaponPoints. Validates the resulting tree as a whole. Nothing changes on a refusal.
  x5::ec::EErrorCode change_perks(WorldProgress &progress, int class_id,
                                  const std::vector<PerkUpdate> &updates) const;

  // Deactivates every earned node of one theme (or of every theme when `theme` is nullopt)
  // and refunds its cost. Returns the deactivated nodes in `reset_nodes` when supplied.
  x5::ec::EErrorCode reset_perks(WorldProgress &progress, int class_id, std::optional<int> theme,
                                 std::vector<int> *reset_nodes = nullptr) const;

  // Auto-registered nodes of the class plus active earned nodes, ordered by CMS id.
  std::vector<PerkUpdate> active_perks(int class_id, const CharacterBuild &build) const;

private:
  struct LevelRow {
    std::int64_t next_level_experience{};
    int level_up_points{};
  };

  struct PerkNode {
    int cms_id{};
    int class_id{};
    int theme{};
    int cost_type{};
    int cost{};
    int max_level{};
    int group_id{};
    bool auto_registration{};
    std::vector<int> unlock_nodes;
  };

  const PerkNode *perk(int cms_id) const;
  int active_level(const PerkNode &node, const CharacterBuild &build) const;
  x5::ec::EErrorCode validate_perk_tree(int class_id, const CharacterBuild &build) const;
  void add_levels(WorldProgress &progress, int target_level, ExperienceChange &change) const;

  std::map<int, LevelRow> levels_;
  int max_level_{};
  std::map<int, PerkNode> perks_;
  std::set<int> themes_;
  std::map<int, std::int64_t> monster_experience_;
};

// remaining_char_build_points map of WorldCharPerkNoti, ChangePerkRes and ResetPerkNoti.
nlohmann::json remaining_build_points_json(const WorldProgress &progress);

// PS2C_ChangeCharacterBaseNoti body for one EXP change in the observed shape: `exp` always,
// plus `level` and the LevelUpPoint balance when the level changed.
nlohmann::json experience_change_message(const WorldProgress &progress,
                                         const ExperienceChange &change);

// PS2C_ChangeCharacterBaseNoti body for a WeaponPoint balance change.
nlohmann::json weapon_points_message(const WorldProgress &progress);

// PS2C_ChangeCharacterBaseNoti body for a LevelUpPoint balance change without a level-up
// (GM grants and attribute resets).
nlohmann::json level_up_points_message(const WorldProgress &progress);

// PS2C_WorldCharPerkNoti body: active perks and both build point balances.
nlohmann::json perk_snapshot_message(const Progression &progression, int class_id,
                                     const WorldProgress &progress);

// Parses a decoded UpdateStatBonusReq. Returns nullopt for a malformed message.
std::optional<std::vector<StatBonusAllocation>>
parse_stat_bonus_request(const nlohmann::json &message);

// Parses a decoded ChangePerkReq. Returns nullopt for a malformed message.
struct PerkChangeRequest {
  int packet_index{};
  std::vector<PerkUpdate> updates;
};
std::optional<PerkChangeRequest> parse_perk_change_request(const nlohmann::json &message);

// Parses a decoded ResetPerkReq theme. Returns nullopt for a malformed message.
std::optional<int> parse_perk_reset_request(const nlohmann::json &message);

} // namespace chronicles
