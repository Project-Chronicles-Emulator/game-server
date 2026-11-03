#pragma once

#include "world/session/tests/native_world_test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <future>

namespace chronicles::native_testing {

// These tests deliberately leave an uncertain offline write fenced forever. They admit no
// clients and cannot use the ordinary fixture's successful-drain teardown contract.
class OfflineQuarantineTestServer {
public:
  OfflineQuarantineTestServer(AuthService &auth, const TemporaryWorldCertificate &certificate)
      : workers_(1),
        listener_(start_native_world(
            io_context_, workers_, auth, tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            CHRONICLES_PROTOCOL_DIR, certificate.certificate(), certificate.private_key())),
        io_thread_([this] { io_context_.run(); }) {
  }

  ~OfflineQuarantineTestServer() {
    if (!listener_->connections().empty()) {
      std::fputs("Offline quarantine fixture must not admit world clients\n", stderr);
      std::abort();
    }
    // This models ending an isolated process with an unresolved quarantine. It does not
    // clear that fence, report a successful drain, or provide production recovery.
    listener_->stop();
    io_context_.stop();
    io_thread_.join();
    listener_.reset();
    workers_.join();
  }

  NativeWorldListener &listener() {
    return *listener_;
  }

  bool shutdown_remains_blocked() {
    auto completed = std::make_shared<std::promise<void>>();
    auto result = completed->get_future();
    listener_->shutdown_when_drained([completed] { completed->set_value(); });
    return result.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
  }

private:
  // Workers outlive the I/O context whose pending handlers can own worker strands.
  asio::thread_pool workers_;
  asio::io_context io_context_;
  std::shared_ptr<NativeWorldListener> listener_;
  std::thread io_thread_;
};

} // namespace chronicles::native_testing
