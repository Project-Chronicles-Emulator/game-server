#include "world/features/friends/friends.hpp"
#include "world/features/friends/native_friend.hpp"
#include "services/front/native_ws.hpp"
#include "testing/memory_repositories.hpp"
#include "p_friend.pb.h"
#include "p_system.pb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace chronicles {
namespace {

constexpr int kRealm = 1;
constexpr std::int64_t kFirstTick = 17'000'000'000'000'000;

// In-memory characters and guild labels for service tests.
class FakeFriendDirectory final : public FriendDirectory {
public:
  void add(const CharacterSummary &character) {
    characters_[character.id] = character;
  }

  void set_guild(std::int64_t character_id, const std::string &guild_name) {
    guilds_[character_id] = guild_name;
  }

  std::optional<CharacterSummary> character(std::int64_t character_id) override {
    const auto found = characters_.find(character_id);
    if (found == characters_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::optional<CharacterSummary> character_by_name(int world_id,
                                                    const std::string &name) override {
    for (const auto &[id, character] : characters_) {
      if (character.world_id == world_id && character.name == name) {
        return character;
      }
    }
    return std::nullopt;
  }

  std::vector<CharacterSummary> account_characters(std::int64_t account_id, int world_id) override {
    std::vector<CharacterSummary> owned;
    for (const auto &[id, character] : characters_) {
      if (character.account_id == account_id && character.world_id == world_id) {
        owned.push_back(character);
      }
    }
    return owned;
  }

  std::string guild_name(std::int64_t character_id) override {
    const auto found = guilds_.find(character_id);
    return found == guilds_.end() ? std::string() : found->second;
  }

private:
  std::map<std::int64_t, CharacterSummary> characters_;
  std::map<std::int64_t, std::string> guilds_;
};

CharacterSummary character(std::int64_t id, std::int64_t account_id, const std::string &name,
                           int level = 5) {
  CharacterSummary summary;
  summary.id = id;
  summary.account_id = account_id;
  summary.world_id = kRealm;
  summary.class_id = 1;
  summary.level = level;
  summary.name = name;
  return summary;
}

class FriendServiceTest : public testing::Test {
protected:
  FriendServiceTest() {
    directory_->add(character(101, 1, "Alice"));
    directory_->add(character(201, 2, "Bobby"));
    directory_->add(character(301, 3, "Carol"));
    directory_->add(character(302, 3, "Caroline"));
    service_ = make_service();
  }

  std::unique_ptr<FriendService> make_service(FriendRules rules = {}) {
    auto service =
        std::make_unique<FriendService>(store_, directory_, rules, kRealm, [this] { return now_; });
    service->subscribe([this](const std::vector<FriendEvent> &events) {
      events_.insert(events_.end(), events.begin(), events.end());
    });
    return service;
  }

  static FriendActor actor(std::int64_t account_id, std::int64_t char_id) {
    return FriendActor{kRealm, account_id, char_id};
  }

  std::vector<FriendUpdated> updates() const {
    std::vector<FriendUpdated> found;
    for (const auto &event : events_) {
      if (const auto *update = std::get_if<FriendUpdated>(&event)) {
        found.push_back(*update);
      }
    }
    return found;
  }

  std::vector<FriendAddedAlarm> alarms() const {
    std::vector<FriendAddedAlarm> found;
    for (const auto &event : events_) {
      if (const auto *alarm = std::get_if<FriendAddedAlarm>(&event)) {
        found.push_back(*alarm);
      }
    }
    return found;
  }

  std::int64_t now_ = kFirstTick;
  std::shared_ptr<MemoryFriendStore> store_ = std::make_shared<MemoryFriendStore>();
  std::shared_ptr<FakeFriendDirectory> directory_ = std::make_shared<FakeFriendDirectory>();
  std::vector<FriendEvent> events_;
  std::unique_ptr<FriendService> service_;
};

TEST_F(FriendServiceTest, AddListsTheFriendAndRecommendsTheRequesterUntilAddedBack) {
  const auto alice = actor(1, 101);
  const auto bobby = actor(2, 201);

  const auto added = service_->add(alice, 2);
  ASSERT_EQ(added.error, x5::ec::Success);
  EXPECT_EQ(added.added.account_id, 2);
  // An offline friend carries no character, so the alias defaults to the character name.
  EXPECT_EQ(added.added.alias, "Bobby");
  EXPECT_EQ(added.added.created_epoch_ticks, kFirstTick);
  EXPECT_FALSE(added.added.character.has_value());

  const auto alice_friends = service_->friends(alice);
  ASSERT_EQ(alice_friends.size(), 1);
  EXPECT_EQ(alice_friends.front().account_id, 2);
  EXPECT_TRUE(service_->recommended(alice).empty());

  const auto bobby_recommended = service_->recommended(bobby);
  ASSERT_EQ(bobby_recommended.size(), 1);
  EXPECT_EQ(bobby_recommended.front().account_id, 1);
  EXPECT_EQ(bobby_recommended.front().alias, "Alice");
  EXPECT_TRUE(service_->friends(bobby).empty());

  ASSERT_EQ(service_->add(bobby, 1).error, x5::ec::Success);
  EXPECT_TRUE(service_->recommended(bobby).empty());
  EXPECT_EQ(service_->friends(bobby).size(), 1);
}

TEST_F(FriendServiceTest, AddRefusalsUseTheClientsFriendErrorMapping) {
  const auto alice = actor(1, 101);

  EXPECT_EQ(service_->add(alice, 1).error, x5::ec::BadRequest);
  EXPECT_EQ(service_->add(alice, 99).error, x5::ec::InvalidChar);
  EXPECT_EQ(service_->add(alice, 0).error, x5::ec::InvalidChar);

  ASSERT_EQ(service_->add(alice, 2).error, x5::ec::Success);
  EXPECT_EQ(service_->add(alice, 2).error, x5::ec::DuplicateRequest);

  FriendRules one_friend;
  one_friend.max_friends = 1;
  service_ = make_service(one_friend);
  EXPECT_EQ(service_->add(alice, 3).error, x5::ec::InvalidState);
}

TEST_F(FriendServiceTest, SearchFindsCharactersByNameIdAndAccount) {
  const auto alice = actor(1, 101);
  now_ = kFirstTick + 50;
  service_->character_entered_world(kRealm, 301);
  now_ = kFirstTick + 90;
  service_->character_left_world(kRealm, 301);

  const auto by_name = service_->search(alice, {FriendSearchKind::character_name, 0, "Caroline"});
  ASSERT_EQ(by_name.error, x5::ec::Success);
  EXPECT_EQ(by_name.found.account_id, 3);
  ASSERT_TRUE(by_name.found.character.has_value());
  EXPECT_EQ(by_name.found.character->char_id, 302);
  ASSERT_TRUE(by_name.offline_since_epoch_ticks.has_value());
  EXPECT_EQ(*by_name.offline_since_epoch_ticks, kFirstTick + 90);

  // An account search resolves to the character the account last played.
  const auto by_account = service_->search(alice, {FriendSearchKind::account_id, 3, {}});
  ASSERT_EQ(by_account.error, x5::ec::Success);
  EXPECT_EQ(by_account.found.character->char_id, 301);

  const auto by_id = service_->search(alice, {FriendSearchKind::character_id, 201, {}});
  ASSERT_EQ(by_id.error, x5::ec::Success);
  EXPECT_EQ(by_id.found.account_id, 2);

  service_->character_entered_world(kRealm, 201);
  const auto online = service_->search(alice, {FriendSearchKind::character_id, 201, {}});
  EXPECT_FALSE(online.offline_since_epoch_ticks.has_value());

  EXPECT_EQ(service_->search(alice, {FriendSearchKind::character_name, 0, "alice2"}).error,
            x5::ec::InvalidChar);
  EXPECT_EQ(service_->search(alice, {FriendSearchKind::steam_friend_code, 0, "123"}).error,
            x5::ec::InvalidChar);
}

TEST_F(FriendServiceTest, PresenceChangesReachOnlineOwnersAsFriendUpdates) {
  const auto alice = actor(1, 101);
  ASSERT_EQ(service_->add(alice, 2).error, x5::ec::Success);
  directory_->set_guild(201, "Wanderers");

  // Bobby's departure is not announced to an owner who is offline.
  service_->character_entered_world(kRealm, 201);
  EXPECT_TRUE(updates().empty());

  service_->character_entered_world(kRealm, 101);
  service_->character_left_world(kRealm, 201);
  service_->character_entered_world(kRealm, 201);
  auto received = updates();
  ASSERT_EQ(received.size(), 2);
  EXPECT_EQ(received.back().recipient_char_id, 101);
  ASSERT_TRUE(received.back().view.character.has_value());
  EXPECT_EQ(received.back().view.character->name, "Bobby");
  EXPECT_EQ(received.back().view.character->guild_name, "Wanderers");
  EXPECT_EQ(received.back().view.alias, "Bobby");

  now_ = kFirstTick + 1000;
  service_->character_left_world(kRealm, 201);
  received = updates();
  ASSERT_EQ(received.size(), 3);
  EXPECT_FALSE(received.back().view.character.has_value());
  EXPECT_EQ(received.back().view.last_login_epoch_ticks, kFirstTick + 1000);

  const auto listed = service_->friends(alice);
  ASSERT_EQ(listed.size(), 1);
  EXPECT_EQ(listed.front().last_login_epoch_ticks, kFirstTick + 1000);
}

TEST_F(FriendServiceTest, ASecondConnectionKeepsTheAccountOnlineUntilBothLeave) {
  service_->character_entered_world(kRealm, 201);
  service_->character_entered_world(kRealm, 201);
  service_->character_left_world(kRealm, 201);
  EXPECT_TRUE(service_->online(2));

  service_->character_left_world(kRealm, 201);
  EXPECT_FALSE(service_->online(2));

  // A stale departure of a replaced character does not end the newer presence.
  service_->character_entered_world(kRealm, 301);
  service_->character_entered_world(kRealm, 302);
  service_->character_left_world(kRealm, 301);
  EXPECT_TRUE(service_->online(3));
}

TEST_F(FriendServiceTest, OnlyANewRecommendationAlarmsAnOnlineTarget) {
  service_->character_entered_world(kRealm, 101);
  service_->character_entered_world(kRealm, 201);

  ASSERT_EQ(service_->add(actor(2, 201), 1).error, x5::ec::Success);
  auto received = alarms();
  ASSERT_EQ(received.size(), 1);
  EXPECT_EQ(received.front().recipient_char_id, 101);

  // Alice adding Bobby back creates no new recommended entry for Bobby.
  ASSERT_EQ(service_->add(actor(1, 101), 2).error, x5::ec::Success);
  EXPECT_EQ(alarms().size(), 1);

  // An offline target sees the recommendation in its list later.
  ASSERT_EQ(service_->add(actor(1, 101), 3).error, x5::ec::Success);
  EXPECT_EQ(alarms().size(), 1);
}

TEST_F(FriendServiceTest, RemoveDeletesFriendsAndDismissesRecommendations) {
  const auto alice = actor(1, 101);
  const auto bobby = actor(2, 201);
  ASSERT_EQ(service_->add(alice, 2).error, x5::ec::Success);
  ASSERT_EQ(service_->add(actor(3, 301), 2).error, x5::ec::Success);
  ASSERT_EQ(service_->recommended(bobby).size(), 2);

  EXPECT_TRUE(service_->remove(bobby, 1, true).changed);
  EXPECT_FALSE(service_->remove(bobby, 1, true).changed);
  const auto remaining = service_->recommended(bobby);
  ASSERT_EQ(remaining.size(), 1);
  EXPECT_EQ(remaining.front().account_id, 3);
  // Dismissing does not change Alice's own list.
  EXPECT_EQ(service_->friends(alice).size(), 1);

  EXPECT_TRUE(service_->remove(alice, 2, false).changed);
  EXPECT_FALSE(service_->remove(alice, 2, false).changed);
  EXPECT_TRUE(service_->friends(alice).empty());
}

TEST_F(FriendServiceTest, AliasChangesAreValidatedAndAnEmptyAliasRestoresTheName) {
  const auto alice = actor(1, 101);
  ASSERT_EQ(service_->add(alice, 2).error, x5::ec::Success);

  const auto renamed = service_->set_alias(alice, 2, "Best \xC3\xA9pic friend");
  ASSERT_EQ(renamed.error, x5::ec::Success);
  EXPECT_EQ(renamed.updated.alias, "Best \xC3\xA9pic friend");
  EXPECT_EQ(service_->friends(alice).front().alias, "Best \xC3\xA9pic friend");

  EXPECT_EQ(service_->set_alias(alice, 2, std::string(17, 'x')).error, x5::ec::BadRequest);
  EXPECT_EQ(service_->set_alias(alice, 2, "tab\there").error, x5::ec::BadRequest);
  EXPECT_EQ(service_->set_alias(alice, 2, "\xC3").error, x5::ec::BadRequest);
  EXPECT_EQ(service_->set_alias(alice, 3, "Carol").error, x5::ec::BadRequest);

  const auto restored = service_->set_alias(alice, 2, "");
  ASSERT_EQ(restored.error, x5::ec::Success);
  EXPECT_EQ(restored.updated.alias, "Bobby");
}

TEST_F(FriendServiceTest, FriendshipsAliasesAndLastLoginsSurviveAServiceRestart) {
  const auto alice = actor(1, 101);
  ASSERT_EQ(service_->add(alice, 2).error, x5::ec::Success);
  ASSERT_EQ(service_->set_alias(alice, 2, "Bob").error, x5::ec::Success);
  ASSERT_TRUE(service_->remove(actor(2, 201), 1, true).changed);
  service_->character_entered_world(kRealm, 201);
  now_ = kFirstTick + 77;
  service_->character_left_world(kRealm, 201);

  service_ = make_service();
  const auto listed = service_->friends(alice);
  ASSERT_EQ(listed.size(), 1);
  EXPECT_EQ(listed.front().alias, "Bob");
  EXPECT_EQ(listed.front().last_login_epoch_ticks, kFirstTick + 77);
  EXPECT_TRUE(service_->recommended(actor(2, 201)).empty());
}

// Wraps the memory store and fails every write while `failing` is set.
class FlakyFriendStore final : public FriendStore {
public:
  FriendSnapshot load(int world_id) override {
    return inner.load(world_id);
  }

  void insert_link(const FriendLink &link) override {
    fail_if_requested();
    inner.insert_link(link);
  }

  void update_link(const FriendLink &link) override {
    fail_if_requested();
    inner.update_link(link);
  }

  void remove_link(int world_id, std::int64_t owner_account_id,
                   std::int64_t friend_account_id) override {
    fail_if_requested();
    inner.remove_link(world_id, owner_account_id, friend_account_id);
  }

  void save_presence(const FriendPresenceRecord &presence) override {
    fail_if_requested();
    inner.save_presence(presence);
  }

  bool failing{};

private:
  void fail_if_requested() const {
    if (failing) {
      throw std::runtime_error("database unavailable");
    }
  }

  MemoryFriendStore inner;
};

TEST_F(FriendServiceTest, StoreFailuresReportSystemErrorAndKeepTheLists) {
  auto flaky = std::make_shared<FlakyFriendStore>();
  FriendService service(flaky, directory_, FriendRules{}, kRealm, [this] { return now_; });
  const auto alice = actor(1, 101);
  const auto bobby = actor(2, 201);
  ASSERT_EQ(service.add(alice, 2).error, x5::ec::Success);

  flaky->failing = true;
  EXPECT_EQ(service.add(alice, 3).error, x5::ec::SystemError);
  EXPECT_EQ(service.set_alias(alice, 2, "Bob").error, x5::ec::SystemError);
  EXPECT_EQ(service.remove(alice, 2, false).error, x5::ec::SystemError);
  EXPECT_EQ(service.remove(bobby, 1, true).error, x5::ec::SystemError);

  // Presence still follows the world connection; only the persisted record is lost.
  service.character_entered_world(kRealm, 201);
  EXPECT_TRUE(service.online(2));

  const auto listed = service.friends(alice);
  ASSERT_EQ(listed.size(), 1);
  EXPECT_EQ(listed.front().account_id, 2);
  EXPECT_EQ(listed.front().alias, "Bobby");
  EXPECT_EQ(service.recommended(bobby).size(), 1);
}

TEST(FriendAlias, AcceptsBoundedPrintableUtf8) {
  EXPECT_TRUE(valid_friend_alias("", 16));
  EXPECT_TRUE(valid_friend_alias("\xEC\xB9\x9C\xEA\xB5\xAC", 2));
  EXPECT_FALSE(valid_friend_alias("\xEC\xB9\x9C\xEA\xB5\xAC", 1));
  EXPECT_FALSE(valid_friend_alias("bell\x07", 16));
  EXPECT_FALSE(valid_friend_alias("c1\xC2\x85", 16));
}

// Uses the same isolated cluster variable as the other PostgreSQL service tests.
TEST(FriendStorePostgres, FriendshipsPresenceAndSummariesSurviveReconstruction) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL test cluster";
  }

  auto characters = make_postgres_character_repositories(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto name_suffix = suffix.substr(suffix.size() - 8);

  const auto create_player = [&](const std::string &prefix) {
    Account account;
    account.name = prefix + "_" + suffix;
    if (!characters.accounts->add_account(account)) {
      throw std::runtime_error("Cannot create test account");
    }

    Character character;
    character.account_id = characters.accounts->account(account.name)->id;
    character.world_id = kRealm;
    character.name = prefix + name_suffix;
    return characters.characters->create(character);
  };
  const auto owner = create_player("Fa");
  const auto listed = create_player("Fb");

  const auto by_name = characters.characters->character_summary_by_name(kRealm, listed.name);
  ASSERT_TRUE(by_name.has_value());
  EXPECT_EQ(by_name->id, listed.id);
  EXPECT_EQ(by_name->account_id, listed.account_id);
  EXPECT_FALSE(
      characters.characters->character_summary_by_name(kRealm, "fb" + name_suffix).has_value());
  EXPECT_EQ(characters.characters->character_summary(owner.id)->name, owner.name);
  ASSERT_EQ(characters.characters->account_character_summaries(listed.account_id, kRealm).size(),
            1);

  auto store = std::make_shared<PostgresFriendStore>(connection);
  const auto directory =
      std::make_shared<CharacterFriendDirectory>(characters.characters, nullptr);
  std::int64_t now = kFirstTick;
  const auto clock = [&now] { return now; };
  const FriendActor owner_actor{kRealm, owner.account_id, owner.id};
  const FriendActor listed_actor{kRealm, listed.account_id, listed.id};
  {
    FriendService service(store, directory, FriendRules{}, kRealm, clock);
    ASSERT_EQ(service.add(owner_actor, listed.account_id).error, x5::ec::Success);
    EXPECT_EQ(service.add(owner_actor, listed.account_id).error, x5::ec::DuplicateRequest);
    ASSERT_EQ(service.set_alias(owner_actor, listed.account_id, "Pal").error, x5::ec::Success);
    ASSERT_TRUE(service.remove(listed_actor, owner.account_id, true).changed);
    service.character_entered_world(kRealm, listed.id);
    now = kFirstTick + 500;
    service.character_left_world(kRealm, listed.id);
  }

  FriendService reloaded(store, directory, FriendRules{}, kRealm, clock);
  const auto friends = reloaded.friends(owner_actor);
  ASSERT_EQ(friends.size(), 1);
  EXPECT_EQ(friends.front().alias, "Pal");
  EXPECT_EQ(friends.front().created_epoch_ticks, kFirstTick);
  EXPECT_EQ(friends.front().last_login_epoch_ticks, kFirstTick + 500);
  EXPECT_TRUE(reloaded.recommended(listed_actor).empty());

  ASSERT_TRUE(reloaded.remove(owner_actor, listed.account_id, false).changed);
  FriendService emptied(store, directory, FriendRules{}, kRealm, clock);
  EXPECT_TRUE(emptied.friends(owner_actor).empty());
}

// Wire-level checks through the front WebSocket dispatcher, using the memory-backed
// directory the server installs.
template <typename Message>
Message decode_frame(const std::string &wire, std::uint16_t expected_packet_id) {
  const auto frames = native_ws::decode_frames(wire);
  if (frames.size() != 1 || frames.front().packet_id != expected_packet_id) {
    throw std::runtime_error("Unexpected friend reply packet");
  }

  Message message;
  const auto payload = frames.front().payload;
  if (!message.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    throw std::runtime_error("Malformed friend reply payload");
  }
  return message;
}

struct FrontPlayer {
  std::string token;
  std::int64_t account_id{};
  std::int64_t character_id{};
  std::uint64_t connection_id{};
};

class NativeFriends : public testing::Test {
protected:
  FrontPlayer player(const std::string &name) {
    if (!auth_.create_account(name, "friend-test-password")) {
      throw std::runtime_error("Cannot provision test account");
    }

    FrontPlayer result;
    result.token = auth_.login(name, "friend-test-password").access_token();
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(kRealm);
    if (auth_.enter(result.token, enter).error_code() != x5::ec::Success) {
      throw std::runtime_error("Cannot enter test realm");
    }

    x5::pb::ws::front::CharCreateReq create;
    create.set_slot(0);
    create.set_char_name(name);
    create.set_char_class(x5::cms::EX5CharacterClass_Wild);
    const auto created = auth_.create(result.token, create);
    if (created.ec() != x5::ec::Success) {
      throw std::runtime_error("Cannot create test character");
    }
    result.character_id = created.char_().char_id();

    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(result.character_id);
    if (auth_.select(result.token, select).ec() != x5::ec::Success) {
      throw std::runtime_error("Cannot select test character");
    }

    result.account_id = auth_.selected_character(result.token)->account_id;
    result.connection_id = dispatcher_.chat().connect(result.token);
    return result;
  }

  std::string send(const FrontPlayer &sender, std::uint16_t packet_id,
                   const google::protobuf::Message &request) {
    return dispatcher_.handle(NativeWsChannel::front, sender.token,
                              native_ws::encode_frame(packet_id, request));
  }

  MemoryRepositories repositories_;
  AuthService auth_{repositories_.character_repositories(), Config{}};
  std::shared_ptr<FriendService> friends_ = std::make_shared<FriendService>(
      std::make_shared<MemoryFriendStore>(),
      std::make_shared<CharacterFriendDirectory>(repositories_.characters, nullptr),
      FriendRules{}, kRealm);
  NativeWsDispatcher dispatcher_{auth_, nullptr, nullptr, friends_};
};

TEST_F(NativeFriends, SearchAddListModifyAndRemoveUseTheRecoveredReplies) {
  const auto alice = player("Alice");
  const auto bobby = player("Bobby");
  friends_->character_entered_world(kRealm, bobby.character_id);

  x5::pb::ws::front::FriendSearchReq search;
  search.set_req_seq(7);
  search.set_char_name("Bobby");
  const auto found = decode_frame<x5::pb::frontwebsocket::FriendSearchRes>(
      send(alice, native_friend::kSearchRequest, search), native_friend::kSearchResponse);
  ASSERT_EQ(found.ec(), x5::ec::Success);
  EXPECT_EQ(found.req_seq(), 7u);
  EXPECT_EQ(found.friend_().account_id(), bobby.account_id);
  EXPECT_EQ(found.friend_().char_().char_name(), "Bobby");
  EXPECT_FALSE(found.has_last_login_epochtime());

  x5::pb::ws::front::FriendAddReq add;
  add.set_account_id(bobby.account_id);
  const auto added = decode_frame<x5::pb::frontwebsocket::FriendAddRes>(
      send(alice, native_friend::kAddRequest, add), native_friend::kAddResponse);
  ASSERT_EQ(added.ec(), x5::ec::Success);
  EXPECT_EQ(added.friend_().status_case(), x5::pb::FriendInfo::kChar);
  EXPECT_EQ(added.friend_().char_().char_id(), bobby.character_id);

  // Bobby is in the world, so his front socket receives SM_FRIEND_ADD_ALARM.
  const auto pushed = dispatcher_.chat().drain(bobby.connection_id);
  ASSERT_EQ(pushed.size(), 1);
  const auto alarm = decode_frame<x5::pb::SystemMessageNoti>(
      pushed.front(), native_friend::kSystemMessageNotification);
  EXPECT_EQ(alarm.key(), x5::cms::ESystemMessage_SM_FRIEND_ADD_ALARM);

  const auto duplicate = decode_frame<x5::pb::frontwebsocket::FriendAddRes>(
      send(alice, native_friend::kAddRequest, add), native_friend::kAddResponse);
  EXPECT_EQ(duplicate.ec(), x5::ec::DuplicateRequest);

  x5::pb::ws::front::FriendListReq recommended_list;
  recommended_list.set_recommend(true);
  const auto recommended = decode_frame<x5::pb::frontwebsocket::FriendListRes>(
      send(bobby, native_friend::kListRequest, recommended_list), native_friend::kListResponse);
  EXPECT_TRUE(recommended.recommend());
  ASSERT_EQ(recommended.friends_size(), 1);
  EXPECT_EQ(recommended.friends(0).account_id(), alice.account_id);
  EXPECT_EQ(recommended.friends(0).status_case(), x5::pb::FriendInfo::kLastLoginEpochtime);

  x5::pb::ws::front::FriendModifyReq modify;
  modify.set_account_id(bobby.account_id);
  modify.set_alias("Bob");
  const auto modified = decode_frame<x5::pb::frontwebsocket::FriendUpdateNoti>(
      send(alice, native_friend::kModifyRequest, modify), native_friend::kUpdateNotification);
  EXPECT_EQ(modified.friend_().alias(), "Bob");

  x5::pb::ws::front::FriendListReq friend_list;
  friend_list.set_recommend(false);
  const auto listed = decode_frame<x5::pb::frontwebsocket::FriendListRes>(
      send(alice, native_friend::kListRequest, friend_list), native_friend::kListResponse);
  EXPECT_TRUE(listed.has_recommend());
  EXPECT_FALSE(listed.recommend());
  ASSERT_EQ(listed.friends_size(), 1);
  EXPECT_EQ(listed.friends(0).alias(), "Bob");

  x5::pb::ws::front::FriendRemoveReq remove;
  remove.set_account_id(bobby.account_id);
  const auto removed = decode_frame<x5::pb::frontwebsocket::FriendRemoveNoti>(
      send(alice, native_friend::kRemoveRequest, remove), native_friend::kRemoveNotification);
  EXPECT_EQ(removed.account_id(), bobby.account_id);
  EXPECT_FALSE(removed.recommend());
  EXPECT_TRUE(friends_->friends({kRealm, alice.account_id, alice.character_id}).empty());
}

TEST_F(NativeFriends, PresenceChangesArePushedAsFriendUpdates) {
  const auto alice = player("Alice");
  const auto bobby = player("Bobby");
  friends_->character_entered_world(kRealm, alice.character_id);

  x5::pb::ws::front::FriendAddReq add;
  add.set_account_id(bobby.account_id);
  ASSERT_EQ(decode_frame<x5::pb::frontwebsocket::FriendAddRes>(
                send(alice, native_friend::kAddRequest, add), native_friend::kAddResponse)
                .ec(),
            x5::ec::Success);

  friends_->character_entered_world(kRealm, bobby.character_id);
  friends_->character_left_world(kRealm, bobby.character_id);

  const auto pushed = dispatcher_.chat().drain(alice.connection_id);
  ASSERT_EQ(pushed.size(), 2);
  const auto online = decode_frame<x5::pb::frontwebsocket::FriendUpdateNoti>(
      pushed.front(), native_friend::kUpdateNotification);
  EXPECT_EQ(online.friend_().status_case(), x5::pb::FriendInfo::kChar);
  EXPECT_EQ(online.friend_().char_().char_name(), "Bobby");

  const auto offline = decode_frame<x5::pb::frontwebsocket::FriendUpdateNoti>(
      pushed.back(), native_friend::kUpdateNotification);
  EXPECT_EQ(offline.friend_().status_case(), x5::pb::FriendInfo::kLastLoginEpochtime);
  EXPECT_GT(offline.friend_().last_login_epochtime(), 0);
}

TEST_F(NativeFriends, SearchMissesAndUnknownKeysKeepTheSocketOpen) {
  const auto alice = player("Alice");

  x5::pb::ws::front::FriendSearchReq missing;
  missing.set_req_seq(3);
  missing.set_char_name("Nobody");
  const auto not_found = decode_frame<x5::pb::frontwebsocket::FriendSearchRes>(
      send(alice, native_friend::kSearchRequest, missing), native_friend::kSearchResponse);
  EXPECT_EQ(not_found.ec(), x5::ec::InvalidChar);
  EXPECT_EQ(not_found.req_seq(), 3u);
  EXPECT_FALSE(not_found.has_friend_());

  x5::pb::ws::front::FriendSearchReq keyless;
  keyless.set_req_seq(4);
  const auto refused = decode_frame<x5::pb::frontwebsocket::FriendSearchRes>(
      send(alice, native_friend::kSearchRequest, keyless), native_friend::kSearchResponse);
  EXPECT_EQ(refused.ec(), x5::ec::BadRequest);

  x5::pb::ws::front::FriendModifyReq stranger;
  stranger.set_account_id(alice.account_id + 100);
  stranger.set_alias("Who");
  const auto error = decode_frame<x5::pb::CommonErrorRes>(
      send(alice, native_friend::kModifyRequest, stranger), native_friend::kCommonErrorResponse);
  EXPECT_EQ(error.ec(), x5::ec::BadRequest);
}

} // namespace
} // namespace chronicles
