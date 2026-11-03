#include "world/core/retained_owner_supervisor.hpp"

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace chronicles {
namespace {

using namespace std::chrono_literals;

static_assert(noexcept(std::declval<RetainedOwnerSupervisor &>().activate_owner({1, 1})));
static_assert(noexcept(std::declval<RetainedOwnerSupervisor &>().cancel_reservation({1, 1})));

RetainedOwnerSupervisor::Options test_options() {
  RetainedOwnerSupervisor::Options options;
  options.capacity = 2;
  options.page_size = 2;
  options.poll_interval = 5ms;
  options.initial_backoff = 5ms;
  options.maximum_backoff = 20ms;
  options.overdue_after = 100ms;
  options.scheduler_stall_after = 100ms;
  return options;
}

template <typename Predicate> bool eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

TEST(RetainedOwnerSupervisor, DormantReservationRequiresExplicitActivationOrRollback) {
  std::atomic<int> attempts{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        ++attempts;
        complete(RetainedOwnerOutcome::settled);
        return RetainedOwnerSubmit::accepted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  const RetainedOwnerKey cancelled{20, 1};
  const RetainedOwnerKey activated{21, 1};
  EXPECT_TRUE(supervisor.reserve_owner(cancelled));
  EXPECT_TRUE(supervisor.reserve_owner(activated));
  EXPECT_FALSE(supervisor.reserve_owner(activated));
  EXPECT_EQ(supervisor.health().dormant, 2U);
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(attempts.load(), 0);
  EXPECT_FALSE(supervisor.stop());
  EXPECT_TRUE(supervisor.cancel_reservation(cancelled));
  EXPECT_FALSE(supervisor.activate_owner(cancelled));
  EXPECT_TRUE(supervisor.activate_owner(activated));
  EXPECT_TRUE(eventually([&] { return attempts == 1 && supervisor.health().retained == 0; }));
  EXPECT_FALSE(supervisor.cancel_reservation(activated));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, ActiveOwnerIsNotDelayedBy4096DormantReservations) {
  auto options = test_options();
  options.capacity = 4097;
  options.page_size = 1;
  std::atomic<int> attempts{};
  RetainedOwnerSupervisor supervisor(
      options,
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        ++attempts;
        complete(RetainedOwnerOutcome::settled);
        return RetainedOwnerSubmit::accepted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  for (std::int64_t id = 1; id <= 4097; ++id) {
    EXPECT_TRUE(supervisor.reserve_owner({id, 1}));
  }
  EXPECT_FALSE(supervisor.reserve_owner({4098, 1}));
  EXPECT_TRUE(supervisor.activate_owner({4097, 1}));
  EXPECT_TRUE(eventually([&] { return attempts == 1 && supervisor.health().retained == 4096; }));
  EXPECT_EQ(supervisor.health().dormant, 4096U);
  EXPECT_FALSE(supervisor.stop());
  for (std::int64_t id = 1; id <= 4096; ++id) {
    EXPECT_TRUE(supervisor.cancel_reservation({id, 1}));
  }
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, SupportsConfiguredOneHundredThousandConnectionCeiling) {
  auto options = test_options();
  options.capacity = 100000;
  RetainedOwnerSupervisor supervisor(
      options,
      [](RetainedOwnerKey, RetainedOwnerSupervisor::Completion) {
        return RetainedOwnerSubmit::definitely_not_posted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  for (std::int64_t id = 1; id <= 100000; ++id) {
    EXPECT_TRUE(supervisor.reserve_owner({id, 1}));
  }
  EXPECT_EQ(supervisor.health().dormant, 100000U);
  EXPECT_FALSE(supervisor.reserve_owner({100001, 1}));
  for (std::int64_t id = 1; id <= 100000; ++id) {
    EXPECT_TRUE(supervisor.cancel_reservation({id, 1}));
  }
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, RetriesDefiniteNoPostAndRetiresOnlyOnCompletion) {
  std::atomic<int> attempts{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        if (++attempts == 1) {
          return RetainedOwnerSubmit::definitely_not_posted;
        }
        complete(RetainedOwnerOutcome::settled);
        return RetainedOwnerSubmit::accepted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  EXPECT_TRUE(supervisor.reserve_owner({1, 1}));
  EXPECT_TRUE(supervisor.activate_owner({1, 1}));
  EXPECT_TRUE(eventually([&] { return attempts.load() >= 2 && supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, PostExceptionRetainsOwnerUntilProvenCancellation) {
  std::atomic<bool> allow_post{};
  std::atomic<int> alarms{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        if (!allow_post) {
          boost::asio::any_io_executor invalid_executor;
          boost::asio::post(invalid_executor, [] {});
        }
        complete(RetainedOwnerOutcome::settled);
        return RetainedOwnerSubmit::accepted;
      },
      [&](RetainedOwnerAlarm reason, RetainedOwnerKey) {
        if (reason == RetainedOwnerAlarm::post_outcome_unknown) {
          ++alarms;
        }
      });
  const RetainedOwnerKey key{2, 1};
  EXPECT_TRUE(supervisor.reserve_owner(key));
  EXPECT_TRUE(supervisor.activate_owner(key));
  EXPECT_TRUE(eventually([&] { return supervisor.health().uncertain == 1 && alarms > 0; }));
  EXPECT_FALSE(supervisor.stop());
  allow_post = true;
  EXPECT_TRUE(supervisor.confirm_not_posted(key));
  EXPECT_TRUE(eventually([&] { return supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, StoppedExecutorIsReportedWithoutDuplicateSubmission) {
  boost::asio::io_context io;
  io.stop();
  std::atomic<int> posts{};
  std::atomic<int> overdue{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        ++posts;
        boost::asio::post(io, [complete = std::move(complete)] {
          complete(RetainedOwnerOutcome::settled);
        });
        return RetainedOwnerSubmit::accepted;
      },
      [&](RetainedOwnerAlarm reason, RetainedOwnerKey) {
        if (reason == RetainedOwnerAlarm::attempt_overdue) {
          ++overdue;
        }
      });
  EXPECT_TRUE(supervisor.reserve_owner({3, 1}));
  EXPECT_TRUE(supervisor.activate_owner({3, 1}));
  EXPECT_TRUE(eventually([&] { return overdue > 0; }));
  EXPECT_EQ(posts.load(), 1);
  EXPECT_EQ(supervisor.health().retained, 1U);
  EXPECT_EQ(supervisor.health().overdue, 1U);
  EXPECT_FALSE(supervisor.cancel_reservation({3, 1}));
  EXPECT_FALSE(supervisor.stop());
  io.restart();
  io.run();
  EXPECT_TRUE(eventually([&] { return supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, CancelledAsioTimerRetriesOnlyAfterCancellationHandlerRuns) {
  boost::asio::io_context io;
  std::mutex mutex;
  std::shared_ptr<boost::asio::steady_timer> timer;
  std::atomic<int> attempts{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        if (++attempts == 1) {
          auto waiting = std::make_shared<boost::asio::steady_timer>(io, 1h);
          waiting->async_wait([waiting, complete = std::move(complete)](
                                  const boost::system::error_code &error) {
            complete(error == boost::asio::error::operation_aborted
                         ? RetainedOwnerOutcome::retry
                         : RetainedOwnerOutcome::settled);
          });
          std::lock_guard lock(mutex);
          timer = std::move(waiting);
        } else {
          complete(RetainedOwnerOutcome::settled);
        }
        return RetainedOwnerSubmit::accepted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  EXPECT_TRUE(supervisor.reserve_owner({4, 1}));
  EXPECT_TRUE(supervisor.activate_owner({4, 1}));
  EXPECT_TRUE(eventually([&] {
    std::lock_guard lock(mutex);
    return static_cast<bool>(timer);
  }));
  {
    std::lock_guard lock(mutex);
    if (timer) {
      timer->cancel();
    }
  }
  EXPECT_FALSE(supervisor.stop());
  io.run();
  EXPECT_TRUE(eventually([&] { return attempts >= 2 && supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, WaitFailureIsReportedAndSchedulerContinues) {
  std::atomic<int> wait_failures{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        complete(RetainedOwnerOutcome::settled);
        return RetainedOwnerSubmit::accepted;
      },
      [&](RetainedOwnerAlarm reason, RetainedOwnerKey) {
        if (reason == RetainedOwnerAlarm::wait_failed) {
          ++wait_failures;
        }
      });
  supervisor.inject_wait_failure_for_testing();
  EXPECT_TRUE(eventually([&] { return wait_failures > 0; }));
  EXPECT_TRUE(supervisor.health().wait_failure_seen);
  EXPECT_TRUE(supervisor.reserve_owner({5, 1}));
  EXPECT_TRUE(supervisor.activate_owner({5, 1}));
  EXPECT_TRUE(eventually([&] { return supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, AlarmFailureRemainsVisibleInHealth) {
  std::atomic<bool> retry_safe{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        if (!retry_safe) {
          throw std::runtime_error("post failed");
        }
        complete(RetainedOwnerOutcome::settled);
        return RetainedOwnerSubmit::accepted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {
        throw std::runtime_error("alarm unavailable");
      });
  const RetainedOwnerKey key{13, 1};
  EXPECT_TRUE(supervisor.reserve_owner(key));
  EXPECT_TRUE(supervisor.activate_owner(key));
  EXPECT_TRUE(eventually([&] { return supervisor.health().alarm_delivery_failed; }));
  EXPECT_EQ(supervisor.health().uncertain, 1U);
  retry_safe = true;
  EXPECT_TRUE(supervisor.confirm_not_posted(key));
  EXPECT_TRUE(eventually([&] { return supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, HealthRemainsAvailableWhileAlarmCallbackIsBlocked) {
  std::atomic<bool> retry_safe{};
  std::atomic<bool> alarm_entered{};
  std::atomic<bool> release_alarm{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        if (!retry_safe) {
          throw std::runtime_error("post failed");
        }
        complete(RetainedOwnerOutcome::settled);
        return RetainedOwnerSubmit::accepted;
      },
      [&](RetainedOwnerAlarm, RetainedOwnerKey) {
        alarm_entered = true;
        while (!release_alarm) {
          std::this_thread::sleep_for(5ms);
        }
      });
  const RetainedOwnerKey key{14, 1};
  EXPECT_TRUE(supervisor.reserve_owner(key));
  EXPECT_TRUE(supervisor.activate_owner(key));
  EXPECT_TRUE(eventually([&] {
    const auto health = supervisor.health();
    return alarm_entered && health.alarm_stalled && health.scheduler_stalled &&
           health.uncertain == 1 && health.overdue == 1;
  }));
  retry_safe = true;
  release_alarm = true;
  EXPECT_TRUE(supervisor.confirm_not_posted(key));
  EXPECT_TRUE(eventually([&] { return supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, CapacityAndGenerationKeepDistinctOwnersRetained) {
  std::mutex mutex;
  std::map<RetainedOwnerKey, RetainedOwnerSupervisor::Completion> completions;
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey key, RetainedOwnerSupervisor::Completion complete) {
        std::lock_guard lock(mutex);
        completions.emplace(key, std::move(complete));
        return RetainedOwnerSubmit::accepted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  const RetainedOwnerKey first{10, 1};
  const RetainedOwnerKey second{10, 2};
  EXPECT_TRUE(supervisor.reserve_owner(first));
  EXPECT_TRUE(supervisor.activate_owner(first));
  EXPECT_TRUE(supervisor.reserve_owner(second));
  EXPECT_TRUE(supervisor.activate_owner(second));
  EXPECT_FALSE(supervisor.reserve_owner({11, 1}));
  EXPECT_TRUE(eventually([&] {
    std::lock_guard lock(mutex);
    return completions.size() == 2;
  }));
  {
    std::lock_guard lock(mutex);
    for (const auto &[key, complete] : completions) {
      complete(RetainedOwnerOutcome::settled);
    }
  }
  EXPECT_TRUE(eventually([&] { return supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, StaleCompletionCannotRetireNewAttempt) {
  std::mutex mutex;
  std::vector<RetainedOwnerSupervisor::Completion> completions;
  std::atomic<int> overdue{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        std::lock_guard lock(mutex);
        completions.push_back(std::move(complete));
        return RetainedOwnerSubmit::accepted;
      },
      [&](RetainedOwnerAlarm reason, RetainedOwnerKey) {
        if (reason == RetainedOwnerAlarm::attempt_overdue) {
          ++overdue;
        }
      });
  const RetainedOwnerKey key{12, 1};
  EXPECT_TRUE(supervisor.reserve_owner(key));
  EXPECT_TRUE(supervisor.activate_owner(key));
  EXPECT_TRUE(eventually([&] { return overdue > 0; }));
  // In production this requires independent proof that the first post can never execute.
  EXPECT_TRUE(supervisor.confirm_not_posted(key));
  EXPECT_TRUE(eventually([&] {
    std::lock_guard lock(mutex);
    return completions.size() == 2;
  }));
  {
    std::lock_guard lock(mutex);
    if (completions.size() == 2) {
      completions[0](RetainedOwnerOutcome::settled);
      EXPECT_EQ(supervisor.health().retained, 1U);
      completions[1](RetainedOwnerOutcome::settled);
    }
  }
  EXPECT_TRUE(eventually([&] { return supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, ABlockedOwnerDoesNotStarveTheNextPage) {
  auto options = test_options();
  options.page_size = 1;
  std::mutex mutex;
  RetainedOwnerSupervisor::Completion blocked;
  std::atomic<bool> other_settled{};
  RetainedOwnerSupervisor supervisor(
      options,
      [&](RetainedOwnerKey key, RetainedOwnerSupervisor::Completion complete) {
        if (key.character_id == 1) {
          std::lock_guard lock(mutex);
          blocked = std::move(complete);
        } else {
          other_settled = true;
          complete(RetainedOwnerOutcome::settled);
        }
        return RetainedOwnerSubmit::accepted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  EXPECT_TRUE(supervisor.reserve_owner({1, 1}));
  EXPECT_TRUE(supervisor.activate_owner({1, 1}));
  EXPECT_TRUE(supervisor.reserve_owner({2, 1}));
  EXPECT_TRUE(supervisor.activate_owner({2, 1}));
  EXPECT_TRUE(eventually([&] { return other_settled.load(); }));
  EXPECT_EQ(supervisor.health().retained, 1U);
  {
    std::lock_guard lock(mutex);
    if (blocked) {
      blocked(RetainedOwnerOutcome::settled);
    }
  }
  EXPECT_TRUE(eventually([&] { return supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

TEST(RetainedOwnerSupervisor, WatchdogReportsBlockedSchedulerAndRecoversAfterRelease) {
  std::atomic<bool> entered{};
  std::atomic<bool> release{};
  std::atomic<int> stalled{};
  RetainedOwnerSupervisor supervisor(
      test_options(),
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion complete) {
        entered = true;
        while (!release) {
          std::this_thread::sleep_for(5ms);
        }
        complete(RetainedOwnerOutcome::settled);
        return RetainedOwnerSubmit::accepted;
      },
      [&](RetainedOwnerAlarm reason, RetainedOwnerKey) {
        if (reason == RetainedOwnerAlarm::scheduler_stalled) {
          ++stalled;
        }
      });
  EXPECT_TRUE(supervisor.reserve_owner({6, 1}));
  EXPECT_TRUE(supervisor.activate_owner({6, 1}));
  EXPECT_TRUE(eventually([&] { return entered && stalled > 0; }));
  EXPECT_FALSE(supervisor.stop());
  release = true;
  EXPECT_TRUE(eventually([&] { return supervisor.health().retained == 0; }));
  EXPECT_TRUE(supervisor.stop());
}

} // namespace
} // namespace chronicles
