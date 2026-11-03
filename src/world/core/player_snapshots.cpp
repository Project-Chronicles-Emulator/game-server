#include "world/core/player_snapshots.hpp"

#include "auth/auth.hpp"
#include "p_character.pb.h"
#include "persistence/character_repository.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace chronicles {
namespace {

constexpr char kDiagnosticPlayerIdleState[] = "EX5UnitStateType_Idle";
constexpr int kDiagnosticMoveSpeedRateStatId = 84;
constexpr int kDiagnosticMoveSpeedRate = 1000;

nlohmann::json character_equipment(const Character &character) {
  nlohmann::json equipment = nlohmann::json::array();
  for (const auto &item : character.items) {
    if (!item.equip_slot) {
      continue;
    }

    nlohmann::json entry{{"item_cms_id", item.cms_id}, {"slot", *item.equip_slot}};
    if (item.grade != 0) {
      entry["grade"] = item.grade;
    }
    equipment.push_back(std::move(entry));
  }
  return equipment;
}

nlohmann::json player_nub(const Character &character, const nlohmann::json &stats, bool dead) {
  auto player_stats = stats;
  if (player_stats.empty()) {
    player_stats[std::to_string(kDiagnosticMoveSpeedRateStatId)] = kDiagnosticMoveSpeedRate;
  }
  nlohmann::json player{{"char_id", std::to_string(character.id)},
                        {"char_class", character.class_id},
                        {"level", character.level},
                        {"exp", std::to_string(character.experience)},
                        {"name", character.name},
                        {"equipments", character_equipment(character)},
                        {"activation_weapon_slot", character.build.active_weapon_slot},
                        {"state", dead ? "EX5UnitStateType_Demise" : kDiagnosticPlayerIdleState},
                        {"stats", player_stats},
                        {"weapon_state_type", "EX5WeaponStateType_Sheathed"},
                        {"world_level", 1}};
  if (!character.customization_hex.empty()) {
    x5::pb::DbCharacterCustomization customization;
    const auto saved_bytes = hex_decode(character.customization_hex);
    if (!customization.ParseFromString(saved_bytes)) {
      throw std::runtime_error("Saved character customization is invalid");
    }

    player["customization"] = message_json(customization);
  }
  return player;
}

} // namespace

nlohmann::json player_unit_nub(const Character &character, const WorldPosition &position,
                               const nlohmann::json &stats, bool dead) {
  return {{"pos",
           {{"millimeter_x", position.x_mm},
            {"millimeter_y", position.y_mm},
            {"millimeter_z", position.z_mm}}},
          {"yaw", position.yaw},
          {"player", player_nub(character, stats, dead)}};
}

} // namespace chronicles
