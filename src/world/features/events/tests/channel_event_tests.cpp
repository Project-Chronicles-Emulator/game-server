#include "world/features/events/channel_event.hpp"
#include "world/features/events/event_gm_commands.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace chronicles::channel_event {
namespace {

using Json = nlohmann::json;

constexpr int kChannelMap = 10125;
constexpr std::uint64_t kAltar = 21533402654060550ULL;
constexpr int kOffering = 100194;

Json definitions() {
  return {{"format_version", 1},
          {"channel_events", Json::array({{{"map_id", kChannelMap},
                                           {"zone_key", "Zone_01_ch21"},
                                           {"channel_event_id", 101015},
                                           {"reward_id", 101015}},
                                          {{"map_id", 8000},
                                           {"zone_key", "Tutorial_Area"},
                                           {"channel_event_id", 101019},
                                           {"reward_id", nullptr}}})},
          {"tribute_altars", Json::array({{{"map_id", kChannelMap},
                                           {"unit_id", std::to_string(kAltar)},
                                           {"doodad_cms_id", 49054},
                                           {"item_cms_id", kOffering},
                                           {"position", {-2450974, 3854433, -604092}}}})}};
}

WorldPosition beside_altar(int offset_mm = 3000) {
  return {kChannelMap, -2450974 + offset_mm, 3854433, -604092, 0.0F};
}

WorldProgress owning(int count) {
  WorldProgress progress;
  progress.revision = 4;
  progress.stackables[kOffering] = count;
  return progress;
}

Json tribute(int count, std::uint64_t target = kAltar, int item = kOffering) {
  return {{"item_cms_id", item},
          {"count", count},
          {"target_unit_id", std::to_string(target)},
          {"packet_seq", {{"seq", "31"}}}};
}

TEST(ChannelEventDataTest, LoadsBindingsAndAltars) {
  const ChannelEventData data(definitions());
  ASSERT_NE(data.binding(kChannelMap), nullptr);
  EXPECT_EQ(data.binding(kChannelMap)->channel_event_id, 101015);
  EXPECT_FALSE(data.binding(8000)->reward_id.has_value());
  ASSERT_NE(data.altar(kChannelMap, kAltar), nullptr);
  EXPECT_EQ(data.nearest_altar(beside_altar(), kTributeRangeMm), data.altar(kChannelMap, kAltar));
  EXPECT_EQ(data.nearest_altar(beside_altar(20000), kTributeRangeMm), nullptr);

  auto duplicate = definitions();
  duplicate["tribute_altars"].push_back(duplicate["tribute_altars"][0]);
  EXPECT_THROW(ChannelEventData{duplicate}, std::runtime_error);
}

TEST(ChannelEventDataTest, GeneratedDefinitionsLoadWhenExported) {
  const auto path = std::filesystem::path(CHRONICLES_PROJECT_ROOT) /
                    "data/generated/channel-events/definitions.json";
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "Requires tools/build_channel_event_data.py output";
  }
  const auto data = ChannelEventData::load(path);
  EXPECT_GT(data->binding_count(), 0U);
  EXPECT_GT(data->altar_count(), 0U);
  ASSERT_NE(data->binding(kChannelMap), nullptr);
}

TEST(TributePlanTest, ValidatesTargetRangeItemAndOwnership) {
  const ChannelEventData data(definitions());
  EXPECT_EQ(plan_tribute(data, tribute(1, 5), beside_altar(), owning(3)).error,
            "Tribute_InvalidTarget");
  EXPECT_EQ(plan_tribute(data, tribute(1), beside_altar(20000), owning(3)).error,
            "Tribute_TargetTooFar");
  EXPECT_EQ(plan_tribute(data, tribute(1, kAltar, 2000), beside_altar(), owning(3)).error,
            "Tribute_InvalidItem");
  EXPECT_EQ(plan_tribute(data, tribute(4), beside_altar(), owning(3)).error, "Tribute_InvalidItem");
  EXPECT_EQ(plan_tribute(data, tribute(0), beside_altar(), owning(3)).error, "BadRequest");

  auto other_map = beside_altar();
  other_map.map_id = 9012;
  EXPECT_EQ(plan_tribute(data, tribute(1), other_map, owning(3)).error, "Tribute_InvalidTarget");

  const auto accepted = plan_tribute(data, tribute(3), beside_altar(), owning(3));
  ASSERT_FALSE(accepted.error.has_value());
  const auto after = consume_tribute(owning(3), accepted);
  EXPECT_EQ(after.stackables.at(kOffering), 0);
  EXPECT_EQ(after.revision, 5);

  const auto update = tribute_items_packet(accepted, after, tribute(3), 99);
  EXPECT_EQ(update.opcode, kItemsUpdateNotification);
  EXPECT_EQ(update.message.at("stackables")[0].at("quantity"), 0);
  EXPECT_TRUE(update.message.contains("tribute"));
  EXPECT_EQ(update.message.at("packet_seq").at("seq"), "31");
}

TEST(ChannelEventBoardTest, CommitsOnlyAfterItemsAndNeverExceedsTheTarget) {
  ChannelEventBoard board(5);
  const auto first = board.commit_tribute(kChannelMap, kAltar, 3, [] { return true; });
  EXPECT_EQ(first.status, TributeCommitStatus::committed);
  EXPECT_EQ(first.progress.count, 3);
  EXPECT_FALSE(first.completed_event);

  bool charged{};
  const auto over = board.commit_tribute(kChannelMap, kAltar, 3, [&] {
    charged = true;
    return true;
  });
  EXPECT_EQ(over.status, TributeCommitStatus::limit_reached);
  EXPECT_FALSE(charged);
  const auto full = board.commit_tribute(kChannelMap, kAltar, 2, [] { return true; });
  EXPECT_TRUE(full.completed_event);
  EXPECT_EQ(board.status(kChannelMap), EventStatus::success);
  EXPECT_EQ(board.tribute_progress(kChannelMap, kAltar).count, 5);
  board.reset(kChannelMap);
  EXPECT_EQ(board.status(kChannelMap), EventStatus::none);
  EXPECT_EQ(board.tribute_progress(kChannelMap, kAltar).count, 0);
}

TEST(ChannelEventBoardTest, FailedItemCommitsNeverPublishCountOrCompletion) {
  ChannelEventBoard board(5);
  const auto failed = board.commit_tribute(kChannelMap, kAltar, 3, [] { return false; });
  EXPECT_EQ(failed.status, TributeCommitStatus::storage_conflict);
  EXPECT_EQ(board.tribute_progress(kChannelMap, kAltar).count, 0);
  const auto second = board.commit_tribute(kChannelMap, kAltar, 2, [] { return true; });
  EXPECT_EQ(second.progress.count, 2);
  EXPECT_FALSE(second.completed_event);
  EXPECT_EQ(board.status(kChannelMap), EventStatus::none);
  EXPECT_THROW(
      board.commit_tribute(kChannelMap, kAltar, 3,
                           []() -> bool { throw std::runtime_error("storage unavailable"); }),
      std::runtime_error);
  EXPECT_EQ(board.tribute_progress(kChannelMap, kAltar).count, 2);
}

TEST(ChannelEventPackets, UseRecoveredFieldNames) {
  EXPECT_EQ(status_packet(EventStatus::success).message.at("status_type"),
            "EX5ChannelEventStatusType_Success");
  const auto count = tribute_count_packet({2, 10});
  EXPECT_EQ(count.opcode, kTributeCountNotification);
  EXPECT_EQ(count.message.at("target_count"), 10);
  EXPECT_EQ(common_error("Tribute_LimitReached", 5).message.at("packet_seq").at("seq"), "5");
}

TEST(ChannelEventPackets, ReportedPositionMustStayNearTheServerPosition) {
  const WorldPosition server{9012, 1000, 2000, 0, 0.0F};
  EXPECT_TRUE(reported_position_near({{"pos", {{"millimeter_x", 1500}, {"millimeter_y", 2000}}}},
                                     server, 5000));
  EXPECT_FALSE(reported_position_near({{"pos", {{"millimeter_x", 9000}, {"millimeter_y", 2000}}}},
                                      server, 5000));
  EXPECT_FALSE(reported_position_near({{"map_id", 10103}}, server, 5000));
  EXPECT_FALSE(reported_position_near({{"pos", {{"millimeter_x", "1"}}}}, server, 5000));
}

TEST(EventGmCommands, ParsesTextAndStructuredForms) {
  using event_gm::EventCommandKind;
  const auto start = event_gm::parse_event_command(20009, {{"text", "Ranking_Start 1000001"}});
  ASSERT_TRUE(start.command.has_value());
  EXPECT_EQ(start.command->kind, EventCommandKind::ranking_start);
  EXPECT_EQ(start.command->target_id, 1000001);

  const auto missing = event_gm::parse_event_command(20009, {{"text", "ranking_finish"}});
  EXPECT_TRUE(missing.matched);
  EXPECT_FALSE(missing.command.has_value());

  const auto raid = event_gm::parse_event_command(
      20008, {{"cmds", Json::array({{{"raid_channel_enter", {{"map_id", 10103}}}}})}});
  ASSERT_TRUE(raid.command.has_value());
  EXPECT_EQ(raid.command->target_id, 10103);

  EXPECT_FALSE(event_gm::parse_event_command(20009, {{"text", "god on"}}).matched);
  // The debug panel parser owns channel_event_info.
  EXPECT_FALSE(event_gm::parse_event_command(20009, {{"text", "channel_event_info"}}).matched);
  const auto extra = event_gm::parse_event_command(20009, {{"text", "ranking_cancel now"}});
  EXPECT_TRUE(extra.matched);
  EXPECT_FALSE(extra.command.has_value());
}

} // namespace
} // namespace chronicles::channel_event
