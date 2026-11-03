#include "cms.generated.pb.h"
#include "protocol/protocol.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/adventure/adventure.hpp"
#include "data/client_catalog.hpp"
#include "world/features/character/progression.hpp"
#include "world/features/quests/world_quests.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace chronicles::adventure {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kNow = 100000000;
constexpr WorldPosition kPosition{9012, 0, 0, 0};

Json definitions() {
  const Json object{{"unit_id", "100"},      {"cms_id", 2001},      {"position", {0, 0, 0}},
                    {"interaction_id", 300}, {"animation_id", 100}, {"execute_time_ms", 200},
                    {"duration_ms", 500},    {"loot_pack_id", 1},   {"rewards", Json::array()}};
  const Json map{{"format_version", 1},
                 {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
                 {"profile_scope", "shared_adventure"},
                 {"map_id", 9012},
                 {"objects", {object}},
                 {"quests", Json::array()},
                 {"doodads", Json::array()},
                 {"sectors", Json::array()},
                 {"utility_items", Json::array()},
                 {"civilians", Json::array()}};
  const Json entry{{"kind", "item"},
                   {"item_cms_id", 50},
                   {"min_quantity", 1},
                   {"max_quantity", 1},
                   {"weight", 1}};
  const Json loot{
      {"format_version", 2},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"looting_range_mm", 5000},
      {"corpse_lootable_ms", 1000},
      {"corpse_empty_ms", 1000},
      {"gold_limit", 10000000},
      {"npc_fields",
       {"npc_cms_id", "loot_pack_id", "handling_pack_id", "handling_rate_percent", "kill_gold"}},
      {"npcs", Json::array()},
      {"handling_packs", Json::array()},
      {"items",
       {{"50",
         {{"storage", "stackable"},
          {"grade", "Normal"},
          {"max_durability", 0},
          {"required_quests", Json::array()}}}}},
      {"packs",
       {{{"loot_pack_id", 1},
         {"rate_percent", 100},
         {"groups",
          {{{"loot_group_id", 1},
            {"rate_percent", 100},
            {"custom_rules", Json::array()},
            {"entries", {entry}}}}}}}}};
  return {{"format_version", 1},
          {"source_executable_dump_sha256",
           "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6"},
          {"maps", {map}},
          {"loot", loot}};
}

struct Storage final : ProgressAccess {
  WorldProgress saved;
  bool reject{};
  int commits{};

  WorldProgress load() override {
    return saved;
  }

  std::optional<Character> character() override {
    return Character{};
  }

  bool save(const WorldProgress &before, const WorldProgress &after) {
    if (reject || before != saved) {
      return false;
    }
    saved = after;
    ++commits;
    return true;
  }

  std::optional<std::vector<CharacterItem>> commit(const WorldProgress &, const WorldProgress &,
                                                   const ItemTrade &) override {
    return std::nullopt;
  }
};

class Adventure : public testing::Test {
protected:
  Storage storage;
  tutorial::TutorialRewardPersistence items;
  std::shared_ptr<const Catalog> catalog = std::make_shared<Catalog>(definitions());
  std::unique_ptr<Session> session;

  void SetUp() override {
    reconnect();
  }

  void reconnect() {
    session = std::make_unique<Session>(
        catalog, 7, storage,
        [this](const auto &before, const auto &after) { return storage.save(before, after); },
        items, nullptr, std::make_unique<SeededLootRandom>(1));
  }

  std::vector<GameplayPacket> start() {
    return session->receive({20069,
                             1,
                             {{"target_unit_id", "100"},
                              {"interaction_anim_id", 100},
                              {"param", {{"interaction", {{"interaction_id", 300}}}}},
                              {"pos", {{"millimeter_x", 0}}},
                              {"yaw", 0}}},
                            kPosition, kNow, false);
  }

  std::vector<GameplayPacket> execute(std::int64_t now = kNow + 3000000) {
    return session->receive({20070, 2, {{"interaction_anim_id", 100}}}, kPosition, now, false);
  }

  void check_encoding(const std::vector<GameplayPacket> &packets) {
    Protocol protocol(CHRONICLES_PROTOCOL_DIR);
    for (const auto &packet : packets) {
      EXPECT_NO_THROW(protocol.encode(packet.opcode, packet.message));
    }
  }
};

TEST_F(Adventure, ActivationAndRewardCommitTogetherAndNeverRepeatAfterReconnect) {
  const auto started = start();
  ASSERT_EQ(started.size(), 1u);
  EXPECT_EQ(started[0].opcode, 166);
  check_encoding(started);
  EXPECT_TRUE(storage.saved.adventure_objects.empty());
  check_encoding(execute());
  EXPECT_EQ(storage.saved.adventure_objects.at(2001), 2);
  EXPECT_EQ(storage.saved.stackables.at(50), 1);
  EXPECT_EQ(storage.commits, 1);
  execute();
  EXPECT_EQ(storage.commits, 1);
  reconnect();
  start();
  execute();
  EXPECT_EQ(storage.saved.stackables.at(50), 1);
  EXPECT_EQ(storage.commits, 1);
}

TEST_F(Adventure, BossGatedCoilstoneAwardsTraitPointsOnce) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported catalog";
  }
  ClientCatalog client_catalog(catalog_path);
  auto progression = Progression::load(client_catalog);
  auto document = definitions();
  auto &object = document["maps"][0]["objects"][0];
  object["loot_pack_id"] = 0;
  object["build_points"] = {{{"type", 2}, {"value", 2}}};
  object["interaction_conditions"] = {{{"group", 1}, {"operator", "OR"}, {"negated", false},
                                        {"kind", "PlayRecord"}, {"play_record_id", 524},
                                        {"count", 1}, {"comparison", "GreaterThanOrEqual"},
                                        {"record", {{"kind", "HuntMonster"},
                                                    {"npc_id", 5001535}}}}};
  catalog = std::make_shared<Catalog>(std::move(document));
  session = std::make_unique<Session>(
      catalog, 7, storage,
      [this](const auto &before, const auto &after) { return storage.save(before, after); },
      items, progression, std::make_unique<SeededLootRandom>(1));
  session->set_requirement_check([this](const Json &, const WorldPosition &) {
    return storage.saved.play_records.contains(524);
  });

  const auto refused = start();
  ASSERT_EQ(refused.size(), 1U);
  EXPECT_EQ(refused.front().message.at("ec"), "Interaction_ConditionsNotMatch");
  EXPECT_EQ(storage.saved.weapon_points, 0);

  storage.saved.play_records[524] = 1;
  ASSERT_EQ(start().front().opcode, 166);
  const auto granted = execute();
  EXPECT_EQ(storage.saved.weapon_points, 2);
  EXPECT_EQ(storage.saved.adventure_objects.at(2001), 2);
  EXPECT_TRUE(std::ranges::any_of(granted, [](const GameplayPacket &packet) {
    return packet.opcode == 36;
  }));
  check_encoding(granted);
  execute();
  EXPECT_EQ(storage.saved.weapon_points, 2);
}

TEST_F(Adventure, EarlyExecutionAndMovementCancellationDoNotGrantRewards) {
  start();
  check_encoding(execute(kNow));
  EXPECT_TRUE(storage.saved.adventure_objects.empty());
  const auto canceled = session->advance(kNow + 1000000, WorldPosition{9012, 2000, 0, 0}, false);
  ASSERT_EQ(canceled.size(), 1u);
  EXPECT_EQ(canceled[0].opcode, 167);
  EXPECT_FALSE(session->busy());
  EXPECT_TRUE(storage.saved.stackables.empty());
}

TEST_F(Adventure, ConflictKeepsActivationAndRewardUncommittedAndCanRetry) {
  start();
  storage.reject = true;
  EXPECT_THROW(execute(), std::runtime_error);
  EXPECT_TRUE(storage.saved.adventure_objects.empty());
  EXPECT_TRUE(storage.saved.stackables.empty());
  storage.reject = false;
  execute();
  EXPECT_EQ(storage.saved.stackables.at(50), 1);
  EXPECT_EQ(storage.commits, 1);
}

TEST_F(Adventure, OtherProgressCommitsArePreservedWhileAnAnimationIsOpen) {
  start();
  storage.saved.gold = 50;
  ++storage.saved.revision;
  execute();
  EXPECT_EQ(storage.saved.gold, 50);
  EXPECT_EQ(storage.saved.revision, 2);
  EXPECT_EQ(storage.saved.stackables.at(50), 1);
}

TEST_F(Adventure, DeathLoadingAndWrongMapCancelThePendingInteraction) {
  start();
  storage.saved.dead = true;
  ++storage.saved.revision;
  EXPECT_EQ(session->advance(kNow, kPosition, false).size(), 1u);
  storage.saved.dead = false;
  ++storage.saved.revision;
  start();
  EXPECT_EQ(session->advance(kNow, std::nullopt, false).size(), 1u);
  start();
  EXPECT_EQ(session->advance(kNow, WorldPosition{9031, 0, 0, 0}, false).size(), 1u);
  EXPECT_TRUE(storage.saved.adventure_objects.empty());
}

TEST_F(Adventure, UnavailableRewardDefinitionsRefuseBeforeStartingAnAnimation) {
  auto document = definitions();
  document["maps"][0]["objects"][0]["unavailable_reason"] = "missing authored reward";
  catalog = std::make_shared<Catalog>(document);
  reconnect();
  const auto packets = start();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].opcode, 165);
  check_encoding(packets);
  EXPECT_FALSE(session->busy());
  EXPECT_TRUE(storage.saved.adventure_objects.empty());
}

// First Hunt (10000125) gates its hunting knife 20019119 on objective group 1 in progress.
Json first_hunt_knife_conditions() {
  return Json::array({{{"group", 1},
                       {"operator", "AND"},
                       {"negated", false},
                       {"kind", "Quest"},
                       {"quest_id", 10000125},
                       {"requirement", "Progressing"},
                       {"progress_group", 1}}});
}

// The knife has no interaction animation, so the client uses it with one InteractUnitReq.
Json quest_knife_definitions() {
  auto document = definitions();
  auto &object = document["maps"][0]["objects"][0];
  object.erase("animation_id");
  object.erase("execute_time_ms");
  object.erase("duration_ms");
  object["interaction_id"] = 10000135;
  object["interaction_conditions"] = first_hunt_knife_conditions();
  return document;
}

class QuestGatedAdventure : public Adventure {
protected:
  std::vector<Json> checked_conditions;
  bool conditions_met{};

  void SetUp() override {
    catalog = std::make_shared<Catalog>(quest_knife_definitions());
    reconnect();
    session->set_requirement_check([this](const Json &conditions, const WorldPosition &position) {
      EXPECT_EQ(position.map_id, kPosition.map_id);
      checked_conditions.push_back(conditions);
      return conditions_met;
    });
  }

  std::vector<GameplayPacket> use() {
    return session->receive({world_request::kInteractionUnitRequest,
                             3,
                             {{"target_unit_id", "100"},
                              {"param", {{"interaction_id", 10000135}}}}},
                            kPosition, kNow, false);
  }
};

TEST_F(QuestGatedAdventure, QuestItemIsGrantedOnlyWhileItsQuestStepIsInProgress) {
  const auto refused = use();
  ASSERT_EQ(refused.size(), 1u);
  EXPECT_EQ(refused[0].opcode, 2);
  EXPECT_EQ(refused[0].message.at("ec"), "Interaction_ConditionsNotMatch");
  check_encoding(refused);
  EXPECT_TRUE(storage.saved.adventure_objects.empty());
  EXPECT_EQ(storage.commits, 0);
  ASSERT_EQ(checked_conditions.size(), 1u);
  EXPECT_EQ(checked_conditions[0], first_hunt_knife_conditions());

  conditions_met = true;
  const auto granted = use();
  check_encoding(granted);
  EXPECT_EQ(storage.saved.adventure_objects.at(2001), 2);
  EXPECT_EQ(storage.saved.stackables.at(50), 1);
  EXPECT_EQ(storage.commits, 1);
  EXPECT_TRUE(std::ranges::any_of(granted, [](const GameplayPacket &packet) {
    return packet.opcode == 91;
  }));
}

TEST_F(QuestGatedAdventure, ActivatedQuestItemReplaysWithoutCheckingOrGrantingAgain) {
  conditions_met = true;
  use();
  conditions_met = false;
  checked_conditions.clear();

  const auto replay = use();
  EXPECT_TRUE(checked_conditions.empty());
  EXPECT_TRUE(std::ranges::none_of(replay, [](const GameplayPacket &packet) {
    return packet.opcode == 2;
  }));
  EXPECT_EQ(storage.saved.stackables.at(50), 1);
  EXPECT_EQ(storage.commits, 1);
}

TEST_F(QuestGatedAdventure, MissingRequirementCheckFailsClosed) {
  reconnect();
  const auto refused = use();
  ASSERT_EQ(refused.size(), 1u);
  EXPECT_EQ(refused[0].message.at("ec"), "Interaction_ConditionsNotMatch");
  EXPECT_TRUE(storage.saved.adventure_objects.empty());
}

// The real hunting knife is equipment: its loot row is an item instance with a grade, not a
// stack. The live server rejected exactly this take and disconnected the player.
class EquipmentRewardAdventure : public QuestGatedAdventure {
protected:
  std::vector<CharacterItem> stored_items;

  void SetUp() override {
    auto document = quest_knife_definitions();
    auto &knife = document["loot"]["items"]["50"];
    knife["storage"] = "instance";
    knife["max_durability"] = 4000;
    catalog = std::make_shared<Catalog>(document);

    items.load_character = [] {
      return std::optional<Character>{Character{}};
    };
    items.commit = [this](const WorldProgress &before, const WorldProgress &after,
                          const ItemTrade &trade) -> std::optional<std::vector<CharacterItem>> {
      if (!storage.save(before, after)) {
        return std::nullopt;
      }
      auto rows = trade.granted_items;
      for (std::size_t index = 0; index < rows.size(); ++index) {
        rows[index].id = static_cast<std::int64_t>(index) + 1;
      }
      stored_items.insert(stored_items.end(), rows.begin(), rows.end());
      return rows;
    };
    items.items_changed = [](const Character &) {};

    reconnect();
    session->set_requirement_check([this](const Json &, const WorldPosition &) {
      return conditions_met;
    });
  }
};

TEST_F(EquipmentRewardAdventure, QuestEquipmentFromAWorldObjectIsGrantedAsAnItemInstance) {
  conditions_met = true;

  std::vector<GameplayPacket> granted;
  ASSERT_NO_THROW(granted = use());
  check_encoding(granted);

  EXPECT_EQ(storage.saved.adventure_objects.at(2001), 2);
  EXPECT_FALSE(storage.saved.stackables.contains(50));
  ASSERT_EQ(stored_items.size(), 1u);
  EXPECT_EQ(stored_items[0].cms_id, 50);
  EXPECT_EQ(stored_items[0].grade, x5::cms::EX5ItemGrade_Normal);
  EXPECT_EQ(stored_items[0].durability, 4000);
}

TEST(AdventureProfileValidation, UnsupportedInteractionConditionsRejectTheProfile) {
  auto document = quest_knife_definitions();
  document["maps"][0]["objects"][0]["interaction_conditions"][0]["kind"] = "ConsumeItem";
  EXPECT_THROW(Catalog{document}, std::runtime_error);

  document = quest_knife_definitions();
  document["maps"][0]["objects"][0]["interaction_conditions"] = Json::array();
  EXPECT_THROW(Catalog{document}, std::runtime_error);
}

TEST(AdventureProfile, GeneratedBossRecordUnlocksCoilstoneAndPurchasableTrait) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  const auto *quest_path = std::getenv("CHRONICLES_COILSTONE_QUEST_PROFILE");
  const auto *adventure_path = std::getenv("CHRONICLES_COILSTONE_ADVENTURE_PROFILE");
  if (!catalog_path || !quest_path || !adventure_path) {
    GTEST_SKIP() << "Set the client catalog and both generated Coilstone profile paths";
  }

  ClientCatalog client_catalog(catalog_path);
  const auto progression = Progression::load(client_catalog);
  const auto quests = QuestCatalog::load(quest_path);
  const auto adventure = Catalog::load(adventure_path);
  const auto map = adventure->map(9012);
  ASSERT_TRUE(map);
  const auto coilstone = std::ranges::find_if(map->objects, [](const auto &entry) {
    return entry.second.at("cms_id") == 10010002;
  });
  ASSERT_NE(coilstone, map->objects.end());
  const auto &object = coilstone->second;
  const auto &point = object.at("position");
  const WorldPosition position{9012, point.at(0), point.at(1), point.at(2), 0.0F};
  const auto unit_id = std::to_string(coilstone->first);
  const int animation = object.at("animation_id");
  const int interaction = object.at("interaction_id");

  Storage storage;
  Character character;
  character.class_id = x5::cms::EX5CharacterClass_Wild;
  QuestWorldView view;
  view.position = position;
  view.character = &character;
  WorldQuestBook book(quests, 7, progression);
  const auto make_session = [&] {
    auto session = std::make_unique<Session>(
        adventure, 7, storage,
        [&](const auto &before, const auto &after) { return storage.save(before, after); },
        tutorial::TutorialRewardPersistence{}, progression, std::make_unique<SeededLootRandom>(1));
    session->set_requirement_check([&](const Json &conditions, const WorldPosition &) {
      return book.requirements_met(conditions, storage.saved, view);
    });
    return session;
  };
  const auto start = [&](Session &session) {
    return session.receive(
        {20069, 1,
         {{"target_unit_id", unit_id},
          {"interaction_anim_id", animation},
          {"param", {{"interaction", {{"interaction_id", interaction}}}}},
          {"pos", {{"millimeter_x", position.x_mm},
                   {"millimeter_y", position.y_mm},
                   {"millimeter_z", position.z_mm}}},
          {"yaw", 0}}},
        position, kNow, false);
  };
  const auto execute = [&](Session &session) {
    const auto execute_tick = kNow + object.at("execute_time_ms").get<std::int64_t>() * 10000;
    return session.receive({20070, 2, {{"interaction_anim_id", animation}}}, position,
                           execute_tick, false);
  };

  const auto next_level = progression->next_level_experience(storage.saved.level);
  ASSERT_TRUE(next_level);
  const auto level_change = progression->award_experience(storage.saved, *next_level);
  ASSERT_EQ(level_change.levels_gained, 1);
  EXPECT_GT(storage.saved.level_up_points, 0);
  EXPECT_EQ(storage.saved.weapon_points, 0);

  auto session = make_session();
  const auto refused = start(*session);
  ASSERT_EQ(refused.size(), 1U);
  EXPECT_EQ(refused.front().message.at("ec"), "Interaction_ConditionsNotMatch");
  EXPECT_EQ(storage.saved.weapon_points, 0);

  const auto defeated = book.record_monster_defeat(1234, 5001535, storage.saved, view, false);
  ASSERT_TRUE(defeated.next);
  storage.saved = *defeated.next;
  ASSERT_EQ(storage.saved.play_records.at(524), 1);
  ASSERT_EQ(start(*session).front().opcode, 166);
  const auto awarded = execute(*session);
  EXPECT_EQ(storage.saved.weapon_points, 2);
  EXPECT_EQ(storage.saved.adventure_objects.at(10010002), 2);
  EXPECT_TRUE(std::ranges::any_of(awarded, [](const GameplayPacket &packet) {
    return packet.opcode == kChangeCharacterBaseNotification;
  }));

  auto learned = storage.saved;
  ASSERT_EQ(progression->change_perks(learned, character.class_id, {{1230201, 1}}),
            x5::ec::Success);
  EXPECT_EQ(learned.weapon_points, 1);
  EXPECT_EQ(learned.build.perks.at(1230201), 1);
  ++learned.revision;
  ASSERT_TRUE(storage.save(storage.saved, learned));
  session = make_session();
  ASSERT_EQ(start(*session).front().opcode, 166);
  execute(*session);
  EXPECT_EQ(storage.saved.weapon_points, 1);
  EXPECT_EQ(storage.saved.build.perks.at(1230201), 1);
}

TEST(AdventureProfile, GeneratedSharedProfileLoadsTheReceivedInteractionMaps) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }
  const auto profile_path =
      std::filesystem::path(catalog_path).parent_path() / "adventure" / "definitions.json";
  if (!std::filesystem::exists(profile_path)) {
    GTEST_SKIP() << "Generate data/generated/adventure/definitions.json first";
  }

  const auto loaded = Catalog::load(profile_path);
  ASSERT_NE(loaded, nullptr);
  // The audited live reports came from chest/record interactions on these shared maps.
  for (const int map_id : {9012, 9031}) {
    const auto map = loaded->map(map_id);
    ASSERT_NE(map, nullptr) << map_id;
    EXPECT_FALSE(map->objects.empty()) << map_id;
  }
  EXPECT_EQ(loaded->map(8000), nullptr);

  // First Hunt's hunting knife is a quest-gated looting object on map 9012.
  const auto shining_shore = loaded->map(9012);
  const bool has_knife =
      std::ranges::any_of(shining_shore->objects, [](const auto &entry) {
        const auto &object = entry.second;
        return object.at("cms_id") == 20019119 && object.contains("interaction_conditions") &&
               object.at("interaction_conditions").at(0).at("quest_id") == 10000125;
      });
  EXPECT_TRUE(has_knife);
}

// Every equipment reward any shared object can roll must name a grade the tutorial reward
// commit accepts; otherwise taking it rejects the request and disconnects the player.
TEST(AdventureProfile, EveryGeneratedObjectRewardUsesACommittableGradeName) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }
  const auto profile_path =
      std::filesystem::path(catalog_path).parent_path() / "adventure" / "definitions.json";
  if (!std::filesystem::exists(profile_path)) {
    GTEST_SKIP() << "Generate data/generated/adventure/definitions.json first";
  }

  const auto loaded = Catalog::load(profile_path);
  ASSERT_NE(loaded, nullptr);

  constexpr int kRollsPerObject = 8;
  SeededLootRandom random(7);
  const WorldProgress progress;
  int instance_rewards = 0;
  for (const int map_id : loaded->map_ids()) {
    const auto map = loaded->map(map_id);
    for (const auto &[unit, object] : map->objects) {
      const int pack_id = object.value("loot_pack_id", 0);
      if (pack_id == 0) {
        continue;
      }
      for (int roll = 0; roll < kRollsPerObject; ++roll) {
        for (const auto &reward : loaded->roll_rewards(pack_id, random, progress)) {
          if (!reward.contains("instance")) {
            continue;
          }
          ++instance_rewards;
          const auto grade = reward.at("instance").at("grade").get<std::string>();
          x5::cms::EX5ItemGrade parsed{};
          EXPECT_TRUE(x5::cms::EX5ItemGrade_Parse("EX5ItemGrade_" + grade, &parsed))
              << "object " << object.at("cms_id") << " grade " << grade;
        }
      }
    }
  }
  EXPECT_GT(instance_rewards, 0);
}

TEST(AdventureProfile, CliffsPrisonKeyUsesAnIndividualItemReward) {
  const auto *profile_path = std::getenv("CHRONICLES_CLIFFS_ADVENTURE_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIFFS_ADVENTURE_PROFILE to the regenerated adventure profile";
  }

  const auto catalog = Catalog::load(profile_path);
  const auto map = catalog->map(30104);
  ASSERT_NE(map, nullptr);
  const auto key_object = std::ranges::find_if(map->objects, [](const auto &entry) {
    return entry.second.at("cms_id") == 20014613;
  });
  ASSERT_NE(key_object, map->objects.end());
  SeededLootRandom random(7);
  const auto rewards = catalog->roll_rewards(key_object->second.at("loot_pack_id"), random,
                                              WorldProgress{});
  ASSERT_EQ(rewards.size(), 1U);
  EXPECT_EQ(rewards[0].at("item_cms_id"), 120109);
  EXPECT_EQ(rewards[0].at("instance").at("max_durability"), 1);
}

} // namespace
} // namespace chronicles::adventure
