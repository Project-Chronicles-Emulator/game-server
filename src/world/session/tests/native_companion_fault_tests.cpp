#include "world/features/companion/companion_world.hpp"
#include "world/features/companion/tests/native_vehicle_test_support.hpp"

#include <gtest/gtest.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace chronicles {

// These symbols exist only in the separately compiled fault-test session library.
void arm_companion_remove_fault_for_testing() noexcept;
bool companion_remove_fault_observed_for_testing() noexcept;
void observe_companion_entry_binding_for_testing(std::uint64_t unit_id) noexcept;
bool companion_entry_binding_observed_for_testing() noexcept;
void arm_companion_first_frame_fault_for_testing() noexcept;
bool companion_first_frame_fault_observed_for_testing() noexcept;
void arm_companion_entry_close_fault_for_testing() noexcept;
bool companion_close_post_fault_observed_for_testing() noexcept;
void arm_companion_terminal_pause_for_testing(std::uint64_t recipient);
bool wait_companion_terminal_pause_for_testing();
void release_companion_terminal_pause_for_testing();
bool companion_terminal_stale_observed_for_testing() noexcept;
void set_companion_detach_budget_for_testing(
    std::shared_ptr<companion::CompanionDetachBudget> budget);
bool companion_budget_exhaustion_observed_for_testing() noexcept;
void arm_companion_transition_fault_for_testing() noexcept;
bool companion_transition_fault_observed_for_testing() noexcept;
void arm_general_transition_fault_for_testing() noexcept;
bool general_transition_fault_observed_for_testing() noexcept;
void arm_map_transition_log_fault_for_testing() noexcept;
bool map_transition_log_fault_observed_for_testing() noexcept;
void arm_map_transition_io_fault_for_testing() noexcept;
bool map_transition_io_fault_observed_for_testing() noexcept;
bool general_transition_staged_observed_for_testing() noexcept;
void arm_companion_fanout_allocation_fault_for_testing() noexcept;
bool companion_fanout_allocation_fault_observed_for_testing() noexcept;
void arm_companion_fanout_pause_for_testing();
bool wait_companion_fanout_pause_for_testing();
void release_companion_fanout_pause_for_testing();
bool companion_fanout_old_view_skipped_for_testing() noexcept;

namespace {

using namespace native_testing;
using namespace vehicle_native_testing;

// Pause only the observer's worker after StartPlaying assembled its response. Other
// workers and the I/O executor remain free to settle the owner's disconnect.
class ObserverEntryPause final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
  bool wait_until_paused() {
    return entered_future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }

  void release() {
    if (!released_.exchange(true)) {
      release_.set_value();
    }
  }

  void wait_until_exited() {
    if (paused_.load()) {
      exited_future_.wait();
    }
  }

protected:
  void sink_it_(const spdlog::details::log_msg &message) override {
    const std::string_view text(message.payload.data(), message.payload.size());
    if (text.find("connection=2 opcode=20086") == std::string_view::npos ||
        paused_.exchange(true)) {
      return;
    }
    entered_.set_value();
    release_future_.wait_for(std::chrono::seconds(30));
    exited_.set_value();
  }

  void flush_() override {
  }

private:
  std::atomic<bool> paused_{};
  std::atomic<bool> released_{};
  std::promise<void> entered_;
  std::future<void> entered_future_{entered_.get_future()};
  std::promise<void> exited_;
  std::future<void> exited_future_{exited_.get_future()};
  std::promise<void> release_;
  std::shared_future<void> release_future_{release_.get_future().share()};
};

class ScopedObserverEntryPause final {
public:
  ScopedObserverEntryPause()
      : previous_(spdlog::default_logger()), sink_(std::make_shared<ObserverEntryPause>()) {
    auto logger = std::make_shared<spdlog::logger>("companion-entry-pause", sink_);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
  }

  ~ScopedObserverEntryPause() {
    sink_->release();
    sink_->wait_until_exited();
    spdlog::set_default_logger(std::move(previous_));
  }

  ObserverEntryPause &sink() {
    return *sink_;
  }

private:
  std::shared_ptr<spdlog::logger> previous_;
  std::shared_ptr<ObserverEntryPause> sink_;
};

class ScopedTerminalPause final {
public:
  explicit ScopedTerminalPause(std::uint64_t recipient) {
    arm_companion_terminal_pause_for_testing(recipient);
  }

  ~ScopedTerminalPause() {
    release_companion_terminal_pause_for_testing();
  }
};

class ScopedCompanionBudget final {
public:
  explicit ScopedCompanionBudget(std::shared_ptr<companion::CompanionDetachBudget> budget) {
    set_companion_detach_budget_for_testing(std::move(budget));
  }

  ~ScopedCompanionBudget() {
    set_companion_detach_budget_for_testing({});
  }
};

class ScopedFanoutPause final {
public:
  ScopedFanoutPause() {
    arm_companion_fanout_pause_for_testing();
  }

  ~ScopedFanoutPause() {
    release_companion_fanout_pause_for_testing();
  }
};

std::optional<nlohmann::json> start_wagon(VehicleWorldFixture &fixture,
                                          const OverworldEntry &owner_entry) {
  auto &owner = fixture.client(0);
  std::string ileste;
  for (const auto &unit : owner_entry.unit_adds) {
    if (unit.at("nub").at("npc").at("npc_cms_id") == 1001021) {
      ileste = unit.at("unit_id").get<std::string>();
      break;
    }
  }
  if (ileste.empty()) {
    const auto profile = std::filesystem::path(std::getenv("CHRONICLES_CLIENT_CATALOG"))
                             .parent_path() / "static-npcs" / "definitions.json";
    std::ifstream input(profile);
    if (!input) {
      throw std::runtime_error("The static NPC fixture is unavailable");
    }
    const auto document = nlohmann::json::parse(input);
    for (const auto &target : document.at("targets")) {
      if (target.at("map_id") == kOverworldMapId && target.at("npc_cms_id") == 1001021) {
        ileste = target.at("unit_id").get<std::string>();
        break;
      }
    }
    if (ileste.empty()) {
      throw std::runtime_error("The trade manager is absent from the NPC fixture");
    }
  }
  owner.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                     {"yaw", 0.0}});
  owner.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1},
                     {"select_order", 1}});
  owner.send(20105, nlohmann::json::object());
  const auto departure = owner.send(
      20106, {{"add_goods", nlohmann::json::array(
                               {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  const auto wagons = frames_of(departure, kUnitAddOpcode);
  if (wagons.size() != 1) {
    return std::nullopt;
  }
  return std::optional<nlohmann::json>(std::in_place, wagons.front().at("unit_id"));
}

bool is_closed(const boost::system::error_code &code) {
  return code == asio::error::eof || code == asio::error::connection_reset ||
         code == ssl::error::stream_truncated;
}

TEST(NativeCompanionFault, PartialDeliveryClosesObserverAndReleasesOwnerAfterCleanup) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  OverworldEntry owner_entry;
  OverworldEntry observer_entry;
  fixture.start(0, owner_entry);
  fixture.start(1, observer_entry);
  auto &owner = fixture.client(0);
  auto &observer = fixture.client(1);
  observer.drain();

  const auto ileste = npc_unit_id(owner_entry, 1001021);
  owner.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                     {"yaw", 0.0}});
  owner.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1},
                     {"select_order", 1}});
  owner.send(20105, nlohmann::json::object());
  const auto departure = owner.send(
      20106, {{"add_goods", nlohmann::json::array(
                               {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  const auto wagons = frames_of(departure, kUnitAddOpcode);
  ASSERT_EQ(wagons.size(), 1U);
  const auto wagon_id = wagons.front().at("unit_id");
  const auto cargo = fixture.progress(0).trade_run.goods;
  ASSERT_EQ(frames_of(observer.drain(), kUnitAddOpcode).size(), 1U);

  auto &world = fixture.world();
  auto &owner_session = world.sessions[0];
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(owner_session.character_id);
  const auto ticket = world.auth->select(owner_session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  arm_companion_remove_fault_for_testing();
  owner_session.client->disconnect();
  bool observer_closed = false;
  std::size_t dismount_count = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !observer_closed) {
    try {
      const auto frame = world.sessions[1].client->read();
      if (frame.opcode() == companion::kMountUnitNotification &&
          world.protocol.decode(frame).at("message").at("unit_id") == wagon_id) {
        ++dismount_count;
      }
    } catch (const boost::system::system_error &error) {
      observer_closed = error.code() == asio::error::eof ||
                        error.code() == asio::error::connection_reset ||
                        error.code() == ssl::error::stream_truncated;
      ASSERT_TRUE(observer_closed) << error.what();
    }
  }
  ASSERT_TRUE(companion_remove_fault_observed_for_testing());
  ASSERT_TRUE(observer_closed);
  EXPECT_LE(dismount_count, 1U);

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  EXPECT_EQ(fixture.progress(0).trade_run.goods, cargo);
}

TEST(NativeCompanionFault, QueuedEntryCannotAddACompanionAfterItsOwnerLeaves) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  OverworldEntry owner_entry;
  fixture.start(0, owner_entry);
  auto &owner = fixture.client(0);
  const auto ileste = npc_unit_id(owner_entry, 1001021);
  owner.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                     {"yaw", 0.0}});
  owner.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1},
                     {"select_order", 1}});
  owner.send(20105, nlohmann::json::object());
  const auto departure = owner.send(
      20106, {{"add_goods", nlohmann::json::array(
                               {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  const auto wagons = frames_of(departure, kUnitAddOpcode);
  ASSERT_EQ(wagons.size(), 1U);
  const auto wagon_id = wagons.front().at("unit_id");

  auto &world = fixture.world();
  auto &owner_session = world.sessions[0];
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(owner_session.character_id);
  const auto ticket = world.auth->select(owner_session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  OverworldEntry observer_entry;
  std::future<std::vector<DecodedWorldFrame>> entry;
  ScopedObserverEntryPause pause;
  observe_companion_entry_binding_for_testing(std::stoull(wagon_id.get<std::string>()));
  entry = std::async(std::launch::async, [&] { return fixture.start(1, observer_entry); });
  ASSERT_TRUE(pause.sink().wait_until_paused());
  ASSERT_TRUE(companion_entry_binding_observed_for_testing());
  owner_session.client->disconnect();

  std::unique_ptr<WorldTestClient> reconnected;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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
  pause.sink().release();
  ASSERT_EQ(entry.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto observer_frames = entry.get();
  EXPECT_FALSE(std::ranges::any_of(frames_of(observer_frames, kUnitAddOpcode),
                                   [&](const auto &unit) {
                                     return unit.at("unit_id") == wagon_id;
                                   }));
}

TEST(NativeCompanionFault, OldRemovalCannotReachObserverAfterMapLeaveAndReturn) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  fixture.make_game_master(1);
  OverworldEntry owner_entry;
  OverworldEntry observer_entry;
  fixture.start(0, owner_entry);
  fixture.start(1, observer_entry);
  auto &observer = fixture.client(1);
  observer.drain();
  const auto wagon_id = start_wagon(fixture, owner_entry);
  ASSERT_TRUE(wagon_id);
  const auto visible = frames_of(observer.drain(), kUnitAddOpcode);
  ASSERT_TRUE(std::ranges::any_of(visible, [&](const auto &unit) {
    return unit.at("unit_id") == *wagon_id;
  }));

  auto &world = fixture.world();
  auto &owner_session = world.sessions[0];
  const auto cargo = fixture.progress(0).trade_run.goods;
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(owner_session.character_id);
  const auto ticket = world.auth->select(owner_session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  ScopedTerminalPause pause(2);
  owner_session.client->disconnect();
  ASSERT_TRUE(wait_companion_terminal_pause_for_testing());
  const auto away = observer.send(20009, {{"text", "tp_map 10111"}});
  ASSERT_EQ(frames_of(away, 175).size(), 1U);
  ASSERT_EQ(frames_of(observer.send(20085, nlohmann::json::object()), 176).size(), 1U);
  observer.send(20086, nlohmann::json::object());
  const auto back = observer.send(20009, {{"text", "tp_map 9012 -1486 3371 -601.67"}});
  ASSERT_EQ(frames_of(back, 175).size(), 1U);
  ASSERT_EQ(frames_of(observer.send(20085, nlohmann::json::object()), 176).size(), 1U);
  observer.send(20086, nlohmann::json::object());

  release_companion_terminal_pause_for_testing();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!companion_terminal_stale_observed_for_testing() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(companion_terminal_stale_observed_for_testing());
  const auto after_return = observer.drain();
  for (const auto &frame : after_return) {
    if (frame.opcode == companion::kMountUnitNotification) {
      EXPECT_NE(frame.message.at("unit_id"), *wagon_id);
    }
    if (frame.opcode == companion::kUnitRemoveNotification) {
      const auto &unit_ids = frame.message.at("unit_ids");
      EXPECT_EQ(std::ranges::find(unit_ids, *wagon_id), unit_ids.end());
    }
  }
  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  EXPECT_EQ(fixture.progress(0).trade_run.goods, cargo);
}

TEST(NativeCompanionFault, FirstTerminalFrameFailureClosesObserverBeforeOwnerRetry) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  OverworldEntry owner_entry;
  OverworldEntry observer_entry;
  fixture.start(0, owner_entry);
  fixture.start(1, observer_entry);
  auto &observer = fixture.client(1);
  observer.drain();
  const auto wagon_id = start_wagon(fixture, owner_entry);
  ASSERT_TRUE(wagon_id);
  const auto visible = frames_of(observer.drain(), kUnitAddOpcode);
  ASSERT_TRUE(std::ranges::any_of(visible, [&](const auto &unit) {
    return unit.at("unit_id") == *wagon_id;
  }));
  auto &world = fixture.world();
  auto &owner_session = world.sessions[0];
  const auto cargo = fixture.progress(0).trade_run.goods;
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(owner_session.character_id);
  const auto ticket = world.auth->select(owner_session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  arm_companion_first_frame_fault_for_testing();
  owner_session.client->disconnect();
  bool observer_closed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!observer_closed && std::chrono::steady_clock::now() < deadline) {
    try {
      const auto frame = world.sessions[1].client->read();
      const auto message = world.protocol.decode(frame).at("message");
      if (frame.opcode() == companion::kMountUnitNotification) {
        EXPECT_NE(message.at("unit_id"), *wagon_id);
      }
      if (frame.opcode() == companion::kUnitRemoveNotification) {
        const auto &unit_ids = message.at("unit_ids");
        EXPECT_EQ(std::ranges::find(unit_ids, *wagon_id), unit_ids.end());
      }
    } catch (const boost::system::system_error &error) {
      observer_closed = is_closed(error.code());
      ASSERT_TRUE(observer_closed) << error.what();
    }
  }
  ASSERT_TRUE(companion_first_frame_fault_observed_for_testing());
  ASSERT_TRUE(observer_closed);
  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  EXPECT_EQ(fixture.progress(0).trade_run.goods, cargo);
}

TEST(NativeCompanionFault, FailedEntryAndFirstClosePostAreRecoveredBySupervisor) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  auto &world = fixture.world();
  auto &session = world.sessions[0];
  const auto entry = world.enter(session);
  static_cast<void>(entry);
  VehicleClient client(world, session, 3);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);
  const auto before = fixture.progress(0);
  const auto before_character = world.repositories.characters->character_by_id(session.character_id);
  ASSERT_TRUE(before_character);

  arm_companion_entry_close_fault_for_testing();
  try {
    client.send(20086, nlohmann::json::object());
  } catch (const boost::system::system_error &error) {
    ASSERT_TRUE(is_closed(error.code())) << error.what();
  }
  bool closed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!closed && std::chrono::steady_clock::now() < deadline) {
    try {
      static_cast<void>(session.client->read());
    } catch (const boost::system::system_error &error) {
      closed = is_closed(error.code());
      ASSERT_TRUE(closed) << error.what();
    }
  }
  ASSERT_TRUE(companion_close_post_fault_observed_for_testing());
  ASSERT_TRUE(closed);

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  const auto after = fixture.progress(0);
  const auto after_character = world.repositories.characters->character_by_id(session.character_id);
  ASSERT_TRUE(after_character);
  EXPECT_EQ(after.trade_run.goods, before.trade_run.goods);
  EXPECT_EQ(after_character->world_position, before_character->world_position);
}

TEST(NativeCompanionFault, FailedRegionalReentryIsClosedAndRecoveredBySupervisor) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  const std::filesystem::path catalog_path(std::getenv("CHRONICLES_CLIENT_CATALOG"));
  auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  const auto population = load_npc_population_file(std::getenv("CHRONICLES_NPC_PLACEMENTS_9012"),
                                                   kMaxPopulatedNpcUnits);
  const WorldPosition boundary{9012, -4615500, 994500, -600000, 60};
  OverworldSessions world(population, {boundary}, catalog, catalog_path);
  auto &session = world.sessions[0];
  const auto entry = world.enter(session);
  static_cast<void>(entry);
  VehicleClient client(world, session, 3);
  client.send(20086, nlohmann::json::object());
  const nlohmann::json position{{"millimeter_x", boundary.x_mm},
                                {"millimeter_y", boundary.y_mm},
                                {"millimeter_z", boundary.z_mm}};
  const auto move = client.send(20090, {{"pos", position}, {"body_yaw", boundary.yaw}});
  ASSERT_EQ(frames_of(move, 175).size(), 1U);
  const auto loaded = client.send(20085, nlohmann::json::object());
  ASSERT_EQ(frames_of(loaded, 176).size(), 1U);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);
  const auto before = world.repositories.characters->character_by_id(session.character_id);
  ASSERT_TRUE(before);

  arm_companion_entry_close_fault_for_testing();
  try {
    client.send(20086, nlohmann::json::object());
  } catch (const boost::system::system_error &error) {
    ASSERT_TRUE(is_closed(error.code())) << error.what();
  }
  bool closed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!closed && std::chrono::steady_clock::now() < deadline) {
    try {
      static_cast<void>(session.client->read());
    } catch (const boost::system::system_error &error) {
      closed = is_closed(error.code());
      ASSERT_TRUE(closed) << error.what();
    }
  }
  ASSERT_TRUE(companion_close_post_fault_observed_for_testing());
  ASSERT_TRUE(closed);

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  const auto after = world.repositories.characters->character_by_id(session.character_id);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->world_position, before->world_position);
}

TEST(NativeCompanionFault, ExhaustedDetachBudgetRetainsOwnerUntilRetryDelivers) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  auto budget = std::make_shared<companion::CompanionDetachBudget>();
  ScopedCompanionBudget selected_budget(budget);
  VehicleWorldFixture fixture;
  OverworldEntry owner_entry;
  OverworldEntry observer_entry;
  fixture.start(0, owner_entry);
  fixture.start(1, observer_entry);
  auto &observer = fixture.client(1);
  observer.drain();
  const auto wagon_id = start_wagon(fixture, owner_entry);
  ASSERT_TRUE(wagon_id);
  const auto visible = frames_of(observer.drain(), kUnitAddOpcode);
  ASSERT_TRUE(std::ranges::any_of(visible, [&](const auto &unit) {
    return unit.at("unit_id") == *wagon_id;
  }));
  const auto cargo = fixture.progress(0).trade_run.goods;
  auto &world = fixture.world();
  auto &owner_session = world.sessions[0];
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(owner_session.character_id);
  const auto ticket = world.auth->select(owner_session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);
  auto exhaustion = budget->reserve(companion::CompanionDetachBudget::kDefaultLimitBytes - 1);
  ASSERT_TRUE(exhaustion);

  owner_session.client->disconnect();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!companion_budget_exhaustion_observed_for_testing() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(companion_budget_exhaustion_observed_for_testing());
  const auto pending = observer.drain();
  for (const auto &frame : pending) {
    if (frame.opcode == companion::kMountUnitNotification) {
      EXPECT_NE(frame.message.at("unit_id"), *wagon_id);
    }
    if (frame.opcode == companion::kUnitRemoveNotification) {
      const auto &unit_ids = frame.message.at("unit_ids");
      EXPECT_EQ(std::ranges::find(unit_ids, *wagon_id), unit_ids.end());
    }
  }
  {
    WorldTestClient refused(world.server->port());
    refused.send_key(ticket.packet_key());
    try {
      const auto frame = refused.read();
      FAIL() << "Unsettled owner entered with opcode " << frame.opcode();
    } catch (const boost::system::system_error &error) {
      ASSERT_TRUE(is_closed(error.code())) << error.what();
    }
  }
  exhaustion.reset();

  std::vector<DecodedWorldFrame> received;
  const auto delivery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < delivery_deadline) {
    auto frames = observer.drain();
    received.insert(received.end(), frames.begin(), frames.end());
    if (std::ranges::any_of(received, [&](const auto &frame) {
          if (frame.opcode != companion::kUnitRemoveNotification) {
            return false;
          }
          const auto &unit_ids = frame.message.at("unit_ids");
          return std::ranges::find(unit_ids, *wagon_id) != unit_ids.end();
        })) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const auto dismount = std::ranges::find_if(received, [&](const auto &frame) {
    return frame.opcode == companion::kMountUnitNotification &&
           frame.message.at("unit_id") == *wagon_id;
  });
  const auto removal = std::ranges::find_if(received, [&](const auto &frame) {
    if (frame.opcode != companion::kUnitRemoveNotification) {
      return false;
    }
    const auto &unit_ids = frame.message.at("unit_ids");
    return std::ranges::find(unit_ids, *wagon_id) != unit_ids.end();
  });
  ASSERT_NE(dismount, received.end());
  ASSERT_NE(removal, received.end());
  EXPECT_LT(dismount, removal);

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  EXPECT_EQ(fixture.progress(0).trade_run.goods, cargo);
}

void verify_failed_regional_transition(bool fail_io_staging) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  OverworldEntry owner_entry;
  fixture.start(0, owner_entry);
  const auto wagon_id = start_wagon(fixture, owner_entry);
  ASSERT_TRUE(wagon_id);
  auto &world = fixture.world();
  auto &session = world.sessions[0];
  auto &owner = fixture.client(0);
  const auto cargo = fixture.progress(0).trade_run.goods;
  ASSERT_EQ(cargo.size(), 2U);
  for (const auto &good : cargo) {
    EXPECT_EQ(good.good_cms_id, 1102);
    EXPECT_EQ(good.origin_post_id, 1100);
  }
  const WorldPosition boundary{9012, -4615500, 994500, -600000, 60};
  owner.move_to(boundary);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  if (fail_io_staging) {
    arm_map_transition_io_fault_for_testing();
  } else {
    arm_companion_transition_fault_for_testing();
  }
  owner.send_without_barrier(20090, {{"pos", world_pos_json(boundary)},
                                      {"body_yaw", boundary.yaw}});
  bool closed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!closed && std::chrono::steady_clock::now() < deadline) {
    try {
      static_cast<void>(session.client->read());
    } catch (const boost::system::system_error &error) {
      closed = is_closed(error.code());
      ASSERT_TRUE(closed) << error.what();
    }
  }
  if (fail_io_staging) {
    ASSERT_TRUE(map_transition_io_fault_observed_for_testing());
  } else {
    ASSERT_TRUE(companion_transition_fault_observed_for_testing());
  }
  ASSERT_TRUE(closed);

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  const auto saved = fixture.progress(0);
  ASSERT_TRUE(saved.trade_run.wagon_position);
  EXPECT_EQ(saved.trade_run.vehicle_cms_id, 200);
  EXPECT_EQ(saved.trade_run.wagon_position->map_id, 9021);
  EXPECT_EQ(saved.trade_run.wagon_position->x_mm, boundary.x_mm);
  EXPECT_EQ(saved.trade_run.wagon_position->y_mm, boundary.y_mm);
  EXPECT_EQ(saved.trade_run.wagon_position->z_mm, boundary.z_mm);
  EXPECT_EQ(saved.trade_run.goods, cargo);
  const auto character = world.repositories.characters->character_by_id(session.character_id);
  ASSERT_TRUE(character);
  ASSERT_TRUE(character->world_position);
  EXPECT_EQ(character->world_position->map_id, 9021);
  EXPECT_EQ(character->world_position->x_mm, boundary.x_mm);

  session.client = std::move(reconnected);
  const auto reentry = world.enter_after_key_exchange(session);
  EXPECT_EQ(reentry.map_start.at("map_id"), 9021);
  VehicleClient resumed(world, session, 3);
  const auto restored = resumed.send(20086, nlohmann::json::object());
  const auto restored_units = frames_of(restored, kUnitAddOpcode);
  const auto wagon = std::ranges::find_if(restored_units, [](const auto &unit) {
    const auto &nub = unit.at("nub");
    return nub.contains("operable") && nub.at("operable").contains("vehicle") &&
           nub.at("operable").at("vehicle").at("vehicle_cms_id") == 200;
  });
  ASSERT_NE(wagon, restored_units.end());
  const auto &wagon_position = wagon->at("nub").at("pos");
  EXPECT_EQ(wagon_position.at("millimeter_x"), boundary.x_mm);
  EXPECT_EQ(wagon_position.at("millimeter_y"), boundary.y_mm);
  EXPECT_EQ(wagon_position.at("millimeter_z"), boundary.z_mm);
  const auto after_bootstrap = fixture.progress(0);
  EXPECT_EQ(after_bootstrap.trade_run.goods, cargo);
  EXPECT_EQ(after_bootstrap.trade_run.vehicle_cms_id, 200);
  EXPECT_EQ(after_bootstrap.trade_run.wagon_position, saved.trade_run.wagon_position);
}

TEST(NativeCompanionFault, PartialRegionalTransitionPreservesCommittedWagonOnReconnect) {
  verify_failed_regional_transition(false);
}

TEST(NativeCompanionFault, RegionalStagingIoFailurePreservesCommittedWagonOnReconnect) {
  verify_failed_regional_transition(true);
}

void verify_failed_general_map_move(bool fail_log) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  fixture.make_game_master(0);
  OverworldEntry entry;
  fixture.start(0, entry);
  auto &world = fixture.world();
  auto &session = world.sessions[0];
  auto &client = fixture.client(0);
  const auto before = world.repositories.characters->character_by_id(session.character_id);
  ASSERT_TRUE(before);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  arm_general_transition_fault_for_testing();
  if (fail_log) {
    arm_map_transition_log_fault_for_testing();
  }
  client.send_without_barrier(20009, {{"text", "tp_map 10111"}});
  bool closed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!closed && std::chrono::steady_clock::now() < deadline) {
    try {
      const auto frame = session.client->read();
      EXPECT_NE(frame.opcode(), 175);
    } catch (const boost::system::system_error &error) {
      closed = is_closed(error.code());
      ASSERT_TRUE(closed) << error.what();
    }
  }
  ASSERT_TRUE(general_transition_fault_observed_for_testing());
  if (fail_log) {
    ASSERT_TRUE(map_transition_log_fault_observed_for_testing());
  }
  EXPECT_FALSE(general_transition_staged_observed_for_testing());
  ASSERT_TRUE(closed);

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  const auto after = world.repositories.characters->character_by_id(session.character_id);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->world_position, before->world_position);
}

TEST(NativeCompanionFault, FailedGeneralMapMoveClosesWithoutStagingDestination) {
  verify_failed_general_map_move(false);
}

TEST(NativeCompanionFault, FailedTransitionLogKeepsDurableLocationFence) {
  verify_failed_general_map_move(true);
}

TEST(NativeCompanionFault, MapStagingIoFailureClosesAndKeepsSavedDestination) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  fixture.make_game_master(0);
  OverworldEntry entry;
  fixture.start(0, entry);
  auto &world = fixture.world();
  auto &session = world.sessions[0];
  auto &client = fixture.client(0);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(session.character_id);
  const auto ticket = world.auth->select(session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  arm_map_transition_io_fault_for_testing();
  client.send_without_barrier(20009, {{"text", "tp_map 10111"}});
  bool closed = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!closed && std::chrono::steady_clock::now() < deadline) {
    try {
      const auto frame = session.client->read();
      EXPECT_NE(frame.opcode(), 175);
    } catch (const boost::system::system_error &error) {
      closed = is_closed(error.code());
      ASSERT_TRUE(closed) << error.what();
    }
  }
  ASSERT_TRUE(closed);
  ASSERT_TRUE(map_transition_io_fault_observed_for_testing());

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  const auto after = world.repositories.characters->character_by_id(session.character_id);
  ASSERT_TRUE(after);
  ASSERT_TRUE(after->world_position);
  EXPECT_EQ(after->world_position->map_id, 10111);
}

TEST(NativeCompanionFault, OrdinaryFanoutAllocationFailureClosesAffectedViews) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  OverworldEntry owner_entry;
  OverworldEntry observer_entry;
  fixture.start(0, owner_entry);
  fixture.start(1, observer_entry);
  auto &owner = fixture.client(0);
  fixture.client(1).drain();
  const auto ileste = npc_unit_id(owner_entry, 1001021);
  owner.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                     {"yaw", 0.0}});
  owner.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1},
                     {"select_order", 1}});
  owner.send(20105, nlohmann::json::object());
  auto &world = fixture.world();
  auto &owner_session = world.sessions[0];
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(owner_session.character_id);
  const auto ticket = world.auth->select(owner_session.access_token, select);
  ASSERT_EQ(ticket.ec(), x5::ec::Success);

  arm_companion_fanout_allocation_fault_for_testing();
  owner.send_without_barrier(
      20106, {{"add_goods", nlohmann::json::array(
                               {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!companion_fanout_allocation_fault_observed_for_testing() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(companion_fanout_allocation_fault_observed_for_testing());
  const auto committed_cargo = fixture.progress(0).trade_run.goods;
  ASSERT_FALSE(committed_cargo.empty());
  for (const auto index : {0U, 1U}) {
    bool closed = false;
    while (!closed && std::chrono::steady_clock::now() < deadline) {
      try {
        static_cast<void>(world.sessions[index].client->read());
      } catch (const boost::system::system_error &error) {
        closed = is_closed(error.code());
        ASSERT_TRUE(closed) << error.what();
      }
    }
    ASSERT_TRUE(closed) << "connection " << index << " remained live after partial fanout";
  }

  std::unique_ptr<WorldTestClient> reconnected;
  const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < reconnect_deadline && !reconnected) {
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
  EXPECT_EQ(fixture.progress(0).trade_run.goods, committed_cargo);
}

TEST(NativeCompanionFault, FanoutFailureDoesNotCloseAReenteredObserverView) {
  if (!VehicleWorldFixture::available()) {
    GTEST_SKIP() << "Requires the verified client catalog and NPC placements";
  }

  VehicleWorldFixture fixture;
  fixture.make_game_master(1);
  OverworldEntry owner_entry;
  OverworldEntry observer_entry;
  fixture.start(0, owner_entry);
  fixture.start(1, observer_entry);
  auto &owner = fixture.client(0);
  auto &observer = fixture.client(1);
  observer.drain();
  const auto ileste = npc_unit_id(owner_entry, 1001021);
  owner.send(20065, {{"target_unit_id", ileste}, {"pos", world_pos_json(kNearIleste)},
                     {"yaw", 0.0}});
  owner.send(20067, {{"dialogue_cms_id", 100117}, {"sequence_order", 1},
                     {"select_order", 1}});
  owner.send(20105, nlohmann::json::object());

  ScopedFanoutPause pause;
  arm_companion_fanout_allocation_fault_for_testing();
  owner.send_without_barrier(
      20106, {{"add_goods", nlohmann::json::array(
                               {{{"good_count", {{"trade_good_cms_id", 1102}, {"count", 2}}}}})},
              {"wagon_cms_id", 200}, {"start", true}});
  ASSERT_TRUE(wait_companion_fanout_pause_for_testing());
  ASSERT_TRUE(companion_fanout_allocation_fault_observed_for_testing());

  ASSERT_EQ(frames_of(observer.send(20009, {{"text", "tp_map 10111"}}), 175).size(), 1U);
  ASSERT_EQ(frames_of(observer.send(20085, nlohmann::json::object()), 176).size(), 1U);
  observer.send(20086, nlohmann::json::object());
  ASSERT_EQ(frames_of(observer.send(20009, {{"text", "tp_map 9012 -1486 3371 -601.67"}}),
                      175).size(), 1U);
  ASSERT_EQ(frames_of(observer.send(20085, nlohmann::json::object()), 176).size(), 1U);
  observer.send(20086, nlohmann::json::object());
  release_companion_fanout_pause_for_testing();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!companion_fanout_old_view_skipped_for_testing() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(companion_fanout_old_view_skipped_for_testing());
  EXPECT_NO_THROW(observer.drain());
}

} // namespace
} // namespace chronicles
