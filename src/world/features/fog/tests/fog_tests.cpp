#include "world/features/fog/fog_rest.hpp"
#include "world/features/fog/fog_reveal.hpp"

#include "persistence/memory_database.hpp"
#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "testing/memory_repositories.hpp"

#include "p_c2f.rest.pb.h"
#include "p_f2c.rest.pb.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iterator>
#include <numeric>

namespace chronicles {
namespace {

constexpr int kWorldId = 1;
constexpr std::int64_t kAccountId = 73;

Character create_character(CharacterRepository &characters, std::int64_t account_id,
                           int slot = 0) {
  Character character;
  character.account_id = account_id;
  character.slot = slot;
  character.name = "Fog" + std::to_string(account_id) + "Slot" + std::to_string(slot);
  return characters.create(character);
}

class FogRestTest : public testing::Test {
protected:
  FogRestTest()
      : fog_(repositories_.database),
        auth_(repositories_.character_repositories(), Config{}), rest_(auth_, fog_) {
    auth_.create_account("fog_owner", "fog-test-password-123");
    token_ = auth_.login("fog_owner", "fog-test-password-123").access_token();
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(kWorldId);
    EXPECT_EQ(auth_.enter(token_, enter).error_code(), x5::ec::Success);
    x5::pb::ws::front::CharCreateReq create;
    create.set_slot(0);
    create.set_char_name("FogExplorer");
    create.set_char_class(x5::cms::EX5CharacterClass_Fight);
    character_id_ = auth_.create(token_, create).char_().char_id();
    select();
  }

  void select() {
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(character_id_);
    EXPECT_EQ(auth_.select(token_, select).ec(), x5::ec::Success);
  }

  std::string path(const char *route) const {
    return std::string("/api/front/") + route + "/" + std::to_string(character_id_);
  }

  NativeHttpResponse save(const std::vector<int> &areas) {
    x5::pb::rest::FogAreaSaveReq request;
    for (const int area : areas) {
      request.add_fog_area_ids(area);
    }
    return rest_.handle("PUT", path("CharFogAreaSaveReq"), "Bearer " + token_,
                        "application/protobuf", request.SerializeAsString());
  }

  std::vector<int> load() {
    const auto response =
        rest_.handle("GET", path("CharFogAreaLoadReq"), "Bearer " + token_, {}, {});
    EXPECT_EQ(response.status, 200u);
    EXPECT_EQ(response.content_type, "application/protobuf");
    x5::pb::rest::FogAreaLoadRes message;
    EXPECT_TRUE(message.ParseFromString(response.body));
    EXPECT_EQ(message.ec(), x5::ec::Success);
    return {message.fog_area_ids().begin(), message.fog_area_ids().end()};
  }

  MemoryRepositories repositories_;
  MemoryFogRepository fog_;
  AuthService auth_;
  FogRest rest_;
  std::string token_;
  std::int64_t character_id_{};
};

TEST_F(FogRestTest, IncrementalDuplicateAndEmptySavesSurviveRelogin) {
  EXPECT_TRUE(load().empty());
  ASSERT_EQ(save({30}).status, 200u);
  ASSERT_EQ(save({10, 20, 30}).status, 200u);
  ASSERT_EQ(save({10}).status, 200u);
  ASSERT_EQ(save({}).status, 200u);
  EXPECT_EQ(load(), (std::vector<int>{10, 20, 30}));

  auth_.logout(token_);
  token_ = auth_.login("fog_owner", "fog-test-password-123").access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(kWorldId);
  ASSERT_EQ(auth_.enter(token_, enter).error_code(), x5::ec::Success);
  select();
  EXPECT_EQ(load(), (std::vector<int>{10, 20, 30}));
}

TEST_F(FogRestTest, RefusesForeignCharactersBadSessionsMethodsAndPayloads) {
  const auto load_path = path("CharFogAreaLoadReq");
  const auto save_path = path("CharFogAreaSaveReq");
  EXPECT_EQ(rest_.handle("GET", load_path, "Bearer invalid", {}, {}).status, 401u);
  EXPECT_EQ(rest_.handle("GET", load_path, {}, {}, {}).status, 401u);
  EXPECT_EQ(rest_.handle("GET", load_path + "999", "Bearer " + token_, {}, {}).status, 403u);
  EXPECT_EQ(rest_.handle("POST", load_path, "Bearer " + token_, {}, {}).status, 405u);
  EXPECT_EQ(rest_.handle("GET", save_path, "Bearer " + token_, {}, {}).status, 405u);
  EXPECT_EQ(rest_.handle("PUT", save_path, "Bearer " + token_, "application/json", {}).status,
            415u);
  EXPECT_EQ(
      rest_.handle("PUT", save_path, "Bearer " + token_, "application/protobuf-bad", {}).status,
      415u);
  EXPECT_EQ(
      rest_.handle("PUT", save_path, "Bearer " + token_, "application/protobuf", "\xff").status,
      400u);
  EXPECT_EQ(rest_
                .handle("PUT", save_path, "Bearer " + token_, "application/protobuf",
                        std::string(32769, '\0'))
                .status,
            413u);
  EXPECT_EQ(save({10, -1}).status, 400u);
  EXPECT_EQ(save({0}).status, 400u);
  EXPECT_TRUE(load().empty());
}

TEST_F(FogRestTest, MatchesExactClientRoutesAndLaunchGrantPrefixes) {
  EXPECT_TRUE(FogRest::handles(path("CharFogAreaLoadReq")));
  EXPECT_TRUE(FogRest::handles("/launch/test" + path("CharFogAreaSaveReq") + "?v=1"));
  EXPECT_FALSE(FogRest::handles(path("CharFogAreaLoadReq") + "/1"));
  EXPECT_FALSE(FogRest::handles("/api/front/CharFogAreaSaveReq/-1"));
  EXPECT_FALSE(FogRest::handles("/api/front/CharFogAreaSaveReq/9223372036854775808"));
  EXPECT_FALSE(FogRest::handles("/api/front/CharHotKeyLoadReq/1/1"));
}

TEST_F(FogRestTest, CatalogRejectsUnknownAreasBeforeAnyWrite) {
  const auto catalog_path =
      std::filesystem::path(CHRONICLES_PROJECT_ROOT) / "data/generated/catalog";
  if (!std::filesystem::exists(catalog_path / "manifest.json")) {
    GTEST_SKIP() << "Matching-build catalog not available";
  }
  ClientCatalog catalog(catalog_path);
  FogRest rest(auth_, fog_, &catalog);
  x5::pb::rest::FogAreaSaveReq request;
  request.add_fog_area_ids(10);
  request.add_fog_area_ids(2147483647);
  EXPECT_EQ(rest.handle("PUT", path("CharFogAreaSaveReq"), "Bearer " + token_,
                        "application/protobuf", request.SerializeAsString())
                .status,
            400u);
  EXPECT_TRUE(load().empty());
  request.mutable_fog_area_ids()->RemoveLast();
  EXPECT_EQ(rest.handle("PUT", path("CharFogAreaSaveReq"), "Bearer " + token_,
                        "application/protobuf; charset=binary", request.SerializeAsString())
                .status,
            200u);
  EXPECT_EQ(load(), (std::vector<int>{10}));
}

TEST_F(FogRestTest, GmRevealMergesEveryCatalogAreaAndSurvivesDelayedSaves) {
  const auto catalog_path =
      std::filesystem::path(CHRONICLES_PROJECT_ROOT) / "data/generated/catalog";
  if (!std::filesystem::exists(catalog_path / "manifest.json")) {
    GTEST_SKIP() << "Matching-build catalog not available";
  }
  ClientCatalog catalog(catalog_path);
  const auto known = catalog.fog_area_ids();
  ASSERT_FALSE(known.empty());

  // World handoffs are server-issued; a non-GM handoff cannot use this operation.
  const auto account = repositories_.accounts->account("fog_owner");
  ASSERT_TRUE(account);
  const auto account_id = account->id;
  Handoff handoff;
  handoff.account_id = account_id;
  handoff.character_id = character_id_;
  handoff.world_id = kWorldId;
  EXPECT_FALSE(reveal_world_map(fog_, handoff, catalog).has_value());
  EXPECT_TRUE(load().empty());

  handoff.game_master = true;
  auto foreign = handoff;
  ++foreign.account_id;
  EXPECT_FALSE(reveal_world_map(fog_, foreign, catalog).has_value());
  auto wrong_world = handoff;
  ++wrong_world.world_id;
  EXPECT_FALSE(reveal_world_map(fog_, wrong_world, catalog).has_value());

  ASSERT_EQ(save({known.front()}).status, 200u);
  const auto before =
      repositories_.character_state->load_world_progress(account_id, character_id_, kWorldId);
  EXPECT_EQ(reveal_world_map(fog_, handoff, catalog), known.size());
  EXPECT_EQ(load(), known);
  EXPECT_EQ(reveal_world_map(fog_, handoff, catalog), known.size());
  EXPECT_EQ(save({known.front()}).status, 200u);
  EXPECT_EQ(save({}).status, 200u);
  EXPECT_EQ(load(), known);
  EXPECT_EQ(repositories_.character_state->load_world_progress(account_id, character_id_,
                                                               kWorldId)
                .revision,
            before.revision);

  auth_.logout(token_);
  token_ = auth_.login("fog_owner", "fog-test-password-123").access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(kWorldId);
  ASSERT_EQ(auth_.enter(token_, enter).error_code(), x5::ec::Success);
  select();
  EXPECT_EQ(load(), known);
}

TEST(FogStorage, MemoryKeepsCharactersWorldsAndGameplayRevisionsIndependent) {
  MemoryRepositories store;
  MemoryFogRepository fog(store.database);
  const auto first = create_character(*store.characters, kAccountId);
  const auto second = create_character(*store.characters, kAccountId, 1);
  const auto before = store.character_state->load_world_progress(kAccountId, first.id, kWorldId);
  ASSERT_TRUE(fog.discover_fog_areas(kAccountId, first.id, kWorldId, {10, 20}));
  EXPECT_FALSE(fog.discover_fog_areas(kAccountId + 1, first.id, kWorldId, {30}));
  EXPECT_FALSE(fog.discover_fog_areas(kAccountId, first.id, kWorldId + 1, {30}));
  EXPECT_TRUE(fog.character_fog_areas(kAccountId + 1, first.id, kWorldId).empty());
  EXPECT_TRUE(fog.character_fog_areas(kAccountId, second.id, kWorldId).empty());

  auto after = before;
  ++after.revision;
  after.gold = 42;
  ASSERT_TRUE(
      store.character_state->save_world_progress(kAccountId, first.id, kWorldId, before, after));
  EXPECT_EQ(fog.character_fog_areas(kAccountId, first.id, kWorldId), (std::vector<int>{10, 20}));
  ASSERT_TRUE(store.characters->delete_character(first.id));
  EXPECT_TRUE(fog.character_fog_areas(kAccountId, first.id, kWorldId).empty());
}

void check_capacity_and_atomicity(FogRepository &fog, std::int64_t account_id,
                                  std::int64_t character_id) {
  std::vector<int> full(kMaximumFogAreas);
  std::iota(full.begin(), full.end(), 1);
  ASSERT_TRUE(fog.discover_fog_areas(account_id, character_id, kWorldId, full));
  ASSERT_TRUE(fog.discover_fog_areas(account_id, character_id, kWorldId, {1, 1}));
  EXPECT_THROW(fog.discover_fog_areas(account_id, character_id, kWorldId, {999999}),
               std::invalid_argument);
  EXPECT_THROW(fog.discover_fog_areas(account_id, character_id, kWorldId, {1, -1}),
               std::invalid_argument);
  EXPECT_THROW(fog.discover_fog_areas(account_id, character_id, kWorldId,
                                      std::vector<int>(kMaximumFogAreas + 1, 1)),
               std::invalid_argument);
  EXPECT_EQ(fog.character_fog_areas(account_id, character_id, kWorldId), full);
}

TEST(FogStorage, MemoryEnforcesCapacityWithoutLosingPriorDiscoveries) {
  MemoryRepositories store;
  MemoryFogRepository fog(store.database);
  const auto character = create_character(*store.characters, kAccountId);
  check_capacity_and_atomicity(fog, kAccountId, character.id);
}

// The fog repository's deletion hook erases a deleted character's areas, like the
// former memory table and PostgreSQL's ON DELETE CASCADE.
TEST(Fog, MemoryRepositoryForgetsDeletedCharacters) {
  auto database = std::make_shared<MemoryDatabase>();
  MemoryAccountRepository accounts(database);
  MemoryCharacterRepository characters(database);
  MemoryFogRepository fog(database);

  Account account;
  account.name = "fogged";
  ASSERT_TRUE(accounts.add_account(account));
  const auto stored_account = accounts.account("fogged");
  ASSERT_TRUE(stored_account);

  const auto character = create_character(characters, stored_account->id);
  ASSERT_TRUE(fog.discover_fog_areas(stored_account->id, character.id, kWorldId, {1, 2}));
  ASSERT_TRUE(characters.delete_character(character.id));
  EXPECT_TRUE(fog.character_fog_areas(stored_account->id, character.id, kWorldId).empty());
}

TEST(FogStorage, PostgresMergesConcurrentSavesAndSurvivesReopen) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires isolated PostgreSQL test cluster";
  }
  auto store = make_postgres_character_repositories(connection);
  std::ifstream migration(std::filesystem::path(CHRONICLES_PROJECT_ROOT) /
                          "migrations/001_accounts_characters.sql");
  ASSERT_TRUE(migration);
  const std::string sql(std::istreambuf_iterator<char>{migration}, {});
  migrate_postgres(connection, sql);
  migrate_postgres(connection, sql);
  Account owner;
  owner.name = "fog_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(store.accounts->add_account(owner));
  const auto account = store.accounts->account(owner.name);
  ASSERT_TRUE(account);
  const auto character = create_character(*store.characters, account->id);
  const auto second = create_character(*store.characters, account->id, 1);

  auto first_save = std::async(std::launch::async, [&] {
    PostgresFogRepository writer(connection);
    return writer.discover_fog_areas(account->id, character.id, kWorldId, {10, 20});
  });
  auto second_save = std::async(std::launch::async, [&] {
    PostgresFogRepository writer(connection);
    return writer.discover_fog_areas(account->id, character.id, kWorldId, {20, 30});
  });
  ASSERT_TRUE(first_save.get());
  ASSERT_TRUE(second_save.get());

  PostgresFogRepository reopened(connection);
  ASSERT_TRUE(reopened.discover_fog_areas(account->id, character.id, kWorldId, {}));
  ASSERT_TRUE(reopened.discover_fog_areas(account->id, character.id, kWorldId, {10}));
  EXPECT_FALSE(reopened.discover_fog_areas(account->id + 99999, character.id, kWorldId, {40}));
  EXPECT_FALSE(reopened.discover_fog_areas(account->id, character.id, kWorldId + 1, {40}));
  EXPECT_TRUE(reopened.character_fog_areas(account->id + 99999, character.id, kWorldId).empty());
  EXPECT_TRUE(reopened.character_fog_areas(account->id, second.id, kWorldId).empty());
  EXPECT_EQ(reopened.character_fog_areas(account->id, character.id, kWorldId),
            (std::vector<int>{10, 20, 30}));
  check_capacity_and_atomicity(reopened, account->id, second.id);

  const auto catalog_path =
      std::filesystem::path(CHRONICLES_PROJECT_ROOT) / "data/generated/catalog";
  if (std::filesystem::exists(catalog_path / "manifest.json")) {
    ClientCatalog catalog(catalog_path);
    Handoff handoff;
    handoff.account_id = account->id;
    handoff.character_id = character.id;
    handoff.world_id = kWorldId;
    handoff.game_master = true;
    const auto areas = catalog.fog_area_ids();
    ASSERT_EQ(reveal_world_map(reopened, handoff, catalog), areas.size());
    ASSERT_TRUE(reopened.discover_fog_areas(account->id, character.id, kWorldId, {10}));
    const auto saved = reopened.character_fog_areas(account->id, character.id, kWorldId);
    for (const auto area : areas) {
      EXPECT_TRUE(std::binary_search(saved.begin(), saved.end(), area));
    }
    EXPECT_EQ(reveal_world_map(reopened, handoff, catalog), areas.size());
  }

  ASSERT_TRUE(store.characters->delete_character(character.id));
  EXPECT_TRUE(reopened.character_fog_areas(account->id, character.id, kWorldId).empty());
}

} // namespace
} // namespace chronicles
