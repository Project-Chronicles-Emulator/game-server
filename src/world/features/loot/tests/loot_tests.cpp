#include "world/features/loot/loot.hpp"

#include "testing/loot_test_support.hpp"
#include "testing/memory_repositories.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

namespace chronicles {
namespace {

using namespace loot_test;

TEST(LootData, LoadsPacksItemsAndSettingsFromTheGeneratedProfileShape) {
  const LootData data(profile_fixture());

  EXPECT_EQ(data.pack_count(), 8U);
  EXPECT_EQ(data.npc_count(), 10U);
  EXPECT_EQ(data.withheld_group_count(), 0U);
  EXPECT_DOUBLE_EQ(data.looting_range_mm(), 5000.0);
  EXPECT_EQ(data.corpse_lootable_ms(), 90000);
  EXPECT_EQ(data.corpse_empty_ms(), 10000);
  EXPECT_EQ(data.gold_limit(), kGoldLimit);

  const auto *npc = data.npc(kFullPackNpc);
  ASSERT_NE(npc, nullptr);
  EXPECT_EQ(npc->pack_id, kFullPack);
  const auto *pack = data.pack(npc->pack_id);
  ASSERT_NE(pack, nullptr);
  ASSERT_EQ(pack->groups.size(), 3U);
  EXPECT_TRUE(pack->groups[1].entries.at(0).is_gold);
  EXPECT_FALSE(pack->has_first_kill_group);
  EXPECT_EQ(data.npc(kUnlootedNpc), nullptr);

  const auto *sword = data.item(kSword);
  ASSERT_NE(sword, nullptr);
  EXPECT_EQ(sword->storage, LootItemStorage::instance);
  EXPECT_EQ(sword->grade, kSwordGrade);
  EXPECT_EQ(sword->max_durability, 10000);
  EXPECT_EQ(data.item(kHerb)->storage, LootItemStorage::stackable);
  EXPECT_EQ(data.item(kQuestItem)->required_quests, (std::vector<int>{kQuestId}));
  EXPECT_EQ(data.item(999), nullptr);
}

TEST(LootData, CustomNpcFactsAreLoaded) {
  const LootData data(profile_fixture());

  EXPECT_EQ(data.npc(kGoldOnlyNpc)->kill_gold, kKillGold);
  EXPECT_EQ(data.npc(kGoldOnlyNpc)->pack_id, 0);
  EXPECT_EQ(data.npc(kHandlingNpc)->handling_pack_id, kHandlingPack);
  EXPECT_DOUBLE_EQ(data.npc(kHandlingNpc)->handling_rate_percent, 50.0);
  EXPECT_TRUE(data.pack(kFirstKillPack)->has_first_kill_group);
  EXPECT_TRUE(data.pack(kFirstKillPack)->groups.at(1).first_kill_only);
  EXPECT_EQ(data.pack(kPolishPack)->groups.at(0).entries.at(0).polish_step, kPolishStep);
  ASSERT_NE(data.handling_pack(kHandlingPack), nullptr);
  EXPECT_EQ(data.handling_pack(kSinglePack), nullptr);
}

TEST(LootData, MalformedOrInconsistentProfilesFailAtStartup) {
  const auto rejected = [](const std::function<void(Json &)> &damage) {
    auto profile = profile_fixture();
    damage(profile);
    EXPECT_THROW(LootData{profile}, std::runtime_error) << profile.dump();
  };

  rejected([](Json &p) { p["build_id"] = "another-client-build"; });
  rejected([](Json &p) { p["format_version"] = 1; });
  rejected([](Json &p) { p["npc_fields"] = Json::array({"npc_cms_id", "loot_pack_id"}); });
  rejected([](Json &p) { p.erase("looting_range_mm"); });
  rejected([](Json &p) { p["looting_range_mm"] = 0; });
  rejected([](Json &p) { p["corpse_lootable_ms"] = -1; });
  rejected([](Json &p) { p["gold_limit"] = 0; });
  rejected([](Json &p) { p["items"]["abc"] = p["items"]["100"]; });
  rejected([](Json &p) { p["items"]["100"]["storage"] = "bag"; });
  rejected([](Json &p) { p["items"]["100"]["grade"] = "Bogus"; });
  rejected([](Json &p) { p["items"]["200"]["max_durability"] = 0; });
  rejected([](Json &p) { p["items"]["100"]["required_quests"] = Json::array({0}); });
  rejected([](Json &p) { p["items"]["100"].erase("required_quests"); });
  rejected([](Json &p) { p["packs"][0]["groups"][0]["entries"][0]["item_cms_id"] = 777; });
  rejected([](Json &p) { p["packs"][0]["groups"][0]["entries"][0]["kind"] = "weird"; });
  rejected([](Json &p) { p["packs"][0]["groups"][0]["entries"][0]["min_quantity"] = 0; });
  rejected([](Json &p) {
    p["packs"][0]["groups"][0]["entries"][0]["min_quantity"] = 5;
    p["packs"][0]["groups"][0]["entries"][0]["max_quantity"] = 4;
  });
  rejected([](Json &p) { p["packs"][0]["groups"][0]["entries"][0]["weight"] = 0; });
  rejected([](Json &p) { p["packs"][0]["groups"][0]["entries"] = Json::array(); });
  rejected([](Json &p) { p["packs"][0]["groups"][0]["custom_rules"] = Json::array({"bogus"}); });
  rejected([](Json &p) { p["packs"][0]["groups"] = Json::array(); });
  rejected([](Json &p) { p["packs"][0]["rate_percent"] = 0; });
  rejected([](Json &p) { p["packs"][0]["groups"][0]["rate_percent"] = 100.5; });
  rejected([](Json &p) { p["packs"].push_back(p["packs"][0]); });
  rejected([](Json &p) { p["handling_packs"].push_back(p["handling_packs"][0]); });
  rejected([](Json &p) { p["npcs"].push_back(npc_row(kFullPackNpc, kSinglePack)); });
  rejected([](Json &p) { p["npcs"].push_back(npc_row(6000, 999)); });
  rejected([](Json &p) { p["npcs"].push_back(npc_row(6000, 0, 999, 50.0)); });
  rejected([](Json &p) { p["npcs"].push_back(npc_row(6000, 0, kHandlingPack, 0.0)); });
  rejected([](Json &p) { p["npcs"].push_back(npc_row(6000, 0, 0, 0.0, -1)); });
  rejected([](Json &p) { p["npcs"].push_back(Json::array({6000, 0})); });
  rejected([](Json &p) { p["npcs"].push_back(npc_row(0, kFullPack)); });
  // The tool bounds an instance entry so one corpse cannot exceed a single storage trade.
  rejected([](Json &p) {
    p["packs"][0]["groups"][2]["entries"][0]["max_quantity"] = kMaximumInstancesPerLootEntry + 1;
  });
  // A polish step needs an instance item and a valid step.
  rejected([](Json &p) { p["packs"][5]["groups"][0]["entries"][0]["polish_step"] = 0; });
  rejected([](Json &p) {
    p["packs"][5]["groups"][0]["entries"][0]["polish_step"] = kMaxItemPolishStep + 1;
  });
  rejected([](Json &p) { p["packs"][1]["groups"][0]["entries"][0]["polish_step"] = 2; });
}

TEST(LootRoll, FollowsPackGroupWeightedEntryAndQuantityOrder) {
  const LootData data(profile_fixture());
  ScriptedLootRandom random;
  random.indexes = {1};

  const auto contents = roll_loot(data, *data.pack(kPartialPack), random);

  EXPECT_EQ(random.rates_seen, (std::vector<double>{50.0, 25.0}));
  ASSERT_EQ(random.weights_seen.size(), 1U);
  EXPECT_EQ(random.weights_seen.front(), (std::vector<int>{3, 1}));
  EXPECT_EQ(random.ranges_seen, (std::vector<std::pair<int, int>>{{1, 1}}));
  EXPECT_EQ(contents.stackables, (std::map<int, std::int64_t>{{kOre, 1}}));
  EXPECT_EQ(contents.gold, 0);
  EXPECT_TRUE(contents.instances.empty());
}

TEST(LootRoll, FailedPackRateStopsBeforeAnyGroupIsConsidered) {
  const LootData data(profile_fixture());
  ScriptedLootRandom random;
  random.pass_results = {false};

  const auto contents = roll_loot(data, *data.pack(kPartialPack), random);

  EXPECT_TRUE(contents.empty());
  EXPECT_EQ(random.rates_seen, (std::vector<double>{50.0}));
  EXPECT_EQ(random.draws(), 1U);
}

TEST(LootRoll, FailedGroupRateSkipsOnlyThatGroup) {
  const LootData data(profile_fixture());
  ScriptedLootRandom random;
  random.pass_results = {true, false, true, true};
  random.quantities = {7, 1};

  const auto contents = roll_loot(data, *data.pack(kFullPack), random);

  EXPECT_EQ(contents.gold, 7);
  EXPECT_TRUE(contents.stackables.empty());
  EXPECT_EQ(contents.instances, (std::vector<LootInstance>{{kSword, 0}}));
  EXPECT_EQ(random.ranges_seen, (std::vector<std::pair<int, int>>{{5, 9}, {1, 1}}));
}

TEST(LootRoll, QuantitiesFromSeveralGroupsAddUpPerItem) {
  auto profile = profile_fixture();
  profile["packs"][1]["groups"].push_back(group(100.0, {entry("item", kHerb, 1, 1)}));
  profile["packs"][1]["groups"].push_back(group(100.0, {entry("item", kHerb, 1, 1)}));
  const LootData data(profile);
  ScriptedLootRandom random;

  const auto contents = roll_loot(data, *data.pack(kSinglePack), random);

  EXPECT_EQ(contents.stackables, (std::map<int, std::int64_t>{{kHerb, 3}}));
}

TEST(LootRoll, InstanceCountPerCorpseIsBounded) {
  auto profile = profile_fixture();
  auto &groups = profile["packs"][1]["groups"];
  groups = Json::array();
  for (int index = 0; index < 4; ++index) {
    groups.push_back(group(100.0, {entry("item", kSword, 16, 16)}));
  }
  const LootData data(profile);
  ScriptedLootRandom random;

  const auto contents = roll_loot(data, *data.pack(kSinglePack), random);

  EXPECT_EQ(contents.instances.size(), kMaximumLootInstancesPerCorpse);
}

TEST(LootRandom, SeededSequencesRepeatAndRespectRatesWeightsAndRanges) {
  SeededLootRandom first(1234);
  SeededLootRandom second(1234);
  const std::vector<int> weights{3, 1};
  for (int index = 0; index < 50; ++index) {
    ASSERT_EQ(first.passes(25.0), second.passes(25.0));
    ASSERT_EQ(first.pick_index(weights), second.pick_index(weights));
    ASSERT_EQ(first.pick_quantity(2, 9), second.pick_quantity(2, 9));
  }

  SeededLootRandom random(99);
  constexpr int kTrials = 20000;
  int passed = 0;
  int heavy_picks = 0;
  int lowest = std::numeric_limits<int>::max();
  int highest = std::numeric_limits<int>::min();
  for (int trial = 0; trial < kTrials; ++trial) {
    passed += random.passes(25.0) ? 1 : 0;
    heavy_picks += random.pick_index(weights) == 0 ? 1 : 0;
    const auto quantity = random.pick_quantity(2, 9);
    lowest = std::min(lowest, quantity);
    highest = std::max(highest, quantity);
  }
  EXPECT_NEAR(passed / static_cast<double>(kTrials), 0.25, 0.02);
  EXPECT_NEAR(heavy_picks / static_cast<double>(kTrials), 0.75, 0.02);
  EXPECT_EQ(lowest, 2);
  EXPECT_EQ(highest, 9);
  for (int trial = 0; trial < 100; ++trial) {
    EXPECT_TRUE(random.passes(100.0));
  }
}

TEST_F(LootSessionTest, KillAnnouncesLootOnlyToTheKillerForModeledPacks) {
  auto session = make_session();

  const auto reaction = session.monster_defeated(defeat(kCorpse, kFullPackNpc));
  ASSERT_EQ(reaction.packets.size(), 1U);
  EXPECT_EQ(reaction.packets.front().opcode, kUnitLootItemInfoOpcode);
  EXPECT_EQ(reaction.packets.front().message,
            (Json{{"unit_id", std::to_string(kCorpse)}, {"has_chance_to_loot", true}}));
  // The session keeps this corpse past combat's default timer, so it owns the removal.
  EXPECT_TRUE(reaction.manages_corpse_removal);
  expect_encodes(reaction.packets);

  const auto no_pack = session.monster_defeated(defeat(1, kUnlootedNpc));
  EXPECT_TRUE(no_pack.packets.empty());
  EXPECT_FALSE(no_pack.manages_corpse_removal);
  auto other_killer = defeat(2, kFullPackNpc);
  other_killer.killer_character_id = character.id + 1;
  const auto foreign = session.monster_defeated(other_killer);
  EXPECT_TRUE(foreign.packets.empty());
  EXPECT_FALSE(foreign.manages_corpse_removal);
  EXPECT_EQ(session.corpse_count(), 3U);
}

TEST_F(LootSessionTest, TakeGrantsGoldStacksAndInstancesAtomicallyWithCapturedPacketOrder) {
  update_progress([](WorldProgress &state) {
    state.gold = 20;
    state.stackables[kHerb] = 3;
  });
  auto session = make_session();
  const auto revision = progress().revision;
  random->quantities = {4, 7, 1};
  session.monster_defeated(defeat(kCorpse, kFullPackNpc));

  const auto packets = session.receive(loot_request(kCorpse), player_at(1000), kStartTicks);

  ASSERT_EQ(packets.size(), 3U);
  EXPECT_EQ(packets[0].opcode, kLootItemsUpdateOpcode);
  EXPECT_EQ(packets[1].opcode, kUnitLootItemInfoOpcode);
  EXPECT_EQ(packets[2].opcode, kLootItemResponseOpcode);

  const auto &update = packets[0].message;
  EXPECT_EQ(update.at("looting").at("target_unit_id"), std::to_string(kCorpse));
  EXPECT_EQ(update.at("packet_seq"), Json::object());
  // Stack updates report the resulting total, as in the captured looting updates.
  EXPECT_EQ(update.at("stackables"), (Json{{{"item_cms_id", kHerb}, {"quantity", 7}}}));
  EXPECT_EQ(update.at("money_changes"), (Json{{{"change", "7"}, {"result", "27"}}}));
  ASSERT_EQ(update.at("items").size(), 1U);
  const auto &sword = update.at("items").at(0);
  EXPECT_EQ(sword.at("item_cms_id"), kSword);
  EXPECT_EQ(sword.at("durability"), 10000);
  EXPECT_EQ(sword.at("grade"), kSwordGrade);
  EXPECT_EQ(sword.at("created_epochtime"), std::to_string(kCreatedEpochTicks));
  EXPECT_NE(sword.at("item_id").get<std::string>(), std::string("0"));

  EXPECT_EQ(packets[1].message, (Json{{"unit_id", std::to_string(kCorpse)}}));
  EXPECT_EQ(packets[2].message,
            (Json{{"unit_id", std::to_string(kCorpse)}, {"packet_seq", Json::object()}}));
  expect_encodes(packets);

  const auto saved = progress();
  EXPECT_EQ(saved.revision, revision + 1);
  EXPECT_EQ(saved.gold, 27);
  EXPECT_EQ(saved.stackables.at(kHerb), 7);
  const auto items = stored_items();
  const auto sword_id = sword.at("item_id").get<std::string>();
  const auto created = std::ranges::find_if(items, [&](const CharacterItem &item) {
    return std::to_string(item.id) == sword_id && item.cms_id == kSword;
  });
  ASSERT_NE(created, items.end());
  EXPECT_FALSE(created->equip_slot.has_value());
  EXPECT_EQ(created->durability, 10000);
  EXPECT_EQ(created->grade, kSwordGrade);
  EXPECT_EQ(created->created_epoch_ticks, kCreatedEpochTicks);
  EXPECT_EQ(std::ranges::count_if(items,
                                  [&](const CharacterItem &item) {
                                    return item.inventory_slot == created->inventory_slot;
                                  }),
            1);
}

TEST_F(LootSessionTest, ASecondTakeIsRefusedAndWritesNothing) {
  auto session = make_session();
  session.monster_defeated(defeat(kCorpse, kSinglePackNpc));
  ASSERT_EQ(session.receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);
  const auto after_first = progress();

  const auto repeated = session.receive(loot_request(kCorpse), player_at(0), kStartTicks + 1);

  EXPECT_EQ(error_of(repeated), "Npc_HasNoLoot");
  EXPECT_EQ(progress(), after_first);
  EXPECT_EQ(commit_attempts, 1);
}

TEST_F(LootSessionTest, EmptyRollStillAnswersAndClearsTheFlagWithoutTouchingStorage) {
  auto session = make_session();
  random->pass_results = {false};
  const auto announced = session.monster_defeated(defeat(kCorpse, kPartialPackNpc));
  // The captured flag appears even when the roll turns out empty.
  ASSERT_EQ(announced.packets.size(), 1U);
  const auto before = progress();

  const auto packets = session.receive(loot_request(kCorpse), player_at(0), kStartTicks);

  ASSERT_EQ(packets.size(), 2U);
  EXPECT_EQ(packets[0].opcode, kUnitLootItemInfoOpcode);
  EXPECT_EQ(packets[0].message, (Json{{"unit_id", std::to_string(kCorpse)}}));
  EXPECT_EQ(packets[1].opcode, kLootItemResponseOpcode);
  EXPECT_FALSE(packets[1].message.contains("ec"));
  EXPECT_EQ(commit_attempts, 0);
  EXPECT_EQ(progress(), before);
}

TEST_F(LootSessionTest, RangeLimitIncludesToleranceAndOutOfRangeLeavesTheCorpseLootable) {
  auto session = make_session();
  session.monster_defeated(defeat(1, kSinglePackNpc));
  session.monster_defeated(defeat(2, kSinglePackNpc));
  const int limit = static_cast<int>(data->looting_range_mm() + data->rules().range_tolerance_mm);

  EXPECT_EQ(error_of(session.receive(loot_request(1), player_at(limit + 1), kStartTicks)),
            "Action_InvalidLootingRange");
  EXPECT_EQ(commit_attempts, 0);
  EXPECT_EQ(session.receive(loot_request(1), player_at(limit), kStartTicks).size(), 3U);
  EXPECT_EQ(session.receive(loot_request(2), player_at(-limit), kStartTicks).size(), 3U);

  session.monster_defeated(defeat(3, kSinglePackNpc));
  WorldPosition above{kMap, 0, 0, limit + 1, 0};
  EXPECT_EQ(error_of(session.receive(loot_request(3), above, kStartTicks)),
            "Action_InvalidLootingRange");
}

TEST_F(LootSessionTest, UnknownAndMalformedRequestsAreAnsweredWithoutSideEffects) {
  auto session = make_session();
  session.monster_defeated(defeat(kCorpse, kSinglePackNpc));
  const auto before = progress();

  const auto unknown = session.receive(loot_request(77), player_at(0), kStartTicks);
  EXPECT_EQ(error_of(unknown), "Npc_InvalidId");
  EXPECT_EQ(unknown.front().message.at("unit_id"), "77");

  for (const Json &malformed :
       {Json::object(), Json{{"unit_id", "abc"}}, Json{{"unit_id", "-1"}}, Json{{"unit_id", ""}},
        Json{{"unit_id", "12x"}}, Json{{"unit_id", 1.5}}, Json{{"unit_id", true}},
        Json{{"unit_id", "99999999999999999999999"}}}) {
    const auto rejected = session.receive(malformed, player_at(0), kStartTicks);
    EXPECT_EQ(error_of(rejected), "BadRequest") << malformed;
    EXPECT_FALSE(rejected.front().message.contains("unit_id")) << malformed;
    expect_encodes(rejected);
  }
  EXPECT_EQ(progress(), before);

  // The protobuf JSON mapping may deliver a uint64 as a number.
  const auto numeric = session.receive(Json{{"unit_id", kCorpse}}, player_at(0), kStartTicks);
  EXPECT_EQ(numeric.size(), 3U);
}

TEST_F(LootSessionTest, OnlyTheKillerMayTakeTheCorpse) {
  auto session = make_session();
  auto foreign = defeat(kCorpse, kSinglePackNpc);
  foreign.killer_character_id = character.id + 1;
  session.monster_defeated(foreign);
  const auto before = progress();

  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_ImpossibleLoot");
  EXPECT_EQ(progress(), before);
  EXPECT_EQ(commit_attempts, 0);
}

TEST_F(LootSessionTest, CorpsesWithoutLootAreRefusedAsEmpty) {
  auto session = make_session();
  session.monster_defeated(defeat(kCorpse, kUnlootedNpc));

  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_HasNoLoot");
}

TEST_F(LootSessionTest, CorpseExpiresOnTheServerDeadlineAndIsRemovedFromTheClient) {
  auto session = make_session();
  session.monster_defeated(defeat(kCorpse, kSinglePackNpc));
  const auto deadline = kStartTicks + 90000 * kTicksPerMs;
  EXPECT_EQ(session.next_action_tick(), deadline);

  EXPECT_TRUE(session.advance(deadline - 1).empty());
  // A request at the deadline is refused even before advance() has removed the corpse.
  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), deadline)),
            "Npc_InvalidId");

  const auto removed = session.advance(deadline);
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_EQ(removed.front().opcode, kLootUnitRemoveOpcode);
  EXPECT_EQ(removed.front().message, (Json{{"unit_ids", Json::array({std::to_string(kCorpse)})}}));
  expect_encodes(removed);
  EXPECT_EQ(session.corpse_count(), 0U);
  EXPECT_FALSE(session.next_action_tick().has_value());
  EXPECT_TRUE(session.advance(deadline + 1).empty());
  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), deadline + 1)),
            "Npc_InvalidId");
}

TEST_F(LootSessionTest, TakenCorpsesUseTheShortLifetimeButNeverExtendIt) {
  auto session = make_session();
  session.monster_defeated(defeat(2, kSinglePackNpc));
  session.monster_defeated(defeat(3, kSinglePackNpc));
  const auto short_lifetime = 10000 * kTicksPerMs;
  const auto long_lifetime = 90000 * kTicksPerMs;
  EXPECT_EQ(session.next_action_tick(), kStartTicks + long_lifetime);

  // Taken early: the corpse disappears 10 s later.
  const auto early = kStartTicks + 1000 * kTicksPerMs;
  ASSERT_EQ(session.receive(loot_request(2), player_at(0), early).size(), 3U);
  EXPECT_EQ(session.next_action_tick(), early + short_lifetime);
  // Taken near the end of its long lifetime: the deadline is not pushed back.
  const auto late = kStartTicks + 85000 * kTicksPerMs;
  ASSERT_EQ(session.receive(loot_request(3), player_at(0), late).size(), 3U);

  EXPECT_TRUE(session.advance(early + short_lifetime - 1).empty());
  const auto first_removal = session.advance(early + short_lifetime);
  ASSERT_EQ(first_removal.size(), 1U);
  EXPECT_EQ(first_removal.front().message.at("unit_ids"), Json::array({"2"}));
  EXPECT_EQ(session.next_action_tick(), kStartTicks + long_lifetime);

  const auto second_removal = session.advance(kStartTicks + long_lifetime);
  ASSERT_EQ(second_removal.size(), 1U);
  EXPECT_EQ(second_removal.front().message.at("unit_ids"), Json::array({"3"}));
  EXPECT_EQ(session.corpse_count(), 0U);
}

// Captured official butchering (wavesword-mob-combat-20261026): the Alpha Wolf death announced
// possible_handling_loot, an unbutchered corpse stayed 90 s, and a butchered corpse disappeared
// about 10 s after the knife was used.
TEST_F(LootSessionTest, ButcherableCorpseStaysUntilButcheredThenUsesTheShortLifetime) {
  auto session = make_session();
  session.set_butchering([](int npc_cms_id) { return npc_cms_id == kUnlootedNpc; });
  const auto short_lifetime = 10000 * kTicksPerMs;
  const auto long_lifetime = 90000 * kTicksPerMs;

  // A monster without corpse loot is still kept and announced for butchering.
  const auto reaction = session.monster_defeated(defeat(1, kUnlootedNpc));
  ASSERT_EQ(reaction.packets.size(), 1U);
  EXPECT_EQ(reaction.packets.front().message,
            (Json{{"unit_id", "1"}, {"possible_handling_loot", true}}));
  EXPECT_TRUE(reaction.manages_corpse_removal);
  expect_encodes(reaction.packets);
  EXPECT_EQ(session.next_action_tick(), kStartTicks + long_lifetime);

  const auto target = session.butcherable_corpse(1, kStartTicks + 1);
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->npc_cms_id, kUnlootedNpc);
  EXPECT_FALSE(session.butcherable_corpse(2, kStartTicks + 1).has_value());

  const auto butchered_at = kStartTicks + 5000 * kTicksPerMs;
  const auto flags = session.corpse_butchered(1, butchered_at);
  ASSERT_EQ(flags.size(), 1U);
  EXPECT_EQ(flags.front().message, (Json{{"unit_id", "1"}}));
  EXPECT_FALSE(session.butcherable_corpse(1, butchered_at).has_value());
  EXPECT_TRUE(session.corpse_butchered(1, butchered_at).empty());
  EXPECT_EQ(session.next_action_tick(), butchered_at + short_lifetime);

  const auto removed = session.advance(butchered_at + short_lifetime);
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_EQ(removed.front().message.at("unit_ids"), Json::array({"1"}));
}

TEST_F(LootSessionTest, TakingAButcherableCorpseKeepsItForTheKnife) {
  auto session = make_session();
  session.set_butchering([](int) { return true; });
  const auto reaction = session.monster_defeated(defeat(kCorpse, kSinglePackNpc));
  EXPECT_EQ(reaction.packets.front().message,
            (Json{{"unit_id", std::to_string(kCorpse)},
                  {"has_chance_to_loot", true},
                  {"possible_handling_loot", true}}));

  // The take clears only the loot flag and does not shorten the corpse lifetime.
  const auto taken_at = kStartTicks + 1000 * kTicksPerMs;
  const auto packets = session.receive(loot_request(kCorpse), player_at(0), taken_at);
  ASSERT_EQ(packets.size(), 3U);
  EXPECT_EQ(packets[1].message,
            (Json{{"unit_id", std::to_string(kCorpse)}, {"possible_handling_loot", true}}));
  EXPECT_EQ(session.next_action_tick(), kStartTicks + 90000 * kTicksPerMs);
  ASSERT_TRUE(session.butcherable_corpse(kCorpse, taken_at).has_value());

  const auto butchered_at = kStartTicks + 20000 * kTicksPerMs;
  session.corpse_butchered(kCorpse, butchered_at);
  EXPECT_EQ(session.next_action_tick(), butchered_at + 10000 * kTicksPerMs);
}

TEST_F(LootSessionTest, OnlyTheKillerMayButcherAndExpiredCorpsesAreGone) {
  auto session = make_session();
  session.set_butchering([](int) { return true; });
  auto foreign = defeat(1, kSinglePackNpc);
  foreign.killer_character_id = member.id;
  session.monster_defeated(foreign);
  EXPECT_FALSE(session.butcherable_corpse(1, kStartTicks).has_value());

  session.monster_defeated(defeat(2, kSinglePackNpc));
  const auto deadline = kStartTicks + 90000 * kTicksPerMs;
  EXPECT_TRUE(session.butcherable_corpse(2, deadline - 1).has_value());
  EXPECT_FALSE(session.butcherable_corpse(2, deadline).has_value());
}

TEST_F(LootSessionTest, ButcheringLeaseRetainsOneCorpseLifeAcrossExpiry) {
  auto session = make_session();
  session.set_butchering([](int) { return true; });
  session.monster_defeated(defeat(kCorpse, kSinglePackNpc));
  const auto target = session.butcherable_corpse(kCorpse, kStartTicks);
  ASSERT_TRUE(target);
  ASSERT_TRUE(target->handling);
  const auto token = target->handling->reserve();
  ASSERT_TRUE(token);
  EXPECT_FALSE(session.butcherable_corpse(kCorpse, kStartTicks + 1));
  EXPECT_FALSE(target->handling->reserve());

  session.advance(kStartTicks + 90000 * kTicksPerMs);
  EXPECT_EQ(session.corpse_count(), 0U);
  EXPECT_TRUE(target->handling->finish(*token, true));
  EXPECT_FALSE(target->handling->available());
  EXPECT_FALSE(target->handling->finish(*token, false));

  auto next_life = defeat(kCorpse, kSinglePackNpc, kStartTicks + 90001 * kTicksPerMs);
  session.monster_defeated(next_life);
  const auto next_target = session.butcherable_corpse(kCorpse, next_life.ticks);
  ASSERT_TRUE(next_target);
  ASSERT_TRUE(next_target->handling);
  EXPECT_NE(next_target->handling->incarnation(), target->handling->incarnation());
  EXPECT_TRUE(next_target->handling->available());
}

TEST_F(LootSessionTest, CorpsesThatCombatRemovesAreForgottenWithoutASecondRemoval) {
  auto session = make_session();
  session.monster_defeated(defeat(1, kUnlootedNpc));
  const auto short_lifetime = 10000 * kTicksPerMs;
  EXPECT_EQ(session.next_action_tick(), kStartTicks + short_lifetime);

  // Combat's own timer removes this unit; announcing it here would duplicate the removal.
  EXPECT_TRUE(session.advance(kStartTicks + short_lifetime).empty());
  EXPECT_EQ(session.corpse_count(), 0U);
}

class ThrowAfterLootNotice final : public LootAudience {
public:
  std::vector<std::int64_t> party_members_near(const MonsterDefeat &, double) override {
    ++queries;
    return {2, 3};
  }

  void notify(std::int64_t character_id, std::vector<GameplayPacket>) override {
    notices.push_back(character_id);
    if (!thrown) {
      thrown = true;
      throw std::runtime_error("Synthetic lost notification acknowledgement");
    }
  }

  int queries{};
  bool thrown{};
  std::vector<std::int64_t> notices;
};

class ThrowFirstPartyLookup final : public LootAudience {
public:
  std::vector<std::int64_t> party_members_near(const MonsterDefeat &, double) override {
    if (++queries == 1) {
      throw std::runtime_error("Synthetic party lookup failure");
    }
    return {2};
  }

  void notify(std::int64_t, std::vector<GameplayPacket>) override {
  }

  int queries{};
};

TEST_F(LootSessionTest, FailedPartyLookupRetainsRollAndFirstKillReservation) {
  auto audience = std::make_shared<ThrowFirstPartyLookup>();
  auto board = std::make_shared<SharedLootBoard>();
  auto session = make_session(profile_fixture(), LootCustomRules::all_enabled(), audience, board);
  auto first = defeat(kCorpse, kFirstKillNpc);
  first.reward_event_id = "22222222222222222222222222222222";
  EXPECT_THROW(session.monster_defeated(first), std::runtime_error);
  const auto draws = random->draws();
  EXPECT_EQ(session.corpse_count(), 1U);
  EXPECT_EQ(board->size(), 0U);

  const auto reaction = session.monster_defeated(first);
  ASSERT_TRUE(reaction.on_published);
  reaction.on_published(kStartTicks);
  EXPECT_EQ(random->draws(), draws);
  auto second = defeat(kCorpse + 1, kFirstKillNpc);
  second.reward_event_id = "33333333333333333333333333333333";
  const auto second_reaction = session.monster_defeated(second);
  ASSERT_TRUE(second_reaction.on_published);
  second_reaction.on_published(kStartTicks);
  const auto first_claim = board->claim({character.id, kCorpse}, character.id, kStartTicks);
  const auto second_claim = board->claim({character.id, kCorpse + 1}, character.id, kStartTicks);
  ASSERT_EQ(first_claim.status, ClaimStatus::claimed);
  ASSERT_EQ(second_claim.status, ClaimStatus::claimed);
  EXPECT_TRUE(first_claim.corpse->records_first_kill);
  EXPECT_FALSE(second_claim.corpse->records_first_kill);
}

TEST_F(LootSessionTest, FailedNoticeResumesSameCorpseAndFirstKillRollWithoutDuplicateNotice) {
  auto audience = std::make_shared<ThrowAfterLootNotice>();
  auto board = std::make_shared<SharedLootBoard>();
  auto session = make_session(profile_fixture(), LootCustomRules::all_enabled(), audience, board);
  auto event = defeat(kCorpse, kFirstKillNpc);
  event.reward_event_id = "11111111111111111111111111111111";

  const auto reaction = session.monster_defeated(event);
  ASSERT_TRUE(reaction.on_published);
  EXPECT_TRUE(reaction.manages_corpse_removal);
  ASSERT_EQ(reaction.packets.size(), 1U);
  const auto draws = random->draws();
  EXPECT_EQ(session.corpse_count(), 1U);
  EXPECT_EQ(board->size(), 1U);
  EXPECT_EQ(board->claim({character.id, kCorpse}, character.id, kStartTicks).status,
            ClaimStatus::unknown);
  EXPECT_THROW(reaction.on_published(kStartTicks), std::runtime_error);
  EXPECT_TRUE(session.advance(kStartTicks + 200000 * kTicksPerMs).empty());
  EXPECT_EQ(session.corpse_count(), 1U);

  event.ticks += 200000 * kTicksPerMs;
  reaction.on_published(event.ticks);
  EXPECT_EQ(random->draws(), draws);
  EXPECT_EQ(audience->queries, 1);
  EXPECT_EQ(audience->notices, (std::vector<std::int64_t>{2, 3}));
  EXPECT_EQ(board->size(), 1U);
  EXPECT_EQ(session.corpse_count(), 1U);
  EXPECT_EQ(board->claim({character.id, kCorpse}, character.id, event.ticks).status,
            ClaimStatus::claimed);
  EXPECT_EQ(board->claim({character.id, kCorpse}, 2, event.ticks).status,
            ClaimStatus::already_taken);
  reaction.on_published(event.ticks);
  EXPECT_TRUE(session.monster_defeated(event).packets.empty());
  EXPECT_EQ(audience->notices.size(), 2U);
}

TEST_F(LootSessionTest, LostCommitAcknowledgementReconcilesBeforeInventoryPlanning) {
  auto storage = persistence();
  std::map<std::string, std::string> receipts;
  bool fail_after_commit = true;
  bool forbid_character_load = false;
  int reconnect_requests = 0;
  storage.force_reconnect = [&] { ++reconnect_requests; };
  const auto load_character = storage.load_character;
  storage.load_character = [&] {
    if (forbid_character_load) {
      throw std::runtime_error("Inventory must not be replanned after a committed receipt");
    }
    return load_character();
  };
  storage.lookup_claim = [&](const std::string &id, const std::string &fingerprint) {
    const auto receipt = receipts.find(id);
    if (receipt == receipts.end()) {
      return LootClaimResult{LootClaimStatus::not_found, {}};
    }
    EXPECT_EQ(receipt->second, fingerprint);
    return LootClaimResult{LootClaimStatus::already_claimed_by_same_character, {}};
  };
  storage.commit_once = [&](const LootCommit &commit) {
    const auto result = store.character_state->commit_item_trade(
        character.account_id, character.id, character.world_id, commit.before, commit.after,
        ItemTrade{commit.granted_items, {}, {}});
    EXPECT_TRUE(result);
    receipts.emplace(commit.receipt_id, commit.corpse_fingerprint);
    if (fail_after_commit) {
      fail_after_commit = false;
      throw std::runtime_error("Synthetic lost committed notification");
    }
    return LootClaimResult{LootClaimStatus::committed, *result};
  };
  storage.commit = {};
  auto loot_data = std::make_shared<const LootData>(profile_fixture());
  auto randomizer = std::make_unique<ScriptedLootRandom>();
  LootSession session(loot_data, character.id, std::move(storage), std::move(randomizer));
  session.monster_defeated(defeat(kCorpse, kSinglePackNpc));

  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_FailLootTransaction");
  const auto committed = progress();
  ASSERT_TRUE(committed.stackables.contains(kHerb));
  EXPECT_EQ(reconnect_requests, 1);
  forbid_character_load = true;
  const auto retry = session.receive(loot_request(kCorpse), player_at(0), kStartTicks);
  ASSERT_EQ(retry.size(), 2U);
  EXPECT_FALSE(retry.back().message.contains("ec"));
  EXPECT_EQ(progress(), committed);
  EXPECT_EQ(reconnect_requests, 2);
  EXPECT_EQ(receipts.size(), 1U);
}

TEST_F(LootSessionTest, SharedLostAcknowledgementCannotAwardAnotherClaimant) {
  auto audience = std::make_shared<FakeAudience>();
  audience->members = {member.id};
  auto board = std::make_shared<SharedLootBoard>();
  auto loot_data = std::make_shared<const LootData>(profile_fixture());
  struct Receipt {
    std::int64_t claimant{};
    std::string fingerprint;
  };
  std::map<std::string, Receipt> receipts;
  bool fail_after_commit = true;
  auto durable_storage = [&](const Character &player) {
    auto storage = persistence_for(player);
    const auto claimant = player.id;
    storage.lookup_claim = [&, claimant](const std::string &id, const std::string &fingerprint) {
      const auto receipt = receipts.find(id);
      if (receipt == receipts.end()) {
        return LootClaimResult{LootClaimStatus::not_found, {}};
      }
      if (receipt->second.fingerprint != fingerprint) {
        return LootClaimResult{LootClaimStatus::mismatch, {}};
      }
      const auto status = receipt->second.claimant == claimant
                              ? LootClaimStatus::already_claimed_by_same_character
                              : LootClaimStatus::claimed_by_other_character;
      return LootClaimResult{status, {}};
    };
    storage.commit_once = [&, claimant](const LootCommit &commit) {
      auto result = store.character_state->commit_item_trade(
          character.account_id, claimant, character.world_id, commit.before, commit.after,
          ItemTrade{commit.granted_items, {}, {}});
      EXPECT_TRUE(result);
      receipts.emplace(commit.receipt_id, Receipt{claimant, commit.corpse_fingerprint});
      if (fail_after_commit) {
        fail_after_commit = false;
        throw std::runtime_error("Synthetic lost committed notification");
      }
      return LootClaimResult{LootClaimStatus::committed, *result};
    };
    storage.commit = {};
    return storage;
  };
  LootSession owner(loot_data, character.id, durable_storage(character),
                    std::make_unique<ScriptedLootRandom>(), audience, board);
  LootSession other(loot_data, member.id, durable_storage(member),
                    std::make_unique<ScriptedLootRandom>(), audience, board);
  owner.monster_defeated(defeat(kCorpse, kSinglePackNpc));

  EXPECT_EQ(error_of(owner.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_FailLootTransaction");
  const auto owner_after = progress_of(character);
  const auto other_before = progress_of(member);
  EXPECT_EQ(error_of(other.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_HasNoLoot");
  EXPECT_EQ(progress_of(character), owner_after);
  EXPECT_EQ(progress_of(member), other_before);
  EXPECT_EQ(receipts.size(), 1U);
}

TEST_F(LootSessionTest, ARepeatedDefeatNeitherRerollsNorExtendsTheCorpse) {
  auto session = make_session();
  ASSERT_EQ(session.monster_defeated(defeat(kCorpse, kSinglePackNpc)).packets.size(), 1U);
  const auto draws = random->draws();
  const auto deadline = session.next_action_tick();

  const auto repeated = session.monster_defeated(defeat(kCorpse, kSinglePackNpc, kStartTicks + 5));
  EXPECT_TRUE(repeated.packets.empty());
  EXPECT_FALSE(repeated.manages_corpse_removal);

  EXPECT_EQ(random->draws(), draws);
  EXPECT_EQ(session.next_action_tick(), deadline);
  EXPECT_EQ(session.corpse_count(), 1U);
}

TEST_F(LootSessionTest, DefeatsWithoutAUnitOrTickAreIgnored) {
  auto session = make_session();

  EXPECT_TRUE(session.monster_defeated(defeat(0, kSinglePackNpc)).packets.empty());
  EXPECT_TRUE(session.monster_defeated(defeat(kCorpse, kSinglePackNpc, 0)).packets.empty());
  EXPECT_EQ(session.corpse_count(), 0U);
  EXPECT_THROW(session.monster_defeated(
                   defeat(kCorpse, kSinglePackNpc, std::numeric_limits<std::int64_t>::max())),
               std::invalid_argument);
}

TEST_F(LootSessionTest, TrackedCorpsesAreBounded) {
  auto session = make_session();
  for (std::size_t index = 1; index <= kMaximumTrackedCorpses + 5; ++index) {
    session.monster_defeated(defeat(index, kUnlootedNpc));
  }

  EXPECT_EQ(session.corpse_count(), kMaximumTrackedCorpses);
  EXPECT_EQ(error_of(session.receive(loot_request(kMaximumTrackedCorpses + 1), player_at(0),
                                     kStartTicks)),
            "Npc_InvalidId");
}

TEST_F(LootSessionTest, FullInventoryRefusesInstancesAndKeepsTheLootForALaterTake) {
  auto session = make_session();
  session.monster_defeated(defeat(kCorpse, kFullPackNpc));
  const auto before = progress();

  auto crowded = *persistence().load_character();
  crowded.items.clear();
  for (int slot = 0; slot <= kMaximumInventorySlot; ++slot) {
    CharacterItem item;
    item.cms_id = kHerb;
    item.inventory_slot = slot;
    crowded.items.push_back(item);
  }
  forced_character = crowded;

  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "ItemSlotLimit");
  EXPECT_EQ(progress(), before);
  EXPECT_EQ(commit_attempts, 0);

  forced_character.reset();
  EXPECT_EQ(session.receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);
}

TEST_F(LootSessionTest, OverflowingCountsAreRefusedAndGoldStopsAtTheWalletLimit) {
  auto session = make_session();
  random->quantities = {4, 9, 1};
  session.monster_defeated(defeat(kCorpse, kFullPackNpc));

  update_progress(
      [](WorldProgress &state) { state.stackables[kHerb] = std::numeric_limits<int>::max() - 3; });
  auto before = progress();
  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "ItemCountError");
  EXPECT_EQ(progress(), before);

  // Progress records are tombstoned rather than deleted, so lower the stack instead.
  // CUSTOM: gold above the wallet limit stays behind while the items are still taken.
  update_progress([](WorldProgress &state) {
    state.stackables[kHerb] = 1;
    state.gold = kGoldLimit - 8;
  });
  const auto packets = session.receive(loot_request(kCorpse), player_at(0), kStartTicks);
  ASSERT_EQ(packets.size(), 3U);
  EXPECT_EQ(progress().gold, kGoldLimit);
  EXPECT_EQ(progress().stackables.at(kHerb), 5);
  EXPECT_EQ(packets[0].message.at("money_changes"),
            (Json{{{"change", "8"}, {"result", std::to_string(kGoldLimit)}}}));
}

TEST_F(LootSessionTest, ANewStackIsRefusedWhenEveryStackKindIsInUse) {
  update_progress([](WorldProgress &state) {
    for (std::size_t index = 0; index < kMaximumStackableKinds; ++index) {
      state.stackables[1000 + static_cast<int>(index)] = 1;
    }
  });
  auto session = make_session();
  session.monster_defeated(defeat(kCorpse, kSinglePackNpc));
  const auto before = progress();

  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "ItemSlotLimit");
  EXPECT_EQ(progress(), before);
  EXPECT_EQ(commit_attempts, 0);
}

TEST_F(LootSessionTest, WeightPolicyCanRefuseATakeAndLaterAllowIt) {
  bool allowed = false;
  weight_policy = [&](const LootCommit &commit) {
    // The policy sees the planned result, not the unmodified state.
    EXPECT_EQ(commit.after.revision, commit.before.revision + 1);
    EXPECT_EQ(commit.granted_items.size(), 1U);
    return allowed;
  };
  auto session = make_session();
  session.monster_defeated(defeat(kCorpse, kFullPackNpc));
  const auto before = progress();

  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_LootOverMaxWeight");
  EXPECT_EQ(progress(), before);
  EXPECT_EQ(commit_attempts, 0);

  allowed = true;
  EXPECT_EQ(session.receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);
}

TEST_F(LootSessionTest, ConcurrentWriterConflictFailsTheTakeAndLeavesItRetryable) {
  auto session = make_session();
  random->quantities = {2, 5, 1};
  session.monster_defeated(defeat(kCorpse, kFullPackNpc));
  before_commit = [this] { update_progress([](WorldProgress &state) { state.gold = 100; }); };

  const auto conflicted = session.receive(loot_request(kCorpse), player_at(0), kStartTicks);

  EXPECT_EQ(error_of(conflicted), "Npc_FailLootTransaction");
  EXPECT_EQ(progress().gold, 100);
  EXPECT_FALSE(progress().stackables.contains(kHerb));
  EXPECT_EQ(std::ranges::count_if(stored_items(),
                                  [](const CharacterItem &item) { return item.cms_id == kSword; }),
            0);

  before_commit = nullptr;
  const auto retried = session.receive(loot_request(kCorpse), player_at(0), kStartTicks);
  ASSERT_EQ(retried.size(), 3U);
  EXPECT_EQ(retried.front().message.at("money_changes").at(0).at("result"), "105");
  EXPECT_EQ(progress().gold, 105);
}

TEST_F(LootSessionTest, StorageExceptionsAreReportedAsTransactionFailures) {
  auto session = make_session();
  session.monster_defeated(defeat(kCorpse, kSinglePackNpc));
  before_commit = [] { throw std::runtime_error("storage offline"); };

  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_FailLootTransaction");
  EXPECT_FALSE(progress().stackables.contains(kHerb));

  before_commit = nullptr;
  EXPECT_EQ(session.receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);
  EXPECT_EQ(progress().stackables.at(kHerb), 1);
}

TEST_F(LootSessionTest, MissingCharacterFailsTheTransaction) {
  // The session's storage no longer knows this character.
  character.id += 1000;
  auto session = make_session();
  session.monster_defeated(defeat(kCorpse, kSinglePackNpc));

  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_FailLootTransaction");
}

TEST_F(LootSessionTest, ConstructionRequiresDataIdentityRandomnessAndStorage) {
  const auto data_ptr = std::make_shared<const LootData>(profile_fixture());
  const auto build = [&](std::shared_ptr<const LootData> loot, std::int64_t id,
                         LootPersistence storage, bool with_random) {
    return LootSession(std::move(loot), id, std::move(storage),
                       with_random ? std::make_unique<ScriptedLootRandom>() : nullptr);
  };

  EXPECT_NO_THROW(build(data_ptr, 1, persistence(), true));
  EXPECT_THROW(build(nullptr, 1, persistence(), true), std::invalid_argument);
  EXPECT_THROW(build(data_ptr, 0, persistence(), true), std::invalid_argument);
  EXPECT_THROW(build(data_ptr, 1, persistence(), false), std::invalid_argument);
  auto missing_commit = persistence();
  missing_commit.commit = nullptr;
  EXPECT_THROW(build(data_ptr, 1, std::move(missing_commit), true), std::invalid_argument);
}

TEST(LootData, ExtractedProfileRollsEveryModeledNpcWithinTheStorageBounds) {
  const auto *path = std::getenv("CHRONICLES_LOOT_TEST_PROFILE");
  if (!path) {
    GTEST_SKIP() << "Run explicitly against the generated local loot profile";
  }
  const auto extracted = LootData::load(path);
  EXPECT_GT(extracted->npc_count(), 3000U);
  EXPECT_EQ(extracted->withheld_group_count(), 0U);

  // Tutorial Skeleton Scout: pack 3105051 drops only stackable materials, and its level 2
  // Normal template pays the captured 6 gold.
  const auto *scout = extracted->npc(5001010);
  ASSERT_NE(scout, nullptr);
  EXPECT_EQ(scout->pack_id, 3105051);
  EXPECT_EQ(scout->kill_gold, 6);

  std::ifstream input(path);
  const auto document = Json::parse(input);
  SeededLootRandom seeded(2026);
  for (const auto &row : document.at("npcs")) {
    const auto *npc = extracted->npc(row.at(0).get<int>());
    ASSERT_NE(npc, nullptr);
    for (int trial = 0; trial < 10; ++trial) {
      LootRollContext context;
      context.first_kill_available = trial % 2 == 0;
      const auto rolled = roll_corpse(*extracted, *npc, seeded, context);
      EXPECT_LE(rolled.contents.instances.size(), kMaximumLootInstancesPerCorpse);
    }
  }

  // Every group that the builder tags is loaded with the rules enabled and withheld without.
  const LootData evidence_only(document, LootCustomRules::evidence_only());
  EXPECT_GT(evidence_only.withheld_group_count(), 0U);
  EXPECT_LT(evidence_only.npc_count(), extracted->npc_count());
}

TEST(LootTutorialPersistence, CommitIsAdoptedByTheTutorialSessionAndRefreshesTheItemCache) {
  MemoryRepositories store;
  AuthService auth(store.character_repositories());
  Character character;
  character.account_id = 1;
  character.name = "TutorialLoot";
  character = store.characters->create(character);
  Handoff handoff;
  handoff.account_id = character.account_id;
  handoff.character_id = character.id;
  handoff.world_id = 1;

  const auto tutorial_data =
      std::make_shared<TutorialData>(Json{{"format_version", 1},
                                          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
                                          {"map_id", 8000},
                                          {"objects", Json::array()},
                                          {"doodads", Json::array()},
                                          {"utility_items", Json::array()},
                                          {"civilians", Json::array()},
                                          {"sectors", {1}},
                                          {"npc_spawns", Json::array()},
                                          {"quests", Json::array()}});
  TutorialSession tutorial(
      tutorial_data, character.id, store.character_state->load_world_progress(1, character.id, 1),
      [&](const WorldProgress &before, const WorldProgress &after) {
        return store.character_state->save_world_progress(1, character.id, 1, before, after);
      });

  std::vector<Character> refreshed;
  auto persistence = tutorial_loot_persistence(
      auth, handoff, tutorial, [&](const Character &live) { refreshed.push_back(live); });
  const auto data = std::make_shared<const LootData>(profile_fixture());
  auto scripted = std::make_unique<ScriptedLootRandom>();
  scripted->quantities = {3, 6, 1};
  LootSession session(data, character.id, std::move(persistence), std::move(scripted));

  MonsterDefeat defeat;
  defeat.unit_id = kCorpse;
  defeat.npc_cms_id = kFullPackNpc;
  defeat.killer_character_id = character.id;
  defeat.position = {kMap, 0, 0, 0, 0};
  defeat.ticks = kStartTicks;
  session.monster_defeated(defeat);

  const auto packets = session.receive(loot_request(kCorpse), {kMap, 0, 0, 0, 0}, kStartTicks);

  ASSERT_EQ(packets.size(), 3U);
  const auto stored = store.character_state->load_world_progress(1, character.id, 1);
  EXPECT_EQ(stored.gold, 6);
  EXPECT_EQ(stored.stackables.at(kHerb), 3);
  // The tutorial session holds the committed revision, so its next write is not a conflict.
  EXPECT_EQ(tutorial.progress(), stored);
  ASSERT_EQ(refreshed.size(), 1U);
  EXPECT_EQ(std::ranges::count_if(refreshed.front().items,
                                  [](const CharacterItem &item) { return item.cms_id == kSword; }),
            1);
  EXPECT_EQ(refreshed.front().items.size(), character.items.size() + 1);

  // A stack-only take changes progress but not the item cache.
  MonsterDefeat second = defeat;
  second.unit_id = kCorpse + 1;
  second.npc_cms_id = kSinglePackNpc;
  session.monster_defeated(second);
  ASSERT_EQ(session.receive(loot_request(kCorpse + 1), {kMap, 0, 0, 0, 0}, kStartTicks).size(), 3U);
  EXPECT_EQ(refreshed.size(), 1U);
  EXPECT_EQ(tutorial.progress(), store.character_state->load_world_progress(1, character.id, 1));
  EXPECT_EQ(tutorial.progress().stackables.at(kHerb), 4);
}

} // namespace
} // namespace chronicles
