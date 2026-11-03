#include "world/features/combat/gliding.hpp"

#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr WorldPosition kGlidePosition{9012, 1000, 2000, 3000, 45};

Json start_request() {
  return {{"body_yaw", 90},
          {"tick", "1000000000"},
          {"pos", {{"millimeter_x", 1000}, {"millimeter_y", 2000}, {"millimeter_z", 3000}}}};
}

TEST(Gliding, FallingStartIsSilentAndLandingPermitsAnotherFlight) {
  GlidingSession gliding;
  gliding.observe_movement({{"mode", "MoveMode_Falling"}});
  EXPECT_FALSE(gliding.start(start_request(), kGlidePosition, {}, 10'000'000));
  EXPECT_TRUE(gliding.active());

  gliding.observe_movement({{"mode", "MoveMode_Gliding"}});
  EXPECT_TRUE(gliding.active());
  gliding.observe_movement(Json::object());
  EXPECT_FALSE(gliding.active());
  gliding.observe_movement({{"mode", "MoveMode_Falling"}});
  EXPECT_FALSE(gliding.start(start_request(), kGlidePosition, {}, 20'000'000));
}

TEST(Gliding, PredictedMovementStillChecksLifeMountAndActionConditions) {
  for (const auto conditions :
       {GlidingConditions{true, false, false, false}, GlidingConditions{false, true, false, false},
        GlidingConditions{false, false, true, false},
        GlidingConditions{false, false, false, true}}) {
    GlidingSession gliding;
    gliding.observe_movement({{"mode", "MoveMode_Gliding"}});
    const auto reply = gliding.start(start_request(), kGlidePosition, conditions, 10'000'000);
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->opcode, 180);
    EXPECT_FALSE(gliding.active());
  }
}

TEST(Gliding, StunnedPlayerReceivesTheCrowdControlRefusal) {
  GlidingSession gliding;
  gliding.observe_movement({{"mode", "MoveMode_Falling"}});
  GlidingConditions conditions;
  conditions.crowd_controlled = true;

  const auto reply = gliding.start(start_request(), kGlidePosition, conditions, 10'000'000);
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->message.at("ec"), "Move_GlideWhileCCed");
  EXPECT_FALSE(gliding.active());

  // Death and mounting keep their more specific refusals while stunned.
  conditions.dead = true;
  const auto dead = gliding.start(start_request(), kGlidePosition, conditions, 10'000'000);
  ASSERT_TRUE(dead);
  EXPECT_EQ(dead->message.at("ec"), "Move_GlideWhileDead");
}

TEST(Gliding, GroundLoadingDuplicateAndMapResetHaveDefinedResults) {
  GlidingSession gliding;
  auto reply = gliding.start(start_request(), kGlidePosition, {}, 10'000'000);
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->message.at("ec"), "Move_GlideNotFalling");
  reply = gliding.start(start_request(), std::nullopt, {}, 10'000'000);
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->message.at("ec"), "InvalidState");

  gliding.observe_movement({{"mode", "MoveMode_Falling"}});
  EXPECT_FALSE(gliding.start(start_request(), kGlidePosition, {}, 10'000'000));
  reply = gliding.start(start_request(), kGlidePosition, {}, 10'000'000);
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->message.at("ec"), "Move_GlideStartDuplication");
  EXPECT_FALSE(gliding.active());

  gliding.reset();
  reply = gliding.start(start_request(), kGlidePosition, {}, 20'000'000);
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->message.at("ec"), "Move_GlideNotFalling");
}

TEST(Gliding, InvalidPoseUsesAuthoritativeCorrectionAndRecoveredSchema) {
  GlidingSession gliding;
  gliding.observe_movement({{"mode", "MoveMode_Falling"}});
  auto request = start_request();
  request["body_yaw"] = std::numeric_limits<double>::infinity();
  request["pos"]["millimeter_x"] = 2'000'000'000;
  const auto reply = gliding.start(request, kGlidePosition, {}, 10'000'000);
  ASSERT_TRUE(reply);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto decoded = protocol.decode(protocol.encode(reply->opcode, reply->message));
  const auto &message = decoded.at("message");
  EXPECT_EQ(message.at("ec"), "BadRequest");
  EXPECT_EQ(message.at("tick"), "1000000000");
  EXPECT_EQ(message.at("body_yaw"), 45);
  EXPECT_EQ(message.at("pos").at("millimeter_x"), kGlidePosition.x_mm);
  EXPECT_FALSE(gliding.active());
}

} // namespace
} // namespace chronicles
