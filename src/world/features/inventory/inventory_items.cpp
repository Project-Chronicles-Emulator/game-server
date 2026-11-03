#include "world/features/inventory/inventory_items.hpp"

#include "persistence/item_wire.hpp"
#include "world/core/inventory_slots.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace chronicles {

nlohmann::json character_inventory(const Character &character) {
  nlohmann::json items = nlohmann::json::array();
  for (const auto &item : character.items) {
    items.push_back(item_instance_json(item));
  }
  return {{"items", std::move(items)}, {"weapon_slot", character.build.active_weapon_slot}};
}

std::optional<std::vector<int>> lowest_free_inventory_slots(const Character &character,
                                                            std::size_t needed) {
  auto free_slots = available_inventory_slots(character, needed);
  if (free_slots.size() < needed) {
    return std::nullopt;
  }
  return free_slots;
}

} // namespace chronicles
