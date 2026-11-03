#include "world/features/combat/combat_timeline.hpp"

#include <gtest/gtest.h>

#include <future>
#include <new>

namespace chronicles::combat {
namespace {

TEST(CombatTimeline, EarlierDeadlineWinsEvenWhenRegisteredLater) {
  CombatTimeline timeline(8);
  const CombatTimeline::Owner first_owner{1, 1};
  const CombatTimeline::Owner second_owner{2, 1};
  const auto later = timeline.enqueue(first_owner, 2300, CombatTimeline::Kind::milestone);
  const auto earlier = timeline.enqueue(second_owner, 1750, CombatTimeline::Kind::milestone);
  ASSERT_TRUE(later.token);
  ASSERT_TRUE(earlier.token);

  const auto blocked = timeline.try_claim(*later.token, 2400);
  EXPECT_EQ(blocked.status, CombatTimeline::ClaimStatus::waiting);
  EXPECT_EQ(blocked.head_owner, second_owner);
  EXPECT_EQ(timeline.try_claim(*earlier.token, 2400).status, CombatTimeline::ClaimStatus::ready);
  ASSERT_TRUE(timeline.begin_milestone(*earlier.token));
  ASSERT_TRUE(timeline.mark_milestone_staged(*earlier.token));
  EXPECT_EQ(timeline.complete(*earlier.token).status, CombatTimeline::EnqueueStatus::accepted);
  EXPECT_EQ(timeline.try_claim(*later.token, 2400).status, CombatTimeline::ClaimStatus::ready);
}

TEST(CombatTimeline, EqualDeadlinesUseRegistrationOrderAndRejectLateEarlierWork) {
  CombatTimeline timeline(4);
  const CombatTimeline::Owner first_owner{1, 1};
  const CombatTimeline::Owner second_owner{2, 1};
  const auto first = timeline.enqueue(first_owner, 100, CombatTimeline::Kind::milestone);
  const auto second = timeline.enqueue(second_owner, 100, CombatTimeline::Kind::input);
  ASSERT_TRUE(first.token);
  ASSERT_TRUE(second.token);

  EXPECT_EQ(timeline.try_claim(*second.token, 100).status, CombatTimeline::ClaimStatus::waiting);
  EXPECT_EQ(timeline.try_claim(*first.token, 99).status, CombatTimeline::ClaimStatus::not_due);
  EXPECT_EQ(timeline.try_claim(*first.token, 100).status, CombatTimeline::ClaimStatus::ready);
  EXPECT_EQ(timeline.enqueue(second_owner, 99, CombatTimeline::Kind::input).status,
            CombatTimeline::EnqueueStatus::stale_tick);
  EXPECT_FALSE(timeline.cancel_queued(*first.token));
  ASSERT_TRUE(timeline.begin_milestone(*first.token));
  ASSERT_TRUE(timeline.mark_milestone_staged(*first.token));
  EXPECT_EQ(timeline.complete(*first.token).status, CombatTimeline::EnqueueStatus::accepted);
  EXPECT_EQ(timeline.try_claim(*second.token, 100).status, CombatTimeline::ClaimStatus::ready);
}

TEST(CombatTimeline, FullQueueCanAtomicallyReplaceAClaimedEventWithItsSuccessor) {
  CombatTimeline timeline(2);
  const CombatTimeline::Owner first_owner{1, 1};
  const CombatTimeline::Owner second_owner{2, 1};
  const auto first = timeline.enqueue(first_owner, 100, CombatTimeline::Kind::milestone);
  const auto second = timeline.enqueue(second_owner, 150, CombatTimeline::Kind::input);
  ASSERT_TRUE(first.token);
  ASSERT_TRUE(second.token);
  EXPECT_EQ(timeline.enqueue(second_owner, 200, CombatTimeline::Kind::input).status,
            CombatTimeline::EnqueueStatus::full);
  ASSERT_EQ(timeline.try_claim(*first.token, 100).status, CombatTimeline::ClaimStatus::ready);
  ASSERT_TRUE(timeline.begin_milestone(*first.token));
  ASSERT_TRUE(timeline.mark_milestone_staged(*first.token));

  const auto rejected = timeline.complete(
      *first.token, CombatTimeline::Successor{99, CombatTimeline::Kind::milestone});
  EXPECT_EQ(rejected.status, CombatTimeline::EnqueueStatus::stale_tick);
  EXPECT_FALSE(timeline.cancel_queued(*first.token));
  const auto completed = timeline.complete(
      *first.token, CombatTimeline::Successor{120, CombatTimeline::Kind::milestone});
  ASSERT_EQ(completed.status, CombatTimeline::EnqueueStatus::accepted);
  ASSERT_TRUE(completed.token);
  EXPECT_EQ(timeline.size(), 2U);
  EXPECT_EQ(timeline.try_claim(*second.token, 200).head_owner, first_owner);
  EXPECT_EQ(timeline.try_claim(*completed.token, 200).status,
            CombatTimeline::ClaimStatus::ready);
}

TEST(CombatTimeline, InputRekeysOrCancelsOnlyItsOwnQueuedMilestone) {
  const CombatTimeline::Owner owner{1, 1};
  const CombatTimeline::Owner other{2, 1};
  for (const auto next_tick : {std::optional<std::int64_t>{120},
                               std::optional<std::int64_t>{180},
                               std::optional<std::int64_t>{}}) {
    SCOPED_TRACE(next_tick.value_or(-1));
    CombatTimeline timeline(3);
    const auto input = timeline.enqueue(owner, 100, CombatTimeline::Kind::input);
    const auto previous = timeline.enqueue(owner, 200, CombatTimeline::Kind::milestone);
    const auto competing = timeline.enqueue(other, 150, CombatTimeline::Kind::milestone);
    ASSERT_TRUE(input.token);
    ASSERT_TRUE(previous.token);
    ASSERT_TRUE(competing.token);
    ASSERT_EQ(timeline.try_claim(*input.token, 100).status,
              CombatTimeline::ClaimStatus::ready);

    auto wrong_owner = *previous.token;
    wrong_owner.owner = other;
    EXPECT_EQ(timeline.complete_input_with_milestone(*input.token, wrong_owner, next_tick).status,
              CombatTimeline::EnqueueStatus::invalid);
    EXPECT_EQ(timeline.complete_input_with_milestone(*input.token, previous.token, 99).status,
              CombatTimeline::EnqueueStatus::stale_tick);
    EXPECT_TRUE(timeline.is_claimed(*input.token));
    EXPECT_EQ(timeline.size(), 3U);

    const auto completed =
        timeline.complete_input_with_milestone(*input.token, previous.token, next_tick);
    ASSERT_EQ(completed.status, CombatTimeline::EnqueueStatus::accepted);
    EXPECT_EQ(completed.token.has_value(), next_tick.has_value());
    EXPECT_EQ(timeline.try_claim(*previous.token, 200).status,
              CombatTimeline::ClaimStatus::stale);
    if (next_tick && *next_tick < 150) {
      ASSERT_TRUE(completed.token);
      EXPECT_EQ(timeline.try_claim(*competing.token, 200).head_owner, owner);
      EXPECT_EQ(timeline.try_claim(*completed.token, 200).status,
                CombatTimeline::ClaimStatus::ready);
    } else {
      EXPECT_EQ(timeline.try_claim(*competing.token, 200).status,
                CombatTimeline::ClaimStatus::ready);
    }
  }
}

TEST(CombatTimeline, UnchangedInputDeadlinePreservesEqualTickRegistrationOrder) {
  CombatTimeline timeline(3);
  const CombatTimeline::Owner owner{1, 1};
  const auto input = timeline.enqueue(owner, 100, CombatTimeline::Kind::input);
  const auto previous = timeline.enqueue(owner, 200, CombatTimeline::Kind::milestone);
  const auto other = timeline.enqueue({2, 1}, 200, CombatTimeline::Kind::milestone);
  ASSERT_TRUE(input.token);
  ASSERT_TRUE(previous.token);
  ASSERT_TRUE(other.token);
  ASSERT_EQ(timeline.try_claim(*input.token, 100).status, CombatTimeline::ClaimStatus::ready);

  const auto completed = timeline.complete_input_with_milestone(*input.token, previous.token, 200);
  ASSERT_EQ(completed.status, CombatTimeline::EnqueueStatus::accepted);
  ASSERT_EQ(completed.token, previous.token);
  EXPECT_EQ(timeline.size(), 2U);
  EXPECT_EQ(timeline.try_claim(*other.token, 200).head_owner, owner);
  EXPECT_EQ(timeline.try_claim(*previous.token, 200).status, CombatTimeline::ClaimStatus::ready);
}

TEST(CombatTimeline, InputCanPublishItsFirstDeadlineEvenAtCapacity) {
  CombatTimeline timeline(2);
  const CombatTimeline::Owner owner{1, 1};
  const auto input = timeline.enqueue(owner, 100, CombatTimeline::Kind::input);
  const auto other = timeline.enqueue({2, 1}, 200, CombatTimeline::Kind::milestone);
  ASSERT_TRUE(input.token);
  ASSERT_TRUE(other.token);
  ASSERT_EQ(timeline.try_claim(*input.token, 100).status, CombatTimeline::ClaimStatus::ready);

  const auto completed = timeline.complete_input_with_milestone(*input.token, std::nullopt, 150);
  ASSERT_EQ(completed.status, CombatTimeline::EnqueueStatus::accepted);
  ASSERT_TRUE(completed.token);
  EXPECT_EQ(timeline.size(), 2U);
  EXPECT_EQ(timeline.try_claim(*other.token, 200).head_owner, owner);
  EXPECT_EQ(timeline.try_claim(*completed.token, 200).status, CombatTimeline::ClaimStatus::ready);
}

TEST(CombatTimeline, AnOwnerCannotCreateOrHideADuplicateMilestone) {
  CombatTimeline timeline(4);
  const CombatTimeline::Owner owner{1, 1};
  const auto input = timeline.enqueue(owner, 100, CombatTimeline::Kind::input);
  const auto previous = timeline.enqueue(owner, 200, CombatTimeline::Kind::milestone);
  ASSERT_TRUE(input.token);
  ASSERT_TRUE(previous.token);
  EXPECT_EQ(timeline.enqueue(owner, 210, CombatTimeline::Kind::milestone).status,
            CombatTimeline::EnqueueStatus::invalid);
  ASSERT_EQ(timeline.try_claim(*input.token, 100).status, CombatTimeline::ClaimStatus::ready);
  EXPECT_EQ(timeline.complete(*input.token).status, CombatTimeline::EnqueueStatus::invalid);
  EXPECT_EQ(timeline.complete_input_with_milestone(*input.token, std::nullopt, 150).status,
            CombatTimeline::EnqueueStatus::invalid);
  EXPECT_TRUE(timeline.is_claimed(*input.token));
  EXPECT_EQ(timeline.size(), 2U);

  const auto completed = timeline.complete_input_with_milestone(*input.token, previous.token, 150);
  ASSERT_EQ(completed.status, CombatTimeline::EnqueueStatus::accepted);
  ASSERT_TRUE(completed.token);
  EXPECT_EQ(timeline.enqueue(owner, 220, CombatTimeline::Kind::milestone).status,
            CombatTimeline::EnqueueStatus::invalid);
  EXPECT_EQ(timeline.try_claim(*completed.token, 200).status,
            CombatTimeline::ClaimStatus::ready);
}

TEST(CombatTimeline, StrictOwnerSealRejectsStaleWorkAndNeverReusesAGeneration) {
  CombatTimeline timeline(2, true);
  const auto owner = timeline.register_owner(7);
  ASSERT_TRUE(owner);
  EXPECT_FALSE(timeline.register_owner(7));
  const auto milestone = timeline.enqueue(*owner, 100, CombatTimeline::Kind::milestone);
  ASSERT_TRUE(milestone.token);
  ASSERT_TRUE(timeline.seal_owner(*owner));
  EXPECT_EQ(timeline.enqueue(*owner, 110, CombatTimeline::Kind::input).status,
            CombatTimeline::EnqueueStatus::invalid);
  EXPECT_FALSE(timeline.retire_owner_if_idle(*owner));
  ASSERT_EQ(timeline.try_claim(*milestone.token, 100).status,
            CombatTimeline::ClaimStatus::ready);
  ASSERT_EQ(timeline.replace_unstarted_milestone(*milestone.token).status,
            CombatTimeline::EnqueueStatus::accepted);
  ASSERT_TRUE(timeline.retire_owner_if_idle(*owner));
  const auto replacement = timeline.register_owner(7);
  ASSERT_TRUE(replacement);
  EXPECT_GT(replacement->generation, owner->generation);
  EXPECT_EQ(timeline.enqueue(*owner, 110, CombatTimeline::Kind::input).status,
            CombatTimeline::EnqueueStatus::invalid);
  EXPECT_EQ(timeline.enqueue(*replacement, 110, CombatTimeline::Kind::input).status,
            CombatTimeline::EnqueueStatus::accepted);
}

TEST(CombatTimeline, SealingAndConcurrentAdmissionCannotLoseAcceptedWork) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    CombatTimeline timeline(2, true);
    const auto owner = timeline.register_owner(7);
    ASSERT_TRUE(owner);
    std::promise<void> start;
    const auto gate = start.get_future().share();
    auto admission = std::async(std::launch::async, [&] {
      gate.wait();
      return timeline.enqueue(*owner, 100, CombatTimeline::Kind::input);
    });
    auto retirement = std::async(std::launch::async, [&] {
      gate.wait();
      const bool sealed = timeline.seal_owner(*owner);
      const bool retired = timeline.retire_owner_if_idle(*owner);
      return std::pair{sealed, retired};
    });
    start.set_value();
    const auto submitted = admission.get();
    const auto [sealed, retired] = retirement.get();
    ASSERT_TRUE(sealed);
    if (submitted.token) {
      EXPECT_EQ(submitted.status, CombatTimeline::EnqueueStatus::accepted);
      EXPECT_FALSE(retired);
      ASSERT_TRUE(timeline.cancel_queued(*submitted.token));
      EXPECT_TRUE(timeline.retire_owner_if_idle(*owner));
    } else {
      EXPECT_EQ(submitted.status, CombatTimeline::EnqueueStatus::invalid);
      EXPECT_TRUE(retired);
    }
    EXPECT_EQ(timeline.enqueue(*owner, 110, CombatTimeline::Kind::input).status,
              CombatTimeline::EnqueueStatus::invalid);
  }
}

TEST(CombatTimeline, FailedInputRekeyKeepsClaimAndOldDeadlineUntilRetry) {
  for (const bool fail_after_entry : {false, true}) {
    SCOPED_TRACE(fail_after_entry ? "after entry insertion" : "before entry insertion");
    CombatTimeline timeline(2);
    const CombatTimeline::Owner owner{1, 1};
    const auto input = timeline.enqueue(owner, 100, CombatTimeline::Kind::input);
    const auto previous = timeline.enqueue(owner, 200, CombatTimeline::Kind::milestone);
    ASSERT_TRUE(input.token);
    ASSERT_TRUE(previous.token);
    ASSERT_EQ(timeline.try_claim(*input.token, 100).status, CombatTimeline::ClaimStatus::ready);
    const auto fail = [] {
      throw std::bad_alloc();
    };
    if (fail_after_entry) {
      timeline.set_after_insert_hook_for_test(fail);
    } else {
      timeline.set_before_insert_hook_for_test(fail);
    }

    EXPECT_THROW(timeline.complete_input_with_milestone(*input.token, previous.token, 150),
                 std::bad_alloc);
    EXPECT_EQ(timeline.size(), 2U);
    EXPECT_TRUE(timeline.is_claimed(*input.token));
    EXPECT_EQ(timeline.try_claim(*previous.token, 200).status,
              CombatTimeline::ClaimStatus::waiting);
    timeline.set_before_insert_hook_for_test({});
    timeline.set_after_insert_hook_for_test({});
    const auto completed = timeline.complete_input_with_milestone(*input.token, previous.token, 150);
    ASSERT_EQ(completed.status, CombatTimeline::EnqueueStatus::accepted);
    ASSERT_TRUE(completed.token);
    EXPECT_EQ(timeline.try_claim(*previous.token, 200).status,
              CombatTimeline::ClaimStatus::stale);
    EXPECT_EQ(timeline.try_claim(*completed.token, 200).status,
              CombatTimeline::ClaimStatus::ready);
  }
}

TEST(CombatTimeline, FailedFirstMilestoneInsertionDoesNotLeakOwnerIndexOrEntry) {
  CombatTimeline timeline(2);
  const CombatTimeline::Owner owner{1, 1};
  timeline.set_after_insert_hook_for_test([] {
    throw std::bad_alloc();
  });
  EXPECT_THROW(timeline.enqueue(owner, 100, CombatTimeline::Kind::milestone), std::bad_alloc);
  EXPECT_EQ(timeline.size(), 0U);
  timeline.set_after_insert_hook_for_test({});
  const auto inserted = timeline.enqueue(owner, 100, CombatTimeline::Kind::milestone);
  ASSERT_EQ(inserted.status, CombatTimeline::EnqueueStatus::accepted);
  ASSERT_TRUE(inserted.token);
  EXPECT_EQ(timeline.try_claim(*inserted.token, 100).status,
            CombatTimeline::ClaimStatus::ready);
}

TEST(CombatTimeline, AStaleOwnerCannotClaimOrCancelAnotherGeneration) {
  CombatTimeline timeline(2);
  const auto entry = timeline.enqueue({7, 2}, 10, CombatTimeline::Kind::close);
  ASSERT_TRUE(entry.token);
  auto stale = *entry.token;
  stale.owner.generation = 1;
  EXPECT_EQ(timeline.try_claim(stale, 10).status, CombatTimeline::ClaimStatus::stale);
  EXPECT_FALSE(timeline.cancel_queued(stale));
  EXPECT_EQ(timeline.try_claim(*entry.token, 10).status, CombatTimeline::ClaimStatus::ready);
  EXPECT_EQ(timeline.complete(stale).status, CombatTimeline::EnqueueStatus::invalid);
  EXPECT_EQ(timeline.size(), 1U);
}

TEST(CombatTimeline, CompletedFrontierRejectsOlderLateRegistration) {
  CombatTimeline timeline(2);
  const CombatTimeline::Owner owner{4, 1};
  const auto first = timeline.enqueue(owner, 100, CombatTimeline::Kind::milestone);
  ASSERT_TRUE(first.token);
  ASSERT_EQ(timeline.try_claim(*first.token, 100).status, CombatTimeline::ClaimStatus::ready);
  ASSERT_TRUE(timeline.begin_milestone(*first.token));
  ASSERT_TRUE(timeline.mark_milestone_staged(*first.token));
  ASSERT_EQ(timeline.complete(*first.token).status, CombatTimeline::EnqueueStatus::accepted);

  EXPECT_EQ(timeline.enqueue({5, 1}, 99, CombatTimeline::Kind::input).status,
            CombatTimeline::EnqueueStatus::stale_tick);
  const auto same_tick = timeline.enqueue({5, 1}, 100, CombatTimeline::Kind::input);
  ASSERT_EQ(same_tick.status, CombatTimeline::EnqueueStatus::accepted);
  ASSERT_TRUE(same_tick.token);
  EXPECT_GT(same_tick.token->sequence, first.token->sequence);
  EXPECT_EQ(timeline.try_claim(*same_tick.token, 100).status, CombatTimeline::ClaimStatus::ready);
  EXPECT_EQ(timeline.complete(*first.token).status, CombatTimeline::EnqueueStatus::invalid);
}

TEST(CombatTimeline, AClaimedMilestoneCannotCompleteUntilItsOutcomeIsStaged) {
  CombatTimeline timeline(2);
  const auto entry = timeline.enqueue({1, 1}, 100, CombatTimeline::Kind::milestone);
  ASSERT_TRUE(entry.token);
  ASSERT_EQ(timeline.try_claim(*entry.token, 100).status, CombatTimeline::ClaimStatus::ready);
  EXPECT_EQ(timeline.complete(*entry.token).status, CombatTimeline::EnqueueStatus::invalid);
  ASSERT_TRUE(timeline.begin_milestone(*entry.token));
  EXPECT_FALSE(timeline.begin_milestone(*entry.token));
  EXPECT_EQ(timeline.complete(*entry.token).status, CombatTimeline::EnqueueStatus::invalid);
  ASSERT_TRUE(timeline.mark_milestone_staged(*entry.token));
  EXPECT_FALSE(timeline.mark_milestone_staged(*entry.token));
  EXPECT_EQ(timeline.complete(*entry.token).status, CombatTimeline::EnqueueStatus::accepted);
}

TEST(CombatTimeline, OnlyUnstartedMilestonesCanBeReplaced) {
  CombatTimeline timeline(2);
  const auto entry = timeline.enqueue({1, 1}, 100, CombatTimeline::Kind::milestone);
  ASSERT_TRUE(entry.token);
  ASSERT_EQ(timeline.try_claim(*entry.token, 100).status, CombatTimeline::ClaimStatus::ready);
  EXPECT_EQ(timeline.replace_unstarted_milestone(
                *entry.token, CombatTimeline::Successor{99, CombatTimeline::Kind::milestone}).status,
            CombatTimeline::EnqueueStatus::stale_tick);
  const auto replaced = timeline.replace_unstarted_milestone(
      *entry.token, CombatTimeline::Successor{150, CombatTimeline::Kind::milestone});
  ASSERT_TRUE(replaced.token);
  EXPECT_EQ(timeline.try_claim(*entry.token, 200).status, CombatTimeline::ClaimStatus::stale);
  ASSERT_EQ(timeline.try_claim(*replaced.token, 200).status, CombatTimeline::ClaimStatus::ready);
  ASSERT_TRUE(timeline.begin_milestone(*replaced.token));
  EXPECT_EQ(timeline.replace_unstarted_milestone(*replaced.token).status,
            CombatTimeline::EnqueueStatus::invalid);
}

TEST(CombatTimeline, MilestoneCannotCreateAnInputWithoutItsFrameOwner) {
  for (const bool started : {false, true}) {
    SCOPED_TRACE(started);
    CombatTimeline timeline(2);
    const auto milestone = timeline.enqueue({1, 1}, 100, CombatTimeline::Kind::milestone);
    ASSERT_TRUE(milestone.token);
    ASSERT_EQ(timeline.try_claim(*milestone.token, 100).status,
              CombatTimeline::ClaimStatus::ready);
    const CombatTimeline::Successor orphan_input{150, CombatTimeline::Kind::input};
    if (started) {
      ASSERT_TRUE(timeline.begin_milestone(*milestone.token));
      ASSERT_TRUE(timeline.mark_milestone_staged(*milestone.token));
      EXPECT_EQ(timeline.complete(*milestone.token, orphan_input).status,
                CombatTimeline::EnqueueStatus::invalid);
    } else {
      EXPECT_EQ(timeline.replace_unstarted_milestone(*milestone.token, orphan_input).status,
                CombatTimeline::EnqueueStatus::invalid);
    }
    EXPECT_TRUE(timeline.is_claimed(*milestone.token));
    EXPECT_EQ(timeline.size(), 1U);
    const auto settled = started ? timeline.complete(*milestone.token)
                                 : timeline.replace_unstarted_milestone(*milestone.token);
    EXPECT_EQ(settled.status, CombatTimeline::EnqueueStatus::accepted);
    EXPECT_EQ(timeline.size(), 0U);
  }
}

TEST(CombatTimeline, CloseCannotCancelAnAlreadyDueOrOtherOwnersMilestone) {
  CombatTimeline timeline(4);
  const CombatTimeline::Owner owner{1, 1};
  const auto impact = timeline.enqueue(owner, 100, CombatTimeline::Kind::milestone);
  const auto close = timeline.enqueue(owner, 200, CombatTimeline::Kind::close);
  ASSERT_TRUE(impact.token);
  ASSERT_TRUE(close.token);
  EXPECT_FALSE(timeline.cancel_queued(*impact.token));
  EXPECT_EQ(timeline.try_claim(*close.token, 200).status, CombatTimeline::ClaimStatus::waiting);
  EXPECT_FALSE(timeline.cancel_milestone_after_close(*close.token, *impact.token));
  ASSERT_EQ(timeline.try_claim(*impact.token, 200).status, CombatTimeline::ClaimStatus::ready);
  ASSERT_TRUE(timeline.begin_milestone(*impact.token));
  ASSERT_TRUE(timeline.mark_milestone_staged(*impact.token));
  ASSERT_EQ(timeline.complete(*impact.token).status, CombatTimeline::EnqueueStatus::accepted);
  ASSERT_EQ(timeline.try_claim(*close.token, 200).status, CombatTimeline::ClaimStatus::ready);
  EXPECT_FALSE(timeline.cancel_milestone_after_close(*close.token, *impact.token));
}

TEST(CombatTimeline, OrderedCloseCancelsOnlyTheSameOwnersFutureMilestones) {
  CombatTimeline timeline(4);
  const CombatTimeline::Owner owner{1, 1};
  const auto close = timeline.enqueue(owner, 100, CombatTimeline::Kind::close);
  const auto future = timeline.enqueue(owner, 200, CombatTimeline::Kind::milestone);
  const auto other = timeline.enqueue({2, 1}, 200, CombatTimeline::Kind::milestone);
  ASSERT_TRUE(close.token);
  ASSERT_TRUE(future.token);
  ASSERT_TRUE(other.token);
  EXPECT_FALSE(timeline.cancel_milestone_after_close(*close.token, *future.token));
  ASSERT_EQ(timeline.try_claim(*close.token, 200).status, CombatTimeline::ClaimStatus::ready);
  EXPECT_EQ(timeline.complete(*close.token).status, CombatTimeline::EnqueueStatus::invalid);
  EXPECT_FALSE(timeline.cancel_milestone_after_close(*close.token, *other.token));
  auto stale_owner = *future.token;
  ++stale_owner.owner.generation;
  EXPECT_FALSE(timeline.cancel_milestone_after_close(*close.token, stale_owner));
  EXPECT_TRUE(timeline.cancel_milestone_after_close(*close.token, *future.token));
  EXPECT_FALSE(timeline.cancel_milestone_after_close(*close.token, *future.token));
  EXPECT_EQ(timeline.try_claim(*future.token, 200).status, CombatTimeline::ClaimStatus::stale);
  EXPECT_EQ(timeline.complete(*close.token).status, CombatTimeline::EnqueueStatus::accepted);
  EXPECT_EQ(timeline.try_claim(*other.token, 200).status, CombatTimeline::ClaimStatus::ready);
}

TEST(CombatTimeline, SimultaneousCloseAndImpactUseTheirRegistrationOrder) {
  const CombatTimeline::Owner owner{1, 1};
  for (const bool close_first : {false, true}) {
    SCOPED_TRACE(close_first);
    CombatTimeline timeline(2);
    const auto first = timeline.enqueue(
        owner, 100, close_first ? CombatTimeline::Kind::close : CombatTimeline::Kind::milestone);
    const auto second = timeline.enqueue(
        owner, 100, close_first ? CombatTimeline::Kind::milestone : CombatTimeline::Kind::close);
    ASSERT_TRUE(first.token);
    ASSERT_TRUE(second.token);
    ASSERT_EQ(timeline.try_claim(*first.token, 100).status, CombatTimeline::ClaimStatus::ready);
    if (close_first) {
      EXPECT_TRUE(timeline.cancel_milestone_after_close(*first.token, *second.token));
    } else {
      EXPECT_FALSE(timeline.cancel_milestone_after_close(*second.token, *first.token));
      ASSERT_TRUE(timeline.begin_milestone(*first.token));
      ASSERT_TRUE(timeline.mark_milestone_staged(*first.token));
    }
    EXPECT_EQ(timeline.complete(*first.token).status, CombatTimeline::EnqueueStatus::accepted);
  }
}

} // namespace
} // namespace chronicles::combat
