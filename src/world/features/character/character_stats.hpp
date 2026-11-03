#pragma once

#include "data/client_catalog.hpp"
#include "persistence/character_repository.hpp"

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>
#include <vector>

namespace chronicles {

// Immutable definitions and initial tutorial stats. Active combat vitals remain
// session-owned; this service must not be used to refill an active unit.
class CharacterStats {
public:
  static std::shared_ptr<const CharacterStats> load(ClientCatalog &catalog);

  nlohmann::json stats(const Character &character) const;

  nlohmann::json stats(const Character &character, int active_weapon_slot) const;

  nlohmann::json base_notification(const Character &character) const;
  nlohmann::json starting_perks(int class_id) const;

private:
  struct ClassDefinition {
    std::map<int, int> attributes;
  };

  struct EquipmentDefinition {
    int level{};
    int max_durability{};
    int slot{};
    std::map<int, float> rates;
    std::vector<std::pair<int, float>> basic_ratios;
    // ItemPolishIncreaseValue percent by polish step; empty when the item has no increase table.
    std::map<int, double> polish_percents;
  };

  struct GradeDefinition {
    std::map<int, float> rates;
    float basic_ratio{};
  };

  std::map<int, int> equipment_stats(const Character &character, int active_weapon_slot) const;

  std::map<int, ClassDefinition> classes_;
  std::map<int, std::vector<int>> starting_perks_;
  // PerkNode id -> (EX5StatType, statAdd) for nodes whose effect is a flat stat.
  std::map<int, std::pair<int, int>> perk_stats_;
  // Reached blessing level -> cumulative (MaxHP, MaxSP) catalog rewards.
  std::map<int, std::pair<int, int>> nui_blessing_rewards_;
  std::map<int, EquipmentDefinition> equipment_;
  std::map<int, GradeDefinition> grades_;
  std::map<int, std::pair<int, int>> limits_;
  float durability_danger_ratio_{};
  float durability_penalty_{};
};

} // namespace chronicles
