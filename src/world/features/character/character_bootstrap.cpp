#include "world/features/character/character_bootstrap.hpp"

#include "world/features/character/character_stats.hpp"

#include <stdexcept>
#include <utility>

namespace chronicles::character {
namespace {

constexpr std::uint16_t kWorldCharBaseNotification = 125;
constexpr std::uint16_t kWorldCharPerkNotification = 135;

} // namespace

CharacterBootstrap::CharacterBootstrap(std::shared_ptr<const CharacterStats> stats)
    : stats_model_(std::move(stats)) {
}

void CharacterBootstrap::prepare_stats(const Character &character) {
  if (stats_model_) {
    stats_ = stats_model_->stats(character);
  }
}

void CharacterBootstrap::set_entry_dead(bool dead) {
  dead_ = dead;
}

void CharacterBootstrap::select_character(Character character) {
  character_ = std::move(character);
}

const Character *CharacterBootstrap::selected_character() const noexcept {
  return character_ ? &*character_ : nullptr;
}

const nlohmann::json &CharacterBootstrap::stats() const noexcept {
  return stats_;
}

bool CharacterBootstrap::dead() const noexcept {
  return dead_;
}

void CharacterBootstrap::relocate(const WorldPosition &position) {
  if (!character_) {
    throw std::logic_error("Unsupported authoritative player relocation");
  }
  character_->world_position = position;
}

void CharacterBootstrap::refresh_items_and_stats(const Character &character,
                                                 const nlohmann::json *prepared_stats) {
  if (!character_ || character_->id != character.id) {
    return;
  }

  character_->items = character.items;
  character_->build.active_weapon_slot = character.build.active_weapon_slot;
  character_->nui_blessing_level = character.nui_blessing_level;
  character_->dedicated_nui_tears = character.dedicated_nui_tears;
  if (prepared_stats) {
    stats_ = *prepared_stats;
  }
}

void CharacterBootstrap::refresh_progression(int level, std::int64_t experience,
                                             const CharacterBuild &build) {
  if (!character_) {
    return;
  }

  character_->level = level;
  character_->experience = experience;
  character_->build = build;
}

void CharacterBootstrap::set_perk_snapshot(nlohmann::json snapshot) {
  perk_snapshot_ = std::move(snapshot);
}

void CharacterBootstrap::set_game_master(bool game_master) {
  game_master_ = game_master;
}

void CharacterBootstrap::set_authoritative_player_state(const nlohmann::json &stats, bool dead) {
  if (!character_) {
    throw std::logic_error("No authenticated player snapshot");
  }

  stats_ = stats;
  dead_ = dead;
}

std::optional<GameplayPacket> CharacterBootstrap::base_snapshot() const {
  if (!character_ || !stats_model_) {
    return std::nullopt;
  }
  auto base = stats_model_->base_notification(*character_);
  if (game_master_) {
    base["gm"] = true;
  }
  return GameplayPacket{kWorldCharBaseNotification, std::move(base)};
}

std::optional<GameplayPacket> CharacterBootstrap::perk_snapshot() const {
  if (!character_) {
    return std::nullopt;
  }
  if (perk_snapshot_) {
    return GameplayPacket{kWorldCharPerkNotification, *perk_snapshot_};
  }
  if (stats_model_) {
    return GameplayPacket{kWorldCharPerkNotification,
                          stats_model_->starting_perks(character_->class_id)};
  }
  return std::nullopt;
}

} // namespace chronicles::character
