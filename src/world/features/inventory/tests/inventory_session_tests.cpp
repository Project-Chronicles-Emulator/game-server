#include "world/features/inventory/inventory_feature.hpp"

#include "cms.generated.pb.h"
#include "data/client_catalog.hpp"
#include "testing/catalog_test_support.hpp"
#include "world/features/inventory/tests/encumbrance_test_support.hpp"
#include "world/features/inventory/utility_bag.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>

namespace chronicles::inventory {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t kCharacterId = 17;
constexpr std::uint64_t kPlayerUnitId = 100017;
constexpr int kMaterial = 3001;
constexpr std::uint16_t kEquipRequest = 20074;
constexpr std::uint16_t kDiscardRequest = 20072;
constexpr std::uint16_t kPolishRequest = 20075;
constexpr std::uint16_t kTimeSyncRequest = 20001;

class InventoryProgress final : public ProgressAccess {
public:
  WorldProgress load() override {
    ++loads;
    return progress;
  }

  std::optional<Character> character() override {
    ++character_loads;
    return selected ? std::optional<Character>(player) : std::nullopt;
  }

  std::optional<std::vector<CharacterItem>> commit(const WorldProgress &, const WorldProgress &,
                                                   const ItemTrade &) override {
    ADD_FAILURE() << "Inventory retains the specific atomic equip/discard/polish commands";
    return std::nullopt;
  }

  Character player;
  WorldProgress progress;
  bool selected{true};
  int loads{};
  int character_loads{};
};

class InventoryContext final : public InventorySessionContext {
public:
  explicit InventoryContext(InventoryProgress &storage) : storage_(storage) {
  }

  bool inventory_has_handoff() const override {
    return authenticated;
  }

  std::int64_t inventory_character_id() const override {
    return kCharacterId;
  }

  std::uint64_t inventory_connection_id() const override {
    return 71;
  }

  std::uint64_t inventory_player_unit_id(std::int64_t character_id) const override {
    EXPECT_EQ(character_id, kCharacterId);
    return kPlayerUnitId;
  }

  std::int64_t inventory_monotonic_ticks() const override {
    return 12345;
  }

  bool inventory_has_tutorial() const override {
    return tutorial;
  }

  const WorldProgress *inventory_tutorial_progress() const override {
    return tutorial ? &tutorial_progress : nullptr;
  }

  bool inventory_has_character_stats() const override {
    return stats_available;
  }

  Json inventory_character_stats(const Character &character) const override {
    order.push_back("prepare");
    ++stats_calls;
    if (fail_stats) {
      throw std::runtime_error("stat calculation failed");
    }
    const int step = character.items.empty() ? 0 : character.items.front().polish_step;
    Json result{{"18", 1000}, {"19", 1000}, {"69", 700}, {"84", 1000}, {"20", 100 + step}};
    last_prepared_object = result.get_ptr<const Json::object_t *>();
    return result;
  }

  bool inventory_has_combat() const override {
    return combat;
  }

  bool inventory_player_in_combat() const override {
    return in_combat;
  }

  bool inventory_action_busy() const override {
    return busy;
  }

  const Json *inventory_combat_stats() const override {
    return combat ? &combat_stats : nullptr;
  }

  const Json &inventory_update_combat_stats(Json stats) override {
    order.push_back("combat");
    ++combat_updates;
    if (expect_prepared_move) {
      EXPECT_EQ(stats.get_ptr<const Json::object_t *>(), last_prepared_object);
    }
    combat_stats = std::move(stats);
    return combat_stats;
  }

  bool inventory_update_items(std::int64_t revision,
                              const std::vector<CharacterItem> &mutations) override {
    order.push_back("commit");
    ++equipment_commits;
    if (fail_commit || revision != storage_.progress.revision) {
      return false;
    }
    for (const auto &mutation : mutations) {
      const auto item =
          std::ranges::find_if(storage_.player.items, [&](const CharacterItem &owned) {
            return owned.id == mutation.id;
          });
      if (item == storage_.player.items.end()) {
        return false;
      }
      *item = mutation;
    }
    ++storage_.progress.revision;
    return true;
  }

  bool inventory_discard_items(std::int64_t revision, const std::vector<std::int64_t> &item_ids,
                               const std::map<int, int> &counts) override {
    order.push_back("discard commit");
    ++discard_commits;
    if (fail_commit || revision != storage_.progress.revision) {
      return false;
    }
    std::erase_if(storage_.player.items, [&](const CharacterItem &item) {
      return std::ranges::find(item_ids, item.id) != item_ids.end();
    });
    for (const auto &[cms_id, count] : counts) {
      storage_.progress.stackables[cms_id] -= count;
    }
    ++storage_.progress.revision;
    return true;
  }

  bool inventory_apply_polish(std::int64_t revision, const ItemPolishChange &change) override {
    order.push_back("polish commit");
    ++polish_commits;
    if (fail_commit || revision != storage_.progress.revision) {
      return false;
    }
    const auto item = std::ranges::find_if(storage_.player.items, [&](const CharacterItem &owned) {
      return owned.id == change.item_id;
    });
    if (item == storage_.player.items.end()) {
      return false;
    }
    storage_.progress.gold -= change.gold_cost;
    storage_.progress.stackables[change.material_cms_id] -= change.material_count;
    if (change.outcome == PolishOutcome::success) {
      ++item->polish_step;
    } else if (change.outcome == PolishOutcome::destruction) {
      storage_.player.items.erase(item);
    }
    ++storage_.progress.revision;
    return true;
  }

  bool inventory_save_progress(const WorldProgress &before, const WorldProgress &after) override {
    order.push_back("progress commit");
    ++progress_commits;
    if (fail_commit || storage_.progress != before) {
      return false;
    }
    storage_.progress = after;
    storage_.player.build = after.build;
    return true;
  }

  void inventory_publish_equipment_revision(std::int64_t revision) override {
    order.push_back("tutorial revision");
    ++revision_publications;
    tutorial_progress.revision = revision;
  }

  void inventory_publish_world_loot_progress(const WorldProgress &progress) override {
    if (combat && world_mobs) {
      order.push_back("world loot cache");
      world_loot_progress = progress;
    }
  }

  void inventory_publish_weapon_selection(const WorldProgress &progress) override {
    if (tutorial) {
      tutorial_progress = progress;
    }
    inventory_publish_world_loot_progress(progress);
  }

  void inventory_publish_snapshot(const Character &character, const Json *prepared) override {
    order.push_back("snapshot");
    inventory_refresh_snapshot(character, prepared);
  }

  void inventory_refresh_snapshot(const Character &character, const Json *prepared) override {
    ++snapshots;
    snapshot_character = character;
    snapshot_stats = prepared ? std::optional<Json>(*prepared) : std::nullopt;
  }

  InventoryProgress &storage_;
  bool authenticated{true};
  bool tutorial{};
  bool stats_available{};
  bool combat{};
  bool in_combat{};
  bool busy{};
  bool world_mobs{};
  bool fail_commit{};
  bool fail_stats{};
  bool expect_prepared_move{};
  mutable int stats_calls{};
  mutable const Json::object_t *last_prepared_object{};
  mutable std::vector<std::string> order;
  int equipment_commits{};
  int discard_commits{};
  int polish_commits{};
  int progress_commits{};
  int combat_updates{};
  int revision_publications{};
  int snapshots{};
  WorldProgress tutorial_progress;
  WorldProgress world_loot_progress;
  Json combat_stats{{"18", 1000}, {"19", 1000}, {"69", 700}, {"84", 1000}};
  Character snapshot_character;
  std::optional<Json> snapshot_stats;
};

std::map<std::string, std::vector<Json>> catalog_tables() {
  const auto encumbrance = inventory_testing::catalog_rows();
  return {
      {"GameVariable", encumbrance.game_variables},
      {"StatusEffect", encumbrance.status_effects},
      {"MaintainEffect", encumbrance.maintain_effects},
      {"MaintainStat", encumbrance.maintain_stats},
      {"Stat", encumbrance.stats},
      {"Item",
       {{{"id", "1001"},
         {"category", "Equipment"},
         {"grade", "Normal"},
         {"equipSlot", "Weapon_1"},
         {"FK_itemAttribute", "1001"},
         {"class", "None"},
         {"levelReq", "0"},
         {"discardable", "True"},
         {"weight", "1"}},
        {{"id", "1002"},
         {"category", "Equipment"},
         {"grade", "Normal"},
         {"equipSlot", "Weapon_1"},
         {"FK_itemAttribute", "1001"},
         {"class", "None"},
         {"levelReq", "0"},
         {"discardable", "True"},
         {"weight", "60000"}},
        {{"id", "3001"},
         {"discardable", "True"},
         {"FK_utilityBagFullFillCnt", "1000"},
         {"weight", "1"}}}},
      {"ItemAttribute", {{{"id", "1001"}, {"FK_itemPolishIncreaseValueId", "fixture"}}}},
      {"ItemPolishIncreaseValue", {{{"id", "fixture"}, {"polishStep", "1"}}}},
      {"ItemPolish",
       {{{"grade", "Normal"},
         {"equipCategory", "Weapon"},
         {"polishStep", "1"},
         {"successProbability", "10000"},
         {"failProbability", "0"},
         {"destructionProbability", "0"},
         {"materialId01", "3001"},
         {"materialCnt01", "1"},
         {"costType", "Gold"},
         {"costValue", "10"}}}},
  };
}

class InventorySessionTest : public testing::Test {
protected:
  InventorySessionTest()
      : catalog_fixture_(catalog_tables()),
        catalog_(std::make_shared<ClientCatalog>(catalog_fixture_.root())), feature_(catalog_),
        context_(progress_), session_(feature_.create_session(progress_, context_)) {
    progress_.player.id = kCharacterId;
    progress_.player.name = "Selected";
    progress_.player.level = 8;
    progress_.player.class_id = 1;
    progress_.player.items = {
        CharacterItem{1, 1001, 0, x5::cms::EX5EquipSlotType_Weapon_1, 1, 10000, 0},
        CharacterItem{2, 1002, 1, std::nullopt, 1, 10000, 0},
    };
    progress_.progress.revision = 4;
    progress_.progress.gold = 100;
    progress_.progress.stackables[kMaterial] = 3;
    context_.tutorial_progress = progress_.progress;
  }

  FrameContext frame(std::uint16_t opcode, Json message) {
    return FrameContext(opcode, WorldGameplayRequest{opcode, 41, std::move(message)});
  }

  testing_support::TestCatalog catalog_fixture_;
  std::shared_ptr<ClientCatalog> catalog_;
  InventoryFeature feature_;
  InventoryProgress progress_;
  InventoryContext context_;
  std::unique_ptr<InventorySession> session_;
};

TEST_F(InventorySessionTest, SkillItemConsumptionCommitsTheLastUnitBeforeNotifying) {
  progress_.progress.stackables[kMaterial] = 1;
  progress_.progress.utility_slots[0] = {kMaterial, 1};
  context_.combat = true;
  context_.world_mobs = true;

  const auto packets = session_->consume_skill_item(kMaterial);

  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets[0].opcode, 169);
  EXPECT_EQ(packets[0].message.at("stackables").at(0).at("quantity"), 0);
  EXPECT_EQ(session_->stack_quantity(kMaterial), 0);
  EXPECT_EQ(progress_.progress.utility_slots[0].count, 0);
  EXPECT_EQ(context_.world_loot_progress, progress_.progress);
  EXPECT_EQ(context_.order, (std::vector<std::string>{"progress commit", "world loot cache"}));
  EXPECT_THROW(session_->consume_skill_item(kMaterial), std::runtime_error);
  EXPECT_EQ(context_.progress_commits, 1);
}

TEST_F(InventorySessionTest, SkillItemPlanIsPreparedWithoutChangingInventory) {
  progress_.progress.utility_slots[0] = {kMaterial, 3};
  const auto before = progress_.progress;
  const auto plan = session_->prepare_skill_item_use("0123456789abcdef", kMaterial);
  EXPECT_EQ(progress_.progress, before);
  EXPECT_EQ(context_.progress_commits, 0);
  EXPECT_EQ(plan.before, before);
  EXPECT_EQ(plan.after.stackables.at(kMaterial), 2);
  EXPECT_EQ(plan.after.utility_slots[0].count, 2);
  EXPECT_EQ(Json::parse(plan.response_json).at("stackables").at(0).at("quantity"), 2);
}

TEST_F(InventorySessionTest, SkillItemPlanMatchesLegacyMultiSlotConsumptionAndPacket) {
  progress_.progress.utility_slots[0] = {kMaterial, 1};
  progress_.progress.utility_slots[1] = {kMaterial, 2};
  const auto plan = session_->prepare_skill_item_use("0123456789abcdef", kMaterial);

  const auto packets = session_->consume_skill_item(kMaterial);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(progress_.progress, plan.after);
  EXPECT_EQ(packets[0].message, Json::parse(plan.response_json));
  EXPECT_EQ(progress_.progress.utility_slots[0].count, 0);
  EXPECT_EQ(progress_.progress.utility_slots[1].count, 2);
}

TEST_F(InventorySessionTest, SkillItemPlanRejectsUnavailableAndExhaustedProgress) {
  const auto original = progress_.progress;
  EXPECT_THROW(session_->prepare_skill_item_use("0123456789abcdef", 9999), std::runtime_error);
  EXPECT_EQ(progress_.progress, original);

  progress_.progress.stackables[kMaterial] = 0;
  const auto empty = progress_.progress;
  EXPECT_THROW(session_->prepare_skill_item_use("0123456789abcdef", kMaterial), std::runtime_error);
  EXPECT_EQ(progress_.progress, empty);

  progress_.progress = original;
  progress_.progress.revision = (std::numeric_limits<std::int64_t>::max)();
  const auto exhausted = progress_.progress;
  EXPECT_THROW(session_->prepare_skill_item_use("0123456789abcdef", kMaterial),
               std::invalid_argument);
  EXPECT_THROW(session_->consume_skill_item(kMaterial), std::runtime_error);
  EXPECT_EQ(progress_.progress, exhausted);
  EXPECT_EQ(context_.progress_commits, 0);
}

TEST_F(InventorySessionTest, FailedSkillItemCommitKeepsInventoryAndQuickSlotsUnchanged) {
  progress_.progress.utility_slots[0] = {kMaterial, 3};
  const auto before = progress_.progress;
  context_.fail_commit = true;

  EXPECT_THROW(session_->consume_skill_item(kMaterial), std::runtime_error);

  EXPECT_EQ(progress_.progress, before);
  EXPECT_EQ(context_.order, (std::vector<std::string>{"progress commit"}));
  EXPECT_EQ(session_->stack_quantity(9999), 0);
}

TEST_F(InventorySessionTest, SecondWeaponSlotKeepsTheFirstWeaponEquipped) {
  auto request = frame(kEquipRequest, {{"item_id", "2"}, {"slot", "EX5EquipSlotType_Weapon_2"}});

  ASSERT_TRUE(session_->handle_equip(request));
  ASSERT_EQ(request.session_packets.size(), 2U);
  EXPECT_EQ(request.session_packets[0].opcode, 169);
  EXPECT_EQ(request.session_packets[1].opcode, 168);
  EXPECT_EQ(progress_.player.items[0].equip_slot, x5::cms::EX5EquipSlotType_Weapon_1);
  EXPECT_EQ(progress_.player.items[1].equip_slot, x5::cms::EX5EquipSlotType_Weapon_2);
  EXPECT_EQ(context_.equipment_commits, 1);
  EXPECT_EQ(request.session_packets[1].message.at("equipments").at(0).at("slot"), 1);
}

TEST_F(InventorySessionTest, MovingEquippedWeaponClearsItsPreviousVisualSlot) {
  auto request = frame(kEquipRequest, {{"item_id", "1"}, {"slot", "EX5EquipSlotType_Weapon_2"}});

  ASSERT_TRUE(session_->handle_equip(request));
  ASSERT_EQ(request.session_packets.size(), 2U);
  ASSERT_EQ(request.session_packets[1].opcode, 168);
  const auto &visual = request.session_packets[1].message.at("equipments");
  ASSERT_EQ(visual.size(), 2U);
  EXPECT_EQ(visual[0].at("slot"), 1);
  EXPECT_EQ(visual[1].at("slot"), 0);
  EXPECT_FALSE(visual[1].contains("item_cms_id"));
  EXPECT_EQ(progress_.player.items[0].equip_slot, x5::cms::EX5EquipSlotType_Weapon_2);
}

TEST_F(InventorySessionTest, WeaponSelectionCommitsAndSurvivesSessionRecreation) {
  progress_.player.items[1].equip_slot = 1;
  auto request = frame(20078, {{"slot", 1}});
  ASSERT_TRUE(session_->handle_equip(request));
  ASSERT_EQ(request.session_packets.size(), 1U);
  EXPECT_EQ(request.session_packets.front().opcode, 172);
  EXPECT_EQ(request.session_packets.front().message.at("slot"), 1);
  EXPECT_EQ(progress_.progress.build.active_weapon_slot, 1);
  EXPECT_EQ(context_.snapshot_character.build.active_weapon_slot, 1);
  EXPECT_EQ(progress_.progress.revision, 5);

  session_ = feature_.create_session(progress_, context_);
  auto duplicate = frame(20078, {{"slot", "EX5EquipSlotType_Weapon_2"}});
  ASSERT_TRUE(session_->handle_equip(duplicate));
  EXPECT_EQ(duplicate.session_packets.front().message.at("ec"), "Success");
  EXPECT_EQ(progress_.progress.revision, 5);
  auto first = frame(20078, Json::object());
  ASSERT_TRUE(session_->handle_equip(first));
  EXPECT_EQ(first.session_packets.front().message.at("slot"), 0);
  EXPECT_EQ(progress_.progress.build.active_weapon_slot, 0);
}

TEST_F(InventorySessionTest, WeaponSelectionRefusalRestoresCurrentSlotWithoutMutation) {
  for (const auto slot : {1, 2, -1}) {
    auto request = frame(20078, {{"slot", slot}});
    ASSERT_TRUE(session_->handle_equip(request));
    EXPECT_NE(request.session_packets.front().message.at("ec"), "Success");
    EXPECT_EQ(request.session_packets.front().message.at("slot"), 0);
  }
  progress_.player.items[1].equip_slot = 1;
  context_.fail_commit = true;
  auto conflict = frame(20078, {{"slot", 1}});
  ASSERT_TRUE(session_->handle_equip(conflict));
  EXPECT_EQ(conflict.session_packets.front().message.at("slot"), 0);
  EXPECT_EQ(progress_.progress.revision, 4);
  EXPECT_EQ(context_.snapshots, 0);
  EXPECT_EQ(context_.combat_updates, 0);
}

TEST_F(InventorySessionTest, WeaponSelectionUpdatesTutorialRevisionAndStatsAfterCommit) {
  progress_.player.items[1].equip_slot = 1;
  context_.tutorial = true;
  context_.stats_available = true;
  context_.combat = true;
  auto request = frame(20078, {{"slot", 1}});
  ASSERT_TRUE(session_->handle_equip(request));
  EXPECT_EQ(context_.tutorial_progress, progress_.progress);
  EXPECT_EQ(context_.tutorial_progress.build.active_weapon_slot, 1);
  EXPECT_EQ(context_.order, (std::vector<std::string>{"prepare", "prepare", "progress commit",
                                                      "combat", "snapshot"}));
  ASSERT_EQ(request.session_packets.size(), 2U);
  EXPECT_EQ(request.session_packets[1].opcode, 47);
}

TEST_F(InventorySessionTest, EquipMovesPreparedStatsAfterCommitAndPublishesTutorialRevisionFirst) {
  context_.stats_available = true;
  context_.combat = true;
  context_.tutorial = true;
  context_.expect_prepared_move = true;
  auto request = frame(kEquipRequest, {{"item_id", "2"}});
  ASSERT_TRUE(session_->handle_equip(request));
  ASSERT_EQ(request.session_packets.size(), 2U);
  EXPECT_EQ(request.session_packets[0].opcode, 169);
  EXPECT_EQ(request.session_packets[1].opcode, 168);
  EXPECT_EQ(context_.order, (std::vector<std::string>{"prepare", "commit", "tutorial revision",
                                                      "combat", "snapshot"}));
  EXPECT_EQ(context_.stats_calls, 1);
  EXPECT_EQ(progress_.loads, 0);
  EXPECT_EQ(context_.tutorial_progress.revision, 5);
  EXPECT_EQ(context_.snapshot_character.items[1].equip_slot, x5::cms::EX5EquipSlotType_Weapon_1);
}

TEST_F(InventorySessionTest, StaleEquipmentCommitPublishesNoRevisionStatsOrSnapshot) {
  context_.stats_available = true;
  context_.combat = true;
  context_.tutorial = true;
  context_.fail_commit = true;
  const auto before = progress_.player.items;
  auto request = frame(kEquipRequest, {{"item_id", "2"}});
  ASSERT_TRUE(session_->handle_equip(request));
  ASSERT_EQ(request.session_packets.size(), 1U);
  EXPECT_EQ(request.session_packets[0].message.at("ec"), "ItemError");
  EXPECT_EQ(progress_.player.items, before);
  EXPECT_EQ(context_.order, (std::vector<std::string>{"prepare", "commit"}));
  EXPECT_EQ(context_.tutorial_progress.revision, 4);
  EXPECT_EQ(context_.snapshots, 0);
}

TEST_F(InventorySessionTest, BareUnequipUsesAbsentVisualItemAndEmptySlotIsSilent) {
  auto request = frame(kEquipRequest, {{"slot", x5::cms::EX5EquipSlotType_Weapon_1}});
  ASSERT_TRUE(session_->handle_equip(request));
  ASSERT_EQ(request.session_packets.size(), 2U);
  EXPECT_FALSE(request.session_packets[0].message.at("items").at(0).contains("equip_slot"));
  EXPECT_FALSE(request.session_packets[1].message.at("equipments").at(0).contains("item_cms_id"));
  auto repeat = frame(kEquipRequest, {{"slot", x5::cms::EX5EquipSlotType_Weapon_1}});
  EXPECT_TRUE(session_->handle_equip(repeat));
  EXPECT_TRUE(repeat.session_packets.empty());
  EXPECT_EQ(context_.equipment_commits, 1);
}

TEST_F(InventorySessionTest, DiscardValidatesTheWholeBatchBeforeItsOneCommit) {
  const auto before = progress_.player.items;
  auto duplicate = frame(
      kDiscardRequest, {{"identity_list", Json::array({{{"item_id", "2"}}, {{"item_id", "2"}}})}});
  EXPECT_TRUE(session_->handle_discard(duplicate));
  EXPECT_EQ(context_.discard_commits, 0);
  EXPECT_EQ(progress_.player.items, before);
  auto accepted =
      frame(kDiscardRequest,
            {{"identity_list",
              Json::array({{{"item_id", "2"}},
                           {{"stackable", {{"item_cms_id", kMaterial}, {"count", 2}}}}})}});
  EXPECT_TRUE(session_->handle_discard(accepted));
  ASSERT_EQ(accepted.session_packets.size(), 1U);
  EXPECT_TRUE(accepted.session_packets[0].message.contains("discard"));
  EXPECT_EQ(context_.discard_commits, 1);
  EXPECT_EQ(progress_.player.items.size(), 1U);
  EXPECT_EQ(progress_.progress.stackables.at(kMaterial), 1);
}

TEST_F(InventorySessionTest, UtilityBagPublishesWorldCacheOnlyAfterSuccessfulRevisionCommit) {
  context_.combat = true;
  context_.world_mobs = true;
  auto request =
      frame(kUtilityBagSetRequestOpcode,
            {{"slots", Json::array({{{"slot", 0}, {"item_cms_id", kMaterial}, {"count", 2}}})}});
  EXPECT_TRUE(session_->handle_utility_bag(request));
  ASSERT_EQ(request.session_packets.size(), 1U);
  EXPECT_EQ(request.session_packets[0].opcode, kUtilityBagSetResponseOpcode);
  EXPECT_EQ(context_.order, (std::vector<std::string>{"progress commit", "world loot cache"}));
  EXPECT_EQ(context_.world_loot_progress, progress_.progress);
  EXPECT_EQ(progress_.progress.utility_slots[0], (UtilitySlot{kMaterial, 2}));
  auto unchanged = frame(kUtilityBagSetRequestOpcode, request.request()->message);
  EXPECT_TRUE(session_->handle_utility_bag(unchanged));
  EXPECT_EQ(context_.progress_commits, 1);
}

TEST_F(InventorySessionTest, UtilityBagCommitFailurePreservesTheOldCacheAndProgress) {
  context_.combat = true;
  context_.world_mobs = true;
  context_.fail_commit = true;
  const auto before = progress_.progress;
  auto request =
      frame(kUtilityBagSetRequestOpcode,
            {{"slots", Json::array({{{"slot", 0}, {"item_cms_id", kMaterial}, {"count", 2}}})}});
  EXPECT_TRUE(session_->handle_utility_bag(request));
  EXPECT_EQ(progress_.progress, before);
  EXPECT_EQ(context_.order, (std::vector<std::string>{"progress commit"}));
  EXPECT_EQ(request.session_packets[0].message.at("ec"), "BadRequest");
}

TEST_F(InventorySessionTest, UtilityBagCannotBeChangedWhileAnEnemyIsEngaged) {
  context_.in_combat = true;
  const auto before = progress_.progress;
  auto request =
      frame(kUtilityBagSetRequestOpcode,
            {{"slots", Json::array({{{"slot", 0}, {"item_cms_id", kMaterial}, {"count", 2}}})}});
  ASSERT_TRUE(session_->handle_utility_bag(request));
  ASSERT_EQ(request.session_packets.size(), 1U);
  EXPECT_EQ(request.session_packets.front().opcode, 2);
  EXPECT_EQ(request.session_packets.front().message.at("ec"), "UtilityBag_InCombat");
  EXPECT_EQ(progress_.progress, before);
  EXPECT_EQ(context_.progress_commits, 0);
  context_.in_combat = false;
  auto retry = frame(kUtilityBagSetRequestOpcode, request.request()->message);
  ASSERT_TRUE(session_->handle_utility_bag(retry));
  EXPECT_EQ(retry.session_packets.front().opcode, kUtilityBagSetResponseOpcode);
  EXPECT_EQ(context_.progress_commits, 1);
}

TEST_F(InventorySessionTest, UtilityBagRefillAndClearAreBothBlockedDuringCombat) {
  progress_.progress.utility_slots[0] = {kMaterial, 1};
  context_.in_combat = true;
  const auto before = progress_.progress;
  const std::vector<Json> changes{
      {{"slots", Json::array({{{"slot", 0}, {"item_cms_id", kMaterial}, {"count", 2}}})}},
      {{"slots", Json::array({{{"slot", 0}}})}},
  };
  for (const auto &change : changes) {
    auto request = frame(kUtilityBagSetRequestOpcode, change);
    ASSERT_TRUE(session_->handle_utility_bag(request));
    ASSERT_EQ(request.session_packets.size(), 1U);
    EXPECT_EQ(request.session_packets.front().message.at("ec"), "UtilityBag_InCombat");
    EXPECT_EQ(progress_.progress, before);
    EXPECT_EQ(context_.progress_commits, 0);
  }
}

TEST_F(InventorySessionTest, UtilitySnapshotKeepsOriginalTimeSyncPositionAndOneConnectionGuard) {
  progress_.progress.utility_slots[0] = {kMaterial, 2};
  auto unrelated = frame(20003, Json::object());
  session_->observe_utility_bag_snapshot(unrelated);
  EXPECT_TRUE(unrelated.session_packets.empty());
  auto first = frame(kTimeSyncRequest, Json::object());
  session_->observe_utility_bag_snapshot(first);
  ASSERT_EQ(first.session_packets.size(), 1U);
  EXPECT_EQ(first.session_packets[0].opcode, kWorldCharUtilityBagNotificationOpcode);
  auto repeat = frame(kTimeSyncRequest, Json::object());
  session_->observe_utility_bag_snapshot(repeat);
  EXPECT_TRUE(repeat.session_packets.empty());
}

TEST_F(InventorySessionTest, EquippedPolishKeepsPreparedCopyAndItemsStatsEquipmentOrder) {
  feature_.load_polish_catalog();
  context_.stats_available = true;
  context_.combat = true;
  auto request = frame(kPolishRequest, {{"item_id", "1"}});
  EXPECT_TRUE(session_->handle_polish(request));
  ASSERT_EQ(request.session_packets.size(), 3U);
  EXPECT_EQ(request.session_packets[0].opcode, 169);
  EXPECT_EQ(request.session_packets[1].opcode, 47);
  EXPECT_EQ(request.session_packets[2].opcode, 168);
  EXPECT_EQ(context_.order, (std::vector<std::string>{"prepare", "prepare", "polish commit",
                                                      "combat", "snapshot"}));
  EXPECT_EQ(context_.stats_calls, 2);
  EXPECT_EQ(progress_.player.items[0].polish_step, 1);
  EXPECT_EQ(progress_.progress.gold, 90);
}

TEST_F(InventorySessionTest, DamagedPolishRefusesBeforePreparationOrStorage) {
  feature_.load_polish_catalog();
  context_.stats_available = true;
  progress_.player.items[0].durability = 0;
  auto request = frame(kPolishRequest, {{"item_id", "1"}});
  EXPECT_TRUE(session_->handle_polish(request));
  ASSERT_EQ(request.session_packets.size(), 1U);
  EXPECT_EQ(request.session_packets[0].message.at("ec"), "ItemPolish_Damaged");
  EXPECT_TRUE(context_.order.empty());
}

TEST_F(InventorySessionTest, ToolWearReloadsSnapshotAndUsesCurrentCombatVitalsWithoutRecomputing) {
  context_.combat = true;
  context_.stats_available = true;
  const auto packets = session_->tool_wear_updates(progress_.player.items.front());
  ASSERT_EQ(packets.size(), 2U);
  EXPECT_EQ(packets[0].opcode, 47);
  EXPECT_EQ(packets[1].opcode, 168);
  EXPECT_EQ(context_.order, (std::vector<std::string>{"snapshot"}));
  EXPECT_EQ(context_.stats_calls, 0);
  EXPECT_EQ(progress_.character_loads, 1);
  EXPECT_EQ(context_.snapshot_stats, context_.combat_stats);
}

TEST_F(InventorySessionTest, SnapshotSinkUsesBorrowedPreparedDataWithoutCombatRecomputation) {
  context_.combat = true;
  context_.stats_available = true;
  const Json prepared{{"69", 500}};
  session_->on_inventory_snapshot_changed(progress_.player, &prepared);
  EXPECT_EQ(context_.stats_calls, 0);
  EXPECT_EQ(context_.combat_updates, 0);
  EXPECT_EQ(context_.snapshot_stats, prepared);
  session_->on_inventory_stats_changed(progress_.player);
  EXPECT_EQ(context_.stats_calls, 1);
  EXPECT_EQ(context_.combat_updates, 1);
  EXPECT_EQ(context_.snapshots, 1);
}

TEST_F(InventorySessionTest, EncumbranceFailureKeepsOldTierAndRetryRetainsTheAppliedSlowdown) {
  feature_.load_capacity_and_encumbrance();
  session_->on_connection_open();
  context_.combat = true;
  context_.stats_available = true;
  context_.expect_prepared_move = true;
  context_.fail_stats = true;
  std::vector<GameplayPacket> failed{{169, Json::object()}};
  session_->append_encumbrance_changes(failed, false);
  EXPECT_EQ(failed.size(), 1U);
  EXPECT_EQ(context_.combat_updates, 0);

  context_.fail_stats = false;
  std::vector<GameplayPacket> changed{{169, Json::object()}};
  session_->append_encumbrance_changes(changed, false);
  ASSERT_EQ(changed.size(), 3U);
  EXPECT_EQ(changed[1].opcode, 47);
  EXPECT_EQ(context_.combat_stats.at("84"), 500);
  EXPECT_EQ(session_->session_stats(progress_.player).at("84"), 500);
}

} // namespace
} // namespace chronicles::inventory
