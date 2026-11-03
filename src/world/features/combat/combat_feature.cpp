#include "world/features/combat/combat_feature.hpp"

#include "common/catalog_fields.hpp"
#include "data/client_catalog.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <stdexcept>
#include <utility>

namespace chronicles::combat {

CombatFeature::CombatFeature(std::shared_ptr<ClientCatalog> catalog)
    : catalog_(std::move(catalog)) {
}

void CombatFeature::load_encounters(const TutorialData *tutorial,
                                    const std::filesystem::path &catalog_root) {
  if (tutorial && tutorial->document.contains("combat")) {
    data_ = std::make_shared<const CombatData>(tutorial->document.at("combat"));
  }
  if (!catalog_root.empty()) {
    const auto path = catalog_root.parent_path() / "world-mobs" / "definitions.json";
    if (std::filesystem::exists(path)) {
      std::ifstream input(path);
      nlohmann::json profile;
      input >> profile;
      if (profile.at("format_version") != 1 ||
          profile.at("build_id") != "40367e24241c9481bfc07fbf9ef1919f25fb0eaa") {
        throw std::runtime_error("World monster profile belongs to an unsupported build");
      }
      data_ = std::make_shared<const CombatData>(profile.at("combat"));
      for (const auto &map : profile.at("maps")) {
        const int map_id = map.at("map_id");
        if (!populations_.emplace(map_id, CombatSession::create_world_mobs(map, data_)).second) {
          throw std::runtime_error("Duplicate world monster map");
        }
        definitions_.emplace(map_id, map);
      }
      spdlog::info("World monster profile loaded: {} maps", populations_.size());
    }
  }
}

void CombatFeature::load_movement_rules(ClientCatalog &catalog) {
  movement_rules_ = MovementVitalsRules::load(catalog);
  if (const auto safe_height_cm =
          game_variable_integer(catalog, "FallHeightSafe", 0, 1000000, "Fall damage")) {
    fall_rules_.safe_height_mm = *safe_height_cm * 10;
  }
}

void CombatFeature::load_revival_spawns(bool tutorial_enabled, ClientCatalog *catalog,
                                        const std::filesystem::path &catalog_root) {
  if (tutorial_enabled) {
    if (!catalog || catalog_root.empty()) {
      throw std::runtime_error("Tutorial revival requires a verified catalog and raw export");
    }
    revival_spawns_.emplace(catalog_root, catalog->rows("Map"));
  }
}

const std::shared_ptr<const CombatData> &CombatFeature::data() const noexcept {
  return data_;
}

const FallDamageRules &CombatFeature::fall_rules() const noexcept {
  return fall_rules_;
}

const MovementVitalsRules &CombatFeature::movement_rules() const noexcept {
  return movement_rules_;
}

const std::optional<RevivalSpawns> &CombatFeature::revival_spawns() const noexcept {
  return revival_spawns_;
}

bool CombatFeature::has_catalog() const noexcept {
  return static_cast<bool>(catalog_);
}

std::optional<nlohmann::json> CombatFeature::equipment_definition(int item_cms_id) const {
  return catalog_->find_row("Item", "id", std::to_string(item_cms_id));
}

PlayerPvpRegistry &CombatFeature::player_pvp() noexcept {
  return player_pvp_;
}

const PlayerPvpRegistry &CombatFeature::player_pvp() const noexcept {
  return player_pvp_;
}

std::shared_ptr<CombatSession::WorldMobs>
CombatFeature::population_for(int map_id, std::int64_t character_id,
                              const CombatEncounterAdmission &admission) {
  const auto found = populations_.find(map_id);
  if (found == populations_.end()) {
    return {};
  }
  auto population = found->second;
  std::optional<std::uint64_t> instance_owner;
  if (admission.combat_is_raid_map(map_id)) {
    instance_owner = admission.combat_raid_instance_owner(character_id, map_id);
    if (!instance_owner) {
      return {};
    }
  } else if (const auto copy = admission.combat_instance_copy(map_id)) {
    // The copy owns its monsters, so every party member in it fights the same ones and they
    // survive same-map handoffs while anyone remains inside.
    const auto &definition = definitions_.at(map_id);
    return copy->resource<CombatSession::WorldMobs>([&] {
      return CombatSession::create_world_mobs(definition, data_);
    });
  } else if (definitions_.at(map_id).value("instance_type", "Field") == "Instance") {
    // Without a copy registry (tests, tools) instances stay private to the character. A
    // separate key space prevents a character ID from being confused with a matchmaker
    // channel ID.
    instance_owner = InstanceCopies::solo_owner(character_id);
  }
  if (instance_owner) {
    std::lock_guard lock(instance_mutex_);
    std::erase_if(instances_, [](const auto &entry) {
      return entry.second.expired();
    });
    auto &weak = instances_[{map_id, *instance_owner}];
    population = weak.lock();
    if (!population) {
      population = CombatSession::create_world_mobs(definitions_.at(map_id), data_);
      weak = population;
    }
  }
  return population;
}

bool CombatFeature::announce_population_after_start(int map_id) const {
  const auto found = definitions_.find(map_id);
  if (found == definitions_.end()) {
    return false;
  }

  const auto type = found->second.value("instance_type", "Field");
  return type == "Dungeon" || type == "Instance" || type == "PointOfInterest";
}

} // namespace chronicles::combat
