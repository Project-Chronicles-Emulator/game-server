#include "world/features/npc/npc_ai.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace chronicles {
namespace {

constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr std::int64_t kTicksPerSecond = 10000000;
constexpr std::int64_t kIdleThinkIntervalMs = 250;
constexpr std::int64_t kMovementIntervalMs = 100;
constexpr std::int64_t kMaximumDistanceMm = 1000000000;
constexpr std::int64_t kMaximumResetDelayMs = 86400000;
constexpr double kMaximumSpeedMmPerSecond = 1000000.0;
constexpr double kMovementToleranceMm = kNpcPursuitSettleToleranceMm;
constexpr double kMotionRedirectThresholdMm = 1000.0;
// Leave a small buffer beyond the minimum so millimetre rounding does not re-enter the dead zone.
constexpr double kRetreatRangeMarginMm = 100.0;

std::int64_t integer_field(const nlohmann::json &document, const char *key, std::int64_t minimum,
                           std::int64_t maximum) {
  const auto value = document.at(key).get<std::int64_t>();
  if (value < minimum || value > maximum) {
    throw std::runtime_error(std::string("Invalid NPC AI field: ") + key);
  }
  return value;
}

double floating_field(const nlohmann::json &document, const char *key, double minimum,
                      double maximum) {
  const auto value = document.at(key).get<double>();
  if (!std::isfinite(value) || value < minimum || value > maximum) {
    throw std::runtime_error(std::string("Invalid NPC AI field: ") + key);
  }
  return value;
}

double distance_squared(const WorldPosition &left, const WorldPosition &right) {
  const double x = static_cast<double>(left.x_mm) - right.x_mm;
  const double y = static_cast<double>(left.y_mm) - right.y_mm;
  const double z = static_cast<double>(left.z_mm) - right.z_mm;
  return x * x + y * y + z * z;
}

double distance(const WorldPosition &left, const WorldPosition &right) {
  return std::sqrt(distance_squared(left, right));
}

WorldPosition interpolate_position(const WorldPosition &start, const WorldPosition &destination,
                                   double ratio) {
  const double x = static_cast<double>(destination.x_mm) - start.x_mm;
  const double y = static_cast<double>(destination.y_mm) - start.y_mm;
  const double z = static_cast<double>(destination.z_mm) - start.z_mm;
  WorldPosition position = start;
  position.x_mm = start.x_mm + std::llround(x * ratio);
  position.y_mm = start.y_mm + std::llround(y * ratio);
  position.z_mm = start.z_mm + std::llround(z * ratio);
  if (x != 0 || y != 0) {
    position.yaw = static_cast<float>(std::atan2(y, x) * 180.0 / std::numbers::pi);
  }
  return position;
}

} // namespace

std::optional<NpcAiDefinition> NpcAiDefinition::from_json(const nlohmann::json &document) {
  const bool has_speed = document.contains("run_speed_mm_per_second");
  const bool has_rate = document.contains("move_speed_rate");
  if (!has_speed && !has_rate) {
    return std::nullopt;
  }
  if (has_speed != has_rate) {
    throw std::runtime_error("Invalid NPC AI field: run_speed_mm_per_second/move_speed_rate pair");
  }
  NpcAiDefinition definition;
  definition.search_circle_range_mm =
      integer_field(document, "search_circle_range_mm", 0, kMaximumDistanceMm);
  definition.search_cone_range_mm =
      integer_field(document, "search_cone_range_mm", 0, kMaximumDistanceMm);
  definition.search_half_cone_angle =
      floating_field(document, "search_half_cone_angle", 0.0, 180.0);
  if (definition.search_cone_range_mm > 0 && definition.search_half_cone_angle == 0.0) {
    throw std::runtime_error("Invalid NPC AI field: search_half_cone_angle");
  }
  definition.hearing_range_mm = integer_field(document, "hearing_range_mm", 0, kMaximumDistanceMm);
  definition.leash_range_mm = integer_field(document, "leash_range_mm", 1, kMaximumDistanceMm);
  definition.reset_delay_ms = integer_field(document, "reset_delay_ms", 0, kMaximumResetDelayMs);
  definition.combat_distance_min_mm =
      integer_field(document, "combat_distance_min_mm", 0, kMaximumDistanceMm);
  definition.combat_distance_max_mm =
      integer_field(document, "combat_distance_max_mm", 1, kMaximumDistanceMm);
  if (definition.combat_distance_min_mm > definition.combat_distance_max_mm) {
    throw std::runtime_error("Invalid NPC AI combat distance range");
  }
  definition.run_speed_mm_per_second =
      floating_field(document, "run_speed_mm_per_second", 0.0, kMaximumSpeedMmPerSecond);
  if (definition.run_speed_mm_per_second == 0.0) {
    throw std::runtime_error("Invalid NPC AI field: run_speed_mm_per_second");
  }
  definition.move_speed_rate = integer_field(document, "move_speed_rate", 1, 10000);
  return definition;
}

NpcAi::NpcAi(NpcAiDefinition definition, WorldPosition home_position)
    : definition_(std::move(definition)), home_position_(home_position), position_(home_position) {}

NpcAiUpdate NpcAi::advance(const NpcAiInput &input) {
  NpcAiUpdate update;
  if (!input.monster_active || !input.monster_alive || !input.monster_hostile ||
      input.player_position.map_id != home_position_.map_id) {
    // A living, active monster that loses its target needs a visible state
    // change. Do not overwrite Demise or removal for dead/inactive monsters.
    update.aggro_released = state_ == NpcAiState::Engaged;
    update.target_cleared =
        state_ == NpcAiState::Engaged && input.monster_alive && input.monster_active;
    update.cancel_action = input.action_active || state_ == NpcAiState::Engaged;
    complete_motion(update, input.ticks);
    state_ = NpcAiState::Idle;
    next_action_tick_.reset();
    last_advance_tick_.reset();
    reset_tick_.reset();
    last_contact_tick_.reset();
    return update;
  }

  if (last_advance_tick_ && input.ticks < *last_advance_tick_) {
    return update;
  }

  // Contact and disengagement are evaluated on every call, not only on think ticks: a hit that
  // lands between thinks is reported once and must still count.
  if (state_ == NpcAiState::Engaged) {
    record_contact(input);
    if (should_disengage(input)) {
      begin_return(input.ticks, update);
      return update;
    }
  }

  if (next_action_tick_ && input.ticks < *next_action_tick_) {
    return update;
  }

  last_advance_tick_ = input.ticks;

  if (state_ == NpcAiState::Returning) {
    if (!reset_tick_) {
      if (distance_squared(position_, home_position_) > 0) {
        start_motion(home_position_, 0, input.ticks, update);
        advance_motion(input.ticks, update);
        if (motion_) {
          schedule(input.ticks, kMovementIntervalMs);
          return update;
        }
      }
      reset_tick_ = input.ticks + definition_.reset_delay_ms * kTicksPerMillisecond;
    }

    if (input.ticks < *reset_tick_) {
      next_action_tick_ = reset_tick_;
      return update;
    }

    state_ = NpcAiState::Idle;
    reset_tick_.reset();
    update.reset_health = true;
    schedule(input.ticks, kIdleThinkIntervalMs);
    return update;
  }

  const bool perceived = input.perception_enabled && can_detect(input.player_position);
  if (state_ == NpcAiState::Idle && input.player_alive && (input.damaged || perceived)) {
    state_ = NpcAiState::Engaged;
    last_contact_tick_ = input.ticks;
    update.acquired = true;
  }

  if (state_ == NpcAiState::Engaged) {
    const auto engagement_range_mm = engagement_range(input);
    if (input.action_active) {
      advance_motion(input.ticks, update);
      complete_motion(update, input.ticks);
    } else {
      // Advance the previously announced trajectory before reacting to the
      // newest target position. New goals must not inherit elapsed travel time.
      advance_motion(input.ticks, update);
      const double target_distance = distance(position_, input.player_position);
      if (target_distance + kMovementToleranceMm < input.minimum_engagement_range_mm) {
        // A ranged-only NPC creates room instead of closing indefinitely inside its dead zone.
        // This is a local steering policy; obstacle avoidance still belongs to navigation.
        double away_x = static_cast<double>(position_.x_mm) - input.player_position.x_mm;
        double away_y = static_cast<double>(position_.y_mm) - input.player_position.y_mm;
        double planar_distance = std::hypot(away_x, away_y);
        if (planar_distance == 0) {
          away_x = 1;
          away_y = 0;
          planar_distance = 1;
        }
        const double desired_distance =
            static_cast<double>(input.minimum_engagement_range_mm) + kRetreatRangeMarginMm;
        WorldPosition retreat = position_;
        retreat.x_mm =
            input.player_position.x_mm + std::llround(away_x / planar_distance * desired_distance);
        retreat.y_mm =
            input.player_position.y_mm + std::llround(away_y / planar_distance * desired_distance);
        if (distance(retreat, home_position_) <= definition_.leash_range_mm) {
          start_motion(retreat, 0, input.ticks, update);
        } else {
          complete_motion(update, input.ticks);
        }
      } else if (target_distance > engagement_range_mm + kMovementToleranceMm) {
        start_motion(input.player_position, engagement_range_mm, input.ticks, update);
      } else {
        complete_motion(update, input.ticks);
      }
    }
    schedule(input.ticks, kMovementIntervalMs);
  } else {
    schedule(input.ticks, kIdleThinkIntervalMs);
  }
  return update;
}

NpcAiState NpcAi::state() const {
  return state_;
}

const WorldPosition &NpcAi::position() const {
  return position_;
}

bool NpcAi::moving() const {
  return motion_.has_value();
}

void NpcAi::reset_at(const WorldPosition &position) {
  position_ = position;
  motion_.reset();
  state_ = NpcAiState::Idle;
  reset_tick_.reset();
  last_advance_tick_.reset();
  next_action_tick_.reset();
  last_contact_tick_.reset();
}

bool NpcAi::can_retarget(const WorldPosition &target) const {
  const auto leash_range = static_cast<double>(definition_.leash_range_mm);
  return target.map_id == home_position_.map_id &&
         distance(target, home_position_) <= leash_range;
}

void NpcAi::pause_until(std::int64_t ticks, std::int64_t resume_tick) {
  if (const auto current = route_position_at(ticks)) {
    position_ = *current;
  }
  motion_.reset();
  last_advance_tick_ = ticks;
  next_action_tick_ = std::max(ticks, resume_tick);
  if (state_ == NpcAiState::Engaged) {
    last_contact_tick_ = *next_action_tick_;
  }
}

void NpcAi::resume_after_pause(std::int64_t ticks) {
  next_action_tick_ = ticks;
  if (state_ == NpcAiState::Engaged) {
    last_contact_tick_ = ticks;
  }
}

void NpcAi::place(const WorldPosition &position) {
  position_ = position;
}

std::optional<WorldPosition> NpcAi::route_position_at(std::int64_t ticks) const {
  if (!motion_) {
    return std::nullopt;
  }
  const auto &motion = *motion_;
  const auto duration = motion.start.end_tick - motion.start_tick;
  const double elapsed_fraction = std::clamp(
      static_cast<double>(ticks - motion.start_tick) / static_cast<double>(duration), 0.0, 1.0);
  return interpolate_position(motion.start.start_position, motion.start.end_position,
                              elapsed_fraction);
}

std::optional<WorldPosition> NpcAi::route_end() const {
  if (!motion_) {
    return std::nullopt;
  }
  return motion_->start.end_position;
}

const NpcAiDefinition &NpcAi::definition() const {
  return definition_;
}

bool NpcAi::idle_and_not_due(std::int64_t ticks, int player_map_id) const {
  return state_ == NpcAiState::Idle && next_action_tick_ && ticks < *next_action_tick_ &&
         player_map_id == home_position_.map_id;
}

std::optional<std::int64_t> NpcAi::next_action_tick() const {
  return next_action_tick_;
}

bool NpcAi::can_detect(const WorldPosition &player_position) const {
  const auto range = static_cast<double>(definition_.search_circle_range_mm);
  if (range > 0 && distance_squared(position_, player_position) <= range * range) {
    return true;
  }

  const auto cone_range = static_cast<double>(definition_.search_cone_range_mm);
  if (cone_range == 0 || distance_squared(position_, player_position) > cone_range * cone_range) {
    return false;
  }

  const double x = static_cast<double>(player_position.x_mm) - position_.x_mm;
  const double y = static_cast<double>(player_position.y_mm) - position_.y_mm;
  if (x == 0 && y == 0) {
    return true;
  }

  const double facing = position_.yaw * std::numbers::pi / 180.0;
  const double direction = std::atan2(y, x);
  const double difference = std::atan2(std::sin(direction - facing), std::cos(direction - facing));
  return std::abs(difference) <= definition_.search_half_cone_angle * std::numbers::pi / 180.0;
}

std::int64_t NpcAi::engagement_range(const NpcAiInput &input) const {
  if (input.engagement_range_mm > 0) {
    return input.engagement_range_mm;
  }
  return definition_.combat_distance_max_mm;
}

void NpcAi::record_contact(const NpcAiInput &input) {
  const auto current_position = route_position_at(input.ticks).value_or(position_);
  const double target_distance = distance(current_position, input.player_position);
  const bool target_in_reach =
      target_distance <= static_cast<double>(engagement_range(input)) + kMovementToleranceMm;
  if (input.action_active || input.damaged || target_in_reach) {
    last_contact_tick_ = input.ticks;
  }
}

bool NpcAi::should_disengage(const NpcAiInput &input) const {
  if (!input.player_alive) {
    return true;
  }

  const auto leash_range = static_cast<double>(definition_.leash_range_mm);
  if (distance(input.player_position, home_position_) > leash_range) {
    return true;
  }

  // The monster itself must not be dragged beyond its leash either, for example while it
  // chases a target that circles at the leash edge.
  const auto current_position = route_position_at(input.ticks).value_or(position_);
  if (distance(current_position, home_position_) > leash_range) {
    return true;
  }

  const auto give_up_ticks = kNpcGiveUpWithoutContactMs * kTicksPerMillisecond;
  return last_contact_tick_ && input.ticks - *last_contact_tick_ >= give_up_ticks;
}

void NpcAi::begin_return(std::int64_t ticks, NpcAiUpdate &update) {
  // Settle the old route at the interruption time before starting the return leg.
  advance_motion(ticks, update);
  state_ = NpcAiState::Returning;
  complete_motion(update, ticks);
  last_advance_tick_ = ticks;
  last_contact_tick_.reset();
  if (distance_squared(position_, home_position_) == 0) {
    reset_tick_ = ticks + definition_.reset_delay_ms * kTicksPerMillisecond;
  } else {
    reset_tick_.reset();
  }

  update.target_cleared = true;
  update.aggro_released = true;
  update.cancel_action = true;
  if (!reset_tick_) {
    start_motion(home_position_, 0, ticks, update);
  }
  schedule(ticks, kMovementIntervalMs);
}

void NpcAi::start_motion(const WorldPosition &destination, std::int64_t stop_distance_mm,
                         std::int64_t ticks, NpcAiUpdate &update) {
  if (motion_ && motion_->stop_distance_mm == stop_distance_mm &&
      distance(motion_->start.destination, destination) <= kMotionRedirectThresholdMm) {
    return;
  }

  const double travel_distance = distance(position_, destination) - stop_distance_mm;
  if (travel_distance <= kMovementToleranceMm) {
    complete_motion(update, ticks);
    if (stop_distance_mm == 0) {
      update.moved = update.moved || position_.x_mm != destination.x_mm ||
                     position_.y_mm != destination.y_mm || position_.z_mm != destination.z_mm;
      position_.x_mm = destination.x_mm;
      position_.y_mm = destination.y_mm;
      position_.z_mm = destination.z_mm;
    }
    return;
  }

  const double speed = definition_.run_speed_mm_per_second * definition_.move_speed_rate / 1000.0;
  if (!std::isfinite(speed) || speed <= 0) {
    throw std::runtime_error("NPC motion requires a finite positive speed");
  }
  const long double duration_ticks =
      std::ceil(static_cast<long double>(travel_distance) / speed * kTicksPerSecond);
  if (!std::isfinite(duration_ticks) || ticks < 0 ||
      duration_ticks >=
          static_cast<long double>(std::numeric_limits<std::int64_t>::max() - ticks)) {
    throw std::runtime_error("NPC motion deadline exceeds tick range");
  }

  const auto end_tick =
      ticks + std::max<std::int64_t>(1, static_cast<std::int64_t>(duration_ticks));
  complete_motion(update, ticks);
  const auto end_position = interpolate_position(
      position_, destination, travel_distance / distance(position_, destination));
  // The first moving notification must face along the route, including a return or retreat.
  position_.yaw = end_position.yaw;
  motion_ = MotionPlan{{position_, destination, end_tick, end_position}, ticks, stop_distance_mm};
  update.motion_start = motion_->start;
}

void NpcAi::complete_motion(NpcAiUpdate &update, std::optional<std::int64_t> interrupted_tick) {
  if (!motion_) {
    return;
  }
  update.motion_complete_tick = interrupted_tick.value_or(motion_->start.end_tick);
  motion_.reset();
}

void NpcAi::advance_motion(std::int64_t ticks, NpcAiUpdate &update) {
  if (!motion_) {
    return;
  }
  const auto &motion = *motion_;
  const auto &start = motion.start.start_position;
  const auto duration = motion.start.end_tick - motion.start_tick;
  const double elapsed_fraction = std::clamp(
      static_cast<double>(ticks - motion.start_tick) / static_cast<double>(duration), 0.0, 1.0);
  const auto previous = position_;

  // Interpolate from the announced origin, avoiding accumulated rounding drift
  // and keeping arrival tied to the original deadline, even after coarse polls.
  position_ = interpolate_position(start, motion.start.end_position, elapsed_fraction);
  const bool moved = position_.x_mm != previous.x_mm || position_.y_mm != previous.y_mm ||
                     position_.z_mm != previous.z_mm;
  if (!moved) {
    position_.yaw = previous.yaw;
  }
  update.moved = update.moved || moved;
  if (elapsed_fraction >= 1.0) {
    complete_motion(update);
  }
}

void NpcAi::schedule(std::int64_t ticks, std::int64_t interval_ms) {
  next_action_tick_ = ticks + interval_ms * kTicksPerMillisecond;
  if (motion_ && motion_->start.end_tick > ticks) {
    next_action_tick_ = std::min(*next_action_tick_, motion_->start.end_tick);
  }
}

} // namespace chronicles
