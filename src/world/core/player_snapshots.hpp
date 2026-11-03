#pragma once

#include <nlohmann/json_fwd.hpp>

namespace chronicles {

struct Character;
struct WorldPosition;

// Shared player projection for initial world entry and later observer sight-entry packets.
nlohmann::json player_unit_nub(const Character &character, const WorldPosition &position,
                               const nlohmann::json &stats, bool dead);

} // namespace chronicles
