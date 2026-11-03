#include "world/features/combat/combat_bootstrap.hpp"

#include "common/clock_ticks.hpp"
#include "persistence/character_state_types.hpp"

namespace chronicles::combat {
namespace {

constexpr std::uint16_t kWeaponGaugeNotification = 78;
constexpr std::uint16_t kUnitAttributeNotification = 33;
constexpr std::uint16_t kUnitTraitNotification = 115;
constexpr std::uint16_t kUnitStateNotification = 32;
constexpr std::uint16_t kDeathNotification = 23;

} // namespace

GameplayPacket entry_weapon_gauge() {
  return {kWeaponGaugeNotification, nlohmann::json::object()};
}

GameplayPacket entry_unit_attribute(std::uint32_t unit_id, int observed_value) {
  return {kUnitAttributeNotification,
          {{"unit_id", std::to_string(unit_id)}, {"attributes", {{"2", observed_value}}}}};
}

GameplayPacket entry_unit_traits(std::uint32_t unit_id) {
  return {kUnitTraitNotification,
          {{"unit_id", std::to_string(unit_id)},
           {"unitTraits",
            {{"1", {{"type", "EX5UnitTraitValueType_PerMille"}}},
             {"2", {{"type", "EX5UnitTraitValueType_Constant"}}},
             {"3", {{"type", "EX5UnitTraitValueType_Constant"}}},
             {"6", {{"type", "EX5UnitTraitValueType_Constant"}}}}}}};
}

GameplayPacket saved_player_demise(const std::string &unit_id, std::int64_t internal_tick) {
  return {kUnitStateNotification,
          {{"unit_id", unit_id},
           {"state", "EX5UnitStateType_Demise"},
           {"tick", wire_monotonic_tick(internal_tick)}}};
}

GameplayPacket saved_player_death(const std::string &unit_id, const WorldPosition &position) {
  return {kDeathNotification,
          {{"target_unit_id", unit_id},
           {"pos",
            {{"millimeter_x", position.x_mm},
             {"millimeter_y", position.y_mm},
             {"millimeter_z", position.z_mm}}}}};
}

} // namespace chronicles::combat
