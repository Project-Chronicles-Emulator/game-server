#include "world/features/character/character_feature.hpp"

#include "data/client_catalog.hpp"
#include "data/shop_catalog.hpp"

#include <spdlog/spdlog.h>

#include <limits>
#include <utility>

namespace chronicles::character {
namespace {

std::shared_ptr<const Progression> load_progression(ClientCatalog *catalog) {
  if (!catalog) {
    return {};
  }
  for (const char *table : {"CharacterLevel", "PerkNode", "PerkTheme", "Npc", "NpcTemplate"}) {
    if (!catalog->has_table(table)) {
      spdlog::warn("Character progression disabled: client catalog table {} is absent", table);
      return {};
    }
  }

  auto progression = Progression::load(*catalog);
  spdlog::info("Character progression loaded: level cap {}", progression->max_level());
  return progression;
}

std::shared_ptr<const AchievementCatalog> load_achievements(ClientCatalog *catalog) {
  if (!catalog || !catalog->has_table("Achievement") || !catalog->has_table("AchievementGoal") ||
      !catalog->has_table("Trophy")) {
    return {};
  }
  auto achievements = AchievementCatalog::load(*catalog);
  spdlog::info("Achievement catalog loaded: {} achievements", achievements->size());
  return achievements;
}

} // namespace

CharacterFeature::CharacterFeature(std::shared_ptr<const CharacterStats> stats,
                                   std::shared_ptr<ClientCatalog> catalog,
                                   std::shared_ptr<const ShopCatalog> items)
    : stats_(std::move(stats)), catalog_(std::move(catalog)), items_(std::move(items)),
      progression_(load_progression(catalog_.get())),
      achievements_(load_achievements(catalog_.get())) {}

bool CharacterFeature::has_stats() const noexcept {
  return static_cast<bool>(stats_);
}

nlohmann::json CharacterFeature::stats(const Character &character) const {
  return stats_->stats(character);
}

const std::shared_ptr<const CharacterStats> &CharacterFeature::stats_model() const noexcept {
  return stats_;
}

const std::shared_ptr<const Progression> &CharacterFeature::progression() const noexcept {
  return progression_;
}

const std::shared_ptr<const AchievementCatalog> &CharacterFeature::achievements() const noexcept {
  return achievements_;
}

bool CharacterFeature::has_graduation() const noexcept {
  return static_cast<bool>(loadout_);
}

std::optional<std::string> CharacterFeature::graduation_notice(bool game_master) const {
  if (!loadout_ || game_master) {
    return std::nullopt;
  }
  // The kit is applied at the next entry, before that entry's snapshots are built.
  return "Tutorial complete! Return to character selection and enter the world again to "
         "receive the test kit: maximum level and skill points, the best gear, gold and "
         "every quest completed. Type /gm car to call a car.";
}

void CharacterFeature::enable_graduation() {
  if (!catalog_ || !progression_ || !items_) {
    spdlog::warn("Graduation grant stays off: it needs the client catalog and progression");
    return;
  }
  loadout_ =
      std::make_shared<const GraduationLoadout>(catalog_->rows("Item"), progression_->max_level());
  spdlog::info("Graduation grant enabled: characters entering the overworld below level {} "
               "receive the test kit",
               progression_->max_level());
}

GraduationGrantServices CharacterFeature::graduation_services() const {
  return {items_, progression_, loadout_};
}

void CharacterFeature::reconcile_entry(CharacterProgressStore &store, Character &character) const {
  if (!progression_) {
    return;
  }
  const auto before = store.progression_stored_progress();
  auto after = before;
  const auto change = progression_->reconcile_level(after);
  if (change.levels_gained == 0 || before.revision == std::numeric_limits<std::int64_t>::max()) {
    return;
  }

  ++after.revision;
  if (!store.progression_save(before, after)) {
    spdlog::warn("Character {} level reconciliation conflicted; entering at level {}", character.id,
                 before.level);
    return;
  }
  character.level = after.level;
  spdlog::info("Character {} reconciled from level {} to {} with {} LevelUpPoints", character.id,
               change.previous_level, after.level, change.level_up_points_awarded);
}

void CharacterFeature::apply_entry_graduation(GraduationGrantContext &context,
                                              Character &character) const {
  if (!loadout_ || !progression_ || !items_) {
    return;
  }
  if (!chronicles::character::apply_graduation_grant(context, graduation_services()).applied) {
    return;
  }
  if (auto reloaded = context.progression_character()) {
    character = std::move(*reloaded);
  }
}

std::optional<nlohmann::json> CharacterFeature::entry_perks(const Character &character,
                                                            const WorldProgress &progress) const {
  if (!progression_) {
    return std::nullopt;
  }
  return perk_snapshot_message(*progression_, character.class_id, progress);
}

std::unique_ptr<CharacterSession> CharacterFeature::create_session(CharacterSessionContext &context,
                                                                   SessionEventBus &events) {
  return std::make_unique<CharacterSession>(*this, context, events);
}

} // namespace chronicles::character
