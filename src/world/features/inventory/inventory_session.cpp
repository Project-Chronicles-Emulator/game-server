#include "world/features/inventory/inventory_feature.hpp"

#include "cms.generated.pb.h"
#include "common/clock_ticks.hpp"
#include "data/client_catalog.hpp"
#include "data/starter_gear.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/inventory/utility_bag.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace chronicles::inventory {
namespace {

using world_request::kDiscardRequest;
using world_request::kEquipRequest;
using world_request::kPolishRequest;

// Native-world packet constants and the captured vital-stat list.
constexpr std::uint16_t kCommonErrorResponse = 2;
constexpr std::uint16_t kItemsUpdateNotification = 169;
constexpr std::uint16_t kEquipmentsUpdateNotification = 168;
constexpr std::uint16_t kChangeStatNotification = 47;
constexpr std::uint16_t kTimeSyncRequest = 20001;
constexpr std::array<int, 7> kPolishVitalStats{68, 69, 70, 71, 72, 73, 80};

} // namespace

InventorySession::InventorySession(InventoryFeature &feature, ProgressAccess &progress,
                                   InventorySessionContext &context)
    : feature_(feature), progress_(progress), context_(context) {
}

void InventorySession::on_connection_open() {
  if (feature_.encumbrance_rules_) {
    encumbrance_.emplace(feature_.encumbrance_rules_);
  }
}

std::int64_t InventorySession::stack_quantity(int item_cms_id) {
  const auto progress = progress_.load();
  const auto stack = progress.stackables.find(item_cms_id);
  return stack == progress.stackables.end() ? 0 : stack->second;
}

SkillItemUsePlan InventorySession::prepare_skill_item_use(const std::string &nonce,
                                                          int item_cms_id) {
  return chronicles::prepare_skill_item_use(nonce, item_cms_id, progress_.load());
}

std::vector<GameplayPacket> InventorySession::consume_skill_item(int item_cms_id) {
  const auto before = progress_.load();
  auto after = before;
  if (!spend_utility_item(after, item_cms_id)) {
    throw std::runtime_error("Accepted item skill lost its inventory item");
  }
  if (before.revision == std::numeric_limits<std::int64_t>::max()) {
    throw std::runtime_error("Item skill inventory revision exhausted");
  }

  const auto remaining = after.stackables.at(item_cms_id);
  ++after.revision;
  if (!context_.inventory_save_progress(before, after)) {
    throw std::runtime_error("Item skill inventory commit failed");
  }
  context_.inventory_publish_world_loot_progress(after);

  return {{kItemsUpdateNotification,
           {{"stackables", {{{"item_cms_id", item_cms_id}, {"quantity", remaining}}}},
            {"utility_bag", nlohmann::json::object()}}}};
}

void InventorySession::on_inventory_stats_changed(const Character &character) {
  if (context_.inventory_has_combat() && context_.inventory_has_character_stats()) {
    context_.inventory_update_combat_stats(session_stats(character));
  }
}

void InventorySession::on_inventory_snapshot_changed(const Character &character,
                                                     const nlohmann::json *prepared_stats) {
  context_.inventory_refresh_snapshot(character, prepared_stats);
}

GameplayPacket InventorySession::equip_error(const WorldGameplayRequest &request,
                                             x5::ec::EErrorCode code) {
  return {kCommonErrorResponse,
          {{"ec", x5::ec::EErrorCode_Name(code)},
           {"packet_seq", {{"seq", std::to_string(request.sequence)}}}}};
}

std::optional<int> InventorySession::parse_equip_slot(const nlohmann::json &raw_slot) {
  int slot{};
  if (raw_slot.is_number_integer()) {
    if (raw_slot < 0 || raw_slot >= kEquipSlotCount) {
      return std::nullopt;
    }
    slot = raw_slot.get<int>();
  } else if (raw_slot.is_string()) {
    x5::cms::EX5EquipSlotType parsed;
    if (!x5::cms::EX5EquipSlotType_Parse(raw_slot.get<std::string>(), &parsed)) {
      return std::nullopt;
    }
    slot = static_cast<int>(parsed);
  } else {
    return std::nullopt;
  }
  if (slot < 0 || slot >= kEquipSlotCount) {
    return std::nullopt;
  }
  return slot;
}

// Observed bare unequip: ItemEquipReq carries only `slot`; the client builder
// writes item_id = 0, which proto3 omits (a literal 0 is accepted as well).
bool InventorySession::is_bare_unequip_request(const nlohmann::json &message) {
  if (!message.is_object() || !message.contains("slot")) {
    return false;
  }
  if (!message.contains("item_id")) {
    return true;
  }
  const auto &raw_id = message.at("item_id");
  if (raw_id.is_number_integer()) {
    return raw_id.get<std::int64_t>() == 0;
  }
  return raw_id.is_string() && raw_id.get<std::string>() == "0";
}

// Server handling of bare unequip is observed only for the 169/168 replies.
// Unobserved cases get no invented error: an empty slot is a silent no-op and
// the Bag slot (client-blocked, server rule unverified) is treated like any slot.
std::vector<GameplayPacket> InventorySession::unequip_slot(const WorldGameplayRequest &request,
                                                           const Character &character) {
  const auto slot = parse_equip_slot(request.message.at("slot"));
  if (!slot) {
    return {equip_error(request, x5::ec::ItemEquipInvalidSlot)};
  }

  VisualSlotClear visual_clear;
  const auto update = InventoryRules::bare_unequip(character.items, *slot, visual_clear);
  if (!update.vacated) {
    return {};
  }

  const auto vacated = std::ranges::find_if(character.items, [&](const auto &item) {
    return item.id == update.item_id;
  });
  auto cleared = *vacated;
  cleared.equip_slot.reset();
  return commit_equipment_change(request, character, {cleared}, *slot);
}

std::vector<GameplayPacket> InventorySession::equip_items(const WorldGameplayRequest &request) {
  if (!context_.inventory_has_handoff()) {
    return {equip_error(request, x5::ec::BadRequest)};
  }

  const auto character = progress_.character();
  if (!character) {
    return {equip_error(request, x5::ec::ItemIdInvalid)};
  }

  const auto &message = request.message;
  if (is_bare_unequip_request(message)) {
    return unequip_slot(request, *character);
  }

  if (!message.is_object() || !message.contains("item_id")) {
    return {equip_error(request, x5::ec::ItemIdInvalid)};
  }

  std::int64_t item_id{};
  try {
    const auto &raw_id = message.at("item_id");
    item_id =
        raw_id.is_string() ? std::stoll(raw_id.get<std::string>()) : raw_id.get<std::int64_t>();
  } catch (const std::exception &) {
    return {equip_error(request, x5::ec::ItemIdInvalid)};
  }

  // A weapon equip omits the slot; every other observed request sends it.
  int requested_slot = x5::cms::EX5EquipSlotType_Weapon_1;
  if (message.contains("slot")) {
    const auto parsed_slot = parse_equip_slot(message.at("slot"));
    if (!parsed_slot) {
      return {equip_error(request, x5::ec::ItemEquipInvalidSlot)};
    }
    requested_slot = *parsed_slot;
  }

  const auto target = std::ranges::find_if(character->items, [&](const auto &item) {
    return item.id == item_id;
  });
  if (target == character->items.end()) {
    return {equip_error(request, x5::ec::ItemIdInvalid)};
  }
  if (target->equip_slot == requested_slot) {
    return {equip_error(request, x5::ec::ItemEquipped)};
  }
  if (target->durability <= 0) {
    return {equip_error(request, x5::ec::ItemEquipBroken)};
  }

  if (!feature_.catalog_) {
    return {equip_error(request, x5::ec::ItemError)};
  }

  const auto item_row = feature_.catalog_->find_row("Item", "id", std::to_string(target->cms_id));
  if (!item_row) {
    return {equip_error(request, x5::ec::ItemCmsIdInvalid)};
  }

  const std::string equip_slot_name = item_row->value("equipSlot", std::string());
  x5::cms::EX5EquipSlotType catalog_slot;
  if (equip_slot_name.empty() ||
      !x5::cms::EX5EquipSlotType_Parse("EX5EquipSlotType_" + equip_slot_name, &catalog_slot)) {
    return {equip_error(request, x5::ec::ItemEquipInvalidItemType)};
  }
  const bool weapon_item = catalog_slot == x5::cms::EX5EquipSlotType_Weapon_1 ||
                           catalog_slot == x5::cms::EX5EquipSlotType_Weapon_2;
  const bool weapon_destination = requested_slot == x5::cms::EX5EquipSlotType_Weapon_1 ||
                                  requested_slot == x5::cms::EX5EquipSlotType_Weapon_2;
  // The catalog names the default weapon slot; both character weapon slots accept it.
  if (static_cast<int>(catalog_slot) != requested_slot && !(weapon_item && weapon_destination)) {
    return {equip_error(request, x5::ec::ItemEquipInvalidSlot)};
  }

  const std::string item_class = item_row->value("class", std::string());
  if (!item_class.empty() && item_class != "None") {
    x5::cms::EX5CharacterClass required_class;
    if (!x5::cms::EX5CharacterClass_Parse("EX5CharacterClass_" + item_class, &required_class) ||
        static_cast<int>(required_class) != character->class_id) {
      return {equip_error(request, x5::ec::ItemClassMismatch)};
    }
  }

  const std::string level_requirement = item_row->value("levelReq", std::string("0"));
  if (!level_requirement.empty() && character->level < std::stoi(level_requirement)) {
    return {equip_error(request, x5::ec::ItemLevelReqNotMet)};
  }

  std::vector<CharacterItem> mutations;
  auto updated = *target;
  updated.equip_slot = requested_slot;

  auto vacated = std::ranges::find_if(character->items, [&](const auto &item) {
    return item.equip_slot == requested_slot && item.id != item_id;
  });
  if (vacated != character->items.end()) {
    auto cleared = *vacated;
    cleared.equip_slot.reset();
    mutations.push_back(cleared);
  }
  mutations.push_back(updated);

  return commit_equipment_change(request, *character, mutations, target->equip_slot);
}

std::vector<GameplayPacket> InventorySession::activate_weapon(const WorldGameplayRequest &request) {
  if (!context_.inventory_has_handoff()) {
    return {equip_error(request, x5::ec::BadRequest)};
  }
  const auto character = progress_.character();
  if (!character) {
    return {equip_error(request, x5::ec::BadRequest)};
  }
  return select_weapon_slot(*character, request.message.value("slot", nlohmann::json(0)), true);
}

std::vector<GameplayPacket> InventorySession::select_weapon_for_skill(const nlohmann::json &slot) {
  if (!context_.inventory_has_handoff()) {
    return {};
  }
  const auto character = progress_.character();
  if (!character) {
    return {};
  }
  // A SwapWeapon skill is itself the player's action: the combat start that follows decides
  // whether it may cancel the current one, so an open combat action does not refuse here.
  return select_weapon_slot(*character, slot, false);
}

std::vector<GameplayPacket> InventorySession::select_weapon_slot(const Character &character,
                                                                 const nlohmann::json &raw_slot,
                                                                 bool refuse_when_busy) {
  const auto before = context_.inventory_has_tutorial() ? *context_.inventory_tutorial_progress()
                                                        : progress_.load();
  const auto reply = [&](x5::ec::EErrorCode error, int slot) {
    // The client applies slot even on failure, so refusals must name the current slot.
    return GameplayPacket{
        172,
        {{"ec", x5::ec::EErrorCode_Name(error)},
         {"unit_id", std::to_string(context_.inventory_player_unit_id(character.id))},
         {"slot", slot}}};
  };
  const auto refuse = [&](x5::ec::EErrorCode error) {
    return std::vector<GameplayPacket>{reply(error, before.build.active_weapon_slot)};
  };
  const auto slot = parse_equip_slot(raw_slot);
  if (!slot || *slot > 1) {
    return refuse(x5::ec::Weapon_InvalidSlot);
  }
  const auto weapon = std::ranges::find_if(
      character.items, [&](const CharacterItem &item) { return item.equip_slot == *slot; });
  if (weapon == character.items.end() || weapon->durability <= 0) {
    return refuse(x5::ec::Skill_NotEquipWeaponSlot);
  }
  if (before.dead) {
    return refuse(x5::ec::Skill_Dead);
  }
  if (refuse_when_busy && context_.inventory_action_busy()) {
    return refuse(x5::ec::ItemError);
  }
  if (*slot == before.build.active_weapon_slot) {
    return {reply(x5::ec::Success, *slot)};
  }
  if (before.revision == std::numeric_limits<std::int64_t>::max()) {
    return refuse(x5::ec::ItemError);
  }

  auto after = before;
  after.build.active_weapon_slot = *slot;
  ++after.revision;
  auto selected = character;
  selected.build = after.build;
  std::optional<nlohmann::json> previous_stats;
  std::optional<nlohmann::json> next_stats;
  if (context_.inventory_has_character_stats()) {
    try {
      previous_stats = session_stats(character);
      next_stats = session_stats(selected);
      if (context_.inventory_has_combat() &&
          (next_stats->value("18", 0) <= 0 || next_stats->value("19", 0) <= 0)) {
        return refuse(x5::ec::ItemError);
      }
    } catch (const std::exception &error) {
      spdlog::warn("Cannot calculate weapon switch stats: {}", error.what());
      return refuse(x5::ec::ItemError);
    }
  }
  if (!context_.inventory_save_progress(before, after)) {
    return refuse(x5::ec::ItemError);
  }
  context_.inventory_publish_weapon_selection(after);

  std::vector<GameplayPacket> packets{reply(x5::ec::Success, *slot)};
  if (next_stats && context_.inventory_has_combat()) {
    const auto &current = context_.inventory_update_combat_stats(*next_stats);
    context_.inventory_publish_snapshot(selected, &current);
    packets.push_back(equipment_stat_notification(context_.inventory_player_unit_id(selected.id),
                                                  *previous_stats, *next_stats, current));
  } else {
    context_.inventory_publish_snapshot(selected, next_stats ? &*next_stats : nullptr);
  }
  return packets;
}

// Single commit-and-notify path for equip swaps and bare unequip: persists the
// mutations under the current revision, refreshes stats, then reports the item
// changes (169) and the visual change (168) back to this client.
std::vector<GameplayPacket> InventorySession::commit_equipment_change(
    const WorldGameplayRequest &request, const Character &character,
    const std::vector<CharacterItem> &mutations, std::optional<int> cleared_visual_slot) {
  Character live = character;
  for (auto &item : live.items) {
    for (const auto &mutation : mutations) {
      if (item.id == mutation.id) {
        item = mutation;
      }
    }
  }
  std::optional<nlohmann::json> prepared_stats;
  if (context_.inventory_has_character_stats()) {
    try {
      prepared_stats = session_stats(live);
    } catch (const std::exception &error) {
      spdlog::warn("Cannot calculate equipment stats for character {}: {}", live.id, error.what());
      return {equip_error(request, x5::ec::ItemError)};
    }
  }
  if (context_.inventory_has_combat() && prepared_stats &&
      (prepared_stats->value("18", 0) <= 0 || prepared_stats->value("19", 0) <= 0)) {
    return {equip_error(request, x5::ec::ItemError)};
  }

  const auto expected_revision = context_.inventory_has_tutorial()
                                     ? context_.inventory_tutorial_progress()->revision
                                     : progress_.load().revision;
  if (!context_.inventory_update_items(expected_revision, mutations)) {
    return {equip_error(request, x5::ec::ItemError)};
  }
  if (context_.inventory_has_tutorial()) {
    context_.inventory_publish_equipment_revision(expected_revision + 1);
  }

  if (context_.inventory_has_combat() && prepared_stats) {
    const auto &live_stats = context_.inventory_update_combat_stats(std::move(*prepared_stats));
    context_.inventory_publish_snapshot(live, &live_stats);
  } else {
    context_.inventory_publish_snapshot(live, prepared_stats ? &*prepared_stats : nullptr);
  }

  nlohmann::json changed = nlohmann::json::array();
  nlohmann::json visual = nlohmann::json::array();
  for (const auto &mutation : mutations) {
    nlohmann::json entry{{"item_id", std::to_string(mutation.id)},
                         {"item_cms_id", mutation.cms_id},
                         {"durability", mutation.durability},
                         {"grade", mutation.grade},
                         {"polish_step", mutation.polish_step}};
    if (mutation.equip_slot) {
      entry["equip_slot"] = *mutation.equip_slot;
      visual.push_back({{"item_cms_id", mutation.cms_id},
                        {"grade", mutation.grade},
                        {"slot", *mutation.equip_slot}});
    }
    // A vacated item intentionally carries no equip_slot: absent means
    // unequipped on this packet.
    changed.push_back(std::move(entry));
  }
  if (cleared_visual_slot) {
    // Observed bare unequip: the visual broadcast names the slot and carries
    // no item_cms_id, which clears it for observers.
    visual.push_back({{"slot", *cleared_visual_slot}});
  }

  return {{kItemsUpdateNotification,
           {{"items", std::move(changed)}, {"equip", nlohmann::json::object()}}},
          {kEquipmentsUpdateNotification,
           {{"unit_id", std::to_string(context_.inventory_player_unit_id(live.id))},
            {"equipments", std::move(visual)}}}};
}

std::vector<GameplayPacket> InventorySession::discard_items(const WorldGameplayRequest &request) {
  // Schema-only/Candidate: no discard request or deletion notification was
  // captured. Reject unsupported voucher identities rather than guess refunds.
  if (!context_.inventory_has_handoff() || !feature_.catalog_ ||
      context_.inventory_has_tutorial()) {
    return {equip_error(request, x5::ec::BadRequest)};
  }

  const auto character = progress_.character();
  if (!character) {
    return {equip_error(request, x5::ec::ItemIdInvalid)};
  }

  const auto &message = request.message;
  if (!message.is_object() || !message.contains("identity_list") ||
      !message.at("identity_list").is_array() || message.at("identity_list").empty() ||
      message.at("identity_list").size() > 64) {
    return {equip_error(request, x5::ec::BadRequest)};
  }

  const auto progress = progress_.load();
  std::vector<std::int64_t> item_ids;
  std::map<int, int> stackable_counts;
  nlohmann::json removed_items = nlohmann::json::array();
  nlohmann::json updated_stacks = nlohmann::json::array();
  for (const auto &identity : message.at("identity_list")) {
    if (!identity.is_object() || identity.contains("trade_voucher_id") ||
        identity.contains("item_id") == identity.contains("stackable")) {
      return {equip_error(request, x5::ec::BadRequest)};
    }

    int cms_id{};
    if (identity.contains("item_id")) {
      std::int64_t item_id{};
      try {
        item_id = std::stoll(identity.at("item_id").get<std::string>());
      } catch (const std::exception &) {
        return {equip_error(request, x5::ec::ItemIdInvalid)};
      }
      const auto found = std::ranges::find_if(character->items, [item_id](const auto &item) {
        return item.id == item_id;
      });
      if (found == character->items.end() ||
          std::ranges::find(item_ids, item_id) != item_ids.end()) {
        return {equip_error(request, x5::ec::ItemIdInvalid)};
      }
      if (found->equip_slot) {
        return {equip_error(request, x5::ec::ItemEquipped)};
      }
      cms_id = found->cms_id;
      item_ids.push_back(item_id);
      removed_items.push_back({{"item_id", std::to_string(item_id)}});
    } else {
      const auto &stack = identity.at("stackable");
      if (!stack.is_object() || !stack.contains("item_cms_id") || !stack.contains("count") ||
          !stack.at("item_cms_id").is_number_integer() || !stack.at("count").is_number_integer()) {
        return {equip_error(request, x5::ec::ItemCountError)};
      }
      cms_id = stack.at("item_cms_id").get<int>();
      const int count = stack.at("count").get<int>();
      const auto owned = progress.stackables.find(cms_id);
      if (cms_id <= 0 || count <= 0 || owned == progress.stackables.end() ||
          count > owned->second || !stackable_counts.emplace(cms_id, count).second) {
        return {equip_error(request, x5::ec::ItemCountError)};
      }
      updated_stacks.push_back({{"item_cms_id", cms_id}, {"quantity", owned->second - count}});
    }

    const auto catalog_item = feature_.catalog_->find_row("Item", "id", std::to_string(cms_id));
    if (!catalog_item || catalog_item->value("discardable", std::string()) != "True") {
      return {equip_error(request, x5::ec::ItemNonDiscardable)};
    }
  }

  if (!context_.inventory_discard_items(progress.revision, item_ids, stackable_counts)) {
    return {equip_error(request, x5::ec::ItemError)};
  }

  return {{kItemsUpdateNotification,
           {{"items", std::move(removed_items)},
            {"stackables", std::move(updated_stacks)},
            {"discard", nlohmann::json::object()}}}};
}

// Catalog stats of `character` with the applied carried-weight tier on top. Session stat
// refreshes (equipment, trades, tempering) go through here, so they keep the slowdown.
nlohmann::json InventorySession::session_stats(const Character &character) const {
  auto stats = context_.inventory_character_stats(character);
  if (encumbrance_) {
    encumbrance_->apply_to(stats);
  }
  return stats;
}

bool InventorySession::changes_inventory(const std::vector<GameplayPacket> &packets) {
  return std::ranges::any_of(packets, [](const GameplayPacket &packet) {
    return packet.opcode == kItemsUpdateNotification;
  });
}

// Re-measures the carried weight after a packet batch that changed the inventory, or when the
// character enters the world, and appends the packets of a tier change. The weight is read
// from committed storage, so every inventory path (loot, shop, quest, gathering, crafting,
// equip, discard, GM) is covered by the ItemsUpdateNoti it already sends. A weight the catalog
// cannot measure leaves the applied tier unchanged.
void InventorySession::append_encumbrance_changes(std::vector<GameplayPacket> &packets,
                                                  bool entering_world) {
  if (!encumbrance_ || !context_.inventory_has_handoff() || !feature_.capacity_) {
    return;
  }
  if (!entering_world && !changes_inventory(packets)) {
    return;
  }

  const auto character = progress_.character();
  if (!character) {
    return;
  }
  const auto progress = context_.inventory_has_tutorial() ? *context_.inventory_tutorial_progress()
                                                          : progress_.load();

  CarriedWeight carried;
  EncumbranceLevel next{};
  try {
    carried = feature_.capacity_->measure(*character, progress);
    next = encumbrance_->rules().classify(carried.total, carried.safe);
  } catch (const std::exception &error) {
    spdlog::warn("Carried weight of character {} not measured: {}", character->id, error.what());
    return;
  }
  if (next == encumbrance_->level()) {
    return;
  }

  // The next tier's stats are computed before the tier changes, so a stat model failure
  // leaves both the session and the client on the previous tier.
  std::optional<nlohmann::json> next_stats;
  if (context_.inventory_has_character_stats()) {
    try {
      next_stats = context_.inventory_character_stats(*character);
    } catch (const std::exception &error) {
      spdlog::warn("Cannot calculate encumbered stats for character {}: {}", character->id,
                   error.what());
      return;
    }
    encumbrance_->rules().apply_modifiers(next, *next_stats);
    if (context_.inventory_has_combat() &&
        (next_stats->value("18", 0) <= 0 || next_stats->value("19", 0) <= 0)) {
      return;
    }
  }

  const auto transition = encumbrance_->change_level(next, context_.inventory_monotonic_ticks());
  if (!transition) {
    return;
  }

  nlohmann::json current_stats = nlohmann::json::object();
  if (next_stats) {
    if (context_.inventory_has_combat()) {
      // Combat keeps its own timed statuses on top of these stats.
      current_stats = context_.inventory_update_combat_stats(std::move(*next_stats));
    } else {
      current_stats = std::move(*next_stats);
    }
    context_.inventory_publish_snapshot(*character, &current_stats);
  }

  auto announcements = encumbrance_transition_packets(
      *transition, context_.inventory_player_unit_id(character->id), current_stats);
  packets.insert(packets.end(), std::make_move_iterator(announcements.begin()),
                 std::make_move_iterator(announcements.end()));
  spdlog::info("Character {} carries {} of safe weight {}: encumbrance {} -> {}", character->id,
               carried.total, carried.safe, encumbrance_level_name(transition->previous),
               encumbrance_level_name(transition->current));
}

// Shared-world utility bag (quick item slots 1-4). Inside the private tutorial instance the
// TutorialSession owns the same request; both persist WorldProgress::utility_slots.
bool InventorySession::utility_bag_request(const WorldGameplayRequest *request) const {
  return request && !context_.inventory_has_tutorial() && context_.inventory_has_handoff() &&
         request->opcode == kUtilityBagSetRequestOpcode;
}

// Validates the request against the catalog's bag-eligible items and the owned stacks, commits
// the slots through the progress revision, then answers UtilityBagSetRes with the saved bag.
// Refusals answer CommonErrorRes with the tutorial's codes and change nothing.
std::vector<GameplayPacket>
InventorySession::receive_utility_bag(const WorldGameplayRequest &request) {
  const auto refuse = [&request](const std::string &code) {
    return std::vector<GameplayPacket>{
        {kCommonErrorResponse,
         {{"ec", code}, {"packet_seq", {{"seq", std::to_string(request.sequence)}}}}}};
  };

  if (context_.inventory_player_in_combat()) {
    return refuse("UtilityBag_InCombat");
  }

  // The eligible-item list comes from the client catalog's Item table. Without it no slot can
  // be validated, so nothing is stored.
  if (!feature_.utility_items_) {
    spdlog::warn("Utility bag request refused connection={}: no item eligibility data",
                 context_.inventory_connection_id());
    return refuse("BadRequest");
  }

  const auto before = progress_.load();
  auto after = before;
  const auto refusal = apply_utility_bag_request(request.message, *feature_.utility_items_, after);
  if (refusal) {
    return refuse(*refusal);
  }

  if (after != before) {
    if (after.revision == std::numeric_limits<std::int64_t>::max()) {
      throw std::runtime_error("World progress revision exhausted");
    }
    ++after.revision;
    if (!context_.inventory_save_progress(before, after)) {
      // Nothing was committed, so the client's previous bag stays authoritative.
      spdlog::warn("Utility bag commit conflict connection={}", context_.inventory_connection_id());
      return refuse("BadRequest");
    }
    context_.inventory_publish_world_loot_progress(after);
  }

  return {{kUtilityBagSetResponseOpcode, {{"bag", utility_bag_json(after)}}}};
}

std::optional<InventorySession::PolishRequest>
InventorySession::parse_polish_request(const nlohmann::json &message) {
  if (!message.is_object() || !message.contains("item_id")) {
    return std::nullopt;
  }

  PolishRequest request;
  try {
    const auto &raw_id = message.at("item_id");
    request.item_id =
        raw_id.is_string() ? std::stoll(raw_id.get<std::string>()) : raw_id.get<std::int64_t>();
    request.additive_cms_id = message.value("add_material_item_cms_id", 0);
  } catch (const std::exception &) {
    return std::nullopt;
  }

  if (request.item_id <= 0 || request.additive_cms_id < 0) {
    return std::nullopt;
  }
  return request;
}

// Candidate refusal codes: no tempering refusal was captured. The ItemPolish_* codes and
// their texts exist in the client (TXT_ERRORCODE_0000001033..1038); shortage codes reuse the
// generic item count and money codes.
x5::ec::EErrorCode InventorySession::polish_plan_error(PolishPlanStatus status) {
  switch (status) {
  case PolishPlanStatus::NotEnoughMaterial:
  case PolishPlanStatus::NotEnoughAdditive:
    return x5::ec::ItemCountError;
  case PolishPlanStatus::NotEnoughGold:
    return x5::ec::MoneyNotEnough;
  case PolishPlanStatus::Planned:
  case PolishPlanStatus::InvalidRequirement:
  case PolishPlanStatus::StepOutOfRange:
    break;
  }
  return x5::ec::ItemPolish_Invalid;
}

// x5.pb.ItemRollResult enum names for the protobuf JSON codec.
const char *InventorySession::polish_outcome_name(PolishOutcome outcome) {
  switch (outcome) {
  case PolishOutcome::fail:
    return "ItemRollResult_Fail";
  case PolishOutcome::destruction:
    return "ItemRollResult_Destruction";
  case PolishOutcome::success:
    break;
  }
  return "ItemRollResult_Success";
}

// Uniform roll in [0, 10000) on this session's own engine.
int InventorySession::roll_polish() {
  std::uniform_int_distribution<int> roll(0, kPolishProbabilityScale - 1);
  return roll(polish_random_);
}

// ChangeStatNoti after an equipped tempering success: every stat whose value changed plus the
// vitals present in the current stats (Observed order of captured entries: ascending stat).
GameplayPacket InventorySession::equipment_stat_notification(std::int64_t unit_id,
                                                             const nlohmann::json &before,
                                                             const nlohmann::json &after,
                                                             const nlohmann::json &current) const {
  std::map<int, int> results;
  for (const auto &[key, value] : after.items()) {
    if (!value.is_number_integer() || before.value(key, 0) == value.get<int>()) {
      continue;
    }
    int stat{};
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), stat);
    if (parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size()) {
      results[stat] = value.get<int>();
    }
  }
  for (const int stat : kPolishVitalStats) {
    const auto key = std::to_string(stat);
    if (current.contains(key)) {
      results[stat] = current.at(key).get<int>();
    }
  }

  nlohmann::json changes = nlohmann::json::array();
  for (const auto &[stat, result] : results) {
    changes.push_back({{"stat_type", stat}, {"result", result}});
  }
  return {kChangeStatNotification,
          {{"unit_id", std::to_string(unit_id)},
           {"tick", wire_monotonic_tick(context_.inventory_monotonic_ticks())},
           {"default_source", "SCS_Equip"},
           {"stat_changes", std::move(changes)}}};
}

// One ItemsUpdateNoti for a committed attempt. Success and fail carry the whole item (fail
// keeps its step); destruction carries only item_id, which the client treats as a removal
// (AAC_Inventory_ApplyItemUpdate). Success keeps the observed empty polish marker; other
// results name the roll so the client's result popup can show it.
GameplayPacket InventorySession::polish_items_update(const CharacterItem &item,
                                                     const PolishPlan &plan,
                                                     PolishOutcome outcome) {
  nlohmann::json entry{{"item_id", std::to_string(item.id)}};
  if (outcome != PolishOutcome::destruction) {
    entry["item_cms_id"] = item.cms_id;
    entry["durability"] = item.durability;
    entry["grade"] = item.grade;
    entry["polish_step"] =
        outcome == PolishOutcome::success ? plan.new_polish_step : item.polish_step;
    if (item.equip_slot) {
      entry["equip_slot"] = *item.equip_slot;
    }
  }

  nlohmann::json stackables = nlohmann::json::array();
  stackables.push_back(
      {{"item_cms_id", plan.material_cms_id}, {"quantity", plan.material_remaining}});
  if (plan.additive_cms_id != 0) {
    stackables.push_back(
        {{"item_cms_id", plan.additive_cms_id}, {"quantity", plan.additive_remaining}});
  }

  nlohmann::json reason = nlohmann::json::object();
  if (outcome != PolishOutcome::success) {
    reason["result"] = polish_outcome_name(outcome);
  }

  return {kItemsUpdateNotification,
          {{"items", nlohmann::json::array({std::move(entry)})},
           {"money_changes",
            nlohmann::json::array({{{"change", std::to_string(-plan.gold_cost)},
                                    {"result", std::to_string(plan.gold_remaining)}}})},
           {"stackables", std::move(stackables)},
           {"polish", std::move(reason)}}};
}

// Item tempering (PC2S_ItemPolishReq, 20075). Observed: an accepted request is answered by one
// ItemsUpdateNoti (169) with the item, money_changes, the remaining material and the polish
// reason; an equipped success adds ChangeStatNoti (47) and EquipmentsUpdateNoti (168). The
// server rolls the catalog chances itself: success raises the step, fail keeps it and
// destruction deletes the (necessarily unequipped) item; every outcome charges the material,
// the optional additive and gold in one revisioned commit. Every refusal happens before any
// mutation or roll. See docs/inventory-polish-evidence.md.
std::vector<GameplayPacket> InventorySession::polish_item(const WorldGameplayRequest &request) {
  const auto refuse = [this, &request](x5::ec::EErrorCode code) {
    return std::vector<GameplayPacket>{equip_error(request, code)};
  };
  if (!context_.inventory_has_handoff() || !feature_.polish_catalog_ ||
      context_.inventory_has_tutorial()) {
    return refuse(x5::ec::BadRequest);
  }

  const auto parsed = parse_polish_request(request.message);
  const auto character = progress_.character();
  if (!parsed || !character) {
    return refuse(x5::ec::BadRequest);
  }
  const auto owned = std::ranges::find_if(character->items, [&](const auto &item) {
    return item.id == parsed->item_id;
  });
  if (owned == character->items.end()) {
    return refuse(x5::ec::ItemIdInvalid);
  }

  // Static eligibility, in the client's error-code order.
  const auto &catalog = *feature_.polish_catalog_;
  const auto *polishable = catalog.item(owned->cms_id);
  if (polishable == nullptr) {
    return refuse(x5::ec::ItemPolish_Invalid);
  }
  if (owned->durability <= 0) {
    return refuse(x5::ec::ItemPolish_Damaged);
  }
  if (owned->polish_step >= polishable->max_step) {
    return refuse(x5::ec::ItemPolish_StepIsMax);
  }
  const auto *requirement = catalog.requirement(*polishable, owned->polish_step + 1);
  if (requirement == nullptr) {
    return refuse(x5::ec::ItemPolish_Invalid);
  }
  const PolishAdditive *additive = nullptr;
  if (parsed->additive_cms_id != 0) {
    additive = catalog.additive(parsed->additive_cms_id);
    if (additive == nullptr) {
      return refuse(x5::ec::ItemPolish_InvalidAdditiveMaterial);
    }
  }

  const auto progress = progress_.load();
  const auto plan = InventoryRules::plan_polish(*requirement, owned->polish_step,
                                                progress.stackables, progress.gold, additive);
  if (plan.status != PolishPlanStatus::Planned) {
    return refuse(polish_plan_error(plan.status));
  }

  // A destruction would delete the item, so a possible one requires it to be unequipped
  // (Candidate use of ItemPolish_RequireUnEquip). Granting Item.polishResidueId is not
  // modelled; refuse rather than destroy without the residue.
  if (plan.chances.destruction > 0) {
    if (owned->equip_slot) {
      return refuse(x5::ec::ItemPolish_RequireUnEquip);
    }
    if (polishable->has_polish_residue) {
      spdlog::warn("Tempering of item {} could destroy it, but its polish residue grant is not "
                   "modelled",
                   owned->cms_id);
      return refuse(x5::ec::ItemPolish_Invalid);
    }
  }

  // The stats a success would produce are computed before the roll, so a step the stat model
  // cannot evaluate is refused without consuming anything or revealing a roll.
  Character polished = *character;
  for (auto &cached : polished.items) {
    if (cached.id == owned->id) {
      cached.polish_step = plan.new_polish_step;
    }
  }
  std::optional<nlohmann::json> stats_before;
  std::optional<nlohmann::json> stats_after_success;
  if (context_.inventory_has_character_stats()) {
    try {
      stats_before = session_stats(*character);
      stats_after_success = session_stats(polished);
    } catch (const std::exception &error) {
      spdlog::warn("Cannot calculate tempered equipment stats for character {}: {}", character->id,
                   error.what());
      return refuse(x5::ec::BadRequest);
    }
    if (context_.inventory_has_combat() &&
        (stats_after_success->value("18", 0) <= 0 || stats_after_success->value("19", 0) <= 0)) {
      return refuse(x5::ec::BadRequest);
    }
  }

  const auto outcome = InventoryRules::resolve_polish_roll(plan.chances, roll_polish());
  const ItemPolishChange change{
      owned->id,           owned->cms_id,  owned->polish_step,   plan.material_cms_id,
      plan.material_count, plan.gold_cost, plan.additive_cms_id, outcome};
  if (!context_.inventory_apply_polish(progress.revision, change)) {
    return refuse(x5::ec::ItemError);
  }

  std::vector<GameplayPacket> packets{polish_items_update(*owned, plan, outcome)};
  if (outcome == PolishOutcome::success) {
    append_polish_success_updates(*owned, polished, stats_before, stats_after_success, packets);
  } else if (outcome == PolishOutcome::destruction) {
    Character remaining = *character;
    std::erase_if(remaining.items, [&](const auto &item) {
      return item.id == owned->id;
    });
    context_.inventory_publish_snapshot(
        remaining, context_.inventory_has_combat() ? context_.inventory_combat_stats() : nullptr);
  }
  return packets;
}

// After a committed success: refresh the session's cached items and stats so later snapshots
// carry the new step, then add the observed equipped follow-ups. Observed in all four captured
// equipped (weapon) polishes: 169, 47 (SCS_Equip), 168; the three unequipped armor polishes
// sent only 169. Candidate: an equipped armor success also sends 47 with its changed stats.
void InventorySession::append_polish_success_updates(
    const CharacterItem &item, const Character &polished,
    const std::optional<nlohmann::json> &stats_before,
    const std::optional<nlohmann::json> &stats_after, std::vector<GameplayPacket> &packets) {
  if (context_.inventory_has_combat() && stats_after) {
    const auto &live_stats = context_.inventory_update_combat_stats(*stats_after);
    context_.inventory_publish_snapshot(polished, &live_stats);
  } else {
    context_.inventory_publish_snapshot(polished, stats_after ? &*stats_after : nullptr);
  }

  if (!item.equip_slot) {
    return;
  }
  const auto unit_id = context_.inventory_player_unit_id(polished.id);
  if (stats_before && stats_after) {
    const auto &current =
        context_.inventory_has_combat() ? *context_.inventory_combat_stats() : *stats_after;
    packets.push_back(equipment_stat_notification(unit_id, *stats_before, *stats_after, current));
  }
  packets.push_back({kEquipmentsUpdateNotification,
                     {{"unit_id", std::to_string(unit_id)},
                      {"equipments", nlohmann::json::array({{{"item_cms_id", item.cms_id},
                                                             {"grade", item.grade},
                                                             {"slot", *item.equip_slot}}})}}});
}

// Observed after every official tool gather: ChangeStatNoti (SCS_Equip) with the vitals and
// EquipmentsUpdateNoti naming the worn tool's slot. Durability does not change stats, so the
// stat list carries only the vitals, as captured.
std::vector<GameplayPacket> InventorySession::tool_wear_updates(const CharacterItem &tool) {
  std::vector<GameplayPacket> packets;
  if (!context_.inventory_has_handoff() || !tool.equip_slot) {
    return packets;
  }

  if (const auto character = progress_.character()) {
    context_.inventory_publish_snapshot(
        *character, context_.inventory_has_combat() ? context_.inventory_combat_stats() : nullptr);
  }
  const auto unit_id = context_.inventory_player_unit_id(context_.inventory_character_id());
  if (context_.inventory_has_combat()) {
    const auto &current = *context_.inventory_combat_stats();
    packets.push_back(equipment_stat_notification(unit_id, current, current, current));
  }
  packets.push_back({kEquipmentsUpdateNotification,
                     {{"unit_id", std::to_string(unit_id)},
                      {"equipments", nlohmann::json::array({{{"item_cms_id", tool.cms_id},
                                                             {"grade", tool.grade},
                                                             {"slot", *tool.equip_slot}}})}}});
  return packets;
}

bool InventorySession::handle_equip(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;

  // Item equip is a session-scoped store operation: it validates against
  // the catalog, persists both sides of a swap atomically, then reports
  // the item and visual changes back to this client.
  if (gameplay_request && gameplay_request->opcode == kEquipRequest) {
    session_packets = equip_items(*gameplay_request);
    return true;
  }
  if (gameplay_request && gameplay_request->opcode == world_request::kWeaponSlotActivationRequest) {
    session_packets = activate_weapon(*gameplay_request);
    return true;
  }

  return false;
}

bool InventorySession::handle_discard(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;

  if (gameplay_request && gameplay_request->opcode == kDiscardRequest) {
    session_packets = discard_items(*gameplay_request);
    return true;
  }

  return false;
}

bool InventorySession::handle_polish(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;

  if (gameplay_request && gameplay_request->opcode == kPolishRequest) {
    session_packets = polish_item(*gameplay_request);
    return true;
  }

  return false;
}

bool InventorySession::handle_utility_bag(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;

  if (utility_bag_request(gameplay_request)) {
    auto bag_packets = receive_utility_bag(*gameplay_request);
    session_packets.insert(session_packets.end(), std::make_move_iterator(bag_packets.begin()),
                           std::make_move_iterator(bag_packets.end()));
    return true;
  }

  return false;
}

void InventorySession::observe_utility_bag_snapshot(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;

  // Shared-world entry restores the saved utility bag (quick item slots 1-4) once, after
  // the quest list, in the tutorial snapshot's order. Without it the client starts every
  // shared-world login with empty slots although the bag is persisted. An empty bag is
  // the client's initial state and is not announced.
  if (gameplay_request && gameplay_request->opcode == kTimeSyncRequest &&
      !context_.inventory_has_tutorial() && context_.inventory_has_handoff() &&
      !utility_bag_snapshot_sent_) {
    const auto progress = progress_.load();
    if (has_utility_bag_contents(progress)) {
      session_packets.push_back(
          {kWorldCharUtilityBagNotificationOpcode, {{"bag", utility_bag_json(progress)}}});
    }
    utility_bag_snapshot_sent_ = true;
  }
}

} // namespace chronicles::inventory
