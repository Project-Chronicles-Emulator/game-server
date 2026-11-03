#pragma once

// Pure combat rules: damage, status stat multipliers and abnormal status gauges.
//
// The damage formulas are Candidate fits to official PS2C_DamageNoti values (see
// docs/item-effects.md); the gauge rules are catalog GameVariable values whose decay matches the
// captured gauge notifications. Nothing here owns session state.

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace chronicles::combat_rules {

// --- Damage -------------------------------------------------------------------------------

// True when a unit's stats carry the attack-power ratings (stats 53/48) that the fitted
// formulas read. Fixtures without them keep the earlier local power model.
bool has_attack_ratings(const nlohmann::json &stats);

// Damage of one player hit on an NPC. Fitted to 867 official hits (absolute error at most 1.4):
// 5.7685 x (physical coefficient x PhysicalAttackPower + magic coefficient x MagicAttackPower)
// + 47.109 - 0.5717 x target armor, where the armor is the target's physical and magic armor
// weighted by the two attack shares. The constant and armor terms only apply to a hit that has
// an attack-power share; fixed damage is added unmitigated.
int player_damage(const nlohmann::json &parameters, const nlohmann::json &caster_stats,
                  const nlohmann::json &target_stats);

// Damage of one NPC hit on a player. Fitted to 89 official hits (91% exact after rounding):
// 1.2106 x coefficient x attack x 44.88 / (44.88 + armor) per physical and magic share, plus
// 1.0282 x elemental coefficient x elemental attack mitigated the same way by elemental armor.
int npc_damage(const nlohmann::json &parameters, const nlohmann::json &caster_stats,
               const nlohmann::json &target_stats);

// The earlier local model: fixed damage plus coefficient x PhysicalPower/MagicPower (22/17).
int legacy_damage(const nlohmann::json &parameters, const nlohmann::json &caster_stats);

// --- Status stat multipliers --------------------------------------------------------------

// Change in a stat from a MaintainStat.statMul value. The client shows a multiplier as
// floor(statMul x 0.01) x 0.1 percent (perk tooltip FTEXTFORMAT_PERK_DESC_STAT_MUL), so the
// unit is 1/1000 of a percent: 10000 is +10%. Rounded to the nearest whole point.
int stat_multiplier_delta(int stat_value, int stat_mul);

// --- Abnormal status gauges ---------------------------------------------------------------

// x5.pb.EX5AbnormalStatusType values.
enum class AbnormalType : int {
  burning = 1,
  freezing = 2,
  electric_shock = 3,
  bleeding = 4,
  poisoning = 5,
  stun = 6,
  shrink = 7,
};

// Parses a catalog type name such as "Burning" or "ElectricShock".
std::optional<AbnormalType> abnormal_type_from_name(std::string_view name);

// The wire enum name, for example "EX5AbnormalStatusType_Burning".
std::string_view abnormal_type_wire_name(AbnormalType type);

// Candidate: gauge points added per point of a damage effect's abnormalStatus*Attack value. The
// eight official gains measured against their catalog attack lay between 1.51 and 2.50 with no
// caster or target stat explaining the spread; this is their median.
inline constexpr double kGaugeGainPerAttack = 1.7;

struct AbnormalGaugeRule {
  int maximum{};
  int reduce_per_tick{};
  // Status applied when the gauge reaches its maximum (GameVariable
  // AbnormalStatus*StatusEffectId).
  int status_effect_id{};
};

struct AbnormalGaugeRules {
  int reduce_tick_ms{};
  std::map<AbnormalType, AbnormalGaugeRule> types;

  // Reads the profile's abnormal_gauges object; throws on malformed or out-of-range values.
  static AbnormalGaugeRules parse(const nlohmann::json &document);
};

// The abnormalStatus*Attack values of a damage effect, as (type, attack) pairs.
std::vector<std::pair<AbnormalType, int>> abnormal_attacks(const nlohmann::json &parameters);

// The *Add values of an addAbnormalGauge effect (burningAdd and so on), as (type, add) pairs.
std::vector<std::pair<AbnormalType, int>> abnormal_gauge_adds(const nlohmann::json &parameters);

// One unit's gauges. A gauge stores the value it had at its last update; the current value
// decays by reduce_per_tick every reduce_tick_ms, as the client simulates it from the update
// tick it was sent.
class AbnormalGauges {
public:
  // Current value at `ticks` (100 ns units), never below zero.
  int value(AbnormalType type, const AbnormalGaugeRules &rules, std::int64_t ticks) const;

  // Records the value the unit has from `ticks` on.
  void set(AbnormalType type, int value, std::int64_t ticks);

  void clear();

private:
  struct Gauge {
    int value{};
    std::int64_t updated_tick{};
  };

  std::map<AbnormalType, Gauge> gauges_;
};

} // namespace chronicles::combat_rules
