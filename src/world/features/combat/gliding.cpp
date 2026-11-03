#include "world/features/combat/gliding.hpp"

#include "common/clock_ticks.hpp"

#include <cmath>

namespace chronicles {
namespace {

// Local tolerance for the short gap between a movement report and the start request.
// This is not evidence for an official flight-speed or collision rule.
constexpr double kStartPositionToleranceMm = 5000.0;
constexpr std::uint16_t kStartGlidingFailure = 180;

bool near_position(const nlohmann::json &request, const WorldPosition &position) {
  const auto found = request.find("pos");
  if (found == request.end() || !found->is_object()) {
    return false;
  }

  const double dx = found->value("millimeter_x", 0.0) - position.x_mm;
  const double dy = found->value("millimeter_y", 0.0) - position.y_mm;
  const double dz = found->value("millimeter_z", 0.0) - position.z_mm;
  const double distance_squared = dx * dx + dy * dy + dz * dz;
  return std::isfinite(distance_squared) &&
         distance_squared <= kStartPositionToleranceMm * kStartPositionToleranceMm;
}

} // namespace

void GlidingSession::observe_movement(const nlohmann::json &movement) {
  movement_mode_ = movement.value("mode", std::string("MoveMode_Normal"));
  if (movement_mode_ != "MoveMode_Gliding") {
    active_ = false;
  }
}

void GlidingSession::reset() {
  movement_mode_ = "MoveMode_Normal";
  active_ = false;
}

bool GlidingSession::active() const noexcept {
  return active_;
}

std::optional<GameplayPacket> GlidingSession::start(const nlohmann::json &request,
                                                    const std::optional<WorldPosition> &position,
                                                    GlidingConditions conditions,
                                                    std::int64_t now) {
  const char *error = nullptr;
  const auto yaw = request.value("body_yaw", 0.0);
  if (!position) {
    error = "InvalidState";
  } else if (!std::isfinite(yaw) || !near_position(request, *position)) {
    error = "BadRequest";
  } else if (conditions.dead) {
    error = "Move_GlideWhileDead";
  } else if (conditions.riding) {
    error = "Move_GlideWhileRiding";
  } else if (conditions.crowd_controlled) {
    error = "Move_GlideWhileCCed";
  } else if (conditions.using_skill || conditions.interacting) {
    error = "Move_GlideUsingSkill";
  } else if (active_) {
    error = "Move_GlideStartDuplication";
  } else if (movement_mode_ != "MoveMode_Falling" && movement_mode_ != "MoveMode_Gliding") {
    error = "Move_GlideNotFalling";
  }

  if (!error) {
    // A predicted Gliding move can precede its reliable start packet. It does not by itself
    // mark a start accepted, so a later blocked start still produces the correction.
    active_ = true;
    return std::nullopt;
  }

  // A refusal ends the prediction, including a duplicate start. Use the server's current
  // pose and clock rather than copying an untrusted position/timestamp into the correction.
  active_ = false;
  const auto pose = position.value_or(WorldPosition{});
  return GameplayPacket{
      kStartGlidingFailure,
      {{"ec", error},
       {"body_yaw", pose.yaw},
       {"tick", wire_monotonic_tick(now)},
       {"pos",
        {{"millimeter_x", pose.x_mm}, {"millimeter_y", pose.y_mm}, {"millimeter_z", pose.z_mm}}}}};
}

} // namespace chronicles
