#pragma once

#include "world/core/retained_owner_supervisor.hpp"

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

namespace chronicles {

enum class BossWakePhase { pending, running, cancelled };

template <typename OnCancellationDrained>
struct BossWakeState {
  explicit BossWakeState(OnCancellationDrained callback)
      : cancellation_drained(std::move(callback)) {
  }

  ~BossWakeState() noexcept {
    if (phase.load() != BossWakePhase::cancelled) {
      return;
    }
    try {
      cancellation_drained();
    } catch (...) {
      // The supervisor retains the attempt and its watchdog reports the missing completion.
    }
  }

  std::atomic<BossWakePhase> phase{BossWakePhase::pending};
  OnCancellationDrained cancellation_drained;
};

// A throw can follow enqueue. Do not retry until every copy of the canceled handler has
// actually been destroyed: a stalled executor can then retain at most one such handler.
template <typename Post, typename Work, typename Failure, typename OnCancellationDrained>
RetainedOwnerSubmit dispatch_boss_wake(Post &&post, Work &&work, Failure &&failure,
                                       OnCancellationDrained &&on_cancellation_drained) noexcept {

  try {
    using State = BossWakeState<std::decay_t<OnCancellationDrained>>;
    auto state = std::make_shared<State>(std::forward<OnCancellationDrained>(
        on_cancellation_drained));
    auto handler = [state, work = std::forward<Work>(work),
                    failure = std::forward<Failure>(failure)]() mutable noexcept {
      auto expected = BossWakePhase::pending;
      if (!state->phase.compare_exchange_strong(expected, BossWakePhase::running)) {
        return;
      }
      try {
        work();
      } catch (...) {
        try {
          failure();
        } catch (...) {
          // The owner remains in flight; its watchdog reports the missing completion.
        }
      }
    };

    try {
      std::forward<Post>(post)(std::move(handler));
      return RetainedOwnerSubmit::accepted;
    } catch (...) {
      auto expected = BossWakePhase::pending;
      state->phase.compare_exchange_strong(expected, BossWakePhase::cancelled);
      return RetainedOwnerSubmit::accepted;
    }
  } catch (...) {
    return RetainedOwnerSubmit::definitely_not_posted;
  }
}

} // namespace chronicles
