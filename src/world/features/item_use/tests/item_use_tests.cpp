#include "world/features/item_use/item_use.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr int kBox = 205;
constexpr int kBoxId = 50;
constexpr int kFood = 90101003;
constexpr int kCraftMaterial = 60007;
constexpr int kSword = 11102302;
constexpr int kChair = 49001;

// Returns scripted picks: weighted indexes and quantities in the order they are drawn.
class ScriptedRandom final : public LootRandom {
public:
  std::deque<std::size_t> indexes;
  std::deque<int> quantities;

  bool passes(double) override {
    return true;
  }

  std::size_t pick_index(const std::vector<int> &) override {
    const auto value = indexes.front();
    indexes.pop_front();
    return value;
  }

  int pick_quantity(int minimum, int maximum) override {
    if (quantities.empty()) {
      return minimum;
    }
    const auto value = std::clamp(quantities.front(), minimum, maximum);
    quantities.pop_front();
    return value;
  }
};

std::shared_ptr<const ShopCatalog> item_definitions() {
  ShopCatalogTables tables;
  tables.items = {{{"id", std::to_string(kBox)}, {"category", "Consumable"}, {"grade", "Normal"}},
                  {{"id", std::to_string(kFood)}, {"category", "Consumable"}, {"grade", "Normal"}},
                  {{"id", std::to_string(kCraftMaterial)},
                   {"category", "CraftMaterial"},
                   {"grade", "Normal"}},
                  {{"id", std::to_string(kSword)},
                   {"category", "Equipment"},
                   {"grade", "Rare"},
                   {"maxDurability", "10000"}},
                  {{"id", std::to_string(kChair)}, {"category", "HousingObject"}, {"grade", "Normal"}}};
  tables.game_variables = {{{"name", "ItemMaxStack"}, {"value", "1000"}},
                           {{"name", "ResetBaseDateTime"}, {"value", "2023-01-02 05:00:00"}}};
  tables.money = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  return std::make_shared<const ShopCatalog>(tables);
}

// Box 50 of the catalog, shortened: gold and one material always, one of three weighted rows.
std::shared_ptr<const ItemUseCatalog> box_catalog() {
  const std::vector<Json> items{
      {{"id", std::to_string(kBox)}, {"FK_drawBoxId", std::to_string(kBoxId)}},
      {{"id", std::to_string(kFood)}, {"FK_drawBoxId", ""}}};
  const std::vector<Json> rows{
      {{"drawBoxId", "50"}, {"itemId", ""}, {"moneyType", "Gold"}, {"minAmount", "50000"},
       {"maxAmount", "50000"}, {"drawWeight", ""}},
      {{"drawBoxId", "50"}, {"itemId", std::to_string(kCraftMaterial)}, {"moneyType", ""},
       {"minAmount", "6"}, {"maxAmount", "6"}, {"drawWeight", ""}},
      {{"drawBoxId", "50"}, {"itemId", std::to_string(kSword)}, {"moneyType", ""},
       {"minAmount", "1"}, {"maxAmount", "1"}, {"drawWeight", "300"}},
      {{"drawBoxId", "50"}, {"itemId", std::to_string(kChair)}, {"moneyType", ""},
       {"minAmount", "1"}, {"maxAmount", "2"}, {"drawWeight", "300"}},
      {{"drawBoxId", "50"}, {"itemId", ""}, {"moneyType", "HersanderToken"}, {"minAmount", "3"},
       {"maxAmount", "3"}, {"drawWeight", "300"}}};
  return std::make_shared<const ItemUseCatalog>(items, rows);
}

// One character's storage in memory; a commit fails when `refuse` is set.
struct TestStore {
  Character character;
  WorldProgress progress;
  std::vector<CharacterItem> granted;
  bool refuse{};
  std::int64_t next_item_id{100};

  ItemUsePersistence persistence() {
    ItemUsePersistence result;
    result.load_character = [this] {
      return std::optional<Character>(character);
    };
    result.load_progress = [this] {
      return progress;
    };
    result.commit = [this](const WorldProgress &before, const WorldProgress &after,
                           const std::vector<CharacterItem> &items)
        -> std::optional<std::vector<CharacterItem>> {
      if (refuse || before.revision != progress.revision) {
        return std::nullopt;
      }
      progress = after;
      std::vector<CharacterItem> stored;
      for (auto item : items) {
        item.id = next_item_id++;
        stored.push_back(item);
        character.items.push_back(item);
      }
      granted.insert(granted.end(), stored.begin(), stored.end());
      return stored;
    };
    return result;
  }
};

struct ItemUseFixture {
  ItemUseFixture() {
    auto scripted = std::make_unique<ScriptedRandom>();
    random = scripted.get();
    ItemUseServices services{box_catalog(), item_definitions(), {}};
    services.effects.effect_list = [this](int item_cms_id) -> const std::vector<int> * {
      return item_cms_id == kFood ? &food_effects : nullptr;
    };
    services.effects.ready = [this] {
      return !user_dead;
    };
    services.effects.apply = [this](const std::vector<int> &effect_ids) {
      applied.push_back(effect_ids);
      return std::vector<GameplayPacket>{{49, {{"status_effect", {{"status_effect_id", 7000}}}}}};
    };
    session = std::make_unique<ItemUseSession>(std::move(services), store.persistence(),
                                               std::move(scripted));
  }

  std::vector<GameplayPacket> use(int item_cms_id) {
    const Json request{{"item_cms_id", item_cms_id}, {"count", 1}};
    EXPECT_TRUE(session->handles(request));
    return session->receive(request, 9);
  }

  TestStore store;
  ScriptedRandom *random{};
  std::vector<int> food_effects{7000};
  std::vector<std::vector<int>> applied;
  bool user_dead{};
  std::unique_ptr<ItemUseSession> session;
};

std::string error_of(const std::vector<GameplayPacket> &packets) {
  if (packets.size() == 1 && packets.front().opcode == 2) {
    return packets.front().message.at("ec").get<std::string>();
  }
  return "none";
}

} // namespace

TEST(ItemUse, CatalogSplitsGuaranteedAndWeightedRowsAndRejectsDanglingBoxes) {
  const auto catalog = box_catalog();
  const auto *box = catalog->draw_box_for_item(kBox);
  ASSERT_NE(box, nullptr);
  EXPECT_EQ(box->guaranteed.size(), 2u);
  EXPECT_EQ(box->weighted.size(), 3u);
  EXPECT_EQ(catalog->draw_box_for_item(kFood), nullptr);

  const std::vector<Json> dangling{{{"id", "1"}, {"FK_drawBoxId", "999"}}};
  EXPECT_THROW(ItemUseCatalog(dangling, {}), std::runtime_error);
  const std::vector<Json> bad_money{{{"drawBoxId", "1"}, {"moneyType", "Seashells"},
                                     {"minAmount", "1"}, {"maxAmount", "1"}}};
  EXPECT_THROW(ItemUseCatalog({}, bad_money), std::runtime_error);
}

TEST(ItemUse, CatalogWithoutTheRecipeTableKeepsOtherConsumablesAndLeavesRecipesUnusable) {
  const std::vector<Json> items{
      {{"id", "1234"}, {"category", "CraftRecipe"}, {"FK_itemCraftId", "5678"}}};
  const ItemUseCatalog without_crafts(items, {});
  EXPECT_FALSE(without_crafts.recipe_for_item(1234));

  // With the table present, the same dangling recipe reference is a catalog error.
  EXPECT_THROW(ItemUseCatalog(items, {}, std::vector<Json>{}), std::runtime_error);
}

TEST(ItemUse, RecipeLearningIsAtomicAndReplaySafeAcrossReconnect) {
  constexpr int recipe_item = 60203031;
  constexpr int craft_id = 60203031;
  const auto catalog = std::make_shared<const ItemUseCatalog>(
      std::vector<Json>{{{"id", std::to_string(recipe_item)},
                         {"category", "CraftRecipe"},
                         {"FK_itemCraftId", std::to_string(craft_id)}}},
      std::vector<Json>{},
      std::vector<Json>{{{"craftId", std::to_string(craft_id)}, {"autoRegistration", "False"}}});
  TestStore store;
  store.progress.stackables[recipe_item] = 2;
  const auto connect = [&] {
    return ItemUseSession({catalog, item_definitions(), {}}, store.persistence(),
                          std::make_unique<SeededLootRandom>(1));
  };
  const Json request{{"item_cms_id", recipe_item}, {"count", 1}, {"packet_seq", {{"seq", "19"}}}};
  auto session = connect();
  ASSERT_TRUE(session.handles(request));

  store.refuse = true;
  EXPECT_EQ(error_of(session.receive(request, 19)), "ItemError");
  EXPECT_EQ(store.progress.stackables.at(recipe_item), 2);
  EXPECT_TRUE(store.progress.learned_crafts.empty());
  store.refuse = false;

  const auto learned = session.receive(request, 19);
  ASSERT_EQ(learned.size(), 2u);
  EXPECT_EQ(learned[0].opcode, 169);
  EXPECT_EQ(learned[1].opcode, 132);
  EXPECT_EQ(learned[1].message, (Json{{"update", true}, {"craft_ids", {craft_id}}}));
  EXPECT_EQ(store.progress.stackables.at(recipe_item), 1);
  EXPECT_TRUE(store.progress.learned_crafts.contains(craft_id));
  EXPECT_EQ(store.progress.revision, 1);

  auto reconnected = connect();
  EXPECT_EQ(error_of(reconnected.receive(request, 19)), "Craft_SkillAlreadyLearn");
  EXPECT_EQ(store.progress.stackables.at(recipe_item), 1);
  EXPECT_EQ(store.progress.revision, 1);
}

TEST(ItemUse, RecipeOwnershipAndAutomaticUnlocksCannotConsumeAnExtraItem) {
  constexpr int recipe_item = 1234;
  const auto make_catalog = [](bool automatic) {
    return std::make_shared<const ItemUseCatalog>(
        std::vector<Json>{
            {{"id", "1234"}, {"category", "CraftRecipe"}, {"FK_itemCraftId", "5678"}}},
        std::vector<Json>{},
        std::vector<Json>{
            {{"craftId", "5678"}, {"autoRegistration", automatic ? "True" : "False"}}});
  };
  TestStore store;
  ItemUseSession session({make_catalog(false), item_definitions(), {}}, store.persistence(),
                         std::make_unique<SeededLootRandom>(1));
  Json request{{"item_cms_id", recipe_item}, {"count", 1}};
  EXPECT_EQ(error_of(session.receive(request, 1)), "ItemCountError");
  store.progress.stackables[recipe_item] = 1;
  request["count"] = 2;
  EXPECT_EQ(error_of(session.receive(request, 1)), "BadRequest");
  request["count"] = 1;
  request["target_unit_id"] = "123";
  EXPECT_EQ(error_of(session.receive(request, 1)), "BadRequest");
  request.erase("target_unit_id");
  store.progress.dead = true;
  EXPECT_EQ(error_of(session.receive(request, 1)), "InvalidState");
  store.progress.dead = false;
  ItemUseSession automatic({make_catalog(true), item_definitions(), {}}, store.persistence(),
                           std::make_unique<SeededLootRandom>(1));
  EXPECT_EQ(error_of(automatic.receive(request, 1)), "Craft_SkillAlreadyLearn");
  EXPECT_EQ(store.progress.stackables.at(recipe_item), 1);
  EXPECT_EQ(store.progress.revision, 0);
}

TEST(ItemUse, OpeningABoxSpendsItAndGrantsItsGuaranteedRowsAndOneWeightedPick) {
  ItemUseFixture fixture;
  fixture.store.progress.stackables[kBox] = 2;
  fixture.random->indexes = {0};

  const auto packets = fixture.use(kBox);
  ASSERT_EQ(packets.size(), 1u);
  const auto &update = packets.front().message;
  EXPECT_EQ(packets.front().opcode, 169);
  EXPECT_TRUE(update.contains("use"));
  EXPECT_EQ(update.at("stackables").at(0), (Json{{"item_cms_id", kBox}, {"quantity", 1}}));
  EXPECT_EQ(update.at("stackables").at(1),
            (Json{{"item_cms_id", kCraftMaterial}, {"quantity", 6}}));
  EXPECT_EQ(update.at("money_changes").at(0), (Json{{"change", "50000"}, {"result", "50000"}}));
  ASSERT_EQ(update.at("items").size(), 1u);
  EXPECT_EQ(update.at("items").at(0).at("item_cms_id"), kSword);
  EXPECT_EQ(update.at("items").at(0).at("durability"), 10000);

  EXPECT_EQ(fixture.store.progress.stackables.at(kBox), 1);
  EXPECT_EQ(fixture.store.progress.gold, 50000);
  ASSERT_EQ(fixture.store.granted.size(), 1u);
  EXPECT_EQ(fixture.store.granted.front().cms_id, kSword);
}

TEST(ItemUse, WeightedHousingAndTokenRewardsUseTheirOwnStorage) {
  ItemUseFixture fixture;
  fixture.store.progress.stackables[kBox] = 2;
  fixture.random->indexes = {1, 2};
  fixture.random->quantities = {50000, 6, 2};

  const auto chair = fixture.use(kBox).front().message;
  EXPECT_EQ(chair.at("housing_items").at(0).at("item_cms_id"), kChair);
  EXPECT_EQ(chair.at("housing_items").at(0).at("quantity"), 2);
  EXPECT_EQ(fixture.store.progress.housing_items.at(kChair).quantity, 2);

  const auto token = fixture.use(kBox).front().message;
  ASSERT_EQ(token.at("money_changes").size(), 2u);
  EXPECT_EQ(token.at("money_changes").at(1),
            (Json{{"change", "3"}, {"result", "3"}, {"money_type", "EX5MoneyType_HersanderToken"}}));
  EXPECT_EQ(fixture.store.progress.hersander_token, 3);
  EXPECT_EQ(fixture.store.progress.stackables.at(kBox), 0);
}

TEST(ItemUse, RefusalsChangeNothing) {
  ItemUseFixture fixture;
  EXPECT_EQ(error_of(fixture.use(kBox)), "ItemCountError");

  fixture.store.progress.stackables[kBox] = 1;
  fixture.store.refuse = true;
  fixture.random->indexes = {0};
  EXPECT_EQ(error_of(fixture.use(kBox)), "ItemError");
  EXPECT_EQ(fixture.store.progress.stackables.at(kBox), 1);
  EXPECT_TRUE(fixture.store.granted.empty());

  fixture.store.refuse = false;
  fixture.store.progress.dead = true;
  EXPECT_EQ(error_of(fixture.use(kBox)), "InvalidState");

  const Json instance_request{{"item_cms_id", kBox}, {"item_id", "77"}};
  EXPECT_EQ(error_of(fixture.session->receive(instance_request, 1)), "ItemIdInvalid");
  EXPECT_FALSE(fixture.session->handles(Json{{"item_cms_id", 4242}}));
}

TEST(ItemUse, EffectConsumableIsSpentBeforeItsEffectsAndKeptWhileTheUserIsDead) {
  ItemUseFixture fixture;
  fixture.store.progress.stackables[kFood] = 2;

  const auto packets = fixture.use(kFood);
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets.at(0).opcode, 169);
  EXPECT_EQ(packets.at(0).message.at("stackables").at(0),
            (Json{{"item_cms_id", kFood}, {"quantity", 1}}));
  EXPECT_EQ(packets.at(1).opcode, 49);
  ASSERT_EQ(fixture.applied.size(), 1u);
  EXPECT_EQ(fixture.applied.front(), std::vector<int>{7000});

  fixture.user_dead = true;
  EXPECT_EQ(error_of(fixture.use(kFood)), "InvalidState");
  EXPECT_EQ(fixture.store.progress.stackables.at(kFood), 1);
  EXPECT_EQ(fixture.applied.size(), 1u);
}

TEST(ItemUse, SpendingAReservedUnitLowersTheUtilitySlotAndAnnouncesTheBag) {
  ItemUseFixture fixture;
  fixture.store.progress.stackables[kFood] = 2;
  fixture.store.progress.utility_slots[2] = {kFood, 2};

  const auto packets = fixture.use(kFood);
  ASSERT_EQ(packets.size(), 3u);
  EXPECT_EQ(packets.at(0).opcode, 169);
  EXPECT_EQ(packets.at(1).opcode, 104);
  EXPECT_EQ(packets.at(1).message.at("bag").at("item_cms_id3"), kFood);
  EXPECT_EQ(packets.at(1).message.at("bag").at("count3"), 1);
  EXPECT_EQ(packets.at(2).opcode, 49);
  EXPECT_EQ(fixture.store.progress.utility_slots[2], (UtilitySlot{kFood, 1}));

  // An unreserved unit leaves the bag and its packet out.
  fixture.store.progress.stackables[kFood] = 3;
  const auto unreserved = fixture.use(kFood);
  ASSERT_EQ(unreserved.size(), 2u);
  EXPECT_EQ(unreserved.at(1).opcode, 49);
  EXPECT_EQ(fixture.store.progress.utility_slots[2], (UtilitySlot{kFood, 1}));
}

// A quest objective's use item (waypoint stone 10140 for quest 20001004) has no other use: it is
// spent once per request and reported to the quest service, like every committed use.
TEST(ItemUse, QuestUseItemsAreSpentAndEveryCommittedUseIsReportedForQuests) {
  constexpr int waypoint_stone = 10140;
  TestStore store;
  store.progress.stackables[waypoint_stone] = 2;
  ItemUseServices services{box_catalog(), item_definitions(), {}, {}};
  services.quest_use_item = [](int item_cms_id) {
    return item_cms_id == waypoint_stone;
  };
  ItemUseSession session(std::move(services), store.persistence(),
                         std::make_unique<SeededLootRandom>(1));

  const Json request{{"item_cms_id", waypoint_stone}, {"count", 1}};
  ASSERT_TRUE(session.handles(request));
  const auto used = session.receive(request, 4);
  ASSERT_FALSE(used.empty());
  EXPECT_EQ(used.front().opcode, 169);
  EXPECT_EQ(store.progress.stackables.at(waypoint_stone), 1);
  EXPECT_EQ(session.take_quest_events(),
            (QuestEvents{{QuestEventKind::ItemUsed, waypoint_stone, 0, 1}}));
  EXPECT_TRUE(session.take_quest_events().empty());

  // A refused use (nothing left) reports nothing.
  store.progress.stackables[waypoint_stone] = 0;
  EXPECT_EQ(error_of(session.receive(request, 5)), "ItemCountError");
  EXPECT_TRUE(session.take_quest_events().empty());
  EXPECT_FALSE(session.handles({{"item_cms_id", waypoint_stone + 1}, {"count", 1}}));
}

} // namespace chronicles
