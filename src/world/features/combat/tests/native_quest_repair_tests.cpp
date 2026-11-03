#include "persistence/character_repositories.hpp"
#include "testing/catalog_test_support.hpp"
#include "testing/company_test_support.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;
using testing_support::kBlueSalt;

// The real store supplies all transaction and guard behavior. This wrapper only observes
// whether the login repair attempted a write and whether that attempt was refused.
class ObservedCompanyStore final : public CompanyStore {
public:
  explicit ObservedCompanyStore(std::shared_ptr<CompanyStore> inner)
      : inner_(std::move(inner)) {}

  std::vector<CompanyRecord> load(int world_id) override {
    return inner_->load(world_id);
  }

  void save(const CompanyRecord &record) override {
    ++attempts;
    try {
      inner_->save(record);
    } catch (...) {
      ++failures;
      throw;
    }
  }

  void adopt_quest_owner_epoch(std::int64_t epoch) override {
    inner_->adopt_quest_owner_epoch(epoch);
  }

  CompanyQuestSettlementResult read_quest_effect(const QuestServiceEffectEvent &event) override {
    return inner_->read_quest_effect(event);
  }

  CompanyQuestSettlementResult settle_quest_effect(const QuestServiceEffectEvent &event,
                                                    const CompanyRecord &before,
                                                    const CompanyQuestPlanner &planner) override {
    return inner_->settle_quest_effect(event, before, planner);
  }

  std::atomic<int> attempts{};
  std::atomic<int> failures{};

private:
  std::shared_ptr<CompanyStore> inner_;
};

TEST(NativeWorld, PostgresLoginRetriesHistoricalUnlockAfterPendingEffectSettles) {
  const char *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL quest repair harness";
  }
  {
    pqxx::connection connection(connection_string);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "quest_repair_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }

  testing_support::TestCatalog fixture({});
  const auto quest_directory = fixture.root() / "quests";
  std::filesystem::create_directories(quest_directory);
  const nlohmann::json quest{
      {"cms_id", 502},
      {"type", "HiddenAutoComplete"},
      {"accept", {{{"kind", "NpcId"}, {"npc_id", 1001539}}}},
      {"conditions", nlohmann::json::array()},
      {"completion", {{"kind", "Auto"}}},
      {"supplies", nlohmann::json::array()},
      {"auto_complete", true},
      {"sequential", false},
      {"progresses", nlohmann::json::array()},
      {"experience", 0},
      {"rewards", nlohmann::json::array()},
      {"company_unlocks", {{{"company_type", kBlueSalt}, {"level", 2}}}},
      {"dependent_items", nlohmann::json::array()},
      {"unsupported", nlohmann::json::array()},
  };
  const nlohmann::json catalog{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"map_worlds", {{"9012", "ArcheWorld"}}},
      {"quests", {quest}},
  };
  {
    std::ofstream output(quest_directory / "catalog.json");
    ASSERT_TRUE(output);
    output << catalog;
  }

  auto repositories = make_postgres_character_repositories(connection_string);
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories, config);
  const auto epoch = auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("quest_repair_player", "test-world-password-123"));
  const auto login = auth.login("quest_repair_player", "test-world-password-123");
  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);
  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("QuestRepair");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  const auto character_id = created.char_().char_id();
  ASSERT_TRUE(repositories.characters->save_world_position(
      login.account_id(), character_id, 1, WorldPosition{9012, 0, 0, 0, 0}));

  auto company_store = std::make_shared<ObservedCompanyStore>(
      std::make_shared<PostgresCompanyStore>(connection_string));
  company_store->adopt_quest_owner_epoch(epoch);
  auto companies = std::make_shared<CompanyService>(company_store,
                                                     testing_support::company_rules(), 1);
  ASSERT_EQ(companies->join(character_id, kBlueSalt).error, x5::ec::Success);
  company_store->attempts = 0;
  company_store->failures = 0;
  auto before = repositories.character_state->load_world_progress(login.account_id(), character_id, 1);
  auto completed = before;
  ++completed.revision;
  completed.quests[502].complete = true;
  completed.quests[502].success_count = 1;
  ASSERT_TRUE(repositories.character_state->save_world_progress(
      login.account_id(), character_id, 1, before, completed));

  const auto membership = repositories.character_state->read_quest_service_membership(
      login.account_id(), character_id, 1);
  ASSERT_TRUE(membership);
  QuestServiceEffects effects;
  effects.expected_company_type = membership->company_type;
  effects.expected_company_generation = membership->company_generation;
  effects.expected_guild_id = membership->guild_id;
  effects.expected_guild_generation = membership->guild_generation;
  effects.company_contribution = 1;
  const auto committed = repositories.character_state->commit_quest_mutation(
      login.account_id(), character_id, 1, std::string(32, 'a'), completed, std::nullopt,
      ItemTrade{}, effects);
  ASSERT_EQ(committed.status, QuestMutationStatus::committed);
  const auto pending = repositories.character_state->pending_quest_service_effects(1, character_id, 1);
  ASSERT_EQ(pending.size(), 1U);

  NpcPlacementSet placements;
  placements.map_id = 9012;
  auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  WorldSocialServices social;
  social.companies = companies;
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, {}, fixture.root() / "client", population,
                              {}, {}, {}, {}, {}, social);
  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(character_id);
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  WorldTestClient client(server.port());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  client.send_key(selected.packet_key());
  ASSERT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  ASSERT_EQ(client.read().opcode(), 175);
  const auto read_through = [&](std::uint16_t expected) {
    for (int count = 0; count < 32; ++count) {
      if (client.read().opcode() == expected) {
        return;
      }
    }
    ADD_FAILURE() << "Expected world entry opcode " << expected;
  };
  for (const std::uint16_t opcode : {1, 130, 126}) {
    read_through(opcode);
  }
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  read_through(176);
  for (const std::uint16_t opcode : {78, 78, 33, 115, 33}) {
    read_through(opcode);
  }
  client.send(protocol.encode(20003, nlohmann::json::object(), 2));
  read_through(kPongOpcode);
  ASSERT_EQ(company_store->failures.load(), 1);
  EXPECT_EQ(companies->status(character_id).record.unlocked_position_level, 0);

  ASSERT_EQ(companies->settle_quest_effect(pending.front()).status,
            QuestEffectSettlementStatus::applied);
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  client.send(protocol.encode(20001, nlohmann::json::object(), 3));
  client.send(protocol.encode(20003, nlohmann::json::object(), 4));
  read_through(kPongOpcode);
  EXPECT_GE(company_store->attempts.load(), 2);
  EXPECT_EQ(companies->status(character_id).record.unlocked_position_level, 2);
  const auto reloaded = company_store->load(1);
  const auto saved = std::ranges::find(reloaded, character_id, &CompanyRecord::char_id);
  ASSERT_NE(saved, reloaded.end());
  EXPECT_EQ(saved->unlocked_position_level, 2);
}

} // namespace
} // namespace chronicles
