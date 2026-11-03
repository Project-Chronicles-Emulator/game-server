#include "world/features/character/character_stats.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>
namespace chronicles {
namespace {

using namespace native_testing;

TEST(NativeWorld, NuiaTearsCollectOncePerCharacterAndRestoreOverTls) {
  TemporaryWorldCertificate files;
  const auto root = files.certificate().parent_path();
  std::filesystem::create_directory(root / "nuia-tears");
  const nlohmann::json definitions{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"source_executable_dump_sha256",
       "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6"},
      {"placements",
       {{{"map_id", kOverworldMapId},
         {"unit_id", "123006"},
         {"cms_id", 5010058},
         {"quantity", 1},
         {"position", {kOverworldSpawn.x_mm, kOverworldSpawn.y_mm, kOverworldSpawn.z_mm}}}}}};
  {
    std::ofstream output(root / "nuia-tears/definitions.json");
    output << definitions.dump(2);
    ASSERT_TRUE(output.good());
  }
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn, kOverworldSpawn}, {},
                          root / "catalog");
  const nlohmann::json request{{"spot_object_id", "123006"},
                               {"pos",
                                {{"millimeter_x", kOverworldSpawn.x_mm},
                                 {"millimeter_y", kOverworldSpawn.y_mm},
                                 {"millimeter_z", kOverworldSpawn.z_mm}}}};
  for (auto &session : world.sessions) {
    world.enter(session);
    const auto before = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    auto unrelated_discovery = request;
    unrelated_discovery["spot_object_id"] = "999006";
    session.client->send(world.protocol.encode(20050, unrelated_discovery, 3));
    const auto ignored = world.read_until_pong(session, 4);
    ASSERT_EQ(ignored.size(), 1); // Only the time-sync reply, with no error or fake discovery.
    EXPECT_EQ(world.repositories.character_state->load_world_progress(session.account_id,
                                                                      session.character_id, 1),
              before);

    session.client->send(world.protocol.encode(20050, request, 5));
    const auto discovered = world.read_until_pong(session, 6);
    ASSERT_EQ(discovered.size(), 2);
    EXPECT_EQ(discovered[0].opcode(), 90);
    const auto after_discovery = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    EXPECT_EQ(after_discovery.adventure_objects.at(5010058), 1);
    EXPECT_EQ(after_discovery.stackables, before.stackables);

    session.client->send(world.protocol.encode(20049, request, 7));
    const auto frames = world.read_until_pong(session, 8);
    ASSERT_EQ(frames.size(), 3);
    EXPECT_EQ(frames[0].opcode(), 91);
    ASSERT_EQ(frames[1].opcode(), 169);
    const auto update = world.protocol.decode(frames[1]).at("message");
    EXPECT_EQ(update.at("stackables")[0].at("item_cms_id"), 2000);
    EXPECT_EQ(update.at("stackables")[0].at("quantity"), 1);
    const auto stored = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    EXPECT_EQ(stored.stackables.at(2000), 1);
    EXPECT_EQ(stored.adventure_objects.at(5010058), 2);
    session.client->send(world.protocol.encode(20049, request, 9));
    const auto duplicate = world.read_until_pong(session, 10);
    ASSERT_EQ(duplicate.size(), 2);
    EXPECT_EQ(duplicate[0].opcode(), 91);
  }

  auto &session = world.sessions.front();
  session.client->disconnect();
  // Collection is already committed, but the old connection still owns admission until
  // its asynchronous close completes. The probe performs no character write.
  std::unique_ptr<OfflineCharacterReservation> retired_character;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!retired_character && std::chrono::steady_clock::now() < deadline) {
    retired_character = world.server->listener().reserve_offline_character(session.character_id);
    if (!retired_character) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(retired_character);
  retired_character.reset();

  world.connect(session);
  session.client->send(world.protocol.encode(20001, nlohmann::json::object(), 0));
  const auto reconnect = world.read_until_pong(session, 1);
  const auto saved =
      std::ranges::find_if(reconnect, [](const Frame &frame) { return frame.opcode() == 127; });
  ASSERT_NE(saved, reconnect.end());
  const auto objects = world.protocol.decode(*saved).at("message").at("adventure_objects");
  ASSERT_EQ(objects.size(), 1);
  EXPECT_EQ(objects[0].at("state"), "EX5AdventureObjectState_Activated");
  session.client->send(world.protocol.encode(20085, nlohmann::json::object(), 2));
  world.read_until_pong(session, 3);
  session.client->send(world.protocol.encode(20049, request, 4));
  const auto duplicate = world.read_until_pong(session, 5);
  EXPECT_EQ(
      std::ranges::count_if(duplicate, [](const Frame &frame) { return frame.opcode() == 91; }), 1);
  EXPECT_EQ(
      std::ranges::count_if(duplicate, [](const Frame &frame) { return frame.opcode() == 169; }),
      0);
  for (auto &connected : world.sessions) {
    connected.client->disconnect();
  }
}

TEST(NativeWorld, NuiStatueActivationPersistsAndRestoresWithoutTheTearService) {
  TemporaryWorldCertificate files;
  const auto root = files.certificate().parent_path();
  std::filesystem::create_directory(root / "nui-statues");
  const nlohmann::json point{{"millimeter_x", kOverworldSpawn.x_mm},
                              {"millimeter_y", kOverworldSpawn.y_mm},
                              {"millimeter_z", kOverworldSpawn.z_mm}};
  const nlohmann::json definitions{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"source_executable_dump_sha256",
       "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6"},
      {"interactions", {{"10", {{"execute_time_ms", 1}, {"duration_ms", 6333}}},
                         {"16", {{"execute_time_ms", 1}, {"duration_ms", 4400}}}}},
      {"utility_capacity", {{"40101101", 5}}},
      {"blessing_levels", {{{"level", 1}, {"required_tears", 1}},
                             {{"level", 2}, {"required_tears", 2}},
                             {{"level", 3}, {"required_tears", 3}}}},
      {"flame_only_maps", nlohmann::json::array()},
      {"placements", {{{"map_id", kOverworldMapId}, {"unit_id", "123006"},
                        {"cms_id", 6010006}, {"kind", "statue"},
                        {"position", {kOverworldSpawn.x_mm, kOverworldSpawn.y_mm,
                                      kOverworldSpawn.z_mm}},
                        {"spawn", {kOverworldSpawn.x_mm + 500, kOverworldSpawn.y_mm,
                                   kOverworldSpawn.z_mm}},
                        {"spawn_yaw", 90}, {"restricted_combat_radius_mm", 10000},
                        {"restricted_npc_spawn_radius_mm", 10000}, {"revive_task_id", 0}}}}};
  {
    std::ofstream output(root / "nui-statues/definitions.json");
    output << definitions.dump(2);
    ASSERT_TRUE(output.good());
  }
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn, kOverworldSpawn}, {},
                          root / "catalog");
  auto &session = world.sessions.front();
  world.enter(session);
  const nlohmann::json start{{"interaction_anim_id", 10}, {"target_unit_id", "123006"},
                              {"param", {{"interaction", {{"interaction_id", 10}}}}},
                              {"pos", point}, {"tick", "1"}};
  session.client->send(world.protocol.encode(20069, start, 3));
  const auto started = world.read_until_pong(session, 4);
  ASSERT_TRUE(std::ranges::any_of(started, [](const Frame &frame) {
    return frame.opcode() == 166;
  }));
  session.client->send(world.protocol.encode(20070, {{"interaction_anim_id", 10}}, 5));
  const auto executed = world.read_until_pong(session, 6);
  ASSERT_TRUE(std::ranges::any_of(executed, [](const Frame &frame) {
    return frame.opcode() == 91;
  }));
  EXPECT_EQ(world.repositories.character_state
                ->load_world_progress(session.account_id, session.character_id, 1)
                .adventure_objects.at(6010006),
            2);
  const auto &other = world.sessions.back();
  EXPECT_FALSE(world.repositories.character_state
                   ->load_world_progress(other.account_id, other.character_id, 1)
                   .adventure_objects.contains(6010006));

  session.client->send(world.protocol.encode(
      20071, {{"end_interaction_anim", {{"interaction_anim_id", 10}}}}, 7));
  world.read_until_pong(session, 8);
  const auto before = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  auto with_tears = before;
  ++with_tears.revision;
  with_tears.stackables[2000] = 4;
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      session.account_id, session.character_id, 1, before, with_tears));
  session.client->send(world.protocol.encode(
      20077, {{"item_cms_id", 2000}, {"count", 4}, {"target_unit_id", "123006"}}, 9));
  const auto offered = world.read_until_pong(session, 10);
  EXPECT_EQ(std::ranges::count_if(offered, [](const Frame &frame) {
    return frame.opcode() == 74;
  }), 1);
  const auto blessed = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  EXPECT_EQ(blessed.nui_blessing_level, 2);
  EXPECT_EQ(blessed.dedicated_nui_tears, 1);
  EXPECT_EQ(blessed.stackables.at(2000), 0);
  session.client->disconnect();
  world.connect(session);
  session.client->send(world.protocol.encode(20001, nlohmann::json::object(), 0));
  const auto reconnect = world.read_until_pong(session, 1);
  const auto saved = std::ranges::find_if(reconnect, [](const Frame &frame) {
    return frame.opcode() == 127;
  });
  ASSERT_NE(saved, reconnect.end());
  EXPECT_EQ(world.protocol.decode(*saved).at("message").at("adventure_objects")[0].at("cms_id"),
            6010006);
  session.client->disconnect();
}

TEST(NativeWorld, NuiFlameRevivalCommitsSuppliesOnlyAfterTeleportCompletion) {
  const auto catalog_path =
      std::filesystem::path(CHRONICLES_PROTOCOL_DIR).parent_path() / "data/generated/catalog";
  if (!std::filesystem::exists(catalog_path / "manifest.json")) {
    GTEST_SKIP() << "Requires the verified private catalog for player vitals";
  }
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  auto stats = CharacterStats::load(*catalog);
  TemporaryWorldCertificate files;
  const auto root = files.certificate().parent_path();
  std::filesystem::create_directory(root / "nui-statues");
  std::filesystem::create_directory(root / "world-mobs");
  const nlohmann::json definitions{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"source_executable_dump_sha256",
       "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6"},
      {"interactions", {{"10", {{"execute_time_ms", 5000}, {"duration_ms", 6333}}},
                         {"16", {{"execute_time_ms", 3408}, {"duration_ms", 4400}}}}},
      {"utility_capacity", {{"40101101", 5}}},
      {"revive_tasks", {{"1", {{"supplies", {{{"itemId", 40101101}, {"maxCount", 5}}}},
                                {"refillUtilityBag", true}}}}},
      {"flame_only_maps", nlohmann::json::array()},
      {"placements", {{{"map_id", kOverworldMapId}, {"unit_id", "123006"},
                        {"cms_id", 0}, {"kind", "flame"},
                        {"position", {kOverworldSpawn.x_mm, kOverworldSpawn.y_mm,
                                      kOverworldSpawn.z_mm}},
                        {"spawn", {kOverworldSpawn.x_mm + 500, kOverworldSpawn.y_mm,
                                   kOverworldSpawn.z_mm}},
                        {"spawn_yaw", 90}, {"restricted_combat_radius_mm", 10000},
                        {"restricted_npc_spawn_radius_mm", 10000}, {"revive_task_id", 1}}}}};
  {
    std::ofstream output(root / "nui-statues/definitions.json");
    output << definitions.dump(2);
    ASSERT_TRUE(output.good());
  }
  const nlohmann::json combat{
      {"format_version", 1}, {"skills", nlohmann::json::array()},
      {"effects", nlohmann::json::object()}, {"projectiles", nlohmann::json::object()},
      {"npc_stats", nlohmann::json::object()}, {"npc_skills", nlohmann::json::object()},
      {"weapon_items", nlohmann::json::object()}, {"recoveries", nlohmann::json::object()}};
  {
    std::ofstream output(root / "world-mobs/definitions.json");
    output << nlohmann::json{{"format_version", 1},
                             {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
                             {"combat", combat}, {"maps", nlohmann::json::array()}}.dump(2);
    ASSERT_TRUE(output.good());
  }
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn, kOverworldSpawn}, catalog,
                          root / "catalog", {}, stats);
  auto &session = world.sessions.front();
  const auto before = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  auto prepared = before;
  ++prepared.revision;
  prepared.stackables[40101101] = 1;
  prepared.utility_slots[0] = {40101101, 1};
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      session.account_id, session.character_id, 1, before, prepared));
  ASSERT_TRUE(world.repositories.character_state->commit_world_death(
      session.account_id, session.character_id, 1, prepared.revision, {}));
  const auto dead = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  const auto entry = world.enter(session);
  auto dead_after_entry = dead;
  ++dead_after_entry.revision;
  EXPECT_EQ(entry.map_complete.at("nub").at("player").at("state"), "EX5UnitStateType_Demise");
  session.client->send(world.protocol.encode(20086, nlohmann::json::object(), 3));
  const auto restored_death = world.read_until_pong(session, 4);
  std::vector<int> restored_death_opcodes;
  for (const auto &frame : restored_death) {
    if (frame.opcode() == 32 || frame.opcode() == 23) {
      restored_death_opcodes.push_back(frame.opcode());
    }
    EXPECT_NE(frame.opcode(), 19);
    EXPECT_NE(frame.opcode(), 169);
  }
  EXPECT_EQ(restored_death_opcodes, std::vector<int>({32, 23}));
  EXPECT_EQ(world.repositories.character_state->load_world_progress(session.account_id,
                                                                    session.character_id, 1),
            dead_after_entry);
  session.client->send(world.protocol.encode(20086, nlohmann::json::object(), 5));
  const auto repeated_entry = world.read_until_pong(session, 6);
  EXPECT_FALSE(std::ranges::any_of(repeated_entry, [](const Frame &frame) {
    return frame.opcode() == 32 || frame.opcode() == 23;
  }));
  session.client->send(world.protocol.encode(20039, {{"to_nui_flame", true}}, 7));
  const auto requested = world.read_until_pong(session, 8);
  const auto teleport = std::ranges::find_if(requested, [](const Frame &frame) {
    return frame.opcode() == 181;
  });
  ASSERT_NE(teleport, requested.end());
  EXPECT_TRUE(world.repositories.character_state
                  ->load_world_progress(session.account_id, session.character_id, 1)
                  .dead);
  const auto request = world.protocol.decode(*teleport).at("message");
  session.client->send(world.protocol.encode(20088, {{"tick", request.at("tick")}}, 9));
  world.read_until_pong(session, 10);
  EXPECT_TRUE(world.repositories.character_state
                  ->load_world_progress(session.account_id, session.character_id, 1)
                  .dead);
  session.client->send(world.protocol.encode(20089, nlohmann::json::object(), 11));
  const auto completed = world.read_until_pong(session, 12);
  const auto revived = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  EXPECT_FALSE(revived.dead);
  EXPECT_EQ(revived.revision, dead_after_entry.revision + 1);
  EXPECT_EQ(revived.stackables.at(40101101), 5);
  EXPECT_EQ(revived.utility_slots[0].count, 5);
  EXPECT_TRUE(std::ranges::any_of(completed, [](const Frame &frame) {
    return frame.opcode() == 47;
  }));
  EXPECT_TRUE(std::ranges::any_of(completed, [](const Frame &frame) {
    return frame.opcode() == 169;
  }));
  const auto character = world.repositories.characters->characters(session.account_id, 1).front();
  ASSERT_TRUE(character.world_position);
  EXPECT_EQ(character.world_position->x_mm, kOverworldSpawn.x_mm + 500);
  auto &observer = world.sessions.back();
  const auto observed_entry = world.enter(observer);
  const auto player_id = std::to_string(player_wire_unit_id(session.character_id));
  const auto seen = std::ranges::find_if(observed_entry.unit_adds, [&](const auto &message) {
    return message.at("unit_id") == player_id;
  });
  ASSERT_NE(seen, observed_entry.unit_adds.end());
  const auto &seen_player = seen->at("nub").at("player");
  EXPECT_EQ(seen_player.at("state"), "EX5UnitStateType_Idle");
  EXPECT_GT(seen_player.at("stats").at("69").get<int>(), 0);
  observer.client->disconnect();
  session.client->disconnect();
}

} // namespace
} // namespace chronicles
