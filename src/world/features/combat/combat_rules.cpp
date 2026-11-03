#include "world/features/combat/combat_rules.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace chronicles::combat_rules {
namespace {

using Json = nlohmann::json;

// EX5StatType values the formulas read.
constexpr int kMagicPowerStat = 17;
constexpr int kPhysicalPowerStat = 22;
constexpr int kMagicArmorStat = 47;
constexpr int kMagicAttackPowerStat = 48;
constexpr int kPhysicalArmorStat = 52;
constexpr int kPhysicalAttackPowerStat = 53;
constexpr int kIncomingDamageDecStat = 77;
constexpr int kIncomingDamageIncStat = 78;
constexpr int kOutgoingDamageDecStat = 87;
constexpr int kOutgoingDamageIncStat = 88;

// Player-on-NPC fit (867 official hits, every residual within 1.4 points).
constexpr double kPlayerAttackScale = 5.7685;
constexpr double kPlayerBaseDamage = 47.109;
constexpr double kPlayerDamagePerArmor = 0.5717;

// NPC-on-player fit (89 official hits, 91% exact after rounding).
constexpr double kNpcAttackScale = 1.2106;
constexpr double kNpcArmorConstant = 44.88;
constexpr double kNpcElementalScale = 1.0282;

constexpr double kMaximumDamage = 1000000.0;

// Candidate: the Incoming/Outgoing damage stats are capped at 1000 by the Stat table, read here
// as per mille of the hit.
constexpr double kDamageRateBase = 1000.0;

constexpr double kStatMulPerMultiple = 100000.0;

// Elemental and hit/impact coefficient names with their attack and armor stats.
struct ElementalTerm {
  std::string_view coefficient;
  int attack_stat;
  int armor_stat;
};

constexpr std::array<ElementalTerm, 10> kElementalTerms{{
    {"electricCoefficient", 30, 29},
    {"fireCoefficient", 33, 32},
    {"fogCoefficient", 36, 35},
    {"hitCoefficient", 39, 38},
    {"impactCoefficient", 42, 41},
    {"ironCoefficient", 45, 44},
    {"quakeCoefficient", 58, 57},
    {"siegeCoefficient", 61, 60},
    {"tidalCoefficient", 63, 62},
    {"windCoefficient", 66, 65},
}};

constexpr std::array<std::pair<AbnormalType, std::string_view>, 7> kAbnormalNames{{
    {AbnormalType::burning, "Burning"},
    {AbnormalType::freezing, "Freezing"},
    {AbnormalType::electric_shock, "ElectricShock"},
    {AbnormalType::bleeding, "Bleeding"},
    {AbnormalType::poisoning, "Poisoning"},
    {AbnormalType::stun, "Stun"},
    {AbnormalType::shrink, "Shrink"},
}};

// Catalog parameter keys of addAbnormalGauge effects, in AbnormalType order.
constexpr std::array<std::pair<AbnormalType, std::string_view>, 7> kGaugeAddKeys{{
    {AbnormalType::burning, "burningAdd"},
    {AbnormalType::freezing, "freezingAdd"},
    {AbnormalType::electric_shock, "electricShockAdd"},
    {AbnormalType::bleeding, "bleedingAdd"},
    {AbnormalType::poisoning, "poisoningAdd"},
    {AbnormalType::stun, "stunAdd"},
    {AbnormalType::shrink, "shrinkAdd"},
}};

constexpr int kMaximumGaugeValue = 1000000;

double stat(const Json &stats, int stat_type) {
  return stats.value(std::to_string(stat_type), 0);
}

double parameter(const Json &parameters, std::string_view key) {
  const auto found = parameters.find(std::string(key));
  if (found == parameters.end() || !found->is_number()) {
    return 0.0;
  }
  const double value = found->get<double>();
  return std::isfinite(value) ? value : 0.0;
}

// Armor below zero is clamped: the Stat table gives every armor a minimum of 0.
double armor(const Json &stats, int stat_type) {
  return std::max(0.0, stat(stats, stat_type));
}

double npc_mitigation(double armor_value) {
  return kNpcArmorConstant / (kNpcArmorConstant + armor_value);
}

double elemental_damage(const Json &parameters, const Json &caster_stats,
                        const Json &target_stats) {
  double total = 0.0;
  for (const auto &term : kElementalTerms) {
    const double coefficient = parameter(parameters, term.coefficient);
    if (coefficient <= 0.0) {
      continue;
    }
    const double attack = stat(caster_stats, term.attack_stat);
    total += kNpcElementalScale * coefficient * attack *
             npc_mitigation(armor(target_stats, term.armor_stat));
  }
  return total;
}

double damage_rate(const Json &caster_stats, const Json &target_stats) {
  const double outgoing = kDamageRateBase + stat(caster_stats, kOutgoingDamageIncStat) -
                          stat(caster_stats, kOutgoingDamageDecStat);
  const double incoming = kDamageRateBase + stat(target_stats, kIncomingDamageIncStat) -
                          stat(target_stats, kIncomingDamageDecStat);
  return std::max(0.0, outgoing) / kDamageRateBase * std::max(0.0, incoming) / kDamageRateBase;
}

int finish_damage(double amount) {
  if (!std::isfinite(amount)) {
    return 0;
  }
  return static_cast<int>(std::lround(std::clamp(amount, 0.0, kMaximumDamage)));
}

int bounded_integer(const Json &document, const char *key, int minimum, int maximum) {
  if (!document.contains(key) || !document.at(key).is_number_integer()) {
    throw std::runtime_error(std::string("Abnormal gauge rule lacks integer ") + key);
  }
  const auto value = document.at(key).get<std::int64_t>();
  if (value < minimum || value > maximum) {
    throw std::runtime_error(std::string("Abnormal gauge rule out of range: ") + key);
  }
  return static_cast<int>(value);
}

} // namespace

bool has_attack_ratings(const Json &stats) {
  return stats.is_object() && (stats.contains(std::to_string(kPhysicalAttackPowerStat)) ||
                               stats.contains(std::to_string(kMagicAttackPowerStat)));
}

int player_damage(const Json &parameters, const Json &caster_stats, const Json &target_stats) {
  const double physical =
      parameter(parameters, "physicalCoefficient") * stat(caster_stats, kPhysicalAttackPowerStat);
  const double magic =
      parameter(parameters, "magicCoefficient") * stat(caster_stats, kMagicAttackPowerStat);
  const double attack = std::max(0.0, physical) + std::max(0.0, magic);

  double amount = 0.0;
  if (attack > 0.0) {
    // The target's two armors weighted by the share each attack type has in this hit.
    const double weighted_armor = (std::max(0.0, physical) * armor(target_stats, kPhysicalArmorStat) +
                                   std::max(0.0, magic) * armor(target_stats, kMagicArmorStat)) /
                                  attack;
    amount = kPlayerAttackScale * attack + kPlayerBaseDamage -
             kPlayerDamagePerArmor * weighted_armor;
  }
  // Candidate: no captured player effect has an elemental coefficient; the NPC fit is reused.
  amount += elemental_damage(parameters, caster_stats, target_stats);
  amount = std::max(0.0, amount) * damage_rate(caster_stats, target_stats);
  amount += std::max(0.0, parameter(parameters, "fixedDamage"));
  return finish_damage(amount);
}

int npc_damage(const Json &parameters, const Json &caster_stats, const Json &target_stats) {
  const double physical = parameter(parameters, "physicalCoefficient") *
                          stat(caster_stats, kPhysicalAttackPowerStat) *
                          npc_mitigation(armor(target_stats, kPhysicalArmorStat));
  const double magic = parameter(parameters, "magicCoefficient") *
                       stat(caster_stats, kMagicAttackPowerStat) *
                       npc_mitigation(armor(target_stats, kMagicArmorStat));

  double amount = kNpcAttackScale * (std::max(0.0, physical) + std::max(0.0, magic));
  amount += elemental_damage(parameters, caster_stats, target_stats);
  amount *= damage_rate(caster_stats, target_stats);
  amount += std::max(0.0, parameter(parameters, "fixedDamage"));
  return finish_damage(amount);
}

int legacy_damage(const Json &parameters, const Json &caster_stats) {
  const double amount =
      parameter(parameters, "fixedDamage") +
      parameter(parameters, "physicalCoefficient") * stat(caster_stats, kPhysicalPowerStat) +
      parameter(parameters, "magicCoefficient") * stat(caster_stats, kMagicPowerStat);
  return static_cast<int>(std::clamp(amount, 0.0, kMaximumDamage));
}

int stat_multiplier_delta(int stat_value, int stat_mul) {
  const double delta = static_cast<double>(stat_value) * stat_mul / kStatMulPerMultiple;
  return static_cast<int>(std::lround(std::clamp(delta, -kMaximumDamage, kMaximumDamage)));
}

std::optional<AbnormalType> abnormal_type_from_name(std::string_view name) {
  for (const auto &[type, type_name] : kAbnormalNames) {
    if (type_name == name) {
      return type;
    }
  }
  return std::nullopt;
}

std::string_view abnormal_type_wire_name(AbnormalType type) {
  switch (type) {
  case AbnormalType::burning:
    return "EX5AbnormalStatusType_Burning";
  case AbnormalType::freezing:
    return "EX5AbnormalStatusType_Freezing";
  case AbnormalType::electric_shock:
    return "EX5AbnormalStatusType_ElectricShock";
  case AbnormalType::bleeding:
    return "EX5AbnormalStatusType_Bleeding";
  case AbnormalType::poisoning:
    return "EX5AbnormalStatusType_Poisoning";
  case AbnormalType::stun:
    return "EX5AbnormalStatusType_Stun";
  case AbnormalType::shrink:
    return "EX5AbnormalStatusType_Shrink";
  }
  return "EX5AbnormalStatusType_None";
}

AbnormalGaugeRules AbnormalGaugeRules::parse(const Json &document) {
  if (!document.is_object() || !document.contains("types") || !document.at("types").is_object()) {
    throw std::runtime_error("Invalid abnormal gauge rules");
  }

  AbnormalGaugeRules rules;
  rules.reduce_tick_ms = bounded_integer(document, "reduce_tick_ms", 1, 60000);
  for (const auto &[name, row] : document.at("types").items()) {
    const auto type = abnormal_type_from_name(name);
    if (!type || !row.is_object()) {
      throw std::runtime_error("Unknown abnormal gauge type: " + name);
    }
    AbnormalGaugeRule rule;
    rule.maximum = bounded_integer(row, "maximum", 1, kMaximumGaugeValue);
    rule.reduce_per_tick = bounded_integer(row, "reduce_per_tick", 0, kMaximumGaugeValue);
    rule.status_effect_id = bounded_integer(row, "status_effect_id", 1, 2147483647);
    rules.types.emplace(*type, rule);
  }
  return rules;
}

std::vector<std::pair<AbnormalType, int>> abnormal_attacks(const Json &parameters) {
  std::vector<std::pair<AbnormalType, int>> attacks;
  for (const auto &[type, name] : kAbnormalNames) {
    const double value = parameter(parameters, "abnormalStatus" + std::string(name) + "Attack");
    if (value > 0.0) {
      attacks.emplace_back(type, static_cast<int>(std::min(value, 1.0 * kMaximumGaugeValue)));
    }
  }
  return attacks;
}

std::vector<std::pair<AbnormalType, int>> abnormal_gauge_adds(const Json &parameters) {
  std::vector<std::pair<AbnormalType, int>> adds;
  for (const auto &[type, key] : kGaugeAddKeys) {
    const double value = parameter(parameters, key);
    if (value != 0.0) {
      const double bounded = std::clamp(value, -1.0 * kMaximumGaugeValue, 1.0 * kMaximumGaugeValue);
      adds.emplace_back(type, static_cast<int>(bounded));
    }
  }
  return adds;
}

int AbnormalGauges::value(AbnormalType type, const AbnormalGaugeRules &rules,
                          std::int64_t ticks) const {
  const auto gauge = gauges_.find(type);
  const auto rule = rules.types.find(type);
  if (gauge == gauges_.end() || rule == rules.types.end()) {
    return 0;
  }

  // Native world ticks are 100 ns units.
  constexpr std::int64_t kTicksPerMillisecond = 10000;
  const std::int64_t elapsed_ms =
      std::max<std::int64_t>(0, ticks - gauge->second.updated_tick) / kTicksPerMillisecond;
  const std::int64_t reductions = elapsed_ms / rules.reduce_tick_ms;
  const std::int64_t decayed =
      static_cast<std::int64_t>(gauge->second.value) - reductions * rule->second.reduce_per_tick;
  return static_cast<int>(std::max<std::int64_t>(0, decayed));
}

void AbnormalGauges::set(AbnormalType type, int value, std::int64_t ticks) {
  gauges_[type] = Gauge{std::max(0, value), ticks};
}

void AbnormalGauges::clear() {
  gauges_.clear();
}

} // namespace chronicles::combat_rules
