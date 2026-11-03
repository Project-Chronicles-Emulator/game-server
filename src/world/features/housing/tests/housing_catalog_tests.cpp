#include "data/client_catalog.hpp"
#include "world/features/housing/housing_catalog.hpp"
#include "testing/housing_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace chronicles::housing {
namespace {

using namespace testing_support;

TEST(HousingCatalog, IndexesZonesMapsAndLots) {
  const auto catalog = housing_catalog();

  const auto *zone = catalog->zone(kZoneKey);
  ASSERT_NE(zone, nullptr);
  EXPECT_EQ(zone->zone_name, "Zone_01_Housing_001");
  ASSERT_TRUE(zone->map_id.has_value());
  EXPECT_EQ(*zone->map_id, kHousingMapId);
  EXPECT_EQ(catalog->zone_for_map(kHousingMapId), zone);
  EXPECT_EQ(catalog->zone_for_map(9012), nullptr);

  // The second zone has lots but no HousingArea map in this build, so it is not rentable.
  ASSERT_NE(catalog->zone(kUnmappedZoneKey), nullptr);
  EXPECT_FALSE(catalog->zone(kUnmappedZoneKey)->map_id.has_value());
  ASSERT_EQ(catalog->rentable_zones().size(), 1U);

  const auto lots = catalog->sites_in_zone(kZoneKey);
  ASSERT_EQ(lots.size(), 3U);
  EXPECT_EQ(lots.front()->lot_number, kSmallLot);
  EXPECT_EQ(catalog->site(kMediumLot)->type, SiteType::medium);
}

TEST(HousingCatalog, IndexesOnlyUnconditionalPoseInteractions) {
  const auto catalog = housing_catalog();

  const auto *chair = catalog->furniture_interactions(kChairDoodad);
  ASSERT_NE(chair, nullptr);
  EXPECT_EQ(*chair,
            (std::vector<FurnitureInteraction>{{kSitInteraction, kSitAnimation, true},
                                               {kStandInteraction, kStandAnimation, false}}));
  // The farm's conditional Use and its executing animation are not poses.
  EXPECT_EQ(catalog->furniture_interactions(kFarmDoodad), nullptr);
  EXPECT_EQ(catalog->furniture_interactions(kPresetDoodad), nullptr);
}

TEST(HousingCatalog, LinksBuildingsObjectsAndCosts) {
  const auto catalog = housing_catalog();

  ASSERT_NE(catalog->housing(kMainBuilding), nullptr);
  EXPECT_EQ(catalog->housing(kMainBuilding)->item_cms_id, kMainBuildingItem);
  EXPECT_EQ(catalog->item_for_doodad(kChairDoodad), kChairItem);
  // A crafting station is a building item that the client also places as its doodad.
  EXPECT_EQ(catalog->housing(kFacility)->item_cms_id, kFacilityItem);
  EXPECT_EQ(catalog->item_for_doodad(kStationDoodad), kFacilityItem);
  EXPECT_FALSE(catalog->item_for_doodad(kPresetDoodad).has_value());
  EXPECT_TRUE(catalog->is_storage_item(kChairItem));
  EXPECT_FALSE(catalog->is_discardable_storage_item(kChairItem));
  EXPECT_TRUE(catalog->is_discardable_storage_item(kFarmItem));
  EXPECT_FALSE(catalog->is_storage_item(kPermitItem));

  const auto *small = catalog->rent_cost(AuthorityType::individual, SiteType::small);
  ASSERT_NE(small, nullptr);
  EXPECT_EQ(small->rent_item_cms_id, kPermitItem);
  EXPECT_EQ(small->rent_item_count, 60);
  EXPECT_EQ(small->unlock_value, 0);
  const auto *medium = catalog->rent_cost(AuthorityType::individual, SiteType::medium);
  ASSERT_NE(medium, nullptr);
  EXPECT_EQ(medium->unlock_money, MoneyKind::hersander_token);
  EXPECT_EQ(medium->unlock_value, 1000);
  // The guild row is not split by site size.
  ASSERT_NE(catalog->rent_cost(AuthorityType::guild, SiteType::large), nullptr);
  EXPECT_EQ(catalog->rent_cost(AuthorityType::guild, SiteType::large)->rent_item_count, 3000);

  const auto *upgrade = catalog->upgrade(kMainBuilding, kUpgradedBuilding);
  ASSERT_NE(upgrade, nullptr);
  ASSERT_EQ(upgrade->materials.size(), 1U);
  EXPECT_EQ(upgrade->materials.front(), (ItemCount{kUpgradeMaterial, 20}));
}

TEST(HousingCatalog, ConvertsPresetPositionsToSiteLocalMillimetres) {
  const auto catalog = housing_catalog();
  const auto *preset = catalog->preset(kSmallPreset);
  ASSERT_NE(preset, nullptr);
  ASSERT_EQ(preset->houses.size(), 1U);
  EXPECT_EQ(preset->houses.front().position, (LocalPosition{-4800, -300, 0}));
  EXPECT_EQ(preset->houses.front().attach_index, -1);
  ASSERT_EQ(preset->doodads.size(), 1U);
  EXPECT_EQ(preset->doodads.front().position, (LocalPosition{7425, -3675, 0}));
  EXPECT_FLOAT_EQ(preset->doodads.front().yaw, 180.0F);

  const auto *add_ons = catalog->model_add_ons(kFacility);
  ASSERT_NE(add_ons, nullptr);
  ASSERT_EQ(add_ons->size(), 1U);
  EXPECT_EQ(add_ons->front().doodad_cms_id, kStationDoodad);
  EXPECT_EQ(add_ons->front().position, (LocalPosition{-880, 0, 0}));
}

TEST(HousingCatalog, ReadsGameVariables) {
  const auto catalog = housing_catalog();
  const auto &rules = catalog->rules();
  EXPECT_EQ(rules.site_rent_duration_days, 30);
  EXPECT_EQ(rules.edit_save_cooldown.count(), 5000);
  EXPECT_EQ(rules.object_limit.at(SiteType::small), 8);
  EXPECT_EQ(rules.farm_doodad_limit.at(SiteType::small), 1);
  // Absent rows keep the exported defaults.
  EXPECT_EQ(rules.object_limit.at(SiteType::large), 200);
}

TEST(HousingCatalog, RejectsInvalidRows) {
  auto tables = housing_tables();
  tables["HousingSite"].front()["lotNumber"] = "999";
  EXPECT_THROW(HousingCatalog::from_rows(tables), std::runtime_error);

  tables = housing_tables();
  tables["HousingSite"].front()["fk_PresetId"] = "MissingPreset";
  EXPECT_THROW(HousingCatalog::from_rows(tables), std::runtime_error);

  tables = housing_tables();
  tables["HousingPresetDoodad"].front()["housingDoodadId"] = "16384";
  EXPECT_THROW(HousingCatalog::from_rows(tables), std::runtime_error);

  tables = housing_tables();
  tables.erase("Housing");
  EXPECT_THROW(HousingCatalog::from_rows(tables), std::runtime_error);
}

TEST(HousingUnitIds, MatchTheClientLayout) {
  // AAC_Unit_MakeTypedUnitId: type | (flag << 4) | (key << 12).
  EXPECT_EQ(housing_unit_id(101), (std::uint64_t{101} << 12) | 5);
  const auto doodad = housing_doodad_unit_id(101, 2, 7);
  EXPECT_EQ(doodad >> 32, (std::uint64_t{2} << 14) | 7);
  EXPECT_EQ(doodad & 0xFFFF'FFFF, housing_unit_id(101));

  const auto parsed = parse_housing_doodad_unit_id(doodad);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->lot_number, 101);
  EXPECT_EQ(parsed->house_id, 2U);
  EXPECT_EQ(parsed->doodad_id, 7U);

  EXPECT_FALSE(parse_housing_doodad_unit_id(housing_unit_id(101)).has_value());
  EXPECT_FALSE(parse_housing_doodad_unit_id(doodad ^ 0x5).has_value());
  EXPECT_FALSE(parse_housing_doodad_unit_id(doodad | (std::uint64_t{1} << 63)).has_value());
  EXPECT_THROW(housing_unit_id(0), std::invalid_argument);
  EXPECT_THROW(housing_doodad_unit_id(101, 0, 0), std::invalid_argument);
}

TEST(HousingUnitIds, SectorFollowsTheClientIndexFormula) {
  // AAC_Sector_Candidate_IndexFromPositionCm: floor(cm / 5100) + 1, 14-bit fields.
  EXPECT_EQ(sector_id_for_position(0.0, 0.0), 1U | (1U << 14));
  EXPECT_EQ(sector_id_for_position(5100.0, 10200.0), 2U | (3U << 14));
  const auto negative = sector_id_for_position(-53530.11, 343941.5);
  EXPECT_EQ(negative & 0x3FFF, static_cast<std::uint32_t>(-10) & 0x3FFF);
  EXPECT_EQ(negative >> 14, 68U);
}

// Loads the verified export when CHRONICLES_CLIENT_CATALOG names it (data/generated/catalog).
TEST(HousingCatalog, ExportedCatalogLoads) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }

  ClientCatalog client(catalog_path);
  const auto catalog = HousingCatalog::load(client);
  ASSERT_EQ(catalog->rentable_zones().size(), 1U);
  const auto *zone = catalog->rentable_zones().front();
  EXPECT_EQ(zone->zone_key, 112);
  EXPECT_EQ(zone->map_id, 20101);
  EXPECT_EQ(catalog->sites_in_zone(zone->zone_key).size(), 41U);
  EXPECT_EQ(catalog->sites_in_zone(702).size(), 30U);
  EXPECT_EQ(catalog->rules().object_limit.at(SiteType::small), 160);
  EXPECT_EQ(catalog->rules().edit_save_cooldown.count(), 5000);
  EXPECT_EQ(catalog->rent_cost(AuthorityType::individual, SiteType::large)->rent_item_count, 480);
  ASSERT_NE(catalog->preset("GT_MAIN_S__SUB_2__F_1"), nullptr);
  EXPECT_EQ(catalog->preset("GT_MAIN_S__SUB_2__F_1")->doodads.size(), 565U);
  EXPECT_EQ(catalog->item_for_doodad(160066), 1900000001);
  // The five crafting stations (Kitchen 85002 ... Smelting Furnace 85001) place as doodads.
  EXPECT_EQ(catalog->item_for_doodad(85002), 2000000020);
  EXPECT_EQ(catalog->item_for_doodad(85003), 2000000021);
  EXPECT_EQ(catalog->item_for_doodad(85004), 2000000022);
  EXPECT_EQ(catalog->item_for_doodad(85005), 2000000023);
  EXPECT_EQ(catalog->item_for_doodad(85001), 2000000024);
  // [Furniture] chair doodad 161053 (item 1900469002) sits and stands through group 70.
  ASSERT_NE(catalog->furniture_interactions(161053), nullptr);
  EXPECT_EQ(*catalog->furniture_interactions(161053),
            (std::vector<FurnitureInteraction>{{70, 70, true}, {73, 71, false}}));
}

} // namespace
} // namespace chronicles::housing
