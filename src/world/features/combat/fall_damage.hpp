#pragma once

#include "persistence/character_state_types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace chronicles {

// Local gameplay policy. The safe height is the catalog's FallHeightSafe (centimeters)
// converted to wire millimeters; that unit interpretation and the damage curve are Candidate.
struct FallDamageRules {
  std::int64_t safe_height_mm{10000};
  std::int64_t lethal_excess_mm{20000};

  int damage(std::int64_t height_mm, int maximum_health) const;
};

// One player's accepted movement stream. Only Falling -> Normal/Crouching is a landing.
// Other modes break the fall, preventing a swim, glide or climb from being charged as impact.
class FallTracker {
public:
  std::optional<std::int64_t> observe(const WorldPosition &position, std::string_view mode);
  void continue_on_map(int map_id);
  void reset();

private:
  std::optional<WorldPosition> grounded_;
  std::optional<int> map_id_;
  std::optional<std::int64_t> peak_z_mm_;
};

} // namespace chronicles
