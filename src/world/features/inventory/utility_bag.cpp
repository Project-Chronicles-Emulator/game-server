#include "world/features/inventory/utility_bag.hpp"

#include "persistence/skill_item_use.hpp"

#include <algorithm>
#include <charconv>
#include <climits>
#include <map>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr char kBadRequest[] = "BadRequest";
// The tutorial's established refusal for a reservation larger than the owned stack.
constexpr char kNotEnoughOwnedItems[] = "Interaction_ConditionsNotMatch";

// Protobuf JSON omits zero-valued fields, so an absent field reads as zero. Returns nullopt for a
// non-integer value or one outside the int32 range of the schema fields.
std::optional<int> slot_field(const Json &slot, const char *key) {
  const auto found = slot.find(key);
  if (found == slot.end()) {
    return 0;
  }

  if (found->is_number_unsigned()) {
    const auto value = found->get<std::uint64_t>();
    if (value > static_cast<std::uint64_t>(INT_MAX)) {
      return std::nullopt;
    }
    return static_cast<int>(value);
  }

  if (found->is_number_integer()) {
    const auto value = found->get<std::int64_t>();
    if (value < INT_MIN || value > INT_MAX) {
      return std::nullopt;
    }
    return static_cast<int>(value);
  }

  return std::nullopt;
}

bool reservations_are_owned(const WorldProgress &progress,
                            const std::array<UtilitySlot, 4> &slots) {
  std::map<int, std::int64_t> reserved;
  for (const auto &slot : slots) {
    reserved[slot.item_cms_id] += slot.count;
  }

  for (const auto &[item_cms_id, count] : reserved) {
    if (count == 0) {
      continue;
    }
    const auto owned = progress.stackables.find(item_cms_id);
    if (owned == progress.stackables.end() || count > owned->second) {
      return false;
    }
  }
  return true;
}

} // namespace

nlohmann::json utility_bag_json(const WorldProgress &progress) {
  Json bag = Json::object();
  for (std::size_t index = 0; index < progress.utility_slots.size(); ++index) {
    const auto suffix = std::to_string(index + 1);
    bag["item_cms_id" + suffix] = progress.utility_slots[index].item_cms_id;
    bag["count" + suffix] = progress.utility_slots[index].count;
  }
  return bag;
}

bool has_utility_bag_contents(const WorldProgress &progress) {
  return std::ranges::any_of(progress.utility_slots, [](const UtilitySlot &slot) {
    return slot.item_cms_id != 0 || slot.count != 0;
  });
}

std::unordered_set<int> utility_bag_eligible_items(const std::vector<nlohmann::json> &item_rows) {
  std::unordered_set<int> eligible;
  for (const auto &row : item_rows) {
    // Catalog columns are exported as text; an empty or absent fill-count reference means the
    // item cannot be placed in the bag.
    const auto fill_count = row.find("FK_utilityBagFullFillCnt");
    if (fill_count == row.end() || fill_count->is_null()) {
      continue;
    }
    if (!fill_count->is_string()) {
      throw std::invalid_argument("Item FK_utilityBagFullFillCnt must be catalog text");
    }
    if (fill_count->get_ref<const std::string &>().empty()) {
      continue;
    }

    const auto id = row.find("id");
    if (id == row.end() || !id->is_string()) {
      throw std::invalid_argument("Utility bag item row has no catalog id");
    }
    const auto &text = id->get_ref<const std::string &>();
    int item_cms_id{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), item_cms_id);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || item_cms_id <= 0) {
      throw std::invalid_argument("Utility bag item id is not a positive integer: " + text);
    }
    eligible.insert(item_cms_id);
  }
  return eligible;
}

std::optional<std::string> apply_utility_bag_request(const nlohmann::json &request,
                                                     const std::unordered_set<int> &eligible_items,
                                                     WorldProgress &progress) {
  // The client sends at most four entries (clear the conflicting slot, then the target slot
  // three times), which matches the number of bag slots.
  const auto requested = request.find("slots");
  if (!request.is_object() || requested == request.end() || !requested->is_array() ||
      requested->size() > progress.utility_slots.size()) {
    return kBadRequest;
  }

  // Slots not named by the request keep their current reservation. Entries apply in order, so a
  // later entry for the same slot replaces an earlier one: when the item (or its utilityBagGroup)
  // is already in the bag, the client sender AAC_UtilityBag_SetReq_SendSlotAssignment emits the
  // target slot up to three times (cleared, its current contents, then the new assignment).
  auto slots = progress.utility_slots;
  std::array<unsigned, 4> assignments{};
  for (const auto &slot : *requested) {
    if (!slot.is_object()) {
      return kBadRequest;
    }
    const auto index = slot_field(slot, "slot");
    const auto item_cms_id = slot_field(slot, "item_cms_id");
    const auto count = slot_field(slot, "count");
    if (!index || !item_cms_id || !count) {
      return kBadRequest;
    }

    const bool index_valid = *index >= 0 && *index < static_cast<int>(slots.size());
    // An empty slot carries no item and no count; an assigned slot names a bag-eligible item.
    const bool content_valid =
        *item_cms_id == 0 ? *count == 0 : *item_cms_id > 0 && eligible_items.contains(*item_cms_id);
    if (!index_valid || *count < 0 || !content_valid) {
      return kBadRequest;
    }
    ++assignments.at(*index);
    slots.at(*index) = {*item_cms_id, *count};
  }

  // Re-equipping an existing quick-slot item asks for its full capacity even when the
  // inventory holds fewer units. Refill only from unreserved owned items.
  for (std::size_t index = 0; index < slots.size(); ++index) {
    auto &slot = slots[index];
    if (assignments[index] < 2 || slot.item_cms_id <= 0 ||
        progress.utility_slots[index].item_cms_id != slot.item_cms_id) {
      continue;
    }
    const auto owned = progress.stackables.find(slot.item_cms_id);
    const int owned_count = owned == progress.stackables.end() ? 0 : std::max(0, owned->second);
    std::int64_t reserved_elsewhere = 0;
    for (std::size_t other = 0; other < slots.size(); ++other) {
      if (other != index && slots[other].item_cms_id == slot.item_cms_id) {
        reserved_elsewhere += slots[other].count;
      }
    }
    const auto available = std::max<std::int64_t>(0, owned_count - reserved_elsewhere);
    slot.count = static_cast<int>(std::min<std::int64_t>(slot.count, available));
  }

  if (!reservations_are_owned(progress, slots)) {
    return kNotEnoughOwnedItems;
  }

  progress.utility_slots = slots;
  return std::nullopt;
}

bool spend_utility_item(WorldProgress &progress, int item_cms_id) {
  return spend_skill_item(progress, item_cms_id);
}

bool release_unbacked_utility_units(WorldProgress &progress, int item_cms_id) {
  if (item_cms_id <= 0) {
    return false;
  }

  const auto owned = progress.stackables.find(item_cms_id);
  int available = owned == progress.stackables.end() ? 0 : std::max(0, owned->second);
  bool changed = false;
  for (auto &slot : progress.utility_slots) {
    if (slot.item_cms_id != item_cms_id) {
      continue;
    }
    const int kept = std::clamp(slot.count, 0, available);
    if (kept != slot.count) {
      slot.count = kept;
      changed = true;
    }
    available -= kept;
  }
  return changed;
}

} // namespace chronicles
