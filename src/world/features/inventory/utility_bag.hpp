#pragma once

// Utility bag: the four quick item slots (HUD keys 1-4, EX5KeyAction_UI_UtilityBag1..4).
//
// Each slot reserves `count` units of an owned stackable for quick use; it never holds items of
// its own. WorldProgress::utility_slots is the durable state for both the private tutorial
// instance (TutorialSession) and the shared world (WorldSession).
//
// Wire contract (recovered schemas, official captures in docs/inventory-equipment.md):
// - PC2S_UtilityBagSetReq 20080: repeated UtilityBagSlot{slot, item_cms_id, count}.
// - PS2C_UtilityBagSetRes 104: {ec, UtilityBag}.
// - PS2C_WorldCharUtilityBagNoti 140: {UtilityBag}, sent once at world entry.
// UtilityBag has fixed fields item_cms_id1..4/count1..4, one pair per slot.

#include "persistence/character_state_types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace chronicles {

inline constexpr std::uint16_t kUtilityBagSetRequestOpcode = 20080;
inline constexpr std::uint16_t kUtilityBagSetResponseOpcode = 104;
inline constexpr std::uint16_t kWorldCharUtilityBagNotificationOpcode = 140;

// x5.pb.UtilityBag for the saved slots.
nlohmann::json utility_bag_json(const WorldProgress &progress);

// True when any slot names an item or reserves units. An empty bag matches the client's
// initial state, so a shared-world entry needs no snapshot for it.
bool has_utility_bag_contents(const WorldProgress &progress);

// Item CMS ids the bag accepts: client catalog Item rows with a non-empty
// FK_utilityBagFullFillCnt. tools/build_tutorial_data.py exports the same rule as the tutorial
// profile's `utility_items`. Throws std::invalid_argument for a malformed row.
std::unordered_set<int> utility_bag_eligible_items(const std::vector<nlohmann::json> &item_rows);

// Applies one UtilityBagSetReq message to `progress.utility_slots`.
//
// `eligible_items` lists the item CMS ids the catalog allows in the bag (Item rows with
// FK_utilityBagFullFillCnt). A slot naming no item must have count zero. Entries apply in order,
// so a slot named more than once takes its last entry. After the change, the slots may reserve
// at most the owned units of each stackable. Returns the error code name of a
// refusal and leaves `progress` unchanged in that case; returns nullopt when accepted.
std::optional<std::string> apply_utility_bag_request(const nlohmann::json &request,
                                                     const std::unordered_set<int> &eligible_items,
                                                     WorldProgress &progress);

// Lowers slot reservations of `item_cms_id`, in slot order, until the owned stack covers them.
// Callers use this after spending stack units, so the bag never reserves items the character no
// longer owns. Returns true when any slot changed.
bool release_unbacked_utility_units(WorldProgress &progress, int item_cms_id);

// Spends one owned unit and its first quick-slot reservation. The caller commits the result
// before publishing success. Returns false without changing progress when the stack is empty.
bool spend_utility_item(WorldProgress &progress, int item_cms_id);

} // namespace chronicles
