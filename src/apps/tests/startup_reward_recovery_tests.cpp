#include "apps/startup_reward_recovery.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace chronicles {
namespace {

TEST(StartupRewardRecovery, RecoversBelowAndExactlyAtLimit) {
  for (const std::size_t initial : {std::size_t{4}, std::size_t{5}}) {
    auto staged = initial;
    const auto recovered = recover_startup_rewards(
        2, 5,
        [&staged](std::size_t requested) {
          const auto count = std::min(staged, requested);
          staged -= count;
          return count;
        },
        [&staged] { return staged != 0; });
    EXPECT_EQ(recovered, initial);
    EXPECT_EQ(staged, 0U);
  }
}

TEST(StartupRewardRecovery, OverflowFailsWithoutRecoveringPastLimit) {
  std::size_t staged = 6;
  EXPECT_THROW(
      recover_startup_rewards(
          2, 5,
          [&staged](std::size_t requested) {
            const auto count = std::min(staged, requested);
            staged -= count;
            return count;
          },
          [&staged] { return staged != 0; }),
      std::runtime_error);
  EXPECT_EQ(staged, 1U);
}

} // namespace
} // namespace chronicles
