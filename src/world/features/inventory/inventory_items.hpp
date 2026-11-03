#pragma once

#include "persistence/character_repository.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace chronicles {

// Entry snapshot item projection; player observer equipment/nub projection belongs to Core.
nlohmann::json character_inventory(const Character &character);

// The tutorial reward path requires the entire grant to fit, represented by optional.
// Core's existing available_inventory_slots owns the common lowest-slot search.
std::optional<std::vector<int>> lowest_free_inventory_slots(const Character &character,
                                                            std::size_t needed);

} // namespace chronicles
