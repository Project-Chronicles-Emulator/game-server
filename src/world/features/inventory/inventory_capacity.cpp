#include "world/features/inventory/inventory_capacity.hpp"
#include "data/client_catalog.hpp"
#include "world/features/inventory/inventory_rules.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles {

InventoryCapacity::InventoryCapacity(ClientCatalog &catalog) {
  const auto baseline = catalog.find_row("GameVariable", "name", "BagDefaultSafeWeight");
  if (!baseline) {
    throw std::runtime_error("Missing BagDefaultSafeWeight catalog variable");
  }
  default_safe_weight_ = std::stoll(baseline->at("value").get<std::string>());
  if (default_safe_weight_ <= 0) {
    throw std::runtime_error("Invalid BagDefaultSafeWeight catalog variable");
  }

  for (const auto &row : catalog.rows("Item")) {
    const auto cms_id = std::stoi(row.at("id").get<std::string>());
    const auto weight_text = row.value("weight", std::string());
    const auto safe_text = row.value("safeWeight", std::string());
    ItemWeight weight;
    weight.per_unit = weight_text.empty() ? 0 : std::stoll(weight_text);
    weight.bag_safe = safe_text.empty() ? 0 : std::stoll(safe_text);
    weight.is_bag = row.value("equipSlot", std::string()) == "Bag";
    if (cms_id <= 0 || weight.per_unit < 0 || weight.bag_safe < 0 ||
        !item_weights_.emplace(cms_id, weight).second) {
      throw std::runtime_error("Invalid or duplicate Item weight catalog row");
    }
  }
}

CarriedWeight InventoryCapacity::measure(const Character &character,
                                         const WorldProgress &progress) const {
  std::vector<ItemWeightLine> lines;
  lines.reserve(character.items.size() + progress.stackables.size());
  std::int64_t equipped_bag_safe_weight = 0;

  const auto add_line = [&](int cms_id, int quantity) -> const ItemWeight & {
    const auto found = item_weights_.find(cms_id);
    if (found == item_weights_.end() || quantity < 0) {
      throw std::runtime_error("Missing or invalid carried Item weight");
    }
    lines.push_back({found->second.per_unit, quantity});
    return found->second;
  };

  for (const auto &item : character.items) {
    const auto &definition = add_line(item.cms_id, item.quantity);
    if (item.equip_slot == 24 && definition.is_bag && definition.bag_safe > 0) {
      equipped_bag_safe_weight = definition.bag_safe;
    }
  }
  for (const auto &[cms_id, quantity] : progress.stackables) {
    add_line(cms_id, quantity);
  }

  // InventoryRules::compute_weight owns the formula; overflow surfaces as
  // std::overflow_error exactly as before, and malformed input as runtime_error.
  try {
    const auto status =
        InventoryRules::compute_weight(lines, default_safe_weight_, equipped_bag_safe_weight);
    return CarriedWeight{status.carried_weight, status.safe_capacity};
  } catch (const std::invalid_argument &error) {
    throw std::runtime_error(std::string("Invalid carried weight input: ") + error.what());
  }
}

} // namespace chronicles
