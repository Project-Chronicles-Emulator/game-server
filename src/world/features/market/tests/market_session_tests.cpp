#include "world/features/market/market_session.hpp"

#include "world/core/tests/economy_test_support.hpp"
#include "world/features/market/market_protocol.hpp"

#include <gtest/gtest.h>

namespace chronicles::market {
namespace {

using namespace economy_testing;

class ProbeContext final : public MarketSessionContext {
public:
  explicit ProbeContext(EconomyFixture &fixture) : fixture_(fixture) {
  }

  bool market_ready() const override {
    return ready;
  }

  bool market_has_tutorial() const override {
    return tutorial;
  }

  std::int64_t market_epoch_ticks() const override {
    return kNow;
  }

  const Json *market_player_stats() const override {
    return &fixture_.prepared_stats;
  }

  void market_append_encumbrance(std::vector<GameplayPacket> &packets) override {
    fixture_.trace.push_back("encumbrance");
    packets.push_back({115, Json::object()});
  }

  bool ready{true};
  bool tutorial{true};

private:
  EconomyFixture &fixture_;
};

class RecordingMarket final : public MarketRepository {
public:
  explicit RecordingMarket(EconomyFixture &fixture)
      : delegate_(fixture.repositories.database), fixture_(fixture) {
  }

  std::optional<MarketListing> market_listing(int world_id, std::int64_t listing_id) override {
    return delegate_.market_listing(world_id, listing_id);
  }

  std::optional<MarketListing> market_registration(int world_id, std::int64_t seller_id,
                                                   const std::string &key) override {
    registration_keys.push_back(key);
    return delegate_.market_registration(world_id, seller_id, key);
  }

  MarketPage market_query(int world_id, std::int64_t character_id, const MarketQuery &query,
                          std::int64_t now) override {
    return delegate_.market_query(world_id, character_id, query, now);
  }

  MarketCommit market_commit(const Character &actor, const WorldProgress &before,
                             const WorldProgress &after, const ItemTrade &trade,
                             const MarketListing &expected, const MarketListing &next) override {
    fixture_.trace.push_back("market-commit");
    auto committed = delegate_.market_commit(actor, before, after, trade, expected, next);
    if (throw_after_commit) {
      throw std::runtime_error("Synthetic unknown market COMMIT outcome");
    }
    return committed;
  }

  bool throw_after_commit{};
  std::vector<std::string> registration_keys;

private:
  MemoryMarketRepository delegate_;
  EconomyFixture &fixture_;
};

std::shared_ptr<const MarketCatalog> definitions() {
  return std::make_shared<const MarketCatalog>(
      std::vector<Json>{{{"id", "701"}}, {{"id", "702"}}},
      std::vector<Json>{{{"FK_ItemId", "701"}, {"FK_CategoryGroupId", "301"}},
                        {{"FK_ItemId", "702"}, {"FK_CategoryGroupId", "101"}}},
      std::vector<Json>{{{"Id", "301"}, {"MainType", "Material"}, {"SubType", "RareCraftMaterial"}},
                        {{"Id", "101"}, {"MainType", "Equipment"}, {"SubType", "Weapon"}}});
}

WorldGameplayRequest registration(std::uint32_t sequence = 1) {
  return {kMarketRegisterRequest,
          sequence,
          {{"identity", {{"stackable", {{"item_cms_id", kMaterial}, {"count", 3}}}}},
           {"price", "25"},
           {"period", 2}}};
}

TEST(MarketSession, DisabledServiceRefusesBeforeActorOrProgressQueries) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MarketFeature feature(static_cast<ClientCatalog *>(nullptr), fixture.items);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);

  const auto packets = session->receive(registration());
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets.front().message.at("ec"), x5::ec::Market_Disabled);
  EXPECT_TRUE(fixture.trace.empty());
}

TEST(MarketSession, MissingActorStopsBeforeProgressAndStorage) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MarketFeature feature(definitions(), fixture.items);
  feature.set_repository(std::make_shared<RecordingMarket>(fixture));
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  fixture.progress->missing_character = true;

  const auto packets = session->receive(registration());
  EXPECT_EQ(packets.front().message.at("ec"), x5::ec::Market_InvalidItemMarketSession);
  EXPECT_EQ(fixture.trace, (std::vector<std::string>{"character"}));
}

TEST(MarketSession, CommitPrecedesTutorialAdoptionSnapshotAndEncumbrancePackets) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MarketFeature feature(definitions(), fixture.items);
  feature.set_repository(std::make_shared<RecordingMarket>(fixture));
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);

  const auto packets = session->receive(registration());
  ASSERT_EQ(packets.size(), 3);
  EXPECT_EQ(packets[0].opcode, kMarketItemsUpdate);
  EXPECT_EQ(packets[1].opcode, kMarketRegisterResponse);
  EXPECT_EQ(packets[2].opcode, 115);
  EXPECT_EQ(fixture.stored_progress().stackables.at(kMaterial), 17);
  EXPECT_LT(fixture.trace_index("market-commit"), fixture.trace_index("adopt"));
  EXPECT_LT(fixture.trace_index("adopt"), fixture.trace_index("snapshot"));
  EXPECT_LT(fixture.trace_index("snapshot"), fixture.trace_index("encumbrance"));
  EXPECT_EQ(std::ranges::count(fixture.trace, "stats"), 0);
}

TEST(MarketSession, OneConnectionScopeReplaysAndANewScopeDoesNotAliasIt) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MarketFeature feature(definitions(), fixture.items);
  auto repository = std::make_shared<RecordingMarket>(fixture);
  feature.set_repository(repository);
  auto first = feature.create_session(*fixture.progress, context, *fixture.events);
  first->receive(registration(7));
  first->receive(registration(7));
  ASSERT_GE(repository->registration_keys.size(), 2);
  EXPECT_EQ(repository->registration_keys[0], repository->registration_keys[1]);
  EXPECT_EQ(fixture.stored_progress().stackables.at(kMaterial), 17);

  auto reconnected = feature.create_session(*fixture.progress, context, *fixture.events);
  reconnected->receive(registration(7));
  EXPECT_NE(repository->registration_keys.front(), repository->registration_keys.back());
  EXPECT_EQ(fixture.stored_progress().stackables.at(kMaterial), 14);
}

TEST(MarketSession, AReadOnlyQueryDoesNotAdoptOrRefreshInventory) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MarketFeature feature(definitions(), fixture.items);
  feature.set_repository(std::make_shared<RecordingMarket>(fixture));
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);

  const auto packets = session->receive({kMarketQueryRequest, 1, Json::object()});
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets.front().opcode, kMarketQueryResponse);
  EXPECT_FALSE(fixture.last_snapshot);
  EXPECT_EQ(std::ranges::count(fixture.trace, "adopt"), 0);
  EXPECT_EQ(std::ranges::count(fixture.trace, "encumbrance"), 0);
}

TEST(MarketSession, AnUnknownCommitOutcomeStillEscapesToTheApplicationFailurePath) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MarketFeature feature(definitions(), fixture.items);
  auto repository = std::make_shared<RecordingMarket>(fixture);
  feature.set_repository(repository);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  repository->throw_after_commit = true;

  EXPECT_THROW(session->receive(registration()), std::runtime_error);
  EXPECT_EQ(fixture.stored_progress().stackables.at(kMaterial), 17);
  EXPECT_FALSE(fixture.last_snapshot);
  EXPECT_EQ(std::ranges::count(fixture.trace, "adopt"), 0);
}

TEST(MarketSession, UnrelatedFramesDoNotConsumeOrInvalidateTheirPositions) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MarketFeature feature(definitions(), fixture.items);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  FrameContext frame(20003, WorldGameplayRequest{20003, 4, Json::object()});
  frame.party_position = WorldPosition{8000, 0, 0, 0, 0.0F};
  frame.gameplay_position = frame.party_position;

  EXPECT_FALSE(session->handle(frame));
  EXPECT_TRUE(frame.party_position);
  EXPECT_TRUE(frame.gameplay_position);
  EXPECT_TRUE(fixture.trace.empty());
}

} // namespace
} // namespace chronicles::market
