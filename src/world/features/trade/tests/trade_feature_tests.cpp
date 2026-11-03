#include "world/features/companion/tests/companion_test_support.hpp"
#include "world/features/trade/trade_feature.hpp"

namespace chronicles {
namespace {

using namespace companion_test_support;

class TradeFeatureTest : public TradeTest, public trade::TradeSessionContext {
protected:
  void SetUp() override {
    TradeTest::SetUp();
    feature_ = std::make_unique<trade::TradeFeature>(
        trade_catalog(), std::make_shared<const std::set<int>>(std::set<int>{1001316}),
        item_catalog());
    const auto position = here(origin_, 1000);
    npc_ = AnnouncedUnit{77, 1001316, kMap, "test", {position.x_mm, position.y_mm, position.z_mm}};
  }

  const AnnouncedUnit *trade_interaction_npc(std::uint64_t unit_id) const override {
    requested_unit_ = unit_id;
    return unit_id == npc_.unit_id ? &npc_ : nullptr;
  }

  std::int64_t trade_epoch_ticks() const override {
    return kNow;
  }

  std::unique_ptr<trade::TradeFeature> feature_;
  AnnouncedUnit npc_;
  mutable std::optional<std::uint64_t> requested_unit_;
};

TEST_F(TradeFeatureTest, DialogueUsesTheAnnouncedManagerAndOriginalWireUnit) {
  companion::SessionResult result;
  const WorldGameplayRequest dialogue{
      trade::kDialogueStartRequest, 7,
      request(trade::kDialogueStartRequest, {{"target_unit_id", "77"}})};
  ASSERT_TRUE(feature_->handle_dialogue(trade_.get(), dialogue, here(origin_), *this, result));
  EXPECT_EQ(requested_unit_, 77);
  const auto replies = own(result.own, trade::kDialogueStartResponse);
  ASSERT_EQ(replies.size(), 1U);
  EXPECT_EQ(replies.front().at("target_unit_id"), "77");
  EXPECT_EQ(replies.front().at("target_unit_pos").at("millimeter_x"), npc_.position.x_mm);

  const WorldGameplayRequest end{trade::kDialogueEndRequest, 8,
                                 request(trade::kDialogueEndRequest, Json::object())};
  ASSERT_TRUE(feature_->handle_request(trade_.get(), end, here(origin_), *this, result));
  const auto ended = own(result.own, trade::kDialogueEndNotification);
  ASSERT_EQ(ended.size(), 1U);
  EXPECT_EQ(ended.front().at("player_unit_id"), player_wire_unit_id(character_.id));
}

TEST_F(TradeFeatureTest, InvalidTargetsAndUnavailableWorldFallThroughBeforeDialogueWork) {
  companion::SessionResult result;
  WorldGameplayRequest dialogue{trade::kDialogueStartRequest, 7, {{"target_unit_id", 77}}};
  EXPECT_FALSE(feature_->handle_dialogue(trade_.get(), dialogue, here(origin_), *this, result));
  EXPECT_FALSE(requested_unit_);

  dialogue.message = {{"target_unit_id", "+77"}};
  EXPECT_FALSE(feature_->handle_dialogue(trade_.get(), dialogue, here(origin_), *this, result));
  EXPECT_FALSE(requested_unit_);

  dialogue.message = {{"target_unit_id", "77"}};
  EXPECT_FALSE(feature_->handle_dialogue(trade_.get(), dialogue, std::nullopt, *this, result));
  EXPECT_FALSE(requested_unit_);

  npc_.npc_template_id = 999;
  EXPECT_FALSE(feature_->handle_dialogue(trade_.get(), dialogue, here(origin_), *this, result));
  EXPECT_EQ(requested_unit_, 77);
  EXPECT_TRUE(result.own.empty());
}

TEST(TradeBootstrap, PreservesUnredeemedOnlyOrderingIncludingExpiredRecordsAndClearsForTutorial) {
  std::optional<WorldProgress> progress{WorldProgress{}};
  progress->trade_vouchers = {{9, {kItemRewardGood, kNow, kNow + 100, false}},
                              {4, {kGoldRewardGood, kNow - 200, kNow + 100, true}},
                              {7, {kGoldRewardGood, kNow - 200, kNow - 100, false}}};
  trade::TradeBootstrap bootstrap;
  bootstrap.configure(progress);
  progress->trade_vouchers.emplace(11,
                                   TradeVoucherRecord{kItemRewardGood, kNow, kNow + 200, false});

  const auto packet = bootstrap.snapshot();
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->opcode, 142);
  const auto payload = wire(packet->opcode, packet->message);
  ASSERT_EQ(payload.at("vouchers").size(), 2U);
  EXPECT_EQ(payload.at("vouchers")[0].at("voucher_id"), "7");
  EXPECT_EQ(payload.at("vouchers")[1].at("voucher_id"), "9");

  std::optional<WorldProgress> tutorial;
  bootstrap.configure(tutorial);
  EXPECT_FALSE(bootstrap.snapshot());
}

} // namespace
} // namespace chronicles
