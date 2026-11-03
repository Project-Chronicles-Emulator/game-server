#include "world/features/item_use/item_use.hpp"

#include "common/currency.hpp"
#include "ec.generated.pb.h"
#include "persistence/item_wire.hpp"
#include "world/core/inventory_slots.hpp"
#include "world/core/progress_packets.hpp"
#include "world/features/inventory/utility_bag.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::uint16_t kCommonErrorResponse = 2;
constexpr std::uint16_t kItemsUpdateNotification = 169;
constexpr char kTokenMoneyTypeName[] = "EX5MoneyType_HersanderToken";

// No box row grants more than this per roll, so summing rows cannot overflow.
constexpr int kMaximumEntryQuantity = 100000000;
// Equipment rows are stored one instance per unit; a box never grants more in one use.
constexpr int kMaximumInstancesPerUse = 32;

std::optional<int> positive_integer(const Json &message, const char *key) {
  const auto found = message.find(key);
  if (found == message.end() || !found->is_number_integer()) {
    return std::nullopt;
  }
  const auto value = found->get<std::int64_t>();
  if (value <= 0 || value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

// Parses a catalog integer column; an empty column is `fallback`.
int catalog_integer(const Json &row, const char *key, int fallback) {
  const auto text = row.value(key, std::string{});
  if (text.empty()) {
    return fallback;
  }
  int value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw std::runtime_error(std::string("Malformed draw-box column ") + key + ": " + text);
  }
  return value;
}

Json packet_seq(const Json &message, std::uint32_t sequence) {
  const auto found = message.find("packet_seq");
  if (found != message.end() && found->is_object()) {
    return *found;
  }
  return {{"seq", std::to_string(sequence)}};
}

GameplayPacket common_error(const Json &message, std::uint32_t sequence,
                            x5::ec::EErrorCode code) {
  return {kCommonErrorResponse,
          {{"ec", x5::ec::EErrorCode_Name(code)}, {"packet_seq", packet_seq(message, sequence)}}};
}

// Candidate: when a spent unit leaves a utility slot reserving more than the remaining stack,
// the committed bag is announced with UtilityBagSetRes, as the Nui statue refill does. No
// official ItemUseReq frame shows how the client learns a lowered quick-slot count.
void append_utility_bag_update(const WorldProgress &after, bool bag_changed,
                               std::vector<GameplayPacket> &packets) {
  if (!bag_changed) {
    return;
  }
  packets.push_back({kUtilityBagSetResponseOpcode, {{"bag", utility_bag_json(after)}}});
}

DrawBoxEntry parse_entry(const Json &row) {
  DrawBoxEntry entry;
  const auto money = row.value("moneyType", std::string{});
  if (money == "Gold") {
    entry.reward = DrawBoxReward::gold;
  } else if (money == "HersanderToken") {
    entry.reward = DrawBoxReward::hersander_token;
  } else if (money.empty()) {
    entry.item_cms_id = catalog_integer(row, "itemId", 0);
    if (entry.item_cms_id <= 0) {
      throw std::runtime_error("Draw-box row names neither an item nor money");
    }
  } else {
    throw std::runtime_error("Unknown draw-box money type: " + money);
  }

  entry.min_quantity = catalog_integer(row, "minAmount", 1);
  entry.max_quantity = catalog_integer(row, "maxAmount", entry.min_quantity);
  entry.weight = catalog_integer(row, "drawWeight", 0);
  if (entry.min_quantity <= 0 || entry.min_quantity > entry.max_quantity ||
      entry.max_quantity > kMaximumEntryQuantity || entry.weight < 0) {
    throw std::runtime_error("Draw-box row quantity or weight is out of range");
  }
  return entry;
}

} // namespace

std::vector<std::string> ItemUseCatalog::required_tables() {
  return {"Item", "DrawBox"};
}

std::shared_ptr<const ItemUseCatalog> ItemUseCatalog::load(ClientCatalog &catalog) {
  std::optional<std::vector<Json>> crafts;
  if (catalog.has_table("ItemCraft")) {
    crafts = catalog.rows("ItemCraft");
  }
  return std::make_shared<const ItemUseCatalog>(catalog.rows("Item"), catalog.rows("DrawBox"),
                                                crafts);
}

ItemUseCatalog::ItemUseCatalog(const std::vector<Json> &items, const std::vector<Json> &draw_boxes,
                               const std::optional<std::vector<Json>> &crafts) {
  const std::vector<Json> no_crafts;
  std::set<int> defined_crafts;
  for (const auto &craft : crafts ? *crafts : no_crafts) {
    const int craft_id = catalog_integer(craft, "craftId", 0);
    if (craft_id <= 0 || !defined_crafts.insert(craft_id).second) {
      throw std::runtime_error("Recipe definition has an invalid craft id");
    }
    const auto automatic = craft.value("autoRegistration", "");
    if (automatic != "True" && automatic != "False") {
      throw std::runtime_error("Recipe definition has an invalid registration flag");
    }
    if (automatic == "True") {
      automatic_crafts_.insert(craft_id);
    }
  }
  for (const auto &row : draw_boxes) {
    const int box_id = catalog_integer(row, "drawBoxId", 0);
    if (box_id <= 0) {
      throw std::runtime_error("Draw-box row without a box id");
    }
    auto entry = parse_entry(row);
    auto &box = boxes_[box_id];
    if (entry.weight > 0) {
      box.weighted.push_back(entry);
    } else {
      box.guaranteed.push_back(entry);
    }
  }

  for (const auto &row : items) {
    // Without the ItemCraft table a recipe cannot be validated, so it is left unusable.
    if (crafts && row.value("category", "") == "CraftRecipe") {
      const int item_id = catalog_integer(row, "id", 0);
      const int craft_id = catalog_integer(row, "FK_itemCraftId", 0);
      if (item_id <= 0 || !defined_crafts.contains(craft_id) ||
          !item_recipes_.emplace(item_id, craft_id).second) {
        throw std::runtime_error("Recipe item names an undefined craft");
      }
    }
    const int box_id = catalog_integer(row, "FK_drawBoxId", 0);
    if (box_id <= 0) {
      continue;
    }
    if (!boxes_.contains(box_id)) {
      throw std::runtime_error("Item names an undefined draw box " + std::to_string(box_id));
    }
    item_boxes_.emplace(catalog_integer(row, "id", 0), box_id);
  }
}

const DrawBox *ItemUseCatalog::draw_box_for_item(int item_cms_id) const {
  const auto item = item_boxes_.find(item_cms_id);
  if (item == item_boxes_.end()) {
    return nullptr;
  }
  return &boxes_.at(item->second);
}

std::size_t ItemUseCatalog::draw_box_count() const {
  return item_boxes_.size();
}

std::optional<int> ItemUseCatalog::recipe_for_item(int item_cms_id) const {
  const auto found = item_recipes_.find(item_cms_id);
  return found == item_recipes_.end() ? std::nullopt : std::optional{found->second};
}

bool ItemUseCatalog::recipe_is_automatic(int craft_id) const {
  return automatic_crafts_.contains(craft_id);
}

ItemUseSession::ItemUseSession(ItemUseServices services, ItemUsePersistence persistence,
                               std::unique_ptr<LootRandom> random)
    : services_(std::move(services)), persistence_(std::move(persistence)),
      random_(std::move(random)) {
  if (!services_.catalog || !services_.items || !persistence_.load_character ||
      !persistence_.load_progress || !persistence_.commit || !random_) {
    throw std::invalid_argument("ItemUseSession requires its catalogs, storage and random source");
  }
}

bool ItemUseSession::handles(const Json &message) const {
  const auto item = positive_integer(message, "item_cms_id");
  if (!item) {
    return false;
  }
  if (services_.catalog->draw_box_for_item(*item) || services_.catalog->recipe_for_item(*item)) {
    return true;
  }
  if (services_.effects.effect_list && services_.effects.effect_list(*item) != nullptr) {
    return true;
  }
  return services_.quest_use_item && services_.quest_use_item(*item);
}

QuestEvents ItemUseSession::take_quest_events() {
  auto events = std::move(quest_events_);
  quest_events_.clear();
  return events;
}

std::vector<GameplayPacket> ItemUseSession::receive(const Json &message, std::uint32_t sequence) {
  const auto item = positive_integer(message, "item_cms_id");
  if (!item) {
    return {common_error(message, sequence, x5::ec::BadRequest)};
  }
  // Stackable consumables carry no instance id; one use spends one item.
  if (message.contains("item_id") && message.at("item_id") != "0") {
    return {common_error(message, sequence, x5::ec::ItemIdInvalid)};
  }

  std::vector<GameplayPacket> packets;
  if (const auto *box = services_.catalog->draw_box_for_item(*item)) {
    packets = open_draw_box(message, sequence, *item, *box);
  } else if (const auto craft_id = services_.catalog->recipe_for_item(*item)) {
    packets = learn_recipe(message, sequence, *item, *craft_id);
  } else if (const auto *effects =
                 services_.effects.effect_list ? services_.effects.effect_list(*item) : nullptr) {
    packets = use_effect_item(message, sequence, *item, *effects);
  } else if (services_.quest_use_item && services_.quest_use_item(*item)) {
    packets = use_quest_item(message, sequence, *item);
  } else {
    return {common_error(message, sequence, x5::ec::ItemUnusable)};
  }

  // Every committed use (its reply leads with the ItemsUpdateNoti use reason) counts toward
  // CounterUseItem objectives; refusals reply with an error instead.
  if (!packets.empty() && packets.front().opcode == kItemsUpdateNotification) {
    quest_events_.push_back({QuestEventKind::ItemUsed, *item, 0, 1});
  }
  return packets;
}

std::vector<GameplayPacket> ItemUseSession::use_quest_item(const Json &message,
                                                           std::uint32_t sequence,
                                                           int item_cms_id) {
  if (message.value("count", 1) != 1) {
    return {common_error(message, sequence, x5::ec::BadRequest)};
  }
  const auto before = persistence_.load_progress();
  if (before.dead) {
    return {common_error(message, sequence, x5::ec::InvalidState)};
  }
  const auto owned = before.stackables.find(item_cms_id);
  if (owned == before.stackables.end() || owned->second <= 0) {
    return {common_error(message, sequence, x5::ec::ItemCountError)};
  }
  if (before.revision == std::numeric_limits<std::int64_t>::max()) {
    return {common_error(message, sequence, x5::ec::ItemError)};
  }

  auto after = before;
  ++after.revision;
  --after.stackables.at(item_cms_id);
  const bool bag_changed = release_unbacked_utility_units(after, item_cms_id);
  if (!persistence_.commit(before, after, {})) {
    return {common_error(message, sequence, x5::ec::ItemError)};
  }

  std::vector<GameplayPacket> packets{
      {kItemsUpdateNotification,
       {{"stackables", Json::array({{{"item_cms_id", item_cms_id},
                                     {"quantity", after.stackables.at(item_cms_id)}}})},
        {"use", Json::object()},
        {"packet_seq", packet_seq(message, sequence)}}}};
  append_utility_bag_update(after, bag_changed, packets);
  return packets;
}

std::vector<GameplayPacket> ItemUseSession::learn_recipe(const Json &message,
                                                         std::uint32_t sequence, int item_cms_id,
                                                         int craft_id) {
  if (message.value("count", 1) != 1 ||
      (message.contains("target_unit_id") && message.at("target_unit_id") != "0")) {
    return {common_error(message, sequence, x5::ec::BadRequest)};
  }
  const auto before = persistence_.load_progress();
  if (before.dead) {
    return {common_error(message, sequence, x5::ec::InvalidState)};
  }
  if (services_.catalog->recipe_is_automatic(craft_id) ||
      before.learned_crafts.contains(craft_id)) {
    return {common_error(message, sequence, x5::ec::Craft_SkillAlreadyLearn)};
  }
  const auto owned = before.stackables.find(item_cms_id);
  if (owned == before.stackables.end() || owned->second <= 0) {
    return {common_error(message, sequence, x5::ec::ItemCountError)};
  }
  if (before.learned_crafts.size() >= kMaximumLearnedCrafts ||
      before.revision == std::numeric_limits<std::int64_t>::max()) {
    return {common_error(message, sequence, x5::ec::ItemError)};
  }

  auto after = before;
  ++after.revision;
  --after.stackables.at(item_cms_id);
  after.learned_crafts.insert(craft_id);
  const bool bag_changed = release_unbacked_utility_units(after, item_cms_id);
  if (!persistence_.commit(before, after, {})) {
    return {common_error(message, sequence, x5::ec::ItemError)};
  }

  // WorldCharItemCraftsNoti.update merges new ids; a non-update entry snapshot replaces them.
  // Candidate acquisition ordering: the consumed inventory item precedes the learned recipe.
  std::vector<GameplayPacket> packets{
      {kItemsUpdateNotification,
       {{"stackables", Json::array({{{"item_cms_id", item_cms_id},
                                     {"quantity", after.stackables.at(item_cms_id)}}})},
        {"use", Json::object()},
        {"packet_seq", packet_seq(message, sequence)}}},
      {132, {{"update", true}, {"craft_ids", Json::array({craft_id})}}}};
  append_utility_bag_update(after, bag_changed, packets);
  return packets;
}

bool ItemUseSession::grant(const DrawBoxEntry &entry, int quantity, DrawResult &result) const {
  switch (entry.reward) {
  case DrawBoxReward::gold:
    result.gold += quantity;
    return true;
  case DrawBoxReward::hersander_token:
    result.hersander_token += quantity;
    return true;
  case DrawBoxReward::item:
    break;
  }

  const auto *definition = services_.items->item(entry.item_cms_id);
  if (!definition) {
    return false;
  }
  switch (definition->storage) {
  case ShopItemStorage::stackable:
    result.stackables[entry.item_cms_id] += quantity;
    return true;
  case ShopItemStorage::housing:
    result.housing_items[entry.item_cms_id] += quantity;
    return true;
  case ShopItemStorage::instance:
    if (result.instances.size() + quantity > kMaximumInstancesPerUse) {
      return false;
    }
    result.instances.insert(result.instances.end(), quantity, entry.item_cms_id);
    return true;
  case ShopItemStorage::unsupported:
    return false;
  }
  return false;
}

bool ItemUseSession::roll(const DrawBox &box, DrawResult &result) {
  for (const auto &entry : box.guaranteed) {
    if (!grant(entry, random_->pick_quantity(entry.min_quantity, entry.max_quantity), result)) {
      return false;
    }
  }
  if (box.weighted.empty()) {
    return true;
  }

  std::vector<int> weights;
  for (const auto &entry : box.weighted) {
    weights.push_back(entry.weight);
  }
  const auto &picked = box.weighted.at(random_->pick_index(weights));
  return grant(picked, random_->pick_quantity(picked.min_quantity, picked.max_quantity), result);
}

std::vector<GameplayPacket> ItemUseSession::open_draw_box(const Json &message,
                                                          std::uint32_t sequence,
                                                          int item_cms_id, const DrawBox &box) {
  const auto character = persistence_.load_character();
  const auto before = persistence_.load_progress();
  const auto owned = before.stackables.find(item_cms_id);
  if (!character || before.dead) {
    return {common_error(message, sequence, x5::ec::InvalidState)};
  }
  if (owned == before.stackables.end() || owned->second <= 0) {
    return {common_error(message, sequence, x5::ec::ItemCountError)};
  }

  DrawResult result;
  if (!roll(box, result)) {
    return {common_error(message, sequence, x5::ec::ItemUnusable)};
  }

  // Plan every change on a copy; nothing is written unless all of it fits.
  auto after = before;
  if (after.revision == std::numeric_limits<std::int64_t>::max()) {
    return {common_error(message, sequence, x5::ec::ItemError)};
  }
  ++after.revision;
  after.stackables[item_cms_id] -= 1;
  const bool bag_changed = release_unbacked_utility_units(after, item_cms_id);
  // CUSTOM: money above the wallet limit is dropped and the box still opens. Graduated
  // characters hold the full wallet, so refusing made money-paying boxes unusable for them
  // (official rule not captured). The reported change is the amount actually credited.
  result.gold = std::min(result.gold, std::max<std::int64_t>(0, kMoneyLimit - after.gold));
  result.hersander_token = std::min(
      result.hersander_token, std::max<std::int64_t>(0, kMoneyLimit - after.hersander_token));
  after.gold += result.gold;
  after.hersander_token += result.hersander_token;
  for (const auto &[stack_id, quantity] : result.stackables) {
    if (!after.stackables.contains(stack_id) &&
        after.stackables.size() >= kMaximumStackableKinds) {
      return {common_error(message, sequence, x5::ec::ItemSlotLimit)};
    }
    auto &stack = after.stackables[stack_id];
    if (quantity > std::numeric_limits<int>::max() - stack) {
      return {common_error(message, sequence, x5::ec::ItemCountError)};
    }
    stack += quantity;
  }
  for (const auto &[housing_id, quantity] : result.housing_items) {
    auto &record = after.housing_items[housing_id];
    if (quantity > std::numeric_limits<int>::max() - record.quantity) {
      return {common_error(message, sequence, x5::ec::ItemCountError)};
    }
    record.quantity += quantity;
  }

  const auto slots = available_inventory_slots(*character, result.instances.size());
  if (slots.size() < result.instances.size()) {
    return {common_error(message, sequence, x5::ec::ItemSlotLimit)};
  }
  std::vector<CharacterItem> granted;
  for (std::size_t index = 0; index < result.instances.size(); ++index) {
    const auto *definition = services_.items->item(result.instances[index]);
    CharacterItem item;
    item.cms_id = result.instances[index];
    item.inventory_slot = slots[index];
    item.durability = definition->max_durability;
    item.grade = definition->grade;
    granted.push_back(item);
  }

  const auto stored = persistence_.commit(before, after, granted);
  if (!stored) {
    return {common_error(message, sequence, x5::ec::ItemError)};
  }

  // ItemStackable.quantity and HousingItemCount.quantity are resulting totals, as in every other
  // ItemsUpdateNoti reason; MoneyChange.result is the new balance.
  Json stackables = Json::array({{{"item_cms_id", item_cms_id},
                                  {"quantity", after.stackables.at(item_cms_id)}}});
  for (const auto &[stack_id, quantity] : result.stackables) {
    if (stack_id != item_cms_id) {
      stackables.push_back(
          {{"item_cms_id", stack_id}, {"quantity", after.stackables.at(stack_id)}});
    }
  }
  Json update{{"stackables", std::move(stackables)},
              {"use", Json::object()},
              {"packet_seq", packet_seq(message, sequence)}};
  if (!stored->empty()) {
    Json items = Json::array();
    for (const auto &item : *stored) {
      items.push_back(item_instance_json(item));
    }
    update["items"] = std::move(items);
  }
  if (!result.housing_items.empty()) {
    Json housing = Json::array();
    for (const auto &[housing_id, quantity] : result.housing_items) {
      const auto &record = after.housing_items.at(housing_id);
      housing.push_back(housing_item_count_json(housing_id, record));
    }
    update["housing_items"] = std::move(housing);
  }
  Json money = Json::array();
  if (result.gold > 0) {
    money.push_back({{"change", std::to_string(result.gold)},
                     {"result", std::to_string(after.gold)}});
  }
  if (result.hersander_token > 0) {
    money.push_back({{"change", std::to_string(result.hersander_token)},
                     {"result", std::to_string(after.hersander_token)},
                     {"money_type", kTokenMoneyTypeName}});
  }
  if (!money.empty()) {
    update["money_changes"] = std::move(money);
  }
  std::vector<GameplayPacket> packets{{kItemsUpdateNotification, std::move(update)}};
  append_utility_bag_update(after, bag_changed, packets);
  return packets;
}

std::vector<GameplayPacket> ItemUseSession::use_effect_item(const Json &message,
                                                            std::uint32_t sequence,
                                                            int item_cms_id,
                                                            const std::vector<int> &effect_ids) {
  if (!services_.effects.apply || !services_.effects.ready) {
    return {common_error(message, sequence, x5::ec::ItemUnusable)};
  }
  const auto before = persistence_.load_progress();
  const auto owned = before.stackables.find(item_cms_id);
  if (before.dead || !services_.effects.ready()) {
    return {common_error(message, sequence, x5::ec::InvalidState)};
  }
  if (owned == before.stackables.end() || owned->second <= 0) {
    return {common_error(message, sequence, x5::ec::ItemCountError)};
  }

  // The item is committed first: the effects must never apply without spending it.
  auto after = before;
  if (after.revision == std::numeric_limits<std::int64_t>::max()) {
    return {common_error(message, sequence, x5::ec::ItemError)};
  }
  ++after.revision;
  after.stackables[item_cms_id] -= 1;
  const bool bag_changed = release_unbacked_utility_units(after, item_cms_id);
  if (!persistence_.commit(before, after, {})) {
    return {common_error(message, sequence, x5::ec::ItemError)};
  }

  std::vector<GameplayPacket> packets;
  packets.push_back({kItemsUpdateNotification,
                     {{"stackables", Json::array({{{"item_cms_id", item_cms_id},
                                                   {"quantity", after.stackables.at(item_cms_id)}}})},
                      {"use", Json::object()},
                      {"packet_seq", packet_seq(message, sequence)}}});
  append_utility_bag_update(after, bag_changed, packets);
  auto effects = services_.effects.apply(effect_ids);
  packets.insert(packets.end(), std::make_move_iterator(effects.begin()),
                 std::make_move_iterator(effects.end()));
  return packets;
}

} // namespace chronicles
