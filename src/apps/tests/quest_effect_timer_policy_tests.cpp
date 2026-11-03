#include "apps/quest_effect_timer_policy.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>

namespace chronicles {
namespace {

TEST(QuestEffectTimerPolicy, SchedulingFailuresStopInsteadOfDroppingTheRetry) {
  for (const auto failed_step : {0, 1, 2}) {
    int scheduled_steps = 0;
    int stops = 0;
    bool pending = true;
    for (int step = 0; step < 3; ++step) {
      const auto scheduled = try_schedule_quest_effect_step(
          [&] {
            if (step == failed_step) {
              throw std::runtime_error("Injected timer or executor failure");
            }
            ++scheduled_steps;
          },
          [&] { ++stops; });
      if (!scheduled) {
        break;
      }
      if (step == 2) {
        pending = false;
      }
    }
    EXPECT_EQ(stops, 1);
    EXPECT_EQ(scheduled_steps, failed_step);
    EXPECT_TRUE(pending);
  }
}

TEST(QuestEffectTimerPolicy, FatalFailureSurvivesShutdownAndReturnsNonzero) {
  QuestEffectSchedulerState state;
  EXPECT_EQ(state.exit_code(), 0);
  state.fail();
  EXPECT_TRUE(state.stopping());
  EXPECT_TRUE(state.failed());
  state.stop();
  EXPECT_EQ(state.exit_code(), 1);
  QuestEffectSchedulerState normal_shutdown;
  normal_shutdown.stop();
  EXPECT_EQ(normal_shutdown.exit_code(), 0);
}

TEST(QuestEffectTimerPolicy, UnexpectedWaitErrorsStopButShutdownCancellationDoesNot) {
  int stops = 0;
  const auto fail_closed = [&] { ++stops; };
  EXPECT_TRUE(quest_effect_wait_can_continue(false, false, false, fail_closed));
  EXPECT_FALSE(quest_effect_wait_can_continue(true, false, false, fail_closed));
  EXPECT_FALSE(quest_effect_wait_can_continue(true, true, false, fail_closed));
  EXPECT_EQ(stops, 2);
  EXPECT_FALSE(quest_effect_wait_can_continue(true, true, true, fail_closed));
  EXPECT_FALSE(quest_effect_wait_can_continue(false, false, true, fail_closed));
  EXPECT_EQ(stops, 2);
}

TEST(QuestEffectTimerPolicy, PartialDrainStartFailureKeepsTimerRunningAndAllowsRetry) {
  QuestEffectSchedulerState timer;
  bool requested = false;
  bool admission_fenced = false;
  bool listener_open = true;
  int retries = 0;
  std::function<void()> completion;

  const auto failed = try_start_drained_shutdown(
      requested,
      [&] {
        admission_fenced = true;
        listener_open = false;
        throw std::runtime_error("injected drain-thread creation failure");
      },
      [&] { ++retries; });
  EXPECT_FALSE(failed);
  EXPECT_FALSE(requested);
  EXPECT_TRUE(admission_fenced);
  EXPECT_FALSE(listener_open);
  EXPECT_FALSE(timer.stopping());
  EXPECT_EQ(retries, 1);

  const auto started = try_start_drained_shutdown(
      requested, [&] { completion = [&] { timer.stop(); }; }, [&] { ++retries; });
  EXPECT_TRUE(started);
  EXPECT_TRUE(requested);
  EXPECT_FALSE(timer.stopping());
  ASSERT_TRUE(completion);
  completion();
  EXPECT_TRUE(timer.stopping());
  EXPECT_EQ(retries, 1);
}

} // namespace
} // namespace chronicles
