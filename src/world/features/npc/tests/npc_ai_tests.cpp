#include "world/features/npc/npc_ai.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace chronicles {
namespace {

constexpr std::int64_t kStart = 100000000;
constexpr std::int64_t kMillisecond = 10000;
constexpr std::int64_t kSecond = 10000000;

nlohmann::json ai_definition_json() {
  return {{"search_circle_range_mm", 2000},    {"search_cone_range_mm", 8000},
          {"search_half_cone_angle", 90.0},    {"hearing_range_mm", 0},
          {"leash_range_mm", 50000},           {"reset_delay_ms", 3000},
          {"combat_distance_min_mm", 1000},    {"combat_distance_max_mm", 2000},
          {"run_speed_mm_per_second", 3000.0}, {"move_speed_rate", 1000}};
}

WorldPosition home_position() {
  return {8000, 0, 0, 0, 0.0F};
}

NpcAi make_ai() {
  return {NpcAiDefinition::from_json(ai_definition_json()).value(), home_position()};
}

NpcAiInput input_at(int x_mm, std::int64_t ticks) {
  NpcAiInput input;
  input.player_position = {8000, x_mm, 0, 0, 0.0F};
  input.ticks = ticks;
  return input;
}

TEST(NpcAi, DetectsInsideConeAndCloseCircleOnlyWhenEligible) {
  auto ai = make_ai();
  auto update = ai.advance(input_at(7000, kStart));
  EXPECT_TRUE(update.acquired);
  EXPECT_EQ(ai.state(), NpcAiState::Engaged);

  auto outside = make_ai();
  EXPECT_FALSE(outside.advance(input_at(-7000, kStart)).acquired);
  EXPECT_EQ(outside.state(), NpcAiState::Idle);

  auto close_circle = make_ai();
  auto circle_input = input_at(-100, kStart);
  circle_input.player_position.y_mm = 1500;
  EXPECT_TRUE(close_circle.advance(circle_input).acquired);

  auto inactive = make_ai();
  auto inactive_input = input_at(1000, kStart);
  inactive_input.monster_active = false;
  inactive_input.damaged = true;
  EXPECT_FALSE(inactive.advance(inactive_input).acquired);
  EXPECT_FALSE(inactive.next_action_tick().has_value());

  auto friendly = make_ai();
  auto friendly_input = input_at(1000, kStart);
  friendly_input.monster_hostile = false;
  EXPECT_FALSE(friendly.advance(friendly_input).acquired);

  auto dead_player = make_ai();
  auto dead_input = input_at(1000, kStart);
  dead_input.player_alive = false;
  EXPECT_FALSE(dead_player.advance(dead_input).acquired);
}

TEST(NpcAi, DisabledPerceptionStillAllowsRetaliationAfterDamage) {
  auto ai = make_ai();
  auto unseen = input_at(1000, kStart);
  unseen.perception_enabled = false;
  EXPECT_FALSE(ai.advance(unseen).acquired);
  EXPECT_EQ(ai.state(), NpcAiState::Idle);

  auto damaged = input_at(1000, kStart + 250 * kMillisecond);
  damaged.perception_enabled = false;
  damaged.damaged = true;
  EXPECT_TRUE(ai.advance(damaged).acquired);
  EXPECT_EQ(ai.state(), NpcAiState::Engaged);
}

TEST(NpcAi, DamageAcquiresTargetOutsidePerception) {
  auto ai = make_ai();
  auto input = input_at(20000, kStart);
  input.damaged = true;

  const auto acquired = ai.advance(input);
  EXPECT_TRUE(acquired.acquired);
  ASSERT_TRUE(acquired.motion_start.has_value());
  EXPECT_EQ(acquired.motion_start->destination.x_mm, 20000);
  EXPECT_EQ(acquired.motion_start->end_tick, kStart + 6 * kSecond);
  EXPECT_EQ(ai.state(), NpcAiState::Engaged);
  EXPECT_EQ(ai.next_action_tick(), kStart + 100 * kMillisecond);
  const auto motion = ai.advance(input_at(20000, kStart + 100 * kMillisecond));
  EXPECT_FALSE(motion.motion_start.has_value());
}

TEST(NpcAi, PursuitUsesElapsedTimeStopsAtCombatRangeAndDoesNotOvershoot) {
  auto ai = make_ai();
  auto initial = input_at(10000, kStart);
  initial.damaged = true;
  const auto acquired = ai.advance(initial);
  ASSERT_TRUE(acquired.motion_start.has_value());
  const auto motion_end_tick = acquired.motion_start->end_tick;

  const auto first_step = ai.advance(input_at(10000, kStart + kSecond));
  EXPECT_TRUE(first_step.moved);
  EXPECT_FALSE(first_step.motion_start.has_value());
  EXPECT_EQ(ai.position().x_mm, 3000);
  EXPECT_EQ(ai.position().yaw, 0.0F);

  ai.advance(input_at(10000, kStart + 2 * kSecond));
  EXPECT_EQ(ai.position().x_mm, 6000);
  const auto arrival = ai.advance(input_at(10000, kStart + 3 * kSecond));
  EXPECT_EQ(arrival.motion_complete_tick, motion_end_tick);
  EXPECT_EQ(ai.position().x_mm, 8000);
  EXPECT_FALSE(ai.advance(input_at(10000, kStart + 4 * kSecond)).moved);
  EXPECT_EQ(ai.position().x_mm, 8000);

  auto close = make_ai();
  auto close_input = input_at(2500, kStart);
  close_input.damaged = true;
  close.advance(close_input);
  close.advance(input_at(2500, kStart + kSecond));
  EXPECT_EQ(close.position().x_mm, 500);
}

TEST(NpcAi, SettledDiagonalPursuitStaysQuietWhileTheTargetIsStationary) {
  for (const int y : {1, 1000, 3000, 7000, 10000}) {
    SCOPED_TRACE(y);
    auto ai = make_ai();
    auto input = input_at(10000, kStart);
    input.player_position.y_mm = y;
    input.damaged = true;
    input.engagement_range_mm = 1500;
    const auto started = ai.advance(input);
    ASSERT_TRUE(started.motion_start);

    input.damaged = false;
    input.ticks = started.motion_start->end_tick;
    EXPECT_TRUE(ai.advance(input).motion_complete_tick);
    const auto settled = ai.position();

    // Rounded endpoints may sit just outside the exact stopping radius. That
    // residue must not produce repeated sub-millimeter replans on later polls.
    for (int poll = 1; poll <= 20; ++poll) {
      input.ticks += 100 * kMillisecond;
      const auto update = ai.advance(input);
      EXPECT_FALSE(update.motion_start);
      EXPECT_FALSE(update.motion_complete_tick);
      EXPECT_FALSE(update.moved);
    }
    EXPECT_EQ(ai.position().x_mm, settled.x_mm);
    EXPECT_EQ(ai.position().y_mm, settled.y_mm);
  }
}

TEST(NpcAi, ActiveActionPausesPursuitWithoutStoppingDeadlines) {
  auto ai = make_ai();
  auto initial = input_at(10000, kStart);
  initial.damaged = true;
  initial.action_active = true;
  ai.advance(initial);

  auto input = input_at(10000, kStart + kSecond);
  input.action_active = true;
  EXPECT_FALSE(ai.advance(input).moved);
  EXPECT_EQ(ai.position().x_mm, 0);
  EXPECT_EQ(ai.next_action_tick(), kStart + kSecond + 100 * kMillisecond);
}

TEST(NpcAi, RedirectCompletesOldMotionAndStartsFromTheUpdatedAuthoritativePosition) {
  auto ai = make_ai();
  auto initial = input_at(10000, kStart);
  initial.damaged = true;
  ASSERT_TRUE(ai.advance(initial).motion_start);

  const auto redirect_tick = kStart + 100 * kMillisecond;
  const auto redirect = ai.advance(input_at(20000, redirect_tick));
  ASSERT_TRUE(redirect.motion_start);
  EXPECT_EQ(redirect.motion_complete_tick, redirect_tick);
  EXPECT_EQ(redirect.motion_start->start_position.x_mm, 300);
  EXPECT_EQ(redirect.motion_start->destination.x_mm, 20000);
  EXPECT_EQ(redirect.motion_start->end_tick, kStart + 6 * kSecond);
  EXPECT_EQ(ai.position().x_mm, 300);

  const auto next_step = ai.advance(input_at(20000, kStart + 200 * kMillisecond));
  EXPECT_EQ(ai.position().x_mm, 600);
  EXPECT_FALSE(next_step.motion_start);
  EXPECT_FALSE(next_step.motion_complete_tick);
}

TEST(NpcAi, SmallTargetChangesFinishTheAnnouncedPathBeforeStartingTheNext) {
  auto ai = make_ai();
  auto initial = input_at(10000, kStart);
  initial.damaged = true;
  const auto acquired = ai.advance(initial);
  ASSERT_TRUE(acquired.motion_start);
  const auto end_tick = acquired.motion_start->end_tick;

  const auto nearby_goal = ai.advance(input_at(10500, kStart + 100 * kMillisecond));
  EXPECT_FALSE(nearby_goal.motion_start);
  EXPECT_FALSE(nearby_goal.motion_complete_tick);
  const auto arrival = ai.advance(input_at(10500, end_tick));
  EXPECT_EQ(ai.position().x_mm, 8000);
  EXPECT_EQ(arrival.motion_complete_tick, end_tick);
  ASSERT_TRUE(arrival.motion_start);
  EXPECT_EQ(arrival.motion_start->start_position.x_mm, 8000);
  EXPECT_EQ(arrival.motion_start->destination.x_mm, 10500);
}

TEST(NpcAi, PausingAnActivePursuitCompletesItOnceAndResumesWithoutBorrowingTime) {
  auto ai = make_ai();
  auto initial = input_at(10000, kStart);
  initial.damaged = true;
  ai.advance(initial);
  ai.advance(input_at(10000, kStart + kSecond));
  ASSERT_EQ(ai.position().x_mm, 3000);

  auto paused = input_at(10000, kStart + 2 * kSecond);
  paused.action_active = true;
  const auto interruption = ai.advance(paused);
  EXPECT_EQ(interruption.motion_complete_tick, paused.ticks);
  EXPECT_FALSE(interruption.motion_start);
  EXPECT_TRUE(interruption.moved);
  EXPECT_EQ(ai.position().x_mm, 6000);
  paused.ticks += 100 * kMillisecond;
  EXPECT_FALSE(ai.advance(paused).motion_complete_tick);

  const auto resumed = ai.advance(input_at(10000, kStart + 3 * kSecond));
  ASSERT_TRUE(resumed.motion_start);
  EXPECT_EQ(resumed.motion_start->start_position.x_mm, 6000);
  EXPECT_EQ(ai.position().x_mm, 6000);
  ai.advance(input_at(10000, kStart + 3 * kSecond + 100 * kMillisecond));
  EXPECT_EQ(ai.position().x_mm, 6300);
}

TEST(NpcAi, LosingEligibilityCancelsMotionOnceAndClearsItsDeadline) {
  for (int reason = 0; reason < 4; ++reason) {
    SCOPED_TRACE(reason);
    auto ai = make_ai();
    auto initial = input_at(10000, kStart);
    initial.damaged = true;
    ai.advance(initial);
    ai.advance(input_at(10000, kStart + kSecond));
    auto stopped = input_at(10000, kStart + kSecond + 10 * kMillisecond);
    switch (reason) {
    case 0:
      stopped.monster_alive = false;
      break;
    case 1:
      stopped.monster_active = false;
      break;
    case 2:
      stopped.monster_hostile = false;
      break;
    case 3:
      stopped.player_position.map_id = 10117;
      break;
    }
    const auto cancelled = ai.advance(stopped);
    EXPECT_EQ(cancelled.target_cleared, reason >= 2);
    EXPECT_TRUE(cancelled.aggro_released);
    EXPECT_TRUE(cancelled.cancel_action);
    EXPECT_EQ(cancelled.motion_complete_tick, stopped.ticks);
    EXPECT_FALSE(cancelled.motion_start);
    EXPECT_EQ(ai.position().x_mm, 3000);
    EXPECT_EQ(ai.state(), NpcAiState::Idle);
    EXPECT_FALSE(ai.next_action_tick());
    EXPECT_FALSE(ai.advance(stopped).motion_complete_tick);
  }
}

TEST(NpcAi, ArrivalIsIndependentOfPollingGranularityAndCompletesOnlyOnce) {
  auto coarse = make_ai();
  auto fine = make_ai();
  auto initial = input_at(10000, kStart);
  initial.player_position.y_mm = 7000;
  initial.damaged = true;
  const auto started = coarse.advance(initial);
  fine.advance(initial);
  ASSERT_TRUE(started.motion_start);
  const auto end_tick = started.motion_start->end_tick;

  auto poll = initial;
  poll.damaged = false;
  for (poll.ticks = kStart + 100 * kMillisecond; poll.ticks < end_tick;
       poll.ticks += 100 * kMillisecond) {
    fine.advance(poll);
  }
  poll.ticks = end_tick;
  EXPECT_EQ(fine.advance(poll).motion_complete_tick, end_tick);
  EXPECT_EQ(coarse.advance(poll).motion_complete_tick, end_tick);
  EXPECT_EQ(fine.position().x_mm, coarse.position().x_mm);
  EXPECT_EQ(fine.position().y_mm, coarse.position().y_mm);
  const auto remaining = std::hypot(10000.0 - fine.position().x_mm, 7000.0 - fine.position().y_mm);
  EXPECT_NEAR(remaining, 2000.0, 1.0);
  poll.ticks += 100 * kMillisecond;
  const auto settled = fine.advance(poll);
  EXPECT_FALSE(settled.motion_start);
  EXPECT_FALSE(settled.motion_complete_tick);
  EXPECT_FALSE(settled.moved);
}

TEST(NpcAi, EngagementRangeChangesReplanTheActiveMotion) {
  auto ai = make_ai();
  auto initial = input_at(10000, kStart);
  initial.damaged = true;
  ai.advance(initial);
  auto changed = input_at(10000, kStart + 100 * kMillisecond);
  changed.engagement_range_mm = 3000;
  const auto update = ai.advance(changed);
  EXPECT_EQ(update.motion_complete_tick, changed.ticks);
  ASSERT_TRUE(update.motion_start);
  EXPECT_EQ(update.motion_start->start_position.x_mm, 300);
  changed.ticks = update.motion_start->end_tick;
  EXPECT_EQ(ai.advance(changed).motion_complete_tick, changed.ticks);
  EXPECT_EQ(ai.position().x_mm, 7000);
}

TEST(NpcAi, LeashReturnsHomeBeforeDelayedHealthReset) {
  auto ai = make_ai();
  auto initial = input_at(10000, kStart);
  initial.damaged = true;
  ai.advance(initial);
  ai.advance(input_at(10000, kStart + kSecond));
  ASSERT_EQ(ai.position().x_mm, 3000);

  auto leash = ai.advance(input_at(60000, kStart + kSecond + kMillisecond));
  EXPECT_TRUE(leash.target_cleared);
  EXPECT_TRUE(leash.aggro_released);
  EXPECT_TRUE(leash.cancel_action);
  EXPECT_EQ(leash.motion_complete_tick, kStart + kSecond + kMillisecond);
  ASSERT_TRUE(leash.motion_start);
  EXPECT_EQ(leash.motion_start->start_position.x_mm, 3003);
  EXPECT_EQ(leash.motion_start->destination.x_mm, 0);
  EXPECT_EQ(leash.motion_start->end_tick, kStart + 2 * kSecond + 2 * kMillisecond);
  EXPECT_EQ(ai.state(), NpcAiState::Returning);

  auto returning = input_at(60000, kStart + 2 * kSecond + 2 * kMillisecond);
  returning.damaged = true;
  const auto returned = ai.advance(returning);
  EXPECT_EQ(returned.motion_complete_tick, leash.motion_start->end_tick);
  EXPECT_FALSE(returned.motion_start);
  EXPECT_EQ(ai.state(), NpcAiState::Returning);
  EXPECT_EQ(ai.position().x_mm, 0);
  EXPECT_EQ(ai.next_action_tick(), kStart + 5 * kSecond + 2 * kMillisecond);

  EXPECT_FALSE(ai.advance(input_at(60000, kStart + 5 * kSecond)).reset_health);
  auto reset = ai.advance(input_at(1000, kStart + 5 * kSecond + 2 * kMillisecond));
  EXPECT_TRUE(reset.reset_health);
  EXPECT_EQ(ai.state(), NpcAiState::Idle);
  EXPECT_FALSE(ai.advance(input_at(1000, kStart + 5 * kSecond + 3 * kMillisecond)).acquired);
}

// The target stays 45 m from home (inside the 50 m leash), so only the no-contact give-up can
// release it: at 3 m/s the monster needs about 14 s to come within its 2 m attack reach.
constexpr int kFarTargetInsideLeashMm = 45000;

NpcAi ai_chasing_far_target() {
  auto ai = make_ai();
  auto hit = input_at(kFarTargetInsideLeashMm, kStart);
  hit.damaged = true;
  EXPECT_TRUE(ai.advance(hit).acquired);
  return ai;
}

TEST(NpcAi, GivesUpAndReturnsWhenItCannotReachItsTargetWithoutAnyContact) {
  auto ai = ai_chasing_far_target();
  const auto give_up_ms = kNpcGiveUpWithoutContactMs;
  for (std::int64_t elapsed_ms = 100; elapsed_ms < give_up_ms; elapsed_ms += 100) {
    const auto update =
        ai.advance(input_at(kFarTargetInsideLeashMm, kStart + elapsed_ms * kMillisecond));
    ASSERT_FALSE(update.aggro_released) << "still chasing at " << elapsed_ms << " ms";
  }
  EXPECT_EQ(ai.state(), NpcAiState::Engaged);

  const auto released =
      ai.advance(input_at(kFarTargetInsideLeashMm, kStart + give_up_ms * kMillisecond));
  EXPECT_TRUE(released.aggro_released);
  EXPECT_TRUE(released.target_cleared);
  EXPECT_TRUE(released.cancel_action);
  ASSERT_TRUE(released.motion_start);
  EXPECT_EQ(released.motion_start->destination.x_mm, home_position().x_mm);
  EXPECT_EQ(ai.state(), NpcAiState::Returning);
}

TEST(NpcAi, AHitDuringTheChasePostponesTheGiveUp) {
  auto ai = ai_chasing_far_target();
  for (std::int64_t elapsed_ms = 100; elapsed_ms <= 12000; elapsed_ms += 100) {
    auto input = input_at(kFarTargetInsideLeashMm, kStart + elapsed_ms * kMillisecond);
    input.damaged = elapsed_ms == 8000;
    ASSERT_FALSE(ai.advance(input).aggro_released) << "still chasing at " << elapsed_ms << " ms";
  }
  EXPECT_EQ(ai.state(), NpcAiState::Engaged);
}

TEST(NpcAi, AMonsterCarriedBeyondItsLeashReturnsEvenWhileItsTargetIsInside) {
  auto ai = make_ai();
  ASSERT_TRUE(ai.advance(input_at(1000, kStart)).acquired);

  // An attack lunge (owned by combat) can carry the unit past its leash; the target stays
  // inside it, so only the monster's own distance from home can trigger the return.
  ai.place({8000, 51000, 0, 0, 0.0F});
  const auto update = ai.advance(input_at(49000, kStart + 100 * kMillisecond));
  EXPECT_TRUE(update.aggro_released);
  EXPECT_EQ(ai.state(), NpcAiState::Returning);
}

TEST(NpcAi, ADisabledPerceptionConeCanHaveZeroAngleAndStillRetaliate) {
  auto definition = ai_definition_json();
  definition["search_circle_range_mm"] = 0;
  definition["search_cone_range_mm"] = 0;
  definition["search_half_cone_angle"] = 0;
  auto parsed = NpcAiDefinition::from_json(definition);
  ASSERT_TRUE(parsed);
  NpcAi ai(*parsed, home_position());
  auto target = input_at(5000, kStart);
  EXPECT_FALSE(ai.advance(target).acquired);
  target.damaged = true;
  target.ticks += 250 * kMillisecond;
  EXPECT_TRUE(ai.advance(target).acquired);
  EXPECT_TRUE(ai.moving());

  definition["search_cone_range_mm"] = 5000;
  EXPECT_THROW(NpcAiDefinition::from_json(definition), std::runtime_error);
}

TEST(NpcAi, RouteFacesItsDirectionBeforeTheFirstMovementPoll) {
  auto ai = make_ai();
  auto target = input_at(0, kStart);
  target.player_position.y_mm = 10000;
  target.damaged = true;

  const auto started = ai.advance(target);
  ASSERT_TRUE(started.motion_start);
  EXPECT_TRUE(ai.moving());
  EXPECT_FLOAT_EQ(ai.position().yaw, 90.0F);
  EXPECT_FLOAT_EQ(started.motion_start->start_position.yaw, 90.0F);
  EXPECT_EQ(ai.position().y_mm, 0);
}

TEST(NpcAi, ReturnBeginsAtTheCurrentRoutePositionWhenTargetIsLostBetweenPolls) {
  auto ai = make_ai();
  auto target = input_at(10000, kStart);
  target.damaged = true;
  ai.advance(target);
  target.damaged = false;
  target.ticks += 100 * kMillisecond;
  ai.advance(target);

  target.player_alive = false;
  target.ticks += 50 * kMillisecond;
  const auto returning = ai.advance(target);
  ASSERT_TRUE(returning.motion_start);
  EXPECT_EQ(returning.motion_start->start_position.x_mm, 450);
  EXPECT_FLOAT_EQ(returning.motion_start->start_position.yaw, 180.0F);
  EXPECT_EQ(returning.motion_start->end_position.x_mm, 0);
  EXPECT_TRUE(ai.moving());
}

TEST(NpcAi, RejectsInvalidDefinitions) {
  auto definition = ai_definition_json();
  definition["run_speed_mm_per_second"] = 0;
  EXPECT_THROW(NpcAiDefinition::from_json(definition), std::runtime_error);

  definition = ai_definition_json();
  definition["combat_distance_min_mm"] = 3000;
  EXPECT_THROW(NpcAiDefinition::from_json(definition), std::runtime_error);

  definition = ai_definition_json();
  definition["search_half_cone_angle"] = 181;
  EXPECT_THROW(NpcAiDefinition::from_json(definition), std::runtime_error);
}

TEST(NpcAi, RejectsMotionDeadlinesOutsideTheTickRange) {
  auto ai = make_ai();
  auto input = input_at(10000, std::numeric_limits<std::int64_t>::max() - kMillisecond);
  input.damaged = true;
  EXPECT_THROW(ai.advance(input), std::runtime_error);
}

TEST(NpcAiDefinition, PresentSpeedFieldsReturnDefinition) {
  auto json = ai_definition_json();
  auto definition = NpcAiDefinition::from_json(json);
  ASSERT_TRUE(definition.has_value());
  EXPECT_DOUBLE_EQ(definition->run_speed_mm_per_second, 3000.0);
  EXPECT_EQ(definition->move_speed_rate, 1000);
}

TEST(NpcAiDefinition, AbsentSpeedFieldsDisableAi) {
  auto json = ai_definition_json();
  json.erase("run_speed_mm_per_second");
  json.erase("move_speed_rate");
  EXPECT_FALSE(NpcAiDefinition::from_json(json).has_value());
}

TEST(NpcAiDefinition, HalfPresentSpeedPairThrows) {
  auto speed_only = ai_definition_json();
  speed_only.erase("move_speed_rate");
  EXPECT_THROW(NpcAiDefinition::from_json(speed_only), std::runtime_error);

  auto rate_only = ai_definition_json();
  rate_only.erase("run_speed_mm_per_second");
  EXPECT_THROW(NpcAiDefinition::from_json(rate_only), std::runtime_error);
}

TEST(NpcAiDefinition, InvalidPresentSpeedFieldsThrow) {
  auto zero_speed = ai_definition_json();
  zero_speed["run_speed_mm_per_second"] = 0.0;
  EXPECT_THROW(NpcAiDefinition::from_json(zero_speed), std::runtime_error);

  auto negative_rate = ai_definition_json();
  negative_rate["move_speed_rate"] = 0;
  EXPECT_THROW(NpcAiDefinition::from_json(negative_rate), std::runtime_error);

  auto non_numeric = ai_definition_json();
  non_numeric["run_speed_mm_per_second"] = "fast";
  EXPECT_THROW(NpcAiDefinition::from_json(non_numeric), nlohmann::json::exception);
}

} // namespace
} // namespace chronicles
