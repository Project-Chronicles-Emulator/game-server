#include "world/features/npc/map_static_npcs.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr int kHousingMap = 20101;
constexpr std::uint64_t kArchitect =
    (static_cast<std::uint64_t>(172 | (297 << 14)) << 32) | (3ULL << 12) | 7;

Json profile() {
  return {{"format_version", 1},
          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
          {"targets", Json::array({{{"map_id", kHousingMap},
                                    {"unit_id", std::to_string(kArchitect)},
                                    {"npc_cms_id", 1001641},
                                    {"position_mm", {-417526, 3390365, -631063}}}})}};
}

TEST(MapStaticNpcs, ResolvesOnlyTheAuthoredMapAndExactStaticIdentity) {
  const MapStaticNpcs catalog(profile());
  const auto *npc = catalog.find(kHousingMap, kArchitect);
  ASSERT_NE(npc, nullptr);
  EXPECT_EQ(npc->npc_template_id, 1001641);
  EXPECT_EQ(npc->position.x_mm, -417526);
  EXPECT_EQ(npc->position.z_mm, -631063);
  EXPECT_EQ(catalog.find(9012, kArchitect), nullptr);
  EXPECT_EQ(catalog.find(kHousingMap, kArchitect + (1 << 12)), nullptr);
  EXPECT_EQ(catalog.find(kHousingMap, (kArchitect & ~0xFULL) | 2), nullptr);
}

TEST(MapStaticNpcs, ExportedHousingMapResolvesStationArchitectGyrenn) {
  const auto *root = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!root) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog";
  }
  const auto catalog = MapStaticNpcs::load(root);
  ASSERT_TRUE(catalog);
  const auto *npc = catalog->find(kHousingMap, kArchitect);
  ASSERT_NE(npc, nullptr);
  EXPECT_EQ(npc->npc_template_id, 1001641);
}

TEST(MapStaticNpcs, RejectsInvalidProfileIdentityCoordinatesAndDuplicateEntries) {
  auto invalid = profile();
  invalid["targets"][0]["unit_id"] = std::to_string((kArchitect & ~0xFULL) | 2);
  EXPECT_THROW(MapStaticNpcs{invalid}, std::runtime_error);

  invalid = profile();
  invalid["targets"][0]["position_mm"][0] = 1ULL << 40;
  EXPECT_THROW(MapStaticNpcs{invalid}, std::runtime_error);

  invalid = profile();
  invalid["targets"].push_back(invalid["targets"][0]);
  EXPECT_THROW(MapStaticNpcs{invalid}, std::runtime_error);
}

} // namespace
} // namespace chronicles
