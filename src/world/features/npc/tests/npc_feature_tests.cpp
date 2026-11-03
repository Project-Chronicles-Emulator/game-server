#include "world/features/npc/npc_feature.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace chronicles::npc {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kEpoch = 1000;
constexpr std::int64_t kSecond = 10000000;

std::shared_ptr<const NpcPopulation> population(bool patrol = false) {
  Json placement{
      {"npc_cms_id", 1001405}, {"position_mm", {{"x", 0}, {"y", 0}, {"z", 0}}}, {"yaw_degrees", 0}};
  if (patrol) {
    placement["patrol"] = {{"mode", "circle_forward"},
                           {"nodes",
                            {{{"position_mm", {{"x", 0}, {"y", 0}, {"z", 0}}},
                              {"speed_mm_per_second", 1100.0},
                              {"move_detail", "MoveDetail_Walk"},
                              {"wait_ms", 0}},
                             {{"position_mm", {{"x", 1100}, {"y", 0}, {"z", 0}}},
                              {"speed_mm_per_second", 1100.0},
                              {"move_detail", "MoveDetail_Walk"},
                              {"wait_ms", 1000},
                              {"wait_yaw", 90}}}}};
  }
  const Json document{{"format_version", 1}, {"map_id", 9012}, {"placements", {placement}}};
  return std::make_shared<NpcPopulation>(NpcPlacementSet::from_json(document, 4), 4);
}

class NpcContext final : public NpcSessionContext {
public:
  UnitRegistry &npc_registry() override {
    return registry;
  }

  const UnitRegistry &npc_registry() const override {
    return registry;
  }

  int npc_session_map_id() const override {
    return map_id;
  }

  std::string npc_instance_key() const override {
    return instance;
  }

  bool npc_has_tutorial() const override {
    return tutorial;
  }

  bool npc_authenticated() const override {
    return authenticated;
  }

  std::int64_t npc_monotonic_ticks() const override {
    ++clock_reads;
    return ticks;
  }

  bool npc_player_alive() const override {
    return alive;
  }

  std::string npc_reaction_scope() const override {
    return "shared:9012:1";
  }

  void npc_deliver_observers(const std::vector<GameplayPacket> &packets) override {
    observer_packets.insert(observer_packets.end(), packets.begin(), packets.end());
  }

  bool alive{true};
  std::vector<GameplayPacket> observer_packets;
  UnitRegistry registry{8};
  std::string instance{"17"};
  int map_id{9012};
  std::int64_t ticks{kEpoch};
  mutable unsigned clock_reads{};
  bool tutorial{};
  bool authenticated{true};
};

TEST(NpcFeature, ThreatRequiresNearbyOwnedTargetAndPausesItsPatrolForEveryViewer) {
  const Json profile{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"source_executable_dump_sha256",
       "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6"},
      {"range_mm", 5000},
      {"npcs", {{{"npc_cms_id", 1001405}, {"game_anim_mapping_id", 1939}, {"duration_ms", 6500}}}}};
  const NpcFeature feature(population(true), kEpoch, NpcReactionCatalog(profile));
  NpcContext context;
  NpcPlayerSession session(feature, context);
  session.set_population_map(9012);
  session.announce_population();
  const auto id = feature.population()->units().front().unit_id;
  const auto low_id = static_cast<std::uint32_t>(id);
  const auto send = [&](int x) {
    FrameContext frame(kThreatenedNpcRequest,
                       WorldGameplayRequest{
                           kThreatenedNpcRequest, 1, {{"target_npc_unit_ids", {low_id, low_id}}}});
    frame.gameplay_position = WorldPosition{9012, x, 0, 0, 0};
    EXPECT_TRUE(session.handle_threat(frame));
    return frame.session_packets;
  };
  EXPECT_TRUE(send(5001).empty());
  context.alive = false;
  EXPECT_TRUE(send(1000).empty());
  context.alive = true;
  context.registry.announce({id + (1ULL << 32), 1001405, 9012, context.instance, {0, 0, 0}});
  EXPECT_TRUE(send(1000).empty());
  context.registry.despawn(id + (1ULL << 32));

  const auto reaction = send(1000);
  ASSERT_EQ(reaction.size(), 1U);
  EXPECT_EQ(reaction.front().opcode, 69);
  EXPECT_EQ(reaction.front().message.at("x5_motion_data").at("game_anim_mapping_id"), 1939);
  EXPECT_EQ(context.observer_packets.size(), 1U);
  EXPECT_TRUE(send(1000).empty());

  context.ticks += 3 * kSecond;
  session.observe_patrol_position(kThreatenedNpcRequest);
  EXPECT_EQ(context.registry.find(id)->position.x_mm, 0);
  NpcContext viewer;
  viewer.instance = "other-player";
  viewer.ticks = context.ticks;
  NpcPlayerSession other(feature, viewer);
  other.set_population_map(9012);
  other.announce_population();
  const auto entry = other.refresh_npc_sight({9012, 0, 0, 0, 0});
  ASSERT_EQ(entry.size(), 2U);
  EXPECT_EQ(entry[0].message.at("nub").at("pos").at("millimeter_x"), 0);
  EXPECT_EQ(entry[1].opcode, 69);
  EXPECT_EQ(entry[1].message.at("tick"), reaction.front().message.at("tick"));

  context.ticks = kEpoch + 7 * kSecond;
  session.observe_patrol_position(kThreatenedNpcRequest);
  EXPECT_EQ(context.registry.find(id)->position.x_mm, 550);
  EXPECT_EQ(feature.reaction_timeline()
                .sample(context.npc_reaction_scope(), id, 7 * kSecond)
                .route_elapsed,
            kSecond / 2);
}

TEST(NpcFeature, ReactionProfileFromAnotherClientDumpIsRefused) {
  const Json profile{{"format_version", 1},
                     {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
                     {"source_executable_dump_sha256", std::string(64, '0')},
                     {"range_mm", 5000},
                     {"npcs", Json::array()}};
  EXPECT_THROW(NpcReactionCatalog{profile}, std::runtime_error);
}

TEST(NpcFeature, ReactionTimelineSeparatesChannelsAndCannotBeExtendedByDuplicates) {
  NpcReactionTimeline timeline;
  const ThreatReaction reaction{1939, 65000000};
  ASSERT_TRUE(timeline.begin("channel-one", 1, 100, reaction, 90, true));
  EXPECT_FALSE(timeline.begin("channel-one", 1, 200, reaction, 0, true));
  EXPECT_EQ(timeline.sample("channel-one", 1, 1000).route_elapsed, 100);
  EXPECT_EQ(timeline.sample("channel-two", 1, 1000).route_elapsed, 1000);
  EXPECT_FALSE(timeline.sample("channel-one", 1, 65000100).active);
  ASSERT_TRUE(timeline.begin("channel-one", 1, 65000200, reaction, 45, true));
  EXPECT_EQ(timeline.sample("channel-one", 1, 65000300).route_elapsed, 200);
}

TEST(NpcFeature, ReactionTimelineStaysBoundedWithoutLosingSharedRouteOffsets) {
  NpcReactionTimeline timeline;
  const ThreatReaction reaction{1939, 1000};
  constexpr auto kCapacity = NpcReactionTimeline::kMaximumActors;

  // One shared patrol keeps its pause; the rest are finished stationary or private reactions.
  ASSERT_TRUE(timeline.begin("shared:9012:1", 1, 0, reaction, 0, true));
  for (std::uint64_t unit = 2; unit <= kCapacity / 2; ++unit) {
    ASSERT_TRUE(timeline.begin("shared:9012:1", unit, 0, reaction, 0, false));
  }
  for (std::uint64_t unit = kCapacity / 2 + 1; unit <= kCapacity; ++unit) {
    ASSERT_TRUE(timeline.begin("private:session-a", unit, 0, reaction, 0, true));
  }
  ASSERT_EQ(timeline.size(), kCapacity);

  // While every state is still animating, nothing may be discarded.
  EXPECT_FALSE(timeline.begin("shared:9012:1", kCapacity + 1, 500, reaction, 0, false));
  EXPECT_EQ(timeline.size(), kCapacity);

  // After they finish, stationary states are dropped first; the patrol offset survives.
  ASSERT_TRUE(timeline.begin("shared:9012:1", kCapacity + 1, 2000, reaction, 0, false));
  EXPECT_EQ(timeline.size(), kCapacity / 2 + 2);
  EXPECT_EQ(timeline.sample("shared:9012:1", 1, 5000).route_elapsed, 4000);
}

TEST(NpcFeature, FullReactionTimelineDiscardsFinishedPrivateInstancesBeforeRefusing) {
  NpcReactionTimeline timeline;
  const ThreatReaction reaction{1939, 1000};
  constexpr auto kCapacity = NpcReactionTimeline::kMaximumActors;

  for (std::uint64_t unit = 1; unit <= kCapacity / 2; ++unit) {
    ASSERT_TRUE(timeline.begin("shared:9012:1", unit, 0, reaction, 0, true));
  }
  for (std::uint64_t unit = kCapacity / 2 + 1; unit <= kCapacity; ++unit) {
    ASSERT_TRUE(timeline.begin("private:session-a", unit, 0, reaction, 0, true));
  }

  ASSERT_TRUE(timeline.begin("shared:9012:2", 1, 2000, reaction, 0, true));
  EXPECT_EQ(timeline.size(), kCapacity / 2 + 1);
  EXPECT_EQ(timeline.sample("shared:9012:1", 1, 5000).route_elapsed, 4000);
  EXPECT_EQ(timeline.sample("private:session-a", kCapacity, 5000).route_elapsed, 5000);
}

TEST(NpcFeature, PopulationRemainsColdUntilTheSessionOwnsTheConfiguredMap) {
  const NpcFeature feature(population(), kEpoch);
  NpcContext context;
  NpcPlayerSession session(feature, context);
  session.announce_population();
  session.set_population_map(8000);
  session.announce_population();
  EXPECT_EQ(context.registry.size(), 0);
  EXPECT_EQ(context.clock_reads, 0);
  EXPECT_TRUE(session.refresh_npc_sight({9012, 0, 0, 0, 0}).empty());
  EXPECT_FALSE(session.has_patrol());
  EXPECT_FALSE(session.next_action_tick());

  session.set_population_map(9012);
  session.announce_population();
  EXPECT_EQ(context.registry.size(), 1);
  EXPECT_FALSE(session.has_patrol());
  session.announce_population();
  EXPECT_EQ(context.registry.size(), 1);
  EXPECT_EQ(context.clock_reads, 1);
}

TEST(NpcFeature, EveryMapPlacementFileBesideTheCatalogGetsItsOwnPopulation) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("npc-map-populations-" + std::to_string(::testing::UnitTest::GetInstance()
                                                                  ->random_seed()));
  const auto placements = root / "npc" / "placements";
  std::filesystem::create_directories(placements);
  std::filesystem::create_directories(root / "catalog");
  const auto write = [&placements](int map_id, int x_mm) {
    const Json placement{{"npc_cms_id", 1001405},
                         {"position_mm", {{"x", x_mm}, {"y", 0}, {"z", 0}}},
                         {"yaw_degrees", 0}};
    std::ofstream output(placements / ("map-" + std::to_string(map_id) + ".json"));
    output << Json{{"format_version", 1}, {"map_id", map_id}, {"placements", {placement}}};
  };
  write(9021, 500);
  // The configured primary population wins over another file for its map.
  write(9012, 99999);

  NpcFeature feature(population(), kEpoch);
  feature.load_map_populations(root / "catalog");
  EXPECT_EQ(feature.population_map_count(), 2U);
  ASSERT_NE(feature.population_for(9021), nullptr);
  EXPECT_EQ(feature.population_for(9012), feature.population());
  EXPECT_EQ(feature.population_for(9031), nullptr);
  EXPECT_EQ(feature.population()->map_id(), 9012);

  NpcContext context;
  context.map_id = 9021;
  NpcPlayerSession session(feature, context);
  session.set_population_map(9021);
  session.announce_population();
  ASSERT_EQ(context.registry.size(), 1);
  const auto id = feature.population_for(9021)->units().front().unit_id;
  ASSERT_NE(context.registry.find(id), nullptr);
  EXPECT_EQ(context.registry.find(id)->position.x_mm, 500);
  EXPECT_FALSE(session.refresh_npc_sight({9021, 0, 0, 0, 0}).empty());
  EXPECT_TRUE(session.refresh_npc_sight({9012, 0, 0, 0, 0}).empty());

  std::filesystem::remove_all(root);
}

TEST(NpcFeature, SameMapAssignmentKeepsActorsWhileActualDepartureClearsTheSharedRegistry) {
  const NpcFeature feature(population(true), kEpoch);
  NpcContext context;
  NpcPlayerSession session(feature, context);
  session.set_population_map(9012);
  session.announce_population();
  const auto id = feature.population()->units().front().unit_id;
  ASSERT_NE(context.registry.find(id), nullptr);
  context.registry.announce({47, 900, 9012, "17", {3, 4, 5}});

  session.set_population_map(9012);
  EXPECT_EQ(context.registry.size(), 2);
  EXPECT_TRUE(session.has_patrol());
  session.set_population_map(std::nullopt);
  EXPECT_EQ(context.registry.size(), 0);
  EXPECT_FALSE(session.has_patrol());
  EXPECT_FALSE(session.next_action_tick());
  EXPECT_TRUE(session.refresh_npc_sight({9012, 0, 0, 0, 0}).empty());
}

TEST(NpcFeature, RegistryCollisionFailsBeforeMarkingPopulationAnnounced) {
  const NpcFeature feature(population(), kEpoch);
  NpcContext context;
  NpcPlayerSession session(feature, context);
  session.set_population_map(9012);
  const auto id = feature.population()->units().front().unit_id;
  ASSERT_EQ(context.registry.announce({id, 9, 9012, "other", {0, 0, 0}}),
            UnitAnnounceResult::Added);
  EXPECT_THROW(session.announce_population(), std::runtime_error);
  context.registry.clear();
  EXPECT_NO_THROW(session.announce_population());
  ASSERT_NE(context.registry.find(id), nullptr);
  EXPECT_EQ(context.registry.find(id)->instance_key, "17");
}

TEST(NpcFeature, TargetedRequestsSynchronizePatrolButPingClockAndMovementDoNot) {
  const NpcFeature feature(population(true), kEpoch);
  NpcContext context;
  NpcPlayerSession session(feature, context);
  session.set_population_map(9012);
  session.announce_population();
  const auto id = feature.population()->units().front().unit_id;
  context.ticks += kSecond / 2;
  const auto reads = context.clock_reads;
  for (const auto opcode : {20003, 20001, 20087}) {
    session.observe_patrol_position(static_cast<std::uint16_t>(opcode));
  }
  EXPECT_EQ(context.clock_reads, reads);
  ASSERT_NE(context.registry.find(id), nullptr);
  EXPECT_EQ(context.registry.find(id)->position.x_mm, 0);

  session.observe_patrol_position(20068);
  EXPECT_EQ(context.registry.find(id)->position.x_mm, 550);
  EXPECT_EQ(context.clock_reads, reads + 1);
  context.instance = "other";
  EXPECT_EQ(session.interaction_npc(id), nullptr);
}

TEST(NpcFeature, LateEntryUsesTheSharedEpochAndClampsOnlyNegativeElapsedTime) {
  const NpcFeature feature(population(true), kEpoch);
  NpcContext first_context;
  NpcContext later_context;
  first_context.ticks = kEpoch - 1;
  later_context.ticks = kEpoch + kSecond / 2;
  NpcPlayerSession first(feature, first_context);
  NpcPlayerSession later(feature, later_context);
  first.set_population_map(9012);
  later.set_population_map(9012);
  first.announce_population();
  later.announce_population();
  const auto id = feature.population()->units().front().unit_id;
  EXPECT_EQ(first.population_elapsed_ticks(), 0);
  EXPECT_EQ(first_context.registry.find(id)->position.x_mm, 0);
  EXPECT_EQ(later_context.registry.find(id)->position.x_mm, 550);
}

TEST(NpcFeature, SharedMapObserverRearmsGenericSectorsWithoutConsumingTheRequest) {
  const NpcFeature feature(nullptr, kEpoch);
  NpcContext context;
  NpcPlayerSession session(feature, context);
  FrameContext frame(20085, WorldGameplayRequest{20085, 9, Json::object()});
  EXPECT_TRUE(session.observe_shared_population(frame));
  EXPECT_NE(frame.request(), nullptr);
  EXPECT_EQ(context.clock_reads, 0);
  context.tutorial = true;
  EXPECT_FALSE(session.observe_shared_population(frame));
  FrameContext handshake_only(20085, std::nullopt);
  EXPECT_FALSE(session.observe_shared_population(handshake_only));
}

TEST(NpcFeature, WalkingTargetStopsAndReactsOnceInTheThreateningPlayersOwnPatrol) {
  const Json profile{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"source_executable_dump_sha256",
       "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6"},
      {"range_mm", 5000},
      {"npcs", {{{"npc_cms_id", 1001405}, {"game_anim_mapping_id", 1939}, {"duration_ms", 6500}}}}};
  const NpcFeature feature(population(true), kEpoch, NpcReactionCatalog(profile));
  NpcContext context;
  NpcPlayerSession session(feature, context);
  session.set_population_map(9012);
  session.announce_population();
  session.refresh_npc_sight({9012, 0, 0, 0, 0});
  std::vector<GameplayPacket> start;
  session.observe_start_playing_patrol(true, start);

  // Mid-walk: the first leg moves 1100 mm per second.
  context.ticks += kSecond / 4;
  session.advance_patrol(context.ticks, {9012, 0, 0, 0, 0});
  const auto id = feature.population()->units().front().unit_id;
  FrameContext frame(
      kThreatenedNpcRequest,
      WorldGameplayRequest{
          kThreatenedNpcRequest, 1, {{"target_npc_unit_ids", {static_cast<std::uint32_t>(id)}}}});
  frame.gameplay_position = WorldPosition{9012, 0, 0, 0, 0};
  ASSERT_TRUE(session.handle_threat(frame));

  // Observers receive the reaction directly; the own client receives it from its patrol.
  EXPECT_TRUE(frame.session_packets.empty());
  ASSERT_EQ(context.observer_packets.size(), 1U);
  ASSERT_EQ(session.next_action_tick(), context.ticks);

  const auto own = session.advance_patrol(context.ticks, {9012, 0, 0, 0, 0});
  const auto stop = std::ranges::find(own, std::uint16_t{177}, &GameplayPacket::opcode);
  const auto motion = std::ranges::find(own, std::uint16_t{69}, &GameplayPacket::opcode);
  ASSERT_NE(stop, own.end());
  ASSERT_NE(motion, own.end());
  EXPECT_LT(stop, motion);
  EXPECT_EQ(stop->message.at("detail"), "MoveDetail_Stop");
  EXPECT_EQ(motion->message.at("tick"), context.observer_packets.front().message.at("tick"));
  EXPECT_EQ(std::ranges::count(own, std::uint16_t{69}, &GameplayPacket::opcode), 1);

  // The next wake-up during the same reaction repeats nothing.
  context.ticks += kSecond;
  const auto later = session.advance_patrol(context.ticks, {9012, 0, 0, 0, 0});
  EXPECT_EQ(std::ranges::count(later, std::uint16_t{69}, &GameplayPacket::opcode), 0);
}

TEST(NpcFeature, SightAnnouncementPrecedesPatrolMovementWhenThePlayerReturnsIntoRange) {
  const NpcFeature feature(population(true), kEpoch);
  NpcContext context;
  NpcPlayerSession session(feature, context);
  session.set_population_map(9012);
  session.announce_population();
  EXPECT_TRUE(session.refresh_npc_sight({9012, 300000, 0, 0, 0}).empty());
  std::vector<GameplayPacket> start;
  session.observe_start_playing_patrol(false, start);
  EXPECT_FALSE(session.next_action_tick());
  session.observe_start_playing_patrol(true, start);
  EXPECT_TRUE(start.empty());
  ASSERT_TRUE(session.next_action_tick());
  context.ticks += kSecond / 2;

  const auto packets = session.advance_patrol(context.ticks, {9012, 0, 0, 0, 0});
  const auto announcement = std::ranges::find(packets, kUnitAddedOpcode, &GameplayPacket::opcode);
  const auto movement = std::ranges::find(packets, std::uint16_t{177}, &GameplayPacket::opcode);
  ASSERT_NE(announcement, packets.end());
  ASSERT_NE(movement, packets.end());
  EXPECT_LT(announcement, movement);
}

TEST(NpcFeature, TutorialAnnouncementsRequireServerNpcShapeAndRemovalsIgnoreTutorialState) {
  const NpcFeature feature(nullptr, kEpoch);
  NpcContext context;
  context.map_id = 8000;
  NpcPlayerSession session(feature, context);
  GameplayPacket npc{
      10,
      {{"unit_id", "42"},
       {"nub",
        {{"npc", {{"npc_cms_id", 901}}},
         {"pos", {{"millimeter_x", 3}, {"millimeter_y", 4}, {"millimeter_z", 5}}}}}}};
  session.track_announced_unit(npc);
  EXPECT_EQ(context.registry.size(), 0);
  context.tutorial = true;
  context.authenticated = false;
  session.track_announced_unit(npc);
  EXPECT_EQ(context.registry.size(), 0);
  context.authenticated = true;
  auto invalid = npc;
  invalid.message["nub"]["pos"]["millimeter_x"] = "client text";
  session.track_announced_unit(invalid);
  EXPECT_EQ(context.registry.size(), 0);
  session.track_announced_unit(npc);
  ASSERT_NE(session.interaction_npc(42), nullptr);
  EXPECT_EQ(session.interaction_npc(42)->position.x_mm, 3);
  EXPECT_EQ(session.interaction_npc(43), nullptr);
  context.tutorial = false;
  context.authenticated = false;
  session.track_announced_unit({11, {{"unit_ids", {42, "not an id", "42"}}}});
  EXPECT_EQ(context.registry.size(), 0);
}

TEST(NpcFeature, LeavingAndReturningToSightPreservesTheAuthoritativeInteractionTarget) {
  const NpcFeature feature(population(true), kEpoch);
  NpcContext context;
  NpcPlayerSession session(feature, context);
  session.set_population_map(9012);
  session.announce_population();
  const auto id = feature.population()->units().front().unit_id;

  for (const auto &packet : session.refresh_npc_sight({9012, 0, 0, 0, 0})) {
    session.track_announced_unit(packet);
  }
  ASSERT_NE(session.interaction_npc(id), nullptr);

  const auto leaving = session.refresh_npc_sight({9012, 300000, 0, 0, 0});
  ASSERT_EQ(leaving.size(), 1);
  ASSERT_EQ(leaving.front().opcode, kNpcSightUnitRemovedOpcode);
  for (const auto &packet : leaving) {
    session.track_announced_unit(packet);
  }

  context.ticks += kSecond / 2;
  session.observe_patrol_position(20065);
  const auto returning = session.refresh_npc_sight({9012, 0, 0, 0, 0});
  ASSERT_EQ(returning.size(), 1);
  ASSERT_EQ(returning.front().opcode, kUnitAddedOpcode);
  for (const auto &packet : returning) {
    session.track_announced_unit(packet);
  }

  const auto *target = session.interaction_npc(id);
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(target->npc_template_id, 1001405);
  EXPECT_EQ(target->position.x_mm, 550);
  EXPECT_EQ(target->map_id, 9012);
  EXPECT_EQ(target->instance_key, context.instance);

  // Actual despawns and map departures still invalidate the target.
  session.track_announced_unit({11, {{"unit_ids", {std::to_string(id)}}}});
  EXPECT_EQ(session.interaction_npc(id), nullptr);
  session.set_population_map(std::nullopt);
  EXPECT_EQ(context.registry.size(), 0);
}

} // namespace
} // namespace chronicles::npc
