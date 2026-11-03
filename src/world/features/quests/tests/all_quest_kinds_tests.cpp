// Quest mechanics added for full catalog coverage: job boards, abandonment and repetition,
// acceptance costs, doodad/memo/server-offered starts, extra rewards and the state, event and
// unplaced objectives. Synthetic catalog fixtures; no original-client behavior is implied.

#include "protocol/protocol.hpp"
#include "world/features/quests/world_quests.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kGuide = (1ULL << 60) | (1ULL << 12) | 2;
constexpr int kGuideTemplate = 1001539;
constexpr int kMap = 9012;
// Spot unit ids: sector << 32 | local_id << 12 | 6.
constexpr std::uint64_t kBoardSpot = (77ULL << 32) | (5ULL << 12) | 6;
constexpr std::uint64_t kDoodadSpot = (77ULL << 32) | (6ULL << 12) | 6;
constexpr std::uint64_t kMemoSpot = (77ULL << 32) | (7ULL << 12) | 6;
constexpr int kBoard = 101;
constexpr int kBoardDoodad = 67;
constexpr int kQuestDoodad = 763;
constexpr int kMemo = 2002052;
constexpr int kMonster = 5004213;
// 2026-10-30 12:00:00 UTC in Unix epoch milliseconds; the schedule starts at 2022-09-01.
constexpr std::int64_t kNow = 1793361600000;
constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;

Json placement(std::uint64_t unit_id) {
  return {{"world", "ArcheWorld"}, {"unit_id", std::to_string(unit_id)}, {"center", {0, 0, 0}}};
}

Json quest(int cms_id, Json accept, Json progresses, Json completion = {{"kind", "Auto"}}) {
  return {{"cms_id", cms_id},
          {"type", "None"},
          {"accept", std::move(accept)},
          {"conditions", Json::array()},
          {"completion", std::move(completion)},
          {"supplies", Json::array()},
          {"require_gold", 0},
          {"require_items", Json::array()},
          {"auto_complete", false},
          {"sequential", false},
          {"abandonable", true},
          {"progresses", std::move(progresses)},
          {"experience", 0},
          {"rewards", Json::array()},
          {"loot_packs", Json::array()},
          {"guild_experience", 0},
          {"mates", Json::array()},
          {"mate_intimacy", Json::array()},
          {"build_points", Json::array()},
          {"dependent_items", Json::array()},
          {"unsupported", Json::array()}};
}

Json objective(int sequence, const char *kind, Json parameters) {
  return {{"sequence", sequence},
          {"group", sequence},
          {"kind", kind},
          {"parameters", std::move(parameters)}};
}

Json npc_accept() {
  return Json::array({{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}});
}

Json catalog_fixture() {
  Json quests = Json::array();

  // Job board quests: three in the slot pools, turned in at the board, with guild EXP.
  for (const int id : {601, 602, 603}) {
    auto job = quest(id, Json::array({{{"kind", "JobBoard"}, {"board_id", kBoard}}}),
                     Json::array({objective(1, "HuntMonster", {{"npcId", kMonster}, {"count", 1}})}),
                     {{"kind", "JobBoard"}, {"board_id", kBoard}});
    job["type"] = "JobBoardCompany";
    job["rewards"] = Json::array({{{"item_cms_id", 0}, {"quantity", 100}}});
    job["guild_experience"] = 10;
    quests.push_back(job);
  }

  // Acceptance costs: 500 gold and a quest item, checked but not consumed.
  auto costly = quest(610, npc_accept(), Json::array());
  costly["require_gold"] = 500;
  costly["require_items"] = Json::array({{{"item_cms_id", 120343}, {"quantity", 1}}});
  quests.push_back(costly);

  // Accepted by using a placed doodad or reading a placed memo.
  quests.push_back(quest(620,
                         Json::array({{{"kind", "InteractDoodad"},
                                       {"doodad_id", kQuestDoodad},
                                       {"target_phase", 1},
                                       {"doodads", Json::array({placement(kDoodadSpot)})}}}),
                         Json::array({objective(1, "InteractDoodad",
                                                {{"doodadId", kQuestDoodad},
                                                 {"targetPhase", 1},
                                                 {"count", 1}})})));
  quests.push_back(quest(621,
                         Json::array({{{"kind", "InteractMemo"},
                                       {"memo_id", kMemo},
                                       {"memos", Json::array({placement(kMemoSpot)})}}}),
                         Json::array()));

  // Server-offered starts: an NPC anchor, a map anchor after its predecessor, and GM-only.
  quests.push_back(quest(630,
                         Json::array({{{"kind", "ServerOffered"},
                                       {"npc_ids", {kGuideTemplate}},
                                       {"monster_ids", Json::array()},
                                       {"doodad_ids", Json::array()},
                                       {"zone_maps", Json::array()}}}),
                         Json::array({objective(1, "InteractNpc",
                                                {{"npcId", kGuideTemplate}, {"count", 1}})})));
  auto offered_on_map = quest(632,
                              Json::array({{{"kind", "ServerOffered"},
                                            {"npc_ids", Json::array()},
                                            {"monster_ids", Json::array()},
                                            {"doodad_ids", Json::array()},
                                            {"zone_maps", {kMap}},
                                            {"after_quest_id", 631}}}),
                              Json::array());
  offered_on_map["type"] = "HiddenAutoComplete";
  offered_on_map["auto_complete"] = true;
  offered_on_map["rewards"] = Json::array({{{"item_cms_id", 0}, {"quantity", 7}}});
  quests.push_back(quest(631, npc_accept(), Json::array()));
  quests.push_back(offered_on_map);
  quests.push_back(quest(640, Json::array({{{"kind", "GmOnly"}}}), Json::array()));

  // Conditions: an owned item, a kill play record and an activated adventure object.
  auto conditioned = quest(650, npc_accept(), Json::array());
  conditioned["conditions"] = Json::array(
      {{{"group", 1}, {"operator", "AND"}, {"negated", false}, {"kind", "OwnItem"},
        {"item_id", 120359}, {"count", 1}},
       {{"group", 1}, {"operator", "AND"}, {"negated", false}, {"kind", "PlayRecord"},
        {"play_record_id", 30}, {"count", 1}, {"comparison", "GreaterThanOrEqual"},
        {"record", {{"kind", "HuntMonster"}, {"npc_id", kMonster}}}},
       {{"group", 1}, {"operator", "AND"}, {"negated", false}, {"kind", "AdventureObject"},
        {"adventure_object_id", 6020051}, {"active", true}}});
  quests.push_back(conditioned);

  // Extra rewards: a certain loot pack, a mate and its intimacy.
  auto rewarding = quest(660, npc_accept(), Json::array());
  rewarding["loot_packs"] = Json::array(
      {{{"loot_pack_id", 1}, {"pack_rate_percent", 100.0},
        {"groups", Json::array({{{"rate_percent", 100.0},
                                 {"items", Json::array({{{"item_cms_id", 70102001},
                                                         {"min_quantity", 3},
                                                         {"max_quantity", 3},
                                                         {"weight", 1}}})}}})}}});
  rewarding["mates"] = Json::array({2});
  rewarding["mate_intimacy"] = Json::array({{{"mate_id", 2}, {"amount", 100}, {"limit", 150}}});
  quests.push_back(rewarding);

  // State, report and event objectives, plus a step whose volume was never placed.
  quests.push_back(quest(
      670, npc_accept(),
      Json::array({objective(1, "TimeElapsed", {{"time", 6000}}),
                   objective(2, "CheckHp", {{"target", "EX5GameUnitType_Player"}, {"hpRate", 50}}),
                   objective(3, "CompanyContributionTotal", {{"count", 1500}}),
                   objective(4, "housingFunishing", Json::object()),
                   objective(5, "CounterUseItem", {{"itemId", 10140}, {"count", 1}}),
                   objective(6, "TradePayment",
                             {{"tradingPostId", 2000}, {"tradeGoodId", 1101}, {"count", 1}}),
                   objective(7, "CounterUseSkill", {{"skillId", 1100000}, {"count", 1}}),
                   objective(8, "AddStatusEffect", {{"statusEffectId", 7000}, {"count", 1}}),
                   objective(9, "AdventureLogbookPercentage",
                             {{"zoneKey", "Zone_01"}, {"percentage", 18}}),
                   {{"sequence", 10},
                    {"group", 10},
                    {"kind", "TriggerVolumeEnter"},
                    {"parameters", Json::object()},
                    {"auto_credit", true}},
                   {{"sequence", 11},
                    {"group", 11},
                    {"kind", "EtherealHuntMonster"},
                    {"parameters", {{"npcGroupId", 500228}, {"count", 4}, {"rate", 100},
                                    {"minValue", 4}, {"maxValue", 4}}},
                    {"npc_ids", {500207, 500227}}}})));

  // First Hunt shape (10000125): take the knife from a quest-gated looting object, then
  // equip it. The object's InteractionCondition asks for objective group 1 in progress.
  quests.push_back(quest(680, npc_accept(),
                         Json::array({objective(1, "AdventureObjectStatus",
                                                {{"adventureObjectId", 20019119},
                                                 {"state", "EX5AdventureObjectState_Activated"}}),
                                      objective(2, "EquipItem",
                                                {{"equipSlotType", "EX5EquipSlotType_Tool_1"},
                                                 {"itemId", 11200301}})})));

  Json slots = Json::array();
  for (int order = 1; order <= 3; ++order) {
    slots.push_back({{"company_type", 2},
                     {"is_guild", false},
                     {"company_level", 2},
                     {"guild_level", 0},
                     {"order", order},
                     {"quest_ids", {601, 602, 603}}});
  }
  return {{"format_version", 1},
          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
          {"map_worlds", {{std::to_string(kMap), "ArcheWorld"}}},
          {"job_boards",
           {{std::to_string(kBoard),
             {{"doodad_id", kBoardDoodad},
              {"doodads", Json::array({placement(kBoardSpot)})},
              {"reset_cost", 100},
              {"slots", slots}}}}},
          {"job_board_schedule",
           {{"pick_count", 3}, {"start_epoch_ms", 1661990400000}, {"period_ms", kDayMs}}},
          {"quests", quests}};
}

class AllQuestKinds : public testing::Test {
protected:
  std::shared_ptr<const QuestCatalog> catalog =
      std::make_shared<const QuestCatalog>(catalog_fixture());
  WorldQuestBook book{catalog, 4097};
  WorldProgress progress;
  Character character;
  AnnouncedUnit guide{kGuide, kGuideTemplate, kMap, "", {1000, 0, 0}};
  WorldPosition position{kMap, 0, 0, 0, 0};
  std::int64_t now{kNow};
  std::optional<int> hp_percent;
  std::optional<QuestCompanyStanding> company;
  std::uint32_t sequence{};

  QuestWorldView view() {
    QuestWorldView result;
    result.position = position;
    result.character = &character;
    result.find_unit = [this](std::uint64_t unit_id) -> const AnnouncedUnit * {
      return unit_id == guide.unit_id ? &guide : nullptr;
    };
    result.company = company;
    result.now_epoch_ms = now;
    result.hp_percent = hp_percent;
    return result;
  }

  Json here() const {
    return {{"millimeter_x", position.x_mm},
            {"millimeter_y", position.y_mm},
            {"millimeter_z", position.z_mm}};
  }

  void commit(QuestResult result) {
    Protocol protocol(CHRONICLES_PROTOCOL_DIR);
    for (const auto &packet : result.packets) {
      const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
      EXPECT_EQ(decoded.at("status"), "decoded") << packet.opcode;
    }
    if (result.next) {
      progress = std::move(*result.next);
    }
  }

  std::vector<GameplayPacket> send(std::uint16_t opcode, const Json &request) {
    auto result = book.receive(opcode, request, sequence++, progress, view());
    auto packets = result.packets;
    commit(std::move(result));
    return packets;
  }

  std::vector<GameplayPacket> accept_with_unit(int quest_id, std::uint64_t unit_id) {
    return send(20016, {{"quest_cms_id", quest_id},
                        {"pos", here()},
                        {"unit", {{"unit_id", std::to_string(unit_id)}}}});
  }

  static bool has_opcode(const std::vector<GameplayPacket> &packets, std::uint16_t opcode) {
    return std::ranges::any_of(packets,
                               [opcode](const GameplayPacket &packet) {
                                 return packet.opcode == opcode;
                               });
  }

  void blue_salt_member(int level) {
    QuestCompanyStanding standing;
    standing.company_type = 2;
    standing.position_level = level;
    company = standing;
  }

  std::vector<int> board_offers() {
    const auto packets =
        send(20020, {{"quest_job_board_cms_id", kBoard},
                     {"doodad_unit_id", std::to_string(kBoardSpot)},
                     {"refresh_using_cost", false}});
    for (const auto &packet : packets) {
      if (packet.opcode == 30) {
        return packet.message.at("jobs").at(std::to_string(kBoard)).at("quest_cms_ids");
      }
    }
    return {};
  }
};

TEST_F(AllQuestKinds, JobBoardOffersAcceptsTurnsInAndRepeatsAfterTheDailyRefresh) {
  // Without the board's company standing the board offers nothing.
  EXPECT_TRUE(board_offers().empty());

  blue_salt_member(2);
  const auto offers = board_offers();
  ASSERT_EQ(offers.size(), 3U);
  EXPECT_EQ(board_offers(), offers);
  ASSERT_TRUE(progress.job_boards.contains(kBoard));

  // Only the board's own doodad accepts the offered job.
  const int job = offers.front();
  EXPECT_EQ(accept_with_unit(job, kDoodadSpot).front().opcode, 2);
  EXPECT_TRUE(has_opcode(accept_with_unit(job, kBoardSpot), 28));
  commit(book.record_monster_defeat(9001, kMonster, progress, view()));
  const auto turned_in =
      send(20018, {{"quest_cms_id", job}, {"unit_id", std::to_string(kBoardSpot)}});
  EXPECT_TRUE(has_opcode(turned_in, 27));
  EXPECT_TRUE(progress.quests.at(job).complete);
  EXPECT_EQ(progress.gold, 100);
  EXPECT_EQ(book.guild_experience_earned(WorldProgress{}, progress), 10);

  // The same day it stays complete; on the next day it is offered and accepted again.
  EXPECT_EQ(accept_with_unit(job, kBoardSpot).front().opcode, 27);
  now += kDayMs;
  const auto before_renewal = progress;
  board_offers();
  EXPECT_TRUE(progress.quests.at(job).abandoned);
  EXPECT_EQ(progress.quests.at(job).success_count, 1);
  const auto renewed_offers = board_offers();
  if (std::ranges::find(renewed_offers, job) != renewed_offers.end()) {
    EXPECT_TRUE(has_opcode(accept_with_unit(job, kBoardSpot), 28));
    EXPECT_FALSE(progress.quests.at(job).abandoned);
    EXPECT_GT(progress.quests.at(job).attempt, before_renewal.quests.at(job).attempt);
  }
}

TEST_F(AllQuestKinds, PaidRefreshCostsTheBoardGoldAndRefusesWithoutIt) {
  blue_salt_member(2);
  board_offers();
  const Json refresh{{"quest_job_board_cms_id", kBoard},
                     {"doodad_unit_id", std::to_string(kBoardSpot)},
                     {"refresh_using_cost", true}};
  EXPECT_EQ(send(20020, refresh).front().opcode, 2);

  progress.gold = 250;
  const auto refreshed = send(20020, refresh);
  EXPECT_TRUE(has_opcode(refreshed, 169));
  EXPECT_TRUE(has_opcode(refreshed, 30));
  EXPECT_EQ(progress.gold, 150);
  EXPECT_EQ(progress.job_boards.at(kBoard).refreshes, 1);
}

TEST_F(AllQuestKinds, AbandonLeavesATombstoneAndAcceptanceStartsANewAttempt) {
  progress.gold = 600;
  progress.stackables[120343] = 1;
  ASSERT_TRUE(has_opcode(accept_with_unit(610, kGuide), 28));

  const auto abandoned = send(20017, {{"quest_cms_id", 610}});
  ASSERT_EQ(abandoned.front().opcode, 29);
  EXPECT_FALSE(abandoned.front().message.at("force").get<bool>());
  EXPECT_TRUE(progress.quests.at(610).abandoned);
  EXPECT_TRUE(book.snapshot(progress).message.at("quests").empty());
  EXPECT_EQ(send(20017, {{"quest_cms_id", 610}}).front().opcode, 2);

  ASSERT_TRUE(has_opcode(accept_with_unit(610, kGuide), 28));
  EXPECT_FALSE(progress.quests.at(610).abandoned);
  EXPECT_EQ(progress.quests.at(610).attempt, 2);
  EXPECT_EQ(progress.quests.at(610).started_epoch_ms, kNow);
}

TEST_F(AllQuestKinds, AcceptanceCostsAreCheckedWithTheirOwnErrorsAndNotConsumed) {
  const auto no_gold = accept_with_unit(610, kGuide);
  EXPECT_EQ(no_gold.front().message.at("ec"), "Quest_IsNotAcceptable_RequireGold");
  progress.gold = 500;
  const auto no_item = accept_with_unit(610, kGuide);
  EXPECT_EQ(no_item.front().message.at("ec"), "Quest_IsNotAcceptable_RequireItemId");
  progress.stackables[120343] = 1;
  EXPECT_TRUE(has_opcode(accept_with_unit(610, kGuide), 28));
  EXPECT_EQ(progress.gold, 500);
  EXPECT_EQ(progress.stackables.at(120343), 1);
}

TEST_F(AllQuestKinds, DoodadAndMemoStartsNeedTheirPlacedObjectNearThePlayer) {
  EXPECT_EQ(accept_with_unit(620, kMemoSpot).front().opcode, 2);
  EXPECT_TRUE(has_opcode(accept_with_unit(620, kDoodadSpot), 28));
  EXPECT_TRUE(has_opcode(accept_with_unit(621, kMemoSpot), 28));

  position = {kMap, 50000, 0, 0, 0};
  progress = WorldProgress{};
  EXPECT_EQ(accept_with_unit(621, kMemoSpot).front().opcode, 2);
}

TEST_F(AllQuestKinds, DoodadInteractionEventsStartAndAdvanceDoodadQuests) {
  // The wrong phase starts nothing; the authored phase starts and credits the quest.
  EXPECT_FALSE(book.record_events({{QuestEventKind::DoodadInteracted, kQuestDoodad, 2, 1}},
                                  progress, view())
                   .next);
  commit(book.record_events({{QuestEventKind::DoodadInteracted, kQuestDoodad, 1, 1}}, progress,
                            view()));
  ASSERT_TRUE(progress.quests.contains(620));
  EXPECT_EQ(progress.quests.at(620).objectives.at(1), 1);
}

TEST_F(AllQuestKinds, ServerOfferedQuestsStartAtTheirAnchorAndGmOnlyQuestsRefuseClients) {
  send(20065, {{"target_unit_id", std::to_string(kGuide)}, {"pos", here()}});
  ASSERT_TRUE(progress.quests.contains(630));
  EXPECT_EQ(progress.quests.at(630).objectives.at(1), 1);

  // The map offer waits for the preceding step of its series, then auto-completes.
  commit(book.periodic_update(progress, view()));
  EXPECT_FALSE(progress.quests.contains(632));
  ASSERT_TRUE(has_opcode(accept_with_unit(631, kGuide), 28));
  commit(book.periodic_update(progress, view()));
  EXPECT_FALSE(progress.quests.contains(632));
  ASSERT_TRUE(has_opcode(send(20018, {{"quest_cms_id", 631}}), 27));
  commit(book.periodic_update(progress, view()));
  ASSERT_TRUE(progress.quests.contains(632));
  EXPECT_TRUE(progress.quests.at(632).complete);
  EXPECT_EQ(progress.gold, 7);

  EXPECT_EQ(send(20016, {{"quest_cms_id", 640}, {"pos", here()}}).front().opcode, 2);
  EXPECT_TRUE(book.gm_accept(640, true, progress, view()).next);
}

TEST_F(AllQuestKinds, ItemPlayRecordAndAdventureConditionsGateAcceptance) {
  EXPECT_EQ(accept_with_unit(650, kGuide).front().opcode, 2);
  progress.stackables[120359] = 1;
  progress.adventure_objects[6020051] = 2;
  EXPECT_EQ(accept_with_unit(650, kGuide).front().opcode, 2);

  // A kill of the recorded template raises play record 30.
  commit(book.record_monster_defeat(9100, kMonster, progress, view()));
  EXPECT_EQ(progress.play_records.at(30), 1);
  EXPECT_TRUE(has_opcode(accept_with_unit(650, kGuide), 28));
}

TEST_F(AllQuestKinds, QuestGatedObjectIsUsableOnlyWhileItsStepIsInProgressAndCreditsIt) {
  const Json knife_conditions = Json::array({{{"group", 1},
                                              {"operator", "AND"},
                                              {"negated", false},
                                              {"kind", "Quest"},
                                              {"quest_id", 680},
                                              {"requirement", "Progressing"},
                                              {"progress_group", 1}}});
  EXPECT_FALSE(book.requirements_met(knife_conditions, progress, view()));

  ASSERT_TRUE(has_opcode(accept_with_unit(680, kGuide), 28));
  EXPECT_TRUE(book.requirements_met(knife_conditions, progress, view()));

  // The adventure session committed the activation; the next refresh credits the step.
  progress.adventure_objects[20019119] = 2;
  ++progress.revision;
  const auto refreshed = book.periodic_update(progress, view());
  ASSERT_TRUE(refreshed.next);
  EXPECT_TRUE(has_opcode(refreshed.packets, 27));
  commit(refreshed);
  EXPECT_EQ(progress.quests.at(680).objectives.at(1), 1);
  EXPECT_FALSE(book.requirements_met(knife_conditions, progress, view()));
}

TEST_F(AllQuestKinds, CompletionGrantsRolledLootMatesAndIntimacy) {
  ASSERT_TRUE(has_opcode(accept_with_unit(660, kGuide), 28));
  const auto rewarded = send(20018, {{"quest_cms_id", 660}});
  EXPECT_TRUE(progress.quests.at(660).complete);
  EXPECT_EQ(progress.stackables.at(70102001), 3);
  ASSERT_TRUE(progress.mates.contains(2));
  EXPECT_EQ(progress.mates.at(2).intimacy, 100);
  EXPECT_TRUE(has_opcode(rewarded, 58));
  EXPECT_TRUE(has_opcode(rewarded, 59));
}

TEST_F(AllQuestKinds, StateReportAndEventObjectivesAllReachCompletion) {
  ASSERT_TRUE(has_opcode(accept_with_unit(670, kGuide), 28));
  auto &objectives = progress.quests.at(670).objectives;
  // The unplaced step is credited by the server at once.
  EXPECT_EQ(objectives.at(10), 1);

  // Time, health, company contribution and furniture are server state.
  now += 6000;
  hp_percent = 40;
  QuestCompanyStanding standing;
  standing.company_type = 2;
  standing.contribution = 1500;
  company = standing;
  progress.housing_items[3000] = HousingItemRecord{1, 1};
  commit(book.periodic_update(progress, view()));
  for (const int sequence : {1, 2, 3, 4}) {
    EXPECT_GE(progress.quests.at(670).objectives.at(sequence), 1) << sequence;
  }

  commit(book.record_events({{QuestEventKind::ItemUsed, 10140, 0, 1},
                             {QuestEventKind::TradeGoodSold, 1101, 0, 1, 2000},
                             {QuestEventKind::SkillUsed, 1100000, 0, 1},
                             {QuestEventKind::StatusEffectApplied, 7000, 0, 1}},
                            progress, view()));
  for (const int sequence : {5, 6, 7, 8}) {
    EXPECT_EQ(progress.quests.at(670).objectives.at(sequence), 1) << sequence;
  }

  const Json logbook{{"quest_cms_id", 670},
                     {"quest_progress_sequence_id", 9},
                     {"pos", here()},
                     {"adventure_logbook_percentage", {{"zone_key", "Zone_01"}, {"percentage", 10}}}};
  EXPECT_EQ(send(20019, logbook).front().opcode, 2);
  auto reached = logbook;
  reached["adventure_logbook_percentage"]["percentage"] = 18;
  EXPECT_EQ(send(20019, reached).front().opcode, 27);

  // Any member of the NPC group credits the group Ethereal hunt.
  commit(book.record_monster_defeat(9200, 500207, progress, view()));
  EXPECT_EQ(progress.quests.at(670).objectives.at(11), 4);

  EXPECT_TRUE(has_opcode(send(20018, {{"quest_cms_id", 670}}), 27));
  EXPECT_TRUE(progress.quests.at(670).complete);
}

} // namespace
} // namespace chronicles
