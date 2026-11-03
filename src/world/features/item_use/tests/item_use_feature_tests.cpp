#include "world/features/item_use/item_use_feature.hpp"

#include "world/core/tests/economy_test_support.hpp"

#include <gtest/gtest.h>

namespace chronicles::item_use {
namespace {

using namespace economy_testing;

class ProbeContext final : public ItemUseSessionContext {
public:
  explicit ProbeContext(EconomyFixture &fixture) : fixture_(fixture) {
  }

  const WorldProgress *item_use_tutorial_progress() const override {
    return tutorial ? &fixture_.progress->cached : nullptr;
  }

  const Json *item_use_player_stats() const override {
    return &fixture_.prepared_stats;
  }

  bool item_use_has_combat() const override {
    return combat_at_creation;
  }

  const std::vector<int> *item_use_effect_list(int item_cms_id) const override {
    return item_cms_id == kMaterial ? &current_effects : nullptr;
  }

  bool item_use_effect_ready() const override {
    return ready;
  }

  std::vector<GameplayPacket> item_use_apply_effects(const std::vector<int> &effect_ids) override {
    EXPECT_EQ(fixture_.stored_progress().stackables.at(kMaterial), 19);
    EXPECT_EQ(fixture_.progress->cached.stackables.at(kMaterial), 19);
    applied_effects = effect_ids;
    return {{49, {{"generation", generation}}}};
  }

  std::optional<std::int64_t> item_use_next_gameplay_tick() const override {
    return deadline;
  }

  bool tutorial{true};
  bool combat_at_creation{true};
  bool ready{true};
  int generation{1};
  std::vector<int> current_effects{1001};
  std::vector<int> applied_effects;
  std::int64_t deadline{123456};

private:
  EconomyFixture &fixture_;
};

std::shared_ptr<const ItemUseCatalog> empty_catalog() {
  return std::make_shared<const ItemUseCatalog>(std::vector<Json>{}, std::vector<Json>{});
}

std::shared_ptr<RecordingProgress> captured_progress(EconomyFixture &fixture) {
  return std::make_shared<RecordingProgress>(fixture.repositories, fixture.actor, fixture.trace);
}

TEST(ItemUseFeature, DisabledCatalogCreatesNoPlayerOrProgressCallbacks) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ItemUseFeature feature(std::shared_ptr<const ItemUseCatalog>{}, fixture.items);

  EXPECT_FALSE(feature.create_session({}, context, *fixture.events));
  EXPECT_TRUE(fixture.trace.empty());
}

TEST(ItemUseFeature, EffectCallbacksResolveRebuiltCombatAndCommitBeforeApplyingItsEffects) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ItemUseFeature feature(empty_catalog(), fixture.items);
  auto session = feature.create_session(captured_progress(fixture), context, *fixture.events,
                                        std::make_unique<SeededLootRandom>(7));
  ASSERT_TRUE(session);
  context.generation = 2;
  context.current_effects = {2002, 2003};
  FrameContext frame(kItemUseRequestOpcode,
                     WorldGameplayRequest{kItemUseRequestOpcode, 9, {{"item_cms_id", kMaterial}}});
  frame.session_packets.push_back({999, Json::object()});

  EXPECT_TRUE(feature.handle(session.get(), frame, context));
  EXPECT_EQ(context.applied_effects, (std::vector<int>{2002, 2003}));
  ASSERT_EQ(frame.session_packets.size(), 3U);
  EXPECT_EQ(frame.session_packets[0].opcode, 999);
  EXPECT_EQ(frame.session_packets[1].opcode, 169);
  EXPECT_EQ(frame.session_packets[2].opcode, 49);
  EXPECT_EQ(frame.session_packets[2].message.at("generation"), 2);
  EXPECT_EQ(frame.action_deadline, context.deadline);
  EXPECT_FALSE(fixture.last_snapshot);
  EXPECT_FALSE(fixture.prepared_stats.at("refreshed").get<bool>());
}

TEST(ItemUseFeature, CombatAbsentAtCreationDoesNotInstallEffectCapabilitiesLater) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  context.combat_at_creation = false;
  ItemUseFeature feature(empty_catalog(), fixture.items);
  auto session = feature.create_session(captured_progress(fixture), context, *fixture.events,
                                        std::make_unique<SeededLootRandom>(7));
  context.combat_at_creation = true;
  FrameContext frame(kItemUseRequestOpcode,
                     WorldGameplayRequest{kItemUseRequestOpcode, 9, {{"item_cms_id", kMaterial}}});

  EXPECT_FALSE(feature.handle(session.get(), frame, context));
  EXPECT_TRUE(fixture.trace.empty());
  EXPECT_FALSE(frame.action_deadline);
  EXPECT_TRUE(context.applied_effects.empty());
}

TEST(ItemUseFeature, RefusedEffectUseDoesNotApplyOrAdoptAndStillRearmsTheOriginalDeadline) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  context.ready = false;
  ItemUseFeature feature(empty_catalog(), fixture.items);
  auto session = feature.create_session(captured_progress(fixture), context, *fixture.events,
                                        std::make_unique<SeededLootRandom>(7));
  const auto before = fixture.stored_progress();
  FrameContext frame(kItemUseRequestOpcode,
                     WorldGameplayRequest{kItemUseRequestOpcode, 9, {{"item_cms_id", kMaterial}}});

  EXPECT_TRUE(feature.handle(session.get(), frame, context));
  EXPECT_EQ(fixture.stored_progress(), before);
  EXPECT_TRUE(context.applied_effects.empty());
  ASSERT_EQ(frame.session_packets.size(), 1U);
  EXPECT_EQ(frame.session_packets.front().opcode, 2);
  EXPECT_EQ(frame.action_deadline, context.deadline);
  EXPECT_EQ(fixture.trace, (std::vector<std::string>{}));
}

TEST(ItemUseFeature, DrawBoxGrantsAdoptThenReloadTheItemSnapshotBeforeItsReply) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  context.combat_at_creation = false;
  const std::vector<Json> boxes{{{"id", std::to_string(kMaterial)}, {"FK_drawBoxId", "50"}}};
  const std::vector<Json> rows{{{"drawBoxId", "50"},
                                {"itemId", std::to_string(kEquipment)},
                                {"moneyType", ""},
                                {"minAmount", "1"},
                                {"maxAmount", "1"},
                                {"drawWeight", ""}}};
  ItemUseFeature feature(std::make_shared<const ItemUseCatalog>(boxes, rows), fixture.items);
  auto session = feature.create_session(captured_progress(fixture), context, *fixture.events,
                                        std::make_unique<SeededLootRandom>(7));
  FrameContext frame(kItemUseRequestOpcode,
                     WorldGameplayRequest{kItemUseRequestOpcode, 9, {{"item_cms_id", kMaterial}}});

  ASSERT_TRUE(feature.handle(session.get(), frame, context));
  EXPECT_EQ(fixture.stored_progress().stackables.at(kMaterial), 19);
  ASSERT_TRUE(fixture.last_snapshot);
  EXPECT_EQ(fixture.last_snapshot->items.size(), fixture.actor.items.size() + 1);
  EXPECT_LT(fixture.trace_index("commit"), fixture.trace_index("adopt"));
  EXPECT_LT(fixture.trace_index("adopt"), fixture.trace_index("snapshot"));
  ASSERT_EQ(frame.session_packets.size(), 1U);
  EXPECT_EQ(frame.session_packets.front().opcode, 169);
  EXPECT_FALSE(fixture.prepared_stats.at("refreshed").get<bool>());
}

} // namespace
} // namespace chronicles::item_use
