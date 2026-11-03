#include "persistence/character_repositories.hpp"
#include "world/features/gathering/gathering.hpp"
#include "world/features/trade/tests/pausing_region_checkpoint_store.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;

constexpr int kCliffsMap = 30104;
constexpr int kEliteBoss = 5001510;
constexpr int kCliffsQuest = 20001054;
constexpr int kPartyUnlockQuest = 90100011;
constexpr int kEliteObjective = 6;

enum class BossFault {
  none,
  settlement_acknowledgement,
  publication_acknowledgement,
};

struct BossPlayer {
  std::int64_t account_id{};
  std::int64_t character_id{};
  std::string access_token;
  std::unique_ptr<WorldTestClient> client;
  std::uint32_t sequence{};
  std::vector<DecodedWorldFrame> entry_frames;
  std::vector<DecodedWorldFrame> start_frames;
};

class BossPublicationRelease {
public:
  explicit BossPublicationRelease(std::shared_ptr<PausingRegionCheckpointStore> store)
      : store_(std::move(store)) {
  }

  ~BossPublicationRelease() {
    store_->allow_boss_publication();
  }

private:
  std::shared_ptr<PausingRegionCheckpointStore> store_;
};

std::vector<DecodedWorldFrame> exchange(BossPlayer &player, Protocol &protocol,
                                         std::uint16_t opcode,
                                         const nlohmann::json &message) {
  player.client->send(protocol.encode(opcode, message, player.sequence++));
  if (opcode != 20003) {
    player.client->send(protocol.encode(20003, nlohmann::json::object(), player.sequence++));
  }

  std::vector<DecodedWorldFrame> output;
  for (std::size_t index = 0; index < 512; ++index) {
    const auto frame = player.client->read();
    if (frame.opcode() == kPongOpcode) {
      return output;
    }
    output.push_back({frame.opcode(), protocol.decode(frame).at("message")});
  }
  throw std::runtime_error("World test did not receive its pong barrier");
}

void read_through(BossPlayer &player, Protocol &protocol, std::uint16_t opcode) {
  for (int count = 0; count < 64; ++count) {
    const auto frame = player.client->read();
    if (frame.opcode() == 193 || frame.opcode() == 194) {
      player.entry_frames.push_back({frame.opcode(), protocol.decode(frame).at("message")});
    }
    if (frame.opcode() == opcode) {
      return;
    }
  }
  throw std::runtime_error("Expected world entry frame was not received");
}

void enter_world(BossPlayer &player, AuthService &auth, unsigned short port,
                 Protocol &protocol) {
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(player.character_id);
  const auto selected = auth.select(player.access_token, select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  player.client = std::make_unique<WorldTestClient>(port);
  player.client->send_key(selected.packet_key());
  ASSERT_EQ(player.client->read().opcode(), 5);
  player.client->send(protocol.encode(20001, nlohmann::json::object(), 0));
  read_through(player, protocol, 175);
  for (const auto opcode : {1, 130, 126}) {
    read_through(player, protocol, static_cast<std::uint16_t>(opcode));
  }

  player.client->send(protocol.encode(20085, nlohmann::json::object(), 1));
  read_through(player, protocol, 176);
  for (const auto opcode : {78, 78, 33, 115, 33}) {
    read_through(player, protocol, static_cast<std::uint16_t>(opcode));
  }
  player.sequence = 2;
  player.start_frames = exchange(player, protocol, 20086, nlohmann::json::object());
}

std::string find_boss(const std::vector<DecodedWorldFrame> &frames) {
  for (const auto &frame : frames_of(frames, kUnitAddOpcode)) {
    if (frame.at("nub").contains("npc") &&
        frame.at("nub").at("npc").at("npc_cms_id") == kEliteBoss) {
      return frame.at("unit_id").get<std::string>();
    }
  }
  return {};
}

bool has_elite_barrier_phase(const std::vector<DecodedWorldFrame> &frames) {
  const auto target = std::to_string(doodad_unit_id(170 | (289 << 14), 5));
  for (const auto &frame : frames_of(frames, 194)) {
    if (frame.at("unit_id") == target && frame.at("phase_id") == 2) {
      return true;
    }
  }
  return false;
}

bool has_elite_barrier_snapshot(const std::vector<DecodedWorldFrame> &frames) {
  constexpr std::uint32_t sector = 170 | (289 << 14);
  for (const auto &frame : frames_of(frames, 193)) {
    if (frame.value("sector_id", 0U) != sector) {
      continue;
    }
    for (const auto &doodad : frame.value("doodads", nlohmann::json::array())) {
      if (doodad.value("id", 0U) == 5 && doodad.value("phase", 0) == 2) {
        return true;
      }
    }
  }
  return has_elite_barrier_phase(frames);
}

void verify_shared_cliffs_publication(BossFault fault, bool enter_observer = false,
                                      const char *crash_signal = nullptr,
                                      bool test_shutdown = false) {
  const char *database_url = std::getenv("CHRONICLES_BOSS_E2E_DATABASE_URL");
  const char *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  const char *proxy_url = std::getenv("CHRONICLES_BOSS_E2E_PROXY_URL");
  if (!database_url || !catalog_environment || (fault != BossFault::none && !proxy_url)) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL boss harness and verified profile";
  }
  {
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "boss_publication_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }

  const std::filesystem::path catalog_path(catalog_environment);
  std::ifstream input(catalog_path.parent_path() / "world-mobs" / "definitions.json");
  ASSERT_TRUE(input);
  nlohmann::json mobs;
  input >> mobs;
  const auto map = std::ranges::find_if(mobs.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == kCliffsMap;
  });
  ASSERT_NE(map, mobs.at("maps").end());
  const auto spawn = std::ranges::find_if(map->at("spawns"), [](const auto &entry) {
    return entry.at("cms_id") == kEliteBoss;
  });
  ASSERT_NE(spawn, map->at("spawns").end());
  const auto &point = spawn->at("position");
  const WorldPosition position{kCliffsMap, point.at(0), point.at(1), point.at(2), 0};

  auto repositories = make_postgres_character_repositories(database_url);
  auto forwarding = std::make_shared<PausingRegionCheckpointStore>(repositories.character_state);
  auto serving_repositories = repositories;
  serving_repositories.character_state = forwarding;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(serving_repositories, config);
  const auto epoch = auth.acquire_monster_reward_owner_epoch();

  std::vector<BossPlayer> players(enter_observer ? 3 : 2);
  for (std::size_t index = 0; index < players.size(); ++index) {
    const auto suffix = std::to_string(index);
    const auto account_name = "boss_test_" + suffix;
    ASSERT_TRUE(auth.create_account(account_name, "test-world-password-123"));
    if (index == 0) {
      ASSERT_TRUE(auth.set_account_role(account_name, AccountRole::game_master));
    }
    const auto login = auth.login(account_name, "test-world-password-123");
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(1);
    ASSERT_EQ(auth.enter(login.access_token(), enter).error_code(), x5::ec::Success);

    x5::pb::ws::front::CharCreateReq create;
    create.set_slot(0);
    create.set_char_name("BossTest" + suffix);
    create.set_char_class(x5::cms::EX5CharacterClass_Fight);
    const auto created = auth.create(login.access_token(), create);
    ASSERT_EQ(created.ec(), x5::ec::Success);
    auto &player = players[index];
    player.account_id = login.account_id();
    player.character_id = created.char_().char_id();
    player.access_token = login.access_token();
    ASSERT_TRUE(repositories.characters->save_world_position(player.account_id,
                                                              player.character_id, 1, position));

    const auto before = repositories.character_state->load_world_progress(
        player.account_id, player.character_id, 1);
    auto after = before;
    ++after.revision;
    after.quests[kPartyUnlockQuest].complete = true;
    auto &quest = after.quests[kCliffsQuest];
    for (int objective = 1; objective <= 5; ++objective) {
      quest.objectives[objective] = 1;
    }
    ASSERT_TRUE(repositories.character_state->save_world_progress(
        player.account_id, player.character_id, 1, before, after));
  }

  NpcPlacementSet empty_population;
  empty_population.map_id = kCliffsMap;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  auto gathering = GatheringData::load(catalog_path.parent_path() / "gathering" /
                                        "definitions.json");
  TemporaryWorldCertificate certificate;
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  TwoClientWorldServer server(auth, certificate, {}, CharacterStats::load(*catalog), catalog,
                              catalog_path, population, {}, {}, {}, gathering);
  BossPublicationRelease release_publication(forwarding);
  for (std::size_t index = 0; index < 2; ++index) {
    enter_world(players[index], auth, server.port(), protocol);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
  }

  const auto leader_id = std::to_string(players[0].character_id);
  const auto member_id = std::to_string(players[1].character_id);
  exchange(players[0], protocol, 20097, {{"target_char_id", member_id}});
  std::vector<nlohmann::json> invitations;
  for (int attempt = 0; attempt < 50 && invitations.empty(); ++attempt) {
    invitations = frames_of(exchange(players[1], protocol, 20003, nlohmann::json::object()), 188);
    if (invitations.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ASSERT_EQ(invitations.size(), 1U);
  const auto accepted = exchange(players[1], protocol, 20098,
                                 {{"leader_char_id", leader_id},
                                  {"target_char_id", member_id},
                                  {"party_id", invitations.front().at("party_id")}});
  ASSERT_FALSE(frames_of(accepted, 184).empty());

  std::string boss = find_boss(players[0].start_frames);
  for (int attempt = 0; attempt < 50 && boss.empty(); ++attempt) {
    boss = find_boss(exchange(players[0], protocol, 20003, nlohmann::json::object()));
    if (boss.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ASSERT_FALSE(boss.empty());

  std::shared_ptr<CharacterStateRepository> faulted_state;
  if (fault != BossFault::none) {
    auto faulted = make_postgres_character_repositories(proxy_url);
    faulted.character_state->adopt_monster_reward_owner_epoch(epoch);
    faulted_state = std::move(faulted.character_state);
    if (fault == BossFault::settlement_acknowledgement) {
      forwarding->arm_boss_settlement_proxy(faulted_state);
    }
  }
  forwarding->refuse_boss_publication();
  const auto defeated = exchange(players[0], protocol, 20009,
                                  {{"text", "monster_kill " + boss}});
  ASSERT_FALSE(frames_of(defeated, 23).empty());

  for (int attempt = 0; attempt < 250 && !forwarding->saw_refused_boss_publication(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_TRUE(forwarding->saw_refused_boss_publication());
  for (const auto &player : players) {
    const auto progress = repositories.character_state->load_world_progress(
        player.account_id, player.character_id, 1);
    ASSERT_TRUE(progress.quests.contains(kCliffsQuest));
    EXPECT_FALSE(progress.quests.at(kCliffsQuest).objectives.contains(kEliteObjective));
  }
  {
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto events = transaction.exec(
        "SELECT boss_script_applied,boss_script_published "
        "FROM emulator_monster_reward_events WHERE boss_script IS NOT NULL");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(events[0][0].as<bool>());
    EXPECT_FALSE(events[0][1].as<bool>());
  }
  if (enter_observer) {
    auto &observer = players[2];
    enter_world(observer, auth, server.port(), protocol);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    const bool saw_barrier = has_elite_barrier_snapshot(observer.entry_frames) ||
                             has_elite_barrier_snapshot(observer.start_frames);
    EXPECT_TRUE(saw_barrier);
    const auto progress = repositories.character_state->load_world_progress(
        observer.account_id, observer.character_id, 1);
    EXPECT_FALSE(progress.quests.at(kCliffsQuest).objectives.contains(kEliteObjective));
  }
  if (crash_signal) {
    std::ofstream signal(crash_signal);
    ASSERT_TRUE(signal);
    signal << "barrier installed, publication withheld\n";
    signal.close();
    ASSERT_TRUE(signal);
    // The harness terminates this disposable process after observing the flushed signal.
    std::this_thread::sleep_for(std::chrono::seconds(120));
    FAIL() << "Crash harness did not terminate the test process";
  }
  if (test_shutdown) {
    auto completion = std::make_shared<std::promise<void>>();
    auto completed = completion->get_future();
    server.listener().shutdown_when_drained([completion] {
      completion->set_value();
    });
    EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(750)), std::future_status::timeout);
    forwarding->allow_boss_publication();
    ASSERT_EQ(completed.wait_for(std::chrono::seconds(20)), std::future_status::ready);

    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto event = transaction.exec1(
        "SELECT ready,boss_script_applied,boss_script_published "
        "FROM emulator_monster_reward_events WHERE boss_script IS NOT NULL");
    EXPECT_TRUE(event[0].as<bool>());
    EXPECT_TRUE(event[1].as<bool>());
    EXPECT_TRUE(event[2].as<bool>());
    const auto receipts = transaction.exec1("SELECT count(*) FROM emulator_monster_reward_receipts");
    EXPECT_EQ(receipts[0].as<int>(), 2);
    return;
  }
  if (fault == BossFault::publication_acknowledgement) {
    forwarding->arm_boss_publication_proxy(faulted_state);
  }
  forwarding->allow_boss_publication();

  bool actor_saw_phase = has_elite_barrier_phase(defeated);
  bool member_saw_phase = false;
  for (int attempt = 0; attempt < 200; ++attempt) {
    const auto actor_updates = exchange(players[0], protocol, 20003, nlohmann::json::object());
    actor_saw_phase = actor_saw_phase || has_elite_barrier_phase(actor_updates);
    const auto updates = exchange(players[1], protocol, 20003, nlohmann::json::object());
    member_saw_phase = member_saw_phase || has_elite_barrier_phase(updates);
    const auto leader = repositories.character_state->load_world_progress(
        players[0].account_id, players[0].character_id, 1);
    const auto member = repositories.character_state->load_world_progress(
        players[1].account_id, players[1].character_id, 1);
    if (actor_saw_phase && member_saw_phase &&
        leader.quests.at(kCliffsQuest).objectives.contains(kEliteObjective) &&
        member.quests.at(kCliffsQuest).objectives.contains(kEliteObjective)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_TRUE(actor_saw_phase);
  EXPECT_TRUE(member_saw_phase);
  for (std::size_t index = 0; index < 2; ++index) {
    const auto &player = players[index];
    const auto progress = repositories.character_state->load_world_progress(
        player.account_id, player.character_id, 1);
    ASSERT_TRUE(progress.quests.contains(kCliffsQuest));
    ASSERT_TRUE(progress.quests.at(kCliffsQuest).objectives.contains(kEliteObjective));
    EXPECT_EQ(progress.quests.at(kCliffsQuest).objectives.at(kEliteObjective), 1);
    EXPECT_TRUE(repositories.character_state
                    ->pending_monster_rewards(player.account_id, player.character_id, 1)
                    .empty());
  }
  {
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto events = transaction.exec(
        "SELECT ready,boss_script_applied,boss_script_published "
        "FROM emulator_monster_reward_events WHERE boss_script IS NOT NULL");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(events[0][0].as<bool>());
    EXPECT_TRUE(events[0][1].as<bool>());
    EXPECT_TRUE(events[0][2].as<bool>());
    const auto count = transaction.exec1(
        "SELECT count(*) FROM emulator_monster_reward_receipts WHERE processed");
    EXPECT_EQ(count[0].as<int>(), 2);
  }
}

TEST(NativeBossPublication, SharedCliffsPartyCreditsAfterLivePublication) {
  verify_shared_cliffs_publication(BossFault::none);
}

TEST(NativeBossPublication, SharedCliffsRecoversLostSettlementCommitAcknowledgement) {
  verify_shared_cliffs_publication(BossFault::settlement_acknowledgement);
}

TEST(NativeBossPublication, SharedCliffsRecoversLostPublicationCommitAcknowledgement) {
  verify_shared_cliffs_publication(BossFault::publication_acknowledgement);
}

TEST(NativeBossPublication, SharedCliffsObserverEntersAfterBarrierInstall) {
  verify_shared_cliffs_publication(BossFault::none, true);
}

TEST(NativeBossPublication, ShutdownWaitsForBossPublication) {
  verify_shared_cliffs_publication(BossFault::none, false, nullptr, true);
}

TEST(NativeBossPublication, SyntheticPrivateCliffsCopiesIsolateBossProgression) {
  const char *database_url = std::getenv("CHRONICLES_BOSS_E2E_DATABASE_URL");
  const char *catalog_environment = std::getenv("CHRONICLES_BOSS_PRIVATE_CATALOG");
  if (!database_url || !catalog_environment) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL private-copy fixture";
  }
  {
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "boss_publication_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }

  const std::filesystem::path catalog_path(catalog_environment);
  std::ifstream mob_input(catalog_path.parent_path() / "world-mobs" / "definitions.json");
  ASSERT_TRUE(mob_input);
  nlohmann::json mobs;
  mob_input >> mobs;
  const auto map = std::ranges::find_if(mobs.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == kCliffsMap;
  });
  ASSERT_NE(map, mobs.at("maps").end());
  const auto spawn = std::ranges::find_if(map->at("spawns"), [](const auto &entry) {
    return entry.at("cms_id") == kEliteBoss;
  });
  ASSERT_NE(spawn, map->at("spawns").end());
  const auto &point = spawn->at("position");
  const WorldPosition field{9012, point.at(0), point.at(1), point.at(2), 0};
  const WorldPosition cliffs{kCliffsMap, point.at(0), point.at(1), point.at(2), 0};

  auto repositories = make_postgres_character_repositories(database_url);
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories, config);
  auth.acquire_monster_reward_owner_epoch();
  std::vector<BossPlayer> players(3);
  for (std::size_t index = 0; index < players.size(); ++index) {
    const auto suffix = std::to_string(index);
    const auto account = "private_boss_" + suffix;
    ASSERT_TRUE(auth.create_account(account, "test-world-password-123"));
    ASSERT_TRUE(auth.set_account_role(account, AccountRole::game_master));
    const auto login = auth.login(account, "test-world-password-123");
    x5::pb::authwebsocket::WorldEnterReq realm;
    realm.set_world_id(1);
    ASSERT_EQ(auth.enter(login.access_token(), realm).error_code(), x5::ec::Success);
    x5::pb::ws::front::CharCreateReq create;
    create.set_slot(0);
    create.set_char_name("PrivateBoss" + suffix);
    create.set_char_class(x5::cms::EX5CharacterClass_Fight);
    const auto created = auth.create(login.access_token(), create);
    ASSERT_EQ(created.ec(), x5::ec::Success);
    auto &player = players[index];
    player.account_id = login.account_id();
    player.character_id = created.char_().char_id();
    player.access_token = login.access_token();
    ASSERT_TRUE(repositories.characters->save_world_position(
        player.account_id, player.character_id, 1, index < 2 ? field : cliffs));
    const auto before = repositories.character_state->load_world_progress(
        player.account_id, player.character_id, 1);
    auto after = before;
    ++after.revision;
    after.quests[kPartyUnlockQuest].complete = true;
    for (int objective = 1; objective <= 5; ++objective) {
      after.quests[kCliffsQuest].objectives[objective] = 1;
    }
    ASSERT_TRUE(repositories.character_state->save_world_progress(
        player.account_id, player.character_id, 1, before, after));
  }

  NpcPlacementSet empty_population;
  empty_population.map_id = 9012;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  auto gathering = GatheringData::load(catalog_path.parent_path() / "gathering" /
                                      "definitions.json");
  TemporaryWorldCertificate certificate;
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  TwoClientWorldServer server(auth, certificate, {}, CharacterStats::load(*catalog), catalog,
                              catalog_path, population, {}, {}, {}, gathering);
  for (auto &player : players) {
    enter_world(player, auth, server.port(), protocol);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
  }
  const auto outsider_boss = find_boss(players[2].start_frames);
  ASSERT_FALSE(outsider_boss.empty());
  EXPECT_FALSE(has_elite_barrier_snapshot(players[2].entry_frames));
  EXPECT_FALSE(has_elite_barrier_snapshot(players[2].start_frames));

  const auto join_party = [&](std::size_t index) {
    const auto leader_id = std::to_string(players[0].character_id);
    const auto target_id = std::to_string(players[index].character_id);
    exchange(players[0], protocol, 20097, {{"target_char_id", target_id}});
    std::vector<nlohmann::json> invitations;
    for (int attempt = 0; attempt < 50 && invitations.empty(); ++attempt) {
      invitations = frames_of(exchange(players[index], protocol, 20003,
                                        nlohmann::json::object()), 188);
      if (invitations.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    ASSERT_EQ(invitations.size(), 1U);
    const auto accepted = exchange(players[index], protocol, 20098,
                                   {{"leader_char_id", leader_id},
                                    {"target_char_id", target_id},
                                    {"party_id", invitations.front().at("party_id")}});
    ASSERT_FALSE(frames_of(accepted, 184).empty());
  };
  join_party(1);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());

  const auto enter_cliffs = [&](BossPlayer &player) {
    const auto moved = exchange(player, protocol, 20009, {{"text", "tp_map 30104"}});
    EXPECT_EQ(frames_of(moved, 175).size(), 1U);
    auto loaded = exchange(player, protocol, 20085, nlohmann::json::object());
    EXPECT_EQ(frames_of(loaded, 176).size(), 1U);
    auto playing = exchange(player, protocol, 20086, nlohmann::json::object());
    loaded.insert(loaded.end(), std::make_move_iterator(playing.begin()),
                  std::make_move_iterator(playing.end()));
    return loaded;
  };
  const auto leader_entry = enter_cliffs(players[0]);
  const auto member_entry = enter_cliffs(players[1]);
  const auto leader_boss = find_boss(leader_entry);
  ASSERT_FALSE(leader_boss.empty());
  ASSERT_EQ(find_boss(member_entry), leader_boss);
  ASSERT_NE(outsider_boss, leader_boss);
  join_party(2);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());

  const auto defeated = exchange(players[0], protocol, 20009,
                                  {{"text", "monster_kill " + leader_boss}});
  ASSERT_FALSE(frames_of(defeated, 23).empty());
  bool leader_phase = has_elite_barrier_phase(defeated);
  bool member_phase = false;
  bool outsider_phase = false;
  for (int attempt = 0; attempt < 200; ++attempt) {
    leader_phase = leader_phase || has_elite_barrier_phase(
        exchange(players[0], protocol, 20003, nlohmann::json::object()));
    member_phase = member_phase || has_elite_barrier_phase(
        exchange(players[1], protocol, 20003, nlohmann::json::object()));
    outsider_phase = outsider_phase || has_elite_barrier_phase(
        exchange(players[2], protocol, 20003, nlohmann::json::object()));
    const auto leader = repositories.character_state->load_world_progress(
        players[0].account_id, players[0].character_id, 1);
    const auto member = repositories.character_state->load_world_progress(
        players[1].account_id, players[1].character_id, 1);
    if (leader_phase && member_phase &&
        leader.quests.at(kCliffsQuest).objectives.contains(kEliteObjective) &&
        member.quests.at(kCliffsQuest).objectives.contains(kEliteObjective)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_TRUE(leader_phase);
  EXPECT_TRUE(member_phase);
  EXPECT_FALSE(outsider_phase);
  for (std::size_t index = 0; index < players.size(); ++index) {
    const auto progress = repositories.character_state->load_world_progress(
        players[index].account_id, players[index].character_id, 1);
    const auto &objectives = progress.quests.at(kCliffsQuest).objectives;
    if (index < 2) {
      ASSERT_TRUE(objectives.contains(kEliteObjective));
      EXPECT_EQ(objectives.at(kEliteObjective), 1);
    } else {
      EXPECT_FALSE(objectives.contains(kEliteObjective));
    }
  }
  std::string first_copy_scope;
  {
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto event = transaction.exec1(
        "SELECT boss_script->>'copy_scope', boss_script_published "
        "FROM emulator_monster_reward_events WHERE boss_script IS NOT NULL");
    first_copy_scope = event[0].as<std::string>();
    EXPECT_NE(first_copy_scope, "shared");
    EXPECT_TRUE(event[1].as<bool>());
    EXPECT_EQ(transaction.exec1("SELECT count(*) FROM emulator_monster_reward_receipts "
                                "WHERE processed")[0].as<int>(), 2);
  }

  const auto leave_cliffs = [&](BossPlayer &player) {
    const auto moved = exchange(player, protocol, 20009, {{"text", "tp_map 9012"}});
    ASSERT_EQ(frames_of(moved, 175).size(), 1U);
    const auto loaded = exchange(player, protocol, 20085, nlohmann::json::object());
    ASSERT_EQ(frames_of(loaded, 176).size(), 1U);
    exchange(player, protocol, 20086, nlohmann::json::object());
  };
  for (std::size_t index = 0; index < 2; ++index) {
    leave_cliffs(players[index]);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
  }
  const auto recreated_entry = enter_cliffs(players[0]);
  const auto recreated_boss = find_boss(recreated_entry);
  ASSERT_FALSE(recreated_boss.empty());
  EXPECT_NE(recreated_boss, leader_boss);
  const auto progress = repositories.character_state->load_world_progress(
      players[0].account_id, players[0].character_id, 1);
  EXPECT_EQ(progress.quests.at(kCliffsQuest).objectives.at(kEliteObjective), 1);
  EXPECT_FALSE(progress.doodad_phases.contains(doodad_unit_id(170 | (289 << 14), 5)));
  // Cliffs reconstructs this phase from the participant's durable quest objective on map load.
  EXPECT_TRUE(has_elite_barrier_snapshot(recreated_entry));
  leave_cliffs(players[2]);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  const auto observer_entry = enter_cliffs(players[2]);
  EXPECT_EQ(find_boss(observer_entry), recreated_boss);
  EXPECT_TRUE(has_elite_barrier_snapshot(observer_entry));
  const auto observer_progress = repositories.character_state->load_world_progress(
      players[2].account_id, players[2].character_id, 1);
  EXPECT_FALSE(observer_progress.quests.at(kCliffsQuest).objectives.contains(kEliteObjective));
  const auto second_defeat = exchange(players[0], protocol, 20009,
                                       {{"text", "monster_kill " + recreated_boss}});
  ASSERT_FALSE(frames_of(second_defeat, 23).empty());
  std::vector<std::string> copy_scopes;
  for (int attempt = 0; attempt < 200 && copy_scopes.size() < 2; ++attempt) {
    exchange(players[0], protocol, 20003, nlohmann::json::object());
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto events = transaction.exec(
        "SELECT boss_script->>'copy_scope' FROM emulator_monster_reward_events "
        "WHERE boss_script IS NOT NULL AND boss_script_published");
    copy_scopes.clear();
    for (const auto &event : events) {
      copy_scopes.push_back(event[0].as<std::string>());
    }
    if (copy_scopes.size() < 2) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_EQ(copy_scopes.size(), 2U);
  const auto old_scope_count = std::ranges::count(copy_scopes, first_copy_scope);
  EXPECT_EQ(old_scope_count, 1) << "A recreated copy must have a different private scope";
}

TEST(NativeBossPublication, SharedCliffsCrashAfterBarrierInstall) {
  const char *signal = std::getenv("CHRONICLES_BOSS_CRASH_SIGNAL");
  if (!signal) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL crash harness";
  }
  verify_shared_cliffs_publication(BossFault::none, false, signal);
}

TEST(NativeBossPublication, SharedCliffsRestartsAfterBarrierInstall) {
  const char *database_url = std::getenv("CHRONICLES_BOSS_E2E_DATABASE_URL");
  const char *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!database_url || !catalog_environment) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL crash harness and verified profile";
  }
  {
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "boss_publication_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
    const auto event = transaction.exec1(
        "SELECT boss_script_applied, boss_script_published "
        "FROM emulator_monster_reward_events WHERE boss_script IS NOT NULL");
    ASSERT_TRUE(event[0].as<bool>());
    const bool production_recovered = std::getenv("CHRONICLES_BOSS_PRODUCTION_RECOVERED");
    ASSERT_EQ(event[1].as<bool>(), production_recovered);
  }

  const std::filesystem::path catalog_path(catalog_environment);
  auto repositories = make_postgres_character_repositories(database_url);
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories, config);
  auth.acquire_monster_reward_owner_epoch();

  std::vector<BossPlayer> players(2);
  for (std::size_t index = 0; index < players.size(); ++index) {
    const auto login = auth.login("boss_test_" + std::to_string(index), "test-world-password-123");
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(1);
    ASSERT_EQ(auth.enter(login.access_token(), enter).error_code(), x5::ec::Success);
    auto &player = players[index];
    player.account_id = login.account_id();
    player.access_token = login.access_token();
    pqxx::connection connection(database_url);
    pqxx::read_transaction transaction(connection);
    const auto character = transaction.exec_params1(
        "SELECT id FROM emulator_characters WHERE account_id = $1 AND world_id = 1",
        player.account_id);
    player.character_id = character[0].as<std::int64_t>();
  }

  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  auto gathering = GatheringData::load(catalog_path.parent_path() / "gathering" /
                                        "definitions.json");
  NpcPlacementSet empty_population;
  empty_population.map_id = kCliffsMap;
  auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  TemporaryWorldCertificate certificate;
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  TwoClientWorldServer server(auth, certificate, {}, CharacterStats::load(*catalog), catalog,
                              catalog_path, population, {}, {}, {}, gathering);

  for (auto &player : players) {
    enter_world(player, auth, server.port(), protocol);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    EXPECT_TRUE(has_elite_barrier_snapshot(player.entry_frames) ||
                has_elite_barrier_snapshot(player.start_frames));
  }
  for (int attempt = 0; attempt < 200; ++attempt) {
    bool all_credited = true;
    for (auto &player : players) {
      exchange(player, protocol, 20003, nlohmann::json::object());
      const auto progress = repositories.character_state->load_world_progress(
          player.account_id, player.character_id, 1);
      all_credited = all_credited && progress.quests.at(kCliffsQuest).objectives.contains(
                                          kEliteObjective);
    }
    if (all_credited) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  for (const auto &player : players) {
    const auto progress = repositories.character_state->load_world_progress(
        player.account_id, player.character_id, 1);
    ASSERT_TRUE(progress.quests.at(kCliffsQuest).objectives.contains(kEliteObjective));
    EXPECT_EQ(progress.quests.at(kCliffsQuest).objectives.at(kEliteObjective), 1);
    EXPECT_TRUE(repositories.character_state
                    ->pending_monster_rewards(player.account_id, player.character_id, 1)
                    .empty());
  }
  pqxx::connection connection(database_url);
  pqxx::read_transaction transaction(connection);
  const auto event = transaction.exec1(
      "SELECT boss_script_published FROM emulator_monster_reward_events "
      "WHERE boss_script IS NOT NULL");
  EXPECT_TRUE(event[0].as<bool>());
}

} // namespace
} // namespace chronicles
