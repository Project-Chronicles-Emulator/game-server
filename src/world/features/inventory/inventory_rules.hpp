#pragma once

#include "persistence/character_state_types.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace chronicles {

struct InventoryUpdate {
    std::int64_t item_id{};
    std::optional<int> equip_slot;
    bool vacated{false};
};

struct VisualSlotClear {
    int slot{};
    bool cleared{true};
};

struct ItemWeightLine {
    std::int64_t per_unit_weight{};
    std::int64_t quantity{};
};

struct WeightStatus {
    std::int64_t carried_weight{};
    std::int64_t safe_capacity{};
    std::int64_t free_weight{};
};

// Probability scale of the ItemPolish table: parts per 10000.
inline constexpr int kPolishProbabilityScale = 10000;

// One ItemPolish catalog row for (grade, equipCategory, target polishStep). Probabilities are in
// parts per 10000 exactly as the catalog states them.
struct PolishRequirement {
    int success_probability{};
    int fail_probability{};
    int destruction_probability{};
    int material_cms_id{};
    int material_count{};
    std::int64_t gold_cost{};
};

// x5.cms.EX5AdditiveMaterialEffect of a PolishingMaterial item.
enum class PolishAdditiveEffect {
    // Item.additiveMaterialValue is a percentage of success to add.
    AddSuccessPercent,
    // A destruction roll becomes a failure ("Prevents gear from being destroyed").
    DestructionProhibition,
};

// A tempering additive from the Item table. One unit is consumed per attempt.
struct PolishAdditive {
    int cms_id{};
    PolishAdditiveEffect effect{PolishAdditiveEffect::AddSuccessPercent};
    int value{};
};

// Outcome chances after any additive, in parts per 10000; they always sum to 10000.
struct PolishChances {
    int success{};
    int fail{};
    int destruction{};
};

// Why a polish cannot be planned. These are internal categories; the world handler maps them to
// wire error codes.
enum class PolishPlanStatus {
    Planned,
    InvalidRequirement,
    StepOutOfRange,
    NotEnoughMaterial,
    NotEnoughAdditive,
    NotEnoughGold,
};

struct PolishPlan {
    PolishPlanStatus status{PolishPlanStatus::InvalidRequirement};
    // The step reached on success; failure keeps the current step.
    int new_polish_step{};
    PolishChances chances{};
    int material_cms_id{};
    int material_count{};
    int material_remaining{};
    // Zero when no additive is used.
    int additive_cms_id{};
    int additive_remaining{};
    std::int64_t gold_cost{};
    std::int64_t gold_remaining{};
};

class InventoryRules {
public:
    static InventoryUpdate bare_unequip(
        const std::vector<CharacterItem>& items,
        int slot,
        VisualSlotClear& out_visual_clear);

    // Mirrors InventoryCapacity::measure: carried = sum(per_unit * quantity); safe =
    // default_safe_weight, REPLACED by equipped_bag_safe_weight when it is > 0.
    // Throws std::invalid_argument on negative input, std::overflow_error on int64
    // overflow. No stat bonus, no maximum, no refusal code (all unobserved).
    static WeightStatus compute_weight(
        const std::vector<ItemWeightLine>& lines,
        std::int64_t default_safe_weight,
        std::int64_t equipped_bag_safe_weight);

    // ItemPolish rows exist only for weapons and armor. Returns "Weapon" for a catalog equipSlot
    // name starting with "Weapon", "Armor" for one starting with "Armor", otherwise nullopt
    // (costume, bag, accessory and tool slots have no polish rows).
    static std::optional<std::string_view> polish_category_for_slot_name(
        std::string_view catalog_equip_slot);

    // Plans one tempering attempt from the catalog row of the NEXT step (current_polish_step + 1).
    // The row's probabilities must each lie in [0, 10000] and sum to 10000. With an additive,
    // one unit of it must be owned and the chances are adjusted by apply_polish_additive.
    // Checks material, then additive, then gold (the order is unobserved). Pure: it mutates
    // nothing and does not roll.
    static PolishPlan plan_polish(
        const PolishRequirement& requirement,
        int current_polish_step,
        const std::map<int, int>& stackables,
        std::int64_t gold,
        const PolishAdditive* additive = nullptr);

    // Candidate additive rule; the shipping tempering widget never sends an additive, so no
    // capture shows its effect. AddSuccessPercent adds value percentage points of success
    // (value * 100 parts), capped at certainty and taken from fail first, then destruction.
    // DestructionProhibition turns the destruction chance into fail chance.
    static PolishChances apply_polish_additive(PolishChances chances,
                                               const PolishAdditive& additive);

    // Maps a uniform roll in [0, 10000) onto the chances in the ItemRollResult order: success,
    // then fail, then destruction. Throws std::invalid_argument for a roll outside that range or
    // chances that do not sum to 10000.
    static PolishOutcome resolve_polish_roll(const PolishChances& chances, int roll);
};

} // namespace chronicles
