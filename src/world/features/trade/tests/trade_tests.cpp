#include "world/features/companion/tests/companion_test_support.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using namespace companion_test_support;

class SettlementFaultProgress final : public ProgressAccess {
public:
  enum class Mode { conflict, lost_acknowledgement, trade_commit_conflict };

  SettlementFaultProgress(CharacterRepository &characters, CharacterStateRepository &state,
                          std::int64_t character_id, Mode mode)
      : source_(characters, state, kAccountId, character_id, kWorldId), mode_(mode) {
  }

  WorldProgress load() override {
    return source_.load();
  }

  std::optional<Character> character() override {
    return source_.character();
  }

  std::optional<std::vector<CharacterItem>> commit(const WorldProgress &before,
                                                   const WorldProgress &after,
                                                   const ItemTrade &trade) override {
    if (mode_ == Mode::trade_commit_conflict) {
      return std::nullopt;
    }
    return source_.commit(before, after, trade);
  }

  bool commit_with_position(const WorldProgress &before, const WorldProgress &after,
                            const WorldPosition &position) override {
    if (mode_ == Mode::conflict) {
      return false;
    }
    if (!source_.commit_with_position(before, after, position)) {
      throw std::logic_error("Unexpected settlement conflict");
    }
    throw std::runtime_error("Simulated lost settlement acknowledgement");
  }

private:
  RepositoryProgressAccess source_;
  Mode mode_;
};

TEST(TradeCatalog, PlacesPostsInMillimetersOnTheirMapsAndIndexesRules) {
  const auto catalog = trade_catalog();

  const auto *destination = catalog->post(kDestinationPost);
  ASSERT_NE(destination, nullptr);
  EXPECT_TRUE(destination->placed);
  EXPECT_EQ(destination->departure.x_mm, -847260);
  EXPECT_EQ(destination->sphere_radius_mm, 12000);
  EXPECT_EQ(destination->map_ids, std::set<int>{kMap});

  EXPECT_TRUE(catalog->post_sells(kOriginPost, kItemRewardGood));
  EXPECT_FALSE(catalog->post_sells(kDestinationPost, kItemRewardGood));
  EXPECT_EQ(catalog->production_cost(kItemRewardGood)->fee, 80);
  const auto *item_reward = catalog->purchase_reward(kDestinationPost, kItemRewardGood,
                                                     kOriginPost);
  ASSERT_NE(item_reward, nullptr);
  EXPECT_EQ(item_reward->item_cms_id, kRewardItem);
  EXPECT_EQ(item_reward->item_amount, 6);
  EXPECT_TRUE(catalog->is_trade_management_dialogue(100097));
  EXPECT_EQ(catalog->patterns(3, 0), std::vector<int>{3000});
}

TEST(TradeCatalog, RejectsAPurchaseRowWithBothAnItemAndAMoneyReward) {
  auto tables = trade_tables();
  tables.trading_post_purchases.front()["moneyType"] = "Gold";
  EXPECT_THROW(trade::TradeCatalog{tables}, std::runtime_error);
}

TEST(TradeMarket, QuotesAreDeterministicInsideTheBandsAndRefreshEachWindow) {
  const auto catalog = trade_catalog();
  const auto first = trade::market_quote(*catalog, kDestinationPost, 0, kNow);
  const auto again = trade::market_quote(*catalog, kDestinationPost, 0, kNow + 1);
  EXPECT_EQ(first.basic_basis_points, again.basic_basis_points);
  EXPECT_GE(first.basic_basis_points, 10000);
  EXPECT_LE(first.basic_basis_points, 14000);
  EXPECT_EQ(first.basic_basis_points % 500, 0);
  EXPECT_GE(first.total_basis_points, 5000);
  EXPECT_LE(first.total_basis_points, 18000);

  const auto message = wire(trade::kTradeMarketPriceResponse,
                            trade::market_price_message(*catalog, kNow));
  EXPECT_GT(std::stoll(message.at("next_update_tick").get<std::string>()), kNow);
  EXPECT_EQ(message.at("trading_posts").size(), 2u);
}

TEST_F(TradeTest, LoadingRequiresTheTradeUiAtAPostAndChargesForPurchasedGoods) {
  const auto load = Json{{"add_goods", Json::array({{{"good_count",
                                                      {{"trade_good_cms_id", kItemRewardGood},
                                                       {"count", 2}}}}})},
                         {"wagon_cms_id", kWagonId}};
  const auto refused = send(trade::kTradeGoodLoadingRequest, load, here(origin_));
  EXPECT_EQ(own(refused.own, trade::kCommonErrorResponse).front().at("ec"), "Trade_PostTooFar");

  open_trade_ui(origin_);
  const auto loaded = send(trade::kTradeGoodLoadingRequest, load, here(origin_, 2000));
  const auto update = own(loaded.own, trade::kItemsUpdateNotification);
  ASSERT_EQ(update.size(), 1u);
  const auto &goods = update.front().at("trade_loading").at("trade_goods");
  ASSERT_EQ(goods.size(), 1u);
  EXPECT_EQ(goods.at(0).at("trading_post_cms_id"), kOriginPost);
  EXPECT_EQ(goods.at(0).at("production_epochtimes").size(), 2u);
  EXPECT_EQ(update.front().at("money_changes").at(0).at("change"), "-160");
  EXPECT_EQ(stored().gold, 9840);
  EXPECT_EQ(stored().trade_run.phase, TradeRunPhase::loading);

  const auto too_many = Json{{"add_goods", Json::array({{{"good_count",
                                                          {{"trade_good_cms_id", kItemRewardGood},
                                                           {"count", 6}}}}})},
                             {"wagon_cms_id", kWagonId}};
  EXPECT_EQ(own(send(trade::kTradeGoodLoadingRequest, too_many, here(origin_, 2000)).own,
                trade::kCommonErrorResponse)
                .front()
                .at("ec"),
            "Trade_WagonLoadOverLimit");
}

TEST_F(TradeTest, FullRunDepartsParksStowsAndSellsForTheRouteRewards) {
  open_trade_ui(origin_);
  const auto start = Json{
      {"add_goods",
       Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood}, {"count", 2}}}},
                    {{"good_count", {{"trade_good_cms_id", kGoldRewardGood}, {"count", 1}}}}})},
      {"wagon_cms_id", kWagonId},
      {"start", true}};
  const auto departed = send(trade::kTradeGoodLoadingRequest, start, here(origin_, 2000));
  ASSERT_EQ(own(departed.own, trade::kSpawnTradeWagonResponse).size(), 1u);
  ASSERT_EQ(addressed(departed.broadcast, 100, companion::kUnitAddNotification).size(), 1u);
  ASSERT_EQ(addressed(departed.broadcast, 100, companion::kMountUnitNotification).size(), 1u);
  const auto started = own(departed.trailing, trade::kTradeStartNotification);
  ASSERT_EQ(started.size(), 1u);
  EXPECT_EQ(started.front().at("trade_goods").size(), 2u);
  // 2 x 80 production fee, 1 x 1200 basic value and the 100 Gold wagon rental.
  EXPECT_EQ(stored().gold, 10000 - 160 - 1200 - 100);
  EXPECT_EQ(stored().trade_run.phase, TradeRunPhase::running);

  // Parking far from the destination is refused; the driven wagon then reaches its sphere.
  const auto parking = Json{{"trading_post_cms_id", kDestinationPost}};
  EXPECT_EQ(own(send(trade::kTradingPostWagonParkingRequest, parking, here(origin_)).own,
                trade::kTradingPostWagonParkingResponse)
                .front()
                .at("ec"),
            "Trade_PostTooFar");
  companions_->move(here(destination_, 500), nullptr);
  const auto parked = send(trade::kTradingPostWagonParkingRequest, parking,
                           here(destination_, 500));
  const auto parking_reply = own(parked.own, trade::kTradingPostWagonParkingResponse);
  ASSERT_EQ(parking_reply.size(), 1u);
  EXPECT_FALSE(parking_reply.front().contains("ec"));
  EXPECT_EQ(stored().trade_run.phase, TradeRunPhase::parked);

  const auto stowed = send(trade::kTradingPostWagonRemoveRequest, Json::object(),
                           here(destination_, 500));
  EXPECT_EQ(addressed(stowed.broadcast, 100, companion::kUnitRemoveNotification).size(), 1u);
  EXPECT_EQ(stored().trade_run.phase, TradeRunPhase::stowed);

  open_trade_ui(destination_);
  const auto run = stored().trade_run;
  Json sold_goods = Json::array();
  for (const int good : {kItemRewardGood, kGoldRewardGood}) {
    Json times = Json::array();
    for (const auto &unit : run.goods) {
      if (unit.good_cms_id == good) {
        times.push_back(std::to_string(unit.production_epoch_ticks));
      }
    }
    sold_goods.push_back({{"good_cms_id", good},
                          {"trading_post_cms_id", kOriginPost},
                          {"production_epochtimes", times}});
  }
  const auto gold_before_sale = stored().gold;
  const auto sold = send(trade::kTradeGoodSellRequest, {{"goods", sold_goods}},
                         here(destination_, 2000));
  const auto sale = own(sold.own, trade::kItemsUpdateNotification);
  ASSERT_EQ(sale.size(), 1u);
  const auto &results = sale.front().at("trade_sell").at("results");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results.at(0).at("default_reward").at("item_cms_id"), kRewardItem);
  EXPECT_EQ(results.at(0).at("default_reward").at("value"), "12");
  EXPECT_EQ(stored().stackables.at(kRewardItem), 12);
  const auto quote = trade::market_quote(*trade_catalog(), kDestinationPost, 0, kNow);
  const auto expected_gold = 1200 * 13000 / 10000 * quote.total_basis_points / 10000;
  EXPECT_EQ(results.at(1).at("default_reward").at("value"), std::to_string(expected_gold));
  EXPECT_EQ(stored().gold, gold_before_sale + expected_gold);
  EXPECT_EQ(own(sold.trailing, trade::kTradeEndNotification).size(), 1u);
  EXPECT_EQ(stored().trade_run.phase, TradeRunPhase::idle);
  // Each sold good counts toward TradePayment objectives of the selling post.
  EXPECT_EQ(trade_->take_quest_events(),
            (QuestEvents{{QuestEventKind::TradeGoodSold, kItemRewardGood, 0, 1, kDestinationPost},
                         {QuestEventKind::TradeGoodSold, kGoldRewardGood, 0, 1, kDestinationPost}}));
  EXPECT_TRUE(trade_->take_quest_events().empty());
}

TEST_F(TradeTest, SellingAtTheOriginPaysOnlyTheRefundRate) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kGoldRewardGood},
                                                     {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId}},
       here(origin_, 2000));
  const auto unit = stored().trade_run.goods.front();

  const auto sold = send(trade::kTradeGoodSellRequest,
                         {{"goods", Json::array({{{"good_cms_id", kGoldRewardGood},
                                                  {"trading_post_cms_id", kOriginPost},
                                                  {"production_epochtimes",
                                                   Json::array({std::to_string(
                                                       unit.production_epoch_ticks)})}}})}},
                         here(origin_, 2000));
  const auto result = own(sold.own, trade::kItemsUpdateNotification)
                          .front()
                          .at("trade_sell")
                          .at("results")
                          .at(0);
  EXPECT_EQ(result.at("price_ratio_basis_points"), 1000);
  EXPECT_EQ(result.at("default_reward").at("value"), "120");
}

TEST_F(TradeTest, CancelEndsTheRunWithoutRefundAndRemovesTheWagon) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                     {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  const auto gold_after_departure = stored().gold;

  const auto cancelled = send(trade::kTradeCancelRequest, Json::object(), here(origin_));
  EXPECT_EQ(own(cancelled.trailing, trade::kTradeEndNotification).size(), 1u);
  const auto dismounts = addressed(cancelled.broadcast, 100, companion::kMountUnitNotification);
  ASSERT_EQ(dismounts.size(), 1u);
  EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_ClearTrade");
  EXPECT_EQ(stored().gold, gold_after_departure);
  EXPECT_EQ(stored().trade_run.phase, TradeRunPhase::idle);
  EXPECT_FALSE(world_->vehicle_of(character_.id));
  EXPECT_EQ(own(send(trade::kTradeCancelRequest, Json::object(), here(origin_)).own,
                trade::kCommonErrorResponse)
                .front()
                .at("ec"),
            "Trade_NotInProgress");
}

TEST_F(TradeTest, EscapeHonorsTheRespawnCooldownAndReturnsTheWagonToItsOwner) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                     {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  const auto escape = Json{{"is_trading", true}};
  ASSERT_TRUE(trade_->handles(trade::kEscapeRequest, request(trade::kEscapeRequest, escape)));
  EXPECT_FALSE(trade_->handles(trade::kEscapeRequest,
                               request(trade::kEscapeRequest, {{"is_trading", false}})));

  const auto early = send(trade::kEscapeRequest, escape, here(origin_), kNow + 10'000'000);
  EXPECT_EQ(own(early.own, trade::kSystemMessageNotification).front().at("key"),
            "ESystemMessage_SM_TRADEWAGON_RESPAWN_COOLDOWN");

  const auto later = kNow + 31LL * 10'000'000;
  const auto escaped = send(trade::kEscapeRequest, escape, here(origin_, 30000), later);
  ASSERT_EQ(own(escaped.own, trade::kSpawnTradeWagonResponse).size(), 1u);
  EXPECT_FALSE(own(escaped.own, trade::kSpawnTradeWagonResponse).front().contains("ec"));
  ASSERT_TRUE(world_->vehicle_of(character_.id));
  EXPECT_EQ(world_->vehicle_of(character_.id)->position.x_mm, origin_.x_mm + 30000);
  EXPECT_TRUE(world_->vehicle_of(character_.id)->owner_riding);
  EXPECT_EQ(stored().trade_run.last_wagon_spawn_epoch_ticks, later);
}

TEST_F(TradeTest, ReconnectionRestoresTheWagonAtItsSavedPlaceAndTheGoods) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                     {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  companions_->move(here(origin_, 40000), nullptr);

  const auto detached = companions_->leave();
  trade_->wagons_left(detached.vehicles, kNow);
  ASSERT_TRUE(stored().trade_run.wagon_position);
  EXPECT_EQ(stored().trade_run.wagon_position->x_mm, origin_.x_mm + 40000);

  companions_ = make_companions(character_.id, 100);
  companions_->enter_world(participant(100, character_.id, here(origin_, 40000)));
  const auto restored = trade_->enter_world(here(origin_, 40000), kNow);
  const auto adds = addressed(restored.broadcast, 100, companion::kUnitAddNotification);
  ASSERT_EQ(adds.size(), 1u);
  EXPECT_EQ(adds.front().at("nub").at("pos").at("millimeter_x"), origin_.x_mm + 40000);

  // The client blacks out the screen on TradeStartNoti until its character rides a vehicle,
  // so an owner restored on foot only gets the wagon marker.
  ASSERT_EQ(restored.trailing.size(), 1u);
  EXPECT_EQ(restored.trailing.at(0).opcode, trade::kTradeWagonPosNotification);
  EXPECT_TRUE(trade_->wagon_boarded().trailing.empty());

  const auto wagon = world_->vehicle_of(character_.id);
  ASSERT_TRUE(wagon);
  const auto boarded = companions_->receive(
      companion::kRideUnitRequest, {{"mount_unit_id", std::to_string(wagon->unit_id)}, {"slot", 0}},
      1, kNow);
  EXPECT_FALSE(addressed(boarded.broadcast, 100, companion::kMountUnitNotification).empty());
  ASSERT_TRUE(world_->vehicle_of(character_.id)->owner_riding);

  const auto announced = trade_->wagon_boarded();
  ASSERT_EQ(announced.trailing.size(), 1u);
  EXPECT_EQ(announced.trailing.at(0).opcode, trade::kTradeStartNotification);
  EXPECT_TRUE(trade_->wagon_boarded().trailing.empty());
}

TEST_F(TradeTest, EscapeAfterAnOnFootReconnectionAnnouncesTheRunWithTheOwnerSeated) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                     {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  const auto detached = companions_->leave();
  trade_->wagons_left(detached.vehicles, kNow);

  companions_ = make_companions(character_.id, 100);
  companions_->enter_world(participant(100, character_.id, here(origin_, 30000)));
  const auto restored = trade_->enter_world(here(origin_, 30000), kNow);
  ASSERT_EQ(restored.trailing.size(), 1u);
  EXPECT_EQ(restored.trailing.at(0).opcode, trade::kTradeWagonPosNotification);

  const auto later = kNow + 31LL * 10'000'000;
  const auto escaped =
      send(trade::kEscapeRequest, {{"is_trading", true}}, here(origin_, 30000), later);
  ASSERT_EQ(own(escaped.own, trade::kSpawnTradeWagonResponse).size(), 1u);
  ASSERT_TRUE(world_->vehicle_of(character_.id)->owner_riding);
  ASSERT_EQ(escaped.trailing.size(), 1u);
  EXPECT_EQ(escaped.trailing.at(0).opcode, trade::kTradeStartNotification);
}

TEST_F(TradeTest, RegionReentryKeepsTheExistingWagonAndTradeGoods) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                       {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  const auto before = stored().trade_run;
  const auto original = world_->vehicle_of(character_.id);
  ASSERT_TRUE(original);
  ASSERT_TRUE(original->owner_riding);
  ASSERT_EQ(before.goods.size(), 1U);

  auto destination = here(origin_, 40000);
  destination.map_id += 1;
  companions_->reenter_region(destination, kNow);
  const auto result = trade_->enter_world(destination, kNow);
  const auto wagon = world_->vehicle_of(character_.id);
  ASSERT_TRUE(wagon);
  EXPECT_EQ(wagon->unit_id, original->unit_id);
  EXPECT_TRUE(wagon->owner_riding);
  EXPECT_EQ(wagon->position.map_id, destination.map_id);
  EXPECT_EQ(stored().trade_run, before);
  EXPECT_EQ(world_->unit_count(), 1U);
  ASSERT_EQ(result.trailing.size(), 2U);
  EXPECT_EQ(result.trailing.at(0).opcode, trade::kTradeStartNotification);
  EXPECT_EQ(result.trailing.at(1).opcode, trade::kTradeWagonPosNotification);
}

TEST_F(TradeTest, RegionalWagonCheckpointSurvivesAnUnexpectedDisconnect) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                       {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  const auto goods = stored().trade_run.goods;
  auto destination = here(origin_, 40000);
  destination.map_id += 1;
  ASSERT_EQ(trade_->checkpoint_region(destination, kNow), trade::RegionCheckpointResult::committed);
  ASSERT_TRUE(stored().trade_run.wagon_position);
  EXPECT_EQ(*stored().trade_run.wagon_position, destination);
  EXPECT_EQ(stored().trade_run.goods, goods);
  const auto character_after = store_.characters->characters(kAccountId, kWorldId).front();
  ASSERT_TRUE(character_after.world_position);
  EXPECT_EQ(*character_after.world_position, destination);

  // Simulate losing the in-memory session without its normal wagon-exit checkpoint.
  companions_->leave();
  companions_ = make_companions(character_.id, 100);
  companions_->enter_world(participant(100, character_.id, destination));
  const auto restored = trade_->enter_world(destination, kNow);
  const auto wagon = world_->vehicle_of(character_.id);
  ASSERT_TRUE(wagon);
  EXPECT_EQ(wagon->position, destination);
  EXPECT_EQ(stored().trade_run.goods, goods);
  ASSERT_EQ(restored.trailing.size(), 1u);
  EXPECT_EQ(restored.trailing.front().opcode, trade::kTradeWagonPosNotification);
}

TEST_F(TradeTest, LegacyMismatchedWagonMapRemainsAccessible) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                       {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  const auto goods = stored().trade_run.goods;
  const auto origin = here(origin_, 2000);
  auto destination = here(origin_, 40000);
  destination.map_id += 1;
  ASSERT_EQ(trade_->checkpoint_region(destination, kNow), trade::RegionCheckpointResult::committed);

  // A legacy release could have saved different player and wagon map positions.
  ASSERT_TRUE(store_.characters->save_world_position(kAccountId, character_.id, kWorldId, origin));
  companions_->leave();
  companions_ = make_companions(character_.id, 100);
  companions_->enter_world(participant(100, character_.id, origin));
  trade_->enter_world(origin, kNow);
  const auto wagon = world_->vehicle_of(character_.id);
  ASSERT_TRUE(wagon);
  EXPECT_EQ(wagon->position, destination);
  EXPECT_EQ(stored().trade_run.goods, goods);
  const auto arrived = companions_->reenter_region(destination, kNow);
  EXPECT_EQ(addressed(arrived.broadcast, 100, companion::kUnitAddNotification).size(), 1U);
}

TEST_F(TradeTest, RejectedRegionalCheckpointDoesNotChangeTradeProgress) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                       {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  const auto before = stored();

  class RefusingProgress final : public ProgressAccess {
  public:
    RefusingProgress(CharacterRepository &characters, CharacterStateRepository &state,
                     std::int64_t character_id)
        : source_(characters, state, kAccountId, character_id, kWorldId) {
    }

    WorldProgress load() override {
      return source_.load();
    }

    std::optional<Character> character() override {
      return source_.character();
    }

    std::optional<std::vector<CharacterItem>> commit(const WorldProgress &, const WorldProgress &,
                                                     const ItemTrade &) override {
      return std::nullopt;
    }

    bool commit_with_position(const WorldProgress &, const WorldProgress &,
                              const WorldPosition &) override {
      return false;
    }

  private:
    RepositoryProgressAccess source_;
  };

  trade::TradeSession refusing(
      trade::TradeServices{world_, companion_catalog(), trade_catalog(), item_catalog(),
                           std::make_shared<const std::set<int>>(std::set<int>{1001316})},
      std::make_unique<RefusingProgress>(*store_.characters, *store_.character_state, character_.id),
      100, character_.id);
  companions_->leave();
  companions_ = make_companions(character_.id, 100);
  companions_->enter_world(participant(100, character_.id, here(origin_)));
  refusing.enter_world(here(origin_), kNow);
  const auto refusing_wagon = world_->vehicle_of(character_.id);
  ASSERT_TRUE(refusing_wagon);
  ASSERT_EQ(world_->ride(100, refusing_wagon->unit_id, 0, kNow).error, x5::ec::Success);
  auto destination = here(origin_, 40000);
  destination.map_id += 1;
  EXPECT_EQ(refusing.checkpoint_region(destination, kNow), trade::RegionCheckpointResult::conflict);
  EXPECT_EQ(stored(), before);
}

TEST_F(TradeTest, AmbiguousRegionalCheckpointIsUnknownAndKeepsBothCommittedPositions) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                       {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));

  class LostAcknowledgement final : public ProgressAccess {
  public:
    LostAcknowledgement(CharacterRepository &characters, CharacterStateRepository &state,
                        std::int64_t character_id)
        : source_(characters, state, kAccountId, character_id, kWorldId) {
    }

    WorldProgress load() override {
      return source_.load();
    }

    std::optional<Character> character() override {
      return source_.character();
    }

    std::optional<std::vector<CharacterItem>> commit(const WorldProgress &, const WorldProgress &,
                                                     const ItemTrade &) override {
      throw std::logic_error("Unexpected item commit");
    }

    bool commit_with_position(const WorldProgress &before, const WorldProgress &after,
                              const WorldPosition &position) override {
      if (!source_.commit_with_position(before, after, position)) {
        throw std::logic_error("Unexpected checkpoint conflict");
      }
      throw std::runtime_error("Simulated lost commit acknowledgement");
    }

  private:
    RepositoryProgressAccess source_;
  };

  trade::TradeSession uncertain(
      trade::TradeServices{world_, companion_catalog(), trade_catalog(), item_catalog(),
                           std::make_shared<const std::set<int>>(std::set<int>{1001316})},
      std::make_unique<LostAcknowledgement>(*store_.characters, *store_.character_state,
                                            character_.id),
      100, character_.id);
  companions_->leave();
  companions_ = make_companions(character_.id, 100);
  companions_->enter_world(participant(100, character_.id, here(origin_)));
  uncertain.enter_world(here(origin_), kNow);
  const auto uncertain_wagon = world_->vehicle_of(character_.id);
  ASSERT_TRUE(uncertain_wagon);
  ASSERT_EQ(world_->ride(100, uncertain_wagon->unit_id, 0, kNow).error, x5::ec::Success);
  const auto goods = stored().trade_run.goods;
  auto destination = here(origin_, 40000);
  destination.map_id += 1;
  EXPECT_EQ(uncertain.checkpoint_region(destination, kNow), trade::RegionCheckpointResult::unknown);
  const auto character_after = store_.characters->characters(kAccountId, kWorldId).front();
  ASSERT_TRUE(character_after.world_position);
  ASSERT_TRUE(stored().trade_run.wagon_position);
  EXPECT_EQ(*character_after.world_position, destination);
  EXPECT_EQ(*stored().trade_run.wagon_position, destination);
  EXPECT_EQ(stored().trade_run.goods, goods);
}

TEST_F(TradeTest, DetachedWagonRetriesAndRecognizesAnUnacknowledgedAtomicCommit) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                       {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  auto wagon = world_->vehicle_of(character_.id);
  ASSERT_TRUE(wagon);
  wagon->position = here(destination_, 300);
  wagon->health -= 1;
  const auto player_position = here(destination_, 2000);
  const auto pending = trade_->capture_wagon_settlement({*wagon}, player_position);
  ASSERT_TRUE(pending);
  ASSERT_TRUE(pending->run_identity);
  trade_->enter_world(here(origin_), kNow);
  const auto restored_pending = trade_->capture_wagon_settlement({*wagon}, player_position);
  ASSERT_TRUE(restored_pending);
  EXPECT_EQ(restored_pending->run_identity, pending->run_identity);
  const auto before = stored();

  SettlementFaultProgress conflict(*store_.characters, *store_.character_state, character_.id,
                                   SettlementFaultProgress::Mode::conflict);
  EXPECT_EQ(trade::reconcile_wagon_settlement(conflict, *pending),
            trade::WagonSettlementResult::retry);
  EXPECT_EQ(stored(), before);

  SettlementFaultProgress lost_acknowledgement(
      *store_.characters, *store_.character_state, character_.id,
      SettlementFaultProgress::Mode::lost_acknowledgement);
  EXPECT_EQ(trade::reconcile_wagon_settlement(lost_acknowledgement, *pending),
            trade::WagonSettlementResult::retry);
  const auto committed = stored();
  EXPECT_EQ(committed.trade_run.wagon_position, wagon->position);
  EXPECT_EQ(committed.trade_run.wagon_health, wagon->health);
  EXPECT_EQ(committed.trade_run.goods, before.trade_run.goods);
  EXPECT_EQ(store_.characters->characters(kAccountId, kWorldId).front().world_position,
            player_position);

  RepositoryProgressAccess recovery(*store_.characters, *store_.character_state, kAccountId,
                                    character_.id, kWorldId);
  EXPECT_EQ(trade::reconcile_wagon_settlement(recovery, *pending),
            trade::WagonSettlementResult::already_applied);
  EXPECT_EQ(stored().revision, committed.revision);

  change_progress([](WorldProgress &progress) {
    ++progress.trade_run.started_epoch_ticks;
  });
  const auto replacement = stored();
  EXPECT_EQ(trade::reconcile_wagon_settlement(recovery, *pending),
            trade::WagonSettlementResult::superseded);
  EXPECT_EQ(stored(), replacement);
}

TEST_F(TradeTest, UnrelatedVehicleDoesNotCreatePendingWagonSettlement) {
  companion::VehicleSnapshot wagon;
  wagon.vehicle_cms_id = kWagonId;
  wagon.position = here(origin_);
  wagon.health = 100;
  EXPECT_FALSE(trade_->capture_wagon_settlement({wagon}, std::nullopt));
  const trade::PendingWagonSettlement unidentified{wagon, std::nullopt, std::nullopt};
  const auto before = stored();
  RepositoryProgressAccess recovery(*store_.characters, *store_.character_state, kAccountId,
                                    character_.id, kWorldId);
  EXPECT_EQ(trade::reconcile_wagon_settlement(recovery, unidentified),
            trade::WagonSettlementResult::retry);
  EXPECT_EQ(stored(), before);
}

TEST_F(TradeTest, AForeignSessionDoesNotAdoptAnExistingSameModelVehicle) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                       {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  const auto wagon = world_->vehicle_of(character_.id);
  ASSERT_TRUE(wagon);
  trade::TradeSession other(
      trade::TradeServices{world_, companion_catalog(), trade_catalog(), item_catalog(),
                           std::make_shared<const std::set<int>>(std::set<int>{1001316})},
      std::make_unique<RepositoryProgressAccess>(*store_.characters, *store_.character_state,
                                                 kAccountId, character_.id, kWorldId),
      100, character_.id);
  other.enter_world(here(origin_), kNow);
  EXPECT_FALSE(other.capture_wagon_settlement({*wagon}, std::nullopt));
  EXPECT_EQ(other.checkpoint_region(here(destination_), kNow),
            trade::RegionCheckpointResult::no_driven_trade_wagon);
  EXPECT_TRUE(trade_->capture_wagon_settlement({*wagon}, std::nullopt));
}

TEST_F(TradeTest, EscapeTagsRespawnedWagonEvenWhenItsProgressCommitFails) {
  open_trade_ui(origin_);
  send(trade::kTradeGoodLoadingRequest,
       {{"add_goods", Json::array({{{"good_count", {{"trade_good_cms_id", kItemRewardGood},
                                                       {"count", 1}}}}})},
        {"wagon_cms_id", kWagonId},
        {"start", true}},
       here(origin_, 2000));
  companions_->leave();
  companions_ = make_companions(character_.id, 100);
  companions_->enter_world(participant(100, character_.id, here(origin_)));
  trade::TradeSession failing(
      trade::TradeServices{world_, companion_catalog(), trade_catalog(), item_catalog(),
                           std::make_shared<const std::set<int>>(std::set<int>{1001316})},
      std::make_unique<SettlementFaultProgress>(
          *store_.characters, *store_.character_state, character_.id,
          SettlementFaultProgress::Mode::trade_commit_conflict),
      100, character_.id);
  failing.enter_world(here(origin_), kNow);
  const auto before = world_->vehicle_of(character_.id);
  ASSERT_TRUE(before);
  const auto result = failing.receive(trade::kEscapeRequest, {{"is_trading", true}}, 1,
                                      here(origin_), kNow + 100000000000LL);
  ASSERT_EQ(own(result.own, trade::kSpawnTradeWagonResponse).size(), 1U);
  const auto respawned = world_->vehicle_of(character_.id);
  ASSERT_TRUE(respawned);
  EXPECT_NE(respawned->unit_id, before->unit_id);
  const auto pending = failing.capture_wagon_settlement({*respawned}, std::nullopt);
  ASSERT_TRUE(pending);
  ASSERT_TRUE(pending->run_identity);
  EXPECT_EQ(pending->run_identity->started_epoch_ticks, stored().trade_run.started_epoch_ticks);
}

TEST_F(TradeTest, GmVoucherGrantCanBeLoadedAndADestroyedWagonEndsTheRun) {
  const auto voucher_command = companion::parse_companion_gm_request(
      20009, request(20009, {{"text", "trade_voucher_add 1102"}}));
  ASSERT_TRUE(voucher_command);
  const auto granted = trade_->gm_command(*voucher_command, kNow);
  const auto voucher_update = own(granted.own, trade::kItemsUpdateNotification);
  ASSERT_EQ(voucher_update.size(), 1u);
  const auto voucher_id =
      voucher_update.front().at("trade_voucher").at("vouchers").at(0).at("voucher_id");
  EXPECT_EQ(stored().trade_vouchers.size(), 1u);

  open_trade_ui(origin_);
  const auto loaded = send(trade::kTradeGoodLoadingRequest,
                           {{"add_goods", Json::array({{{"voucher_id", voucher_id}}})},
                            {"wagon_cms_id", kWagonId},
                            {"start", true}},
                           here(origin_, 2000));
  const auto loading = own(loaded.own, trade::kItemsUpdateNotification).front();
  EXPECT_EQ(loading.at("trade_loading").at("vouchers").at(0),
            Json({{"voucher_id", voucher_id}}));
  EXPECT_TRUE(stored().trade_vouchers.begin()->second.redeemed);

  const auto damage = companion::parse_companion_gm_request(
      20009, request(20009, {{"text", "damage_vehicle 99999"}}));
  ASSERT_TRUE(damage);
  const auto destroyed = trade_->gm_command(*damage, kNow);
  EXPECT_EQ(own(destroyed.trailing, trade::kTradeEndNotification).size(), 1u);
  EXPECT_EQ(stored().trade_run.phase, TradeRunPhase::idle);
}

} // namespace
} // namespace chronicles
