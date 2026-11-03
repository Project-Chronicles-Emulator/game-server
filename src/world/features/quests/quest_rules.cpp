#include "world/features/quests/quest_rules.hpp"

#include "common/currency.hpp"
#include "world/core/game_rates.hpp"
#include "world/features/character/progression.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>

namespace chronicles::quest_rules {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumDistinctStacks = 1000;

const char *quest_state_name(const QuestProgress &quest) {
  return quest.complete ? "EX5QuestStateType_Complete" : "EX5QuestStateType_InProgress";
}

// Wire form of MoneyChange: observed Gold entries omit money_type; other types name it.
Json money_change_json(const MoneyChange &change) {
  Json entry{{"change", std::to_string(change.change)}, {"result", std::to_string(change.result)}};
  if (!change.omit_type_on_wire) {
    require(change.type == MoneyType::HersanderToken, "BadRequest");
    entry["money_type"] = "EX5MoneyType_HersanderToken";
  }
  return entry;
}

// Credits Gold or Hersander tokens through the currency module.
// CUSTOM: a reward that would pass the wallet limit (kMoneyLimit, the catalog Money limit) fills
// the wallet to the limit instead of refusing the turn-in. The official behaviour is not
// captured; refusing made every gold-paying quest impossible to finish for a character whose
// wallet was full, because each claim was answered with BadRequest and nothing was committed.
void credit_money(WorldProgress &next, MoneyType type, std::int64_t quantity, Json &update) {
  const auto balance = type == MoneyType::HersanderToken ? next.hersander_token : next.gold;
  const auto headroom = std::max<std::int64_t>(0, kMoneyLimit - balance);
  const auto credited = std::min(quantity, headroom);
  if (credited <= 0) {
    return;
  }

  change_money(next, type, credited, update, "BadRequest");
}

} // namespace

void change_money(WorldProgress &next, MoneyType type, std::int64_t delta, Json &update,
                  const char *code) {
  CurrencyBalances balances;
  balances.gold = next.gold;
  balances.hersander_token = next.hersander_token;
  const auto applied = apply_money_change(balances, type, delta);
  require(applied.ok, code);
  if (type == MoneyType::HersanderToken) {
    next.hersander_token = applied.change.result;
  } else {
    next.gold = applied.change.result;
  }
  update["money_changes"].push_back(money_change_json(applied.change));
}

void require(bool condition, const char *code) {
  if (!condition) {
    throw RuleError{code};
  }
}

double distance_squared(const WorldPosition &position, const Json &point) {
  const double delta_x = static_cast<double>(position.x_mm) - point.at(0).get<double>();
  const double delta_y = static_cast<double>(position.y_mm) - point.at(1).get<double>();
  const double delta_z = static_cast<double>(position.z_mm) - point.at(2).get<double>();
  return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;
}

bool within_discovery_range(const WorldPosition &position, double target_x_mm, double target_y_mm) {
  const double delta_x = static_cast<double>(position.x_mm) - target_x_mm;
  const double delta_y = static_cast<double>(position.y_mm) - target_y_mm;
  const double limit_mm = kDiscoveryRangeMm + kPositionToleranceMm;

  return delta_x * delta_x + delta_y * delta_y <= limit_mm * limit_mm;
}

bool within_discovery_range(const WorldPosition &position, const Json &point) {
  return within_discovery_range(position, point.at(0).get<double>(), point.at(1).get<double>());
}

bool request_position_matches(const Json &request, const WorldPosition &position) {
  if (!request.contains("pos")) {
    return false;
  }
  const auto &point = request.at("pos");
  const Json coordinates = {point.value("millimeter_x", 0), point.value("millimeter_y", 0),
                            point.value("millimeter_z", 0)};
  return distance_squared(position, coordinates) <= kPositionToleranceMm * kPositionToleranceMm;
}

bool position_within_trigger(const Json &trigger, const WorldPosition &position,
                             double tolerance_mm) {
  if (trigger.contains("radius")) {
    const double radius = std::max(0.0, trigger.at("radius").get<double>() + tolerance_mm);
    return distance_squared(position, trigger.at("center")) <= radius * radius;
  }
  const double delta_x =
      static_cast<double>(position.x_mm) - trigger.at("center").at(0).get<double>();
  const double delta_y =
      static_cast<double>(position.y_mm) - trigger.at("center").at(1).get<double>();
  const double delta_z =
      static_cast<double>(position.z_mm) - trigger.at("center").at(2).get<double>();
  const double radians = trigger.at("yaw").get<double>() * std::numbers::pi / 180.0;
  const double local_x = std::cos(radians) * delta_x + std::sin(radians) * delta_y;
  const double local_y = -std::sin(radians) * delta_x + std::cos(radians) * delta_y;
  return std::abs(local_x) <= trigger.at("extent").at(0).get<double>() + tolerance_mm &&
         std::abs(local_y) <= trigger.at("extent").at(1).get<double>() + tolerance_mm &&
         std::abs(delta_z) <= trigger.at("extent").at(2).get<double>() + tolerance_mm;
}

Json quest_json(int cms_id, const QuestProgress &quest) {
  Json progresses = Json::array();
  for (const auto &[sequence, value] : quest.objectives) {
    progresses.push_back({{"sequence_id", sequence}, {"value", value}});
  }
  return {{"cms_id", cms_id},
          {"state", quest_state_name(quest)},
          {"success_count", quest.success_count},
          {"progresses", std::move(progresses)}};
}

// Official QuestUpdateNoti for a 20019 progress (capture records 320, 1339, 1361) lists only the
// objective that changed; the client store upserts objectives by key. The full list is sent by
// the snapshot (136) and by the completion update.
Json quest_progress_update_json(int cms_id, const QuestProgress &quest, int sequence_id) {
  return {{"cms_id", cms_id},
          {"state", quest_state_name(quest)},
          {"success_count", quest.success_count},
          {"progresses", Json::array({{{"sequence_id", sequence_id},
                                       {"value", quest.objectives.at(sequence_id)}}})}};
}

int objective_required_count(const Json &objective) {
  return objective.at("parameters").value("count", 1);
}

bool objective_ready(const Json &definition, const QuestProgress &quest, int group) {
  if (!definition.at("sequential").get<bool>()) {
    return true;
  }
  std::map<int, bool> groups;
  for (const auto &objective : definition.at("progresses")) {
    const int objective_group = objective.at("group");
    if (objective_group >= group) {
      continue;
    }
    const int sequence = objective.at("sequence");
    const auto value = quest.objectives.find(sequence);
    groups[objective_group] =
        groups[objective_group] || (value != quest.objectives.end() &&
                                    value->second >= objective_required_count(objective));
  }
  return std::ranges::all_of(groups, [](const auto &entry) { return entry.second; });
}

bool objectives_complete(const Json &definition, const QuestProgress &quest) {
  std::map<int, bool> groups;
  for (const auto &objective : definition.at("progresses")) {
    const auto value = quest.objectives.find(objective.at("sequence").get<int>());
    const int group = objective.at("group");
    groups[group] = groups[group] || (value != quest.objectives.end() &&
                                      value->second >= objective_required_count(objective));
  }
  return std::ranges::all_of(groups, [](const auto &entry) { return entry.second; });
}

bool objective_group_complete(const Json &definition, const QuestProgress &quest, int group) {
  for (const auto &objective : definition.at("progresses")) {
    if (objective.at("group").get<int>() != group) {
      continue;
    }

    const auto value = quest.objectives.find(objective.at("sequence").get<int>());
    if (value != quest.objectives.end() && value->second >= objective_required_count(objective)) {
      return true;
    }
  }
  return false;
}

bool quest_requirement_met(const QuestLookup &lookup, const Json &condition,
                           const WorldProgress &state) {
  const int quest_id = condition.at("quest_id");
  const auto requirement = condition.at("requirement").get<std::string>();
  const auto saved = state.quests.find(quest_id);
  // An abandoned record is a tombstone: the quest counts as never accepted.
  const bool accepted = saved != state.quests.end() && !saved->second.abandoned;
  const bool complete = accepted && saved->second.complete;

  if (requirement == "Acceptable") {
    return !accepted;
  }
  if (requirement == "QuestComplete") {
    return complete;
  }

  const auto *definition = lookup(quest_id);
  if (!accepted || !definition) {
    return false;
  }
  if (!condition.contains("progress_group")) {
    return requirement == "Progressing" ? !complete : true;
  }

  const int group = condition.at("progress_group");
  const bool group_done = complete || objective_group_complete(*definition, saved->second, group);
  if (requirement == "QuestProgress") {
    return group_done;
  }
  return !complete && !group_done && objective_ready(*definition, saved->second, group);
}

bool grouped_conditions_pass(const Json &conditions,
                             const std::function<bool(const Json &)> &evaluate) {
  if (conditions.empty()) {
    return true;
  }

  std::map<int, std::vector<const Json *>> groups;
  for (const auto &condition : conditions) {
    groups[condition.at("group").get<int>()].push_back(&condition);
  }

  for (const auto &[group, members] : groups) {
    const bool require_all = members.front()->value("operator", std::string("AND")) == "AND";
    bool group_result = require_all;
    for (const auto *condition : members) {
      const bool met = evaluate(*condition) != condition->value("negated", false);
      if (require_all) {
        group_result = group_result && met;
      } else {
        group_result = group_result || met;
      }
    }
    if (group_result) {
      return true;
    }
  }
  return false;
}

void grant_rewards(WorldProgress &next, const Json &rewards, std::vector<GameplayPacket> &packets,
                   const char *reason) {
  Json update{{"stackables", Json::array()}, {reason, Json::object()}};
  for (const auto &reward : rewards) {
    const int cms_id = reward.at("item_cms_id");
    const int quantity = reward.at("quantity");
    require(quantity > 0, "BadRequest");
    if (cms_id == 0) {
      const bool token = reward.value("money_type", std::string()) == "HersanderToken";
      credit_money(next, token ? MoneyType::HersanderToken : MoneyType::Gold, quantity, update);
      continue;
    }

    require(next.stackables.size() < kMaximumDistinctStacks || next.stackables.contains(cms_id),
            "BadRequest");
    auto &owned = next.stackables[cms_id];
    require(owned <= std::numeric_limits<int>::max() - quantity, "BadRequest");
    owned += quantity;
    update["stackables"].push_back({{"item_cms_id", cms_id}, {"quantity", owned}});
  }
  if (!rewards.empty()) {
    packets.push_back({kItemsUpdatedOpcode, std::move(update)});
  }
}

void grant_experience(WorldProgress &next, std::int64_t catalog_experience,
                      std::vector<GameplayPacket> &packets, const Progression *progression) {
  require(catalog_experience >= 0, "BadRequest");
  // Operator EXP rate (game_rates.hpp); the default 100% keeps the catalog amount.
  const auto experience = scale_experience_reward(catalog_experience);
  if (experience == 0) {
    return;
  }

  if (!progression) {
    require(next.experience <= std::numeric_limits<std::int64_t>::max() - experience,
            "BadRequest");
    next.experience += experience;
    packets.push_back(
        {kCharacterBaseChangedOpcode, {{"exp", std::to_string(next.experience)}}});
    return;
  }

  const auto change = progression->award_experience(next, experience);
  if (change.changed()) {
    packets.push_back({kCharacterBaseChangedOpcode, experience_change_message(next, change)});
  }
}

} // namespace chronicles::quest_rules
