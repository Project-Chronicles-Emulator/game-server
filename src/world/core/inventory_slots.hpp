#pragma once

#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"

#include <cstddef>
#include <set>
#include <vector>

namespace chronicles {

// Lowest unoccupied storage slots, up to the requested count. Returning fewer slots means
// the entire grant cannot fit; callers must refuse it before committing any rewards.
inline std::vector<int> available_inventory_slots(const Character &character, std::size_t needed) {
  std::set<int> occupied;
  for (const auto &item : character.items) {
    occupied.insert(item.inventory_slot);
  }

  std::vector<int> slots;
  for (int slot = 0; slot <= kMaximumInventorySlot && slots.size() < needed; ++slot) {
    if (!occupied.contains(slot)) {
      slots.push_back(slot);
    }
  }
  return slots;
}

// Units of one stackable that the utility bag (quick item slots) references. Removing stack
// units below this count would leave a utility slot pointing at items the character lacks.
inline int reserved_utility_count(const WorldProgress &progress, int item_cms_id) {
  int reserved = 0;
  for (const auto &slot : progress.utility_slots) {
    if (slot.item_cms_id == item_cms_id) {
      reserved += slot.count;
    }
  }
  return reserved;
}

} // namespace chronicles
