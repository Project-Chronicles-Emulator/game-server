#pragma once

#include <atomic>
#include <utility>

namespace chronicles {

class QuestEffectSchedulerState {
public:
  void stop() noexcept {
    stopping_.store(true);
  }

  void fail() noexcept {
    failed_.store(true);
    stopping_.store(true);
  }

  bool stopping() const noexcept {
    return stopping_.load();
  }

  bool failed() const noexcept {
    return failed_.load();
  }

  int exit_code() const noexcept {
    return failed() ? 1 : 0;
  }

private:
  std::atomic<bool> stopping_{};
  std::atomic<bool> failed_{};
};

// A scheduling failure must stop the server: otherwise durable effects could remain pending
// indefinitely while the process continues admitting players.
template <typename Schedule, typename FailClosed>
bool try_schedule_quest_effect_step(Schedule &&schedule, FailClosed &&fail_closed) {
  try {
    std::forward<Schedule>(schedule)();
    return true;
  } catch (...) {
    std::forward<FailClosed>(fail_closed)();
    return false;
  }
}

template <typename FailClosed>
bool quest_effect_wait_can_continue(bool has_error, bool operation_aborted, bool stopping,
                                    FailClosed &&fail_closed) {
  if (!has_error) {
    return !stopping;
  }
  if (operation_aborted && stopping) {
    return false;
  }
  std::forward<FailClosed>(fail_closed)();
  return false;
}

// A failed drain start may already have fenced admission. Keep the request retryable;
// the caller must not stop its effect timer until the drain's completion runs.
template <typename Start, typename OnFailure>
bool try_start_drained_shutdown(bool &requested, Start &&start, OnFailure &&on_failure) {
  if (requested) {
    return true;
  }
  requested = true;
  try {
    std::forward<Start>(start)();
    return true;
  } catch (...) {
    requested = false;
    std::forward<OnFailure>(on_failure)();
    return false;
  }
}

} // namespace chronicles
