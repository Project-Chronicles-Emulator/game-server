#include "common/currency.hpp"
#include "protocol/protocol.hpp"
#include "world/features/tutorial/tutorial.hpp"

#include "testing/memory_repositories.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kTestTicks = 100000000;
constexpr int kPotionCmsId = 40101101;

Json test_definitions() {
  const Json trigger{
      {"guid", {"123", "456"}}, {"center", {0, 0, 0}}, {"extent", {1000, 1000, 1000}}, {"yaw", 0}};
  return {{"format_version", 1},
          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
          {"map_id", 8000},
          {"civilians", Json::array()},
          {"sectors", {98317}},
          {"utility_items", {kPotionCmsId}},
          {"potions",
           {{{"item_cms_id", kPotionCmsId},
             {"skill_id", 1000},
             {"effect_id", 100000141},
             {"execute_time_ms", 435},
             {"duration_ms", 2881}}}},
          {"objects",
           {{{"unit_id", "422268299649030"},
             {"cms_id", 20080000},
             {"interaction_id", 23},
             {"position", {0, 0, 0}},
             {"rewards", {{{"item_cms_id", kPotionCmsId}, {"quantity", 5}}}}},
            {{"unit_id", "422268299644934"},
             {"cms_id", 20080001},
             {"interaction_id", 23},
             {"position", {0, 0, 0}},
             {"rewards", {{{"item_cms_id", 0}, {"quantity", 37}}}}}}},
          {"doodads",
           {{{"unit_id", "351865195728902"},
             {"cms_id", 49013},
             {"sector_id", 98317},
             {"local_id", 1},
             {"position", {0, 0, 0}},
             {"yaw", 0},
             {"animation_id", 200},
             {"interaction_id", 1221},
             {"initial_phase", 2},
             {"completed_phase", 3},
             {"quest_id", 10000042},
             {"quest_sequence", 1},
             {"consumes", {{{"item_cms_id", 100109}, {"quantity", 1}}}}}}},
          {"quests",
           {{{"cms_id", 10000040},
             {"auto_complete", false},
             {"sequential", true},
             {"accept_triggers", {trigger}},
             {"experience", 0},
             {"rewards", Json::array()},
             {"next_quest", 10000042},
             {"progresses",
              {{{"sequence", 1},
                {"group", 1},
                {"kind", "TriggerVolumeEnter"},
                {"parameters", Json::object()},
                {"trigger", trigger}}}}},
            {{"cms_id", 10000042},
             {"auto_complete", false},
             {"sequential", true},
             {"accept_triggers", Json::array()},
             {"experience", 200},
             {"rewards", Json::array()},
             {"next_quest", 10000043},
             {"progresses",
              {{{"sequence", 1},
                {"group", 1},
                {"kind", "InteractDoodad"},
                {"parameters", {{"doodadId", 49013}, {"count", 1}}}}}}},
            {{"cms_id", 10000043},
             {"auto_complete", false},
             {"sequential", false},
             {"accept_triggers", Json::array()},
             {"experience", 0},
             {"rewards", Json::array()},
             {"progresses",
              {{{"sequence", 1},
                {"group", 1},
                {"kind", "HuntMonster"},
                {"parameters", {{"npcId", 5001529}, {"count", 1}}}}}}}}}};
}

Json interaction(std::string target = "422268299649030") {
  return {{"target_unit_id", std::move(target)}, {"param", {{"interaction_id", 23}}}};
}

Json quest_trigger(int cms_id, bool progress = false) {
  Json request{
      {"quest_cms_id", cms_id},
      {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
      {"trigger_volume_enter", {{"trigger_volume_x", "123"}, {"trigger_volume_y", "456"}}}};
  if (progress) {
    request["quest_progress_sequence_id"] = 1;
  }
  return request;
}

Json potion_start(std::uint32_t instance = 1) {
  return {{"use_item", {{"item_cms_id", kPotionCmsId}}},
          {"start_skill",
           {{"skill_id", 1000},
            {"instance_id", instance},
            {"tick", std::to_string(kTestTicks)},
            {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
            {"rotation", {{"yaw", 0}}}}}};
}

Json potion_effect(std::uint32_t instance = 1) {
  return {{"skill_instance_id", instance},
          {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
          {"skill_effect", {{"effect_id", 100000141}, {"targets", {{{"unit_id", "7"}}}}}}};
}

void verify_packets(const std::vector<GameplayPacket> &packets) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : packets) {
    const auto frame = protocol.encode(packet.opcode, packet.message);
    EXPECT_EQ(protocol.decode(frame).at("status"), "decoded");
  }
}

class TutorialTest : public testing::Test {
protected:
  std::shared_ptr<const TutorialData> data = std::make_shared<TutorialData>(test_definitions());
  WorldProgress durable;
  unsigned commits{};
  WorldPosition position{8000, 0, 0, 0, 0};

  TutorialSession session(WorldProgress initial = {}) {
    return TutorialSession(data, 7, std::move(initial),
                           [this](const auto &before, const auto &after) {
                             EXPECT_EQ(before.revision, durable.revision);
                             durable = after;
                             ++commits;
                             return true;
                           });
  }
};

TEST_F(TutorialTest, PickupCommitsOnceAndRestoresAcrossReconnect) {
  auto client = session();
  auto packets = client.receive(20068, interaction(), 12, position, kTestTicks);
  ASSERT_EQ(packets.size(), 3);
  EXPECT_EQ(packets[0].opcode, 91);
  EXPECT_EQ(packets[1].message.at("stackables").at(0).at("quantity"), 5);
  EXPECT_EQ(commits, 1);
  verify_packets(packets);
  auto reconnected = session(durable);
  packets = reconnected.receive(20068, interaction(), 13, position, kTestTicks);
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(durable.stackables.at(kPotionCmsId), 5);
  verify_packets(reconnected.snapshot());

  packets = reconnected.receive(20068, interaction("422268299644934"), 14, position, kTestTicks);
  EXPECT_EQ(durable.gold, 37);
  verify_packets(packets);
}

TEST_F(TutorialTest, SnapshotMoneyIncludesHersanderTokenOnlyWhenNonzero) {
  const auto money_values = [](const std::vector<GameplayPacket> &packets) {
    return std::ranges::find_if(packets, [](const auto &packet) { return packet.opcode == 126; })
        ->message.at("money")
        .at("values");
  };

  WorldProgress gold_only;
  gold_only.gold = 37;
  const auto zero_token = money_values(session(gold_only).snapshot());
  EXPECT_EQ(zero_token.size(), 1);
  EXPECT_EQ(zero_token.at("0"), "37");

  WorldProgress with_token = gold_only;
  with_token.hersander_token = 20;
  const auto nonzero_token = money_values(session(with_token).snapshot());
  EXPECT_EQ(nonzero_token.size(), 2);
  EXPECT_EQ(nonzero_token.at("0"), "37");
  EXPECT_EQ(nonzero_token.at("1"), "20");
}

TEST_F(TutorialTest, GoldRewardLandingExactlyOnCatalogLimitIsAccepted) {
  WorldProgress initial;
  initial.gold = kMoneyLimit - 37;
  auto client = session(initial);
  durable = initial;
  const auto packets =
      client.receive(20068, interaction("422268299644934"), 1, position, kTestTicks);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(durable.gold, kMoneyLimit);
  const auto &update = std::ranges::find_if(packets, [](const auto &packet) {
                         return packet.opcode == 169;
                       })->message;
  EXPECT_EQ(update.at("money_changes").at(0).at("result"), std::to_string(kMoneyLimit));
  EXPECT_FALSE(update.at("money_changes").at(0).contains("money_type"));
}

// CUSTOM: graduated characters hold the full wallet, so refusing an overflowing gold reward made
// every gold-paying object and quest impossible for them. The gold stops at the limit instead.
TEST_F(TutorialTest, GoldRewardAboveCatalogLimitFillsTheWalletAndGrantsTheRest) {
  auto document = test_definitions();
  document["objects"][1]["rewards"] = {{{"item_cms_id", kPotionCmsId}, {"quantity", 5}},
                                       {{"item_cms_id", 0}, {"quantity", 37}}};
  data = std::make_shared<TutorialData>(document);
  WorldProgress initial;
  initial.gold = kMoneyLimit - 36;
  durable = initial;
  auto client = session(initial);
  const auto packets =
      client.receive(20068, interaction("422268299644934"), 1, position, kTestTicks);
  EXPECT_TRUE(std::ranges::none_of(packets, [](const auto &packet) {
    return packet.opcode == 2;
  }));
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(durable.gold, kMoneyLimit);
  EXPECT_EQ(durable.stackables.at(kPotionCmsId), 5);
  EXPECT_TRUE(durable.adventure_objects.contains(20080001));
}

TEST_F(TutorialTest, GoldRewardWithAFullWalletGrantsOnlyTheItems) {
  auto document = test_definitions();
  document["objects"][1]["rewards"] = {{{"item_cms_id", kPotionCmsId}, {"quantity", 5}},
                                       {{"item_cms_id", 0}, {"quantity", 37}}};
  data = std::make_shared<TutorialData>(document);
  WorldProgress initial;
  initial.gold = kMoneyLimit;
  durable = initial;
  auto client = session(initial);
  client.receive(20068, interaction("422268299644934"), 1, position, kTestTicks);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(durable.gold, kMoneyLimit);
  EXPECT_EQ(durable.stackables.at(kPotionCmsId), 5);
}

TEST_F(TutorialTest, InvalidTargetsRangeMapAndUtilityAllocationsCannotGrantItems) {
  auto client = session();
  for (const auto &request : {interaction("999"), interaction("-1")}) {
    EXPECT_EQ(client.receive(20068, request, 0, position, kTestTicks).at(0).opcode, 2);
  }
  auto distant = position;
  distant.x_mm = 8000;
  EXPECT_EQ(client.receive(20068, interaction(), 1, distant, kTestTicks).at(0).opcode, 2);
  distant.map_id = 9011;
  EXPECT_EQ(client.receive(20068, interaction(), 2, distant, kTestTicks).at(0).opcode, 2);
  EXPECT_EQ(commits, 0);
  client.receive(20068, interaction(), 3, position, kTestTicks);
  Json slots{{"slots",
              {{{"slot", 0}, {"item_cms_id", kPotionCmsId}, {"count", 3}},
               {{"slot", 1}, {"item_cms_id", kPotionCmsId}, {"count", 3}}}}};
  EXPECT_EQ(client.receive(20080, slots, 4, position, kTestTicks).at(0).opcode, 2);
  slots["slots"].erase(1);
  auto packets = client.receive(20080, slots, 5, position, kTestTicks);
  ASSERT_EQ(packets.at(0).opcode, 104);
  EXPECT_EQ(durable.utility_slots[0].count, 3);
  EXPECT_EQ(durable.stackables.at(kPotionCmsId), 5);
  verify_packets(packets);
}

TEST_F(TutorialTest, PotionConsumesAtItsEffectOnceAndPersistsAcrossReconnect) {
  durable.stackables[kPotionCmsId] = 5;
  durable.utility_slots[0] = {kPotionCmsId, 5};
  auto client = session(durable);
  const auto start = client.receive(20010, potion_start(), 0, position, kTestTicks);
  ASSERT_EQ(start.size(), 2);
  EXPECT_EQ(start.at(0).opcode, 53);
  EXPECT_EQ(start.at(1).opcode, 14);
  EXPECT_EQ(client.next_action_tick(), kTestTicks + 4350000);
  EXPECT_EQ(commits, 0);
  verify_packets(start);

  EXPECT_TRUE(client.receive(20011, potion_effect(), 1, position, kTestTicks + 4000000).empty());
  EXPECT_TRUE(client.advance(kTestTicks + 4349999, position).empty());
  const auto effect = client.advance(kTestTicks + 4350000, position);
  ASSERT_EQ(effect.size(), 2);
  EXPECT_EQ(effect.at(0).opcode, 169);
  EXPECT_EQ(effect.at(0).message.at("stackables").at(0).at("quantity"), 4);
  EXPECT_EQ(effect.at(1).opcode, 104);
  EXPECT_EQ(durable.stackables.at(kPotionCmsId), 4);
  EXPECT_EQ(durable.utility_slots[0].count, 4);
  EXPECT_EQ(commits, 1);
  verify_packets(effect);
  EXPECT_TRUE(client.advance(kTestTicks + 5000000, position).empty());
  EXPECT_TRUE(client.receive(20011, potion_effect(), 2, position, kTestTicks + 5000000).empty());
  EXPECT_EQ(commits, 1);

  auto reconnected = session(durable);
  EXPECT_EQ(
      reconnected.receive(20011, potion_effect(), 0, position, kTestTicks + 6000000).at(0).opcode,
      2);
  EXPECT_EQ(reconnected.progress().stackables.at(kPotionCmsId), 4);
  verify_packets(reconnected.snapshot());
  EXPECT_EQ(client.receive(20010, potion_start(), 3, position, kTestTicks + 30000000).at(0).opcode,
            2);
  position.x_mm = 123;
  const auto ended = client.advance(kTestTicks + 30000000, position);
  ASSERT_EQ(ended.size(), 1);
  EXPECT_EQ(ended.at(0).opcode, 15);
  EXPECT_EQ(ended.at(0).message.at("end_skill").at("pos").at("millimeter_x"), 123);
  EXPECT_FALSE(client.next_action_tick());
  verify_packets(ended);
}

TEST_F(TutorialTest, PotionRejectsMissingInventoryForgedEffectsAndPrematureReuse) {
  auto empty = session();
  EXPECT_EQ(empty.receive(20010, potion_start(), 0, position, kTestTicks).at(0).opcode, 2);
  durable.stackables[kPotionCmsId] = 1;
  auto client = session(durable);
  auto wrong_start = potion_start();
  wrong_start["start_skill"]["skill_id"] = 1001;
  EXPECT_EQ(client.receive(20010, wrong_start, 1, position, kTestTicks).at(0).opcode, 2);
  verify_packets(client.receive(20010, potion_start(), 2, position, kTestTicks));
  EXPECT_EQ(client.receive(20010, potion_start(2), 3, position, kTestTicks + 5000000).at(0).opcode,
            2);
  EXPECT_EQ(client.receive(20011, potion_effect(), 4, position, kTestTicks).at(0).opcode, 2);
  auto wrong_effect = potion_effect();
  wrong_effect["skill_effect"]["effect_id"] = 100000145;
  EXPECT_EQ(client.receive(20011, wrong_effect, 5, position, kTestTicks + 4000000).at(0).opcode, 2);
  wrong_effect = potion_effect();
  wrong_effect["skill_effect"]["targets"][0]["unit_id"] = "8";
  EXPECT_EQ(client.receive(20011, wrong_effect, 6, position, kTestTicks + 4000000).at(0).opcode, 2);
  EXPECT_EQ(client.receive(20011, potion_effect(2), 7, position, kTestTicks + 4000000).at(0).opcode,
            2);
  EXPECT_EQ(commits, 0);
  verify_packets(client.receive(20011, potion_effect(), 8, position, kTestTicks + 4000000));
  EXPECT_EQ(durable.stackables.at(kPotionCmsId), 1);
  verify_packets(client.advance(kTestTicks + 5000000, position));
  EXPECT_EQ(durable.stackables.at(kPotionCmsId), 0);
  EXPECT_EQ(client.receive(20010, potion_start(2), 9, position, kTestTicks + 30000000).at(0).opcode,
            2);
}

TEST_F(TutorialTest, PotionStorageFailureCannotPublishConsumption) {
  durable.stackables[kPotionCmsId] = 5;
  TutorialSession client(data, 7, durable, [](const auto &, const auto &) { return false; });
  client.receive(20010, potion_start(), 0, position, kTestTicks);
  EXPECT_THROW(client.advance(kTestTicks + 5000000, position), std::runtime_error);
  EXPECT_EQ(client.progress().stackables.at(kPotionCmsId), 5);
  EXPECT_EQ(durable.stackables.at(kPotionCmsId), 5);
}

TEST_F(TutorialTest, PotionConsumesWithoutAnyClientEffectReport) {
  durable.stackables[kPotionCmsId] = 5;
  durable.utility_slots[0] = {kPotionCmsId, 5};
  auto client = session(durable);
  client.receive(20010, potion_start(), 0, position, kTestTicks);
  const auto packets = client.advance(kTestTicks + 5000000, position);
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(durable.stackables.at(kPotionCmsId), 4);
  EXPECT_EQ(durable.utility_slots[0].count, 4);
  EXPECT_EQ(commits, 1);
  EXPECT_TRUE(client.advance(kTestTicks + 6000000, position).empty());
  EXPECT_EQ(commits, 1);
  verify_packets(packets);
}

TEST_F(TutorialTest, QuestTriggersAdvanceChainAndClientCannotClaimMonsterKills) {
  auto client = session();
  auto request = quest_trigger(10000040);
  request["trigger_volume_enter"]["trigger_volume_y"] = "999";
  EXPECT_EQ(client.receive(20016, request, 0, position, kTestTicks).at(0).opcode, 2);
  verify_packets(client.receive(20016, quest_trigger(10000040), 1, position, kTestTicks));
  EXPECT_EQ(
      client.receive(20018, {{"quest_cms_id", 10000040}}, 2, position, kTestTicks).at(0).opcode, 2);
  verify_packets(client.receive(20019, quest_trigger(10000040, true), 3, position, kTestTicks));
  const auto completed =
      client.receive(20018, {{"quest_cms_id", 10000040}}, 4, position, kTestTicks);
  ASSERT_EQ(completed.size(), 2);
  EXPECT_TRUE(durable.quests.at(10000040).complete);
  EXPECT_TRUE(durable.quests.contains(10000042));
  verify_packets(completed);
  EXPECT_EQ(
      client.receive(20019, quest_trigger(10000042, true), 5, position, kTestTicks).at(0).opcode,
      2);
}

TEST_F(TutorialTest, AnimatedQuestObjectConsumesKeyAtomicallyAndCannotRepeat) {
  durable.stackables[100109] = 1;
  durable.quests[10000042].objectives[1] = 0;
  auto client = session(durable);
  Json start{{"target_unit_id", "351865195728902"},
             {"interaction_anim_id", 200},
             {"param", {{"interaction", {{"interaction_id", 1221}}}}},
             {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}}};
  verify_packets(client.receive(20069, start, 0, position, kTestTicks));
  EXPECT_EQ(
      client.receive(20070, {{"interaction_anim_id", 200}}, 1, position, kTestTicks).at(0).opcode,
      2);
  auto packets =
      client.receive(20070, {{"interaction_anim_id", 200}}, 2, position, kTestTicks + 2000000);
  EXPECT_EQ(durable.stackables.at(100109), 0);
  EXPECT_EQ(durable.quests.at(10000042).objectives.at(1), 1);
  EXPECT_EQ(durable.doodad_phases.at(351865195728902ULL), 3);
  verify_packets(packets);
  client.receive(20070, {{"interaction_anim_id", 200}}, 3, position, kTestTicks + 3000000);
  EXPECT_EQ(commits, 1);
  auto reconnected = session(durable);
  verify_packets(reconnected.sectors());
  const auto repeated_start = reconnected.receive(20069, start, 4, position, kTestTicks + 4000000);
  ASSERT_EQ(repeated_start.at(0).opcode, 166);
  verify_packets(repeated_start);
  verify_packets(reconnected.receive(20070, {{"interaction_anim_id", 200}}, 5, position,
                                     kTestTicks + 6000000));
  EXPECT_EQ(durable.stackables.at(100109), 0);
  EXPECT_EQ(durable.doodad_phases.at(351865195728902ULL), 3);
  EXPECT_EQ(commits, 1);
  verify_packets(
      client.receive(20018, {{"quest_cms_id", 10000042}}, 5, position, kTestTicks + 5000000));
  EXPECT_EQ(durable.experience, 200);
  EXPECT_TRUE(durable.quests.contains(10000043));
  EXPECT_EQ(client.receive(20019, quest_trigger(10000043, true), 6, position, kTestTicks + 6000000)
                .at(0)
                .opcode,
            2);
  EXPECT_EQ(client.receive(20018, {{"quest_cms_id", 10000043}}, 7, position, kTestTicks + 7000000)
                .at(0)
                .opcode,
            2);
}

TEST_F(TutorialTest, PersistenceFailurePublishesNoReward) {
  TutorialSession client(data, 7, {}, [](const auto &, const auto &) -> bool {
    throw std::runtime_error("Injected database failure");
  });
  EXPECT_THROW(client.receive(20068, interaction(), 0, position, kTestTicks), std::runtime_error);
  EXPECT_TRUE(client.progress().stackables.empty());
  EXPECT_TRUE(client.progress().adventure_objects.empty());
}

TEST_F(TutorialTest, DialogueProgressRequiresTheActiveCatalogNpcAndSelection) {
  auto document = test_definitions();
  document["civilians"].push_back({{"unit_id", "1152921504606851074"},
                                   {"cms_id", 1001671},
                                   {"position", {0, 0, 0}},
                                   {"yaw", 0}});
  document["quests"].push_back({{"cms_id", 10000044},
                                {"auto_complete", false},
                                {"sequential", true},
                                {"accept_triggers", Json::array()},
                                {"experience", 0},
                                {"rewards", Json::array()},
                                {"progresses",
                                 {{{"sequence", 1},
                                   {"group", 1},
                                   {"kind", "DialogueSelection"},
                                   {"parameters",
                                    {{"targetId", 1001671},
                                     {"dialogueId", 101010652},
                                     {"sequenceOrder", 1},
                                     {"selectOrder", 1}}}}}}});
  data = std::make_shared<TutorialData>(std::move(document));
  durable.quests[10000044].objectives[1] = 0;
  auto client = session(durable);
  Json request{{"quest_cms_id", 10000044},
               {"quest_progress_sequence_id", 1},
               {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
               {"dialogue_selection",
                {{"unit_id", "1152921504606851074"},
                 {"dialogue_cms_id", 101010652},
                 {"sequence_order", 1},
                 {"select_order", 1}}}};
  EXPECT_EQ(client.receive(20019, request, 0, position, kTestTicks).at(0).opcode, 2);
  verify_packets(
      client.receive(20065, {{"target_unit_id", "1152921504606851074"}, {"pos", request.at("pos")}},
                     1, position, kTestTicks));
  auto wrong_selection = request;
  wrong_selection["dialogue_selection"]["select_order"] = 2;
  EXPECT_EQ(client.receive(20019, wrong_selection, 2, position, kTestTicks).at(0).opcode, 2);
  verify_packets(client.receive(20019, request, 3, position, kTestTicks));
  EXPECT_EQ(durable.quests.at(10000044).objectives.at(1), 1);
  verify_packets(client.receive(20066, Json::object(), 4, position, kTestTicks));
  verify_packets(client.sectors());
}

TEST(TutorialData, RejectsUnknownBuildAndInvalidRewards) {
  auto document = test_definitions();
  document["build_id"] = "unverified-client-build";
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
  document = test_definitions();
  document["objects"][0]["rewards"][0]["quantity"] = -1;
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
}

Json test_npc_spawn() {
  return {{"unit_id", "2305843009213698050"},
          {"cms_id", 5001082},
          {"behavior", "Monster"},
          {"faction", "EX5Faction_Bandit"},
          {"position", {-1026849, 839774, -699937}},
          {"yaw", -70},
          {"spawner_guid", {"14028797334773314400", "6330217708590029032"}},
          {"anim_stance_key", 0}};
}

TEST_F(TutorialTest, NpcPlacementsUseLocalIdsAndAuthoredTransformsOnReconnect) {
  auto document = test_definitions();
  const auto monster = test_npc_spawn();
  auto civilian = monster;
  civilian["unit_id"] = "1152921504606851074";
  civilian["cms_id"] = 1001671;
  civilian["behavior"] = "Civilian";
  civilian["faction"] = "EX5Faction_TownNpc";
  civilian["position"] = {-1383698, 751225, -695744};
  civilian["yaw"] = 35;
  civilian["spawner_guid"] = {"15264741207652092833", "16482980755502628751"};
  document["civilians"] = Json::array({civilian});
  document["npc_spawns"] = Json::array({monster, civilian});
  data = std::make_shared<TutorialData>(std::move(document));

  const auto packets = session().sectors();
  verify_packets(packets);
  std::size_t npc_count{};
  for (const auto &packet : packets) {
    if (packet.opcode != 10) {
      continue;
    }
    ++npc_count;
    const auto unit_id = packet.message.at("unit_id").get<std::string>();
    const auto &expected = unit_id == monster.at("unit_id").get<std::string>() ? monster : civilian;
    const auto &nub = packet.message.at("nub");
    EXPECT_EQ(nub.at("pos").at("millimeter_x"), expected.at("position").at(0));
    EXPECT_EQ(nub.at("pos").at("millimeter_y"), expected.at("position").at(1));
    EXPECT_EQ(nub.at("pos").at("millimeter_z"), expected.at("position").at(2));
    EXPECT_EQ(nub.at("yaw"), expected.at("yaw"));
    EXPECT_EQ(nub.at("npc").at("faction_type"), expected.at("faction"));
    EXPECT_EQ(nub.at("npc").at("spawner_id_x"), expected.at("spawner_guid").at(0));
    EXPECT_EQ(nub.at("npc").at("base_unit_id").get<std::string>(), unit_id);
  }
  EXPECT_EQ(npc_count, 2);
  const auto reconnected = session().sectors();
  ASSERT_EQ(reconnected.size(), packets.size());
  for (std::size_t index = 0; index < packets.size(); ++index) {
    EXPECT_EQ(reconnected[index].message, packets[index].message);
  }
  EXPECT_EQ(commits, 0);

  // Placement alone must not turn a monster into a dialogue target.
  auto client = session();
  const Json request{{"target_unit_id", monster.at("unit_id")},
                     {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}}};
  EXPECT_EQ(client.receive(20065, request, 0, position, kTestTicks).at(0).opcode, 2);
}

TEST(TutorialData, RejectsDuplicateNpcIdsUnknownNamespacesAndMismatchedCivilians) {
  auto document = test_definitions();
  const auto monster = test_npc_spawn();
  document["npc_spawns"] = Json::array({monster, monster});
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
  document["npc_spawns"] = Json::array({monster});
  document["npc_spawns"][0]["unit_id"] = "12290";
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
  document["npc_spawns"] = Json::array({monster});
  document["npc_spawns"][0]["spawner_guid"] = {"18446744073709551616", "1"};
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
  document["npc_spawns"] = Json::array({monster});
  document["npc_spawns"][0]["position"] = {0.5, 0, 0};
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
  document["npc_spawns"] = Json::array({monster});
  document["npc_spawns"][0]["initially_active"] = "false";
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
  document["npc_spawns"] = Json::array({monster});
  document["civilians"] = Json::array({{{"unit_id", "1152921504606851074"},
                                        {"cms_id", 1001671},
                                        {"position", {0, 0, 0}},
                                        {"yaw", 0}}});
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
}

TEST_F(TutorialTest, ScriptControlledNpcRemainsMappedWithoutAppearingAtEntry) {
  auto document = test_definitions();
  auto monster = test_npc_spawn();
  monster["initially_active"] = false;
  document["npc_spawns"] = Json::array({monster});
  data = std::make_shared<TutorialData>(std::move(document));
  EXPECT_EQ(data->npcs.size(), 1);
  const auto packets = session().sectors();
  EXPECT_EQ(std::ranges::count_if(packets, [](const auto &packet) { return packet.opcode == 10; }),
            0);
}

TEST(TutorialData, ExportedNpcPlacementsEncodeUsingTheRuntimeProfile) {
  const auto *profile = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!profile) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS to validate the private static export";
  }
  const auto data = TutorialData::load(profile);
  TutorialSession client(data, 7, {}, [](const auto &, const auto &) { return true; });
  const auto packets = client.sectors();
  verify_packets(packets);
  EXPECT_EQ(data->npcs.size(), 12);
  EXPECT_EQ(data->civilians.size(), 1);
  EXPECT_EQ(std::ranges::count_if(packets, [](const auto &packet) { return packet.opcode == 10; }),
            9);
}

void verify_storage(const CharacterRepositories &store) {
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account account;
  account.name = "tutorial_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(store.accounts->add_account(account));
  const auto owned = store.accounts->account(account.name);
  ASSERT_TRUE(owned);
  Character character;
  character.account_id = owned->id;
  character.name = "Tutor" + suffix;
  character = store.characters->create(character);
  auto initial = store.character_state->load_world_progress(owned->id, character.id, 1);
  auto next = initial;
  next.revision = 1;
  next.stackables[kPotionCmsId] = 5;
  next.gold = 37;
  next.experience = 200;
  next.adventure_objects[20080000] = 2;
  next.quests[10000040] = {true, 1, {{1, 1}}};
  next.quests[10000040].credited_kills.insert(2305843009213698050ULL);
  next.doodad_phases[351865195728902ULL] = 3;
  next.utility_slots[0] = {kPotionCmsId, 3};
  EXPECT_THROW(store.character_state->load_world_progress(owned->id + 10000, character.id, 1),
               std::runtime_error);
  EXPECT_FALSE(store.character_state->save_world_progress(owned->id + 10000, character.id, 1,
                                                          initial, next));
  std::atomic<int> committed{};
  auto save = [&] {
    if (store.character_state->save_world_progress(owned->id, character.id, 1, initial, next)) {
      ++committed;
    }
  };
  std::thread first(save);
  std::thread second(save);
  first.join();
  second.join();
  EXPECT_EQ(committed, 1);
  EXPECT_EQ(store.character_state->load_world_progress(owned->id, character.id, 1), next);
  auto regressed_objective = next;
  regressed_objective.revision = 2;
  regressed_objective.quests.at(10000040).objectives.at(1) = 0;
  EXPECT_THROW(store.character_state->save_world_progress(owned->id, character.id, 1, next,
                                                          regressed_objective),
               std::invalid_argument);
  auto missing_receipt = next;
  missing_receipt.revision = 2;
  missing_receipt.quests.at(10000040).credited_kills.clear();
  EXPECT_THROW(
      store.character_state->save_world_progress(owned->id, character.id, 1, next, missing_receipt),
      std::invalid_argument);
  auto invalid = next;
  invalid.revision = 2;
  invalid.stackables[kPotionCmsId] = -1;
  EXPECT_THROW(
      store.character_state->save_world_progress(owned->id, character.id, 1, next, invalid),
      std::invalid_argument);
  EXPECT_EQ(store.character_state->load_world_progress(owned->id, character.id, 1), next);
  auto reset = next;
  reset.revision = 2;
  reset.adventure_objects.clear();
  EXPECT_THROW(store.character_state->save_world_progress(owned->id, character.id, 1, next, reset),
               std::invalid_argument);
  EXPECT_EQ(store.character_state->load_world_progress(owned->id, character.id, 1), next);
}

TEST(TutorialStorage, MemoryOwnershipAtomicityAndRevisionConflict) {
  MemoryRepositories repositories;
  verify_storage(repositories.character_repositories());
}

TEST(TutorialStorage, PostgresOwnershipAtomicityAndRevisionConflict) {
  const auto *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  verify_storage(make_postgres_character_repositories(connection));
}

} // namespace
} // namespace chronicles
