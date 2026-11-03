#include "data/client_catalog.hpp"
#include "protocol/protocol.hpp"
#include "world/features/inventory/inventory_capacity.hpp"
#include "world/features/inventory/inventory_encumbrance.hpp"
#include "world/features/inventory/tests/encumbrance_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr int kMoveSpeedRate = 84;
constexpr int kNeutralMoveSpeedRate = 1000;
constexpr int kHeavyStatus = 50030004;
constexpr int kVeryHeavyStatus = 50030005;
constexpr std::int64_t kStarterBagSafeWeight = 135000;
constexpr std::int64_t kDefaultSafeWeight = 50000;
constexpr std::uint64_t kPlayerUnitId = 4242;
constexpr std::uint32_t kFirstInstanceId = 0x4000'0000U;
constexpr std::uint16_t kChangeStat = 47;
constexpr std::uint16_t kAddStatusEffect = 49;
constexpr std::uint16_t kRemoveStatusEffect = 51;

// Rows with the values of the exported demo catalog (GameVariable, StatusEffect 50030004 and
// 50030005, their MaintainEffect/MaintainStat rows and the MoveSpeedRate Stat limit).
using inventory_testing::catalog_rows;

std::shared_ptr<const EncumbranceRules> catalog_rules() {
  auto rules = EncumbranceRules::parse(catalog_rows());
  if (!rules) {
    throw std::runtime_error("Test catalog rows produced no encumbrance rules");
  }
  return rules;
}

Json neutral_stats() {
  return Json{{std::to_string(kMoveSpeedRate), kNeutralMoveSpeedRate}, {"18", 900}};
}

void set_game_variable(EncumbranceCatalogRows &rows, const std::string &name,
                       const std::string &value) {
  for (auto &row : rows.game_variables) {
    if (row.at("name") == name) {
      row["value"] = value;
      return;
    }
  }
  rows.game_variables.push_back({{"name", name}, {"value", value}});
}

void erase_game_variable(EncumbranceCatalogRows &rows, const std::string &name) {
  std::erase_if(rows.game_variables, [&](const Json &row) {
    return row.at("name") == name;
  });
}

TEST(InventoryEncumbrance, ParsesCatalogTiersAndThresholds) {
  const auto rules = catalog_rules();

  EXPECT_EQ(rules->hard_ceiling_percent(), 200);
  EXPECT_EQ(rules->very_heavy_from_permille(), 500);
  EXPECT_EQ(rules->tier(EncumbranceLevel::none), nullptr);

  const auto *heavy = rules->tier(EncumbranceLevel::heavy);
  ASSERT_NE(heavy, nullptr);
  EXPECT_EQ(heavy->status_effect_id, kHeavyStatus);
  ASSERT_EQ(heavy->modifiers.size(), 1U);
  EXPECT_EQ(heavy->modifiers.front().stat_type, kMoveSpeedRate);
  EXPECT_EQ(heavy->modifiers.front().add, -500);

  const auto *very_heavy = rules->tier(EncumbranceLevel::very_heavy);
  ASSERT_NE(very_heavy, nullptr);
  EXPECT_EQ(very_heavy->status_effect_id, kVeryHeavyStatus);
  ASSERT_EQ(very_heavy->modifiers.size(), 1U);
  EXPECT_EQ(very_heavy->modifiers.front().add, -1500);
}

TEST(InventoryEncumbrance, ClassifiesAgainstSafeWeightAndOverweightBand) {
  const auto rules = catalog_rules();

  // Starter bag: safe 135000, ceiling 270000, VeryHeavy from half the band (202500).
  EXPECT_EQ(rules->classify(0, kStarterBagSafeWeight), EncumbranceLevel::none);
  EXPECT_EQ(rules->classify(kStarterBagSafeWeight, kStarterBagSafeWeight),
            EncumbranceLevel::none);
  EXPECT_EQ(rules->classify(kStarterBagSafeWeight + 1, kStarterBagSafeWeight),
            EncumbranceLevel::heavy);
  EXPECT_EQ(rules->classify(202'499, kStarterBagSafeWeight), EncumbranceLevel::heavy);
  EXPECT_EQ(rules->classify(202'500, kStarterBagSafeWeight), EncumbranceLevel::very_heavy);
  EXPECT_EQ(rules->classify(270'000, kStarterBagSafeWeight), EncumbranceLevel::very_heavy);
  EXPECT_EQ(rules->classify(std::numeric_limits<std::int64_t>::max(), kStarterBagSafeWeight),
            EncumbranceLevel::very_heavy);

  // No bag: the default safe weight 50000 moves both thresholds down.
  EXPECT_EQ(rules->classify(60'000, kDefaultSafeWeight), EncumbranceLevel::heavy);
  EXPECT_EQ(rules->classify(75'000, kDefaultSafeWeight), EncumbranceLevel::very_heavy);
}

TEST(InventoryEncumbrance, ClassifyHandlesExtremeSafeWeightsWithoutOverflow) {
  const auto rules = catalog_rules();
  const auto huge_safe_weight = std::numeric_limits<std::int64_t>::max() / 2;

  EXPECT_EQ(rules->classify(huge_safe_weight, huge_safe_weight), EncumbranceLevel::none);
  EXPECT_EQ(rules->classify(huge_safe_weight + 1, huge_safe_weight), EncumbranceLevel::heavy);
  EXPECT_EQ(rules->classify(std::numeric_limits<std::int64_t>::max(), huge_safe_weight),
            EncumbranceLevel::very_heavy);
}

TEST(InventoryEncumbrance, ClassifyRejectsNegativeWeights) {
  const auto rules = catalog_rules();

  EXPECT_THROW(rules->classify(-1, kStarterBagSafeWeight), std::invalid_argument);
  EXPECT_THROW(rules->classify(10, -1), std::invalid_argument);
}

TEST(InventoryEncumbrance, AppliesModifiersClampedToStatLimits) {
  const auto rules = catalog_rules();
  const auto key = std::to_string(kMoveSpeedRate);

  auto unencumbered = neutral_stats();
  rules->apply_modifiers(EncumbranceLevel::none, unencumbered);
  EXPECT_EQ(unencumbered.at(key), kNeutralMoveSpeedRate);

  auto heavy = neutral_stats();
  rules->apply_modifiers(EncumbranceLevel::heavy, heavy);
  EXPECT_EQ(heavy.at(key), 500);
  EXPECT_EQ(heavy.at("18"), 900);

  // 1000 - 1500 is below the catalog minimum of MoveSpeedRate, so it clamps to 200.
  auto very_heavy = neutral_stats();
  rules->apply_modifiers(EncumbranceLevel::very_heavy, very_heavy);
  EXPECT_EQ(very_heavy.at(key), 200);

  // A stat the caller did not compute is not invented.
  Json without_speed{{"18", 900}};
  rules->apply_modifiers(EncumbranceLevel::heavy, without_speed);
  EXPECT_FALSE(without_speed.contains(key));
}

TEST(InventoryEncumbrance, CatalogWithoutWeightStatusesDisablesTheRules) {
  auto rows = catalog_rows();
  erase_game_variable(rows, "StatusEffectIdForInvWeightHeavy");
  erase_game_variable(rows, "StatusEffectIdForInvWeightVeryHeavy");

  EXPECT_EQ(EncumbranceRules::parse(rows), nullptr);
}

TEST(InventoryEncumbrance, RejectsIncompleteOrMalformedCatalogRows) {
  auto only_heavy = catalog_rows();
  erase_game_variable(only_heavy, "StatusEffectIdForInvWeightVeryHeavy");
  EXPECT_THROW(EncumbranceRules::parse(only_heavy), std::runtime_error);

  auto missing_ceiling = catalog_rows();
  erase_game_variable(missing_ceiling, "BagMaxWeightMultiplier");
  EXPECT_THROW(EncumbranceRules::parse(missing_ceiling), std::runtime_error);

  auto ceiling_below_safe = catalog_rows();
  set_game_variable(ceiling_below_safe, "BagMaxWeightMultiplier", "50");
  EXPECT_THROW(EncumbranceRules::parse(ceiling_below_safe), std::runtime_error);

  auto bad_ratio = catalog_rows();
  set_game_variable(bad_ratio, "InvWeightMoveSpeedStep2RatioFrom", "1.5");
  EXPECT_THROW(EncumbranceRules::parse(bad_ratio), std::runtime_error);

  auto missing_status = catalog_rows();
  missing_status.status_effects.pop_back();
  EXPECT_THROW(EncumbranceRules::parse(missing_status), std::runtime_error);

  auto multiplier_modifier = catalog_rows();
  multiplier_modifier.maintain_stats.front()["statMul"] = "-100";
  EXPECT_THROW(EncumbranceRules::parse(multiplier_modifier), std::runtime_error);

  auto unknown_stat = catalog_rows();
  unknown_stat.maintain_stats.front()["statType"] = "NotAStat";
  EXPECT_THROW(EncumbranceRules::parse(unknown_stat), std::runtime_error);

  auto missing_limit = catalog_rows();
  std::erase_if(missing_limit.stats, [](const Json &row) {
    return row.at("id") == "MoveSpeedRate";
  });
  EXPECT_THROW(EncumbranceRules::parse(missing_limit), std::invalid_argument);
}

TEST(InventoryEncumbrance, StateRecordsEachTierChangeOnce) {
  EncumbranceState state(catalog_rules());
  EXPECT_EQ(state.level(), EncumbranceLevel::none);
  EXPECT_FALSE(state.change_level(EncumbranceLevel::none, 10).has_value());

  const auto to_heavy = state.change_level(EncumbranceLevel::heavy, 20);
  ASSERT_TRUE(to_heavy.has_value());
  EXPECT_EQ(to_heavy->previous, EncumbranceLevel::none);
  EXPECT_EQ(to_heavy->current, EncumbranceLevel::heavy);
  EXPECT_FALSE(to_heavy->removed.has_value());
  ASSERT_TRUE(to_heavy->added.has_value());
  EXPECT_EQ(to_heavy->added->status_effect_id, kHeavyStatus);
  EXPECT_EQ(to_heavy->added->instance_id, kFirstInstanceId);
  EXPECT_EQ(to_heavy->added->start_tick, 20);
  EXPECT_EQ(to_heavy->changed_stat_types, std::vector<int>{kMoveSpeedRate});
  EXPECT_FALSE(state.change_level(EncumbranceLevel::heavy, 25).has_value());

  const auto to_very_heavy = state.change_level(EncumbranceLevel::very_heavy, 30);
  ASSERT_TRUE(to_very_heavy.has_value());
  ASSERT_TRUE(to_very_heavy->removed.has_value());
  EXPECT_EQ(to_very_heavy->removed->status_effect_id, kHeavyStatus);
  EXPECT_EQ(to_very_heavy->removed->instance_id, kFirstInstanceId);
  ASSERT_TRUE(to_very_heavy->added.has_value());
  EXPECT_EQ(to_very_heavy->added->status_effect_id, kVeryHeavyStatus);
  EXPECT_EQ(to_very_heavy->added->instance_id, kFirstInstanceId + 1);

  const auto to_none = state.change_level(EncumbranceLevel::none, 40);
  ASSERT_TRUE(to_none.has_value());
  ASSERT_TRUE(to_none->removed.has_value());
  EXPECT_EQ(to_none->removed->status_effect_id, kVeryHeavyStatus);
  EXPECT_FALSE(to_none->added.has_value());
  EXPECT_EQ(state.level(), EncumbranceLevel::none);
}

TEST(InventoryEncumbrance, StateAppliesOnlyTheCurrentTier) {
  EncumbranceState state(catalog_rules());
  const auto key = std::to_string(kMoveSpeedRate);

  state.change_level(EncumbranceLevel::very_heavy, 1);
  auto encumbered = neutral_stats();
  state.apply_to(encumbered);
  EXPECT_EQ(encumbered.at(key), 200);

  state.change_level(EncumbranceLevel::none, 2);
  auto recovered = neutral_stats();
  state.apply_to(recovered);
  EXPECT_EQ(recovered.at(key), kNeutralMoveSpeedRate);
}

TEST(InventoryEncumbrance, TransitionPacketsReportStatThenStatusChanges) {
  EncumbranceState state(catalog_rules());
  constexpr std::int64_t kTick = 5'000;

  const auto to_heavy = state.change_level(EncumbranceLevel::heavy, kTick);
  ASSERT_TRUE(to_heavy.has_value());
  auto heavy_stats = neutral_stats();
  state.apply_to(heavy_stats);
  const auto heavy_packets = encumbrance_transition_packets(*to_heavy, kPlayerUnitId, heavy_stats);

  ASSERT_EQ(heavy_packets.size(), 2U);
  EXPECT_EQ(heavy_packets[0].opcode, kChangeStat);
  EXPECT_EQ(heavy_packets[0].message.at("unit_id"), std::to_string(kPlayerUnitId));
  EXPECT_EQ(heavy_packets[0].message.at("default_source"), "SCS_StatusEffect");
  EXPECT_EQ(heavy_packets[0].message.at("stat_changes"),
            (Json::array({{{"stat_type", kMoveSpeedRate}, {"result", 500}}})));
  EXPECT_EQ(heavy_packets[1].opcode, kAddStatusEffect);
  const auto &added = heavy_packets[1].message.at("status_effect");
  EXPECT_EQ(added.at("status_effect_id"), kHeavyStatus);
  EXPECT_EQ(added.at("instance_id"), kFirstInstanceId);
  EXPECT_EQ(added.at("caster_unit_id"), std::to_string(kPlayerUnitId));
  EXPECT_EQ(added.at("start_tick"), std::to_string(kTick * 100));
  EXPECT_EQ(added.at("expire_tick"), "9223372036854775807");

  const auto to_none = state.change_level(EncumbranceLevel::none, kTick + 1);
  ASSERT_TRUE(to_none.has_value());
  const auto recovered_packets =
      encumbrance_transition_packets(*to_none, kPlayerUnitId, neutral_stats());

  ASSERT_EQ(recovered_packets.size(), 2U);
  EXPECT_EQ(recovered_packets[0].opcode, kChangeStat);
  EXPECT_EQ(recovered_packets[0].message.at("stat_changes"),
            (Json::array({{{"stat_type", kMoveSpeedRate}, {"result", kNeutralMoveSpeedRate}}})));
  EXPECT_EQ(recovered_packets[1].opcode, kRemoveStatusEffect);
  EXPECT_EQ(recovered_packets[1].message.at("status_effects"),
            (Json::array({{{"instance_id", kFirstInstanceId},
                           {"status_effect_id", kHeavyStatus}}})));
}

TEST(InventoryEncumbrance, TransitionPacketsSkipStatsTheCallerCannotReport) {
  EncumbranceState state(catalog_rules());
  const auto transition = state.change_level(EncumbranceLevel::heavy, 1);
  ASSERT_TRUE(transition.has_value());

  const auto packets = encumbrance_transition_packets(*transition, kPlayerUnitId, Json::object());

  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets.front().opcode, kAddStatusEffect);
}

TEST(InventoryEncumbrance, TransitionPacketsEncodeWithRecoveredSchemas) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  EncumbranceState state(catalog_rules());

  std::vector<GameplayPacket> packets;
  for (const auto level :
       {EncumbranceLevel::heavy, EncumbranceLevel::very_heavy, EncumbranceLevel::none}) {
    const auto transition = state.change_level(level, 1'000);
    ASSERT_TRUE(transition.has_value());

    auto stats = neutral_stats();
    state.apply_to(stats);
    const auto step = encumbrance_transition_packets(*transition, kPlayerUnitId, stats);
    packets.insert(packets.end(), step.begin(), step.end());
  }

  ASSERT_EQ(packets.size(), 7U);
  for (const auto &packet : packets) {
    const auto frame = protocol.encode(packet.opcode, packet.message);
    const auto decoded = protocol.decode(frame).at("message");
    EXPECT_EQ(decoded.at("unit_id"), std::to_string(kPlayerUnitId));
  }

  const auto very_heavy_add =
      protocol.decode(protocol.encode(packets[4].opcode, packets[4].message));
  EXPECT_EQ(very_heavy_add.at("message").at("status_effect").at("status_effect_id"),
            kVeryHeavyStatus);
  EXPECT_EQ(very_heavy_add.at("message").at("status_effect").at("expire_tick"),
            "9223372036854775807");
}

// The exported demo catalog must produce the tiers the evidence record describes.
TEST(InventoryEncumbrance, ExportedCatalogMatchesTheEvidenceRecord) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }

  ClientCatalog catalog(catalog_path);
  const auto rules = EncumbranceRules::load(catalog);
  ASSERT_NE(rules, nullptr);
  EXPECT_EQ(rules->hard_ceiling_percent(), 200);
  EXPECT_EQ(rules->very_heavy_from_permille(), 500);
  EXPECT_EQ(rules->tier(EncumbranceLevel::heavy)->status_effect_id, kHeavyStatus);
  EXPECT_EQ(rules->tier(EncumbranceLevel::very_heavy)->status_effect_id, kVeryHeavyStatus);

  auto stats = neutral_stats();
  rules->apply_modifiers(EncumbranceLevel::heavy, stats);
  EXPECT_EQ(stats.at(std::to_string(kMoveSpeedRate)), 500);
}

// The weights the real catalog gives real items must move a real character between tiers:
// the carried weight comes from InventoryCapacity, exactly as the world session measures it.
TEST(InventoryEncumbrance, RealCatalogItemWeightsMoveACharacterBetweenTiers) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }

  ClientCatalog catalog(catalog_path);
  const auto rules = EncumbranceRules::load(catalog);
  ASSERT_NE(rules, nullptr);
  const InventoryCapacity capacity(catalog);

  // A Fight starter loadout with the starter bag worn (safeWeight 135000).
  constexpr int kStarterBag = 11700201;
  constexpr int kBagSlot = 24;
  constexpr int kStarterWeapon = 11101102;
  // A catalog consumable weighing 10 per unit.
  constexpr int kConsumable = 40101101;

  Character character;
  character.id = 7;
  std::int64_t next_item_id = 1;
  const auto wear = [&](int cms_id, std::optional<int> slot) {
    CharacterItem item;
    item.id = next_item_id++;
    item.cms_id = cms_id;
    item.equip_slot = slot;
    character.items.push_back(item);
  };
  wear(kStarterBag, kBagSlot);
  wear(kStarterWeapon, std::nullopt);

  WorldProgress progress;
  const auto tier_with_consumables = [&](int count) {
    progress.stackables[kConsumable] = count;
    const auto carried = capacity.measure(character, progress);
    return std::pair{carried, rules->classify(carried.total, carried.safe)};
  };

  const auto [light, light_tier] = tier_with_consumables(1);
  EXPECT_EQ(light.safe, 135000);
  EXPECT_LT(light.total, light.safe);
  EXPECT_EQ(light_tier, EncumbranceLevel::none);

  // Each unit weighs 10, so a large stack is what pushes this loadout over the safe weight.
  const auto [heavy, heavy_tier] = tier_with_consumables(14000);
  EXPECT_GT(heavy.total, heavy.safe);
  EXPECT_EQ(heavy_tier, EncumbranceLevel::heavy);

  const auto [very_heavy, very_heavy_tier] = tier_with_consumables(21000);
  EXPECT_GT(very_heavy.total, 202'500);
  EXPECT_EQ(very_heavy_tier, EncumbranceLevel::very_heavy);

  // Dropping the stack again restores full speed, which is what the player feels.
  const auto [unloaded, unloaded_tier] = tier_with_consumables(0);
  EXPECT_LT(unloaded.total, unloaded.safe);
  EXPECT_EQ(unloaded_tier, EncumbranceLevel::none);

  auto stats = neutral_stats();
  rules->apply_modifiers(unloaded_tier, stats);
  EXPECT_EQ(stats.at(std::to_string(kMoveSpeedRate)), kNeutralMoveSpeedRate);
}

} // namespace
} // namespace chronicles
