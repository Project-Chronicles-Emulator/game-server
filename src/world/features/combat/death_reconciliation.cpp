#include "world/features/combat/death_reconciliation.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace chronicles::combat {

DeathReconciliationOutcome classify_fenced_death(
    const WorldProgress &before, const std::vector<CharacterItem> &equipped_before,
    const std::vector<EquippedDurabilityChange> &changes, const WorldProgress &fenced_progress,
    const std::vector<CharacterItem> &fenced_equipment) {
  if (before.dead || before.revision < 0 || fenced_progress.revision <= before.revision ||
      equipped_before.size() != changes.size() || equipped_before.size() != fenced_equipment.size()) {
    return DeathReconciliationOutcome::conflict;
  }

  auto normalized = fenced_progress;
  normalized.revision = before.revision;
  normalized.dead = false;
  if (normalized != before ||
      (fenced_progress.dead && fenced_progress.revision - before.revision < 2)) {
    return DeathReconciliationOutcome::conflict;
  }

  std::map<std::int64_t, EquippedDurabilityChange> by_item;
  std::set<int> occupied_slots;
  for (const auto &change : changes) {
    if (change.item_id <= 0 || change.cms_id <= 0 || change.expected_durability < 0 ||
        change.durability < 0 || change.durability > change.expected_durability ||
        !by_item.emplace(change.item_id, change).second ||
        !occupied_slots.insert(change.equip_slot).second) {
      return DeathReconciliationOutcome::conflict;
    }
  }

  std::set<std::int64_t> observed_ids;
  for (const auto &item : equipped_before) {
    const auto change = by_item.find(item.id);
    if (!item.equip_slot || change == by_item.end() || item.cms_id != change->second.cms_id ||
        *item.equip_slot != change->second.equip_slot ||
        item.durability != change->second.expected_durability ||
        !observed_ids.insert(item.id).second) {
      return DeathReconciliationOutcome::conflict;
    }
    const auto current = std::ranges::find_if(fenced_equipment, [&](const CharacterItem &candidate) {
      return candidate.id == item.id;
    });
    auto expected = item;
    if (fenced_progress.dead) {
      expected.durability = change->second.durability;
    }
    if (current == fenced_equipment.end() || *current != expected) {
      return DeathReconciliationOutcome::conflict;
    }
  }

  return fenced_progress.dead ? DeathReconciliationOutcome::committed
                              : DeathReconciliationOutcome::not_committed;
}

} // namespace chronicles::combat
