#include "world/features/inventory/inventory_rules.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace chronicles {

InventoryUpdate InventoryRules::bare_unequip(
    const std::vector<CharacterItem>& items,
    int slot,
    VisualSlotClear& out_visual_clear) {
    InventoryUpdate result{};
    out_visual_clear = VisualSlotClear{slot, false};

    for (const auto& item : items) {
        if (item.equip_slot.has_value() && item.equip_slot.value() == slot) {
            result.item_id = item.id;
            out_visual_clear.cleared = true;
            result.equip_slot = std::nullopt;
            result.vacated = true;
            break;
        }
    }

    return result;
}

WeightStatus InventoryRules::compute_weight(
    const std::vector<ItemWeightLine>& lines,
    std::int64_t default_safe_weight,
    std::int64_t equipped_bag_safe_weight) {
    constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

    if (default_safe_weight < 0) {
        throw std::invalid_argument("compute_weight: negative default_safe_weight");
    }
    if (equipped_bag_safe_weight < 0) {
        throw std::invalid_argument("compute_weight: negative equipped_bag_safe_weight");
    }

    std::int64_t carried_weight = 0;
    for (const auto& line : lines) {
        if (line.per_unit_weight < 0) {
            throw std::invalid_argument("compute_weight: negative per_unit_weight");
        }
        if (line.quantity < 0) {
            throw std::invalid_argument("compute_weight: negative quantity");
        }

        if (line.per_unit_weight > 0 && line.quantity > kInt64Max / line.per_unit_weight) {
            throw std::overflow_error("compute_weight: per_unit_weight * quantity overflows");
        }
        const std::int64_t line_weight = line.per_unit_weight * line.quantity;

        if (line_weight > kInt64Max - carried_weight) {
            throw std::overflow_error("compute_weight: carried weight sum overflows");
        }
        carried_weight += line_weight;
    }

    // Same rule as InventoryCapacity::measure: an equipped bag with safe weight > 0
    // replaces the default safe weight; it does not add to it.
    const std::int64_t safe_capacity =
        equipped_bag_safe_weight > 0 ? equipped_bag_safe_weight : default_safe_weight;

    // Both operands are non-negative, so the difference cannot overflow int64.
    WeightStatus status{};
    status.carried_weight = carried_weight;
    status.safe_capacity = safe_capacity;
    status.free_weight = safe_capacity - carried_weight;
    return status;
}


std::optional<std::string_view> InventoryRules::polish_category_for_slot_name(
    std::string_view catalog_equip_slot) {
    if (catalog_equip_slot.starts_with("Weapon")) {
        return std::string_view("Weapon");
    }
    if (catalog_equip_slot.starts_with("Armor")) {
        return std::string_view("Armor");
    }
    return std::nullopt;
}

PolishPlan InventoryRules::plan_polish(
    const PolishRequirement& requirement,
    int current_polish_step,
    const std::map<int, int>& stackables,
    std::int64_t gold,
    const PolishAdditive* additive) {
    PolishPlan plan{};
    const auto valid_probability = [](int value) {
        return value >= 0 && value <= kPolishProbabilityScale;
    };
    const bool probabilities_valid =
        valid_probability(requirement.success_probability) &&
        valid_probability(requirement.fail_probability) &&
        valid_probability(requirement.destruction_probability) &&
        requirement.success_probability + requirement.fail_probability +
                requirement.destruction_probability ==
            kPolishProbabilityScale;
    const bool additive_valid =
        additive == nullptr ||
        (additive->cms_id > 0 && additive->cms_id != requirement.material_cms_id &&
         additive->value >= 0 && additive->value <= 100);
    if (!probabilities_valid || !additive_valid || requirement.material_cms_id <= 0 ||
        requirement.material_count <= 0 || requirement.gold_cost < 0 || gold < 0) {
        plan.status = PolishPlanStatus::InvalidRequirement;
        return plan;
    }
    if (current_polish_step < 0 || current_polish_step == std::numeric_limits<int>::max()) {
        plan.status = PolishPlanStatus::StepOutOfRange;
        return plan;
    }

    const auto owned = stackables.find(requirement.material_cms_id);
    if (owned == stackables.end() || owned->second < requirement.material_count) {
        plan.status = PolishPlanStatus::NotEnoughMaterial;
        return plan;
    }
    auto owned_additive = stackables.end();
    if (additive != nullptr) {
        owned_additive = stackables.find(additive->cms_id);
        if (owned_additive == stackables.end() || owned_additive->second < 1) {
            plan.status = PolishPlanStatus::NotEnoughAdditive;
            return plan;
        }
    }
    if (gold < requirement.gold_cost) {
        plan.status = PolishPlanStatus::NotEnoughGold;
        return plan;
    }

    PolishChances chances{requirement.success_probability, requirement.fail_probability,
                          requirement.destruction_probability};
    if (additive != nullptr) {
        chances = apply_polish_additive(chances, *additive);
        plan.additive_cms_id = additive->cms_id;
        plan.additive_remaining = owned_additive->second - 1;
    }

    plan.status = PolishPlanStatus::Planned;
    plan.new_polish_step = current_polish_step + 1;
    plan.chances = chances;
    plan.material_cms_id = requirement.material_cms_id;
    plan.material_count = requirement.material_count;
    plan.material_remaining = owned->second - requirement.material_count;
    plan.gold_cost = requirement.gold_cost;
    plan.gold_remaining = gold - requirement.gold_cost;
    return plan;
}

PolishChances InventoryRules::apply_polish_additive(PolishChances chances,
                                                    const PolishAdditive& additive) {
    constexpr int kPartsPerPercent = kPolishProbabilityScale / 100;

    if (additive.effect == PolishAdditiveEffect::DestructionProhibition) {
        chances.fail += chances.destruction;
        chances.destruction = 0;
        return chances;
    }

    // AddSuccessPercent: the added success is taken from fail first, then destruction, so the
    // three chances keep summing to 10000.
    int added = std::min(additive.value * kPartsPerPercent,
                         kPolishProbabilityScale - chances.success);
    const int from_fail = std::min(added, chances.fail);
    chances.fail -= from_fail;
    added -= from_fail;
    const int from_destruction = std::min(added, chances.destruction);
    chances.destruction -= from_destruction;
    chances.success += from_fail + from_destruction;
    return chances;
}

PolishOutcome InventoryRules::resolve_polish_roll(const PolishChances& chances, int roll) {
    if (roll < 0 || roll >= kPolishProbabilityScale) {
        throw std::invalid_argument("resolve_polish_roll: roll outside [0, 10000)");
    }
    if (chances.success < 0 || chances.fail < 0 || chances.destruction < 0 ||
        chances.success + chances.fail + chances.destruction != kPolishProbabilityScale) {
        throw std::invalid_argument("resolve_polish_roll: chances do not sum to 10000");
    }

    if (roll < chances.success) {
        return PolishOutcome::success;
    }
    if (roll < chances.success + chances.fail) {
        return PolishOutcome::fail;
    }
    return PolishOutcome::destruction;
}

} // namespace chronicles
