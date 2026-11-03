#include "world/features/combat/revival_spawns.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

using namespace chronicles;

namespace {

class TemporaryRawTree {
public:
  TemporaryRawTree() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("chronicles-revival-spawns-test-" + std::to_string(unique));
    if (!std::filesystem::create_directory(root)) {
      throw std::runtime_error("Cannot create revival test directory");
    }
    std::filesystem::create_directory(catalog_root());
  }

  ~TemporaryRawTree() {
    std::error_code ignored_error;
    std::filesystem::remove_all(root, ignored_error);
  }

  std::filesystem::path catalog_root() const {
    return root / "catalog";
  }

  std::filesystem::path maps_root() const {
    return root / "raw" / "X5" / "Content" / "Data" / "BuiltData" / "Maps";
  }

  void spot(const std::filesystem::path &relative, const std::string &contents) const {
    const auto path = maps_root() / relative / "DataLayers" / "Runtime" /
                      "test" / "spot_object.json";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << contents;
    if (!output) {
      throw std::runtime_error("Cannot write revival test spot file");
    }
  }

  std::filesystem::path root;
};

nlohmann::json map_row(int id, const std::string &world,
                       int min_x = -1000, int max_x = 1000) {
  const std::string area = "\"min\":{\"x\":" + std::to_string(min_x) +
                           ",\"y\":-1000,\"z\":-1000},\"max\":{\"x\":" +
                           std::to_string(max_x) + ",\"y\":1000,\"z\":1000}";
  return {{"id", std::to_string(id)}, {"world", world}, {"area", area}};
}

std::string spot_document(const nlohmann::json &objects) {
  nlohmann::json document;
  document["sectors"] = nlohmann::json::array();
  document["sectors"].push_back({{"spotObjects", objects}});
  return document.dump();
}

nlohmann::json shrine_object(const std::string &kind, double x, double y,
                             double z, double yaw) {
  const nlohmann::json point = {{"x", x}, {"y", y}, {"z", z}, {"yaw", yaw}};
  nlohmann::json spawn;
  spawn["spawnVolume"]["spawnVolumeSphere"]["spawnPosition"] = point;

  nlohmann::json object;
  object["types"] = nlohmann::json::array();
  if (kind == "idol") {
    object["types"].push_back({{"adventureObject", {{"nuiIdol", spawn}}}});
  } else {
    object["types"].push_back({{"nuiFlameObject", spawn}});
  }
  return object;
}

TEST(RevivalSpawns, SelectsNearestSameMapAndRequestedShrineType) {
  TemporaryRawTree tree;
  const nlohmann::json spots = nlohmann::json::array({
      shrine_object("idol", 10.49, 0, -5, 90),
      shrine_object("idol", 80, 0, -5, 180),
      shrine_object("flame", 11, 0, -5, -30),
      shrine_object("idol", 500, 0, -5, 0)});
  tree.spot("ArcheWorld", spot_document(spots));
  tree.spot("ArcheWorld_Tutorial", spot_document(nlohmann::json::array({
                                      shrine_object("flame", 0, 0, 0, 45)})));
  const auto rows = std::vector<nlohmann::json>{
      map_row(9012, "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld", -100, 100),
      map_row(10117, "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld", 400, 600),
      map_row(8000, "/Game/Maps/ArcheWorld_Tutorial/ArcheWorld_Tutorial.ArcheWorld_Tutorial")};
  const RevivalSpawns index(tree.catalog_root(), rows);

  const auto idol = index.nearest({9012, 100, 0, -50, 0}, false);
  ASSERT_TRUE(idol);
  EXPECT_EQ(idol->map_id, 9012);
  EXPECT_EQ(idol->x_mm, 105);
  EXPECT_EQ(idol->z_mm, -50);
  EXPECT_FLOAT_EQ(idol->yaw, 90);

  const auto flame = index.nearest({9012, 100, 0, -50, 0}, true);
  ASSERT_TRUE(flame);
  EXPECT_EQ(flame->x_mm, 110);
  EXPECT_FLOAT_EQ(flame->yaw, -30);

  const auto isolated_map = index.nearest({10117, 105, 0, -50, 0}, false);
  ASSERT_TRUE(isolated_map);
  EXPECT_EQ(isolated_map->map_id, 10117);
  EXPECT_EQ(isolated_map->x_mm, 5000);
  EXPECT_FALSE(index.nearest({10117, 105, 0, 0, 0}, true));
  EXPECT_FALSE(index.nearest({8000, 0, 0, 0, 0}, false));
  EXPECT_TRUE(index.nearest({8000, 0, 0, 0, 0}, true));
  EXPECT_FALSE(index.nearest({9999, 0, 0, 0, 0}, false));
}

TEST(RevivalSpawns, KeepsDungeonAssetsSeparateWithinOneMapDirectory) {
  TemporaryRawTree tree;
  tree.spot("MazeDungeons/Dungeon_A", spot_document(nlohmann::json::array({
                                         shrine_object("idol", 10, 0, 0, 0)})));
  tree.spot("MazeDungeons/Dungeon_B", spot_document(nlohmann::json::array({
                                         shrine_object("flame", 10, 0, 0, 0)})));
  const auto rows = std::vector<nlohmann::json>{
      map_row(101, "/Game/Maps/MazeDungeons/Dungeon_A.Dungeon_A"),
      map_row(102, "/Game/Maps/MazeDungeons/Dungeon_B.Dungeon_B")};
  const RevivalSpawns index(tree.catalog_root(), rows);

  EXPECT_TRUE(index.nearest({101, 0, 0, 0, 0}, false));
  EXPECT_FALSE(index.nearest({101, 0, 0, 0, 0}, true));
  EXPECT_FALSE(index.nearest({102, 0, 0, 0, 0}, false));
  EXPECT_TRUE(index.nearest({102, 0, 0, 0, 0}, true));
}

TEST(RevivalSpawns, FailsStartupWithoutRawFilesOrWithMalformedData) {
  TemporaryRawTree tree;
  const auto rows = std::vector<nlohmann::json>{
      map_row(8000, "/Game/Maps/ArcheWorld_Tutorial/ArcheWorld_Tutorial.ArcheWorld_Tutorial")};
  EXPECT_THROW(RevivalSpawns(tree.catalog_root(), rows), std::runtime_error);

  tree.spot("ArcheWorld_Tutorial", "{invalid json");
  EXPECT_THROW(RevivalSpawns(tree.catalog_root(), rows), std::runtime_error);

  tree.spot("ArcheWorld_Tutorial", spot_document(nlohmann::json::array()));
  EXPECT_THROW(RevivalSpawns(tree.catalog_root(), rows), std::runtime_error);

  nlohmann::json missing_position;
  missing_position["types"] = nlohmann::json::array();
  missing_position["types"].push_back(
      {{"adventureObject", {{"nuiIdol", nlohmann::json::object()}}}});
  tree.spot("ArcheWorld_Tutorial",
            spot_document(nlohmann::json::array({missing_position})));
  EXPECT_THROW(RevivalSpawns(tree.catalog_root(), rows), std::runtime_error);

  tree.spot("ArcheWorld_Tutorial", spot_document(nlohmann::json::array({
                                      shrine_object("idol", 214748365, 0, 0, 0)})));
  EXPECT_THROW(RevivalSpawns(tree.catalog_root(), rows), std::runtime_error);
}

} // namespace
