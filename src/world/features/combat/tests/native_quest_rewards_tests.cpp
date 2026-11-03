#include "persistence/character_repositories.hpp"
#include "testing/catalog_test_support.hpp"
#include "testing/company_test_support.hpp"
#include "world/features/guild/guild.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace chronicles {
namespace {

using namespace native_testing;
using testing_support::kBlueSalt;

TEST(NativeWorld, PostgresPlayerQuestAndQueuedMonsterRewardsSettleOnce) {
  const char *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL quest reward harness";
  }
  {
    pqxx::connection connection(connection_string);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "quest_rewards_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }

  testing_support::TestCatalog fixture({});
  const auto quest_directory = fixture.root() / "quests";
  std::filesystem::create_directories(quest_directory);
  nlohmann::json ordinary{
      {"cms_id", 503},
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
      {"company_contribution", 1},
      {"guild_experience", 2},
      {"dependent_items", nlohmann::json::array()},
      {"unsupported", nlohmann::json::array()},
  };
  auto hunt = ordinary;
  hunt["cms_id"] = 504;
  hunt["type"] = "None";
  hunt["progresses"] = nlohmann::json::array({{{"sequence", 1},
                                                 {"group", 1},
                                                 {"kind", "HuntMonster"},
                                                 {"parameters", {{"npcId", 5001}, {"count", 1}}}}});
  const nlohmann::json catalog{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"map_worlds", {{"9012", "ArcheWorld"}}},
      {"quests", {ordinary, hunt}},
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
  ASSERT_TRUE(auth.create_account("quest_rewards_player", "test-world-password-123"));
  const auto login = auth.login("quest_rewards_player", "test-world-password-123");
  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);
  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("QuestRewards");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  const auto character_id = created.char_().char_id();
  ASSERT_TRUE(repositories.characters->save_world_position(
      login.account_id(), character_id, 1, WorldPosition{9012, 0, 0, 0, 0}));

  auto company_store = std::make_shared<PostgresCompanyStore>(connection_string);
  company_store->adopt_quest_owner_epoch(epoch);
  auto companies = std::make_shared<CompanyService>(company_store,
                                                     testing_support::company_rules(), 1);
  ASSERT_EQ(companies->join(character_id, kBlueSalt).error, x5::ec::Success);
  auto guild_store = std::make_shared<PostgresGuildStore>(connection_string);
  guild_store->adopt_quest_service_owner_epoch(epoch);
  GuildRecord guild;
  guild.world_id = 1;
  guild.name = "QuestRewardsGuild";
  GuildMemberRecord master;
  master.char_id = character_id;
  master.grade = GuildGrade::master;
  ASSERT_TRUE(guild_store->create_guild(guild, master));
  auto guilds = std::make_shared<GuildService>(guild_store, GuildRules{}, 1);

  const auto before = repositories.character_state->load_world_progress(login.account_id(),
                                                                         character_id, 1);
  auto seeded = before;
  ++seeded.revision;
  seeded.quests[504] = QuestProgress{};
  ASSERT_TRUE(repositories.character_state->save_world_progress(login.account_id(), character_id,
                                                                  1, before, seeded));
  MonsterRewardReceipt receipt;
  receipt.account_id = login.account_id();
  receipt.character_id = character_id;
  receipt.world_id = 1;
  receipt.receipt_id = std::string(32, 'a');
  receipt.credited_unit_id = 4242;
  receipt.npc_cms_id = 5001;
  receipt.position = WorldPosition{9012, 0, 0, 0, 0};
  receipt.quest_attempts[504] = MonsterRewardQuestAttempt{};
  ASSERT_TRUE(repositories.character_state->enqueue_monster_rewards({receipt}));
  ASSERT_TRUE(repositories.character_state->mark_monster_reward_ready(receipt.receipt_id));

  NpcPlacement guide;
  guide.npc_cms_id = 1001539;
  guide.faction = "EX5Faction_TownNpc";
  guide.behavior = "Civilian";
  NpcPlacementSet placements;
  placements.map_id = 9012;
  placements.placements.push_back(guide);
  auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  ASSERT_EQ(population->units().size(), 1U);
  const auto guide_unit_id = population->units().front().unit_id;
  WorldSocialServices social;
  social.companies = companies;
  social.guilds = guilds;
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
    for (int count = 0; count < 64; ++count) {
      if (client.read().opcode() == expected) {
        return;
      }
    }
    ADD_FAILURE() << "Expected world opcode " << expected;
  };
  for (const std::uint16_t opcode : {1, 130, 126}) {
    read_through(opcode);
  }
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  read_through(176);
  for (const std::uint16_t opcode : {78, 78, 33, 115, 33}) {
    read_through(opcode);
  }
  client.send(protocol.encode(20086, nlohmann::json::object(), 2));
  client.send(protocol.encode(20003, nlohmann::json::object(), 3));
  read_through(kPongOpcode);

  auto progress = repositories.character_state->load_world_progress(login.account_id(),
                                                                     character_id, 1);
  ASSERT_TRUE(progress.quests.at(504).complete);
  auto pending = repositories.character_state->pending_quest_service_effects(1, character_id, 1);
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_TRUE(pending.front().monster_receipt);
  EXPECT_EQ(pending.front().effect_id, receipt.receipt_id);
  EXPECT_EQ(pending.front().effects.company_contribution, 1);
  EXPECT_EQ(pending.front().effects.guild_experience, 2);
  ASSERT_EQ(companies->settle_quest_effect(pending.front()).status,
            QuestEffectSettlementStatus::applied);
  pending = repositories.character_state->pending_quest_service_effects(1, character_id, 1);
  ASSERT_EQ(pending.size(), 1U);
  ASSERT_EQ(guilds->settle_quest_effect(pending.front()),
            QuestEffectSettlementStatus::applied);

  const nlohmann::json acceptance{
      {"quest_cms_id", 503},
      {"unit", {{"unit_id", std::to_string(guide_unit_id)}}},
      {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
  };
  client.send(protocol.encode(20016, acceptance, 4));
  client.send(protocol.encode(20003, nlohmann::json::object(), 5));
  read_through(kPongOpcode);
  progress = repositories.character_state->load_world_progress(login.account_id(), character_id, 1);
  ASSERT_TRUE(progress.quests.at(503).complete);
  pending = repositories.character_state->pending_quest_service_effects(1, character_id, 1);
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_FALSE(pending.front().monster_receipt);
  EXPECT_EQ(pending.front().effects.company_contribution, 1);
  EXPECT_EQ(pending.front().effects.guild_experience, 2);
  ASSERT_EQ(companies->settle_quest_effect(pending.front()).status,
            QuestEffectSettlementStatus::applied);
  pending = repositories.character_state->pending_quest_service_effects(1, character_id, 1);
  ASSERT_EQ(pending.size(), 1U);
  ASSERT_EQ(guilds->settle_quest_effect(pending.front()),
            QuestEffectSettlementStatus::applied);
  client.send(protocol.encode(20016, acceptance, 6));
  client.send(protocol.encode(20003, nlohmann::json::object(), 7));
  read_through(kPongOpcode);
  EXPECT_TRUE(
      repositories.character_state->pending_quest_service_effects(1, character_id, 1).empty());
  EXPECT_EQ(companies->status(character_id).record.contribution, 2);
  {
    pqxx::connection connection(connection_string);
    pqxx::read_transaction transaction(connection);
    const auto rows = transaction.exec(
        "SELECT effect_id FROM emulator_quest_service_effects WHERE character_id=$1",
        pqxx::params{character_id});
    EXPECT_EQ(rows.size(), 2U);
  }
  const auto reloaded = guild_store->load(1);
  ASSERT_EQ(reloaded.guilds.size(), 1U);
  ASSERT_EQ(reloaded.members.size(), 1U);
  EXPECT_EQ(reloaded.guilds.front().experience, 4);
  EXPECT_EQ(reloaded.members.front().contribution, 4);
}

} // namespace
} // namespace chronicles
