#include "world/features/character/achievements.hpp"

#include "common/currency.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace chronicles::character {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kCompletedAt = 17933000000000000;

Json achievement(int id, const char *reward = "Money", const char *parameters = "\"value\":100") {
  return {{"id", std::to_string(id)},
          {"parentId", "0"},
          {"FK_achievementGoalTable", std::to_string(id)},
          {"typeCase", reward},
          {"type", parameters}};
}

Json goal(int id, int order, const char *kind, const char *parameters) {
  return {{"id", std::to_string(id)},
          {"order", std::to_string(order)},
          {"goalTypeCase", kind},
          {"goalType", parameters}};
}

AchievementCatalog level_catalog() {
  return {{achievement(1000)}, {goal(1000, 1, "CharLevel", "\"charLevel\":10")}, {}};
}

AchievementGoalValue stored_facts(const WorldProgress &progress) {
  return [&progress](const AchievementGoal &definition) {
    return stored_achievement_goal_value(definition, progress);
  };
}

TEST(Achievements, CannotClaimOrCompleteUsingOnlyTheClientsStateAssertion) {
  const auto catalog = level_catalog();
  WorldProgress before;
  EXPECT_EQ(catalog.plan(before, 1000, 2, kCompletedAt, stored_facts(before), nullptr).error,
            x5::ec::Achievement_InvalidState);
  const auto incomplete =
      catalog.plan(before, 1000, 1, kCompletedAt, stored_facts(before), nullptr);
  EXPECT_EQ(incomplete.error, x5::ec::Achievement_InvalidCharLevel);
  EXPECT_EQ(incomplete.after, before);
  EXPECT_TRUE(incomplete.packets.empty());
  EXPECT_EQ(catalog.plan(before, 999, 1, kCompletedAt, stored_facts(before), nullptr).error,
            x5::ec::Achievement_InvalidAchievementGoalCmsId);
  EXPECT_EQ(catalog.plan(before, 1000, 0, kCompletedAt, stored_facts(before), nullptr).error,
            x5::ec::Achievement_InvalidState);
}

TEST(Achievements, CompletionAndClaimHaveOneDurableReceiptAndStableCompletionTime) {
  const auto catalog = level_catalog();
  WorldProgress before;
  before.level = 10;
  const auto completed = catalog.plan(before, 1000, 1, kCompletedAt, stored_facts(before), nullptr);
  ASSERT_EQ(completed.error, x5::ec::Success);
  ASSERT_EQ(completed.packets.size(), 1u);
  EXPECT_EQ(completed.packets.front().opcode, 121);
  EXPECT_EQ(completed.after.gold, 0);
  EXPECT_EQ(completed.after.achievements.at(1000), (AchievementRecord{1, kCompletedAt}));
  EXPECT_EQ(completed.after.revision, before.revision);

  const auto claimed = catalog.plan(completed.after, 1000, 2, kCompletedAt + 100,
                                    stored_facts(completed.after), nullptr);
  ASSERT_EQ(claimed.error, x5::ec::Success);
  ASSERT_EQ(claimed.packets.size(), 2u);
  EXPECT_EQ(claimed.packets.front().opcode, 169);
  EXPECT_EQ(claimed.after.gold, 100);
  EXPECT_EQ(claimed.after.achievements.at(1000), (AchievementRecord{2, kCompletedAt}));
  const auto repeated = catalog.plan(claimed.after, 1000, 2, kCompletedAt + 200,
                                     stored_facts(claimed.after), nullptr);
  EXPECT_EQ(repeated.after, claimed.after);
  ASSERT_EQ(repeated.packets.size(), 1u);
  EXPECT_EQ(repeated.packets.front().opcode, 121);
  const auto stale_complete = catalog.plan(claimed.after, 1000, 1, kCompletedAt + 300,
                                           stored_facts(claimed.after), nullptr);
  EXPECT_EQ(stale_complete.after, claimed.after);
  EXPECT_EQ(stale_complete.packets.front().message.at("achievement").at("state"),
            "EX5AchievementState_Rewarded");

  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : claimed.packets) {
    EXPECT_EQ(protocol.decode(protocol.encode(packet.opcode, packet.message)).at("status"),
              "decoded");
  }
  const auto snapshot = achievement_snapshot(claimed.after);
  EXPECT_EQ(snapshot.at("achievements").front().at("complete_tick"), std::to_string(kCompletedAt));
  EXPECT_EQ(protocol.decode(protocol.encode(143, snapshot)).at("status"), "decoded");
}

TEST(Achievements, EveryGoalMustBeSatisfiedAndMissingServerFactsDoNotCountAsCompletion) {
  AchievementCatalog catalog({achievement(1)},
                             {goal(1, 1, "CharLevel", "\"charLevel\":10"),
                              goal(1, 2, "QuestCount", "\"questId\":42,\"count\":2"),
                              goal(1, 3, "AdventureObjectTarget", "\"adventureObjectId\":88")},
                             {});
  WorldProgress before;
  before.level = 10;
  before.quests[42].success_count = 2;
  before.adventure_objects[88] = 1;
  EXPECT_EQ(catalog.plan(before, 1, 1, kCompletedAt, stored_facts(before), nullptr).error,
            x5::ec::Achievement_InvalidAdventureObjectTarget);
  before.adventure_objects[88] = 2;
  EXPECT_EQ(catalog.plan(before, 1, 1, kCompletedAt, stored_facts(before), nullptr).error,
            x5::ec::Success);
  const AchievementGoalValue unavailable = [](const AchievementGoal &) {
    return std::optional<std::int64_t>{};
  };
  EXPECT_EQ(catalog.plan(before, 1, 1, kCompletedAt, unavailable, nullptr).error,
            x5::ec::Achievement_InvalidCharLevel);
}

// CUSTOM: graduated characters hold the full wallet; the claim fills it to the limit and the
// achievement becomes rewarded instead of staying unclaimable forever.
TEST(Achievements, FullCurrencyBalanceFillsTheWalletAndRewardsTheAchievement) {
  const auto catalog = level_catalog();
  WorldProgress before;
  before.gold = kMoneyLimit - 99;
  before.achievements[1000] = {1, kCompletedAt};
  const auto claimed = catalog.plan(before, 1000, 2, kCompletedAt, stored_facts(before), nullptr);
  ASSERT_EQ(claimed.error, x5::ec::Success);
  EXPECT_EQ(claimed.after.gold, kMoneyLimit);
  EXPECT_EQ(claimed.after.achievements.at(1000).state, 2);
  EXPECT_EQ(claimed.packets.front().message.at("money_changes").front().at("change"), "99");

  WorldProgress full = before;
  full.gold = kMoneyLimit;
  const auto at_limit = catalog.plan(full, 1000, 2, kCompletedAt, stored_facts(full), nullptr);
  ASSERT_EQ(at_limit.error, x5::ec::Success);
  EXPECT_EQ(at_limit.after.gold, kMoneyLimit);
  EXPECT_EQ(at_limit.after.achievements.at(1000).state, 2);
}

TEST(Achievements, TokenAndTrophyRewardsUseTheirOwnContracts) {
  AchievementCatalog catalog(
      {achievement(1, "Money", "\"moneyType\":\"EX5MoneyType_HersanderToken\",\"value\":50"),
       achievement(2, "Trophy", "\"value\":37")},
      {goal(1, 1, "CharLevel", "\"charLevel\":10"), goal(2, 1, "CharLevel", "\"charLevel\":10")},
      {{{"id", "37"}}});
  WorldProgress before;
  before.achievements[1] = {1, kCompletedAt};
  before.achievements[2] = {1, kCompletedAt};
  const auto token = catalog.plan(before, 1, 2, kCompletedAt, stored_facts(before), nullptr);
  ASSERT_EQ(token.error, x5::ec::Success);
  EXPECT_EQ(token.after.hersander_token, 50);
  EXPECT_EQ(token.after.gold, 0);
  EXPECT_EQ(token.packets.front().message.at("money_changes").front().at("money_type"),
            "EX5MoneyType_HersanderToken");
  const auto trophy = catalog.plan(before, 2, 2, kCompletedAt, stored_facts(before), nullptr);
  EXPECT_EQ(trophy.error, x5::ec::Success);
  EXPECT_EQ(trophy.after.achievements.at(2).state, 2);
  ASSERT_EQ(trophy.packets.size(), 1u);
  EXPECT_EQ(trophy.packets.front().opcode, 121);
  EXPECT_EQ(trophy.after.housing_items, before.housing_items);
}

TEST(Achievements, VerifiedCatalogLoadsEveryAuthoredAchievement) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified export";
  }
  ClientCatalog source(catalog_path);
  const auto catalog = AchievementCatalog::load(source);
  EXPECT_EQ(catalog->size(), source.row_count("Achievement"));
}

} // namespace
} // namespace chronicles::character
