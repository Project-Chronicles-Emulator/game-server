#include "world/features/gm/gm_commands.hpp"

#include "cms.generated.pb.h"
#include "world/features/gm/gm_tools.hpp"
#include "world/core/inventory_slots.hpp"
#include "persistence/account_repository.hpp"

#include <set>

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;

// GmCmd.ItemAdd.count is omitted (zero) when the sender relies on the default.
constexpr int kDefaultGmItemCount = 1;

constexpr std::string_view kGoldMoneyTypeName = "EX5MoneyType_Gold";

GmCommandParse parse_failure(std::string reason) {
  GmCommandParse result;
  result.error = std::move(reason);
  return result;
}

bool is_ascii_space(char character) {
  return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
         character == '\f' || character == '\v';
}

std::vector<std::string_view> split_tokens(std::string_view text) {
  std::vector<std::string_view> tokens;
  std::size_t position = 0;
  while (position < text.size()) {
    while (position < text.size() && is_ascii_space(text[position])) {
      ++position;
    }

    const auto token_start = position;
    while (position < text.size() && !is_ascii_space(text[position])) {
      ++position;
    }
    if (position > token_start) {
      tokens.push_back(text.substr(token_start, position - token_start));
    }
  }
  return tokens;
}

// Accepts only a complete decimal token, so "12abc" or "1e3" are rejected.
template <typename Integer> std::optional<Integer> parse_decimal(std::string_view token) {
  Integer value{};
  const auto *begin = token.data();
  const auto *end = token.data() + token.size();
  const auto [parsed_end, error] = std::from_chars(begin, end, value);
  if (error != std::errc() || parsed_end != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<GmCommandKind> text_command_kind(std::string_view name) {
  const auto normalized = ascii_lower(std::string(name));
  if (normalized == "item_add" || normalized == "itemadd" || normalized == "additem") {
    return GmCommandKind::item_add;
  }
  if (normalized == "add_money" || normalized == "addmoney" || normalized == "money" ||
      normalized == "gold") {
    return GmCommandKind::add_money;
  }
  if (normalized == "set_weapon_durability" || normalized == "weapon_durability" ||
      normalized == "durability") {
    return GmCommandKind::set_weapon_durability;
  }
  if (normalized == "itembag_clear" || normalized == "clear_bag") {
    return GmCommandKind::itembag_clear;
  }
  if (normalized == "exp_up" || normalized == "expup" || normalized == "exp") {
    return GmCommandKind::exp_up;
  }
  if (normalized == "set_level" || normalized == "setlevel" || normalized == "level") {
    return GmCommandKind::set_level;
  }
  if (normalized == "add_char_build_point") {
    return GmCommandKind::add_char_build_point;
  }
  if (normalized == "set_char_build_point") {
    return GmCommandKind::set_char_build_point;
  }
  if (normalized == "reset_stat_bonus") {
    return GmCommandKind::reset_stat_bonus;
  }
  if (normalized == "force_reset_perk") {
    return GmCommandKind::force_reset_perk;
  }
  if (normalized == "add_stat_bonus") {
    return GmCommandKind::add_stat_bonus;
  }
  if (normalized == "set_stat_bonus") {
    return GmCommandKind::set_stat_bonus;
  }
  if (normalized == "learn_all_perks") {
    return GmCommandKind::learn_all_perks;
  }
  return std::nullopt;
}

// Build point type token: the EX5CharBuildPointType number or a short name.
std::optional<int> text_build_point_type(std::string_view token) {
  const auto normalized = ascii_lower(std::string(token));
  if (normalized == "1" || normalized == "levelup" || normalized == "levelup_point") {
    return x5::cms::EX5CharBuildPointType_LevelUpPoint;
  }
  if (normalized == "2" || normalized == "weapon" || normalized == "weapon_point") {
    return x5::cms::EX5CharBuildPointType_WeaponPoint;
  }
  return std::nullopt;
}

GmCommandParse progression_command(GmCommandKind kind, std::int64_t value,
                                   int build_point_type = 0) {
  GmCommand command;
  command.kind = kind;
  command.progression_value = value;
  command.build_point_type = build_point_type;
  return GmCommandParse{{command}, {}};
}

GmCommandParse parse_text_exp_up(const std::vector<std::string_view> &tokens) {
  if (tokens.size() != 2) {
    return parse_failure("exp_up expects <exp_delta>");
  }

  const auto delta = parse_decimal<std::int32_t>(tokens[1]);
  if (!delta || *delta <= 0) {
    return parse_failure("exp_up exp_delta must be a positive int32");
  }
  return progression_command(GmCommandKind::exp_up, *delta);
}

GmCommandParse parse_text_set_level(const std::vector<std::string_view> &tokens) {
  if (tokens.size() != 2) {
    return parse_failure("set_level expects <level>");
  }

  const auto level = parse_decimal<int>(tokens[1]);
  if (!level || *level <= 0) {
    return parse_failure("set_level level must be a positive integer");
  }
  return progression_command(GmCommandKind::set_level, *level);
}

GmCommandParse parse_text_build_point(GmCommandKind kind,
                                      const std::vector<std::string_view> &tokens) {
  if (tokens.size() != 3) {
    return parse_failure("char_build_point expects <type> <value>");
  }

  const auto point_type = text_build_point_type(tokens[1]);
  const auto value = parse_decimal<std::int32_t>(tokens[2]);
  if (!point_type) {
    return parse_failure("char_build_point type must be 1, 2, levelup or weapon");
  }
  const bool valid_value = value && (kind == GmCommandKind::add_char_build_point
                                         ? *value != 0
                                         : *value >= 0);
  if (!valid_value) {
    return parse_failure("char_build_point value is out of range");
  }
  return progression_command(kind, *value, *point_type);
}

GmCommandParse parse_text_no_arguments(GmCommandKind kind,
                                       const std::vector<std::string_view> &tokens) {
  if (tokens.size() != 1) {
    return parse_failure(std::string(gm_command_name(kind)) + " takes no arguments");
  }
  return progression_command(kind, 0);
}

// Attribute bonuses exist only for Con..Tal (EX5StatType 1-6).
GmCommandParse stat_bonus_command(GmCommandKind kind, std::optional<int> stat_type,
                                  std::optional<std::int64_t> value) {
  const bool allocatable = stat_type && *stat_type >= x5::cms::EX5StatType_Con &&
                           *stat_type <= x5::cms::EX5StatType_Tal;
  if (!allocatable) {
    return parse_failure("stat_bonus type must be con, dex, end, int, str or tal");
  }
  if (!value || *value < 0 || *value > kMaximumStatBonus) {
    return parse_failure("stat_bonus value must be 0-" + std::to_string(kMaximumStatBonus));
  }

  GmCommand command;
  command.kind = kind;
  command.stat_type = *stat_type;
  command.progression_value = *value;
  return GmCommandParse{{command}, {}};
}

GmCommandParse parse_text_stat_bonus(GmCommandKind kind,
                                     const std::vector<std::string_view> &tokens) {
  if (tokens.size() != 3) {
    return parse_failure(std::string(gm_command_name(kind)) + " expects <stat> <value>");
  }
  return stat_bonus_command(kind, gm::parse_stat_type_token(tokens[1]),
                            parse_decimal<std::int64_t>(tokens[2]));
}

GmCommandParse parse_text_item_add(const std::vector<std::string_view> &tokens) {
  if (tokens.size() != 2 && tokens.size() != 3) {
    return parse_failure("item_add expects <item_cms_id> [count]");
  }

  const auto item_cms_id = parse_decimal<int>(tokens[1]);
  if (!item_cms_id || *item_cms_id <= 0) {
    return parse_failure("item_add item_cms_id must be a positive integer");
  }

  int item_count = kDefaultGmItemCount;
  if (tokens.size() == 3) {
    const auto parsed_count = parse_decimal<int>(tokens[2]);
    if (!parsed_count || *parsed_count <= 0) {
      return parse_failure("item_add count must be a positive integer");
    }
    item_count = *parsed_count;
  }

  GmCommand command;
  command.kind = GmCommandKind::item_add;
  command.item_cms_id = *item_cms_id;
  command.item_count = item_count;
  return GmCommandParse{{command}, {}};
}

GmCommandParse parse_text_add_money(const std::vector<std::string_view> &tokens) {
  if (tokens.size() != 2) {
    return parse_failure("add_money expects <gold_delta>");
  }

  const auto amount = parse_decimal<std::int64_t>(tokens[1]);
  if (!amount || *amount == 0) {
    return parse_failure("add_money gold_delta must be a nonzero integer");
  }

  GmCommand command;
  command.kind = GmCommandKind::add_money;
  command.money_amount = *amount;
  return GmCommandParse{{command}, {}};
}

GmCommandParse durability_command(std::int64_t durability) {
  if (durability < 0 || durability > std::numeric_limits<std::int32_t>::max()) {
    return parse_failure("set_weapon_durability value must be a non-negative int32");
  }

  GmCommand command;
  command.kind = GmCommandKind::set_weapon_durability;
  command.durability = static_cast<int>(durability);
  return GmCommandParse{{command}, {}};
}

GmCommandParse parse_text_weapon_durability(const std::vector<std::string_view> &tokens) {
  if (tokens.size() != 2) {
    return parse_failure("set_weapon_durability expects <value>");
  }

  const auto durability = parse_decimal<std::int64_t>(tokens[1]);
  if (!durability) {
    return parse_failure("set_weapon_durability value must be a non-negative int32");
  }
  return durability_command(*durability);
}

GmCommandParse parse_text_request(const Json &message) {
  const auto text_field = message.find("text");
  if (text_field == message.end() || !text_field->is_string()) {
    return parse_failure("GmCmdTextReq has no text");
  }

  const auto &text = text_field->get_ref<const std::string &>();
  if (text.size() > kMaximumGmCommandTextBytes) {
    return parse_failure("GmCmdTextReq text exceeds the emulator length limit");
  }

  const auto tokens = split_tokens(text);
  if (tokens.empty()) {
    return parse_failure("GmCmdTextReq text is empty");
  }

  const auto kind = text_command_kind(tokens.front());
  if (!kind) {
    return parse_failure("unsupported GM text command");
  }

  switch (*kind) {
  case GmCommandKind::item_add:
    return parse_text_item_add(tokens);
  case GmCommandKind::add_money:
    return parse_text_add_money(tokens);
  case GmCommandKind::set_weapon_durability:
    return parse_text_weapon_durability(tokens);
  case GmCommandKind::itembag_clear:
    if (tokens.size() != 1) {
      return parse_failure("itembag_clear takes no arguments");
    }
    return GmCommandParse{{GmCommand{GmCommandKind::itembag_clear}}, {}};
  case GmCommandKind::exp_up:
    return parse_text_exp_up(tokens);
  case GmCommandKind::set_level:
    return parse_text_set_level(tokens);
  case GmCommandKind::add_char_build_point:
  case GmCommandKind::set_char_build_point:
    return parse_text_build_point(*kind, tokens);
  case GmCommandKind::reset_stat_bonus:
  case GmCommandKind::force_reset_perk:
  case GmCommandKind::learn_all_perks:
    return parse_text_no_arguments(*kind, tokens);
  case GmCommandKind::add_stat_bonus:
  case GmCommandKind::set_stat_bonus:
    return parse_text_stat_bonus(*kind, tokens);
  }
  return parse_failure("unsupported GM text command");
}

// Protobuf JSON prints int32 as a number and int64 as a decimal string; omitted means zero.
std::optional<std::int64_t> json_integer(const Json &object, const char *field) {
  const auto value = object.find(field);
  if (value == object.end()) {
    return std::int64_t{};
  }
  if (value->is_number_integer()) {
    return value->get<std::int64_t>();
  }
  if (value->is_string()) {
    return parse_decimal<std::int64_t>(value->get_ref<const std::string &>());
  }
  return std::nullopt;
}

GmCommandParse parse_structured_item_add(const Json &body) {
  const auto item_cms_id = json_integer(body, "cms_id");
  const auto item_count = json_integer(body, "count");
  if (!item_cms_id || *item_cms_id <= 0 || *item_cms_id > std::numeric_limits<int>::max()) {
    return parse_failure("GmCmd.item_add cms_id must be a positive int32");
  }
  if (!item_count || *item_count < 0 || *item_count > std::numeric_limits<int>::max()) {
    return parse_failure("GmCmd.item_add count must be a non-negative int32");
  }

  GmCommand command;
  command.kind = GmCommandKind::item_add;
  command.item_cms_id = static_cast<int>(*item_cms_id);
  command.item_count = *item_count == 0 ? kDefaultGmItemCount : static_cast<int>(*item_count);
  return GmCommandParse{{command}, {}};
}

GmCommandParse parse_structured_add_money(const Json &body) {
  const auto money_type = body.find("type");
  if (money_type != body.end()) {
    const bool gold_name =
        money_type->is_string() && money_type->get_ref<const std::string &>() == kGoldMoneyTypeName;
    const bool gold_number = money_type->is_number_integer() && money_type->get<int>() == 0;
    if (!gold_name && !gold_number) {
      // Only gold is persisted by the emulator; Hersander Tokens have no storage yet.
      return parse_failure("GmCmd.add_money supports only EX5MoneyType_Gold");
    }
  }

  const auto amount = json_integer(body, "amount");
  if (!amount || *amount == 0) {
    return parse_failure("GmCmd.add_money amount must be a nonzero int64");
  }

  GmCommand command;
  command.kind = GmCommandKind::add_money;
  command.money_amount = *amount;
  return GmCommandParse{{command}, {}};
}

GmCommandParse parse_structured_exp_up(const Json &body) {
  const auto delta = json_integer(body, "delta");
  if (!delta || *delta <= 0 || *delta > std::numeric_limits<std::int32_t>::max()) {
    return parse_failure("GmCmd.exp_up delta must be a positive int32");
  }
  return progression_command(GmCommandKind::exp_up, *delta);
}

GmCommandParse parse_structured_set_level(const Json &body) {
  const auto level = json_integer(body, "level");
  if (!level || *level <= 0 || *level > std::numeric_limits<int>::max()) {
    return parse_failure("GmCmd.set_level level must be a positive integer");
  }
  return progression_command(GmCommandKind::set_level, *level);
}

// EX5CharBuildPointType arrives by enum name (protobuf JSON) or number.
int structured_build_point_type(const Json &body) {
  const auto type_field = body.find("type");
  if (type_field == body.end()) {
    return 0;
  }
  if (type_field->is_number_integer()) {
    return type_field->get<int>();
  }
  x5::cms::EX5CharBuildPointType parsed;
  if (type_field->is_string() &&
      x5::cms::EX5CharBuildPointType_Parse(type_field->get<std::string>(), &parsed)) {
    return static_cast<int>(parsed);
  }
  return 0;
}

// EX5StatType arrives by enum name (protobuf JSON) or number; omitted means None.
std::optional<int> structured_stat_type(const Json &body) {
  const auto type_field = body.find("type");
  if (type_field == body.end()) {
    return std::nullopt;
  }
  if (type_field->is_number_integer()) {
    return type_field->get<int>();
  }
  x5::cms::EX5StatType parsed;
  if (type_field->is_string() &&
      x5::cms::EX5StatType_Parse(type_field->get<std::string>(), &parsed)) {
    return static_cast<int>(parsed);
  }
  return std::nullopt;
}

GmCommandParse parse_structured_build_point(GmCommandKind kind, const Json &body) {
  const int point_type = structured_build_point_type(body);
  if (point_type != x5::cms::EX5CharBuildPointType_LevelUpPoint &&
      point_type != x5::cms::EX5CharBuildPointType_WeaponPoint) {
    return parse_failure("GmCmd char_build_point type must be LevelUpPoint or WeaponPoint");
  }

  const auto value = json_integer(body, "value");
  const bool in_range = value && *value >= std::numeric_limits<std::int32_t>::min() &&
                        *value <= std::numeric_limits<std::int32_t>::max();
  bool valid_value = false;
  if (in_range) {
    valid_value = kind == GmCommandKind::add_char_build_point ? *value != 0 : *value >= 0;
  }
  if (!valid_value) {
    return parse_failure("GmCmd char_build_point value is out of range");
  }
  return progression_command(kind, *value, point_type);
}

GmCommandParse parse_structured_request(const Json &message) {
  const auto commands = message.find("cmds");
  if (commands == message.end() || !commands->is_array() || commands->empty()) {
    return parse_failure("GmCmdReq has no commands");
  }
  if (commands->size() > kMaximumGmCommandsPerRequest) {
    return parse_failure("GmCmdReq exceeds the emulator command limit");
  }

  GmCommandParse result;
  for (const auto &entry : *commands) {
    // GmCmd is a single `cmd` oneof, so a well-formed entry has exactly one member.
    if (!entry.is_object() || entry.size() != 1 || !entry.begin().value().is_object()) {
      return parse_failure("GmCmd entry must set exactly one command");
    }

    const auto &field_name = entry.begin().key();
    const auto &body = entry.begin().value();
    GmCommandParse parsed;
    if (field_name == "item_add") {
      parsed = parse_structured_item_add(body);
    } else if (field_name == "add_money") {
      parsed = parse_structured_add_money(body);
    } else if (field_name == "set_weapon_durability") {
      const auto durability = json_integer(body, "durability");
      parsed = durability ? durability_command(*durability)
                          : parse_failure("GmCmd.set_weapon_durability durability is malformed");
    } else if (field_name == "itembag_clear") {
      // Removing equipped items would also need equipment-visual updates; not served.
      const auto equip = body.find("equip");
      const bool include_equipment = equip != body.end() && equip->is_boolean() && equip->get<bool>();
      parsed = include_equipment
                   ? parse_failure("GmCmd.itembag_clear with equip=true is not supported")
                   : GmCommandParse{{GmCommand{GmCommandKind::itembag_clear}}, {}};
    } else if (field_name == "exp_up") {
      parsed = parse_structured_exp_up(body);
    } else if (field_name == "set_level") {
      parsed = parse_structured_set_level(body);
    } else if (field_name == "add_char_build_point") {
      parsed = parse_structured_build_point(GmCommandKind::add_char_build_point, body);
    } else if (field_name == "set_char_build_point") {
      parsed = parse_structured_build_point(GmCommandKind::set_char_build_point, body);
    } else if (field_name == "reset_stat_bonus") {
      parsed = progression_command(GmCommandKind::reset_stat_bonus, 0);
    } else if (field_name == "force_reset_perk") {
      parsed = progression_command(GmCommandKind::force_reset_perk, 0);
    } else if (field_name == "learn_all_perks") {
      parsed = progression_command(GmCommandKind::learn_all_perks, 0);
    } else if (field_name == "add_stat_bonus" || field_name == "set_stat_bonus") {
      const auto kind = field_name == "add_stat_bonus" ? GmCommandKind::add_stat_bonus
                                                       : GmCommandKind::set_stat_bonus;
      parsed = stat_bonus_command(kind, structured_stat_type(body), json_integer(body, "value"));
    } else {
      // The field name comes from the recovered descriptor, not from free-form player text.
      return parse_failure("unsupported GmCmd field " + field_name);
    }
    if (!parsed.error.empty()) {
      return parsed;
    }

    result.commands.insert(result.commands.end(), parsed.commands.begin(), parsed.commands.end());
  }
  return result;
}

std::vector<int> free_inventory_slots(const Character &character) {
  std::set<int> occupied;
  for (const auto &item : character.items) {
    occupied.insert(item.inventory_slot);
  }

  std::vector<int> free_slots;
  for (int slot = 0; slot <= kMaximumInventorySlot; ++slot) {
    if (!occupied.contains(slot)) {
      free_slots.push_back(slot);
    }
  }
  return free_slots;
}

x5::ec::EErrorCode add_purchased_stack(ShopTrade &trade, int item_cms_id, int count) {
  auto &stackables = trade.next.stackables;
  if (!stackables.contains(item_cms_id) && stackables.size() >= kMaximumStackableKinds) {
    return x5::ec::ItemSlotLimit;
  }

  auto &owned = stackables[item_cms_id];
  if (owned > std::numeric_limits<int>::max() - count) {
    return x5::ec::ItemCountError;
  }
  owned += count;
  trade.changed_stackables.push_back(item_cms_id);
  return x5::ec::Success;
}

x5::ec::EErrorCode plan_gm_item_grant(const ShopCatalog &catalog, int item_cms_id, int count,
                           const Character &character, int maximum_instances, ShopTrade &trade) {
  const auto *item = catalog.item(item_cms_id);
  if (!item) {
    return x5::ec::ItemCmsIdInvalid;
  }
  if (count <= 0 || count > catalog.maximum_stack()) {
    return x5::ec::ItemCountError;
  }
  if (item->storage == ShopItemStorage::unsupported || item->storage == ShopItemStorage::housing) {
    return x5::ec::ItemError;
  }
  if (item->storage == ShopItemStorage::stackable) {
    return add_purchased_stack(trade, item_cms_id, count);
  }

  if (item->max_durability <= 0) {
    return x5::ec::Interaction_InvalidData;
  }
  const auto instance_count = trade.granted_items.size() + static_cast<std::size_t>(count);
  if (maximum_instances < 0 || instance_count > static_cast<std::size_t>(maximum_instances)) {
    return x5::ec::ItemCountError;
  }

  // Skip slots that instances planned earlier in this trade already reserved.
  std::set<int> planned_slots;
  for (const auto &planned : trade.granted_items) {
    planned_slots.insert(planned.inventory_slot);
  }
  std::vector<int> available_slots;
  for (const int slot : free_inventory_slots(character)) {
    if (!planned_slots.contains(slot)) {
      available_slots.push_back(slot);
    }
  }
  if (available_slots.size() < static_cast<std::size_t>(count)) {
    return x5::ec::ItemSlotLimit;
  }

  for (int index = 0; index < count; ++index) {
    CharacterItem granted;
    granted.cms_id = item_cms_id;
    granted.inventory_slot = available_slots[static_cast<std::size_t>(index)];
    granted.durability = item->max_durability;
    granted.grade = item->grade;
    trade.granted_items.push_back(granted);
  }

  return x5::ec::Success;
}

x5::ec::EErrorCode plan_money_change(const ShopCatalog &catalog, std::int64_t amount,
                                     ShopTrade &trade) {
  const auto current_gold = trade.next.gold;
  const bool overflows =
      amount > 0 && current_gold > std::numeric_limits<std::int64_t>::max() - amount;
  if (overflows) {
    return x5::ec::GmCmd_Fail;
  }

  const auto resulting_gold = current_gold + amount;
  if (resulting_gold < 0) {
    return x5::ec::MoneyNotEnough;
  }
  if (resulting_gold > catalog.gold_limit()) {
    // Money.limitUserMax would be exceeded; no official GM rejection was observed.
    return x5::ec::GmCmd_Fail;
  }

  trade.next.gold = resulting_gold;
  if (amount != 0) {
    trade.money_changes.push_back({MoneyType::Gold, amount, resulting_gold, true});
  }
  return x5::ec::Success;
}

bool is_weapon_slot(int equip_slot) {
  return equip_slot == x5::cms::EX5EquipSlotType_Weapon_1 ||
         equip_slot == x5::cms::EX5EquipSlotType_Weapon_2;
}

// Every equipped weapon takes the requested durability, capped at its catalog maximum. Weapons
// already at that value are left out; a request that changes nothing is refused.
x5::ec::EErrorCode plan_weapon_durability(const ShopCatalog &catalog, int durability,
                                          const Character &character, ShopTrade &trade) {
  bool weapon_equipped = false;
  for (const auto &owned : character.items) {
    if (!owned.equip_slot || !is_weapon_slot(*owned.equip_slot)) {
      continue;
    }
    weapon_equipped = true;

    const auto *item = catalog.item(owned.cms_id);
    if (!item || item->max_durability <= 0) {
      return x5::ec::Interaction_InvalidData;
    }
    const int target = std::min(durability, item->max_durability);
    if (target == owned.durability) {
      continue;
    }

    CharacterItem changed = owned;
    changed.durability = target;
    trade.durability_updates.push_back({owned.id, owned.durability, target});
    trade.repaired_items.push_back(changed);
  }

  if (!weapon_equipped || trade.repaired_items.empty()) {
    return x5::ec::GmCmd_Fail;
  }
  return x5::ec::Success;
}

// Unequipped instances leave the inventory; stackables keep only the units the utility bag
// references, so no quick slot points at missing items. Housing objects and vouchers stay.
x5::ec::EErrorCode plan_itembag_clear(const Character &character, ShopTrade &trade) {
  for (const auto &owned : character.items) {
    if (!owned.equip_slot) {
      trade.removed_items.push_back(owned);
    }
  }

  for (auto &[item_cms_id, quantity] : trade.next.stackables) {
    const int kept = std::min(quantity, reserved_utility_count(trade.next, item_cms_id));
    if (kept != quantity) {
      quantity = kept;
      trade.changed_stackables.push_back(item_cms_id);
    }
  }

  if (trade.removed_items.empty() && trade.changed_stackables.empty()) {
    return x5::ec::GmCmd_Fail;
  }
  return x5::ec::Success;
}

} // namespace

bool is_gm_command_opcode(std::uint16_t opcode) {
  return opcode == kGmCommandRequestOpcode || opcode == kGmCommandTextRequestOpcode;
}

GmCommandParse parse_gm_request(std::uint16_t opcode, const nlohmann::json &message) {
  if (!message.is_object()) {
    return parse_failure("GM request is not a message object");
  }
  if (opcode == kGmCommandTextRequestOpcode) {
    return parse_text_request(message);
  }
  if (opcode == kGmCommandRequestOpcode) {
    return parse_structured_request(message);
  }
  return parse_failure("not a GM command opcode");
}

GmCommandPlan plan_gm_command(const ShopCatalog &catalog, const GmCommand &command,
                              const Character &character, const WorldProgress &before) {
  GmCommandPlan plan;
  if (before.revision == std::numeric_limits<std::int64_t>::max()) {
    plan.error = x5::ec::GmCmd_Fail;
    return plan;
  }

  // Storage accepts a transition only when the revision advances by exactly one.
  plan.trade.next = before;
  ++plan.trade.next.revision;

  switch (command.kind) {
  case GmCommandKind::item_add:
    plan.error = plan_gm_item_grant(catalog, command.item_cms_id, command.item_count, character,
                                 kMaximumGmGrantedInstances, plan.trade);
    break;
  case GmCommandKind::add_money:
    plan.error = plan_money_change(catalog, command.money_amount, plan.trade);
    break;
  case GmCommandKind::set_weapon_durability:
    plan.error = plan_weapon_durability(catalog, command.durability, character, plan.trade);
    break;
  case GmCommandKind::itembag_clear:
    plan.error = plan_itembag_clear(character, plan.trade);
    break;
  case GmCommandKind::exp_up:
  case GmCommandKind::set_level:
  case GmCommandKind::add_char_build_point:
  case GmCommandKind::set_char_build_point:
  case GmCommandKind::reset_stat_bonus:
  case GmCommandKind::force_reset_perk:
  case GmCommandKind::add_stat_bonus:
  case GmCommandKind::set_stat_bonus:
  case GmCommandKind::learn_all_perks:
    // Progression commands are applied through Progression by the world session.
    plan.error = x5::ec::GmCmd_Fail;
    break;
  }
  return plan;
}

GmBundlePlan plan_gm_grant_bundle(const ShopCatalog &catalog,
                                  const std::vector<GmBundleItem> &items,
                                  std::optional<std::int64_t> gold_target,
                                  const Character &character, const WorldProgress &before,
                                  int maximum_instances) {
  GmBundlePlan plan;
  if (before.revision == std::numeric_limits<std::int64_t>::max()) {
    plan.error = x5::ec::GmCmd_Fail;
    return plan;
  }

  // Storage accepts a transition only when the revision advances by exactly one.
  plan.trade.next = before;
  ++plan.trade.next.revision;

  std::set<int> owned_instance_ids;
  for (const auto &owned : character.items) {
    owned_instance_ids.insert(owned.cms_id);
  }

  for (const auto &entry : items) {
    const auto *item = catalog.item(entry.item_cms_id);
    const bool storable =
        item && (item->storage == ShopItemStorage::stackable ||
                 (item->storage == ShopItemStorage::instance && item->max_durability > 0));
    if (!storable || entry.count <= 0) {
      plan.skipped_item_ids.push_back(entry.item_cms_id);
      continue;
    }

    if (item->storage == ShopItemStorage::stackable) {
      const int wanted = std::min(entry.count, catalog.maximum_stack());
      const auto owned = plan.trade.next.stackables.find(entry.item_cms_id);
      const int current = owned == plan.trade.next.stackables.end() ? 0 : owned->second;
      if (current >= wanted) {
        continue;
      }

      plan.error = add_purchased_stack(plan.trade, entry.item_cms_id, wanted - current);
    } else {
      if (owned_instance_ids.contains(entry.item_cms_id)) {
        continue;
      }

      plan.error = plan_gm_item_grant(catalog, entry.item_cms_id, 1, character,
                                      maximum_instances, plan.trade);
      owned_instance_ids.insert(entry.item_cms_id);
    }

    if (plan.error != x5::ec::Success) {
      return plan;
    }
  }

  if (gold_target) {
    const auto capped_target = std::min(*gold_target, catalog.gold_limit());
    if (capped_target > plan.trade.next.gold) {
      plan.error = plan_money_change(catalog, capped_target - plan.trade.next.gold, plan.trade);
    }
  }
  return plan;
}

GameplayPacket gm_command_packet(const ShopTrade &trade,
                                 const std::vector<CharacterItem> &committed_grants) {
  // Reuse the common item and balance serializer; GM updates carry the command reason.
  auto packet = merchant_trade_packets(ShopRequest{}, trade, committed_grants).front();
  packet.message.erase("shop");
  packet.message.erase("packet_seq");
  packet.message["command"] = nlohmann::json::object();
  return packet;
}

std::vector<std::string_view> split_gm_command_tokens(std::string_view text) {
  return split_tokens(text);
}

std::optional<std::int64_t> parse_gm_integer(std::string_view token) {
  return parse_decimal<std::int64_t>(token);
}

const char *gm_command_name(GmCommandKind kind) {
  switch (kind) {
  case GmCommandKind::item_add:
    return "item_add";
  case GmCommandKind::add_money:
    return "add_money";
  case GmCommandKind::set_weapon_durability:
    return "set_weapon_durability";
  case GmCommandKind::itembag_clear:
    return "itembag_clear";
  case GmCommandKind::exp_up:
    return "exp_up";
  case GmCommandKind::set_level:
    return "set_level";
  case GmCommandKind::add_char_build_point:
    return "add_char_build_point";
  case GmCommandKind::set_char_build_point:
    return "set_char_build_point";
  case GmCommandKind::reset_stat_bonus:
    return "reset_stat_bonus";
  case GmCommandKind::force_reset_perk:
    return "force_reset_perk";
  case GmCommandKind::add_stat_bonus:
    return "add_stat_bonus";
  case GmCommandKind::set_stat_bonus:
    return "set_stat_bonus";
  case GmCommandKind::learn_all_perks:
    return "learn_all_perks";
  }
  return "unknown";
}

bool is_progression_gm_command(GmCommandKind kind) {
  return kind != GmCommandKind::item_add && kind != GmCommandKind::add_money &&
         kind != GmCommandKind::set_weapon_durability && kind != GmCommandKind::itembag_clear;
}

} // namespace chronicles
