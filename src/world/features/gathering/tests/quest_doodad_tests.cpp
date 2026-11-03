// Quest doodads in the shared doodad engine: animated quest interactions, instant
// (animation-less) PC2S_InteractUnitReq interactions, shared phase state and quest events.
// Synthetic profile; no original-client capture of these doodads exists.

#include "protocol/protocol.hpp"
#include "world/features/gathering/gathering.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t kStartTicks = 100000000;
constexpr std::int64_t kTicksPerMs = 10000;
constexpr std::uint64_t kPlayerUnit = 4242;
constexpr int kSharedMap = 9012;
constexpr int kDungeonMap = 10203;
constexpr std::uint32_t kDoodadSector = 4735150;
constexpr std::uint32_t kAltarLocalId = 3;
constexpr std::uint32_t kLeverLocalId = 4;
constexpr int kAltarDoodad = 763;
constexpr int kLeverDoodad = 456;
constexpr int kAltarInteraction = 10204;
constexpr int kLeverInteraction = 35;
constexpr int kAltarAnimation = 201;
constexpr int kAltarExecuteMs = 1200;
constexpr int kAltarResetMs = 20000;
constexpr int kLeverResetMs = 120000;
constexpr std::uint32_t kPrisonDoorLocalId = 5;
constexpr int kPrisonDoor = 49023;
constexpr int kPrisonInteraction = 1242;
constexpr int kPrisonKey = 120109;
constexpr int kRescueQuest = 20001053;
constexpr int kCliffsMap = 30104;
constexpr int kCannonDoodad = 10152;
constexpr int kCannonInteraction = 312;
constexpr int kCannonQuest = 20001054;
constexpr std::uint32_t kCannonSector = 171 | (290 << 14);
constexpr std::uint32_t kBarrierSector = 171 | (289 << 14);
constexpr std::uint32_t kEliteSector = 170 | (289 << 14);
constexpr std::uint32_t kRiftSector = 170 | (290 << 14);
constexpr int kCaptainQuest = 10000146;
constexpr int kRiftDoodad = 10130601;
constexpr int kEliteObject = 777;
constexpr int kEliteObjectInteraction = 10000146;

Json quest_doodad_profile() {
  Json profile = {
      {"format_version", 2},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"maps", Json::array({{{"map_id", kSharedMap},
                             {"area_min_mm", {-100000, -100000, -100000}},
                             {"area_max_mm", {100000, 100000, 100000}}},
                            {{"map_id", kDungeonMap},
                             {"world", 1},
                             {"area_min_mm", {-100000, -100000, -100000}},
                             {"area_max_mm", {100000, 100000, 100000}}}})},
      {"tool_durability_loss", 100},
      {"sp_regen_delay_ms", 300},
      {"animations", Json::array({{{"interaction_anim_id", kAltarAnimation},
                                   {"sp_cost", 0},
                                   {"tool_equipment_type", "None"},
                                   {"execute_time_ms", kAltarExecuteMs},
                                   {"duration_ms", 2000}}})},
      // A profile always carries gathering placements; one herb keeps the loader satisfied.
      {"gatherings", Json::array({{{"gathering_id", 80},
                                   {"interaction_anim_id", kAltarAnimation},
                                   {"deactivate_delay_ms", 1000},
                                   {"activate_delay_min_ms", 30000},
                                   {"activate_delay_max_ms", 180000},
                                   {"pack_rate_percent", 100.0},
                                   {"groups", Json::array({{{"rate_percent", 100.0},
                                                            {"items", Json::array({{
                                                                {"item_cms_id", 70102001},
                                                                {"min_quantity", 1},
                                                                {"max_quantity", 1},
                                                                {"weight", 1}}})}}})}}})},
      {"unsupported_gatherings", Json::array()},
      {"tool_items", Json::object()},
      {"points", Json::array({{4784280, 23, 80, 0, 0, 0}})},
      // An altar used through an animation that resets after 20 s, and a lever used at once
      // whose triggerless phase returns to the initial phase like a respawn.
      {"doodads",
       Json::array(
           {{{"doodad_id", kAltarDoodad},
             {"initial_phase", 1},
             {"quest", true},
             {"phases", Json::array({{{"phase_id", 1},
                                      {"trigger", "interaction"},
                                      {"interaction_id", kAltarInteraction},
                                      {"interaction_anim_id", kAltarAnimation},
                                      {"next", Json::array({Json::array({2, 1})})}},
                                     {{"phase_id", 2},
                                      {"trigger", "timer"},
                                      {"duration_ms", kAltarResetMs},
                                      {"next", Json::array({Json::array({1, 1})})}}})}},
            {{"doodad_id", kLeverDoodad},
             {"initial_phase", 1},
             {"quest", true},
             {"phases", Json::array({{{"phase_id", 1},
                                      {"trigger", "interaction"},
                                      {"interaction_id", kLeverInteraction},
                                      {"instant", true},
                                      {"map_move",
                                       {{"map_id", 20310},
                                        {"x_mm", -1804040},
                                        {"y_mm", 504080},
                                        {"z_mm", -494230}}},
                                      {"next", Json::array({Json::array({2, 1})})}},
                                     {{"phase_id", 2},
                                      {"trigger", "timer"},
                                      {"duration_ms", kLeverResetMs},
                                      {"next", Json::array({Json::array({1, 1})})}}})}}})},
      {"doodad_points",
       Json::array({{kDoodadSector, kAltarLocalId, kAltarDoodad, 0, 500, 0},
                    {kDoodadSector, kLeverLocalId, kLeverDoodad, 500, 0, 0},
                    // The same spot unit id in a dungeon world (index 1) names another doodad.
                    {kDoodadSector, kAltarLocalId, kLeverDoodad, 0, 0, 0, 1}})},
      {"worlds", Json::array({"ArcheWorld", "MazeDungeons/DungeonZ02D03"})}};

  profile["doodads"].push_back({
      {"doodad_id", kPrisonDoor},
      {"initial_phase", 1},
      {"quest", true},
      {"phases", Json::array({{{"phase_id", 1},
                               {"trigger", "interaction"},
                               {"interaction_id", kPrisonInteraction},
                               {"interaction_anim_id", kAltarAnimation},
                               {"next", Json::array({Json::array({2, 1})})}},
                              {{"phase_id", 2},
                               {"trigger", "timer"},
                               {"duration_ms", kAltarResetMs},
                               {"next", Json::array({Json::array({1, 1})})}}})}});
  profile["doodad_points"].push_back({kDoodadSector, kPrisonDoorLocalId, kPrisonDoor, 0, 0, 0});
  profile["maps"].push_back({{"map_id", kCliffsMap},
                              {"area_min_mm", {-100000, -100000, -100000}},
                              {"area_max_mm", {100000, 100000, 100000}}});
  profile["doodads"].push_back({
      {"doodad_id", kCannonDoodad},
      {"initial_phase", 1},
      {"quest", true},
      {"phases", Json::array({{{"phase_id", 1},
                               {"trigger", "interaction"},
                               {"interaction_id", kCannonInteraction},
                               {"instant", true},
                               {"next", Json::array({Json::array({2, 1})})}},
                              {{"phase_id", 2},
                               {"trigger", "timer"},
                               {"duration_ms", 500},
                               {"next", Json::array({Json::array({1, 1})})}}})}});
  profile["doodad_points"].push_back({kCannonSector, 1, kCannonDoodad, 0, 0, 0});
  profile["doodads"].push_back({
      {"doodad_id", kRiftDoodad},
      {"initial_phase", 1},
      {"quest", true},
      {"phases", Json::array({{{"phase_id", 1},
                               {"trigger", "interaction"},
                               {"interaction_id", kRiftDoodad},
                               {"interaction_anim_id", kAltarAnimation},
                               {"map_move", {{"map_id", kCliffsMap},
                                             {"x_mm", 100},
                                             {"y_mm", 200},
                                             {"z_mm", 300}}},
                               {"next", Json::array({Json::array({2, 1})})}},
                              {{"phase_id", 2},
                               {"trigger", "timer"},
                               {"duration_ms", 3000},
                               {"next", Json::array({Json::array({1, 1})})}}})}});
  profile["doodad_points"].push_back({kRiftSector, 2, kRiftDoodad, 0, 0, 0});
  profile["doodads"].push_back({
      {"doodad_id", kEliteObject},
      {"initial_phase", 1},
      {"quest", true},
      {"phases", Json::array({{{"phase_id", 1},
                               {"trigger", "interaction"},
                               {"interaction_id", kEliteObjectInteraction},
                               {"instant", true},
                               {"next", Json::array({Json::array({2, 1})})}},
                              {{"phase_id", 2},
                               {"trigger", "interaction"},
                               {"interaction_id", kEliteObjectInteraction},
                               {"instant", true},
                               {"next", Json::array({Json::array({1, 1})})}}})}});
  profile["doodad_points"].push_back({kEliteSector, 8, kEliteObject, 0, 0, 0});
  return profile;
}

Json interaction_request(std::uint32_t local_id, int interaction_id) {
  return {{"target_unit_id", std::to_string(doodad_unit_id(kDoodadSector, local_id))},
          {"param", {{"interaction", {{"interaction_id", interaction_id}}}}},
          {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
          {"yaw", 10.0}};
}

Json instant_interaction_request(std::uint32_t local_id, int interaction_id) {
  return {{"target_unit_id", std::to_string(doodad_unit_id(kDoodadSector, local_id))},
          {"param", {{"interaction_id", interaction_id}}}};
}

class QuestDoodadTest : public testing::Test {
protected:
  std::shared_ptr<const GatheringData> data =
      std::make_shared<const GatheringData>(quest_doodad_profile());
  std::shared_ptr<GatheringNodes> nodes = std::make_shared<GatheringNodes>(data, 3);
  WorldProgress durable;
  Character character;
  bool commit_succeeds{true};
  bool conditions_allowed{false};
  Json observed_conditions;
  ItemTrade committed_trade;
  WorldPosition position{kSharedMap, 0, 0, 0, 0};

  GatheringSession session() {
    GatheringPersistence persistence;
    persistence.load_character = [this]() -> std::optional<Character> { return character; };
    persistence.load_progress = [this] { return durable; };
    persistence.conditions_met = [this](const Json &conditions, const WorldPosition &) {
      observed_conditions = conditions;
      return conditions_allowed;
    };
    persistence.commit = [this](const WorldProgress &, const WorldProgress &after,
                                const ItemTrade &trade) {
      if (!commit_succeeds) {
        return false;
      }
      durable = after;
      committed_trade = trade;
      std::erase_if(character.items, [&](const CharacterItem &item) {
        return std::ranges::find(trade.removed_item_ids, item.id) != trade.removed_item_ids.end();
      });
      return true;
    };
    return GatheringSession(data, nodes, kPlayerUnit, std::move(persistence), 7);
  }

  static void expect_wire(const std::vector<GameplayPacket> &packets) {
    Protocol protocol(CHRONICLES_PROTOCOL_DIR);
    for (const auto &packet : packets) {
      const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
      EXPECT_EQ(decoded.at("status"), "decoded") << "opcode " << packet.opcode;
    }
  }

  GatheringNodeKey node(std::uint32_t local_id) const {
    return {kSharedMap, GatheringNodeKind::Doodad, kDoodadSector, local_id};
  }
};

TEST_F(QuestDoodadTest, AnimatedQuestInteractionAdvancesTheSharedPhaseAndReportsTheQuest) {
  auto player = session();
  auto request = interaction_request(kAltarLocalId, kAltarInteraction);
  request["interaction_anim_id"] = kAltarAnimation;
  ASSERT_TRUE(player.handles(kGatheringStartInteractionRequest, request));

  const auto started =
      player.receive(kGatheringStartInteractionRequest, request, 1, position, kStartTicks);
  ASSERT_EQ(started.packets.back().opcode, 166);
  const auto executed = player.receive(
      kGatheringExecuteInteractionRequest,
      {{"interaction_anim_id", kAltarAnimation}, {"tick", "1"}}, 2, position,
      kStartTicks + kAltarExecuteMs * kTicksPerMs);
  expect_wire(executed.packets);
  ASSERT_EQ(executed.broadcasts.size(), 1U);
  EXPECT_EQ(executed.broadcasts.front().phase, 2);
  EXPECT_EQ(player.take_quest_events(),
            (QuestEvents{{QuestEventKind::DoodadInteracted, kAltarDoodad, 1, 1}}));
  EXPECT_EQ(nodes->doodad_phase(node(kAltarLocalId), *data->doodad(kAltarDoodad)), 2);

  // The timed reset returns the altar to its usable phase.
  const auto reset = nodes->collect_respawns(kStartTicks + (kAltarExecuteMs + kAltarResetMs) *
                                                               kTicksPerMs);
  EXPECT_EQ(reset.size(), 1U);
  EXPECT_EQ(nodes->doodad_phase(node(kAltarLocalId), *data->doodad(kAltarDoodad)), 1);
}

TEST_F(QuestDoodadTest, InstantInteractionAnswersInteractUnitAndBlocksASecondUse) {
  auto player = session();
  const auto request = instant_interaction_request(kLeverLocalId, kLeverInteraction);
  ASSERT_TRUE(player.handles(kGatheringInteractUnitRequest, request));

  const auto used =
      player.receive(kGatheringInteractUnitRequest, request, 5, position, kStartTicks);
  expect_wire(used.packets);
  ASSERT_FALSE(used.packets.empty());
  EXPECT_EQ(used.packets.back().opcode, kInteractUnitResponse);
  EXPECT_EQ(used.packets.back().message.at("interaction_id"), kLeverInteraction);
  EXPECT_FALSE(used.packets.back().message.contains("ec"));
  EXPECT_EQ(player.take_quest_events(),
            (QuestEvents{{QuestEventKind::DoodadInteracted, kLeverDoodad, 1, 1}}));
  // The lever is a portal: its MapMove destination travels with the outcome.
  ASSERT_TRUE(used.travel.has_value());
  EXPECT_EQ(used.travel->map_id, 20310);
  EXPECT_EQ(used.travel->x_mm, -1804040);

  // Until the lever returns to phase 1 nobody can use it again.
  const auto again =
      player.receive(kGatheringInteractUnitRequest, request, 6, position, kStartTicks + 1);
  ASSERT_FALSE(again.packets.empty());
  EXPECT_EQ(again.packets.back().opcode, kInteractUnitResponse);
  EXPECT_TRUE(again.packets.back().message.contains("ec"));
  EXPECT_TRUE(player.take_quest_events().empty());
}

TEST_F(QuestDoodadTest, SpotUnitIdsResolvePerWorldOfTheMap) {
  const auto unit_id = doodad_unit_id(kDoodadSector, kAltarLocalId);
  const auto *field = data->doodad_point_of_unit(kSharedMap, unit_id);
  const auto *dungeon = data->doodad_point_of_unit(kDungeonMap, unit_id);
  ASSERT_NE(field, nullptr);
  ASSERT_NE(dungeon, nullptr);
  EXPECT_EQ(field->doodad_id, kAltarDoodad);
  EXPECT_EQ(dungeon->doodad_id, kLeverDoodad);
  EXPECT_EQ(dungeon->world, 1);
  EXPECT_EQ(data->doodad_point_of_unit(4242, unit_id), nullptr);
  EXPECT_TRUE(data->names_quest_doodad(unit_id));
  EXPECT_EQ(data->doodad_point_count(), 7U);
}

TEST_F(QuestDoodadTest, CliffsDefeatsUnlockOnlyTheEncounterObjectsOnTheCorrectMap) {
  EXPECT_TRUE(nodes->cliffs_monster_defeated(kSharedMap, 5001510, kStartTicks).empty());
  EXPECT_TRUE(nodes->cliffs_monster_defeated(kCliffsMap, 999, kStartTicks).empty());
  const auto elite = nodes->cliffs_monster_defeated(kCliffsMap, 5001510, kStartTicks);
  ASSERT_EQ(elite.size(), 2U);
  EXPECT_EQ(elite[0].node.sector_id, kEliteSector);
  EXPECT_EQ(elite[0].node.index, 5U);
  EXPECT_EQ(elite[0].phase, 2);
  EXPECT_EQ(elite[1].node.index, 8U);
  EXPECT_EQ(elite[1].phase, 1);
  const auto captain = nodes->cliffs_monster_defeated(kCliffsMap, 5001104, kStartTicks);
  ASSERT_EQ(captain.size(), 1U);
  EXPECT_EQ(captain[0].node.sector_id, kRiftSector);
  EXPECT_EQ(captain[0].node.index, 2U);
  EXPECT_EQ(captain[0].phase, 1);
  EXPECT_TRUE(nodes->cliffs_monster_defeated(kCliffsMap, 5001104, kStartTicks).empty());
  expect_wire(gathering_state_packets(elite));
  expect_wire(gathering_state_packets(captain));
}

TEST_F(QuestDoodadTest, CliffsProgressRestoresUnlocksWithoutResettingUsedObjects) {
  EXPECT_TRUE(nodes->restore_cliffs_progress(kCliffsMap, durable, kStartTicks).empty());
  QuestProgress elite;
  elite.objectives[6] = 1;
  durable.quests.emplace(kCannonQuest, elite);
  QuestProgress captain;
  captain.objectives[1] = 1;
  durable.quests.emplace(kCaptainQuest, captain);
  EXPECT_TRUE(nodes->restore_cliffs_progress(kSharedMap, durable, kStartTicks).empty());
  ASSERT_EQ(nodes->restore_cliffs_progress(kCliffsMap, durable, kStartTicks).size(), 4U);
  const GatheringNodeKey rift{kCliffsMap, GatheringNodeKind::Doodad, kRiftSector, 2};
  ASSERT_TRUE(nodes->try_advance_doodad(rift, *data->doodad(kRiftDoodad), 1, 2,
                                        kStartTicks, std::nullopt));
  EXPECT_TRUE(nodes->restore_cliffs_progress(kCliffsMap, durable, kStartTicks).empty());
  EXPECT_EQ(nodes->doodad_phase(rift, *data->doodad(kRiftDoodad)), 2);
}

TEST_F(QuestDoodadTest, CliffsRiftRequiresCaptainCreditAndReportsQuestTravel) {
  position.map_id = kCliffsMap;
  auto player = session();
  auto request = interaction_request(2, kRiftDoodad);
  request["target_unit_id"] = std::to_string(doodad_unit_id(kRiftSector, 2));
  request["interaction_anim_id"] = kAltarAnimation;
  const auto refused = player.receive(kGatheringStartInteractionRequest, request, 1, position,
                                      kStartTicks);
  EXPECT_EQ(refused.packets.back().message.at("ec"), "InteractionAnim_InvalidCondition");
  QuestProgress quest;
  quest.objectives[1] = 1;
  durable.quests.emplace(kCaptainQuest, quest);
  const auto started = player.receive(kGatheringStartInteractionRequest, request, 2, position,
                                      kStartTicks);
  EXPECT_FALSE(started.packets.back().message.contains("ec"));
  const auto executed = player.receive(kGatheringExecuteInteractionRequest,
                                       {{"interaction_anim_id", kAltarAnimation}, {"tick", "1"}},
                                       3, position, kStartTicks + kAltarExecuteMs * kTicksPerMs);
  ASSERT_TRUE(executed.travel.has_value());
  EXPECT_EQ(executed.travel->map_id, kCliffsMap);
  EXPECT_EQ(player.take_quest_events(),
            (QuestEvents{{QuestEventKind::DoodadInteracted, kRiftDoodad, 1, 1}}));
  expect_wire(executed.packets);
}

TEST_F(QuestDoodadTest, CliffsEliteObjectPlaysCapturedSceneAndReportsQuestInteraction) {
  position.map_id = kCliffsMap;
  auto player = session();
  nodes->cliffs_monster_defeated(kCliffsMap, 5001510, kStartTicks);
  const Json request{{"target_unit_id", std::to_string(doodad_unit_id(kEliteSector, 8))},
                     {"param", {{"interaction_id", kEliteObjectInteraction}}}};
  const auto refused = player.receive(kGatheringInteractUnitRequest, request, 1, position,
                                      kStartTicks);
  EXPECT_EQ(refused.packets.back().message.at("ec"), "InteractionAnim_InvalidCondition");
  ASSERT_EQ(observed_conditions.size(), 4U);
  EXPECT_EQ(observed_conditions.at(0).at("quest_id"), 90100021);
  EXPECT_EQ(observed_conditions.at(1).at("progress_group"), 4);
  EXPECT_EQ(observed_conditions.at(3).at("requirement"), "QuestComplete");
  EXPECT_TRUE(player.take_quest_events().empty());
  conditions_allowed = true;
  const auto used = player.receive(kGatheringInteractUnitRequest, request, 2, position,
                                   kStartTicks);
  ASSERT_TRUE(used.cut_scene_id.has_value());
  EXPECT_EQ(*used.cut_scene_id, 1550);
  EXPECT_EQ(player.take_quest_events(),
            (QuestEvents{{QuestEventKind::DoodadInteracted, kEliteObject, 1, 1}}));
  expect_wire(used.packets);

  // Another eligible character sees phase 2 and receives the same scene and quest event.
  auto second_player = session();
  const auto second = second_player.receive(kGatheringInteractUnitRequest, request, 3, position,
                                            kStartTicks + 1);
  ASSERT_TRUE(second.cut_scene_id.has_value());
  EXPECT_EQ(*second.cut_scene_id, 1550);
  EXPECT_EQ(second_player.take_quest_events(),
            (QuestEvents{{QuestEventKind::DoodadInteracted, kEliteObject, 2, 1}}));
}

TEST_F(QuestDoodadTest, CliffsCannonOpensTheBarrierAndReplaysItsPhaseToLaterVisitors) {
  position.map_id = kCliffsMap;
  auto player = session();
  const Json request{{"target_unit_id", std::to_string(doodad_unit_id(kCannonSector, 1))},
                     {"param", {{"interaction_id", kCannonInteraction}}}};
  const auto refused = player.receive(kGatheringInteractUnitRequest, request, 1, position,
                                      kStartTicks);
  EXPECT_EQ(refused.packets.back().message.at("ec"), "InteractionAnim_InvalidCondition");

  QuestProgress quest;
  quest.objectives[1] = 1;
  durable.quests.emplace(kCannonQuest, quest);
  const auto used = player.receive(kGatheringInteractUnitRequest, request, 2, position,
                                   kStartTicks + kTicksPerMs);
  ASSERT_TRUE(used.cut_scene_id.has_value());
  EXPECT_EQ(*used.cut_scene_id, 301006);
  ASSERT_EQ(used.broadcasts.size(), 2U);
  EXPECT_EQ(used.broadcasts[1].node.sector_id, kBarrierSector);
  EXPECT_EQ(used.broadcasts[1].node.index, 2U);
  EXPECT_EQ(used.broadcasts[1].phase, 0);
  EXPECT_EQ(nodes->doodads_on_map(kCliffsMap).size(), 2U);
  EXPECT_EQ(player.take_quest_events(),
            (QuestEvents{{QuestEventKind::DoodadInteracted, kCannonDoodad, 1, 1}}));
  expect_wire(used.packets);
}

TEST_F(QuestDoodadTest, RescueDoorConsumesTheCapturedKeyItemAfterACommittedInteraction) {
  CharacterItem key;
  key.id = 27;
  key.cms_id = kPrisonKey;
  character.items.push_back(key);
  auto player = session();
  auto request = interaction_request(kPrisonDoorLocalId, kPrisonInteraction);
  request["interaction_anim_id"] = kAltarAnimation;

  const auto refused = player.receive(kGatheringStartInteractionRequest, request, 1, position,
                                      kStartTicks);
  EXPECT_EQ(refused.packets.back().message.at("ec"), "InteractionAnim_InvalidCondition");

  QuestProgress quest;
  quest.objectives[1] = 1;
  durable.quests.emplace(kRescueQuest, quest);
  const auto started = player.receive(kGatheringStartInteractionRequest, request, 2, position,
                                      kStartTicks + kTicksPerMs);
  EXPECT_EQ(started.packets.back().opcode, 166);
  const auto executed = player.receive(
      kGatheringExecuteInteractionRequest,
      {{"interaction_anim_id", kAltarAnimation}, {"tick", "1"}}, 3, position,
      kStartTicks + (kAltarExecuteMs + 1) * kTicksPerMs);
  expect_wire(executed.packets);
  EXPECT_TRUE(character.items.empty());
  EXPECT_EQ(committed_trade.removed_item_ids, std::vector<std::int64_t>{27});
  EXPECT_EQ(player.take_quest_events(),
            (QuestEvents{{QuestEventKind::DoodadInteracted, kPrisonDoor, 1, 1}}));
}

TEST_F(QuestDoodadTest, RescueDoorKeepsLegacyKeyAndPhaseWhenCommitFails) {
  QuestProgress quest;
  quest.objectives[1] = 1;
  durable.quests.emplace(kRescueQuest, quest);
  durable.stackables[kPrisonKey] = 1;
  auto player = session();
  auto request = interaction_request(kPrisonDoorLocalId, kPrisonInteraction);
  request["interaction_anim_id"] = kAltarAnimation;
  commit_succeeds = false;
  player.receive(kGatheringStartInteractionRequest, request, 1, position, kStartTicks);
  const auto failed = player.receive(
      kGatheringExecuteInteractionRequest,
      {{"interaction_anim_id", kAltarAnimation}, {"tick", "1"}}, 2, position,
      kStartTicks + kAltarExecuteMs * kTicksPerMs);
  EXPECT_EQ(failed.packets.back().message.at("ec"), "InteractionAnim_GatheringFail");
  EXPECT_EQ(durable.stackables.at(kPrisonKey), 1);
  EXPECT_EQ(nodes->doodad_phase(node(kPrisonDoorLocalId), *data->doodad(kPrisonDoor)), 1);
  EXPECT_TRUE(player.take_quest_events().empty());
}

TEST_F(QuestDoodadTest, UnitInteractionsOfOtherTargetsAreLeftToTheirOwners) {
  auto player = session();
  // An unprofiled spot and an NPC naming the same interaction id are not quest doodads.
  auto stranger = instant_interaction_request(99, kLeverInteraction);
  EXPECT_FALSE(player.handles(kGatheringInteractUnitRequest, stranger));
  stranger["target_unit_id"] = std::to_string((1ULL << 60) | 2);
  EXPECT_FALSE(player.handles(kGatheringInteractUnitRequest, stranger));
  EXPECT_FALSE(player.handles(kGatheringInteractUnitRequest,
                              instant_interaction_request(kLeverLocalId, kLeverInteraction + 1)));

  // Too far from the lever.
  position = {kSharedMap, 60000, 0, 0, 0};
  const auto far_request = instant_interaction_request(kLeverLocalId, kLeverInteraction);
  const auto refused =
      player.receive(kGatheringInteractUnitRequest, far_request, 7, position, kStartTicks);
  EXPECT_EQ(refused.packets.back().message.at("ec"), "TooFar");
}

} // namespace
} // namespace chronicles
