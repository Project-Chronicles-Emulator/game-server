#include "world/features/combat/combat_rules.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using combat_rules::AbnormalType;

constexpr std::int64_t kMillisecond = 10000;

Json attacker(int physical_attack, int magic_attack) {
  return {{"53", physical_attack}, {"48", magic_attack}};
}

Json armored(int physical_armor, int magic_armor) {
  return {{"52", physical_armor}, {"47", magic_armor}};
}

} // namespace

// Official hits from the open-world capture: a Wild bow skill (effect 140913) at
// PhysicalAttackPower 39 / MagicAttackPower 14 on a target with 28 armor dealt 1084, and a Fight
// sword skill (effect 1100006) at 17/14 on 16 armor dealt 557.
TEST(CombatRules, PlayerDamageReproducesOfficialHits) {
  const Json bow{{"physicalCoefficient", 3.667999}, {"magicCoefficient", 2.821}};
  EXPECT_EQ(combat_rules::player_damage(bow, attacker(39, 14), armored(28, 28)), 1084);

  const Json sword{{"physicalCoefficient", 2.963999}, {"magicCoefficient", 2.822}};
  EXPECT_EQ(combat_rules::player_damage(sword, attacker(17, 14), armored(16, 16)), 557);
}

// Official NPC hits: NPC 5001104 (37/37, coefficient 1) on a player with 62/47 armor dealt 41,
// and NPC 5001503 (55/7) on 137/77 armor dealt 20.
TEST(CombatRules, NpcDamageReproducesOfficialHits) {
  const Json strike{{"physicalCoefficient", 1.0}, {"magicCoefficient", 1.0}};
  EXPECT_EQ(combat_rules::npc_damage(strike, attacker(37, 37), armored(62, 47)), 41);
  EXPECT_EQ(combat_rules::npc_damage(strike, attacker(55, 7), armored(137, 77)), 20);
}

TEST(CombatRules, ArmorAndAttackModifiersChangeDamage) {
  const Json skill{{"physicalCoefficient", 2.0}};
  const int base = combat_rules::player_damage(skill, attacker(40, 0), armored(30, 30));
  // An attack elixir (+50 PhysicalAttackPower) and an armor-break debuff both raise the hit.
  EXPECT_GT(combat_rules::player_damage(skill, attacker(90, 0), armored(30, 30)), base + 500);
  EXPECT_EQ(combat_rules::player_damage(skill, attacker(40, 0), armored(0, 0)), base + 18);
  // Armor below zero counts as zero, as the Stat table minimum says.
  EXPECT_EQ(combat_rules::player_damage(skill, attacker(40, 0), armored(-70, -70)),
            combat_rules::player_damage(skill, attacker(40, 0), armored(0, 0)));
  // Higher player armor lowers an NPC hit.
  const Json strike{{"physicalCoefficient", 1.0}};
  EXPECT_LT(combat_rules::npc_damage(strike, attacker(50, 0), armored(162, 0)),
            combat_rules::npc_damage(strike, attacker(50, 0), armored(62, 0)));
}

TEST(CombatRules, FixedDamageIsUnmitigatedAndZeroCoefficientHitsDealNothing) {
  // Official: the throwing dagger's 2120 fixed damage landed unchanged on armored NPCs.
  const Json dagger{{"fixedDamage", 2120}, {"physicalCoefficient", 0.0}};
  EXPECT_EQ(combat_rules::player_damage(dagger, attacker(39, 14), armored(37, 37)), 2120);
  // A balm's elemental interval hit has no coefficient: it carries only its debuff.
  const Json debuff_only{{"physicalCoefficient", 0.0}, {"magicCoefficient", 0.0}};
  EXPECT_EQ(combat_rules::player_damage(debuff_only, attacker(39, 14), armored(0, 0)), 0);
}

TEST(CombatRules, OutgoingAndIncomingRatesScaleTheHit) {
  const Json skill{{"physicalCoefficient", 2.0}};
  const int base = combat_rules::player_damage(skill, attacker(40, 0), armored(0, 0));
  auto boosted = attacker(40, 0);
  boosted["88"] = 200;
  EXPECT_NEAR(combat_rules::player_damage(skill, boosted, armored(0, 0)), base * 1.2, 1.0);
  auto shielded = armored(0, 0);
  shielded["77"] = 1000;
  EXPECT_EQ(combat_rules::player_damage(skill, attacker(40, 0), shielded), 0);
}

TEST(CombatRules, LegacyModelIsKeptForStatsWithoutAttackRatings) {
  EXPECT_FALSE(combat_rules::has_attack_ratings(Json{{"22", 60}}));
  EXPECT_TRUE(combat_rules::has_attack_ratings(attacker(1, 0)));
  const Json skill{{"physicalCoefficient", 1.0}, {"fixedDamage", 5}};
  EXPECT_EQ(combat_rules::legacy_damage(skill, Json{{"22", 60}}), 65);
}

// The client shows statMul as floor(statMul x 0.01) x 0.1 percent: 10000 is +10%.
TEST(CombatRules, StatMultiplierUsesThousandthsOfAPercent) {
  EXPECT_EQ(combat_rules::stat_multiplier_delta(100, 10000), 10);
  EXPECT_EQ(combat_rules::stat_multiplier_delta(1000, 200), 2);
  EXPECT_EQ(combat_rules::stat_multiplier_delta(300, -100000), -300);
  EXPECT_EQ(combat_rules::stat_multiplier_delta(39, 200), 0);
}

// Official bleeding gauge: 10256 at one update, 13 decay ticks of 48 later the next hit started
// from 9632.
TEST(CombatRules, GaugesDecayLikeTheClientFromTheirUpdateTick) {
  const auto rules = combat_rules::AbnormalGaugeRules::parse(Json::parse(R"({
    "reduce_tick_ms": 100,
    "types": {"Bleeding": {"maximum": 20000, "reduce_per_tick": 48, "status_effect_id": 15}}
  })"));
  combat_rules::AbnormalGauges gauges;
  gauges.set(AbnormalType::bleeding, 10256, 0);
  EXPECT_EQ(gauges.value(AbnormalType::bleeding, rules, 99 * kMillisecond), 10256);
  EXPECT_EQ(gauges.value(AbnormalType::bleeding, rules, 1341 * kMillisecond), 9632);
  EXPECT_EQ(gauges.value(AbnormalType::bleeding, rules, 60000 * kMillisecond), 0);
  EXPECT_EQ(gauges.value(AbnormalType::burning, rules, 0), 0);

  EXPECT_THROW(combat_rules::AbnormalGaugeRules::parse(
                   Json::parse(R"({"reduce_tick_ms": 0, "types": {}})")),
               std::runtime_error);
  EXPECT_THROW(combat_rules::AbnormalGaugeRules::parse(
                   Json::parse(R"({"reduce_tick_ms": 100, "types": {"Wet": {}}})")),
               std::runtime_error);
}

TEST(CombatRules, AbnormalParametersAreReadFromCatalogKeys) {
  const Json bleed_hit{{"physicalCoefficient", 1.0}, {"abnormalStatusBleedingAttack", 2700.0}};
  const auto attacks = combat_rules::abnormal_attacks(bleed_hit);
  ASSERT_EQ(attacks.size(), 1u);
  EXPECT_EQ(attacks.front(), std::make_pair(AbnormalType::bleeding, 2700));

  const Json cure{{"burningAdd", -20000.0}, {"poisoningAdd", -20000.0}};
  const auto adds = combat_rules::abnormal_gauge_adds(cure);
  ASSERT_EQ(adds.size(), 2u);
  EXPECT_EQ(adds.at(0), std::make_pair(AbnormalType::burning, -20000));
  EXPECT_EQ(adds.at(1), std::make_pair(AbnormalType::poisoning, -20000));
  EXPECT_EQ(combat_rules::abnormal_type_wire_name(AbnormalType::electric_shock),
            "EX5AbnormalStatusType_ElectricShock");
}

} // namespace chronicles
