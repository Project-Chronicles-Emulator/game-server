#include "world/session/tests/native_world_test_support.hpp"

#include <future>

namespace chronicles {
namespace {

using namespace native_testing;

nlohmann::json escape_candidate(const WorldPosition &position) {
  const nlohmann::json candidate{{"millimeter_x", position.x_mm},
                                 {"millimeter_y", position.y_mm},
                                 {"millimeter_z", position.z_mm}};
  return {{"candidate_pos", candidate}};
}

TEST(NativeWorld, OverworldEscapeTeleportsToTheCandidateAndSavesTheDestination) {
  // 5,001 mm east of the spawn is just beyond EscapeCandidateMaxDistance; the destination is
  // about 1.8 m away, like the captured candidate.
  const WorldPosition too_far{kOverworldMapId, kOverworldSpawn.x_mm + 5001, kOverworldSpawn.y_mm,
                              kOverworldSpawn.z_mm, kOverworldSpawn.yaw};
  const WorldPosition destination{kOverworldMapId, kOverworldSpawn.x_mm + 1500,
                                  kOverworldSpawn.y_mm - 1000, kOverworldSpawn.z_mm + 60,
                                  kOverworldSpawn.yaw};
  const std::vector<int> escape_opcodes{181, 49, 51, 33};

  OverworldSessions world(small_overworld_population(), {kOverworldSpawn});
  auto &session = world.sessions.front();
  const auto entry = world.enter(session);
  expect_overworld_entry(entry, 3);
  std::uint32_t sequence = 3;

  session.client->send(world.protocol.encode(20048, escape_candidate(too_far), sequence++));
  auto frames = world.read_until_pong(session, sequence++);
  EXPECT_TRUE(decoded_frames(world.protocol, frames, escape_opcodes).empty());

  session.client->send(world.protocol.encode(20048, escape_candidate(destination), sequence++));
  frames = world.read_until_pong(session, sequence++);
  const auto requested = decoded_frames(world.protocol, frames, escape_opcodes);
  ASSERT_EQ(requested.size(), 2);
  EXPECT_EQ(requested.at(0).at("opcode"), 181);
  const auto &teleport = requested.at(0).at("message");
  EXPECT_EQ(teleport.at("pos").at("millimeter_x"), destination.x_mm);
  EXPECT_EQ(teleport.at("pos").at("millimeter_y"), destination.y_mm);
  EXPECT_EQ(teleport.at("pos").at("millimeter_z"), destination.z_mm);
  EXPECT_EQ(requested.at(1).at("opcode"), 49);
  EXPECT_EQ(requested.at(1).at("message").at("status_effect").at("status_effect_id"), 50030003);

  session.client->send(world.protocol.encode(20088, {{"tick", teleport.at("tick")}}, sequence++));
  frames = world.read_until_pong(session, sequence++);
  const auto started = decoded_frames(world.protocol, frames, escape_opcodes);
  ASSERT_EQ(started.size(), 1);
  EXPECT_EQ(started.at(0).at("message").at("attributes").at("2"), 1);

  session.client->send(world.protocol.encode(20089, nlohmann::json::object(), sequence++));
  frames = world.read_until_pong(session, sequence++);
  std::vector<int> completed_opcodes;
  for (const auto &frame : decoded_frames(world.protocol, frames, escape_opcodes)) {
    completed_opcodes.push_back(frame.at("opcode").get<int>());
  }
  EXPECT_EQ(completed_opcodes, (std::vector<int>{49, 33, 51}));

  // EscapeCooltimeSec: a second escape inside the 300 s cooldown sends nothing.
  session.client->send(world.protocol.encode(20048, escape_candidate(destination), sequence++));
  frames = world.read_until_pong(session, sequence++);
  EXPECT_TRUE(decoded_frames(world.protocol, frames, escape_opcodes).empty());

  // The completed teleport is the authoritative position saved when the connection closes.
  session.client->disconnect();
  std::optional<WorldPosition> saved;
  for (int attempt = 0; attempt < 100 && saved != destination; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (const auto &character : world.repositories.characters->characters(session.account_id, 1)) {
      if (character.id == session.character_id) {
        saved = character.world_position;
      }
    }
  }
  EXPECT_EQ(saved, destination);
}

TEST(NativeWorld, PlayerUnstuckReloadsAServerSelectedShrineAndEnforcesCooldown) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  const auto *tutorial_path = std::getenv("CHRONICLES_CLIFFS_TUTORIAL_PROFILE");
  if (!catalog_path || !tutorial_path) {
    GTEST_SKIP() << "Requires the verified catalog, raw map export and tutorial profile";
  }

  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn}, catalog, catalog_path,
                          TutorialData::load(tutorial_path), CharacterStats::load(*catalog));
  auto &session = world.sessions.front();
  const auto entry = world.enter(session);
  ASSERT_EQ(entry.map_start.at("map_id"), kOverworldMapId);
  VehicleClient client(world, session, 3);
  client.send(20086, nlohmann::json::object());

  const ChatIdentity identity{1, session.character_id, "Overworld0"};
  const auto run_command = [&] {
    auto promise = std::make_shared<std::promise<std::string>>();
    auto reply = promise->get_future();
    const bool queued = world.server->listener().player_unstuck(
        identity, [promise](std::string message) { promise->set_value(std::move(message)); });
    EXPECT_TRUE(queued);
    if (reply.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
      ADD_FAILURE() << "Unstuck command did not complete";
      return std::string{};
    }
    return reply.get();
  };

  const auto progress = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  auto trading = progress;
  trading.revision += 1;
  trading.trade_run.phase = TradeRunPhase::loading;
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      session.account_id, session.character_id, 1, progress, trading));
  EXPECT_EQ(run_command(), "Finish your trade run before using !unstuck.");
  auto finished_trade = trading;
  finished_trade.revision += 1;
  finished_trade.trade_run.phase = TradeRunPhase::idle;
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      session.account_id, session.character_id, 1, trading, finished_trade));

  ASSERT_EQ(run_command(), "Moving you to a safe location.");
  auto requested = frames_of(client.drain(), 175);
  for (int attempt = 0; requested.empty() && attempt < 4; ++attempt) {
    requested = frames_of(client.drain(), 175);
  }
  ASSERT_EQ(requested.size(), 1);
  EXPECT_EQ(requested.front().at("map_id"), kOverworldMapId);
  EXPECT_NE(requested.front().at("spawn_point").at("pos").at("millimeter_x"),
            kOverworldSpawn.x_mm);
  EXPECT_EQ(frames_of(client.send(20085, nlohmann::json::object()), 176).size(), 1);
  EXPECT_EQ(frames_of(client.send(20086, nlohmann::json::object()), 183).size(), 1);
  EXPECT_NE(run_command().find("cooling down"), std::string::npos);
}

TEST(NativeWorld, PlayerUnstuckLeavesAnInstanceWithoutALocalShrine) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  const auto *tutorial_path = std::getenv("CHRONICLES_CLIFFS_TUTORIAL_PROFILE");
  if (!catalog_path || !tutorial_path) {
    GTEST_SKIP() << "Requires the verified catalog, raw map export and tutorial profile";
  }

  const std::filesystem::path root(catalog_path);
  std::ifstream input(root.parent_path() / "world-mobs/definitions.json");
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(profile.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == 10119;
  });
  ASSERT_NE(map, profile.at("maps").end());
  ASSERT_FALSE(map->at("spawns").empty());
  const auto &point = map->at("spawns").front().at("position");
  const WorldPosition position{10119, point.at(0), point.at(1), point.at(2), 0};
  NpcPlacementSet empty_population;
  empty_population.map_id = 10119;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  auto catalog = std::make_shared<ClientCatalog>(root);
  OverworldSessions world(population, {position}, catalog, root, TutorialData::load(tutorial_path),
                          CharacterStats::load(*catalog));
  auto &session = world.sessions.front();
  ASSERT_EQ(world.enter(session).map_start.at("map_id"), 10119);
  VehicleClient client(world, session, 3);
  client.send(20086, nlohmann::json::object());

  const ChatIdentity identity{1, session.character_id, "Overworld0"};
  auto promise = std::make_shared<std::promise<std::string>>();
  auto reply = promise->get_future();
  ASSERT_TRUE(world.server->listener().player_unstuck(
      identity, [promise](std::string message) { promise->set_value(std::move(message)); }));
  ASSERT_EQ(reply.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_EQ(reply.get(), "Moving you to a safe location.");
  auto moves = frames_of(client.drain(), 175);
  for (int attempt = 0; moves.empty() && attempt < 4; ++attempt) {
    moves = frames_of(client.drain(), 175);
  }
  ASSERT_EQ(moves.size(), 1);
  EXPECT_EQ(moves.front().at("map_id"), 9012);
}

} // namespace
} // namespace chronicles
