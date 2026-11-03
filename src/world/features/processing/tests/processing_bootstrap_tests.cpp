#include "world/features/processing/processing_feature.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace chronicles {
namespace {

TEST(ProcessingBootstrapOwner, ExpertiseHasEveryOfficialEnumAndNewCharactersHaveNoLearnedRecipes) {
  const auto expertise = processing::ProcessingFeature::expertise_entry_snapshot();
  EXPECT_EQ(expertise.opcode, 128);
  const auto &skills = expertise.message.at("skills");
  ASSERT_EQ(skills.size(), 16U);
  EXPECT_TRUE(std::ranges::all_of(skills, [](const auto &skill) {
    return skill.at("level") == "EX5ExpertiseLvType_Lv1";
  }));
  // TradeItem is present despite having no corresponding ExpertiseSkill catalog row.
  EXPECT_TRUE(std::ranges::any_of(skills, [](const auto &skill) {
    return skill.at("type") == "EX5ExpertiseType_TradeItem";
  }));

  const auto crafts = processing::ProcessingFeature::crafts_entry_snapshot();
  EXPECT_EQ(crafts.opcode, 132);
  EXPECT_EQ(crafts.message, nlohmann::json::object());
}

TEST(ProcessingBootstrapOwner, ReconnectReplacesLearnedRecipesWithTheSavedOrderedSet) {
  const auto crafts = processing::ProcessingFeature::crafts_entry_snapshot({60203032, 60203031});
  EXPECT_EQ(crafts.opcode, 132);
  EXPECT_EQ(crafts.message,
            (nlohmann::json{{"craft_ids", nlohmann::json::array({60203031, 60203032})}}));
  EXPECT_FALSE(crafts.message.value("update", false));
}

} // namespace
} // namespace chronicles
