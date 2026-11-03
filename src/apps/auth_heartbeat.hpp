#pragma once

#include "auth/auth.hpp"

#include <atomic>
#include <boost/asio.hpp>
#include <memory>

namespace chronicles {

// One worker operation at a time. A login outage makes new realm admissions fail
// closed after five seconds without stopping already running world simulation.
class AuthHeartbeat : public std::enable_shared_from_this<AuthHeartbeat> {
public:
  AuthHeartbeat(boost::asio::io_context &context, boost::asio::thread_pool &workers,
                AuthService &auth);

  void start();
  void stop();

private:
  void schedule(std::chrono::milliseconds delay);
  void publish();

  boost::asio::steady_timer timer_;
  boost::asio::thread_pool &workers_;
  AuthService &auth_;
  std::atomic<bool> stopping_{};
  bool previously_available_{true};
};

} // namespace chronicles
