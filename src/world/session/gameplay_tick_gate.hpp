#pragma once

#include <cstdint>
#include <optional>

namespace chronicles {

// Owned by the session's I/O executor. A fired timer remains in flight until its worker
// result returns to I/O, so slow frames cannot enqueue more ticks for the same overdue work.
class GameplayTickGate {
public:
  bool reschedule(std::optional<std::int64_t> deadline) noexcept {
    // The pending worker shares the gameplay strand with frame mutations and will compute
    // the next deadline from that current state. Its completion owns the next timer arm.
    if (in_flight_ || deadline == scheduled_deadline_) {
      return false;
    }

    scheduled_deadline_ = deadline;
    ++generation_;
    return true;
  }

  std::uint64_t generation() const noexcept {
    return generation_;
  }

  bool try_start(std::uint64_t generation) noexcept {
    // cancel() cannot retract a successful timer callback that is already ready to run.
    // Such a callback must not consume the replacement timer or enqueue another worker.
    if (in_flight_ || !scheduled_deadline_ || generation != generation_) {
      return false;
    }

    scheduled_deadline_.reset();
    in_flight_ = true;
    return true;
  }

  void complete() noexcept {
    in_flight_ = false;
  }

private:
  std::optional<std::int64_t> scheduled_deadline_;
  std::uint64_t generation_{};
  bool in_flight_{};
};

} // namespace chronicles
