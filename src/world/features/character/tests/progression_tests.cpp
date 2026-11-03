#include "world/features/character/character_stats.hpp"
#include "cms.generated.pb.h"
#include "world/features/character/progression.hpp"
#include "protocol/protocol.hpp"
#include "world/features/tutorial/tutorial.hpp"

#include "testing/memory_repositories.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <random>
#include <string>
#include <vector>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr char kVerifiedSourceSha256[] =
    "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6";
constexpr int kWildClass = 2;
constexpr int kMagicClass = 5;
constexpr int kLongBowTheme = 8;
constexpr int kCrossBowTheme = 9;
constexpr int kTutorialMonsterCmsId = 5001529;

std::string sha256_hex(const std::string &bytes) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length = 0;
  EVP_Digest(bytes.data(), bytes.size(), digest, &digest_length, EVP_sha256(), nullptr);

  const char *alphabet = "0123456789abcdef";
  std::string hex;
  for (unsigned int index = 0; index < digest_length; ++index) {
    hex.push_back(alphabet[digest[index] >> 4]);
    hex.push_back(alphabet[digest[index] & 0x0f]);
  }
  return hex;
}

// A hash-checked ClientCatalog root holding only the tables Progression reads.
class ProgressionCatalog {
public:
  explicit ProgressionCatalog(const std::map<std::string, std::vector<Json>> &tables)
      : root_(std::filesystem::temp_directory_path() /
              ("chronicles-progression-test-" + std::to_string(std::random_device{}()))) {
    std::filesystem::create_directories(root_ / "Tables");

    Json files = Json::array();
    for (const auto &[name, rows] : tables) {
      std::string body;
      for (const auto &row : rows) {
        body += row.dump();
        body += "\n";
      }
      const auto relative = "Tables/" + name + ".jsonl";
      write(root_ / relative, body);
      files.push_back({{"path", relative},
                       {"bytes", body.size()},
                       {"records", rows.size()},
                       {"sha256", sha256_hex(body)}});
    }

    const Json manifest{{"format_version", 1},
                        {"source_executable_dump_sha256", kVerifiedSourceSha256},
                        {"table_count", tables.size()},
                        {"files", std::move(files)}};
    write(root_ / "manifest.json", manifest.dump());
  }

  ~ProgressionCatalog() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  const std::filesystem::path &root() const {
    return root_;
  }

private:
  static void write(const std::filesystem::path &path, const std::string &body) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << body;
  }

  std::filesystem::path root_;
};

Json level_row(int level, std::int64_t next_level_experience) {
  return {{"level", std::to_string(level)},
          {"nextLevelExp", std::to_string(next_level_experience)},
          {"charBuildPointType", "LevelUpPoint"},
          {"charBuildPoint", "3"}};
}

Json perk_row(int id, const char *class_name, const char *theme, const char *unlock_nodes,
              int cost, bool auto_registration = false, const char *group = "") {
  return {{"id", std::to_string(id)},
          {"characterClass", class_name},
          {"theme", theme},
          {"FK_unlockNodeIds", unlock_nodes},
          {"groupId", group},
          {"costType", "WeaponPoint"},
          {"cost", std::to_string(cost)},
          {"maxLevel", "1"},
          {"autoRegistration", auto_registration ? "True" : ""},
          {"jsonCase", "SkillOpen"},
          {"json", "\"skillId01\":1"}};
}

Json npc_row(int id, int level, const char *template_id, const char *behavior = "Monster") {
  return {{"id", std::to_string(id)},
          {"level", std::to_string(level)},
          {"FK_template", template_id},
          {"behaviorType", behavior}};
}

// The first CharacterLevel thresholds of the client build, a small LongBow/CrossBow tree and
// one NPC per captured kill grade.
std::map<std::string, std::vector<Json>> progression_tables() {
  std::map<std::string, std::vector<Json>> tables;
  tables["CharacterLevel"] = {level_row(1, 500), level_row(2, 2000), level_row(3, 4000),
                              level_row(4, 6500), level_row(5, 1030279500)};
  tables["PerkTheme"] = {{{"perkThemeType", "LongBow"}}, {{"perkThemeType", "CrossBow"}}};
  tables["PerkNode"] = {
      perk_row(1230101, "Wild", "LongBow", "", 1, true),
      perk_row(1230201, "Wild", "LongBow", "1230101", 1),
      perk_row(1230301, "Wild", "LongBow", "1230201", 2),
      // An exclusive pair and a node that either member unlocks.
      perk_row(1230401, "Wild", "LongBow", "1230201", 1, false, "1230401"),
      perk_row(1230402, "Wild", "LongBow", "1230201", 1, false, "1230401"),
      perk_row(1230501, "Wild", "LongBow", "1230401,1230402", 1),
      perk_row(1225101, "Wild", "CrossBow", "", 1, true),
      perk_row(1225201, "Wild", "CrossBow", "1225101", 1),
      perk_row(1425101, "Magic", "LongBow", "", 1, true),
  };
  tables["NpcTemplate"] = {{{"id", "1"}, {"grade", "Normal"}}, {{"id", "2"}, {"grade", "Weak"}},
                           {{"id", "3"}, {"grade", "Strong"}}, {{"id", "4"}, {"grade", "Elite"}},
                           {{"id", "5"}, {"grade", "Boss_B"}}};
  tables["Npc"] = {npc_row(kTutorialMonsterCmsId, 2, "1"), npc_row(2, 6, "1"),
                   npc_row(3, 4, "2"),
                   npc_row(4, 8, "3"),
                   npc_row(5, 6, "4"),
                   npc_row(6, 5, "5"),
                   npc_row(7, 8, "1", "TownNpc")};
  return tables;
}

class ProgressionRules : public testing::Test {
protected:
  ProgressionCatalog fixture{progression_tables()};
  ClientCatalog catalog{fixture.root()};
  std::shared_ptr<const Progression> progression = Progression::load(catalog);
  WorldProgress progress;
};

TEST_F(ProgressionRules, CumulativeExperienceLevelsUpAtTheCapturedThreshold) {
  // Captured tutorial frames: exp=130 at level 1, then a 500 EXP quest reward reports exp=630,
  // level=2 and three LevelUpPoints.
  progress.experience = 130;
  const auto change = progression->award_experience(progress, 500);

  EXPECT_EQ(progress.experience, 630);
  EXPECT_EQ(progress.level, 2);
  EXPECT_EQ(progress.level_up_points, 3);
  EXPECT_EQ(change.levels_gained, 1);
  EXPECT_EQ(experience_change_message(progress, change),
            (Json{{"exp", "630"}, {"level", 2}, {"remaining_char_build_points", {{"1", 3}}}}));

  const auto small_gain = progression->award_experience(progress, 6);
  EXPECT_EQ(experience_change_message(progress, small_gain), (Json{{"exp", "636"}}));
}

TEST_F(ProgressionRules, OneAwardCrossesSeveralLevelsAndUnspentPointsCarryOver) {
  const auto change = progression->award_experience(progress, 4075);
  EXPECT_EQ(progress.level, 4);
  EXPECT_EQ(change.levels_gained, 3);
  EXPECT_EQ(progress.level_up_points, 9);

  progression->award_experience(progress, 6500 - 4075);
  EXPECT_EQ(progress.level, 5);
  EXPECT_EQ(progress.level_up_points, 12);
}

TEST_F(ProgressionRules, TheCapRowIsNeverReached) {
  progression->award_experience(progress, std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(progress.level, progression->max_level());
  EXPECT_EQ(progress.experience, 1030279500 - 1);
  EXPECT_FALSE(progression->next_level_experience(progression->max_level()));
  EXPECT_THROW(progression->award_experience(progress, -1), std::invalid_argument);
}

TEST_F(ProgressionRules, StoredExperienceFromBeforeLevelUpsIsReconciledOnce) {
  progress.experience = 2176;
  const auto change = progression->reconcile_level(progress);
  EXPECT_EQ(progress.level, 3);
  EXPECT_EQ(change.level_up_points_awarded, 6);
  EXPECT_EQ(progression->reconcile_level(progress).levels_gained, 0);
  EXPECT_EQ(progress.level_up_points, 6);
}

TEST_F(ProgressionRules, GameMasterLevelChangesMoveTheExperienceFloor) {
  progression->set_level(progress, 4);
  EXPECT_EQ(progress.level, 4);
  EXPECT_EQ(progress.experience, 4000);
  EXPECT_EQ(progress.level_up_points, 9);

  progression->set_level(progress, 2);
  EXPECT_EQ(progress.level, 2);
  EXPECT_EQ(progress.experience, 500);
  EXPECT_EQ(progress.level_up_points, 9);
  EXPECT_THROW(progression->set_level(progress, 6), std::invalid_argument);
}

TEST_F(ProgressionRules, MonsterExperienceMatchesEveryCapturedKillGrade) {
  EXPECT_EQ(progression->monster_experience(kTutorialMonsterCmsId), 6);
  EXPECT_EQ(progression->monster_experience(2), 7);
  EXPECT_EQ(progression->monster_experience(3), 2);
  EXPECT_EQ(progression->monster_experience(4), 11);
  EXPECT_EQ(progression->monster_experience(5), 104);
  EXPECT_EQ(progression->monster_experience(6), 375);
  EXPECT_EQ(progression->monster_experience(7), 0);
  EXPECT_EQ(progression->monster_experience(999), 0);
}

TEST_F(ProgressionRules, AttributeAllocationSpendsDeltasAndRefusesWithoutChange) {
  progress.level_up_points = 6;
  ASSERT_EQ(progression->allocate_stat_bonus(progress, {{x5::cms::EX5StatType_Dex, 3}}),
            x5::ec::Success);
  ASSERT_EQ(progression->allocate_stat_bonus(
                progress, {{x5::cms::EX5StatType_End, 1}, {x5::cms::EX5StatType_Dex, 2}}),
            x5::ec::Success);
  EXPECT_EQ(progress.build.stat_bonuses.at(x5::cms::EX5StatType_Dex), 5);
  EXPECT_EQ(progress.build.stat_bonuses.at(x5::cms::EX5StatType_End), 1);
  EXPECT_EQ(progress.level_up_points, 0);

  const auto unchanged = progress;
  EXPECT_EQ(progression->allocate_stat_bonus(progress, {{x5::cms::EX5StatType_Str, 1}}),
            x5::ec::StatBonus_OverCost);
  progress.level_up_points = 3;
  EXPECT_EQ(progression->allocate_stat_bonus(progress, {{x5::cms::EX5StatType_MaxHP, 1}}),
            x5::ec::StatBonus_NoConditionalStatType);
  EXPECT_EQ(progression->allocate_stat_bonus(
                progress, {{x5::cms::EX5StatType_Str, 1}, {x5::cms::EX5StatType_Str, 1}}),
            x5::ec::StatBonus_InvalidRequest);
  EXPECT_EQ(progression->allocate_stat_bonus(progress, {{x5::cms::EX5StatType_Str, 0}}),
            x5::ec::StatBonus_InvalidRequest);
  EXPECT_EQ(progress.build, unchanged.build);
  EXPECT_EQ(progress.level_up_points, 3);

  progression->reset_stat_bonus(progress);
  EXPECT_TRUE(progress.build.stat_bonuses.empty());
  EXPECT_EQ(progress.level_up_points, 9);
}

TEST_F(ProgressionRules, PerkActivationFollowsPrerequisitesCostsAndExclusiveGroups) {
  progress.weapon_points = 2;
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{1230301, 1}}),
            x5::ec::PerkNode_Locked);
  ASSERT_EQ(progression->change_perks(progress, kWildClass, {{1230201, 1}, {1230401, 1}}),
            x5::ec::Success);
  EXPECT_EQ(progress.weapon_points, 0);
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{1230501, 1}}),
            x5::ec::PerkNode_OverCost);

  progress.weapon_points = 3;
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{1230402, 1}}),
            x5::ec::PerkNode_ExclusiveActivation);
  // Either member of the exclusive pair satisfies the shared successor.
  ASSERT_EQ(progression->change_perks(progress, kWildClass, {{1230501, 1}}), x5::ec::Success);
  EXPECT_EQ(progress.weapon_points, 2);

  // Deactivating a prerequisite that a node still needs locks the tree.
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{1230401, 0}}),
            x5::ec::PerkNode_Locked);
  ASSERT_EQ(progression->change_perks(progress, kWildClass, {{1230501, 0}, {1230401, 0}}),
            x5::ec::Success);
  EXPECT_EQ(progress.weapon_points, 4);
  EXPECT_EQ(progress.build.perks.at(1230401), 0);
}

TEST_F(ProgressionRules, PerkRequestsNameTheRecoveredRefusalReasons) {
  progress.weapon_points = 5;
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {}), x5::ec::PerkNode_NotChanged);
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{42, 1}}),
            x5::ec::PerkNode_InvalidCmsId);
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{1425101, 1}}),
            x5::ec::PerkNode_InvalidClass);
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{1230201, 2}}),
            x5::ec::PerkNode_InvalidLevel);
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{1230101, 0}}),
            x5::ec::PerkNode_TryDeactivateAutoRegistedPerk);
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{1230201, 0}}),
            x5::ec::PerkNode_TryDeactivateNotActivated);
  EXPECT_EQ(progression->change_perks(progress, kWildClass, {{1230201, 1}, {1230201, 1}}),
            x5::ec::PerkNode_TryUpdateDuplicatedPerk);
  EXPECT_TRUE(progress.build.perks.empty());
  EXPECT_EQ(progress.weapon_points, 5);
}

TEST_F(ProgressionRules, ThemeResetRefundsOnlyThatTheme) {
  progress.weapon_points = 4;
  ASSERT_EQ(progression->change_perks(progress, kWildClass,
                                      {{1230201, 1}, {1230301, 1}, {1225201, 1}}),
            x5::ec::Success);
  EXPECT_EQ(progress.weapon_points, 0);

  std::vector<int> reset_nodes;
  ASSERT_EQ(progression->reset_perks(progress, kWildClass, kLongBowTheme, &reset_nodes),
            x5::ec::Success);
  EXPECT_EQ(progress.weapon_points, 3);
  EXPECT_EQ(reset_nodes, (std::vector<int>{1230201, 1230301}));
  EXPECT_EQ(progress.build.perks.at(1225201), 1);
  EXPECT_EQ(progression->reset_perks(progress, kWildClass, 25), x5::ec::PerkNode_InvalidTheme);

  progression->reset_perks(progress, kWildClass, std::nullopt);
  EXPECT_EQ(progress.weapon_points, 4);
  EXPECT_EQ(progression->active_perks(kWildClass, progress.build).size(), 2);
}

TEST_F(ProgressionRules, GmGrantsRespectAttributeBoundsAndPerkExclusions) {
  EXPECT_EQ(progression->gm_set_stat_bonus(progress, 1, 50), x5::ec::Success);
  EXPECT_EQ(Progression::stat_bonus(progress, 1), 50);
  EXPECT_EQ(progress.level_up_points, 0);
  EXPECT_NE(progression->gm_set_stat_bonus(progress, 18, 50), x5::ec::Success);
  EXPECT_NE(progression->gm_set_stat_bonus(progress, 1, 1001), x5::ec::Success);
  const auto granted = progression->gm_learn_all_perks(progress, kWildClass);
  EXPECT_FALSE(granted.empty());
  EXPECT_EQ(progress.weapon_points, 0);
  EXPECT_TRUE(progress.build.perks.contains(1230401));
  EXPECT_FALSE(progress.build.perks.contains(1230402));
  EXPECT_TRUE(progression->gm_learn_all_perks(progress, kWildClass).empty());
}

TEST_F(ProgressionRules, GmPerkGrantKeepsThePersistentCapacityBound) {
  for (int index = 0; index < kMaximumEarnedPerks - 1; ++index) {
    progress.build.perks.emplace(9000000 + index, 1);
  }
  progression->gm_learn_all_perks(progress, kWildClass);
  EXPECT_LE(progress.build.perks.size(), static_cast<std::size_t>(kMaximumEarnedPerks));
}

TEST_F(ProgressionRules, SnapshotListsAutoAndEarnedPerksWithBothBalances) {
  progress.weapon_points = 1;
  progress.level_up_points = 3;
  ASSERT_EQ(progression->change_perks(progress, kWildClass, {{1225201, 1}}), x5::ec::Success);

  const auto snapshot = perk_snapshot_message(*progression, kWildClass, progress);
  EXPECT_EQ(snapshot.at("perks").size(), 3);
  EXPECT_EQ(snapshot.at("remaining_char_build_points"), (Json{{"1", 3}, {"2", 0}}));
  EXPECT_EQ(perk_snapshot_message(*progression, kMagicClass, progress).at("perks").size(), 1);
}

TEST(ProgressionRequests, ParsesDecodedProtobufJson) {
  const auto stats = parse_stat_bonus_request(
      {{"stats", {{{"stat_type", "EX5StatType_End"}, {"value", 3}},
                  {{"stat_type", "EX5StatType_Dex"}, {"value", 3}}}}});
  ASSERT_TRUE(stats);
  EXPECT_EQ(stats->size(), 2);
  EXPECT_EQ(stats->at(0).stat_type, x5::cms::EX5StatType_End);
  EXPECT_EQ(stats->at(1).points, 3);
  EXPECT_FALSE(parse_stat_bonus_request({{"stats", {{{"stat_type", "Unknown"}}}}}));

  // Protobuf JSON omits zero values: no packet_index and a level-0 deactivation.
  const auto perks = parse_perk_change_request(
      {{"updated_perks", {{{"cms_id", 1230201}, {"level", 1}}, {{"cms_id", 1230301}}}}});
  ASSERT_TRUE(perks);
  EXPECT_EQ(perks->packet_index, 0);
  EXPECT_EQ(perks->updates.at(1).level, 0);
  EXPECT_FALSE(parse_perk_change_request({{"updated_perks", {{{"cms_id", "x"}}}}}));

  EXPECT_EQ(parse_perk_reset_request({{"theme_type", "EX5PerkNodeThemeType_LongBow"}}),
            kLongBowTheme);
  EXPECT_EQ(parse_perk_reset_request({{"theme_type", kCrossBowTheme}}), kCrossBowTheme);
  EXPECT_FALSE(parse_perk_reset_request(Json::object()));
}

TEST(ProgressionPackets, EveryMessageEncodesWithTheRecoveredSchemas) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldProgress progress;
  progress.experience = 630;
  progress.level = 2;
  progress.level_up_points = 3;
  progress.weapon_points = 2;
  ExperienceChange change;
  change.previous_level = 1;
  change.levels_gained = 1;

  const std::vector<GameplayPacket> packets{
      {kChangeCharacterBaseNotification, experience_change_message(progress, change)},
      {kChangeCharacterBaseNotification, weapon_points_message(progress)},
      {kChangeStatBonusNotification, level_up_points_message(progress)},
      {kChangePerkResponse,
       {{"packet_index", 1}, {"remaining_char_build_points", remaining_build_points_json(progress)}}},
      {kResetPerkNotification,
       {{"remaining_char_build_points", remaining_build_points_json(progress)}}},
      {kChangeCharacterPublicNotification, {{"unit_id", "4097"}, {"level", 2}}},
      {kUnitPerkNotification, {{"unit_id", "4097"}, {"perks", {{{"cms_id", 1}, {"level", 0}}}}}}};
  for (const auto &packet : packets) {
    const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
    EXPECT_EQ(decoded.at("status"), "decoded") << packet.opcode;
  }
}

// Exercises one store: level, points and build commit together, perk rows are tombstones and
// an invalid attribute is refused. Account and character names carry `suffix` so repeated
// PostgreSQL runs stay independent.
void exercise_progression_storage(const CharacterRepositories &store,
                                  const std::string &suffix) {
  Account account;
  account.name = "progression_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(store.accounts->add_account(account));
  const auto owner = store.accounts->account(account.name);
  ASSERT_TRUE(owner);

  Character character;
  character.account_id = owner->id;
  character.name = "Build" + suffix.substr(suffix.size() > 10 ? suffix.size() - 10 : 0);
  character.class_id = kWildClass;
  const auto created = store.characters->create(character);

  const auto before =
      store.character_state->load_world_progress(owner->id, created.id, created.world_id);
  auto after = before;
  ++after.revision;
  after.level = 3;
  after.experience = 2176;
  after.level_up_points = 3;
  after.weapon_points = 2;
  EXPECT_EQ(before.nui_blessing_level, 0);
  EXPECT_EQ(before.dedicated_nui_tears, 0);
  after.nui_blessing_level = 2;
  after.dedicated_nui_tears = 123;
  after.build.stat_bonuses[x5::cms::EX5StatType_Dex] = 3;
  after.build.perks[1230201] = 1;
  after.build.active_weapon_slot = 1;
  ASSERT_TRUE(store.character_state->save_world_progress(owner->id, created.id, created.world_id,
                                                         before, after));

  const auto loaded =
      store.character_state->load_world_progress(owner->id, created.id, created.world_id);
  EXPECT_EQ(loaded.level, 3);
  EXPECT_EQ(loaded.experience, 2176);
  EXPECT_EQ(loaded.level_up_points, 3);
  EXPECT_EQ(loaded.weapon_points, 2);
  EXPECT_EQ(loaded.nui_blessing_level, 2);
  EXPECT_EQ(loaded.dedicated_nui_tears, 123);
  EXPECT_EQ(loaded.build, after.build);
  const auto stored = store.characters->characters(owner->id, created.world_id).front();
  EXPECT_EQ(stored.level, 3);
  EXPECT_EQ(stored.nui_blessing_level, 2);
  EXPECT_EQ(stored.dedicated_nui_tears, 123);
  EXPECT_EQ(stored.build, after.build);
  const auto by_id = store.characters->character_by_id(created.id);
  ASSERT_TRUE(by_id);
  EXPECT_EQ(by_id->build.active_weapon_slot, 1);
  auto invalid_weapon = loaded;
  ++invalid_weapon.revision;
  invalid_weapon.build.active_weapon_slot = 2;
  EXPECT_THROW(store.character_state->save_world_progress(owner->id, created.id, created.world_id,
                                                          loaded, invalid_weapon),
               std::invalid_argument);
  EXPECT_EQ(by_id->nui_blessing_level, 2);
  EXPECT_EQ(by_id->dedicated_nui_tears, 123);

  for (const int invalid_level : {-1, 31}) {
    auto invalid_blessing = loaded;
    ++invalid_blessing.revision;
    invalid_blessing.nui_blessing_level = invalid_level;
    EXPECT_THROW(store.character_state->save_world_progress(owner->id, created.id, created.world_id,
                                          loaded, invalid_blessing), std::invalid_argument);
  }
  for (const int invalid_balance : {-1, kMaximumDedicatedNuiTears + 1}) {
    auto invalid_tears = loaded;
    ++invalid_tears.revision;
    invalid_tears.dedicated_nui_tears = invalid_balance;
    EXPECT_THROW(store.character_state->save_world_progress(owner->id, created.id, created.world_id,
                                          loaded, invalid_tears), std::invalid_argument);
  }
  EXPECT_EQ(store.character_state->load_world_progress(owner->id, created.id, created.world_id),
            loaded);

  // Deactivation keeps the row with level 0; removing it is refused.
  auto deactivated = loaded;
  ++deactivated.revision;
  deactivated.build.perks[1230201] = 0;
  deactivated.nui_blessing_level = kMaximumNuiBlessingLevel;
  deactivated.dedicated_nui_tears = kMaximumDedicatedNuiTears;
  ASSERT_TRUE(store.character_state->save_world_progress(owner->id, created.id, created.world_id,
                                                         loaded, deactivated));
  const auto maximum_blessing =
      store.character_state->load_world_progress(owner->id, created.id, created.world_id);
  EXPECT_EQ(maximum_blessing.nui_blessing_level, kMaximumNuiBlessingLevel);
  EXPECT_EQ(maximum_blessing.dedicated_nui_tears, kMaximumDedicatedNuiTears);
  EXPECT_EQ(store.character_state->load_world_progress(owner->id, created.id, created.world_id)
                .build.perks,
            (std::map<int, int>{{1230201, 0}}));

  auto removed = deactivated;
  ++removed.revision;
  removed.build.perks.clear();
  EXPECT_THROW(store.character_state->save_world_progress(owner->id, created.id, created.world_id,
                                                          deactivated, removed),
               std::invalid_argument);
  auto invalid = deactivated;
  ++invalid.revision;
  invalid.build.stat_bonuses[x5::cms::EX5StatType_MaxHP] = 1;
  EXPECT_THROW(store.character_state->save_world_progress(owner->id, created.id, created.world_id,
                                                          deactivated, invalid),
               std::invalid_argument);
}

std::string unique_suffix() {
  return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

TEST(ProgressionStorage, MemoryStoreKeepsLevelPointsAndBuildTogether) {
  MemoryRepositories repositories;
  exercise_progression_storage(repositories.character_repositories(), unique_suffix());
}

// Needs a disposable cluster migrated to schema version 29 (never the development database).
TEST(ProgressionStorage, PostgresKeepsLevelPointsAndBuildTogether) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }
  exercise_progression_storage(make_postgres_character_repositories(connection),
                               unique_suffix());
}

// Quest kills and the 500 EXP reward of the shared quest fixture level a character to 2.
Json leveling_quest_fixture() {
  Json document{{"format_version", 1},
                {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
                {"map_id", 8000},
                {"objects", Json::array()},
                {"doodads", Json::array()},
                {"sectors", Json::array()},
                {"civilians", Json::array()},
                {"npc_spawns", Json::array()},
                {"utility_items", Json::array()},
                {"quests", Json::array()}};
  constexpr std::uint64_t kMonster = (2ULL << 60) | (1ULL << 12) | 2;
  document["npc_spawns"].push_back({{"unit_id", std::to_string(kMonster)},
                                    {"cms_id", kTutorialMonsterCmsId},
                                    {"position", {0, 0, 0}},
                                    {"yaw", 0},
                                    {"spawner_guid", {"123", "456"}},
                                    {"anim_stance_key", 0},
                                    {"behavior", "Monster"},
                                    {"faction", "EX5Faction_Bandit"}});
  document["quests"].push_back({{"cms_id", 200},
                                {"auto_complete", false},
                                {"sequential", true},
                                {"accept_triggers", Json::array()},
                                {"experience", 500},
                                {"rewards", Json::array()},
                                {"build_points", {{{"type", 2}, {"value", 1}}}},
                                {"progresses",
                                 {{{"sequence", 1},
                                   {"group", 1},
                                   {"kind", "HuntMonster"},
                                   {"parameters", {{"npcId", kTutorialMonsterCmsId}, {"count", 1}}}}}}});
  return document;
}

TEST_F(ProgressionRules, TutorialKillAndQuestRewardLevelTheCharacter) {
  const auto data = std::make_shared<TutorialData>(leveling_quest_fixture());
  WorldProgress durable;
  durable.quests[200].objectives[1] = 0;
  TutorialSession session(
      data, 7, durable,
      [&durable](const WorldProgress &before, const WorldProgress &after) {
        if (before.revision != durable.revision) {
          return false;
        }
        durable = after;
        return true;
      },
      progression);
  const WorldPosition position{8000, 0, 0, 0, 0};
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto verify = [&protocol](const std::vector<GameplayPacket> &packets) {
    for (const auto &packet : packets) {
      const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
      EXPECT_EQ(decoded.at("status"), "decoded") << packet.opcode;
    }
  };

  constexpr std::uint64_t kMonster = (2ULL << 60) | (1ULL << 12) | 2;
  const auto kill = session.record_monster_defeat(kMonster, position);
  verify(kill);
  ASSERT_FALSE(kill.empty());
  EXPECT_EQ(kill.back().opcode, kChangeCharacterBaseNotification);
  EXPECT_EQ(kill.back().message, (Json{{"exp", "6"}}));
  EXPECT_EQ(durable.experience, 6);
  EXPECT_EQ(durable.level, 1);

  const auto reward = session.receive(20018, {{"quest_cms_id", 200}}, 0, position, 100000000);
  verify(reward);
  EXPECT_EQ(durable.experience, 506);
  EXPECT_EQ(durable.level, 2);
  EXPECT_EQ(durable.level_up_points, 3);
  EXPECT_EQ(durable.weapon_points, 1);
  const auto level_up = std::ranges::find_if(reward, [](const GameplayPacket &packet) {
    return packet.opcode == kChangeCharacterBaseNotification && packet.message.contains("level");
  });
  ASSERT_NE(level_up, reward.end());
  EXPECT_EQ(level_up->message.at("level"), 2);
}

// Optional real-catalog checks (skipped when data/generated/catalog is absent).
std::filesystem::path real_catalog_root() {
  const char *configured = std::getenv("CHRONICLES_CATALOG_DIR");
  return configured ? std::filesystem::path(configured)
                    : std::filesystem::path("data/generated/catalog");
}

TEST(ProgressionRealCatalog, MatchesTheClientBuildAndCapturedSnapshots) {
  const auto root = real_catalog_root();
  if (!std::filesystem::exists(root / "manifest.json")) {
    GTEST_SKIP() << "exported client catalog not present";
  }
  ClientCatalog catalog(root);
  const auto progression = Progression::load(catalog);

  EXPECT_EQ(progression->max_level(), 40);
  EXPECT_EQ(progression->next_level_experience(1), 500);
  EXPECT_EQ(progression->next_level_experience(7), 19500);
  EXPECT_EQ(progression->level_for_experience(20123), 8);
  // Official level-1 snapshots list 38 Wild and 42 Magic auto-registered nodes.
  EXPECT_EQ(progression->active_perks(kWildClass, {}).size(), 38);
  EXPECT_EQ(progression->active_perks(kMagicClass, {}).size(), 42);

  // Captured level-6 sequence: LongBow 1230201 (1 point), then 1230203 and 1230403 together.
  WorldProgress progress;
  progress.weapon_points = 3;
  ASSERT_EQ(progression->change_perks(progress, kWildClass, {{1230201, 1}}), x5::ec::Success);
  ASSERT_EQ(progression->change_perks(progress, kWildClass, {{1230203, 1}, {1230403, 1}}),
            x5::ec::Success);
  EXPECT_EQ(progress.weapon_points, 0);
}

TEST(ProgressionRealCatalog, AllocatedAttributesAndStatPerksRaiseStats) {
  const auto root = real_catalog_root();
  if (!std::filesystem::exists(root / "manifest.json")) {
    GTEST_SKIP() << "exported client catalog not present";
  }
  ClientCatalog catalog(root);
  const auto stats = CharacterStats::load(catalog);

  Character character;
  character.id = 1;
  character.class_id = kWildClass;
  const auto base = stats->stats(character);

  character.build.stat_bonuses[x5::cms::EX5StatType_Str] = 3;
  // Wild LongBow Stat perk 1230605: TidalAttackPower +20.
  character.build.perks[1230605] = 1;
  const auto raised = stats->stats(character);

  EXPECT_EQ(raised.at("5").get<int>(), base.at("5").get<int>() + 3);
  // Core physical power is 6 * Str + Dex below Str 70.
  EXPECT_EQ(raised.at("22").get<int>(), base.at("22").get<int>() + 18);
  EXPECT_EQ(raised.at("63").get<int>(), base.at("63").get<int>() + 20);

  character.build.perks[1230605] = 0;
  EXPECT_EQ(stats->stats(character).at("63"), base.at("63"));

  character.nui_blessing_level = 30;
  const auto blessed = stats->stats(character);
  EXPECT_EQ(blessed.at("18").get<int>(), base.at("18").get<int>() + 600);
  EXPECT_EQ(blessed.at("19"), base.at("19"));
}

} // namespace
} // namespace chronicles
