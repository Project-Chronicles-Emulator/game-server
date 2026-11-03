#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>

namespace chronicles {

// A stale handler must not clear a newer retry after an uncertain scheduling failure.
class RetainedRetryGate final {
public:
  std::optional<std::uint64_t> arm() noexcept {
    if (active_.load() != 0) {
      return std::nullopt;
    }
    auto sequence = next_.load();
    for (;;) {
      if (sequence == 0 || sequence == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::nullopt;
      }
      if (next_.compare_exchange_weak(sequence, sequence + 1)) {
        break;
      }
    }
    std::uint64_t idle = 0;
    if (!active_.compare_exchange_strong(idle, sequence)) {
      return std::nullopt;
    }
    return sequence;
  }

  bool is_active(std::uint64_t sequence) const noexcept {
    return sequence != 0 && active_.load() == sequence;
  }

  bool finish(std::uint64_t sequence) noexcept {
    return sequence != 0 && active_.compare_exchange_strong(sequence, 0);
  }

  bool armed() const noexcept {
    return active_.load() != 0;
  }

private:
  std::atomic<std::uint64_t> next_{1};
  std::atomic<std::uint64_t> active_{};
};

} // namespace chronicles
