#include "world/features/character/graduation_grant.hpp"

#include "cms.generated.pb.h"
#include "common/catalog_fields.hpp"
#include "data/shop_catalog.hpp"
#include "world/features/character/progression.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <string_view>

namespace chronicles {
namespace {

constexpr std::string_view kEquipmentCategory = "Equipment";
constexpr std::string_view kClassNeutral = "None";
constexpr std::string_view kCharacterClassPrefix = "EX5CharacterClass_";

constexpr std::string_view kItemGradePrefix = "EX5ItemGrade_";

// The enum order is the quality order (Normal, Grand, Rare, Heroic, Legendary, Mythic).
std::optional<int> grade_rank(const std::string &grade) {
  x5::cms::EX5ItemGrade parsed{};
  if (!x5::cms::EX5ItemGrade_Parse(std::string(kItemGradePrefix) + grade, &parsed)) {
    return std::nullopt;
  }
  return static_cast<int>(parsed);
}

} // namespace

GraduationLoadout::GraduationLoadout(const std::vector<nlohmann::json> &item_rows,
                                     int maximum_level) {
  for (const auto &row : item_rows) {
    if (catalog_text_field(row, "category") != kEquipmentCategory) {
      continue;
    }

    const auto id = catalog_integer(row, "id");
    const auto slot = catalog_text_field(row, "equipSlot");
    if (!id || slot.empty()) {
      continue;
    }

    // Item.level is the piece's tier and may exceed the character level cap (tier 80 gear is
    // wearable at level 40); only the required level decides. Some pieces require an
    // unreachable level (999) and cannot be equipped by anyone.
    const auto level = catalog_integer(row, "level").value_or(0);
    const auto required_level = catalog_integer(row, "levelReq").value_or(0);
    if (required_level > maximum_level) {
      continue;
    }

    const auto grade = grade_rank(catalog_text_field(row, "grade"));
    if (!grade) {
      continue;
    }

    equipment_.push_back({static_cast<int>(*id), catalog_text_field(row, "class"), slot,
                          static_cast<int>(level), *grade});
  }
}

std::vector<ItemBundleEntry> GraduationLoadout::bundle_for(const std::string &class_name) const {
  // Best (level, grade) of each slot, then every piece that ties with it, in id order.
  std::map<std::string, std::pair<int, int>> best_by_slot;
  for (const auto &row : equipment_) {
    if (row.class_name != class_name && row.class_name != kClassNeutral) {
      continue;
    }

    const auto rank = std::make_pair(row.level, row.grade_rank);
    auto &best = best_by_slot[row.slot];
    best = std::max(best, rank);
  }

  std::map<std::string, std::vector<int>> chosen_by_slot;
  for (const auto &row : equipment_) {
    if (row.class_name != class_name && row.class_name != kClassNeutral) {
      continue;
    }

    if (std::make_pair(row.level, row.grade_rank) == best_by_slot[row.slot]) {
      chosen_by_slot[row.slot].push_back(row.item_cms_id);
    }
  }

  std::vector<ItemBundleEntry> bundle;
  for (auto &[slot, ids] : chosen_by_slot) {
    std::ranges::sort(ids);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.size() > kGraduationMaximumEquipmentPerSlot) {
      ids.resize(kGraduationMaximumEquipmentPerSlot);
    }
    for (const int id : ids) {
      bundle.push_back({id, 1});
    }
  }

  bundle.push_back({kGraduationHealthPotionItemId, kGraduationHealthPotionCount});
  bundle.push_back({kGraduationStaminaPotionItemId, kGraduationStaminaPotionCount});
  bundle.push_back({kGraduationRepairerItemId, kGraduationRepairerCount});
  return bundle;
}

std::string graduation_class_name(int class_id) {
  if (!x5::cms::EX5CharacterClass_IsValid(class_id)) {
    return {};
  }

  const auto character_class = static_cast<x5::cms::EX5CharacterClass>(class_id);
  std::string name = x5::cms::EX5CharacterClass_Name(character_class);
  if (name.starts_with(kCharacterClassPrefix)) {
    name.erase(0, kCharacterClassPrefix.size());
  }
  return name;
}

namespace character {
namespace {

constexpr int kFillBalanceAmount = std::numeric_limits<int>::max();

std::size_t grant_kit_items(GraduationGrantContext &context,
                            const GraduationGrantServices &services) {
  const auto character = context.progression_character();
  if (!character) {
    return 0;
  }

  const auto class_name = graduation_class_name(character->class_id);
  if (class_name.empty()) {
    spdlog::warn("Graduation items skipped: character {} has class {}", character->id,
                 character->class_id);
    return 0;
  }

  const auto before = context.progression_stored_progress();
  const auto &catalog = *services.shop_catalog;
  const auto plan = context.graduation_plan_items(catalog, services.loadout->bundle_for(class_name),
                                                  catalog.gold_limit(), *character, before,
                                                  kGraduationMaximumEquipmentInstances);
  if (plan.error != x5::ec::Success) {
    spdlog::warn("Graduation items refused for character {}: {}", character->id,
                 x5::ec::EErrorCode_Name(plan.error));
    return 0;
  }
  if (plan.empty()) {
    return 0;
  }

  if (!context.graduation_commit_items(before, plan.trade.next, plan.trade.storage_changes())) {
    spdlog::warn("Graduation items for character {} conflicted with another change", character->id);
    return 0;
  }

  spdlog::info("Graduation grant gave character {} {} equipment pieces ({} catalog ids skipped)",
               character->id, plan.trade.granted_items.size(), plan.skipped_item_ids.size());
  return plan.trade.granted_items.size();
}

bool raise_to_level_cap(GraduationGrantContext &context, const Progression &progression) {
  const auto before = context.progression_stored_progress();
  if (before.revision == std::numeric_limits<std::int64_t>::max()) {
    return false;
  }

  auto after = before;
  progression.set_level(after, progression.max_level());
  progression.grant_build_points(after, kLevelUpPointType, kFillBalanceAmount);
  progression.grant_build_points(after, kWeaponPointType, kFillBalanceAmount);

  ++after.revision;
  if (!context.progression_save(before, after)) {
    spdlog::warn("Graduation level for character {} conflicted with another change",
                 context.progression_character_id());
    return false;
  }

  spdlog::info("Graduation grant set character {} to level {} with {} level-up points and {} "
               "weapon points",
               context.progression_character_id(), after.level, after.level_up_points,
               after.weapon_points);
  return true;
}

} // namespace

GraduationResult apply_graduation_grant(GraduationGrantContext &context,
                                        const GraduationGrantServices &services) {
  GraduationResult result;
  if (!services.loadout || !services.progression || !services.shop_catalog ||
      context.graduation_game_master()) {
    return result;
  }
  if (context.progression_stored_progress().level >= services.progression->max_level()) {
    return result;
  }

  result.applied = true;
  try {
    result.quests_completed = context.graduation_complete_quests();
    spdlog::info("Graduation grant completed {} quests for character {}", result.quests_completed,
                 context.progression_character_id());
  } catch (const std::exception &error) {
    spdlog::warn("Graduation quests failed for character {}: {}",
                 context.progression_character_id(), error.what());
  }

  try {
    result.equipment_granted = grant_kit_items(context, services);
  } catch (const std::exception &error) {
    spdlog::warn("Graduation items failed for character {}: {}", context.progression_character_id(),
                 error.what());
  }

  try {
    result.level_raised = raise_to_level_cap(context, *services.progression);
  } catch (const std::exception &error) {
    spdlog::warn("Graduation level failed for character {}: {}", context.progression_character_id(),
                 error.what());
  }
  return result;
}

} // namespace character
} // namespace chronicles
