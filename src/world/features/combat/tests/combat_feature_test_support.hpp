#pragma once

#include "data/client_catalog.hpp"
#include "testing/catalog_test_support.hpp"
#include "world/features/combat/combat_feature.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>

namespace chronicles::combat::testing_support {

using Json = nlohmann::json;
inline constexpr WorldPosition kPosition{9012, 1000, 2000, 3000, 0};
inline constexpr std::int64_t kStart = 100000000;
inline constexpr std::int64_t kMillisecond = 10000;

// Same minimal real CombatData shape used by the existing MovementVitals cases.
inline Json empty_combat_data() {
  return {{"format_version", 1},
          {"skills", Json::array()},
          {"effects", Json::object()},
          {"projectiles", Json::object()},
          {"npc_stats", Json::object()},
          {"npc_skills", Json::object()},
          {"weapon_items", Json::object()},
          {"recoveries", Json::object()}};
}

inline Json empty_population(int map_id, const std::string &scope = "Field") {
  return {{"map_id", map_id}, {"instance_type", scope}, {"spawns", Json::array()}};
}

inline void write_profile(const std::filesystem::path &root, Json document) {
  const auto path = root / "world-mobs/definitions.json";
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << document;
  if (!output) {
    throw std::runtime_error("Could not write the Combat profile fixture");
  }
}

inline Json world_profile(Json maps = Json::array()) {
  return {{"format_version", 1},
          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
          {"combat", empty_combat_data()},
          {"maps", std::move(maps)}};
}

inline std::map<std::string, std::vector<Json>> catalog_tables() {
  return {{"GameVariable", {{{"name", "FallHeightSafe"}, {"value", "1000"}}}},
          {"Item", {{{"id", "200"}, {"maxDurability", "1000"}}}}};
}

class Admission final : public CombatEncounterAdmission {
public:
  bool combat_is_raid_map(int map_id) const override {
    ++map_queries;
    return map_id == raid_map;
  }

  std::optional<std::uint64_t> combat_raid_instance_owner(std::int64_t character_id,
                                                          int map_id) const override {
    ++owner_queries;
    last_character = character_id;
    last_map = map_id;
    return owner;
  }

  std::shared_ptr<InstanceCopy> combat_instance_copy(int map_id) const override {
    return copy && copy->map_id() == map_id ? copy : nullptr;
  }

  int raid_map{9014};
  std::optional<std::uint64_t> owner;
  std::shared_ptr<InstanceCopy> copy;
  mutable int map_queries{};
  mutable int owner_queries{};
  mutable std::int64_t last_character{};
  mutable int last_map{};
};

} // namespace chronicles::combat::testing_support
