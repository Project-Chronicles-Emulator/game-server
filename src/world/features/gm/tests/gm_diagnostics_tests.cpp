#include "world/features/gm/gm_diagnostics.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <string>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::uint16_t kTextRequest = 20009;
constexpr std::uint16_t kStructuredRequest = 20008;

gm::DiagnosticParse text(const std::string &line) {
  return gm::parse_diagnostic_command(kTextRequest, {{"text", line}});
}

gm::DiagnosticParse structured(const Json &command) {
  return gm::parse_diagnostic_command(kStructuredRequest, {{"cmds", Json::array({command})}});
}

gm::DiagnosticCommand command_of(const gm::DiagnosticParse &parse) {
  EXPECT_TRUE(parse.matched);
  EXPECT_TRUE(parse.error.empty()) << parse.error;
  EXPECT_TRUE(parse.command.has_value());
  return parse.command.value_or(gm::DiagnosticCommand{});
}

// Encodes against the recovered schema and returns the decoded message, proving the shape.
// Client requests carry a frame sequence; server notifications do not.
Json client_round_trip(std::uint16_t opcode, const Json &message) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  return protocol.decode(protocol.encode(opcode, message, 0)).at("message");
}

Json server_round_trip(std::uint16_t opcode, const Json &message) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  return protocol.decode(protocol.encode(opcode, message)).at("message");
}

TEST(GmDiagnostics, LeavesOtherGmCommandsToTheirOwnParsers) {
  EXPECT_FALSE(text("pos").matched);
  EXPECT_FALSE(text("item_add 1 1").matched);
  EXPECT_FALSE(text("").matched);
  EXPECT_FALSE(structured({{"revive", Json::object()}}).matched);
  EXPECT_FALSE(gm::parse_diagnostic_command(20010, {{"text", "game_time 12"}}).matched);
}

TEST(GmDiagnostics, ParsesTheGameClockGrammar) {
  const auto running = command_of(text("game_time 18.5"));
  EXPECT_EQ(running.kind, gm::DiagnosticKind::game_time);
  EXPECT_DOUBLE_EQ(running.game_time.hour, 18.5);
  EXPECT_FALSE(running.game_time.stopped);
  EXPECT_EQ(running.game_time.day_seconds, gm::kDefaultGameDaySeconds);

  const auto frozen = command_of(text("TIME stop 6 600"));
  EXPECT_TRUE(frozen.game_time.stopped);
  EXPECT_DOUBLE_EQ(frozen.game_time.hour, 6.0);
  EXPECT_EQ(frozen.game_time.day_seconds, 600);

  EXPECT_FALSE(text("game_time").command.has_value());
  EXPECT_FALSE(text("game_time 25").command.has_value());
  EXPECT_FALSE(text("game_time -1").command.has_value());
  EXPECT_FALSE(text("game_time 12 10").command.has_value());
  EXPECT_FALSE(text("game_time stop").command.has_value());
  EXPECT_TRUE(text("game_time noon").matched);
}

TEST(GmDiagnostics, ParsesWatchTargetsAndTypes) {
  const auto unit = command_of(text("watch 4026597410"));
  EXPECT_EQ(unit.kind, gm::DiagnosticKind::watch);
  EXPECT_EQ(unit.target, gm::WatchTarget::unit);
  EXPECT_EQ(unit.unit_id, 4026597410u);
  EXPECT_EQ(unit.watch_types, std::vector<gm::WatchType>{gm::WatchType::position});

  const auto self = command_of(text("debug_watch me all"));
  EXPECT_EQ(self.target, gm::WatchTarget::self);
  EXPECT_EQ(self.watch_types.size(), 3u);
  EXPECT_EQ(command_of(text("watch spawned path")).target, gm::WatchTarget::spawned);

  EXPECT_FALSE(text("watch").command.has_value());
  EXPECT_FALSE(text("watch 0").command.has_value());
  EXPECT_FALSE(text("watch 5 sound").command.has_value());

  EXPECT_EQ(command_of(text("unwatch")).unit_id, 0u);
  EXPECT_EQ(command_of(text("unwatch 77")).unit_id, 77u);
  EXPECT_FALSE(text("unwatch everything").command.has_value());

  EXPECT_EQ(command_of(text("regen_stat")).kind, gm::DiagnosticKind::regen_stat);
  EXPECT_EQ(command_of(text("channel_event")).kind, gm::DiagnosticKind::channel_event_info);
  EXPECT_EQ(command_of(text("debug_npc_field")).kind, gm::DiagnosticKind::npc_field);
  EXPECT_FALSE(text("regen_stat now").command.has_value());
}

TEST(GmDiagnostics, ParsesTheClientDebugUiRequestsFromTheWireSchema) {
  // The client's debug watch sends unit_id, type and add=true; removal leaves add unset.
  const Json add_body{{"unit_id", "4026597410"}, {"type", "Path"}, {"add", true}};
  const Json add_request{{"cmds", Json::array({{{"debug_watch", add_body}}})}};
  const auto added = command_of(gm::parse_diagnostic_command(
      kStructuredRequest, client_round_trip(kStructuredRequest, add_request)));
  EXPECT_EQ(added.kind, gm::DiagnosticKind::watch);
  EXPECT_EQ(added.unit_id, 4026597410u);
  EXPECT_EQ(added.watch_types, std::vector<gm::WatchType>{gm::WatchType::path});

  const Json remove_request{
      {"cmds", Json::array({{{"debug_watch", {{"unit_id", "4026597410"}, {"type", "AI"}}}}})}};
  const auto removed = command_of(gm::parse_diagnostic_command(
      kStructuredRequest, client_round_trip(kStructuredRequest, remove_request)));
  EXPECT_EQ(removed.kind, gm::DiagnosticKind::unwatch);
  EXPECT_EQ(removed.watch_types, std::vector<gm::WatchType>{gm::WatchType::ai});

  const Json channel_request{{"cmds", Json::array({{{"channel_event_info", Json::object()}}})}};
  const auto channel = command_of(gm::parse_diagnostic_command(
      kStructuredRequest, client_round_trip(kStructuredRequest, channel_request)));
  EXPECT_EQ(channel.kind, gm::DiagnosticKind::channel_event_info);

  const auto clock =
      command_of(structured({{"game_time", {{"one_day_secs", 900}, {"hours", 7.25}}}}));
  EXPECT_EQ(clock.game_time.day_seconds, 900);
  EXPECT_DOUBLE_EQ(clock.game_time.hour, 7.25);
  EXPECT_EQ(command_of(structured({{"game_time", Json::object()}})).game_time.day_seconds,
            gm::kDefaultGameDaySeconds);
  EXPECT_FALSE(structured({{"game_time", {{"one_day_secs", 5}}}}).command.has_value());
  EXPECT_FALSE(structured({{"debug_watch", {{"type", "Path"}}}}).command.has_value());
  EXPECT_EQ(command_of(structured({{"debug_player_regen_stat", Json::object()}})).kind,
            gm::DiagnosticKind::regen_stat);
}

TEST(GmDiagnostics, GameTimeCarriesTheClockFieldsTheClientReads) {
  gm::GameTimeSetting setting;
  setting.hour = 18.0;
  setting.day_seconds = 1200;
  const auto running = server_round_trip(gm::kWorldGameTimeNotification,
                                         gm::world_game_time_message(setting, 1'800'000'000));
  EXPECT_EQ(running.at("started_timestamp"), "1800000000");
  EXPECT_EQ(running.at("one_day_secs"), "1200");
  EXPECT_FLOAT_EQ(running.at("daily_ratio").get<float>(), 0.75F);
  EXPECT_FALSE(running.value("stopped", false));

  setting.stopped = true;
  setting.hour = 0.0;
  const auto frozen = server_round_trip(gm::kWorldGameTimeNotification,
                                        gm::world_game_time_message(setting, 1'800'000'000));
  EXPECT_TRUE(frozen.at("stopped").get<bool>());
  // daily_ratio is a proto3 optional field: an explicit zero must stay present (midnight).
  EXPECT_TRUE(frozen.contains("daily_ratio"));
}

TEST(GmDiagnostics, OverlayPacketsEncodeAgainstTheDebugSchema) {
  const WorldPosition position{9012, 1000, -2000, 300, 90.0F};
  const WorldPosition destination{9012, 5000, -2000, 300, 0.0F};

  const auto located = server_round_trip(gm::kDebugPositionNotification,
                                         gm::debug_position_message(4026597410, position));
  EXPECT_EQ(located.at("unit_id"), "4026597410");
  EXPECT_EQ(located.at("pos").at("millimeter_x"), 1000);
  EXPECT_FLOAT_EQ(located.at("yaw").get<float>(), 90.0F);

  const auto path = server_round_trip(gm::kDebugPathNotification,
                                      gm::debug_path_message(4026597410, position, destination));
  ASSERT_EQ(path.at("paths").size(), 2u);
  EXPECT_EQ(path.at("paths").at(1).at("pos").at("millimeter_x"), 5000);
  const auto idle_path = gm::debug_path_message(4026597410, position, std::nullopt);
  EXPECT_EQ(idle_path.at("paths").size(), 1u);

  const auto ai = server_round_trip(gm::kDebugAiNotification,
                                    gm::debug_ai_message(4026597410, NpcAiState::Engaged));
  EXPECT_EQ(ai.at("phase"), 1);
  EXPECT_EQ(ai.at("nodes").at(0).at("name"), "Engaged");

  const Json stats{{"15", 12}, {"18", 500}, {"69", 480}, {"72", 90}, {"3", 7}};
  const auto regen = server_round_trip(gm::kDebugPlayerRegenStatNotification,
                                       gm::player_regen_stat_message(stats, 10'000'000));
  EXPECT_EQ(regen.at("tick"), "1000000000");
  // Only the regen panel stats are reported, in stat type order; stat 3 is not one of them.
  ASSERT_EQ(regen.at("debug_stat_infos").size(), 4u);
  EXPECT_EQ(regen.at("debug_stat_infos").at(0).at("stat_value"), 12);

  const auto channel = server_round_trip(gm::kDebugChannelEventInfoNotification,
                                         gm::channel_event_info_message(9012));
  EXPECT_EQ(channel.at("map_cms_id"), 9012);
  EXPECT_FALSE(channel.contains("event_cms_id"));
}

TEST(GmDiagnostics, WatchListIsBoundedAndPaced) {
  gm::WatchList watches;
  EXPECT_FALSE(watches.next_tick());
  EXPECT_FALSE(watches.due(0));

  EXPECT_EQ(watches.add(10, gm::WatchType::position), gm::WatchAddResult::added);
  EXPECT_EQ(watches.add(10, gm::WatchType::position), gm::WatchAddResult::already_watched);
  EXPECT_EQ(watches.add(10, gm::WatchType::skill_collision), gm::WatchAddResult::unsupported_type);
  EXPECT_EQ(watches.add(10, gm::WatchType::abnormal_status), gm::WatchAddResult::unsupported_type);
  EXPECT_TRUE(watches.due(0));

  watches.mark_emitted(100);
  EXPECT_EQ(watches.next_tick(), 100 + gm::kWatchIntervalTicks);
  EXPECT_FALSE(watches.due(100 + gm::kWatchIntervalTicks - 1));
  EXPECT_TRUE(watches.due(100 + gm::kWatchIntervalTicks));

  for (std::uint64_t unit = 11; watches.entries().size() < gm::kMaximumWatchesPerSession; ++unit) {
    ASSERT_EQ(watches.add(unit, gm::WatchType::path), gm::WatchAddResult::added);
  }
  EXPECT_EQ(watches.add(99, gm::WatchType::ai), gm::WatchAddResult::limit_reached);

  EXPECT_TRUE(watches.remove(10, gm::WatchType::position));
  EXPECT_FALSE(watches.remove(10, gm::WatchType::position));
  EXPECT_EQ(watches.remove_unit(11), 1u);
  watches.clear();
  EXPECT_TRUE(watches.empty());
  EXPECT_FALSE(watches.next_tick());
}

} // namespace
} // namespace chronicles
