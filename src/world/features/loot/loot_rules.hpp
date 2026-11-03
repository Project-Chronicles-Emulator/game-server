#pragma once

#include "ec.generated.pb.h"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles {

// The loot behaviors that are our own guesses, not reproductions of the official server. Each
// has a switch in LootCustomRules; the loot profile builder (tools/build_loot_data.py) tags the
// groups that depend on a rule with the same names.
enum class LootRule {
  kill_gold,
  first_kill_rewards,
  quest_gated_rows,
  full_moon_stones,
  polish_step_rows,
  handling_loot,
  party_shared_loot,
};

// Name used in configuration, profile tags and logs.
std::string_view loot_rule_name(LootRule rule);

// Throws std::runtime_error for a name that is not a loot rule.
LootRule parse_loot_rule(std::string_view name);

// What the server answers when a corpse had loot but its roll produced nothing.
enum class EmptyRollReply {
  // CUSTOM default: succeed with cleared loot flags, as for a normal take.
  success,
  // Refuse with Npc_HasNoLoot.
  no_loot_error,
};

// When the shortened lifetime of an emptied corpse starts counting.
enum class EmptiedLifetimeAnchor {
  // CUSTOM default: from the moment the corpse is taken.
  take,
  // From the monster's death.
  death,
};

// CUSTOM (guessed, not official) defaults. The captures show clients looting from several
// metres but never state the server's exact tolerance or party sharing distance.
inline constexpr double kGuessedRangeToleranceMm = 2000.0;
inline constexpr double kGuessedPartyRangeMm = 30000.0;

// CUSTOM (guessed, not official): every value here is our own choice. The captures do not
// establish the official rule, so docs/monster-loot.md lists each guess and its evidence.
//
// Defaults enable every rule, as requested for local play. evidence_only() switches the rules off
// and removes the position tolerance so only behavior backed by the captures remains.
struct LootCustomRules {
  // --- Rules (see LootRule) ---
  bool kill_gold{true};
  bool first_kill_rewards{true};
  bool quest_gated_rows{true};
  bool full_moon_stones{true};
  bool polish_step_rows{true};
  bool handling_loot{true};
  bool party_shared_loot{true};

  // --- Guessed details ---
  // Slack added to GameVariable.LootingRange. The server knows the player's position only from
  // the last accepted report, so an exact limit would refuse requests the client allowed.
  double range_tolerance_mm{kGuessedRangeToleranceMm};
  // Refusal for a corpse that was already taken (or has nothing to take).
  x5::ec::EErrorCode repeated_take_error{x5::ec::Npc_HasNoLoot};
  // Refusal when no free inventory slot or stack kind is left.
  x5::ec::EErrorCode full_bag_error{x5::ec::ItemSlotLimit};
  // Refusal when a stack or gold total would overflow its limit.
  x5::ec::EErrorCode count_limit_error{x5::ec::ItemCountError};
  // Refusal from the optional weight policy.
  x5::ec::EErrorCode over_weight_error{x5::ec::Npc_LootOverMaxWeight};
  EmptyRollReply empty_roll_reply{EmptyRollReply::success};
  // Corpse lifetimes. Unset values use the catalog's NpcCorpse*DestroyMillis.
  std::optional<std::int64_t> lootable_lifetime_ms;
  std::optional<std::int64_t> emptied_lifetime_ms;
  // The lootable lifetime always starts at the monster's death.
  EmptiedLifetimeAnchor emptied_lifetime_anchor{EmptiedLifetimeAnchor::take};
  // Party members this close to the kill share the corpse (party_shared_loot).
  double party_range_mm{kGuessedPartyRangeMm};

  // Defaults plus every rule on.
  static LootCustomRules all_enabled();

  // Every rule off and no tolerance: only the evidence-backed behavior remains.
  static LootCustomRules evidence_only();

  // Reads the "custom_rules" object of a loot rules document. Missing keys keep their defaults;
  // unknown keys, wrong types and out-of-range values throw std::invalid_argument.
  static LootCustomRules from_json(const nlohmann::json &custom_rules);

  // Reads {"custom_rules": {...}} from a file with a bounded size.
  static LootCustomRules load(const std::filesystem::path &path);

  bool enabled(LootRule rule) const;

  // Rules that are switched on.
  std::vector<LootRule> active_rules() const;

  // One line for the startup log: active rules and the guessed details.
  std::string describe() const;
};

} // namespace chronicles
