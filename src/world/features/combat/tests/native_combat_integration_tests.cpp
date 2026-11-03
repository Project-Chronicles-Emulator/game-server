#include "world/features/combat/tests/native_combat_test_support.hpp"
#include "world/features/gathering/gathering.hpp"
#include "world/features/loot/loot.hpp"
#include "world/features/trade/tests/pausing_region_checkpoint_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <thread>
#include <tuple>

namespace chronicles {
namespace {

using namespace native_testing;
using namespace combat_native_testing;

TEST(NativeWorld, DungeonMonsterAnnouncementsFollowStartPlaying) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  const auto *tutorial_path = std::getenv("CHRONICLES_CLIFFS_TUTORIAL_PROFILE");
  if (!catalog_path || !tutorial_path) {
    GTEST_SKIP() << "Requires generated world monster, travel and tutorial profiles";
  }

  const std::filesystem::path root(catalog_path);
  std::ifstream input(root.parent_path() / "world-mobs/definitions.json");
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(profile.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == 10111;
  });
  ASSERT_NE(map, profile.at("maps").end());
  ASSERT_FALSE(map->at("spawns").empty());
  const auto &spawn = map->at("spawns").front();
  const auto &point = spawn.at("position");
  const WorldPosition position{10111, point.at(0), point.at(1), point.at(2), 0};
  NpcPlacementSet empty_population;
  empty_population.map_id = 10111;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  auto catalog = std::make_shared<ClientCatalog>(root);
  OverworldSessions world(population, {position}, catalog, root, TutorialData::load(tutorial_path),
                          CharacterStats::load(*catalog));
  auto &session = world.sessions.front();
  ASSERT_EQ(world.repositories.characters->characters(session.account_id, 1).front().world_position,
            position);
  const auto entry = world.enter(session);
  ASSERT_EQ(entry.map_start.at("map_id"), 10111);
  const auto is_monster = [&](const nlohmann::json &add) {
    return add.at("nub").contains("npc") &&
           add.at("nub").at("npc").at("npc_cms_id") == spawn.at("cms_id");
  };
  EXPECT_FALSE(std::ranges::any_of(entry.unit_adds, is_monster));

  VehicleClient client(world, session, 3);
  const auto started = client.send(20086, nlohmann::json::object());
  const auto adds = frames_of(started, kUnitAddOpcode);
  EXPECT_TRUE(std::ranges::any_of(adds, is_monster));
  const auto response = std::ranges::find(started, 183, &DecodedWorldFrame::opcode);
  const auto monster = std::ranges::find_if(started, [&](const DecodedWorldFrame &frame) {
    return frame.opcode == kUnitAddOpcode && is_monster(frame.message);
  });
  ASSERT_NE(response, started.end());
  ASSERT_NE(monster, started.end());
  EXPECT_LT(response, monster);
}

void verify_shared_world_monster_spawns_and_rewards(bool enable_gathering) {
  const auto catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  const auto profile_path = catalog_path.parent_path() / "world-mobs" / "definitions.json";
  if (!std::filesystem::exists(profile_path)) {
    GTEST_SKIP() << "Build the world monster profile first";
  }
  std::ifstream input(profile_path);
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(
      profile.at("maps"), [](const auto &entry) { return entry.at("map_id") == 9012; });
  ASSERT_NE(map, profile.at("maps").end());
  ASSERT_FALSE(map->at("spawns").empty());
  const auto loot_path = catalog_path.parent_path() / "loot" / "definitions.json";
  if (!std::filesystem::exists(loot_path)) {
    GTEST_SKIP() << "Build the monster loot profile first";
  }
  const auto loot = LootData::load(loot_path);
  const auto selected_spawn = std::ranges::find_if(map->at("spawns"), [&](const auto &entry) {
    const auto *npc = loot->npc(entry.at("cms_id"));
    return npc && npc->kill_gold > 0 && !entry.contains("patrol") &&
           entry.at("faction") != "EX5Faction_Player";
  });
  ASSERT_NE(selected_spawn, map->at("spawns").end());
  const auto &spawn = *selected_spawn;
  const auto &point = spawn.at("position");
  const WorldPosition position{9012, point.at(0), point.at(1), point.at(2), 0};
  NpcPlacementSet empty_population;
  empty_population.map_id = 9012;
  const auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  const auto stats = CharacterStats::load(*catalog);
  std::shared_ptr<const GatheringData> gathering;
  if (enable_gathering) {
    const auto gathering_path = catalog_path.parent_path() / "gathering" / "definitions.json";
    ASSERT_TRUE(std::filesystem::exists(gathering_path));
    gathering = GatheringData::load(gathering_path);
    ASSERT_TRUE(gathering->has_map(position.map_id));
  }

  OverworldSessions world(population, {position, position}, catalog, catalog_path, {}, stats, loot,
                           gathering);
  ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
  // Login again so the GM role is in the selected session's authenticated handoff.
  const auto login = world.auth->login("overworld_sessions0", "test-world-password-123");
  world.sessions[0].access_token = login.access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);
  const auto first = world.enter(world.sessions[0]);
  const auto second = world.enter(world.sessions[1]);
  const auto monster = std::ranges::find_if(first.unit_adds, [&](const auto &add) {
    return add.at("nub").contains("npc") &&
           add.at("nub").at("npc").at("npc_cms_id") == spawn.at("cms_id") &&
           add.at("nub").at("pos").at("millimeter_x") == point.at(0) &&
           add.at("nub").at("pos").at("millimeter_y") == point.at(1) &&
           add.at("nub").at("pos").at("millimeter_z") == point.at(2);
  });
  ASSERT_NE(monster, first.unit_adds.end());
  const auto id = monster->at("unit_id").get<std::string>();
  EXPECT_TRUE(std::ranges::any_of(second.unit_adds,
                                  [&](const auto &add) { return add.at("unit_id") == id; }));
  VehicleClient killer(world, world.sessions[0], 3);
  VehicleClient watcher(world, world.sessions[1], 3);
  killer.send(20086, nlohmann::json::object());
  watcher.send(20086, nlohmann::json::object());
  const auto killed = killer.send(20009, {{"text", "monster_kill " + id}});
  EXPECT_FALSE(frames_of(killed, 23).empty());
  WorldProgress after;
  for (int poll = 0; poll < 50; ++poll) {
    after = world.repositories.character_state->load_world_progress(world.sessions[0].account_id,
                                             world.sessions[0].character_id, 1);
    if (after.experience > 0) {
      break;
    }
    killer.drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(after.experience, 0);
  bool observed = false;
  for (int poll = 0; poll < 50 && !observed; ++poll) {
    const auto packets = watcher.drain();
    observed = std::ranges::any_of(frames_of(packets, 23), [&](const auto &death) {
      return death.at("target_unit_id") == id;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(observed);
  const auto refused = watcher.send(20079, {{"unit_id", id}});
  ASSERT_EQ(frames_of(refused, 26).size(), 1u);
  EXPECT_NE(frames_of(refused, 26).front().value("ec", "Success"), "Success");
  const auto taken = killer.send(20079, {{"unit_id", id}});
  ASSERT_EQ(frames_of(taken, 26).size(), 1u);
  EXPECT_EQ(frames_of(taken, 26).front().value("ec", "Success"), "Success");
  const auto rewarded = world.repositories.character_state->load_world_progress(
      world.sessions[0].account_id, world.sessions[0].character_id, 1);
  EXPECT_GT(rewarded.gold, 0);
  killer.send(20079, {{"unit_id", id}});
  const auto repeated = world.repositories.character_state->load_world_progress(
      world.sessions[0].account_id, world.sessions[0].character_id, 1);
  EXPECT_EQ(repeated.gold, rewarded.gold);
}

TEST(NativeWorld, ReconnectRestoresLifeStateAndReportsDeathOnceAfterStartPlaying) {
  for (const bool dead : {false, true}) {
    SCOPED_TRACE(dead ? "saved dead character" : "living character");
    Protocol protocol(CHRONICLES_PROTOCOL_DIR);
    WorldHandshake handshake(protocol, true);
    Character character;
    character.id = 6;
    character.class_id = 5;
    character.level = 1;
    character.name = "Reconnect";
    character.world_position = WorldPosition{9012, 1000, 2000, 3000, 0.0F};
    WorldProgress progress;
    progress.dead = dead;
    progress.revision = 7;
    ASSERT_TRUE(handshake.configure_bootstrap_character(character, progress, 9012));

    const auto drain = [&] {
      std::vector<nlohmann::json> packets;
      while (const auto frame = handshake.take_pending_notification()) {
        packets.push_back(protocol.decode(*frame));
      }
      return packets;
    };
    ASSERT_TRUE(handshake.receive(protocol.encode(20001, nlohmann::json::object(), 0), 100, 100));
    drain();
    const auto loaded =
        handshake.receive(protocol.encode(20085, nlohmann::json::object(), 1), 200, 200);
    ASSERT_TRUE(loaded);
    const auto entry = protocol.decode(*loaded).at("message");
    EXPECT_EQ(entry.at("nub").at("player").at("state"),
              dead ? "EX5UnitStateType_Demise" : "EX5UnitStateType_Idle");
    const auto before_playing = drain();
    EXPECT_FALSE(std::ranges::any_of(before_playing,
                                     [](const auto &packet) { return packet.at("opcode") == 23; }));

    ASSERT_TRUE(handshake.receive(protocol.encode(20086, nlohmann::json::object(), 2), 300, 300));
    const auto after_playing = drain();
    std::vector<int> death_opcodes;
    for (const auto &packet : after_playing) {
      const int opcode = packet.at("opcode");
      if (opcode == 32 || opcode == 23) {
        death_opcodes.push_back(opcode);
        const auto &message = packet.at("message");
        const char *target = opcode == 32 ? "unit_id" : "target_unit_id";
        EXPECT_EQ(message.at(target), std::to_string(player_wire_unit_id(character.id)));
      }
      EXPECT_NE(opcode, 19) << "Reconnect must not replay damage";
      EXPECT_NE(opcode, 169) << "Reconnect must not repeat durability loss";
    }
    EXPECT_EQ(death_opcodes, dead ? std::vector<int>({32, 23}) : std::vector<int>());
    EXPECT_FALSE(handshake.receive(protocol.encode(20086, nlohmann::json::object(), 3), 400, 400));
    EXPECT_TRUE(drain().empty());

    const auto snapshot = handshake.presence_snapshot();
    ASSERT_TRUE(snapshot);
    WorldPresence presence;
    std::vector<WorldPresenceEvent> events;
    presence.enter(1, *snapshot, events);
    auto observer = *snapshot;
    observer.character.id = 9;
    presence.enter(2, observer, events);
    const auto seen = std::ranges::find_if(events, [](const auto &event) {
      return event.recipient_connection_id == 2 && event.kind == WorldPresenceEventKind::add;
    });
    ASSERT_NE(seen, events.end());
    EXPECT_EQ(seen->message.at("nub").at("player").at("state"),
              dead ? "EX5UnitStateType_Demise" : "EX5UnitStateType_Idle");
  }
}

TEST(NativeWorld, LateObserversSeeTheLatestDeathAndRevivalState) {
  Character character;
  character.id = 6;
  character.class_id = 5;
  character.name = "LifeState";
  WorldPresence presence;
  std::vector<WorldPresenceEvent> events;
  presence.enter(1, {character, {9012, 1000, 2000, 3000, 0.0F}}, events);

  std::int64_t observer_id = 10;
  for (const bool dead : {true, false}) {
    SCOPED_TRACE(dead ? "after death" : "after revival");
    const nlohmann::json stats{{"69", dead ? 0 : 100}};
    presence.refresh_player_life(1, dead, stats);
    auto observer = character;
    observer.id = observer_id++;
    events.clear();
    presence.enter(2, {observer, {9012, 1000, 2000, 3000, 0.0F}}, events);
    const auto seen = std::ranges::find_if(events, [](const auto &event) {
      return event.recipient_connection_id == 2 && event.kind == WorldPresenceEventKind::add;
    });
    ASSERT_NE(seen, events.end());
    const auto &player = seen->message.at("nub").at("player");
    EXPECT_EQ(player.at("state"), dead ? "EX5UnitStateType_Demise" : "EX5UnitStateType_Idle");
    EXPECT_EQ(player.at("stats").at("69"), dead ? 0 : 100);
    presence.leave(2);
  }
}

TEST(NativeWorld, InPlaceReviveIsDurableRepeatableAndIgnoredForALivingOrRepeatRequest) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the profile with overlap groups";
  }
  DeathFixture fixture(profile_path);
  auto session = fixture.connect();
  const auto bootstrap_revision = fixture.progress().revision;

  // A living player's 20039 changes nothing: no revival frame and no persisted change.
  fixture.send(session, 20039, nlohmann::json::object());
  EXPECT_FALSE(DeathFixture::contains_revival_frame(fixture.frames_until_ping(session)));
  EXPECT_FALSE(fixture.progress().dead);
  EXPECT_EQ(fixture.progress().revision, bootstrap_revision);

  // Death through the real fight is persisted dead; the in-place revive commits dead=false
  // first and then sends exactly 49, 51, 47, 32.
  fixture.await_player_death(session);
  const auto dead_progress = fixture.progress();
  EXPECT_TRUE(dead_progress.dead);
  EXPECT_EQ(fixture.revive_and_collect_revival_opcodes(session), (std::vector<int>{49, 51, 47, 32}));
  const auto revived_progress = fixture.progress();
  EXPECT_FALSE(revived_progress.dead);
  EXPECT_EQ(revived_progress.revision, dead_progress.revision + 1);

  // A duplicate request is a no-op: no frame and no further commit.
  fixture.send(session, 20039, nlohmann::json::object());
  EXPECT_FALSE(DeathFixture::contains_revival_frame(fixture.frames_until_ping(session)));
  EXPECT_EQ(fixture.progress().revision, revived_progress.revision);

  // An immediate reconnect reloads from the store and the character is still alive.
  session.client->disconnect();
  auto reconnected = fixture.connect();
  EXPECT_FALSE(fixture.progress().dead);
  fixture.send(reconnected, 20039, nlohmann::json::object());
  EXPECT_FALSE(DeathFixture::contains_revival_frame(fixture.frames_until_ping(reconnected)));

  // A second death and a second revive work the same way.
  fixture.await_player_death(reconnected);
  EXPECT_TRUE(fixture.progress().dead);
  EXPECT_EQ(fixture.revive_and_collect_revival_opcodes(reconnected),
            (std::vector<int>{49, 51, 47, 32}));
  EXPECT_FALSE(fixture.progress().dead);
  reconnected.client->disconnect();
}

TEST(NativeWorld, InPlaceReviveWithARevisionConflictSendsNoSuccessAndLeavesThePlayerDead) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the profile with overlap groups";
  }
  DeathFixture fixture(profile_path);
  auto session = fixture.connect();
  fixture.await_player_death(session);
  ASSERT_TRUE(fixture.progress().dead);

  // Another writer advances the revision behind the session, so the guarded revive commit fails.
  // The persisted position exists, so the position fallback cannot be the reason for a refusal:
  // the first test shows the same sequence without the bump revives, so this refusal is the
  // revision guard. The refused request must not end the session.
  ASSERT_TRUE(fixture.bump_revision_behind_the_session());
  const auto bumped_progress = fixture.progress();
  ASSERT_TRUE(bumped_progress.dead);
  fixture.send(session, 20039, nlohmann::json::object());

  const auto frames = fixture.frames_until_ping(session);
  EXPECT_FALSE(DeathFixture::contains_revival_frame(frames));
  const auto progress = fixture.progress();
  EXPECT_TRUE(progress.dead);
  EXPECT_EQ(progress.revision, bumped_progress.revision);
  session.client->disconnect();
}

TEST(NativeWorld, NpcPursuitMoveNotificationsReachTheClientSocketInRunningShape) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the profile with overlap groups";
  }
  constexpr int kStartDistanceMm = 2950;
  DeathFixture fixture(profile_path, 0, kStartDistanceMm);
  auto session = fixture.connect();
  std::vector<nlohmann::json> seen;
  fixture.read_until(session, 14, &seen);

  const std::string player_text =
      std::to_string(player_wire_unit_id(fixture.character_id()));
  std::int64_t previous_tick = 0;
  std::size_t run_frames = 0;
  std::size_t stop_frames = 0;
  bool run_after_stop = false;
  for (const auto &frame : seen) {
    // No skill is active before the first 14. Pursuit must not emit skill-only 64/106.
    EXPECT_NE(frame.at("opcode"), 64);
    EXPECT_NE(frame.at("opcode"), 106);
    if (frame.at("opcode") != 177) {
      continue;
    }
    const auto &message = frame.at("message");
    if (message.at("unit_id").get<std::string>() == player_text) {
      continue;
    }
    const auto tick = std::stoll(message.at("tick").get<std::string>());
    EXPECT_GT(tick, previous_tick) << "ticks of the NPC strictly increase (nanoseconds)";
    previous_tick = tick;
    // The decoded JSON omits default-valued fields: MoveMode_Normal and MoveDetail_Stop are the
    // zero values of their enums (p_common.proto) and a zero coordinate is absent too.
    EXPECT_EQ(message.value("mode", "MoveMode_Normal"), "MoveMode_Normal");
    const auto detail = message.value("detail", "MoveDetail_Stop");
    if (detail == "MoveDetail_Run") {
      ++run_frames;
      run_after_stop = run_after_stop || stop_frames > 0;
      EXPECT_GE(message.at("pred_milli").get<int>(), 16);
      EXPECT_LE(message.at("pred_milli").get<int>(), 900);
      ASSERT_TRUE(message.contains("pred_pos"));
      // The fixture is planar (the player stands at the monster's z), so the route's predicted z
      // equals the NPC's current z. An absent coordinate is the descriptor default 0 on both.
      const auto goal_z = message.at("pred_pos").value("millimeter_z", 0);
      const auto position_z = message.at("pos").value("millimeter_z", 0);
      EXPECT_EQ(goal_z, position_z) << "the goal z is the route's predicted z, exactly";
      EXPECT_FALSE(message.contains("velocity"));
    } else {
      EXPECT_EQ(detail, "MoveDetail_Stop");
      EXPECT_FALSE(message.contains("pred_pos")) << "a Stop carries no goal";
      ++stop_frames;
    }
  }
  EXPECT_GE(run_frames, 1u) << "the pursuing NPC was announced running off the socket";
  EXPECT_EQ(stop_frames, 1u) << "exactly one Stop, before the attack";
  EXPECT_FALSE(run_after_stop);
}

TEST(NativeWorld, WorldMonstersAreSharedAcrossTlsClientsAndKilledByServerCombat) {
  verify_shared_world_monster_spawns_and_rewards(false);
}

TEST(NativeWorld, NearbyPartyMemberReceivesTheSameMonsterQuestCreditOverTls) {
  constexpr int kMonsterCmsId = 5001535;
  constexpr int kHuntQuestId = 10000127;
  constexpr int kKillRecordId = 524;
  constexpr int kPartyUnlockQuestId = 90100011;
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }

  const std::filesystem::path catalog_path(catalog_environment);
  std::ifstream input(catalog_path.parent_path() / "world-mobs/definitions.json");
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(profile.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == 9012;
  });
  ASSERT_NE(map, profile.at("maps").end());
  const auto spawn = std::ranges::find_if(map->at("spawns"), [](const auto &entry) {
    return entry.at("cms_id") == kMonsterCmsId;
  });
  ASSERT_NE(spawn, map->at("spawns").end());
  const auto &point = spawn->at("position");
  const WorldPosition position{9012, point.at(0), point.at(1), point.at(2), 0};
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  NpcPlacementSet empty_population;
  empty_population.map_id = position.map_id;
  const auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  OverworldSessions world(population, {position, position, position}, catalog, catalog_path, {},
                          CharacterStats::load(*catalog));

  for (const auto &session : world.sessions) {
    const auto before = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    auto after = before;
    ++after.revision;
    after.quests[kPartyUnlockQuestId].complete = true;
    after.quests[kHuntQuestId] = QuestProgress{};
    ASSERT_TRUE(world.repositories.character_state->save_world_progress(
        session.account_id, session.character_id, 1, before, after));
  }

  ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
  const auto login = world.auth->login("overworld_sessions0", "test-world-password-123");
  world.sessions[0].access_token = login.access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);

  const auto entry = world.enter(world.sessions[0]);
  world.enter(world.sessions[1]);
  world.enter(world.sessions[2]);
  const auto monster = std::ranges::find_if(entry.unit_adds, [&](const auto &add) {
    return add.at("nub").contains("npc") &&
           add.at("nub").at("npc").at("npc_cms_id") == kMonsterCmsId;
  });
  ASSERT_NE(monster, entry.unit_adds.end()) << "map=" << entry.map_start.at("map_id")
                                            << " unit adds=" << entry.unit_adds.size();
  const auto monster_id = monster->at("unit_id").get<std::string>();

  VehicleClient killer(world, world.sessions[0], 3);
  VehicleClient member(world, world.sessions[1], 3);
  VehicleClient outsider(world, world.sessions[2], 3);
  killer.send(20086, nlohmann::json::object());
  member.send(20086, nlohmann::json::object());
  outsider.send(20086, nlohmann::json::object());
  const auto killer_id = std::to_string(world.sessions[0].character_id);
  const auto member_id = std::to_string(world.sessions[1].character_id);
  killer.send(20097, {{"target_char_id", member_id}});
  std::vector<nlohmann::json> invitations;
  for (int attempt = 0; attempt < 50 && invitations.empty(); ++attempt) {
    invitations = frames_of(member.drain(), 188);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(invitations.size(), 1U);
  const auto party_id = invitations.front().at("party_id").get<std::string>();
  const auto joined = member.send(20098, {{"leader_char_id", killer_id},
                                          {"target_char_id", member_id},
                                          {"party_id", party_id}});
  ASSERT_FALSE(frames_of(joined, 184).empty());

  const auto killed = killer.send(20009, {{"text", "monster_kill " + monster_id}});
  ASSERT_FALSE(frames_of(killed, 23).empty());
  WorldProgress member_progress;
  for (int attempt = 0; attempt < 50; ++attempt) {
    member_progress = world.repositories.character_state->load_world_progress(
        world.sessions[1].account_id, world.sessions[1].character_id, 1);
    if (member_progress.play_records.contains(kKillRecordId)) {
      break;
    }
    member.drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(member_progress.play_records.contains(kKillRecordId));
  EXPECT_EQ(member_progress.play_records.at(kKillRecordId), 1);
  EXPECT_EQ(member_progress.quests.at(kHuntQuestId).objectives.at(1), 1);
  EXPECT_EQ(member_progress.experience, 0);
  const auto other_progress = world.repositories.character_state->load_world_progress(
      world.sessions[2].account_id, world.sessions[2].character_id, 1);
  EXPECT_FALSE(other_progress.play_records.contains(kKillRecordId));
  EXPECT_FALSE(other_progress.quests.at(kHuntQuestId).objectives.contains(1));
}

TEST(NativeWorld, ReconnectWaitsForPendingMonsterRewardAndRetainsItsTicket) {
  constexpr int kMonsterCmsId = 5001535;
  constexpr int kHuntQuestId = 10000127;
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }

  const std::filesystem::path catalog_path(catalog_environment);
  std::ifstream input(catalog_path.parent_path() / "world-mobs/definitions.json");
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(profile.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == 9012;
  });
  ASSERT_NE(map, profile.at("maps").end());
  const auto spawn = std::ranges::find_if(map->at("spawns"), [](const auto &entry) {
    return entry.at("cms_id") == kMonsterCmsId;
  });
  ASSERT_NE(spawn, map->at("spawns").end());
  const auto &point = spawn->at("position");
  const WorldPosition position{9012, point.at(0), point.at(1), point.at(2), 0};
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  NpcPlacementSet placements;
  placements.map_id = position.map_id;
  const auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  std::shared_ptr<PausingRegionCheckpointStore> store;
  OverworldSessions world(
      population, {position}, catalog, catalog_path, {}, CharacterStats::load(*catalog), {}, {}, {},
      [&](std::shared_ptr<CharacterStateRepository> inner) {
        store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
        return store;
      });
  auto &session = world.sessions.front();
  auto before = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  auto after = before;
  ++after.revision;
  after.quests[kHuntQuestId] = QuestProgress{};
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      session.account_id, session.character_id, 1, before, after));
  ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
  const auto login = world.auth->login("overworld_sessions0", "test-world-password-123");
  session.access_token = login.access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);

  const auto entry = world.enter(session);
  const auto monster = std::ranges::find_if(entry.unit_adds, [&](const auto &add) {
    return add.at("nub").contains("npc") &&
           add.at("nub").at("npc").at("npc_cms_id") == kMonsterCmsId;
  });
  ASSERT_NE(monster, entry.unit_adds.end());
  VehicleClient killer(world, session, 3);
  killer.send(20086, nlohmann::json::object());
  killer.move_to(position);
  store->refuse_monster_rewards();
  killer.send(20009, {{"text", "monster_kill " + monster->at("unit_id").get<std::string>()}});
  ASSERT_TRUE(store->wait_for_refused_monster_reward());

  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto selected = world.auth->select(session.access_token, select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  store->observe_next_position_checkpoint();
  ASSERT_EQ(world.server->listener().disconnect_account(session.account_id), 1U);
  ASSERT_TRUE(store->wait_for_position_checkpoint());
  // The checkpoint completes before the close cleanup releases admission. Repeated attempts
  // allow that worker to finish and prove the lease remains held by the unresolved defeat.
  for (int attempt = 0; attempt < 10; ++attempt) {
    WorldTestClient busy(world.server->port());
    busy.send_key(selected.packet_key());
    EXPECT_THROW(busy.read(), boost::system::system_error);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  store->lose_next_ready_acknowledgement();
  store->allow_monster_rewards();
  ASSERT_TRUE(store->wait_for_lost_ready_acknowledgement());
  {
    WorldTestClient busy(world.server->port());
    busy.send_key(selected.packet_key());
    EXPECT_THROW(busy.read(), boost::system::system_error);
  }
  std::unique_ptr<WorldTestClient> reconnected;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    auto attempt = std::make_unique<WorldTestClient>(world.server->port());
    attempt->send_key(selected.packet_key());
    try {
      const auto response = attempt->read();
      ASSERT_EQ(response.opcode(), 5);
      reconnected = std::move(attempt);
      break;
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(reconnected) << "The same ticket was not accepted after the reward settled";
  session.client = std::move(reconnected);
  world.enter_after_key_exchange(session);
  VehicleClient resumed(world, session, 3);
  resumed.send(20086, nlohmann::json::object());
  const auto progress = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  EXPECT_EQ(progress.quests.at(kHuntQuestId).objectives.at(1), 1);
}

TEST(NativeWorld, LostRecipientRewardAckSurvivesDisconnectAndUnblocksAnotherKill) {
  constexpr int kFirstMonster = 5001535;
  constexpr int kSecondMonster = 5001247;
  constexpr int kHuntQuest = 10000127;
  constexpr int kPartyUnlockQuest = 90100011;
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  std::ifstream input(catalog_path.parent_path() / "world-mobs/definitions.json");
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(profile.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == 9012;
  });
  ASSERT_NE(map, profile.at("maps").end());
  const auto first_spawn = std::ranges::find_if(map->at("spawns"), [](const auto &entry) {
    return entry.at("cms_id") == kFirstMonster;
  });
  ASSERT_NE(first_spawn, map->at("spawns").end());
  const auto second_spawn = std::ranges::find_if(map->at("spawns"), [](const auto &entry) {
    return entry.at("cms_id") == kSecondMonster && entry.at("unit_id") == "576460752303427122";
  });
  ASSERT_NE(second_spawn, map->at("spawns").end());
  const auto &first_point = first_spawn->at("position");
  const auto &second_point = second_spawn->at("position");
  const WorldPosition position{9012,
                               (first_point.at(0).get<int>() + second_point.at(0).get<int>()) / 2,
                               (first_point.at(1).get<int>() + second_point.at(1).get<int>()) / 2,
                               (first_point.at(2).get<int>() + second_point.at(2).get<int>()) / 2,
                               0};
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  NpcPlacementSet placements;
  placements.map_id = position.map_id;
  const auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  std::shared_ptr<PausingRegionCheckpointStore> store;
  OverworldSessions world(
      population, {position, position}, catalog, catalog_path, {}, CharacterStats::load(*catalog),
      {}, {}, {}, [&](std::shared_ptr<CharacterStateRepository> inner) {
        store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
        return store;
      });
  for (const auto &session : world.sessions) {
    const auto before = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    auto after = before;
    ++after.revision;
    after.quests[kPartyUnlockQuest].complete = true;
    after.quests[kHuntQuest] = QuestProgress{};
    ASSERT_TRUE(world.repositories.character_state->save_world_progress(
        session.account_id, session.character_id, 1, before, after));
  }
  ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
  const auto login = world.auth->login("overworld_sessions0", "test-world-password-123");
  world.sessions[0].access_token = login.access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);

  const auto entry = world.enter(world.sessions[0]);
  world.enter(world.sessions[1]);
  const auto find_monster = [&](int cms_id) {
    return std::ranges::find_if(entry.unit_adds, [cms_id](const auto &add) {
      return add.at("nub").contains("npc") &&
             add.at("nub").at("npc").at("npc_cms_id") == cms_id;
    });
  };
  const auto first = find_monster(kFirstMonster);
  const auto second = std::ranges::find_if(entry.unit_adds, [&](const auto &add) {
    const auto &nub = add.at("nub");
    if (!nub.contains("npc") || nub.at("npc").at("npc_cms_id") != kSecondMonster ||
        !nub.contains("pos")) {
      return false;
    }
    const auto &point = nub.at("pos");
    const auto delta_x = static_cast<std::int64_t>(point.value("millimeter_x", 0)) -
                         second_point.at(0).get<int>();
    const auto delta_y = static_cast<std::int64_t>(point.value("millimeter_y", 0)) -
                         second_point.at(1).get<int>();
    return delta_x * delta_x + delta_y * delta_y <= 10000LL * 10000LL;
  });
  ASSERT_NE(first, entry.unit_adds.end());
  ASSERT_NE(second, entry.unit_adds.end());
  VehicleClient killer(world, world.sessions[0], 3);
  VehicleClient member(world, world.sessions[1], 3);
  killer.send(20086, nlohmann::json::object());
  member.send(20086, nlohmann::json::object());
  const auto killer_id = std::to_string(world.sessions[0].character_id);
  const auto member_id = std::to_string(world.sessions[1].character_id);
  killer.send(20097, {{"target_char_id", member_id}});
  std::vector<nlohmann::json> invitations;
  for (int attempt = 0; attempt < 50 && invitations.empty(); ++attempt) {
    invitations = frames_of(member.drain(), 188);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(invitations.size(), 1U);
  const auto joined = member.send(20098, {{"leader_char_id", killer_id},
                                          {"target_char_id", member_id},
                                          {"party_id", invitations.front().at("party_id")
                                                           .get<std::string>()}});
  ASSERT_FALSE(frames_of(joined, 184).empty());

  store->refuse_fences_for(world.sessions[1].character_id);
  store->lose_next_monster_commit_acknowledgement(world.sessions[1].character_id);
  store->observe_monster_batch(kSecondMonster, world.sessions[1].character_id);
  killer.send(20009, {{"text", "monster_kill " + first->at("unit_id").get<std::string>()}});
  ASSERT_TRUE(store->wait_for_lost_monster_commit_acknowledgement());
  const auto second_kill = killer.send(
      20009, {{"text", "monster_kill " + second->at("unit_id").get<std::string>()}});
  const auto second_id = second->at("unit_id").get<std::string>();
  for (const auto &death : frames_of(second_kill, 23)) {
    EXPECT_NE(death.value("target_unit_id", std::string{}), second_id);
  }
  EXPECT_FALSE(store->wait_for_monster_batch(std::chrono::milliseconds(100)));

  auto &recipient = world.sessions[1];
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(recipient.character_id);
  const auto selected = world.auth->select(recipient.access_token, select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  ASSERT_EQ(world.server->listener().disconnect_account(recipient.account_id), 1U);
  {
    WorldTestClient busy(world.server->port());
    busy.send_key(selected.packet_key());
    EXPECT_THROW(busy.read(), boost::system::system_error);
  }
  store->allow_fences();
  ASSERT_TRUE(store->wait_for_monster_batch(std::chrono::seconds(10)));

  std::unique_ptr<WorldTestClient> reconnected;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    auto attempt = std::make_unique<WorldTestClient>(world.server->port());
    attempt->send_key(selected.packet_key());
    try {
      const auto response = attempt->read();
      ASSERT_EQ(response.opcode(), 5);
      reconnected = std::move(attempt);
      break;
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(reconnected);
  recipient.client = std::move(reconnected);
  world.enter_after_key_exchange(recipient);
  VehicleClient resumed(world, recipient, 3);
  resumed.send(20086, nlohmann::json::object());
  const auto progress = world.repositories.character_state->load_world_progress(
      recipient.account_id, recipient.character_id, 1);
  EXPECT_EQ(progress.quests.at(kHuntQuest).objectives.at(1), 1);
  EXPECT_EQ(progress.play_records.at(524), 1);
}

void verify_ambiguous_gm_quest_capture(bool commit_first) {
  constexpr int kMonster = 5001535;
  constexpr int kHuntQuest = 10000127;
  constexpr int kPartyUnlock = 90100011;
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  std::ifstream input(catalog_path.parent_path() / "world-mobs/definitions.json");
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(profile.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == 9012;
  });
  ASSERT_NE(map, profile.at("maps").end());
  const auto spawn = std::ranges::find_if(map->at("spawns"), [](const auto &entry) {
    return entry.at("cms_id") == kMonster;
  });
  ASSERT_NE(spawn, map->at("spawns").end());
  const auto &point = spawn->at("position");
  const WorldPosition position{9012, point.at(0), point.at(1), point.at(2), 0};
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  NpcPlacementSet placements;
  placements.map_id = position.map_id;
  const auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  std::shared_ptr<PausingRegionCheckpointStore> store;
  OverworldSessions world(
      population, {position, position}, catalog, catalog_path, {}, CharacterStats::load(*catalog),
      {}, {}, {}, [&](std::shared_ptr<CharacterStateRepository> inner) {
        store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
        return store;
      });
  for (const auto &session : world.sessions) {
    const auto before = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    auto after = before;
    ++after.revision;
    after.quests[kPartyUnlock].complete = true;
    ASSERT_TRUE(world.repositories.character_state->save_world_progress(
        session.account_id, session.character_id, 1, before, after));
  }
  for (std::size_t index = 0; index < world.sessions.size(); ++index) {
    const auto name = "overworld_sessions" + std::to_string(index);
    ASSERT_TRUE(world.auth->set_account_role(name, AccountRole::game_master));
    const auto login = world.auth->login(name, "test-world-password-123");
    world.sessions[index].access_token = login.access_token();
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(1);
    ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);
  }

  const auto entry = world.enter(world.sessions[0]);
  world.enter(world.sessions[1]);
  const auto monster = std::ranges::find_if(entry.unit_adds, [](const auto &add) {
    return add.at("nub").contains("npc") &&
           add.at("nub").at("npc").at("npc_cms_id") == kMonster;
  });
  ASSERT_NE(monster, entry.unit_adds.end());
  VehicleClient killer(world, world.sessions[0], 3);
  VehicleClient member(world, world.sessions[1], 3);
  killer.send(20086, nlohmann::json::object());
  member.send(20086, nlohmann::json::object());
  member.move_to(position);
  const auto killer_id = std::to_string(world.sessions[0].character_id);
  const auto member_id = std::to_string(world.sessions[1].character_id);
  killer.send(20097, {{"target_char_id", member_id}});
  std::vector<nlohmann::json> invitations;
  for (int attempt = 0; attempt < 50 && invitations.empty(); ++attempt) {
    invitations = frames_of(member.drain(), 188);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(invitations.size(), 1U);
  const auto party_id = invitations.front().at("party_id").get<std::string>();
  const auto joined = member.send(20098, {{"leader_char_id", killer_id},
                                          {"target_char_id", member_id},
                                          {"party_id", party_id}});
  ASSERT_FALSE(frames_of(joined, 184).empty());

  auto &recipient = world.sessions[1];
  const auto original_gold = world.repositories.character_state->load_world_progress(
      recipient.account_id, recipient.character_id, 1).gold;
  store->refuse_fences_for(recipient.character_id);
  store->lose_next_quest_write_acknowledgement(recipient.character_id, commit_first);
  store->observe_monster_batch(kMonster, recipient.character_id);
  member.send_without_barrier(20009, {{"text", "accept_quest 10000127"}});
  ASSERT_TRUE(store->wait_for_lost_quest_write_acknowledgement());
  // A later writer is queued while the earlier COMMIT outcome is unknown. It must not
  // pass the synchronous close gate even if its frame was already received by the server.
  member.send_without_barrier(20009, {{"text", "add_money 500"}});
  const auto monster_id = monster->at("unit_id").get<std::string>();
  const auto kill = killer.send(20009, {{"text", "monster_kill " + monster_id}});
  store->release_quest_write();
  EXPECT_TRUE(frames_of(kill, 23).empty());
  EXPECT_FALSE(store->wait_for_monster_batch(std::chrono::milliseconds(100)));

  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(recipient.character_id);
  const auto selected = world.auth->select(recipient.access_token, select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool old_connection_closed = false;
  while (std::chrono::steady_clock::now() < close_deadline) {
    const auto connections = world.server->listener().connections();
    if (std::ranges::none_of(connections, [&](const auto &connection) {
          return connection.authenticated && connection.character_id == recipient.character_id;
        })) {
      old_connection_closed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(old_connection_closed);
  EXPECT_EQ(world.repositories.character_state->load_world_progress(
                recipient.account_id, recipient.character_id, 1).gold,
            original_gold);
  {
    WorldTestClient busy(world.server->port());
    busy.send_key(selected.packet_key());
    EXPECT_THROW(busy.read(), boost::system::system_error);
  }
  store->allow_fences();
  ASSERT_TRUE(store->wait_for_monster_batch(std::chrono::seconds(10)));

  std::unique_ptr<WorldTestClient> reconnected;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    auto attempt = std::make_unique<WorldTestClient>(world.server->port());
    attempt->send_key(selected.packet_key());
    try {
      const auto response = attempt->read();
      ASSERT_EQ(response.opcode(), 5);
      reconnected = std::move(attempt);
      break;
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(reconnected);
  recipient.client = std::move(reconnected);
  world.enter_after_key_exchange(recipient);
  VehicleClient resumed(world, recipient, 3);
  resumed.send(20086, nlohmann::json::object());
  const auto progress = world.repositories.character_state->load_world_progress(
      recipient.account_id, recipient.character_id, 1);
  EXPECT_EQ(progress.gold, original_gold);
  ASSERT_EQ(progress.quests.contains(kHuntQuest), commit_first);
  if (commit_first) {
    EXPECT_EQ(progress.quests.at(kHuntQuest).objectives.at(1), 1);
  }
  EXPECT_EQ(progress.play_records.at(524), 1);
}

TEST(NativeWorld, AmbiguousGmQuestWriteFencesDisconnectAndAnotherPlayersKill) {
  verify_ambiguous_gm_quest_capture(false);
  verify_ambiguous_gm_quest_capture(true);
}

TEST(NativeWorld, GatheringSnapshotPreservesWorldMonsterSpawnsAndRewards) {
  verify_shared_world_monster_spawns_and_rewards(true);
}

TEST(NativeWorld, CliffsDefeatsBroadcastEncounterPhasesOverTls) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  std::ifstream input(catalog_path.parent_path() / "world-mobs" / "definitions.json");
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(profile.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == 30104;
  });
  ASSERT_NE(map, profile.at("maps").end());

  for (const auto &[npc_id, sector, local_id, phase] :
       std::vector<std::tuple<int, std::uint32_t, std::uint32_t, int>>{
           {5001510, 170 | (289 << 14), 5, 2},
           {5001104, 170 | (290 << 14), 2, 1}}) {
    SCOPED_TRACE(npc_id);
    const auto spawn = std::ranges::find_if(map->at("spawns"), [&](const auto &entry) {
      return entry.at("cms_id") == npc_id;
    });
    ASSERT_NE(spawn, map->at("spawns").end());
    const auto &point = spawn->at("position");
    const WorldPosition position{30104, point.at(0), point.at(1), point.at(2), 0};
    NpcPlacementSet empty_population;
    empty_population.map_id = 30104;
    auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
    auto catalog = std::make_shared<ClientCatalog>(catalog_path);
    auto gathering = GatheringData::load(catalog_path.parent_path() / "gathering" /
                                          "definitions.json");
    OverworldSessions world(population, {position, position}, catalog, catalog_path, {},
                            CharacterStats::load(*catalog), {}, gathering);
    ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
    const auto login = world.auth->login("overworld_sessions0", "test-world-password-123");
    world.sessions[0].access_token = login.access_token();
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(1);
    ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);
    world.enter(world.sessions[0]);
    world.enter(world.sessions[1]);
    VehicleClient killer(world, world.sessions[0], 3);
    VehicleClient watcher(world, world.sessions[1], 3);
    const auto started = killer.send(20086, nlohmann::json::object());
    watcher.send(20086, nlohmann::json::object());
    const auto adds = frames_of(started, kUnitAddOpcode);
    const auto monster = std::ranges::find_if(adds, [&](const auto &add) {
      return add.at("nub").contains("npc") && add.at("nub").at("npc").at("npc_cms_id") == npc_id;
    });
    ASSERT_NE(monster, adds.end());
    const auto target = std::to_string(doodad_unit_id(sector, local_id));
    const auto killed = killer.send(20009, {{"text", "monster_kill " +
                                                     monster->at("unit_id").get<std::string>()}});
    const auto has_phase = [&](const auto &frames) {
      const auto changes = frames_of(frames, 194);
      return std::ranges::any_of(changes, [&](const auto &change) {
        return change.at("unit_id") == target && change.at("phase_id") == phase;
      });
    };
    EXPECT_TRUE(has_phase(killed));
    bool observed = false;
    for (int poll = 0; poll < 50 && !observed; ++poll) {
      observed = has_phase(watcher.drain());
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(observed);
  }
}

// Corpse butchering over the real frame path: the nub offers it, the death announces it, and a
// knife interaction grants the monster's ExpertiseReward pack (captured official flow).
TEST(NativeWorld, KillerButchersAWorldMonsterCorpseWithAKnifeOverTls) {
  constexpr int kKnife = 11200301;
  constexpr int kToolSlot = 2;
  constexpr int kKnifeAnimation = 1000;
  const auto catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  const auto profile_path = catalog_path.parent_path() / "world-mobs" / "definitions.json";
  const auto loot_path = catalog_path.parent_path() / "loot" / "definitions.json";
  const auto gathering_path = catalog_path.parent_path() / "gathering" / "definitions.json";
  if (!std::filesystem::exists(profile_path) || !std::filesystem::exists(loot_path) ||
      !std::filesystem::exists(gathering_path)) {
    GTEST_SKIP() << "Build the world monster, loot and gathering profiles first";
  }
  std::ifstream input(profile_path);
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(
      profile.at("maps"), [](const auto &entry) { return entry.at("map_id") == 9012; });
  ASSERT_NE(map, profile.at("maps").end());
  const auto gathering = GatheringData::load(gathering_path);
  // Every butcherable 9012 slot takes short idle walks (at most 3 m from home), so the corpse
  // stays within the interaction range of a player standing at the home point.
  const auto selected_spawn = std::ranges::find_if(map->at("spawns"), [&](const auto &entry) {
    return entry.value("handling_loot_candidate", false) && !entry.contains("patrol") &&
           gathering->handling_reward(entry.at("cms_id").get<int>()) != nullptr;
  });
  ASSERT_NE(selected_spawn, map->at("spawns").end());
  const auto &spawn = *selected_spawn;
  const auto &point = spawn.at("position");
  const WorldPosition position{9012, point.at(0), point.at(1), point.at(2), 0};

  NpcPlacementSet empty_population;
  empty_population.map_id = 9012;
  const auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  OverworldSessions world(population, {position}, catalog, catalog_path, {},
                          CharacterStats::load(*catalog), LootData::load(loot_path), gathering);
  auto &session = world.sessions[0];
  ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
  const auto login = world.auth->login("overworld_sessions0", "test-world-password-123");
  session.access_token = login.access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);
  ASSERT_TRUE(world.repositories.character_state
                  ->grant_item(session.account_id, session.character_id, 1,
                               CharacterItem{0, kKnife, 60, kToolSlot, 1, 1500, 0})
                  .has_value());

  const auto entered = world.enter(session);
  const auto monster = std::ranges::find_if(entered.unit_adds, [&](const auto &add) {
    return add.at("nub").contains("npc") &&
           add.at("nub").at("npc").at("npc_cms_id") == spawn.at("cms_id") &&
           add.at("nub").at("pos").at("millimeter_x") == point.at(0) &&
           add.at("nub").at("pos").at("millimeter_y") == point.at(1);
  });
  ASSERT_NE(monster, entered.unit_adds.end());
  EXPECT_TRUE(monster->at("nub").at("npc").value("is_handling_loot_candidate", false));
  const auto id = monster->at("unit_id").get<std::string>();

  VehicleClient killer(world, session, 3);
  killer.send(20086, nlohmann::json::object());
  auto killed = killer.send(20009, {{"text", "monster_kill " + id}});
  bool announced = false;
  for (int poll = 0; poll < 50 && !announced; ++poll) {
    announced = std::ranges::any_of(frames_of(killed, kUnitLootItemInfoOpcode),
                                    [&](const auto &flags) {
                                      return flags.at("unit_id") == id &&
                                             flags.value("possible_handling_loot", false);
                                    });
    if (!announced) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      killed = killer.drain();
    }
  }
  ASSERT_TRUE(announced);

  const nlohmann::json pos{{"millimeter_x", position.x_mm},
                           {"millimeter_y", position.y_mm},
                           {"millimeter_z", position.z_mm}};
  const auto started = killer.send(20069, {{"interaction_anim_id", kKnifeAnimation},
                                           {"target_unit_id", id},
                                           {"param", {{"handling_loot", nlohmann::json::object()}}},
                                           {"pos", pos},
                                           {"yaw", 0.0}});
  ASSERT_EQ(frames_of(started, 166).size(), 1u);
  const auto before = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);

  // The server accepts the execute only after the montage's execute event (2.93 s).
  std::this_thread::sleep_for(std::chrono::milliseconds(3000));
  const auto executed = killer.send(20070, {{"interaction_anim_id", kKnifeAnimation}});
  ASSERT_EQ(frames_of(executed, 169).size(), 1u) << "execute frames: " << executed.size();
  const auto update = frames_of(executed, 169).front();
  EXPECT_EQ(update.at("items").at(0).at("durability"), 1400);
  EXPECT_EQ(frames_of(executed, 167).size(), 1u);
  const auto after = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  EXPECT_EQ(after.revision, before.revision + 1);
  for (const auto &stack : update.at("stackables")) {
    const int item_cms_id = stack.at("item_cms_id");
    EXPECT_EQ(after.stackables.at(item_cms_id), stack.at("quantity").get<int>());
  }

  // The same corpse cannot be butchered twice.
  killer.send(20071, {{"end_interaction_anim", {{"interaction_anim_id", kKnifeAnimation}}}});
  const auto again = killer.send(20069, {{"interaction_anim_id", kKnifeAnimation},
                                         {"target_unit_id", id},
                                         {"param", {{"handling_loot", nlohmann::json::object()}}},
                                         {"pos", pos},
                                         {"yaw", 0.0}});
  ASSERT_EQ(frames_of(again, 165).size(), 1u);
  EXPECT_EQ(frames_of(again, 165).front().value("ec", ""), "Npc_HasNoHandlingLoot");
}

TEST(NativeWorld, SharedFieldPotionConsumesLastItemAndRefusesEmptyAnimation) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Needs the generated catalog and world combat profile";
  }
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  const WorldPosition position{9012, -1000000, 3500000, -600000};
  OverworldSessions world({}, {position}, catalog, catalog_path, {},
                          CharacterStats::load(*catalog));
  auto &session = world.sessions.front();
  const auto load = [&] {
    return world.repositories.character_state->load_world_progress(session.account_id,
                                                                   session.character_id, 1);
  };
  const auto before = load();
  auto stocked = before;
  stocked.stackables[40101101] = 1;
  stocked.utility_slots[0] = {40101101, 1};
  ++stocked.revision;
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      session.account_id, session.character_id, 1, before, stocked));
  world.enter(session);
  const nlohmann::json pos{{"millimeter_x", position.x_mm},
                           {"millimeter_y", position.y_mm},
                           {"millimeter_z", position.z_mm}};
  nlohmann::json drink{{"use_item", {{"item_cms_id", 40101101}}},
                       {"start_skill",
                        {{"skill_id", 1000},
                         {"instance_id", 1},
                         {"tick", "1"},
                         {"pos", pos},
                         {"rotation", {{"yaw", 0}}}}}};
  session.client->send(world.protocol.encode(20010, drink, 3));
  EXPECT_EQ(world.protocol.decode(read_until_opcode(*session.client, 14))
                .at("message")
                .at("start_skill")
                .at("skill_id"),
            1000);
  const auto spent = world.protocol.decode(read_until_opcode(*session.client, 169)).at("message");
  EXPECT_EQ(spent.at("stackables").at(0).value("quantity", 0), 0);
  read_until_opcode(*session.client, 15);
  EXPECT_EQ(load().stackables.at(40101101), 0);
  EXPECT_EQ(load().utility_slots.at(0).count, 0);
  const auto consumed = load();

  drink["start_skill"]["instance_id"] = 2;
  session.client->send(world.protocol.encode(20010, drink, 4));
  bool refused = false;
  for (const auto &frame : world.read_until_pong(session, 5)) {
    EXPECT_NE(frame.opcode(), 14);
    EXPECT_NE(frame.opcode(), 53);
    if (frame.opcode() == 13) {
      refused = true;
      EXPECT_EQ(world.protocol.decode(frame).at("message").at("instance_id"), 2);
    }
  }
  EXPECT_TRUE(refused);
  EXPECT_EQ(load(), consumed);
}

// Two characters without a party enter the first authored map of `instance_type` with monster
// spawns; each must fight its own copy's monsters.
void expect_solo_copies_have_separate_monsters(const std::string &instance_type) {
  const auto catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified export";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  std::ifstream input(catalog_path.parent_path() / "world-mobs" / "definitions.json");
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(profile.at("maps"), [&](const auto &entry) {
    return entry.value("instance_type", "Field") == instance_type && !entry.at("spawns").empty();
  });
  ASSERT_NE(map, profile.at("maps").end());
  const int map_id = map->at("map_id");
  const auto &point = map->at("spawns").front().at("position");
  const WorldPosition position{map_id, point.at(0), point.at(1), point.at(2)};
  NpcPlacementSet empty_population;
  empty_population.map_id = map_id;
  const auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  OverworldSessions world(population, {position, position}, catalog, catalog_path, {},
                           CharacterStats::load(*catalog));
  world.enter(world.sessions[0]);
  world.enter(world.sessions[1]);
  // Instance monsters are announced after StartPlaying, as in the captured entry.
  VehicleClient first_client(world, world.sessions[0], 3);
  VehicleClient second_client(world, world.sessions[1], 3);
  const auto first = frames_of(first_client.send(20086, nlohmann::json::object()),
                               kUnitAddOpcode);
  const auto second = frames_of(second_client.send(20086, nlohmann::json::object()),
                                kUnitAddOpcode);
  std::set<std::string> first_monsters;
  for (const auto &add : first) {
    if (add.at("nub").contains("npc")) {
      first_monsters.insert(add.at("unit_id").get<std::string>());
    }
  }
  EXPECT_FALSE(first_monsters.empty());
  // Characters without a party occupy separate copies with separate monster lives.
  std::size_t second_monsters = 0;
  for (const auto &add : second) {
    if (add.at("nub").contains("npc")) {
      ++second_monsters;
      EXPECT_FALSE(first_monsters.contains(add.at("unit_id").get<std::string>()));
    }
  }
  EXPECT_GT(second_monsters, 0u);
}

TEST(NativeWorld, AuthoredInstanceMonsterPopulationsArePrivatePerCharacter) {
  expect_solo_copies_have_separate_monsters("Instance");
}

TEST(NativeWorld, DungeonMonsterPopulationsArePrivatePerSoloCharacter) {
  expect_solo_copies_have_separate_monsters("Dungeon");
}

TEST(NativeWorld, FallDamageReachesSocketAndLethalLandingPersistsThroughReconnect) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Requires generated tutorial profile and its catalog/raw export";
  }

  DeathFixture fixture(profile_path);
  auto session = fixture.connect();
  fixture.send(session, 20086, nlohmann::json::object());
  fixture.read_until(session, 183);
  fixture.frames_until_ping(session);

  // Away from tutorial enemies: the only damage in these batches must be our landing.
  const auto move = [&](int height_mm, const char *mode) {
    fixture.send(
        session, 20087,
        {{"mode", mode},
         {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", height_mm}}}});
  };
  const auto damages = [](const std::vector<nlohmann::json> &frames) {
    std::vector<nlohmann::json> result;
    for (const auto &frame : frames) {
      if (frame.at("opcode") == 19) {
        result.push_back(frame.at("message"));
      }
    }
    return result;
  };

  move(50000, "MoveMode_Normal");
  move(49000, "MoveMode_Falling");
  move(45000, "MoveMode_Normal");
  EXPECT_TRUE(damages(fixture.frames_until_ping(session)).empty());

  move(50000, "MoveMode_Normal");
  move(49000, "MoveMode_Falling");
  move(35000, "MoveMode_Normal");
  const auto impact = damages(fixture.frames_until_ping(session));
  ASSERT_EQ(impact.size(), 1U);
  EXPECT_GT(impact.front().at("final_damage_amount").get<int>(), 0);
  EXPECT_GT(impact.front().at("stat_changes").at(0).at("result").get<int>(), 0);
  EXPECT_FALSE(fixture.progress().dead);
  move(35000, "MoveMode_Normal");
  EXPECT_TRUE(damages(fixture.frames_until_ping(session)).empty());

  move(50000, "MoveMode_Normal");
  move(49000, "MoveMode_Falling");
  move(0, "MoveMode_Normal");
  const auto lethal = damages(fixture.frames_until_ping(session));
  ASSERT_EQ(lethal.size(), 1U);
  EXPECT_EQ(lethal.front().at("stat_changes").at(0).value("result", 0), 0);
  EXPECT_TRUE(fixture.progress().dead);

  session.client->disconnect();
  auto resumed = fixture.connect();
  EXPECT_TRUE(fixture.progress().dead);
  EXPECT_EQ(fixture.revive_and_collect_revival_opcodes(resumed),
            (std::vector<int>{49, 51, 47, 32}));
  EXPECT_FALSE(fixture.progress().dead);
  resumed.client->disconnect();
}

TEST(NativeWorld, SharedMapFallDamagePersistsDeathAndSupportsRevival) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Requires generated tutorial profile and catalog/raw export";
  }
  const auto definitions = TutorialData::load(profile_path);
  const auto catalog_root =
      std::filesystem::path(profile_path).parent_path().parent_path() / "catalog";
  const auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  const auto stats = CharacterStats::load(*catalog);
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn}, catalog, catalog_root,
                          definitions, stats);
  auto &session = world.sessions.front();
  const auto enter = [&] {
    world.connect(session);
    const auto read_until = [&](std::uint16_t opcode) {
      for (int count = 0; count < 2000; ++count) {
        const auto frame = session.client->read();
        if (frame.opcode() == opcode) {
          return world.protocol.decode(frame).at("message");
        }
      }
      throw std::runtime_error("Fall fixture did not receive its bootstrap frame");
    };
    session.client->send(world.protocol.encode(20001, nlohmann::json::object(), 0));
    read_until(175);
    session.client->send(world.protocol.encode(20085, nlohmann::json::object(), 1));
    auto map_complete = read_until(176);
    world.read_until_pong(session, 2);
    return map_complete;
  };
  enter();
  std::uint32_t sequence = 3;
  const auto move = [&](int height_mm, const char *mode) {
    session.client->send(world.protocol.encode(20087,
                                               {{"mode", mode},
                                                {"pos",
                                                 {{"millimeter_x", kOverworldSpawn.x_mm},
                                                  {"millimeter_y", kOverworldSpawn.y_mm},
                                                  {"millimeter_z", height_mm}}}},
                                               sequence++));
  };
  move(50000, "MoveMode_Normal");
  move(49000, "MoveMode_Falling");
  move(35000, "MoveMode_Normal");
  auto frames = world.read_until_pong(session, sequence++);
  EXPECT_EQ(std::ranges::count_if(frames, [](const Frame &frame) {
    return frame.opcode() == 19;
  }),
            1);
  EXPECT_FALSE(world.repositories.character_state
                   ->load_world_progress(session.account_id, session.character_id, 1)
                   .dead);

  move(50000, "MoveMode_Normal");
  move(49000, "MoveMode_Falling");
  move(0, "MoveMode_Normal");
  frames = world.read_until_pong(session, sequence++);
  EXPECT_EQ(std::ranges::count_if(frames, [](const Frame &frame) {
    return frame.opcode() == 23;
  }),
            1);
  EXPECT_TRUE(world.repositories.character_state
                  ->load_world_progress(session.account_id, session.character_id, 1)
                  .dead);

  session.client->disconnect();
  const auto entry = enter();
  const auto &player = entry.at("nub").at("player");
  EXPECT_EQ(player.at("stats").value("69", 0), 0);
  session.client->send(world.protocol.encode(20039, nlohmann::json::object(), 3));
  frames = world.read_until_pong(session, 4);
  EXPECT_EQ(std::ranges::count_if(frames, [](const Frame &frame) {
    return frame.opcode() == 49;
  }),
            1);
  EXPECT_FALSE(world.repositories.character_state
                   ->load_world_progress(session.account_id, session.character_id, 1)
                   .dead);
  session.client->disconnect();
}

TEST(NativeWorld, SwimmingAndSprintDrainStaminaAndSurfacingStopsDrowningWithoutMovement) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Requires generated tutorial profile and catalog/raw export";
  }
  DeathFixture fixture(profile_path);
  auto session = fixture.connect();
  const auto move = [&](const char *mode, const char *detail) {
    fixture.send(session, 20087,
                 {{"mode", mode},
                  {"detail", detail},
                  {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 10000}}}});
  };
  move("MoveMode_Swimming", "MoveDetail_SW_Dive");
  std::vector<nlohmann::json> first_tick;
  const auto first = fixture.read_until(session, 19, &first_tick);
  EXPECT_EQ(first.at("skill_effect_origin_type"), "EX5SkillEffectOriginType_Drowned");
  EXPECT_TRUE(std::ranges::any_of(first_tick, [](const nlohmann::json &frame) {
    if (frame.at("opcode") != 54 || !frame.at("message").contains("flows")) {
      return false;
    }
    return std::ranges::any_of(frame.at("message").at("flows"), [](const auto &flow) {
      return flow.value("source", std::string()) == "SCS_SwimFlow" &&
             flow.value("rate_per_sec", 0.0) < 0;
    });
  }));

  // Stop is still underwater: damage must continue on the server timer.
  move("MoveMode_Swimming", "MoveDetail_Stop");
  const auto stopped = fixture.read_until(session, 19);
  EXPECT_EQ(stopped.at("skill_effect_origin_type"), "EX5SkillEffectOriginType_Drowned");
  move("MoveMode_Swimming", "MoveDetail_SurfaceIdle");
  fixture.frames_until_ping(session);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  const auto surfaced = fixture.frames_until_ping(session);
  EXPECT_FALSE(std::ranges::any_of(
      surfaced, [](const nlohmann::json &frame) { return frame.at("opcode") == 19; }));

  move("MoveMode_Normal", "MoveDetail_Sprint");
  std::vector<nlohmann::json> sprint_frames;
  fixture.read_until(session, 47, &sprint_frames);
  // A prior recovery can be queued before the first sprint cost; use a bounded barrier loop.
  bool sprint_cost = false;
  for (int count = 0; count < 20 && !sprint_cost; ++count) {
    for (const auto &frame : sprint_frames) {
      sprint_cost = sprint_cost || (frame.at("opcode") == 47 &&
                                    frame.at("message").value("default_source", std::string()) ==
                                        "SCS_SprintFlow");
    }
    if (!sprint_cost) {
      sprint_frames.clear();
      fixture.read_until(session, 47, &sprint_frames);
    }
  }
  EXPECT_TRUE(sprint_cost);
  move("MoveMode_Normal", "MoveDetail_Run");
  fixture.frames_until_ping(session);
  session.client->disconnect();
}

TEST(NativeWorld, SharedMapDrowningTimerPersistsDeathAndRevivalClearsTheDive) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Requires generated tutorial profile and catalog/raw export";
  }
  const auto definitions = TutorialData::load(profile_path);
  const auto catalog_root =
      std::filesystem::path(profile_path).parent_path().parent_path() / "catalog";
  const auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  const auto stats = CharacterStats::load(*catalog);
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn}, catalog, catalog_root,
                          definitions, stats);
  auto &session = world.sessions.front();
  const auto read_until = [&](std::uint16_t opcode) {
    for (int count = 0; count < 2000; ++count) {
      const auto frame = session.client->read();
      if (frame.opcode() == opcode) {
        return world.protocol.decode(frame).at("message");
      }
    }
    throw std::runtime_error("Drowning fixture did not receive its expected frame");
  };
  const auto enter = [&](bool wait_for_retirement = false) {
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(session.character_id);
    const auto selected = world.auth->select(session.access_token, select);
    if (selected.ec() != x5::ec::Success) {
      throw std::runtime_error("Drowning fixture cannot select its character");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
      try {
        session.client = std::make_unique<WorldTestClient>(world.server->port());
        session.client->send_key(selected.packet_key());
        if (session.client->read().opcode() != 5) {
          throw std::runtime_error("Drowning fixture received an unexpected key response");
        }
        break;
      } catch (const boost::system::system_error &error) {
        const bool admission_closed = error.code() == asio::error::eof ||
                                      error.code() == asio::error::connection_reset ||
                                      error.code() == ssl::error::stream_truncated;
        if (!wait_for_retirement || !admission_closed ||
            std::chrono::steady_clock::now() >= deadline) {
          throw;
        }
        // Socket closure precedes retained death/close settlement and admission retirement.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    session.client->send(world.protocol.encode(20001, nlohmann::json::object(), 0));
    read_until(175);
    session.client->send(world.protocol.encode(20085, nlohmann::json::object(), 1));
    const auto complete = read_until(176);
    world.read_until_pong(session, 2);
    return complete;
  };
  enter();
  std::uint32_t sequence = 3;
  const auto move = [&](int z_mm, const char *mode, const char *detail) {
    session.client->send(world.protocol.encode(20087,
                                               {{"mode", mode},
                                                {"detail", detail},
                                                {"pos",
                                                 {{"millimeter_x", kOverworldSpawn.x_mm},
                                                  {"millimeter_y", kOverworldSpawn.y_mm},
                                                  {"millimeter_z", z_mm}}}},
                                               sequence++));
  };
  // The tested 29 m fall leaves at most 5% HP; the first underwater timer tick kills.
  move(50000, "MoveMode_Normal", "MoveDetail_Stop");
  move(49000, "MoveMode_Falling", "MoveDetail_Down");
  move(21000, "MoveMode_Normal", "MoveDetail_Stop");
  world.read_until_pong(session, sequence++);
  move(21000, "MoveMode_Swimming", "MoveDetail_SW_Dive");
  const auto damage = read_until(19);
  EXPECT_EQ(damage.at("skill_effect_origin_type"), "EX5SkillEffectOriginType_Drowned");
  read_until(23);
  world.read_until_pong(session, sequence++);
  EXPECT_TRUE(world.repositories.character_state
                  ->load_world_progress(session.account_id, session.character_id, 1)
                  .dead);

  session.client->disconnect();
  const auto entry = enter(true);
  EXPECT_EQ(entry.at("nub").at("player").at("stats").value("69", 0), 0);
  session.client->send(world.protocol.encode(20039, nlohmann::json::object(), 3));
  world.read_until_pong(session, 4);
  EXPECT_FALSE(world.repositories.character_state
                   ->load_world_progress(session.account_id, session.character_id, 1)
                   .dead);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  const auto after_revive = world.read_until_pong(session, 5);
  EXPECT_FALSE(std::ranges::any_of(after_revive, [](const Frame &frame) {
    return frame.opcode() == 19;
  }));
  session.client->disconnect();
}

TEST(NativeWorld, PlayerPvpFlagReachesOwnerObserversAndResetsAfterReconnect) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  const auto first = select_world_test_character(auth, "pvp_owner_test", "PvpOwner", 1);
  const auto second = select_world_test_character(auth, "pvp_observer_test", "PvpObserver", 3);
  const auto first_character_id = first.character().char_id();
  const auto first_unit_id = std::to_string(player_wire_unit_id(first_character_id));
  const ChatIdentity first_identity{1, first_character_id, "PvpOwner"};

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  std::atomic<unsigned> entries{};
  server.listener().set_world_entry_handler([&entries](const ChatIdentity &) { ++entries; });
  // Shared ownership: connections that close during teardown still report their departure.
  struct WorldExits {
    std::atomic<unsigned> count{};
    std::atomic<std::int64_t> character_id{};
  };
  const auto exits = std::make_shared<WorldExits>();
  server.listener().set_world_exit_handler([exits](const ChatIdentity &identity) {
    exits->character_id = identity.character_id;
    ++exits->count;
  });

  WorldTestClient first_client(server.port());
  enter_shared_world(first_client, protocol, first.packet_key());
  WorldTestClient second_client(server.port());
  enter_shared_world(second_client, protocol, second.packet_key());
  ASSERT_EQ(first_client.read().opcode(), 10);
  ASSERT_EQ(second_client.read().opcode(), 10);
  first_client.send(protocol.encode(20086, nlohmann::json::object(), 2));
  ASSERT_EQ(read_until_opcode(first_client, 183).opcode(), 183);
  second_client.send(protocol.encode(20086, nlohmann::json::object(), 2));
  ASSERT_EQ(read_until_opcode(second_client, 183).opcode(), 183);
  EXPECT_EQ(entries.load(), 2U);
  EXPECT_EQ(exits->count.load(), 0U);

  auto enabled_completion = std::make_shared<std::promise<std::string>>();
  auto enabled = enabled_completion->get_future();
  ASSERT_TRUE(
      server.listener().player_pvp(first_identity, true, [enabled_completion](std::string text) {
        enabled_completion->set_value(std::move(text));
      }));
  ASSERT_EQ(enabled.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_EQ(enabled.get(), "PvP flag enabled.");
  for (auto *client : {&first_client, &second_client}) {
    const auto changed = protocol.decode(read_until_opcode(*client, 82)).at("message");
    EXPECT_EQ(changed.at("unit_id"), first_unit_id);
    EXPECT_EQ(changed.at("type"), "EX5Faction_PvP_MODE");
  }

  // Repeated StartPlaying must neither reset the flag nor invoke the entry callback again.
  first_client.send(protocol.encode(20086, nlohmann::json::object(), 3));
  first_client.send(protocol.encode(20003, {{"ping_cycles64", "1"}, {"tick", "1"}}, 4));
  ASSERT_EQ(read_until_opcode(first_client, 4).opcode(), 4);
  EXPECT_EQ(entries.load(), 2U);
  auto status_completion = std::make_shared<std::promise<std::string>>();
  auto status = status_completion->get_future();
  ASSERT_TRUE(server.listener().player_pvp(
      first_identity, std::nullopt,
      [status_completion](std::string text) { status_completion->set_value(std::move(text)); }));
  ASSERT_EQ(status.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_EQ(status.get(), "PvP flag: ON.");

  first_client.disconnect();
  ASSERT_EQ(read_until_opcode(second_client, 11).opcode(), 11);
  // The departure is reported once, for the character whose entry was reported.
  for (int attempt = 0; attempt < 30 && exits->count.load() == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_EQ(exits->count.load(), 1U);
  EXPECT_EQ(exits->character_id.load(), first_character_id);
  const auto login = auth.login("pvp_owner_test", "test-world-password-123");
  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);
  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(first_character_id);
  const auto reselected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(reselected.ec(), x5::ec::Success);
  // The exit callback precedes final close settlement. Observe the existing admission
  // fence rather than assuming that the departure notification makes reconnect safe.
  std::unique_ptr<OfflineCharacterReservation> retired_character;
  const auto retirement_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!retired_character && std::chrono::steady_clock::now() < retirement_deadline) {
    retired_character = server.listener().reserve_offline_character(first_character_id);
    if (!retired_character) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(retired_character);
  retired_character.reset();

  WorldTestClient reconnected(server.port());
  enter_shared_world(reconnected, protocol, reselected.packet_key());
  const auto readded = protocol.decode(read_until_opcode(second_client, 10)).at("message");
  EXPECT_EQ(readded.at("unit_id"), first_unit_id);
  EXPECT_EQ(readded.at("nub").at("player").value("faction_type", "EX5Faction_Player"),
            "EX5Faction_Player");
  reconnected.send(protocol.encode(20086, nlohmann::json::object(), 2));
  ASSERT_EQ(read_until_opcode(reconnected, 183).opcode(), 183);
  EXPECT_EQ(entries.load(), 3U);

  auto reset_completion = std::make_shared<std::promise<std::string>>();
  auto reset = reset_completion->get_future();
  ASSERT_TRUE(server.listener().player_pvp(
      first_identity, std::nullopt,
      [reset_completion](std::string text) { reset_completion->set_value(std::move(text)); }));
  ASSERT_EQ(reset.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_EQ(reset.get(), "PvP flag: OFF.");
  EXPECT_FALSE(server.listener().player_pvp(ChatIdentity{2, first_character_id, "PvpOwner"}, true,
                                            [](std::string) {}));
}

} // namespace
} // namespace chronicles
