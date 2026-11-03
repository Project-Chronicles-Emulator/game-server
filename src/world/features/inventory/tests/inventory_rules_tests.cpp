#include "world/features/inventory/inventory_rules.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>

namespace chronicles {
namespace {

TEST(InventoryRules, BareUnequipProducesVacatedUpdateAndVisualClear) {
    CharacterItem item{};
    item.id = 12345;
    item.equip_slot = 23;

    std::vector<CharacterItem> items{item};
    VisualSlotClear visual{};
    const auto update = InventoryRules::bare_unequip(items, 23, visual);

    EXPECT_EQ(update.item_id, 12345);
    EXPECT_FALSE(update.equip_slot.has_value());
    EXPECT_TRUE(update.vacated);
    EXPECT_EQ(visual.slot, 23);
    EXPECT_TRUE(visual.cleared);
}

TEST(InventoryRules, AbsentSlotDoesNotClearEquipment) {
    CharacterItem item{};
    item.id = 12345;
    item.equip_slot = 23;

    std::vector<CharacterItem> items{item};
    VisualSlotClear visual{};
    const auto update = InventoryRules::bare_unequip(items, 22, visual);

    EXPECT_FALSE(update.vacated);
    EXPECT_FALSE(visual.cleared);
}

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

TEST(InventoryRulesWeight, ZeroItemsUsesDefaultSafeWeight) {
    const WeightStatus status = InventoryRules::compute_weight({}, 50000, 0);

    EXPECT_EQ(status.carried_weight, 0);
    EXPECT_EQ(status.safe_capacity, 50000);
    EXPECT_EQ(status.free_weight, 50000);
}

TEST(InventoryRulesWeight, ZeroQuantityAddsNothing) {
    const WeightStatus status = InventoryRules::compute_weight({{1500, 0}}, 50000, 0);

    EXPECT_EQ(status.carried_weight, 0);
}

TEST(InventoryRulesWeight, DefaultBranchSumsLines) {
    const WeightStatus status =
        InventoryRules::compute_weight({{10, 5}, {1500, 1}}, 50000, 0);

    EXPECT_EQ(status.carried_weight, 1550);
    EXPECT_EQ(status.safe_capacity, 50000);
    EXPECT_EQ(status.free_weight, 48450);
}

TEST(InventoryRulesWeight, BagBranchReplacesDefaultInsteadOfAdding) {
    const WeightStatus status = InventoryRules::compute_weight({{10, 3}}, 50000, 200000);

    EXPECT_EQ(status.safe_capacity, 200000);
    EXPECT_EQ(status.free_weight, 200000 - 30);
}

TEST(InventoryRulesWeight, OverCapacityYieldsNegativeFreeWeightWithoutRefusal) {
    const WeightStatus status = InventoryRules::compute_weight({{1000, 100}}, 50000, 0);

    EXPECT_EQ(status.carried_weight, 100000);
    EXPECT_EQ(status.free_weight, -50000);
}

TEST(InventoryRulesWeight, RejectsNegativeInputs) {
    EXPECT_THROW(InventoryRules::compute_weight({{-1, 1}}, 50000, 0), std::invalid_argument);
    EXPECT_THROW(InventoryRules::compute_weight({{1, -1}}, 50000, 0), std::invalid_argument);
    EXPECT_THROW(InventoryRules::compute_weight({}, -1, 0), std::invalid_argument);
    EXPECT_THROW(InventoryRules::compute_weight({}, 50000, -1), std::invalid_argument);
}

TEST(InventoryRulesWeight, ExactInt64ProductIsRepresentable) {
    const WeightStatus status = InventoryRules::compute_weight({{kInt64Max, 1}}, 0, 0);

    EXPECT_EQ(status.carried_weight, kInt64Max);
    EXPECT_EQ(status.free_weight, -kInt64Max);
}

TEST(InventoryRulesWeight, ProductOverflowThrows) {
    EXPECT_THROW(
        InventoryRules::compute_weight({{kInt64Max / 2 + 1, 2}}, 50000, 0),
        std::overflow_error);
}

TEST(InventoryRulesWeight, SumOverflowThrows) {
    EXPECT_THROW(
        InventoryRules::compute_weight({{kInt64Max, 1}, {1, 1}}, 50000, 0),
        std::overflow_error);
}

// Rows copied from Catalog/Tables/ItemPolish.jsonl; every plan below reproduces one observed
// ItemPolish exchange (cost and material count; balances here are synthetic).
constexpr PolishRequirement kGrandWeaponStep1{10000, 0, 0, 90211101, 1, 1000};
constexpr PolishRequirement kNormalWeaponStep1{10000, 0, 0, 90211101, 1, 200};
constexpr PolishRequirement kGrandWeaponStep2{6000, 4000, 0, 90211101, 2, 1425};
constexpr PolishRequirement kGrandArmorStep1{10000, 0, 0, 90211201, 1, 1000};

TEST(InventoryRulesPolish, GrandWeaponFirstStepChargesCatalogCostAndOneMaterial) {
    const auto plan = InventoryRules::plan_polish(kGrandWeaponStep1, 0, {{90211101, 2}}, 5000);

    EXPECT_EQ(plan.status, PolishPlanStatus::Planned);
    EXPECT_EQ(plan.new_polish_step, 1);
    EXPECT_EQ(plan.material_cms_id, 90211101);
    EXPECT_EQ(plan.material_count, 1);
    EXPECT_EQ(plan.material_remaining, 1);
    EXPECT_EQ(plan.gold_cost, 1000);
    EXPECT_EQ(plan.gold_remaining, 4000);
}

TEST(InventoryRulesPolish, NormalWeaponUsesTheCheaperRowAndConsumesTheLastMaterial) {
    const auto plan = InventoryRules::plan_polish(kNormalWeaponStep1, 0, {{90211101, 1}}, 200);

    EXPECT_EQ(plan.status, PolishPlanStatus::Planned);
    EXPECT_EQ(plan.gold_cost, 200);
    EXPECT_EQ(plan.gold_remaining, 0);
    EXPECT_EQ(plan.material_remaining, 0);
}

TEST(InventoryRulesPolish, ThreeArmorPiecesDrainThreeMaterialsOneAtATime) {
    std::map<int, int> stackables{{90211201, 3}};
    std::int64_t gold = 3000;
    for (const int expected_remaining : {2, 1, 0}) {
        const auto plan = InventoryRules::plan_polish(kGrandArmorStep1, 0, stackables, gold);
        ASSERT_EQ(plan.status, PolishPlanStatus::Planned);
        EXPECT_EQ(plan.material_remaining, expected_remaining);
        stackables[90211201] = plan.material_remaining;
        gold = plan.gold_remaining;
    }
    EXPECT_EQ(gold, 0);
}

TEST(InventoryRulesPolish, ChanceBasedStepIsPlannedWithTheCatalogChancesAndFullCharge) {
    const auto plan =
        InventoryRules::plan_polish(kGrandWeaponStep2, 1, {{90211101, 2}}, 5000);

    EXPECT_EQ(plan.status, PolishPlanStatus::Planned);
    EXPECT_EQ(plan.new_polish_step, 2);
    EXPECT_EQ(plan.chances.success, 6000);
    EXPECT_EQ(plan.chances.fail, 4000);
    EXPECT_EQ(plan.chances.destruction, 0);
    EXPECT_EQ(plan.material_count, 2);
    EXPECT_EQ(plan.material_remaining, 0);
    EXPECT_EQ(plan.gold_cost, 1425);
    EXPECT_EQ(plan.additive_cms_id, 0);
}

// Additive rows copied from Catalog/Tables/Item.jsonl (PolishingMaterial with an effect).
constexpr PolishAdditive kReinforcedAdditive{
    90351001, PolishAdditiveEffect::AddSuccessPercent, 10};
constexpr PolishAdditive kMysticalAdditive{
    90551001, PolishAdditiveEffect::AddSuccessPercent, 100};
constexpr PolishAdditive kSafeguardAdditive{
    90552001, PolishAdditiveEffect::DestructionProhibition, 1};

TEST(InventoryRulesPolish, AdditiveIsChargedOnceAndShiftsTheChances) {
    const auto plan = InventoryRules::plan_polish(kGrandWeaponStep2, 1,
                                                  {{90211101, 2}, {90351001, 3}}, 5000,
                                                  &kReinforcedAdditive);

    ASSERT_EQ(plan.status, PolishPlanStatus::Planned);
    EXPECT_EQ(plan.additive_cms_id, 90351001);
    EXPECT_EQ(plan.additive_remaining, 2);
    EXPECT_EQ(plan.chances.success, 7000);
    EXPECT_EQ(plan.chances.fail, 3000);
    EXPECT_EQ(plan.chances.destruction, 0);

    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep2, 1, {{90211101, 2}}, 5000,
                                          &kReinforcedAdditive)
                  .status,
              PolishPlanStatus::NotEnoughAdditive);
}

TEST(InventoryRulesPolish, AddSuccessTakesFromFailThenDestructionAndCapsAtCertainty) {
    const PolishChances risky{2000, 3000, 5000};

    const auto reinforced = InventoryRules::apply_polish_additive(risky, kReinforcedAdditive);
    EXPECT_EQ(reinforced.success, 3000);
    EXPECT_EQ(reinforced.fail, 2000);
    EXPECT_EQ(reinforced.destruction, 5000);

    const auto mystical = InventoryRules::apply_polish_additive(risky, kMysticalAdditive);
    EXPECT_EQ(mystical.success, 10000);
    EXPECT_EQ(mystical.fail, 0);
    EXPECT_EQ(mystical.destruction, 0);

    const PolishAdditive large{90451001, PolishAdditiveEffect::AddSuccessPercent, 50};
    const auto partial = InventoryRules::apply_polish_additive(risky, large);
    EXPECT_EQ(partial.success, 7000);
    EXPECT_EQ(partial.fail, 0);
    EXPECT_EQ(partial.destruction, 3000);
}

TEST(InventoryRulesPolish, SafeguardTurnsDestructionIntoFail) {
    const auto guarded =
        InventoryRules::apply_polish_additive({2000, 3000, 5000}, kSafeguardAdditive);

    EXPECT_EQ(guarded.success, 2000);
    EXPECT_EQ(guarded.fail, 8000);
    EXPECT_EQ(guarded.destruction, 0);
}

TEST(InventoryRulesPolish, RollMapsOntoSuccessFailAndDestructionBoundaries) {
    const PolishChances chances{6000, 3000, 1000};

    EXPECT_EQ(InventoryRules::resolve_polish_roll(chances, 0), PolishOutcome::success);
    EXPECT_EQ(InventoryRules::resolve_polish_roll(chances, 5999), PolishOutcome::success);
    EXPECT_EQ(InventoryRules::resolve_polish_roll(chances, 6000), PolishOutcome::fail);
    EXPECT_EQ(InventoryRules::resolve_polish_roll(chances, 8999), PolishOutcome::fail);
    EXPECT_EQ(InventoryRules::resolve_polish_roll(chances, 9000), PolishOutcome::destruction);
    EXPECT_EQ(InventoryRules::resolve_polish_roll(chances, 9999), PolishOutcome::destruction);

    // A certain step ignores the roll value.
    EXPECT_EQ(InventoryRules::resolve_polish_roll({10000, 0, 0}, 9999), PolishOutcome::success);

    EXPECT_THROW(InventoryRules::resolve_polish_roll(chances, -1), std::invalid_argument);
    EXPECT_THROW(InventoryRules::resolve_polish_roll(chances, 10000), std::invalid_argument);
    EXPECT_THROW(InventoryRules::resolve_polish_roll({6000, 3000, 0}, 0), std::invalid_argument);
}

TEST(InventoryRulesPolish, RefusesMissingOrShortMaterialAndShortGoldAtTheExactBoundary) {
    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep1, 0, {}, 5000).status,
              PolishPlanStatus::NotEnoughMaterial);
    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep1, 0, {{90211101, 0}}, 5000).status,
              PolishPlanStatus::NotEnoughMaterial);
    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep1, 0, {{90211201, 9}}, 5000).status,
              PolishPlanStatus::NotEnoughMaterial);

    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep1, 0, {{90211101, 1}}, 999).status,
              PolishPlanStatus::NotEnoughGold);
    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep1, 0, {{90211101, 1}}, 1000).status,
              PolishPlanStatus::Planned);
}

TEST(InventoryRulesPolish, RejectsMalformedRequirementsAndStepsWithoutPlanning) {
    auto bad_material = kGrandWeaponStep1;
    bad_material.material_cms_id = 0;
    auto bad_count = kGrandWeaponStep1;
    bad_count.material_count = 0;
    auto bad_cost = kGrandWeaponStep1;
    bad_cost.gold_cost = -1;
    auto bad_probability = kGrandWeaponStep1;
    bad_probability.success_probability = 10001;
    // Each value is in range, but the row no longer sums to 10000.
    auto bad_total = kGrandWeaponStep1;
    bad_total.fail_probability = 1;

    for (const auto &requirement :
         {bad_material, bad_count, bad_cost, bad_probability, bad_total}) {
        EXPECT_EQ(InventoryRules::plan_polish(requirement, 0, {{90211101, 5}}, 5000).status,
                  PolishPlanStatus::InvalidRequirement);
    }
    const PolishAdditive stone_as_additive{90211101, PolishAdditiveEffect::AddSuccessPercent, 10};
    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep1, 0, {{90211101, 5}}, 5000,
                                          &stone_as_additive)
                  .status,
              PolishPlanStatus::InvalidRequirement);
    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep1, 0, {{90211101, 5}}, -1).status,
              PolishPlanStatus::InvalidRequirement);
    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep1, -1, {{90211101, 5}}, 5000).status,
              PolishPlanStatus::StepOutOfRange);
    EXPECT_EQ(InventoryRules::plan_polish(kGrandWeaponStep1, std::numeric_limits<int>::max(),
                                          {{90211101, 5}}, 5000)
                  .status,
              PolishPlanStatus::StepOutOfRange);
}

TEST(InventoryRulesPolish, OnlyWeaponAndArmorSlotsHavePolishCategories) {
    EXPECT_EQ(InventoryRules::polish_category_for_slot_name("Weapon_1"), "Weapon");
    EXPECT_EQ(InventoryRules::polish_category_for_slot_name("ArmorSuit"), "Armor");
    EXPECT_EQ(InventoryRules::polish_category_for_slot_name("ArmorShoes"), "Armor");
    for (const char *slot : {"CostumeSuit", "Bag", "Ring", "Necklace", "FlightTool", "Tool_1", ""}) {
        EXPECT_FALSE(InventoryRules::polish_category_for_slot_name(slot).has_value()) << slot;
    }
}

} // namespace
} // namespace chronicles
