#pragma once

#include "world/features/inventory/inventory_rules.hpp"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>

namespace chronicles {

class ClientCatalog;

// Static tempering facts for one equipment item.
struct PolishableItem {
  // Catalog grade name, for example "Grand".
  std::string grade;
  // ItemPolish equipCategory: "Weapon" or "Armor".
  std::string category;
  // Highest step the item can reach: the smaller of the ItemPolish maximum for its grade and
  // category (the client's own cap, AAC_ItemPolish_GetMaxStepForItem), the length of its
  // ItemPolishIncreaseValue table and the storage maximum.
  int max_step{};
  // Item.polishResidueId is set. No row sets it in this catalog; a destruction that would have
  // to grant the residue is refused instead of silently dropping it.
  bool has_polish_residue{};
};

// Startup index of the ItemPolish table, the equipment that can be tempered and the tempering
// additives. Immutable after construction and safe to share between sessions.
class ItemPolishCatalog {
public:
  // Null when the catalog lacks one of the Item, ItemAttribute, ItemPolish or
  // ItemPolishIncreaseValue tables (test catalogs without tempering).
  static std::shared_ptr<const ItemPolishCatalog> load(ClientCatalog &catalog);

  // Throws std::runtime_error for a malformed ItemPolish, increase or additive row.
  explicit ItemPolishCatalog(ClientCatalog &catalog);

  // Null when the item cannot be tempered.
  const PolishableItem *item(int cms_id) const;

  // The row that reaches target_step; null when the catalog has no row the server can apply
  // (rows with a second material or a non-Gold cost are not modelled).
  const PolishRequirement *requirement(const PolishableItem &item, int target_step) const;

  // Null when cms_id is not a tempering additive.
  const PolishAdditive *additive(int cms_id) const;

private:
  // (grade, equipCategory, target polishStep)
  using RequirementKey = std::tuple<std::string, std::string, int>;

  std::map<RequirementKey, PolishRequirement> requirements_;
  std::unordered_map<int, PolishableItem> items_;
  std::unordered_map<int, PolishAdditive> additives_;
};

} // namespace chronicles
