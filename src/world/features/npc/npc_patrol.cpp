#include "world/features/npc/npc_patrol.hpp"

#include "common/clock_ticks.hpp"
#include "world/features/npc/npc_population.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr std::int64_t kTicksPerSecond = 10000000;
constexpr std::int64_t kUpdateMilliseconds = 500;
constexpr std::int64_t kMaximumCycleTicks = 24 * 60 * 60 * kTicksPerSecond;
constexpr std::size_t kMaximumNodes = 1024;
constexpr std::int64_t kMinimumPredictionMilliseconds = 16;
constexpr std::uint16_t kMoveNotificationOpcode = 177;
constexpr std::uint16_t kStartX5MotionNotificationOpcode = 69;

[[noreturn]] void reject(const std::string &field) {
  throw std::runtime_error("Invalid NPC patrol field: " + field);
}

std::int64_t integer(const Json &object, const char *key, std::int64_t minimum,
                     std::int64_t maximum) {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_number_integer() || *found < minimum ||
      *found > maximum) {
    reject(key);
  }
  return found->get<std::int64_t>();
}

UnitPositionMm position(const Json &node) {
  const auto found = node.find("position_mm");
  if (found == node.end() || !found->is_object()) {
    reject("position_mm");
  }
  constexpr auto kMin = std::numeric_limits<int>::min();
  constexpr auto kMax = std::numeric_limits<int>::max();
  return {static_cast<int>(integer(*found, "x", kMin, kMax)),
          static_cast<int>(integer(*found, "y", kMin, kMax)),
          static_cast<int>(integer(*found, "z", kMin, kMax))};
}

float facing(const UnitPositionMm &from, const UnitPositionMm &to) {
  constexpr double kDegreesPerRadian = 180.0 / 3.14159265358979323846;
  return static_cast<float>(std::atan2(static_cast<double>(to.y_mm) - from.y_mm,
                                       static_cast<double>(to.x_mm) - from.x_mm) *
                            kDegreesPerRadian);
}

Json wire_position(const UnitPositionMm &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

Json validated_motion(const Json &node) {
  const auto found = node.find("motion");
  if (found == node.end()) {
    return nullptr;
  }
  if (!found->is_object()) {
    reject("motion");
  }
  Json result{{"game_anim_mapping_id", integer(*found, "game_anim_mapping_id", 0, 1000000)},
              {"loop_count", integer(*found, "loop_count", -1, 100000)}};
  if (found->contains("target_anim_stance_mapping_id")) {
    result["target_anim_stance_mapping_id"] =
        integer(*found, "target_anim_stance_mapping_id", 0, 1000000);
  }
  return result;
}

} // namespace

NpcPatrolRoute NpcPatrolRoute::from_json(const Json &document) {
  if (!document.is_object() || document.value("mode", std::string()) != "circle_forward") {
    reject("mode");
  }
  const auto nodes = document.find("nodes");
  if (nodes == document.end() || !nodes->is_array() || nodes->size() < 2 ||
      nodes->size() > kMaximumNodes) {
    reject("nodes");
  }

  NpcPatrolRoute route;
  const auto append = [&](Phase phase, std::int64_t duration) {
    if (duration <= 0 || duration > kMaximumCycleTicks - route.cycle_ticks_) {
      reject("cycle duration");
    }
    phase.starts = route.cycle_ticks_;
    route.cycle_ticks_ += duration;
    phase.ends = route.cycle_ticks_;
    route.phases_.push_back(std::move(phase));
  };

  bool has_movement = false;
  for (std::size_t index = 0; index < nodes->size(); ++index) {
    const auto &node = nodes->at(index);
    const auto from = position(node);
    const auto to = position(nodes->at((index + 1) % nodes->size()));
    const auto speed_value = node.find("speed_mm_per_second");
    if (speed_value == node.end() || !speed_value->is_number() ||
        !std::isfinite(speed_value->get<double>()) || speed_value->get<double>() < 1 ||
        speed_value->get<double>() > 100000) {
      reject("speed_mm_per_second");
    }
    const auto speed = speed_value->get<double>();
    const auto wait_ms = integer(node, "wait_ms", 0, 3600000);
    const auto detail = node.value("move_detail", std::string());
    if (detail != "MoveDetail_Walk" && detail != "MoveDetail_Run") {
      reject("move_detail");
    }
    Phase phase;
    phase.from = from;
    phase.to = from;
    phase.yaw = facing(from, to);
    if (node.contains("stance")) {
      phase.stance = static_cast<int>(integer(node, "stance", 0, 1000000));
    }
    phase.motion = validated_motion(node);
    if ((!phase.motion.is_null() || phase.stance) && wait_ms == 0) {
      reject("motion without wait");
    }
    if (node.contains("wait_yaw")) {
      const auto &yaw = node.at("wait_yaw");
      if (!yaw.is_number() || !std::isfinite(yaw.get<double>()) ||
          std::abs(yaw.get<double>()) > 360000) {
        reject("wait_yaw");
      }
      phase.yaw = yaw.get<float>();
    }
    if (wait_ms > 0) {
      append(phase, wait_ms * kTicksPerMillisecond);
    }

    const double distance = distance_mm(from, to);
    if (distance == 0) {
      continue;
    }
    const double duration = std::ceil(distance / speed * kTicksPerSecond);
    if (!std::isfinite(duration) || duration > kMaximumCycleTicks) {
      reject("leg duration");
    }
    phase.to = to;
    phase.moving = true;
    phase.move_detail = detail;
    phase.yaw = facing(from, to);
    phase.stance.reset();
    phase.motion = nullptr;
    append(std::move(phase), static_cast<std::int64_t>(duration));
    has_movement = true;
  }
  if (!has_movement) {
    reject("route without movement");
  }
  return route;
}

void NpcPatrolRoute::sample(std::int64_t elapsed_ticks, NpcPatrolSample &out) const {
  if (elapsed_ticks < 0) {
    reject("negative elapsed ticks");
  }
  const auto cycle = elapsed_ticks / cycle_ticks_;
  const auto offset = elapsed_ticks % cycle_ticks_;
  const auto found = std::ranges::upper_bound(phases_, offset, {}, &Phase::ends);
  const auto phase_index = static_cast<std::int64_t>(found - phases_.begin());
  const double fraction = static_cast<double>(offset - found->starts) /
                          static_cast<double>(found->ends - found->starts);
  const auto coordinate = [&](int from, int to) {
    return static_cast<int>(std::llround(from + (static_cast<double>(to) - from) * fraction));
  };

  out.position = {coordinate(found->from.x_mm, found->to.x_mm),
                  coordinate(found->from.y_mm, found->to.y_mm),
                  coordinate(found->from.z_mm, found->to.z_mm)};
  out.yaw = found->yaw;
  out.moving = found->moving;
  out.move_detail = found->move_detail;
  out.phase_id = cycle * static_cast<std::int64_t>(phases_.size()) + phase_index;
  out.phase_started = cycle * cycle_ticks_ + found->starts;
  out.remaining_ticks = found->ends - offset;
  out.stance = found->stance;
  out.motion = found->motion;
}

NpcPatrolSample NpcPatrolRoute::sample(std::int64_t elapsed_ticks) const {
  NpcPatrolSample result;
  sample(elapsed_ticks, result);
  return result;
}

NpcPatrolSession::NpcPatrolSession(const NpcPopulation &population, std::int64_t epoch_tick,
                                   npc::ReactionTimeline timeline)
    : population_(population), epoch_tick_(epoch_tick), timeline_(std::move(timeline)) {
  if (epoch_tick < 0) {
    reject("negative epoch tick");
  }
}

void NpcPatrolSession::synchronize(std::int64_t ticks, UnitRegistry &registry,
                                   std::string_view instance_key) const {
  const auto elapsed = std::max<std::int64_t>(0, ticks - epoch_tick_);
  for (const auto &unit : population_.units()) {
    if (!unit.placement.patrol) {
      continue;
    }
    const auto sample =
        timeline_ ? timeline_(unit.unit_id, elapsed) : npc::ReactionTimelineSample{elapsed};
    const auto pose = unit.placement.patrol->sample(sample.route_elapsed);
    if (!registry.update_position(unit.unit_id, population_.map_id(), instance_key,
                                  pose.position)) {
      throw std::runtime_error("NPC patrol lost its announced map/instance ownership");
    }
  }
}

std::vector<GameplayPacket> NpcPatrolSession::start(std::int64_t ticks,
                                                    UnitRegistry &registry,
                                                    std::string_view instance_key,
                                                    const NpcSightTracker *sight) {
  if (started_) {
    return {};
  }
  started_ = true;
  next_tick_ = ticks;
  return advance(ticks, registry, instance_key, sight);
}

std::vector<GameplayPacket> NpcPatrolSession::advance(std::int64_t ticks,
                                                      UnitRegistry &registry,
                                                      std::string_view instance_key,
                                                      const NpcSightTracker *sight) {
  if (!next_tick_ || ticks < *next_tick_) {
    return {};
  }
  const auto elapsed = std::max<std::int64_t>(0, ticks - epoch_tick_);
  next_tick_.reset();
  std::vector<GameplayPacket> packets;
  NpcPatrolSample pose;
  for (const auto &unit : population_.units()) {
    const auto &route = unit.placement.patrol;
    if (!route) {
      continue;
    }
    // One sample per unit per wake-up: the same pose feeds the registry
    // position update (formerly synchronize's own sample) and the due/packet
    // decision below.
    const auto reaction =
        timeline_ ? timeline_(unit.unit_id, elapsed) : npc::ReactionTimelineSample{elapsed};
    route->sample(reaction.route_elapsed, pose);
    pose.phase_started += elapsed - reaction.route_elapsed;
    if (reaction.active) {
      pose.moving = false;
      pose.phase_id = -reaction.started - 1;
      pose.phase_started = reaction.started;
      pose.remaining_ticks = reaction.ends - elapsed;
      pose.yaw = reaction.yaw;
      pose.stance.reset();
      pose.motion = {{"game_anim_mapping_id", reaction.animation_id}, {"loop_count", 1}};
    }
    ++stats_.route_samples;
    if (!registry.update_position(unit.unit_id, population_.map_id(), instance_key,
                                  pose.position)) {
      throw std::runtime_error("NPC patrol lost its announced map/instance ownership");
    }
    ++stats_.position_updates;

    const auto interval =
        std::min(kUpdateMilliseconds * kTicksPerMillisecond, pose.remaining_ticks);
    const auto previous = last_phases_.find(unit.unit_id);
    const bool changed = previous == last_phases_.end() || previous->second != pose.phase_id;
    auto &unit_deadline = next_updates_[unit.unit_id];
    const bool due = changed || ticks >= unit_deadline;
    if (due) {
      // Bound timer wakeups even if an authored route contains nearly coincident points.
      unit_deadline =
          ticks + std::max(kMinimumPredictionMilliseconds * kTicksPerMillisecond, interval);
    }
    if (!next_tick_ || unit_deadline < *next_tick_) {
      next_tick_ = unit_deadline;
    }
    if (!due) {
      continue;
    }
    if (!pose.moving && !changed) {
      continue;
    }

    // Internal state must track every due unit exactly as before; only the
    // packet build is skipped for a unit outside the client's view.
    last_phases_[unit.unit_id] = pose.phase_id;
    if (!pose.moving) {
      stopped_units_.insert(unit.unit_id);
    }

    const bool emit_move = sight == nullptr || sight->visible(unit.unit_id);
    if (!emit_move) {
      ++stats_.packets_dropped_out_of_sight;
    } else {
      const auto id = std::to_string(unit.unit_id);
      Json move{{"unit_id", id},
                {"tick", wire_monotonic_tick(ticks)},
                {"mode", "MoveMode_Normal"},
                {"detail", pose.moving ? pose.move_detail : "MoveDetail_Stop"},
                {"pos", wire_position(pose.position)},
                {"body_yaw", pose.yaw}};
      if (pose.moving) {
        const auto prediction_ms =
            std::clamp(interval / kTicksPerMillisecond, kMinimumPredictionMilliseconds,
                       kUpdateMilliseconds);
        NpcPatrolSample goal;
        route->sample(reaction.route_elapsed + interval, goal);
        ++stats_.route_samples;
        move["pred_pos"] = wire_position(goal.position);
        move["pred_milli"] = prediction_ms;
        // A work stance must be released before the next walking leg.
        if (stopped_units_.erase(unit.unit_id) != 0 && unit.placement.anim_stance_key) {
          packets.push_back(
              {kStartX5MotionNotificationOpcode,
               {{"unit_id", id},
                {"tick", wire_monotonic_tick(ticks)},
                {"pos", wire_position(pose.position)},
                {"body_yaw", pose.yaw},
                {"anim_stance_change_data",
                 {{"target_anim_stance_mapping_id", *unit.placement.anim_stance_key}}}}});
          ++stats_.packets_built;
        }
      }
      packets.push_back({kMoveNotificationOpcode, std::move(move)});
      ++stats_.packets_built;
    }

    if (!pose.moving && changed && (pose.stance || !pose.motion.is_null())) {
      const bool emit_activity = sight == nullptr || sight->visible(unit.unit_id);
      if (!emit_activity) {
        ++stats_.packets_dropped_out_of_sight;
      } else {
        const auto id = std::to_string(unit.unit_id);
        Json activity{{"unit_id", id},
                      {"tick", wire_monotonic_tick(epoch_tick_ + pose.phase_started)},
                      {"pos", wire_position(pose.position)},
                      {"body_yaw", pose.yaw}};
        if (!pose.motion.is_null()) {
          activity["x5_motion_data"] = pose.motion;
        } else {
          activity["anim_stance_change_data"] = {
              {"target_anim_stance_mapping_id", *pose.stance}};
        }
        packets.push_back({kStartX5MotionNotificationOpcode, std::move(activity)});
        ++stats_.packets_built;
      }
    }
  }
  return packets;
}

std::optional<std::int64_t> NpcPatrolSession::next_action_tick() const {
  return next_tick_;
}

void NpcPatrolSession::wake_at(std::int64_t ticks) {
  if (next_tick_ && ticks < *next_tick_) {
    next_tick_ = ticks;
  }
}

} // namespace chronicles
