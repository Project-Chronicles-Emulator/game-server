#include "world/core/world_sectors.hpp"

#include "data/client_catalog.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr char kArcheWorld[] = "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld";
constexpr int kSharedFieldMap = 9012;
constexpr int kSmallInstanceMap = 10109;
constexpr int kPointOfInterestMap = 30104;

// The bonfire station of the 2026-10-27 local trial: sector (154, 291), spot 1.
constexpr std::uint32_t kBonfireSector = 4767898;
constexpr double kBonfireXmm = -1372165;
constexpr double kBonfireYmm = 3103870;

Json origin_row(const std::string &world, const std::string &area,
                const std::string &sector_width = "5100") {
  return {{"world", world}, {"sectorWidth", sector_width}, {"area", area}};
}

Json map_row(int map_id, const std::string &world, const std::string &area) {
  return {{"id", std::to_string(map_id)}, {"world", world}, {"area", area}};
}

// Catalog rows copied from OriginMap and Map (build 40367e24...).
WorldSectorGrid catalog_grid() {
  const std::vector<Json> origins{
      origin_row(kArcheWorld,
                 R"("min":{"x":-918000,"y":-1173000,"z":-97800},)"
                 R"("max":{"x":918000,"y":1275000,"z":106320})"),
      origin_row("/Game/Maps/ArcheWorld_Tutorial/ArcheWorld_Tutorial.ArcheWorld_Tutorial",
                 R"("min":{"x":-150000,"y":48000,"z":-115000},)"
                 R"("max":{"x":-50000,"y":98000,"z":-15000})")};
  const std::vector<Json> maps{
      map_row(kSharedFieldMap, kArcheWorld,
              R"("min":{"x":-265200,"y":224400,"z":-97800},)"
              R"("max":{"x":15300,"y":459000,"z":106320})"),
      map_row(kSmallInstanceMap, kArcheWorld,
              R"("min":{"x":-96718.906252005923,"y":293966.52170684194,"z":-66624.2857},)"
              R"("max":{"x":-82718.906252005923,"y":305966.52170684194,"z":-56624.2857})"),
      map_row(kPointOfInterestMap, kArcheWorld,
              R"("min":{"x":-61200,"y":285600,"z":-97800},)"
              R"("max":{"x":-30600,"y":316200,"z":106320})"),
      // A map whose world has no OriginMap row has no sector grid.
      map_row(77, "/Game/Maps/Unknown/Unknown.Unknown",
              R"("min":{"x":0,"y":0,"z":0},"max":{"x":1,"y":1,"z":1})")};
  return WorldSectorGrid(origins, maps);
}

std::vector<std::uint32_t> block(std::int32_t first_x, std::int32_t last_x,
                                 std::int32_t first_y, std::int32_t last_y) {
  std::vector<std::uint32_t> sectors;
  for (auto y = first_y; y <= last_y; ++y) {
    for (auto x = first_x; x <= last_x; ++x) {
      sectors.push_back(pack_sector_id({x, y}));
    }
  }
  return sectors;
}

// A position in the middle of an ArcheWorld sector, in millimetres.
std::pair<double, double> sector_center_mm(std::int32_t x, std::int32_t y) {
  return {(-918000.0 + (x - 0.5) * 5100.0) * 10.0, (-1173000.0 + (y - 0.5) * 5100.0) * 10.0};
}

TEST(WorldSectors, SectorIdsPackTwoFourteenBitAxes) {
  EXPECT_EQ(pack_sector_id({154, 291}), kBonfireSector);
  EXPECT_EQ(unpack_sector_id(kBonfireSector), (SectorCoordinate{154, 291}));
  EXPECT_THROW(pack_sector_id({0, 1}), std::invalid_argument);
  EXPECT_THROW(pack_sector_id({1, kSectorAxisCount}), std::invalid_argument);
}

TEST(WorldSectors, PositionsUseTheWorldOriginLikeTheAuthoredSpotSectors) {
  const auto grid = catalog_grid();

  const auto bonfire = grid.sector_at(kSharedFieldMap, kBonfireXmm, kBonfireYmm);
  ASSERT_TRUE(bonfire.has_value());
  EXPECT_EQ(pack_sector_id(*bonfire), kBonfireSector);

  EXPECT_FALSE(grid.sector_at(77, 0, 0).has_value());
  EXPECT_FALSE(grid.sector_at(kSharedFieldMap, 1e12, 0).has_value());
  EXPECT_FALSE(grid.has_map(77));
  EXPECT_EQ(grid.map_count(), 3u);
}

// Official map-9012 entry at (-132715.9 cm, 310813.5 cm): 25 sectors, y outer, x inner.
TEST(WorldSectors, MapEntryWindowMatchesTheOfficialBurstOrder) {
  const auto grid = catalog_grid();
  const auto center = grid.sector_at(kSharedFieldMap, -1327159, 3108135);
  ASSERT_TRUE(center.has_value());
  EXPECT_EQ(*center, (SectorCoordinate{154, 291}));

  EXPECT_EQ(grid.sight_window(kSharedFieldMap, *center), block(152, 156, 289, 293));
}

// Official map-10109 entries announced 16 sectors: x 161-164 and y 287-290.
TEST(WorldSectors, SmallMapsClipTheWindowLikeTheOfficialServer) {
  const auto grid = catalog_grid();
  const auto center = grid.sector_at(kSmallInstanceMap, -919648, 2943051);
  ASSERT_TRUE(center.has_value());
  EXPECT_EQ(*center, (SectorCoordinate{162, 288}));

  EXPECT_EQ(grid.sight_window(kSmallInstanceMap, *center), block(161, 164, 287, 290));
}

// Official map-30104 entry at sector (174, 289) announced 20 sectors ending at x = 175.
TEST(WorldSectors, WindowStopsAtTheSectorHoldingTheAreaMaximum) {
  const auto grid = catalog_grid();
  const auto [x_mm, y_mm] = sector_center_mm(174, 289);
  const auto center = grid.sector_at(kPointOfInterestMap, x_mm, y_mm);
  ASSERT_TRUE(center.has_value());

  EXPECT_EQ(grid.sight_window(kPointOfInterestMap, *center), block(172, 175, 287, 291));
}

TEST(WorldSectors, TrackerAnnouncesOnlyAfterMapLoadAndThenEnteringSectors) {
  const auto grid = catalog_grid();
  SectorSightTracker tracker;
  const auto [x_mm, y_mm] = sector_center_mm(154, 291);

  EXPECT_TRUE(tracker.update(grid, kSharedFieldMap, x_mm, y_mm).empty());

  tracker.arm(kSharedFieldMap);
  EXPECT_EQ(tracker.update(grid, kSharedFieldMap, x_mm, y_mm), block(152, 156, 289, 293));
  EXPECT_TRUE(tracker.update(grid, kSharedFieldMap, x_mm + 1000, y_mm).empty());

  const auto [east_x_mm, east_y_mm] = sector_center_mm(155, 291);
  EXPECT_EQ(tracker.update(grid, kSharedFieldMap, east_x_mm, east_y_mm),
            block(157, 157, 289, 293));

  // The official server announced a sector again each time it re-entered the window.
  EXPECT_EQ(tracker.update(grid, kSharedFieldMap, x_mm, y_mm), block(152, 152, 289, 293));
}

TEST(WorldSectors, MapChangeWaitsForTheNextMapLoad) {
  const auto grid = catalog_grid();
  SectorSightTracker tracker;
  const auto [x_mm, y_mm] = sector_center_mm(162, 288);

  tracker.arm(kSharedFieldMap);
  EXPECT_EQ(tracker.update(grid, kSharedFieldMap, x_mm, y_mm).size(), 25u);

  EXPECT_TRUE(tracker.update(grid, kSmallInstanceMap, x_mm, y_mm).empty());
  EXPECT_TRUE(tracker.update(grid, kSharedFieldMap, x_mm, y_mm).empty());

  tracker.arm(kSmallInstanceMap);
  EXPECT_EQ(tracker.update(grid, kSmallInstanceMap, x_mm, y_mm), block(161, 164, 287, 290));

  // Re-arming the same map (reload after a same-map move) announces the window again.
  tracker.arm(kSmallInstanceMap);
  EXPECT_EQ(tracker.update(grid, kSmallInstanceMap, x_mm, y_mm).size(), 16u);
}

TEST(WorldSectors, InvalidCatalogRowsFailClosed) {
  const auto area = std::string(R"("min":{"x":0,"y":0,"z":0},"max":{"x":1,"y":1,"z":1})");
  EXPECT_THROW(WorldSectorGrid({origin_row("w", area), origin_row("w", area)}, {}),
               std::runtime_error);
  EXPECT_THROW(WorldSectorGrid({origin_row("w", area, "0")}, {}), std::runtime_error);
  EXPECT_THROW(WorldSectorGrid({origin_row("w", area)},
                               {map_row(1, "w",
                                        R"("min":{"x":5,"y":0,"z":0},"max":{"x":1,"y":1,"z":1})")}),
               std::runtime_error);
  EXPECT_THROW(
      WorldSectorGrid({origin_row("w", area)}, {map_row(1, "w", area), map_row(1, "w", area)}),
      std::runtime_error);
}

TEST(WorldSectors, UpdatePacketEncodesSectorAndDepletedGatheringIndices) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  const auto empty = sector_update_packet(kBonfireSector, {});
  EXPECT_EQ(empty.opcode, kSectorUpdateNotification);
  const auto decoded_empty = protocol.decode(protocol.encode(empty.opcode, empty.message));
  ASSERT_EQ(decoded_empty.at("status"), "decoded");
  EXPECT_EQ(decoded_empty.at("message"), (Json{{"sector_id", kBonfireSector}}));

  const auto depleted = sector_update_packet(kBonfireSector, {1, 4});
  const auto decoded = protocol.decode(protocol.encode(depleted.opcode, depleted.message));
  ASSERT_EQ(decoded.at("status"), "decoded");
  EXPECT_EQ(decoded.at("message").at("gathering_deactivated_indices"), (Json{1, 4}));
}

// With the generated catalog: the real OriginMap/Map tables place the bonfire in its sector.
TEST(WorldSectors, RealCatalogPlacesTheBonfireInItsAuthoredSector) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Requires CHRONICLES_CLIENT_CATALOG";
  }
  ClientCatalog catalog(catalog_path);
  const auto grid = WorldSectorGrid::load(catalog);
  ASSERT_TRUE(grid.has_value());

  const auto bonfire = grid->sector_at(kSharedFieldMap, kBonfireXmm, kBonfireYmm);
  ASSERT_TRUE(bonfire.has_value());
  EXPECT_EQ(pack_sector_id(*bonfire), kBonfireSector);
  EXPECT_EQ(grid->sight_window(kSmallInstanceMap, {162, 288}), block(161, 164, 287, 290));
}

} // namespace
} // namespace chronicles
