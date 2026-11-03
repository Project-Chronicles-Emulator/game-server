#include "protocol/protocol.hpp"
#include "world/features/travel/map_travel.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace chronicles::travel {
namespace {

using Json = nlohmann::json;

constexpr int kField = 9012;
constexpr int kHousingArea = 20101;
constexpr int kCave = 10119;
constexpr int kTrainingGround = 10118;
constexpr int kDungeon = 10111;
constexpr int kGateQuest = 10000046;

Json map_row(int map_id, const char *type, const char *world, int parent) {
  return {{"map_id", map_id},           {"type", type},
          {"world", world},             {"parent_map_id", parent},
          {"origin_map_id", 1},         {"channel_max", 4},
          {"retain_on_disconnect", true}, {"entrance_conditions", Json::array()},
          {"exit_conditions", Json::array()}};
}

Json box(const char *kind, const char *world, std::vector<int> maps, std::array<double, 3> center,
         const char *wire_x) {
  return {{"trigger_id_wire", {wire_x, "7"}},
          {"trigger_guid", std::string("guid-") + wire_x},
          {"world", world},
          {"layer", "layer"},
          {"maps", maps},
          {"active", true},
          {"kind", kind},
          {"shape", "box"},
          {"center_cm", {center[0], center[1], center[2]}},
          {"extent_cm", {500, 500, 500}},
          {"rotation_deg", {0, 0, 0}}};
}

Json document() {
  auto field = map_row(kField, "Field", "ArcheWorld", 0);
  auto housing = map_row(kHousingArea, "HousingArea", "ArcheWorld", kField);
  auto cave = map_row(kCave, "Instance", "ArcheWorld", kField);
  cave["area_cm"] = {{"min", {-1000, -1000, -1000}}, {"max", {1000, 1000, 1000}}};
  auto training = map_row(kTrainingGround, "Instance", "ArcheWorld", kField);
  training["entrance_conditions"] = Json::array({{{"group", 0},
                                                  {"operator", "OR"},
                                                  {"negated", false},
                                                  {"kind", "Quest"},
                                                  {"quest_id", kGateQuest},
                                                  {"requirement", "QuestComplete"}}});
  auto dungeon = map_row(kDungeon, "Dungeon", "DGL_Cave", kField);
  dungeon["area_cm"] = {{"min", {9000, -1000, -1000}}, {"max", {11000, 1000, 1000}}};
  dungeon["translation_cm"] = {10000, 0, 0};
  dungeon["entrance_conditions"] =
      Json::array({{{"group", 1}, {"operator", "AND"}, {"negated", false},
                    {"kind", "PartyState"}, {"state", "NoParty"}},
                   {{"group", 2}, {"operator", "AND"}, {"negated", false},
                    {"kind", "PartyState"}, {"state", "LeaderOnly"}}});

  auto cave_entry = box("channelSession", "ArcheWorld", {kField}, {0, 0, 0}, "1");
  cave_entry["destination_map_id"] = kCave;
  cave_entry["spawn_position_id"] = 0;
  auto training_entry = box("channelSession", "ArcheWorld", {kField}, {5000, 0, 0}, "2");
  training_entry["destination_map_id"] = kTrainingGround;
  training_entry["spawn_position_id"] = 0;
  auto dungeon_entry = box("portal", "ArcheWorld", {kField}, {10000, 0, 0}, "3");
  dungeon_entry["portal_id"] = 101011;
  auto dungeon_exit = box("portal", "DGL_Cave", {kDungeon}, {10200, 0, 0}, "4");
  dungeon_exit["portal_id"] = 1;
  auto same_channel = box("channel", "ArcheWorld", {kField}, {-5000, 0, 0}, "5");
  same_channel["destination_map_id"] = kField;
  auto scripted = box("rules", "ArcheWorld", {}, {20000, 0, 0}, "6");
  scripted["rules"] = Json::array(
      {{{"trigger_type", "Enter"},
        {"conditions", Json::array()},
        {"tasks", {{{"kind", "PlayCutScene"}, {"cut_scene_id", 77}},
                   {{"kind", "ClientPlayDialogue"}, {"dialogue_id", 5}},
                   {{"kind", "ChangeDoodadPhase"},
                    {"doodad_key", "door"},
                    {"phase", 2},
                    {"unit_id", "1240411111440390"}}}}}});

  return {{"format_version", 2},
          {"maps", {field, housing, cave, training, dungeon}},
          {"volumes",
           {cave_entry, training_entry, dungeon_entry, dungeon_exit, same_channel, scripted}},
          {"portals",
           {{{"portal_id", 101011}, {"from_map_id", kField}, {"to_map_id", kDungeon},
             {"spawn_id", 1}},
            {{"portal_id", 1}, {"from_map_id", kDungeon}, {"to_map_id", kField},
             {"spawn_id", 101011}},
            {{"portal_id", 11}, {"from_map_id", kDungeon}, {"to_map_id", kDungeon},
             {"spawn_id", 11}, {"arrival_mm", {104000, 5000, -200}},
             {"arrival_interaction_id", 10110101}},
            {{"portal_id", 13}, {"from_map_id", kDungeon}, {"to_map_id", kDungeon},
             {"spawn_id", 13}}}},
          {"map_move_cut_scenes",
           {{{"from_map_id", kField}, {"to_map_id", kDungeon}, {"timing", "Pre"},
             {"cut_scene_id", 303}, {"conditions", Json::array()}}}},
          {"cut_scenes",
           {{{"cut_scene_id", 77}, {"invisible", true}, {"client_only", false}},
            {{"cut_scene_id", 303}, {"invisible", false}, {"client_only", false}},
            {{"cut_scene_id", 401},
             {"invisible", true},
             {"client_only", false},
             {"end_point",
              {{"map_id", kField},
               {"end_point_id", 30200230},
               {"position_mm", {-2718096, 3700107, -548910}},
               {"source", "doodad"}}}},
            {{"cut_scene_id", 402},
             {"invisible", false},
             {"client_only", false},
             {"end_point", {{"map_id", kCave}, {"end_point_id", 7}, {"source", "arrival_rule"}}}},
            {{"cut_scene_id", 403},
             {"invisible", false},
             {"client_only", false},
             {"end_point",
              {{"map_id", kField}, {"end_point_id", 8}, {"source", "arrival_rule"}}}}}},
          {"cliffs_gate",
           {{"unit_id", "123"},
            {"interaction_id", 456},
            {"source_map_id", kField},
            {"position_mm", {0, 0, 0}},
            {"required_quest_id", kGateQuest},
            {"cut_scene_id", 77},
            {"destination_map_id", kCave},
            {"destination_mm", {100, 200, 300}},
            {"destination_yaw", 90.0}}}};
}

Json trigger(const char *wire_x, const WorldPosition &position, bool leave = false) {
  return {{"trigger_id_x", wire_x},
          {"trigger_id_y", "7"},
          {"leave", leave},
          {"pos",
           {{"millimeter_x", position.x_mm},
            {"millimeter_y", position.y_mm},
            {"millimeter_z", position.z_mm}}}};
}

class MapTravel : public testing::Test {
protected:
  Catalog catalog{document()};
  bool gate_quest_complete{};
  TravelContext context{[this](const Json &row) {
                          return row.at("kind") == "Quest" && gate_quest_complete;
                        },
                        PartyRole::none};
};

TEST_F(MapTravel, InstanceVolumeMovesTheCharacterIntoTheInstanceWhereItStands) {
  const WorldPosition at_cave{kField, 1000, 2000, 300, 45.0};
  const auto plan = catalog.level_trigger(at_cave, trigger("1", at_cave), context);
  ASSERT_TRUE(plan.handled);
  ASSERT_TRUE(plan.destination.has_value());
  EXPECT_EQ(plan.destination->map_id, kCave);
  EXPECT_EQ(plan.destination->x_mm, 1000);
  EXPECT_EQ(plan.destination->y_mm, 2000);
  EXPECT_TRUE(plan.client_waits_for_move);
}

TEST_F(MapTravel, GatedInstanceOpensOnlyWhenItsEntranceConditionHolds) {
  const WorldPosition at_gate{kField, 50000, 0, 0, 0.0};
  const auto closed = catalog.level_trigger(at_gate, trigger("2", at_gate), context);
  EXPECT_TRUE(closed.handled);
  EXPECT_FALSE(closed.destination.has_value());
  EXPECT_TRUE(closed.client_waits_for_move);
  EXPECT_FALSE(closed.refusal.empty());

  gate_quest_complete = true;
  const auto open = catalog.level_trigger(at_gate, trigger("2", at_gate), context);
  ASSERT_TRUE(open.destination.has_value());
  EXPECT_EQ(open.destination->map_id, kTrainingGround);
}

TEST_F(MapTravel, DungeonPortalsEnterAndLeaveWithTheMapMoveCutScene) {
  const WorldPosition at_entrance{kField, 100000, 0, 0, 0.0};
  const auto entered = catalog.level_trigger(at_entrance, trigger("3", at_entrance), context);
  ASSERT_TRUE(entered.destination.has_value());
  EXPECT_EQ(entered.destination->map_id, kDungeon);
  EXPECT_EQ(entered.destination->x_mm, 100000);
  EXPECT_EQ(entered.cut_scene_id, 303);

  const WorldPosition at_exit{kDungeon, 102000, 0, 0, 0.0};
  const auto left = catalog.level_trigger(at_exit, trigger("4", at_exit), context);
  ASSERT_TRUE(left.destination.has_value());
  EXPECT_EQ(left.destination->map_id, kField);
  EXPECT_EQ(left.cut_scene_id, 0);
}

TEST_F(MapTravel, DungeonEntryFollowsTheAuthoredPartyStates) {
  const WorldPosition at_entrance{kField, 100000, 0, 0, 0.0};
  context.party_role = PartyRole::member;
  EXPECT_FALSE(
      catalog.level_trigger(at_entrance, trigger("3", at_entrance), context).destination);
  context.party_role = PartyRole::leader;
  EXPECT_TRUE(catalog.level_trigger(at_entrance, trigger("3", at_entrance), context).destination);
}

TEST_F(MapTravel, ChannelEntryNeedsTheCharacterAtTheChildMap) {
  const WorldPosition outside{kField, 0, 0, 0, 0.0};
  EXPECT_TRUE(catalog.travel_allowed(kField, kDungeon, context));
  const auto refused = catalog.channel_entry(outside, {{"map_id", kDungeon}}, context);
  EXPECT_FALSE(refused.destination.has_value());
  EXPECT_TRUE(refused.client_waits_for_move);

  const WorldPosition inside_area{kField, 100500, 0, 0, 0.0};
  const auto entered = catalog.channel_entry(inside_area, {{"map_id", kDungeon}}, context);
  ASSERT_TRUE(entered.destination.has_value());
  EXPECT_EQ(entered.destination->map_id, kDungeon);
}

TEST_F(MapTravel, RequestsAwayFromTheVolumeOrTheTrackedPositionAreRefused) {
  const WorldPosition elsewhere{kField, 30000000, 0, 0, 0.0};
  const auto outside = catalog.level_trigger(elsewhere, trigger("1", elsewhere), context);
  EXPECT_FALSE(outside.destination.has_value());
  EXPECT_TRUE(outside.client_waits_for_move);

  const WorldPosition at_cave{kField, 0, 0, 0, 0.0};
  const WorldPosition far_report{kField, 20000, 0, 0, 0.0};
  EXPECT_FALSE(catalog.level_trigger(at_cave, trigger("1", far_report), context).destination);
}

TEST_F(MapTravel, ChannelVolumeOfTheCurrentMapNeedsNoMove) {
  const WorldPosition here{kField, -50000, 0, 0, 0.0};
  const auto plan = catalog.level_trigger(here, trigger("5", here), context);
  EXPECT_TRUE(plan.handled);
  EXPECT_FALSE(plan.destination.has_value());
  EXPECT_FALSE(plan.client_waits_for_move);
}

TEST_F(MapTravel, SameMapPortalsJumpToTheirRecoveredArrival) {
  Json changed = document();
  changed["volumes"][3]["portal_id"] = 11;
  const Catalog dungeon_catalog(changed);
  const WorldPosition at_exit{kDungeon, 102000, 0, 0, 30.0};
  const auto plan = dungeon_catalog.level_trigger(at_exit, trigger("4", at_exit), context);
  ASSERT_TRUE(plan.destination.has_value());
  EXPECT_EQ(plan.destination->map_id, kDungeon);
  EXPECT_EQ(plan.destination->x_mm, 104000);
  EXPECT_EQ(plan.destination->y_mm, 5000);
  EXPECT_EQ(plan.destination->z_mm, -200);
  EXPECT_FLOAT_EQ(plan.destination->yaw, 30.0f);
  EXPECT_EQ(plan.cut_scene_id, 0);
  EXPECT_TRUE(plan.client_waits_for_move);
}

TEST_F(MapTravel, SameMapPortalsWithoutRecoveredArrivalAreRefused) {
  Json changed = document();
  changed["volumes"][3]["portal_id"] = 13;
  const Catalog dungeon_catalog(changed);
  const WorldPosition at_exit{kDungeon, 102000, 0, 0, 0.0};
  const auto plan = dungeon_catalog.level_trigger(at_exit, trigger("4", at_exit), context);
  EXPECT_FALSE(plan.destination.has_value());
  EXPECT_FALSE(plan.refusal.empty());
  EXPECT_TRUE(plan.client_waits_for_move);
}

TEST_F(MapTravel, ArrivalsAreOnlyAcceptedOnSameMapPortals) {
  Json changed = document();
  changed["portals"][0]["arrival_mm"] = {1, 2, 3};
  EXPECT_THROW(Catalog{changed}, std::runtime_error);
}

TEST_F(MapTravel, ScriptedVolumesPlayTheirServerCutSceneAndLeaveDoodadsToTheWorld) {
  const WorldPosition here{kField, 200000, 0, 0, 0.0};
  const auto plan = catalog.level_trigger(here, trigger("6", here), context);
  ASSERT_EQ(plan.cut_scenes, std::vector<int>{77});
  ASSERT_EQ(plan.doodad_phases.size(), 1u);
  EXPECT_EQ(plan.doodad_phases.front().unit_id, "1240411111440390");
  EXPECT_EQ(plan.doodad_phases.front().phase, 2);
  EXPECT_TRUE(plan.actor_doodad_phases.empty());
  EXPECT_FALSE(plan.client_waits_for_move);

  TravelSession session(std::make_shared<Catalog>(document()), 4097);
  const auto started = session.play(plan, here, 1000000);
  // The world records and delivers volume doodad phases; the session plays the cutscene only.
  ASSERT_EQ(started.size(), 2u);
  EXPECT_EQ(started[0].opcode, kVisibleChannelNotification);
  EXPECT_EQ(started[1].opcode, kCutScenePlayNotification);
  EXPECT_EQ(started[1].message.at("cut_scene_id"), 77);
  // A repeat while the cutscene plays never starts a second cutscene.
  EXPECT_TRUE(session.play(plan, here, 1000000).empty());

  EXPECT_FALSE(session.end({kCutSceneEndRequest, 1, {{"cut_scene_id", 5}}}).has_value());
  const auto ended = session.end({kCutSceneEndRequest, 2, {{"cut_scene_id", 77}}});
  ASSERT_TRUE(ended.has_value());
  ASSERT_EQ(ended->size(), 1u);
  EXPECT_EQ(ended->front().message.at("visible_channel"), "3");
  EXPECT_FALSE(session.take_cut_scene_destination().has_value());

  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : started) {
    EXPECT_NO_THROW(protocol.encode(packet.opcode, packet.message));
  }
}

TEST_F(MapTravel, VolumeDoodadsAreListedForTheMapsTheVolumeIsPlacedOn) {
  EXPECT_EQ(catalog.volume_doodads(kField), std::vector<std::uint64_t>{1240411111440390ULL});
  // Static volumes belong to every map of their world.
  EXPECT_EQ(catalog.volume_doodads(kCave), std::vector<std::uint64_t>{1240411111440390ULL});
  EXPECT_TRUE(catalog.volume_doodads(kDungeon).empty());
}

TEST_F(MapTravel, InstancesAndDungeonsGetCopies) {
  EXPECT_EQ(catalog.copy_maps(), (std::set<int>{kTrainingGround, kDungeon, kCave}));
}

TEST_F(MapTravel, CutSceneEndPointsMoveTheCharacterAfterTheMatchingEnd) {
  const auto shared = std::make_shared<Catalog>(document());
  TravelSession session(shared, 4097);
  const WorldPosition here{kCave, 100, 200, 300, 15.0};

  TravelPlan plan;
  plan.cut_scenes.push_back(401);
  const auto started = session.play(plan, here, 1000000);
  ASSERT_EQ(started.size(), 2u);
  const auto ended = session.end({kCutSceneEndRequest, 1, {{"cut_scene_id", 401}}});
  ASSERT_TRUE(ended.has_value());
  // The map move restores visibility itself.
  EXPECT_TRUE(ended->empty());
  const auto destination = session.take_cut_scene_destination();
  ASSERT_TRUE(destination.has_value());
  EXPECT_EQ(destination->map_id, kField);
  EXPECT_EQ(destination->x_mm, -2718096);
  EXPECT_EQ(destination->y_mm, 3700107);
}

TEST_F(MapTravel, CutSceneEndPointsWithoutPositionUseTheArrivalRule) {
  // Inside the cave's area the character keeps its position on the end map.
  const WorldPosition in_field{kField, 500, 600, 700, 20.0};
  const auto into_cave = catalog.cut_scene_end_destination(402, in_field);
  ASSERT_TRUE(into_cave.has_value());
  EXPECT_EQ(into_cave->map_id, kCave);
  EXPECT_EQ(into_cave->x_mm, 500);
  EXPECT_FLOAT_EQ(into_cave->yaw, 20.0f);

  // Outside the cave's area, without a level origin, there is no arrival.
  const WorldPosition far_away{kField, 900000, 0, 0, 0.0};
  EXPECT_FALSE(catalog.cut_scene_end_destination(402, far_away).has_value());

  // An end point that would leave the character where it stands needs no move.
  EXPECT_FALSE(catalog.cut_scene_end_destination(403, in_field).has_value());
  // Cutscenes without an end point never move the character.
  EXPECT_FALSE(catalog.cut_scene_end_destination(77, in_field).has_value());

  ASSERT_TRUE(catalog.cut_scene_end_point(401).has_value());
  EXPECT_EQ(catalog.cut_scene_end_point(401)->end_point_id, 30200230);
}

TEST_F(MapTravel, CliffsGateRequiresTheQuestAndMovesOnlyAfterTheMatchingCutSceneEnds) {
  const WorldPosition at_gate{kField, 0, 0, 0, 0.0};
  const Json request{{"target_unit_id", "123"}, {"param", {{"interaction_id", 456}}}};
  EXPECT_FALSE(catalog.cliffs_gate(at_gate, request, context).refusal.empty());

  gate_quest_complete = true;
  const auto plan = catalog.cliffs_gate(at_gate, request, context);
  ASSERT_TRUE(plan.handled);
  ASSERT_TRUE(plan.refusal.empty());
  ASSERT_EQ(plan.cut_scenes, std::vector<int>{77});
  ASSERT_TRUE(plan.cut_scene_destination.has_value());

  TravelSession session(std::make_shared<Catalog>(document()), 4097);
  const auto started = session.play(plan, at_gate, 1000000);
  ASSERT_EQ(started.size(), 3u);
  EXPECT_EQ(started[0].opcode, kDoodadPhaseChangedNotification);
  EXPECT_EQ(started[1].opcode, kVisibleChannelNotification);
  EXPECT_EQ(started[2].opcode, kCutScenePlayNotification);
  EXPECT_FALSE(session.end({kCutSceneEndRequest, 1, {{"cut_scene_id", 78}}}).has_value());
  EXPECT_FALSE(session.take_cut_scene_destination().has_value());

  const auto ended = session.end({kCutSceneEndRequest, 2, {{"cut_scene_id", 77}}});
  ASSERT_TRUE(ended.has_value());
  EXPECT_TRUE(ended->empty());
  const auto destination = session.take_cut_scene_destination();
  ASSERT_TRUE(destination.has_value());
  EXPECT_EQ(destination->map_id, kCave);
  EXPECT_EQ(destination->x_mm, 100);
  EXPECT_FALSE(session.take_cut_scene_destination().has_value());

  context.party_role = PartyRole::member;
  EXPECT_FALSE(catalog.cliffs_gate(at_gate, request, context).refusal.empty());
  context.party_role = PartyRole::none;
  const WorldPosition far_away{kField, 100000, 0, 0, 0.0};
  EXPECT_FALSE(catalog.cliffs_gate(far_away, request, context).refusal.empty());
}

TEST_F(MapTravel, ChannelEntryLeavesAnInstanceForItsParent) {
  const WorldPosition inside{kCave, 0, 0, 0, 0.0};
  const auto plan = catalog.channel_entry(inside, {{"map_id", 0}}, context);
  ASSERT_TRUE(plan.destination.has_value());
  EXPECT_EQ(plan.destination->map_id, kField);
}

TEST_F(MapTravel, ParentArrivalKeepsThePositionOnTheParentMapAndNeedsAParent) {
  const WorldPosition inside{kCave, 400, -300, 20, 1.25F};
  const auto parent = catalog.parent_arrival(inside);
  ASSERT_TRUE(parent.has_value());
  EXPECT_EQ(*parent, (WorldPosition{kField, 400, -300, 20, 1.25F}));

  EXPECT_FALSE(catalog.parent_arrival(WorldPosition{kField, 0, 0, 0, 0.0F}).has_value());
  EXPECT_FALSE(catalog.parent_arrival(WorldPosition{424242, 0, 0, 0, 0.0F}).has_value());
}

TEST_F(MapTravel, HousingAreaParentNamesOnlyTheFieldUnderAHousingArea) {
  EXPECT_EQ(catalog.housing_area_parent(kHousingArea), kField);

  // Instances, dungeons and fields have their own coordinates or no parent.
  EXPECT_FALSE(catalog.housing_area_parent(kCave).has_value());
  EXPECT_FALSE(catalog.housing_area_parent(kDungeon).has_value());
  EXPECT_FALSE(catalog.housing_area_parent(kField).has_value());
  EXPECT_FALSE(catalog.housing_area_parent(424242).has_value());
}

TEST_F(MapTravel, UnknownTriggersAreLeftToOtherHandlers) {
  const WorldPosition here{kField, 0, 0, 0, 0.0};
  EXPECT_FALSE(catalog.level_trigger(here, trigger("999", here), context).handled);
  EXPECT_TRUE(catalog.resumable(kDungeon));
  EXPECT_TRUE(catalog.resumable(kCave));
}

// The live report: the first cave after the tutorial (the Watchtower of Hope smuggling cave,
// map 10119) and the first dungeon (Z01D01, map 10111) never opened.
TEST(MapTravelProfile, GeneratedProfileOpensTheFirstCaveAndDungeon) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }
  const auto profile =
      std::filesystem::path(catalog_path).parent_path() / "travel" / "definitions.json";
  if (!std::filesystem::exists(profile)) {
    GTEST_SKIP() << "Generate data/generated/travel/definitions.json first";
  }
  const auto catalog = Catalog::load(profile);
  TravelContext solo{[](const Json &) { return false; }, PartyRole::none};

  // Smuggling cave channelSession volume in Zone_01_Public_02 at (-134601, 327303) cm.
  const WorldPosition cave_mouth{kField, -1346010, 3273030, -623280, 0.0};
  const auto cave = catalog->channel_entry(cave_mouth, {{"map_id", kCave}}, solo);
  ASSERT_TRUE(cave.destination.has_value()) << cave.refusal;
  EXPECT_EQ(cave.destination->map_id, kCave);

  // Shabby Temple field portal 101011 at (-147794, 382501) cm lies inside the dungeon's area.
  const WorldPosition temple{kField, -1477940, 3825010, -537870, 0.0};
  const auto dungeon = catalog->channel_entry(temple, {{"map_id", kDungeon}}, solo);
  ASSERT_TRUE(dungeon.destination.has_value()) << dungeon.refusal;
  EXPECT_EQ(dungeon.destination->map_id, kDungeon);
  EXPECT_TRUE(catalog->resumable(kDungeon));

  // The training ground opens only after quest 10000046.
  EXPECT_FALSE(catalog->travel_allowed(kField, kTrainingGround, solo));
  TravelContext finished{[](const Json &row) {
                           return row.at("kind") == "Quest" &&
                                  row.at("quest_id") == kGateQuest &&
                                  row.at("requirement") == "QuestComplete";
                         },
                         PartyRole::none};
  EXPECT_TRUE(catalog->travel_allowed(kField, kTrainingGround, finished));
  EXPECT_GT(catalog->volume_count(), 500u);

  // The housing area (map 20101) lies inside its parent field's area, so a character leaving
  // it without a recorded origin stays where it stands on map 9012.
  const WorldPosition housing_edge{kHousingArea, -480000, 3300000, -540000, 0.5F};
  const auto parent = catalog->parent_arrival(housing_edge);
  ASSERT_TRUE(parent.has_value());
  EXPECT_EQ(*parent, (WorldPosition{kField, -480000, 3300000, -540000, 0.5F}));
  EXPECT_EQ(catalog->housing_area_parent(kHousingArea), kField);
}

// Dungeon boss and exit jumps, cutscene end points and instance copies of the generated profile.
TEST(MapTravelProfile, GeneratedProfileResolvesDungeonJumpsAndCutSceneEndPoints) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }
  const auto profile =
      std::filesystem::path(catalog_path).parent_path() / "travel" / "definitions.json";
  if (!std::filesystem::exists(profile)) {
    GTEST_SKIP() << "Generate data/generated/travel/definitions.json first";
  }
  std::ifstream input(profile);
  const auto document = Json::parse(input);
  const auto catalog = Catalog::load(profile);

  // GoBoss (11) and GoExit (12) of the Shabby Temple use its boss and exit doodads' MapMove
  // interactions 10110101 and 10110102.
  std::map<int, Json> jumps;
  for (const auto &portal : document.at("portals")) {
    if (portal.at("from_map_id") == kDungeon && portal.contains("arrival_mm")) {
      jumps.emplace(portal.at("portal_id").get<int>(), portal);
    }
  }
  ASSERT_EQ(jumps.size(), 2u);
  EXPECT_EQ(jumps.at(11).at("arrival_interaction_id"), 10110101);
  EXPECT_EQ(jumps.at(11).at("arrival_mm"), Json::array({-1427810, 4003780, -540977}));
  EXPECT_EQ(jumps.at(12).at("arrival_interaction_id"), 10110102);

  // Cutscene 100002 (Ruins of Gold doodad 30200230) ends at that doodad's placement.
  const WorldPosition elsewhere{9021, -2700000, 3600000, -550000, 0.0};
  const auto ruins = catalog->cut_scene_end_destination(100002, elsewhere);
  ASSERT_TRUE(ruins.has_value());
  EXPECT_EQ(ruins->map_id, 9021);
  EXPECT_EQ(ruins->x_mm, -2718096);
  EXPECT_EQ(ruins->y_mm, 3700107);
  // Raid end points are left to the raid service.
  EXPECT_FALSE(catalog->cut_scene_end_destination(1501, elsewhere).has_value());

  const auto copies = catalog->copy_maps();
  EXPECT_TRUE(copies.contains(kDungeon));
  EXPECT_TRUE(copies.contains(kCave));
  EXPECT_FALSE(copies.contains(kField));
  EXPECT_FALSE(copies.contains(30104));
}

TEST(MapTravelProfile, CapturedCliffsGateUsesTheExportedPlacementAndHandoff) {
  const auto *profile_path = std::getenv("CHRONICLES_CLIFFS_TRAVEL_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIFFS_TRAVEL_PROFILE to the regenerated travel profile";
  }
  std::ifstream input(profile_path);
  const auto profile = Json::parse(input);
  const auto &gate = profile.at("cliffs_gate");
  const auto catalog = Catalog::load(profile_path);
  const auto &point = gate.at("position_mm");
  const WorldPosition position{gate.at("source_map_id"), point.at(0), point.at(1), point.at(2), 0};
  const Json request{{"target_unit_id", gate.at("unit_id")},
                     {"param", {{"interaction_id", gate.at("interaction_id")}}}};
  const TravelContext completed{[](const Json &row) {
                                  return row.at("kind") == "Quest" &&
                                         row.at("quest_id") == 20001051 &&
                                         row.at("requirement") == "QuestComplete";
                                },
                                PartyRole::none};
  const auto plan = catalog->cliffs_gate(position, request, completed);
  ASSERT_TRUE(plan.handled);
  ASSERT_TRUE(plan.refusal.empty());
  ASSERT_TRUE(plan.cut_scene_destination.has_value());
  EXPECT_EQ(plan.cut_scenes, std::vector<int>{1565});
  EXPECT_EQ(plan.cut_scene_destination->map_id, 30104);
  EXPECT_TRUE(catalog->resumable(30104));

  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto decoded = protocol.decode(protocol.encode(20068, request, 1));
  EXPECT_EQ(decoded.at("message").at("param").at("interaction_id"), 10100002);
}

TEST(MapTravelProfile, CliffsEliteObjectSceneCanPlayAndRestoreVisibility) {
  const auto *profile_path = std::getenv("CHRONICLES_CLIFFS_TRAVEL_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIFFS_TRAVEL_PROFILE to the regenerated travel profile";
  }
  const auto catalog = Catalog::load(profile_path);
  ASSERT_TRUE(catalog->server_cut_scene(1550));
  TravelSession session(catalog, 4097);
  TravelPlan plan;
  plan.cut_scenes.push_back(1550);
  const WorldPosition position{30104, 0, 0, 0, 0};
  const auto started = session.play(plan, position, 1000000);
  ASSERT_EQ(started.size(), 2U);
  EXPECT_EQ(started[0].opcode, kVisibleChannelNotification);
  EXPECT_EQ(started[1].opcode, kCutScenePlayNotification);
  EXPECT_EQ(started[1].message.at("cut_scene_id"), 1550);
  const auto ended = session.end({kCutSceneEndRequest, 1, {{"cut_scene_id", 1550}}});
  ASSERT_TRUE(ended.has_value());
  ASSERT_EQ(ended->size(), 1U);
  EXPECT_EQ(ended->front().message.at("visible_channel"), "3");
}

} // namespace
} // namespace chronicles::travel
