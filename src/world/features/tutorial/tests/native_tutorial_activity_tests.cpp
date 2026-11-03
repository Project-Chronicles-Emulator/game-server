#include "persistence/character_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "testing/company_test_support.hpp"
#include "world/features/trade/tests/pausing_region_checkpoint_store.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;

// The companion harness provisions a disposable database and, for the lost-ACK cases,
// proves that the one routed transaction received a real COMMIT before dropping its reply.
TEST(NativeTutorialActivity, PostgresCommitFaultReconnectsWithoutDuplicateReward) {
  const auto *database = std::getenv("CHRONICLES_ACTIVITY_TEST_DATABASE_URL");
  const auto *proxy_database = std::getenv("CHRONICLES_ACTIVITY_PROXY_DATABASE_URL");
  const auto *scenario_value = std::getenv("CHRONICLES_ACTIVITY_TEST_SCENARIO");
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!database || !scenario_value || !profile_path) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL tutorial fault harness";
  }
  std::string scenario{scenario_value};
  const std::string unaffiliated_prefix = "unaffiliated_";
  const bool unaffiliated = scenario.starts_with(unaffiliated_prefix);
  if (unaffiliated) {
    scenario.erase(0, unaffiliated_prefix.size());
  }
  const bool before_commit = scenario == "before";
  const bool item_reward = scenario == "item";
  ASSERT_TRUE(before_commit || item_reward || scenario == "progress");
  ASSERT_TRUE(before_commit || proxy_database != nullptr);

  const auto original = TutorialData::load(profile_path);
  auto document = original->document;
  auto &object = document.at("objects").at(0);
  object["experience"] = 200;
  object["rewards"] = nlohmann::json::array();
  if (item_reward) {
    object["rewards"].push_back(
        {{"item_cms_id", 72000016}, {"quantity", 1},
         {"instance", {{"grade", "Normal"}, {"max_durability", 10000}}}});
  }
  const auto definitions = std::make_shared<const TutorialData>(std::move(document));
  const auto &target = definitions->document.at("objects").at(0);
  const auto &point = target.at("position");
  const auto catalog_root = std::filesystem::path(profile_path).parent_path().parent_path() /
                            "catalog";
  auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  const auto stats = CharacterStats::load(*catalog);

  auto repositories = make_postgres_character_repositories(database);
  const auto direct_store = repositories.character_state;
  auto fault_store = std::make_shared<PausingRegionCheckpointStore>(direct_store);
  repositories.character_state = fault_store;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(std::move(repositories), config);
  const auto epoch = auth.acquire_monster_reward_owner_epoch();
  ASSERT_GT(epoch, 0);
  ASSERT_EQ(auth.recover_staged_monster_rewards(kMonsterRewardRecoveryPageSize), 0U);

  auto company_store = std::make_shared<PostgresCompanyStore>(database);
  company_store->adopt_quest_owner_epoch(epoch);
  auto company = std::make_shared<CompanyService>(company_store,
                                                   testing_support::company_rules(), 1);
  ASSERT_TRUE(auth.create_account("activity_fixture", "activity-fixture-password"));
  const auto login = auth.login("activity_fixture", "activity-fixture-password");
  x5::pb::authwebsocket::WorldEnterReq enter_world;
  enter_world.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), enter_world).error_code(), x5::ec::Success);

  auto characters = make_postgres_character_repositories(database).characters;
  Character character;
  character.account_id = login.account_id();
  character.name = "ActivityFixture";
  character.class_id = 1;
  character.world_position = WorldPosition{8000, point.at(0), point.at(1), point.at(2), 0};
  character = characters->create(character);
  ASSERT_TRUE(characters->save_world_position(character.account_id, character.id, 1,
                                              *character.world_position));
  const auto initial_character = characters->character_by_id(character.id);
  ASSERT_TRUE(initial_character);
  const auto initial_item_count = initial_character->items.size();
  if (!unaffiliated) {
    ASSERT_EQ(company->join(character.id, testing_support::kAntiqus).error, x5::ec::Success);
  }

  WorldSocialServices social;
  social.companies = company;
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, definitions, stats, catalog, catalog_root,
                              {}, {}, {}, {}, {}, {}, std::move(social));
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto connect = [&]() -> std::unique_ptr<WorldTestClient> {
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(character.id);
    const auto selected = auth.select(login.access_token(), select);
    auto client = std::make_unique<WorldTestClient>(server.port());
    client->send_key(selected.packet_key());
    if (client->read().opcode() != 5) {
      return {};
    }
    client->send(protocol.encode(20001, nlohmann::json::object(), 0));
    EXPECT_EQ(client->read().opcode(), 175);
    EXPECT_EQ(client->read().opcode(), 1);
    EXPECT_EQ(client->read().opcode(), 125);
    read_entry_inventory(*client);
    EXPECT_EQ(client->read().opcode(), 135);
    for (int index = 0; index < 5; ++index) {
      EXPECT_EQ(protocol.decode(client->read()).at("status"), "decoded");
    }
    client->send(protocol.encode(20085, nlohmann::json::object(), 1));
    EXPECT_EQ(read_until_opcode(*client, 176).opcode(), 176);
    expect_pre_playing_combat_bootstrap(*client, protocol);
    const auto active_npcs = std::ranges::count_if(definitions->npcs, [](const auto &entry) {
      return entry.second.value("initially_active", true);
    });
    const auto count = definitions->document.at("sectors").size() + active_npcs;
    for (std::size_t index = 0; index < count; ++index) {
      EXPECT_EQ(protocol.decode(client->read()).at("status"), "decoded");
    }
    return client;
  };

  auto client = connect();
  ASSERT_TRUE(client);
  const auto before = direct_store->load_world_progress(character.account_id, character.id, 1);
  if (before_commit) {
    fault_store->fail_next_quest_commit_before_write();
  } else {
    auto proxy = std::make_shared<PostgresCharacterStateRepository>(proxy_database);
    proxy->adopt_monster_reward_owner_epoch(epoch);
    fault_store->arm_quest_commit_proxy(std::move(proxy));
  }
  const nlohmann::json pickup{{"target_unit_id", target.at("unit_id")},
                              {"param", {{"interaction_id", target.at("interaction_id")}}}};
  client->send(protocol.encode(20068, pickup, 2));
  try {
    while (client->read().opcode() != 2) {
    }
  } catch (const std::exception &) {
    // An uncertain commit deliberately closes this connection.
  }
  client->disconnect();
  client.reset();

  // The reconciliation owner keeps admission until its revision fence and readback finish.
  for (int attempt = 0; attempt < 40 && !client; ++attempt) {
    try {
      client = connect();
    } catch (const std::exception &) {
    }
    if (!client) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  ASSERT_TRUE(client) << "Tutorial reconnect remained blocked after reconciliation";
  auto events = direct_store->pending_quest_service_effects(1, character.id, 10);
  if (before_commit) {
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(direct_store->load_world_progress(character.account_id, character.id, 1).experience,
              before.experience);
    client->send(protocol.encode(20068, pickup, 2));
    for (int index = 0; index < 40; ++index) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      events = direct_store->pending_quest_service_effects(1, character.id, 10);
      const auto progress = direct_store->load_world_progress(character.account_id, character.id, 1);
      if (progress.experience > before.experience) {
        events = direct_store->pending_quest_service_effects(1, character.id, 10);
        break;
      }
    }
  }
  if (unaffiliated) {
    EXPECT_TRUE(events.empty());
    const auto committed = direct_store->load_world_progress(character.account_id, character.id, 1);
    ASSERT_GT(committed.experience, before.experience);
    const auto stored_character = characters->character_by_id(character.id);
    ASSERT_TRUE(stored_character);
    EXPECT_EQ(stored_character->items.size(), initial_item_count + (item_reward ? 1U : 0U));

    // A repeated interaction after reconnect cannot award the same object or item twice.
    const std::uint32_t replay_sequence = before_commit ? 3 : 2;
    client->send(protocol.encode(20068, pickup, replay_sequence));
    client->send(protocol.encode(20003, nlohmann::json::object(), replay_sequence + 1));
    EXPECT_EQ(read_until_opcode(*client, 4).opcode(), 4);
    const auto replayed = direct_store->load_world_progress(character.account_id, character.id, 1);
    EXPECT_EQ(replayed.experience, committed.experience);
    const auto replayed_character = characters->character_by_id(character.id);
    ASSERT_TRUE(replayed_character);
    EXPECT_EQ(replayed_character->items.size(), stored_character->items.size());
    client->disconnect();
    return;
  }
  ASSERT_EQ(events.size(), 1U);
  const auto event = events.front();
  EXPECT_GT(event.effects.activity_experience, 0);
  const auto committed = direct_store->load_world_progress(character.account_id, character.id, 1);
  EXPECT_EQ(committed.experience - before.experience, event.effects.activity_experience);
  ASSERT_EQ(company->settle_quest_effect(event).status, QuestEffectSettlementStatus::applied);
  const auto contribution = company->status(character.id).record.contribution;
  EXPECT_EQ(contribution, event.effects.activity_experience /
                              event.effects.activity_company_rate);
  EXPECT_EQ(company->settle_quest_effect(event).status,
            QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(company->status(character.id).record.contribution, contribution);
  const auto stored_character = characters->character_by_id(character.id);
  ASSERT_TRUE(stored_character);
  EXPECT_EQ(stored_character->items.size(), initial_item_count + (item_reward ? 1U : 0U));
  client->disconnect();
}

} // namespace
} // namespace chronicles
