#include "testing/catalog_test_support.hpp"
#include "world/features/gathering/gathering.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;

// A herb placement and a Roseberry Bush doodad (74) next to the overworld spawn. The synthetic
// 50 ms execute time keeps the real-socket test fast; the server deadline rule is unchanged.
std::shared_ptr<const GatheringData> overworld_gathering_profile() {
  constexpr int kAreaHalfExtentMm = 100000;
  const nlohmann::json loot{
      {"pack_rate_percent", 100.0},
      {"groups", nlohmann::json::array({{{"rate_percent", 100.0},
                                         {"items", nlohmann::json::array({{{"item_cms_id", 70102001},
                                                                           {"min_quantity", 5},
                                                                           {"max_quantity", 7},
                                                                           {"weight", 1}}})}}})}};
  auto herb = loot;
  herb.update({{"gathering_id", 80},
               {"interaction_anim_id", 1006},
               {"deactivate_delay_ms", 1000},
               {"activate_delay_min_ms", 30000},
               {"activate_delay_max_ms", 180000}});
  auto bush_reward = loot;
  bush_reward["reason"] = "looting";
  const nlohmann::json profile{
      {"format_version", 2},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"maps", nlohmann::json::array({{{"map_id", kOverworldMapId},
                                       {"area_min_mm",
                                        {kOverworldSpawn.x_mm - kAreaHalfExtentMm,
                                         kOverworldSpawn.y_mm - kAreaHalfExtentMm,
                                         kOverworldSpawn.z_mm - kAreaHalfExtentMm}},
                                       {"area_max_mm",
                                        {kOverworldSpawn.x_mm + kAreaHalfExtentMm,
                                         kOverworldSpawn.y_mm + kAreaHalfExtentMm,
                                         kOverworldSpawn.z_mm + kAreaHalfExtentMm}}}})},
      {"tool_durability_loss", 100},
      {"sp_regen_delay_ms", 300},
      {"animations", nlohmann::json::array({{{"interaction_anim_id", 1006},
                                             {"sp_cost", 50},
                                             {"tool_equipment_type", "None"},
                                             {"execute_time_ms", 50},
                                             {"duration_ms", 100}}})},
      {"gatherings", nlohmann::json::array({herb})},
      {"unsupported_gatherings", nlohmann::json::array()},
      {"tool_items", nlohmann::json::object()},
      {"points", nlohmann::json::array({{4784280, 23, 80, kOverworldSpawn.x_mm + 800,
                                         kOverworldSpawn.y_mm, kOverworldSpawn.z_mm}})},
      {"doodads", nlohmann::json::array({{{"doodad_id", 74},
                                          {"initial_phase", 1},
                                          {"respawn_delay_min_ms", 60000},
                                          {"respawn_delay_max_ms", 60000},
                                          {"phases", nlohmann::json::array(
                                                         {{{"phase_id", 1},
                                                           {"trigger", "interaction"},
                                                           {"interaction_id", 20001},
                                                           {"interaction_anim_id", 1006},
                                                           {"reward", bush_reward},
                                                           {"next", {{0, 1}}}}})}}})},
      {"doodad_points", nlohmann::json::array({{4735150, 9, 74, kOverworldSpawn.x_mm,
                                                kOverworldSpawn.y_mm + 900,
                                                kOverworldSpawn.z_mm}})}};
  return std::make_shared<const GatheringData>(profile);
}

// Live trial 2026-10-27: pressing F at a Roseberry Bush either did nothing (the doodad's
// interaction 20001 had no handler) or started an animation that never executed, because the
// StartInteractionAnimNoti named the raw character ID with a 100 ns tick. Official captures
// name the wire unit, echo the request, and commit rewards under the next progress revision.
// Opcodes: 20069/20070/20071 Start/Execute/EndInteraction, 166 StartInteractionAnimNoti,
// 167 InteractionAnimRes, 169 ItemsUpdateNoti, 194 ChangeDoodadPhaseNoti, 196 ChangeGathering.
TEST(NativeWorld, GatheringPlacementsAndDoodadsRewardTheWireUnitAndShareStateOverTls) {
  const std::vector<int> gathering_opcodes{165, 166, 167, 169, 194, 196};
  testing_support::TestCatalog fixture({});
  const auto quest_directory = fixture.root() / "quests";
  std::filesystem::create_directories(quest_directory);
  const nlohmann::json doodad_quest{
      {"cms_id", 501},
      {"type", "None"},
      {"accept", {{{"kind", "NpcId"}, {"npc_id", 1001539}}}},
      {"conditions", nlohmann::json::array()},
      {"completion", {{"kind", "Npc"}, {"npc_ids", {1001539}}}},
      {"supplies", nlohmann::json::array()},
      {"auto_complete", false},
      {"sequential", false},
      {"progresses", nlohmann::json::array({{{"sequence", 1},
                                               {"group", 1},
                                               {"kind", "InteractDoodad"},
                                               {"parameters", {{"doodadId", 74},
                                                               {"targetPhase", 1},
                                                               {"count", 1}}}}})},
      {"experience", 0},
      {"rewards", nlohmann::json::array()},
      {"dependent_items", nlohmann::json::array()},
      {"unsupported", nlohmann::json::array()},
  };
  const nlohmann::json catalog{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"map_worlds", {{std::to_string(kOverworldMapId), "ArcheWorld"}}},
      {"quests", {doodad_quest}},
  };
  {
    std::ofstream output(quest_directory / "catalog.json");
    ASSERT_TRUE(output);
    output << catalog;
  }
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn, kOverworldSpawn}, {},
                          fixture.root() / "client", {}, {}, {}, overworld_gathering_profile());
  auto &gatherer = world.sessions.at(0);
  auto &observer = world.sessions.at(1);
  const auto initial = world.repositories.character_state->load_world_progress(
      gatherer.account_id, gatherer.character_id, 1);
  auto accepted = initial;
  ++accepted.revision;
  accepted.quests[501] = QuestProgress{};
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      gatherer.account_id, gatherer.character_id, 1, initial, accepted));
  world.enter(gatherer);
  world.enter(observer);
  const auto gatherer_unit = std::to_string(player_wire_unit_id(gatherer.character_id));
  const auto revision = world.repositories.character_state
                            ->load_world_progress(gatherer.account_id, gatherer.character_id, 1)
                            .revision;

  const nlohmann::json request_position{{"millimeter_x", kOverworldSpawn.x_mm},
                                        {"millimeter_y", kOverworldSpawn.y_mm},
                                        {"millimeter_z", kOverworldSpawn.z_mm}};
  const nlohmann::json herb_start{
      {"interaction_anim_id", 1006},
      {"target_unit_id", std::to_string(static_cast<std::uint64_t>(4784280) << 32)},
      {"param", {{"gathering", {{"indices", {23}}}}}},
      {"pos", request_position},
      {"yaw", -92.5}};
  std::uint32_t sequence = 3;
  gatherer.client->send(world.protocol.encode(20069, herb_start, sequence++));
  auto own = decoded_frames(world.protocol, world.read_until_pong(gatherer, sequence++),
                            gathering_opcodes);
  ASSERT_EQ(own.size(), 1);
  EXPECT_EQ(own.at(0).at("opcode"), 166);
  EXPECT_EQ(own.at(0).at("message").at("unit_id"), gatherer_unit);
  EXPECT_EQ(own.at(0).at("message").at("pos"), request_position);

  gatherer.client->send(world.protocol.encode(20070, {{"interaction_anim_id", 1006}}, sequence++));
  own = decoded_frames(world.protocol, world.read_until_pong(gatherer, sequence++),
                       gathering_opcodes);
  ASSERT_EQ(own.size(), 3);
  EXPECT_EQ(own.at(0).at("opcode"), 169);
  const int herbs = own.at(0).at("message").at("stackables").at(0).at("quantity");
  EXPECT_GE(herbs, 5);
  EXPECT_LE(herbs, 7);
  EXPECT_EQ(own.at(1).at("opcode"), 196);
  EXPECT_EQ(own.at(2).at("opcode"), 167);
  EXPECT_EQ(own.at(2).at("message").at("unit_id"), gatherer_unit);
  const auto stored = world.repositories.character_state->load_world_progress(
      gatherer.account_id, gatherer.character_id, 1);
  EXPECT_EQ(stored.stackables.at(70102001), herbs);
  EXPECT_EQ(stored.revision, revision + 1);

  gatherer.client->send(world.protocol.encode(
      20071, {{"end_interaction_anim", {{"interaction_anim_id", 1006}}}}, sequence++));
  own = decoded_frames(world.protocol, world.read_until_pong(gatherer, sequence++),
                       gathering_opcodes);
  ASSERT_EQ(own.size(), 1);
  EXPECT_EQ(own.at(0).at("opcode"), 167);

  // The nearby player saw the animation start, the depletion and both results.
  const auto seen = decoded_frames(world.protocol, world.read_until_pong(observer, 3),
                                   gathering_opcodes);
  const auto count_seen = [&](int opcode) {
    return std::ranges::count_if(seen, [&](const nlohmann::json &frame) {
      return frame.at("opcode") == opcode &&
             (opcode == 196 || frame.at("message").value("unit_id", "") == gatherer_unit);
    });
  };
  EXPECT_EQ(count_seen(166), 1);
  EXPECT_EQ(count_seen(196), 1);
  EXPECT_EQ(count_seen(167), 2);

  const auto bush_unit = std::to_string(doodad_unit_id(4735150, 9));
  const nlohmann::json bush_start{{"interaction_anim_id", 1006},
                                  {"target_unit_id", bush_unit},
                                  {"param", {{"interaction", {{"interaction_id", 20001}}}}},
                                  {"pos", request_position},
                                  {"yaw", 25.0}};
  gatherer.client->send(world.protocol.encode(20069, bush_start, sequence++));
  own = decoded_frames(world.protocol, world.read_until_pong(gatherer, sequence++),
                       gathering_opcodes);
  ASSERT_EQ(own.size(), 1);
  EXPECT_EQ(own.at(0).at("opcode"), 166);
  gatherer.client->send(world.protocol.encode(20070, {{"interaction_anim_id", 1006}}, sequence++));
  own = decoded_frames(world.protocol, world.read_until_pong(gatherer, sequence++),
                       gathering_opcodes);
  ASSERT_EQ(own.size(), 3);
  EXPECT_EQ(own.at(0).at("opcode"), 169);
  EXPECT_EQ(own.at(0).at("message").at("looting").at("target_unit_id"), bush_unit);
  EXPECT_EQ(own.at(1).at("opcode"), 194);
  EXPECT_EQ(own.at(1).at("message").at("unit_id"), bush_unit);
  EXPECT_EQ(own.at(1).at("message").at("phase_id"), 0);
  EXPECT_EQ(own.at(2).at("opcode"), 167);
  const auto after_bush = world.repositories.character_state->load_world_progress(
      gatherer.account_id, gatherer.character_id, 1);
  EXPECT_EQ(after_bush.quests.at(501).objectives.at(1), 1);

  // A later map load restores both depletions without sector streaming.
  observer.client->disconnect();
  x5::pb::ws::front::CharSelectReq reconnect_request;
  reconnect_request.set_char_id(observer.character_id);
  const auto reconnect_selection = world.auth->select(observer.access_token, reconnect_request);
  ASSERT_EQ(reconnect_selection.ec(), x5::ec::Success);

  // A closed key exchange can precede completion of the previous session's close. Retry the
  // same selected ticket for a bounded period; an eventual failure remains a test failure.
  constexpr auto kReconnectTimeout = std::chrono::seconds(10);
  constexpr auto kReconnectRetryDelay = std::chrono::milliseconds(100);
  const auto reconnect_deadline = std::chrono::steady_clock::now() + kReconnectTimeout;
  int closed_key_exchanges = 0;
  for (;;) {
    observer.client = std::make_unique<WorldTestClient>(world.server->port());
    observer.client->send_key(reconnect_selection.packet_key());
    try {
      const auto response = observer.client->read();
      testing::Test::RecordProperty("closed_key_exchanges", closed_key_exchanges);
      ASSERT_LT(std::chrono::steady_clock::now(), reconnect_deadline)
          << "Observer reconnect succeeded after the deadline";
      ASSERT_EQ(response.opcode(), 5);
      break;
    } catch (const boost::system::system_error &error) {
      const auto code = error.code();
      if (code != asio::error::eof && code != asio::error::connection_reset &&
          code != ssl::error::stream_truncated) {
        throw;
      }

      ++closed_key_exchanges;
      observer.client.reset();
      if (std::chrono::steady_clock::now() >= reconnect_deadline) {
        testing::Test::RecordProperty("closed_key_exchanges", closed_key_exchanges);
        FAIL() << "Observer reconnect did not succeed within the deadline after "
               << closed_key_exchanges << " closed key exchanges: " << error.what();
      }
      std::this_thread::sleep_until(std::min(
          reconnect_deadline, std::chrono::steady_clock::now() + kReconnectRetryDelay));
      if (std::chrono::steady_clock::now() >= reconnect_deadline) {
        testing::Test::RecordProperty("closed_key_exchanges", closed_key_exchanges);
        FAIL() << "Observer reconnect did not succeed within the deadline after "
               << closed_key_exchanges << " closed key exchanges";
      }
    }
  }
  observer.client->send(world.protocol.encode(20001, nlohmann::json::object(), 0));
  world.read_until_pong(observer, 1);
  observer.client->send(world.protocol.encode(20085, nlohmann::json::object(), 2));
  const auto restored = decoded_frames(world.protocol, world.read_until_pong(observer, 3),
                                       gathering_opcodes);
  const auto restored_opcode = [&](int opcode) {
    return std::ranges::any_of(restored, [&](const nlohmann::json &frame) {
      return frame.at("opcode") == opcode;
    });
  };
  EXPECT_TRUE(restored_opcode(196));
  EXPECT_TRUE(restored_opcode(194));

  gatherer.client->disconnect();
  observer.client->disconnect();
}

} // namespace
} // namespace chronicles
