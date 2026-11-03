#include "world/features/inventory/item_polish_catalog.hpp"

#include "data/client_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <spdlog/spdlog.h>

namespace chronicles {
namespace {

constexpr int kMaximumAdditivePercent = 100;

// Catalog cells are strings. An empty or absent cell is nullopt; anything that is not a whole
// base-10 number is a corrupt catalog and throws.
std::optional<std::int64_t> whole_number(const nlohmann::json &row, const char *field) {
  const auto cell = row.find(field);
  if (cell == row.end() || !cell->is_string() || cell->get_ref<const std::string &>().empty()) {
    return std::nullopt;
  }

  const auto &text = cell->get_ref<const std::string &>();
  std::int64_t value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw std::runtime_error(std::string("Tempering catalog field is not a whole number: ") +
                             field);
  }
  return value;
}

std::int64_t required_number(const nlohmann::json &row, const char *field, std::int64_t minimum,
                             std::int64_t maximum) {
  const auto value = whole_number(row, field);
  if (!value || *value < minimum || *value > maximum) {
    throw std::runtime_error(std::string("Tempering catalog field is missing or out of range: ") +
                             field);
  }
  return *value;
}

std::string text_field(const nlohmann::json &row, const char *field) {
  const auto cell = row.find(field);
  if (cell == row.end() || !cell->is_string()) {
    return {};
  }
  return cell->get<std::string>();
}

// ItemPolishIncreaseValue: increase id -> number of steps it defines.
std::unordered_map<std::string, int> increase_table_lengths(ClientCatalog &catalog) {
  std::unordered_map<std::string, int> lengths;
  for (const auto &row : catalog.rows("ItemPolishIncreaseValue")) {
    const auto id = text_field(row, "id");
    const auto step = required_number(row, "polishStep", 1, kMaxItemPolishStep);
    if (id.empty()) {
      throw std::runtime_error("ItemPolishIncreaseValue row has no id");
    }

    auto &length = lengths[id];
    length = std::max(length, static_cast<int>(step));
  }
  return lengths;
}

// ItemAttribute: attribute id -> increase id, for rows that link an increase table.
std::unordered_map<std::string, std::string> attribute_increase_ids(ClientCatalog &catalog) {
  std::unordered_map<std::string, std::string> increase_ids;
  for (const auto &row : catalog.rows("ItemAttribute")) {
    const auto increase_id = text_field(row, "FK_itemPolishIncreaseValueId");
    if (!increase_id.empty()) {
      increase_ids.emplace(text_field(row, "id"), increase_id);
    }
  }
  return increase_ids;
}

PolishAdditiveEffect additive_effect(const std::string &name) {
  if (name == "AddSuccessPercent") {
    return PolishAdditiveEffect::AddSuccessPercent;
  }
  if (name == "DestructionProhibition") {
    return PolishAdditiveEffect::DestructionProhibition;
  }
  throw std::runtime_error("Unknown tempering additive effect: " + name);
}

} // namespace

std::shared_ptr<const ItemPolishCatalog> ItemPolishCatalog::load(ClientCatalog &catalog) {
  for (const char *table : {"Item", "ItemAttribute", "ItemPolish", "ItemPolishIncreaseValue"}) {
    if (!catalog.has_table(table)) {
      return nullptr;
    }
  }
  return std::make_shared<const ItemPolishCatalog>(catalog);
}

ItemPolishCatalog::ItemPolishCatalog(ClientCatalog &catalog) {
  // The client caps tempering per (grade, equipCategory) with the rows it has, whether or not
  // the server can apply each one (AAC_CmsItemPolish_FindMaxStepByGradeCategory).
  std::map<std::pair<std::string, std::string>, int> client_step_caps;
  int unsupported_rows = 0;
  for (const auto &row : catalog.rows("ItemPolish")) {
    const auto grade = text_field(row, "grade");
    const auto category = text_field(row, "equipCategory");
    const auto step = static_cast<int>(required_number(row, "polishStep", 1, kMaxItemPolishStep));
    auto &cap = client_step_caps[{grade, category}];
    cap = std::max(cap, step);

    // A second material or a non-Gold cost has no modelled charge; that step stays refused.
    if (whole_number(row, "materialId02") || text_field(row, "costType") != "Gold") {
      ++unsupported_rows;
      continue;
    }

    PolishRequirement requirement;
    requirement.success_probability = static_cast<int>(
        required_number(row, "successProbability", 0, kPolishProbabilityScale));
    requirement.fail_probability =
        static_cast<int>(required_number(row, "failProbability", 0, kPolishProbabilityScale));
    requirement.destruction_probability = static_cast<int>(
        required_number(row, "destructionProbability", 0, kPolishProbabilityScale));
    requirement.material_cms_id = static_cast<int>(
        required_number(row, "materialId01", 1, std::numeric_limits<int>::max()));
    requirement.material_count = static_cast<int>(
        required_number(row, "materialCnt01", 1, std::numeric_limits<int>::max()));
    requirement.gold_cost =
        required_number(row, "costValue", 0, std::numeric_limits<std::int64_t>::max());

    const auto total = requirement.success_probability + requirement.fail_probability +
                       requirement.destruction_probability;
    if (total != kPolishProbabilityScale) {
      throw std::runtime_error("ItemPolish row probabilities do not sum to 10000");
    }
    if (!requirements_.emplace(RequirementKey{grade, category, step}, requirement).second) {
      throw std::runtime_error("Duplicate ItemPolish row for one grade, category and step");
    }
  }
  if (unsupported_rows > 0) {
    spdlog::warn("Tempering: {} ItemPolish rows use a second material or a non-Gold cost and "
                 "stay refused",
                 unsupported_rows);
  }

  const auto increase_lengths = increase_table_lengths(catalog);
  const auto increase_ids = attribute_increase_ids(catalog);
  for (const auto &row : catalog.rows("Item")) {
    const auto cms_id = whole_number(row, "id");
    if (!cms_id || *cms_id <= 0 || *cms_id > std::numeric_limits<int>::max()) {
      continue;
    }

    const auto category_text = text_field(row, "category");
    if (category_text == "PolishingMaterial") {
      const auto effect_name = text_field(row, "additiveMaterialEffect");
      if (!effect_name.empty()) {
        PolishAdditive additive;
        additive.cms_id = static_cast<int>(*cms_id);
        additive.effect = additive_effect(effect_name);
        additive.value = static_cast<int>(
            required_number(row, "additiveMaterialValue", 0, kMaximumAdditivePercent));
        additives_.emplace(additive.cms_id, additive);
      }
      continue;
    }
    if (category_text != "Equipment") {
      continue;
    }

    const auto equip_slot = text_field(row, "equipSlot");
    const auto category = InventoryRules::polish_category_for_slot_name(equip_slot);
    if (!category) {
      continue;
    }
    const auto grade = text_field(row, "grade");
    const auto cap = client_step_caps.find({grade, std::string(*category)});
    const auto increase_id = increase_ids.find(text_field(row, "FK_itemAttribute"));
    if (cap == client_step_caps.end() || increase_id == increase_ids.end()) {
      continue;
    }
    const auto increase_length = increase_lengths.find(increase_id->second);
    if (increase_length == increase_lengths.end()) {
      throw std::runtime_error("ItemAttribute links a missing ItemPolishIncreaseValue table: " +
                               increase_id->second);
    }

    PolishableItem item;
    item.grade = grade;
    item.category = std::string(*category);
    item.max_step = std::min({cap->second, increase_length->second, kMaxItemPolishStep});
    item.has_polish_residue = whole_number(row, "polishResidueId").has_value();
    items_.emplace(static_cast<int>(*cms_id), std::move(item));
  }

  spdlog::info("Tempering catalog: {} items, {} steps, {} additives", items_.size(),
               requirements_.size(), additives_.size());
}

const PolishableItem *ItemPolishCatalog::item(int cms_id) const {
  const auto found = items_.find(cms_id);
  return found == items_.end() ? nullptr : &found->second;
}

const PolishRequirement *ItemPolishCatalog::requirement(const PolishableItem &item,
                                                        int target_step) const {
  const auto found = requirements_.find(RequirementKey{item.grade, item.category, target_step});
  return found == requirements_.end() ? nullptr : &found->second;
}

const PolishAdditive *ItemPolishCatalog::additive(int cms_id) const {
  const auto found = additives_.find(cms_id);
  return found == additives_.end() ? nullptr : &found->second;
}

} // namespace chronicles
