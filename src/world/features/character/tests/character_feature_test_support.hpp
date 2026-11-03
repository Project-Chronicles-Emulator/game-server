#pragma once

#include "data/client_catalog.hpp"
#include "testing/catalog_test_support.hpp"
#include "world/features/character/character_feature.hpp"

#include <array>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace chronicles::character::testing_support {

using Json = nlohmann::json;

inline std::map<std::string, std::vector<Json>> character_tables() {
  std::map<std::string, std::vector<Json>> tables;
  tables["CharacterLevel"] = {
      {{"level", "1"},
       {"nextLevelExp", "100"},
       {"charBuildPointType", "LevelUpPoint"},
       {"charBuildPoint", "3"}},
      {{"level", "2"},
       {"nextLevelExp", "300"},
       {"charBuildPointType", "LevelUpPoint"},
       {"charBuildPoint", "3"}},
      {{"level", "3"},
       {"nextLevelExp", "1000"},
       {"charBuildPointType", "LevelUpPoint"},
       {"charBuildPoint", "3"}},
  };
  tables["PerkTheme"] = {{{"perkThemeType", "LongBow"}}};
  int node_id = 1001;
  for (const auto *name : std::array{"Fight", "Wild", "Adamant", "Magic"}) {
    tables["CharacterBasicStat"].push_back(
        {{"FK_character", name}, {"typeValue", "Str:15,Int:12,Con:8,End:11,Dex:27,Tal:17"}});
    tables["PerkNode"].push_back({{"id", std::to_string(node_id)},
                                  {"characterClass", name},
                                  {"theme", "LongBow"},
                                  {"FK_unlockNodeIds", ""},
                                  {"groupId", ""},
                                  {"costType", "WeaponPoint"},
                                  {"cost", "1"},
                                  {"maxLevel", "1"},
                                  {"autoRegistration", "True"},
                                  {"jsonCase", "SkillOpen"},
                                  {"json", "\"skillId01\":1"}});
    ++node_id;
  }
  tables["NpcTemplate"] = {{{"id", "1"}, {"grade", "Normal"}}};
  tables["Npc"] = {
      {{"id", "5001"}, {"level", "2"}, {"FK_template", "1"}, {"behaviorType", "Monster"}}};
  tables["Achievement"] = {{{"id", "1000"},
                            {"parentId", "0"},
                            {"FK_achievementGoalTable", "1000"},
                            {"typeCase", "Money"},
                            {"type", "\"value\":100"}}};
  tables["AchievementGoal"] = {{{"id", "1000"},
                                {"order", "1"},
                                {"goalTypeCase", "CharLevel"},
                                {"goalType", "\"charLevel\":2"}}};
  tables["Achievement"].push_back({{"id", "1001"},
                                     {"parentId", "0"},
                                     {"FK_achievementGoalTable", "1001"},
                                     {"typeCase", "Exp"},
                                     {"type", "\"value\":50"}});
  tables["AchievementGoal"].push_back({{"id", "1001"},
                                         {"order", "1"},
                                         {"goalTypeCase", "CharLevel"},
                                         {"goalType", "\"charLevel\":2"}});
  tables["Trophy"] = {};
  tables["EquipmentGrade"] = {};
  tables["Item"] = {};
  tables["Stat"] = {};
  tables["GameVariable"] = {
      {{"name", "DurabilityDangerRatio"}, {"value", "0.1"}},
      {{"name", "DurabilityPanaltyValue"}, {"value", "0.5"}},
  };
  return tables;
}

// Reuse the repository's hash-checked table writer. CharacterStats additionally requires
// the researched build identity; this changes only the private fixture manifest identity.
class CharacterCatalog final {
public:
  CharacterCatalog() : fixture_(character_tables()) {
    const auto path = fixture_.root() / "manifest.json";
    Json manifest;
    {
      std::ifstream input(path);
      input >> manifest;
    }
    manifest["source_executable_dump_sha256"] =
        "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << manifest.dump();
    if (!output) {
      throw std::runtime_error("Could not write Character fixture build identity");
    }
  }

  const std::filesystem::path &root() const {
    return fixture_.root();
  }

private:
  chronicles::testing_support::TestCatalog fixture_;
};

class ProgressStore : public CharacterProgressStore {
public:
  ProgressStore() {
    actor.id = 17;
    actor.class_id = 2;
    actor.level = 1;
    actor.name = "owner";
  }

  std::int64_t progression_character_id() const override {
    return actor.id;
  }

  std::optional<Character> progression_character() override {
    ++character_reads;
    if (!character_exists) {
      return std::nullopt;
    }
    return actor;
  }

  WorldProgress progression_stored_progress() override {
    ++progress_reads;
    return progress;
  }

  bool progression_save(const WorldProgress &before, const WorldProgress &after) override {
    ++saves;
    if (before_save) {
      before_save(before, after);
    }
    if (!commit_succeeds || before != progress) {
      return false;
    }
    progress = after;
    return true;
  }

  Character actor;
  WorldProgress progress;
  bool character_exists{true};
  bool commit_succeeds{true};
  int character_reads{};
  int progress_reads{};
  int saves{};
  std::function<void(const WorldProgress &, const WorldProgress &)> before_save;
};

} // namespace chronicles::character::testing_support
