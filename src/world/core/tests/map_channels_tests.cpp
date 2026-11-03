#include "world/core/map_channels.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace chronicles {
namespace {

using Json = nlohmann::json;

Json map_row(int id) {
  return {{"id", std::to_string(id)},
          {"world", "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld"},
          {"type", "Field"},
          {"FK_enteranceConditionId", ""},
          {"FK_exitConditionId", ""}};
}

std::vector<Json> maps() {
  return {map_row(9012), map_row(10110), map_row(9361)};
}

Json boundary(int destination = 10110) {
  return {{"trigger_guid", "authored-checkpoint"},
          {"trigger_id_wire", {"1", "2"}},
          {"is_activate", true},
          {"kind", "channelSession"},
          {"shape", "box"},
          {"destination_map_id", destination},
          {"spawn_position_id", 0},
          {"geometry",
           {{"center", {{"x", 1000}, {"y", 2000}, {"z", -60000}}},
            {"extent", {{"x", 400}, {"y", 100}, {"z", 1000}}},
            {"rotation", {{"pitch", 0}, {"yaw", 90}, {"roll", 0}}}}}};
}

Json document(Json volumes = Json::array({boundary()})) {
  return {{"format_version", 1}, {"world", "ArcheWorld"}, {"volumes", volumes}};
}

WorldPosition position() {
  return {9012, 10000, 22500, -600000, 37};
}

Json request(const WorldPosition &point = position()) {
  return {
      {"pos",
       {{"millimeter_x", point.x_mm}, {"millimeter_y", point.y_mm}, {"millimeter_z", point.z_mm}}},
      {"body_yaw", point.yaw}};
}

TEST(MapChannels, RotatedBoundaryUsesTheReportedPointAndTheAuthoritativeYaw) {
  const MapChannels channels(document(), maps());
  auto reported = position();
  reported.y_mm += 100;
  reported.yaw = -20;
  const auto result = channels.resolve(position(), request(reported));

  ASSERT_TRUE(result);
  auto expected = reported;
  expected.map_id = 10110;
  expected.yaw = position().yaw;
  EXPECT_EQ(*result, expected);
  EXPECT_TRUE(channels.supports_map(10110));
  EXPECT_FALSE(channels.supports_map(8000));
}

TEST(MapChannels, RejectsOutsideRotatedThinAxis) {
  const MapChannels channels(document(), maps());
  auto outside = position();
  outside.x_mm += 2100;
  EXPECT_FALSE(channels.resolve(outside, request(outside)));
}

TEST(MapChannels, RejectsRemoteSpoofAndMalformedCoordinates) {
  const MapChannels channels(document(), maps());
  auto far = position();
  far.y_mm += 5001;
  EXPECT_FALSE(channels.resolve(far, request()));
  EXPECT_FALSE(channels.resolve(position(), Json::object()));

  auto malformed = request();
  malformed["pos"]["millimeter_x"] = 10000.5;
  EXPECT_FALSE(channels.resolve(position(), malformed));
  malformed["pos"]["millimeter_x"] = std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(channels.resolve(position(), malformed));
  malformed = request();
  malformed["body_yaw"] = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(channels.resolve(position(), malformed));
}

TEST(MapChannels, UnknownAndGatedTargetsAreNotUnlocked) {
  const MapChannels unknown(document(Json::array({boundary(12345)})), maps());
  EXPECT_FALSE(unknown.resolve(position(), request()));

  auto rows = maps();
  rows.at(1)["FK_enteranceConditionId"] = "10025";
  const MapChannels gated(document(), rows);
  EXPECT_FALSE(gated.resolve(position(), request()));
  EXPECT_FALSE(gated.supports_map(10110));

  rows = maps();
  rows.at(1)["FK_exitConditionId"] = "10026";
  EXPECT_FALSE(MapChannels(document(), rows).supports_map(10110));
}

TEST(MapChannels, RejectsOtherWorldAndUnknownSourceMap) {
  auto rows = maps();
  rows.at(1)["world"] = "/Game/Maps/Other/Other.Other";
  const MapChannels channels(document(), rows);
  EXPECT_FALSE(channels.resolve(position(), request()));

  auto unknown_source = position();
  unknown_source.map_id = 12345;
  EXPECT_FALSE(channels.resolve(unknown_source, request(unknown_source)));
}

TEST(MapChannels, InactiveAndUnresolvedKindsDoNotMove) {
  auto volume = boundary();
  volume["is_activate"] = false;
  EXPECT_FALSE(MapChannels(document(Json::array({volume})), maps()).resolve(position(), request()));

  volume = boundary();
  volume["kind"] = "channel";
  EXPECT_FALSE(MapChannels(document(Json::array({volume})), maps()).resolve(position(), request()));

  volume = boundary();
  volume["spawn_position_id"] = 1;
  EXPECT_FALSE(MapChannels(document(Json::array({volume})), maps()).resolve(position(), request()));
}

TEST(MapChannels, AmbiguousOverlapsAreRefusedUnlessTriggerIsExplicit) {
  auto large = boundary(9361);
  large["trigger_guid"] = "larger-checkpoint";
  large["trigger_id_wire"] = {"3", "4"};
  large["geometry"]["extent"] = {{"x", 1000}, {"y", 1000}, {"z", 1000}};
  const MapChannels channels(document({large, boundary()}), maps());
  EXPECT_FALSE(channels.resolve(position(), request()));

  auto matching = request();
  matching["trigger_id_x"] = "3";
  matching["trigger_id_y"] = "4";
  const auto selected = channels.resolve(position(), matching);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->map_id, 9361);

  matching["trigger_id_y"] = "9";
  EXPECT_FALSE(channels.resolve(position(), matching));
}

TEST(MapChannels, GatedAndUnknownNestedTargetsStillBlockAmbiguousRequests) {
  auto outer = boundary(9361);
  outer["trigger_guid"] = "outer-checkpoint";
  outer["trigger_id_wire"] = {"3", "4"};
  outer["geometry"]["extent"] = {{"x", 1000}, {"y", 1000}, {"z", 1000}};
  auto rows = maps();
  rows.at(1)["FK_enteranceConditionId"] = "10025";
  const MapChannels gated(document({outer, boundary()}), rows);
  EXPECT_FALSE(gated.resolve(position(), request()));

  auto explicit_gated = request();
  explicit_gated["trigger_id_x"] = "1";
  explicit_gated["trigger_id_y"] = "2";
  EXPECT_FALSE(gated.resolve(position(), explicit_gated));

  auto explicit_outer = request();
  explicit_outer["trigger_id_x"] = "3";
  explicit_outer["trigger_id_y"] = "4";
  const auto eligible = gated.resolve(position(), explicit_outer);
  ASSERT_TRUE(eligible);
  EXPECT_EQ(eligible->map_id, 9361);

  const MapChannels unknown(document({outer, boundary(12345)}), maps());
  EXPECT_FALSE(unknown.resolve(position(), request()));
}

TEST(MapChannels, SameTargetOverlapsAreUnambiguousAndCurrentMapIsNotADestination) {
  auto second = boundary();
  second["trigger_guid"] = "second-checkpoint";
  second["trigger_id_wire"] = {"3", "4"};
  second["geometry"]["extent"] = {{"x", 1000}, {"y", 1000}, {"z", 1000}};
  const MapChannels same_target(document({second, boundary()}), maps());
  const auto destination = same_target.resolve(position(), request());
  ASSERT_TRUE(destination);
  EXPECT_EQ(destination->map_id, 10110);

  second["destination_map_id"] = position().map_id;
  const MapChannels current_map(document({second, boundary()}), maps());
  const auto crossing = current_map.resolve(position(), request());
  ASSERT_TRUE(crossing);
  EXPECT_EQ(crossing->map_id, 10110);
}

TEST(MapChannels, SupportsAuthoredReturnCrossingWithoutSelfLoop) {
  const MapChannels channels(document(Json::array({boundary(9361)})), maps());
  auto returning = position();
  returning.map_id = 10110;
  const auto destination = channels.resolve(returning, request(returning));
  ASSERT_TRUE(destination);
  EXPECT_EQ(destination->map_id, 9361);

  returning.map_id = 9361;
  EXPECT_FALSE(channels.resolve(returning, request(returning)));
}

TEST(MapChannels, RejectsDuplicateDefinitionsAndInvalidGeometry) {
  EXPECT_THROW(MapChannels(document({boundary(), boundary()}), maps()), std::runtime_error);

  auto invalid = boundary();
  invalid["geometry"]["extent"]["x"] = 0;
  EXPECT_THROW(MapChannels(document(Json::array({invalid})), maps()), std::runtime_error);
  invalid = boundary();
  invalid["geometry"]["center"]["x"] = std::numeric_limits<double>::infinity();
  EXPECT_THROW(MapChannels(document(Json::array({invalid})), maps()), std::runtime_error);

  auto rows = maps();
  rows.push_back(rows.front());
  EXPECT_THROW(MapChannels(document(), rows), std::runtime_error);
}

TEST(MapChannels, MissingOptionalExportLeavesFeatureDisabled) {
  EXPECT_FALSE(MapChannels::load({}, maps()));
}

Json spatial_grid() {
  return {{"format_version", 1},
          {"world", "ArcheWorld"},
          {"cell_width_cm", 5100},
          {"grids",
           {{{"map_id", 9361},
             {"min_cell_x", -2},
             {"min_cell_y", 3},
             {"width", 2},
             {"height", 2},
             {"mask", "1001"}}}}};
}

TEST(MapChannels, SpatialFieldCrossingNeedsNoTriggerBoxAndUsesOccupiedCells) {
  const MapChannels channels(document(Json::array()), maps(), spatial_grid());
  const WorldPosition crossing{9012, -76500, 178500, 0, 12};
  const auto destination = channels.resolve(crossing, request(crossing));
  ASSERT_TRUE(destination);
  EXPECT_EQ(destination->map_id, 9361);
  EXPECT_EQ(destination->x_mm, crossing.x_mm);
  EXPECT_EQ(destination->yaw, crossing.yaw);

  auto unused = crossing;
  unused.x_mm += 51000;
  EXPECT_FALSE(channels.resolve(unused, request(unused)));
  auto remote = crossing;
  remote.x_mm += 5001;
  EXPECT_FALSE(channels.resolve(crossing, request(remote)));
  auto same_map = crossing;
  same_map.map_id = 9361;
  EXPECT_FALSE(channels.resolve(same_map, request(same_map)));
}

// A public zone field row: top-level ArcheWorld field with a zone key and an authored area
// (centimetres), like the live 9012/9021 rows.
Json zone_field(int id, const char *zone, int min_x, int min_y, int max_x, int max_y) {
  auto row = map_row(id);
  row["parentMapId"] = "";
  row["FK_zoneKey"] = zone;
  row["area"] = "\"min\":{\"x\":" + std::to_string(min_x) + ",\"y\":" + std::to_string(min_y) +
                ",\"z\":-97800},\"max\":{\"x\":" + std::to_string(max_x) +
                ",\"y\":" + std::to_string(max_y) + ",\"z\":106320}";
  return row;
}

// Areas in centimetres; they overlap between x -265200 and -239700 like the live 9012/9021.
std::vector<Json> region_rows() {
  return {map_row(1), zone_field(9012, "Zone_01", -265200, 224400, 15300, 459000),
          zone_field(9021, "Zone_02", -571200, 91800, -239700, 459000), map_row(10110),
          map_row(9361)};
}

TEST(MapChannels, CrossingOutsideEveryOccupiedCellUsesTheRegionAreaThatHoldsIt) {
  const MapChannels channels(document(Json::array()), region_rows(), spatial_grid());

  // Inside Ruins of Gold's area and outside 9012's: the crossing reaches 9021.
  const WorldPosition into_ruins{9012, -3000000, 3000000, 0, 0};
  const auto destination = channels.resolve(into_ruins, request(into_ruins));
  ASSERT_TRUE(destination);
  EXPECT_EQ(destination->map_id, 9021);

  // Occupied cells still decide first, even inside another region's area.
  const WorldPosition cell{9012, -76500, 178500, 0, 0};
  const auto cell_destination = channels.resolve(cell, request(cell));
  ASSERT_TRUE(cell_destination);
  EXPECT_EQ(cell_destination->map_id, 9361);

  // A position only the current region holds is not a crossing.
  const WorldPosition home{9012, -1000000, 3000000, 0, 0};
  EXPECT_FALSE(channels.resolve(home, request(home)));
}

TEST(MapChannels, TeleportDestinationsUseTheRegionChannelOnlyForOpenWorldMaps) {
  const MapChannels channels(document(Json::array()), region_rows());

  // Main-world map 1 is routed to the region that contains the destination (Romeline's village).
  const WorldPosition ruins_of_gold{1, -3559332, 3287514, -579433, 0};
  EXPECT_EQ(channels.region_map_for(ruins_of_gold), 9021);

  // A public channel teleporting into another region switches to that region's channel.
  auto from_shore = ruins_of_gold;
  from_shore.map_id = 9012;
  EXPECT_EQ(channels.region_map_for(from_shore), 9021);

  // A destination inside the requested channel's own area keeps it.
  const WorldPosition shore{9012, -418599, 3416930, -631885, 0};
  EXPECT_FALSE(channels.region_map_for(shore));
  // In the overlap strip the nearest region centre wins.
  const WorldPosition border{1, -2550000, 3000000, 0, 0};
  EXPECT_EQ(channels.region_map_for(border), 9012);

  // `tp_map <id>` keeps a named public channel, but the whole-world map is still routed.
  EXPECT_FALSE(channels.region_map_for(from_shore, true));
  EXPECT_EQ(channels.region_map_for(ruins_of_gold, true), 9021);

  // Maps that are not public zone fields (instances) keep their identity.
  auto instance = ruins_of_gold;
  instance.map_id = 10110;
  EXPECT_FALSE(channels.region_map_for(instance));
  const WorldPosition nowhere{1, 90000000, 90000000, 0, 0};
  EXPECT_FALSE(channels.region_map_for(nowhere));
}

TEST(MapChannels, SpatialMasksRejectMalformedOrAmbiguousDefinitions) {
  auto spatial = spatial_grid();
  spatial["grids"][0]["mask"] = "100";
  EXPECT_THROW(MapChannels(document(), maps(), spatial), std::runtime_error);
  spatial = spatial_grid();
  spatial["grids"].push_back(spatial["grids"][0]);
  EXPECT_THROW(MapChannels(document(), maps(), spatial), std::runtime_error);
  spatial = spatial_grid();
  spatial["grids"][0]["map_id"] = 12345;
  EXPECT_THROW(MapChannels(document(), maps(), spatial), std::runtime_error);
}

// The live layout: village 9013 (and housing area 20101) nest inside field 9012's cells, and a
// gated point of interest 30103 too. Cells are 5100 cm; the field covers four cells around the
// origin and every nested zone one of them.
std::vector<Json> nested_rows() {
  auto village = map_row(9013);
  village["parentMapId"] = "9012";
  auto point_of_interest = map_row(30103);
  point_of_interest["type"] = "PointOfInterest";
  point_of_interest["parentMapId"] = "9012";
  point_of_interest["FK_enteranceConditionId"] = "10025";
  auto housing = map_row(20101);
  housing["type"] = "HousingArea";
  housing["parentMapId"] = "9012";
  return {map_row(9012), village, point_of_interest, housing};
}

Json nested_grid(int map_id, int depth, int min_cell_x, int min_cell_y, const char *mask,
                 int width = 1, int height = 1) {
  return {{"map_id", map_id}, {"depth", depth},         {"min_cell_x", min_cell_x},
          {"min_cell_y", min_cell_y}, {"width", width}, {"height", height},
          {"mask", mask}};
}

Json nested_spatial() {
  return {{"format_version", 2},
          {"world", "ArcheWorld"},
          {"cell_width_cm", 5100},
          {"grids",
           {nested_grid(9012, 0, 0, 0, "1111", 2, 2), nested_grid(9013, 1, 1, 0, "1"),
            nested_grid(30103, 1, 0, 1, "1"), nested_grid(20101, 1, 1, 1, "1")}}};
}

WorldPosition in_cell(int map_id, int cell_x, int cell_y) {
  return {map_id, cell_x * 51000 + 25000, cell_y * 51000 + 25000, -600000, 12};
}

TEST(MapChannels, NestedZonesAreEnteredFromTheirFieldAndLeftForIt) {
  const MapChannels channels(document(Json::array()), nested_rows(), nested_spatial());

  // Field cell -> village cell: the client asks for the village map.
  const auto into_village = in_cell(9012, 1, 0);
  const auto entered = channels.resolve(into_village, request(into_village));
  ASSERT_TRUE(entered);
  EXPECT_EQ(entered->map_id, 9013);
  EXPECT_EQ(entered->x_mm, into_village.x_mm);

  // Village -> plain field cell, or -> another nested zone: the parent field first.
  const auto out_of_village = in_cell(9013, 0, 0);
  const auto left = channels.resolve(out_of_village, request(out_of_village));
  ASSERT_TRUE(left);
  EXPECT_EQ(left->map_id, 9012);
  const auto village_to_point = in_cell(9013, 0, 1);
  const auto via_parent = channels.resolve(village_to_point, request(village_to_point));
  ASSERT_TRUE(via_parent);
  EXPECT_EQ(via_parent->map_id, 9012);

  // Still inside the village cell: not a crossing.
  const auto staying = in_cell(9013, 1, 0);
  EXPECT_FALSE(channels.resolve(staying, request(staying)));

  // Gated points of interest and housing areas resolve; their conditions are the caller's.
  const auto into_point = in_cell(9012, 0, 1);
  const auto point = channels.resolve(into_point, request(into_point));
  ASSERT_TRUE(point);
  EXPECT_EQ(point->map_id, 30103);
  ASSERT_NE(channels.zone_map(30103), nullptr);
  EXPECT_TRUE(channels.zone_map(30103)->gated);
  EXPECT_FALSE(channels.supports_map(30103));
  const auto into_housing = in_cell(9012, 1, 1);
  const auto housing = channels.resolve(into_housing, request(into_housing));
  ASSERT_TRUE(housing);
  EXPECT_EQ(housing->map_id, 20101);
  EXPECT_EQ(channels.zone_map(20101)->type, "HousingArea");
}

TEST(MapChannels, NestedGridsNeedTheirMapsDepthAndNoSameDepthOverlap) {
  auto spatial = nested_spatial();
  spatial["grids"].push_back(nested_grid(30103, 1, 1, 0, "1"));
  EXPECT_THROW(MapChannels(document(Json::array()), nested_rows(), spatial),
               std::runtime_error);

  spatial = nested_spatial();
  spatial["grids"][1]["depth"] = 0;
  EXPECT_THROW(MapChannels(document(Json::array()), nested_rows(), spatial),
               std::runtime_error);

  spatial = nested_spatial();
  spatial["format_version"] = 3;
  EXPECT_THROW(MapChannels(document(Json::array()), nested_rows(), spatial),
               std::runtime_error);
}

} // namespace
} // namespace chronicles
