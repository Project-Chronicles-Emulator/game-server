#include "persistence/character_repositories.hpp"
#include "world/features/trade/tests/pausing_region_checkpoint_store.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;

class DeathFenceRelease {
public:
  explicit DeathFenceRelease(std::shared_ptr<PausingRegionCheckpointStore> store)
      : store_(std::move(store)) {
  }

  ~DeathFenceRelease() {
    store_->release_death_fence_pause();
  }

private:
  std::shared_ptr<PausingRegionCheckpointStore> store_;
};

struct EntryFrames {
  std::vector<DecodedWorldFrame> load;
  std::vector<DecodedWorldFrame> playing;
};

void collect_until(WorldTestClient &client, Protocol &protocol, std::uint16_t target,
                   std::vector<DecodedWorldFrame> &frames) {
  for (int index = 0; index < 512; ++index) {
    const auto frame = client.read();
    const auto opcode = frame.opcode();
    if (opcode == 176 || opcode == 19 || opcode == 23 || opcode == 32 || opcode == 169) {
      frames.push_back({opcode, protocol.decode(frame).at("message")});
    }
    if (opcode == target) {
      return;
    }
  }
  throw std::runtime_error("Expected native world frame was not received");
}

EntryFrames enter_playing(WorldTestClient &client, Protocol &protocol) {
  EntryFrames result;
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  collect_until(client, protocol, 175, result.load);
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  collect_until(client, protocol, 176, result.load);
  client.send(protocol.encode(20086, nlohmann::json::object(), 2));
  client.send(protocol.encode(20003, nlohmann::json::object(), 3));
  collect_until(client, protocol, 4, result.playing);
  return result;
}

std::vector<int> equipped_durability(const Character &character) {
  std::vector<int> result;
  for (const auto &item : character.items) {
    if (item.equip_slot) {
      result.push_back(item.durability);
    }
  }
  return result;
}

TEST(NativeDeathPostgres, LostDeathCommitAckFencesAdmissionAndReconnectsDead) {
  const char *database_url = std::getenv("CHRONICLES_DEATH_E2E_DATABASE_URL");
  const char *proxy_url = std::getenv("CHRONICLES_DEATH_E2E_PROXY_URL");
  const char *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!database_url || !proxy_url || !catalog_environment) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL death harness and local profile";
  }
  {
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "death_reconciliation_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }

  auto repositories = make_postgres_character_repositories(database_url);
  auto forwarding = std::make_shared<PausingRegionCheckpointStore>(repositories.character_state);
  auto serving = repositories;
  serving.character_state = forwarding;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(serving, config);
  const auto epoch = auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("death_test", "test-world-password-123"));
  ASSERT_TRUE(auth.set_account_role("death_test", AccountRole::game_master));
  const auto login = auth.login("death_test", "test-world-password-123");
  x5::pb::authwebsocket::WorldEnterReq realm;
  realm.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), realm).error_code(), x5::ec::Success);
  x5::pb::ws::front::CharCreateReq create;
  create.set_slot(0);
  create.set_char_name("DeathTest");
  create.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  const auto character_id = created.char_().char_id();
  const WorldPosition position{9012, -1242227, 2993829, -647306, 0};
  ASSERT_TRUE(repositories.characters->save_world_position(login.account_id(), character_id, 1,
                                                            position));

  const std::filesystem::path catalog_path(catalog_environment);
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  TemporaryWorldCertificate certificate;
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  TwoClientWorldServer server(auth, certificate, {}, CharacterStats::load(*catalog), catalog,
                              catalog_path);
  DeathFenceRelease release_fence(forwarding);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(character_id);
  const auto selected = auth.select(login.access_token(), select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  auto first = std::make_unique<WorldTestClient>(server.port());
  first->send_key(selected.packet_key());
  ASSERT_EQ(first->read().opcode(), 5);
  enter_playing(*first, protocol);
  const auto before_death = repositories.characters->character_by_id(character_id);
  ASSERT_TRUE(before_death);
  const auto durability_before_death = equipped_durability(*before_death);
  ASSERT_FALSE(durability_before_death.empty());

  auto proxy = make_postgres_character_repositories(proxy_url);
  proxy.character_state->adopt_monster_reward_owner_epoch(epoch);
  forwarding->arm_death_commit_proxy(proxy.character_state);
  forwarding->arm_death_fence_pause();
  first->send(protocol.encode(20009, {{"text", "suicide"}}, 4));
  ASSERT_TRUE(forwarding->wait_for_death_fence_pause());
  const auto committed = repositories.character_state->load_world_progress(
      login.account_id(), character_id, 1);
  ASSERT_TRUE(committed.dead);
  const auto after_death = repositories.characters->character_by_id(character_id);
  ASSERT_TRUE(after_death);
  const auto durability_after_death = equipped_durability(*after_death);
  EXPECT_NE(durability_after_death, durability_before_death);

  const auto retry_selection = auth.select(login.access_token(), select);
  ASSERT_EQ(retry_selection.ec(), x5::ec::Success);
  {
    WorldTestClient refused(server.port());
    refused.send_key(retry_selection.packet_key());
    EXPECT_THROW(refused.read(), boost::system::system_error);
  }
  forwarding->release_death_fence_pause();
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
  ASSERT_TRUE(reconnected) << "Old death owner did not drain and release admission";
  const auto entry = enter_playing(*reconnected, protocol);
  const auto maps = frames_of(entry.load, 176);
  ASSERT_EQ(maps.size(), 1U);
  EXPECT_EQ(maps.front().at("nub").at("player").at("state"), "EX5UnitStateType_Demise");
  EXPECT_EQ(frames_of(entry.playing, 23).size(), 1U);
  EXPECT_TRUE(frames_of(entry.playing, 19).empty());
  EXPECT_TRUE(frames_of(entry.playing, 169).empty());
  const auto final_progress = repositories.character_state->load_world_progress(
      login.account_id(), character_id, 1);
  EXPECT_TRUE(final_progress.dead);
  const auto final_character = repositories.characters->character_by_id(character_id);
  ASSERT_TRUE(final_character);
  EXPECT_EQ(equipped_durability(*final_character), durability_after_death);
  reconnected->disconnect();
}

} // namespace
} // namespace chronicles
