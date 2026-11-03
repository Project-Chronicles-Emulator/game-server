#include "world/session/combat_input_admission.hpp"
#include "world/session/combat_owner_wakes.hpp"
#include "world/session/combat_work_schedule.hpp"
#include "world/session/retained_frame_delivery.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <future>

namespace chronicles {
namespace {

TEST(CombatWorkSchedule, CompletionFailureRetainsClaimWithoutReexecutingMilestone) {
  using Timeline = combat::CombatTimeline;
  auto timeline = std::make_shared<Timeline>(8, true);
  CombatWorkSchedule schedule(timeline, 1, std::make_shared<int>(1));
  schedule.initialize(10);
  ASSERT_EQ(schedule.claim(100).status, Timeline::ClaimStatus::ready);
  const auto original = schedule.active()->token;
  timeline->set_before_insert_hook_for_test([] { throw std::bad_alloc(); });
  EXPECT_THROW(schedule.complete(20), std::bad_alloc);
  ASSERT_TRUE(schedule.active());
  EXPECT_EQ(schedule.active()->token, original);
  EXPECT_EQ(schedule.claim(100).status, Timeline::ClaimStatus::stale);
  timeline->set_before_insert_hook_for_test({});
  schedule.complete(20);
  EXPECT_FALSE(schedule.active());
  ASSERT_TRUE(schedule.next());
  EXPECT_EQ(schedule.next()->token.tick, 20);
  schedule.submit_close(15);
  ASSERT_EQ(schedule.claim(100).status, Timeline::ClaimStatus::ready);
  EXPECT_EQ(schedule.active()->kind, Timeline::Kind::close);
  schedule.complete(std::nullopt);
  schedule.retire();
  EXPECT_EQ(timeline->size(), 0U);
}

TEST(CombatWorkSchedule, CloseInsertionRetryPreservesEarlierWorkAndCancelsLaterWork) {
  using Timeline = combat::CombatTimeline;
  auto timeline = std::make_shared<Timeline>(8, true);
  CombatWorkSchedule schedule(timeline, 1, std::make_shared<int>(1));
  schedule.initialize(10);
  timeline->set_before_insert_hook_for_test([] { throw std::bad_alloc(); });
  EXPECT_THROW(schedule.submit_close(15), std::bad_alloc);
  EXPECT_FALSE(schedule.closing());
  timeline->set_before_insert_hook_for_test({});
  schedule.submit_close(15);
  ASSERT_EQ(schedule.claim(100).status, Timeline::ClaimStatus::ready);
  EXPECT_EQ(schedule.active()->token.tick, 10);
  schedule.complete(20);
  ASSERT_EQ(schedule.claim(100).status, Timeline::ClaimStatus::ready);
  EXPECT_EQ(schedule.active()->kind, Timeline::Kind::close);
  EXPECT_EQ(schedule.active()->token.tick, 15);
  schedule.complete(std::nullopt);
  schedule.retire();
  EXPECT_EQ(timeline->size(), 0U);
}

TEST(CombatWorkSchedule, NeverAdmittedCloseCanRetryAfterAnotherOwnerAdvances) {
  using Timeline = combat::CombatTimeline;
  auto timeline = std::make_shared<Timeline>(8, true);
  const auto lifetime = std::make_shared<int>(1);
  CombatWorkSchedule departing(timeline, 1, lifetime);
  CombatWorkSchedule other(timeline, 2, lifetime);
  departing.initialize(200);
  other.initialize(110);
  timeline->set_before_insert_hook_for_test([] {
    throw std::bad_alloc();
  });
  EXPECT_THROW(departing.submit_close(100), std::bad_alloc);
  timeline->set_before_insert_hook_for_test({});
  ASSERT_EQ(other.claim(110).status, Timeline::ClaimStatus::ready);
  other.complete(std::nullopt);
  other.retire();

  // Native recovery retries a never-admitted close at the current clock, rather than
  // repeatedly submitting the rejected tick behind the population frontier.
  departing.submit_close(120);
  ASSERT_EQ(departing.claim(120).status, Timeline::ClaimStatus::ready);
  EXPECT_EQ(departing.active()->token.tick, 120);
  departing.complete(std::nullopt);
  departing.retire();
  EXPECT_EQ(timeline->size(), 0U);
}

TEST(CombatInputAdmission, SuspendsBeforeDispatchAndCannotReplayAClaimedFrame) {
  auto timeline = std::make_shared<combat::CombatTimeline>(8);
  const combat::CombatTimeline::Owner first_owner{1, 1};
  const auto earlier = timeline->enqueue(first_owner, 10, combat::CombatTimeline::Kind::milestone);
  ASSERT_TRUE(earlier.token);
  CombatInputAdmission admission;
  auto owner_lifetime = std::make_shared<int>(1);
  ASSERT_TRUE(admission.publish(CombatInputAdmission::Snapshot{timeline, {2, 1}, owner_lifetime}));
  Frame frame;
  frame.bytes = {4, 0, 1, 0};
  ASSERT_EQ(admission.submit(std::move(frame), 11), CombatInputAdmission::SubmitStatus::accepted);
  EXPECT_EQ(admission.claim(11).status, combat::CombatTimeline::ClaimStatus::waiting);
  EXPECT_FALSE(admission.publish(std::nullopt));
  ASSERT_EQ(timeline->try_claim(*earlier.token, 11).status,
            combat::CombatTimeline::ClaimStatus::ready);
  ASSERT_TRUE(timeline->begin_milestone(*earlier.token));
  ASSERT_TRUE(timeline->mark_milestone_staged(*earlier.token));
  ASSERT_EQ(timeline->complete(*earlier.token).status,
            combat::CombatTimeline::EnqueueStatus::accepted);
  ASSERT_EQ(admission.claim(11).status, combat::CombatTimeline::ClaimStatus::ready);
  EXPECT_FALSE(admission.cancel_queued());
  EXPECT_EQ(admission.take_claimed_frame().bytes.size(), 4U);
  EXPECT_THROW(admission.take_claimed_frame(), std::logic_error);
  EXPECT_EQ(admission.complete_with_milestone(std::nullopt, std::nullopt).status,
            combat::CombatTimeline::EnqueueStatus::accepted);
  EXPECT_TRUE(admission.publish(std::nullopt));
}

TEST(CombatInputAdmission, RejectedAndCancelledInputsDoNotRetainTheirSlot) {
  auto timeline = std::make_shared<combat::CombatTimeline>(1);
  CombatInputAdmission admission;
  auto owner_lifetime = std::make_shared<int>(1);
  ASSERT_TRUE(admission.publish(CombatInputAdmission::Snapshot{timeline, {2, 1}, owner_lifetime}));
  Frame frame;
  ASSERT_EQ(admission.submit(frame, 11), CombatInputAdmission::SubmitStatus::accepted);
  EXPECT_EQ(admission.submit(frame, 12), CombatInputAdmission::SubmitStatus::busy);
  EXPECT_TRUE(admission.cancel_queued());
  EXPECT_EQ(timeline->size(), 0U);
  ASSERT_EQ(admission.submit(frame, 12), CombatInputAdmission::SubmitStatus::accepted);
  ASSERT_EQ(admission.claim(12).status, combat::CombatTimeline::ClaimStatus::ready);
  admission.take_claimed_frame();
  ASSERT_EQ(admission.complete_with_milestone(std::nullopt, std::nullopt).status,
            combat::CombatTimeline::EnqueueStatus::accepted);
  EXPECT_EQ(admission.submit(frame, 11), CombatInputAdmission::SubmitStatus::rejected);
}

TEST(CombatInputAdmission, SettledInputAtomicallyRekeysItsPendingDeadline) {
  auto timeline = std::make_shared<combat::CombatTimeline>(3);
  const combat::CombatTimeline::Owner owner{2, 1};
  CombatInputAdmission admission;
  auto owner_lifetime = std::make_shared<int>(1);
  ASSERT_TRUE(admission.publish(CombatInputAdmission::Snapshot{timeline, owner, owner_lifetime}));
  ASSERT_EQ(admission.submit(Frame{}, 100), CombatInputAdmission::SubmitStatus::accepted);
  const auto previous = timeline->enqueue(owner, 200, combat::CombatTimeline::Kind::milestone);
  const auto other = timeline->enqueue({3, 1}, 150, combat::CombatTimeline::Kind::milestone);
  ASSERT_TRUE(previous.token);
  ASSERT_TRUE(other.token);
  ASSERT_EQ(admission.claim(100).status, combat::CombatTimeline::ClaimStatus::ready);
  admission.take_claimed_frame();
  const auto completed = admission.complete_with_milestone(previous.token, 120);
  ASSERT_EQ(completed.status, combat::CombatTimeline::EnqueueStatus::accepted);
  ASSERT_TRUE(completed.token);
  EXPECT_EQ(timeline->try_claim(*previous.token, 200).status,
            combat::CombatTimeline::ClaimStatus::stale);
  EXPECT_EQ(timeline->try_claim(*other.token, 200).head_owner, owner);
  EXPECT_EQ(timeline->try_claim(*completed.token, 200).status,
            combat::CombatTimeline::ClaimStatus::ready);
}

TEST(CombatInputAdmission, AcceptedInputRetainsItsOwnerUntilSettlement) {
  struct Owner {
    CombatInputAdmission admission;
  };
  auto timeline = std::make_shared<combat::CombatTimeline>(2);
  auto owner = std::make_shared<Owner>();
  std::weak_ptr<Owner> weak_owner = owner;
  ASSERT_TRUE(owner->admission.publish(CombatInputAdmission::Snapshot{timeline, {2, 1}, owner}));
  ASSERT_EQ(owner->admission.submit(Frame{}, 100), CombatInputAdmission::SubmitStatus::accepted);
  owner.reset();
  EXPECT_FALSE(weak_owner.expired());
  auto retained = weak_owner.lock();
  ASSERT_TRUE(retained);
  ASSERT_EQ(retained->admission.claim(100).status, combat::CombatTimeline::ClaimStatus::ready);
  retained->admission.take_claimed_frame();
  EXPECT_FALSE(retained->admission.cancel_queued());
  EXPECT_FALSE(weak_owner.expired());
  EXPECT_EQ(retained->admission.complete_with_milestone(std::nullopt, std::nullopt).status,
            combat::CombatTimeline::EnqueueStatus::accepted);
  retained.reset();
  EXPECT_TRUE(weak_owner.expired());
}

TEST(CombatInputAdmission, ExpiredOwnerCannotLeaveAnUnownedTimelineInput) {
  auto timeline = std::make_shared<combat::CombatTimeline>(2);
  auto owner_lifetime = std::make_shared<int>(1);
  CombatInputAdmission admission;
  ASSERT_TRUE(admission.publish(CombatInputAdmission::Snapshot{timeline, {2, 1}, owner_lifetime}));
  owner_lifetime.reset();
  EXPECT_EQ(admission.submit(Frame{}, 100), CombatInputAdmission::SubmitStatus::unavailable);
  EXPECT_EQ(timeline->size(), 0U);
}

TEST(CombatOwnerWakes, ReentrantWakeAndGenerationChangeNeverWakeTheWrongOwner) {
  CombatOwnerWakes registry(1);
  const combat::CombatTimeline::Owner original{12, 1};
  const combat::CombatTimeline::Owner replacement{12, 2};
  bool woke = false;
  ASSERT_TRUE(registry.register_owner(original, [&] {
    woke = true;
    return registry.unregister_owner(original);
  }));
  EXPECT_FALSE(registry.register_owner(replacement, [] {
    return true;
  }));
  EXPECT_TRUE(registry.request(original));
  EXPECT_TRUE(woke);
  EXPECT_FALSE(registry.request(original));
  EXPECT_TRUE(registry.register_owner(replacement, [] {
    return true;
  }));
  EXPECT_FALSE(registry.request(original));
  EXPECT_TRUE(registry.request(replacement));
}

TEST(CombatOwnerWakes, ExpiredOwnerIsReportedInsteadOfAcknowledgingItsWork) {
  CombatOwnerWakes registry(1);
  const combat::CombatTimeline::Owner owner{12, 1};
  auto session = std::make_shared<int>(7);
  std::weak_ptr<int> weak_session = session;
  ASSERT_TRUE(registry.register_owner(owner, [weak_session] {
    return !weak_session.expired();
  }));
  session.reset();
  EXPECT_FALSE(registry.request(owner));
  EXPECT_TRUE(registry.unregister_owner(owner));
}

TEST(RetainedFrameDelivery, PartialDeliveryAndStaleViewsDoNotRepeatInsertedFrames) {
  Frame first;
  first.bytes = {4, 0, 1, 0};
  Frame second;
  second.bytes = {4, 0, 2, 0};
  const GatheringViewIdentity view{1, 9012, 7};
  RetainedFrameDelivery delivery("event", {{10, view, {first, second}}, {11, view, {first}}});
  const auto attempt = delivery.begin(0);
  ASSERT_TRUE(attempt);
  EXPECT_FALSE(delivery.begin(0));
  EXPECT_TRUE(delivery.finish(*attempt, FrameEnqueueStatus::inserted, true));
  EXPECT_FALSE(delivery.finish(*attempt, FrameEnqueueStatus::inserted, true));
  auto next = delivery.begin(0);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->frame_index, 1U);
  EXPECT_TRUE(delivery.finish(*next, FrameEnqueueStatus::stale, true));
  EXPECT_FALSE(delivery.begin(0));
  Frame refreshed;
  refreshed.bytes = {4, 0, 3, 0};
  EXPECT_TRUE(delivery.replace_stale(0, std::move(refreshed)));
  next = delivery.begin(0);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->frame_index, 1U);
  EXPECT_EQ(next->frame.bytes[2], 3);
  EXPECT_TRUE(delivery.finish(*next, FrameEnqueueStatus::inserted, true));
  auto other = delivery.begin(1);
  ASSERT_TRUE(other);
  EXPECT_TRUE(delivery.finish(*other, FrameEnqueueStatus::stale, false));
  EXPECT_TRUE(delivery.complete());
}

TEST(RetainedFrameDelivery, TimedOutOrCancelledPostsRequireDefiniteDeparture) {
  Frame frame;
  frame.bytes = {4, 0, 1, 0};
  const GatheringViewIdentity view{1, 9012, 7};
  RetainedFrameDelivery delivery("event", {{10, view, {frame}}, {11, view, {frame}}});
  const auto first = delivery.begin(0);
  ASSERT_TRUE(first);
  ASSERT_TRUE(delivery.timeout(0));
  EXPECT_FALSE(delivery.begin(0));
  const auto second = delivery.begin(1);
  ASSERT_TRUE(second);
  ASSERT_TRUE(delivery.finish(*second, FrameEnqueueStatus::cancelled, true));
  EXPECT_FALSE(delivery.begin(1));
  EXPECT_FALSE(delivery.complete());
  EXPECT_TRUE(delivery.confirm_departed(0));
  EXPECT_FALSE(delivery.finish(*first, FrameEnqueueStatus::inserted, true));
  EXPECT_TRUE(delivery.confirm_departed(1));
  EXPECT_TRUE(delivery.complete());
}

TEST(RetainedFrameDelivery, ConcurrentAttemptsNeverPostTheSameFrameTwice) {
  Frame frame;
  frame.bytes = {4, 0, 1, 0};
  RetainedFrameDelivery delivery("event", {{10, {1, 9012, 7}, {frame}}});
  auto first = std::async(std::launch::async, [&] {
    return delivery.begin(0);
  });
  auto second = std::async(std::launch::async, [&] {
    return delivery.begin(0);
  });
  const auto one = first.get();
  const auto two = second.get();
  EXPECT_NE(one.has_value(), two.has_value());
  const auto &claimed = one ? *one : *two;
  EXPECT_TRUE(delivery.finish(claimed, FrameEnqueueStatus::closed, false));
  EXPECT_TRUE(delivery.complete());
}

} // namespace
} // namespace chronicles
