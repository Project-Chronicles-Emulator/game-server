#include "world/features/combat/movement_vitals.hpp"

#include "world/features/combat/combat.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kStart = 100000000;
constexpr std::int64_t kMillisecond = 10000;
constexpr WorldPosition kPosition{8000, 1000, 2000, 3000, 0.0F};

std::unique_ptr<CombatSession> movement_combat(int maximum_stamina = 100, int stamina_regen = 10,
                                               int health_regen = 0) {
  auto data = std::make_shared<CombatData>(Json{{"format_version", 1},
                                                {"skills", Json::array()},
                                                {"effects", Json::object()},
                                                {"projectiles", Json::object()},
                                                {"npc_stats", Json::object()},
                                                {"npc_skills", Json::object()},
                                                {"weapon_items", Json::object()},
                                                {"recoveries", Json::object()}});
  Character character;
  character.id = 7;
  character.class_id = 1;
  return std::make_unique<CombatSession>(data, nullptr, character,
                                         Json{{"18", 1000},
                                              {"19", maximum_stamina},
                                              {"69", 1000},
                                              {"72", maximum_stamina},
                                              {"15", health_regen},
                                              {"25", stamina_regen}});
}

Json movement(const char *mode, const char *detail) {
  // Client ticks deliberately disagree with the server clock used by every test.
  return {{"mode", mode}, {"detail", detail}, {"tick", "999999999999999"}};
}

bool contains_opcode(const std::vector<GameplayPacket> &packets, std::uint16_t opcode) {
  return std::ranges::any_of(packets, [opcode](const GameplayPacket &packet) {
    return packet.opcode == opcode;
  });
}

TEST(MovementVitals, SprintUsesCatalogCadenceWithoutClientPacketPolling) {
  auto combat = movement_combat();
  MovementVitals vitals;
  EXPECT_TRUE(
      vitals.move(movement("MoveMode_Normal", "MoveDetail_Sprint"), kPosition, *combat, kStart)
          .empty());
  EXPECT_EQ(vitals.next_action_tick(), kStart + 600 * kMillisecond);
  EXPECT_TRUE(vitals.advance(kPosition, *combat, kStart + 599 * kMillisecond).empty());
  const auto packets = vitals.advance(kPosition, *combat, kStart + 600 * kMillisecond);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets.front().message.at("default_source"), "SCS_SprintFlow");
  EXPECT_EQ(combat->player_stats().at("72"), 97);
  vitals.advance(kPosition, *combat, kStart + 1800 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 91);
}

TEST(MovementVitals, DuplicateMovementCannotPostponeTheDrainDeadline) {
  auto combat = movement_combat();
  MovementVitals vitals;
  const auto sprint = movement("MoveMode_Normal", "MoveDetail_Sprint");
  vitals.move(sprint, kPosition, *combat, kStart);
  for (int elapsed = 1; elapsed < 600; ++elapsed) {
    vitals.move(sprint, kPosition, *combat, kStart + elapsed * kMillisecond);
  }
  EXPECT_EQ(vitals.next_action_tick(), kStart + 600 * kMillisecond);
  vitals.advance(kPosition, *combat, kStart + 600 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 97);
  EXPECT_TRUE(vitals.advance(kPosition, *combat, kStart + 600 * kMillisecond).empty());
}

TEST(MovementVitals, StoppingPausesAndResumingPreservesFractionalSprintCost) {
  auto combat = movement_combat();
  MovementVitals vitals;
  const auto sprint = movement("MoveMode_Normal", "MoveDetail_Sprint");
  vitals.move(sprint, kPosition, *combat, kStart);
  vitals.move(movement("MoveMode_Normal", "MoveDetail_Run"), kPosition, *combat,
              kStart + 300 * kMillisecond);
  EXPECT_FALSE(vitals.next_action_tick());
  vitals.move(sprint, kPosition, *combat, kStart + 2000 * kMillisecond);
  vitals.advance(kPosition, *combat, kStart + 2300 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 97);
}

TEST(MovementVitals, InsufficientStaminaRefusesSprintOnceUntilThePlayerReleasesIt) {
  auto combat = movement_combat(49);
  MovementVitals vitals;
  const auto sprint = movement("MoveMode_Normal", "MoveDetail_Sprint");
  const auto refused = vitals.move(sprint, kPosition, *combat, kStart);
  ASSERT_EQ(refused.size(), 1U);
  EXPECT_EQ(refused.front().opcode, 55);
  EXPECT_TRUE(refused.front().message.at("sprint_failed"));
  EXPECT_FALSE(vitals.next_action_tick());
  EXPECT_TRUE(vitals.move(sprint, kPosition, *combat, kStart + kMillisecond).empty());
  EXPECT_EQ(combat->player_stats().at("72"), 49);
}

TEST(MovementVitals, SprintExhaustionClampsStaminaAndEmitsOneEndNotification) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Normal", "MoveDetail_Sprint"), kPosition, *combat, kStart);
  const auto exhausted = vitals.advance(kPosition, *combat, kStart + 60000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 0);
  EXPECT_EQ(std::ranges::count_if(exhausted,
                                  [](const GameplayPacket &packet) {
                                    return packet.opcode == 55;
                                  }),
            1);
  EXPECT_FALSE(vitals.next_action_tick());
  EXPECT_TRUE(vitals.advance(kPosition, *combat, kStart + 61000 * kMillisecond).empty());
}

TEST(MovementVitals, SwimmingAndSwimmingSprintHaveDifferentCosts) {
  for (const auto &[detail, expected] :
       {std::pair{"MoveDetail_Run", 90}, std::pair{"MoveDetail_Sprint", 80}}) {
    auto combat = movement_combat();
    MovementVitals vitals;
    const auto started = vitals.move(movement("MoveMode_Swimming", detail), kPosition, *combat, kStart);
    ASSERT_EQ(started.size(), 1U);
    EXPECT_EQ(started.front().opcode, 54);
    const auto &flow = started.front().message.at("flows").at(0);
    EXPECT_EQ(flow.at("source"), "SCS_SwimFlow");
    EXPECT_DOUBLE_EQ(flow.at("rate_per_sec"), static_cast<double>(expected - 100));
    const auto packets = vitals.advance(kPosition, *combat, kStart + 1000 * kMillisecond);
    EXPECT_EQ(combat->player_stats().at("72"), expected);
    EXPECT_TRUE(packets.empty());
    EXPECT_FALSE(contains_opcode(packets, 19));
  }
}

TEST(MovementVitals, RegionLoadingPausesUnderwaterDrainWithoutResettingTheFlow) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SW_Dive"), kPosition, *combat, kStart);
  vitals.advance(kPosition, *combat, kStart + 500 * kMillisecond);
  const auto stamina = combat->player_stats().at("72");
  const auto health = combat->player_stats().at("69");

  auto destination = kPosition;
  destination.map_id += 1;
  const auto arrived = kStart + 60000 * kMillisecond;
  vitals.continue_on_map(destination.map_id, arrived);
  vitals.advance(destination, *combat, arrived);
  EXPECT_EQ(combat->player_stats().at("72"), stamina);
  EXPECT_EQ(combat->player_stats().at("69"), health);
  EXPECT_TRUE(vitals.submerged());
  vitals.advance(destination, *combat, arrived + 500 * kMillisecond);
  EXPECT_LT(combat->player_stats().at("72").get<int>(), stamina.get<int>());
}

TEST(MovementVitals, SwimmingFlowStopsOnSurfacingAndRestoresRegen) {
  auto combat = movement_combat();
  MovementVitals vitals;
  const auto started = vitals.move(movement("MoveMode_Swimming", "MoveDetail_Run"),
                                    kPosition, *combat, kStart);
  ASSERT_EQ(started.size(), 1U);
  const auto instance = started.front().message.at("flows").at(0).at("instance_id");
  const auto stopped = vitals.move(movement("MoveMode_Swimming", "MoveDetail_SurfaceIdle"),
                                    kPosition, *combat, kStart + 1000 * kMillisecond);
  ASSERT_EQ(stopped.size(), 1U);
  const auto &flow = stopped.front().message.at("flows").at(0);
  EXPECT_EQ(flow.at("instance_id"), instance);
  EXPECT_FALSE(flow.contains("rate_per_sec"));
  EXPECT_EQ(flow.at("checkpoint").at("result"), 90);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  EXPECT_EQ(protocol.decode(protocol.encode(stopped.front().opcode, stopped.front().message))
                .at("status"), "decoded");
}

TEST(MovementVitals, ExhaustionKeepsRegenerationBlockedUntilSurfacing) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_Run"), kPosition, *combat, kStart);
  const auto exhausted = vitals.advance(kPosition, *combat, kStart + 10000 * kMillisecond);
  ASSERT_EQ(combat->player_stats().at("72"), 0);
  EXPECT_TRUE(exhausted.empty());
  combat->advance_player_vitals(kStart + 11000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 0);
  const auto surfaced = vitals.move(movement("MoveMode_Swimming", "MoveDetail_SurfaceIdle"),
                                   kPosition, *combat, kStart + 11000 * kMillisecond);
  ASSERT_EQ(surfaced.size(), 1U);
  EXPECT_NE(surfaced.front().message.at("regens").at(0).at("applied_regen_range_end_tick"),
            "9223372036854775807");
}

TEST(MovementVitals, EnteringWaterWithoutStaminaStillBlocksClientRegeneration) {
  auto combat = movement_combat();
  combat->change_movement_stamina(-100, "SCS_SprintFlow", kStart);
  MovementVitals vitals;
  const auto started = vitals.move(movement("MoveMode_Swimming", "MoveDetail_Run"),
                                   kPosition, *combat, kStart);
  ASSERT_EQ(started.size(), 1U);
  EXPECT_EQ(started.front().message.at("flows").at(0).at("checkpoint").at("result"), 0);
  EXPECT_EQ(started.front().message.at("regens").at(0).at("applied_regen_range_end_tick"),
            "9223372036854775807");
  const auto surfaced = vitals.move(movement("MoveMode_Swimming", "MoveDetail_SurfaceIdle"),
                                   kPosition, *combat, kStart + 1000 * kMillisecond);
  ASSERT_EQ(surfaced.size(), 1U);
  EXPECT_FALSE(surfaced.front().message.at("flows").at(0).contains("rate_per_sec"));
}

TEST(MovementVitals, EnablingInfiniteStaminaStopsAnExistingSwimmingFlow) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_Run"), kPosition, *combat, kStart);
  combat->set_gm_protection(false, false, true);
  const auto packets = vitals.advance(kPosition, *combat, kStart + 1000 * kMillisecond);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_FALSE(packets.front().message.at("flows").at(0).contains("rate_per_sec"));
  EXPECT_EQ(combat->player_stats().at("72"), 100);
}

TEST(MovementVitals, SurfaceFloatingIsSafeAndDoesNotConsumeStamina) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SurfaceIdle"), kPosition, *combat, kStart);
  EXPECT_TRUE(vitals.advance(kPosition, *combat, kStart + 10000 * kMillisecond).empty());
  EXPECT_EQ(combat->player_stats().at("72"), 100);
  EXPECT_FALSE(vitals.submerged());
  EXPECT_FALSE(vitals.next_action_tick());
}

TEST(MovementVitals, DivingUsesDrownedOriginAndHurtsWithoutFurtherMovement) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SW_Dive"), kPosition, *combat, kStart);
  EXPECT_TRUE(vitals.submerged());
  const auto packets = vitals.advance(kPosition, *combat, kStart + 1000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("69"), 950);
  EXPECT_EQ(combat->player_stats().at("72"), 90);
  const auto damage = std::ranges::find_if(packets, [](const GameplayPacket &packet) {
    return packet.opcode == 19;
  });
  ASSERT_NE(damage, packets.end());
  EXPECT_EQ(damage->message.at("skill_effect_origin_type"), "EX5SkillEffectOriginType_Drowned");
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : packets) {
    EXPECT_NO_THROW(protocol.encode(packet.opcode, packet.message));
  }
}

TEST(MovementVitals, StoppingOrSwimmingUpUnderwaterDoesNotPretendToSurface) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SW_Dive"), kPosition, *combat, kStart);
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_Stop"), kPosition, *combat,
              kStart + 100 * kMillisecond);
  vitals.advance(kPosition, *combat, kStart + 1000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("69"), 950);
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_Up"), kPosition, *combat,
              kStart + 1100 * kMillisecond);
  vitals.advance(kPosition, *combat, kStart + 2000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("69"), 900);
}

TEST(MovementVitals, SurfacingCancelsOverdueDrowningAndResetsTheNextDive) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SW_Dive"), kPosition, *combat, kStart);
  const auto surfaced = vitals.move(movement("MoveMode_Swimming", "MoveDetail_SurfaceIdle"),
                                    kPosition, *combat, kStart + 1500 * kMillisecond);
  EXPECT_FALSE(contains_opcode(surfaced, 19));
  EXPECT_EQ(combat->player_stats().at("69"), 1000);
  EXPECT_TRUE(vitals.advance(kPosition, *combat, kStart + 5000 * kMillisecond).empty());
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SW_Dive"), kPosition, *combat,
              kStart + 6000 * kMillisecond);
  EXPECT_FALSE(
      contains_opcode(vitals.advance(kPosition, *combat, kStart + 6999 * kMillisecond), 19));
  EXPECT_TRUE(
      contains_opcode(vitals.advance(kPosition, *combat, kStart + 7000 * kMillisecond), 19));
}

TEST(MovementVitals, LeavingWaterMapChangesAndResetCancelAllSubmergedDamage) {
  for (const int reset_kind : {0, 1, 2}) {
    auto combat = movement_combat();
    MovementVitals vitals;
    vitals.move(movement("MoveMode_Swimming", "MoveDetail_SW_Dive"), kPosition, *combat, kStart);
    WorldPosition position = kPosition;
    if (reset_kind == 0) {
      vitals.move(movement("MoveMode_Normal", "MoveDetail_Run"), position, *combat,
                  kStart + kMillisecond);
    } else if (reset_kind == 1) {
      position.map_id = 9012;
    } else {
      vitals.reset();
      combat->set_movement_recovery_blocked(false, false, kStart + kMillisecond);
    }
    const auto after_reset = vitals.advance(position, *combat, kStart + 10000 * kMillisecond);
    EXPECT_FALSE(contains_opcode(after_reset, 19));
    if (reset_kind == 1) {
      ASSERT_EQ(after_reset.size(), 1U);
      EXPECT_EQ(after_reset.front().opcode, 54);
      EXPECT_FALSE(after_reset.front().message.at("flows").at(0).contains("rate_per_sec"));
    }
    EXPECT_FALSE(vitals.submerged());
  }
}

TEST(MovementVitals, LethalDrowningUsesDeathCleanupAndDoesNotRepeatAfterDeathOrRevival) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SW_Dive"), kPosition, *combat, kStart);
  const auto packets = vitals.advance(kPosition, *combat, kStart + 20000 * kMillisecond);
  EXPECT_TRUE(combat->player_dead());
  EXPECT_TRUE(contains_opcode(packets, 32));
  EXPECT_TRUE(contains_opcode(packets, 19));
  EXPECT_TRUE(contains_opcode(packets, 23));
  const auto stopped_flow = std::ranges::find_if(packets, [](const GameplayPacket &packet) {
    return packet.opcode == 54 && packet.message.contains("flows") &&
           packet.message.at("flows").at(0).value("source", std::string()) == "SCS_SwimFlow";
  });
  ASSERT_NE(stopped_flow, packets.end());
  EXPECT_FALSE(stopped_flow->message.at("flows").at(0).contains("rate_per_sec"));
  EXPECT_FALSE(vitals.next_action_tick());
  EXPECT_TRUE(vitals.advance(kPosition, *combat, kStart + 21000 * kMillisecond).empty());
  combat->revive_if_dead(kStart + 22000 * kMillisecond);
  EXPECT_TRUE(vitals.advance(kPosition, *combat, kStart + 23000 * kMillisecond).empty());
  EXPECT_EQ(combat->player_stats().at("69"), 1000);
}

TEST(MovementVitals, SwimmingSuppressesStaminaRecoveryAndRestResumesIt) {
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_Run"), kPosition, *combat, kStart);
  vitals.advance(kPosition, *combat, kStart + 1000 * kMillisecond);
  combat->advance_player_vitals(kStart + 2000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 90);
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SurfaceIdle"), kPosition, *combat,
              kStart + 2000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 80);
  combat->advance_player_vitals(kStart + 3000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 90);
  combat->advance_player_vitals(kStart + 4000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 100);
}

TEST(MovementVitals, SubmergedHealthCannotRegenerateAndSurfacingDoesNotRefundBlockedTime) {
  auto combat = movement_combat(100, 10, 100);
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SW_Dive"), kPosition, *combat, kStart);
  vitals.advance(kPosition, *combat, kStart + 1000 * kMillisecond);
  combat->advance_player_vitals(kStart + 2000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("69"), 950);
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SurfaceIdle"), kPosition, *combat,
              kStart + 10000 * kMillisecond);
  combat->advance_player_vitals(kStart + 10000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("69"), 950);
  combat->advance_player_vitals(kStart + 11000 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("69"), 1000);
}

TEST(MovementVitals, InvalidRulesAndDecreasingOrUnrepresentableServerTimesAreRejected) {
  MovementVitalsRules rules;
  rules.swim_interval_ms = 0;
  EXPECT_THROW(MovementVitals{rules}, std::invalid_argument);
  rules = {};
  rules.sprint_cost = -1;
  EXPECT_THROW(MovementVitals{rules}, std::invalid_argument);
  auto combat = movement_combat();
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Normal", "MoveDetail_Run"), kPosition, *combat, kStart);
  EXPECT_THROW(vitals.advance(kPosition, *combat, kStart - 1), std::invalid_argument);
  EXPECT_THROW(vitals.advance(kPosition, *combat, std::numeric_limits<std::int64_t>::max()),
               std::invalid_argument);
  EXPECT_THROW(combat->change_movement_stamina(1, "SCS_SwimFlow", kStart), std::invalid_argument);
  EXPECT_THROW(combat->change_movement_stamina(-1, "SCS_Damage", kStart), std::invalid_argument);
}

TEST(MovementVitals, LongServerDelayClampsConsumptionAndDamageWithoutUnboundedCatchup) {
  auto combat = movement_combat(std::numeric_limits<int>::max());
  MovementVitals vitals;
  vitals.move(movement("MoveMode_Swimming", "MoveDetail_SW_Dive"), kPosition, *combat, kStart);
  const auto packets = vitals.advance(kPosition, *combat, kStart + 86400000LL * kMillisecond);
  EXPECT_TRUE(combat->player_dead());
  EXPECT_EQ(std::ranges::count_if(packets,
                                  [](const GameplayPacket &packet) {
                                    return packet.opcode == 19;
                                  }),
            1);
  EXPECT_LE(packets.size(), 8U);
}

// Gathering's SP-costed interaction flow shares these vitals. Captured herb start: 567 SP,
// cost 50 over 1646 ms (rate -30.377 SP/s), regen range ending 2906 ms after the start.
TEST(MovementVitals, InteractionStaminaFlowIsAnnouncedChargedOnceAndCancellable) {
  constexpr std::int64_t kWireStart = 293046113145200;
  auto combat = movement_combat(600, 10);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  EXPECT_FALSE(combat->begin_interaction_stamina(601, kWireStart, 1646, 2906, kStart));
  const auto begun = combat->begin_interaction_stamina(50, kWireStart, 1646, 2906, kStart);
  ASSERT_TRUE(begun.has_value());
  ASSERT_EQ(begun->size(), 1U);
  const auto &announcement = begun->front();
  EXPECT_EQ(announcement.opcode, 54);
  const auto &flow = announcement.message.at("flows").at(0);
  EXPECT_EQ(flow.at("source"), "SCS_InteractionFlow");
  EXPECT_EQ(flow.at("stat_type"), 72);
  EXPECT_NEAR(flow.at("rate_per_sec").get<double>(), -30.3766708, 1e-4);
  EXPECT_EQ(flow.at("end_tick"), std::to_string(kWireStart + 1646LL * 1000000));
  EXPECT_EQ(flow.at("checkpoint").at("result"), 600);
  EXPECT_EQ(announcement.message.at("regens").at(0).at("applied_regen_range_end_tick"),
            std::to_string(kWireStart + 2906LL * 1000000));
  const auto decoded = protocol.decode(protocol.encode(announcement.opcode, announcement.message));
  EXPECT_EQ(decoded.at("status"), "decoded");

  combat->spend_interaction_stamina(kStart + 1646 * kMillisecond);
  combat->spend_interaction_stamina(kStart + 1700 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 550);
  // Regeneration waits until the announced regen range ends.
  combat->advance_player_vitals(kStart + 2800 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 550);

  ASSERT_TRUE(combat->begin_interaction_stamina(50, kWireStart, 1646, 2906, kStart));
  const auto stopped = combat->cancel_interaction_stamina(kStart + 500 * kMillisecond);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_FALSE(stopped->message.at("flows").at(0).contains("rate_per_sec"));
  EXPECT_FALSE(combat->cancel_interaction_stamina(kStart + 600 * kMillisecond).has_value());
  combat->spend_interaction_stamina(kStart + 1646 * kMillisecond);
  EXPECT_EQ(combat->player_stats().at("72"), 550);
  EXPECT_THROW(combat->begin_interaction_stamina(0, kWireStart, 1646, 2906, kStart),
               std::invalid_argument);
}

} // namespace
} // namespace chronicles
