#include "apps/auth_heartbeat.hpp"

#include <spdlog/spdlog.h>

namespace chronicles {

AuthHeartbeat::AuthHeartbeat(boost::asio::io_context &context, boost::asio::thread_pool &workers,
                             AuthService &auth)
    : timer_(boost::asio::make_strand(context)), workers_(workers), auth_(auth) {}

void AuthHeartbeat::start() {
  boost::asio::post(timer_.get_executor(), [self = shared_from_this()] {
    self->schedule(std::chrono::milliseconds(0));
  });
}

void AuthHeartbeat::stop() {
  stopping_.store(true);
  boost::asio::post(timer_.get_executor(), [self = shared_from_this()] {
    self->timer_.cancel();
  });
}

void AuthHeartbeat::schedule(std::chrono::milliseconds delay) {
  if (stopping_.load()) {
    return;
  }
  timer_.expires_after(delay);
  timer_.async_wait([self = shared_from_this()](boost::system::error_code error) {
    if (!error && !self->stopping_.load()) {
      boost::asio::post(self->workers_, [self] {
        self->publish();
      });
    }
  });
}

void AuthHeartbeat::publish() {
  if (stopping_.load()) {
    return;
  }
  bool available{};
  try {
    auth_.publish_world_status();
    available = true;
  } catch (const std::exception &) {
    if (previously_available_) {
      spdlog::warn("Login authority heartbeat unavailable; new realm admissions will close");
    }
  }
  if (available && !previously_available_) {
    spdlog::info("Login authority heartbeat restored");
  }
  previously_available_ = available;
  boost::asio::post(timer_.get_executor(), [self = shared_from_this()] {
    self->schedule(std::chrono::seconds(1));
  });
}

} // namespace chronicles
