#include "testing/memory_repositories.hpp"
#include "world/session/native_world.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace chronicles {

// This symbol is defined only by the separately compiled fault-test module.
void arm_shutdown_start_fault_for_testing() noexcept;

namespace {

TEST(NativeShutdownFault, FailedStartAfterListenerStopCanBeRetried) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  native_testing::TemporaryWorldCertificate certificate;
  boost::asio::thread_pool workers(1);
  boost::asio::io_context io;
  auto listener = start_native_world(
      io, workers, auth,
      boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0),
      CHRONICLES_PROTOCOL_DIR, certificate.certificate(), certificate.private_key(),
      std::chrono::milliseconds::zero(), false, false, false, {}, {}, {}, {}, {}, {}, {},
      {}, {}, {}, 8);
  const auto port = listener->port();

  arm_shutdown_start_fault_for_testing();
  EXPECT_THROW(listener->shutdown_when_drained([] {}), std::runtime_error);
  EXPECT_FALSE(listener->shutdown_started());
  boost::asio::ip::tcp::socket socket(io);
  boost::system::error_code connect_error;
  socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), port),
                 connect_error);
  EXPECT_TRUE(connect_error);

  auto drained = std::make_shared<std::promise<void>>();
  auto completion = drained->get_future();
  listener->shutdown_when_drained([drained] { drained->set_value(); });
  std::thread io_thread([&] { io.run(); });
  EXPECT_EQ(completion.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  io.stop();
  io_thread.join();
  workers.join();
  listener.reset();
}

} // namespace
} // namespace chronicles
