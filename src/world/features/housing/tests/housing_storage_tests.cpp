#include "testing/company_test_support.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/housing/guild_housing.hpp"
#include "world/features/housing/housing_service.hpp"
#include "world/features/housing/housing_state.hpp"
#include "persistence/postgres_repositories.hpp"
#include "testing/memory_repositories.hpp"
#include "testing/housing_test_support.hpp"
#include "world/features/housing/housing_world.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace chronicles::housing {
namespace {

SiteContents sample_contents() {
  SiteContents contents;
  contents.house_part_custom_ids = std::string("\x00\x7f\xff", 3);
  HouseRecord house;
  house.house_id = 1;
  house.attach_index = -1;
  house.housing_id = 1002;
  house.position = {-4800, -300, 0};
  house.yaw = 90.5F;
  house.parts_state = 0b101;
  house.from_storage = true;
  contents.houses.emplace(1, house);

  DoodadRecord doodad;
  doodad.doodad_id = 4;
  doodad.house_id = 1;
  doodad.cms_id = 160067;
  doodad.phase = 2;
  doodad.phase_begin_tick = 12345;
  doodad.position = {10, 20, 30};
  doodad.pitch = 1.5F;
  doodad.yaw = -45.0F;
  doodad.roll = 0.25F;
  doodad.interaction_count = 3;
  doodad.parent_doodad_id = 2;
  doodad.from_storage = false;
  contents.doodads.emplace(DoodadKey{1, 4}, doodad);
  return contents;
}

TEST(HousingStorage, ContentsRoundTripThroughJson) {
  const auto contents = sample_contents();
  const auto encoded = site_contents_json(contents);
  EXPECT_EQ(encoded.at("custom_ids"), "007fff");
  EXPECT_EQ(site_contents_from_json(encoded), contents);
}

TEST(HousingStorage, RejectsContentsOutsideTheRecordLayout) {
  auto contents = sample_contents();
  contents.houses.at(1).house_id = 2;
  EXPECT_THROW(validate_site_contents(contents), std::invalid_argument);

  contents = sample_contents();
  contents.house_part_custom_ids.assign(257, 'x');
  EXPECT_THROW(validate_site_contents(contents), std::invalid_argument);

  auto encoded = site_contents_json(sample_contents());
  encoded["custom_ids"] = "0";
  EXPECT_THROW(site_contents_from_json(encoded), std::runtime_error);
}

TEST(HousingStorage, MemoryStoreRejectsStaleVersions) {
  MemoryRepositories characters;
  MemoryHousingStore store(characters.character_state);

  SiteRecord site;
  site.key = SiteKey{112, 1, 101};
  site.version = 1;
  site.owner_character_id = 1;
  site.owner_name = "Owner";
  HousingCommit insert;
  insert.world_id = 1;
  insert.sites.push_back({site, 0, false});
  ASSERT_TRUE(store.commit(insert));
  EXPECT_FALSE(store.commit(insert));

  auto updated = site;
  updated.version = 2;
  HousingCommit stale;
  stale.world_id = 1;
  stale.sites.push_back({updated, 5, false});
  EXPECT_FALSE(store.commit(stale));

  HousingCommit current;
  current.world_id = 1;
  current.sites.push_back({updated, 1, false});
  ASSERT_TRUE(store.commit(current));
  const auto snapshot = store.load(1);
  ASSERT_EQ(snapshot.sites.size(), 1U);
  EXPECT_EQ(snapshot.sites.front().version, 2);
  EXPECT_TRUE(store.load(2).sites.empty());
}

std::string read_migration() {
  std::ifstream file(CHRONICLES_MIGRATION_FILE);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

// PostgreSQL round trip on a disposable database named by CHRONICLES_TUTORIAL_TEST_DATABASE_URL.
TEST(HousingStorage, PostgresCommitsHousingAndProgressTogether) {
  const auto *database_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!database_url) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_TEST_DATABASE_URL to a disposable database";
  }

  auto characters = make_postgres_character_repositories(database_url);
  migrate_postgres(database_url, read_migration());
  const auto unique = std::to_string(
      std::chrono::system_clock::now().time_since_epoch().count() % 1'000'000'000);
  Account account;
  account.name = "housing" + unique;
  account.salt = "salt";
  account.hash = "hash";
  ASSERT_TRUE(characters.accounts->add_account(account));
  const auto stored_account = characters.accounts->account(account.name);
  ASSERT_TRUE(stored_account.has_value());
  Character created;
  created.account_id = stored_account->id;
  created.name = "Hs" + unique.substr(0, 9);
  const auto character = characters.characters->create(created);

  PostgresHousingStore store(database_url);
  const auto before = store.load_progress(stored_account->id, character.id, character.world_id);
  auto after = before;
  after.revision = before.revision + 1;
  after.stackables[123] = 40;
  after.housing_items[1900000002] = HousingItemRecord{2, 1};

  SiteRecord site;
  site.key = SiteKey{112, 1, 101};
  site.version = 1;
  site.owner_character_id = character.id;
  site.owner_account_id = stored_account->id;
  site.owner_name = character.name;
  site.contract_type = ContractType::periodic;
  site.expiry_epoch_ticks = 999;
  site.return_position = WorldPosition{8000, 1, 2, 3, 4.0F};
  site.contents = sample_contents();
  const int world_id = 900000 + static_cast<int>(std::stoll(unique) % 1000);

  HousingCommit commit;
  commit.world_id = world_id;
  commit.progress.push_back({stored_account->id, character.id, character.world_id, before, after});
  commit.sites.push_back({site, 0, false});
  commit.next_package_id = 7;
  ASSERT_TRUE(store.commit(commit));

  // The same progress revision cannot be spent twice; nothing else changes either.
  auto second_site = site;
  second_site.key.lot_number = 102;
  HousingCommit replay;
  replay.world_id = world_id;
  replay.progress.push_back({stored_account->id, character.id, character.world_id, before, after});
  replay.sites.push_back({second_site, 0, false});
  EXPECT_FALSE(store.commit(replay));

  const auto snapshot = store.load(world_id);
  ASSERT_EQ(snapshot.sites.size(), 1U);
  EXPECT_EQ(snapshot.sites.front(), site);
  EXPECT_EQ(snapshot.next_package_id, 7);
  const auto reloaded = store.load_progress(stored_account->id, character.id, character.world_id);
  EXPECT_EQ(reloaded.stackables.at(123), 40);
  EXPECT_EQ(reloaded.housing_items.at(1900000002), (HousingItemRecord{2, 1}));

  HousingCommit erase;
  erase.world_id = world_id;
  erase.sites.push_back({site, 1, true});
  ASSERT_TRUE(store.commit(erase));
  const auto after_erase = store.load(world_id);
  EXPECT_TRUE(after_erase.sites.empty());
  EXPECT_EQ(after_erase.next_package_id, 7);
}

// Regression on PostgreSQL. A guild departure and a disband commit before their housing
// cleanup runs; in 0.2.5 a cleanup lost to a storage failure, a session timeout or a server
// stop was never retried. Each phase below reopens both services from the database, as a
// restart does, with no guild event subscriber, so the cleanup is lost every time.
TEST(HousingStorage, PostgresRestartRepairsGuildHousingWhoseCleanupWasLost) {
  const auto *database_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!database_url) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_TEST_DATABASE_URL to a disposable database";
  }

  constexpr std::int64_t kStartTicks = 638'000'000'000'000'000;
  constexpr int kGuildRentPermits = 3000;
  constexpr std::int64_t kGuildUnlockTokens = 5000;
  auto characters = make_postgres_character_repositories(database_url);
  migrate_postgres(database_url, read_migration());
  const auto unique = std::to_string(
      std::chrono::system_clock::now().time_since_epoch().count() % 1'000'000'000);
  Account account;
  account.name = "guildhousing" + unique;
  account.salt = "salt";
  account.hash = "hash";
  ASSERT_TRUE(characters.accounts->add_account(account));
  const auto stored_account = characters.accounts->account(account.name);
  ASSERT_TRUE(stored_account.has_value());
  Character created;
  created.account_id = stored_account->id;
  created.name = "Gm" + unique.substr(0, 9);
  const auto master = characters.characters->create(created);
  created.name = "Gr" + unique.substr(0, 9);
  created.slot = 1;
  const auto resident = characters.characters->create(created);
  const int world_id = master.world_id;

  const auto rent_before =
      characters.character_state->load_world_progress(stored_account->id, master.id, world_id);
  auto rent_after = rent_before;
  rent_after.revision = rent_before.revision + 1;
  rent_after.stackables[testing_support::kPermitItem] += kGuildRentPermits;
  rent_after.hersander_token += kGuildUnlockTokens;
  ASSERT_TRUE(characters.character_state->save_world_progress(stored_account->id, master.id,
                                                              world_id, rent_before, rent_after));

  const auto open_guilds = [&] {
    return std::make_shared<GuildService>(std::make_shared<PostgresGuildStore>(database_url),
                                          GuildRules{}, world_id);
  };
  const auto open_housing = [&](const std::shared_ptr<GuildService> &guilds) {
    return std::make_shared<HousingService>(
        testing_support::housing_catalog(), std::make_shared<PostgresHousingStore>(database_url),
        world_id, [] { return kStartTicks; }, HousingPolicy{},
        std::make_shared<LegacyGuildServiceDirectory>(guilds));
  };
  const auto guild_actor = [&](const Character &character) {
    GuildActor actor;
    actor.world_id = world_id;
    actor.char_id = character.id;
    actor.account_id = stored_account->id;
    actor.name = character.name;
    actor.level = 10;
    actor.company_type = chronicles::testing_support::kAntiqus;
    actor.company_position = 3;
    return actor;
  };

  std::int64_t guild_id = 0;
  {
    const auto guilds = open_guilds();
    const auto housing = open_housing(guilds);
    GuildCreateRequest create;
    create.name = "Gh" + unique.substr(0, 9);
    create.join_type = 1;
    const auto guild = guilds->create(guild_actor(master), create);
    ASSERT_EQ(guild.error, x5::ec::Success);
    guild_id = guild.guild.id;
    ASSERT_EQ(guilds->join(guild_actor(resident), guild_id).error, x5::ec::Success);

    HousingActor payer;
    payer.account_id = stored_account->id;
    payer.character_id = master.id;
    payer.world_id = world_id;
    payer.name = master.name;
    ASSERT_EQ(housing->guild_contract(payer, guild_id, testing_support::kZoneKey,
                                      ContractType::monthly, "Harbor", nullptr)
                  .ec,
              x5::ec::Success);
    GuildResident lot;
    lot.lot_number = testing_support::kSmallLot;
    lot.character_id = resident.id;
    ASSERT_EQ(housing->guild_allow(payer, guild_id, {lot}, {}).ec, x5::ec::Success);

    // Reconciliation leaves a current member's site alone.
    HousingHub hub(housing);
    EXPECT_EQ(hub.reconcile_guild_housing(), 0u);
    ASSERT_TRUE(housing->guild_site_of(resident.id).has_value());

    ASSERT_EQ(guilds->leave(guild_actor(resident), guild_id).error, x5::ec::Success);
  }

  {
    const auto guilds = open_guilds();
    const auto housing = open_housing(guilds);
    ASSERT_TRUE(housing->guild_site_of(resident.id).has_value());
    HousingHub hub(housing);
    EXPECT_EQ(hub.reconcile_guild_housing(), 0u);
    EXPECT_FALSE(housing->guild_site_of(resident.id).has_value());
    EXPECT_TRUE(housing->guild_area(guild_id).has_value());

    ASSERT_EQ(guilds->gm_disband(guild_id), x5::ec::Success);
  }

  {
    const auto guilds = open_guilds();
    const auto housing = open_housing(guilds);
    EXPECT_FALSE(housing->guild_site_of(resident.id).has_value());
    ASSERT_TRUE(housing->guild_area(guild_id).has_value());
    HousingHub hub(housing);
    EXPECT_EQ(hub.reconcile_guild_housing(), 0u);
    EXPECT_FALSE(housing->guild_area(guild_id).has_value());
  }

  const auto guilds = open_guilds();
  const auto housing = open_housing(guilds);
  EXPECT_FALSE(housing->guild_area(guild_id).has_value());
  EXPECT_FALSE(housing->guild_site_of(resident.id).has_value());
  EXPECT_TRUE(housing->stale_guild_housing().empty());
}

} // namespace
} // namespace chronicles::housing
