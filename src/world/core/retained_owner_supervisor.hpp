#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace chronicles {

// A generation distinguishes a new owner from callbacks belonging to an older admission.
struct RetainedOwnerKey {
  std::int64_t character_id{};
  std::uint64_t generation{};

  auto operator<=>(const RetainedOwnerKey &) const = default;
};

enum class RetainedOwnerOutcome { retry, settled };
enum class RetainedOwnerSubmit { accepted, definitely_not_posted };
enum class RetainedOwnerAlarm { post_outcome_unknown, attempt_overdue, scheduler_stalled,
                                wait_failed, internal_failure };

// Own this independently of the retained sessions. The registry remains the authority for
// lifetime and admission; its submit callback must capture that registry weakly and post to the
// appropriate worker strand without blocking. The alarm callback must also be nonblocking and
// thread-safe; external monitoring must independently poll health() if alarm delivery stalls.
// Completion may run synchronously. "settled" means
// the exact retained operation and its admission have already been safely retired.
//
// No process termination is implied: an unresolved owner may contain a death plan that exists
// only in memory. A thrown post has an unknown outcome and is quarantined, never blindly retried.
class RetainedOwnerSupervisor {
public:
  using Completion = std::function<void(RetainedOwnerOutcome)>;
  using Submit = std::function<RetainedOwnerSubmit(RetainedOwnerKey, Completion)>;
  using Alarm = std::function<void(RetainedOwnerAlarm, RetainedOwnerKey)>;

  struct Options {
    std::size_t capacity{100000};
    std::size_t page_size{128};
    std::chrono::milliseconds poll_interval{1000};
    std::chrono::milliseconds initial_backoff{1000};
    std::chrono::milliseconds maximum_backoff{30000};
    std::chrono::milliseconds overdue_after{120000};
    std::chrono::milliseconds scheduler_stall_after{30000};
  };

  struct Health {
    std::size_t retained{};
    std::size_t dormant{};
    std::size_t in_flight{};
    std::size_t uncertain{};
    std::size_t overdue{};
    bool scheduler_stalled{};
    bool alarm_stalled{};
    bool wait_failure_seen{};
    bool alarm_delivery_failed{};
  };

  RetainedOwnerSupervisor(Options options, Submit submit, Alarm alarm);
  ~RetainedOwnerSupervisor();
  RetainedOwnerSupervisor(const RetainedOwnerSupervisor &) = delete;
  RetainedOwnerSupervisor &operator=(const RetainedOwnerSupervisor &) = delete;

  // Reserve before admission/gameplay. False means no reservation occurred. Exceptions can
  // result from allocation and must leave the caller's independent owner unmodified.
  bool reserve_owner(RetainedOwnerKey key);

  // Transfers the preallocated node into the active queue without allocation. False leaves
  // the caller responsible for preserving the owner and reporting the failure.
  bool activate_owner(RetainedOwnerKey key) noexcept;

  // Roll back only a still-dormant reservation, without relying on an executor or worker.
  bool cancel_reservation(RetainedOwnerKey key) noexcept;

  // Use only after proving that the unknown/overdue submission can never run. The next attempt
  // receives a new generation, so a stale completion cannot retire it.
  bool confirm_not_posted(RetainedOwnerKey key);
  Health health() const;

  // Refuses while any owner remains, including a cancelled or stalled callback. The destructor
  // likewise waits for owners to settle; callers must keep the registry/executors alive while
  // draining and must not destroy the supervisor on a thread needed to perform completion.
  bool stop();

  // Test-only fault injection for the scheduler's wait path. It does not skip a real owner.
  void inject_wait_failure_for_testing();

private:
  struct State;
  std::shared_ptr<State> state_;
  std::thread scheduler_;
  std::thread watchdog_;
};

} // namespace chronicles
