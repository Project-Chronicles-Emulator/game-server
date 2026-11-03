#pragma once

#include "world/features/character/character_progress_store.hpp"
#include "world/features/shop/item_bundle.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace chronicles {

class Progression;
class ShopCatalog;

// Emulator policy, not recovered official behavior. While the realm is being tested, a
// character that enters the overworld below the level cap receives a test kit once: the level
// cap and its build points, the best equipment of the game for the character's class, a stock
// of consumables, the gold cap and every quest of the shared catalog completed. The grant is
// guarded by the level cap: a character at the cap is never granted again. The level stage
// runs last, but each stage failure is logged and later stages still run. A successful final
// stage can therefore reach the cap even when an earlier stage failed; preserve that policy.
// This owner holds both the loadout selection and the ordered grant stages.

// Consumables of the kit. Recovered ordinary items that the tutorial itself hands out
// (tutorial_completion_mail.cpp) and the weapon repairer.
inline constexpr int kGraduationHealthPotionItemId = 40101101;
inline constexpr int kGraduationStaminaPotionItemId = 40301201;
inline constexpr int kGraduationRepairerItemId = 50310001;
inline constexpr int kGraduationHealthPotionCount = 200;
inline constexpr int kGraduationStaminaPotionCount = 100;
inline constexpr int kGraduationRepairerCount = 20;

// A slot such as a costume has several equally good pieces. The kit keeps this many per slot so
// the inventory is not flooded with look-alikes and the carried weight stays below the safe
// weight (tests/graduation_grant_tests.cpp measures it against the real catalog).
inline constexpr std::size_t kGraduationMaximumEquipmentPerSlot = 3;

// Equipment instances one kit may add. Bounds the single storage transaction.
inline constexpr int kGraduationMaximumEquipmentInstances = 128;

class GraduationLoadout {
public:
  // `item_rows` are the verified Item catalog rows; `maximum_level` is the character level cap,
  // which also bounds the level an equipment piece may require.
  GraduationLoadout(const std::vector<nlohmann::json> &item_rows, int maximum_level);

  // The kit for one character class, named like x5.cms.EX5CharacterClass without its prefix
  // ("Fight", "Wild", "Adamant", "Magic"...). Equipment is the highest level and grade piece(s)
  // of every slot that the class can wear, including class-neutral pieces; consumables follow.
  std::vector<ItemBundleEntry> bundle_for(const std::string &class_name) const;

private:
  struct EquipmentRow {
    int item_cms_id{};
    std::string class_name;
    std::string slot;
    int level{};
    int grade_rank{};
  };

  std::vector<EquipmentRow> equipment_;
};

// The class name used by the Item catalog for an x5.cms.EX5CharacterClass value; empty for an
// unknown value.
std::string graduation_class_name(int class_id);

struct GraduationResult {
  bool applied{};
  int quests_completed{};
  std::size_t equipment_granted{};
  bool level_raised{};
};

namespace character {

struct GraduationGrantServices {
  std::shared_ptr<const ShopCatalog> shop_catalog;
  std::shared_ptr<const Progression> progression;
  std::shared_ptr<const GraduationLoadout> loadout;
};

// One selected entry subject and its explicit cross-owner services. The character owner
// keeps eligibility, stage order, commits and failure boundaries; it knows no Quest/GM owner.
class GraduationGrantContext : public CharacterProgressStore {
public:
  virtual bool graduation_game_master() const = 0;
  virtual int graduation_complete_quests() = 0;
  virtual ItemBundlePlan
  graduation_plan_items(const ShopCatalog &catalog, const std::vector<ItemBundleEntry> &items,
                        std::optional<std::int64_t> gold_target, const Character &character,
                        const WorldProgress &before, int maximum_instances) = 0;
  virtual std::optional<std::vector<CharacterItem>>
  graduation_commit_items(const WorldProgress &before, const WorldProgress &after,
                          const ItemTrade &trade) = 0;
};

GraduationResult apply_graduation_grant(GraduationGrantContext &context,
                                        const GraduationGrantServices &services);

} // namespace character
} // namespace chronicles
