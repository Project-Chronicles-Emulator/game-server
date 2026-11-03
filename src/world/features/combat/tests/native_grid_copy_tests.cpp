#include "data/client_catalog.hpp"
#include "world/core/world_sectors.hpp"
#include "world/features/combat/tests/native_combat_test_support.hpp"
#include "world/features/gathering/gathering.hpp"

#include <gtest/gtest.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <string_view>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;
using namespace combat_native_testing;
using Json = nlohmann::json;

constexpr int kTestDungeon = 10111;
const WorldPosition kDungeonPosition{kTestDungeon, -1475237, 3827988, -541088, 0.0F};

std::shared_ptr<const GatheringData> dungeon_doodad_profile() {
  const Json reward{{"pack_rate_percent", 100.0},
                    {"reason", "looting"},
                    {"groups", Json::array({{{"rate_percent", 100.0},
                                              {"items", Json::array({{{"item_cms_id", 70102001},
                                                                       {"min_quantity", 1},
                                                                       {"max_quantity", 1},
                                                                       {"weight", 1}}})}}})}};
  auto herb = reward;
  herb.erase("reason");
  herb.update({{"gathering_id", 80}, {"interaction_anim_id", 1006},
               {"deactivate_delay_ms", 1000}, {"activate_delay_min_ms", 30000},
               {"activate_delay_max_ms", 180000}});
  const Json profile{
      {"format_version", 2},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"maps", Json::array({{{"map_id", kTestDungeon},
                              {"area_min_mm", {kDungeonPosition.x_mm - 100000,
                                               kDungeonPosition.y_mm - 100000,
                                               kDungeonPosition.z_mm - 100000}},
                              {"area_max_mm", {kDungeonPosition.x_mm + 100000,
                                               kDungeonPosition.y_mm + 100000,
                                               kDungeonPosition.z_mm + 100000}}}})},
      {"tool_durability_loss", 100},
      {"sp_regen_delay_ms", 300},
      {"animations", Json::array({{{"interaction_anim_id", 1006}, {"sp_cost", 50},
                                    {"tool_equipment_type", "None"}, {"execute_time_ms", 50},
                                    {"duration_ms", 100}}})},
      {"gatherings", Json::array({herb})},
      {"unsupported_gatherings", Json::array()},
      {"tool_items", Json::object()},
      {"points", Json::array({{4784280, 23, 80, kDungeonPosition.x_mm + 800,
                                kDungeonPosition.y_mm, kDungeonPosition.z_mm}})},
      {"doodads", Json::array({{{"doodad_id", 74}, {"initial_phase", 1},
                                  {"respawn_delay_min_ms", 60000},
                                  {"respawn_delay_max_ms", 60000},
                                  {"phases", Json::array({{{"phase_id", 1},
                                                            {"trigger", "interaction"},
                                                            {"interaction_id", 20001},
                                                            {"interaction_anim_id", 1006},
                                                            {"reward", reward},
                                                            {"next", {{0, 1}}}}})}}})},
      {"doodad_points", Json::array({{4735150, 9, 74, kDungeonPosition.x_mm,
                                       kDungeonPosition.y_mm + 900,
                                       kDungeonPosition.z_mm}})}};
  return std::make_shared<const GatheringData>(profile);
}

// Only the selected worker waits; other log calls must remain nonblocking so the
// other party member can mutate the same copy while the map-load frame is pending.
class MapLoadPauseSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
  bool wait_until_paused() {
    return entered_future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }

  void release() {
    if (!released_.exchange(true)) {
      release_.set_value();
    }
  }

  void wait_until_released() {
    if (paused_.load()) {
      exited_future_.wait_for(std::chrono::seconds(5));
    }
  }

protected:
  void sink_it_(const spdlog::details::log_msg &message) override {
    const std::string_view text(message.payload.data(), message.payload.size());
    if (text.find("connection=2 opcode=20085") == std::string_view::npos ||
        paused_.exchange(true)) {
      return;
    }
    entered_.set_value();
    release_future_.wait_for(std::chrono::seconds(15));
    exited_.set_value();
  }

  void flush_() override {
  }

private:
  std::atomic<bool> paused_{};
  std::atomic<bool> released_{};
  std::promise<void> entered_;
  std::future<void> entered_future_{entered_.get_future()};
  std::promise<void> exited_;
  std::future<void> exited_future_{exited_.get_future()};
  std::promise<void> release_;
  std::shared_future<void> release_future_{release_.get_future().share()};
};

class ScopedMapLoadPause final {
public:
  ScopedMapLoadPause()
      : previous_(spdlog::default_logger()), sink_(std::make_shared<MapLoadPauseSink>()) {
    auto logger = std::make_shared<spdlog::logger>("grid-copy-pause", sink_);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
  }

  ~ScopedMapLoadPause() {
    sink_->release();
    sink_->wait_until_released();
    spdlog::set_default_logger(std::move(previous_));
  }

  MapLoadPauseSink &sink() {
    return *sink_;
  }

private:
  std::shared_ptr<spdlog::logger> previous_;
  std::shared_ptr<MapLoadPauseSink> sink_;
};

TEST(NativeGridCopy, MissingMapGridRebuildsQueuedCatchupForTheCorrectCopy) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Requires the verified generated catalog and travel profile";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  const auto grid = WorldSectorGrid::load(*catalog);
  ASSERT_TRUE(grid);
  ASSERT_TRUE(grid->has_map(kOverworldMapId));
  ASSERT_FALSE(grid->has_map(kTestDungeon));

  NpcPlacementSet empty_population;
  empty_population.map_id = kOverworldMapId;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  OverworldSessions world(population, {kOverworldSpawn, kOverworldSpawn, kDungeonPosition},
                          catalog, catalog_path, {}, CharacterStats::load(*catalog), {},
                          dungeon_doodad_profile());
  for (std::size_t index = 0; index < world.sessions.size(); ++index) {
    auto &session = world.sessions[index];
    const auto before = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    auto after = before;
    ++after.revision;
    after.quests[90100011].complete = true;
    ASSERT_TRUE(world.repositories.character_state->save_world_progress(
        session.account_id, session.character_id, 1, before, after));
    if (index < 2) {
      const auto name = "overworld_sessions" + std::to_string(index);
      ASSERT_TRUE(world.auth->set_account_role(name, AccountRole::game_master));
      const auto login = world.auth->login(name, "test-world-password-123");
      session.access_token = login.access_token();
      x5::pb::authwebsocket::WorldEnterReq enter;
      enter.set_world_id(1);
      ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);
      world.enter(session);
    }
  }

  VehicleClient actor(world, world.sessions[0], 3);
  VehicleClient member(world, world.sessions[1], 3);
  actor.send(20086, Json::object());
  member.send(20086, Json::object());
  const auto leader = std::to_string(world.sessions[0].character_id);
  const auto member_id = std::to_string(world.sessions[1].character_id);
  actor.send(20097, {{"target_char_id", member_id}});
  std::vector<Json> invitations;
  for (int attempt = 0; attempt < 50 && invitations.empty(); ++attempt) {
    invitations = frames_of(member.drain(), 188);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(invitations.size(), 1U);
  const auto joined = member.send(20098, {{"leader_char_id", leader},
                                          {"target_char_id", member_id},
                                          {"party_id", invitations.front().at("party_id")}});
  ASSERT_FALSE(frames_of(joined, 184).empty());

  const std::string teleport = "tp_map 10111 -1475.237 3827.988 -541.088";
  ASSERT_EQ(frames_of(actor.send(20009, {{"text", teleport}}), 175).size(), 1U);
  ASSERT_EQ(frames_of(actor.send(20085, Json::object()), 176).size(), 1U);
  actor.send(20086, Json::object());
  ASSERT_EQ(frames_of(member.send(20009, {{"text", teleport}}), 175).size(), 1U);

  ScopedMapLoadPause pause;
  member.send_without_barrier(20085, Json::object());
  ASSERT_TRUE(pause.sink().wait_until_paused());
  const auto unit = std::to_string(doodad_unit_id(4735150, 9));
  const Json position{{"millimeter_x", kDungeonPosition.x_mm},
                      {"millimeter_y", kDungeonPosition.y_mm},
                      {"millimeter_z", kDungeonPosition.z_mm}};
  const auto started = actor.send(20069, {{"interaction_anim_id", 1006},
                                          {"target_unit_id", unit},
                                          {"param", {{"interaction", {{"interaction_id", 20001}}}}},
                                          {"pos", position}, {"yaw", 0.0}});
  ASSERT_FALSE(frames_of(started, 166).empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(75));
  const auto executed = actor.send(20070, {{"interaction_anim_id", 1006}});
  ASSERT_FALSE(frames_of(executed, 194).empty());
  pause.sink().release();
  const auto catchup = member.drain();
  EXPECT_TRUE(std::ranges::any_of(frames_of(catchup, 194), [&](const Json &message) {
    return message.value("unit_id", std::string{}) == unit && message.value("phase_id", -1) == 0;
  }));

  auto &outsider = world.sessions[2];
  world.connect(outsider);
  outsider.client->send(world.protocol.encode(20001, Json::object(), 0));
  world.read_until_pong(outsider, 1);
  outsider.client->send(world.protocol.encode(20085, Json::object(), 2));
  const auto outsider_frames = world.read_until_pong(outsider, 3);
  EXPECT_FALSE(std::ranges::any_of(outsider_frames, [&](const Frame &frame) {
    if (frame.opcode() != 194) {
      return false;
    }
    const auto message = world.protocol.decode(frame).at("message");
    return message.value("unit_id", std::string{}) == unit && message.value("phase_id", -1) == 0;
  }));
}

} // namespace
} // namespace chronicles
