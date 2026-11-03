#include "world/features/loot/loot_rules.hpp"

#include "distribution/distribution.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumRulesFileBytes = 16 * 1024;

// Position tolerance and party range stay within a few tens of metres; anything larger would
// let a client loot from across the map.
constexpr double kMaximumToleranceMm = 100000.0;
constexpr std::int64_t kMaximumLifetimeMs = 24LL * 60 * 60 * 1000;

struct RuleName {
  LootRule rule;
  std::string_view name;
};

constexpr std::array<RuleName, 7> kRuleNames{{
    {LootRule::kill_gold, "kill_gold"},
    {LootRule::first_kill_rewards, "first_kill_rewards"},
    {LootRule::quest_gated_rows, "quest_gated_rows"},
    {LootRule::full_moon_stones, "full_moon_stones"},
    {LootRule::polish_step_rows, "polish_step_rows"},
    {LootRule::handling_loot, "handling_loot"},
    {LootRule::party_shared_loot, "party_shared_loot"},
}};

constexpr std::array<std::string_view, 10> kDetailKeys{{
    "range_tolerance_mm",
    "repeated_take_error",
    "full_bag_error",
    "count_limit_error",
    "over_weight_error",
    "empty_roll_reply",
    "lootable_lifetime_ms",
    "emptied_lifetime_ms",
    "emptied_lifetime_anchor",
    "party_range_mm",
}};

bool is_known_key(const std::string &key) {
  for (const auto &entry : kRuleNames) {
    if (entry.name == key) {
      return true;
    }
  }
  for (const auto &detail : kDetailKeys) {
    if (detail == key) {
      return true;
    }
  }
  return false;
}

bool read_bool(const Json &value, const std::string &key) {
  if (!value.is_boolean()) {
    throw std::invalid_argument("Loot custom rule " + key + " must be true or false");
  }
  return value.get<bool>();
}

double read_distance_mm(const Json &value, const std::string &key) {
  if (!value.is_number()) {
    throw std::invalid_argument("Loot custom rule " + key + " must be a number");
  }

  const auto distance = value.get<double>();
  if (!std::isfinite(distance) || distance < 0 || distance > kMaximumToleranceMm) {
    throw std::invalid_argument("Loot custom rule " + key + " is out of range");
  }
  return distance;
}

std::int64_t read_lifetime_ms(const Json &value, const std::string &key) {
  if (!value.is_number_integer()) {
    throw std::invalid_argument("Loot custom rule " + key + " must be an integer");
  }

  const auto milliseconds = value.get<std::int64_t>();
  if (milliseconds <= 0 || milliseconds > kMaximumLifetimeMs) {
    throw std::invalid_argument("Loot custom rule " + key + " is out of range");
  }
  return milliseconds;
}

x5::ec::EErrorCode read_error(const Json &value, const std::string &key) {
  if (!value.is_string()) {
    throw std::invalid_argument("Loot custom rule " + key + " must be an error code name");
  }

  x5::ec::EErrorCode code{};
  if (!x5::ec::EErrorCode_Parse(value.get<std::string>(), &code) || code == x5::ec::Success) {
    throw std::invalid_argument("Loot custom rule " + key + " names an unknown error code");
  }
  return code;
}

template <typename Enum>
Enum read_choice(const Json &value, const std::string &key,
                 std::initializer_list<std::pair<std::string_view, Enum>> choices) {
  if (value.is_string()) {
    const auto &text = value.get_ref<const std::string &>();
    for (const auto &[name, choice] : choices) {
      if (name == text) {
        return choice;
      }
    }
  }
  throw std::invalid_argument("Loot custom rule " + key + " has an unsupported value");
}

std::string_view empty_roll_name(EmptyRollReply reply) {
  return reply == EmptyRollReply::success ? "success" : "no_loot_error";
}

std::string_view anchor_name(EmptiedLifetimeAnchor anchor) {
  return anchor == EmptiedLifetimeAnchor::take ? "take" : "death";
}

} // namespace

std::string_view loot_rule_name(LootRule rule) {
  for (const auto &entry : kRuleNames) {
    if (entry.rule == rule) {
      return entry.name;
    }
  }
  throw std::logic_error("Unnamed loot rule");
}

LootRule parse_loot_rule(std::string_view name) {
  for (const auto &entry : kRuleNames) {
    if (entry.name == name) {
      return entry.rule;
    }
  }
  throw std::runtime_error("Unknown loot rule: " + std::string(name));
}

LootCustomRules LootCustomRules::all_enabled() {
  return LootCustomRules{};
}

LootCustomRules LootCustomRules::evidence_only() {
  LootCustomRules rules;
  rules.kill_gold = false;
  rules.first_kill_rewards = false;
  rules.quest_gated_rows = false;
  rules.full_moon_stones = false;
  rules.polish_step_rows = false;
  rules.handling_loot = false;
  rules.party_shared_loot = false;
  rules.range_tolerance_mm = 0.0;
  return rules;
}

LootCustomRules LootCustomRules::from_json(const Json &custom_rules) {
  if (!custom_rules.is_object()) {
    throw std::invalid_argument("Loot custom_rules must be an object");
  }

  for (const auto &[key, value] : custom_rules.items()) {
    if (!is_known_key(key)) {
      throw std::invalid_argument("Unknown loot custom rule: " + key);
    }
  }

  LootCustomRules rules;
  const auto set_rule = [&](LootRule rule, bool &target) {
    const std::string key(loot_rule_name(rule));
    if (custom_rules.contains(key)) {
      target = read_bool(custom_rules.at(key), key);
    }
  };
  set_rule(LootRule::kill_gold, rules.kill_gold);
  set_rule(LootRule::first_kill_rewards, rules.first_kill_rewards);
  set_rule(LootRule::quest_gated_rows, rules.quest_gated_rows);
  set_rule(LootRule::full_moon_stones, rules.full_moon_stones);
  set_rule(LootRule::polish_step_rows, rules.polish_step_rows);
  set_rule(LootRule::handling_loot, rules.handling_loot);
  set_rule(LootRule::party_shared_loot, rules.party_shared_loot);

  if (custom_rules.contains("range_tolerance_mm")) {
    rules.range_tolerance_mm =
        read_distance_mm(custom_rules.at("range_tolerance_mm"), "range_tolerance_mm");
  }
  if (custom_rules.contains("party_range_mm")) {
    rules.party_range_mm = read_distance_mm(custom_rules.at("party_range_mm"), "party_range_mm");
  }
  if (custom_rules.contains("repeated_take_error")) {
    rules.repeated_take_error =
        read_error(custom_rules.at("repeated_take_error"), "repeated_take_error");
  }
  if (custom_rules.contains("full_bag_error")) {
    rules.full_bag_error = read_error(custom_rules.at("full_bag_error"), "full_bag_error");
  }
  if (custom_rules.contains("count_limit_error")) {
    rules.count_limit_error = read_error(custom_rules.at("count_limit_error"), "count_limit_error");
  }
  if (custom_rules.contains("over_weight_error")) {
    rules.over_weight_error = read_error(custom_rules.at("over_weight_error"), "over_weight_error");
  }
  if (custom_rules.contains("empty_roll_reply")) {
    rules.empty_roll_reply = read_choice<EmptyRollReply>(
        custom_rules.at("empty_roll_reply"), "empty_roll_reply",
        {{"success", EmptyRollReply::success}, {"no_loot_error", EmptyRollReply::no_loot_error}});
  }
  if (custom_rules.contains("emptied_lifetime_anchor")) {
    rules.emptied_lifetime_anchor = read_choice<EmptiedLifetimeAnchor>(
        custom_rules.at("emptied_lifetime_anchor"), "emptied_lifetime_anchor",
        {{"take", EmptiedLifetimeAnchor::take}, {"death", EmptiedLifetimeAnchor::death}});
  }
  if (custom_rules.contains("lootable_lifetime_ms")) {
    rules.lootable_lifetime_ms =
        read_lifetime_ms(custom_rules.at("lootable_lifetime_ms"), "lootable_lifetime_ms");
  }
  if (custom_rules.contains("emptied_lifetime_ms")) {
    rules.emptied_lifetime_ms =
        read_lifetime_ms(custom_rules.at("emptied_lifetime_ms"), "emptied_lifetime_ms");
  }
  return rules;
}

LootCustomRules LootCustomRules::load(const std::filesystem::path &path) {
  try {
    const auto document =
        Json::parse(distribution::read_bounded_file(path, kMaximumRulesFileBytes));
    if (!document.is_object() || document.size() != 1 || !document.contains("custom_rules")) {
      throw std::invalid_argument("Loot rules file must be {\"custom_rules\": {...}}");
    }
    return from_json(document.at("custom_rules"));
  } catch (const Json::exception &error) {
    throw std::invalid_argument("Invalid loot rules file " + path.string() + ": " + error.what());
  }
}

bool LootCustomRules::enabled(LootRule rule) const {
  switch (rule) {
  case LootRule::kill_gold:
    return kill_gold;
  case LootRule::first_kill_rewards:
    return first_kill_rewards;
  case LootRule::quest_gated_rows:
    return quest_gated_rows;
  case LootRule::full_moon_stones:
    return full_moon_stones;
  case LootRule::polish_step_rows:
    return polish_step_rows;
  case LootRule::handling_loot:
    return handling_loot;
  case LootRule::party_shared_loot:
    return party_shared_loot;
  }
  throw std::logic_error("Unhandled loot rule");
}

std::vector<LootRule> LootCustomRules::active_rules() const {
  std::vector<LootRule> active;
  for (const auto &entry : kRuleNames) {
    if (enabled(entry.rule)) {
      active.push_back(entry.rule);
    }
  }
  return active;
}

std::string LootCustomRules::describe() const {
  std::string text = "custom rules (guessed, not official): ";
  const auto active = active_rules();
  if (active.empty()) {
    text += "none (evidence-only)";
  }
  for (std::size_t index = 0; index < active.size(); ++index) {
    text += index == 0 ? "" : ",";
    text += loot_rule_name(active[index]);
  }

  text += "; range_tolerance_mm=" + std::to_string(static_cast<std::int64_t>(range_tolerance_mm));
  text += " repeated_take_error=" + x5::ec::EErrorCode_Name(repeated_take_error);
  text += " full_bag_error=" + x5::ec::EErrorCode_Name(full_bag_error);
  text += " count_limit_error=" + x5::ec::EErrorCode_Name(count_limit_error);
  text += " over_weight_error=" + x5::ec::EErrorCode_Name(over_weight_error);
  text += " empty_roll_reply=" + std::string(empty_roll_name(empty_roll_reply));
  text += " emptied_lifetime_anchor=" + std::string(anchor_name(emptied_lifetime_anchor));
  text += " lootable_lifetime_ms=" +
          (lootable_lifetime_ms ? std::to_string(*lootable_lifetime_ms) : std::string("catalog"));
  text += " emptied_lifetime_ms=" +
          (emptied_lifetime_ms ? std::to_string(*emptied_lifetime_ms) : std::string("catalog"));
  if (party_shared_loot) {
    text += " party_range_mm=" + std::to_string(static_cast<std::int64_t>(party_range_mm));
  }
  return text;
}

} // namespace chronicles
