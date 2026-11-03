#include "world/features/npc/npc_patrol.hpp"

#include "world/features/npc/npc_population.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kSecond = 10000000;

Json route_document() {
  return {{"mode", "circle_forward"},
          {"nodes",
           {{{"position_mm", {{"x", 0}, {"y", 0}, {"z", 0}}},
             {"speed_mm_per_second", 1100.0},
             {"move_detail", "MoveDetail_Walk"},
             {"wait_ms", 0}},
            {{"position_mm", {{"x", 1100}, {"y", 0}, {"z", 0}}},
             {"speed_mm_per_second", 1100.0},
             {"move_detail", "MoveDetail_Walk"},
             {"wait_ms", 1000},
             {"wait_yaw", 90},
             {"stance", 34},
             {"motion",
              {{"game_anim_mapping_id", 2086},
               {"target_anim_stance_mapping_id", 34},
               {"loop_count", 1}}}}}}};
}

NpcPopulation population() {
  const Json document{{"format_version", 1},
                      {"map_id", 9012},
                      {"placements",
                       {{{"npc_cms_id", 1001405},
                         {"position_mm", {{"x", 0}, {"y", 0}, {"z", 0}}},
                         {"yaw_degrees", 0},
                         {"anim_stance_key", 0},
                         {"patrol", route_document()}}}}};
  return NpcPopulation(NpcPlacementSet::from_json(document, 4), 4);
}

TEST(NpcPatrol, WalksWaitsAndReturnsContinuouslyAtTheLoopBoundary) {
  const auto route = NpcPatrolRoute::from_json(route_document());
  EXPECT_EQ(route.sample(0).position.x_mm, 0);
  EXPECT_EQ(route.sample(kSecond / 2).position.x_mm, 550);
  const auto waiting = route.sample(kSecond);
  EXPECT_FALSE(waiting.moving);
  EXPECT_EQ(waiting.position.x_mm, 1100);
  EXPECT_EQ(waiting.yaw, 90);
  EXPECT_EQ(waiting.stance, 34);
  EXPECT_EQ(waiting.remaining_ticks, kSecond);
  EXPECT_EQ(route.sample(2 * kSecond).position.x_mm, 1100);
  EXPECT_EQ(route.sample(5 * kSecond / 2).position.x_mm, 550);
  EXPECT_EQ(route.sample(3 * kSecond).position.x_mm, 0);
  EXPECT_GT(route.sample(3 * kSecond).phase_id, waiting.phase_id);
}

TEST(NpcPatrol, LateJoinersAndExistingClientsUseTheSameTimeline) {
  const auto npcs = population();
  UnitRegistry first_registry(4);
  UnitRegistry second_registry(4);
  npcs.announce_to(first_registry, "shared");
  npcs.announce_to(second_registry, "shared", kSecond / 2);
  const auto adds = npcs.unit_added_packets(kSecond / 2);
  EXPECT_EQ(adds[0].message.at("nub").at("pos").at("millimeter_x"), 550);
  EXPECT_EQ(adds[0].message.at("nub").at("npc").at("stats").at("84"), 1000);
  EXPECT_EQ(second_registry.find(npcs.units()[0].unit_id)->position.x_mm, 550);
  NpcPatrolSession first(npcs, 1000);
  NpcPatrolSession second(npcs, 1000);
  ASSERT_EQ(first.start(1000, first_registry, "shared").size(), 1);
  EXPECT_TRUE(first.start(2000, first_registry, "shared").empty());
  const auto continued = first.advance(1000 + kSecond / 2, first_registry, "shared");
  const auto joined = second.start(1000 + kSecond / 2, second_registry, "shared");
  ASSERT_EQ(continued.size(), 1);
  ASSERT_EQ(joined.size(), 1);
  EXPECT_EQ(continued[0].message, joined[0].message);
  EXPECT_TRUE(first.advance(1000 + kSecond / 2, first_registry, "shared").empty());
}

TEST(NpcPatrol, StopsPlaysTheAuthoredActivityOnceAndRestoresTheWalkingStance) {
  const auto npcs = population();
  UnitRegistry registry(4);
  npcs.announce_to(registry, "shared");
  NpcPatrolSession session(npcs, 0);
  session.start(0, registry, "shared");
  const auto stopped = session.advance(kSecond, registry, "shared");
  ASSERT_EQ(stopped.size(), 2);
  EXPECT_EQ(stopped[0].opcode, 177);
  EXPECT_EQ(stopped[0].message.at("detail"), "MoveDetail_Stop");
  EXPECT_FALSE(stopped[0].message.contains("pred_pos"));
  EXPECT_EQ(stopped[1].opcode, 69);
  EXPECT_EQ(stopped[1].message.at("x5_motion_data").at("game_anim_mapping_id"), 2086);
  EXPECT_TRUE(session.advance(3 * kSecond / 2, registry, "shared").empty());
  const auto walking = session.advance(2 * kSecond, registry, "shared");
  ASSERT_EQ(walking.size(), 2);
  EXPECT_EQ(walking[0].opcode, 69);
  EXPECT_EQ(walking[0].message.at("anim_stance_change_data").at("target_anim_stance_mapping_id"),
            0);
  EXPECT_EQ(walking[1].message.at("detail"), "MoveDetail_Walk");
  EXPECT_EQ(walking[1].message.at("pred_pos").at("millimeter_x"), 550);
  EXPECT_EQ(walking[1].message.at("pred_milli"), 500);
  const auto corner = session.advance(3 * kSecond, registry, "shared");
  ASSERT_EQ(corner.size(), 1);
  EXPECT_EQ(corner[0].opcode, 177);
}

TEST(NpcPatrol, SynchronizesInteractionPositionsAndRefusesChangedOwnership) {
  const auto npcs = population();
  UnitRegistry registry(4);
  npcs.announce_to(registry, "shared");
  NpcPatrolSession session(npcs, 0);
  session.start(0, registry, "shared");
  session.synchronize(kSecond / 4, registry, "shared");
  const auto id = npcs.units()[0].unit_id;
  EXPECT_EQ(registry.find(id)->position.x_mm, 275);
  EXPECT_TRUE(session.advance(kSecond / 4, registry, "shared").empty());
  EXPECT_FALSE(registry.update_position(id, 8000, "shared", {5, 6, 7}));
  EXPECT_FALSE(registry.update_position(id, 9012, "other", {5, 6, 7}));
  EXPECT_EQ(registry.find(id)->position.x_mm, 275);
  registry.despawn(id);
  EXPECT_THROW(session.synchronize(kSecond, registry, "shared"), std::runtime_error);
  EXPECT_EQ(registry.size(), 0);
}

TEST(NpcPatrol, DelayedTimersSkipOldCyclesWithoutAnUnboundedCatchupQueue) {
  const auto npcs = population();
  UnitRegistry registry(4);
  npcs.announce_to(registry, "shared");
  NpcPatrolSession session(npcs, 0);
  session.start(0, registry, "shared");
  const auto late = session.advance(3600 * kSecond, registry, "shared");
  EXPECT_LE(late.size(), 2);
  ASSERT_TRUE(session.next_action_tick());
  EXPECT_GT(*session.next_action_tick(), 3600 * kSecond);
}

TEST(NpcPatrol, AnotherActorsShortLegDoesNotIncreaseEveryActorsNotificationRate) {
  const auto first = population();
  auto set = NpcPlacementSet{};
  set.map_id = first.map_id();
  set.placements.push_back(first.units()[0].placement);
  auto short_route = route_document();
  short_route["nodes"][1]["position_mm"]["x"] = 110;
  short_route["nodes"][1]["wait_ms"] = 0;
  short_route["nodes"][1].erase("motion");
  short_route["nodes"][1].erase("stance");
  auto other = first.units()[0].placement;
  other.patrol = std::make_shared<const NpcPatrolRoute>(NpcPatrolRoute::from_json(short_route));
  set.placements.push_back(std::move(other));
  const NpcPopulation npcs(std::move(set), 4);
  UnitRegistry registry(4);
  npcs.announce_to(registry, "shared");
  NpcPatrolSession session(npcs, 0);
  EXPECT_EQ(session.start(0, registry, "shared").size(), 2);
  EXPECT_EQ(session.advance(kSecond / 10, registry, "shared").size(), 1);
}

TEST(NpcPatrol, EncodesMovementAndActivitiesWithNanosecondWireTicks) {
  const auto npcs = population();
  UnitRegistry registry(4);
  npcs.announce_to(registry, "shared");
  NpcPatrolSession session(npcs, 1000);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const std::int64_t ticks : {1000LL, 1000LL + kSecond, 1000LL + 2 * kSecond}) {
    const auto packets = ticks == 1000 ? session.start(ticks, registry, "shared")
                                       : session.advance(ticks, registry, "shared");
    for (const auto &packet : packets) {
      EXPECT_NO_THROW(protocol.encode(packet.opcode, packet.message));
      EXPECT_EQ(packet.message.at("tick"), std::to_string(ticks * 100));
    }
  }
}

TEST(NpcPatrol, RejectsInvalidRoutesBeforeStartingSimulation) {
  const auto rejects = [](const auto &mutate) {
    auto document = route_document();
    mutate(document);
    EXPECT_THROW(NpcPatrolRoute::from_json(document), std::exception);
  };
  rejects([](Json &d) { d["mode"] = "random"; });
  rejects([](Json &d) { d["nodes"] = Json::array(); });
  rejects([](Json &d) { d["nodes"][0]["position_mm"]["x"] = 2147483648LL; });
  rejects([](Json &d) { d["nodes"][0]["speed_mm_per_second"] = 0; });
  rejects([](Json &d) { d["nodes"][0]["speed_mm_per_second"] = true; });
  rejects([](Json &d) { d["nodes"][0]["wait_ms"] = -1; });
  rejects([](Json &d) { d["nodes"][0]["move_detail"] = "MoveDetail_Swim"; });
  rejects([](Json &d) { d["nodes"][1]["motion"]["loop_count"] = -2; });
  rejects([](Json &d) { d["nodes"][1]["wait_ms"] = 0; });
  rejects([](Json &d) { d["nodes"][1]["position_mm"] = d["nodes"][0]["position_mm"]; });
  rejects([](Json &d) {
    d["nodes"][0]["position_mm"]["x"] = std::numeric_limits<int>::min();
    d["nodes"][1]["position_mm"]["x"] = std::numeric_limits<int>::max();
    d["nodes"][0]["speed_mm_per_second"] = 1;
  });
  const auto route = NpcPatrolRoute::from_json(route_document());
  EXPECT_THROW(route.sample(-1), std::runtime_error);
}

TEST(NpcPatrol, RealMapPreservesStationaryNpcsAndSamplesEveryExportedRoute) {
  const auto *path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!path) {
    GTEST_SKIP() << "Set the real generated map placement path";
  }
  const auto npcs = load_npc_population_file(path, kMaxPopulatedNpcUnits);
  std::size_t patrols = 0;
  for (const auto &unit : npcs->units()) {
    if (!unit.placement.patrol) {
      continue;
    }
    ++patrols;
    for (const std::int64_t elapsed : {0LL, kSecond, 60 * kSecond, 3600 * kSecond}) {
      EXPECT_NO_THROW(unit.placement.patrol->sample(elapsed));
    }
  }
  // Static-layer civilians inside map 9012's area add two patrols and one stationary NPC.
  EXPECT_EQ(patrols, 118);
  EXPECT_EQ(npcs->units().size() - patrols, 146);
}

} // namespace
} // namespace chronicles
