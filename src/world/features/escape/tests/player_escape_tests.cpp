// Behavioral tests for src/world/features/escape/player_escape.cpp. Every outbound payload is
// encoded and decoded with the recovered descriptors, so field names and wire types are
// schema-checked.

#include "world/features/escape/player_escape.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::uint16_t kEscapeRequest = 20048;
constexpr std::uint16_t kTeleportStartRequest = 20088;
constexpr std::uint16_t kTeleportCompletionRequest = 20089;
constexpr std::uint64_t kPlayerUnitId = 4097;
constexpr std::int64_t kNow = 1'000'000'000;
constexpr std::int64_t kTicksPerSecond = 10'000'000;
// Internal 100 ns ticks are sent as nanoseconds.
constexpr std::int64_t kWireNanosecondsPerTick = 100;

const WorldPosition kStuck{9012, -457236, 2986882, -640861, 14.87F};

Json candidate_request(int x_mm, int y_mm, int z_mm) {
  const Json position{{"millimeter_x", x_mm}, {"millimeter_y", y_mm}, {"millimeter_z", z_mm}};
  return {{"candidate_pos", position}};
}

// The captured candidate, about 1.46 m from the stuck position.
Json captured_candidate() {
  return candidate_request(-456150, 2985900, -640800);
}

EscapeContext in_world() {
  EscapeContext context;
  context.position = kStuck;
  return context;
}

std::string wire_tick(std::int64_t internal_tick) {
  return std::to_string(internal_tick * kWireNanosecondsPerTick);
}

class PlayerEscapeTest : public ::testing::Test {
protected:
  // Round-trips each packet through the descriptors and returns the decoded opcodes.
  std::vector<int> wire_opcodes(const std::vector<GameplayPacket> &packets) {
    std::vector<int> opcodes;
    for (const auto &packet : packets) {
      const auto decoded = protocol_.decode(protocol_.encode(packet.opcode, packet.message));
      EXPECT_EQ(decoded.at("status"), "decoded");
      opcodes.push_back(decoded.at("opcode").get<int>());
    }
    return opcodes;
  }

  EscapeOutcome escape(const Json &message, const EscapeContext &context,
                       std::int64_t now = kNow) {
    return escape_.receive(kEscapeRequest, message, context, now);
  }

  EscapeOutcome start(const std::string &tick) {
    return escape_.receive(kTeleportStartRequest, {{"tick", tick}}, {}, kNow);
  }

  EscapeOutcome complete(std::int64_t now = kNow) {
    return escape_.receive(kTeleportCompletionRequest, Json::object(), {}, now);
  }

  Protocol protocol_{CHRONICLES_PROTOCOL_DIR};
  PlayerEscape escape_{EscapeRules{}, kPlayerUnitId};
};

TEST_F(PlayerEscapeTest, CapturedEscapeRequestRoundTripsThroughTheDescriptors) {
  const auto decoded = protocol_.decode(protocol_.encode(kEscapeRequest, captured_candidate(), 0));
  EXPECT_EQ(decoded.at("status"), "decoded");
  EXPECT_EQ(decoded.at("message"), captured_candidate());
}

TEST_F(PlayerEscapeTest, CapturedEscapeTeleportsToTheCandidateAndStartsTheCooldown) {
  const auto requested = escape(captured_candidate(), in_world());

  ASSERT_EQ(requested.refusal, EscapeRefusal::none);
  EXPECT_EQ(wire_opcodes(requested.packets), (std::vector<int>{181, 49}));
  const auto &teleport = requested.packets.at(0).message;
  EXPECT_EQ(teleport.at("unit_id"), std::to_string(kPlayerUnitId));
  EXPECT_EQ(teleport.at("tick"), wire_tick(kNow));
  EXPECT_EQ(teleport.at("pos").at("millimeter_x"), -456150);
  EXPECT_EQ(teleport.at("pos").at("millimeter_y"), 2985900);
  EXPECT_EQ(teleport.at("pos").at("millimeter_z"), -640800);
  EXPECT_FLOAT_EQ(teleport.at("body_yaw").get<float>(), kStuck.yaw);

  // EscapeCooltimeSec: the hidden cooldown status expires exactly 300 s after the request.
  const auto &cooldown = requested.packets.at(1).message.at("status_effect");
  EXPECT_EQ(cooldown.at("status_effect_id"), 50030003);
  EXPECT_EQ(cooldown.at("caster_unit_id"), std::to_string(kPlayerUnitId));
  EXPECT_EQ(cooldown.at("start_tick"), wire_tick(kNow));
  EXPECT_EQ(cooldown.at("expire_tick"), wire_tick(kNow + 300 * kTicksPerSecond));
  EXPECT_TRUE(escape_.teleport_pending());
  EXPECT_FALSE(requested.relocated_to);
}

TEST_F(PlayerEscapeTest, TeleportHandshakeTogglesInvincibilityAndRelocatesOnCompletion) {
  ASSERT_EQ(escape(captured_candidate(), in_world()).refusal, EscapeRefusal::none);

  const auto started = start(wire_tick(kNow));
  EXPECT_EQ(wire_opcodes(started.packets), (std::vector<int>{33}));
  EXPECT_EQ(started.packets.at(0).message.at("attributes").at("2"), 1);

  const auto later = kNow + 2 * kTicksPerSecond / 100;
  const auto completed = complete(later);
  EXPECT_EQ(wire_opcodes(completed.packets), (std::vector<int>{49, 33, 51}));
  const auto &invincible = completed.packets.at(0).message.at("status_effect");
  EXPECT_EQ(invincible.at("status_effect_id"), 50030002);
  EXPECT_EQ(invincible.at("start_tick"), wire_tick(kNow));
  EXPECT_EQ(invincible.at("expire_tick"), "9223372036854775807");
  EXPECT_EQ(completed.packets.at(1).message.at("attributes").at("2"), 0);
  const auto &removed = completed.packets.at(2).message;
  EXPECT_EQ(removed.at("tick"), wire_tick(later));
  EXPECT_EQ(removed.at("status_effects").at(0).at("instance_id"), invincible.at("instance_id"));

  ASSERT_TRUE(completed.relocated_to);
  EXPECT_EQ(*completed.relocated_to,
            (WorldPosition{kStuck.map_id, -456150, 2985900, -640800, kStuck.yaw}));
  EXPECT_FALSE(escape_.teleport_pending());
  EXPECT_FALSE(escape_.handles(kTeleportStartRequest, {{"tick", wire_tick(kNow)}}));
  EXPECT_FALSE(escape_.handles(kTeleportCompletionRequest, Json::object()));
}

TEST_F(PlayerEscapeTest, OnlyTheEchoedStartTickAndAStartedTeleportAreClaimed) {
  ASSERT_EQ(escape(captured_candidate(), in_world()).refusal, EscapeRefusal::none);

  // A different tick answers another owner's teleport (a tutorial cutscene), and a completion
  // before the start is not this escape's either.
  EXPECT_FALSE(escape_.handles(kTeleportStartRequest, {{"tick", wire_tick(kNow + 1)}}));
  EXPECT_TRUE(start(wire_tick(kNow + 1)).packets.empty());
  EXPECT_TRUE(escape_.receive(kTeleportStartRequest, Json::object(), {}, kNow).packets.empty());
  EXPECT_FALSE(escape_.handles(kTeleportCompletionRequest, Json::object()));
  const auto early_completion = complete();
  EXPECT_TRUE(early_completion.packets.empty());
  EXPECT_FALSE(early_completion.relocated_to);
  EXPECT_TRUE(escape_.teleport_pending());

  EXPECT_TRUE(escape_.handles(kTeleportStartRequest, {{"tick", wire_tick(kNow)}}));
  EXPECT_EQ(wire_opcodes(start(wire_tick(kNow)).packets), (std::vector<int>{33}));
  EXPECT_FALSE(escape_.handles(kTeleportStartRequest, {{"tick", wire_tick(kNow)}}));
  EXPECT_TRUE(escape_.handles(kTeleportCompletionRequest, Json::object()));
  EXPECT_TRUE(complete().relocated_to);
}

TEST_F(PlayerEscapeTest, CooldownRefusesUntilItsRemovalAndThenAllowsAnotherEscape) {
  ASSERT_EQ(escape(captured_candidate(), in_world()).refusal, EscapeRefusal::none);
  ASSERT_FALSE(start(wire_tick(kNow)).packets.empty());
  ASSERT_TRUE(complete().relocated_to);

  const auto expiry = kNow + 300 * kTicksPerSecond;
  ASSERT_EQ(escape_.next_expiry_tick(), expiry);
  const auto early = escape(captured_candidate(), in_world(), expiry - 1);
  EXPECT_EQ(early.refusal, EscapeRefusal::cooling_down);
  EXPECT_TRUE(early.packets.empty());
  EXPECT_TRUE(escape_.expire(expiry - 1).empty());

  const auto removal = escape_.expire(expiry);
  EXPECT_EQ(wire_opcodes(removal), (std::vector<int>{51}));
  const auto &removed = removal.at(0).message.at("status_effects").at(0);
  EXPECT_EQ(removed.at("status_effect_id"), 50030003);
  EXPECT_FALSE(escape_.next_expiry_tick());
  EXPECT_TRUE(escape_.expire(expiry + 1).empty());

  const auto again = escape(captured_candidate(), in_world(), expiry + 1);
  EXPECT_EQ(again.refusal, EscapeRefusal::none);
  EXPECT_EQ(wire_opcodes(again.packets), (std::vector<int>{181, 49}));
}

TEST_F(PlayerEscapeTest, RequestAtTheExpiryRemovesTheOldCooldownBeforeTheNewOne) {
  ASSERT_EQ(escape(captured_candidate(), in_world()).refusal, EscapeRefusal::none);
  ASSERT_FALSE(start(wire_tick(kNow)).packets.empty());
  ASSERT_TRUE(complete().relocated_to);
  const auto first_expiry = escape_.next_expiry_tick();
  ASSERT_TRUE(first_expiry);

  const auto again = escape(captured_candidate(), in_world(), *first_expiry);
  EXPECT_EQ(again.refusal, EscapeRefusal::none);
  EXPECT_EQ(wire_opcodes(again.packets), (std::vector<int>{51, 181, 49}));
  EXPECT_NE(again.packets.at(0).message.at("status_effects").at(0).at("instance_id"),
            again.packets.at(2).message.at("status_effect").at("instance_id"));
}

TEST_F(PlayerEscapeTest, RefusalsSendNothingAndLeaveNoCooldown) {
  struct Case {
    EscapeContext context;
    Json message;
    EscapeRefusal refusal;
  };

  auto dead = in_world();
  dead.dead = true;
  auto busy = in_world();
  busy.busy = true;
  auto riding = in_world();
  riding.riding = true;
  // EscapeCandidateMaxDistance 500 cm: 5000 mm is accepted below, 5001 mm is refused.
  const auto too_far = candidate_request(kStuck.x_mm + 5001, kStuck.y_mm, kStuck.z_mm);
  const Json text_axis{{"candidate_pos", Json{{"millimeter_x", "1"}}}};
  const std::vector<Case> cases{
      {EscapeContext{}, captured_candidate(), EscapeRefusal::not_in_world},
      {dead, captured_candidate(), EscapeRefusal::dead},
      {busy, captured_candidate(), EscapeRefusal::busy},
      {riding, captured_candidate(), EscapeRefusal::riding},
      {in_world(), Json::object(), EscapeRefusal::missing_candidate},
      {in_world(), text_axis, EscapeRefusal::missing_candidate},
      {in_world(), too_far, EscapeRefusal::candidate_too_far},
  };

  for (const auto &refused : cases) {
    const auto outcome = escape(refused.message, refused.context);
    EXPECT_EQ(outcome.refusal, refused.refusal) << escape_refusal_name(refused.refusal);
    EXPECT_TRUE(outcome.packets.empty()) << escape_refusal_name(refused.refusal);
    EXPECT_FALSE(escape_.teleport_pending());
    EXPECT_FALSE(escape_.next_expiry_tick());
  }

  const auto edge = escape(candidate_request(kStuck.x_mm, kStuck.y_mm + 5000, kStuck.z_mm),
                           in_world());
  EXPECT_EQ(edge.refusal, EscapeRefusal::none);
}

TEST_F(PlayerEscapeTest, PendingTeleportRefusesASecondRequestWithoutCooldown) {
  EscapeRules rules;
  rules.cooldown_ticks = 0;
  PlayerEscape escape(rules, kPlayerUnitId);
  ASSERT_EQ(escape.receive(kEscapeRequest, captured_candidate(), in_world(), kNow).refusal,
            EscapeRefusal::none);

  const auto second = escape.receive(kEscapeRequest, captured_candidate(), in_world(), kNow + 1);
  EXPECT_EQ(second.refusal, EscapeRefusal::teleport_pending);
  // The zero-length cooldown is removed even though the request is refused.
  EXPECT_EQ(wire_opcodes(second.packets), (std::vector<int>{51}));
}

TEST_F(PlayerEscapeTest, TradingEscapesAndIdleTeleportPacketsBelongToOtherHandlers) {
  EXPECT_TRUE(escape_.handles(kEscapeRequest, captured_candidate()));
  EXPECT_TRUE(escape_.handles(kEscapeRequest, {{"is_trading", false}}));
  EXPECT_FALSE(escape_.handles(kEscapeRequest, {{"is_trading", true}}));
  EXPECT_FALSE(escape_.handles(kTeleportStartRequest, {{"tick", "1"}}));
  EXPECT_FALSE(escape_.handles(kTeleportCompletionRequest, Json::object()));
  EXPECT_FALSE(escape_.handles(20039, Json::object()));

  ASSERT_EQ(escape(captured_candidate(), in_world()).refusal, EscapeRefusal::none);
  EXPECT_TRUE(escape_.handles(kTeleportStartRequest, {{"tick", wire_tick(kNow)}}));
  EXPECT_FALSE(escape_.handles(kTeleportCompletionRequest, Json::object()));
}

TEST(PlayerEscapeRules, RejectsUnusableRules) {
  EscapeRules no_range;
  no_range.candidate_max_distance_mm = 0;
  EXPECT_THROW(PlayerEscape(no_range, kPlayerUnitId), std::invalid_argument);

  EscapeRules negative_cooldown;
  negative_cooldown.cooldown_ticks = -1;
  EXPECT_THROW(PlayerEscape(negative_cooldown, kPlayerUnitId), std::invalid_argument);
}

} // namespace
} // namespace chronicles
