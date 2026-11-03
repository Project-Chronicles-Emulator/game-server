#include "world/features/inventory/inventory_feature.hpp"

#include "data/catalog_requirements.hpp"
#include "data/client_catalog.hpp"
#include "world/features/inventory/utility_bag.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace chronicles::inventory {
namespace {

std::shared_ptr<const std::unordered_set<int>> load_utility_items(ClientCatalog *catalog) {
  if (!catalog || !has_catalog_tables(*catalog, {"Item"}, "Utility bag")) {
    return {};
  }

  auto items = std::make_shared<const std::unordered_set<int>>(
      utility_bag_eligible_items(catalog->rows("Item")));
  spdlog::info("Utility bag catalog loaded: {} eligible items", items->size());
  return items;
}

} // namespace

InventoryFeature::InventoryFeature(std::shared_ptr<ClientCatalog> catalog)
    : catalog_(std::move(catalog)), utility_items_(load_utility_items(catalog_.get())) {
}

void InventoryFeature::load_capacity_and_encumbrance() {
  capacity_.emplace(*catalog_);
  encumbrance_rules_ = EncumbranceRules::load(*catalog_);
}

void InventoryFeature::load_polish_catalog() {
  polish_catalog_ = ItemPolishCatalog::load(*catalog_);
}

std::unique_ptr<InventorySession>
InventoryFeature::create_session(ProgressAccess &progress, InventorySessionContext &context) {
  return std::make_unique<InventorySession>(*this, progress, context);
}

bool InventoryFeature::has_capacity() const noexcept {
  return capacity_.has_value();
}

CarriedWeight InventoryFeature::measure_weight(const Character &character,
                                               const WorldProgress &progress) const {
  return capacity_->measure(character, progress);
}

} // namespace chronicles::inventory
