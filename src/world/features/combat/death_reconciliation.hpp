#pragma once

#include "persistence/character_state_types.hpp"

#include <vector>

namespace chronicles::combat {

enum class DeathReconciliationOutcome { committed, not_committed, conflict };

// Classify only a snapshot returned by a confirmed revision fence while the old character
// admission remains exclusive. A plain read cannot prove that the original write is finished.
DeathReconciliationOutcome classify_fenced_death(
    const WorldProgress &before, const std::vector<CharacterItem> &equipped_before,
    const std::vector<EquippedDurabilityChange> &changes, const WorldProgress &fenced_progress,
    const std::vector<CharacterItem> &fenced_equipment);

} // namespace chronicles::combat
