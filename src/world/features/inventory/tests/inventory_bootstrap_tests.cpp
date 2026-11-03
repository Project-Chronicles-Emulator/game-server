#include "world/features/inventory/inventory_bootstrap.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

TEST(InventoryBootstrapOwner, TutorialKeepsItemsButHasNoNonTutorialStackOrMoneySnapshot) {
  inventory::InventoryBootstrap bootstrap;
  std::optional<WorldProgress> tutorial_progress;
  bootstrap.configure(tutorial_progress);

  Character character;
  character.items.push_back(CharacterItem{17, 11500101, 4});
  const auto packet = bootstrap.items_snapshot(character);
  EXPECT_EQ(packet.opcode, 130);
  ASSERT_EQ(packet.message.at("items").size(), 1U);
  EXPECT_FALSE(packet.message.contains("stackables"));
  EXPECT_FALSE(bootstrap.money_snapshot());
}

TEST(InventoryBootstrapOwner, OwnsOrderedStackTombstonesAndZeroGoldWithoutMovingOtherProgress) {
  inventory::InventoryBootstrap bootstrap;
  std::optional<WorldProgress> progress{WorldProgress{}};
  progress->gold = 0;
  progress->stackables = {{9, 4}, {3, 0}};
  progress->trade_vouchers.emplace(77, TradeVoucherRecord{});
  bootstrap.configure(progress);
  // Later edits to the source progress cannot replace the owner's moved entry cache.
  progress->stackables = {{81, 12}};
  ASSERT_EQ(progress->trade_vouchers.size(), 1U);

  Character character;
  const auto items = bootstrap.items_snapshot(character);
  ASSERT_EQ(items.message.at("stackables").size(), 2U);
  EXPECT_EQ(items.message.at("stackables")[0],
            (nlohmann::json{{"item_cms_id", 3}, {"quantity", 0}}));
  EXPECT_EQ(items.message.at("stackables")[1],
            (nlohmann::json{{"item_cms_id", 9}, {"quantity", 4}}));

  const auto money = bootstrap.money_snapshot();
  ASSERT_TRUE(money);
  EXPECT_EQ(money->opcode, 126);
  EXPECT_EQ(money->message.at("update"), false);
  EXPECT_EQ(money->message.at("money").at("values"), (nlohmann::json{{"0", "0"}}));
}

TEST(InventoryBootstrapOwner, NonzeroTokenIsRestoredAndTutorialReconfigurationClearsAllBalances) {
  inventory::InventoryBootstrap bootstrap;
  std::optional<WorldProgress> progress{WorldProgress{}};
  progress->gold = 31;
  progress->hersander_token = 7;
  progress->stackables = {{2, 6}};
  bootstrap.configure(progress);
  ASSERT_TRUE(bootstrap.money_snapshot());
  EXPECT_EQ(bootstrap.money_snapshot()->message.at("money").at("values"),
            (nlohmann::json{{"0", "31"}, {"1", "7"}}));

  std::optional<WorldProgress> tutorial_progress;
  bootstrap.configure(tutorial_progress);
  EXPECT_FALSE(bootstrap.money_snapshot());
  EXPECT_FALSE(bootstrap.items_snapshot(Character{}).message.contains("stackables"));
}

} // namespace
} // namespace chronicles
