#include "world/features/companion/companion_world.hpp"
#include "world/features/companion/tests/native_vehicle_test_support.hpp"
#include "world/features/trade/tests/pausing_region_checkpoint_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;
using namespace vehicle_native_testing;

class RewardRefusalRelease {
public:
  explicit RewardRefusalRelease(std::shared_ptr<PausingRegionCheckpointStore> store)
      : store_(std::move(store)) {
  }

  ~RewardRefusalRelease() {
    store_->allow_monster_rewards();
  }

private:
  std::shared_ptr<PausingRegionCheckpointStore> store_;
};

bool is_server_close(const boost::system::error_code &code) {
  return code == asio::error::eof || code == asio::error::connection_reset ||
         code == ssl::error::stream_truncated;
}

TEST(NativeWorld, WagonCloseRetriesFailedCheckpointBeforeShutdownCompletes) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }
  std::shared_ptr<PausingRegionCheckpointStore> store;
  VehicleWorldFixture fixture(false, [&](std::shared_ptr<CharacterStateRepository> inner) {
    store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
    return store;
  });
  struct ReleaseCheckpoint {
    std::shared_ptr<PausingRegionCheckpointStore> store;
    ~ReleaseCheckpoint() {
      store->release();
    }
  } release{store};
  OverworldEntry entry;
  fixture.start(0, entry);
  auto &player = fixture.client(0);
  const auto ileste = npc_unit_id(entry, 1001021);
  player.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                      {"yaw", 0.0}});
  player.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1},
                      {"select_order", 1}});
  player.send(20105, nlohmann::json::object());
  const auto departure = player.send(
      20106, {{"add_goods", nlohmann::json::array(
                                {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  ASSERT_EQ(frames_of(departure, 119).size(), 1U);

  auto destination = kNearIleste;
  destination.x_mm += 1000;
  player.move_to(destination);
  const auto goods = fixture.progress(0).trade_run.goods;
  store->arm(fixture.world().sessions[0].character_id,
             PausingRegionCheckpointStore::Outcome::conflict);
  auto drained = std::make_shared<std::promise<void>>();
  auto completion = drained->get_future();
  fixture.listener().shutdown_when_drained([drained] { drained->set_value(); });

  ASSERT_TRUE(store->wait_until_blocked());
  EXPECT_EQ(completion.wait_for(std::chrono::milliseconds::zero()), std::future_status::timeout);
  store->release();
  ASSERT_EQ(completion.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto saved = fixture.progress(0);
  ASSERT_TRUE(saved.trade_run.wagon_position);
  EXPECT_EQ(saved.trade_run.wagon_position->x_mm, destination.x_mm);
  EXPECT_EQ(saved.trade_run.wagon_position->y_mm, destination.y_mm);
  EXPECT_EQ(saved.trade_run.goods, goods);
}

TEST(NativeWorld, WagonCloseRetriesFailedCheckpointBeforeSameTicketReconnect) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }
  std::shared_ptr<PausingRegionCheckpointStore> store;
  VehicleWorldFixture fixture(false, [&](std::shared_ptr<CharacterStateRepository> inner) {
    store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
    return store;
  });
  struct ReleaseCheckpoint {
    std::shared_ptr<PausingRegionCheckpointStore> store;
    ~ReleaseCheckpoint() {
      store->release();
    }
  } release{store};
  OverworldEntry entry;
  fixture.start(0, entry);
  auto &player = fixture.client(0);
  const auto ileste = npc_unit_id(entry, 1001021);
  player.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                      {"yaw", 0.0}});
  player.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1},
                      {"select_order", 1}});
  player.send(20105, nlohmann::json::object());
  const auto departure = player.send(
      20106, {{"add_goods", nlohmann::json::array(
                                {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  ASSERT_EQ(frames_of(departure, 119).size(), 1U);
  auto destination = kNearIleste;
  destination.x_mm += 1000;
  player.move_to(destination);
  const auto goods = fixture.progress(0).trade_run.goods;
  auto &world = fixture.world();
  auto &rider = world.sessions[0];
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(rider.character_id);
  const auto selected = world.auth->select(rider.access_token, select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  store->arm(rider.character_id, PausingRegionCheckpointStore::Outcome::conflict);
  rider.client->disconnect();
  ASSERT_TRUE(store->wait_until_blocked());
  {
    WorldTestClient busy(world.server->port());
    busy.send_key(selected.packet_key());
    try {
      const auto response = busy.read();
      FAIL() << "Unsettled wagon unexpectedly entered, opcode " << response.opcode();
    } catch (const boost::system::system_error &error) {
      ASSERT_TRUE(is_server_close(error.code())) << error.what();
    }
  }
  store->release();

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
    } catch (const boost::system::system_error &error) {
      ASSERT_TRUE(is_server_close(error.code())) << error.what();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(reconnected);
  const auto saved = fixture.progress(0);
  ASSERT_TRUE(saved.trade_run.wagon_position);
  EXPECT_EQ(saved.trade_run.wagon_position->x_mm, destination.x_mm);
  EXPECT_EQ(saved.trade_run.goods, goods);

  rider.client = std::move(reconnected);
  world.enter_after_key_exchange(rider);
  VehicleClient resumed(world, rider, 3);
  const auto started = resumed.send(20086, nlohmann::json::object());
  const auto wagons = frames_of(started, kUnitAddOpcode);
  ASSERT_EQ(wagons.size(), 1U);
  const auto boarded = resumed.send(20030, {{"mount_unit_id", wagons.front().at("unit_id")},
                                            {"slot", 0}});
  ASSERT_EQ(frames_of(boarded, 56).size(), 1U);
  ASSERT_EQ(frames_of(boarded, 205).size(), 1U);
  EXPECT_EQ(fixture.progress(0).trade_run.goods, goods);
}

// Live 2026-11-02: returning to character selection and picking the character again within
// about three seconds was refused while the previous session was still closing, and the stock
// client could not reconnect afterwards. With an admission wait the same entry is held until
// the previous session releases the character, then admitted.
TEST(NativeWorld, ReconnectWaitsForTheClosingSessionWhenTheListenerAllowsAWait) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }
  std::shared_ptr<PausingRegionCheckpointStore> store;
  VehicleWorldFixture fixture(false, [&](std::shared_ptr<CharacterStateRepository> inner) {
    store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
    return store;
  });
  struct ReleaseCheckpoint {
    std::shared_ptr<PausingRegionCheckpointStore> store;
    ~ReleaseCheckpoint() {
      store->release();
    }
  } release{store};
  OverworldEntry entry;
  fixture.start(0, entry);
  auto destination = kNearIleste;
  destination.x_mm += 1000;
  fixture.client(0).move_to(destination);

  auto &world = fixture.world();
  world.server->listener().set_entry_admission_wait(std::chrono::seconds(10));
  auto &player = world.sessions[0];
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(player.character_id);
  const auto selected = world.auth->select(player.access_token, select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  // The closing session's position settlement pauses, so it keeps the character admitted.
  store->arm(player.character_id, PausingRegionCheckpointStore::Outcome::commit);
  player.client->disconnect();
  ASSERT_TRUE(store->wait_until_blocked());

  auto waiting = std::make_unique<WorldTestClient>(world.server->port());
  waiting->send_key(selected.packet_key());
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const auto connections = fixture.listener().connections();
  EXPECT_TRUE(std::ranges::none_of(connections, [&](const auto &connection) {
    return connection.authenticated && connection.account_id == player.account_id;
  })) << "The entry was admitted while the previous session still held the character";

  store->release();
  const auto response = waiting->read();
  ASSERT_EQ(response.opcode(), 5);

  player.client = std::move(waiting);
  world.enter_after_key_exchange(player);
  VehicleClient resumed(world, player, 3);
  resumed.send(20086, nlohmann::json::object());
  const auto saved = world.repositories.characters->characters(player.account_id, 1).front();
  ASSERT_TRUE(saved.world_position);
  EXPECT_EQ(saved.world_position->x_mm, destination.x_mm);
}

TEST(NativeWorld, WagonClosePublishesDismountBeforeRemovalToObserver) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  OverworldEntry owner_entry;
  OverworldEntry observer_entry;
  fixture.start(0, owner_entry);
  fixture.start(1, observer_entry);
  auto &owner = fixture.client(0);
  auto &observer = fixture.client(1);
  observer.drain();

  const auto ileste = npc_unit_id(owner_entry, 1001021);
  owner.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                     {"yaw", 0.0}});
  owner.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1},
                     {"select_order", 1}});
  owner.send(20105, nlohmann::json::object());
  const auto departure = owner.send(
      20106, {{"add_goods", nlohmann::json::array(
                               {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  const auto wagons = frames_of(departure, kUnitAddOpcode);
  ASSERT_EQ(wagons.size(), 1U);
  const auto wagon_id = wagons.front().at("unit_id");
  const auto visible = frames_of(observer.drain(), kUnitAddOpcode);
  ASSERT_TRUE(std::ranges::any_of(visible, [&](const auto &unit) {
    return unit.at("unit_id") == wagon_id;
  }));

  fixture.world().sessions[0].client->disconnect();
  std::vector<DecodedWorldFrame> received;
  const auto removes_wagon = [&](const DecodedWorldFrame &frame) {
    if (frame.opcode != companion::kUnitRemoveNotification) {
      return false;
    }
    const auto &unit_ids = frame.message.at("unit_ids");
    return std::ranges::find(unit_ids, wagon_id) != unit_ids.end();
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    auto frames = observer.drain();
    received.insert(received.end(), frames.begin(), frames.end());
    const auto removed = std::ranges::any_of(received, removes_wagon);
    if (removed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const auto dismount = std::ranges::find_if(received, [&](const auto &frame) {
    return frame.opcode == companion::kMountUnitNotification &&
           frame.message.at("unit_id") == wagon_id;
  });
  const auto removal = std::ranges::find_if(received, removes_wagon);
  ASSERT_NE(dismount, received.end());
  ASSERT_NE(removal, received.end());
  EXPECT_LT(dismount, removal);
}

void verify_region_with_pending_reward(bool wagon) {
  std::shared_ptr<PausingRegionCheckpointStore> store;
  VehicleWorldFixture fixture(true, [&](std::shared_ptr<CharacterStateRepository> inner) {
    store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
    return store;
  });
  ASSERT_TRUE(store);
  RewardRefusalRelease release(store);
  fixture.make_game_master(0);
  auto &world = fixture.world();
  auto &rider = world.sessions[0];
  OverworldEntry entry;
  OverworldEntry observer_entry;
  const auto started = fixture.start(0, entry);
  fixture.start(1, observer_entry);
  auto &player = fixture.client(0);
  fixture.client(1).drain();

  std::string wagon_unit_id;
  if (wagon) {
    const auto ileste = npc_unit_id(entry, 1001021);
    player.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                        {"yaw", 0.0}});
    player.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1},
                        {"select_order", 1}});
    player.send(20105, nlohmann::json::object());
    const auto departure = player.send(
        20106, {{"add_goods", nlohmann::json::array(
                                  {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
                {"wagon_cms_id", 200}, {"start", true}});
    ASSERT_EQ(frames_of(departure, 119).size(), 1U);
    const auto wagons = frames_of(departure, kUnitAddOpcode);
    ASSERT_EQ(wagons.size(), 1U);
    wagon_unit_id = wagons.front().at("unit_id").get<std::string>();
  }
  const auto goods = fixture.progress(0).trade_run.goods;

  // This authored monster has kill record 545. A server teleport avoids forging a long
  // client movement and then exercises the normal teleport acknowledgement sequence.
  const auto teleport = player.send(20009, {{"text", "tp -1433.205 3433.305 -599.222"}});
  const auto teleport_notices = frames_of(teleport, 181);
  ASSERT_EQ(teleport_notices.size(), 1U);
  const auto teleport_started = player.send(20088, {{"tick", teleport_notices.front().at("tick")}});
  const auto visible = player.send(20089, nlohmann::json::object());
  if (wagon) {
    const auto repeated_board = player.send(20030, {{"mount_unit_id", wagon_unit_id}, {"slot", 0}});
    const auto replies = frames_of(repeated_board, companion::kRideUnitResponse);
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies.front().at("ec"), "Summon_AlreadyRiding");
  }
  auto monsters = frames_of(started, kUnitAddOpcode);
  for (const auto &frame : {teleport, teleport_started}) {
    const auto adds = frames_of(frame, kUnitAddOpcode);
    monsters.insert(monsters.end(), adds.begin(), adds.end());
  }
  const auto moved_monsters = frames_of(visible, kUnitAddOpcode);
  monsters.insert(monsters.end(), moved_monsters.begin(), moved_monsters.end());
  const auto is_target_monster = [](const auto &add) {
    return add.at("nub").contains("npc") &&
           add.at("nub").at("npc").at("npc_cms_id") == 5001202;
  };
  for (int attempt = 0; attempt < 50 &&
                        std::ranges::find_if(monsters, is_target_monster) == monsters.end();
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto adds = frames_of(player.drain(), kUnitAddOpcode);
    monsters.insert(monsters.end(), adds.begin(), adds.end());
  }
  const auto monster = std::ranges::find_if(monsters, is_target_monster);
  ASSERT_NE(monster, monsters.end());
  store->refuse_monster_rewards();
  player.send(20009, {{"text", "monster_kill " + monster->at("unit_id").get<std::string>()}});
  ASSERT_TRUE(store->wait_for_refused_monster_reward());

  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(rider.character_id);
  const auto selected = world.auth->select(rider.access_token, select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  const WorldPosition boundary{9012, -4615500, 994500, -600000, 0.0F};
  player.move_to(boundary);
  player.send_without_barrier(20090, {{"pos", world_pos_json(boundary)}, {"body_yaw", 0.0}});

  // A different player remains responsive while admission for the unresolved defeat stays held.
  fixture.client(1).move_to(kNearIlesteWatcher);
  const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool old_connection_closed = false;
  while (std::chrono::steady_clock::now() < close_deadline) {
    const auto connections = fixture.listener().connections();
    const auto old_connection = std::ranges::find_if(connections, [&](const auto &connection) {
      return connection.authenticated && connection.account_id == rider.account_id;
    });
    if (old_connection == connections.end()) {
      old_connection_closed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(old_connection_closed) << "Pending defeat did not close the original session";
  {
    WorldTestClient busy(world.server->port());
    busy.send_key(selected.packet_key());
    try {
      const auto response = busy.read();
      FAIL() << "Unsettled character unexpectedly entered, opcode " << response.opcode();
    } catch (const boost::system::system_error &error) {
      ASSERT_TRUE(is_server_close(error.code())) << error.what();
    }
  }
  store->lose_next_ready_acknowledgement();
  store->allow_monster_rewards();
  ASSERT_TRUE(store->wait_for_lost_ready_acknowledgement());

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
    } catch (const boost::system::system_error &error) {
      ASSERT_TRUE(is_server_close(error.code())) << error.what();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(reconnected);
  rider.client = std::move(reconnected);
  const auto reentry = world.enter_after_key_exchange(rider);
  EXPECT_EQ(reentry.map_start.at("map_id"), 9012);
  VehicleClient resumed(world, rider, 3);
  resumed.send(20086, nlohmann::json::object());
  const auto progress = fixture.progress(0);
  ASSERT_TRUE(progress.play_records.contains(545));
  EXPECT_EQ(progress.play_records.at(545), 1);
  if (wagon) {
    EXPECT_EQ(progress.trade_run.goods, goods);
    ASSERT_TRUE(progress.trade_run.wagon_position);
    EXPECT_EQ(progress.trade_run.wagon_position->map_id, 9012);
  }
}

TEST(NativeRewardRegion, PendingRewardClosesAndReconnectsOnFoot) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified catalog and NPC placement fixtures";
  }
  verify_region_with_pending_reward(false);
}

TEST(NativeRewardRegion, PendingRewardClosesAndReconnectsWithWagon) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified catalog and NPC placement fixtures";
  }
  verify_region_with_pending_reward(true);
}

TEST(NativeRewardRegion, DungeonBossCreditsOnlyPartyMembersInTheSameCopy) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Requires the verified catalog and world monster profiles";
  }
  constexpr int kDungeon = 10111;
  constexpr int kBoss = 6001101;
  constexpr int kPartyUnlockQuest = 90100011;
  const std::filesystem::path catalog_path(catalog_environment);
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  const WorldPosition field{9012, -1429401, 4056969, -536164, 0.0F};
  const WorldPosition dungeon{kDungeon, field.x_mm, field.y_mm, field.z_mm, 0.0F};
  NpcPlacementSet placements;
  placements.map_id = 9012;
  const auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  OverworldSessions world(population, {field, field, dungeon}, catalog, catalog_path, {},
                          CharacterStats::load(*catalog));

  for (std::size_t index = 0; index < world.sessions.size(); ++index) {
    const auto &session = world.sessions[index];
    const auto before = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    auto after = before;
    ++after.revision;
    after.quests[kPartyUnlockQuest].complete = true;
    ASSERT_TRUE(world.repositories.character_state->save_world_progress(
        session.account_id, session.character_id, 1, before, after));
    if (index < 2) {
      const auto name = "overworld_sessions" + std::to_string(index);
      ASSERT_TRUE(world.auth->set_account_role(name, AccountRole::game_master));
      const auto login = world.auth->login(name, "test-world-password-123");
      world.sessions[index].access_token = login.access_token();
      x5::pb::authwebsocket::WorldEnterReq enter;
      enter.set_world_id(1);
      ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);
    }
  }

  for (auto &session : world.sessions) {
    world.enter(session);
  }
  VehicleClient killer(world, world.sessions[0], 3);
  VehicleClient member(world, world.sessions[1], 3);
  VehicleClient other_copy(world, world.sessions[2], 3);
  killer.send(20086, nlohmann::json::object());
  member.send(20086, nlohmann::json::object());
  const auto outsider_started = other_copy.send(20086, nlohmann::json::object());

  const auto find_boss = [](const std::vector<DecodedWorldFrame> &frames) -> std::string {
    for (const auto &unit : frames_of(frames, kUnitAddOpcode)) {
      if (unit.at("nub").contains("npc") && unit.at("nub").at("npc").at("npc_cms_id") == kBoss) {
        return unit.at("unit_id").get<std::string>();
      }
    }
    return {};
  };
  const auto other_boss = find_boss(outsider_started);
  ASSERT_FALSE(other_boss.empty());

  const auto join_party = [&](VehicleClient &target, std::size_t index) {
    const auto leader_id = std::to_string(world.sessions[0].character_id);
    const auto target_id = std::to_string(world.sessions[index].character_id);
    killer.send(20097, {{"target_char_id", target_id}});
    std::vector<nlohmann::json> invitations;
    for (int attempt = 0; attempt < 50 && invitations.empty(); ++attempt) {
      invitations = frames_of(target.drain(), 188);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(invitations.size(), 1U);
    const auto accepted = target.send(20098, {{"leader_char_id", leader_id},
                                             {"target_char_id", target_id},
                                             {"party_id", invitations.front().at("party_id")}});
    ASSERT_FALSE(frames_of(accepted, 184).empty());
  };
  join_party(member, 1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());

  const auto enter_dungeon = [&](VehicleClient &client) -> std::vector<DecodedWorldFrame> {
    const auto moved = client.send(20009, {{"text", "tp_map 10111"}});
    EXPECT_EQ(frames_of(moved, 175).size(), 1U);
    EXPECT_EQ(frames_of(client.send(20085, nlohmann::json::object()), 176).size(), 1U);
    return client.send(20086, nlohmann::json::object());
  };
  const auto shared_boss = find_boss(enter_dungeon(killer));
  const auto member_boss = find_boss(enter_dungeon(member));
  ASSERT_FALSE(shared_boss.empty());
  ASSERT_EQ(member_boss, shared_boss);
  ASSERT_NE(other_boss, shared_boss);
  join_party(other_copy, 2);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());

  const auto defeated = killer.send(20009, {{"text", "monster_kill " + shared_boss}});
  ASSERT_FALSE(frames_of(defeated, 23).empty());
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto progress = world.repositories.character_state->load_world_progress(
        world.sessions[1].account_id, world.sessions[1].character_id, 1);
    if (progress.play_records.contains(kBoss)) {
      break;
    }
    member.drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  for (std::size_t index = 0; index < world.sessions.size(); ++index) {
    const auto &session = world.sessions[index];
    const auto progress = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    if (index < 2) {
      ASSERT_TRUE(progress.play_records.contains(kBoss));
      EXPECT_EQ(progress.play_records.at(kBoss), 1);
    } else {
      EXPECT_FALSE(progress.play_records.contains(kBoss));
    }
  }
}

} // namespace
} // namespace chronicles
