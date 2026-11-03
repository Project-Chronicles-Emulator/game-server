#include "world/features/combat/tests/native_combat_test_support.hpp"
#include "world/features/shop/tests/native_shop_test_support.hpp"
#include "world/session/native_world.hpp"
#include "world/session/tests/native_map_entry_test_support.hpp"
#include "world/session/tests/native_world_test_support.hpp"
#include "world/session/tests/offline_quarantine_test_server.hpp"

#include "common/clock_ticks.hpp"
#include "data/client_catalog.hpp"
#include "persistence/character_repositories.hpp"
#include "persistence/memory_repositories.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/events/channel_event.hpp"
#include "world/features/events/ranking_repository.hpp"
#include "world/features/fog/fog_repository.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "world/features/market/market_repository.hpp"

#include "ec.generated.pb.h"
#include "testing/company_test_support.hpp"
#include "world/features/company/company.hpp"
#include "world/features/gathering/gathering.hpp"

#include "p_character.pb.h"
#include "world/core/unit_registry.hpp"
#include "world/core/world_sectors.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/inventory/inventory_rules.hpp"
#include "world/features/loot/loot.hpp"
#include "world/features/mail/tutorial_completion_mail.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/features/npc/npc_sight.hpp"
#include "world/features/processing/processing.hpp"
#include "world/features/quests/world_quests.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/system_error.hpp>
#include <gtest/gtest.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

using namespace native_testing;
using namespace combat_native_testing;


// Every entry wraps the inventory snapshot (WorldCharItemsNoti) in the crafting snapshots:
// WorldCharExpertiseSkillsNoti before it and WorldCharItemCraftsNoti after it, as official
// entries do. Returns the inventory frame.


TEST(NativeWorld, EntrySuccessMatchesRecordedEmptyProtobuf) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol);

  const auto entry = handshake.entry_response();
  const Bytes expected{4, 0, 5, 0};
  EXPECT_EQ(entry.bytes, expected);
}

TEST(NativeWorld, ClockRepliesPreserveClientMeasurements) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol);
  const auto request = protocol.encode(
      20001, {{"try_count", 2}, {"try_cycles64", "75764660325"}, {"try_ticks", "30000"}}, 0);

  const auto response = handshake.receive(request, 120000, 17927712622387987);
  ASSERT_TRUE(response);
  const auto decoded = protocol.decode(*response);
  EXPECT_EQ(decoded.at("opcode"), 1);
  EXPECT_EQ(decoded.at("message").at("try_count"), 2);
  EXPECT_EQ(decoded.at("message").at("try_cycles64"), "75764660325");
  EXPECT_EQ(decoded.at("message").at("try_ticks"), "30000");
  EXPECT_EQ(decoded.at("message").at("ticks"), "12000000");
  EXPECT_EQ(decoded.at("message").at("timestamp"), "17927712622387987");
  EXPECT_FALSE(handshake.take_pending_notification());
}

// Pauses a real world worker inside entry preparation, after it consumed the handoff.
class PausingWorldEntryStore final : public MemoryCharacterRepository {
public:
  explicit PausingWorldEntryStore(std::shared_ptr<MemoryDatabase> database)
      : MemoryCharacterRepository(std::move(database)) {}

  ~PausingWorldEntryStore() override {
    release();
  }

  void arm() {
    armed_.store(true);
  }

  bool wait_until_entered() {
    return entered_future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }

  void release() {
    if (!released_.exchange(true)) {
      release_.set_value();
    }
  }

  std::vector<Character> characters(std::int64_t account_id, int world_id) override {
    if (armed_.exchange(false)) {
      entered_.set_value();
      release_future_.wait();
    }
    return MemoryCharacterRepository::characters(account_id, world_id);
  }

private:
  std::atomic_bool armed_{false};
  std::atomic_bool released_{false};
  std::promise<void> entered_;
  std::future<void> entered_future_{entered_.get_future()};
  std::promise<void> release_;
  std::shared_future<void> release_future_{release_.get_future().share()};
};

// Releases a paused store before the world server joins its workers, even after a failure.
class EntryStoreRelease {
public:
  explicit EntryStoreRelease(PausingWorldEntryStore &store) : store_(store) {
  }

  ~EntryStoreRelease() {
    store_.release();
  }

  EntryStoreRelease(const EntryStoreRelease &) = delete;
  EntryStoreRelease &operator=(const EntryStoreRelease &) = delete;

private:
  PausingWorldEntryStore &store_;
};

// Holds the world server's only stream-executor thread until released or destroyed.
class StreamExecutorPause {
public:
  explicit StreamExecutorPause(TwoClientWorldServer &server) {
    auto paused = std::make_shared<std::promise<void>>();
    paused_ = paused->get_future();
    server.post_to_io([paused, resume = resume_future_] {
      paused->set_value();
      resume.wait();
    });
  }

  ~StreamExecutorPause() {
    release();
  }

  StreamExecutorPause(const StreamExecutorPause &) = delete;
  StreamExecutorPause &operator=(const StreamExecutorPause &) = delete;

  bool wait_until_paused() {
    return paused_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }

  void release() {
    if (!released_) {
      released_ = true;
      resume_.set_value();
    }
  }

private:
  std::promise<void> resume_;
  std::shared_future<void> resume_future_{resume_.get_future().share()};
  std::future<void> paused_;
  bool released_{};
};

struct LobbyWithCharacters {
  std::string access_token;
  std::int64_t account_id{};
  std::vector<std::int64_t> character_ids;
};

LobbyWithCharacters enter_lobby_with_characters(AuthService &auth, const std::string &account,
                                                const std::vector<std::string> &names) {
  const std::string password = account + "-password-123";
  EXPECT_TRUE(auth.create_account(account, password));
  const auto login = auth.login(account, password);
  LobbyWithCharacters lobby{login.access_token(), login.account_id(), {}};

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  EXPECT_EQ(auth.enter(lobby.access_token, world_request).error_code(), x5::ec::Success);

  for (std::size_t slot = 0; slot < names.size(); ++slot) {
    x5::pb::ws::front::CharCreateReq create_request;
    create_request.set_slot(static_cast<int>(slot));
    create_request.set_char_name(names[slot]);
    create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
    const auto created = auth.create(lobby.access_token, create_request);
    EXPECT_EQ(created.ec(), x5::ec::Success);
    lobby.character_ids.push_back(created.char_().char_id());
  }
  return lobby;
}

x5::pb::frontwebsocket::CharSelectRes select_character(AuthService &auth,
                                                       const LobbyWithCharacters &lobby,
                                                       std::size_t index) {
  x5::pb::ws::front::CharSelectReq request;
  request.set_char_id(lobby.character_ids.at(index));
  return auth.select(lobby.access_token, request);
}

// A rejected entry must end with the server closing the socket. A read timeout would mean
// that the connection was left open, so it is not accepted as a rejection.
void expect_server_closed_without_entry(WorldTestClient &client) {
  try {
    const auto frame = client.read();
    ADD_FAILURE() << "Received world frame opcode " << frame.opcode() << " after rejection";
  } catch (const boost::system::system_error &error) {
    const auto code = error.code();
    const bool remote_closed = code == asio::error::eof ||
                               code == asio::error::connection_reset ||
                               code == ssl::error::stream_truncated;
    EXPECT_TRUE(remote_closed) << "Expected EOF, reset, or TLS truncation; got " << error.what();
  }
}

bool wait_for_authenticated_connection(NativeWorldListener &listener, std::int64_t account_id) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto &connection : listener.connections()) {
      if (connection.authenticated && connection.account_id == account_id) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

Config world_entry_config() {
  Config config;
  // select() returns the native packet key only when a game address is configured.
  config.game_address = "127.0.0.1:8081";
  return config;
}

TEST(NativeWorldAdmission, MissingGatheringProfileStartsWhenNoSharedSourcesExist) {
  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories(), world_entry_config());
  ASSERT_GT(auth.acquire_monster_reward_owner_epoch(), 0);
  ASSERT_TRUE(auth.shared_gathering_sources(1, std::nullopt, 1).empty());

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  EXPECT_GT(server.port(), 0);
}

TEST(NativeWorldAdmission, MissingGatheringProfileRefusesPersistedSharedSources) {
  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories(), world_entry_config());
  ASSERT_GT(auth.acquire_monster_reward_owner_epoch(), 0);
  SharedGatheringSourceTransition transition;
  transition.map_id = 9012;
  transition.kind = 1;
  transition.sector_id = 42;
  transition.local_index = 1;
  transition.expected_active = true;
  transition.expected_phase = 1;
  transition.resulting_active = true;
  transition.resulting_phase = 2;
  ASSERT_TRUE(auth.update_shared_gathering_source(1, transition, unix_epoch_ticks_now(), false)
                  .committed);
  ASSERT_EQ(auth.shared_gathering_sources(1, std::nullopt, 1).size(), 1U);

  TemporaryWorldCertificate certificate;
  try {
    TwoClientWorldServer server(auth, certificate);
    FAIL() << "Listener accepted connections without the required gathering profile";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("persisted shared sources require recovery"),
              std::string::npos);
  }
}

TEST(NativeWorldAdmission, BossRewardStaysBlockedUntilSourceHydrationAndAcknowledgement) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Requires the generated gathering profile";
  }
  const auto profile_path = std::filesystem::path(catalog_environment).parent_path() /
                            "gathering" / "definitions.json";
  if (!std::filesystem::exists(profile_path)) {
    GTEST_SKIP() << "Requires the generated gathering profile";
  }
  const auto gathering = GatheringData::load(profile_path);
  ASSERT_TRUE(gathering->has_map(30104));

  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories(), world_entry_config());
  ASSERT_GT(auth.acquire_monster_reward_owner_epoch(), 0);
  const auto lobby = enter_lobby_with_characters(auth, "boss_hydration", {"BossHydration"});
  ASSERT_EQ(lobby.character_ids.size(), 1U);
  const auto character_id = lobby.character_ids.front();
  MonsterRewardReceipt receipt;
  receipt.account_id = lobby.account_id;
  receipt.character_id = character_id;
  receipt.world_id = 1;
  receipt.receipt_id = std::string(32, 'a');
  receipt.credited_unit_id = 42;
  receipt.npc_cms_id = 5001510;
  receipt.position = WorldPosition{30104, 0, 0, 0, 0};
  const BossScriptIntent script{receipt.receipt_id, 1, 30104, 5001510, "shared"};
  ASSERT_TRUE(auth.enqueue_monster_rewards_with_script({receipt}, script));
  ASSERT_TRUE(auth.mark_monster_reward_ready(script.event_id));
  EXPECT_FALSE(auth.mark_boss_script_published(script));
  ASSERT_TRUE(auth.settle_shared_boss_script(script).committed);
  EXPECT_TRUE(repositories.character_state->pending_monster_rewards(
                  lobby.account_id, character_id, 1).empty());

  TemporaryWorldCertificate certificate;
  EXPECT_THROW((TwoClientWorldServer(auth, certificate)), std::runtime_error);
  EXPECT_EQ(auth.pending_boss_scripts(1, std::nullopt, kMonsterRewardRecoveryPageSize).size(), 1U);
  EXPECT_TRUE(repositories.character_state->pending_monster_rewards(
                  lobby.account_id, character_id, 1).empty());

  TwoClientWorldServer server(auth, certificate, {}, {}, {}, {}, {}, {}, {}, {}, gathering);
  EXPECT_GT(server.port(), 0);
  EXPECT_TRUE(auth.pending_boss_scripts(1, std::nullopt, kMonsterRewardRecoveryPageSize).empty());
  EXPECT_EQ(repositories.character_state->pending_monster_rewards(
                lobby.account_id, character_id, 1).size(), 1U);
}

TEST(NativeWorldAdmission, RevocationDuringEntryPreparationRejectsConsumedHandoff) {
  MemoryRepositories repositories;
  auto store = std::make_shared<PausingWorldEntryStore>(repositories.database);
  repositories.characters = store;
  AuthService auth(repositories.character_repositories(), world_entry_config());
  auth.acquire_monster_reward_owner_epoch();
  const auto lobby = enter_lobby_with_characters(auth, "barrier_player", {"Barrierplayer"});
  ASSERT_EQ(lobby.character_ids.size(), 1U);
  const auto selected = select_character(auth, lobby, 0);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  ASSERT_FALSE(selected.packet_key().empty());

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  EntryStoreRelease store_release(*store);
  WorldTestClient client(server.port());
  store->arm();
  client.send_key(selected.packet_key());
  ASSERT_TRUE(store->wait_until_entered()) << "World entry did not reach character loading";

  StreamExecutorPause pause(server);
  ASSERT_TRUE(pause.wait_until_paused());
  auth.revoke_account_sessions(lobby.account_id);
  // The worker has not published the identity yet, so there is nothing to disconnect.
  EXPECT_EQ(server.listener().disconnect_account(lobby.account_id), 0U);

  store->release();
  pause.release();
  expect_server_closed_without_entry(client);
}

TEST(NativeWorldAdmission, RevocationAfterIdentityPublicationClosesQueuedEntry) {
  MemoryRepositories repositories;
  auto store = std::make_shared<PausingWorldEntryStore>(repositories.database);
  repositories.characters = store;
  AuthService auth(repositories.character_repositories(), world_entry_config());
  auth.acquire_monster_reward_owner_epoch();
  const auto lobby = enter_lobby_with_characters(auth, "queued_player", {"Queuedplayer"});
  ASSERT_EQ(lobby.character_ids.size(), 1U);
  const auto selected = select_character(auth, lobby, 0);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  EntryStoreRelease store_release(*store);
  WorldTestClient client(server.port());
  store->arm();
  client.send_key(selected.packet_key());
  ASSERT_TRUE(store->wait_until_entered()) << "World entry did not reach character loading";

  // Let preparation finish while the admission completion stays queued on the executor.
  StreamExecutorPause pause(server);
  ASSERT_TRUE(pause.wait_until_paused());
  store->release();
  ASSERT_TRUE(wait_for_authenticated_connection(server.listener(), lobby.account_id));

  auth.revoke_account_sessions(lobby.account_id);
  EXPECT_EQ(server.listener().disconnect_account(lobby.account_id), 1U);
  pause.release();
  expect_server_closed_without_entry(client);
}

TEST(NativeWorldAdmission, ReselectionDuringEntryPreparationRejectsOlderHandoff) {
  MemoryRepositories repositories;
  auto store = std::make_shared<PausingWorldEntryStore>(repositories.database);
  repositories.characters = store;
  AuthService auth(repositories.character_repositories(), world_entry_config());
  auth.acquire_monster_reward_owner_epoch();
  const auto lobby =
      enter_lobby_with_characters(auth, "reselect_player", {"Firstplayer", "Secondplayer"});
  ASSERT_EQ(lobby.character_ids.size(), 2U);
  const auto first_selection = select_character(auth, lobby, 0);
  ASSERT_EQ(first_selection.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  EntryStoreRelease store_release(*store);
  WorldTestClient first_client(server.port());
  store->arm();
  first_client.send_key(first_selection.packet_key());
  ASSERT_TRUE(store->wait_until_entered()) << "World entry did not reach character loading";

  StreamExecutorPause pause(server);
  ASSERT_TRUE(pause.wait_until_paused());
  const auto second_selection = select_character(auth, lobby, 1);
  ASSERT_EQ(second_selection.ec(), x5::ec::Success);
  store->release();
  pause.release();
  expect_server_closed_without_entry(first_client);

  // The newer selection remains admissible.
  WorldTestClient second_client(server.port());
  second_client.send_key(second_selection.packet_key());
  EXPECT_EQ(second_client.read().opcode(), 5);
}

TEST(NativeWorldAdmission, BusyCharacterKeepsTheSameTicketUntilSettlement) {
  MemoryRepositories repositories;
  auto store = std::make_shared<PausingWorldEntryStore>(repositories.database);
  repositories.characters = store;
  AuthService auth(repositories.character_repositories(), world_entry_config());
  auth.acquire_monster_reward_owner_epoch();
  const auto lobby = enter_lobby_with_characters(auth, "retry_player", {"Retryplayer"});
  ASSERT_EQ(lobby.character_ids.size(), 1U);
  const auto first_selection = select_character(auth, lobby, 0);
  ASSERT_EQ(first_selection.ec(), x5::ec::Success);
  const auto other_lobby = enter_lobby_with_characters(auth, "other_retry_player", {"Otherretry"});
  const auto other_selection = select_character(auth, other_lobby, 0);
  ASSERT_EQ(other_selection.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  EntryStoreRelease store_release(*store);
  WorldTestClient first_client(server.port());
  store->arm();
  first_client.send_key(first_selection.packet_key());
  ASSERT_TRUE(store->wait_until_entered());

  const auto retry_selection = select_character(auth, lobby, 0);
  ASSERT_EQ(retry_selection.ec(), x5::ec::Success);
  {
    WorldTestClient busy_client(server.port());
    busy_client.send_key(retry_selection.packet_key());
    expect_server_closed_without_entry(busy_client);
  }
  {
    WorldTestClient other_client(server.port());
    other_client.send_key(other_selection.packet_key());
    EXPECT_EQ(other_client.read().opcode(), 5);
  }

  store->release();
  expect_server_closed_without_entry(first_client);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool entered{};
  while (!entered && std::chrono::steady_clock::now() < deadline) {
    WorldTestClient retry(server.port());
    retry.send_key(retry_selection.packet_key());
    try {
      entered = retry.read().opcode() == 5;
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  EXPECT_TRUE(entered) << "The same ticket was not admitted after old cleanup";
}

TEST(NativeWorldAdmission, OfflineReservationPreservesTicketAndExcludesOnlyItsCharacter) {
  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories(), world_entry_config());
  auth.acquire_monster_reward_owner_epoch();
  const auto lobby = enter_lobby_with_characters(auth, "reserved_player", {"Reservedplayer"});
  const auto selection = select_character(auth, lobby, 0);
  ASSERT_EQ(selection.ec(), x5::ec::Success);
  const auto other_lobby = enter_lobby_with_characters(auth, "unreserved_player", {"Otherplayer"});
  const auto other_selection = select_character(auth, other_lobby, 0);
  ASSERT_EQ(other_selection.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  auto reservation = server.listener().reserve_offline_character(lobby.character_ids[0]);
  ASSERT_NE(reservation, nullptr);
  EXPECT_EQ(server.listener().reserve_offline_character(lobby.character_ids[0]), nullptr);
  {
    WorldTestClient blocked(server.port());
    blocked.send_key(selection.packet_key());
    expect_server_closed_without_entry(blocked);
  }
  {
    WorldTestClient other(server.port());
    other.send_key(other_selection.packet_key());
    EXPECT_EQ(other.read().opcode(), 5);
  }
  reservation.reset();
  WorldTestClient accepted(server.port());
  accepted.send_key(selection.packet_key());
  EXPECT_EQ(accepted.read().opcode(), 5);
  EXPECT_EQ(server.listener().reserve_offline_character(lobby.character_ids[0]), nullptr);
}

TEST(NativeWorldAdmission, UnknownOfflineWriteOutcomeKeepsCharacterFenced) {
  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories(), world_entry_config());
  auth.acquire_monster_reward_owner_epoch();
  TemporaryWorldCertificate certificate;
  OfflineQuarantineTestServer server(auth, certificate);

  auto reservation = server.listener().reserve_offline_character(42);
  ASSERT_NE(reservation, nullptr);
  reservation->begin_write();
  reservation.reset();
  EXPECT_EQ(server.listener().reserve_offline_character(42), nullptr);
  EXPECT_NE(server.listener().reserve_offline_character(43), nullptr);
  EXPECT_TRUE(server.shutdown_remains_blocked());
}

TEST(NativeWorld, NonTutorialEntrySendsPersistedGoldAfterItems) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("money_entry", "test-world-password-123"));
  const auto login = auth.login("money_entry", "test-world-password-123");

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("Moneyentry");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  const auto character_id = created.char_().char_id();
  const auto before =
      repositories.character_state->load_world_progress(login.account_id(), character_id, 1);
  auto after = before;
  after.revision = before.revision + 1;
  after.gold = 321;
  ASSERT_TRUE(repositories.character_state->save_world_progress(login.account_id(), character_id, 1,
                                                                before, after));

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(character_id);
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);

  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  read_entry_inventory(client);
  const auto money_frame = client.read();
  ASSERT_EQ(money_frame.opcode(), 126);
  const auto money = protocol.decode(money_frame).at("message");
  EXPECT_FALSE(money.value("update", false));
  EXPECT_EQ(money.at("money").at("values").at("0"), "321");
  EXPECT_EQ(money.at("money").at("values").size(), 1);
}

TEST(NativeWorld, NonTutorialEntrySendsPersistedHersanderTokenOnlyWhenNonzero) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("hersander_entry", "test-world-password-123"));
  const auto login = auth.login("hersander_entry", "test-world-password-123");

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("Hersanderentry");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  const auto character_id = created.char_().char_id();
  const auto before =
      repositories.character_state->load_world_progress(login.account_id(), character_id, 1);
  auto after = before;
  after.revision = before.revision + 1;
  after.gold = 321;
  after.hersander_token = 20;
  ASSERT_TRUE(repositories.character_state->save_world_progress(login.account_id(), character_id, 1,
                                                                before, after));

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(character_id);
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);

  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  read_entry_inventory(client);
  const auto money_frame = client.read();
  ASSERT_EQ(money_frame.opcode(), 126);
  const auto money = protocol.decode(money_frame).at("message");
  EXPECT_FALSE(money.value("update", false));
  EXPECT_EQ(money.at("money").at("values").at("0"), "321");
  EXPECT_EQ(money.at("money").at("values").at("1"), "20");
}





TEST(NativeWorld, OptInTutorialMoveFollowsClientMapAndPlayingRequests) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol, true);

  Character character;
  character.id = 1;
  character.class_id = 2;
  character.level = 1;
  character.name = "Local";
  x5::pb::DbCharacterCustomization customization;
  customization.mutable_face()->set_hair_cms_id(42);
  character.customization_hex = hex_encode(customization.SerializeAsString());
  MemoryRepositories repositories;
  auto saved_character = repositories.characters->create(character);
  ASSERT_TRUE(handshake.configure_bootstrap_character(saved_character));

  const auto time_request = protocol.encode(20001, nlohmann::json::object(), 0);
  const auto map_start = handshake.receive(time_request, 120000, 190000);
  ASSERT_TRUE(map_start);
  EXPECT_EQ(map_start->opcode(), 175);
  const auto map_start_message = protocol.decode(*map_start).at("message");
  EXPECT_EQ(map_start_message.at("map_id"), 8000);
  const auto spawn_point = map_start_message.at("spawn_point");
  EXPECT_EQ(spawn_point.at("pos").at("millimeter_x"), -795146);
  EXPECT_EQ(spawn_point.at("pos").at("millimeter_y"), 712194);
  EXPECT_EQ(spawn_point.at("pos").at("millimeter_z"), -698778);
  EXPECT_NEAR(spawn_point.at("yaw").get<float>(), 158.56723F, 0.001F);

  const auto time_response = handshake.take_pending_notification();
  ASSERT_TRUE(time_response);
  EXPECT_EQ(time_response->opcode(), 1);

  // Official entries list every expertise type at Lv1; the craft window needs a non-zero level.
  const auto expertise = handshake.take_pending_notification();
  ASSERT_TRUE(expertise);
  ASSERT_EQ(expertise->opcode(), kWorldCharExpertiseSkillsNotification);
  const auto skills = protocol.decode(*expertise).at("message").at("skills");
  ASSERT_EQ(skills.size(), 16);
  for (const auto &skill : skills) {
    EXPECT_EQ(skill.at("level"), "EX5ExpertiseLvType_Lv1");
  }
  EXPECT_FALSE(skills.at(0).contains("type")) << "Farming is the proto3 default type";
  EXPECT_EQ(skills.at(13).at("type"), "EX5ExpertiseType_Jewealry");
  EXPECT_EQ(skills.at(15).at("type"), "EX5ExpertiseType_TradeItem");

  const auto starter_items = handshake.take_pending_notification();
  ASSERT_TRUE(starter_items);
  EXPECT_EQ(starter_items->opcode(), 130);
  const auto inventory = protocol.decode(*starter_items).at("message");
  ASSERT_EQ(inventory.at("items").size(), 5);

  // No recipe is learned yet: an empty, non-update snapshot.
  const auto learned_crafts = handshake.take_pending_notification();
  ASSERT_TRUE(learned_crafts);
  EXPECT_EQ(learned_crafts->opcode(), kWorldCharItemCraftsNotification);
  EXPECT_EQ(protocol.decode(*learned_crafts).at("message"), nlohmann::json::object());
  EXPECT_FALSE(handshake.take_pending_notification());

  const auto map_loaded = protocol.encode(20085, nlohmann::json::object(), 1);
  const auto map_complete = handshake.receive(map_loaded, 120000, 190000);
  ASSERT_TRUE(map_complete);
  EXPECT_EQ(map_complete->opcode(), 176);
  const auto map_message = protocol.decode(*map_complete).at("message");
  EXPECT_EQ(map_message.at("map_id"), 8000);
  EXPECT_EQ(map_message.at("player_unit_id"), player_wire_unit_id(1));
  EXPECT_EQ(map_message.at("nub").at("player").at("char_id"), "1");
  EXPECT_EQ(map_message.at("nub").at("player").at("state"), "EX5UnitStateType_Idle");
  EXPECT_EQ(map_message.at("nub").at("player").at("stats").at("84"), 1000);
  EXPECT_EQ(map_message.at("nub").at("player").at("customization").at("face").at("hair_cms_id"),
            42);
  const auto equipment = map_message.at("nub").at("player").at("equipments");
  ASSERT_EQ(equipment.size(), 5);
  EXPECT_EQ(equipment.at(0).at("item_cms_id"), 11102302);
  EXPECT_EQ(equipment.at(1).at("item_cms_id"), 10130202);
  EXPECT_EQ(map_message.at("nub").at("pos").at("millimeter_x"), -795146);

  const auto first_gauge = handshake.take_pending_notification();
  const auto second_gauge = handshake.take_pending_notification();
  ASSERT_TRUE(first_gauge);
  ASSERT_TRUE(second_gauge);
  EXPECT_EQ(first_gauge->opcode(), 78);
  EXPECT_EQ(second_gauge->opcode(), 78);
  EXPECT_EQ(protocol.decode(*first_gauge).at("message"), nlohmann::json::object());
  EXPECT_EQ(protocol.decode(*second_gauge).at("message"), nlohmann::json::object());

  const auto initial_attributes = handshake.take_pending_notification();
  ASSERT_TRUE(initial_attributes);
  EXPECT_EQ(initial_attributes->opcode(), 33);
  EXPECT_EQ(protocol.decode(*initial_attributes).at("message").at("attributes").at("2"), 1);
  const auto traits_packet = handshake.take_pending_notification();
  ASSERT_TRUE(traits_packet);
  EXPECT_EQ(traits_packet->opcode(), 115);
  const auto traits = protocol.decode(*traits_packet).at("message");
  EXPECT_EQ(traits.at("unit_id"), std::to_string(player_wire_unit_id(1)));
  EXPECT_EQ(traits.at("unitTraits").at("1").at("type"),
            "EX5UnitTraitValueType_PerMille");
  const auto changed_attributes = handshake.take_pending_notification();
  ASSERT_TRUE(changed_attributes);
  EXPECT_EQ(changed_attributes->opcode(), 33);
  EXPECT_EQ(protocol.decode(*changed_attributes).at("message").at("attributes").at("2"), 2);
  EXPECT_FALSE(handshake.take_pending_notification());

  const auto playing_request = protocol.encode(20086, nlohmann::json::object(), 2);
  const auto playing_response = handshake.receive(playing_request, 120000, 190000);
  ASSERT_TRUE(playing_response);
  EXPECT_EQ(playing_response->opcode(), 183);
  EXPECT_EQ(protocol.decode(*playing_response).at("message").at("unit_id"),
            std::to_string(player_wire_unit_id(1)));
  const auto playing_attributes = handshake.take_pending_notification();
  ASSERT_TRUE(playing_attributes);
  EXPECT_EQ(playing_attributes->opcode(), 33);
  const auto attributes = protocol.decode(*playing_attributes).at("message");
  EXPECT_EQ(attributes.at("unit_id"), std::to_string(player_wire_unit_id(1)));
  EXPECT_EQ(attributes.at("attributes").at("2"), 1);
  EXPECT_FALSE(handshake.take_pending_notification());

  const auto move_request = protocol.encode(
      20087,
      {{"pos", {{"millimeter_x", -795100}, {"millimeter_y", 712230}, {"millimeter_z", -698700}}},
       {"body_yaw", 150.0F}},
      3);
  EXPECT_FALSE(handshake.receive(move_request, 120000, 190000));
  const auto last_position = handshake.last_position();
  ASSERT_TRUE(last_position);
  EXPECT_EQ(last_position->map_id, 8000);
  EXPECT_EQ(last_position->x_mm, -795100);
  EXPECT_FLOAT_EQ(last_position->yaw, 150.0F);

  ASSERT_TRUE(repositories.characters->save_world_position(
      saved_character.account_id, saved_character.id, saved_character.world_id, *last_position));
  const auto characters =
      repositories.characters->characters(saved_character.account_id, saved_character.world_id);
  ASSERT_EQ(characters.size(), 1);
  WorldHandshake resumed(protocol, true);
  ASSERT_TRUE(resumed.configure_bootstrap_character(characters.front()));
  const auto resumed_start = resumed.receive(time_request, 120000, 190000);
  ASSERT_TRUE(resumed_start);
  EXPECT_EQ(
      protocol.decode(*resumed_start).at("message").at("spawn_point").at("pos").at("millimeter_x"),
      -795100);

  const auto repeated_time_request = protocol.encode(20001, nlohmann::json::object(), 4);
  ASSERT_TRUE(handshake.receive(repeated_time_request, 120000, 190000));
  EXPECT_FALSE(handshake.take_pending_notification());
}

TEST(NativeWorld, TutorialMoveRequiresSupportedCharacter) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol, true);

  Character character;
  character.id = 5;
  character.class_id = 4;
  character.name = "Other";
  EXPECT_FALSE(handshake.configure_bootstrap_character(character));

  const auto time_request = protocol.encode(20001, nlohmann::json::object(), 0);
  ASSERT_TRUE(handshake.receive(time_request, 120000, 190000));
  EXPECT_FALSE(handshake.take_pending_notification());
}

TEST(NativeWorld, TutorialStartAcceptsClassesSharingMap8000) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  for (const int class_id : {1, 2, 3, 5}) {
    WorldHandshake handshake(protocol, true);
    Character character;
    character.id = class_id;
    character.class_id = class_id;
    character.name = "Local";
    ASSERT_TRUE(handshake.configure_bootstrap_character(character));

    const auto time_request = protocol.encode(20001, nlohmann::json::object(), 0);
    const auto map_start = handshake.receive(time_request, 120000, 190000);
    ASSERT_TRUE(map_start);
    EXPECT_EQ(map_start->opcode(), 175);
    EXPECT_EQ(protocol.decode(*map_start).at("message").at("map_id"), 8000);
  }
}

TEST(NativeWorld, TutorialStartRejectsCharacterSavedOnAnotherMap) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol, true);
  Character character;
  character.id = 1;
  character.class_id = 1;
  character.name = "Local";
  character.world_position = WorldPosition{10116, 0, 0, 0, 0.0F};

  EXPECT_FALSE(handshake.configure_bootstrap_character(character));
}

// The largest supported character id (kMaxCharacterId) still has a wire unit id below the server
// NPC range; the first id above it (0x08000000) and any id in the NPC range are refused by both
// world entry points, because their wire ids would reach kFirstNpcUnitId.
TEST(NativeWorld, CharacterIdBeyondTheSupportedMaximumIsRefusedAtWorldEntry) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  Character allowed;
  allowed.id = kMaxCharacterId;
  allowed.class_id = 1;
  allowed.name = "Local";
  WorldHandshake allowed_handshake(protocol, true);
  EXPECT_TRUE(allowed_handshake.configure_bootstrap_character(allowed));
  EXPECT_LT(player_wire_unit_id(allowed.id), kFirstNpcUnitId);

  Character above_maximum = allowed;
  above_maximum.id = 0x08000000LL;
  Character npc_range = allowed;
  npc_range.id = 0x80000000LL;
  WorldPresence presence;
  std::vector<WorldPresenceEvent> events;
  EXPECT_NO_THROW(presence.enter(1, {allowed, WorldPosition{8000, 0, 0, 0, 0.0F}}, events));
  for (const auto &refused : {above_maximum, npc_range}) {
    WorldHandshake refused_handshake(protocol, true);
    EXPECT_FALSE(refused_handshake.configure_bootstrap_character(refused)) << refused.id;
    EXPECT_THROW(presence.enter(2, {refused, WorldPosition{8000, 0, 0, 0, 0.0F}}, events),
                 std::invalid_argument)
        << refused.id;
  }
}

TEST(NativeWorld, PingThenExitUsesIndependentServerHeader) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol);
  const auto ping = protocol.encode(20003, {{"ping_cycles64", "55"}, {"tick", "123"}}, 0);

  const auto pong = handshake.receive(ping, 180000, 190000);
  ASSERT_TRUE(pong);
  EXPECT_EQ(protocol.decode(*pong).at("message").at("ping_cycles64"), "55");
  const auto pong_message = protocol.decode(*pong).at("message");
  EXPECT_EQ(pong_message.at("server_send_tick"), "18000000");
  EXPECT_EQ(pong_message.at("timestamp"), "190000");

  const auto exit_request = protocol.encode(20002, nlohmann::json::object(), 1);
  const auto exit_response = handshake.receive(exit_request, 180000, 190000);
  ASSERT_TRUE(exit_response);
  EXPECT_EQ(exit_response->bytes, (Bytes{4, 0, 6, 0}));
  EXPECT_TRUE(handshake.exited());
  EXPECT_THROW(handshake.receive(ping, 180000, 190000), std::runtime_error);
}

TEST(NativeWorld, RejectsReorderedSequenceAndWrongDirection) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol);

  const auto reordered = protocol.encode(20001, nlohmann::json::object(), 5);
  EXPECT_THROW(handshake.receive(reordered, 1, 1), std::runtime_error);

  const auto server_packet = handshake.entry_response();
  EXPECT_THROW(handshake.receive(server_packet, 1, 1), std::runtime_error);
}

TEST(NativeWorld, NearbyPlayersReceiveSymmetricPresenceAndMovement) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldPresence presence;

  Character first_character;
  first_character.id = 1;
  first_character.class_id = 2;
  first_character.level = 1;
  first_character.name = "First";

  Character second_character = first_character;
  second_character.id = 2;
  second_character.name = "Second";

  const WorldPosition first_position{8000, 0, 0, 0, 0.0F};
  const WorldPosition nearby_position{8000, 1000, 0, 0, 90.0F};
  std::vector<WorldPresenceEvent> events;
  presence.enter(101, {first_character, first_position}, events);
  EXPECT_TRUE(events.empty());

  presence.enter(102, {second_character, nearby_position}, events);
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events.at(0).recipient_connection_id, 102);
  EXPECT_EQ(events.at(1).recipient_connection_id, 101);
  EXPECT_EQ(events.at(0).kind, WorldPresenceEventKind::add);
  EXPECT_EQ(events.at(1).kind, WorldPresenceEventKind::add);

  for (const auto &event : events) {
    const auto frame = protocol.encode(10, event.message);
    const auto decoded = protocol.decode(frame);
    EXPECT_EQ(decoded.at("status"), "decoded");
    EXPECT_TRUE(decoded.at("message").at("by_sight").get<bool>());
  }
  EXPECT_EQ(events.at(0).message.at("unit_id"), std::to_string(player_wire_unit_id(1)));
  EXPECT_EQ(events.at(1).message.at("unit_id"), std::to_string(player_wire_unit_id(2)));

  const nlohmann::json movement{
      {"tick", "123"},
      {"pos", {{"millimeter_x", 2000}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
      {"body_yaw", 45.0F}};
  events = presence.move(101, {8000, 2000, 0, 0, 45.0F}, movement);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events.at(0).recipient_connection_id, 102);
  EXPECT_EQ(events.at(0).kind, WorldPresenceEventKind::move);
  EXPECT_EQ(events.at(0).message.at("unit_id"), std::to_string(player_wire_unit_id(1)));
  const auto move_frame = protocol.encode(177, events.at(0).message);
  EXPECT_EQ(protocol.decode(move_frame).at("message").at("body_yaw"), 45.0F);
  EXPECT_EQ(protocol.decode(move_frame).at("message").at("tick"), "123");

  events = presence.leave(101);
  ASSERT_EQ(events.size(), 1);
  EXPECT_EQ(events.at(0).recipient_connection_id, 102);
  EXPECT_EQ(events.at(0).kind, WorldPresenceEventKind::remove);
  const auto remove_frame = protocol.encode(11, events.at(0).message);
  EXPECT_EQ(protocol.decode(remove_frame).at("message").at("unit_ids").at(0),
            std::to_string(player_wire_unit_id(1)));
}

TEST(NativeWorld, SightRangeSuppressesDistantAndOtherMapPlayers) {
  WorldPresence presence;
  Character character;
  character.id = 1;
  character.class_id = 2;
  character.name = "First";

  std::vector<WorldPresenceEvent> events;
  presence.enter(101, {character, {8000, 0, 0, 0, 0.0F}}, events);
  character.id = 2;
  presence.enter(102, {character, {8000, 200000, 0, 0, 0.0F}}, events);
  character.id = 3;
  presence.enter(103, {character, {8001, 0, 0, 0, 0.0F}}, events);
  EXPECT_TRUE(events.empty());

  const nlohmann::json movement{
      {"pos", {{"millimeter_x", 99000}, {"millimeter_y", 0}, {"millimeter_z", 0}}}};
  events = presence.move(102, {8000, 99000, 0, 0, 0.0F}, movement);
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events.at(0).recipient_connection_id, 102);
  EXPECT_EQ(events.at(1).recipient_connection_id, 101);

  events = presence.move(102, {8000, 121000, 0, 0, 0.0F}, movement);
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events.at(0).kind, WorldPresenceEventKind::remove);
  EXPECT_EQ(events.at(1).kind, WorldPresenceEventKind::remove);
}

// Fills the catalog tables the NPC shop service requires but a focused merchant fixture does
// not care about, so the shop catalog loads and requests reach the target checks.


// Authorization tests can send many cases; keep those requests out of the rate-limit path.
// The dedicated merchant-service test above/below separately checks the ninth request refusal.







// Reads until a frame with the expected opcode, skipping unrelated notifications.




















// A non-tutorial world session with a merchant catalog: shop 1001 (interaction 212) lists
// stackable 133, stackable 70109003 at 2000 per mille, and equipment 11200201.






// Shop requests resolve the target through the units this server announced to the session. A
// session without tutorial data or a placed population announces none, so even the documented
// merchant target 1001 on the catalog interaction 212 is refused as not found.


// Buy and sell need an announced merchant; a request for an unannounced one must change neither
// money, stackables nor the revision.




// PostgreSQL-backed variant of the fail-closed check: buy and sell are refused and a fresh
// PostgreSQL repository reads back byte-equal progress. Gated like the other
// PostgreSQL tests; use only the disposable cluster (never port 5433).


// Sends a shop request with an explicit `target_unit_id` through the world session;
// MerchantWorld::send_shop always uses one fixed target.


// A zero unit id is malformed; any other id the session never announced is simply not found.




// While no server-owned NPC registry, map/instance or position source exists, no target id
// authorizes a trade: unknown, forged, malformed and even plausible ids are refused, whichever
// interaction they name, and gold, stackables and the revision stay as they were.














// The Grand weapon's second step succeeds 60% of the time. Whatever the roll, the attempt is
// charged in full and reports a consistent step and result.






















// Creates an account and character, enters realm 1 and selects the character.


// Authenticates and completes the shared-map entry sequence through map load.



TEST(NativeWorld, TwoTlsClientsObserveEachOtherAndSerializedWorldFrames) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();

  const auto first = select_world_test_character(auth, "first_world_test", "FirstWorld", 1);
  const auto second = select_world_test_character(auth, "second_world_test", "SecondWorld", 3);
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  const auto enter_world = [&](WorldTestClient &client, const std::string &packet_key) {
    client.send_key(packet_key);
    EXPECT_EQ(client.read().opcode(), 5);

    client.send(protocol.encode(20001, nlohmann::json::object(), 0));
    EXPECT_EQ(client.read().opcode(), 175);
    EXPECT_EQ(client.read().opcode(), 1);
    read_entry_inventory(client);
    EXPECT_EQ(client.read().opcode(), 126);

    client.send(protocol.encode(20085, nlohmann::json::object(), 1));
    const auto map_complete = client.read();
    EXPECT_EQ(map_complete.opcode(), 176);
    EXPECT_EQ(protocol.decode(map_complete).at("status"), "decoded");
    expect_pre_playing_combat_bootstrap(client, protocol);
  };

  WorldTestClient first_client(server.port());
  enter_shared_world(first_client, protocol, first.packet_key());
  WorldTestClient second_client(server.port());
  enter_shared_world(second_client, protocol, second.packet_key());

  const auto first_add = first_client.read();
  const auto second_add = second_client.read();
  ASSERT_EQ(first_add.opcode(), 10);
  ASSERT_EQ(second_add.opcode(), 10);
  EXPECT_EQ(protocol.decode(first_add).at("message").at("unit_id"),
            std::to_string(player_wire_unit_id(second.character().char_id())));
  EXPECT_EQ(protocol.decode(second_add).at("message").at("unit_id"),
            std::to_string(player_wire_unit_id(first.character().char_id())));

  const nlohmann::json first_move{
      {"tick", "123"},
      {"pos", {{"millimeter_x", -794146}, {"millimeter_y", 712194}, {"millimeter_z", -698778}}},
      {"body_yaw", 90.0F}};
  first_client.send(protocol.encode(20087, first_move, 2));
  const auto move_to_second = second_client.read();
  ASSERT_EQ(move_to_second.opcode(), 177);
  EXPECT_EQ(protocol.decode(move_to_second).at("message").at("unit_id"),
            std::to_string(player_wire_unit_id(first.character().char_id())));
  EXPECT_EQ(protocol.decode(move_to_second).at("message").at("tick"), first_move.at("tick"));

  const nlohmann::json second_move{
      {"tick", "124"},
      {"pos", {{"millimeter_x", -793146}, {"millimeter_y", 712194}, {"millimeter_z", -698778}}},
      {"body_yaw", 80.0F}};
  second_client.send(protocol.encode(20087, second_move, 2));
  const auto move_to_first = first_client.read();
  ASSERT_EQ(move_to_first.opcode(), 177);
  EXPECT_EQ(protocol.decode(move_to_first).at("message").at("unit_id"),
            std::to_string(player_wire_unit_id(second.character().char_id())));
  EXPECT_EQ(protocol.decode(move_to_first).at("message").at("tick"), second_move.at("tick"));

  first_client.disconnect();
  const auto removal = second_client.read();
  ASSERT_EQ(removal.opcode(), 11);
  EXPECT_EQ(protocol.decode(removal).at("message").at("unit_ids").at(0),
            std::to_string(player_wire_unit_id(first.character().char_id())));
}

// Two shared-map clients form a party over TLS: invite popup, accept, roster and
// unit identity, party chat, and the leave notification all use recovered schemas.


// A guild master invites an observed player over TLS. Entry snapshots, the invite popup,
// acceptance and both clients' unit labels use the recovered PS2C guild schemas.









// ---- Candidate in-place revival (opcode 20039) on a real fight. ----

// A persistent memory-backed session of a level-1 Wild whose only danger is the real profile
// monster 5001084: standing inside its perception range it acquires the player and kills it in
// about 16 s through the real death path. There is no test seam; the only shortcut is the generous
// per-read deadline the test client already has. Needs the profile with overlap groups
// (CHRONICLES_TUTORIAL_TEST_PROFILE); a skip is not proof.






// The client types units by the low nibble of their 64-bit id (1 = player) and only applies the
// Demise state to a nibble-1 local unit. Production character 19 (0x13, nibble 3) never entered
// Demise; the wire unit id must be (character id << 4) | 1 on every frame naming the player, for
// every raw database id (1 and 17 already had nibble 1, 19 did not).
void expect_player_wire_unit_id_for_raw_character(const char *profile_path, int filler_characters,
                                                  std::int64_t raw_character_id) {
  DeathFixture fixture(profile_path, filler_characters);
  ASSERT_EQ(fixture.character_id(), raw_character_id);
  const std::uint64_t wire_unit_id = player_wire_unit_id(raw_character_id);
  const std::string wire_text = std::to_string(wire_unit_id);
  EXPECT_EQ(wire_unit_id & 0xF, 1u);

  // The world entry announces the wire id, and persistence still resolves the raw character.
  auto session = fixture.connect();
  EXPECT_EQ(fixture.entry_player_unit_id(), wire_unit_id);

  std::vector<nlohmann::json> seen;
  const auto death = fixture.read_until(session, 23, &seen);
  EXPECT_EQ(death.at("target_unit_id"), wire_text);
  EXPECT_TRUE(fixture.progress().dead);

  bool demise_for_wire_id = false;
  for (const auto &frame : seen) {
    const auto &message = frame.at("message");
    if (frame.at("opcode") == 32 &&
        message.value("state", std::string()) == "EX5UnitStateType_Demise") {
      demise_for_wire_id = demise_for_wire_id || message.value("unit_id", std::string()) == wire_text;
    }
    if (frame.at("opcode") == 19) {
      EXPECT_EQ(message.at("target_unit_id"), wire_text);
    }
    // No frame leaks the raw database id as a unit id.
    for (const char *field : {"unit_id", "target_unit_id", "caster_unit_id"}) {
      if (raw_character_id != static_cast<std::int64_t>(wire_unit_id)) {
        EXPECT_NE(message.value(field, std::string()), std::to_string(raw_character_id))
            << frame.at("opcode") << " " << field;
      }
    }
  }
  EXPECT_TRUE(demise_for_wire_id);

  // Official order (4 of 4 player deaths): Demise, then the durability ItemsUpdate, then the
  // lethal DamageNoti. The fixture character wears starter equipment with a durability loss, so
  // the death's own ItemsUpdate must exist and sit strictly between the Demise and the lethal
  // hit; the commit precedes every publication.
  const auto player_frame_index = [&](int opcode, std::size_t from) -> std::ptrdiff_t {
    for (std::size_t index = from; index < seen.size(); ++index) {
      const auto &frame = seen[index];
      const auto &message = frame.at("message");
      const bool names_player = message.value("unit_id", std::string()) == wire_text ||
                                message.value("target_unit_id", std::string()) == wire_text;
      if (frame.at("opcode") == opcode && (opcode == 169 || names_player)) {
        return static_cast<std::ptrdiff_t>(index);
      }
    }
    return -1;
  };
  std::ptrdiff_t demise_index = -1;
  for (std::size_t index = 0; index < seen.size(); ++index) {
    const auto &message = seen[index].at("message");
    if (seen[index].at("opcode") == 32 &&
        message.value("state", std::string()) == "EX5UnitStateType_Demise" &&
        message.value("unit_id", std::string()) == wire_text) {
      demise_index = static_cast<std::ptrdiff_t>(index);
    }
  }
  ASSERT_GE(demise_index, 0);
  const auto items_index = player_frame_index(169, static_cast<std::size_t>(demise_index) + 1);
  const auto lethal_index = player_frame_index(19, static_cast<std::size_t>(demise_index) + 1);
  ASSERT_GT(items_index, demise_index) << "no durability ItemsUpdate follows the Demise";
  ASSERT_GT(lethal_index, demise_index);
  EXPECT_LT(items_index, lethal_index);
  const auto &durability_items = seen[static_cast<std::size_t>(items_index)].at("message").at("items");
  ASSERT_FALSE(durability_items.empty());
  for (const auto &item : durability_items) {
    EXPECT_TRUE(item.contains("durability"));
    EXPECT_TRUE(item.contains("equip_slot"));
  }

  // Revival frames name the same wire id (the idle-state frame is matched by it), and a reconnect
  // announces the same id again while the database row is alive.
  EXPECT_EQ(fixture.revive_and_collect_revival_opcodes(session), (std::vector<int>{49, 51, 47, 32}));
  EXPECT_FALSE(fixture.progress().dead);
  session.client->disconnect();
  auto reconnected = fixture.connect();
  EXPECT_EQ(fixture.entry_player_unit_id(), wire_unit_id);
  reconnected.client->disconnect();
}

TEST(NativeWorld, PlayerWireUnitIdHasThePlayerTypeNibbleForRawCharacterIds1And17And19) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the profile with overlap groups";
  }
  expect_player_wire_unit_id_for_raw_character(profile_path, 0, 1);
  expect_player_wire_unit_id_for_raw_character(profile_path, 16, 17);
  expect_player_wire_unit_id_for_raw_character(profile_path, 18, 19);
}

// The pursuit of an NPC reaches a real client socket as PS2C_MoveNoti (177) in the walking
// shape. Monster 5001084 detects a player inside its 3000 mm perception circle in any direction
// and stops at its 2000 mm combat distance: the player stands 2950 mm away (current persisted
// position, inside perception, outside the engagement distance), so the monster acquires and
// really walks about 950 mm before its first attack (opcode 14), which ends the approach.


// ---- Eligibility regression (target/map/instance), Phase A. ----

// Shop OPEN is refused for every target and interaction in a session that announced no NPC:
// exactly one opcode-2 BadRequest per request, no 163/171 frame, and the persisted progress
// (gold, stackables, revision) never changes. A stray extra frame would be read by the next
// iteration and fail its opcode check. target_unit_id is a uint64 on the wire, so malformed text
// cannot be encoded by the test protocol and is covered by parse_unit_id_text unit tests.


// A refused open must not make a later buy or sell for the same target eligible, and the
// refusal order does not matter: buy and sell first, then open, are all BadRequest.


// Merchant requests are resolved through the session's announced-unit registry in tutorial
// sessions too: an id the server never announced is refused as not found and changes nothing.


// Stress, not a race proof: shop requests are resolved through the session registry on the
// worker strand while a disconnect runs close() on the io executor. Each round queues a burst of
// requests without reading the replies and drops the connection at once. The server must stay up,
// and persisted progress must stay unchanged because every request is refused.
TEST(NativeWorld, DisconnectWithPendingShopRequestsLeavesProgressUnchanged) {
  constexpr int kRounds = 6;
  constexpr int kPendingRequests = 8;
  for (int round = 0; round < kRounds; ++round) {
    SCOPED_TRACE("round " + std::to_string(round));
    MerchantWorld world(1000, {{72000016, 4}});
    const auto before = world.progress();
    for (int request = 0; request < kPendingRequests; ++request) {
      world.send_shop(nlohmann::json::object());
    }
    world.client.reset();
    EXPECT_EQ(world.progress(), before);
  }
}

// Object-level check only: two UnitRegistry objects hold no shared state. This says nothing about
// session isolation (the session's registry is private to the session and has no seam); native
// isolation is covered only through observable server behaviour in the session tests below.
TEST(UnitRegistryObjects, SeparateInstancesShareNoState) {
  UnitRegistry first(4);
  UnitRegistry second(4);
  const AnnouncedUnit unit{5001, 1001671, 8000, "instance-a", {1, 2, 3}};
  ASSERT_EQ(first.announce(unit), UnitAnnounceResult::Added);
  EXPECT_EQ(second.find(5001), nullptr);
  ASSERT_EQ(second.announce(unit), UnitAnnounceResult::Added);

  first.clear();
  EXPECT_EQ(first.find(5001), nullptr);
  EXPECT_NE(second.find_in_instance(5001, 8000, "instance-a"), nullptr);
  EXPECT_EQ(second.find_in_instance(5001, 8000, "instance-b"), nullptr);
  EXPECT_TRUE(second.despawn(5001));
  EXPECT_EQ(second.find(5001), nullptr);
}

// ---- Session behaviour through the wire only (no registry introspection). ----
//
// Every shop request is refused today (the guard has no announced NPC to resolve in a
// non-tutorial session; tutorial sessions bypass it), so these tests cannot see registry
// contents. They prove what is observable: sessions are isolated from each other's traffic and
// progress, a disconnect with frames in flight neither wedges nor corrupts the server, and a
// reconnected character is refused exactly like a fresh one with persisted progress intact.

// One non-tutorial server with several characters, each with its own connection. Presence
// notifications (opcodes 10, 11, 177) from the other characters arrive at any time, so every
// read here skips them and only the remaining frames are counted.
class MerchantSessions {
public:
  struct Session {
    std::string access_token;
    std::int64_t account_id{};
    std::int64_t character_id{};
    std::unique_ptr<WorldTestClient> client;
    std::uint32_t sequence{2};
  };

  explicit MerchantSessions(const std::vector<std::pair<std::int64_t, std::map<int, int>>> &start)
      : protocol(CHRONICLES_PROTOCOL_DIR) {
    config.game_address = "127.0.0.1:8081";
    auth = std::make_unique<AuthService>(repositories.character_repositories(), config);
    auth->acquire_monster_reward_owner_epoch();
    const auto price = [](const char *id, const char *category, const char *price_value,
                          const char *refund_value) {
      return nlohmann::json{{"id", id},
                            {"category", category},
                            {"priceType", "Gold"},
                            {"priceValue", price_value},
                            {"refundType", "Gold"},
                            {"refundValue", refund_value}};
    };
    catalog.write_items(
        {price("133", "Consumable", "500", "500"), price("70109003", "CraftMaterial", "5", "2"),
         price("72000016", "CraftMaterial", "10", "1")},
        {{"Npc", {{{"id", "1001121"}, {"interactionGroupId", "32"}}}},
         {"Interaction",
          {{{"id", "212"},
            {"groupId", "32"},
            {"playerTaskCase", "Shop"},
            {"playerTask", "\"shopId\":1001"}}}},
         {"Shop",
          {{{"id", "1001"}, {"FK_itemId", "133"}, {"priceMultiplierPerMille", ""}},
           {{"id", "1001"}, {"FK_itemId", "70109003"}, {"priceMultiplierPerMille", "2000"}}}}});

    for (std::size_t index = 0; index < start.size(); ++index) {
      const std::string suffix = std::to_string(index);
      const std::string account_name = "merchant_sessions" + suffix;
      auth->create_account(account_name, "test-world-password-123");
      const auto login = auth->login(account_name, "test-world-password-123");
      x5::pb::authwebsocket::WorldEnterReq world_request;
      world_request.set_world_id(1);
      if (auth->enter(login.access_token(), world_request).error_code() != x5::ec::Success) {
        throw std::runtime_error("Session fixture cannot enter the world");
      }
      x5::pb::ws::front::CharCreateReq create_request;
      create_request.set_slot(0);
      create_request.set_char_name("MerchSess" + suffix);
      create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
      const auto created = auth->create(login.access_token(), create_request);
      if (created.ec() != x5::ec::Success) {
        throw std::runtime_error("Session fixture cannot create a character");
      }
      auto &session = sessions.emplace_back();
      session.access_token = login.access_token();
      session.account_id = login.account_id();
      session.character_id = created.char_().char_id();
      set_progress(session, start[index].first, start[index].second);
    }
    server = std::make_unique<TwoClientWorldServer>(*auth, certificate, nullptr, nullptr,
                                                    catalog.open());
    for (auto &session : sessions) {
      enter_world(session);
    }
  }

  void set_progress(const Session &session, std::int64_t gold, const std::map<int, int> &stacks) {
    const auto before = repositories.character_state->load_world_progress(session.account_id,
                                                                          session.character_id, 1);
    auto after = before;
    after.revision = before.revision + 1;
    after.gold = gold;
    after.stackables = stacks;
    ASSERT_TRUE(repositories.character_state->save_world_progress(
        session.account_id, session.character_id, 1, before, after));
  }

  WorldProgress progress(const Session &session) const {
    return repositories.character_state->load_world_progress(session.account_id,
                                                             session.character_id, 1);
  }

  // Next frame that is not a presence notification.
  Frame read_frame(Session &session) {
    for (;;) {
      auto frame = session.client->read();
      if (frame.opcode() != 10 && frame.opcode() != 11 && frame.opcode() != 177) {
        return frame;
      }
    }
  }

  // Reads until the given opcode arrives; used to observe that another character left.
  void wait_for_frame(Session &session, std::uint16_t opcode) {
    for (;;) {
      if (session.client->read().opcode() == opcode) {
        return;
      }
    }
  }

  void enter_world(Session &session) {
    x5::pb::ws::front::CharSelectReq select_request;
    select_request.set_char_id(session.character_id);
    const auto selected = auth->select(session.access_token, select_request);
    if (selected.ec() != x5::ec::Success) {
      throw std::runtime_error("Session fixture cannot select the character");
    }
    session.sequence = 2;
    session.client = std::make_unique<WorldTestClient>(server->port());
    session.client->send_key(selected.packet_key());
    EXPECT_EQ(read_frame(session).opcode(), 5);
    session.client->send(protocol.encode(20001, nlohmann::json::object(), 0));
    for (const std::uint16_t opcode : {175, 1, 128, 130, 132, 126}) {
      EXPECT_EQ(read_frame(session).opcode(), opcode);
    }
    session.client->send(protocol.encode(20085, nlohmann::json::object(), 1));
    for (const std::uint16_t opcode : {176, 78, 78, 33, 115, 33}) {
      EXPECT_EQ(read_frame(session).opcode(), opcode);
    }
  }

  MerchantRequestPacer shop_pacer;

  void send_shop(Session &session, const std::string &target, int interaction_id,
                 const nlohmann::json &shop) {
    shop_pacer.wait();
    session.client->send(protocol.encode(
        20068,
        {{"target_unit_id", target},
         {"param", {{"interaction_id", interaction_id}, {"shop", shop}}}},
        session.sequence++));
  }

  // Exactly one unannounced-target refusal, then the pong of a following ping.
  void expect_one_refusal_then_pong(Session &session) {
    const auto reply = read_frame(session);
    ASSERT_EQ(reply.opcode(), 2);
    EXPECT_EQ(protocol.decode(reply).at("message").at("ec"),
              x5::ec::EErrorCode_Name(x5::ec::Interaction_TargetNotFound));
    session.client->send(protocol.encode(20003, nlohmann::json::object(), session.sequence++));
    EXPECT_EQ(read_frame(session).opcode(), 4);
  }

  MemoryRepositories repositories;
  Config config;
  Protocol protocol;
  std::unique_ptr<AuthService> auth;
  TemporaryWorldCatalog catalog;
  TemporaryWorldCertificate certificate;
  std::unique_ptr<TwoClientWorldServer> server;
  std::vector<Session> sessions;
};

// Two characters on one server send interleaved shop traffic that names each other's unit ids
// (a player's unit id is its character id) and the same fabricated ids. Each request gets exactly
// one BadRequest on its own connection, and each character's gold, stackables and revision stay
// exactly as persisted: nothing leaks across sessions.
TEST(NativeWorld, ConcurrentSessionsRefuseEachOthersTargetsAndKeepSeparateProgress) {
  MerchantSessions world({{1000, {{72000016, 4}}}, {77, {{70109003, 9}}}});
  auto &first = world.sessions[0];
  auto &second = world.sessions[1];
  const auto first_before = world.progress(first);
  const auto second_before = world.progress(second);
  ASSERT_NE(first_before, second_before);

  const std::vector<std::string> targets{std::to_string(first.character_id),
                                         std::to_string(second.character_id), "1001", "999999"};
  for (const auto &target : targets) {
    for (const auto &shop : {nlohmann::json::object(), buy_list(133, 1), sell_list(72000016, 4)}) {
      SCOPED_TRACE("target " + target);
      world.send_shop(first, target, 212, shop);
      world.send_shop(second, target, 212, shop);
      world.expect_one_refusal_then_pong(first);
      world.expect_one_refusal_then_pong(second);
      EXPECT_EQ(world.progress(first), first_before);
      EXPECT_EQ(world.progress(second), second_before);
    }
  }
}

// A client that disconnects while shop requests are still queued must not disturb the other
// session, and the same character reconnecting is refused exactly like a fresh session: every
// request (including the ones the old session had in flight and the unit ids it used) gets one
// BadRequest, persisted progress is unchanged, and no stray frame from the old session shows up.
// This is stress evidence, not a race-detector proof.
TEST(NativeWorld, DisconnectWithFramesInFlightThenReconnectIsRefusedLikeAFreshSession) {
  MerchantSessions world({{1000, {{72000016, 4}}}, {77, {{70109003, 9}}}});
  auto &first = world.sessions[0];
  auto &second = world.sessions[1];
  const auto first_before = world.progress(first);
  const auto second_before = world.progress(second);

  const std::vector<std::string> targets{"1001", "999999", std::to_string(second.character_id)};
  for (int burst = 0; burst < 8; ++burst) {
    for (const auto &target : targets) {
      world.send_shop(first, target, 212, nlohmann::json::object());
      world.send_shop(first, target, 212, buy_list(133, 1));
      world.send_shop(first, target, 212, sell_list(72000016, 4));
    }
  }
  first.client->disconnect();

  // The other session sees the first leave (its presence removal) and is unaffected.
  world.wait_for_frame(second, 11);
  world.send_shop(second, "1001", 212, buy_list(70109003, 1));
  world.expect_one_refusal_then_pong(second);
  EXPECT_EQ(world.progress(second), second_before);
  EXPECT_EQ(world.progress(first), first_before);

  world.enter_world(first);
  auto first_after_reentry = first_before;
  ++first_after_reentry.revision;
  EXPECT_EQ(world.progress(first), first_after_reentry);
  for (const auto &target : targets) {
    for (const auto &shop : {nlohmann::json::object(), buy_list(133, 1), sell_list(72000016, 4)}) {
      SCOPED_TRACE("reconnected, target " + target);
      world.send_shop(first, target, 212, shop);
      world.expect_one_refusal_then_pong(first);
      EXPECT_EQ(world.progress(first), first_after_reentry);
    }
  }
  EXPECT_EQ(world.progress(second), second_before);
}


// The closest observable form today of "a session refuses targets from a tutorial session": a
// non-tutorial session is refused for every unit id the tutorial profile announces (the ids are
// identical in every tutorial session). A map-9012 session cannot be tested yet: there is no entry
// path for it (no `9012` in native_world.cpp at the base), so that case is pending.
TEST(NativeWorld, NonTutorialSessionRefusesEveryUnitIdTheTutorialAnnounces) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the generated local tutorial profile";
  }
  const auto definitions = TutorialData::load(profile_path);
  std::vector<std::uint64_t> tutorial_ids;
  for (const auto &entry : definitions->npcs) {
    tutorial_ids.push_back(entry.first);
  }
  for (const auto &entry : definitions->objects) {
    tutorial_ids.push_back(entry.first);
  }
  ASSERT_FALSE(tutorial_ids.empty());

  MerchantSessions world({{1000, {{72000016, 4}}}});
  auto &session = world.sessions[0];
  const auto before = world.progress(session);
  for (const auto unit_id : tutorial_ids) {
    SCOPED_TRACE("tutorial unit " + std::to_string(unit_id));
    world.send_shop(session, std::to_string(unit_id), 212, nlohmann::json::object());
    world.expect_one_refusal_then_pong(session);
    EXPECT_EQ(world.progress(session), before);
  }
}

// Map-9012 entry through the real caller: a character whose PERSISTED position is on the loaded
// population's map enters a non-tutorial session on that map (the server, never the client,
// picks the map) and receives the server-owned NPC units after its map-load-complete.

// A placement `offset_mm` east of the overworld spawn, inside NPC sight of a player standing
// there. The offset also names the placement's spawner guid.


// Three active placements and one inactive one: only the active ones may be announced.








// A character saved on the shared map (9012) is outside the private tutorial instance. Its quick
// item slots must commit there and come back on the next login; before this path existed the
// request was ignored and the entry sent no WorldCharUtilityBagNoti, so the slots looked empty
// after every rejoin (hosted-server report, 2026-10-27).

void verify_zone_channel_crossing(const WorldPosition &boundary, int destination_map) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs generated tutorial, map-9012 placements and channel boundaries";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  if (!std::filesystem::exists(world.catalog_root.parent_path() / "triggers" /
                               "ArcheWorld-level-triggers.json")) {
    GTEST_SKIP() << "Run tools/build_level_trigger_volumes.py first";
  }

  auto character = world.add_character("ChannelTravel", boundary);
  const auto before = world.progress(character);
  const auto expect_preserved_progress = [&] {
    auto actual = world.progress(character);
    EXPECT_GE(actual.revision, before.revision);
    actual.revision = before.revision;
    EXPECT_EQ(actual, before);
  };
  auto client = std::make_unique<WorldTestClient>(world.server->port());
  world.enter(character, *client);
  const nlohmann::json boundary_pos{{"millimeter_x", boundary.x_mm},
                                    {"millimeter_y", boundary.y_mm},
                                    {"millimeter_z", boundary.z_mm}};
  client->send(world.protocol.encode(
      20087, {{"pos", boundary_pos}, {"mode", "MoveMode_Normal"},
              {"detail", "MoveDetail_Sprint"}}, character.sequence++));
  int stamina_after_sprint = -1;
  for (int count = 0; count < 20 && stamina_after_sprint < 0; ++count) {
    const auto changed = world.protocol.decode(read_until_opcode(*client, 47)).at("message");
    if (changed.value("default_source", std::string{}) != "SCS_SprintFlow") {
      continue;
    }
    for (const auto &stat : changed.at("stat_changes")) {
      if (stat.at("stat_type") == 72) {
        stamina_after_sprint = stat.at("result").get<int>();
      }
    }
  }
  ASSERT_GE(stamina_after_sprint, 0);
  client->send(world.protocol.encode(
      20087, {{"pos", boundary_pos}, {"mode", "MoveMode_Normal"},
              {"detail", "MoveDetail_Stop"}}, character.sequence++));
  client->send(world.protocol.encode(
      20090, {{"pos", {{"millimeter_x", boundary.x_mm}, {"millimeter_y", boundary.y_mm},
                        {"millimeter_z", boundary.z_mm}}}, {"body_yaw", boundary.yaw}},
      character.sequence++));
  const auto started = world.protocol.decode(read_until_opcode(*client, 175)).at("message");
  EXPECT_EQ(started.at("map_id"), destination_map);
  EXPECT_EQ(started.at("spawn_point").at("pos").value("millimeter_x", 0), boundary.x_mm);
  EXPECT_GT(std::stoll(started.at("started_timestamp").get<std::string>()), 0);

  client->send(world.protocol.encode(20085, nlohmann::json::object(), character.sequence++));
  const auto completed = world.protocol.decode(read_until_opcode(*client, 176)).at("message");
  EXPECT_EQ(completed.at("map_id"), destination_map);
  EXPECT_EQ(completed.at("nub").at("player").at("stats").at("72"), stamina_after_sprint);
  client->send(world.protocol.encode(20086, nlohmann::json::object(), character.sequence++));
  read_until_opcode(*client, 183);
  expect_preserved_progress();
  const auto saved = world.repositories.characters->characters(character.account_id, 1).front();
  ASSERT_TRUE(saved.world_position);
  EXPECT_EQ(saved.world_position->map_id, destination_map);
  EXPECT_EQ(saved.world_position->x_mm, boundary.x_mm);
  client->disconnect();
  client.reset();

  const auto resumed = world.enter_when_character_is_free(character, client);
  EXPECT_EQ(resumed.map_ids, (std::vector<int>{destination_map, destination_map}));
  EXPECT_TRUE(resumed.npcs.empty());
  expect_preserved_progress();
}

TEST(NativeWorld, ZoneChannelBoundaryCompletesTravelAndReconnectsWithoutForeignNpcs) {
  verify_zone_channel_crossing({9012, -292793, 3480751, -601055, 60}, 10110);
}

TEST(NativeWorld, SpatialRegionCrossingCompletesTravelAndReconnects) {
  // First occupied cell of the exported Zone_02 grid, with no channelSession volume.
  verify_zone_channel_crossing({9012, -4615500, 994500, -600000, 60}, 9021);
}

TEST(NativeWorld, RegionalHandoffRetainsAcceptedGlidingState) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs generated tutorial, map-9012 placements and channel boundaries";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  const WorldPosition boundary{9012, -4615500, 994500, -600000, 60};
  auto character = world.add_character("GlidingAcrossRegion", boundary);
  WorldTestClient client(world.server->port());
  world.enter(character, client);
  client.send(world.protocol.encode(20086, nlohmann::json::object(), character.sequence++));
  read_until_opcode(client, 183);

  const nlohmann::json position{{"millimeter_x", boundary.x_mm},
                                {"millimeter_y", boundary.y_mm},
                                {"millimeter_z", boundary.z_mm}};
  client.send(world.protocol.encode(20087, {{"pos", position}, {"mode", "MoveMode_Falling"}},
                                    character.sequence++));
  client.send(world.protocol.encode(20032, {{"pos", position}, {"body_yaw", boundary.yaw}},
                                    character.sequence++));
  client.send(world.protocol.encode(20003, nlohmann::json::object(), character.sequence++));
  for (;;) {
    const auto frame = client.read();
    ASSERT_NE(frame.opcode(), 180) << "The initial glide was refused";
    if (frame.opcode() == 4) {
      break;
    }
  }

  client.send(world.protocol.encode(20090, {{"pos", position}, {"body_yaw", boundary.yaw}},
                                    character.sequence++));
  EXPECT_EQ(world.protocol.decode(read_until_opcode(client, 175)).at("message").at("map_id"),
            9021);
  client.send(world.protocol.encode(20085, nlohmann::json::object(), character.sequence++));
  const auto completed = world.protocol.decode(read_until_opcode(client, 176)).at("message");
  EXPECT_EQ(completed.at("nub").at("player").at("move_mode"), "MoveMode_Gliding");
  client.send(world.protocol.encode(20086, nlohmann::json::object(), character.sequence++));
  read_until_opcode(client, 183);

  // A duplicate start is refused only if the accepted glide survived the handoff.
  client.send(world.protocol.encode(20032, {{"pos", position}, {"body_yaw", boundary.yaw}},
                                    character.sequence++));
  const auto failure = world.protocol.decode(read_until_opcode(client, 180)).at("message");
  EXPECT_EQ(failure.at("ec"), "Move_GlideStartDuplication");
  client.disconnect();
}

TEST(NativeWorld, UnresolvedZoneRequestCompletesSameMapRecovery) {
  // Outside every public region area (the origin lies inside map 9071's area since the
  // 2026-10-29 region travel repair).
  verify_zone_channel_crossing({9012, -9000000, -11000000, 0, 60}, 9012);
}

// The centre of the first occupied cell of `map_id` in the exported spatial zone grid that no
// deeper grid covers, on `current_map_id`.
std::optional<WorldPosition> exported_zone_cell_centre(const std::filesystem::path &export_path,
                                                       int map_id, int current_map_id) {
  constexpr int kCellWidthMm = 51000;
  std::ifstream input(export_path);
  const auto document = nlohmann::json::parse(input);

  std::set<std::pair<int, int>> deeper_cells;
  const nlohmann::json *selected{};
  for (const auto &grid : document.at("grids")) {
    if (grid.at("map_id") == map_id) {
      selected = &grid;
    }
  }
  if (!selected) {
    return std::nullopt;
  }
  for (const auto &grid : document.at("grids")) {
    if (grid.value("depth", 0) <= selected->value("depth", 0)) {
      continue;
    }
    const auto &mask = grid.at("mask").get_ref<const std::string &>();
    const int width = grid.at("width");
    for (std::size_t index = 0; index < mask.size(); ++index) {
      if (mask[index] == '1') {
        deeper_cells.emplace(grid.at("min_cell_x").get<int>() + static_cast<int>(index) % width,
                             grid.at("min_cell_y").get<int>() + static_cast<int>(index) / width);
      }
    }
  }

  const auto &mask = selected->at("mask").get_ref<const std::string &>();
  const int width = selected->at("width");
  for (std::size_t index = 0; index < mask.size(); ++index) {
    const std::pair cell{selected->at("min_cell_x").get<int>() + static_cast<int>(index) % width,
                         selected->at("min_cell_y").get<int>() + static_cast<int>(index) / width};
    if (mask[index] == '1' && !deeper_cells.contains(cell)) {
      return WorldPosition{current_map_id, cell.first * kCellWidthMm + kCellWidthMm / 2,
                           cell.second * kCellWidthMm + kCellWidthMm / 2, -600000, 30};
    }
  }
  return std::nullopt;
}

// The 2026-11-02 live report: crossing into the Shining Shore village (map 9013, nested in field
// 9012) showed a loading screen. The client asks for the nested map and keeps the move seamless
// only when MapMoveStartNoti names that map at the point it asked from; leaving, it asks for
// the parent. Opcodes: 20090 ZoneChannelMoveReq, 175 MapMoveStartNoti, 20085/176, 20086/183.
TEST(NativeWorld, VillageZoneIsEnteredAndLeftAtTheReportedPoint) {
  constexpr int kField = 9012;
  constexpr int kVillage = 9013;
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs generated tutorial, map-9012 placements and zone channel cells";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  const auto export_path =
      world.catalog_root.parent_path() / "triggers" / "ArcheWorld-zone-channels.json";
  if (!std::filesystem::exists(export_path)) {
    GTEST_SKIP() << "Run tools/build_zone_channels.py first";
  }
  const auto village = exported_zone_cell_centre(export_path, kVillage, kField);
  const auto field = exported_zone_cell_centre(export_path, kField, kVillage);
  if (!village || !field) {
    GTEST_SKIP() << "The zone export predates nested zones";
  }

  const auto cross = [&](MapEntryWorld::Character &character, WorldTestClient &client,
                         const WorldPosition &from, int expected_map) {
    auto reported = from;
    reported.x_mm += 700;
    const nlohmann::json pos{{"millimeter_x", reported.x_mm},
                             {"millimeter_y", reported.y_mm},
                             {"millimeter_z", reported.z_mm}};
    client.send(world.protocol.encode(20090, {{"pos", pos}, {"body_yaw", 30.0}},
                                      character.sequence++));
    const auto started = world.protocol.decode(read_until_opcode(client, 175)).at("message");
    EXPECT_EQ(started.at("map_id"), expected_map);
    EXPECT_EQ(started.at("spawn_point").at("pos").value("millimeter_x", 0), reported.x_mm);
    client.send(world.protocol.encode(20085, nlohmann::json::object(), character.sequence++));
    EXPECT_EQ(world.protocol.decode(read_until_opcode(client, 176)).at("message").at("map_id"),
              expected_map);
    client.send(world.protocol.encode(20086, nlohmann::json::object(), character.sequence++));
    // The village population announces thousands of units before StartPlayingRes.
    constexpr int kMaximumPopulationFrames = 20000;
    read_until_opcode(client, 183, kMaximumPopulationFrames);
  };

  auto character = world.add_character("VillageWalker", *village);
  auto client = std::make_unique<WorldTestClient>(world.server->port());
  world.enter(character, *client);
  cross(character, *client, *village, kVillage);
  client->disconnect();

  // A character in the village that reaches a field-only cell returns to the field.
  auto field_from_village = *field;
  field_from_village.map_id = kVillage;
  auto leaving = world.add_character("VillageLeaver", field_from_village);
  auto leaving_client = std::make_unique<WorldTestClient>(world.server->port());
  world.enter(leaving, *leaving_client);
  cross(leaving, *leaving_client, field_from_village, kField);
  leaving_client->disconnect();
}

// The live report: the Watchtower of Hope smuggling cave (map 10119), the first cave after the
// tutorial, never opened. The client reports entering its channelSession volume with
// LevelTriggerReq and waits for the server's map move; leaving asks for the parent map with
// ChannelTriggerEntryReq {map_id: 0}. Opcodes: 20084 LevelTriggerReq, 20082
// ChannelTriggerEntryReq, 175/176 MapMoveStart/CompleteNoti, 20085 MapLoadCompleteReq,
// 20086 StartPlayingReq, 183 StartPlayingRes.
TEST(NativeWorld, InstanceVolumeEntersTheCaveAndChannelEntryLeavesIt) {
  constexpr int kCaveMap = 10119;
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs the generated tutorial profile, map-9012 placements and travel profile";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  const auto travel_path = world.catalog_root.parent_path() / "travel" / "definitions.json";
  if (!std::filesystem::exists(travel_path)) {
    GTEST_SKIP() << "Run tools/build_map_travel.py first";
  }

  std::ifstream input(travel_path);
  const auto travel = nlohmann::json::parse(input);
  const nlohmann::json *cave_volume{};
  for (const auto &volume : travel.at("volumes")) {
    if (volume.at("kind") == "channelSession" && volume.value("destination_map_id", 0) == kCaveMap &&
        volume.at("world") == "ArcheWorld") {
      cave_volume = &volume;
      break;
    }
  }
  ASSERT_NE(cave_volume, nullptr);
  const auto &center = cave_volume->at("center_cm");
  const WorldPosition cave_mouth{9012, static_cast<int>(center.at(0).get<double>() * 10),
                                 static_cast<int>(center.at(1).get<double>() * 10),
                                 static_cast<int>(center.at(2).get<double>() * 10), 0};
  const nlohmann::json mouth_pos{{"millimeter_x", cave_mouth.x_mm},
                                 {"millimeter_y", cave_mouth.y_mm},
                                 {"millimeter_z", cave_mouth.z_mm}};

  auto character = world.add_character("CaveTravel", cave_mouth);
  auto client = std::make_unique<WorldTestClient>(world.server->port());
  world.enter(character, *client);
  client->send(world.protocol.encode(
      20084,
      {{"tick", "1"},
       {"pos", mouth_pos},
       {"body_yaw", 0.0},
       {"trigger_id_x", cave_volume->at("trigger_id_wire").at(0)},
       {"trigger_id_y", cave_volume->at("trigger_id_wire").at(1)}},
      character.sequence++));
  const auto entered = world.protocol.decode(read_until_opcode(*client, 175)).at("message");
  EXPECT_EQ(entered.at("map_id"), kCaveMap);
  EXPECT_EQ(entered.at("spawn_point").at("pos").value("millimeter_x", 0), cave_mouth.x_mm);
  client->send(world.protocol.encode(20085, nlohmann::json::object(), character.sequence++));
  EXPECT_EQ(world.protocol.decode(read_until_opcode(*client, 176)).at("message").at("map_id"),
            kCaveMap);
  client->send(world.protocol.encode(20086, nlohmann::json::object(), character.sequence++));
  read_until_opcode(*client, 183);
  const auto saved = world.repositories.characters->characters(character.account_id, 1).front();
  ASSERT_TRUE(saved.world_position);
  EXPECT_EQ(saved.world_position->map_id, kCaveMap);

  client->send(world.protocol.encode(20082, {{"map_id", 0}, {"pos", mouth_pos}},
                                     character.sequence++));
  const auto left = world.protocol.decode(read_until_opcode(*client, 175)).at("message");
  EXPECT_EQ(left.at("map_id"), 9012);
  client->disconnect();
}

// The 2026-11-02 live report: a character that cast a skill while walking into a travel volume
// hit an invisible wall. The client waits for a map move after LevelTriggerReq; the busy
// character's travel was refused and never answered. It now starts once the action ends.
// Opcodes: 20010 SkillReq (potion drink, skill 1000), 14/15 skill start/end, 20084
// LevelTriggerReq, 20090 ZoneChannelMoveReq, 175 MapMoveStartNoti.
constexpr int kBusyTravelPotionItem = 40101101;

// Persists potions before entry; the drink (skill 1000) keeps the character busy for seconds.
void stock_potions(MapEntryWorld &world, const MapEntryWorld::Character &character) {
  const auto before = world.progress(character);
  auto stocked = before;
  ++stocked.revision;
  stocked.stackables[kBusyTravelPotionItem] = 5;
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      character.account_id, character.character_id, 1, before, stocked));
}

void start_drinking_potion(MapEntryWorld &world, MapEntryWorld::Character &character,
                           WorldTestClient &client, const nlohmann::json &pos) {
  constexpr int kDrinkSkill = 1000;
  constexpr std::uint16_t kSkillStarted = 14;
  const nlohmann::json drink{{"start_skill",
                              {{"skill_id", kDrinkSkill},
                               {"instance_id", 1},
                               {"tick", "1"},
                               {"pos", pos},
                               {"rotation", {{"yaw", 0}}}}},
                             {"use_item", {{"item_cms_id", kBusyTravelPotionItem}}}};
  client.send(world.protocol.encode(20010, drink, character.sequence++));
  const auto started = world.protocol.decode(read_until_opcode(client, kSkillStarted));
  ASSERT_EQ(started.at("message").at("start_skill").at("skill_id"), kDrinkSkill);
}

// The drink ends first; only then does the deferred travel start, without another request.
void expect_map_move_after_the_drink(MapEntryWorld &world, WorldTestClient &client,
                                     int destination_map) {
  constexpr std::uint16_t kSkillEnded = 15;
  constexpr std::uint16_t kMapMoveStart = 175;
  bool drink_ended = false;
  for (int count = 0; count < 256; ++count) {
    const auto frame = client.read();
    if (frame.opcode() == kSkillEnded) {
      drink_ended = true;
      continue;
    }
    if (frame.opcode() != kMapMoveStart) {
      continue;
    }
    EXPECT_TRUE(drink_ended) << "the map move started while the character was still drinking";
    EXPECT_EQ(world.protocol.decode(frame).at("message").at("map_id"), destination_map);
    return;
  }
  FAIL() << "the deferred travel never started";
}

TEST(NativeWorld, TravelVolumeEnteredWhileDrinkingAPotionStartsAfterTheDrinkEnds) {
  constexpr int kCaveMap = 10119;
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs the generated tutorial profile, map-9012 placements and travel profile";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  const auto travel_path = world.catalog_root.parent_path() / "travel" / "definitions.json";
  if (!std::filesystem::exists(travel_path)) {
    GTEST_SKIP() << "Run tools/build_map_travel.py first";
  }

  std::ifstream input(travel_path);
  const auto travel = nlohmann::json::parse(input);
  const auto cave_volume = std::ranges::find_if(travel.at("volumes"), [](const auto &volume) {
    return volume.at("kind") == "channelSession" &&
           volume.value("destination_map_id", 0) == kCaveMap && volume.at("world") == "ArcheWorld";
  });
  ASSERT_NE(cave_volume, travel.at("volumes").end());
  const auto &center = cave_volume->at("center_cm");
  const WorldPosition cave_mouth{9012, static_cast<int>(center.at(0).get<double>() * 10),
                                 static_cast<int>(center.at(1).get<double>() * 10),
                                 static_cast<int>(center.at(2).get<double>() * 10), 0};
  const nlohmann::json mouth_pos{{"millimeter_x", cave_mouth.x_mm},
                                 {"millimeter_y", cave_mouth.y_mm},
                                 {"millimeter_z", cave_mouth.z_mm}};

  auto character = world.add_character("BusyTravel", cave_mouth);
  stock_potions(world, character);
  auto client = std::make_unique<WorldTestClient>(world.server->port());
  world.enter(character, *client);
  start_drinking_potion(world, character, *client, mouth_pos);

  client->send(world.protocol.encode(
      20084,
      {{"tick", "1"},
       {"pos", mouth_pos},
       {"body_yaw", 0.0},
       {"trigger_id_x", cave_volume->at("trigger_id_wire").at(0)},
       {"trigger_id_y", cave_volume->at("trigger_id_wire").at(1)}},
      character.sequence++));

  expect_map_move_after_the_drink(world, *client, kCaveMap);
  client->disconnect();
}

// The same for a natural region crossing (the boundary of the zone channel travel tests).
TEST(NativeWorld, RegionCrossedWhileDrinkingAPotionCompletesAfterTheDrinkEnds) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs generated tutorial, map-9012 placements and channel boundaries";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  if (!std::filesystem::exists(world.catalog_root.parent_path() / "triggers" /
                               "ArcheWorld-level-triggers.json")) {
    GTEST_SKIP() << "Run tools/build_level_trigger_volumes.py first";
  }

  const WorldPosition boundary{9012, -292793, 3480751, -601055, 60};
  const nlohmann::json boundary_pos{{"millimeter_x", boundary.x_mm},
                                    {"millimeter_y", boundary.y_mm},
                                    {"millimeter_z", boundary.z_mm}};
  auto character = world.add_character("BusyRegion", boundary);
  stock_potions(world, character);
  auto client = std::make_unique<WorldTestClient>(world.server->port());
  world.enter(character, *client);
  start_drinking_potion(world, character, *client, boundary_pos);

  client->send(world.protocol.encode(20090, {{"pos", boundary_pos}, {"body_yaw", boundary.yaw}},
                                     character.sequence++));

  expect_map_move_after_the_drink(world, *client, 10110);
  client->disconnect();
}

// Two characters without a party each enter their own copy of the smuggling cave: neither sees
// the other inside, although both stand at the same spot of the same map.
TEST(NativeWorld, SoloCharactersEnterSeparateCopiesOfTheCave) {
  constexpr int kCaveMap = 10119;
  constexpr std::uint16_t kUnitAdded = 10;
  constexpr std::uint16_t kPong = 4;
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs the generated tutorial profile, map-9012 placements and travel profile";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  const auto travel_path = world.catalog_root.parent_path() / "travel" / "definitions.json";
  if (!std::filesystem::exists(travel_path)) {
    GTEST_SKIP() << "Run tools/build_map_travel.py first";
  }

  std::ifstream input(travel_path);
  const auto travel = nlohmann::json::parse(input);
  const nlohmann::json *cave_volume{};
  for (const auto &volume : travel.at("volumes")) {
    if (volume.at("kind") == "channelSession" && volume.value("destination_map_id", 0) == kCaveMap &&
        volume.at("world") == "ArcheWorld") {
      cave_volume = &volume;
      break;
    }
  }
  ASSERT_NE(cave_volume, nullptr);
  const auto &center = cave_volume->at("center_cm");
  const WorldPosition cave_mouth{9012, static_cast<int>(center.at(0).get<double>() * 10),
                                 static_cast<int>(center.at(1).get<double>() * 10),
                                 static_cast<int>(center.at(2).get<double>() * 10), 0};
  const nlohmann::json mouth_pos{{"millimeter_x", cave_mouth.x_mm},
                                 {"millimeter_y", cave_mouth.y_mm},
                                 {"millimeter_z", cave_mouth.z_mm}};

  // Reads until `opcode`, recording the player units announced on the way.
  const auto read_recording_units = [&](WorldTestClient &client, std::uint16_t opcode,
                                        std::set<std::string> &announced) {
    constexpr int kMaximumFrames = 1024;
    for (int read = 0; read < kMaximumFrames; ++read) {
      auto frame = client.read();
      if (frame.opcode() == kUnitAdded) {
        announced.insert(world.protocol.decode(frame).at("message").value("unit_id", ""));
      }
      if (frame.opcode() == opcode) {
        return;
      }
    }
    throw std::runtime_error("Expected world frame did not arrive");
  };

  const auto enter_cave = [&](MapEntryWorld::Character &character, WorldTestClient &client,
                              std::set<std::string> &announced) {
    client.send(world.protocol.encode(
        20084,
        {{"tick", "1"},
         {"pos", mouth_pos},
         {"body_yaw", 0.0},
         {"trigger_id_x", cave_volume->at("trigger_id_wire").at(0)},
         {"trigger_id_y", cave_volume->at("trigger_id_wire").at(1)}},
        character.sequence++));
    const auto entered = world.protocol.decode(read_until_opcode(client, 175)).at("message");
    EXPECT_EQ(entered.at("map_id"), kCaveMap);
    announced.clear();
    client.send(world.protocol.encode(20085, nlohmann::json::object(), character.sequence++));
    read_recording_units(client, 176, announced);
    client.send(world.protocol.encode(20086, nlohmann::json::object(), character.sequence++));
    read_recording_units(client, 183, announced);
  };

  auto first = world.add_character("CopyFirst", cave_mouth);
  auto second = world.add_character("CopySecond", cave_mouth);
  WorldTestClient first_client(world.server->port());
  WorldTestClient second_client(world.server->port());
  world.enter(first, first_client);
  world.enter(second, second_client);
  const auto first_unit = std::to_string(player_wire_unit_id(first.character_id));
  const auto second_unit = std::to_string(player_wire_unit_id(second.character_id));

  std::set<std::string> seen_by_first;
  std::set<std::string> seen_by_second;
  // Control: on the shared field the two characters see each other.
  first_client.send(world.protocol.encode(20003, {{"ping_cycles64", "1"}, {"tick", "1"}},
                                          first.sequence++));
  read_recording_units(first_client, kPong, seen_by_first);
  EXPECT_TRUE(seen_by_first.contains(second_unit));

  enter_cave(first, first_client, seen_by_first);
  enter_cave(second, second_client, seen_by_second);
  EXPECT_FALSE(seen_by_second.contains(first_unit));

  // A ping orders the first client's stream after the second character's arrival.
  first_client.send(world.protocol.encode(20003, {{"ping_cycles64", "2"}, {"tick", "2"}},
                                          first.sequence++));
  read_recording_units(first_client, kPong, seen_by_first);
  EXPECT_FALSE(seen_by_first.contains(second_unit));

  first_client.disconnect();
  second_client.disconnect();
}

// The scripted entrance of instance 10104 opens a door (ChangeDoodadPhase phase 2). The phase is
// sent once, saved in the character's progress, not sent again when the character walks in
// again, and comes back with the map after a reconnect into a fresh copy.
TEST(NativeWorld, EntranceDoorPhaseIsSavedAndReturnsWithTheMap) {
  constexpr int kInstanceMap = 10104;
  constexpr std::uint16_t kDoodadPhaseChanged = 194;
  constexpr std::uint16_t kPong = 4;
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs the generated tutorial profile, map-9012 placements and travel profile";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  const auto travel_path = world.catalog_root.parent_path() / "travel" / "definitions.json";
  if (!std::filesystem::exists(travel_path)) {
    GTEST_SKIP() << "Run tools/build_map_travel.py first";
  }

  std::ifstream input(travel_path);
  const auto travel = nlohmann::json::parse(input);
  const nlohmann::json *entrance{};
  std::string door_unit;
  for (const auto &volume : travel.at("volumes")) {
    if (volume.at("maps") != nlohmann::json::array({kInstanceMap}) || !volume.contains("rules")) {
      continue;
    }
    for (const auto &task : volume.at("rules").at(0).at("tasks")) {
      if (task.at("kind") == "ChangeDoodadPhase" && task.at("phase") == 2) {
        entrance = &volume;
        door_unit = task.at("unit_id");
      }
    }
  }
  ASSERT_NE(entrance, nullptr);
  const auto &center = entrance->at("center_cm");
  const WorldPosition inside{kInstanceMap, static_cast<int>(center.at(0).get<double>() * 10),
                             static_cast<int>(center.at(1).get<double>() * 10),
                             static_cast<int>(center.at(2).get<double>() * 10), 0};
  const nlohmann::json inside_pos{{"millimeter_x", inside.x_mm},
                                  {"millimeter_y", inside.y_mm},
                                  {"millimeter_z", inside.z_mm}};
  const nlohmann::json enter_volume{{"tick", "1"},
                                    {"pos", inside_pos},
                                    {"body_yaw", 0.0},
                                    {"trigger_id_x", entrance->at("trigger_id_wire").at(0)},
                                    {"trigger_id_y", entrance->at("trigger_id_wire").at(1)}};

  // Reads up to the pong of a ping sent now, returning the door phases seen on the way.
  const auto door_phases_until_pong = [&](WorldTestClient &client,
                                          MapEntryWorld::Character &character) {
    client.send(world.protocol.encode(20003, {{"ping_cycles64", "9"}, {"tick", "9"}},
                                      character.sequence++));
    std::vector<int> phases;
    constexpr int kMaximumFrames = 1024;
    for (int read = 0; read < kMaximumFrames; ++read) {
      auto frame = client.read();
      if (frame.opcode() == kDoodadPhaseChanged) {
        const auto message = world.protocol.decode(frame).at("message");
        if (message.value("unit_id", "") == door_unit) {
          phases.push_back(message.value("phase_id", 0));
        }
      }
      if (frame.opcode() == kPong) {
        return phases;
      }
    }
    throw std::runtime_error("Expected world frame did not arrive");
  };

  auto character = world.add_character("DoorOpener", inside);
  {
    WorldTestClient client(world.server->port());
    world.enter(character, client);
    client.send(world.protocol.encode(20084, enter_volume, character.sequence++));
    EXPECT_EQ(door_phases_until_pong(client, character), std::vector<int>{2});
    EXPECT_EQ(world.progress(character).doodad_phases.at(std::stoull(door_unit)), 2);

    // Walking in again leaves the open door as it is.
    client.send(world.protocol.encode(20084, enter_volume, character.sequence++));
    EXPECT_TRUE(door_phases_until_pong(client, character).empty());
    client.disconnect();
  }

  // The reconnect builds a fresh copy; the saved phase is restored into it with the map and sent
  // in the map's doodad snapshot (SectorUpdateNoti doodads or DoodadPhaseChangedNoti).
  character.sequence = kFirstSequenceAfterEntry;
  std::unique_ptr<WorldTestClient> reconnected;
  const auto entry = world.enter_when_character_is_free(character, reconnected);
  const auto door_local_id = (std::stoull(door_unit) >> 12) & 0xFFFFF;
  bool restored{};
  for (const auto &[opcode, message] : entry.messages) {
    if (opcode == kDoodadPhaseChanged && message.value("unit_id", "") == door_unit) {
      restored = restored || message.value("phase_id", 0) == 2;
    }
    for (const auto &nub : message.value("doodads", nlohmann::json::array())) {
      restored = restored || (nub.value("id", 0ULL) == door_local_id && nub.value("phase", 0) == 2);
    }
  }
  if (!restored) {
    restored = door_phases_until_pong(*reconnected, character) == std::vector<int>{2};
  }
  EXPECT_TRUE(restored);
  reconnected->disconnect();
}

TEST(NativeWorld, CliffsGateRequiresMissingHusbandAndCompletesTheCapturedHandoff) {
  const auto *profile_path = std::getenv("CHRONICLES_CLIFFS_TUTORIAL_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs the isolated Cliffs tutorial, quest, travel and NPC profiles";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  const auto travel_path = world.catalog_root.parent_path() / "travel" / "definitions.json";
  std::ifstream input(travel_path);
  const auto gate = nlohmann::json::parse(input).at("cliffs_gate");
  const auto &point = gate.at("position_mm");
  const WorldPosition at_gate{gate.at("source_map_id"), point.at(0), point.at(1), point.at(2), 0};
  const nlohmann::json request{{"target_unit_id", gate.at("unit_id")},
                               {"param", {{"interaction_id", gate.at("interaction_id")}}}};

  auto blocked = world.add_character("CliffsBlocked", at_gate);
  WorldTestClient blocked_client(world.server->port());
  world.enter(blocked, blocked_client);
  blocked_client.send(world.protocol.encode(20068, request, blocked.sequence++));
  const auto refusal = world.protocol.decode(read_until_opcode(blocked_client, 163)).at("message");
  EXPECT_EQ(refusal.at("ec"), "Interaction_ConditionsNotMatch");
  blocked_client.disconnect();

  auto player = world.add_character("CliffsReady", at_gate);
  auto before = world.progress(player);
  auto after = before;
  after.revision += 1;
  QuestProgress completed;
  completed.complete = true;
  completed.objectives[1] = 1;
  after.quests.emplace(20001051, completed);
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      player.account_id, player.character_id, 1, before, after));

  WorldTestClient client(world.server->port());
  world.enter(player, client);
  client.send(world.protocol.encode(20068, request, player.sequence++));
  const auto scene = world.protocol.decode(read_until_opcode(client, 70)).at("message");
  EXPECT_EQ(scene.at("cut_scene_id"), 1565);
  const auto accepted = world.protocol.decode(read_until_opcode(client, 163)).at("message");
  EXPECT_FALSE(accepted.contains("ec"));

  client.send(world.protocol.encode(20055, {{"cut_scene_id", 1565}}, player.sequence++));
  const auto move = world.protocol.decode(read_until_opcode(client, 175)).at("message");
  EXPECT_EQ(move.at("map_id"), 30104);
  EXPECT_EQ(move.at("spawn_point").at("pos").at("millimeter_x"),
            gate.at("destination_mm").at(0));
  client.send(world.protocol.encode(20085, nlohmann::json::object(), player.sequence++));
  EXPECT_EQ(world.protocol.decode(read_until_opcode(client, 176)).at("message").at("map_id"),
            30104);
  client.send(world.protocol.encode(20086, nlohmann::json::object(), player.sequence++));
  read_until_opcode(client, 183, 512);
  const auto saved = world.repositories.characters->characters(player.account_id, 1).front();
  ASSERT_TRUE(saved.world_position.has_value());
  EXPECT_EQ(saved.world_position->map_id, 30104);
  client.disconnect();
}

// End-to-end over the TLS world listener with the exported client catalog: a game-master
// handoff receives WorldCharBaseNoti.gm and its console commands commit and notify, while
// a player account's identical GmCmdTextReq is dropped without any state change.

// Session GM commands over TLS: replies reach the private chat handler, god mode sends the
// Invincible attribute, and "tp" completes through the native TeleportReq handshake.


// ---------------------------------------------------------------------------------------------
// Mounts and trade runs through the real world listener, with the exported catalog and the map
// 9012 placements (opt-in: CHRONICLES_CLIENT_CATALOG and CHRONICLES_NPC_PLACEMENTS_9012). A skip
// is not evidence. These are synthetic client frames, not an original-client trial.
























// Escape over the real world transport: refusal beyond EscapeCandidateMaxDistance, the
// TeleportReq/cooldown reply, the start/completion handshake, the cooldown refusal and the
// destination saved on disconnect. Opcodes: 181 TeleportReq, 49/51 status add/remove,
// 33 UnitChangeAttributeNoti.












// Sector rows of an ArcheWorld catalog with one shared field map (OriginMap/Map, build 40367e24).
void write_sector_grid_tables(TemporaryWorldCatalog &catalog) {
  const nlohmann::json origin{
      {"world", "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld"},
      {"sectorWidth", "5100"},
      {"area", R"("min":{"x":-918000,"y":-1173000,"z":-97800},)"
               R"("max":{"x":918000,"y":1275000,"z":106320})"}};
  const nlohmann::json field_map{
      {"id", std::to_string(kOverworldMapId)},
      {"world", "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld"},
      {"area", R"("min":{"x":-265200,"y":224400,"z":-97800},)"
               R"("max":{"x":15300,"y":459000,"z":106320})"}};
  // The world service requires an Item table whenever a client catalog is present.
  catalog.write_items({}, {{"OriginMap", {origin}}, {"Map", {field_map}}});
}

std::vector<std::uint32_t> sector_block(std::int32_t first_x, std::int32_t last_x,
                                        std::int32_t first_y, std::int32_t last_y) {
  std::vector<std::uint32_t> sectors;
  for (auto y = first_y; y <= last_y; ++y) {
    for (auto x = first_x; x <= last_x; ++x) {
      sectors.push_back(pack_sector_id({x, y}));
    }
  }
  return sectors;
}

// Official servers announce the 5 x 5 sector window after every map load and then each sector
// entering it; the client creates a sector's spot doodads only on that notification. The
// overworld spawn (-124200 cm, 299300 cm) lies in ArcheWorld sector (156, 289).
// Opcodes: 20001 TimeSyncReq, 175/176 MapMoveStart/CompleteNoti, 20085 MapLoadCompleteReq,
// 20087 MoveReq, 193 SectorUpdateNoti.
TEST(NativeWorld, SharedMapLoadAnnouncesTheSectorWindowAndThenEnteringSectors) {
  constexpr std::uint16_t kSectorUpdate = 193;
  constexpr int kSectorWidthMm = 51000;
  constexpr int kMaximumFramesBeforeMapStart = 64;

  TemporaryWorldCatalog catalog;
  write_sector_grid_tables(catalog);
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn}, catalog.open());
  auto &session = world.sessions.front();
  world.connect(session);

  session.client->send(world.protocol.encode(20001, nlohmann::json::object(), 0));
  bool map_started{};
  for (int read = 0; read < kMaximumFramesBeforeMapStart && !map_started; ++read) {
    map_started = session.client->read().opcode() == 175;
  }
  ASSERT_TRUE(map_started);

  session.client->send(world.protocol.encode(20085, nlohmann::json::object(), 1));
  const auto loaded = world.read_until_pong(session, 2);

  std::vector<std::uint32_t> announced;
  bool map_completed{};
  for (const auto &frame : loaded) {
    if (frame.opcode() == 176) {
      map_completed = true;
    }
    if (frame.opcode() == kSectorUpdate) {
      EXPECT_TRUE(map_completed) << "SectorUpdateNoti preceded MapMoveCompleteNoti";
      const auto message = world.protocol.decode(frame).at("message");
      announced.push_back(message.at("sector_id").get<std::uint32_t>());
    }
  }
  EXPECT_EQ(announced, sector_block(154, 158, 287, 291));

  // One sector east: only the new column is announced.
  const nlohmann::json east{{"mode", "MoveMode_Normal"},
                            {"pos",
                             {{"millimeter_x", kOverworldSpawn.x_mm + kSectorWidthMm},
                              {"millimeter_y", kOverworldSpawn.y_mm},
                              {"millimeter_z", kOverworldSpawn.z_mm}}}};
  session.client->send(world.protocol.encode(20087, east, 3));
  announced.clear();
  for (const auto &frame : world.read_until_pong(session, 4)) {
    if (frame.opcode() == kSectorUpdate) {
      announced.push_back(
          world.protocol.decode(frame).at("message").at("sector_id").get<std::uint32_t>());
    }
  }
  EXPECT_EQ(announced, sector_block(159, 159, 287, 291));

  session.client->disconnect();
}

TEST(NativeWorld, ContextualGuideIsSentAfterWorldEntryOverTls) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  const auto guide_path = catalog_path.parent_path() / "contents-guides" / "definitions.json";
  if (!std::filesystem::exists(guide_path)) {
    GTEST_SKIP() << "Generate the contents guide profile first";
  }
  const WorldPosition position{9012, -1376964, 3151307, -620012, 0};
  NpcPlacementSet empty_population;
  empty_population.map_id = position.map_id;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  OverworldSessions world(population, {position}, catalog, catalog_path, {},
                          CharacterStats::load(*catalog));
  world.enter(world.sessions.front());
  VehicleClient player(world, world.sessions.front(), 3);
  const auto started = player.send(20086, nlohmann::json::object());
  const auto guides = frames_of(started, 105);
  EXPECT_TRUE(std::ranges::any_of(guides, [](const nlohmann::json &message) {
    return message.value("contents_guide_cms_id", 0) == 93 &&
           message.value("start_trigger", false);
  }));
}

TEST(NativeWorld, ContextualGuideIsSentInTutorialOverTls) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  const auto root = catalog_path.parent_path();
  const auto tutorial_path = root / "tutorial" / "definitions.json";
  const auto guide_path = root / "contents-guides" / "definitions.json";
  if (!std::filesystem::exists(tutorial_path) || !std::filesystem::exists(guide_path)) {
    GTEST_SKIP() << "Generate tutorial and contents guide profiles first";
  }
  const WorldPosition position{8000, -806197, 715381, -697350, 0};
  NpcPlacementSet empty_population;
  empty_population.map_id = position.map_id;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  OverworldSessions world(population, {position}, catalog, catalog_path,
                          TutorialData::load(tutorial_path), CharacterStats::load(*catalog));
  world.enter(world.sessions.front());
  VehicleClient player(world, world.sessions.front(), 3);
  const auto started = player.send(20086, nlohmann::json::object());
  const auto guides = frames_of(started, 105);
  EXPECT_TRUE(std::ranges::any_of(guides, [](const nlohmann::json &message) {
    return message.value("contents_guide_cms_id", 0) == 1000 &&
           message.value("start_trigger", false);
  }));
}

TEST(NativeWorld, TutorialChestRewardStartsEquipmentGuideOverTls) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  const auto root = catalog_path.parent_path();
  const auto tutorial_path = root / "tutorial" / "definitions.json";
  const auto guide_path = root / "contents-guides" / "definitions.json";
  if (!std::filesystem::exists(tutorial_path) || !std::filesystem::exists(guide_path)) {
    GTEST_SKIP() << "Generate tutorial and contents guide profiles first";
  }

  const WorldPosition position{8000, -1311996, 670838, -698869, 0};
  NpcPlacementSet empty_population;
  empty_population.map_id = position.map_id;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  OverworldSessions world(population, {position}, catalog, catalog_path,
                          TutorialData::load(tutorial_path), CharacterStats::load(*catalog));
  world.enter(world.sessions.front());
  VehicleClient player(world, world.sessions.front(), 3);
  player.send(20086, nlohmann::json::object());

  const nlohmann::json interaction{
      {"target_unit_id", "281492156583942"},
      {"interaction_anim_id", 100},
      {"param", {{"interaction", {{"interaction_id", 300}}}}},
      {"pos", {{"millimeter_x", position.x_mm},
               {"millimeter_y", position.y_mm},
               {"millimeter_z", position.z_mm}}}};
  const auto started = player.send(20069, interaction);
  ASSERT_FALSE(frames_of(started, 166).empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(3000));
  const auto executed = player.send(20070, {{"interaction_anim_id", 100}});
  ASSERT_FALSE(frames_of(executed, 169).empty());
  const auto guides = frames_of(executed, 105);
  EXPECT_TRUE(std::ranges::any_of(guides, [](const nlohmann::json &message) {
    return message.value("contents_guide_cms_id", 0) == 3 &&
           message.value("start_trigger", false);
  }));
}












// Emotes and selfie mode over real world sockets: refused emotes reach only the requester, a GM
// grant adds a non-Basic emote and persists it, an accepted emote reaches the nearby player,
// and a player who arrives later still sees the selfie pose.


// Leaderboards, raid channels and channel events over TLS with the exported client catalog:
// an empty board answers like the captured RankingListRes, an operator-timed boss attempt is
// stored and listed as the character's own entry, a raid application is acknowledged and
// cancelled, a GM raid channel forms with the character as its only member, and a tribute
// count far from every altar is refused.


// --max-world-connections validation. The limit itself is a server policy constant
// (the official ceiling is not captured); only the parsing/range contract is pinned.
TEST(NativeWorld, WorldConnectionLimitParseAcceptsInRange) {
  EXPECT_EQ(parse_world_connection_limit("1"), 1u);
  EXPECT_EQ(parse_world_connection_limit("128"), 128u);
  EXPECT_EQ(parse_world_connection_limit(std::to_string(kMaximumWorldCapacity)),
            kMaximumWorldCapacity);
}

TEST(NativeWorld, WorldConnectionLimitParseRejectsOutOfRange) {
  EXPECT_THROW(parse_world_connection_limit("0"), std::invalid_argument);
  EXPECT_THROW(parse_world_connection_limit("-1"), std::invalid_argument);
  EXPECT_THROW(parse_world_connection_limit("-5"), std::invalid_argument);
  EXPECT_THROW(parse_world_connection_limit("abc"), std::invalid_argument);
  EXPECT_THROW(parse_world_connection_limit("128x"), std::invalid_argument);
  EXPECT_THROW(parse_world_connection_limit(""), std::invalid_argument);
  EXPECT_THROW(parse_world_connection_limit("1.5"), std::invalid_argument);
  EXPECT_THROW(parse_world_connection_limit(
                   std::to_string(static_cast<unsigned long long>(kMaximumWorldCapacity) + 1)),
               std::invalid_argument);
}

TEST(NativeWorld, StartWorldRefusesOutOfRangeConnectionLimit) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  TemporaryWorldCertificate certificate;
  asio::io_context io;
  asio::thread_pool workers(1);

  EXPECT_THROW(
      start_native_world(io, workers, auth,
                         tcp::endpoint(asio::ip::address_v4::loopback(), 0),
                         CHRONICLES_PROTOCOL_DIR, certificate.certificate(),
                         certificate.private_key(), std::chrono::milliseconds::zero(), false,
                         false, false, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, 0),
      std::invalid_argument);
  EXPECT_THROW(start_native_world(io, workers, auth,
                                  tcp::endpoint(asio::ip::address_v4::loopback(), 0),
                                  CHRONICLES_PROTOCOL_DIR, certificate.certificate(),
                                  certificate.private_key(), std::chrono::milliseconds::zero(),
                                  false, false, false, {}, {}, {}, {}, {}, {}, {}, {}, {},
                                  {}, kMaximumWorldCapacity + 1u),
               std::invalid_argument);
  io.stop();
  workers.join();
}

// Accept-path enforcement: with maximum_connections=1 the listener closes the second
// socket at accept, and accepts again once the first session disconnects. The refused
// path raises EOF/reset on a plain TCP read.
TEST(NativeWorld, WorldConnectionLimitRefusesAboveCapAndAcceptsBelow) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  TemporaryWorldCertificate certificate;

  asio::thread_pool workers(2);
  // Pending I/O handlers can own sessions with strands backed by the worker pool.
  asio::io_context io;
  auto listener = start_native_world(
      io, workers, auth, tcp::endpoint(asio::ip::address_v4::loopback(), 0),
      CHRONICLES_PROTOCOL_DIR, certificate.certificate(), certificate.private_key(),
      std::chrono::milliseconds::zero(), false, false, false, {}, {}, {}, {}, {}, {}, {},
      {}, {}, {}, 1);
  std::thread io_thread([&io] { io.run(); });

  {
    // Below the cap: accepted and able to complete a TLS handshake.
    WorldTestClient first(listener->port());

    // At the cap: the next connection is closed without a session.
    tcp::socket second(io);
    second.connect(tcp::endpoint(asio::ip::address_v4::loopback(), listener->port()));
    bool refused = false;
    for (int attempt = 0; attempt < 200 && !refused; ++attempt) {
      beast::error_code read_error;
      char byte;
      second.non_blocking(true);
      second.read_some(asio::buffer(&byte, 1), read_error);
      refused =
          read_error == asio::error::eof || read_error == asio::error::connection_reset;
      if (!refused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    EXPECT_TRUE(refused);

    first.disconnect();
  }

  // After the slot frees, a new connection completes the handshake again.
  bool accepted = false;
  for (int attempt = 0; attempt < 100 && !accepted; ++attempt) {
    try {
      WorldTestClient retry(listener->port());
      retry.disconnect();
      accepted = true;
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  EXPECT_TRUE(accepted);

  listener->stop();
  io.stop();
  io_thread.join();
  workers.join();
}

TEST(NativeWorld, DrainedShutdownClosesTlsSessionsBeforeStoppingIo) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  TemporaryWorldCertificate certificate;
  asio::thread_pool workers(2);
  asio::io_context io;
  auto listener = start_native_world(
      io, workers, auth, tcp::endpoint(asio::ip::address_v4::loopback(), 0),
      CHRONICLES_PROTOCOL_DIR, certificate.certificate(), certificate.private_key(),
      std::chrono::milliseconds::zero(), false, false, false, {}, {}, {}, {}, {}, {}, {},
      {}, {}, {}, 8);
  std::thread io_thread([&io] { io.run(); });
  WorldTestClient client(listener->port());
  std::promise<void> drained;
  auto completion = drained.get_future();

  listener->shutdown_when_drained([&] { drained.set_value(); });
  EXPECT_EQ(completion.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  client.disconnect();
  io.stop();
  io_thread.join();
  workers.join();
}

TEST(NativeWorld, RestartedIoRunsAnAlreadyQueuedShutdownCompletion) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  TemporaryWorldCertificate certificate;
  asio::thread_pool workers(1);
  asio::io_context io;
  auto listener = start_native_world(
      io, workers, auth, tcp::endpoint(asio::ip::address_v4::loopback(), 0),
      CHRONICLES_PROTOCOL_DIR, certificate.certificate(), certificate.private_key(),
      std::chrono::milliseconds::zero(), false, false, false, {}, {}, {}, {}, {}, {}, {},
      {}, {}, {}, 8);
  std::promise<void> drained;
  auto completion = drained.get_future();
  io.stop();
  listener->shutdown_when_drained([&] { drained.set_value(); });

  for (int attempt = 0; attempt < 500 && !listener->shutdown_completion_queued(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(listener->shutdown_completion_queued());
  EXPECT_EQ(completion.wait_for(std::chrono::milliseconds::zero()), std::future_status::timeout);

  io.restart();
  io.poll();
  EXPECT_EQ(completion.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  io.stop();
  workers.join();
  listener.reset();
}

TEST(NativeWorld, StalledIoKeepsShutdownClosePostsBounded) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  TemporaryWorldCertificate certificate;
  asio::thread_pool workers(2);
  asio::io_context io;
  auto listener = start_native_world(
      io, workers, auth, tcp::endpoint(asio::ip::address_v4::loopback(), 0),
      CHRONICLES_PROTOCOL_DIR, certificate.certificate(), certificate.private_key(),
      std::chrono::milliseconds::zero(), false, false, false, {}, {}, {}, {}, {}, {}, {},
      {}, {}, {}, 8);
  std::thread io_thread([&io] { io.run(); });
  WorldTestClient client(listener->port());
  std::promise<void> entered;
  std::promise<void> release;
  auto entered_future = entered.get_future();
  auto release_future = release.get_future();
  asio::post(io, [&] {
    entered.set_value();
    release_future.wait();
  });
  entered_future.wait();

  std::promise<void> drained;
  auto completion = drained.get_future();
  listener->shutdown_when_drained([&] { drained.set_value(); });
  for (int attempt = 0; attempt < 500 && listener->shutdown_close_post_attempts() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  EXPECT_EQ(listener->shutdown_close_post_attempts(), 1U);
  EXPECT_EQ(completion.wait_for(std::chrono::milliseconds::zero()), std::future_status::timeout);

  release.set_value();
  EXPECT_EQ(completion.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  client.disconnect();
  io.stop();
  io_thread.join();
  workers.join();
}


} // namespace

} // namespace chronicles
