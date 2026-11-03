#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace chronicles {

// Shared with detached gathering recovery so the same corpse cannot be butchered twice
// while the owning world session is closing or a storage acknowledgement is uncertain.
class CorpseHandlingState final {
public:
  explicit CorpseHandlingState(std::string incarnation) : incarnation_(std::move(incarnation)) {
    if (incarnation_.empty()) {
      throw std::invalid_argument("Corpse incarnation is missing");
    }
  }

  const std::string &incarnation() const noexcept {
    return incarnation_;
  }

  bool available() const {
    std::lock_guard lock(mutex_);
    return !consumed_ && !reservation_;
  }

  std::optional<std::uint64_t> reserve() {
    std::lock_guard lock(mutex_);
    if (consumed_ || reservation_) {
      return std::nullopt;
    }
    if (next_token_ == 0) {
      throw std::overflow_error("Corpse handling reservation tokens are exhausted");
    }
    reservation_ = next_token_++;
    return reservation_;
  }

  bool finish(std::uint64_t token, bool committed) {
    std::lock_guard lock(mutex_);
    if (!reservation_ || *reservation_ != token) {
      return false;
    }
    consumed_ = committed;
    reservation_.reset();
    return true;
  }

private:
  const std::string incarnation_;
  mutable std::mutex mutex_;
  std::optional<std::uint64_t> reservation_;
  std::uint64_t next_token_{1};
  bool consumed_{};
};

} // namespace chronicles
