#include "data/client_catalog.hpp"
#include "world/features/inventory/item_polish_catalog.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <utility>

namespace chronicles {
namespace {

// Opt-in like the other catalog-backed tests: set CHRONICLES_CLIENT_CATALOG to the exported
// data/generated/catalog directory.
std::optional<ClientCatalog> exported_catalog() {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    return std::nullopt;
  }
  return std::optional<ClientCatalog>(std::in_place, catalog_path);
}

TEST(ItemPolishCatalog, ExportedCatalogLoadsEveryTemperingRowAndAdditive) {
  auto catalog = exported_catalog();
  if (!catalog) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }

  const auto polish = ItemPolishCatalog::load(*catalog);
  ASSERT_TRUE(polish);

  // Grand weapon 13102102 (seven captured exchanges): weapons reach step 15.
  const auto *weapon = polish->item(13102102);
  ASSERT_NE(weapon, nullptr);
  EXPECT_EQ(weapon->grade, "Grand");
  EXPECT_EQ(weapon->category, "Weapon");
  EXPECT_EQ(weapon->max_step, 15);
  EXPECT_FALSE(weapon->has_polish_residue);

  const auto *first = polish->requirement(*weapon, 1);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->success_probability, 10000);
  EXPECT_EQ(first->material_cms_id, 90211101);
  EXPECT_EQ(first->material_count, 1);
  EXPECT_EQ(first->gold_cost, 1000);
  const auto *second = polish->requirement(*weapon, 2);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->success_probability, 6000);
  EXPECT_EQ(second->fail_probability, 4000);
  EXPECT_EQ(second->gold_cost, 1425);
  EXPECT_EQ(polish->requirement(*weapon, 16), nullptr);

  // Grand armor 12401201: armor reaches step 10.
  const auto *armor = polish->item(12401201);
  ASSERT_NE(armor, nullptr);
  EXPECT_EQ(armor->category, "Armor");
  EXPECT_EQ(armor->max_step, 10);

  // Tempering stones are materials, not temperable items or additives.
  EXPECT_EQ(polish->item(90211101), nullptr);
  EXPECT_EQ(polish->additive(90211101), nullptr);

  const auto *reinforced = polish->additive(90351001);
  ASSERT_NE(reinforced, nullptr);
  EXPECT_EQ(reinforced->effect, PolishAdditiveEffect::AddSuccessPercent);
  EXPECT_EQ(reinforced->value, 10);
  const auto *mystical = polish->additive(90551001);
  ASSERT_NE(mystical, nullptr);
  EXPECT_EQ(mystical->value, 100);
  const auto *safeguard = polish->additive(90552001);
  ASSERT_NE(safeguard, nullptr);
  EXPECT_EQ(safeguard->effect, PolishAdditiveEffect::DestructionProhibition);
}

} // namespace
} // namespace chronicles
