#include "world/features/gm/gm_tools.hpp"
#include "world/session/native_world.hpp"
#include "world/features/escape/player_escape.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <string>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::uint16_t kTextRequest = 20009;
constexpr std::uint16_t kStructuredRequest = 20008;

gm::SessionParse text(const std::string &line) {
  return gm::parse_session_command(kTextRequest, {{"text", line}});
}

gm::SessionParse structured(const Json &command) {
  return gm::parse_session_command(kStructuredRequest, {{"cmds", Json::array({command})}});
}

gm::SessionCommand command_of(const gm::SessionParse &parse) {
  EXPECT_TRUE(parse.matched);
  EXPECT_TRUE(parse.error.empty()) << parse.error;
  EXPECT_TRUE(parse.command.has_value());
  return parse.command.value_or(gm::SessionCommand{});
}

TEST(GmTools, LeavesInventoryAndCompanionCommandsToTheirOwnParsers) {
  EXPECT_FALSE(text("item_add 15010403 3").matched);
  EXPECT_FALSE(text("set_level 10").matched);
  EXPECT_FALSE(text("spawn_vehicle 5").matched);
  EXPECT_FALSE(text("teleport_to_home").matched);
  EXPECT_FALSE(text("").matched);
  EXPECT_FALSE(gm::parse_session_command(kTextRequest, Json::object()).matched);
  EXPECT_FALSE(gm::parse_session_command(20010, {{"text", "pos"}}).matched);
}

TEST(GmTools, ParsesTeleportCommandsInMeters) {
  const auto teleport = command_of(text("TP -795.15 712.2 -698"));
  EXPECT_EQ(teleport.kind, gm::SessionCommandKind::teleport);
  EXPECT_DOUBLE_EQ(teleport.x_meters, -795.15);
  EXPECT_DOUBLE_EQ(teleport.y_meters, 712.2);
  EXPECT_DOUBLE_EQ(teleport.z_meters, -698.0);

  const auto up = command_of(text("up 50"));
  EXPECT_EQ(up.kind, gm::SessionCommandKind::teleport_relative);
  EXPECT_EQ(up.direction, gm::RelativeDirection::up);
  EXPECT_DOUBLE_EQ(up.z_meters, 50.0);
  EXPECT_EQ(command_of(text("down 2.5")).direction, gm::RelativeDirection::down);
  EXPECT_EQ(command_of(text("fwd 10")).direction, gm::RelativeDirection::forward);

  const auto same_coordinates = command_of(text("tp_map 9011"));
  EXPECT_EQ(same_coordinates.kind, gm::SessionCommandKind::teleport_map);
  EXPECT_EQ(same_coordinates.map_id, 9011);
  EXPECT_FALSE(same_coordinates.has_coordinates);
  const auto explicit_coordinates = command_of(text("map 1 10 20 30"));
  EXPECT_TRUE(explicit_coordinates.has_coordinates);
  EXPECT_DOUBLE_EQ(explicit_coordinates.z_meters, 30.0);

  const auto go_to = command_of(text("goto Keeper"));
  EXPECT_EQ(go_to.kind, gm::SessionCommandKind::go_to_player);
  EXPECT_EQ(go_to.player_name, "Keeper");
  EXPECT_EQ(command_of(text("bring Keeper")).kind, gm::SessionCommandKind::summon_player);
}

TEST(GmTools, RejectsMalformedTeleportsWithUsage) {
  for (const auto *line :
       {"tp 1 2", "tp a b c", "tp 1 2 3 4 5", "tp 1e9 0 0", "up", "up -5", "fwd 999999", "tp_map",
        "tp_map -1", "tp_map 9011 1 2", "goto", "goto a b"}) {
    const auto parse = text(line);
    EXPECT_TRUE(parse.matched) << line;
    EXPECT_FALSE(parse.command.has_value()) << line;
    EXPECT_FALSE(parse.error.empty()) << line;
  }
  EXPECT_FALSE(text("goto " + std::string(40, 'a')).command.has_value());
}

TEST(GmTools, ParsesTogglesAndSpawns) {
  EXPECT_EQ(command_of(text("god on")).enabled, true);
  EXPECT_EQ(command_of(text("invincible off")).enabled, false);
  EXPECT_FALSE(command_of(text("god")).enabled.has_value());
  EXPECT_EQ(command_of(text("hide 1")).kind, gm::SessionCommandKind::invisible);
  EXPECT_EQ(command_of(text("stamina true")).kind, gm::SessionCommandKind::infinite_stamina);
  EXPECT_EQ(command_of(text("noclip on")).kind, gm::SessionCommandKind::free_movement);
  EXPECT_FALSE(text("god maybe").command.has_value());

  const auto spawn = command_of(text("spawn_monster 1001007 3"));
  EXPECT_EQ(spawn.kind, gm::SessionCommandKind::spawn_npc);
  EXPECT_EQ(spawn.npc_cms_id, 1001007);
  EXPECT_EQ(spawn.count, 3);
  EXPECT_EQ(command_of(text("spawn 1001007")).count, 1);
  EXPECT_FALSE(text("spawn 1001007 11").command.has_value());
  EXPECT_FALSE(text("spawn 0").command.has_value());

  EXPECT_EQ(command_of(text("help 3")).help_page, 3);
  EXPECT_FALSE(text("help 9").command.has_value());
  EXPECT_FALSE(text("pos now").command.has_value());
  EXPECT_EQ(command_of(text("who")).kind, gm::SessionCommandKind::list_players);

  const auto all_quests = command_of(text("complete_quests"));
  EXPECT_EQ(all_quests.kind, gm::SessionCommandKind::complete_quests);
  EXPECT_FALSE(all_quests.chain);
  EXPECT_TRUE(command_of(text("finish_quests CHAIN")).chain);
  EXPECT_FALSE(text("complete_quests forever").command.has_value());
  const auto one_quest = command_of(text("complete_quest 90100011"));
  EXPECT_EQ(one_quest.kind, gm::SessionCommandKind::complete_quest);
  EXPECT_EQ(one_quest.quest_id, 90100011);
  EXPECT_FALSE(text("complete_quest").command.has_value());
  EXPECT_FALSE(text("complete_quest -3").command.has_value());
}

TEST(GmTools, ParsesMapRevealWithoutTargetsOrResetArguments) {
  for (const auto *name : {"reveal_map", "unlock_map", "REVEAL_MAP"}) {
    EXPECT_EQ(command_of(text(name)).kind, gm::SessionCommandKind::reveal_map);
  }
  EXPECT_FALSE(text("reveal_map all").command.has_value());
  EXPECT_FALSE(text("unlock_map OtherPlayer").command.has_value());
  EXPECT_FALSE(text("reveal_map off").command.has_value());
}

TEST(GmTools, ParsesTheStructuredClientCommands) {
  EXPECT_EQ(command_of(structured({{"invincible", {{"value", true}}}})).enabled, true);
  EXPECT_EQ(command_of(structured({{"invisible", Json::object()}})).enabled, false);
  EXPECT_EQ(command_of(structured({{"fly_mode", {{"on", true}}}})).kind,
            gm::SessionCommandKind::free_movement);
  // The GM resurrection button sends GmCmd.revive instead of ReviveReq.
  EXPECT_EQ(command_of(structured({{"revive", Json::object()}})).kind,
            gm::SessionCommandKind::revive);
  EXPECT_EQ(command_of(structured({{"spawn_npc", {{"cms_id", 42}}}})).npc_cms_id, 42);
  EXPECT_EQ(command_of(structured({{"completable_quest_all", Json::object()}})).kind,
            gm::SessionCommandKind::complete_quests);
  EXPECT_EQ(command_of(structured({{"reward_quest", {{"cms_id", 501}}}})).quest_id, 501);
  EXPECT_FALSE(structured({{"complete_quest_progress", Json::object()}}).command.has_value());
  EXPECT_FALSE(structured({{"spawn_npc", Json::object()}}).command.has_value());
  EXPECT_FALSE(structured({{"item_add", {{"cms_id", 1}}}}).matched);

  const Json two_commands{
      {"cmds", Json::array({{{"revive", Json::object()}}, {{"invincible", {{"value", true}}}}})}};
  EXPECT_FALSE(gm::parse_session_command(kStructuredRequest, two_commands).matched);
}

TEST(GmTools, ParsesSuicideKickAndAnnouncements) {
  EXPECT_EQ(command_of(text("suicide")).kind, gm::SessionCommandKind::suicide);
  EXPECT_FALSE(text("suicide now").command.has_value());

  const auto immediate = command_of(text("kick"));
  EXPECT_EQ(immediate.kind, gm::SessionCommandKind::kick);
  EXPECT_EQ(immediate.delay_seconds, 0);
  EXPECT_EQ(command_of(text("test_kick 15")).delay_seconds, 15);
  EXPECT_FALSE(text("kick 61").command.has_value());
  EXPECT_FALSE(text("kick -1").command.has_value());

  // The announcement keeps its inner spacing and loses trailing whitespace.
  const auto notice = command_of(text("announce  Server  restart in 5 minutes  "));
  EXPECT_EQ(notice.kind, gm::SessionCommandKind::announce);
  EXPECT_EQ(notice.announcement, "Server  restart in 5 minutes");
  EXPECT_FALSE(text("announce").command.has_value());
  EXPECT_FALSE(
      text("notice " + std::string(gm::kMaximumAnnouncementBytes + 1, 'x')).command.has_value());
}

TEST(GmTools, ParsesTheStructuredServerCommands) {
  EXPECT_EQ(command_of(structured({{"regen", {{"value", true}}}})).kind,
            gm::SessionCommandKind::heal);
  EXPECT_EQ(command_of(structured({{"despawn_npc_all", Json::object()}})).kind,
            gm::SessionCommandKind::despawn_npcs);
  EXPECT_EQ(command_of(structured({{"suicide", Json::object()}})).kind,
            gm::SessionCommandKind::suicide);
  EXPECT_EQ(command_of(structured({{"test_kick", {{"after_sec", 5}}}})).delay_seconds, 5);
  EXPECT_EQ(command_of(structured({{"test_kick", Json::object()}})).delay_seconds, 0);
  EXPECT_FALSE(structured({{"test_kick", {{"after_sec", 3600}}}}).command.has_value());

  const auto notice =
      command_of(structured({{"announce", {{"message", "Hello"}, {"display_secs", 5.0}}}}));
  EXPECT_EQ(notice.announcement, "Hello");
  EXPECT_FALSE(structured({{"announce", Json::object()}}).command.has_value());
}

TEST(GmTools, StructuredRequestsDecodeFromTheWireSchema) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const Json request{{"cmds", Json::array({{{"fly_mode", {{"on", true}}}}})}};
  const auto decoded = protocol.decode(protocol.encode(kStructuredRequest, request, 0));
  const auto command =
      command_of(gm::parse_session_command(kStructuredRequest, decoded.at("message")));
  EXPECT_EQ(command.kind, gm::SessionCommandKind::free_movement);
  EXPECT_EQ(command.enabled, true);
}

TEST(GmTools, ComputesDestinations) {
  EXPECT_EQ(gm::meters_to_millimeters(1.2345), 1235);
  EXPECT_EQ(gm::meters_to_millimeters(-0.5), -500);

  const auto absolute = gm::absolute_destination(9012, 10.0, -20.5, 3.0, 45.0F);
  EXPECT_EQ(absolute.map_id, 9012);
  EXPECT_EQ(absolute.x_mm, 10000);
  EXPECT_EQ(absolute.y_mm, -20500);
  EXPECT_EQ(absolute.z_mm, 3000);
  EXPECT_FLOAT_EQ(absolute.yaw, 45.0F);

  const WorldPosition facing_x{9012, 1000, 2000, 3000, 0.0F};
  const auto up = gm::relative_destination(facing_x, gm::RelativeDirection::up, 10.0);
  ASSERT_TRUE(up);
  EXPECT_EQ(up->z_mm, 13000);
  EXPECT_EQ(up->x_mm, 1000);
  const auto forward = gm::relative_destination(facing_x, gm::RelativeDirection::forward, 5.0);
  ASSERT_TRUE(forward);
  EXPECT_EQ(forward->x_mm, 6000);
  EXPECT_EQ(forward->y_mm, 2000);

  const WorldPosition facing_y{9012, 0, 0, 0, 90.0F};
  const auto sideways = gm::relative_destination(facing_y, gm::RelativeDirection::forward, 5.0);
  ASSERT_TRUE(sideways);
  EXPECT_NEAR(sideways->x_mm, 0, 1);
  EXPECT_EQ(sideways->y_mm, 5000);

  const WorldPosition edge{1, 0, 0, static_cast<int>(gm::kMaximumCoordinateMeters * 1000), 0.0F};
  EXPECT_FALSE(gm::relative_destination(edge, gm::RelativeDirection::up, 10.0));

  const auto arrival = gm::beside(facing_x);
  EXPECT_EQ(arrival.x_mm, 2500);
  EXPECT_EQ(arrival.y_mm, 2000);

  EXPECT_EQ(gm::describe_position({9012, -795150, 712194, -698778, 158.56F}),
            "map 9012 x=-795.15 y=712.19 z=-698.78 yaw=158.6 (meters)");
}

TEST(GmTools, SpawnIdsArePrivateNpcIdsAndPositionsSurroundTheGm) {
  std::set<std::uint64_t> ids;
  for (std::uint32_t sequence = 0; sequence < 100; ++sequence) {
    const auto id = gm::spawned_npc_unit_id(7, sequence);
    EXPECT_EQ(id & 0xF, 0x2U);
    EXPECT_GE(id, 0xF0000000ULL);
    EXPECT_LE(id, 0xFFFFFFFFULL);
    ids.insert(id);
  }
  EXPECT_EQ(ids.size(), 100U);
  EXPECT_NE(gm::spawned_npc_unit_id(7, 0), gm::spawned_npc_unit_id(8, 0));

  const WorldPosition center{9012, 0, 0, 500, 0.0F};
  const auto positions = gm::spawn_positions(center, 4);
  ASSERT_EQ(positions.size(), 4U);
  for (const auto &position : positions) {
    const double distance = std::hypot(position.x_mm, position.y_mm);
    EXPECT_NEAR(distance, 3000.0, 2.0);
    EXPECT_EQ(position.z_mm, 500);
  }
  EXPECT_EQ(positions.front().x_mm, 3000);
  EXPECT_TRUE(gm::spawn_positions(center, 0).empty());
}

TEST(GmTools, SpawnAndRemovalPacketsEncode) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto unit_id = gm::spawned_npc_unit_id(3, 1);
  const auto added = protocol.decode(protocol.encode(
      10, gm::spawned_npc_message(unit_id, 1001007, {9012, 1, 2, 3, 90.0F}, "EX5Faction_Bandit")));
  const auto &message = added.at("message");
  EXPECT_EQ(message.at("unit_id"), std::to_string(unit_id));
  EXPECT_EQ(message.at("nub").at("npc").at("npc_cms_id"), 1001007);
  EXPECT_EQ(message.at("nub").at("npc").at("faction_type"), "EX5Faction_Bandit");
  EXPECT_EQ(message.at("nub").at("pos").at("millimeter_z"), 3);

  const auto removed =
      protocol.decode(protocol.encode(11, gm::removed_units_message({unit_id, unit_id + 16})));
  EXPECT_EQ(removed.at("message").at("unit_ids").size(), 2U);
}

TEST(GmTools, StructuredFollowRejectsAnotherFollowerAndZeroSelfPlacementIsAccepted) {
  EXPECT_FALSE(
      structured({{"follow_user", {{"char_id", "42"}, {"follower_char_id", "9"}}}}).command);
  const auto placement = structured({{"set_pos_and_yaw", {{"unit_id", 0}, {"pos", {1, 2, 3}}}}});
  ASSERT_TRUE(placement.command) << placement.error;
  EXPECT_TRUE(placement.command->has_coordinates);
  EXPECT_EQ(command_of(text("tp 1 2 3 90")).yaw, 90.0F);
}

TEST(GmTools, HelpPagesFitInChatAndNameTheEntryPoints) {
  for (int page = 1; page <= gm::kHelpPageCount; ++page) {
    const auto lines = gm::help_lines(page);
    ASSERT_GE(lines.size(), 5U) << page;
    for (const auto &line : lines) {
      EXPECT_LE(line.size(), 120U) << line;
    }
  }
  EXPECT_NE(gm::help_lines(1).at(1).find("/gm"), std::string::npos);
}

WorldPresenceSnapshot presence_snapshot(std::int64_t character_id, int x_mm, bool hidden) {
  WorldPresenceSnapshot snapshot;
  snapshot.character.id = character_id;
  snapshot.character.name = "Unit" + std::to_string(character_id);
  snapshot.position = WorldPosition{9012, x_mm, 0, 0, 0.0F};
  snapshot.hidden = hidden;
  return snapshot;
}

std::size_t events_for(const std::vector<WorldPresenceEvent> &events, std::uint64_t recipient,
                       WorldPresenceEventKind kind) {
  std::size_t count = 0;
  for (const auto &event : events) {
    if (event.recipient_connection_id == recipient && event.kind == kind) {
      ++count;
    }
  }
  return count;
}

TEST(GmTools, HiddenPresenceSeesOthersWithoutBeingAnnounced) {
  constexpr std::uint64_t kGm = 1;
  constexpr std::uint64_t kPlayer = 2;
  WorldPresence presence;
  std::vector<WorldPresenceEvent> events;
  presence.enter(kPlayer, presence_snapshot(20, 0, false), events);
  presence.enter(kGm, presence_snapshot(10, 1000, true), events);

  EXPECT_EQ(events_for(events, kGm, WorldPresenceEventKind::add), 1U);
  EXPECT_EQ(events_for(events, kPlayer, WorldPresenceEventKind::add), 0U);
  EXPECT_TRUE(presence.observers(kGm).empty());

  const auto moved = presence.move(kGm, {9012, 1500, 0, 0, 0.0F}, {{"mode", "MoveMode_Normal"}});
  EXPECT_EQ(events_for(moved, kPlayer, WorldPresenceEventKind::move), 0U);

  const auto revealed = presence.set_hidden(kGm, false);
  EXPECT_EQ(events_for(revealed, kPlayer, WorldPresenceEventKind::add), 1U);
  EXPECT_TRUE(presence.set_hidden(kGm, false).empty());
  EXPECT_EQ(presence.observers(kGm).size(), 1U);

  const auto hidden = presence.set_hidden(kGm, true);
  EXPECT_EQ(events_for(hidden, kPlayer, WorldPresenceEventKind::remove), 1U);
  EXPECT_TRUE(presence.leave(kGm).empty());
}

TEST(GmTools, ServerTeleportUsesTheNativeHandshakeWithoutEscapeCooldown) {
  constexpr std::uint64_t kUnit = (7ULL << 4) | 1ULL;
  PlayerEscape escape(EscapeRules{}, kUnit);
  const WorldPosition destination{9012, 10000, 20000, 30000, 90.0F};

  auto outcome = escape.request_server_teleport(destination, 1'000'000);
  EXPECT_EQ(outcome.refusal, EscapeRefusal::none);
  ASSERT_EQ(outcome.packets.size(), 1U);
  EXPECT_EQ(outcome.packets.front().opcode, 181);
  EXPECT_EQ(outcome.packets.front().message.at("pos").at("millimeter_x"), 10000);
  EXPECT_FALSE(escape.next_expiry_tick().has_value());
  EXPECT_EQ(escape.request_server_teleport(destination, 1'000'001).refusal,
            EscapeRefusal::teleport_pending);

  const auto tick = outcome.packets.front().message.at("tick");
  EscapeContext context;
  ASSERT_TRUE(escape.handles(20088, {{"tick", tick}}));
  escape.receive(20088, {{"tick", tick}}, context, 1'000'100);
  ASSERT_TRUE(escape.handles(20089, Json::object()));
  const auto completed = escape.receive(20089, Json::object(), context, 1'000'200);
  ASSERT_TRUE(completed.relocated_to.has_value());
  EXPECT_EQ(completed.relocated_to->z_mm, 30000);
  EXPECT_FALSE(escape.teleport_pending());
}

} // namespace
} // namespace chronicles
