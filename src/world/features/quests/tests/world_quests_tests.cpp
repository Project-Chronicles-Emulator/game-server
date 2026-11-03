#include "common/currency.hpp"
#include "protocol/protocol.hpp"
#include "world/features/quests/world_quests.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kGuide = (1ULL << 60) | (1ULL << 12) | 2;
constexpr int kGuideTemplate = 1001539;
constexpr int kMap = 9012;
constexpr int kPotionCraft = 60190101;
constexpr int kMemo = 10000127;
constexpr std::uint64_t kMemoSpot = 20337237082255366ULL;
constexpr int kBoneChip = 70108003;

Json catalog_fixture() {
  const Json trigger{{"guid", {"11", "22"}},
                     {"world", "ArcheWorld"},
                     {"center", {50000, 0, 0}},
                     {"radius", 2000}};
  Json quests = Json::array();
  // Accepted from the guide NPC; objectives: walk into a volume, then talk to the guide,
  // then own two quest items. Turned in at the guide; its successor follows automatically.
  quests.push_back({{"cms_id", 501},
                    {"type", "None"},
                    {"accept", {{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}}},
                    {"conditions",
                     {{{"group", 1},
                       {"operator", "AND"},
                       {"negated", false},
                       {"kind", "CheckUnitLevel"},
                       {"min", 1},
                       {"max", 10}}}},
                    {"completion", {{"kind", "Npc"}, {"npc_ids", {kGuideTemplate}}}},
                    {"supplies", {{{"item_cms_id", 120059}, {"quantity", 1}}}},
                    {"auto_complete", false},
                    {"sequential", true},
                    {"progresses",
                     {{{"sequence", 1},
                       {"group", 1},
                       {"kind", "TriggerVolumeEnter"},
                       {"parameters", Json::object()},
                       {"trigger", trigger}},
                      {{"sequence", 2},
                       {"group", 2},
                       {"kind", "InteractNpc"},
                       {"parameters", {{"npcId", kGuideTemplate}, {"count", 1}}}},
                      {{"sequence", 3},
                       {"group", 3},
                       {"kind", "GetItem"},
                       {"parameters", {{"itemId", 100109}, {"count", 2}}}}}},
                    {"experience", 300},
                    {"rewards", {{{"item_cms_id", 0}, {"quantity", 50}}}},
                    {"next_quest", 502},
                    {"dependent_items", Json::array()},
                    {"unsupported", Json::array()}});
  quests.push_back({{"cms_id", 502},
                    {"type", "Sequential"},
                    {"accept", {{{"kind", "Sequential"}}}},
                    {"conditions", Json::array()},
                    {"completion", {{"kind", "Auto"}}},
                    {"supplies", Json::array()},
                    {"auto_complete", false},
                    {"sequential", false},
                    {"progresses",
                     {{{"sequence", 1},
                       {"group", 1},
                       {"kind", "HuntMonster"},
                       {"parameters", {{"npcId", 5001527}, {"count", 1}}}}}},
                    {"experience", 0},
                    {"rewards", Json::array()},
                    {"dependent_items", Json::array()},
                    {"unsupported", Json::array()}});
  // Company reputation is not modeled: the quest is listed but cannot be accepted.
  quests.push_back({{"cms_id", 503},
                    {"type", "JobBoardCompany"},
                    {"accept", Json::array()},
                    {"conditions", Json::array()},
                    {"completion", {{"kind", "JobBoard"}}},
                    {"supplies", Json::array()},
                    {"auto_complete", false},
                    {"sequential", false},
                    {"progresses", Json::array()},
                    {"experience", 0},
                    {"rewards", Json::array()},
                    {"dependent_items", Json::array()},
                    {"unsupported", {"accept:JobBoard", "completion:JobBoard"}}});
  // A guide quest whose company-contribution reward is not modeled: its gold reward is still
  // paid at turn-in so the region's quest line continues.
  quests.push_back({{"cms_id", 504},
                    {"type", "None"},
                    {"accept", {{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}}},
                    {"conditions", Json::array()},
                    {"completion", {{"kind", "Npc"}, {"npc_ids", {kGuideTemplate}}}},
                    {"supplies", Json::array()},
                    {"auto_complete", false},
                    {"sequential", false},
                    {"progresses", Json::array()},
                    {"experience", 40},
                    {"rewards", {{{"item_cms_id", 0}, {"quantity", 75}}}},
                    {"dependent_items", Json::array()},
                    {"unsupported", {"reward:CompanyContribution", "reward:GuildExp"}}});
  // Gated like Donella's dialogue for quest 80101045: a Blue Salt member at level 2 whose
  // contribution is held at that capped level's ceiling; the permit is ignored.
  quests.push_back({{"cms_id", 505},
                    {"type", "None"},
                    {"accept", {{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}}},
                    {"conditions",
                     {{{"group", 1},
                       {"operator", "AND"},
                       {"negated", false},
                       {"kind", "Company"},
                       {"company_type", 2},
                       {"ignore_permit", true},
                       {"level", 2},
                       {"is_max_contribution", true},
                       {"contribution", 0}}}},
                    {"completion", {{"kind", "Npc"}, {"npc_ids", {kGuideTemplate}}}},
                    {"supplies", Json::array()},
                    {"auto_complete", false},
                    {"sequential", false},
                    {"progresses", Json::array()},
                    {"experience", 0},
                    {"rewards", Json::array()},
                    {"company_unlocks", {{{"company_type", 2}, {"level", 2}}}},
                    {"dependent_items", Json::array()},
                    {"unsupported", Json::array()}});
  // Server-event objectives: a craft (Pioneer's Remedy shape), a doodad in phase 1 and an
  // Ethereal hunt whose virtual drop always adds two.
  quests.push_back({{"cms_id", 511},
                    {"type", "None"},
                    {"accept", {{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}}},
                    {"conditions", Json::array()},
                    {"completion", {{"kind", "Npc"}, {"npc_ids", {kGuideTemplate}}}},
                    {"supplies", Json::array()},
                    {"auto_complete", false},
                    {"sequential", false},
                    {"progresses",
                     {{{"sequence", 1},
                       {"group", 1},
                       {"kind", "CounterSuccessCrafting"},
                       {"parameters", {{"craftId", kPotionCraft}, {"count", 1}}}},
                      {{"sequence", 2},
                       {"group", 2},
                       {"kind", "CounterTryCrafting"},
                       {"parameters", {{"craftId", kPotionCraft}, {"count", 3}}}},
                      {{"sequence", 3},
                       {"group", 3},
                       {"kind", "InteractDoodad"},
                       {"parameters", {{"doodadId", 74}, {"targetPhase", 1}, {"count", 1}}}},
                      {{"sequence", 4},
                       {"group", 4},
                       {"kind", "EtherealHuntMonster"},
                       {"parameters",
                        {{"npcId", 5001200},
                         {"count", 3},
                         {"rate", 100},
                         {"minValue", 2},
                         {"maxValue", 2}}}}}},
                    {"experience", 0},
                    {"rewards", Json::array()},
                    {"dependent_items", Json::array()},
                    {"unsupported", Json::array()}});
  // Words of Warning shape: read the placed board OR enter the volume, then Auto completion.
  quests.push_back({{"cms_id", 512},
                    {"type", "None"},
                    {"accept", {{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}}},
                    {"conditions", Json::array()},
                    {"completion", {{"kind", "Auto"}}},
                    {"supplies", Json::array()},
                    {"auto_complete", false},
                    {"sequential", true},
                    {"progresses",
                     {{{"sequence", 1},
                       {"group", 1},
                       {"kind", "InteractMemo"},
                       {"parameters", {{"memoId", kMemo}}},
                       {"memos",
                        {{{"world", "ArcheWorld"},
                          {"unit_id", std::to_string(kMemoSpot)},
                          {"center", {0, 2000, 0}}}}}},
                      {{"sequence", 2},
                       {"group", 1},
                       {"kind", "TriggerVolumeEnter"},
                       {"parameters", Json::object()},
                       {"trigger", trigger}},
                      {{"sequence", 3},
                       {"group", 2},
                       {"kind", "InteractNpc"},
                       {"parameters", {{"npcId", kGuideTemplate}, {"count", 1}}}}}},
                    {"experience", 0},
                    {"rewards", {{{"item_cms_id", 0}, {"quantity", 10}}}},
                    {"dependent_items", Json::array()},
                    {"unsupported", Json::array()}});
  // Gathering Bone Fragments shape: five bone chips, consumed at turn-in.
  quests.push_back({{"cms_id", 513},
                    {"type", "None"},
                    {"accept", {{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}}},
                    {"conditions", Json::array()},
                    {"completion", {{"kind", "Npc"}, {"npc_ids", {kGuideTemplate}}}},
                    {"supplies", Json::array()},
                    {"auto_complete", false},
                    {"sequential", false},
                    {"progresses",
                     {{{"sequence", 1},
                       {"group", 1},
                       {"kind", "GetItem"},
                       {"parameters", {{"itemId", kBoneChip}, {"count", 5}}}}}},
                    {"experience", 0},
                    {"rewards", {{{"item_cms_id", 0}, {"quantity", 20}}}},
                    {"dependent_items", Json::array()},
                    {"unsupported", Json::array()}});
  return {{"format_version", 1},
          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
          {"map_worlds", {{"9012", "ArcheWorld"}, {"8000", "ArcheWorld_Tutorial"}}},
          {"quests", quests}};
}

class WorldQuests : public testing::Test {
protected:
  std::shared_ptr<const QuestCatalog> catalog =
      std::make_shared<const QuestCatalog>(catalog_fixture());
  WorldQuestBook book{catalog, 4097};
  WorldProgress progress;
  Character character;
  AnnouncedUnit guide{kGuide, kGuideTemplate, kMap, "", {1000, 0, 0}};
  WorldPosition position{kMap, 0, 0, 0, 0};
  std::uint32_t sequence{};
  std::optional<QuestCompanyStanding> company;

  QuestWorldView view() {
    QuestWorldView result;
    result.position = position;
    result.character = &character;
    result.find_unit = [this](std::uint64_t unit_id) -> const AnnouncedUnit * {
      return unit_id == guide.unit_id ? &guide : nullptr;
    };
    result.company = company;
    return result;
  }

  Json here() const {
    return {{"millimeter_x", position.x_mm},
            {"millimeter_y", position.y_mm},
            {"millimeter_z", position.z_mm}};
  }

  // Sends a request and commits any progress, like the session worker does.
  std::vector<GameplayPacket> send(std::uint16_t opcode, const Json &request) {
    auto result = book.receive(opcode, request, sequence++, progress, view());
    Protocol protocol(CHRONICLES_PROTOCOL_DIR);
    for (const auto &packet : result.packets) {
      const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
      EXPECT_EQ(decoded.at("status"), "decoded") << packet.opcode;
    }
    if (result.next) {
      progress = std::move(*result.next);
    }
    return result.packets;
  }

  static std::uint16_t first_opcode(const std::vector<GameplayPacket> &packets) {
    return packets.empty() ? 0 : packets.front().opcode;
  }
};

TEST_F(WorldQuests, CompanyConditionsFollowTheClientCheckOrder) {
  const Json request{{"quest_cms_id", 505},
                     {"unit", {{"unit_id", std::to_string(kGuide)}}},
                     {"pos", here()}};

  QuestCompanyStanding standing;
  standing.company_type = 2;
  standing.position_level = 3;
  standing.contribution = 1501;
  company = standing;
  // Level 3 at 1501 is past level 2's ceiling, so the capped-ceiling requirement fails.
  EXPECT_EQ(first_opcode(send(20016, request)), 2);

  standing.company_type = 1;
  standing.position_level = 2;
  standing.contribution = 1500;
  standing.at_capped_level_ceiling = true;
  company = standing;
  // Another company never satisfies a Blue Salt condition.
  EXPECT_EQ(first_opcode(send(20016, request)), 2);

  standing.company_type = 2;
  company = standing;
  EXPECT_EQ(first_opcode(send(20016, request)), 28);
  EXPECT_TRUE(progress.quests.contains(505));
}

TEST_F(WorldQuests, CompanyRewardsAreReportedOnlyForNewlyCompletedQuests) {
  WorldProgress before;
  WorldProgress after;
  QuestProgress accepted;
  after.quests.emplace(505, accepted);
  EXPECT_TRUE(book.company_rewards_earned(before, after).empty());

  after.quests.at(505).complete = true;
  const auto earned = book.company_rewards_earned(before, after);
  EXPECT_EQ(earned.unlocks, (std::vector<QuestCompanyUnlock>{{2, 2}}));
  EXPECT_EQ(earned.contribution, 0);
  EXPECT_TRUE(book.company_rewards_earned(after, after).empty());
  EXPECT_EQ(book.company_unlocks_owed(after), (std::vector<QuestCompanyUnlock>{{2, 2}}));
}

TEST_F(WorldQuests, ServerMonsterDeathAdvancesHuntsOnceAndKeepsSequentialGates) {
  progress.quests[502] = QuestProgress{};
  auto result = book.record_monster_defeat(1234, 5001527, progress, view());
  ASSERT_TRUE(result.next);
  EXPECT_EQ(result.next->quests.at(502).objectives.at(1), 1);
  EXPECT_TRUE(result.next->quests.at(502).credited_kills.contains(1234));
  EXPECT_EQ(result.next->revision, progress.revision + 1);
  auto repeated = book.record_monster_defeat(1234, 5001527, *result.next, view());
  EXPECT_FALSE(repeated.next);
  auto wrong_type = book.record_monster_defeat(5678, 999, progress, view());
  EXPECT_FALSE(wrong_type.next);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : result.packets) {
    EXPECT_EQ(protocol.decode(protocol.encode(packet.opcode, packet.message)).at("status"),
              "decoded");
  }
}

TEST_F(WorldQuests, DelayedMonsterReceiptDoesNotCreditFutureOrRestartedQuestAttempts) {
  progress.quests[502] = QuestProgress{};
  progress.quests[502].started_epoch_ms = 100;
  MonsterRewardReceipt receipt;
  receipt.credited_unit_id = 1234;
  receipt.quest_attempts[502] = {0, 100};

  const auto eligible = book.record_monster_defeat(1234, 5001527, progress, view(), false,
                                                   &receipt);
  ASSERT_TRUE(eligible.next);
  EXPECT_EQ(eligible.next->quests.at(502).objectives.at(1), 1);

  auto restarted = progress;
  restarted.quests[502].attempt = 1;
  restarted.quests[502].started_epoch_ms = 200;
  EXPECT_FALSE(book.record_monster_defeat(1234, 5001527, restarted, view(), false, &receipt).next);

  receipt.quest_attempts.clear();
  EXPECT_FALSE(book.record_monster_defeat(1234, 5001527, progress, view(), false, &receipt).next);
}

TEST_F(WorldQuests, MonsterQuestIndexIncludesSingleGroupAndServerOfferedTargets) {
  auto document = catalog_fixture();
  auto &quest = document["quests"][1];
  quest["progresses"][0]["npc_ids"] = Json::array({5001528, 5001529});
  quest["accept"].push_back({{"kind", "ServerOffered"}, {"npc_ids", Json::array()},
                              {"monster_ids", Json::array({5001530})},
                              {"doodad_ids", Json::array()}, {"zone_maps", Json::array()}});
  const QuestCatalog catalog(std::move(document));
  EXPECT_EQ(catalog.monster_quest_ids(5001527), (std::vector<int>{502}));
  EXPECT_EQ(catalog.monster_quest_ids(5001528), (std::vector<int>{502}));
  EXPECT_EQ(catalog.monster_quest_ids(5001529), (std::vector<int>{502}));
  EXPECT_EQ(catalog.monster_quest_ids(5001530), (std::vector<int>{502}));
  EXPECT_TRUE(catalog.monster_quest_ids(999).empty());
}

TEST_F(WorldQuests, QueuedMonsterOfferCannotCreditAnAttemptAcceptedAfterTheDefeat) {
  auto document = catalog_fixture();
  auto &quest = document["quests"][1];
  quest["accept"].push_back({{"kind", "ServerOffered"}, {"npc_ids", Json::array()},
                              {"monster_ids", Json::array({5001527})},
                              {"doodad_ids", Json::array()}, {"zone_maps", Json::array()}});
  auto catalog = std::make_shared<const QuestCatalog>(std::move(document));
  WorldQuestBook offered_book(catalog, 4097);
  MonsterRewardReceipt receipt;
  receipt.credited_unit_id = 1234;
  receipt.allow_monster_offered_quests = true;

  const auto offered = offered_book.record_monster_defeat(1234, 5001527, progress, view(),
                                                           false, &receipt);
  ASSERT_TRUE(offered.next);
  EXPECT_EQ(offered.next->quests.at(502).objectives.at(1), 1);

  auto accepted_later = progress;
  accepted_later.quests[502] = QuestProgress{};
  accepted_later.quests[502].started_epoch_ms = 200;
  EXPECT_FALSE(offered_book.record_monster_defeat(1234, 5001527, accepted_later, view(),
                                                   false, &receipt).next);
}

TEST_F(WorldQuests, QueuedConditionalMonsterOfferDoesNotUseLaterPlayerState) {
  for (const bool needs_condition : {false, true}) {
    SCOPED_TRACE(needs_condition);
    auto document = catalog_fixture();
    auto &quest = document["quests"][1];
    quest["accept"].push_back({{"kind", "ServerOffered"}, {"npc_ids", Json::array()},
                                {"monster_ids", Json::array({5001527})},
                                {"doodad_ids", Json::array()}, {"zone_maps", Json::array()}});
    if (needs_condition) {
      quest["conditions"] = Json::array({{{"group", 1}, {"operator", "AND"},
                                            {"negated", false}, {"kind", "CheckUnitLevel"},
                                            {"min", 1}, {"max", 40}}});
    } else {
      quest["require_gold"] = 1;
    }
    auto catalog = std::make_shared<const QuestCatalog>(std::move(document));
    WorldQuestBook offered_book(catalog, 4097);
    MonsterRewardReceipt receipt;
    receipt.credited_unit_id = 1234;
    receipt.allow_monster_offered_quests = true;
    EXPECT_THROW(offered_book.record_monster_defeat(1234, 5001527, progress, view(), false,
                                                    &receipt), std::runtime_error);
    EXPECT_FALSE(progress.quests.contains(502));
  }
}

TEST_F(WorldQuests, QueuedMonsterKillUsesSequentialReadinessAtDefeat) {
  auto document = catalog_fixture();
  auto &quest = document["quests"][1];
  quest["sequential"] = true;
  quest["progresses"].push_back({{"sequence", 2}, {"group", 2}, {"kind", "HuntMonster"},
                                  {"parameters", {{"npcId", 5001527}, {"count", 1}}}});
  auto catalog = std::make_shared<const QuestCatalog>(std::move(document));
  WorldQuestBook sequential(catalog, 4097);
  progress.quests[502] = QuestProgress{};
  progress.quests[502].objectives = {{1, 1}, {2, 0}};
  MonsterRewardReceipt receipt;
  receipt.credited_unit_id = 1234;
  receipt.quest_attempts[502] = {0, 0, false, false, {{1, 0}, {2, 0}}};

  // Group 2 became active only after this kill, so replay cannot advance it.
  EXPECT_FALSE(sequential.record_monster_defeat(1234, 5001527, progress, view(), false,
                                                 &receipt).next);
}

TEST_F(WorldQuests, MonsterDeathPublishesRecordsUsedByInteractionConditions) {
  auto document = catalog_fixture();
  document["kill_play_records"] = {{{"id", 524}, {"npc_id", 5001535}},
                                    {{"id", 525}, {"npc_id", 5001535}}};
  auto record_catalog = std::make_shared<const QuestCatalog>(std::move(document));
  WorldQuestBook record_book(record_catalog, 4097);

  const auto result = record_book.record_monster_defeat(1234, 5001535, progress, view());
  ASSERT_TRUE(result.next);
  EXPECT_EQ(result.next->play_records.at(524), 1);
  EXPECT_EQ(result.next->play_records.at(525), 1);
  ASSERT_EQ(result.packets.size(), 2U);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : result.packets) {
    EXPECT_EQ(packet.opcode, 79);
    EXPECT_EQ(protocol.decode(protocol.encode(packet.opcode, packet.message)).at("status"),
              "decoded");
  }
  EXPECT_FALSE(record_book.record_monster_defeat(1235, 5001500, progress, view()).next);
}

// Pioneer's Remedy: a committed Lifeflow Potion craft completes CounterSuccessCrafting, as the
// captured QuestUpdateNoti after OnDemandDynamicCharItemCraftResultNoti did.
TEST_F(WorldQuests, CommittedCraftsAndDoodadInteractionsAdvanceCounterObjectives) {
  progress.quests[511] = QuestProgress{};
  progress.quests.at(511).objectives = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};

  const QuestEvents crafts{{QuestEventKind::CraftSucceeded, kPotionCraft, 0, 2}};
  auto crafted = book.record_events(crafts, progress, view());
  ASSERT_TRUE(crafted.next);
  EXPECT_EQ(crafted.next->revision, progress.revision + 1);
  EXPECT_EQ(crafted.next->quests.at(511).objectives.at(1), 1);
  EXPECT_EQ(crafted.next->quests.at(511).objectives.at(2), 2);
  ASSERT_EQ(crafted.packets.size(), 2U);
  EXPECT_EQ(crafted.packets.front().opcode, 27);
  EXPECT_EQ(crafted.packets.front().message.at("quest").at("progresses"),
            Json::array({{{"sequence_id", 1}, {"value", 1}}}));
  progress = *crafted.next;

  // Another recipe, a doodad in the wrong phase and an unrelated doodad change nothing.
  const QuestEvents unrelated{{QuestEventKind::CraftSucceeded, kPotionCraft + 1, 0, 1},
                              {QuestEventKind::DoodadInteracted, 74, 2, 1},
                              {QuestEventKind::DoodadInteracted, 90, 1, 1}};
  EXPECT_FALSE(book.record_events(unrelated, progress, view()).next);

  const QuestEvents harvested{{QuestEventKind::DoodadInteracted, 74, 1, 1}};
  auto doodad = book.record_events(harvested, progress, view());
  ASSERT_TRUE(doodad.next);
  EXPECT_EQ(doodad.next->quests.at(511).objectives.at(3), 1);
  progress = *doodad.next;

  // Ethereal hunts add the rolled virtual amount and cap at the required count.
  auto first_kill = book.record_monster_defeat(41, 5001200, progress, view());
  ASSERT_TRUE(first_kill.next);
  EXPECT_EQ(first_kill.next->quests.at(511).objectives.at(4), 2);
  auto second_kill = book.record_monster_defeat(42, 5001200, *first_kill.next, view());
  ASSERT_TRUE(second_kill.next);
  EXPECT_EQ(second_kill.next->quests.at(511).objectives.at(4), 3);

  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : crafted.packets) {
    EXPECT_EQ(protocol.decode(protocol.encode(packet.opcode, packet.message)).at("status"),
              "decoded");
  }
}

// Words of Warning: the captured report interact_memo{10000127, board spot} advanced the
// objective; reports naming another spot, memo or a far position are refused.
TEST_F(WorldQuests, MemoReportsNeedThePlacedBoardNearThePlayer) {
  progress.quests[512] = QuestProgress{};
  progress.quests.at(512).objectives = {{1, 0}, {2, 0}, {3, 0}};
  const auto report = [this](int memo, std::uint64_t spot) {
    return Json{{"quest_cms_id", 512},
                {"quest_progress_sequence_id", 1},
                {"pos", here()},
                {"interact_memo",
                 {{"memo_cms_id", memo}, {"spot_object_id", std::to_string(spot)}}}};
  };

  EXPECT_EQ(first_opcode(send(20019, report(kMemo, kMemoSpot + 4096))), 2);
  EXPECT_EQ(first_opcode(send(20019, report(kMemo + 1, kMemoSpot))), 2);
  position = {kMap, 0, 20000, 0, 0};
  EXPECT_EQ(first_opcode(send(20019, report(kMemo, kMemoSpot))), 2);
  EXPECT_EQ(progress.quests.at(512).objectives.at(1), 0);

  position = {kMap, 0, 0, 0, 0};
  const auto read = send(20019, report(kMemo, kMemoSpot));
  ASSERT_EQ(first_opcode(read), 27);
  EXPECT_EQ(progress.quests.at(512).objectives.at(1), 1);
}

// Company dialogue functions (joins, pioneer permits) read the speaker of the open dialogue.
TEST_F(WorldQuests, DialogueStartRecordsTheSpeakerUntilTheDialogueEnds) {
  EXPECT_FALSE(book.dialogue_npc_template_id());

  send(20065, {{"target_unit_id", std::to_string(kGuide)}, {"pos", here()}});
  EXPECT_EQ(book.dialogue_npc_template_id(), kGuideTemplate);

  send(20066, Json::object());
  EXPECT_FALSE(book.dialogue_npc_template_id());
}

// Captured: after DialogueStartReq the client reports interact_npc{npc_unit_id}; the server
// answered QuestUpdateNoti even though talking had already credited the objective.
TEST_F(WorldQuests, InteractNpcReportsConfirmTheDialogueCredit) {
  progress.quests[512] = QuestProgress{};
  progress.quests.at(512).objectives = {{1, 1}, {2, 0}, {3, 0}};
  const Json report{{"quest_cms_id", 512},
                    {"quest_progress_sequence_id", 3},
                    {"pos", here()},
                    {"interact_npc", {{"npc_unit_id", std::to_string(kGuide)}}}};

  const auto dialogue = send(20065, {{"target_unit_id", std::to_string(kGuide)}, {"pos", here()}});
  EXPECT_EQ(dialogue.back().opcode, 27);
  EXPECT_EQ(progress.quests.at(512).objectives.at(3), 1);
  EXPECT_EQ(first_opcode(send(20019, report)), 27);
  EXPECT_EQ(progress.quests.at(512).objectives.at(3), 1);

  auto stranger = report;
  stranger["interact_npc"]["npc_unit_id"] = "77";
  EXPECT_EQ(first_opcode(send(20019, stranger)), 2);
}

// Gathering Bone Fragments: the captured turn-in removed exactly the five required bone chips
// (6 -> 1) in an ItemsUpdateNoti carrying quest_reward{quest_cms_id}.
TEST_F(WorldQuests, TurnInConsumesTheCollectedStacks) {
  progress.quests[513] = QuestProgress{};
  progress.quests.at(513).objectives = {{1, 0}};
  progress.stackables[kBoneChip] = 6;
  send(20065, {{"target_unit_id", std::to_string(kGuide)}, {"pos", here()}});

  const auto rewarded = send(20018, {{"quest_cms_id", 513}});
  ASSERT_FALSE(rewarded.empty());
  EXPECT_EQ(rewarded.front().opcode, 169);
  EXPECT_EQ(rewarded.front().message.at("quest_reward"), (Json{{"quest_cms_id", 513}}));
  EXPECT_EQ(rewarded.front().message.at("stackables"),
            Json::array({{{"item_cms_id", kBoneChip}, {"quantity", 1}}}));
  EXPECT_TRUE(progress.quests.at(513).complete);
  EXPECT_EQ(progress.stackables.at(kBoneChip), 1);
  EXPECT_EQ(progress.gold, 20);
}

TEST_F(WorldQuests, NpcQuestRunsTriggerDialogueItemsTurnInAndSuccessor) {
  progress.level = 3;
  const Json accept{{"quest_cms_id", 501},
                    {"pos", here()},
                    {"unit", {{"unit_id", std::to_string(kGuide)}}}};
  const auto accepted = send(20016, accept);
  ASSERT_EQ(first_opcode(accepted), 169);
  EXPECT_EQ(accepted.at(1).opcode, 28);
  EXPECT_EQ(accepted.at(1).message.at("npc_pos").at("millimeter_x"), 1000);
  EXPECT_EQ(progress.stackables.at(120059), 1);

  // Repeating the request does not grant the supply again.
  EXPECT_EQ(first_opcode(send(20016, accept)), 28);
  EXPECT_EQ(progress.stackables.at(120059), 1);

  // Sequential: the dialogue objective cannot advance before the trigger group.
  send(20065, {{"target_unit_id", std::to_string(kGuide)}, {"pos", here()}});
  EXPECT_EQ(progress.quests.at(501).objectives.at(2), 0);
  send(20066, Json::object());

  // The trigger requires the tracked position inside the volume.
  const Json trigger_report{{"quest_cms_id", 501},
                            {"quest_progress_sequence_id", 1},
                            {"pos", {{"millimeter_x", 50000}, {"millimeter_y", 0},
                                     {"millimeter_z", 0}}},
                            {"trigger_volume_enter",
                             {{"trigger_volume_x", "11"}, {"trigger_volume_y", "22"}}}};
  EXPECT_EQ(first_opcode(send(20019, trigger_report)), 2);
  position = {kMap, 50000, 0, 0, 0};
  EXPECT_EQ(first_opcode(send(20019, trigger_report)), 27);
  EXPECT_EQ(progress.quests.at(501).objectives.at(1), 1);

  // Talking to the guide credits the NPC objective.
  position = {kMap, 0, 0, 0, 0};
  const auto dialogue = send(20065, {{"target_unit_id", std::to_string(kGuide)}, {"pos", here()}});
  ASSERT_EQ(dialogue.size(), 3u);
  EXPECT_EQ(dialogue.at(2).opcode, 27);
  EXPECT_EQ(progress.quests.at(501).objectives.at(2), 1);

  // Turn-in is refused until the player owns both items.
  EXPECT_EQ(first_opcode(send(20018, {{"quest_cms_id", 501}})), 2);
  progress.stackables[100109] = 2;
  const auto rewarded = send(20018, {{"quest_cms_id", 501}});
  ASSERT_FALSE(rewarded.empty());
  EXPECT_TRUE(progress.quests.at(501).complete);
  EXPECT_EQ(progress.gold, 50);
  EXPECT_EQ(progress.experience, 300);
  ASSERT_TRUE(progress.quests.contains(502));
  EXPECT_FALSE(progress.quests.at(502).complete);

  // A completed quest answers with its state and never pays twice.
  const auto saved = progress;
  EXPECT_EQ(first_opcode(send(20018, {{"quest_cms_id", 501}})), 27);
  EXPECT_EQ(progress, saved);
  EXPECT_EQ(book.snapshot(progress).message.at("quests").size(), 2u);
}

// GM completion skips NPC, range and item checks, grants rewards/EXP like a turn-in, accepts the
// sequential successor, and marks a quest with unverifiable rewards complete without them.
TEST_F(WorldQuests, GameMasterCompletionFinishesQuestsAndFollowsChains) {
  progress.level = 3;
  progress.quests.emplace(501, QuestProgress{});
  progress.quests.at(501).objectives = {{1, 0}, {2, 0}, {3, 0}};

  EXPECT_EQ(WorldQuestBook::active_quest_ids(progress), std::vector<int>{501});
  auto first = book.gm_complete(WorldQuestBook::active_quest_ids(progress), progress, view());
  EXPECT_EQ(first.completed, 1);
  ASSERT_TRUE(first.result.next);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : first.result.packets) {
    EXPECT_EQ(protocol.decode(protocol.encode(packet.opcode, packet.message)).at("status"),
              "decoded")
        << packet.opcode;
  }
  progress = std::move(*first.result.next);
  EXPECT_TRUE(progress.quests.at(501).complete);
  EXPECT_EQ(progress.quests.at(501).objectives.at(3), 2);
  EXPECT_EQ(progress.gold, 50);
  // The sequential successor was accepted and is now the only quest in progress.
  EXPECT_EQ(WorldQuestBook::active_quest_ids(progress), std::vector<int>{502});

  auto second = book.gm_complete(WorldQuestBook::active_quest_ids(progress), progress, view());
  EXPECT_EQ(second.completed, 1);
  progress = std::move(*second.result.next);
  EXPECT_TRUE(WorldQuestBook::active_quest_ids(progress).empty());

  // An unsaved quest is accepted and completed; unverifiable rewards are not granted.
  auto unsupported = book.gm_complete({503}, progress, view());
  EXPECT_EQ(unsupported.completed, 1);
  ASSERT_TRUE(unsupported.result.next);
  EXPECT_TRUE(unsupported.result.next->quests.at(503).complete);

  // Complete or unknown quests are skipped without a change.
  const auto repeated = book.gm_complete({501, 999999}, progress, view());
  EXPECT_EQ(repeated.completed, 0);
  EXPECT_EQ(repeated.skipped, 2);
  EXPECT_FALSE(repeated.result.next);
}

TEST_F(WorldQuests, UnmodeledRewardKindsDoNotBlockTheSupportedTurnInRewards) {
  progress.level = 3;
  send(20016, {{"quest_cms_id", 504},
               {"pos", here()},
               {"unit", {{"unit_id", std::to_string(kGuide)}}}});
  ASSERT_TRUE(progress.quests.contains(504));

  const auto rewarded =
      send(20018, {{"quest_cms_id", 504}, {"unit_id", std::to_string(kGuide)}});
  ASSERT_FALSE(rewarded.empty());
  EXPECT_TRUE(progress.quests.at(504).complete);
  EXPECT_EQ(progress.gold, 75);
  EXPECT_EQ(progress.experience, 40);
}

TEST_F(WorldQuests, GmAcceptanceGrantsSuppliesOnceAndCanRetainConditions) {
  progress.level = 20;
  const auto refused = book.gm_accept(501, true, progress, view());
  EXPECT_FALSE(refused.next);
  const auto accepted = book.gm_accept(501, false, progress, view());
  ASSERT_TRUE(accepted.next);
  EXPECT_TRUE(accepted.next->quests.contains(501));
  EXPECT_EQ(accepted.next->stackables.at(120059), 1);
  EXPECT_FALSE(accepted.next->quests.at(501).complete);
  EXPECT_FALSE(book.gm_accept(501, false, *accepted.next, view()).next);
  EXPECT_FALSE(book.gm_accept(999999, false, progress, view()).next);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : accepted.packets) {
    EXPECT_NO_THROW(protocol.encode(packet.opcode, packet.message));
  }
}

TEST_F(WorldQuests, AcceptanceFailsClosedForConditionsRangeAndUnmodeledRules) {
  const Json accept{{"quest_cms_id", 501},
                    {"pos", here()},
                    {"unit", {{"unit_id", std::to_string(kGuide)}}}};
  progress.level = 20;
  EXPECT_EQ(first_opcode(send(20016, accept)), 2);

  progress.level = 3;
  guide.position = {20000, 0, 0};
  EXPECT_EQ(first_opcode(send(20016, accept)), 2);
  EXPECT_TRUE(progress.quests.empty());

  EXPECT_EQ(first_opcode(send(20016, {{"quest_cms_id", 503}, {"pos", here()}})), 2);
  EXPECT_EQ(first_opcode(send(20016, {{"quest_cms_id", 502}, {"pos", here()}})), 2);
  EXPECT_EQ(first_opcode(send(20016, {{"quest_cms_id", 999}, {"pos", here()}})), 2);
  EXPECT_TRUE(progress.quests.empty());
}

TEST_F(WorldQuests, TriggersBelongToTheirWorldAsset) {
  progress.level = 3;
  send(20016, {{"quest_cms_id", 501},
               {"pos", here()},
               {"unit", {{"unit_id", std::to_string(kGuide)}}}});
  ASSERT_TRUE(progress.quests.contains(501));

  // Same coordinates on the tutorial world asset are a different place.
  position = {8000, 50000, 0, 0, 0};
  const Json report{{"quest_cms_id", 501},
                    {"quest_progress_sequence_id", 1},
                    {"pos", here()},
                    {"trigger_volume_enter",
                     {{"trigger_volume_x", "11"}, {"trigger_volume_y", "22"}}}};
  EXPECT_EQ(first_opcode(send(20019, report)), 2);
  EXPECT_EQ(progress.quests.at(501).objectives.at(1), 0);
}

TEST(QuestCatalog, RejectsBrokenChainsAndLoadsTheGeneratedCatalog) {
  auto document = catalog_fixture();
  document["quests"][0]["next_quest"] = 4242;
  EXPECT_THROW(QuestCatalog invalid(document), std::runtime_error);

  const auto *path = std::getenv("CHRONICLES_QUEST_CATALOG");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_QUEST_CATALOG to the generated quest catalog";
  }
  const auto catalog = QuestCatalog::load(path);
  EXPECT_GT(catalog->size(), 1500u);
  EXPECT_GT(catalog->fully_supported_count(), 800u);
  EXPECT_NE(catalog->quest(10000045), nullptr);
  EXPECT_EQ(catalog->world_of(9012), "ArcheWorld");
}

// Every quest of the generated catalog is modeled: the book can start it and finish it with all
// of its rewards (loot packs, mates, build points, company and guild rewards) without refusal.
TEST(QuestCatalog, EveryGeneratedQuestIsSupportedAndCompletesWithItsRewards) {
  const auto *path = std::getenv("CHRONICLES_QUEST_CATALOG");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_QUEST_CATALOG to the generated quest catalog";
  }
  const auto catalog = QuestCatalog::load(path);
  EXPECT_EQ(catalog->fully_supported_count(), catalog->size());

  WorldQuestBook book(catalog, 4097);
  Character character;
  QuestWorldView view;
  view.position = {9012, 0, 0, 0, 0};
  view.character = &character;
  view.now_epoch_ms = 1793361600000;

  int completed = 0;
  std::vector<int> refused;
  for (const int quest_id : catalog->quest_ids()) {
    // Each quest completes on its own fresh character so no chain or stack limit interferes.
    WorldProgress progress;
    progress.level = 40;
    const auto result = book.gm_complete({quest_id}, progress, view);
    completed += result.completed;
    if (result.completed != 1) {
      refused.push_back(quest_id);
    }
  }
  EXPECT_EQ(completed, static_cast<int>(catalog->size()));
  EXPECT_TRUE(refused.empty()) << "first refused quest " << refused.front();
}

TEST(QuestCatalog, CliffsQuestFollowupsStartOnCompletionAndRepairPreviouslyStrandedProgress) {
  const auto *path = std::getenv("CHRONICLES_QUEST_CATALOG");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_QUEST_CATALOG to the generated quest catalog";
  }
  const auto catalog = QuestCatalog::load(path);
  WorldQuestBook book(catalog, 4097);
  Character character;
  QuestWorldView view;
  view.position = {30104, 0, 0, 0, 0};
  view.character = &character;

  WorldProgress progress;
  QuestProgress officer;
  officer.objectives = {{1, 1}, {2, 1}};
  progress.quests.emplace(20001052, officer);
  const auto officer_reward = book.receive(20018, {{"quest_cms_id", 20001052}}, 1, progress, view);
  ASSERT_TRUE(officer_reward.next.has_value());
  progress = *officer_reward.next;
  EXPECT_TRUE(progress.quests.at(20001052).complete);
  ASSERT_TRUE(progress.quests.contains(20001053));
  EXPECT_FALSE(progress.quests.at(20001053).complete);

  // The key object's committed state advances the first rescue objective.
  progress.adventure_objects[20014613] = 2;
  const auto refreshed = book.periodic_update(progress, view);
  ASSERT_TRUE(refreshed.next.has_value());
  progress = *refreshed.next;
  EXPECT_EQ(progress.quests.at(20001053).objectives.at(1), 1);

  const auto opened = book.record_events(
      {{QuestEventKind::DoodadInteracted, 49023, 1, 1}}, progress, view);
  ASSERT_TRUE(opened.next.has_value());
  progress = *opened.next;
  const auto rescue_reward = book.receive(20018, {{"quest_cms_id", 20001053}}, 2, progress, view);
  ASSERT_TRUE(rescue_reward.next.has_value());
  progress = *rescue_reward.next;
  EXPECT_TRUE(progress.quests.at(20001053).complete);
  ASSERT_TRUE(progress.quests.contains(20001054));

  auto &cliffs_quest = progress.quests.at(20001054);
  cliffs_quest.objectives[1] = 1;
  cliffs_quest.objectives[2] = 1;
  cliffs_quest.objectives[6] = 1;
  cliffs_quest.objectives[8] = 1;
  const auto cliffs_reward = book.receive(20018, {{"quest_cms_id", 20001054}}, 3, progress, view);
  ASSERT_TRUE(cliffs_reward.next.has_value());
  EXPECT_TRUE(cliffs_reward.next->quests.at(20001054).complete);
  EXPECT_TRUE(cliffs_reward.next->quests.contains(10000146));

  WorldProgress stranded;
  QuestProgress previously_completed;
  previously_completed.complete = true;
  stranded.quests.emplace(20001052, previously_completed);
  const auto repaired = book.periodic_update(stranded, view);
  ASSERT_TRUE(repaired.next.has_value());
  EXPECT_TRUE(repaired.next->quests.contains(20001053));
  EXPECT_FALSE(book.periodic_update(*repaired.next, view).next.has_value());

  WorldProgress missing_captain;
  missing_captain.quests.emplace(20001054, previously_completed);
  const auto repaired_captain = book.periodic_update(missing_captain, view);
  ASSERT_TRUE(repaired_captain.next.has_value());
  EXPECT_TRUE(repaired_captain.next->quests.contains(10000146));

  WorldProgress missing_assembly_hall;
  missing_assembly_hall.quests.emplace(10000146, previously_completed);
  const auto repaired_assembly_hall = book.periodic_update(missing_assembly_hall, view);
  ASSERT_TRUE(repaired_assembly_hall.next.has_value());
  EXPECT_TRUE(repaired_assembly_hall.next->quests.contains(10000133));

  WorldProgress missing_assembly_followup;
  missing_assembly_followup.quests.emplace(10000133, previously_completed);
  const auto repaired_assembly_followup = book.periodic_update(missing_assembly_followup, view);
  ASSERT_TRUE(repaired_assembly_followup.next.has_value());
  EXPECT_TRUE(repaired_assembly_followup.next->quests.contains(10000180));

  WorldProgress missing_housing_route;
  missing_housing_route.quests.emplace(10000149, previously_completed);
  const auto without_prerequisite = book.periodic_update(missing_housing_route, view);
  if (without_prerequisite.next) {
    EXPECT_FALSE(without_prerequisite.next->quests.contains(80101030));
  }
  missing_housing_route.quests.emplace(10000056, previously_completed);
  const auto repaired_housing_route = book.periodic_update(missing_housing_route, view);
  ASSERT_TRUE(repaired_housing_route.next.has_value());
  EXPECT_TRUE(repaired_housing_route.next->quests.contains(80101030));

  WorldProgress missing_housing_followup;
  missing_housing_followup.quests.emplace(80101030, previously_completed);
  const auto repaired_housing_followup = book.periodic_update(missing_housing_followup, view);
  ASSERT_TRUE(repaired_housing_followup.next.has_value());
  EXPECT_TRUE(repaired_housing_followup.next->quests.contains(10000118));

  WorldProgress intentionally_abandoned;
  intentionally_abandoned.quests.emplace(20001052, previously_completed);
  QuestProgress abandoned_followup;
  abandoned_followup.abandoned = true;
  intentionally_abandoned.quests.emplace(20001053, abandoned_followup);
  const auto abandoned_update = book.periodic_update(intentionally_abandoned, view);
  if (abandoned_update.next) {
    EXPECT_TRUE(abandoned_update.next->quests.at(20001053).abandoned);
  }
}

TEST(QuestCatalog, CliffsEliteObjectRequirementsUseCurrentGroupOrCompletedQuest) {
  const auto *path = std::getenv("CHRONICLES_QUEST_CATALOG");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_QUEST_CATALOG to the generated quest catalog";
  }
  const auto catalog = QuestCatalog::load(path);
  WorldQuestBook book(catalog, 4097);
  Character character;
  QuestWorldView view;
  view.position = {30104, 0, 0, 0, 0};
  view.character = &character;
  const Json conditions = Json::array({
      {{"group", 1}, {"operator", "AND"}, {"kind", "Quest"},
       {"quest_id", 90100021}, {"requirement", "Acceptable"}},
      {{"group", 1}, {"operator", "AND"}, {"kind", "Quest"},
       {"quest_id", 20001054}, {"requirement", "Progressing"}, {"progress_group", 4}},
      {{"group", 2}, {"operator", "AND"}, {"kind", "Quest"},
       {"quest_id", 90100021}, {"requirement", "Acceptable"}},
      {{"group", 2}, {"operator", "AND"}, {"kind", "Quest"},
       {"quest_id", 20001054}, {"requirement", "QuestComplete"}},
  });
  WorldProgress progress;
  EXPECT_FALSE(book.requirements_met(conditions, progress, view));
  QuestProgress cliffs;
  cliffs.objectives = {{1, 1}, {2, 1}};
  progress.quests.emplace(20001054, cliffs);
  EXPECT_FALSE(book.requirements_met(conditions, progress, view));
  progress.quests.at(20001054).objectives[6] = 1;
  EXPECT_TRUE(book.requirements_met(conditions, progress, view));
  const auto side_quest = book.record_events(
      {{QuestEventKind::DoodadInteracted, 777, 1, 1}}, progress, view);
  ASSERT_TRUE(side_quest.next.has_value());
  ASSERT_TRUE(side_quest.next->quests.contains(90100021));
  EXPECT_TRUE(side_quest.next->quests.at(90100021).complete);
  EXPECT_EQ(side_quest.next->stackables.at(120321), 1);
  progress.quests.at(20001054).objectives[8] = 1;
  EXPECT_FALSE(book.requirements_met(conditions, progress, view));
  progress.quests.at(20001054).complete = true;
  EXPECT_TRUE(book.requirements_met(conditions, progress, view));
  QuestProgress hidden;
  hidden.complete = true;
  progress.quests.emplace(90100021, hidden);
  EXPECT_FALSE(book.requirements_met(conditions, progress, view));
}

// A live graduated character (full test-kit wallet) finished every step of Isolated Coast quest
// 10000046 (an automatic turn-in), but each client QuestRewardReq was answered with BadRequest
// because its 200 gold reward would pass the wallet limit.
TEST(QuestCatalog, FinishedAutomaticQuestIsRewardedThroughTheClientClaimWithAFullWallet) {
  const auto *path = std::getenv("CHRONICLES_QUEST_CATALOG");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_QUEST_CATALOG to the generated quest catalog";
  }
  const auto catalog = QuestCatalog::load(path);
  constexpr int kQuestId = 10000046;
  if (!catalog->quest(kQuestId)) {
    GTEST_SKIP() << "Catalog has no quest " << kQuestId;
  }

  WorldQuestBook book(catalog, 4097);
  Character character;
  QuestWorldView view;
  view.position = {9012, 0, 0, 0, 0};
  view.character = &character;
  view.now_epoch_ms = 1793361600000;

  WorldProgress progress;
  progress.level = 40;
  progress.gold = kMoneyLimit - 91;
  QuestProgress quest;
  for (const auto &objective : catalog->quest(kQuestId)->at("progresses")) {
    quest.objectives.emplace(objective.at("sequence").get<int>(), 1);
  }
  progress.quests.emplace(kQuestId, quest);

  const auto result = book.receive(20018, {{"quest_cms_id", kQuestId}}, 1, progress, view);
  ASSERT_FALSE(result.packets.empty());
  EXPECT_NE(result.packets.front().opcode, 2) << result.packets.front().message.dump();
  ASSERT_TRUE(result.next.has_value());
  EXPECT_TRUE(result.next->quests.at(kQuestId).complete);
  EXPECT_EQ(result.next->gold, kMoneyLimit);
}

} // namespace
} // namespace chronicles
