#include "world/features/inventory/inventory_encumbrance.hpp"

#include "data/client_catalog.hpp"
#include "common/clock_ticks.hpp"
#include "cms.generated.pb.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::uint16_t kChangeStatNotification = 47;
constexpr std::uint16_t kAddStatusEffectNotification = 49;
constexpr std::uint16_t kRemoveStatusEffectNotification = 51;

constexpr std::string_view kHeavyStatusVariable = "StatusEffectIdForInvWeightHeavy";
constexpr std::string_view kVeryHeavyStatusVariable = "StatusEffectIdForInvWeightVeryHeavy";
constexpr std::string_view kHardCeilingVariable = "BagMaxWeightMultiplier";
constexpr std::string_view kVeryHeavyRatioVariable = "InvWeightMoveSpeedStep2RatioFrom";

// The ceiling is a percentage of the safe weight; below 100 it would lie under the safe weight.
constexpr std::int64_t kMinimumHardCeilingPercent = 100;
constexpr std::int64_t kMaximumHardCeilingPercent = 100'000;
constexpr int kPermille = 1000;

// Encumbrance statuses last until the weight drops. Observed: the official server sends
// expire_tick INT64_MAX for statuses without a catalog duration (50030001/50030002 adds).
constexpr std::string_view kPermanentExpireTick = "9223372036854775807";

// Candidate: instance ids are per-unit counters on the official server (observed small values
// from 1). Combat statuses use this emulator's counter from 1, so encumbrance instances use a
// disjoint range to stay unique on the same player unit.
constexpr std::uint32_t kFirstEncumbranceInstanceId = 0x4000'0000U;

const Json *find_by(const std::vector<Json> &rows, std::string_view field, std::string_view value) {
  const std::string key(field);
  for (const auto &row : rows) {
    const auto found = row.find(key);
    if (found != row.end() && found->is_string() && found->get<std::string>() == value) {
      return &row;
    }
  }
  return nullptr;
}

std::string text_field(const Json &row, std::string_view field, std::string_view context) {
  const auto found = row.find(std::string(field));
  if (found == row.end() || !found->is_string()) {
    throw std::runtime_error("Missing encumbrance catalog field " + std::string(field) + " in " +
                             std::string(context));
  }
  return found->get<std::string>();
}

std::int64_t parse_integer(const std::string &text, std::string_view context) {
  std::int64_t value{};
  const auto *end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
    throw std::runtime_error("Invalid encumbrance catalog integer in " + std::string(context) +
                             ": " + text);
  }
  return value;
}

int parse_int(const std::string &text, std::string_view context) {
  const auto value = parse_integer(text, context);
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
    throw std::runtime_error("Encumbrance catalog value out of range in " + std::string(context));
  }
  return static_cast<int>(value);
}

std::optional<std::string> game_variable(const EncumbranceCatalogRows &rows,
                                         std::string_view name) {
  const auto *row = find_by(rows.game_variables, "name", name);
  if (!row) {
    return std::nullopt;
  }
  return text_field(*row, "value", name);
}

std::string required_game_variable(const EncumbranceCatalogRows &rows, std::string_view name) {
  auto value = game_variable(rows, name);
  if (!value) {
    throw std::runtime_error("Missing encumbrance GameVariable " + std::string(name));
  }
  return *value;
}

// A ratio in [0, 1] as an integer per mille ("0.5" -> 500).
int parse_ratio_permille(const std::string &text, std::string_view context) {
  double ratio{};
  const auto *end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, ratio);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(ratio) ||
      ratio < 0.0 || ratio > 1.0) {
    throw std::runtime_error("Invalid encumbrance ratio in " + std::string(context) + ": " + text);
  }
  return static_cast<int>(std::lround(ratio * kPermille));
}

// Catalog id lists are written "(22,23,24)"; "()" is an empty list.
std::vector<std::string> parse_id_list(const std::string &text, std::string_view context) {
  if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
    throw std::runtime_error("Invalid encumbrance id list in " + std::string(context) + ": " +
                             text);
  }

  std::vector<std::string> ids;
  const std::string_view inner(text.data() + 1, text.size() - 2);
  std::size_t start = 0;
  while (start < inner.size()) {
    const auto comma = inner.find(',', start);
    const auto stop = comma == std::string_view::npos ? inner.size() : comma;
    const auto id = std::string(inner.substr(start, stop - start));
    if (id.empty()) {
      throw std::runtime_error("Empty id in encumbrance id list " + std::string(context));
    }
    ids.push_back(id);
    start = stop + 1;
  }
  return ids;
}

int resolve_stat_type(const std::string &name) {
  x5::cms::EX5StatType value{};
  if (!x5::cms::EX5StatType_Parse("EX5StatType_" + name, &value) ||
      value == x5::cms::EX5StatType_None) {
    throw std::runtime_error("Unknown encumbrance stat type: " + name);
  }
  return static_cast<int>(value);
}

// One MaintainStat row as a flat modifier. Only statAdd is representable: the encumbrance
// statuses use it and the unit of statMul is not established, so statMul rows are refused.
EncumbranceStatModifier parse_stat_modifier(const Json &maintain_stat) {
  const auto id = text_field(maintain_stat, "id", "MaintainStat");
  const auto multiplier = maintain_stat.value("statMul", std::string{});
  if (!multiplier.empty()) {
    throw std::runtime_error("Unsupported statMul in encumbrance MaintainStat " + id);
  }

  EncumbranceStatModifier modifier;
  modifier.stat_type = resolve_stat_type(text_field(maintain_stat, "statType", id));
  modifier.add = parse_int(text_field(maintain_stat, "statAdd", id), id);
  return modifier;
}

// Resolves StatusEffect.FK_maintainEffects -> MaintainEffect (type Stat) -> MaintainStat.
EncumbranceTier parse_tier(const EncumbranceCatalogRows &rows, std::string_view variable) {
  const auto status_text = required_game_variable(rows, variable);

  EncumbranceTier tier;
  tier.status_effect_id = parse_int(status_text, variable);
  if (tier.status_effect_id <= 0) {
    throw std::runtime_error("Invalid encumbrance status id in " + std::string(variable));
  }

  const auto *status = find_by(rows.status_effects, "id", status_text);
  if (!status) {
    throw std::runtime_error("Missing encumbrance StatusEffect " + status_text);
  }

  const auto maintain_ids =
      parse_id_list(text_field(*status, "FK_maintainEffects", status_text), status_text);
  for (const auto &maintain_id : maintain_ids) {
    const auto *maintain_effect = find_by(rows.maintain_effects, "id", maintain_id);
    if (!maintain_effect) {
      throw std::runtime_error("Missing encumbrance MaintainEffect " + maintain_id);
    }
    if (text_field(*maintain_effect, "maintainEffectType", maintain_id) != "Stat") {
      throw std::runtime_error("Unsupported encumbrance MaintainEffect type in " + maintain_id);
    }

    const auto stat_id = text_field(*maintain_effect, "maintainEffectValue", maintain_id);
    const auto *maintain_stat = find_by(rows.maintain_stats, "id", stat_id);
    if (!maintain_stat) {
      throw std::runtime_error("Missing encumbrance MaintainStat " + stat_id);
    }
    tier.modifiers.push_back(parse_stat_modifier(*maintain_stat));
  }
  return tier;
}

std::map<int, EncumbranceStatLimit> parse_stat_limits(const EncumbranceCatalogRows &rows,
                                                      const EncumbranceTier &heavy,
                                                      const EncumbranceTier &very_heavy) {
  std::set<int> modified_stats;
  for (const auto *tier : {&heavy, &very_heavy}) {
    for (const auto &modifier : tier->modifiers) {
      modified_stats.insert(modifier.stat_type);
    }
  }

  std::map<int, EncumbranceStatLimit> limits;
  for (const auto &row : rows.stats) {
    const auto name = text_field(row, "id", "Stat");
    const auto stat_type = resolve_stat_type(name);
    if (!modified_stats.contains(stat_type)) {
      continue;
    }

    EncumbranceStatLimit limit;
    limit.minimum = parse_int(text_field(row, "min", name), name);
    limit.maximum = parse_int(text_field(row, "max", name), name);
    if (limit.minimum > limit.maximum || !limits.emplace(stat_type, limit).second) {
      throw std::runtime_error("Invalid or duplicate encumbrance stat limit " + name);
    }
  }
  return limits;
}

void append_unique_stats(const EncumbranceTier *tier, std::set<int> &stat_types) {
  if (!tier) {
    return;
  }

  for (const auto &modifier : tier->modifiers) {
    stat_types.insert(modifier.stat_type);
  }
}

Json status_effect_json(const AppliedEncumbranceStatus &status, std::uint64_t unit_id) {
  return {{"status_effect_id", status.status_effect_id},
          {"instance_id", status.instance_id},
          {"level", 1},
          {"stack_count", 1},
          {"caster_unit_id", std::to_string(unit_id)},
          {"start_tick", wire_monotonic_tick(status.start_tick)},
          {"expire_tick", std::string(kPermanentExpireTick)}};
}

} // namespace

const char *encumbrance_level_name(EncumbranceLevel level) {
  switch (level) {
  case EncumbranceLevel::none:
    return "none";
  case EncumbranceLevel::heavy:
    return "heavy";
  case EncumbranceLevel::very_heavy:
    return "very_heavy";
  }
  return "unknown";
}

std::shared_ptr<const EncumbranceRules> EncumbranceRules::load(ClientCatalog &catalog) {
  for (const char *table :
       {"GameVariable", "StatusEffect", "MaintainEffect", "MaintainStat", "Stat"}) {
    if (!catalog.has_table(table)) {
      spdlog::warn("Carried-weight encumbrance disabled: catalog has no {} table", table);
      return nullptr;
    }
  }

  EncumbranceCatalogRows rows;
  rows.game_variables = catalog.rows("GameVariable");

  // Only the two named statuses are needed from the large StatusEffect table.
  for (const auto variable : {kHeavyStatusVariable, kVeryHeavyStatusVariable}) {
    const auto status_id = game_variable(rows, variable);
    if (!status_id) {
      continue;
    }

    auto status = catalog.find_row("StatusEffect", "id", *status_id);
    if (status) {
      rows.status_effects.push_back(std::move(*status));
    }
  }
  rows.maintain_effects = catalog.rows("MaintainEffect");
  rows.maintain_stats = catalog.rows("MaintainStat");
  rows.stats = catalog.rows("Stat");
  return parse(rows);
}

std::shared_ptr<const EncumbranceRules>
EncumbranceRules::parse(const EncumbranceCatalogRows &rows) {
  const bool names_heavy = game_variable(rows, kHeavyStatusVariable).has_value();
  const bool names_very_heavy = game_variable(rows, kVeryHeavyStatusVariable).has_value();
  if (!names_heavy && !names_very_heavy) {
    spdlog::warn("Carried-weight encumbrance disabled: catalog names no weight statuses");
    return nullptr;
  }

  auto heavy = parse_tier(rows, kHeavyStatusVariable);
  auto very_heavy = parse_tier(rows, kVeryHeavyStatusVariable);

  const auto ceiling_text = required_game_variable(rows, kHardCeilingVariable);
  const auto hard_ceiling_percent = parse_integer(ceiling_text, kHardCeilingVariable);
  if (hard_ceiling_percent < kMinimumHardCeilingPercent ||
      hard_ceiling_percent > kMaximumHardCeilingPercent) {
    throw std::runtime_error("Encumbrance BagMaxWeightMultiplier out of range: " + ceiling_text);
  }

  const auto very_heavy_from_permille = parse_ratio_permille(
      required_game_variable(rows, kVeryHeavyRatioVariable), kVeryHeavyRatioVariable);

  auto limits = parse_stat_limits(rows, heavy, very_heavy);
  return std::make_shared<const EncumbranceRules>(hard_ceiling_percent, very_heavy_from_permille,
                                                  std::move(heavy), std::move(very_heavy),
                                                  std::move(limits));
}

EncumbranceRules::EncumbranceRules(std::int64_t hard_ceiling_percent,
                                   int very_heavy_from_permille, EncumbranceTier heavy,
                                   EncumbranceTier very_heavy,
                                   std::map<int, EncumbranceStatLimit> stat_limits)
    : hard_ceiling_percent_(hard_ceiling_percent),
      very_heavy_from_permille_(very_heavy_from_permille), heavy_(std::move(heavy)),
      very_heavy_(std::move(very_heavy)), stat_limits_(std::move(stat_limits)) {
  if (hard_ceiling_percent_ < kMinimumHardCeilingPercent ||
      hard_ceiling_percent_ > kMaximumHardCeilingPercent) {
    throw std::invalid_argument("Encumbrance hard ceiling percent out of range");
  }
  if (very_heavy_from_permille_ < 0 || very_heavy_from_permille_ > kPermille) {
    throw std::invalid_argument("Encumbrance very-heavy ratio out of range");
  }

  // Every modified stat is clamped like CharacterStats clamps catalog stats.
  for (const auto *applied : {&heavy_, &very_heavy_}) {
    if (applied->status_effect_id <= 0) {
      throw std::invalid_argument("Encumbrance tier requires a status effect id");
    }
    for (const auto &modifier : applied->modifiers) {
      if (!stat_limits_.contains(modifier.stat_type)) {
        throw std::invalid_argument("Missing Stat limit for encumbrance stat " +
                                    std::to_string(modifier.stat_type));
      }
    }
  }
}

EncumbranceLevel EncumbranceRules::classify(std::int64_t carried_weight,
                                            std::int64_t safe_weight) const {
  if (carried_weight < 0 || safe_weight < 0) {
    throw std::invalid_argument("Encumbrance weights must not be negative");
  }
  if (carried_weight <= safe_weight) {
    return EncumbranceLevel::none;
  }

  // band = safe * (percent - 100) / 100, computed without overflowing for large safe weights.
  const auto excess_percent = hard_ceiling_percent_ - 100;
  const auto band = safe_weight / 100 * excess_percent + safe_weight % 100 * excess_percent / 100;

  // The VeryHeavy threshold inside the band; band / 1000 keeps band * permille in range.
  const auto threshold_offset = band / kPermille * very_heavy_from_permille_ +
                                band % kPermille * very_heavy_from_permille_ / kPermille;
  const auto overweight = carried_weight - safe_weight;
  if (overweight >= threshold_offset) {
    return EncumbranceLevel::very_heavy;
  }
  return EncumbranceLevel::heavy;
}

const EncumbranceTier *EncumbranceRules::tier(EncumbranceLevel level) const {
  switch (level) {
  case EncumbranceLevel::none:
    return nullptr;
  case EncumbranceLevel::heavy:
    return &heavy_;
  case EncumbranceLevel::very_heavy:
    return &very_heavy_;
  }
  return nullptr;
}

void EncumbranceRules::apply_modifiers(EncumbranceLevel level, Json &stats) const {
  const auto *applied = tier(level);
  if (!applied) {
    return;
  }

  for (const auto &modifier : applied->modifiers) {
    const auto key = std::to_string(modifier.stat_type);
    const auto current = stats.find(key);
    if (current == stats.end() || !current->is_number_integer()) {
      continue;
    }

    const auto &limit = stat_limits_.at(modifier.stat_type);
    const auto modified = static_cast<std::int64_t>(current->get<int>()) + modifier.add;
    const auto clamped = std::clamp<std::int64_t>(modified, limit.minimum, limit.maximum);
    *current = static_cast<int>(clamped);
  }
}

std::int64_t EncumbranceRules::hard_ceiling_percent() const {
  return hard_ceiling_percent_;
}

int EncumbranceRules::very_heavy_from_permille() const {
  return very_heavy_from_permille_;
}

EncumbranceState::EncumbranceState(std::shared_ptr<const EncumbranceRules> rules)
    : rules_(std::move(rules)), next_instance_id_(kFirstEncumbranceInstanceId) {
  if (!rules_) {
    throw std::invalid_argument("Encumbrance state requires rules");
  }
}

const EncumbranceRules &EncumbranceState::rules() const {
  return *rules_;
}

EncumbranceLevel EncumbranceState::level() const {
  return level_;
}

void EncumbranceState::apply_to(Json &stats) const {
  rules_->apply_modifiers(level_, stats);
}

std::optional<EncumbranceTransition> EncumbranceState::change_level(EncumbranceLevel next,
                                                                    std::int64_t tick) {
  if (next == level_) {
    return std::nullopt;
  }

  EncumbranceTransition transition;
  transition.previous = level_;
  transition.current = next;
  transition.tick = tick;
  transition.removed = applied_;

  const auto *next_tier = rules_->tier(next);
  if (next_tier) {
    AppliedEncumbranceStatus status;
    status.status_effect_id = next_tier->status_effect_id;
    status.instance_id = allocate_instance_id();
    status.start_tick = tick;
    transition.added = status;
  }

  std::set<int> stat_types;
  append_unique_stats(rules_->tier(level_), stat_types);
  append_unique_stats(next_tier, stat_types);
  transition.changed_stat_types.assign(stat_types.begin(), stat_types.end());

  level_ = next;
  applied_ = transition.added;
  return transition;
}

std::uint32_t EncumbranceState::allocate_instance_id() {
  const auto instance_id = next_instance_id_;
  if (next_instance_id_ == std::numeric_limits<std::uint32_t>::max()) {
    next_instance_id_ = kFirstEncumbranceInstanceId;
  } else {
    ++next_instance_id_;
  }
  return instance_id;
}

std::vector<GameplayPacket> encumbrance_transition_packets(const EncumbranceTransition &transition,
                                                           std::uint64_t unit_id,
                                                           const Json &current_stats) {
  std::vector<GameplayPacket> packets;
  const auto unit = std::to_string(unit_id);
  const auto tick = wire_monotonic_tick(transition.tick);

  Json changes = Json::array();
  for (const int stat_type : transition.changed_stat_types) {
    const auto value = current_stats.find(std::to_string(stat_type));
    if (value == current_stats.end() || !value->is_number_integer()) {
      continue;
    }
    changes.push_back({{"stat_type", stat_type}, {"result", value->get<int>()}});
  }
  if (!changes.empty()) {
    packets.push_back({kChangeStatNotification,
                       {{"unit_id", unit},
                        {"tick", tick},
                        {"default_source", "SCS_StatusEffect"},
                        {"stat_changes", std::move(changes)}}});
  }

  if (transition.removed) {
    packets.push_back({kRemoveStatusEffectNotification,
                       {{"unit_id", unit},
                        {"tick", tick},
                        {"status_effects",
                         {{{"instance_id", transition.removed->instance_id},
                           {"status_effect_id", transition.removed->status_effect_id}}}}}});
  }

  if (transition.added) {
    packets.push_back({kAddStatusEffectNotification,
                       {{"unit_id", unit},
                        {"status_effect", status_effect_json(*transition.added, unit_id)}}});
  }
  return packets;
}

} // namespace chronicles
