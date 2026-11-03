#include "world/features/combat/fall_damage.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace chronicles {

int FallDamageRules::damage(std::int64_t height_mm, int maximum_health) const {
  if (safe_height_mm < 0 || lethal_excess_mm <= 0) {
    throw std::invalid_argument("Invalid fall damage height policy");
  }
  if (maximum_health <= 0 || height_mm <= safe_height_mm) {
    return 0;
  }

  const auto excess_mm = height_mm - safe_height_mm;
  if (excess_mm >= lethal_excess_mm) {
    return maximum_health;
  }

  // Round upward so an unsafe landing always hurts, including low-HP characters.
  // Divide in floating point to avoid overflow for externally configured int64 heights.
  const auto fraction = static_cast<long double>(excess_mm) / lethal_excess_mm;
  const auto amount = std::ceil(fraction * maximum_health);
  return static_cast<int>(std::min(static_cast<long double>(maximum_health), amount));
}

void FallTracker::reset() {
  grounded_.reset();
  map_id_.reset();
  peak_z_mm_.reset();
}

void FallTracker::continue_on_map(int map_id) {
  map_id_ = map_id;
}

std::optional<std::int64_t> FallTracker::observe(const WorldPosition &position,
                                                 std::string_view mode) {
  if (map_id_ && *map_id_ != position.map_id) {
    reset();
  }
  map_id_ = position.map_id;

  if (mode == "MoveMode_Falling") {
    if (!peak_z_mm_) {
      peak_z_mm_ = position.z_mm;
      if (grounded_) {
        peak_z_mm_ = std::max(*peak_z_mm_, static_cast<std::int64_t>(grounded_->z_mm));
      }
    }
    peak_z_mm_ = std::max(*peak_z_mm_, static_cast<std::int64_t>(position.z_mm));
    grounded_.reset();
    return std::nullopt;
  }

  std::optional<std::int64_t> height_mm;
  if (mode == "MoveMode_Normal" || mode == "MoveMode_Crouching") {
    if (peak_z_mm_) {
      height_mm = std::max<std::int64_t>(0, *peak_z_mm_ - position.z_mm);
    }
    grounded_ = position;
  } else {
    grounded_.reset();
  }
  peak_z_mm_.reset();
  return height_mm;
}

} // namespace chronicles
