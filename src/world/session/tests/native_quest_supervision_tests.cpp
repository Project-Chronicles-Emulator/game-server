#include "testing/catalog_test_support.hpp"
#include "world/features/character/progression.hpp"
#include "world/features/trade/tests/pausing_region_checkpoint_store.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <thread>

namespace chronicles {

void arm_quest_supervisor_faults_for_testing() noexcept;
bool quest_supervisor_post_fault_observed_for_testing() noexcept;
bool quest_retirement_fault_observed_for_testing() noexcept;
void arm_quest_success_forget_fault_for_testing() noexcept;
bool quest_success_forget_fault_observed_for_testing() noexcept;
void arm_recovered_reward_encoding_fault_for_testing() noexcept;
bool recovered_reward_encoding_fault_observed_for_testing() noexcept;
void arm_gameplay_completion_fault_for_testing(bool unknown) noexcept;
bool gameplay_completion_fault_observed_for_testing() noexcept;
void arm_frame_worker_post_fault_for_testing(bool unknown) noexcept;
bool frame_worker_post_fault_observed_for_testing() noexcept;
void arm_tick_worker_post_fault_for_testing(bool unknown) noexcept;
bool tick_worker_post_fault_observed_for_testing() noexcept;
bool tick_worker_post_timer_observed_for_testing() noexcept;
void arm_response_io_fault_for_testing(bool timer) noexcept;
bool response_io_fault_observed_for_testing(bool timer) noexcept;
void arm_close_hook_retry_fault_for_testing() noexcept;
void release_close_hook_retry_fault_for_testing() noexcept;
bool close_hook_retry_fault_observed_for_testing() noexcept;
unsigned close_hook_party_stage_calls_for_testing() noexcept;

namespace {

using namespace native_testing;

class ReleaseQuestFault final {
public:
  explicit ReleaseQuestFault(std::shared_ptr<PausingRegionCheckpointStore> store)
      : store_(std::move(store)) {
  }

  ~ReleaseQuestFault() {
    store_->release_quest_write();
    store_->allow_fences();
  }

private:
  std::shared_ptr<PausingRegionCheckpointStore> store_;
};

void verify_quest_recovery(bool drain_server) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Requires the verified client catalog and generated quest profile";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  NpcPlacementSet placements;
  placements.map_id = kOverworldMapId;
  auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  std::shared_ptr<PausingRegionCheckpointStore> store;
  OverworldSessions world(
      population, {kOverworldSpawn}, catalog, catalog_path, {}, CharacterStats::load(*catalog),
      {}, {}, {}, [&](std::shared_ptr<CharacterStateRepository> inner) {
        store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
        return store;
      });
  ReleaseQuestFault release(store);
  ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
  const auto login = world.auth->login("overworld_sessions0", "test-world-password-123");
  auto &session = world.sessions[0];
  session.access_token = login.access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);
  const auto entry = world.enter(session);
  static_cast<void>(entry);
  VehicleClient client(world, session, 3);
  client.send(20086, nlohmann::json::object());
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  constexpr int kQuest = 10000127;
  store->refuse_fences_for(session.character_id);
  store->lose_next_quest_write_acknowledgement(session.character_id, true);
  arm_quest_supervisor_faults_for_testing();
  client.send_without_barrier(20009, {{"text", "accept_quest 10000127"}});
  ASSERT_TRUE(store->wait_for_lost_quest_write_acknowledgement());
  store->release_quest_write();
  const auto fault_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!quest_supervisor_post_fault_observed_for_testing() &&
         std::chrono::steady_clock::now() < fault_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(quest_supervisor_post_fault_observed_for_testing());
  {
    WorldTestClient refused(world.server->port());
    refused.send_key(ticket.packet_key());
    try {
      const auto frame = refused.read();
      FAIL() << "Unresolved quest owner admitted opcode " << frame.opcode();
    } catch (const boost::system::system_error &error) {
      EXPECT_TRUE(error.code() == asio::error::eof ||
                  error.code() == asio::error::connection_reset ||
                  error.code() == ssl::error::stream_truncated) << error.what();
    }
  }

  std::shared_ptr<std::promise<void>> drained;
  std::future<void> completion;
  if (drain_server) {
    drained = std::make_shared<std::promise<void>>();
    completion = drained->get_future();
    world.server->listener().shutdown_when_drained([drained] { drained->set_value(); });
    EXPECT_EQ(completion.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
  }
  store->allow_fences();
  const auto retirement_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!quest_retirement_fault_observed_for_testing() &&
         std::chrono::steady_clock::now() < retirement_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(quest_retirement_fault_observed_for_testing());
  if (drain_server) {
    ASSERT_EQ(completion.wait_for(std::chrono::seconds(15)), std::future_status::ready);
  } else {
    std::unique_ptr<WorldTestClient> reconnected;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline && !reconnected) {
      auto attempt = std::make_unique<WorldTestClient>(world.server->port());
      attempt->send_key(ticket.packet_key());
      try {
        ASSERT_EQ(attempt->read().opcode(), 5);
        reconnected = std::move(attempt);
      } catch (const boost::system::system_error &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    ASSERT_TRUE(reconnected);
  }
  const auto progress = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  EXPECT_TRUE(progress.quests.contains(kQuest));
}

TEST(NativeQuestSupervision, FailedWorkerPostAndRetirementRecoverBeforeReconnect) {
  verify_quest_recovery(false);
}

TEST(NativeQuestSupervision, ShutdownWaitsForFencedQuestReconciliation) {
  verify_quest_recovery(true);
}

void verify_shared_kill_without_quest_profile(bool lose_acknowledgement,
                                             bool fail_success_retirement = false,
                                             bool fail_recovered_encoding = false) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Requires the verified client catalog and world monster profile";
  }
  const std::filesystem::path catalog_path(catalog_environment);
  const auto mob_profile = catalog_path.parent_path() / "world-mobs" / "definitions.json";
  if (!std::filesystem::is_regular_file(mob_profile)) {
    GTEST_SKIP() << "Requires the generated world monster profile";
  }
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  const auto progression = Progression::load(*catalog);
  std::ifstream input(mob_profile);
  nlohmann::json profile;
  input >> profile;
  const auto map = std::ranges::find_if(profile.at("maps"), [](const auto &entry) {
    return entry.at("map_id") == 9012;
  });
  ASSERT_NE(map, profile.at("maps").end());
  const auto spawn = std::ranges::find_if(map->at("spawns"), [&](const auto &entry) {
    return !entry.contains("patrol") &&
           progression->monster_experience(entry.at("cms_id").template get<int>()) > 0;
  });
  ASSERT_NE(spawn, map->at("spawns").end());
  const auto npc_id = spawn->at("cms_id").get<int>();
  const auto &point = spawn->at("position");
  const WorldPosition position{9012, point.at(0), point.at(1), point.at(2), 0};

  // Keep the generated monster profile while deliberately omitting quests/catalog.json.
  testing_support::TestCatalog temporary({});
  const auto isolated_catalog_root = temporary.root() / "client";
  std::filesystem::create_directories(isolated_catalog_root);
  std::filesystem::create_directories(temporary.root() / "world-mobs");
  std::filesystem::copy_file(mob_profile, temporary.root() / "world-mobs" / "definitions.json");
  NpcPlacementSet placements;
  placements.map_id = 9012;
  auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  std::shared_ptr<PausingRegionCheckpointStore> store;
  OverworldSessions world(population, {position}, catalog, isolated_catalog_root, {},
                          CharacterStats::load(*catalog), {}, {}, {},
                          [&](std::shared_ptr<CharacterStateRepository> inner) {
                            store = std::make_shared<PausingRegionCheckpointStore>(std::move(inner));
                            return store;
                          });
  ReleaseQuestFault release(store);
  auto &session = world.sessions[0];
  const auto threshold = progression->next_level_experience(1);
  ASSERT_TRUE(threshold);
  ASSERT_GT(*threshold, 0);
  auto before_seed = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  auto seeded = before_seed;
  ++seeded.revision;
  seeded.experience = *threshold - 1;
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      session.account_id, session.character_id, 1, before_seed, seeded));
  ASSERT_TRUE(world.auth->set_account_role("overworld_sessions0", AccountRole::game_master));
  const auto login = world.auth->login("overworld_sessions0", "test-world-password-123");
  session.access_token = login.access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(world.auth->enter(login.access_token(), enter).error_code(), x5::ec::Success);
  const auto entry = world.enter(session);
  const auto monster = std::ranges::find_if(entry.unit_adds, [&](const auto &unit) {
    return unit.at("nub").contains("npc") &&
           unit.at("nub").at("npc").at("npc_cms_id") == npc_id &&
           unit.at("nub").at("pos").at("millimeter_x") == point.at(0) &&
           unit.at("nub").at("pos").at("millimeter_y") == point.at(1);
  });
  ASSERT_NE(monster, entry.unit_adds.end());
  VehicleClient client(world, session, 3);
  const auto before = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);
  if (fail_recovered_encoding) {
    MonsterRewardReceipt receipt;
    receipt.account_id = session.account_id;
    receipt.character_id = session.character_id;
    receipt.world_id = 1;
    receipt.receipt_id = std::string(32, 'b');
    receipt.credited_unit_id = std::stoull(monster->at("unit_id").get<std::string>());
    receipt.npc_cms_id = npc_id;
    receipt.position = position;
    receipt.award_experience = true;
    ASSERT_TRUE(world.auth->enqueue_monster_rewards({receipt}));
    ASSERT_TRUE(world.auth->mark_monster_reward_ready(receipt.receipt_id));
    arm_recovered_reward_encoding_fault_for_testing();
    client.send_without_barrier(20086, nlohmann::json::object());
  } else {
    client.send(20086, nlohmann::json::object());
  }
  const nlohmann::json kill{{"text", "monster_kill " +
                                      monster->at("unit_id").get<std::string>()}};
  if (fail_recovered_encoding) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!recovered_reward_encoding_fault_observed_for_testing() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(recovered_reward_encoding_fault_observed_for_testing());
  } else if (lose_acknowledgement) {
    store->refuse_fences_for(session.character_id);
    store->lose_next_monster_commit_acknowledgement(session.character_id);
    client.send_without_barrier(20009, kill);
    ASSERT_TRUE(store->wait_for_lost_monster_commit_acknowledgement());
  } else if (fail_success_retirement) {
    arm_quest_success_forget_fault_for_testing();
    client.send_without_barrier(20009, kill);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!quest_success_forget_fault_observed_for_testing() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(quest_success_forget_fault_observed_for_testing());
  } else {
    const auto killed = client.send(20009, kill);
    ASSERT_FALSE(frames_of(killed, 23).empty());
  }
  WorldProgress awarded;
  for (int attempt = 0; attempt < 100; ++attempt) {
    awarded = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    if (awarded.experience > before.experience) {
      break;
    }
    if (!lose_acknowledgement && !fail_success_retirement && !fail_recovered_encoding) {
      client.drain();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(awarded.experience - before.experience, progression->monster_experience(npc_id));
  ASSERT_GT(awarded.level, before.level);
  EXPECT_TRUE(world.repositories.character_state->pending_monster_rewards(
      session.account_id, session.character_id, 1).empty());

  if (lose_acknowledgement) {
    WorldTestClient refused(world.server->port());
    refused.send_key(ticket.packet_key());
    try {
      const auto frame = refused.read();
      FAIL() << "Unresolved EXP owner admitted opcode " << frame.opcode();
    } catch (const boost::system::system_error &error) {
      EXPECT_TRUE(error.code() == asio::error::eof ||
                  error.code() == asio::error::connection_reset ||
                  error.code() == ssl::error::stream_truncated) << error.what();
    }
    store->allow_fences();
  } else if (!fail_success_retirement && !fail_recovered_encoding) {
    session.client->disconnect();
  }
  std::unique_ptr<WorldTestClient> reconnected;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline && !reconnected) {
    auto attempt = std::make_unique<WorldTestClient>(world.server->port());
    attempt->send_key(ticket.packet_key());
    try {
      ASSERT_EQ(attempt->read().opcode(), 5);
      reconnected = std::move(attempt);
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(reconnected);
  session.client = std::move(reconnected);
  const auto reentry = world.enter_after_key_exchange(session);
  const auto &player = reentry.map_complete.at("nub").at("player");
  EXPECT_EQ(player.at("level"), awarded.level);
  EXPECT_EQ(player.at("exp"), std::to_string(awarded.experience));
  VehicleClient resumed(world, session, 3);
  resumed.send(20086, nlohmann::json::object());
  const auto restored = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  EXPECT_EQ(restored.experience, awarded.experience);
  EXPECT_EQ(restored.level, awarded.level);
  EXPECT_TRUE(world.repositories.character_state->pending_monster_rewards(
      session.account_id, session.character_id, 1).empty());
}

TEST(NativeQuestSupervision, SharedKillWithoutQuestProfileAwardsExperienceOnceAcrossReconnect) {
  verify_shared_kill_without_quest_profile(false);
}

TEST(NativeQuestSupervision, LostExperienceAcknowledgementFencesAndRestoresLevel) {
  verify_shared_kill_without_quest_profile(true);
}

TEST(NativeQuestSupervision, SuccessfulExperienceOwnerRetirementFailureRetriesSafely) {
  verify_shared_kill_without_quest_profile(false, true);
}

TEST(NativeQuestSupervision, RecoveredRewardEncodingFailureClosesAndReconnects) {
  verify_shared_kill_without_quest_profile(false, false, true);
}

void verify_gameplay_completion_failure(bool unknown) {
  NpcPlacementSet placements;
  placements.map_id = kOverworldMapId;
  auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  OverworldSessions world(population, {kOverworldSpawn});
  auto &session = world.sessions[0];
  world.enter(session);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  arm_gameplay_completion_fault_for_testing(unknown);
  VehicleClient client(world, session, 3);
  client.send_without_barrier(20003, nlohmann::json::object());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!gameplay_completion_fault_observed_for_testing() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(gameplay_completion_fault_observed_for_testing());

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!reconnected && std::chrono::steady_clock::now() < reconnect_deadline) {
    auto attempt = std::make_unique<WorldTestClient>(world.server->port());
    attempt->send_key(ticket.packet_key());
    try {
      ASSERT_EQ(attempt->read().opcode(), 5);
      reconnected = std::move(attempt);
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(reconnected);
  session.client = std::move(reconnected);
  const auto reentry = world.enter_after_key_exchange(session);
  EXPECT_EQ(reentry.map_complete.at("map_id"), kOverworldMapId);
}

TEST(NativeQuestSupervision, GameplayCompletionFailureClosesAndReconnects) {
  verify_gameplay_completion_failure(false);
}

TEST(NativeQuestSupervision, UnknownGameplayCompletionFailureClosesAndReconnects) {
  verify_gameplay_completion_failure(true);
}

void verify_worker_dispatch_failure(bool timer, bool unknown, bool response_io = false) {
  NpcPlacementSet placements;
  placements.map_id = kOverworldMapId;
  auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  OverworldSessions world(population, {kOverworldSpawn});
  auto &session = world.sessions[0];
  world.enter(session);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  if (response_io) {
    arm_response_io_fault_for_testing(timer);
  } else if (timer) {
    arm_tick_worker_post_fault_for_testing(unknown);
  } else {
    arm_frame_worker_post_fault_for_testing(unknown);
  }
  VehicleClient client(world, session, 3);
  client.send_without_barrier(20003, nlohmann::json::object());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool observed = false;
  while (!observed && std::chrono::steady_clock::now() < deadline) {
    if (response_io) {
      observed = response_io_fault_observed_for_testing(timer);
    } else {
      observed = timer ? tick_worker_post_fault_observed_for_testing()
                       : frame_worker_post_fault_observed_for_testing();
    }
    if (!observed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ASSERT_TRUE(observed);
  if (timer) {
    EXPECT_TRUE(tick_worker_post_timer_observed_for_testing());
  }

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!reconnected && std::chrono::steady_clock::now() < reconnect_deadline) {
    auto attempt = std::make_unique<WorldTestClient>(world.server->port());
    attempt->send_key(ticket.packet_key());
    try {
      ASSERT_EQ(attempt->read().opcode(), 5);
      reconnected = std::move(attempt);
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(reconnected);
  session.client = std::move(reconnected);
  const auto reentry = world.enter_after_key_exchange(session);
  EXPECT_EQ(reentry.map_complete.at("map_id"), kOverworldMapId);
}

TEST(NativeQuestSupervision, FrameWorkerPostFailureClosesAndReconnects) {
  verify_worker_dispatch_failure(false, false);
}

TEST(NativeQuestSupervision, UnknownFrameWorkerPostFailureClosesAndReconnects) {
  verify_worker_dispatch_failure(false, true);
}

TEST(NativeQuestSupervision, TimerWorkerPostFailureClosesAndReconnects) {
  verify_worker_dispatch_failure(true, false);
}

TEST(NativeQuestSupervision, UnknownTimerWorkerPostFailureClosesAndReconnects) {
  verify_worker_dispatch_failure(true, true);
}

TEST(NativeQuestSupervision, FrameResponseIoFailureClosesAndReconnects) {
  verify_worker_dispatch_failure(false, false, true);
}

TEST(NativeQuestSupervision, TimerResponseIoFailureClosesAndReconnects) {
  verify_worker_dispatch_failure(true, false, true);
}

TEST(NativeQuestSupervision, CloseHookRetryFencesAdmissionWithoutRepeatingPartyStage) {
  NpcPlacementSet placements;
  placements.map_id = kOverworldMapId;
  auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
  OverworldSessions world(population, {kOverworldSpawn});
  auto &session = world.sessions[0];
  world.enter(session);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  struct FaultRelease {
    ~FaultRelease() {
      release_close_hook_retry_fault_for_testing();
    }
  } fault_release;
  arm_close_hook_retry_fault_for_testing();
  session.client->disconnect();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!close_hook_retry_fault_observed_for_testing() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(close_hook_retry_fault_observed_for_testing());

  WorldTestClient blocked(world.server->port());
  blocked.send_key(ticket.packet_key());
  EXPECT_THROW(blocked.read(), boost::system::system_error);
  EXPECT_EQ(close_hook_party_stage_calls_for_testing(), 1U);
  release_close_hook_retry_fault_for_testing();

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!reconnected && std::chrono::steady_clock::now() < reconnect_deadline) {
    auto attempt = std::make_unique<WorldTestClient>(world.server->port());
    attempt->send_key(ticket.packet_key());
    try {
      ASSERT_EQ(attempt->read().opcode(), 5);
      reconnected = std::move(attempt);
    } catch (const boost::system::system_error &) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(reconnected);
  EXPECT_EQ(close_hook_party_stage_calls_for_testing(), 1U);
  session.client = std::move(reconnected);
  const auto reentry = world.enter_after_key_exchange(session);
  EXPECT_EQ(reentry.map_complete.at("map_id"), kOverworldMapId);
}

} // namespace
} // namespace chronicles
