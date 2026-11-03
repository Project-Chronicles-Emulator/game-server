#include "auth/auth.hpp"
#include "persistence/character_repositories.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/hotkeys/hotkey_rest.hpp"

#include "p_a2c.ws.pb.h"
#include "p_c2a.ws.pb.h"
#include "p_c2f.rest.pb.h"
#include "p_c2f.ws.pb.h"
#include "p_f2c.ws.pb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace chronicles {
namespace {

constexpr int kWorldId = 1;
constexpr std::int64_t kAccountId = 1;
constexpr int kSummonId = 2001;
constexpr int kRepairKit = 140;

// Hot-key quick slots (REST CharHotKeyLoadReq / CharHotKeySaveReq)

class HotKeyRestTest : public testing::Test {
protected:
  HotKeyRestTest() {
    config_.game_address = "127.0.0.1:8081";
    auth_ = std::make_unique<AuthService>(repositories_.character_repositories(),
                                          config_);
    auth_->create_account("hotkey_owner", "test-world-password-123");
    const auto login = auth_->login("hotkey_owner", "test-world-password-123");
    token_ = login.access_token();
    x5::pb::authwebsocket::WorldEnterReq world_request;
    world_request.set_world_id(1);
    EXPECT_EQ(auth_->enter(token_, world_request).error_code(), x5::ec::Success);
    x5::pb::ws::front::CharCreateReq create_request;
    create_request.set_slot(0);
    create_request.set_char_name("Quickslot");
    create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
    character_id_ = auth_->create(token_, create_request).char_().char_id();
    x5::pb::ws::front::CharSelectReq select_request;
    select_request.set_char_id(character_id_);
    EXPECT_EQ(auth_->select(token_, select_request).ec(), x5::ec::Success);
    rest_ = std::make_unique<HotKeyRest>(*auth_, hotkeys_);
  }

  std::string path(const char *route, std::int64_t character) const {
    return std::string("/api/front/") + route + "/" + std::to_string(character) + "/1";
  }

  NativeHttpResponse load(std::int64_t character, const std::string &token) {
    return rest_->handle("GET", path("CharHotKeyLoadReq", character), "Bearer " + token, {}, {});
  }

  NativeHttpResponse save(const x5::pb::rest::CharHotKey &hotkey,
                          std::string_view content_type = "application/protobuf") {
    return rest_->handle("PUT", path("CharHotKeySaveReq", character_id_), "Bearer " + token_,
                         content_type, hotkey.SerializeAsString());
  }

  x5::pb::rest::CharHotKeys loaded() {
    const auto response = load(character_id_, token_);
    EXPECT_EQ(response.status, 200u);
    EXPECT_EQ(response.content_type, "application/protobuf");
    x5::pb::rest::CharHotKeys hotkeys;
    EXPECT_TRUE(hotkeys.ParseFromString(response.body));
    return hotkeys;
  }

  Config config_;
  MemoryRepositories repositories_;
  MemoryHotKeyRepository hotkeys_{repositories_.database};
  std::unique_ptr<AuthService> auth_;
  std::unique_ptr<HotKeyRest> rest_;
  std::string token_;
  std::int64_t character_id_{};
};

TEST_F(HotKeyRestTest, SavedSummonSlotIsLoadedBackAndAnEmptySlotClearsIt) {
  EXPECT_TRUE(HotKeyRest::handles("/api/front/CharHotKeyLoadReq/5/1"));
  EXPECT_TRUE(HotKeyRest::handles("/prefix/api/front/CharHotKeySaveReq/5/1?x=1"));
  EXPECT_FALSE(HotKeyRest::handles("/api/front/MailboxGetReq/5/1"));
  EXPECT_FALSE(HotKeyRest::handles("/api/front/CharHotKeyLoadReq/abc/1"));
  EXPECT_EQ(loaded().hot_keys_size(), 0);

  x5::pb::rest::CharHotKey summon;
  summon.set_slot(1);
  summon.set_summon_npc_cms_id(kSummonId);
  ASSERT_EQ(save(summon).status, 200u);
  x5::pb::rest::CharHotKey item;
  item.set_slot(2);
  item.set_item_cms_id(kRepairKit);
  ASSERT_EQ(save(item).status, 200u);

  const auto stored = loaded();
  ASSERT_EQ(stored.hot_keys_size(), 2);
  EXPECT_EQ(stored.hot_keys(0).slot(), 1u);
  EXPECT_EQ(stored.hot_keys(0).summon_npc_cms_id(), kSummonId);
  EXPECT_EQ(stored.hot_keys(1).item_cms_id(), kRepairKit);

  x5::pb::rest::CharHotKey clear;
  clear.set_slot(1);
  ASSERT_EQ(save(clear).status, 200u);
  ASSERT_EQ(loaded().hot_keys_size(), 1);
  EXPECT_EQ(loaded().hot_keys(0).slot(), 2u);
}

TEST_F(HotKeyRestTest, RefusesOtherCharactersBadTokensMethodsAndBodies) {
  EXPECT_EQ(load(character_id_ + 1, token_).status, 403u);
  EXPECT_EQ(load(character_id_, "not-a-session").status, 401u);
  EXPECT_EQ(rest_->handle("GET", path("CharHotKeyLoadReq", character_id_), "", {}, {}).status,
            401u);
  EXPECT_EQ(rest_->handle("PUT", path("CharHotKeyLoadReq", character_id_), "Bearer " + token_,
                          "application/protobuf", {})
                .status,
            405u);

  x5::pb::rest::CharHotKey summon;
  summon.set_slot(1);
  summon.set_summon_npc_cms_id(kSummonId);
  EXPECT_EQ(save(summon, "application/json").status, 415u);
  summon.set_slot(kMaximumHotKeySlot + 1);
  EXPECT_EQ(save(summon).status, 400u);
  EXPECT_EQ(rest_->handle("PUT", path("CharHotKeySaveReq", character_id_), "Bearer " + token_,
                          "application/protobuf", "\xff\xff\xff")
                .status,
            400u);
  EXPECT_EQ(loaded().hot_keys_size(), 0);
}

// The hosted server logged the client's load before CharSelectReq on a rejoin; the client does
// not repeat it after selection, so an unselected owned character must still be readable.
TEST_F(HotKeyRestTest, RejoinLoadsOwnedSlotsBeforeSelectionButSavesOnlyAfterIt) {
  x5::pb::rest::CharHotKey summon;
  summon.set_slot(1);
  summon.set_summon_npc_cms_id(kSummonId);
  ASSERT_EQ(save(summon).status, 200u);

  const auto rejoin = auth_->login("hotkey_owner", "test-world-password-123");
  const auto lobby_token = rejoin.access_token();
  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth_->enter(lobby_token, world_request).error_code(), x5::ec::Success);

  const auto response = load(character_id_, lobby_token);
  ASSERT_EQ(response.status, 200u);
  x5::pb::rest::CharHotKeys hotkeys;
  ASSERT_TRUE(hotkeys.ParseFromString(response.body));
  ASSERT_EQ(hotkeys.hot_keys_size(), 1);
  EXPECT_EQ(hotkeys.hot_keys(0).summon_npc_cms_id(), kSummonId);

  EXPECT_EQ(load(character_id_ + 1, lobby_token).status, 403u);
  x5::pb::rest::CharHotKey clear;
  clear.set_slot(1);
  const auto clear_path = path("CharHotKeySaveReq", character_id_);
  const auto lobby_authorization = "Bearer " + lobby_token;
  EXPECT_EQ(rest_
                ->handle("PUT", clear_path, lobby_authorization, "application/protobuf",
                         clear.SerializeAsString())
                .status,
            403u);

  // The refused save left the slot; after selection the same session may change it.
  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(character_id_);
  ASSERT_EQ(auth_->select(lobby_token, select_request).ec(), x5::ec::Success);
  const auto selected = load(character_id_, lobby_token);
  ASSERT_TRUE(hotkeys.ParseFromString(selected.body));
  EXPECT_EQ(hotkeys.hot_keys_size(), 1);
  EXPECT_EQ(rest_
                ->handle("PUT", clear_path, lobby_authorization, "application/protobuf",
                         clear.SerializeAsString())
                .status,
            200u);
}

TEST(HotKeyStorage, MemoryStoreKeepsSlotsPerOwnedCharacter) {
  MemoryRepositories store;
  MemoryHotKeyRepository hotkeys(store.database);
  Character created;
  created.account_id = kAccountId;
  created.name = "Slots";
  const auto character = store.characters->create(created);

  const CharacterHotKey summon{0, HotKeyKind::summon_npc, kSummonId};
  EXPECT_TRUE(hotkeys.save_character_hotkey(kAccountId, character.id, kWorldId, 3, summon));
  EXPECT_FALSE(hotkeys.save_character_hotkey(kAccountId + 1, character.id, kWorldId, 3, summon));
  const auto slots = hotkeys.character_hotkeys(kAccountId, character.id, kWorldId);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots.front().slot, 3);
  EXPECT_TRUE(hotkeys.character_hotkeys(kAccountId + 1, character.id, kWorldId).empty());
  EXPECT_THROW(hotkeys.save_character_hotkey(kAccountId, character.id, kWorldId, -1, summon),
               std::invalid_argument);
  EXPECT_TRUE(
      hotkeys.save_character_hotkey(kAccountId, character.id, kWorldId, 3, std::nullopt));
  EXPECT_TRUE(hotkeys.character_hotkeys(kAccountId, character.id, kWorldId).empty());
}

// Needs the disposable cluster migrated to schema version 21 (never port 5433).
TEST(HotKeyStorage, PostgresSlotsSurviveReconnectionAndRefuseOtherAccounts) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  auto store = make_postgres_character_repositories(connection);
  PostgresHotKeyRepository hotkeys(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account owner;
  owner.name = "slots_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(store.accounts->add_account(owner));
  const auto account = store.accounts->account(owner.name);
  ASSERT_TRUE(account);
  Character created;
  created.account_id = account->id;
  created.name = "Slots" + suffix.substr(suffix.size() - 8);
  const auto character = store.characters->create(created);

  const CharacterHotKey summon{0, HotKeyKind::summon_npc, kSummonId};
  const CharacterHotKey item{0, HotKeyKind::item, kRepairKit};
  ASSERT_TRUE(hotkeys.save_character_hotkey(account->id, character.id, kWorldId, 1, summon));
  ASSERT_TRUE(hotkeys.save_character_hotkey(account->id, character.id, kWorldId, 2, item));
  ASSERT_TRUE(hotkeys.save_character_hotkey(account->id, character.id, kWorldId, 2, summon));
  EXPECT_FALSE(
      hotkeys.save_character_hotkey(account->id + 1000000, character.id, kWorldId, 3, item));

  PostgresHotKeyRepository reopened(connection);
  auto slots = reopened.character_hotkeys(account->id, character.id, kWorldId);
  ASSERT_EQ(slots.size(), 2u);
  EXPECT_EQ(slots.at(0), (CharacterHotKey{1, HotKeyKind::summon_npc, kSummonId}));
  EXPECT_EQ(slots.at(1), (CharacterHotKey{2, HotKeyKind::summon_npc, kSummonId}));
  ASSERT_TRUE(
      reopened.save_character_hotkey(account->id, character.id, kWorldId, 1, std::nullopt));
  slots = reopened.character_hotkeys(account->id, character.id, kWorldId);
  ASSERT_EQ(slots.size(), 1u);
  EXPECT_EQ(slots.front().slot, 2);
}

} // namespace
} // namespace chronicles
