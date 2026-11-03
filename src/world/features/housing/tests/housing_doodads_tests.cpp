#include "testing/housing_test_support.hpp"
#include "world/features/housing/housing_doodads.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

namespace chronicles::housing {
namespace {

using namespace testing_support;
using Json = nlohmann::json;

TEST(HousingDoodadProfile, IndexesPhasesTransitionsAndAnimations) {
  const HousingDoodadProfile profile(garden_profile_json());
  EXPECT_EQ(profile.doodad_count(), 1U);
  EXPECT_EQ(profile.animation_count(), 2U);
  EXPECT_EQ(profile.sp_regen_delay_ms(), 300);

  const auto *planting = profile.interaction(kFarmDoodad, kEmptyPlotPhase, kPlantInteraction);
  ASSERT_NE(planting, nullptr);
  EXPECT_EQ(planting->animation_id, kPlantAnimation);
  ASSERT_EQ(planting->conditions.size(), 1U);
  EXPECT_EQ(planting->conditions.front().kind, DoodadCondition::Kind::consume);
  EXPECT_EQ(planting->conditions.front().count, 5);
  EXPECT_FALSE(planting->reward.has_value());

  // The empty plot has no timer; the sown plot has no interaction.
  EXPECT_EQ(profile.timer(kFarmDoodad, kEmptyPlotPhase), nullptr);
  EXPECT_EQ(profile.interaction(kFarmDoodad, kSownPhase, kPlantInteraction), nullptr);
  ASSERT_NE(profile.timer(kFarmDoodad, kSownPhase), nullptr);
  EXPECT_EQ(profile.timer(kFarmDoodad, kSownPhase)->duration_ms, kGrowthMilliseconds);

  const auto *harvest = profile.interaction(kFarmDoodad, kRipePhase, kHarvestInteraction);
  ASSERT_NE(harvest, nullptr);
  ASSERT_TRUE(harvest->reward.has_value());
  EXPECT_FALSE(harvest->reward->looting_reason);
  EXPECT_EQ(profile.animation(kHarvestAnimation)->tool_equipment_type, "Sickle");
  EXPECT_EQ(profile.doodad(kChairDoodad), nullptr);
}

TEST(HousingDoodadProfile, RejectsMalformedProfiles) {
  auto wrong_version = garden_profile_json();
  wrong_version["format_version"] = 2;
  EXPECT_THROW(HousingDoodadProfile{wrong_version}, std::runtime_error);

  auto unknown_animation = garden_profile_json();
  unknown_animation["animations"] = Json::array();
  EXPECT_THROW(HousingDoodadProfile{unknown_animation}, std::runtime_error);

  auto two_timers = garden_profile_json();
  auto &sown = two_timers["doodads"][0]["phases"][1]["transitions"];
  sown.push_back(sown[0]);
  EXPECT_THROW(HousingDoodadProfile{two_timers}, std::runtime_error);

  auto no_next = garden_profile_json();
  no_next["doodads"][0]["phases"][1]["transitions"][0]["next"] = Json::array();
  EXPECT_THROW(HousingDoodadProfile{no_next}, std::runtime_error);

  auto repeated = garden_profile_json();
  repeated["doodads"].push_back(repeated["doodads"][0]);
  EXPECT_THROW(HousingDoodadProfile{repeated}, std::runtime_error);
}

TEST(HousingDoodadProfile, RollsGuaranteedRewardsAndWeightedPhases) {
  const HousingDoodadProfile profile(garden_profile_json());
  const auto *harvest = profile.interaction(kFarmDoodad, kRipePhase, kHarvestInteraction);
  std::mt19937 random(7);
  const auto granted = roll_doodad_reward(*harvest->reward, random);
  EXPECT_EQ(granted, (std::map<int, int>{{kCropItem, 2}}));

  // Weighted choices follow their weights (decay ratio 104:276, 105:22, 20:2 in the export).
  const std::vector<DoodadPhaseChoice> choices{{104, 276}, {105, 22}, {20, 2}};
  std::map<int, int> counts;
  for (int roll = 0; roll < 3000; ++roll) {
    ++counts[choose_doodad_phase(choices, random)];
  }
  EXPECT_GT(counts[104], counts[105]);
  EXPECT_GT(counts[105], counts[20]);
}

// Loads the regenerated profile when CHRONICLES_CLIENT_CATALOG names data/generated/catalog.
TEST(HousingDoodadProfile, ExportedProfileLoadsTheGardenPlots) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }
  const auto path =
      std::filesystem::path(catalog_path).parent_path() / "housing" / "doodads.json";
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "Run tools/build_housing_data.py to export " << path.string();
  }

  const auto profile = HousingDoodadProfile::load(path);
  // Small garden plot: phase 1 plants with five Barley seeds (interaction 160005).
  const auto *planting = profile->interaction(160066, 1, 160005);
  ASSERT_NE(planting, nullptr);
  EXPECT_EQ(planting->conditions.front().item_cms_id, 50000);
  EXPECT_EQ(planting->conditions.front().count, 5);
  // A sown plot grows for 54 minutes before its weighted outcome.
  ASSERT_NE(profile->timer(160066, 102), nullptr);
  EXPECT_EQ(profile->timer(160066, 102)->duration_ms, 3240000);
  EXPECT_EQ(profile->timer(160066, 102)->next.size(), 3U);
  EXPECT_NE(profile->doodad(160068), nullptr);
  EXPECT_NE(profile->doodad(160069), nullptr);
  EXPECT_NE(profile->doodad(161124), nullptr);
}

} // namespace
} // namespace chronicles::housing
