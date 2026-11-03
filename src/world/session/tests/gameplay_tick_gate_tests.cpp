#include "world/session/gameplay_tick_gate.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

TEST(GameplayTickGate, SlowWorkerKeepsOneTickDespiteRepeatedOverdueFrameDeadlines) {
  GameplayTickGate gate;
  ASSERT_TRUE(gate.reschedule(100));
  const auto fired_generation = gate.generation();
  ASSERT_TRUE(gate.try_start(fired_generation));

  // The timer fired, but its worker is queued behind slow gameplay. Frame completions
  // must not create another timer chain while that worker is pending or still running.
  for (int frame = 0; frame < 1000; ++frame) {
    EXPECT_FALSE(gate.reschedule(100));
    EXPECT_FALSE(gate.reschedule(101 + frame));
    EXPECT_FALSE(gate.try_start(fired_generation));
  }

  gate.complete();
  ASSERT_TRUE(gate.reschedule(2000));
  EXPECT_TRUE(gate.try_start(gate.generation()));
}

TEST(GameplayTickGate, CanceledReadyWaitCannotConsumeItsReplacement) {
  GameplayTickGate gate;
  ASSERT_TRUE(gate.reschedule(100));
  const auto canceled_generation = gate.generation();
  ASSERT_TRUE(gate.reschedule(200));
  const auto replacement_generation = gate.generation();

  // Model a success callback already queued before cancel(), followed by the real wait.
  EXPECT_FALSE(gate.try_start(canceled_generation));
  EXPECT_TRUE(gate.try_start(replacement_generation));
  EXPECT_FALSE(gate.try_start(replacement_generation));
}

TEST(GameplayTickGate, MapLoadingCompletionReleasesPendingWorkWithoutRearmingIt) {
  GameplayTickGate gate;
  ASSERT_TRUE(gate.reschedule(100));
  ASSERT_TRUE(gate.try_start(gate.generation()));
  EXPECT_FALSE(gate.reschedule(std::nullopt));

  // The queued worker sees no current map and returns no deadline. A subsequent map-load
  // frame can start its own timers; a skipped tick must not leave the gate stuck forever.
  gate.complete();
  EXPECT_FALSE(gate.reschedule(std::nullopt));
  ASSERT_TRUE(gate.reschedule(500));
  EXPECT_TRUE(gate.try_start(gate.generation()));
}

TEST(GameplayTickGate, CancelAndIdenticalDeadlineKeepOnlyTheCurrentWait) {
  GameplayTickGate gate;
  EXPECT_FALSE(gate.reschedule(std::nullopt));
  ASSERT_TRUE(gate.reschedule(100));
  const auto generation = gate.generation();
  EXPECT_FALSE(gate.reschedule(100));
  EXPECT_EQ(gate.generation(), generation);

  EXPECT_TRUE(gate.reschedule(std::nullopt));
  EXPECT_FALSE(gate.try_start(generation));
  EXPECT_FALSE(gate.try_start(gate.generation()));
  EXPECT_TRUE(gate.reschedule(100));
  EXPECT_TRUE(gate.try_start(gate.generation()));
}

} // namespace
} // namespace chronicles
