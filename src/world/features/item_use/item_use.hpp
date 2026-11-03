#pragma once

// PC2S_ItemUseReq (opcode 20077) for consumables that have no item skill.
//
// No official ItemUseReq was captured; this module is a Candidate built from the client
// catalog. It serves three kinds of consumables:
//   - draw boxes (Item.FK_drawBoxId): one use opens one box and grants its DrawBox rows;
//   - effect consumables (Item.FK_skillEffectList, for example the FGT food buffs): one use
//     applies the listed skill effects to the user through the combat session;
//   - recipe items (category CraftRecipe, Item.FK_itemCraftId): one use spends the item and
//     records the craft as learned, once per character.
// Summon-registering consumables are owned by the companion session and are never claimed here.
// A use answers with the ItemsUpdateNoti use reason, the reply the companion registration path
// already sends; a refusal answers with the common error response and changes nothing.

#include "data/client_catalog.hpp"
#include "data/shop_catalog.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/core/quest_events.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/loot/loot.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace chronicles {

inline constexpr std::uint16_t kItemUseRequestOpcode = world_request::kItemUseRequest;

enum class DrawBoxReward {
  item,
  gold,
  hersander_token,
};

// One DrawBox row: an item or money amount, granted with a uniform quantity in [min, max].
struct DrawBoxEntry {
  DrawBoxReward reward{DrawBoxReward::item};
  int item_cms_id{};
  int min_quantity{};
  int max_quantity{};
  int weight{};
};

// Candidate draw rule: rows without a drawWeight are always granted; exactly one row is picked
// from the rows that carry a weight, proportionally to the weights. The catalog mixes both kinds
// in one box (for example box 50: gold and one item fixed, four weighted alternatives).
struct DrawBox {
  std::vector<DrawBoxEntry> guaranteed;
  std::vector<DrawBoxEntry> weighted;
};

// Immutable draw-box and recipe-item index built once from the client catalog.
class ItemUseCatalog {
public:
  // Item and DrawBox are required. ItemCraft is optional: without it recipe items are not
  // indexed (and so stay unusable), while draw boxes and effect consumables keep working.
  static std::vector<std::string> required_tables();
  static std::shared_ptr<const ItemUseCatalog> load(ClientCatalog &catalog);

  // Throws std::runtime_error for malformed rows, a box an item names but no row defines, or
  // (when crafts are supplied) a recipe item naming an undefined craft.
  ItemUseCatalog(const std::vector<nlohmann::json> &items,
                 const std::vector<nlohmann::json> &draw_boxes,
                 const std::optional<std::vector<nlohmann::json>> &crafts = std::nullopt);

  const DrawBox *draw_box_for_item(int item_cms_id) const;
  std::size_t draw_box_count() const;
  std::optional<int> recipe_for_item(int item_cms_id) const;
  bool recipe_is_automatic(int craft_id) const;

private:
  std::map<int, int> item_boxes_;
  std::map<int, DrawBox> boxes_;
  std::map<int, int> item_recipes_;
  std::set<int> automatic_crafts_;
};

// Durable inventory for one character. commit applies the progress change and stores the
// granted item instances in one transaction; nullopt means nothing was written.
struct ItemUsePersistence {
  std::function<std::optional<Character>()> load_character;
  std::function<WorldProgress()> load_progress;
  std::function<std::optional<std::vector<CharacterItem>>(
      const WorldProgress &before, const WorldProgress &after,
      const std::vector<CharacterItem> &granted_items)>
      commit;
};

// Effect consumables need the combat session. Both callbacks are empty when the session has
// none, and such items are then refused without being spent.
struct ItemEffectAccess {
  std::function<const std::vector<int> *(int item_cms_id)> effect_list;
  // False while the effects cannot apply (the user is dead); the item is then not spent.
  std::function<bool()> ready;
  // Applies an effect list to the user after the item was spent.
  std::function<std::vector<GameplayPacket>(const std::vector<int> &effect_ids)> apply;
};

struct ItemUseServices {
  std::shared_ptr<const ItemUseCatalog> catalog;
  // Item storage kinds, grades and durability (shared with the NPC shop).
  std::shared_ptr<const ShopCatalog> items;
  ItemEffectAccess effects;
  // Items a quest objective asks the player to use (CounterUseItem, UseItemInTriggerVolume)
  // that have no other use on this server; using one spends it for the quest. Empty when the
  // realm has no quest catalog.
  std::function<bool(int item_cms_id)> quest_use_item;
};

class ItemUseSession {
public:
  ItemUseSession(ItemUseServices services, ItemUsePersistence persistence,
                 std::unique_ptr<LootRandom> random);

  // True for an ItemUseReq naming a draw box, an effect consumable, a recipe or a quest item.
  bool handles(const nlohmann::json &message) const;

  std::vector<GameplayPacket> receive(const nlohmann::json &message, std::uint32_t sequence);

  // Committed item uses since the last call, for quest objectives.
  QuestEvents take_quest_events();

private:
  // What one opened box grants before it is committed.
  struct DrawResult {
    std::int64_t gold{};
    std::int64_t hersander_token{};
    std::map<int, int> stackables;
    std::map<int, int> housing_items;
    std::vector<int> instances;
  };

  std::vector<GameplayPacket> open_draw_box(const nlohmann::json &message, std::uint32_t sequence,
                                            int item_cms_id, const DrawBox &box);
  std::vector<GameplayPacket> use_effect_item(const nlohmann::json &message,
                                              std::uint32_t sequence, int item_cms_id,
                                              const std::vector<int> &effect_ids);
  std::vector<GameplayPacket> learn_recipe(const nlohmann::json &message, std::uint32_t sequence,
                                           int item_cms_id, int craft_id);
  // Spends one quest-use item; the quest service credits the use.
  std::vector<GameplayPacket> use_quest_item(const nlohmann::json &message,
                                             std::uint32_t sequence, int item_cms_id);
  // Rolls one box into `result`; false when a row names an item without a storage kind.
  bool roll(const DrawBox &box, DrawResult &result);
  bool grant(const DrawBoxEntry &entry, int quantity, DrawResult &result) const;

  ItemUseServices services_;
  ItemUsePersistence persistence_;
  std::unique_ptr<LootRandom> random_;
  QuestEvents quest_events_;
};

} // namespace chronicles
