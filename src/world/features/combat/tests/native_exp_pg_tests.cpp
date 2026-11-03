#include "persistence/character_repositories.hpp"
#include "world/core/game_rates.hpp"
#include "world/features/character/progression.hpp"
#include "world/features/trade/tests/pausing_region_checkpoint_store.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;

class QuestFenceRelease {
public:
  explicit QuestFenceRelease(std::shared_ptr<PausingRegionCheckpointStore> store)
      : store_(std::move(store)) {
  }

  ~QuestFenceRelease() {
    store_->allow_fences();
  }

private:
  std::shared_ptr<PausingRegionCheckpointStore> store_;
};

struct ExpEntry {
  nlohmann::json player;
  std::vector<DecodedWorldFrame> playing;
};

void read_until(WorldTestClient &client, Protocol &protocol, std::uint16_t target,
                std::vector<DecodedWorldFrame> &frames) {
  for (int index = 0; index < 512; ++index) {
    const auto frame = client.read();
    const auto opcode = frame.opcode();
    if (opcode == 176 || opcode == 10 || opcode == 36) {
      frames.push_back({opcode, protocol.decode(frame).at("message")});
    }
    if (opcode == target) {
      return;
    }
  }
  throw std::runtime_error("Expected EXP test world frame was not received");
}

ExpEntry enter_exp_world(WorldTestClient &client, Protocol &protocol) {
  ExpEntry entry;
  std::vector<DecodedWorldFrame> loaded;
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  read_until(client, protocol, 175, loaded);
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  read_until(client, protocol, 176, loaded);
  const auto maps = frames_of(loaded, 176);
  if (maps.size() != 1) {
    throw std::runtime_error("Expected one EXP test map load");
  }
  entry.player = maps.front().at("nub").at("player");
  client.send(protocol.encode(20086, nlohmann::json::object(), 2));
  client.send(protocol.encode(20003, nlohmann::json::object(), 3));
  read_until(client, protocol, 4, entry.playing);
  return entry;
}

std::string find_exp_monster(const std::vector<DecodedWorldFrame> &frames, int cms_id) {
  for (const auto &frame : frames_of(frames, 10)) {
    if (frame.at("nub").contains("npc") &&
        frame.at("nub").at("npc").at("npc_cms_id") == cms_id) {
      return frame.at("unit_id").get<std::string>();
    }
  }
  return {};
}

TEST(NativeExpPostgres, MissingQuestProfileLostCommitAckFencesAndRestoresThreshold) {
  const char *database_url = std::getenv("CHRONICLES_EXP_E2E_DATABASE_URL");
  const char *proxy_url = std::getenv("CHRONICLES_EXP_E2E_PROXY_URL");
  const char *catalog_environment = std::getenv("CHRONICLES_EXP_E2E_CATALOG");
  if (!database_url || !proxy_url || !catalog_environment) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL EXP harness and local profile";
  }
  {
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "exp_reconciliation_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }

  const std::filesystem::path catalog_path(catalog_environment);
  ASSERT_FALSE(std::filesystem::exists(catalog_path.parent_path() / "quests" / "catalog.json"));
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  const auto progression = Progression::load(*catalog);
  const auto threshold = progression->next_level_experience(1);
  ASSERT_TRUE(threshold);
  ASSERT_GT(*threshold, 0);
  std::ifstream input(catalog_path.parent_path() / "world-mobs" / "definitions.json");
  ASSERT_TRUE(input);
  nlohmann::json mobs;
  input >> mobs;
  const auto map = std::ranges::find_if(mobs.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == 9012;
  });
  ASSERT_NE(map, mobs.at("maps").end());
  const auto spawn = std::ranges::find_if(map->at("spawns"), [&](const auto &entry) {
    return progression->monster_experience(entry.at("cms_id").get<int>()) > 0;
  });
  ASSERT_NE(spawn, map->at("spawns").end());
  const int npc_cms_id = spawn->at("cms_id");
  const auto &point = spawn->at("position");
  const WorldPosition position{9012, point.at(0), point.at(1), point.at(2), 0};

  auto repositories = make_postgres_character_repositories(database_url);
  auto forwarding = std::make_shared<PausingRegionCheckpointStore>(repositories.character_state);
  auto serving = repositories;
  serving.character_state = forwarding;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(serving, config);
  const auto epoch = auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("exp_test", "test-world-password-123"));
  ASSERT_TRUE(auth.set_account_role("exp_test", AccountRole::game_master));
  const auto login = auth.login("exp_test", "test-world-password-123");
  x5::pb::authwebsocket::WorldEnterReq realm;
  realm.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), realm).error_code(), x5::ec::Success);
  x5::pb::ws::front::CharCreateReq create;
  create.set_slot(0);
  create.set_char_name("ExpTest");
  create.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  const auto character_id = created.char_().char_id();
  ASSERT_TRUE(repositories.characters->save_world_position(login.account_id(), character_id, 1,
                                                            position));
  const auto initial = repositories.character_state->load_world_progress(
      login.account_id(), character_id, 1);
  auto seeded = initial;
  ++seeded.revision;
  seeded.experience = *threshold - 1;
  seeded.level = 1;
  ASSERT_TRUE(repositories.character_state->save_world_progress(
      login.account_id(), character_id, 1, initial, seeded));

  NpcPlacementSet empty_population;
  empty_population.map_id = 9012;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  TemporaryWorldCertificate certificate;
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  TwoClientWorldServer server(auth, certificate, {}, CharacterStats::load(*catalog), catalog,
                              catalog_path, population);
  QuestFenceRelease release_fence(forwarding);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(character_id);
  const auto selected = auth.select(login.access_token(), select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  auto first = std::make_unique<WorldTestClient>(server.port());
  first->send_key(selected.packet_key());
  ASSERT_EQ(first->read().opcode(), 5);
  const auto entry = enter_exp_world(*first, protocol);
  const auto monster = find_exp_monster(entry.playing, npc_cms_id);
  ASSERT_FALSE(monster.empty());
  const auto before_reward = repositories.character_state->load_world_progress(
      login.account_id(), character_id, 1);
  ASSERT_EQ(before_reward.level, 1);
  ASSERT_EQ(before_reward.experience, *threshold - 1);
  auto expected = before_reward;
  const auto award = scale_experience_reward(progression->monster_experience(npc_cms_id));
  ASSERT_GT(award, 0);
  progression->award_experience(expected, award);
  ASSERT_GT(expected.level, before_reward.level);

  auto proxy = make_postgres_character_repositories(proxy_url);
  proxy.character_state->adopt_monster_reward_owner_epoch(epoch);
  forwarding->arm_quest_commit_proxy(proxy.character_state);
  forwarding->refuse_fences_for(character_id);
  first->send(protocol.encode(20009, {{"text", "monster_kill " + monster}}, 4));
  ASSERT_TRUE(forwarding->wait_for_refused_quest_fence());
  const auto committed = repositories.character_state->load_world_progress(
      login.account_id(), character_id, 1);
  EXPECT_EQ(committed.experience, expected.experience);
  EXPECT_EQ(committed.level, expected.level);
  EXPECT_EQ(committed.level_up_points, expected.level_up_points);

  const auto retry_selection = auth.select(login.access_token(), select);
  ASSERT_EQ(retry_selection.ec(), x5::ec::Success);
  {
    WorldTestClient refused(server.port());
    refused.send_key(retry_selection.packet_key());
    EXPECT_THROW(refused.read(), boost::system::system_error);
  }
  forwarding->allow_fences();
  first->disconnect();
  first.reset();
  std::unique_ptr<WorldTestClient> reconnected;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline) {
    auto attempt = std::make_unique<WorldTestClient>(server.port());
    attempt->send_key(retry_selection.packet_key());
    try {
      if (attempt->read().opcode() == 5) {
        reconnected = std::move(attempt);
        break;
      }
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  ASSERT_TRUE(reconnected) << "EXP reward owner did not drain and release admission";
  const auto restored = enter_exp_world(*reconnected, protocol);
  EXPECT_EQ(restored.player.at("level"), expected.level);
  EXPECT_EQ(restored.player.at("exp"), std::to_string(expected.experience));
  const auto final_progress = repositories.character_state->load_world_progress(
      login.account_id(), character_id, 1);
  EXPECT_EQ(final_progress.experience, expected.experience);
  EXPECT_EQ(final_progress.level, expected.level);
  EXPECT_EQ(final_progress.level_up_points, expected.level_up_points);
  EXPECT_TRUE(repositories.character_state->pending_monster_rewards(
                  login.account_id(), character_id, 1).empty());
  {
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto count = transaction.exec1(
        "SELECT count(*) FROM emulator_monster_reward_receipts WHERE processed");
    EXPECT_EQ(count[0].as<int>(), 1);
  }
  reconnected->disconnect();
}

} // namespace
} // namespace chronicles
