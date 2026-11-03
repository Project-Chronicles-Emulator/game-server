#include "world/features/companion/tests/native_vehicle_test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace chronicles {
namespace {

using namespace native_testing;
using namespace vehicle_native_testing;

// Runs `!mount` / `!dismount` through the world listener, as the chat command does, and
// returns the reply line the player would see.
std::string run_player_mount_command(VehicleWorldFixture &fixture, std::size_t index,
                                     PlayerMountAction action) {
  auto completion = std::make_shared<std::promise<std::string>>();
  auto reply = completion->get_future();
  const bool queued = fixture.listener().player_mount(
      fixture.identity(index), action,
      [completion](std::string text) { completion->set_value(std::move(text)); });
  if (!queued) {
    ADD_FAILURE() << "The player mount command was not queued";
    return {};
  }
  if (reply.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    ADD_FAILURE() << "The player mount command did not complete";
    return {};
  }
  return reply.get();
}

TEST(NativeWorld, PlayerMountCommandsWorkForACharacterWithoutARegisteredHorse) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG and CHRONICLES_NPC_PLACEMENTS_9012";
  }
  VehicleWorldFixture fixture;
  OverworldEntry watcher_entry;
  OverworldEntry player_entry;
  fixture.start(0, watcher_entry);
  // Session 1 is not a game master and has no registered summon (only session 0 has one).
  fixture.start(1, player_entry);
  fixture.client(0).drain();
  fixture.client(1).drain();

  EXPECT_EQ(run_player_mount_command(fixture, 1, PlayerMountAction::mount),
            "Your horse is ready. Type !dismount to get off.");
  const auto mounted = fixture.client(1).drain();
  std::vector<nlohmann::json> mount_adds;
  for (const auto &add : frames_of(mounted, kUnitAddOpcode)) {
    if (add.at("nub").contains("npc") && add.at("nub").at("npc").contains("summon")) {
      mount_adds.push_back(add);
    }
  }
  ASSERT_EQ(mount_adds.size(), 1u);
  const auto mount_id = mount_adds.front().at("unit_id").get<std::string>();
  const auto seated = frames_of(mounted, 56);
  ASSERT_EQ(seated.size(), 1u);
  EXPECT_EQ(seated.front().at("unit_id"), mount_id);
  EXPECT_EQ(seated.front().at("passenger_unit_id"), std::to_string(fixture.player_unit(1)));
  EXPECT_EQ(seated.front().at("slot"), 0);
  EXPECT_EQ(frames_of(fixture.client(0).drain(), 56).size(), 1u);
  EXPECT_TRUE(fixture.progress(1).registered_summons.empty());

  EXPECT_NE(run_player_mount_command(fixture, 1, PlayerMountAction::mount).find("already riding"),
            std::string::npos);

  EXPECT_EQ(run_player_mount_command(fixture, 1, PlayerMountAction::dismount), "You got off.");
  const auto rider_view = fixture.client(1).drain();
  const auto watcher_view = fixture.client(0).drain();
  for (const auto &frames : {rider_view, watcher_view}) {
    const auto dismounts = frames_of(frames, 56);
    ASSERT_EQ(dismounts.size(), 1u);
    EXPECT_FALSE(dismounts.front().contains("slot"));
    EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_User");
    const auto removals = frames_of(frames, 11);
    ASSERT_EQ(removals.size(), 1u);
    EXPECT_EQ(removals.front().at("unit_ids"), nlohmann::json::array({mount_id}));
  }

  EXPECT_EQ(run_player_mount_command(fixture, 1, PlayerMountAction::dismount),
            "You are not riding anything.");
}

TEST(NativeWorld, RegionalHandoffKeepsTheSameHorseAndRemovesItFromOriginObservers) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG and CHRONICLES_NPC_PLACEMENTS_9012";
  }
  VehicleWorldFixture fixture(true);
  OverworldEntry rider_entry;
  OverworldEntry watcher_entry;
  fixture.start(0, rider_entry);
  fixture.start(1, watcher_entry);
  auto &rider = fixture.client(0);
  auto &watcher = fixture.client(1);
  const WorldPosition boundary{9012, -4615500, 994500, -600000, 0.0F};
  rider.move_to(boundary);
  watcher.move_to(boundary);
  rider.drain();
  watcher.drain();
  ASSERT_EQ(run_player_mount_command(fixture, 0, PlayerMountAction::mount),
            "Your horse is ready. Type !dismount to get off.");
  const auto mounted = rider.drain();
  const auto adds = frames_of(mounted, kUnitAddOpcode);
  ASSERT_EQ(adds.size(), 1U);
  const auto mount_id = adds.front().at("unit_id");
  watcher.drain();

  const auto moving = rider.send(20090, {{"pos", world_pos_json(boundary)}, {"body_yaw", 0.0}});
  ASSERT_EQ(frames_of(moving, 175).size(), 1U);
  EXPECT_EQ(frames_of(moving, 175).front().at("map_id"), 9021);
  const auto removals = frames_of(watcher.drain(), 11);
  EXPECT_TRUE(std::ranges::any_of(removals, [&](const auto &message) {
    return std::ranges::find(message.at("unit_ids"), mount_id) != message.at("unit_ids").end();
  }));

  ASSERT_EQ(frames_of(rider.send(20085, nlohmann::json::object()), 176).size(), 1U);
  const auto arrived = rider.send(20086, nlohmann::json::object());
  const auto reannounced = frames_of(arrived, kUnitAddOpcode);
  EXPECT_TRUE(std::ranges::any_of(reannounced, [&](const auto &message) {
    return message.at("unit_id") == mount_id;
  }));
  const auto seats = frames_of(arrived, 56);
  ASSERT_EQ(seats.size(), 1U);
  EXPECT_EQ(seats.front().at("unit_id"), mount_id);
  EXPECT_NE(run_player_mount_command(fixture, 0, PlayerMountAction::mount).find("already riding"),
            std::string::npos);
}

TEST(NativeWorld, SummonedHorseCarriesItsRiderAndNearbyPlayersSeeTheMountMove) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG and CHRONICLES_NPC_PLACEMENTS_9012";
  }
  VehicleWorldFixture fixture;
  OverworldEntry rider_entry;
  OverworldEntry watcher_entry;
  const auto rider_start = fixture.start(0, rider_entry);
  const auto summons = frames_of(rider_start, 137);
  ASSERT_EQ(summons.size(), 1u);
  EXPECT_EQ(summons.front().at("summon_npc_ids"), nlohmann::json::array({2001}));
  fixture.start(1, watcher_entry);
  fixture.client(1).drain();
  // The rider learns of the watcher's player unit first.
  fixture.client(0).drain();

  const auto summoned = fixture.client(0).send(20027, {{"summon_npc_cms_id", 2001}});

  ASSERT_EQ(frames_of(summoned, 102).size(), 1u);

  // Player presence adds can interleave; the mount is the add carrying a summon nub.
  std::vector<nlohmann::json> mount_adds;
  for (const auto &add : frames_of(summoned, kUnitAddOpcode)) {
    if (add.at("nub").contains("npc") && add.at("nub").at("npc").contains("summon")) {
      mount_adds.push_back(add);
    }
  }
  ASSERT_EQ(mount_adds.size(), 1u);
  const auto mount_id = mount_adds.front().at("unit_id").get<std::string>();
  EXPECT_EQ(std::stoull(mount_id) & 0xF, 2u);
  // The client resolves MountUnitNoti/MoveNoti targets by the id's low 32 bits.
  EXPECT_EQ(std::stoull(mount_id) >> 32, 0u);
  EXPECT_EQ(mount_adds.front().at("nub").at("npc").at("mount").at("passengers"),
            nlohmann::json::array({std::to_string(fixture.player_unit(0))}));
  const auto seated = frames_of(summoned, 56);
  ASSERT_EQ(seated.size(), 1u);
  EXPECT_EQ(seated.front().at("passenger_unit_id"), std::to_string(fixture.player_unit(0)));
  EXPECT_EQ(seated.front().at("slot"), 0);
  EXPECT_LT(first_index(summoned, 102), first_index(summoned, 56));


  const auto watcher_view = fixture.client(1).drain();
  EXPECT_FALSE(frames_of(watcher_view, kUnitAddOpcode).empty());
  EXPECT_EQ(frames_of(watcher_view, 56).size(), 1u);

  auto ahead = kNearIleste;
  ahead.x_mm += 3000;
  fixture.client(0).move_to(ahead);
  const auto watcher_moves = frames_of(fixture.client(1).drain(), 177);
  ASSERT_EQ(watcher_moves.size(), 1u);
  EXPECT_EQ(watcher_moves.front().at("unit_id"), mount_id);

  fixture.client(0).send(20031, nlohmann::json::object());
  const auto watcher_dismount = fixture.client(1).drain();
  const auto dismounts = frames_of(watcher_dismount, 56);
  ASSERT_EQ(dismounts.size(), 1u);
  EXPECT_FALSE(dismounts.front().contains("slot"));
  EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_User");
  const auto removals = frames_of(watcher_dismount, 11);
  ASSERT_EQ(removals.size(), 1u);
  EXPECT_EQ(removals.front().at("unit_ids"), nlohmann::json::array({mount_id}));
}

TEST(NativeWorld, RiderDeathUnseatsTheHorseForRiderAndWatcher) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG and CHRONICLES_NPC_PLACEMENTS_9012";
  }
  VehicleWorldFixture fixture(true);
  fixture.make_game_master(0);
  OverworldEntry rider_entry;
  OverworldEntry watcher_entry;
  fixture.start(0, rider_entry);
  fixture.start(1, watcher_entry);
  fixture.client(0).send(20027, {{"summon_npc_cms_id", 2001}});
  fixture.client(1).drain();

  const auto rider_frames = fixture.client(0).send(20009, {{"text", "suicide"}});
  ASSERT_TRUE(fixture.progress(0).dead);
  const auto watcher_frames = fixture.client(1).drain();
  for (const auto &frames : {rider_frames, watcher_frames}) {
    const auto dismounts = frames_of(frames, 56);
    ASSERT_EQ(dismounts.size(), 1u);
    EXPECT_FALSE(dismounts.front().contains("slot"));
    EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_RiderKilled");
    EXPECT_LT(first_index(frames, 56), first_index(frames, 11));
  }
  EXPECT_TRUE(frames_of(fixture.client(0).send(20003, nlohmann::json::object()), 56).empty());
}

TEST(NativeWorld, GmSpawnedWagonIsBoardedDrivenWatchedAndLeft) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG and CHRONICLES_NPC_PLACEMENTS_9012";
  }
  VehicleWorldFixture fixture;
  fixture.make_game_master(0);
  OverworldEntry driver_entry;
  OverworldEntry watcher_entry;
  fixture.start(0, driver_entry);
  fixture.start(1, watcher_entry);
  fixture.client(1).drain();
  fixture.client(0).drain();
  auto &driver = fixture.client(0);

  // The live report: a GM-spawned wagon appears, but boarding it did nothing.
  const auto spawned = driver.send(20009, {{"text", "spawn_operable 200"}});
  std::vector<nlohmann::json> wagon_adds;
  for (const auto &add : frames_of(spawned, kUnitAddOpcode)) {
    if (add.at("nub").contains("operable")) {
      wagon_adds.push_back(add);
    }
  }
  ASSERT_EQ(wagon_adds.size(), 1u);
  const auto wagon_id = wagon_adds.front().at("unit_id").get<std::string>();
  EXPECT_EQ(std::stoull(wagon_id) & 0xF, 4u);
  // The client's MountUnitNoti and MoveNoti dispatchers look units up by their low 32 bits.
  EXPECT_EQ(std::stoull(wagon_id) >> 32, 0u);
  EXPECT_EQ(frames_of(fixture.client(1).drain(), kUnitAddOpcode).size(), 1u);

  const auto boarded = driver.send(20030, {{"mount_unit_id", wagon_id}, {"slot", 0}});
  const auto ride_replies = frames_of(boarded, 103);
  ASSERT_EQ(ride_replies.size(), 1u);
  EXPECT_FALSE(ride_replies.front().contains("ec"));
  const auto seated = frames_of(boarded, 56);
  ASSERT_EQ(seated.size(), 1u);
  EXPECT_EQ(seated.front().at("unit_id"), wagon_id);
  EXPECT_EQ(seated.front().at("passenger_unit_id"), std::to_string(fixture.player_unit(0)));
  EXPECT_EQ(seated.front().at("slot"), 0);
  EXPECT_LT(first_index(boarded, 103), first_index(boarded, 56));
  EXPECT_EQ(frames_of(fixture.client(1).drain(), 56).size(), 1u);

  // A second boarding attempt while seated is refused without a new seat.
  const auto again = driver.send(20030, {{"mount_unit_id", wagon_id}, {"slot", 0}});
  ASSERT_EQ(frames_of(again, 103).size(), 1u);
  EXPECT_EQ(frames_of(again, 103).front().at("ec"), "Summon_AlreadyRiding");
  EXPECT_TRUE(frames_of(again, 56).empty());

  // The driver's movement is the wagon's movement for everyone else.
  auto ahead = kNearIleste;
  ahead.x_mm += 4000;
  driver.move_to(ahead);
  const auto watcher_moves = frames_of(fixture.client(1).drain(), 177);
  ASSERT_EQ(watcher_moves.size(), 1u);
  EXPECT_EQ(watcher_moves.front().at("unit_id"), wagon_id);

  auto point = ahead;
  point.y_mm += 2000;
  driver.send(20031, {{"dismount_point", {{"pos", world_pos_json(point)}, {"yaw", 0.0}}}});
  const auto dismounts = frames_of(fixture.client(1).drain(), 56);
  ASSERT_EQ(dismounts.size(), 1u);
  EXPECT_EQ(dismounts.front().at("unit_id"), wagon_id);
  EXPECT_FALSE(dismounts.front().contains("slot"));
  EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_User");

  // The wagon stays where its driver left it and can be boarded again.
  const auto reboarded = driver.send(20030, {{"mount_unit_id", wagon_id}, {"slot", 0}});
  ASSERT_EQ(frames_of(reboarded, 103).size(), 1u);
  EXPECT_FALSE(frames_of(reboarded, 103).front().contains("ec"));
  EXPECT_EQ(frames_of(reboarded, 56).size(), 1u);
}

} // namespace
} // namespace chronicles
