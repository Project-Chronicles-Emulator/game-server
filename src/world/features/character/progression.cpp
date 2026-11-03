#include "world/features/character/progression.hpp"

#include "cms.generated.pb.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;

// Core attributes a LevelUpPoint can raise (StatBonus_NoConditionalStatType otherwise).
constexpr int kFirstAllocatableStat = x5::cms::EX5StatType_Con;
constexpr int kLastAllocatableStat = x5::cms::EX5StatType_Tal;

// Kill EXP fit (see progression.hpp). Base EXP in hundredths: 500 + 25 per NPC level, i.e.
// 5 + level / 4. Grade multipliers are in tenths; the product is in 1/1000 EXP units.
constexpr std::int64_t kKillExperienceBaseHundredths = 500;
constexpr std::int64_t kKillExperiencePerLevelHundredths = 25;
constexpr std::int64_t kKillExperienceScale = 1000;
constexpr std::int64_t kKillExperienceRoundingHalf = kKillExperienceScale / 2;
constexpr int kMaximumNpcLevel = 1000;

struct GradeMultiplier {
  std::string_view grade;
  std::int64_t tenths;
};

constexpr std::array<GradeMultiplier, 8> kKillGradeMultipliers{{
    {"Weak", 4},
    {"Normal", 10},
    {"Strong", 16},
    {"Elite", 160},
    {"Boss_B", 600},
    // Unobserved grades: emulator policy reuses the observed Boss_B multiplier.
    {"Boss_C", 600},
    {"Boss_A", 600},
    {"Boss_S", 600},
}};

std::string text_field(const Json &row, const char *field) {
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

template <typename Integer>
std::optional<Integer> parse_integer(std::string_view text) {
  Integer value{};
  const auto *end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return value;
}

template <typename Integer>
Integer required_integer(const Json &row, const char *table, const char *field) {
  const auto parsed = parse_integer<Integer>(text_field(row, field));
  if (!parsed) {
    throw std::runtime_error(std::string(table) + " field " + field + " is not an integer");
  }
  return *parsed;
}

// Optional catalog integer: an empty field is zero.
int optional_integer(const Json &row, const char *table, const char *field) {
  const auto text = text_field(row, field);
  if (text.empty()) {
    return 0;
  }
  return required_integer<int>(row, table, field);
}

int parse_class(const std::string &name) {
  x5::cms::EX5CharacterClass value;
  if (!x5::cms::EX5CharacterClass_Parse("EX5CharacterClass_" + name, &value)) {
    throw std::runtime_error("PerkNode has an unknown character class: " + name);
  }
  return static_cast<int>(value);
}

int parse_theme(const std::string &name) {
  x5::cms::EX5PerkNodeThemeType value;
  if (!x5::cms::EX5PerkNodeThemeType_Parse("EX5PerkNodeThemeType_" + name, &value) ||
      value == x5::cms::EX5PerkNodeThemeType_None) {
    throw std::runtime_error("Unknown perk theme: " + name);
  }
  return static_cast<int>(value);
}

int parse_build_point_type(const std::string &name) {
  x5::cms::EX5CharBuildPointType value;
  if (!x5::cms::EX5CharBuildPointType_Parse("EX5CharBuildPointType_" + name, &value) ||
      value == x5::cms::EX5CharBuildPointType_None) {
    throw std::runtime_error("Unknown character build point type: " + name);
  }
  return static_cast<int>(value);
}

// FK_unlockNodeIds is a comma-separated id list ("1110102,1110103") or empty.
std::vector<int> parse_unlock_nodes(const std::string &text) {
  std::vector<int> nodes;
  std::string_view remaining = text;
  while (!remaining.empty()) {
    const auto separator = remaining.find(',');
    const auto token = remaining.substr(0, separator);
    const auto node = parse_integer<int>(token);
    if (!node || *node <= 0) {
      throw std::runtime_error("PerkNode FK_unlockNodeIds is malformed");
    }
    nodes.push_back(*node);
    if (separator == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(separator + 1);
  }
  return nodes;
}

std::int64_t saturating_add(std::int64_t left, std::int64_t right) {
  if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return left + right;
}

// Protobuf JSON prints enums by name; accept the numeric form as well.
template <typename Enum, typename Parser>
std::optional<int> enum_value(const Json &value, Parser parse) {
  if (value.is_number_integer()) {
    return value.get<int>();
  }
  if (!value.is_string()) {
    return std::nullopt;
  }
  Enum parsed;
  if (!parse(value.get<std::string>(), &parsed)) {
    return std::nullopt;
  }
  return static_cast<int>(parsed);
}

// Protobuf JSON omits zero-valued scalars, so a missing integer field is zero.
std::optional<std::int64_t> message_integer(const Json &object, const char *field) {
  const auto found = object.find(field);
  if (found == object.end()) {
    return std::int64_t{};
  }
  if (found->is_number_integer()) {
    return found->get<std::int64_t>();
  }
  if (found->is_string()) {
    return parse_integer<std::int64_t>(found->get<std::string>());
  }
  return std::nullopt;
}

} // namespace

std::shared_ptr<const Progression> Progression::load(ClientCatalog &catalog) {
  auto progression = std::make_shared<Progression>();

  int expected_level = 1;
  std::int64_t previous_threshold = 0;
  for (const auto &row : catalog.rows("CharacterLevel")) {
    const auto level = required_integer<int>(row, "CharacterLevel", "level");
    const auto threshold = required_integer<std::int64_t>(row, "CharacterLevel", "nextLevelExp");
    if (level != expected_level) {
      throw std::runtime_error("CharacterLevel levels must be contiguous from 1");
    }
    if (threshold <= previous_threshold) {
      throw std::runtime_error("CharacterLevel nextLevelExp must increase with every level");
    }

    LevelRow entry;
    entry.next_level_experience = threshold;
    entry.level_up_points = optional_integer(row, "CharacterLevel", "charBuildPoint");
    const auto point_type = text_field(row, "charBuildPointType");
    if (entry.level_up_points < 0 ||
        (!point_type.empty() && parse_build_point_type(point_type) != kLevelUpPointType)) {
      throw std::runtime_error("CharacterLevel must grant non-negative LevelUpPoints");
    }

    progression->levels_.emplace(level, entry);
    progression->max_level_ = level;
    previous_threshold = threshold;
    ++expected_level;
  }
  if (progression->levels_.empty()) {
    throw std::runtime_error("CharacterLevel table is empty");
  }

  for (const auto &row : catalog.rows("PerkTheme")) {
    progression->themes_.insert(parse_theme(text_field(row, "perkThemeType")));
  }

  for (const auto &row : catalog.rows("PerkNode")) {
    PerkNode node;
    node.cms_id = required_integer<int>(row, "PerkNode", "id");
    node.class_id = parse_class(text_field(row, "characterClass"));
    node.theme = parse_theme(text_field(row, "theme"));
    node.cost_type = parse_build_point_type(text_field(row, "costType"));
    node.cost = required_integer<int>(row, "PerkNode", "cost");
    node.max_level = required_integer<int>(row, "PerkNode", "maxLevel");
    node.group_id = optional_integer(row, "PerkNode", "groupId");
    node.auto_registration = text_field(row, "autoRegistration") == "True";
    node.unlock_nodes = parse_unlock_nodes(text_field(row, "FK_unlockNodeIds"));
    if (node.cms_id <= 0 || node.cost < 0 || node.max_level < 1) {
      throw std::runtime_error("PerkNode row has an invalid id, cost or maximum level");
    }
    if (!progression->perks_.emplace(node.cms_id, std::move(node)).second) {
      throw std::runtime_error("Duplicate PerkNode id");
    }
  }

  std::map<std::string, std::string> template_grades;
  for (const auto &row : catalog.rows("NpcTemplate")) {
    template_grades.emplace(text_field(row, "id"), text_field(row, "grade"));
  }
  for (const auto &row : catalog.rows("Npc")) {
    if (text_field(row, "behaviorType") != "Monster") {
      continue;
    }

    const auto grade = template_grades.find(text_field(row, "FK_template"));
    if (grade == template_grades.end()) {
      continue;
    }
    const auto multiplier =
        std::ranges::find_if(kKillGradeMultipliers, [&](const GradeMultiplier &entry) {
          return entry.grade == grade->second;
        });
    const auto npc_level = optional_integer(row, "Npc", "level");
    if (multiplier == kKillGradeMultipliers.end() || npc_level < 0 ||
        npc_level > kMaximumNpcLevel) {
      continue;
    }

    const auto base_hundredths =
        kKillExperienceBaseHundredths + kKillExperiencePerLevelHundredths * npc_level;
    const auto experience =
        (base_hundredths * multiplier->tenths + kKillExperienceRoundingHalf) /
        kKillExperienceScale;
    progression->monster_experience_.emplace(required_integer<int>(row, "Npc", "id"),
                                             experience);
  }

  return progression;
}

std::optional<std::int64_t> Progression::next_level_experience(int level) const {
  if (level >= max_level_) {
    return std::nullopt;
  }
  const auto found = levels_.find(level);
  if (found == levels_.end()) {
    return std::nullopt;
  }
  return found->second.next_level_experience;
}

std::int64_t Progression::level_floor_experience(int level) const {
  if (level <= 1) {
    return 0;
  }
  const auto previous = levels_.find(std::min(level, max_level_) - 1);
  if (previous == levels_.end()) {
    return 0;
  }
  return previous->second.next_level_experience;
}

int Progression::level_for_experience(std::int64_t experience) const {
  int level = 1;
  while (level < max_level_) {
    const auto threshold = next_level_experience(level);
    if (!threshold || experience < *threshold) {
      break;
    }
    ++level;
  }
  return level;
}

int Progression::level_up_points_on_reach(int level) const {
  const auto found = levels_.find(level);
  if (found == levels_.end()) {
    return 0;
  }
  return found->second.level_up_points;
}

void Progression::add_levels(WorldProgress &progress, int target_level,
                             ExperienceChange &change) const {
  while (progress.level < target_level) {
    ++progress.level;
    const auto points = level_up_points_on_reach(progress.level);
    progress.level_up_points = std::min(progress.level_up_points + points, kMaximumBuildPoints);
    change.level_up_points_awarded += points;
    ++change.levels_gained;
  }
}

ExperienceChange Progression::award_experience(WorldProgress &progress,
                                               std::int64_t amount) const {
  if (amount < 0) {
    throw std::invalid_argument("EXP awards must not be negative");
  }

  ExperienceChange change;
  change.previous_level = progress.level;

  // The cap row's nextLevelExp is a sentinel that is never reached; EXP stops just below it.
  const auto cap = levels_.at(max_level_).next_level_experience - 1;
  const auto total = std::min(saturating_add(progress.experience, amount), cap);
  if (total > progress.experience) {
    change.experience_added = total - progress.experience;
    progress.experience = total;
  }

  add_levels(progress, level_for_experience(progress.experience), change);
  return change;
}

ExperienceChange Progression::reconcile_level(WorldProgress &progress) const {
  return award_experience(progress, 0);
}

ExperienceChange Progression::set_level(WorldProgress &progress, int level) const {
  if (level < 1 || level > max_level_) {
    throw std::invalid_argument("Level is outside the CharacterLevel table");
  }

  ExperienceChange change;
  change.previous_level = progress.level;
  const auto floor = level_floor_experience(level);
  change.experience_added = floor - progress.experience;
  progress.experience = floor;
  if (level < progress.level) {
    // Lowering never removes earned points or allocations; it only moves the EXP bar.
    progress.level = level;
    return change;
  }

  add_levels(progress, level, change);
  return change;
}

std::int64_t Progression::monster_experience(int npc_cms_id) const {
  const auto found = monster_experience_.find(npc_cms_id);
  if (found == monster_experience_.end()) {
    return 0;
  }
  return found->second;
}

x5::ec::EErrorCode Progression::grant_build_points(WorldProgress &progress, int point_type,
                                                   int amount) const {
  int *balance = nullptr;
  if (point_type == kLevelUpPointType) {
    balance = &progress.level_up_points;
  } else if (point_type == kWeaponPointType) {
    balance = &progress.weapon_points;
  } else {
    return x5::ec::BadRequest;
  }

  const auto updated = std::clamp<std::int64_t>(static_cast<std::int64_t>(*balance) + amount, 0,
                                                kMaximumBuildPoints);
  *balance = static_cast<int>(updated);
  return x5::ec::Success;
}

x5::ec::EErrorCode
Progression::allocate_stat_bonus(WorldProgress &progress,
                                 const std::vector<StatBonusAllocation> &entries) const {
  if (entries.empty() || entries.size() > kMaximumStatBonusEntriesPerRequest) {
    return x5::ec::StatBonus_InvalidRequest;
  }

  auto next = progress.build.stat_bonuses;
  std::set<int> seen_types;
  std::int64_t total_points = 0;
  for (const auto &entry : entries) {
    if (entry.stat_type < kFirstAllocatableStat || entry.stat_type > kLastAllocatableStat) {
      return x5::ec::StatBonus_NoConditionalStatType;
    }
    if (entry.points <= 0 || !seen_types.insert(entry.stat_type).second) {
      return x5::ec::StatBonus_InvalidRequest;
    }

    auto &bonus = next[entry.stat_type];
    if (bonus > kMaximumStatBonus - entry.points) {
      return x5::ec::StatBonus_InvalidRequest;
    }
    bonus += entry.points;
    total_points += entry.points;
  }
  if (total_points > progress.level_up_points) {
    return x5::ec::StatBonus_OverCost;
  }

  progress.build.stat_bonuses = std::move(next);
  progress.level_up_points -= static_cast<int>(total_points);
  return x5::ec::Success;
}

void Progression::reset_stat_bonus(WorldProgress &progress) const {
  std::int64_t refunded = 0;
  for (const auto &[stat_type, points] : progress.build.stat_bonuses) {
    refunded += points;
  }
  progress.build.stat_bonuses.clear();
  progress.level_up_points = static_cast<int>(
      std::min<std::int64_t>(progress.level_up_points + refunded, kMaximumBuildPoints));
}

x5::ec::EErrorCode Progression::gm_set_stat_bonus(WorldProgress &progress, int stat_type,
                                                  int value) const {
  if (stat_type < kFirstAllocatableStat || stat_type > kLastAllocatableStat) {
    return x5::ec::StatBonus_NoConditionalStatType;
  }
  if (value < 0 || value > kMaximumStatBonus) {
    return x5::ec::StatBonus_InvalidRequest;
  }

  if (value == 0) {
    progress.build.stat_bonuses.erase(stat_type);
  } else {
    progress.build.stat_bonuses[stat_type] = value;
  }
  return x5::ec::Success;
}

int Progression::stat_bonus(const WorldProgress &progress, int stat_type) {
  const auto found = progress.build.stat_bonuses.find(stat_type);
  if (found == progress.build.stat_bonuses.end()) {
    return 0;
  }
  return found->second;
}

std::vector<PerkUpdate> Progression::gm_learn_all_perks(WorldProgress &progress,
                                                        int class_id) const {
  auto &build = progress.build;
  std::set<int> occupied_groups;
  for (const auto &[cms_id, node] : perks_) {
    if (node.class_id == class_id && node.group_id != 0 && active_level(node, build) > 0) {
      occupied_groups.insert(node.group_id);
    }
  }

  const auto prerequisites_met = [&](const PerkNode &node) {
    if (node.unlock_nodes.empty()) {
      return true;
    }
    return std::ranges::any_of(node.unlock_nodes, [&](int prerequisite) {
      const auto *required = perk(prerequisite);
      return required != nullptr && active_level(*required, build) > 0;
    });
  };

  // Each pass can unlock the next tier, so repeat until nothing more can be activated.
  std::vector<PerkUpdate> activated;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[cms_id, node] : perks_) {
      const bool candidate = node.class_id == class_id && !node.auto_registration &&
                             node.max_level > 0 && active_level(node, build) < node.max_level;
      // An active node may still be raised to its maximum; it already owns its group.
      const bool group_taken = node.group_id != 0 && active_level(node, build) == 0 &&
                               occupied_groups.contains(node.group_id);
      if (!candidate || group_taken || !prerequisites_met(node)) {
        continue;
      }
      if (!build.perks.contains(cms_id) && build.perks.size() >= kMaximumEarnedPerks) {
        continue;
      }
      if (node.group_id != 0) {
        occupied_groups.insert(node.group_id);
      }
      build.perks[cms_id] = node.max_level;
      activated.push_back({cms_id, node.max_level});
      changed = true;
    }
  }
  return activated;
}

const Progression::PerkNode *Progression::perk(int cms_id) const {
  const auto found = perks_.find(cms_id);
  if (found == perks_.end()) {
    return nullptr;
  }
  return &found->second;
}

int Progression::active_level(const PerkNode &node, const CharacterBuild &build) const {
  if (node.auto_registration) {
    return 1;
  }
  const auto earned = build.perks.find(node.cms_id);
  if (earned == build.perks.end()) {
    return 0;
  }
  return earned->second;
}

// The resulting tree must satisfy every node's prerequisites (any one listed node active) and
// hold at most one active node per exclusive group.
x5::ec::EErrorCode Progression::validate_perk_tree(int class_id,
                                                   const CharacterBuild &build) const {
  std::map<int, int> active_group_members;
  for (const auto &[cms_id, node] : perks_) {
    if (node.class_id != class_id || active_level(node, build) <= 0) {
      continue;
    }

    if (node.group_id != 0 && ++active_group_members[node.group_id] > 1) {
      return x5::ec::PerkNode_ExclusiveActivation;
    }
    if (node.auto_registration || node.unlock_nodes.empty()) {
      continue;
    }

    const bool unlocked = std::ranges::any_of(node.unlock_nodes, [&](int prerequisite) {
      const auto *required = perk(prerequisite);
      return required != nullptr && active_level(*required, build) > 0;
    });
    if (!unlocked) {
      return x5::ec::PerkNode_Locked;
    }
  }
  return x5::ec::Success;
}

x5::ec::EErrorCode Progression::change_perks(WorldProgress &progress, int class_id,
                                             const std::vector<PerkUpdate> &updates) const {
  if (updates.empty()) {
    return x5::ec::PerkNode_NotChanged;
  }
  if (updates.size() > kMaximumPerkUpdatesPerRequest) {
    return x5::ec::BadRequest;
  }

  auto build = progress.build;
  std::set<int> updated_nodes;
  std::int64_t net_cost = 0;
  for (const auto &update : updates) {
    const auto *node = perk(update.cms_id);
    if (node == nullptr) {
      return x5::ec::PerkNode_InvalidCmsId;
    }
    if (!updated_nodes.insert(update.cms_id).second) {
      return x5::ec::PerkNode_TryUpdateDuplicatedPerk;
    }
    if (node->class_id != class_id) {
      return x5::ec::PerkNode_InvalidClass;
    }
    if (update.level < 0 || update.level > node->max_level) {
      return x5::ec::PerkNode_InvalidLevel;
    }
    if (node->cost_type != kWeaponPointType) {
      return x5::ec::PerkNode_InvalidCostType;
    }
    if (node->auto_registration) {
      return update.level == 0 ? x5::ec::PerkNode_TryDeactivateAutoRegistedPerk
                               : x5::ec::PerkNode_TryUpdateSameLevel;
    }

    const auto current = active_level(*node, build);
    if (update.level == current) {
      return current == 0 ? x5::ec::PerkNode_TryDeactivateNotActivated
                          : x5::ec::PerkNode_TryUpdateSameLevel;
    }

    // Each level costs the node cost; deactivation refunds the levels it removes.
    net_cost += static_cast<std::int64_t>(node->cost) * (update.level - current);
    build.perks[update.cms_id] = update.level;
  }

  if (build.perks.size() > kMaximumEarnedPerks) {
    return x5::ec::BadRequest;
  }
  const auto tree_status = validate_perk_tree(class_id, build);
  if (tree_status != x5::ec::Success) {
    return tree_status;
  }
  if (net_cost > progress.weapon_points) {
    return x5::ec::PerkNode_OverCost;
  }

  const auto balance = std::min<std::int64_t>(progress.weapon_points - net_cost,
                                              kMaximumBuildPoints);
  progress.build = std::move(build);
  progress.weapon_points = static_cast<int>(balance);
  return x5::ec::Success;
}

x5::ec::EErrorCode Progression::reset_perks(WorldProgress &progress, int class_id,
                                            std::optional<int> theme,
                                            std::vector<int> *reset_nodes) const {
  if (theme && !themes_.contains(*theme)) {
    return x5::ec::PerkNode_InvalidTheme;
  }

  std::int64_t refunded = 0;
  for (auto &[cms_id, level] : progress.build.perks) {
    const auto *node = perk(cms_id);
    if (node == nullptr || level <= 0 || node->class_id != class_id ||
        (theme && node->theme != *theme)) {
      continue;
    }

    refunded += static_cast<std::int64_t>(node->cost) * level;
    level = 0;
    if (reset_nodes) {
      reset_nodes->push_back(cms_id);
    }
  }

  progress.weapon_points = static_cast<int>(
      std::min<std::int64_t>(progress.weapon_points + refunded, kMaximumBuildPoints));
  return x5::ec::Success;
}

std::vector<PerkUpdate> Progression::active_perks(int class_id,
                                                  const CharacterBuild &build) const {
  std::vector<PerkUpdate> active;
  for (const auto &[cms_id, node] : perks_) {
    if (node.class_id != class_id) {
      continue;
    }
    const auto level = active_level(node, build);
    if (level > 0) {
      active.push_back({cms_id, level});
    }
  }
  return active;
}

Json remaining_build_points_json(const WorldProgress &progress) {
  return {{std::to_string(kLevelUpPointType), progress.level_up_points},
          {std::to_string(kWeaponPointType), progress.weapon_points}};
}

Json experience_change_message(const WorldProgress &progress, const ExperienceChange &change) {
  Json message{{"exp", std::to_string(progress.experience)}};
  if (progress.level != change.previous_level) {
    message["level"] = progress.level;
    message["remaining_char_build_points"] = {
        {std::to_string(kLevelUpPointType), progress.level_up_points}};
  }
  return message;
}

Json weapon_points_message(const WorldProgress &progress) {
  return {{"remaining_char_build_points",
           {{std::to_string(kWeaponPointType), progress.weapon_points}}}};
}

Json level_up_points_message(const WorldProgress &progress) {
  return {{"remaining_char_build_points",
           {{std::to_string(kLevelUpPointType), progress.level_up_points}}}};
}

Json perk_snapshot_message(const Progression &progression, int class_id,
                           const WorldProgress &progress) {
  Json perks = Json::array();
  for (const auto &perk : progression.active_perks(class_id, progress.build)) {
    perks.push_back({{"cms_id", perk.cms_id}, {"level", perk.level}});
  }
  return {{"perks", std::move(perks)},
          {"remaining_char_build_points", remaining_build_points_json(progress)}};
}

std::optional<std::vector<StatBonusAllocation>>
parse_stat_bonus_request(const Json &message) {
  if (!message.is_object()) {
    return std::nullopt;
  }
  const auto stats = message.find("stats");
  if (stats == message.end() || !stats->is_array() ||
      stats->size() > kMaximumStatBonusEntriesPerRequest) {
    return std::nullopt;
  }

  std::vector<StatBonusAllocation> entries;
  for (const auto &entry : *stats) {
    if (!entry.is_object()) {
      return std::nullopt;
    }

    std::optional<int> stat_type = 0;
    if (entry.contains("stat_type")) {
      stat_type = enum_value<x5::cms::EX5StatType>(
          entry.at("stat_type"), [](const std::string &name, x5::cms::EX5StatType *value) {
            return x5::cms::EX5StatType_Parse(name, value);
          });
    }
    const auto points = message_integer(entry, "value");
    if (!stat_type || !points || *points < 0 || *points > kMaximumStatBonus) {
      return std::nullopt;
    }
    entries.push_back({*stat_type, static_cast<int>(*points)});
  }
  return entries;
}

std::optional<PerkChangeRequest> parse_perk_change_request(const Json &message) {
  if (!message.is_object()) {
    return std::nullopt;
  }

  PerkChangeRequest request;
  const auto packet_index = message_integer(message, "packet_index");
  if (!packet_index || *packet_index < std::numeric_limits<int>::min() ||
      *packet_index > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  request.packet_index = static_cast<int>(*packet_index);

  const auto updates = message.find("updated_perks");
  if (updates == message.end()) {
    return request;
  }
  if (!updates->is_array() || updates->size() > kMaximumPerkUpdatesPerRequest) {
    return std::nullopt;
  }
  for (const auto &entry : *updates) {
    if (!entry.is_object()) {
      return std::nullopt;
    }
    const auto cms_id = message_integer(entry, "cms_id");
    const auto level = message_integer(entry, "level");
    if (!cms_id || !level || *cms_id <= 0 || *cms_id > std::numeric_limits<int>::max() ||
        *level < std::numeric_limits<int>::min() || *level > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    request.updates.push_back({static_cast<int>(*cms_id), static_cast<int>(*level)});
  }
  return request;
}

std::optional<int> parse_perk_reset_request(const Json &message) {
  if (!message.is_object() || !message.contains("theme_type")) {
    return std::nullopt;
  }
  return enum_value<x5::cms::EX5PerkNodeThemeType>(
      message.at("theme_type"), [](const std::string &name, x5::cms::EX5PerkNodeThemeType *value) {
        return x5::cms::EX5PerkNodeThemeType_Parse(name, value);
      });
}

} // namespace chronicles
