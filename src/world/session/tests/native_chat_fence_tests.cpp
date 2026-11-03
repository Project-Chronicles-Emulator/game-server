#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace chronicles {
namespace {

using namespace native_testing;

class UncertainCloseSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
  explicit UncertainCloseSink(std::shared_ptr<std::atomic<bool>> observed)
      : observed_(std::move(observed)) {
  }

protected:
  void sink_it_(const spdlog::details::log_msg &message) override {
    const std::string_view text(message.payload.data(), message.payload.size());
    if (text.find("Close hook outcome is uncertain") != std::string_view::npos) {
      observed_->store(true);
    }
  }

  void flush_() override {
  }

private:
  std::shared_ptr<std::atomic<bool>> observed_;
};

// This child intentionally retains a fenced session. The process must end without asking the
// listener to discard its uncertain owner; the OS closes its disposable sockets and threads.
[[noreturn]] void finish_chat_fence_child(int code, OverworldSessions *world) {
  if (world) {
    const auto directory = world->certificate.certificate().parent_path();
    std::error_code error;
    const auto temp_directory = std::filesystem::temp_directory_path(error);
    const auto resolved_directory = std::filesystem::canonical(directory, error);
    if (!error) {
      const auto resolved_temp = std::filesystem::canonical(temp_directory, error);
      if (!error && resolved_directory.parent_path() == resolved_temp &&
          resolved_directory.filename().string().starts_with("chronicles-world-test-")) {
        std::filesystem::remove_all(resolved_directory, error);
      }
    }
  }
  if (code != 0) {
    std::fprintf(stderr, "Chat departure fence child failed at check %d\n", code);
    std::fflush(stderr);
  }
  std::_Exit(code);
}

[[noreturn]] void exercise_uncertain_chat_departure() {
  OverworldSessions *world = nullptr;
  try {
    auto uncertain = std::make_shared<std::atomic<bool>>(false);
    auto logger = std::make_shared<spdlog::logger>("chat-fence-test",
                                                   std::make_shared<UncertainCloseSink>(uncertain));
    spdlog::set_default_logger(std::move(logger));
    NpcPlacementSet placements;
    placements.map_id = kOverworldMapId;
    auto population = std::make_shared<NpcPopulation>(std::move(placements), 1024);
    world = new OverworldSessions(population, {kOverworldSpawn});
    auto entries = std::make_shared<std::atomic<unsigned>>(0);
    auto exits = std::make_shared<std::atomic<unsigned>>(0);
    world->server->listener().set_world_entry_handler([entries](const ChatIdentity &) {
      ++*entries;
    });
    world->server->listener().set_world_exit_handler([exits](const ChatIdentity &) {
      ++*exits;
      throw std::runtime_error("Injected uncertain external departure");
    });

    auto &session = world->sessions[0];
    world->enter(session);
    VehicleClient client(*world, session, 3);
    client.send(20086, nlohmann::json::object());
    if (entries->load() != 1) {
      finish_chat_fence_child(1, world);
    }
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(session.character_id);
    const auto ticket = world->auth->select(session.access_token, select);
    if (ticket.ec() != x5::ec::Success) {
      finish_chat_fence_child(2, world);
    }

    session.client->disconnect();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!uncertain->load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!uncertain->load() || exits->load() != 1) {
      finish_chat_fence_child(3, world);
    }

    WorldTestClient attempt(world->server->port());
    attempt.send_key(ticket.packet_key());
    try {
      static_cast<void>(attempt.read());
      finish_chat_fence_child(4, world);
    } catch (const boost::system::system_error &error) {
      if (error.code() != asio::error::eof && error.code() != asio::error::connection_reset &&
          error.code() != ssl::error::stream_truncated) {
        finish_chat_fence_child(7, world);
      }
    }

    auto drained = std::make_shared<std::promise<void>>();
    auto completion = drained->get_future();
    world->server->listener().shutdown_when_drained([drained] {
      drained->set_value();
    });
    if (completion.wait_for(std::chrono::seconds(1)) != std::future_status::timeout ||
        world->server->listener().shutdown_finished() || exits->load() != 1) {
      finish_chat_fence_child(5, world);
    }
    finish_chat_fence_child(0, world);
  } catch (...) {
    finish_chat_fence_child(6, world);
  }
}

TEST(NativeChatFence, UncertainDepartureRetainsAdmissionAndBlocksShutdown) {
  EXPECT_EXIT(exercise_uncertain_chat_departure(), ::testing::ExitedWithCode(0), "");
}

} // namespace
} // namespace chronicles
