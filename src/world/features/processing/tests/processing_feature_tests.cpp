#include "world/features/processing/processing_feature.hpp"

#include "world/core/tests/economy_test_support.hpp"

#include <gtest/gtest.h>

namespace chronicles::processing {
namespace {

using namespace economy_testing;

constexpr std::uint64_t kStation =
    (static_cast<std::uint64_t>(123) << 32) | (static_cast<std::uint64_t>(4) << 12) | 6;
constexpr int kStationDefinition = 80001;
constexpr int kInteraction = 602;
constexpr int kAnimation = 60006;
constexpr int kRecipe = 9001;
constexpr std::int64_t kStartedTicks = 100'000'000;
constexpr std::int64_t kExecuteDelayTicks = 100'000;

std::shared_ptr<const ProcessingData> focused_data() {
  const Json profile{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"maximum_craft_amount", 10},
      {"maps", Json::array({{{"map_id", 8000},
                             {"area_min_mm", {-1000, -1000, -1000}},
                             {"area_max_mm", {1000, 1000, 1000}}}})},
      {"animations",
       Json::array(
           {{{"interaction_anim_id", kAnimation}, {"execute_time_ms", 10}, {"duration_ms", 20}}})},
      {"interactions", Json::array({{{"interaction_id", kInteraction},
                                     {"group_id", kInteraction},
                                     {"interaction_anim_id", kAnimation}}})},
      {"stations", Json::array({{{"doodad_cms_id", kStationDefinition},
                                 {"interaction_group_id", kInteraction},
                                 {"craft_ids", {kRecipe}}}})},
      {"crafts",
       Json::array({{{"item_craft_id", kRecipe},
                     {"product_item_cms_id", kEquipment},
                     {"product_count", 1},
                     {"product_storage", "instance"},
                     {"product_max_durability", 10000},
                     {"product_grade", "Normal"},
                     {"gold_cost", 7},
                     {"materials", Json::array({{{"item_cms_id", kMaterial}, {"count", 2}}})}}})},
      {"unsupported_crafts", Json::array()},
      {"placements", Json::array({{std::to_string(kStation), kStationDefinition, 0, 0, 0}})}};
  return std::make_shared<const ProcessingData>(profile);
}

class ProbeContext final : public ProcessingSessionContext {
public:
  explicit ProbeContext(EconomyFixture &fixture) : fixture_(fixture) {
  }

  const WorldProgress *processing_tutorial_progress() const override {
    return tutorial ? &fixture_.progress->cached : nullptr;
  }

  const Json *processing_player_stats() const override {
    return &fixture_.prepared_stats;
  }

  std::int64_t processing_monotonic_ticks() const override {
    return ticks;
  }

  bool processing_other_action_busy() const override {
    return other_busy;
  }

  bool tutorial{true};
  bool other_busy{};
  std::int64_t ticks{kStartedTicks};

private:
  EconomyFixture &fixture_;
};

FrameContext request_frame(std::uint16_t opcode, Json message) {
  FrameContext frame(opcode, WorldGameplayRequest{opcode, 9, std::move(message)});
  frame.gameplay_position = WorldPosition{8000, 0, 0, 0, 0.0F};
  return frame;
}

Json craft_start() {
  return {{"interaction_anim_id", kAnimation},
          {"target_unit_id", std::to_string(kStation)},
          {"tick", "10000000000"},
          {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
          {"yaw", 0.0},
          {"param",
           {{"interaction",
             {{"interaction_id", kInteraction},
              {"execute_craft", {{"item_craft_id", kRecipe}, {"count", 1}}}}}}}};
}

void open_station(ProcessingFeature &feature, ProcessingSession &session, ProbeContext &context) {
  auto frame =
      request_frame(kProcessingDialogueStartRequest,
                    {{"target_unit_id", std::to_string(kStation)},
                     {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
                     {"yaw", 0.0}});
  ASSERT_TRUE(feature.handle(&session, frame, context));
  ASSERT_EQ(frame.session_packets.size(), 1U);
  ASSERT_EQ(frame.session_packets.front().opcode, 161);
}

std::shared_ptr<RecordingProgress> captured_progress(EconomyFixture &fixture) {
  return std::make_shared<RecordingProgress>(fixture.repositories, fixture.actor, fixture.trace);
}

TEST(ProcessingFeature, MissingProfileCreatesNoPlayerOrDurableQueries) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ProcessingFeature feature({});

  EXPECT_FALSE(feature.create_session(17, {}, context, *fixture.events));
  EXPECT_TRUE(fixture.trace.empty());
}

TEST(ProcessingFeature, UnrelatedAndPositionlessFramesRemainUnconsumed) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ProcessingFeature feature(focused_data());
  auto session = feature.create_session(17, captured_progress(fixture), context, *fixture.events);
  auto ping = request_frame(20003, Json::object());
  auto start = request_frame(kProcessingDialogueStartRequest,
                             {{"target_unit_id", std::to_string(kStation)}});
  start.gameplay_position.reset();

  EXPECT_FALSE(feature.handle(session.get(), ping, context));
  EXPECT_FALSE(feature.handle(session.get(), start, context));
  EXPECT_TRUE(fixture.trace.empty());
}

TEST(ProcessingFeature, CraftCommitsAndAdoptsBeforeReloadedSnapshotAndOrderedReplies) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ProcessingFeature feature(focused_data());
  auto session = feature.create_session(17, captured_progress(fixture), context, *fixture.events);
  open_station(feature, *session, context);
  auto start = request_frame(kProcessingStartInteractionRequest, craft_start());
  ASSERT_TRUE(feature.handle(session.get(), start, context));
  ASSERT_TRUE(session->busy());
  context.ticks += kExecuteDelayTicks;
  auto execute = request_frame(kProcessingExecuteInteractionRequest,
                               {{"interaction_anim_id", kAnimation}, {"tick", "10000100000"}});
  execute.session_packets.push_back({999, Json::object()});

  ASSERT_TRUE(feature.handle(session.get(), execute, context));
  EXPECT_EQ(fixture.stored_progress().gold, 4993);
  EXPECT_EQ(fixture.stored_progress().stackables.at(kMaterial), 18);
  ASSERT_TRUE(fixture.last_snapshot);
  EXPECT_EQ(fixture.last_snapshot->items.size(), fixture.actor.items.size() + 1);
  EXPECT_LT(fixture.trace_index("commit"), fixture.trace_index("adopt"));
  EXPECT_LT(fixture.trace_index("adopt"), fixture.trace_index("snapshot"));
  ASSERT_EQ(execute.session_packets.size(), 3U);
  EXPECT_EQ(execute.session_packets[0].opcode, 169);
  EXPECT_EQ(execute.session_packets[1].opcode, 147);
  EXPECT_EQ(execute.session_packets[2].opcode, 167);
  EXPECT_FALSE(fixture.prepared_stats.at("refreshed").get<bool>());
}

TEST(ProcessingFeature, BusyForeignActionConsumesTheRefusalWithoutSpendingMaterials) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ProcessingFeature feature(focused_data());
  auto session = feature.create_session(17, captured_progress(fixture), context, *fixture.events);
  open_station(feature, *session, context);
  context.other_busy = true;
  const auto before = fixture.stored_progress();
  auto start = request_frame(kProcessingStartInteractionRequest, craft_start());

  EXPECT_TRUE(feature.handle(session.get(), start, context));
  EXPECT_EQ(fixture.stored_progress(), before);
  EXPECT_FALSE(session->busy());
  ASSERT_EQ(start.session_packets.size(), 1U);
  EXPECT_EQ(start.session_packets.front().opcode, 165);
  EXPECT_EQ(start.session_packets.front().message.at("ec"), "InteractionAnim_UsingSkill");
  EXPECT_FALSE(fixture.last_snapshot);
}

TEST(ProcessingFeature, CachedRevisionIsResolvedWhenTheCraftExecutesRatherThanAtFactoryCreation) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  ProcessingFeature feature(focused_data());
  auto subject = captured_progress(fixture);
  auto session = feature.create_session(17, subject, context, *fixture.events);
  const auto before = fixture.stored_progress();
  auto revised = before;
  ++revised.revision;
  revised.gold = 4000;
  ASSERT_TRUE(fixture.repositories.character_state->save_world_progress(
      fixture.actor.account_id, fixture.actor.id, fixture.actor.world_id, before, revised));
  fixture.progress->cached = revised;
  open_station(feature, *session, context);
  auto start = request_frame(kProcessingStartInteractionRequest, craft_start());
  ASSERT_TRUE(feature.handle(session.get(), start, context));
  context.ticks += kExecuteDelayTicks;
  auto execute = request_frame(kProcessingExecuteInteractionRequest,
                               {{"interaction_anim_id", kAnimation}, {"tick", "10000100000"}});

  ASSERT_TRUE(feature.handle(session.get(), execute, context));
  EXPECT_EQ(fixture.stored_progress().gold, 3993);
  EXPECT_EQ(fixture.stored_progress().revision, revised.revision + 1);
  EXPECT_EQ(fixture.progress->cached, fixture.stored_progress());
}

} // namespace
} // namespace chronicles::processing
