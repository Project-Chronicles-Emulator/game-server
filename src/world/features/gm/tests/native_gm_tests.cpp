#include "world/session/tests/native_world_test_support.hpp"

#include "world/features/character/achievements.hpp"
#include "world/features/character/character_stats.hpp"
#include "world/features/combat/combat.hpp"
#include "world/features/fog/fog_repository.hpp"
#include "world/features/gm/gm_feature.hpp"
#include "world/features/mail/mail.hpp"

namespace chronicles {
namespace {

using namespace native_testing;

TEST(NativeWorld, GameMasterConsoleCommandsRequireTheAccountRole) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }
  constexpr int kHealingPotion = 15010403;
  constexpr char kPassword[] = "game-master-world-password";

  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("world_keeper", kPassword));
  ASSERT_TRUE(auth.create_account("world_player", kPassword));
  ASSERT_TRUE(auth.set_account_role("world_keeper", AccountRole::game_master));

  const auto select_character = [&](const std::string &account, const std::string &name) {
    const auto login = auth.login(account, kPassword);
    x5::pb::authwebsocket::WorldEnterReq enter_request;
    enter_request.set_world_id(1);
    EXPECT_EQ(auth.enter(login.access_token(), enter_request).error_code(), x5::ec::Success);
    Character character;
    character.account_id = login.account_id();
    character.name = name;
    character = repositories.characters->create(character);
    x5::pb::ws::front::CharSelectReq select_request;
    select_request.set_char_id(character.id);
    return std::make_pair(character, auth.select(login.access_token(), select_request));
  };
  const auto [keeper, keeper_selection] = select_character("world_keeper", "Worldkeeper");
  const auto [player, player_selection] = select_character("world_player", "Worldplayer");

  auto client_catalog = std::make_shared<ClientCatalog>(catalog_path);
  const auto stats = CharacterStats::load(*client_catalog);
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, stats, client_catalog);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient keeper_client(server.port());
  keeper_client.send_key(keeper_selection.packet_key());
  ASSERT_EQ(keeper_client.read().opcode(), 5);
  keeper_client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  const auto base = protocol.decode(read_until_opcode(keeper_client, 125)).at("message");
  EXPECT_TRUE(base.value("gm", false));
  read_until_opcode(keeper_client, 126);

  keeper_client.send(protocol.encode(20009, {{"text", "item_add 15010403 3"}}, 1));
  const auto item_update = protocol.decode(read_until_opcode(keeper_client, 169)).at("message");
  EXPECT_TRUE(item_update.contains("command"));
  EXPECT_EQ(item_update.at("stackables").at(0).at("item_cms_id"), kHealingPotion);
  EXPECT_EQ(item_update.at("stackables").at(0).at("quantity"), 3);

  const nlohmann::json add_gold{
      {"cmds", nlohmann::json::array({{{"add_money", {{"amount", "500"}}}}})}};
  keeper_client.send(protocol.encode(20008, add_gold, 2));
  const auto money_update = protocol.decode(read_until_opcode(keeper_client, 169)).at("message");
  EXPECT_EQ(money_update.at("money_changes").at(0).at("change"), "500");
  EXPECT_EQ(money_update.at("money_changes").at(0).at("result"), "500");

  const auto keeper_progress =
      repositories.character_state->load_world_progress(keeper.account_id, keeper.id, 1);
  EXPECT_EQ(keeper_progress.stackables.at(kHealingPotion), 3);
  EXPECT_EQ(keeper_progress.gold, 500);
  keeper_client.disconnect();

  WorldTestClient player_client(server.port());
  player_client.send_key(player_selection.packet_key());
  ASSERT_EQ(player_client.read().opcode(), 5);
  player_client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  const auto player_base = protocol.decode(read_until_opcode(player_client, 125)).at("message");
  EXPECT_FALSE(player_base.value("gm", false));
  read_until_opcode(player_client, 126);
  // With the real catalog the achievement snapshot is the last world-entry notification, as in
  // official entries; drain it so the next frame read below is the ping reply.
  read_until_opcode(player_client, character::kAchievementSnapshotNotification);

  // The refused command produces no frame, so the next frame answers the ping.
  player_client.send(protocol.encode(20009, {{"text", "add_money 500"}}, 1));
  player_client.send(protocol.encode(20003, nlohmann::json::object(), 2));
  EXPECT_EQ(player_client.read().opcode(), 4);
  const auto player_progress =
      repositories.character_state->load_world_progress(player.account_id, player.id, 1);
  EXPECT_EQ(player_progress.gold, 0);
  EXPECT_FALSE(player_progress.stackables.contains(kHealingPotion));
  player_client.disconnect();
}

TEST(NativeWorld, GameMasterSessionCommandsTeleportProtectAndReply) {
  constexpr char kPassword[] = "game-master-session-password";
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("session_keeper", kPassword));
  ASSERT_TRUE(auth.create_account("session_player", kPassword));
  ASSERT_TRUE(auth.set_account_role("session_keeper", AccountRole::game_master));

  const auto select_character = [&](const std::string &account, const std::string &name) {
    const auto login = auth.login(account, kPassword);
    x5::pb::authwebsocket::WorldEnterReq enter_request;
    enter_request.set_world_id(1);
    EXPECT_EQ(auth.enter(login.access_token(), enter_request).error_code(), x5::ec::Success);
    Character character;
    character.account_id = login.account_id();
    character.name = name;
    character = repositories.characters->create(character);
    x5::pb::ws::front::CharSelectReq select_request;
    select_request.set_char_id(character.id);
    return auth.select(login.access_token(), select_request);
  };
  const auto keeper_selection = select_character("session_keeper", "Sessionkeeper");
  const auto player_selection = select_character("session_player", "Sessionplayer");

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  std::mutex replies_mutex;
  std::map<std::string, std::vector<std::string>> replies;
  server.listener().set_gm_reply_handler(
      [&](const ChatIdentity &identity, const std::string &text) {
        std::lock_guard lock(replies_mutex);
        replies[identity.name].push_back(text);
      });
  const auto replies_of = [&](const std::string &name) {
    std::lock_guard lock(replies_mutex);
    return replies[name];
  };
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  std::map<const WorldTestClient *, std::uint32_t> sequences;
  WorldTestClient keeper(server.port());
  const auto send = [&](WorldTestClient &client, std::uint16_t opcode,
                        const nlohmann::json &message) {
    client.send(protocol.encode(opcode, message, sequences[&client]++));
  };
  keeper.send_key(keeper_selection.packet_key());
  ASSERT_EQ(keeper.read().opcode(), 5);
  send(keeper, 20001, nlohmann::json::object());
  read_until_opcode(keeper, 175);
  send(keeper, 20085, nlohmann::json::object());
  read_until_opcode(keeper, 176);
  // A ping answered after a command proves that command's replies were already produced. The
  // pause keeps the test under the server's four-GM-requests-per-second limit.
  const auto settle = [&](WorldTestClient &client) {
    send(client, 20003, nlohmann::json::object());
    read_until_opcode(client, 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
  };

  send(keeper, 20009, {{"text", "god on"}});
  const auto attribute = protocol.decode(read_until_opcode(keeper, 33)).at("message");
  EXPECT_EQ(attribute.at("attributes").at("2"), 1);
  settle(keeper);
  EXPECT_EQ(replies_of("Sessionkeeper").back(), "God mode ON.");

  send(keeper, 20009, {{"text", "tp 10 20 30"}});
  const auto teleport = protocol.decode(read_until_opcode(keeper, 181)).at("message");
  EXPECT_EQ(teleport.at("pos").at("millimeter_x"), 10000);
  EXPECT_EQ(teleport.at("pos").at("millimeter_z"), 30000);
  send(keeper, 20088, {{"tick", teleport.at("tick")}});
  send(keeper, 20089, nlohmann::json::object());
  settle(keeper);
  send(keeper, 20009, {{"text", "pos"}});
  settle(keeper);
  EXPECT_NE(replies_of("Sessionkeeper").back().find("x=10.00 y=20.00 z=30.00"),
            std::string::npos);

  send(keeper, 20009, {{"text", "invisible on"}});
  send(keeper, 20009, {{"text", "status"}});
  settle(keeper);
  EXPECT_NE(replies_of("Sessionkeeper").back().find("god ON, invisible ON"), std::string::npos);

  const auto before_help = replies_of("Sessionkeeper").size();
  send(keeper, 20009, {{"text", "help"}});
  settle(keeper);
  EXPECT_GE(replies_of("Sessionkeeper").size(), before_help + 10);

  send(keeper, 20009, {{"text", "fly_through_walls_please"}});
  settle(keeper);
  EXPECT_NE(replies_of("Sessionkeeper").back().find("/gm help"), std::string::npos);

  send(keeper, 20009, {{"text", "players"}});
  settle(keeper);
  EXPECT_NE(replies_of("Sessionkeeper").back().find("Sessionkeeper [GM]"), std::string::npos);

  WorldTestClient player(server.port());
  player.send_key(player_selection.packet_key());
  ASSERT_EQ(player.read().opcode(), 5);
  send(player, 20001, nlohmann::json::object());
  read_until_opcode(player, 175);
  send(player, 20009, {{"text", "god on"}});
  settle(player);
  ASSERT_FALSE(replies_of("Sessionplayer").empty());
  EXPECT_EQ(replies_of("Sessionplayer").back(), "GM commands need a game master account.");

  keeper.disconnect();
  player.disconnect();
}

TEST(NativeWorld, GameMasterDiagnosticsReachTheIssuingClient) {
  constexpr char kPassword[] = "game-master-diagnostics-password";
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("diagnostics_keeper", kPassword));
  ASSERT_TRUE(auth.set_account_role("diagnostics_keeper", AccountRole::game_master));

  const auto login = auth.login("diagnostics_keeper", kPassword);
  x5::pb::authwebsocket::WorldEnterReq enter_request;
  enter_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), enter_request).error_code(), x5::ec::Success);
  Character character;
  character.account_id = login.account_id();
  character.name = "Diagnostician";
  character = repositories.characters->create(character);
  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(character.id);
  const auto selection = auth.select(login.access_token(), select_request);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  std::mutex replies_mutex;
  std::vector<std::string> replies;
  std::vector<std::string> announcements;
  server.listener().set_gm_reply_handler([&](const ChatIdentity &, const std::string &text) {
    std::lock_guard lock(replies_mutex);
    replies.push_back(text);
  });
  server.listener().set_gm_announce_handler(
      [&](const ChatIdentity &identity, const std::string &text) -> std::size_t {
        std::lock_guard lock(replies_mutex);
        announcements.push_back(identity.name + ": " + text);
        return 3;
      });
  const auto last_reply = [&] {
    std::lock_guard lock(replies_mutex);
    return replies.empty() ? std::string() : replies.back();
  };
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  std::uint32_t sequence = 0;
  WorldTestClient keeper(server.port());
  const auto send = [&](std::uint16_t opcode, const nlohmann::json &message) {
    keeper.send(protocol.encode(opcode, message, sequence++));
  };
  keeper.send_key(selection.packet_key());
  ASSERT_EQ(keeper.read().opcode(), 5);
  send(20001, nlohmann::json::object());
  read_until_opcode(keeper, 175);
  send(20085, nlohmann::json::object());
  read_until_opcode(keeper, 176);
  const auto settle = [&] {
    send(20003, nlohmann::json::object());
    read_until_opcode(keeper, 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
  };

  send(20009, {{"text", "game_time stop 12"}});
  const auto clock = protocol.decode(read_until_opcode(keeper, 158)).at("message");
  EXPECT_TRUE(clock.at("stopped").get<bool>());
  EXPECT_FLOAT_EQ(clock.at("daily_ratio").get<float>(), 0.5F);
  settle();
  EXPECT_NE(last_reply().find("stopped at 12:00"), std::string::npos);

  send(20009, {{"text", "watch me"}});
  const auto located = protocol.decode(read_until_opcode(keeper, 152)).at("message");
  EXPECT_EQ(located.at("unit_id"), std::to_string(player_wire_unit_id(character.id)));
  settle();
  EXPECT_NE(last_reply().find("Watching 1 overlays"), std::string::npos);

  // The client's channel-event console sends GmCmd.channel_event_info.
  const nlohmann::json channel_command{{"channel_event_info", nlohmann::json::object()}};
  send(20008, {{"cmds", nlohmann::json::array({channel_command})}});
  const auto channel = protocol.decode(read_until_opcode(keeper, 159)).at("message");
  EXPECT_TRUE(channel.contains("map_cms_id"));
  settle();

  send(20009, {{"text", "announce Maintenance at noon"}});
  settle();
  {
    std::lock_guard lock(replies_mutex);
    ASSERT_EQ(announcements.size(), 1U);
    EXPECT_EQ(announcements.front(), "Diagnostician: Maintenance at noon");
  }
  EXPECT_EQ(last_reply(), "Announcement sent to 3 characters.");

  send(20009, {{"text", "unwatch"}});
  settle();
  EXPECT_EQ(last_reply(), "Stopped 1 overlays.");

  // New world-command parsing and clear refusal routes must reach this authenticated socket,
  // rather than falling through to the older inventory parser's generic unsupported reply.
  send(20009, {{"text", "damage_monster spawned NaN"}});
  settle();
  EXPECT_NE(last_reply().find("Usage: damage_monster"), std::string::npos);
  send(20009, {{"text", "accept_quest 501"}});
  settle();
  EXPECT_EQ(last_reply(), "Quest acceptance requires a shared map and a loaded quest catalog.");
  send(20009, {{"text", "change_class magic"}});
  settle();
  EXPECT_NE(last_reply().find("class changes need a persisted class"), std::string::npos);

  keeper.disconnect();
}

TEST(NativeWorld, GmMapRevealPersistsOnlyTheIssuingCharacterOverTls) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified exported catalog";
  }
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  const auto areas = catalog->fog_area_ids();
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn, kOverworldSpawn},
                          catalog, catalog_path);
  // The GM reveal writes through the world's fog repository, installed like the server does.
  const auto fog = std::make_shared<MemoryFogRepository>(world.repositories.database);
  world.server->listener().set_repositories(
      {std::make_shared<MemoryMailRepository>(world.repositories.database),
       std::make_shared<MemoryMarketRepository>(world.repositories.database),
       std::make_shared<MemoryRankingRepository>(world.repositories.database), fog});
  ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
  auto &keeper_session = world.sessions[0];
  auto &player_session = world.sessions[1];
  keeper_session.access_token =
      world.auth->login("overworld_sessions0", "test-world-password-123").access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(world.auth->enter(keeper_session.access_token, enter).error_code(), x5::ec::Success);

  std::mutex replies_mutex;
  std::vector<std::string> replies;
  world.server->listener().set_gm_reply_handler(
      [&](const ChatIdentity &, const std::string &reply) {
        std::lock_guard lock(replies_mutex);
        replies.push_back(reply);
      });
  world.enter(keeper_session);
  world.enter(player_session);
  VehicleClient keeper(world, keeper_session, 3);
  VehicleClient player(world, player_session, 3);
  player.send(20009, {{"text", "reveal_map"}});
  EXPECT_TRUE(fog->character_fog_areas(player_session.account_id,
                                      player_session.character_id, 1).empty());
  keeper.send(20009, {{"text", "reveal_map"}});
  EXPECT_EQ(fog->character_fog_areas(keeper_session.account_id, keeper_session.character_id, 1),
            areas);
  EXPECT_TRUE(fog->character_fog_areas(player_session.account_id,
                                      player_session.character_id, 1).empty());
  {
    std::lock_guard lock(replies_mutex);
    EXPECT_TRUE(std::ranges::any_of(replies, [&](const std::string &reply) {
      return reply.find("Saved all " + std::to_string(areas.size())) != std::string::npos &&
             reply.find("character selection") != std::string::npos;
    }));
  }
}

TEST(NativeWorld, PrivateGmSpawnOnAnOpenWorldMapPursuesAndStopsWithoutLeakingToPeers) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Requires generated tutorial/combat profile and catalog export";
  }
  const auto definitions = TutorialData::load(profile_path);
  const auto catalog_root =
      std::filesystem::path(profile_path).parent_path().parent_path() / "catalog";
  const auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  const auto stats = CharacterStats::load(*catalog);
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn, kOverworldSpawn},
                          catalog, catalog_root, definitions, stats);
  ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
  auto &keeper_session = world.sessions[0];
  auto &observer_session = world.sessions[1];
  // Role changes take effect in newly authenticated sessions, not an older bearer.
  const auto keeper_login = world.auth->login("overworld_sessions0", "test-world-password-123");
  keeper_session.access_token = keeper_login.access_token();
  x5::pb::authwebsocket::WorldEnterReq enter_request;
  enter_request.set_world_id(1);
  ASSERT_EQ(world.auth->enter(keeper_session.access_token, enter_request).error_code(),
            x5::ec::Success);
  world.enter(keeper_session);
  world.enter(observer_session);
  VehicleClient keeper(world, keeper_session, 3);
  VehicleClient observer(world, observer_session, 3);
  keeper.send(20009, {{"text", "god on"}});
  const auto spawned = keeper.send(20009, {{"text", "spawn_npc 5001084 1"}});
  const auto adds = frames_of(spawned, 10);
  ASSERT_EQ(adds.size(), 1u);
  const auto unit_id = adds.front().at("unit_id").get<std::string>();

  // Move the GM away from the private spawn, leaving several metres of real pursuit.
  auto away = kOverworldSpawn;
  away.x_mm -= 7000;
  keeper.move_to(away);
  bool ran = false;
  bool stopped = false;
  bool cast_after_stop = false;
  int run_frames = 0;
  for (int poll = 0; poll < 400 && !cast_after_stop; ++poll) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    auto frames = keeper.drain();
    for (const auto &frame : frames) {
      if (frame.message.value("unit_id", std::string()) != unit_id) {
        continue;
      }
      if (frame.opcode == 177) {
        const auto detail = frame.message.value("detail", "MoveDetail_Stop");
        if (detail == "MoveDetail_Run") {
          ran = true;
          stopped = false;
          ++run_frames;
          EXPECT_TRUE(frame.message.contains("pred_pos"));
          EXPECT_GE(frame.message.value("pred_milli", 0), 16);
        } else {
          EXPECT_EQ(detail, "MoveDetail_Stop");
          stopped = ran;
        }
      } else if (frame.opcode == 14 && stopped) {
        cast_after_stop = true;
      }
    }
  }
  EXPECT_GE(run_frames, 2);
  EXPECT_TRUE(cast_after_stop) << "NPC must reach range and stop before its next attack";
  for (const auto &frame : observer.drain()) {
    EXPECT_NE(frame.message.value("unit_id", std::string()), unit_id)
        << "A private GM encounter cannot become a peer's public NPC";
  }

  const auto removed = keeper.send(20009, {{"text", "despawn_npcs"}});
  ASSERT_FALSE(frames_of(removed, 11).empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  for (const auto &frame : keeper.drain()) {
    EXPECT_NE(frame.message.value("unit_id", std::string()), unit_id)
        << "No movement or attack may resume after despawn";
  }
}

TEST(NativeWorld, GameMasterMailCommitsAndRejectsForgedRecipientOwnership) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  select_world_test_character(auth, "gm_mail_keeper", "MailKeeper", 1);
  ASSERT_TRUE(auth.set_account_role("gm_mail_keeper", AccountRole::game_master));
  const auto login = auth.login("gm_mail_keeper", "test-world-password-123");
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), enter).error_code(), x5::ec::Success);
  const auto character = repositories.characters->characters(login.account_id(), 1).front();
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(character.id);
  const auto selected = auth.select(login.access_token(), select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCatalog catalog;
  catalog.write_tables({{"Item", {}}, {"Shop", {}}});
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  const auto mail = std::make_shared<MemoryMailRepository>(repositories.database);
  server.listener().set_repositories(
      {mail, std::make_shared<MemoryMarketRepository>(repositories.database),
       std::make_shared<MemoryRankingRepository>(repositories.database),
       std::make_shared<MemoryFogRepository>(repositories.database)});
  std::mutex replies_mutex;
  std::vector<std::string> replies;
  server.listener().set_gm_reply_handler([&](const ChatIdentity &, const std::string &reply) {
    std::lock_guard lock(replies_mutex);
    replies.push_back(reply);
  });
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldTestClient client(server.port());
  enter_shared_world(client, protocol, selected.packet_key());
  client.send(protocol.encode(20086, nlohmann::json::object(), 2));
  read_until_opcode(client, 183);

  client.send(protocol.encode(20009, {{"text", "mail_send me gold 7"}}, 3));
  const auto notification = protocol.decode(read_until_opcode(client, 174)).at("message");
  const auto &wire_mail = notification.at("char_mail");
  ASSERT_EQ(wire_mail.at("attachments").size(), 1U);
  const auto saved_id = std::stoll(wire_mail.at("id").get<std::string>());
  using EpochTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
  const auto now =
      std::chrono::duration_cast<EpochTicks>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const auto saved = mail->mailbox(character.account_id, character.id, 1, now);
  ASSERT_EQ(saved.size(), 1U);
  EXPECT_EQ(saved.front().id, saved_id);
  ASSERT_EQ(saved.front().attachments.size(), 1U);
  EXPECT_EQ(saved.front().attachments.front().count, 7);

  const nlohmann::json forged{
      {"mail_send", {{"account_id", "9223372036854775807"}, {"money_amount", 9}}}};
  client.send(protocol.encode(20008, {{"cmds", nlohmann::json::array({forged})}}, 4));
  client.send(protocol.encode(20003, nlohmann::json::object(), 5));
  read_until_opcode(client, 4);
  {
    std::lock_guard lock(replies_mutex);
    ASSERT_FALSE(replies.empty());
    EXPECT_EQ(replies.back(), "Mail account does not own the selected recipient.");
  }
  EXPECT_EQ(mail->mailbox(character.account_id, character.id, 1, now).size(), 1U);
  client.disconnect();
}

} // namespace
} // namespace chronicles
