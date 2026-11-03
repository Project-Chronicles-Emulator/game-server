#pragma once

#include "protocol/gameplay_packet.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles {

class ClientCatalog;

// Carried-weight encumbrance tiers. The catalog names two penalty statuses
// (GameVariable StatusEffectIdForInvWeightHeavy / ...VeryHeavy); `none` applies neither.
enum class EncumbranceLevel { none, heavy, very_heavy };

const char *encumbrance_level_name(EncumbranceLevel level);

// One flat MaintainStat modifier (statAdd) held by an encumbrance status while it is applied.
struct EncumbranceStatModifier {
  int stat_type{};
  int add{};
};

// The status effect a tier applies and the stat modifiers that status maintains.
struct EncumbranceTier {
  int status_effect_id{};
  std::vector<EncumbranceStatModifier> modifiers;
};

// Catalog Stat.min/max of one EX5StatType.
struct EncumbranceStatLimit {
  int minimum{};
  int maximum{};
};

// The catalog rows the rules are built from. Gathering them (file access) is separate from
// parsing them, so tests can supply rows without a verified catalog on disk.
struct EncumbranceCatalogRows {
  std::vector<nlohmann::json> game_variables;
  std::vector<nlohmann::json> status_effects;
  std::vector<nlohmann::json> maintain_effects;
  std::vector<nlohmann::json> maintain_stats;
  std::vector<nlohmann::json> stats;
};

// Immutable carried-weight encumbrance rules, loaded once at listener startup.
//
// Evidence (docs/inventory-weight-evidence.md):
// - Static catalog: status 50030004 (Heavy) maintains MoveSpeedRate -500 and status 50030005
//   (VeryHeavy) maintains MoveSpeedRate -1500; Stat MoveSpeedRate is limited to 200..1500.
// - Static client: AAC_Unit_Candidate_RefreshMoveSpeedFromStat scales the local movement speed
//   by stat 84 (MoveSpeedRate) * 0.001, so the client moves slower when the server lowers it.
// - Candidate: the tier thresholds. No capture crosses a weight threshold and the client keeps
//   no consumer of InvWeightMoveSpeedStep2RatioFrom, so the rule is a server-side reading of the
//   catalog values (see classify()).
class EncumbranceRules {
public:
  // Null when the catalog has no GameVariable rows naming the encumbrance statuses; throws when
  // the rows exist but are malformed or reference missing status/stat definitions.
  static std::shared_ptr<const EncumbranceRules> load(ClientCatalog &catalog);

  // Same contract as load() for rows gathered by the caller.
  static std::shared_ptr<const EncumbranceRules> parse(const EncumbranceCatalogRows &rows);

  EncumbranceRules(std::int64_t hard_ceiling_percent, int very_heavy_from_permille,
                   EncumbranceTier heavy, EncumbranceTier very_heavy,
                   std::map<int, EncumbranceStatLimit> stat_limits);

  // Candidate tier rule. A character is unencumbered up to its safe weight. Above it, the
  // overweight band runs from the safe weight to the hard ceiling (safe weight *
  // BagMaxWeightMultiplier percent, the same ceiling the NPC shop uses). Heavy applies in the
  // lower part of that band and VeryHeavy from InvWeightMoveSpeedStep2RatioFrom of the band on.
  // Throws std::invalid_argument for negative weights.
  EncumbranceLevel classify(std::int64_t carried_weight, std::int64_t safe_weight) const;

  // The status and modifiers of a tier; null for EncumbranceLevel::none.
  const EncumbranceTier *tier(EncumbranceLevel level) const;

  // Adds the modifiers of `level` to catalog-computed stats (EX5StatType number -> value),
  // clamping each modified stat to its catalog limit. Stats absent from `stats` stay absent.
  void apply_modifiers(EncumbranceLevel level, nlohmann::json &stats) const;

  std::int64_t hard_ceiling_percent() const;

  int very_heavy_from_permille() const;

private:
  std::int64_t hard_ceiling_percent_{};
  int very_heavy_from_permille_{};
  EncumbranceTier heavy_;
  EncumbranceTier very_heavy_;
  std::map<int, EncumbranceStatLimit> stat_limits_;
};

// One encumbrance status instance applied to the player.
struct AppliedEncumbranceStatus {
  int status_effect_id{};
  std::uint32_t instance_id{};
  // Internal 100 ns monotonic tick.
  std::int64_t start_tick{};
};

// A committed tier change. `changed_stat_types` lists every stat the old or new tier modifies.
struct EncumbranceTransition {
  EncumbranceLevel previous{};
  EncumbranceLevel current{};
  std::optional<AppliedEncumbranceStatus> removed;
  std::optional<AppliedEncumbranceStatus> added;
  std::vector<int> changed_stat_types;
  // Internal 100 ns monotonic tick of the change.
  std::int64_t tick{};
};

// Per-session encumbrance tier. Confined to the session worker strand.
class EncumbranceState {
public:
  explicit EncumbranceState(std::shared_ptr<const EncumbranceRules> rules);

  const EncumbranceRules &rules() const;

  EncumbranceLevel level() const;

  // Adds the applied tier's modifiers to catalog-computed stats.
  void apply_to(nlohmann::json &stats) const;

  // Records `next` as the applied tier; nullopt when it is already applied.
  std::optional<EncumbranceTransition> change_level(EncumbranceLevel next, std::int64_t tick);

private:
  std::uint32_t allocate_instance_id();

  std::shared_ptr<const EncumbranceRules> rules_;
  EncumbranceLevel level_{EncumbranceLevel::none};
  std::optional<AppliedEncumbranceStatus> applied_;
  std::uint32_t next_instance_id_{};
};

// Packets announcing a transition for the player unit `unit_id`, in the order observed for
// server status changes (status 100597): ChangeStatNoti (SCS_StatusEffect) first, then the
// RemoveStatusEffectNoti of the old tier, then the AddStatusEffectNoti of the new tier.
// `current_stats` must already include the new tier; stats it lacks are not reported.
std::vector<GameplayPacket> encumbrance_transition_packets(const EncumbranceTransition &transition,
                                                           std::uint64_t unit_id,
                                                           const nlohmann::json &current_stats);

} // namespace chronicles
