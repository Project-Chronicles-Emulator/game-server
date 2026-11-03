// Tests for the CUSTOM (guessed, not official) loot rules: configuration, kill gold, first-kill
// groups, quest gates, polished and full moon stone rows, handling loot, party sharing and the
// formalized details. docs/monster-loot.md lists every guess.
#include "world/features/loot/loot.hpp"

#include "testing/loot_test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

using namespace loot_test;

LootCustomRules rules_without(LootRule rule) {
  auto json = Json::object();
  json[std::string(loot_rule_name(rule))] = false;
  return LootCustomRules::from_json(json);
}

// --- Configuration -------------------------------------------------------------------------

TEST(LootCustomRules, DefaultsEnableEveryRuleAndUseTheNamedGuesses) {
  const auto rules = LootCustomRules::all_enabled();

  for (const auto rule :
       {LootRule::kill_gold, LootRule::first_kill_rewards, LootRule::quest_gated_rows,
        LootRule::full_moon_stones, LootRule::polish_step_rows, LootRule::handling_loot,
        LootRule::party_shared_loot}) {
    EXPECT_TRUE(rules.enabled(rule)) << loot_rule_name(rule);
  }
  EXPECT_EQ(rules.active_rules().size(), 7U);
  EXPECT_DOUBLE_EQ(rules.range_tolerance_mm, kGuessedRangeToleranceMm);
  EXPECT_DOUBLE_EQ(rules.party_range_mm, kGuessedPartyRangeMm);
  EXPECT_EQ(rules.repeated_take_error, x5::ec::Npc_HasNoLoot);
  EXPECT_EQ(rules.full_bag_error, x5::ec::ItemSlotLimit);
  EXPECT_EQ(rules.count_limit_error, x5::ec::ItemCountError);
  EXPECT_EQ(rules.over_weight_error, x5::ec::Npc_LootOverMaxWeight);
  EXPECT_EQ(rules.empty_roll_reply, EmptyRollReply::success);
  EXPECT_EQ(rules.emptied_lifetime_anchor, EmptiedLifetimeAnchor::take);
  EXPECT_FALSE(rules.lootable_lifetime_ms.has_value());
  EXPECT_FALSE(rules.emptied_lifetime_ms.has_value());
  // An empty document keeps every default.
  const auto parsed = LootCustomRules::from_json(Json::object());
  EXPECT_EQ(parsed.active_rules().size(), 7U);
  EXPECT_DOUBLE_EQ(parsed.range_tolerance_mm, kGuessedRangeToleranceMm);
}

TEST(LootCustomRules, EvidenceOnlySwitchesEveryRuleAndTheToleranceOff) {
  const auto rules = LootCustomRules::evidence_only();

  EXPECT_TRUE(rules.active_rules().empty());
  EXPECT_DOUBLE_EQ(rules.range_tolerance_mm, 0.0);
  EXPECT_NE(rules.describe().find("none (evidence-only)"), std::string::npos);
}

TEST(LootCustomRules, FromJsonReadsEverySwitchAndDetail) {
  const Json document{{"kill_gold", false},
                      {"first_kill_rewards", false},
                      {"quest_gated_rows", false},
                      {"full_moon_stones", false},
                      {"polish_step_rows", false},
                      {"handling_loot", false},
                      {"party_shared_loot", false},
                      {"range_tolerance_mm", 500},
                      {"party_range_mm", 12000.5},
                      {"repeated_take_error", "Npc_ImpossibleLoot"},
                      {"full_bag_error", "ItemWeightLimit"},
                      {"count_limit_error", "ItemSlotLimit"},
                      {"over_weight_error", "ItemNotEnoughFreeWeight"},
                      {"empty_roll_reply", "no_loot_error"},
                      {"emptied_lifetime_anchor", "death"},
                      {"lootable_lifetime_ms", 60000},
                      {"emptied_lifetime_ms", 5000}};

  const auto rules = LootCustomRules::from_json(document);

  EXPECT_TRUE(rules.active_rules().empty());
  EXPECT_DOUBLE_EQ(rules.range_tolerance_mm, 500.0);
  EXPECT_DOUBLE_EQ(rules.party_range_mm, 12000.5);
  EXPECT_EQ(rules.repeated_take_error, x5::ec::Npc_ImpossibleLoot);
  EXPECT_EQ(rules.full_bag_error, x5::ec::ItemWeightLimit);
  EXPECT_EQ(rules.count_limit_error, x5::ec::ItemSlotLimit);
  EXPECT_EQ(rules.over_weight_error, x5::ec::ItemNotEnoughFreeWeight);
  EXPECT_EQ(rules.empty_roll_reply, EmptyRollReply::no_loot_error);
  EXPECT_EQ(rules.emptied_lifetime_anchor, EmptiedLifetimeAnchor::death);
  EXPECT_EQ(rules.lootable_lifetime_ms, 60000);
  EXPECT_EQ(rules.emptied_lifetime_ms, 5000);
}

TEST(LootCustomRules, FromJsonRejectsUnknownKeysAndBadValues) {
  const auto rejected = [](const char *key, const Json &value) {
    Json document = Json::object();
    document[key] = value;
    EXPECT_THROW(LootCustomRules::from_json(document), std::invalid_argument)
        << key << "=" << value;
  };

  EXPECT_THROW(LootCustomRules::from_json(Json::array()), std::invalid_argument);
  rejected("unknown_rule", true);
  rejected("kill_gold", "yes");
  rejected("kill_gold", 1);
  rejected("range_tolerance_mm", -1);
  rejected("range_tolerance_mm", "far");
  rejected("range_tolerance_mm", 1.0e9);
  rejected("party_range_mm", -5);
  rejected("repeated_take_error", "Success");
  rejected("repeated_take_error", "NoSuchError");
  rejected("repeated_take_error", 6005);
  rejected("full_bag_error", "");
  rejected("empty_roll_reply", "maybe");
  rejected("empty_roll_reply", 1);
  rejected("emptied_lifetime_anchor", "birth");
  rejected("lootable_lifetime_ms", 0);
  rejected("lootable_lifetime_ms", 90000.5);
  rejected("emptied_lifetime_ms", -3);
  rejected("emptied_lifetime_ms", 1000LL * 60 * 60 * 25);
}

TEST(LootCustomRules, LoadReadsTheRulesFileAndRejectsOtherShapes) {
  const auto directory = std::filesystem::temp_directory_path() / "chronicles-loot-rules-test";
  std::filesystem::create_directories(directory);
  const auto write = [&](const std::string &name, const std::string &text) {
    const auto path = directory / name;
    std::ofstream(path, std::ios::binary) << text;
    return path;
  };

  const auto valid =
      write("valid.json", R"({"custom_rules": {"kill_gold": false, "range_tolerance_mm": 100}})");
  const auto rules = LootCustomRules::load(valid);
  EXPECT_FALSE(rules.kill_gold);
  EXPECT_TRUE(rules.first_kill_rewards);
  EXPECT_DOUBLE_EQ(rules.range_tolerance_mm, 100.0);

  EXPECT_THROW(LootCustomRules::load(write("flat.json", R"({"kill_gold": false})")),
               std::invalid_argument);
  EXPECT_THROW(LootCustomRules::load(write("extra.json", R"({"custom_rules": {}, "x": 1})")),
               std::invalid_argument);
  EXPECT_THROW(LootCustomRules::load(write("broken.json", "{not json")), std::invalid_argument);
  EXPECT_THROW(LootCustomRules::load(write("unknown.json", R"({"custom_rules": {"nope": 1}})")),
               std::invalid_argument);
  EXPECT_ANY_THROW(LootCustomRules::load(directory / "missing.json"));
  std::filesystem::remove_all(directory);
}

TEST(LootCustomRules, DescribeNamesEveryActiveRuleAndGuessedDetail) {
  const auto text = LootCustomRules::all_enabled().describe();

  for (const char *expected :
       {"guessed, not official", "kill_gold", "first_kill_rewards", "quest_gated_rows",
        "full_moon_stones", "polish_step_rows", "handling_loot", "party_shared_loot",
        "range_tolerance_mm=2000", "repeated_take_error=Npc_HasNoLoot",
        "full_bag_error=ItemSlotLimit", "count_limit_error=ItemCountError",
        "over_weight_error=Npc_LootOverMaxWeight", "empty_roll_reply=success",
        "emptied_lifetime_anchor=take", "lootable_lifetime_ms=catalog", "party_range_mm=30000"}) {
    EXPECT_NE(text.find(expected), std::string::npos) << expected << " in " << text;
  }
  EXPECT_EQ(rules_without(LootRule::kill_gold).describe().find("kill_gold"), std::string::npos);
}

TEST(LootCustomRules, RuleNamesRoundTripAndUnknownNamesThrow) {
  for (const auto rule : LootCustomRules::all_enabled().active_rules()) {
    EXPECT_EQ(parse_loot_rule(loot_rule_name(rule)), rule);
  }
  EXPECT_THROW(parse_loot_rule("not_a_rule"), std::runtime_error);
}

// --- Rules applied to the profile ------------------------------------------------------------

TEST(LootCustomData, EachSwitchWithholdsExactlyItsOwnGroupsAndFacts) {
  const auto profile = profile_fixture();

  const LootData no_gold(profile, rules_without(LootRule::kill_gold));
  EXPECT_EQ(no_gold.npc(kGoldOnlyNpc), nullptr);
  EXPECT_EQ(no_gold.withheld_group_count(), 0U);

  const LootData no_first_kill(profile, rules_without(LootRule::first_kill_rewards));
  EXPECT_EQ(no_first_kill.pack(kFirstKillPack)->groups.size(), 1U);
  EXPECT_FALSE(no_first_kill.pack(kFirstKillPack)->has_first_kill_group);
  EXPECT_EQ(no_first_kill.withheld_group_count(), 1U);

  const LootData no_quest(profile, rules_without(LootRule::quest_gated_rows));
  EXPECT_EQ(no_quest.pack(kQuestPack), nullptr);
  EXPECT_EQ(no_quest.npc(kQuestNpc), nullptr);

  const LootData no_polish(profile, rules_without(LootRule::polish_step_rows));
  EXPECT_EQ(no_polish.pack(kPolishPack), nullptr);
  EXPECT_EQ(no_polish.npc(kPolishNpc), nullptr);

  const LootData no_stones(profile, rules_without(LootRule::full_moon_stones));
  EXPECT_EQ(no_stones.pack(kStonePack), nullptr);
  EXPECT_EQ(no_stones.npc(kStoneNpc), nullptr);

  const LootData no_handling(profile, rules_without(LootRule::handling_loot));
  EXPECT_EQ(no_handling.handling_pack(kHandlingPack), nullptr);
  EXPECT_EQ(no_handling.npc(kHandlingOnlyNpc), nullptr);
  ASSERT_NE(no_handling.npc(kHandlingNpc), nullptr);
  EXPECT_EQ(no_handling.npc(kHandlingNpc)->handling_pack_id, 0);
  EXPECT_NE(no_handling.npc(kHandlingNpc)->pack_id, 0);

  // Party sharing only changes session behavior, not the profile.
  const LootData no_party(profile, rules_without(LootRule::party_shared_loot));
  EXPECT_EQ(no_party.npc_count(), LootData(profile).npc_count());
}

TEST(LootCustomData, EvidenceOnlyKeepsOnlyUntaggedGroups) {
  const LootData data(profile_fixture(), LootCustomRules::evidence_only());

  EXPECT_EQ(data.withheld_group_count(), 5U);
  EXPECT_EQ(data.npc_count(), 5U);
  EXPECT_EQ(data.pack_count(), 4U);
  EXPECT_EQ(data.npc(kGoldOnlyNpc), nullptr);
  EXPECT_EQ(data.handling_pack(kHandlingPack), nullptr);
  EXPECT_DOUBLE_EQ(data.rules().range_tolerance_mm, 0.0);
}

TEST(LootCustomData, LifetimeOverridesReplaceTheCatalogValues) {
  auto rules = LootCustomRules::all_enabled();
  rules.lootable_lifetime_ms = 5000;
  rules.emptied_lifetime_ms = 2000;

  const LootData data(profile_fixture(), rules);

  EXPECT_EQ(data.corpse_lootable_ms(), 5000);
  EXPECT_EQ(data.corpse_empty_ms(), 2000);
}

// --- Rolls -------------------------------------------------------------------------------------

TEST(LootCustomRoll, FirstKillGroupsAreSkippedUnlessTheContextAllowsThem) {
  const LootData data(profile_fixture());
  const auto &pack = *data.pack(kFirstKillPack);

  ScriptedLootRandom blocked;
  const auto without = roll_loot(data, pack, blocked, LootRollContext{});
  EXPECT_EQ(without.stackables, (std::map<int, std::int64_t>{{kHerb, 1}}));
  // The skipped group draws nothing: pack rate, group rate, pick and quantity of one group.
  EXPECT_EQ(blocked.draws(), 4U);

  ScriptedLootRandom allowed;
  LootRollContext context;
  context.first_kill_available = true;
  const auto with = roll_loot(data, pack, allowed, context);
  EXPECT_EQ(with.stackables, (std::map<int, std::int64_t>{{kHerb, 1}, {kStone, 1}}));
}

TEST(LootCustomRoll, QuestGatedItemsAreAlternativesOnlyWhileTheirQuestIsActive) {
  const LootData data(profile_fixture());
  const auto &pack = *data.pack(kQuestPack);
  const auto weights_for = [&](const WorldProgress *progress) {
    ScriptedLootRandom random;
    LootRollContext context;
    context.progress = progress;
    roll_loot(data, pack, random, context);
    return random.weights_seen.empty() ? std::vector<int>{} : random.weights_seen.front();
  };

  WorldProgress progress;
  EXPECT_EQ(weights_for(nullptr), (std::vector<int>{1}));
  EXPECT_EQ(weights_for(&progress), (std::vector<int>{1}));

  progress.quests[kQuestId] = QuestProgress{};
  EXPECT_EQ(weights_for(&progress), (std::vector<int>{1, 1}));

  progress.quests[kQuestId].complete = true;
  EXPECT_EQ(weights_for(&progress), (std::vector<int>{1}));
}

TEST(LootCustomRoll, AGroupWithNoAvailableAlternativeDropsNothingAndDrawsNoPick) {
  auto profile = profile_fixture();
  profile["packs"][4]["groups"][0]["entries"] = Json::array({entry("item", kQuestItem, 1, 1)});
  const LootData data(profile);
  ScriptedLootRandom random;

  const auto contents = roll_loot(data, *data.pack(kQuestPack), random, LootRollContext{});

  EXPECT_TRUE(contents.empty());
  EXPECT_TRUE(random.weights_seen.empty());
}

TEST(LootCustomRoll, CorpseRollAddsKillGoldThenPackThenHandlingInOrder) {
  const LootData data(profile_fixture());
  ScriptedLootRandom random;

  const auto rolled = roll_corpse(data, *data.npc(kHandlingNpc), random, LootRollContext{});

  EXPECT_TRUE(rolled.handling_loot);
  EXPECT_EQ(rolled.contents.stackables, (std::map<int, std::int64_t>{{kHerb, 3}}));
  // Loot pack (rate, group), the handling rate, then the handling pack (rate, group).
  EXPECT_EQ(random.rates_seen, (std::vector<double>{100.0, 100.0, 50.0, 100.0, 100.0}));

  ScriptedLootRandom gold_random;
  const auto gold = roll_corpse(data, *data.npc(kGoldOnlyNpc), gold_random, LootRollContext{});
  EXPECT_EQ(gold.contents.gold, kKillGold);
  EXPECT_EQ(gold_random.draws(), 0U);
  EXPECT_FALSE(gold.handling_loot);
  EXPECT_FALSE(gold.consumes_first_kill);
}

TEST(LootCustomRoll, ConsumesTheFirstKillOnlyWhenTheContextAllowsAFirstKillGroup) {
  const LootData data(profile_fixture());
  LootRollContext allowed;
  allowed.first_kill_available = true;

  ScriptedLootRandom first;
  EXPECT_TRUE(roll_corpse(data, *data.npc(kFirstKillNpc), first, allowed).consumes_first_kill);
  ScriptedLootRandom second;
  EXPECT_FALSE(
      roll_corpse(data, *data.npc(kFirstKillNpc), second, LootRollContext{}).consumes_first_kill);
  ScriptedLootRandom plain;
  EXPECT_FALSE(roll_corpse(data, *data.npc(kFullPackNpc), plain, allowed).consumes_first_kill);
}

// --- Kill gold ---------------------------------------------------------------------------------

TEST_F(LootSessionTest, KillGoldNpcWithoutAPackIsLootableAndPaysTheFittedGold) {
  update_progress([](WorldProgress &state) { state.gold = 3; });
  auto session = make_session();

  const auto reaction = session.monster_defeated(defeat(kCorpse, kGoldOnlyNpc));
  ASSERT_EQ(reaction.packets.size(), 1U);
  EXPECT_EQ(reaction.packets.front().message,
            (Json{{"unit_id", std::to_string(kCorpse)}, {"has_chance_to_loot", true}}));
  EXPECT_EQ(random->draws(), 0U);

  const auto packets = session.receive(loot_request(kCorpse), player_at(0), kStartTicks);
  ASSERT_EQ(packets.size(), 3U);
  const auto &update = packets[0].message;
  EXPECT_EQ(update.at("money_changes"), (Json{{{"change", "7"}, {"result", "10"}}}));
  EXPECT_FALSE(update.contains("stackables"));
  EXPECT_FALSE(update.contains("items"));
  EXPECT_EQ(progress().gold, 10);
  expect_encodes(packets);
}

TEST_F(LootSessionTest, KillGoldIsAddedToTheGoldOfTheLootPack) {
  auto profile = profile_fixture();
  profile["npcs"][0] = npc_row(kFullPackNpc, kFullPack, 0, 0.0, 4);
  auto session = make_session(profile);
  random->quantities = {2, 5, 1};
  session.monster_defeated(defeat(kCorpse, kFullPackNpc));

  const auto packets = session.receive(loot_request(kCorpse), player_at(0), kStartTicks);

  ASSERT_EQ(packets.size(), 3U);
  EXPECT_EQ(packets[0].message.at("money_changes"), (Json{{{"change", "9"}, {"result", "9"}}}));
}

// CUSTOM: kill gold above the wallet limit stays behind instead of refusing the take, so the
// configured count-limit error no longer applies to gold.
TEST_F(LootSessionTest, KillGoldStopsAtTheWalletLimitWithoutRefusingTheTake) {
  update_progress([](WorldProgress &state) { state.gold = kGoldLimit - 6; });
  auto rules = LootCustomRules::all_enabled();
  rules.count_limit_error = x5::ec::ItemWeightLimit;
  auto session = make_session(profile_fixture(), rules);
  session.monster_defeated(defeat(kCorpse, kGoldOnlyNpc));

  const auto packets = session.receive(loot_request(kCorpse), player_at(0), kStartTicks);
  EXPECT_EQ(packets.size(), 3U);
  EXPECT_EQ(progress().gold, kGoldLimit);
  EXPECT_EQ(commit_attempts, 1);
}

TEST_F(LootSessionTest, KillGoldCanBeSwitchedOff) {
  auto session = make_session(profile_fixture(), rules_without(LootRule::kill_gold));

  EXPECT_TRUE(session.monster_defeated(defeat(kCorpse, kGoldOnlyNpc)).packets.empty());
  EXPECT_EQ(error_of(session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_HasNoLoot");
  EXPECT_EQ(progress().gold, 0);
}

// --- First-kill groups -------------------------------------------------------------------------

TEST_F(LootSessionTest, FirstKillGroupDropsOnceAndTheKillIsRecordedWithTheTake) {
  auto session = make_session();
  session.monster_defeated(defeat(1, kFirstKillNpc));
  const auto revision = progress().revision;

  const auto first = session.receive(loot_request(1), player_at(0), kStartTicks);
  ASSERT_EQ(first.size(), 3U);
  EXPECT_EQ(first[0].message.at("stackables").size(), 2U);
  EXPECT_EQ(progress().stackables.at(kStone), 1);
  EXPECT_EQ(progress().loot_first_kills, (std::set<int>{kFirstKillNpc}));
  EXPECT_EQ(progress().revision, revision + 1);

  session.monster_defeated(defeat(2, kFirstKillNpc, kStartTicks + 1));
  const auto second = session.receive(loot_request(2), player_at(0), kStartTicks + 1);
  ASSERT_EQ(second.size(), 3U);
  EXPECT_EQ(second[0].message.at("stackables"), (Json{{{"item_cms_id", kHerb}, {"quantity", 2}}}));
  EXPECT_EQ(progress().stackables.at(kStone), 1);
}

TEST_F(LootSessionTest, AKillWaitingToBeLootedDoesNotShareItsFirstKillWithALaterCorpse) {
  auto session = make_session();
  session.monster_defeated(defeat(1, kFirstKillNpc));
  session.monster_defeated(defeat(2, kFirstKillNpc));

  session.receive(loot_request(2), player_at(0), kStartTicks);
  EXPECT_FALSE(progress().stackables.contains(kStone));
  EXPECT_TRUE(progress().loot_first_kills.empty());

  session.receive(loot_request(1), player_at(0), kStartTicks);
  EXPECT_EQ(progress().stackables.at(kStone), 1);
  EXPECT_EQ(progress().loot_first_kills, (std::set<int>{kFirstKillNpc}));
}

TEST_F(LootSessionTest, AnUnlootedFirstKillCorpseReleasesItsReservationWhenItExpires) {
  auto session = make_session();
  session.monster_defeated(defeat(1, kFirstKillNpc));
  const auto expiry = kStartTicks + 90000 * kTicksPerMs;
  session.advance(expiry);

  session.monster_defeated(defeat(2, kFirstKillNpc, expiry));
  session.receive(loot_request(2), player_at(0), expiry);

  EXPECT_EQ(progress().stackables.at(kStone), 1);
}

TEST_F(LootSessionTest, FirstKillGroupsAreWithheldWhenTheRuleIsOff) {
  auto session = make_session(profile_fixture(), rules_without(LootRule::first_kill_rewards));
  session.monster_defeated(defeat(1, kFirstKillNpc));

  session.receive(loot_request(1), player_at(0), kStartTicks);

  EXPECT_FALSE(progress().stackables.contains(kStone));
  EXPECT_TRUE(progress().loot_first_kills.empty());
}

TEST_F(LootSessionTest, AnEmptyFirstKillRollStillCommitsTheRecord) {
  auto session = make_session();
  random->pass_results = {true, false, false};
  session.monster_defeated(defeat(1, kFirstKillNpc));

  const auto packets = session.receive(loot_request(1), player_at(0), kStartTicks);

  // No item update, since nothing dropped, but the first kill is spent.
  ASSERT_EQ(packets.size(), 2U);
  EXPECT_EQ(commit_attempts, 1);
  EXPECT_EQ(progress().loot_first_kills, (std::set<int>{kFirstKillNpc}));
}

TEST_F(LootSessionTest, FirstKillStaysUnavailableWhenProgressCannotBeRead) {
  // The session's storage no longer knows this character, so its progress cannot be read.
  character.id += 1000;
  auto session = make_session();

  session.monster_defeated(defeat(1, kFirstKillNpc));

  // Only the ordinary group was rolled: one pick, so the first-kill stone is never an option.
  EXPECT_EQ(session.corpse_count(), 1U);
  EXPECT_EQ(random->weights_seen.size(), 1U);
}

TEST_F(LootSessionTest, FirstKillRecordsStayWithinTheStorageValidation) {
  update_progress([](WorldProgress &state) { state.loot_first_kills.insert(kFirstKillNpc); });
  const auto stored = progress();
  auto removed = stored;
  removed.loot_first_kills.clear();
  removed.revision = stored.revision + 1;
  EXPECT_THROW(store.character_state->save_world_progress(1, character.id, 1, stored, removed),
               std::invalid_argument);

  auto invalid = stored;
  invalid.loot_first_kills.insert(0);
  invalid.revision = stored.revision + 1;
  EXPECT_THROW(store.character_state->save_world_progress(1, character.id, 1, stored, invalid),
               std::invalid_argument);

  auto oversized = stored;
  for (std::size_t index = 1; index <= kMaximumLootFirstKills; ++index) {
    oversized.loot_first_kills.insert(static_cast<int>(index));
  }
  oversized.revision = stored.revision + 1;
  EXPECT_THROW(store.character_state->save_world_progress(1, character.id, 1, stored, oversized),
               std::invalid_argument);
  EXPECT_EQ(progress(), stored);
}

// --- Quest gates, polish steps and stones ------------------------------------------------------

TEST_F(LootSessionTest, QuestGatedItemDropsOnlyWhileTheQuestIsActive) {
  auto session = make_session();
  session.monster_defeated(defeat(1, kQuestNpc));
  session.receive(loot_request(1), player_at(0), kStartTicks);
  EXPECT_FALSE(progress().stackables.contains(kQuestItem));
  EXPECT_EQ(progress().stackables.at(kHerb), 1);

  update_progress([](WorldProgress &state) { state.quests[kQuestId] = QuestProgress{}; });
  session.monster_defeated(defeat(2, kQuestNpc, kStartTicks + 1));
  session.receive(loot_request(2), player_at(0), kStartTicks + 1);
  EXPECT_EQ(progress().stackables.at(kQuestItem), 1);

  update_progress([](WorldProgress &state) { state.quests[kQuestId].complete = true; });
  session.monster_defeated(defeat(3, kQuestNpc, kStartTicks + 2));
  session.receive(loot_request(3), player_at(0), kStartTicks + 2);
  EXPECT_EQ(progress().stackables.at(kQuestItem), 1);
}

TEST_F(LootSessionTest, QuestGatedRowsAreWithheldWhenTheRuleIsOff) {
  auto session = make_session(profile_fixture(), rules_without(LootRule::quest_gated_rows));

  EXPECT_TRUE(session.monster_defeated(defeat(1, kQuestNpc)).packets.empty());
}

TEST_F(LootSessionTest, PolishStepRowDropsAPolishedInstanceWithCreationTime) {
  auto session = make_session();
  session.monster_defeated(defeat(1, kPolishNpc));

  const auto packets = session.receive(loot_request(1), player_at(0), kStartTicks);

  ASSERT_EQ(packets.size(), 3U);
  const auto &dropped = packets[0].message.at("items").at(0);
  EXPECT_EQ(dropped.at("polish_step"), kPolishStep);
  EXPECT_EQ(dropped.at("created_epochtime"), std::to_string(kCreatedEpochTicks));
  const auto items = stored_items();
  const auto sword =
      std::ranges::find_if(items, [](const CharacterItem &item) { return item.cms_id == kSword; });
  ASSERT_NE(sword, items.end());
  EXPECT_EQ(sword->polish_step, kPolishStep);
  expect_encodes(packets);
}

TEST_F(LootSessionTest, PolishStepRowsAreWithheldWhenTheRuleIsOff) {
  auto session = make_session(profile_fixture(), rules_without(LootRule::polish_step_rows));

  EXPECT_TRUE(session.monster_defeated(defeat(1, kPolishNpc)).packets.empty());
}

TEST_F(LootSessionTest, FullMoonStonesDropAsOrdinaryStacksAndCanBeSwitchedOff) {
  auto session = make_session();
  session.monster_defeated(defeat(1, kStoneNpc));
  session.receive(loot_request(1), player_at(0), kStartTicks);
  EXPECT_EQ(progress().stackables.at(kStone), 1);

  auto off = make_session(profile_fixture(), rules_without(LootRule::full_moon_stones));
  EXPECT_TRUE(off.monster_defeated(defeat(2, kStoneNpc)).packets.empty());
}

// --- Handling loot -----------------------------------------------------------------------------

TEST_F(LootSessionTest, HandlingPackAppliesAtItsRateAndJoinsTheOrdinaryTake) {
  auto session = make_session();

  // possible_handling_loot is reserved for the evidenced knife butchering.
  const auto reaction = session.monster_defeated(defeat(1, kHandlingNpc));
  ASSERT_EQ(reaction.packets.size(), 1U);
  EXPECT_EQ(reaction.packets.front().message,
            (Json{{"unit_id", "1"}, {"has_chance_to_loot", true}}));
  expect_encodes(reaction.packets);

  const auto packets = session.receive(loot_request(1), player_at(0), kStartTicks);
  ASSERT_EQ(packets.size(), 3U);
  EXPECT_EQ(progress().stackables.at(kHerb), 3);
}

TEST_F(LootSessionTest, HandlingPackThatMissesItsRateOnlyAnnouncesTheNormalFlag) {
  auto session = make_session();
  random->pass_results = {true, true, false};

  const auto reaction = session.monster_defeated(defeat(1, kHandlingNpc));
  ASSERT_EQ(reaction.packets.size(), 1U);
  EXPECT_EQ(reaction.packets.front().message,
            (Json{{"unit_id", "1"}, {"has_chance_to_loot", true}}));

  session.receive(loot_request(1), player_at(0), kStartTicks);
  EXPECT_EQ(progress().stackables.at(kHerb), 1);
}

TEST_F(LootSessionTest, AnNpcWithOnlyHandlingLootIsLootableWhenHandlingApplies) {
  auto session = make_session();
  random->pass_results = {false, true};

  EXPECT_TRUE(session.monster_defeated(defeat(1, kHandlingOnlyNpc)).packets.empty());
  EXPECT_EQ(error_of(session.receive(loot_request(1), player_at(0), kStartTicks)), "Npc_HasNoLoot");

  const auto reaction = session.monster_defeated(defeat(2, kHandlingOnlyNpc));
  ASSERT_EQ(reaction.packets.size(), 1U);
  EXPECT_EQ(reaction.packets.front().message,
            (Json{{"unit_id", "2"}, {"has_chance_to_loot", true}}));
  EXPECT_EQ(session.receive(loot_request(2), player_at(0), kStartTicks).size(), 3U);
  EXPECT_EQ(progress().stackables.at(kHerb), 2);
}

TEST_F(LootSessionTest, HandlingLootIsWithheldWhenTheRuleIsOff) {
  auto session = make_session(profile_fixture(), rules_without(LootRule::handling_loot));

  EXPECT_TRUE(session.monster_defeated(defeat(1, kHandlingOnlyNpc)).packets.empty());
  const auto reaction = session.monster_defeated(defeat(2, kHandlingNpc));
  EXPECT_EQ(reaction.packets.front().message,
            (Json{{"unit_id", "2"}, {"has_chance_to_loot", true}}));
}

// --- Formalized guesses ------------------------------------------------------------------------

TEST_F(LootSessionTest, RangeToleranceIsConfigurable) {
  auto rules = LootCustomRules::all_enabled();
  rules.range_tolerance_mm = 0.0;
  auto exact = make_session(profile_fixture(), rules);
  exact.monster_defeated(defeat(1, kSinglePackNpc));
  EXPECT_EQ(error_of(exact.receive(loot_request(1), player_at(5001), kStartTicks)),
            "Action_InvalidLootingRange");
  EXPECT_EQ(exact.receive(loot_request(1), player_at(5000), kStartTicks).size(), 3U);

  rules.range_tolerance_mm = 10000.0;
  auto generous = make_session(profile_fixture(), rules);
  generous.monster_defeated(defeat(2, kSinglePackNpc));
  EXPECT_EQ(generous.receive(loot_request(2), player_at(15000), kStartTicks).size(), 3U);
}

TEST_F(LootSessionTest, RepeatedTakeErrorIsConfigurable) {
  auto rules = LootCustomRules::all_enabled();
  rules.repeated_take_error = x5::ec::Npc_ImpossibleLoot;
  auto session = make_session(profile_fixture(), rules);
  session.monster_defeated(defeat(1, kSinglePackNpc));
  session.receive(loot_request(1), player_at(0), kStartTicks);

  EXPECT_EQ(error_of(session.receive(loot_request(1), player_at(0), kStartTicks)),
            "Npc_ImpossibleLoot");
}

TEST_F(LootSessionTest, FullBagAndWeightErrorsAreConfigurable) {
  auto rules = LootCustomRules::all_enabled();
  rules.full_bag_error = x5::ec::ItemWeightLimit;
  rules.over_weight_error = x5::ec::ItemNotEnoughFreeWeight;
  bool allowed = false;
  weight_policy = [&](const LootCommit &) { return allowed; };
  auto session = make_session(profile_fixture(), rules);
  session.monster_defeated(defeat(1, kPolishNpc));

  EXPECT_EQ(error_of(session.receive(loot_request(1), player_at(0), kStartTicks)),
            "ItemNotEnoughFreeWeight");

  allowed = true;
  auto crowded = *persistence().load_character();
  crowded.items.clear();
  for (int slot = 0; slot <= kMaximumInventorySlot; ++slot) {
    CharacterItem item;
    item.cms_id = kHerb;
    item.inventory_slot = slot;
    crowded.items.push_back(item);
  }
  forced_character = crowded;
  EXPECT_EQ(error_of(session.receive(loot_request(1), player_at(0), kStartTicks)),
            "ItemWeightLimit");
}

TEST_F(LootSessionTest, EmptyRollReplyCanBeAnErrorInsteadOfASuccess) {
  auto rules = LootCustomRules::all_enabled();
  rules.empty_roll_reply = EmptyRollReply::no_loot_error;
  auto session = make_session(profile_fixture(), rules);
  random->pass_results = {false};
  session.monster_defeated(defeat(1, kPartialPackNpc));

  EXPECT_EQ(error_of(session.receive(loot_request(1), player_at(0), kStartTicks)), "Npc_HasNoLoot");
  // The corpse is spent either way.
  EXPECT_EQ(error_of(session.receive(loot_request(1), player_at(0), kStartTicks)), "Npc_HasNoLoot");
  EXPECT_EQ(commit_attempts, 0);
}

TEST_F(LootSessionTest, CorpseLifetimesAreConfigurable) {
  auto rules = LootCustomRules::all_enabled();
  rules.lootable_lifetime_ms = 5000;
  rules.emptied_lifetime_ms = 2000;
  auto session = make_session(profile_fixture(), rules);
  session.monster_defeated(defeat(1, kSinglePackNpc));
  EXPECT_EQ(session.next_action_tick(), kStartTicks + 5000 * kTicksPerMs);

  const auto taken_at = kStartTicks + 1000 * kTicksPerMs;
  session.receive(loot_request(1), player_at(0), taken_at);
  EXPECT_EQ(session.next_action_tick(), taken_at + 2000 * kTicksPerMs);
}

TEST_F(LootSessionTest, EmptiedLifetimeCanBeAnchoredOnTheDeath) {
  const auto taken_at = kStartTicks + 8000 * kTicksPerMs;

  auto from_take = make_session();
  from_take.monster_defeated(defeat(1, kSinglePackNpc));
  from_take.receive(loot_request(1), player_at(0), taken_at);
  EXPECT_EQ(from_take.next_action_tick(), taken_at + 10000 * kTicksPerMs);

  auto rules = LootCustomRules::all_enabled();
  rules.emptied_lifetime_anchor = EmptiedLifetimeAnchor::death;
  auto from_death = make_session(profile_fixture(), rules);
  from_death.monster_defeated(defeat(1, kSinglePackNpc));
  from_death.receive(loot_request(1), player_at(0), taken_at);
  EXPECT_EQ(from_death.next_action_tick(), kStartTicks + 10000 * kTicksPerMs);
}

TEST_F(LootSessionTest, EvidenceOnlyRulesKeepTheRecordedBehaviorOnly) {
  auto session = make_session(profile_fixture(), LootCustomRules::evidence_only());

  // Custom NPC facts are gone, the tolerance is zero, and untagged packs still work.
  EXPECT_TRUE(session.monster_defeated(defeat(1, kGoldOnlyNpc)).packets.empty());
  EXPECT_FALSE(session.monster_defeated(defeat(2, kFullPackNpc)).packets.empty());
  const int limit = static_cast<int>(data->looting_range_mm());
  EXPECT_EQ(error_of(session.receive(loot_request(2), player_at(limit + 1), kStartTicks)),
            "Action_InvalidLootingRange");
  EXPECT_EQ(session.receive(loot_request(2), player_at(limit), kStartTicks).size(), 3U);
}

// --- Party shared loot -------------------------------------------------------------------------

class LootPartyTest : public LootSessionTest {
protected:
  void SetUp() override {
    LootSessionTest::SetUp();
    audience = std::make_shared<FakeAudience>();
    audience->members = {member.id};
    board = std::make_shared<SharedLootBoard>();
  }

  // Owner and member sessions over one profile, board and audience.
  void start(const LootCustomRules &rules = LootCustomRules::all_enabled()) {
    owner = std::make_unique<LootSession>(make_session(profile_fixture(), rules, audience, board));
    member_session = std::make_unique<LootSession>(
        make_session_for(member, data, member_random, audience, board));
  }

  std::shared_ptr<FakeAudience> audience;
  std::shared_ptr<SharedLootBoard> board;
  std::unique_ptr<LootSession> owner;
  std::unique_ptr<LootSession> member_session;
  ScriptedLootRandom *member_random{};
};

TEST_F(LootPartyTest, NearbyMembersAreAnnouncedAndTheFirstToTakeGetsEverything) {
  start();
  random->quantities = {3, 6, 1};

  const auto reaction = owner->monster_defeated(defeat(kCorpse, kFullPackNpc));

  ASSERT_EQ(reaction.packets.size(), 1U);
  EXPECT_EQ(audience->queries, 1);
  EXPECT_DOUBLE_EQ(audience->last_range_mm, data->rules().party_range_mm);
  EXPECT_EQ(board->size(), 1U);
  const auto announced = audience->sent_to(member.id);
  ASSERT_EQ(announced.size(), 1U);
  EXPECT_EQ(announced.front().opcode, kUnitLootItemInfoOpcode);
  EXPECT_EQ(announced.front().message,
            (Json{{"unit_id", std::to_string(kCorpse)}, {"has_chance_to_loot", true}}));

  const auto taken = member_session->receive(loot_request(kCorpse), player_at(1000), kStartTicks);
  ASSERT_EQ(taken.size(), 3U);
  EXPECT_EQ(progress_of(member).gold, 6);
  EXPECT_EQ(progress_of(member).stackables.at(kHerb), 3);
  EXPECT_EQ(progress().gold, 0);

  // The killer's client clears its flag, and the killer's own request now finds it taken.
  const auto cleared = audience->sent_to(character.id);
  ASSERT_EQ(cleared.size(), 1U);
  EXPECT_EQ(cleared.front().message, (Json{{"unit_id", std::to_string(kCorpse)}}));
  EXPECT_EQ(error_of(owner->receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_HasNoLoot");
  EXPECT_EQ(commit_attempts, 1);
}

TEST_F(LootPartyTest, WhenTheKillerTakesFirstTheMemberIsToldAndRefused) {
  start();
  owner->monster_defeated(defeat(kCorpse, kSinglePackNpc));

  ASSERT_EQ(owner->receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);

  const auto to_member = audience->sent_to(member.id);
  ASSERT_EQ(to_member.size(), 2U);
  EXPECT_EQ(to_member.back().message, (Json{{"unit_id", std::to_string(kCorpse)}}));
  EXPECT_EQ(error_of(member_session->receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_HasNoLoot");
  EXPECT_FALSE(progress_of(member).stackables.contains(kHerb));
}

TEST_F(LootPartyTest, AFailedCommitHandsTheClaimBackToTheOthers) {
  start();
  owner->monster_defeated(defeat(kCorpse, kSinglePackNpc));
  int attempts = 0;
  before_commit = [&attempts] {
    if (++attempts == 1) {
      throw std::runtime_error("storage offline");
    }
  };

  EXPECT_EQ(error_of(member_session->receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_FailLootTransaction");
  EXPECT_EQ(owner->receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);
  EXPECT_EQ(progress().stackables.at(kHerb), 1);
}

TEST_F(LootPartyTest, AMemberOutOfRangeIsRefusedWithoutClaiming) {
  start();
  owner->monster_defeated(defeat(kCorpse, kSinglePackNpc));

  EXPECT_EQ(
      error_of(member_session->receive(loot_request(kCorpse), player_at(900000), kStartTicks)),
      "Action_InvalidLootingRange");
  EXPECT_EQ(commit_attempts, 0);
  EXPECT_EQ(owner->receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);
}

TEST_F(LootPartyTest, OnlyEligibleMembersCanFindTheCorpse) {
  start();
  Character outsider;
  outsider.account_id = 1;
  outsider.slot = 2;
  outsider.name = "LootOutsider";
  outsider = store.characters->create(outsider);
  ScriptedLootRandom *outsider_random = nullptr;
  auto outsider_session = make_session_for(outsider, data, outsider_random, nullptr, board);
  owner->monster_defeated(defeat(kCorpse, kSinglePackNpc));

  EXPECT_EQ(error_of(outsider_session.receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_InvalidId");
}

TEST_F(LootPartyTest, PartySharingCanBeSwitchedOff) {
  start(rules_without(LootRule::party_shared_loot));

  owner->monster_defeated(defeat(kCorpse, kSinglePackNpc));

  EXPECT_EQ(audience->queries, 0);
  EXPECT_EQ(board->size(), 0U);
  EXPECT_TRUE(audience->sent.empty());
  EXPECT_EQ(error_of(member_session->receive(loot_request(kCorpse), player_at(0), kStartTicks)),
            "Npc_InvalidId");
  EXPECT_EQ(owner->receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);
}

TEST_F(LootPartyTest, WithoutNearbyMembersTheCorpseStaysPrivate) {
  start();
  audience->members.clear();

  owner->monster_defeated(defeat(kCorpse, kSinglePackNpc));

  EXPECT_EQ(audience->queries, 1);
  EXPECT_EQ(board->size(), 0U);
  EXPECT_TRUE(audience->sent.empty());
  EXPECT_EQ(owner->receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);
}

TEST_F(LootPartyTest, WithoutAnAudienceOrBoardNothingIsShared) {
  auto no_board =
      make_session(profile_fixture(), LootCustomRules::all_enabled(), audience, nullptr);
  no_board.monster_defeated(defeat(1, kSinglePackNpc));
  auto no_audience =
      make_session(profile_fixture(), LootCustomRules::all_enabled(), nullptr, board);
  no_audience.monster_defeated(defeat(2, kSinglePackNpc));

  EXPECT_EQ(audience->queries, 0);
  EXPECT_EQ(board->size(), 0U);
}

TEST_F(LootPartyTest, FirstKillIsRecordedOnTheCharacterWhoTakesTheCorpse) {
  start();
  owner->monster_defeated(defeat(kCorpse, kFirstKillNpc));

  ASSERT_EQ(member_session->receive(loot_request(kCorpse), player_at(0), kStartTicks).size(), 3U);

  EXPECT_EQ(progress_of(member).loot_first_kills, (std::set<int>{kFirstKillNpc}));
  EXPECT_TRUE(progress().loot_first_kills.empty());
  EXPECT_EQ(progress_of(member).stackables.at(kStone), 1);
}

TEST_F(LootPartyTest, TheOwnersExpiryClosesTheSharedCorpse) {
  start();
  owner->monster_defeated(defeat(kCorpse, kSinglePackNpc));
  const auto expiry = kStartTicks + 90000 * kTicksPerMs;
  ASSERT_EQ(board->size(), 1U);

  owner->advance(expiry);

  EXPECT_EQ(board->size(), 0U);
  EXPECT_EQ(error_of(member_session->receive(loot_request(kCorpse), player_at(0), expiry)),
            "Npc_InvalidId");
}

TEST_F(LootPartyTest, AMemberCannotTakeAnExpiredSharedCorpseBeforeTheOwnerNoticesIt) {
  start();
  owner->monster_defeated(defeat(kCorpse, kSinglePackNpc));
  const auto expiry = kStartTicks + 90000 * kTicksPerMs;

  EXPECT_EQ(error_of(member_session->receive(loot_request(kCorpse), player_at(0), expiry)),
            "Npc_InvalidId");
}

TEST(SharedLootBoardTest, ClaimsAreExclusiveAndReleasable) {
  SharedLootBoard board;
  const SharedCorpseKey key{1, 99};
  SharedCorpse corpse;
  corpse.eligible = {1, 2};
  corpse.contents.gold = 5;
  corpse.expire_tick = 1000;

  EXPECT_EQ(board.claim(key, 1, 0).status, ClaimStatus::unknown);
  ASSERT_TRUE(board.open(key, corpse));
  EXPECT_FALSE(board.open(key, corpse));

  EXPECT_EQ(board.claim(key, 3, 0).status, ClaimStatus::not_eligible);
  EXPECT_EQ(board.claim(key, 1, 1000).status, ClaimStatus::expired);
  const auto claimed = board.claim(key, 2, 10);
  ASSERT_EQ(claimed.status, ClaimStatus::claimed);
  EXPECT_EQ(claimed.corpse->contents.gold, 5);
  EXPECT_EQ(board.claim(key, 1, 10).status, ClaimStatus::already_taken);

  board.release(key);
  EXPECT_EQ(board.claim(key, 1, 10).status, ClaimStatus::claimed);
  board.close(key);
  EXPECT_EQ(board.size(), 0U);
  EXPECT_EQ(board.claim(key, 1, 10).status, ClaimStatus::unknown);
  board.release(key);
}

TEST(SharedLootBoardTest, MembersFindOnlyOtherOwnersEligibleUnexpiredCorpses) {
  SharedLootBoard board;
  SharedCorpse corpse;
  corpse.eligible = {1, 2};
  corpse.position = {kMap, 7, 8, 9, 0};
  corpse.expire_tick = 1000;
  ASSERT_TRUE(board.open({1, 99}, corpse));

  const auto found = board.find_for_member(99, 2, 10);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->key, (SharedCorpseKey{1, 99}));
  EXPECT_EQ(found->position.x_mm, 7);
  EXPECT_FALSE(board.find_for_member(99, 1, 10).has_value());
  EXPECT_FALSE(board.find_for_member(99, 3, 10).has_value());
  EXPECT_FALSE(board.find_for_member(98, 2, 10).has_value());
  EXPECT_FALSE(board.find_for_member(99, 2, 1000).has_value());
  EXPECT_EQ(board.eligible({1, 99}), (std::set<std::int64_t>{1, 2}));
  EXPECT_TRUE(board.eligible({1, 1}).empty());
}

TEST(SharedLootBoardTest, CapacityIsBounded) {
  SharedLootBoard board;
  SharedCorpse corpse;
  corpse.eligible = {1};
  for (std::size_t index = 1; index <= kMaximumSharedCorpses; ++index) {
    ASSERT_TRUE(board.open({1, index}, corpse));
  }

  EXPECT_FALSE(board.open({1, kMaximumSharedCorpses + 1}, corpse));
  EXPECT_EQ(board.size(), kMaximumSharedCorpses);
}

} // namespace
} // namespace chronicles
