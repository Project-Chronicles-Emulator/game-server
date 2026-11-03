#include "world/features/character/tests/character_feature_test_support.hpp"

#include "data/shop_catalog.hpp"

#include <gtest/gtest.h>

namespace chronicles::character {
namespace {

class GrantContext final : public GraduationGrantContext {
public:
  std::int64_t progression_character_id() const override {
    return store.actor.id;
  }

  std::optional<Character> progression_character() override {
    return store.progression_character();
  }

  WorldProgress progression_stored_progress() override {
    return store.progression_stored_progress();
  }

  bool progression_save(const WorldProgress &before, const WorldProgress &after) override {
    calls.push_back("level");
    return store.progression_save(before, after);
  }

  bool graduation_game_master() const override {
    return game_master;
  }

  int graduation_complete_quests() override {
    calls.push_back("quests");
    if (quests_throw) {
      throw std::runtime_error("quest fixture refusal");
    }
    return 3;
  }

  ItemBundlePlan graduation_plan_items(const ShopCatalog &, const std::vector<ItemBundleEntry> &,
                                       std::optional<std::int64_t>, const Character &,
                                       const WorldProgress &, int) override {
    calls.push_back("items");
    if (items_throw) {
      throw std::runtime_error("item fixture refusal");
    }
    return {};
  }

  std::optional<std::vector<CharacterItem>> graduation_commit_items(const WorldProgress &,
                                                                    const WorldProgress &,
                                                                    const ItemTrade &) override {
    ADD_FAILURE() << "An empty planned grant must not enter the item transaction";
    return std::nullopt;
  }

  testing_support::ProgressStore store;
  bool game_master{};
  bool quests_throw{};
  bool items_throw{};
  std::vector<std::string> calls;
};

class GraduationOwner : public ::testing::Test {
protected:
  GraduationOwner() {
    ShopCatalogTables tables;
    tables.game_variables = {
        {{"name", "ItemMaxStack"}, {"value", "1000"}},
        {{"name", "ResetBaseDateTime"}, {"value", "2023-01-02 05:00:00"}},
    };
    tables.money = {{{"type", "Gold"}, {"limitUserMax", "5000000"}}};
    items = std::make_shared<const ShopCatalog>(std::move(tables));
    loadout = std::make_shared<const GraduationLoadout>(std::vector<nlohmann::json>{}, 3);
  }

  GraduationGrantServices services() const {
    return {items, progression, loadout};
  }

  testing_support::CharacterCatalog fixture;
  ClientCatalog catalog{fixture.root()};
  std::shared_ptr<const Progression> progression = Progression::load(catalog);
  std::shared_ptr<const ShopCatalog> items;
  std::shared_ptr<const GraduationLoadout> loadout;
  GrantContext context;
};

TEST_F(GraduationOwner, GrantKeepsQuestThenItemThenLevelStageOrder) {
  const auto result = apply_graduation_grant(context, services());
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.quests_completed, 3);
  EXPECT_EQ(result.equipment_granted, 0U);
  EXPECT_TRUE(result.level_raised);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"quests", "items", "level"}));
  EXPECT_EQ(context.store.progress.level, 3);
  EXPECT_EQ(context.store.progress.level_up_points, kMaximumBuildPoints);
  EXPECT_EQ(context.store.progress.weapon_points, kMaximumBuildPoints);
}

TEST_F(GraduationOwner, EarlierStageFailuresStillAllowTheExistingFinalLevelStage) {
  context.quests_throw = true;
  context.items_throw = true;
  const auto result = apply_graduation_grant(context, services());
  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.quests_completed, 0);
  EXPECT_EQ(result.equipment_granted, 0U);
  EXPECT_TRUE(result.level_raised);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"quests", "items", "level"}));
  EXPECT_EQ(context.store.progress.level, 3);
}

TEST_F(GraduationOwner, FinalCommitConflictKeepsTheGrantAppliedWithoutRaisingLevel) {
  context.store.commit_succeeds = false;
  const auto result = apply_graduation_grant(context, services());
  EXPECT_TRUE(result.applied);
  EXPECT_FALSE(result.level_raised);
  EXPECT_EQ(context.store.progress.level, 1);
}

TEST_F(GraduationOwner, GameMasterRefusalPrecedesAnyStoredProgressRead) {
  context.game_master = true;
  const auto result = apply_graduation_grant(context, services());
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(context.store.progress_reads, 0);
  EXPECT_TRUE(context.calls.empty());
}

TEST_F(GraduationOwner, AtCapCharactersRemainIdempotentAndReceiveNoStageEffects) {
  context.store.progress.level = 3;
  const auto result = apply_graduation_grant(context, services());
  EXPECT_FALSE(result.applied);
  EXPECT_EQ(context.store.progress_reads, 1);
  EXPECT_EQ(context.store.character_reads, 0);
  EXPECT_TRUE(context.calls.empty());
}

} // namespace
} // namespace chronicles::character
