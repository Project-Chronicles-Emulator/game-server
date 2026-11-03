#include "world/features/npc/npc_sight.hpp"

#include "world/features/npc/npc_population.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kBound = 16;
constexpr int kMapId = 9012;

Json placement_json(int npc_cms_id, int x_mm, const char *guid_x) {
  return {{"spawner_guid", "00000000-0000-0000-0000-000000000000"},
          {"spawner_guid_wire", {guid_x, "1"}},
          {"npc_cms_id", npc_cms_id},
          {"behavior", "Civilian"},
          {"faction", "TownNpc"},
          {"interaction_group_id", 0},
          {"interactions", Json::array()},
          {"position_mm", {{"x", x_mm}, {"y", 0}, {"z", 0}}},
          {"yaw_degrees", 0.0},
          {"active", true}};
}

// A near unit at 10 m and a far unit at 400 m from the origin.
NpcPopulation make_population() {
  const Json document{{"format_version", 1},
                      {"map_id", kMapId},
                      {"placements",
                       {placement_json(1001121, 10000, "1"),
                        placement_json(1001748, 400000, "2")}}};
  return NpcPopulation(NpcPlacementSet::from_json(document, kBound), kBound);
}

std::vector<std::string> added_ids(const std::vector<GameplayPacket> &packets) {
  std::vector<std::string> ids;
  for (const auto &packet : packets) {
    if (packet.opcode == kUnitAddedOpcode) {
      ids.push_back(packet.message.at("unit_id").get<std::string>());
    }
  }
  return ids;
}

std::vector<std::string> removed_ids(const std::vector<GameplayPacket> &packets) {
  std::vector<std::string> ids;
  for (const auto &packet : packets) {
    if (packet.opcode == kNpcSightUnitRemovedOpcode) {
      for (const auto &id : packet.message.at("unit_ids")) {
        ids.push_back(id.get<std::string>());
      }
    }
  }
  return ids;
}

std::string unit_id_of(const NpcPopulation &population, int npc_cms_id) {
  for (const auto &unit : population.units()) {
    if (unit.placement.npc_cms_id == npc_cms_id) {
      return std::to_string(unit.unit_id);
    }
  }
  throw std::logic_error("unit not in population");
}

TEST(NpcSight, EntryAnnouncesOnlyUnitsInsideTheEnterRadius) {
  const auto population = make_population();
  NpcSightTracker sight;

  const auto packets = sight.update(population, {0, 0, 0}, 0);

  EXPECT_EQ(added_ids(packets), std::vector<std::string>{unit_id_of(population, 1001121)});
  EXPECT_TRUE(removed_ids(packets).empty());
  EXPECT_EQ(sight.visible_count(), 1u);
}

TEST(NpcSight, WalkingTowardADistantUnitAddsItAndWalkingAwayRemovesIt) {
  const auto population = make_population();
  NpcSightTracker sight;
  sight.update(population, {0, 0, 0}, 0);
  const auto far_id = unit_id_of(population, 1001748);
  const auto near_id = unit_id_of(population, 1001121);

  const auto arrived = sight.update(population, {400000 - kNpcSightEnterRadiusMm, 0, 0}, 0);
  EXPECT_EQ(added_ids(arrived), std::vector<std::string>{far_id});
  EXPECT_EQ(removed_ids(arrived), std::vector<std::string>{near_id});

  const auto unchanged = sight.update(population, {400000 - kNpcSightEnterRadiusMm, 0, 0}, 0);
  EXPECT_TRUE(unchanged.empty());

  const auto returned = sight.update(population, {0, 0, 0}, 0);
  EXPECT_EQ(added_ids(returned), std::vector<std::string>{near_id});
  EXPECT_EQ(removed_ids(returned), std::vector<std::string>{far_id});
}

TEST(NpcSight, AKnownUnitStaysUntilItLeavesTheWiderLeaveRadius) {
  const auto population = make_population();
  NpcSightTracker sight;
  sight.update(population, {10000 + kNpcSightEnterRadiusMm, 0, 0}, 0);
  ASSERT_EQ(sight.visible_count(), 1u);

  // Between the two radii: a known unit is kept, an unknown one would not be added.
  EXPECT_TRUE(sight.update(population, {10000 + kNpcSightLeaveRadiusMm, 0, 0}, 0).empty());
  EXPECT_EQ(sight.visible_count(), 1u);

  const auto left = sight.update(population, {10000 + kNpcSightLeaveRadiusMm + 1, 0, 0}, 0);
  EXPECT_EQ(removed_ids(left), std::vector<std::string>{unit_id_of(population, 1001121)});
  EXPECT_EQ(sight.visible_count(), 0u);
}

TEST(NpcSight, HeightDifferenceDoesNotHideAUnit) {
  const auto population = make_population();
  NpcSightTracker sight;

  const auto packets = sight.update(population, {0, 0, 500000}, 0);

  EXPECT_EQ(added_ids(packets).size(), 1u);
}

TEST(NpcSight, ExtremeDecodedCoordinatesCannotWrapIntoSight) {
  constexpr int minimum = std::numeric_limits<int>::min();
  constexpr int maximum = std::numeric_limits<int>::max();
  const Json document{{"format_version", 1},
                      {"map_id", kMapId},
                      {"placements", {placement_json(1001121, maximum, "1")}}};
  const NpcPopulation population(NpcPlacementSet::from_json(document, kBound), kBound);
  NpcSightTracker sight;

  EXPECT_TRUE(sight.update(population, {minimum, 0, 0}, 0).empty());
  EXPECT_EQ(sight.visible_count(), 0u);
  EXPECT_EQ(added_ids(sight.update(population, {maximum, 0, 0}, 0)).size(), 1u);
  EXPECT_EQ(removed_ids(sight.update(population, {minimum, 0, 0}, 0)).size(), 1u);
  EXPECT_EQ(sight.visible_count(), 0u);
}

TEST(NpcSight, VisibleOnlyDropsUpdatesForUnknownServerNpcs) {
  const auto population = make_population();
  NpcSightTracker sight;
  sight.update(population, {0, 0, 0}, 0);

  std::vector<GameplayPacket> packets{
      {177, {{"unit_id", unit_id_of(population, 1001121)}}},
      {177, {{"unit_id", unit_id_of(population, 1001748)}}},
      {177, {{"unit_id", "4097"}}},
      {53, {{"skill_instance_id", 1}}}};

  const auto kept = sight.visible_only(std::move(packets));

  ASSERT_EQ(kept.size(), 3u);
  EXPECT_EQ(kept[0].message.at("unit_id"), unit_id_of(population, 1001121));
  EXPECT_EQ(kept[1].message.at("unit_id"), "4097");
  EXPECT_EQ(kept[2].opcode, 53);
}

TEST(NpcSight, ClearForgetsEveryUnitSoTheNextUpdateAnnouncesThemAgain) {
  const auto population = make_population();
  NpcSightTracker sight;
  sight.update(population, {0, 0, 0}, 0);

  sight.clear();

  EXPECT_EQ(sight.visible_count(), 0u);
  EXPECT_EQ(added_ids(sight.update(population, {0, 0, 0}, 0)).size(), 1u);
}

} // namespace
} // namespace chronicles
