#include "world/features/companion/tests/native_vehicle_test_support.hpp"
#include "world/features/trade/tests/pausing_region_checkpoint_store.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using namespace native_testing;
using namespace vehicle_native_testing;

class CheckpointRelease {
public:
  explicit CheckpointRelease(std::shared_ptr<PausingRegionCheckpointStore> store)
      : store_(std::move(store)) {
  }

  ~CheckpointRelease() {
    store_->release();
  }

private:
  std::shared_ptr<PausingRegionCheckpointStore> store_;
};

bool is_server_close(const boost::system::error_code &code) {
  return code == asio::error::eof || code == asio::error::connection_reset ||
         code == ssl::error::stream_truncated;
}

void verify_close_during_delayed_checkpoint(PausingRegionCheckpointStore::Outcome outcome) {
  std::shared_ptr<PausingRegionCheckpointStore> store;
  VehicleWorldFixture fixture(true, [&](std::shared_ptr<CharacterStateRepository> inner) {
    store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
    return store;
  });
  ASSERT_TRUE(store);
  CheckpointRelease release(store);
  auto &world = fixture.world();
  auto &rider = world.sessions[0];
  OverworldEntry entry;
  OverworldEntry observer_entry;
  fixture.start(0, entry);
  fixture.start(1, observer_entry);
  auto &trader = fixture.client(0);
  fixture.client(1).drain();

  const auto ileste = npc_unit_id(entry, 1001021);
  trader.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                      {"yaw", 0.0}});
  trader.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1}, {"select_order", 1}});
  trader.send(20105, nlohmann::json::object());
  const auto departure = trader.send(
      20106, {{"add_goods", nlohmann::json::array(
                                {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  ASSERT_EQ(frames_of(departure, 119).size(), 1U);
  const auto goods = fixture.progress(0).trade_run.goods;
  ASSERT_EQ(goods.size(), 2U);

  const WorldPosition boundary{9012, -4615500, 994500, -600000, 0.0F};
  trader.move_to(boundary);
  store->arm(rider.character_id, outcome);
  trader.send_without_barrier(20090, {{"pos", world_pos_json(boundary)}, {"body_yaw", 0.0}});
  ASSERT_TRUE(store->wait_until_blocked());

  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(rider.character_id);
  const auto selected = world.auth->select(rider.access_token, select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  ASSERT_FALSE(selected.packet_key().empty());
  ASSERT_EQ(fixture.listener().disconnect_account(rider.account_id), 1U);

  const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool old_connection_closed = false;
  while (std::chrono::steady_clock::now() < close_deadline) {
    const auto connections = fixture.listener().connections();
    const auto old_connection = std::find_if(connections.begin(), connections.end(),
                                             [&](const auto &connection) {
      return connection.authenticated && connection.account_id == rider.account_id;
    });
    if (old_connection == connections.end()) {
      old_connection_closed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(old_connection_closed) << "The I/O executor did not close the blocked session";
  fixture.client(1).move_to(kNearIlesteWatcher);

  {
    WorldTestClient busy(world.server->port());
    busy.send_key(selected.packet_key());
    try {
      const auto response = busy.read();
      FAIL() << "Busy character unexpectedly entered, opcode " << response.opcode();
    } catch (const boost::system::system_error &error) {
      ASSERT_TRUE(is_server_close(error.code())) << error.what();
    }
  }
  store->release();

  std::unique_ptr<WorldTestClient> reconnected;
  const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < retry_deadline) {
    auto attempt = std::make_unique<WorldTestClient>(world.server->port());
    attempt->send_key(selected.packet_key());
    try {
      const auto response = attempt->read();
      ASSERT_EQ(response.opcode(), 5);
      reconnected = std::move(attempt);
      break;
    } catch (const boost::system::system_error &error) {
      ASSERT_TRUE(is_server_close(error.code())) << error.what();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(reconnected) << "The same ticket was not accepted after cleanup";

  const int expected_map = outcome == PausingRegionCheckpointStore::Outcome::conflict ? 9012 : 9021;
  const auto saved = fixture.progress(0);
  ASSERT_TRUE(saved.trade_run.wagon_position);
  EXPECT_EQ(saved.trade_run.wagon_position->map_id, expected_map);
  EXPECT_EQ(saved.trade_run.goods, goods);
  const auto character = world.repositories.characters->characters(rider.account_id, 1).front();
  ASSERT_TRUE(character.world_position);
  EXPECT_EQ(character.world_position->map_id, expected_map);

  rider.client = std::move(reconnected);
  const auto reentry = world.enter_after_key_exchange(rider);
  EXPECT_EQ(reentry.map_start.at("map_id"), expected_map);
  VehicleClient resumed(world, rider, 3);
  const auto started = resumed.send(20086, nlohmann::json::object());
  // The rider re-enters on foot. TradeStartNoti would black out the client's screen until it
  // rides a vehicle, so entry restores the wagon and its marker and boarding announces the run.
  EXPECT_TRUE(frames_of(started, 205).empty());
  EXPECT_EQ(frames_of(started, 207).size(), 1U);
  const auto wagons = frames_of(started, kUnitAddOpcode);
  ASSERT_EQ(wagons.size(), 1U);

  const auto boarded =
      resumed.send(20030, {{"mount_unit_id", wagons.front().at("unit_id")}, {"slot", 0}});
  ASSERT_EQ(frames_of(boarded, 56).size(), 1U);
  ASSERT_EQ(frames_of(boarded, 205).size(), 1U);
  EXPECT_LT(first_index(boarded, 56), first_index(boarded, 205));
  EXPECT_EQ(fixture.progress(0).trade_run.goods, goods);
  fixture.client(1).move_to(kNearIlesteWatcher);
}

TEST(NativeWorld, TradeRunFromWatchersPostToSeaglassHarborPaysTheRouteReward) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG and CHRONICLES_NPC_PLACEMENTS_9012";
  }
  VehicleWorldFixture fixture;
  OverworldEntry entry;
  OverworldEntry watcher_entry;
  fixture.start(0, entry);
  fixture.start(1, watcher_entry);
  fixture.client(1).drain();
  auto &trader = fixture.client(0);

  const auto ileste = npc_unit_id(entry, 1001021);
  const auto opened = trader.send(20065, {{"target_unit_id", ileste},
                                          {"pos", world_pos_json(kNearIleste)},
                                          {"yaw", 0.0}});
  const auto start_reply = frames_of(opened, 161);
  ASSERT_EQ(start_reply.size(), 1u);
  EXPECT_FALSE(start_reply.front().contains("ec"));
  ASSERT_EQ(frames_of(trader.send(20067, {{"dialogue_cms_id", 100117},
                                          {"sequence_order", 1},
                                          {"select_order", 1}}),
                      162)
                .size(),
            1u);
  ASSERT_EQ(frames_of(trader.send(20105, nlohmann::json::object()), 209).size(), 1u);

  const auto departed = trader.send(
      20106, {{"add_goods", nlohmann::json::array(
                                {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200},
              {"start", true}});
  ASSERT_EQ(frames_of(departed, 169).size(), 1u);
  ASSERT_EQ(frames_of(departed, 119).size(), 1u);
  const auto wagon_adds = frames_of(departed, kUnitAddOpcode);
  ASSERT_EQ(wagon_adds.size(), 1u);
  EXPECT_EQ(wagon_adds.front().at("nub").at("operable").at("vehicle").at("vehicle_cms_id"), 200);
  const auto started = frames_of(departed, 205);
  ASSERT_EQ(started.size(), 1u);
  EXPECT_LT(first_index(departed, kUnitAddOpcode), first_index(departed, 56));
  EXPECT_LT(first_index(departed, 56), first_index(departed, 205));
  EXPECT_EQ(fixture.progress(0).gold, 10000 - 2 * 1000 - 100);
  EXPECT_EQ(frames_of(fixture.client(1).drain(), kUnitAddOpcode).size(), 1u);

  trader.move_to(kPost1000Sphere);
  const auto parked = frames_of(trader.send(20108, {{"trading_post_cms_id", 1000}}), 210);
  ASSERT_EQ(parked.size(), 1u);
  EXPECT_FALSE(parked.front().contains("ec"));
  const auto stowed = trader.send(20109, nlohmann::json::object());
  EXPECT_EQ(frames_of(stowed, 56).size(), 1u);
  EXPECT_EQ(frames_of(stowed, 11).size(), 1u);

  trader.move_to(kNearVida);
  const auto vida = placed_npc_unit_id(1001316);
  ASSERT_EQ(frames_of(trader.send(20065, {{"target_unit_id", vida},
                                          {"pos", world_pos_json(kNearVida)},
                                          {"yaw", 0.0}}),
                      161)
                .size(),
            1u);
  ASSERT_EQ(frames_of(trader.send(20067, {{"dialogue_cms_id", 100097},
                                          {"sequence_order", 1},
                                          {"select_order", 1}}),
                      162)
                .size(),
            1u);

  const auto &goods = started.front().at("trade_goods");
  const auto sold = trader.send(20107, {{"goods", goods}});
  const auto sale = frames_of(sold, 169);
  ASSERT_EQ(sale.size(), 1u);
  const auto &result = sale.front().at("trade_sell").at("results").at(0);
  EXPECT_EQ(result.at("default_reward").at("item_cms_id"), 60007);
  EXPECT_EQ(result.at("default_reward").at("value"), "12");
  EXPECT_EQ(frames_of(sold, 206).size(), 1u);

  const auto progress = fixture.progress(0);
  EXPECT_EQ(progress.stackables.at(60007), 12);
  EXPECT_EQ(progress.trade_run.phase, TradeRunPhase::idle);
}

TEST(NativeWorld, RegionalHandoffReannouncesTheSameDrivenTradeWagon) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG and CHRONICLES_NPC_PLACEMENTS_9012";
  }
  VehicleWorldFixture fixture(true);
  OverworldEntry entry;
  fixture.start(0, entry);
  auto &trader = fixture.client(0);
  const auto ileste = npc_unit_id(entry, 1001021);
  trader.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                      {"yaw", 0.0}});
  trader.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1}, {"select_order", 1}});
  trader.send(20105, nlohmann::json::object());
  const auto departed = trader.send(
      20106, {{"add_goods", nlohmann::json::array(
                                {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  const auto original_wagons = frames_of(departed, kUnitAddOpcode);
  ASSERT_EQ(original_wagons.size(), 1U);
  const auto wagon_id = original_wagons.front().at("unit_id");
  const auto goods = fixture.progress(0).trade_run.goods;
  ASSERT_EQ(goods.size(), 2U);

  const WorldPosition boundary{9012, -4615500, 994500, -600000, 0.0F};
  trader.move_to(boundary);
  const auto moving = trader.send(20090, {{"pos", world_pos_json(boundary)}, {"body_yaw", 0.0}});
  ASSERT_EQ(frames_of(moving, 175).size(), 1U);
  EXPECT_EQ(frames_of(moving, 175).front().at("map_id"), 9021);
  const auto saved = fixture.progress(0).trade_run;
  ASSERT_TRUE(saved.wagon_position);
  EXPECT_EQ(saved.wagon_position->map_id, 9021);
  EXPECT_EQ(saved.goods, goods);

  ASSERT_EQ(frames_of(trader.send(20085, nlohmann::json::object()), 176).size(), 1U);
  const auto arrived = trader.send(20086, nlohmann::json::object());
  ASSERT_EQ(frames_of(arrived, 183).size(), 1U);
  const auto wagon_adds = frames_of(arrived, kUnitAddOpcode);
  const auto wagon = std::ranges::find_if(wagon_adds, [&](const auto &unit) {
    return unit.at("unit_id") == wagon_id;
  });
  ASSERT_NE(wagon, wagon_adds.end());
  const auto mounts = frames_of(arrived, 56);
  ASSERT_EQ(mounts.size(), 1U);
  EXPECT_EQ(mounts.front().at("unit_id"), wagon_id);
  ASSERT_EQ(frames_of(arrived, 205).size(), 1U);
  EXPECT_EQ(fixture.progress(0).trade_run.goods, goods);
}

TEST(NativeWorld, DelayedWagonCheckpointKeepsIoResponsiveAndRetainsTheSameTicket) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG and CHRONICLES_NPC_PLACEMENTS_9012";
  }
  for (const auto outcome : {PausingRegionCheckpointStore::Outcome::commit,
                             PausingRegionCheckpointStore::Outcome::conflict,
                             PausingRegionCheckpointStore::Outcome::lost_acknowledgement}) {
    SCOPED_TRACE(static_cast<int>(outcome));
    verify_close_during_delayed_checkpoint(outcome);
  }
}

} // namespace
} // namespace chronicles
