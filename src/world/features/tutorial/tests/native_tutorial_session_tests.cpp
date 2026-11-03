#include "common/clock_ticks.hpp"
#include "world/features/mail/tutorial_completion_mail.hpp"
#include "world/features/quests/world_quests.hpp"
#include "world/features/tutorial/tutorial.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdlib>
#include <future>
#include <thread>

namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace native_testing;

TEST(NativeWorld, TutorialTlsPickupPersistsAndCharactersRemainInstanced) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the generated local tutorial profile";
  }
  const auto definitions = TutorialData::load(profile_path);
  const auto catalog_root = std::filesystem::path(profile_path).parent_path().parent_path() /
                            "catalog";
  auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  const auto stats = CharacterStats::load(*catalog);
  const auto &object = definitions->document.at("objects").at(0);
  const auto &point = object.at("position");
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto prepare = [&](const std::string &account, const std::string &name) {
    if (!auth.create_account(account, "tutorial-world-fixture-password")) {
      throw std::runtime_error("Cannot create tutorial fixture account");
    }
    const auto login = auth.login(account, "tutorial-world-fixture-password");
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(1);
    auth.enter(login.access_token(), enter);
    Character character;
    character.account_id = login.account_id();
    character.name = name;
    character.world_position = WorldPosition{8000, point.at(0), point.at(1), point.at(2), 0};
    character = repositories.characters->create(character);
    repositories.characters->save_world_position(character.account_id, character.id, 1,
                                                 *character.world_position);
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(character.id);
    return std::pair{login.access_token(), auth.select(login.access_token(), select)};
  };
  const auto first = prepare("tutorial_tls_first", "TutorialFirst");
  const auto second = prepare("tutorial_tls_second", "TutorialSecond");
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, definitions, stats, catalog, catalog_root);
  const auto enter = [&](WorldTestClient &client, const auto &selected) {
    client.send_key(selected.packet_key());
    EXPECT_EQ(client.read().opcode(), 5);
    client.send(protocol.encode(20001, nlohmann::json::object(), 0));
    EXPECT_EQ(client.read().opcode(), 175);
    EXPECT_EQ(client.read().opcode(), 1);
    EXPECT_EQ(client.read().opcode(), 125);
    read_entry_inventory(client);
    EXPECT_EQ(client.read().opcode(), 135);
    for (int index = 0; index < 5; ++index) {
      EXPECT_EQ(protocol.decode(client.read()).at("status"), "decoded");
    }
    client.send(protocol.encode(20085, nlohmann::json::object(), 1));
    EXPECT_EQ(client.read().opcode(), 176);
    expect_pre_playing_combat_bootstrap(client, protocol);
    const auto active_npcs = std::ranges::count_if(definitions->npcs, [](const auto &entry) {
      return entry.second.value("initially_active", true);
    });
    const auto notifications = definitions->document.at("sectors").size() + active_npcs;
    for (std::size_t index = 0; index < notifications; ++index) {
      EXPECT_EQ(protocol.decode(client.read()).at("status"), "decoded");
    }
  };
  const nlohmann::json pickup{{"target_unit_id", object.at("unit_id")},
                              {"param", {{"interaction_id", object.at("interaction_id")}}}};
  WorldTestClient first_client(server.port());
  enter(first_client, first.second);
  WorldTestClient second_client(server.port());
  enter(second_client, second.second);
  first_client.send(protocol.encode(20003, nlohmann::json::object(), 2));
  EXPECT_EQ(first_client.read().opcode(), 4);
  const auto pick = [&](WorldTestClient &client, std::uint32_t sequence) {
    client.send(protocol.encode(20068, pickup, sequence));
    EXPECT_EQ(client.read().opcode(), 91);
    const auto update = protocol.decode(client.read());
    EXPECT_EQ(update.at("opcode"), 169);
    EXPECT_EQ(update.at("message").at("stackables").at(0).at("quantity"), 5);
    EXPECT_EQ(client.read().opcode(), 163);
  };
  pick(first_client, 3);
  pick(second_client, 2);
  const auto &potion = definitions->potions.at(40101101);
  const nlohmann::json pos{
      {"millimeter_x", point.at(0)}, {"millimeter_y", point.at(1)}, {"millimeter_z", point.at(2)}};
  first_client.send(
      protocol.encode(20080, {{"slots", {{{"item_cms_id", 40101101}, {"count", 5}}}}}, 4));
  EXPECT_EQ(first_client.read().opcode(), 104);
  const auto start_ticks = std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count() /
                                          100);
  first_client.send(protocol.encode(20010,
                                    {{"use_item", {{"item_cms_id", 40101101}}},
                                     {"start_skill",
                                      {{"skill_id", potion.at("skill_id")},
                                       {"instance_id", 10},
                                       {"tick", start_ticks},
                                       {"pos", pos},
                                       {"rotation", {{"yaw", 0}}}}}},
                                    5));
  EXPECT_EQ(first_client.read().opcode(), 53);
  EXPECT_EQ(first_client.read().opcode(), 14);
  const nlohmann::json effect{
      {"skill_instance_id", 10},
      {"pos", pos},
      {"skill_effect",
       {{"effect_id", potion.at("effect_id")},
        {"targets",
         {{{"unit_id",
            std::to_string(player_wire_unit_id(first.second.character().char_id()))}}}}}}};
  // The real client may omit ExecuteSkillEffectReq. Consumption must still
  // arrive from the server timer, before any further client packet is sent.
  const auto consumed = protocol.decode(first_client.read());
  EXPECT_EQ(consumed.at("opcode"), 169);
  EXPECT_EQ(consumed.at("message").at("stackables").at(0).at("quantity"), 4);
  // Official order after the stack update: the health flow, then the potion's recovery.
  EXPECT_EQ(first_client.read().opcode(), 54);
  const auto recovery = protocol.decode(first_client.read());
  EXPECT_EQ(recovery.at("opcode"), 80);
  EXPECT_EQ(recovery.at("message").at("stat_recovery").at("cms_id"), 4);
  // Effect reports are diagnostic hints for the server-timed action: whatever target they name,
  // they neither spend another potion nor receive a reply.
  auto raw_target_effect = effect;
  raw_target_effect["skill_effect"]["targets"][0]["unit_id"] =
      std::to_string(first.second.character().char_id());
  first_client.send(protocol.encode(20011, raw_target_effect, 6));
  first_client.send(protocol.encode(20011, effect, 7));
  first_client.send(protocol.encode(20003, nlohmann::json::object(), 8));
  EXPECT_EQ(first_client.read().opcode(), 4);
  const auto ended = protocol.decode(first_client.read());
  EXPECT_EQ(ended.at("opcode"), 15);
  EXPECT_EQ(ended.at("message").at("end_skill").at("pos"), pos);
  EXPECT_EQ(repositories.character_state
                ->load_world_progress(repositories.accounts->account("tutorial_tls_first")->id,
                                      first.second.character().char_id(), 1)
                .stackables.at(40101101),
            4);
  first_client.disconnect();
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(first.second.character().char_id());
  const auto reconnect_selection = auth.select(first.first, select);
  WorldTestClient reconnected(server.port());
  enter(reconnected, reconnect_selection);
  reconnected.send(protocol.encode(20068, pickup, 2));
  EXPECT_EQ(reconnected.read().opcode(), 91);
  EXPECT_EQ(reconnected.read().opcode(), 163);
  reconnected.send(protocol.encode(20003, nlohmann::json::object(), 3));
  EXPECT_EQ(reconnected.read().opcode(), 4);
}

TEST(NativeWorld, TutorialTlsCombatAndPotionShareActionChannelAndServerTimer) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the generated local tutorial profile";
  }
  const auto definitions = TutorialData::load(profile_path);
  const auto catalog_root = std::filesystem::path(profile_path).parent_path().parent_path() /
                            "catalog";
  auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  const auto stats = CharacterStats::load(*catalog);
  const auto &object = definitions->document.at("objects").at(0);
  const auto &point = object.at("position");
  const nlohmann::json position{
      {"millimeter_x", point.at(0)}, {"millimeter_y", point.at(1)}, {"millimeter_z", point.at(2)}};
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("combat_tls", "combat-tutorial-fixture-password"));
  const auto login = auth.login("combat_tls", "combat-tutorial-fixture-password");
  x5::pb::authwebsocket::WorldEnterReq world;
  world.set_world_id(1);
  auth.enter(login.access_token(), world);
  Character character;
  character.account_id = login.account_id();
  character.name = "CombatTls";
  character.class_id = 1;
  character.world_position = WorldPosition{8000, point.at(0), point.at(1), point.at(2), 0};
  character = repositories.characters->create(character);
  ASSERT_TRUE(repositories.characters->save_world_position(character.account_id, character.id, 1,
                                                           *character.world_position));
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(character.id);
  const auto selected = auth.select(login.access_token(), select);
  ASSERT_EQ(selected.packet_key().size(), 32u);
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, definitions, stats, catalog, catalog_root);
  WorldTestClient client(server.port());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  std::uint32_t sequence = 0;
  const auto send = [&](std::uint16_t opcode, const nlohmann::json &message) {
    client.send(protocol.encode(opcode, message, sequence++));
  };
  const auto until = [&](std::uint16_t opcode) {
    for (int count = 0; count < 128; ++count) {
      const auto decoded = protocol.decode(client.read());
      if (decoded.at("opcode") == opcode) {
        return decoded.at("message");
      }
      if (decoded.at("opcode") == 2 || decoded.at("opcode") == 13) {
        throw std::runtime_error("Unexpected gameplay error: " + decoded.at("message").dump());
      }
    }
    throw std::runtime_error("Expected gameplay notification did not arrive within frame bound");
  };
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);
  send(20001, nlohmann::json::object());
  until(140);
  send(20085, nlohmann::json::object());
  until(176);
  send(20003, nlohmann::json::object());
  until(4);
  send(20068, {{"target_unit_id", object.at("unit_id")},
               {"param", {{"interaction_id", object.at("interaction_id")}}}});
  until(163);
  send(20080, {{"slots", {{{"item_cms_id", 40101101}, {"count", 5}}}}});
  until(104);
  const auto skill_start = [&](int skill_id, std::uint32_t instance, const nlohmann::json &pos) {
    return nlohmann::json{{"start_skill",
                           {{"skill_id", skill_id},
                            {"instance_id", instance},
                            {"tick", "1"},
                            {"pos", pos},
                            {"rotation", {{"yaw", 0}}}}}};
  };
  auto drink = skill_start(1000, 1, position);
  drink["use_item"] = {{"item_cms_id", 40101101}};
  send(20010, drink);
  EXPECT_EQ(until(14).at("start_skill").at("skill_id"), 1000);
  send(20010, skill_start(1100, 2, position));
  EXPECT_EQ(until(13).at("ec"), "Skill_UsingSkill");
  // Neither client effect reports nor any more incoming packets are necessary.
  EXPECT_EQ(until(169).at("stackables").at(0).at("quantity"), 4);
  EXPECT_EQ(until(15).at("end_skill").at("instance_id"), 1);
  EXPECT_EQ(repositories.character_state->load_world_progress(character.account_id, character.id, 1)
                .stackables.at(40101101),
            4);

  const auto monster = std::ranges::find_if(definitions->npcs, [](const auto &entry) {
    return entry.second.at("behavior") == "Monster" && entry.second.value("initially_active", true);
  });
  ASSERT_NE(monster, definitions->npcs.end());
  const auto &target = monster->second.at("position");
  const nlohmann::json attack_position{{"millimeter_x", target.at(0).get<int>() - 1000},
                                       {"millimeter_y", target.at(1)},
                                       {"millimeter_z", target.at(2)}};
  send(20087, {{"pos", attack_position}, {"body_yaw", 0}});
  auto attack = skill_start(1000000, 3, attack_position);
  attack["lock_on_target_id"] = std::to_string(monster->first);
  send(20010, attack);
  const auto cost = until(53);
  ASSERT_FALSE(cost.at("stat_changes").empty());
  EXPECT_EQ(cost.at("stat_changes").at(0).at("delta"), -20);
  EXPECT_EQ(until(14).at("start_skill").at("skill_id"), 1000000);
  auto overlapping_drink = skill_start(1000, 4, attack_position);
  overlapping_drink["use_item"] = {{"item_cms_id", 40101101}};
  send(20010, overlapping_drink);
  EXPECT_EQ(until(13).at("ec"), "Skill_UsingSkill");
  const auto damage = until(19);
  EXPECT_EQ(damage.at("caster_unit_id"), std::to_string(player_wire_unit_id(character.id)));
  EXPECT_EQ(damage.at("target_unit_id"), std::to_string(monster->first));
  EXPECT_GT(damage.at("final_damage_amount").get<int>(), 0);
  EXPECT_EQ(repositories.character_state->load_world_progress(character.account_id, character.id, 1)
                .stackables.at(40101101),
            4);
  client.disconnect();
}

// Quest 10000040 through the REAL NativeWorld frame path (handshake -> 20087 movement -> 20019 -> durable commit),
// using the official client requests of the full-tutorial capture (records 65, 319, 1338, 1360). Unlike the
// TutorialSession-level replay, the position used by the server is `handshake_.current_position()`, which only a
// 20087 MoveReq (or the saved spawn) updates, and it must lie within 5000 mm of the request `pos` and inside the
// objective volume. Authoritative validation is NOT weakened: case (A) proves a stale tracked position rejects.
TEST(NativeWorld, TutorialQuestObjectivesFollowTrackedMovementOverTheRealFramePath) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the generated local tutorial profile";
  }
  const auto definitions = TutorialData::load(profile_path);
  const auto catalog_root = std::filesystem::path(profile_path).parent_path().parent_path() /
                            "catalog";
  auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  const auto stats = CharacterStats::load(*catalog);
  const auto accept = nlohmann::json::parse(R"json({"pos":{"millimeter_x":-797735,"millimeter_y":713423,"millimeter_z":-699637},"quest_cms_id":10000040,"trigger_volume_enter":{"trigger_volume_x":"8213242118750945670","trigger_volume_y":"6980351610977240307"},"yaw":141.06723})json");
  const auto objective1 = nlohmann::json::parse(R"json({"pos":{"millimeter_x":-943690,"millimeter_y":754645,"millimeter_z":-695294},"quest_cms_id":10000040,"quest_progress_sequence_id":1,"trigger_volume_enter":{"trigger_volume_x":"2801311439973380602","trigger_volume_y":"8198539430178496379"},"yaw":-152.712769})json");
  const auto objective2 = nlohmann::json::parse(R"json({"pos":{"millimeter_x":-1101266,"millimeter_y":831271,"millimeter_z":-699867},"quest_cms_id":10000040,"quest_progress_sequence_id":2,"trigger_volume_enter":{"trigger_volume_x":"15924249501221489941","trigger_volume_y":"2424018298502802238"},"yaw":-117.152771})json");
  const auto objective3 = nlohmann::json::parse(R"json({"pos":{"millimeter_x":-1175279,"millimeter_y":745704,"millimeter_z":-697898},"quest_cms_id":10000040,"quest_progress_sequence_id":3,"trigger_volume_enter":{"trigger_volume_x":"12422980742822208548","trigger_volume_y":"6585567241543601496"},"yaw":-104.492767})json");
  const auto position_of = [](const nlohmann::json &request) {
    return WorldPosition{8000, request.at("pos").at("millimeter_x"),
                         request.at("pos").at("millimeter_y"), request.at("pos").at("millimeter_z"),
                         0};
  };
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("quest_real", "quest-real-path-fixture-password"));
  const auto login = auth.login("quest_real", "quest-real-path-fixture-password");
  x5::pb::authwebsocket::WorldEnterReq world;
  world.set_world_id(1);
  auth.enter(login.access_token(), world);
  Character character;
  character.account_id = login.account_id();
  character.name = "QuestReal";
  character.class_id = 1;
  character.world_position = position_of(accept);
  character = repositories.characters->create(character);
  ASSERT_TRUE(repositories.characters->save_world_position(character.account_id, character.id, 1,
                                                           *character.world_position));
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, definitions, stats, catalog, catalog_root);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  struct Connection {
    std::unique_ptr<WorldTestClient> client;
    std::uint32_t sequence = 0;
    std::vector<nlohmann::json> frames;
  };
  const auto connect = [&]() {
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(character.id);
    const auto selected = auth.select(login.access_token(), select);
    EXPECT_EQ(selected.packet_key().size(), 32u);
    Connection connection{std::make_unique<WorldTestClient>(server.port()), 0, {}};
    connection.client->send_key(selected.packet_key());
    EXPECT_EQ(connection.client->read().opcode(), 5);
    return connection;
  };
  const auto send = [&](Connection &connection, std::uint16_t opcode,
                        const nlohmann::json &message) {
    connection.client->send(protocol.encode(opcode, message, connection.sequence++));
  };
  // Reads frames until `opcode` (or the common error 2) and returns the decoded frame; every frame is recorded.
  const auto next = [&](Connection &connection, std::uint16_t opcode) {
    try {
      for (int count = 0; count < 160; ++count) {
        const auto decoded = protocol.decode(connection.client->read());
        connection.frames.push_back(decoded);
        if (decoded.at("opcode") == opcode || decoded.at("opcode") == 2) {
          return decoded;
        }
      }
    } catch (const std::exception &error) {
      std::string seen;
      for (const auto &frame : connection.frames) {
        seen += std::to_string(frame.at("opcode").get<int>()) + " ";
      }
      throw std::runtime_error("waiting for opcode " + std::to_string(opcode) + ": " + error.what() +
                               " after frames: " + seen);
    }
    throw std::runtime_error("Expected frame did not arrive within the frame bound");
  };
  const auto move_to = [&](Connection &connection, const nlohmann::json &request) {
    send(connection, 20087,
         {{"tick", "1"},
          {"pos", request.at("pos")},
          {"body_yaw", request.value("yaw", 0.0)},
          {"pred_pos", request.at("pos")},
          {"pred_milli", 100}});
  };
  const auto objectives = [&]() {
    return repositories.character_state->load_world_progress(character.account_id, character.id, 1)
        .quests.at(10000040)
        .objectives;
  };

  auto first = connect();
  send(first, 20001, nlohmann::json::object());
  next(first, 140);
  send(first, 20085, nlohmann::json::object());
  next(first, 176);
  send(first, 20003, nlohmann::json::object());
  next(first, 4);
  send(first, 20016, accept);
  const auto accepted = next(first, 28);
  ASSERT_EQ(accepted.at("opcode"), 28) << accepted.dump();
  ASSERT_EQ(accepted.at("message").at("quest").at("cms_id"), 10000040);

  // (A) a stale tracked position: objective 1 reported from far away without a movement update.
  send(first, 20019, objective1);
  const auto stale = next(first, 27);
  EXPECT_EQ(stale.at("opcode"), 2) << "a stale tracked position must reject: " << stale.dump();
  EXPECT_EQ(objectives().at(1), 0);

  // (B) tracked movement to each official volume, then the official progress request.
  move_to(first, objective1);
  send(first, 20019, objective1);
  const auto step1 = next(first, 27);
  ASSERT_EQ(step1.at("opcode"), 27) << step1.dump();
  EXPECT_EQ(objectives().at(1), 1);
  EXPECT_EQ(objectives().at(2), 0);
  move_to(first, objective2);
  send(first, 20019, objective2);
  const auto step2 = next(first, 27);
  ASSERT_EQ(step2.at("opcode"), 27) << step2.dump();
  EXPECT_EQ(objectives().at(2), 1);
  move_to(first, objective3);
  send(first, 20019, objective3);
  const auto step3 = next(first, 27);
  ASSERT_EQ(step3.at("opcode"), 27) << step3.dump();
  EXPECT_EQ(objectives().at(3), 1);

  // (C) a repeated objective is idempotent; reconnect keeps the stored state.
  move_to(first, objective2);
  send(first, 20019, objective2);
  next(first, 27);
  EXPECT_EQ(objectives().at(2), 1);
  first.client->disconnect();
  auto second = connect();
  send(second, 20001, nlohmann::json::object());
  next(second, 140);
  bool listed = false;
  for (const auto &frame : second.frames) {
    if (frame.at("opcode") == 136) {
      for (const auto &quest : frame.at("message").at("quests")) {
        if (quest.at("cms_id") == 10000040) {
          listed = true;
          EXPECT_EQ(quest.at("state"), "EX5QuestStateType_InProgress");
        }
      }
    }
  }
  EXPECT_TRUE(listed) << "the reconnect snapshot must list the in-progress quest";
  EXPECT_EQ(objectives().at(1) + objectives().at(2) + objectives().at(3), 3);
  // Gameplay requests are dispatched only after the map load completes (a position exists), as on first entry.
  send(second, 20085, nlohmann::json::object());
  next(second, 176);
  send(second, 20003, nlohmann::json::object());
  next(second, 4);

  // (D) reward exactly once.
  send(second, 20018, {{"quest_cms_id", 10000040}});
  next(second, 27);
  const auto rewarded =
      repositories.character_state->load_world_progress(character.account_id, character.id, 1);
  ASSERT_TRUE(rewarded.quests.at(10000040).complete);
  const auto experience = rewarded.experience;
  send(second, 20018, {{"quest_cms_id", 10000040}});
  next(second, 27);
  EXPECT_EQ(repositories.character_state->load_world_progress(character.account_id, character.id, 1)
                .experience,
            experience);
  second.client->disconnect();
}

// Real frame path for the tutorial cutscene follow-ups. (A) The Herald Knight door: the
// quest's final trigger plus LevelTriggerReq play cutscene 1523; its end teleports the player
// and binds the boss phase. (B) A character saved in map 10117 resumes there; the quest doodad
// cutscene 1520 ends with a map move out of the tutorial, and the client's MapLoadComplete and
// StartPlaying requests complete the new map's handshake on the same connection.
TEST(NativeWorld, TutorialCutsceneFollowUpsTeleportAndMoveMapsOverTheRealFramePath) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the generated local tutorial profile";
  }
  const auto definitions = TutorialData::load(profile_path);
  const auto followup = definitions->followup(10117);
  ASSERT_NE(followup, nullptr);
  const auto catalog_root = std::filesystem::path(profile_path).parent_path().parent_path() /
                            "catalog";
  auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  const auto stats = CharacterStats::load(*catalog);
  // Hold the map-change worker long enough for its already-armed sprint timer to queue
  // behind it. This reproduces a timer firing after begin_map_move clears current_position.
  std::atomic<bool> delayed_map_change{};
  std::atomic<bool> deferred_timer{};
  struct RestoreLogger {
    std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();

    ~RestoreLogger() {
      spdlog::set_default_logger(previous);
    }
  };
  RestoreLogger logger_guard;
  const auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
      [&](const spdlog::details::log_msg &message) {
        const std::string_view text(message.payload.data(), message.payload.size());
        if (text.starts_with("Tutorial cutscene map move")) {
          delayed_map_change = true;
          std::this_thread::sleep_for(std::chrono::milliseconds(900));
        }
        if (text.starts_with("World gameplay timer deferred during map loading")) {
          deferred_timer = true;
        }
      });
  spdlog::set_default_logger(std::make_shared<spdlog::logger>("map-load-regression", sink));
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  // One character per account, like the other real-path fixtures.
  const auto open_account = [&](const std::string &name) {
    EXPECT_TRUE(auth.create_account(name, "cutscene-path-fixture-password"));
    auto account_login = auth.login(name, "cutscene-path-fixture-password");
    x5::pb::authwebsocket::WorldEnterReq world;
    world.set_world_id(1);
    auth.enter(account_login.access_token(), world);
    return account_login;
  };
  const auto door_login = open_account("cutscene_door");
  const auto instance_login = open_account("cutscene_instance");
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, definitions, stats, catalog, catalog_root);
  const auto mail = std::make_shared<MemoryMailRepository>(repositories.database);
  server.listener().set_repositories(
      {mail, std::make_shared<MemoryMarketRepository>(repositories.database),
       std::make_shared<MemoryRankingRepository>(repositories.database),
       std::make_shared<MemoryFogRepository>(repositories.database)});
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  const auto point_json = [](const nlohmann::json &point) {
    return nlohmann::json{
        {"millimeter_x", point.at(0)}, {"millimeter_y", point.at(1)}, {"millimeter_z", point.at(2)}};
  };
  const auto create_character = [&](std::int64_t account_id, const std::string &name, int map_id,
                                    const nlohmann::json &point, int quest_id,
                                    int finished_objectives, int objective_count) {
    Character character;
    character.account_id = account_id;
    character.name = name;
    character.class_id = 1;
    character.world_position = WorldPosition{map_id, point.at(0).get<int>(),
                                             point.at(1).get<int>(), point.at(2).get<int>(), 0};
    character = repositories.characters->create(character);
    EXPECT_TRUE(repositories.characters->save_world_position(character.account_id, character.id, 1,
                                           *character.world_position));
    const auto before =
        repositories.character_state->load_world_progress(character.account_id, character.id, 1);
    auto after = before;
    QuestProgress quest;
    for (int objective = 1; objective <= objective_count; ++objective) {
      quest.objectives.emplace(objective, objective <= finished_objectives ? 1 : 0);
    }
    after.quests.emplace(quest_id, quest);
    ++after.revision;
    EXPECT_TRUE(repositories.character_state->save_world_progress(character.account_id,
                                                                  character.id, 1, before, after));
    return character;
  };

  std::uint32_t sequence = 0;
  std::unique_ptr<WorldTestClient> client;
  const auto connect = [&](const auto &account_login, const Character &character) {
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(character.id);
    const auto selected = auth.select(account_login.access_token(), select);
    sequence = 0;
    client = std::make_unique<WorldTestClient>(server.port());
    client->send_key(selected.packet_key());
    EXPECT_EQ(client->read().opcode(), 5);
  };
  const auto send = [&](std::uint16_t opcode, const nlohmann::json &message) {
    client->send(protocol.encode(opcode, message, sequence++));
  };
  const auto next = [&](std::uint16_t opcode) {
    for (int count = 0; count < 160; ++count) {
      const auto decoded = protocol.decode(client->read());
      if (decoded.at("opcode") == opcode || decoded.at("opcode") == 2) {
        return decoded;
      }
    }
    throw std::runtime_error("Expected frame did not arrive within the frame bound");
  };
  const auto enter_world = [&](int expected_map) {
    send(20001, nlohmann::json::object());
    next(140);
    send(20085, nlohmann::json::object());
    const auto completed = next(176);
    EXPECT_EQ(completed.at("message").at("map_id"), expected_map);
    send(20086, nlohmann::json::object());
    next(183);
    send(20003, nlohmann::json::object());
    next(4);
  };

  // (A) The door: map 8000, quest 10000044 waiting for its final trigger.
  const auto &door_trigger = definitions->quest(10000044)->at("progresses").at(4).at("trigger");
  const auto door = create_character(door_login.account_id(), "DoorPath", 8000,
                                     door_trigger.at("center"), 10000044, 4, 5);
  connect(door_login, door);
  enter_world(8000);
  const auto door_position = point_json(door_trigger.at("center"));
  send(20019, {{"quest_cms_id", 10000044},
               {"quest_progress_sequence_id", 5},
               {"pos", door_position},
               {"trigger_volume_enter",
                {{"trigger_volume_x", door_trigger.at("guid").at(0)},
                 {"trigger_volume_y", door_trigger.at("guid").at(1)}}}});
  ASSERT_EQ(next(27).at("opcode"), 27);
  send(20084, {{"trigger_id_x", door_trigger.at("guid").at(0)},
               {"trigger_id_y", door_trigger.at("guid").at(1)},
               {"pos", door_position},
               {"body_yaw", 125.0},
               {"tick", "1"}});
  const auto cutscene = next(70);
  ASSERT_EQ(cutscene.at("opcode"), 70) << cutscene.dump();
  EXPECT_EQ(cutscene.at("message").at("cut_scene_id"), 1523);
  send(20018, {{"quest_cms_id", 10000044}});
  ASSERT_EQ(next(28).at("message").at("quest").at("cms_id"), 10000045);
  send(20055, {{"cut_scene_id", 1523}});
  const auto teleport = next(181);
  ASSERT_EQ(teleport.at("opcode"), 181) << teleport.dump();
  EXPECT_EQ(teleport.at("message").at("pos").at("millimeter_x"), -1393646);
  EXPECT_EQ(next(114).at("message").at("new_ai_phase_transition_id"), 2302400);
  send(20088, {{"tick", teleport.at("message").at("tick")}});
  send(20089, nlohmann::json::object());
  send(20003, nlohmann::json::object());
  ASSERT_EQ(next(4).at("opcode"), 4);
  client->disconnect();

  // (B) Map 10117 resume, then the quest doodad cutscene's end point leaves the tutorial.
  const auto &quest_doodad = followup->document.at("doodads").at(0);
  const auto map_10117 = create_character(instance_login.account_id(), "InstancePath", 10117,
                                          quest_doodad.at("position"), 10000046, 1, 4);
  connect(instance_login, map_10117);
  enter_world(10117);
  send(20068, {{"target_unit_id", quest_doodad.at("unit_id")},
               {"param", {{"interaction_id", quest_doodad.at("interaction_id")}}}});
  const auto doodad_cutscene = next(70);
  ASSERT_EQ(doodad_cutscene.at("opcode"), 70) << doodad_cutscene.dump();
  EXPECT_EQ(doodad_cutscene.at("message").at("cut_scene_id"), 1520);
  send(20087, {{"detail", "MoveDetail_Sprint"},
               {"pos", point_json(quest_doodad.at("position"))}});
  send(20003, nlohmann::json::object());
  ASSERT_EQ(next(4).at("opcode"), 4);
  send(20055, {{"cut_scene_id", 1520}});
  const auto map_move = next(175);
  ASSERT_EQ(map_move.at("opcode"), 175) << map_move.dump();
  EXPECT_EQ(map_move.at("message").at("map_id"), 9011);
  EXPECT_EQ(map_move.at("message").at("spawn_point").at("pos").at("millimeter_x"), -1568074);
  // Leaving the tutorial for the overworld mails the account's free supplies and announces
  // them after the map-change frames. The earlier door scenario never left the tutorial.
  const auto reward_notification = next(174);
  ASSERT_EQ(reward_notification.at("opcode"), 174) << reward_notification.dump();
  EXPECT_EQ(reward_notification.at("message").at("account_mail").at("attachments").size(), 2U);
  const auto now_ticks = unix_epoch_ticks_now();
  const auto reward_mail =
      mail->mailbox(map_10117.account_id, map_10117.id, 1, now_ticks);
  ASSERT_EQ(reward_mail.size(), 1U);
  EXPECT_EQ(reward_mail.front().delivery_key, kTutorialCompletionMailKey);
  EXPECT_EQ(reward_mail.front().character_id, 0);
  EXPECT_TRUE(mail->mailbox(door.account_id, door.id, 1, now_ticks).empty());
  // The map-load clock exchange below follows. A clock packet remains legal before MapLoadCompleteReq.
  send(20003, nlohmann::json::object());
  ASSERT_EQ(next(4).at("opcode"), 4);
  EXPECT_TRUE(delayed_map_change);
  EXPECT_TRUE(deferred_timer);
  send(20085, nlohmann::json::object());
  EXPECT_EQ(next(176).at("message").at("map_id"), 9011);
  send(20086, nlohmann::json::object());
  EXPECT_EQ(next(183).at("opcode"), 183);

  // The shared-world quest service accepts the hidden arrival quest from its catalog volume
  // (captured order: StartPlaying, then this AcceptQuestReq).
  const auto quest_catalog_path = catalog_root.parent_path() / "quests" / "catalog.json";
  if (std::filesystem::exists(quest_catalog_path)) {
    const auto quest_catalog = QuestCatalog::load(quest_catalog_path);
    const auto &arrival = quest_catalog->quest(90100011)->at("accept").at(0).at("trigger");
    const nlohmann::json arrival_position{{"millimeter_x", -1568074},
                                          {"millimeter_y", 3083414},
                                          {"millimeter_z", -628005}};
    send(20016, {{"quest_cms_id", 90100011},
                 {"pos", arrival_position},
                 {"trigger_volume_enter",
                  {{"trigger_volume_x", arrival.at("guid").at(0)},
                   {"trigger_volume_y", arrival.at("guid").at(1)}}}});
    const auto arrival_accepted = next(28);
    ASSERT_EQ(arrival_accepted.at("opcode"), 28) << arrival_accepted.dump();
    EXPECT_EQ(next(27).at("message").at("quest").at("state"), "EX5QuestStateType_Complete");
    EXPECT_TRUE(
        repositories.character_state->load_world_progress(map_10117.account_id, map_10117.id, 1)
            .quests.at(90100011)
            .complete);
  }

  const auto saved = repositories.characters->characters(instance_login.account_id(), 1);
  const auto moved = std::ranges::find_if(
      saved, [&](const Character &entry) { return entry.id == map_10117.id; });
  ASSERT_NE(moved, saved.end());
  ASSERT_TRUE(moved->world_position.has_value());
  EXPECT_EQ(moved->world_position->map_id, 9011);
  EXPECT_TRUE(
      repositories.character_state->load_world_progress(map_10117.account_id, map_10117.id, 1)
          .quests.at(10000046)
          .objectives.at(2) == 1);
  client->disconnect();
}

} // namespace
} // namespace chronicles
