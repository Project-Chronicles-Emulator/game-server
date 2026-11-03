#include "world/features/combat/combat.hpp"
#include "world/features/combat/fall_damage.hpp"
#include "world/session/native_world.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace chronicles {
namespace {

WorldPosition at_height(int z_mm, int map_id = 8000) {
  return {map_id, 0, 0, z_mm, 0.0F};
}

TEST(FallDamage, SafeHeightBoundaryAndDamageScaleWithMaximumHealth) {
  FallDamageRules rules;
  EXPECT_EQ(rules.damage(0, 1000), 0);
  EXPECT_EQ(rules.damage(10000, 1000), 0);
  EXPECT_EQ(rules.damage(10001, 1000), 1);
  EXPECT_EQ(rules.damage(15000, 1000), 250);
  EXPECT_EQ(rules.damage(20000, 1000), 500);
  EXPECT_EQ(rules.damage(30000, 1000), 1000);
  EXPECT_EQ(rules.damage(std::numeric_limits<std::int64_t>::max(), 1000), 1000);
  EXPECT_EQ(rules.damage(20000, 2000), 1000);
  EXPECT_EQ(rules.damage(30000, 0), 0);
}

TEST(FallDamage, RejectsInvalidRulesAndHandlesLargeHealthWithoutOverflow) {
  EXPECT_THROW((FallDamageRules{-1, 20000}.damage(10000, 100)), std::invalid_argument);
  EXPECT_THROW((FallDamageRules{10000, 0}.damage(20000, 100)), std::invalid_argument);
  EXPECT_EQ(FallDamageRules{}.damage(30000, std::numeric_limits<int>::max()),
            std::numeric_limits<int>::max());
}

TEST(FallDamage, TracksLedgeAndJumpApexAndChargesOnlyOnLanding) {
  FallTracker tracker;
  EXPECT_FALSE(tracker.observe(at_height(40000), "MoveMode_Normal"));
  EXPECT_FALSE(tracker.observe(at_height(39000), "MoveMode_Falling"));
  EXPECT_FALSE(tracker.observe(at_height(42000), "MoveMode_Falling"));
  EXPECT_FALSE(tracker.observe(at_height(20000), "MoveMode_Falling"));
  EXPECT_EQ(tracker.observe(at_height(10000), "MoveMode_Normal"), 32000);
  EXPECT_FALSE(tracker.observe(at_height(10000), "MoveMode_Normal"));
  EXPECT_FALSE(tracker.observe(at_height(10000), "MoveMode_Crouching"));
}

TEST(FallDamage, RegionalHandoffRetainsTheExistingFallHeight) {
  FallTracker tracker;
  EXPECT_FALSE(tracker.observe(at_height(40000), "MoveMode_Falling"));
  tracker.continue_on_map(8001);
  EXPECT_FALSE(tracker.observe(at_height(20000, 8001), "MoveMode_Falling"));
  EXPECT_EQ(tracker.observe(at_height(10000, 8001), "MoveMode_Normal"), 30000);
}

TEST(FallDamage, WalkingDownhillAndAscendingLandingDoNotCauseDamage) {
  FallTracker tracker;
  EXPECT_FALSE(tracker.observe(at_height(50000), "MoveMode_Normal"));
  EXPECT_FALSE(tracker.observe(at_height(0), "MoveMode_Normal"));
  EXPECT_FALSE(tracker.observe(at_height(1000), "MoveMode_Falling"));
  EXPECT_EQ(tracker.observe(at_height(2000), "MoveMode_Crouching"), 0);
}

TEST(FallDamage, SwimmingGlidingClimbingAndOtherModesBreakTheFall) {
  for (const auto mode : {"MoveMode_Swimming", "MoveMode_Gliding", "MoveMode_Climbing",
                          "MoveMode_Flying", "MoveMode_Parkour", "MoveMode_RootMotion"}) {
    FallTracker tracker;
    tracker.observe(at_height(50000), "MoveMode_Falling");
    EXPECT_FALSE(tracker.observe(at_height(30000), mode));
    EXPECT_FALSE(tracker.observe(at_height(0), "MoveMode_Normal")) << mode;
  }
}

TEST(FallDamage, MapChangesResetAndExplicitResetDiscardOldHeight) {
  FallTracker tracker;
  tracker.observe(at_height(50000), "MoveMode_Falling");
  EXPECT_FALSE(tracker.observe(at_height(0, 10117), "MoveMode_Normal"));
  tracker.observe(at_height(50000), "MoveMode_Falling");
  tracker.reset();
  EXPECT_FALSE(tracker.observe(at_height(0), "MoveMode_Normal"));
}

TEST(FallDamage, IndependentPlayersAndFullCoordinateRange) {
  FallTracker first;
  FallTracker second;
  first.observe(at_height(std::numeric_limits<int>::max()), "MoveMode_Normal");
  first.observe(at_height(0), "MoveMode_Falling");
  EXPECT_FALSE(second.observe(at_height(0), "MoveMode_Normal"));
  EXPECT_EQ(first.observe(at_height(std::numeric_limits<int>::min()), "MoveMode_Normal"),
            4294967295LL);
}

std::unique_ptr<CombatSession> fall_combat(bool private_instance = true) {
  using Json = nlohmann::json;
  auto data = std::make_shared<CombatData>(Json{{"format_version", 1},
                                                {"skills", Json::array()},
                                                {"effects", Json::object()},
                                                {"projectiles", Json::object()},
                                                {"npc_stats", Json::object()},
                                                {"npc_skills", Json::object()},
                                                {"weapon_items", Json::object()},
                                                {"recoveries", Json::object()}});
  auto tutorial =
      std::make_shared<TutorialData>(Json{{"format_version", 1},
                                          {"map_id", 8000},
                                          {"objects", Json::array()},
                                          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
                                          {"doodads", Json::array()},
                                          {"utility_items", Json::array()},
                                          {"civilians", Json::array()},
                                          {"sectors", Json::array()},
                                          {"npc_spawns", Json::array()},
                                          {"quests", Json::array()}});
  Character character;
  character.id = 7;
  character.class_id = 1;
  if (!private_instance) {
    tutorial.reset();
  }
  return std::make_unique<CombatSession>(
      data, tutorial, character, Json{{"18", 1000}, {"19", 100}, {"69", 1000}, {"72", 100}});
}

TEST(FallDamage, NonlethalImpactUpdatesHealthAndEncodesTheClientNotification) {
  auto combat = fall_combat();
  const auto damage = FallDamageRules{}.damage(15000, 1000);
  const auto packets = combat->apply_fall_damage(damage, at_height(0), 100000000);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets.front().opcode, 19);
  EXPECT_EQ(packets.front().message.at("stat_changes").at(0).at("result"), 750);
  EXPECT_EQ(combat->player_stats().at("69"), 750);
  EXPECT_FALSE(combat->player_dead());

  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto decoded = protocol.decode(protocol.encode(19, packets.front().message));
  EXPECT_EQ(decoded.at("message").at("final_damage_amount"), 250);
  EXPECT_EQ(decoded.at("message").at("stat_changes").at(0).at("delta"), -250);
}

TEST(FallDamage, LethalImpactUsesDeathLifecycleAndRevivalRestoresHealth) {
  auto combat = fall_combat();
  const auto packets = combat->apply_fall_damage(2000, at_height(-1000), 100000000);
  ASSERT_GE(packets.size(), 5U);
  const std::vector<std::uint16_t> expected{32, 19, 23, 54, 47};
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (std::size_t index = 0; index < expected.size(); ++index) {
    EXPECT_EQ(packets[index].opcode, expected[index]);
    EXPECT_NO_THROW(protocol.encode(packets[index].opcode, packets[index].message));
  }
  EXPECT_EQ(packets[1].message.at("stat_changes").at(0).at("delta"), -1000);
  EXPECT_TRUE(combat->player_dead());
  EXPECT_TRUE(combat->apply_fall_damage(10, at_height(0), 100000001).empty());
  EXPECT_FALSE(combat->revive_if_dead(100000002).empty());
  EXPECT_EQ(combat->player_stats().at("69"), 1000);
  EXPECT_FALSE(combat->player_dead());
}

TEST(FallDamage, ZeroAndNegativeDamageCannotHealOrEmitPackets) {
  auto combat = fall_combat();
  EXPECT_TRUE(combat->apply_fall_damage(0, at_height(0), 100000000).empty());
  EXPECT_TRUE(combat->apply_fall_damage(-500, at_height(0), 100000000).empty());
  EXPECT_EQ(combat->player_stats().at("69"), 1000);
}

TEST(FallDamage, SharedMapVitalsDoNotRequirePrivateNpcSimulation) {
  auto combat = fall_combat(false);
  const auto packets = combat->apply_fall_damage(250, at_height(0, 9012), 100000000);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(combat->player_stats().at("69"), 750);
  EXPECT_FALSE(combat->player_dead());
}

TEST(FallDamage, AcceptedMovementPreservesZeroValuedProtobufCoordinates) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol, true);
  Character character;
  character.id = 1;
  character.class_id = 1;
  ASSERT_TRUE(handshake.configure_bootstrap_character(character));
  handshake.receive(protocol.encode(20001, nlohmann::json::object(), 0), 100000000, 0);
  handshake.receive(protocol.encode(20085, nlohmann::json::object(), 1), 100000001, 0);
  const nlohmann::json move{{"pos", nlohmann::json::object()}, {"mode", "MoveMode_Falling"}};
  handshake.receive(protocol.encode(20087, move, 2), 100000002, 0);
  const auto position = handshake.last_position();
  ASSERT_TRUE(position);
  EXPECT_EQ(position->x_mm, 0);
  EXPECT_EQ(position->y_mm, 0);
  EXPECT_EQ(position->z_mm, 0);
  EXPECT_TRUE(handshake.take_last_movement());
}

} // namespace
} // namespace chronicles
