#include "world/session/graduation_grant.hpp"

#include "auth/auth.hpp"
#include "data/client_catalog.hpp"
#include "data/shop_catalog.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/character/progression.hpp"
#include "world/features/gm/gm_commands.hpp"
#include "world/features/inventory/inventory_encumbrance.hpp"
#include "world/features/quests/world_quests.hpp"
#include "world/session/native_world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr int kItemMaxStack = 1000;
constexpr std::int64_t kGoldLimit = 5'000'000;

constexpr int kFightHelmet = 1001;
constexpr int kFightHelmetWeaker = 1002;
constexpr int kFightSword = 1003;
constexpr int kMagicStaff = 1004;
constexpr int kNeutralNecklace = 1005;
constexpr int kLockedWeapon = 1006;
constexpr int kHighTierSword = 1007;
constexpr int kCostumeBase = 2000;

Json equipment_row(int item_cms_id, const std::string &class_name, const std::string &slot,
                   int level, const std::string &grade, int required_level = 0) {
  return {{"id", std::to_string(item_cms_id)},
          {"category", "Equipment"},
          {"class", class_name},
          {"equipSlot", slot},
          {"level", std::to_string(level)},
          {"levelReq", std::to_string(required_level)},
          {"grade", grade},
          {"priceType", "Gold"},
          {"priceValue", "1"},
          {"refundType", "Gold"},
          {"refundValue", "1"},
          {"maxDurability", "1000"}};
}

Json consumable_row(int item_cms_id) {
  return {{"id", std::to_string(item_cms_id)}, {"category", "Consumable"}, {"grade", "Normal"},
          {"priceType", "Gold"},               {"priceValue", "1"},        {"refundType", "Gold"},
          {"refundValue", "1"}};
}

std::vector<Json> item_rows() {
  std::vector<Json> rows = {
      equipment_row(kFightHelmet, "Fight", "ArmorHelmet", 80, "Legendary"),
      equipment_row(kFightHelmetWeaker, "Fight", "ArmorHelmet", 60, "Heroic"),
      equipment_row(kFightSword, "Fight", "Weapon_1", 80, "Legendary"),
      equipment_row(kMagicStaff, "Magic", "Weapon_1", 80, "Legendary"),
      equipment_row(kNeutralNecklace, "None", "Necklace", 45, "Rare"),
      equipment_row(kLockedWeapon, "Fight", "Weapon_1", 80, "Legendary", 999),
      equipment_row(kHighTierSword, "Fight", "Weapon_2", 90, "Legendary"),
  };
  // Twelve equally good costumes: only a bounded number may enter one kit.
  for (int index = 0; index < 12; ++index) {
    rows.push_back(equipment_row(kCostumeBase + index, "Fight", "CostumeSuit", 80, "Legendary"));
  }
  return rows;
}

std::vector<int> ids_of(const std::vector<GmBundleItem> &bundle) {
  std::vector<int> ids;
  for (const auto &entry : bundle) {
    ids.push_back(entry.item_cms_id);
  }
  return ids;
}

bool contains(const std::vector<int> &ids, int id) {
  return std::ranges::find(ids, id) != ids.end();
}

const ShopCatalog &test_catalog() {
  static const ShopCatalog catalog([] {
    ShopCatalogTables tables;
    tables.items = item_rows();
    tables.items.push_back(consumable_row(kGraduationHealthPotionItemId));
    tables.items.push_back(consumable_row(kGraduationStaminaPotionItemId));
    tables.game_variables = {
        {{"name", "ItemMaxStack"}, {"value", std::to_string(kItemMaxStack)}},
        {{"name", "ResetBaseDateTime"}, {"value", "2023-01-02 05:00:00"}},
    };
    tables.money = {{{"type", "Gold"}, {"limitUserMax", std::to_string(kGoldLimit)}}};
    return tables;
  }());
  return catalog;
}

WorldProgress fresh_progress() {
  WorldProgress progress;
  progress.revision = 3;
  progress.gold = 10;
  return progress;
}

} // namespace

TEST(GraduationLoadout, KeepsTheBestPieceOfEachSlotForTheClass) {
  const GraduationLoadout loadout(item_rows(), 80);
  const auto ids = ids_of(loadout.bundle_for("Fight"));

  EXPECT_TRUE(contains(ids, kFightHelmet));
  EXPECT_FALSE(contains(ids, kFightHelmetWeaker));
  EXPECT_TRUE(contains(ids, kFightSword));
  EXPECT_TRUE(contains(ids, kNeutralNecklace)) << "class-neutral pieces are part of every kit";
  EXPECT_FALSE(contains(ids, kMagicStaff)) << "another class's weapon is not";
}

TEST(GraduationLoadout, LeavesOutPiecesNobodyCanEquip) {
  const GraduationLoadout loadout(item_rows(), 80);
  const auto ids = ids_of(loadout.bundle_for("Fight"));

  EXPECT_FALSE(contains(ids, kLockedWeapon)) << "requires level 999";
}

TEST(GraduationLoadout, KeepsPiecesWhoseTierExceedsTheCharacterLevelCap) {
  // Item.level is a tier, not a requirement: tier 90 gear is wearable at a lower level cap.
  const GraduationLoadout loadout(item_rows(), 40);
  const auto ids = ids_of(loadout.bundle_for("Fight"));

  EXPECT_TRUE(contains(ids, kHighTierSword));
  EXPECT_TRUE(contains(ids, kFightSword));
}

TEST(GraduationLoadout, BoundsTheLookAlikePiecesOfOneSlot) {
  const GraduationLoadout loadout(item_rows(), 80);
  const auto ids = ids_of(loadout.bundle_for("Fight"));

  const auto costumes = std::ranges::count_if(ids, [](int id) {
    return id >= kCostumeBase && id < kCostumeBase + 12;
  });
  EXPECT_EQ(static_cast<std::size_t>(costumes), kGraduationMaximumEquipmentPerSlot);
  EXPECT_TRUE(contains(ids, kCostumeBase)) << "lowest ids win, so the choice is stable";
  EXPECT_FALSE(contains(ids, kCostumeBase + 11));
}

TEST(GraduationLoadout, AddsTheConsumableStock) {
  const GraduationLoadout loadout(item_rows(), 80);
  const auto bundle = loadout.bundle_for("Magic");

  const auto potion = std::ranges::find(bundle, kGraduationHealthPotionItemId,
                                        &GmBundleItem::item_cms_id);
  ASSERT_NE(potion, bundle.end());
  EXPECT_EQ(potion->count, kGraduationHealthPotionCount);
  EXPECT_TRUE(contains(ids_of(bundle), kMagicStaff));
}

TEST(GraduationLoadout, NamesCharacterClassesLikeTheItemCatalog) {
  EXPECT_EQ(graduation_class_name(1), "Fight");
  EXPECT_EQ(graduation_class_name(2), "Wild");
  EXPECT_EQ(graduation_class_name(3), "Adamant");
  EXPECT_EQ(graduation_class_name(5), "Magic");
  EXPECT_TRUE(graduation_class_name(-4).empty());
}

TEST(GraduationBundle, GrantsEquipmentStacksAndGoldInOneTrade) {
  const GraduationLoadout loadout(item_rows(), 80);
  Character character;
  character.id = 9;
  const auto plan = plan_gm_grant_bundle(test_catalog(), loadout.bundle_for("Fight"),
                                         kGoldLimit, character, fresh_progress(),
                                         kGraduationMaximumEquipmentInstances);

  ASSERT_EQ(plan.error, x5::ec::Success);
  EXPECT_FALSE(plan.empty());
  EXPECT_EQ(plan.trade.next.revision, 4);
  EXPECT_EQ(plan.trade.next.gold, kGoldLimit);
  EXPECT_EQ(plan.trade.next.stackables.at(kGraduationHealthPotionItemId), kGraduationHealthPotionCount);

  std::vector<int> granted;
  for (const auto &item : plan.trade.granted_items) {
    granted.push_back(item.cms_id);
  }
  EXPECT_TRUE(contains(granted, kFightHelmet));
  EXPECT_TRUE(contains(granted, kFightSword));

  // The repairer is not in this catalog: it is left out instead of failing the whole kit.
  EXPECT_TRUE(contains(plan.skipped_item_ids, kGraduationRepairerItemId));
}

TEST(GraduationBundle, GivesEachGrantedPieceItsOwnInventorySlot) {
  const GraduationLoadout loadout(item_rows(), 80);
  Character character;
  const auto plan = plan_gm_grant_bundle(test_catalog(), loadout.bundle_for("Fight"),
                                         std::nullopt, character, fresh_progress(),
                                         kGraduationMaximumEquipmentInstances);

  ASSERT_EQ(plan.error, x5::ec::Success);
  std::vector<int> slots;
  for (const auto &item : plan.trade.granted_items) {
    slots.push_back(item.inventory_slot);
  }
  std::ranges::sort(slots);
  EXPECT_EQ(std::ranges::adjacent_find(slots), slots.end()) << "no two pieces share a slot";
}

TEST(GraduationBundle, RepeatingTheBundleChangesNothing) {
  const GraduationLoadout loadout(item_rows(), 80);
  Character character;
  const auto first = plan_gm_grant_bundle(test_catalog(), loadout.bundle_for("Fight"),
                                          kGoldLimit, character, fresh_progress(),
                                          kGraduationMaximumEquipmentInstances);
  ASSERT_EQ(first.error, x5::ec::Success);

  // Apply the first plan the way a commit would.
  std::int64_t next_id = 500;
  for (auto item : first.trade.granted_items) {
    item.id = next_id++;
    character.items.push_back(item);
  }
  const auto second = plan_gm_grant_bundle(test_catalog(), loadout.bundle_for("Fight"),
                                           kGoldLimit, character, first.trade.next,
                                           kGraduationMaximumEquipmentInstances);

  ASSERT_EQ(second.error, x5::ec::Success);
  EXPECT_TRUE(second.empty());
}

TEST(GraduationBundle, NeverLowersAStockOrGold) {
  const GraduationLoadout loadout(item_rows(), 80);
  Character character;
  auto before = fresh_progress();
  before.gold = kGoldLimit + 1;
  before.stackables[kGraduationHealthPotionItemId] = kGraduationHealthPotionCount + 50;

  const auto plan = plan_gm_grant_bundle(test_catalog(), loadout.bundle_for("Fight"),
                                         kGoldLimit, character, before,
                                         kGraduationMaximumEquipmentInstances);

  ASSERT_EQ(plan.error, x5::ec::Success);
  EXPECT_EQ(plan.trade.next.gold, before.gold);
  EXPECT_EQ(plan.trade.next.stackables.at(kGraduationHealthPotionItemId),
            kGraduationHealthPotionCount + 50);
}

TEST(GraduationBundle, CapsGoldAtTheCatalogLimit) {
  Character character;
  const auto plan = plan_gm_grant_bundle(test_catalog(), {}, kGoldLimit * 100, character,
                                         fresh_progress(), kGraduationMaximumEquipmentInstances);

  ASSERT_EQ(plan.error, x5::ec::Success);
  EXPECT_EQ(plan.trade.next.gold, kGoldLimit);
}

TEST(GraduationBundle, RefusesMoreEquipmentThanTheInstanceBound) {
  const GraduationLoadout loadout(item_rows(), 80);
  Character character;
  const auto plan = plan_gm_grant_bundle(test_catalog(), loadout.bundle_for("Fight"),
                                         std::nullopt, character, fresh_progress(), 2);

  EXPECT_EQ(plan.error, x5::ec::ItemCountError);
}

// ---------------------------------------------------------------------------------------------
// Real catalogs (skipped when the exported client data is absent)

class GraduationRealCatalog : public ::testing::Test {
protected:
  void SetUp() override {
    const char *configured = std::getenv("CHRONICLES_CATALOG_DIR");
    const std::filesystem::path root =
        configured ? std::filesystem::path(configured)
                   : std::filesystem::path("data/generated/catalog");
    const auto quest_path = root.parent_path() / "quests" / "catalog.json";
    if (!std::filesystem::exists(root / "manifest.json") || !std::filesystem::exists(quest_path)) {
      GTEST_SKIP() << "exported client catalog or quest catalog not present";
    }

    ClientCatalog catalog(root);
    capacity_.emplace(catalog);
    encumbrance_ = EncumbranceRules::load(catalog);
    services_.shop_catalog = ShopCatalog::load(catalog);
    services_.progression = Progression::load(catalog);
    services_.quest_catalog = QuestCatalog::load(quest_path);
    services_.loadout = std::make_shared<const GraduationLoadout>(
        catalog.rows("Item"), services_.progression->max_level());

    repositories_ = create_repositories();
    if (!repositories_.accounts) {
      GTEST_SKIP() << "the storage under test is not available";
    }
    auth_ = std::make_unique<AuthService>(repositories_);
  }

  // A new character of `class_id` on a fresh account.
  Handoff new_character(int class_id, const std::string &name, bool game_master = false) {
    const std::string account = name + "_account";
    EXPECT_TRUE(auth_->create_account(account, "graduation-fixture-password"));
    if (game_master) {
      EXPECT_TRUE(auth_->set_account_role(account, AccountRole::game_master));
    }

    const auto login = auth_->login(account, "graduation-fixture-password");
    x5::pb::authwebsocket::WorldEnterReq enter_request;
    enter_request.set_world_id(1);
    EXPECT_EQ(auth_->enter(login.access_token(), enter_request).error_code(), x5::ec::Success);

    x5::pb::ws::front::CharCreateReq create_request;
    create_request.set_slot(0);
    create_request.set_char_name(name);
    create_request.set_char_class(static_cast<x5::cms::EX5CharacterClass>(class_id));
    const auto created = auth_->create(login.access_token(), create_request);
    EXPECT_EQ(created.ec(), x5::ec::Success);

    x5::pb::ws::front::CharSelectReq select_request;
    select_request.set_char_id(created.char_().char_id());
    const auto selected = auth_->select(login.access_token(), select_request);
    auto handoff = auth_->consume_ticket(selected.char_token());
    EXPECT_TRUE(handoff.has_value());
    return *handoff;
  }

  // Repositories under test: memory by default, PostgreSQL in the derived fixture.
  virtual CharacterRepositories create_repositories() {
    return make_memory_character_repositories(std::make_shared<MemoryDatabase>());
  }

  CharacterRepositories repositories_;
  std::unique_ptr<AuthService> auth_;
  GraduationServices services_;
  std::optional<InventoryCapacity> capacity_;
  std::shared_ptr<const EncumbranceRules> encumbrance_;

  void check_each_class();
  void check_applies_once_and_never_to_game_masters();
};

void GraduationRealCatalog::check_each_class() {
  for (const int class_id : {1, 2, 3, 5}) {
    const auto name = "Grad" + std::to_string(class_id);
    const auto handoff = new_character(class_id, name);

    const auto result = apply_graduation_grant(*auth_, handoff, services_);
    ASSERT_TRUE(result.applied) << name;
    EXPECT_TRUE(result.level_raised) << name;
    EXPECT_GT(result.equipment_granted, 5U) << name;
    EXPECT_GT(result.quests_completed, 500) << name;

    const auto progress = auth_->world_progress(handoff);
    EXPECT_EQ(progress.level, services_.progression->max_level()) << name;
    EXPECT_GT(progress.level_up_points, 0) << name;
    EXPECT_GT(progress.weapon_points, 0) << name;
    EXPECT_EQ(progress.gold, services_.shop_catalog->gold_limit()) << name;
    EXPECT_EQ(progress.stackables.at(kGraduationHealthPotionItemId), kGraduationHealthPotionCount);

    const auto character = auth_->world_character(handoff);
    ASSERT_TRUE(character) << name;
    EXPECT_EQ(character->level, services_.progression->max_level()) << name;
    // The character also keeps its starting items.
    EXPECT_GE(character->items.size(), result.equipment_granted) << name;

    // A kit that left the player too heavy to move would defeat its purpose.
    const auto carried = capacity_->measure(*character, progress);
    EXPECT_EQ(encumbrance_->classify(carried.total, carried.safe), EncumbranceLevel::none)
        << name << " carries " << carried.total << " of " << carried.safe << " safe weight";
  }
}

void GraduationRealCatalog::check_applies_once_and_never_to_game_masters() {
  const auto player = new_character(1, "GradOnce");
  EXPECT_TRUE(apply_graduation_grant(*auth_, player, services_).applied);

  // At the level cap the character is never granted again, so nothing is duplicated.
  const auto items_after_first = auth_->world_character(player)->items.size();
  EXPECT_FALSE(apply_graduation_grant(*auth_, player, services_).applied);
  EXPECT_EQ(auth_->world_character(player)->items.size(), items_after_first);

  const auto keeper = new_character(1, "GradKeeper", true);
  EXPECT_FALSE(apply_graduation_grant(*auth_, keeper, services_).applied);
  EXPECT_EQ(auth_->world_progress(keeper).level, 1);
}

TEST_F(GraduationRealCatalog, LevelsGearsAndCompletesEverythingForEachClass) {
  check_each_class();
}

TEST_F(GraduationRealCatalog, AppliesOnceAndNeverToGameMasters) {
  check_applies_once_and_never_to_game_masters();
}

// The same checks against PostgreSQL, which production uses. Needs a disposable cluster whose
// database was migrated to the current schema (never the development database).
class GraduationRealCatalogPostgres : public GraduationRealCatalog {
protected:
  CharacterRepositories create_repositories() override {
    const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
    if (!connection) {
      return {};
    }
    return make_postgres_character_repositories(connection);
  }
};

TEST_F(GraduationRealCatalogPostgres, LevelsGearsAndCompletesEverythingForEachClass) {
  check_each_class();
}

TEST_F(GraduationRealCatalogPostgres, AppliesOnceAndNeverToGameMasters) {
  check_applies_once_and_never_to_game_masters();
}

} // namespace chronicles