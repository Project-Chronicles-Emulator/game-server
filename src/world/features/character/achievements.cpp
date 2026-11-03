#include "world/features/character/achievements.hpp"

#include "cms.generated.pb.h"
#include "common/currency.hpp"
#include "world/core/game_rates.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace chronicles::character {
namespace {

using Json = nlohmann::json;

int row_id(const Json &row, const char *field) {
  const auto value = catalog_integer(row, field);
  if (!value || *value <= 0 || *value > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Invalid achievement catalog identifier: ") + field);
  }
  return static_cast<int>(*value);
}

std::int64_t positive_value(const Json &parameters, const char *field) {
  const auto value = parameters.at(field).get<std::int64_t>();
  if (value <= 0 || value > std::numeric_limits<int>::max()) {
    throw std::runtime_error("Achievement goal threshold is out of range");
  }
  return value;
}

Json parameters(const Json &row, const char *field) {
  return Json::parse("{" + row.at(field).get<std::string>() + "}");
}

AchievementGoal parse_goal(const Json &row) {
  const auto type = row.at("goalTypeCase").get<std::string>();
  const auto values = parameters(row, "goalType");
  AchievementGoal goal;
  if (type == "CharLevel") {
    goal.kind = AchievementGoalKind::character_level;
    goal.required = positive_value(values, "charLevel");
  } else if (type == "QuestCount" || type == "PlayRecord") {
    goal.kind =
        type == "QuestCount" ? AchievementGoalKind::quest_count : AchievementGoalKind::play_record;
    goal.subject_id =
        static_cast<int>(positive_value(values, type == "QuestCount" ? "questId" : "playRecordId"));
    goal.required = positive_value(values, "count");
  } else if (type == "AdventureObjectTarget") {
    goal.kind = AchievementGoalKind::adventure_target;
    goal.subject_id = static_cast<int>(positive_value(values, "adventureObjectId"));
    goal.required = 1;
  } else if (type == "AdventureObjectCount" || type == "AdventureObjectLogbookPercent") {
    goal.kind = type == "AdventureObjectCount" ? AchievementGoalKind::adventure_count
                                               : AchievementGoalKind::logbook_percent;
    goal.zone = values.at("zoneKey").get<std::string>();
    if (goal.zone.empty()) {
      throw std::runtime_error("Achievement collection goal has no zone");
    }
    if (type == "AdventureObjectCount") {
      x5::cms::EX5AdventureObjectType object_type{};
      if (!x5::cms::EX5AdventureObjectType_Parse(values.at("objectType").get<std::string>(),
                                                 &object_type)) {
        throw std::runtime_error("Achievement collection goal has an unknown object type");
      }
      goal.object_type = static_cast<int>(object_type);
      goal.required = positive_value(values, "count");
    } else {
      goal.required = positive_value(values, "requiredPercent");
      if (goal.required > 100) {
        throw std::runtime_error("Achievement percentage is above 100");
      }
    }
  } else if (type == "ExpertiseLevel") {
    goal.kind = AchievementGoalKind::expertise_level;
    x5::cms::EX5ExpertiseType expertise{};
    x5::cms::EX5ExpertiseLvType level{};
    if (!x5::cms::EX5ExpertiseType_Parse(values.at("expertiseType").get<std::string>(),
                                         &expertise) ||
        !x5::cms::EX5ExpertiseLvType_Parse(values.at("expertiseLvType").get<std::string>(),
                                           &level)) {
      throw std::runtime_error("Achievement expertise goal has an unknown type or level");
    }
    goal.subject_id = static_cast<int>(expertise);
    goal.required = static_cast<int>(level);
  } else if (type == "PlayerKill") {
    goal.kind = AchievementGoalKind::player_kills;
    goal.required = positive_value(values, "killCount");
  } else {
    throw std::runtime_error("Unsupported achievement goal type: " + type);
  }
  return goal;
}

x5::ec::EErrorCode goal_error(AchievementGoalKind kind) {
  switch (kind) {
  case AchievementGoalKind::character_level:
    return x5::ec::Achievement_InvalidCharLevel;
  case AchievementGoalKind::quest_count:
    return x5::ec::Achievement_InvalidQuestCount;
  case AchievementGoalKind::adventure_target:
    return x5::ec::Achievement_InvalidAdventureObjectTarget;
  case AchievementGoalKind::adventure_count:
    return x5::ec::Achievement_InvalidAdventureObjectCount;
  case AchievementGoalKind::logbook_percent:
    return x5::ec::Achievement_InvalidLogBookPercentage;
  case AchievementGoalKind::play_record:
    return x5::ec::Achievement_InvalidPlayRecord;
  case AchievementGoalKind::expertise_level:
    return x5::ec::Achievement_InvalidExpertiseLevel;
  case AchievementGoalKind::player_kills:
    return x5::ec::Achievement_InvalidPlayerKill;
  case AchievementGoalKind::zone_state:
    return x5::ec::Achievement_InvalidZoneState;
  }
  return x5::ec::Achievement_InvalidState;
}

Json achievement_json(int cms_id, const AchievementRecord &record) {
  return {{"cms_id", cms_id},
          {"state",
           record.state == 1 ? "EX5AchievementState_Completed" : "EX5AchievementState_Rewarded"},
          {"complete_tick", std::to_string(record.completion_tick)}};
}

AchievementPlan refuse(const WorldProgress &before, x5::ec::EErrorCode error) {
  return {error, before, {}};
}

} // namespace

std::shared_ptr<const AchievementCatalog> AchievementCatalog::load(ClientCatalog &catalog) {
  return std::make_shared<const AchievementCatalog>(
      catalog.rows("Achievement"), catalog.rows("AchievementGoal"), catalog.rows("Trophy"));
}

AchievementCatalog::AchievementCatalog(const std::vector<Json> &achievements,
                                       const std::vector<Json> &goals,
                                       const std::vector<Json> &trophies) {
  std::set<int> trophy_ids;
  for (const auto &row : trophies) {
    trophy_ids.insert(row_id(row, "id"));
  }
  std::map<int, std::vector<AchievementGoal>> goal_groups;
  std::set<std::pair<int, int>> goal_orders;
  for (const auto &row : goals) {
    const int id = row_id(row, "id");
    if (!goal_orders.emplace(id, row_id(row, "order")).second) {
      throw std::runtime_error("Duplicate achievement goal order");
    }
    goal_groups[id].push_back(parse_goal(row));
  }
  if (achievements.size() > kMaximumAchievements) {
    throw std::runtime_error("Achievement catalog exceeds storage bounds");
  }
  for (const auto &row : achievements) {
    const int id = row_id(row, "id");
    Definition definition;
    const auto parent = catalog_integer(row, "parentId").value_or(0);
    if (parent < 0 || parent > std::numeric_limits<int>::max() || parent == id) {
      throw std::runtime_error("Invalid achievement parent");
    }
    definition.parent_id = static_cast<int>(parent);
    const auto goal_id = row_id(row, "FK_achievementGoalTable");
    const auto group = goal_groups.find(goal_id);
    if (group == goal_groups.end() || group->second.empty() || group->second.size() > 512) {
      throw std::runtime_error("Achievement has no bounded goal definition");
    }
    definition.goals = group->second;

    const auto reward = parameters(row, "type");
    const auto kind = row.at("typeCase").get<std::string>();
    definition.reward_value = positive_value(reward, "value");
    if (kind == "Money") {
      const auto money = reward.value("moneyType", "EX5MoneyType_Gold");
      if (money != "EX5MoneyType_Gold" && money != "EX5MoneyType_HersanderToken") {
        throw std::runtime_error("Unsupported achievement reward currency");
      }
      definition.reward =
          money == "EX5MoneyType_Gold" ? RewardKind::gold : RewardKind::hersander_token;
    } else if (kind == "Exp") {
      definition.reward = RewardKind::experience;
    } else if (kind == "Trophy" && trophy_ids.contains(static_cast<int>(definition.reward_value))) {
      definition.reward = RewardKind::trophy;
    } else {
      throw std::runtime_error("Invalid achievement reward definition");
    }
    if (!definitions_.emplace(id, std::move(definition)).second) {
      throw std::runtime_error("Duplicate achievement id");
    }
  }
  for (const auto &[id, definition] : definitions_) {
    if (definition.parent_id != 0) {
      const auto parent = definitions_.find(definition.parent_id);
      if (parent == definitions_.end()) {
        throw std::runtime_error("Achievement names an absent parent");
      }
      parent->second.children.push_back(id);
    }
    std::set<int> visited;
    int ancestor = id;
    while (ancestor != 0) {
      if (!visited.insert(ancestor).second) {
        throw std::runtime_error("Achievement parent cycle");
      }
      const auto found = definitions_.find(ancestor);
      if (found == definitions_.end()) {
        throw std::runtime_error("Achievement names an absent ancestor");
      }
      ancestor = found->second.parent_id;
    }
  }
}

std::size_t AchievementCatalog::size() const noexcept {
  return definitions_.size();
}

AchievementPlan AchievementCatalog::plan(const WorldProgress &before, int cms_id,
                                         int requested_state, std::int64_t completion_tick,
                                         const AchievementGoalValue &goal_value,
                                         const Progression *progression) const {
  const auto found = definitions_.find(cms_id);
  if (found == definitions_.end()) {
    return refuse(before, x5::ec::Achievement_InvalidAchievementGoalCmsId);
  }
  if ((requested_state != 1 && requested_state != 2) || completion_tick <= 0) {
    return refuse(before, x5::ec::Achievement_InvalidState);
  }
  const auto previous = before.achievements.find(cms_id);
  if (previous != before.achievements.end() && previous->second.state >= requested_state) {
    return {x5::ec::Success,
            before,
            {{kAchievementUpdateNotification,
              {{"achievement", achievement_json(cms_id, previous->second)}}}}};
  }

  AchievementPlan result{x5::ec::Success, before, {}};
  const auto &definition = found->second;
  if (requested_state == 1) {
    if (!goal_value || before.achievements.size() >= kMaximumAchievements) {
      return refuse(before, x5::ec::Achievement_InvalidState);
    }
    // Static client evidence at AAC_Achievements_CheckAllGoalThresholds: every registered
    // goal must reach its threshold. Child achievements must also be completed or rewarded.
    for (const auto &goal : definition.goals) {
      const auto value = goal_value(goal);
      if (!value || *value < goal.required) {
        return refuse(before, goal_error(goal.kind));
      }
    }
    for (const int child : definition.children) {
      if (!before.achievements.contains(child)) {
        return refuse(before, x5::ec::Achievement_InvalidState);
      }
    }
    result.after.achievements.emplace(cms_id, AchievementRecord{1, completion_tick});
  } else {
    if (previous == before.achievements.end() || previous->second.state != 1) {
      return refuse(before, x5::ec::Achievement_InvalidState);
    }
    if (definition.reward == RewardKind::gold || definition.reward == RewardKind::hersander_token) {
      const bool token = definition.reward == RewardKind::hersander_token;
      auto &balance = token ? result.after.hersander_token : result.after.gold;
      if (balance < 0 || balance > kMoneyLimit) {
        return refuse(before, x5::ec::BadRequest);
      }
      // CUSTOM: a reward that would pass the wallet limit fills the wallet to the limit and the
      // achievement is still marked rewarded. Graduated characters hold the full wallet, so
      // refusing left every gold achievement unclaimable for them (official rule not captured).
      const auto credited = std::min(definition.reward_value, kMoneyLimit - balance);
      balance += credited;
      if (credited > 0) {
        Json money{{"change", std::to_string(credited)}, {"result", std::to_string(balance)}};
        if (token) {
          money["money_type"] = "EX5MoneyType_HersanderToken";
        }
        result.packets.push_back({169,
                                  {{"achievement", Json::object()},
                                   {"money_changes", Json::array({std::move(money)})}}});
      }
    } else if (definition.reward == RewardKind::experience) {
      if (!progression) {
        return refuse(before, x5::ec::InvalidState);
      }
      const auto change = progression->award_experience(
          result.after, scale_experience_reward(definition.reward_value));
      if (change.changed()) {
        result.packets.push_back(
            {kChangeCharacterBaseNotification, experience_change_message(result.after, change)});
      }
    }
    // Trophy ownership is represented by its rewarded achievement. The matching client's
    // achievement refresh dispatches the Trophy reward when state changes to Rewarded;
    // there is no separate trophy inventory packet in this build.
    result.after.achievements.at(cms_id).state = 2;
  }
  result.packets.push_back(
      {kAchievementUpdateNotification,
       {{"achievement", achievement_json(cms_id, result.after.achievements.at(cms_id))}}});
  return result;
}

Json achievement_snapshot(const WorldProgress &progress) {
  if (progress.achievements.empty()) {
    return Json::object();
  }
  auto entries = Json::array();
  for (const auto &[id, record] : progress.achievements) {
    entries.push_back(achievement_json(id, record));
  }
  return {{"achievements", std::move(entries)}};
}

std::optional<std::int64_t> stored_achievement_goal_value(const AchievementGoal &goal,
                                                          const WorldProgress &progress) {
  switch (goal.kind) {
  case AchievementGoalKind::character_level:
    return progress.level;
  case AchievementGoalKind::quest_count: {
    const auto found = progress.quests.find(goal.subject_id);
    return found == progress.quests.end() ? 0 : found->second.success_count;
  }
  case AchievementGoalKind::adventure_target: {
    const auto found = progress.adventure_objects.find(goal.subject_id);
    return found != progress.adventure_objects.end() && found->second == 2 ? 1 : 0;
  }
  default:
    return std::nullopt;
  }
}

} // namespace chronicles::character
