#include "world/features/shop/shop_session.hpp"

#include "world/core/tests/economy_test_support.hpp"

#include <gtest/gtest.h>

namespace chronicles::shop {
namespace {

using namespace economy_testing;

class ProbeContext final : public ShopSessionContext {
public:
  explicit ProbeContext(EconomyFixture &fixture) : owner_id(fixture.actor.id), fixture_(fixture) {
  }

  std::optional<std::int64_t> shop_character_id() const override {
    return owner_id;
  }

  bool shop_has_tutorial() const override {
    return tutorial;
  }

  std::int64_t shop_monotonic_ticks() const override {
    return monotonic;
  }

  std::int64_t shop_epoch_ticks() const override {
    return kNow;
  }

  std::optional<MerchantTarget> shop_target(std::uint64_t unit_id) const override {
    if (!known_target || unit_id != kMerchantUnit) {
      return std::nullopt;
    }
    return MerchantTarget{kMerchantUnit, kMerchantNpc, {8000, 0, 0, 0, 0.0F}};
  }

  std::optional<WorldPosition> shop_position() const override {
    return WorldPosition{8000, 0, 0, 0, 0.0F};
  }

  const trade::TradeCatalog *shop_trade_catalog() const override {
    return nullptr;
  }

  bool shop_has_weight_measurement() const override {
    return measure_weight;
  }

  MerchantWeightMeasurement shop_measure_weight(const Character &, const WorldProgress &) override {
    fixture_.trace.push_back("weight");
    if (throw_weight) {
      throw std::runtime_error("Synthetic inventory measurement failure");
    }
    return weight;
  }

  const Json *shop_player_stats() const override {
    return &fixture_.prepared_stats;
  }

  ConditionContext shop_condition_context() const override {
    ++condition_reads;
    return conditions;
  }

  std::optional<std::int64_t> owner_id;
  bool tutorial{true};
  bool known_target{true};
  bool measure_weight{};
  bool throw_weight{};
  MerchantWeightMeasurement weight{0, 1};
  std::int64_t monotonic = 20'000'000;
  ConditionContext conditions;
  mutable int condition_reads{};

private:
  EconomyFixture &fixture_;
};

WorldGameplayRequest request(const Json &shop = Json::object(), std::uint32_t sequence = 1) {
  return {kInteractUnitRequestOpcode,
          sequence,
          {{"target_unit_id", std::to_string(kMerchantUnit)},
           {"param", {{"interaction_id", kInteraction}, {"shop", shop}}}}};
}

Json buy_equipment() {
  return {{"buy_list", Json::array({{{"item_cms_id", kEquipment}, {"count", 1}}})}};
}

TEST(ShopSession, UnrelatedRequestsKeepTheirPositionsAndDoNoShopWork) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ShopFeature feature(fixture.items);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  FrameContext frame(20003, WorldGameplayRequest{20003, 1, Json::object()});
  frame.party_position = WorldPosition{8000, 1, 2, 3, 0.0F};
  frame.gameplay_position = frame.party_position;

  EXPECT_FALSE(session->handle(frame));
  EXPECT_TRUE(frame.party_position);
  EXPECT_TRUE(frame.gameplay_position);
  EXPECT_TRUE(frame.session_packets.empty());
  EXPECT_TRUE(fixture.trace.empty());
}

TEST(ShopSession, MissingCatalogRefusesBeforeProgressOrStorage) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ShopFeature feature(static_cast<ClientCatalog *>(nullptr));
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);

  const auto packets = session->receive(request(buy_equipment()));
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets.front().message.at("ec"), "Interaction_Invalid");
  EXPECT_TRUE(fixture.trace.empty());
}

TEST(ShopSession, UnauthorizedTrafficDoesNotSpendTheEightRequestBudget) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ShopFeature feature(fixture.items);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  context.known_target = false;
  for (unsigned index = 0; index < 20; ++index) {
    const auto refused = session->receive(request());
    EXPECT_EQ(refused.front().message.at("ec"), "Interaction_TargetNotFound");
  }
  EXPECT_TRUE(fixture.trace.empty());
  context.known_target = true;
  for (unsigned index = 0; index < 8; ++index) {
    const auto opened = session->receive(request());
    ASSERT_EQ(opened.size(), 2);
    EXPECT_EQ(opened.front().opcode, kInteractUnitResponseOpcode);
  }
  EXPECT_EQ(session->receive(request()).front().message.at("ec"), "BadRequest");
  context.monotonic += 10'000'000;
  EXPECT_EQ(session->receive(request()).front().opcode, kInteractUnitResponseOpcode);
}

TEST(ShopSession, CommittedEquipmentAdoptsThenRefreshesStatsAndItemsBeforePackets) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ShopFeature feature(fixture.items);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  fixture.on_snapshot = [&](const PlayerInventorySnapshotChanged &event) {
    EXPECT_EQ(event.prepared_stats->at("refreshed"), true);
    ASSERT_EQ(event.character.items.size(), fixture.actor.items.size() + 1);
    const auto granted = std::ranges::find_if(event.character.items, [](const CharacterItem &item) {
      return item.cms_id == kEquipment;
    });
    ASSERT_NE(granted, event.character.items.end());
    EXPECT_GT(granted->id, 0);
    auto retained = event.character.items;
    std::erase_if(retained, [](const CharacterItem &item) {
      return item.cms_id == kEquipment;
    });
    EXPECT_EQ(retained, fixture.actor.items);
    EXPECT_EQ(fixture.stored_progress().gold, 4900);
  };

  const auto packets = session->receive(request(buy_equipment()));
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets[0].opcode, kItemsUpdateNotificationOpcode);
  EXPECT_EQ(packets[1].opcode, kInteractUnitResponseOpcode);
  EXPECT_LT(fixture.trace_index("commit"), fixture.trace_index("adopt"));
  EXPECT_LT(fixture.trace_index("adopt"), fixture.trace_index("stats"));
  EXPECT_LT(fixture.trace_index("stats"), fixture.trace_index("snapshot"));
  EXPECT_EQ(fixture.progress->cached.revision, fixture.stored_progress().revision);
}

TEST(ShopSession, RevisionConflictPublishesNoSuccessOrCacheMutation) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ShopFeature feature(fixture.items);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  const auto before = fixture.stored_progress();
  fixture.progress->reject_commit = true;

  const auto packets = session->receive(request(buy_equipment()));
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets.front().message.at("ec"), "ItemError");
  EXPECT_EQ(fixture.stored_progress(), before);
  EXPECT_FALSE(fixture.last_snapshot);
  EXPECT_EQ(std::ranges::count(fixture.trace, "adopt"), 0);
}

TEST(ShopSession, CommitExceptionKeepsTheExistingRefusalBoundary) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ShopFeature feature(fixture.items);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  const auto before = fixture.stored_progress();
  fixture.progress->throw_commit = true;

  const auto packets = session->receive(request(buy_equipment()));
  EXPECT_EQ(packets.front().message.at("ec"), "ItemError");
  EXPECT_EQ(fixture.stored_progress(), before);
  EXPECT_FALSE(fixture.last_snapshot);
}

TEST(ShopSession, WeightCeilingRefusesBeforeCommitButMeasurementFailureRetainsTheFallback) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  auto tables = item_tables();
  tables.game_variables.push_back({{"name", "BagMaxWeightMultiplier"}, {"value", "200"}});
  ShopFeature feature(std::make_shared<const ShopCatalog>(tables));
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  context.measure_weight = true;

  const auto refused = session->receive(request(buy_equipment()));
  EXPECT_EQ(refused.front().message.at("ec"), "ItemWeightLimit");
  EXPECT_EQ(std::ranges::count(fixture.trace, "commit"), 0);
  context.throw_weight = true;
  const auto committed = session->receive(request(buy_equipment()));
  EXPECT_EQ(committed.front().opcode, kItemsUpdateNotificationOpcode);
  EXPECT_EQ(fixture.stored_progress().gold, 4900);
}

TEST(ShopSession, SellingTheGrantedInstanceRefreshesTheSnapshotAfterRemoval) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ShopFeature feature(fixture.items);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  session->receive(request(buy_equipment()));
  ASSERT_TRUE(fixture.last_snapshot);
  ASSERT_EQ(fixture.last_snapshot->items.size(), fixture.actor.items.size() + 1);
  const auto granted =
      std::ranges::find_if(fixture.last_snapshot->items, [](const CharacterItem &item) {
        return item.cms_id == kEquipment;
      });
  ASSERT_NE(granted, fixture.last_snapshot->items.end());
  const auto item_id = granted->id;

  const auto sold = session->receive(
      request({{"sell_list", Json::array({{{"item_id", std::to_string(item_id)}}})}}, 2));
  ASSERT_EQ(sold.front().opcode, kItemsUpdateNotificationOpcode);
  ASSERT_TRUE(fixture.last_snapshot);
  EXPECT_EQ(fixture.last_snapshot->items, fixture.actor.items);
  EXPECT_EQ(fixture.repositories.characters->character_by_id(fixture.actor.id)->items,
            fixture.actor.items);
}

TEST(ShopSession, CompanyGatedGoodsUseTheCharactersCompanyStanding) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  // Shop condition 13 (housing blueprints): Blue Salt position level 3, permit ignored.
  auto tables = item_tables();
  tables.shops[0]["fk_shopItemCondition"] = "13";
  tables.shop_item_conditions = {
      {{"conditionId", "13"},
       {"group", "1"},
       {"logicalOperator", "AND"},
       {"negated", ""},
       {"typeCase", "Company"},
       {"type", "\"companyId\":\"EX5CompanyType_BlueSalt\",\"ignorePermit\":true,\"level\":3"}}};
  ShopFeature feature(std::make_shared<const ShopCatalog>(tables));
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  const Json buy_material{
      {"buy_list", Json::array({{{"item_cms_id", kMaterial}, {"count", 1}}})}};

  const auto owned_before = fixture.stored_progress().stackables.at(kMaterial);
  context.conditions.company_levels["EX5CompanyType_BlueSalt"] = 2;
  const auto refused = session->receive(request(buy_material, 1));
  ASSERT_EQ(refused.size(), 1);
  EXPECT_EQ(refused.front().opcode, 2);
  EXPECT_EQ(fixture.stored_progress().stackables.at(kMaterial), owned_before);

  context.conditions.company_levels["EX5CompanyType_BlueSalt"] = 3;
  const auto bought = session->receive(request(buy_material, 2));
  ASSERT_FALSE(bought.empty());
  EXPECT_EQ(bought.front().opcode, kItemsUpdateNotificationOpcode);
  EXPECT_EQ(fixture.stored_progress().stackables.at(kMaterial), owned_before + 1);
  EXPECT_EQ(context.condition_reads, 2);
}

} // namespace
} // namespace chronicles::shop
