#include "world/features/character/character_stats.hpp"
#include "cms.generated.pb.h"
#include "world/session/native_world.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <openssl/evp.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;

class StartingStatCatalog {
public:
  explicit StartingStatCatalog(
      std::string wild_attributes = "Str:15,Int:12,Con:8,End:11,Dex:27,Tal:17",
      std::string source_hash = "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6")
      : root_(std::filesystem::temp_directory_path() / ("chronicles-stat-test-" + random_hex(8))) {
    std::filesystem::create_directories(root_ / "Tables");
    Json tables = Json::object();
    for (const auto &row : Json::array({
             {{"FK_character", "Fight"}, {"typeValue", "Str:29,Int:10,Con:17,End:7,Dex:16,Tal:11"}},
             {{"FK_character", "Wild"}, {"typeValue", std::move(wild_attributes)}},
             {{"FK_character", "Adamant"},
              {"typeValue", "Str:18,Int:4,Con:26,End:25,Dex:6,Tal:11"}},
             {{"FK_character", "Magic"}, {"typeValue", "Str:10,Int:30,Con:6,End:7,Dex:17,Tal:20"}},
         })) {
      tables["CharacterBasicStat"].push_back(row);
    }

    for (const auto &[class_name, node_id] :
         std::array<std::pair<const char *, int>, 6>{{
             {"Fight", 1010101},
             {"Wild", 1210101},
             {"Adamant", 1310101},
             {"Magic", 1425101},
             {"Magic", 1425102},
             {"Magic", 1425201},
         }}) {
      tables["PerkNode"].push_back({{"characterClass", class_name},
                                     {"id", std::to_string(node_id)},
                                     {"autoRegistration", "True"}});
    }

    Json grade{{"grade", "Normal"}, {"basicStatRatio", "1000"}};
    const std::array<const char *, 22> rates{{
        "physicalDamageRate", "magicDamageRate", "windDamageRate",    "fogDamageRate",
        "quakeDamageRate",    "fireDamageRate",  "tidalDamageRate",   "electricDamageRate",
        "ironDamageRate",     "hitDamageRate",   "impactDamageRate",  "physicalArmorRate",
        "magicArmorRate",     "windArmorRate",   "fogArmorRate",      "quakeArmorRate",
        "fireArmorRate",      "tidalArmorRate",  "electricArmorRate", "ironArmorRate",
        "hitArmorRate",       "impactArmorRate",
    }};
    for (const auto *field : rates) {
      grade[field] = "1000";
    }
    tables["EquipmentGrade"].push_back(grade);
    grade["grade"] = "Grand";
    tables["EquipmentGrade"].push_back(grade);
    grade["grade"] = "Rare";
    grade["windDamageRate"] = "1500";
    tables["EquipmentGrade"].push_back(grade);
    for (int level = 1; level <= 30; ++level) {
      tables["NuiBlessingLevel"].push_back(
          {{"level", std::to_string(level)},
           {"maxHpReward", std::to_string(level == 2 ? 35 : 20)},
           {"maxSpReward", level == 2 ? "7" : ""}});
    }
    tables["Stat"] = Json::array();
    tables["GameVariable"] = Json::array({
        {{"name", "DurabilityDangerRatio"}, {"value", "0.1"}},
        {{"name", "DurabilityPanaltyValue"}, {"value", "0.5"}},
    });
    const auto equipment = [&](int id, int level, const char *slot, Json attributes) {
      const auto key = std::to_string(id);
      tables["Item"].push_back({{"id", key},
                                {"level", std::to_string(level)},
                                {"equipSlot", slot},
                                {"maxDurability", "10000"},
                                {"FK_itemAttribute", key}});
      attributes["id"] = key;
      tables["ItemAttribute"].push_back(std::move(attributes));
    };
    equipment(11101102, 1, "Weapon_1",
              {{"physicalDamageRate", "70"},
               {"magicDamageRate", "30"},
               {"hitDamageRate", "100"},
               {"impactDamageRate", "100"}});
    equipment(11102302, 1, "Weapon_1",
              {{"physicalDamageRate", "44"},
               {"magicDamageRate", "36"},
               {"hitDamageRate", "90"},
               {"impactDamageRate", "80"}});
    equipment(11103102, 1, "Weapon_1",
              {{"physicalDamageRate", "100"},
               {"magicDamageRate", "25"},
               {"hitDamageRate", "100"},
               {"impactDamageRate", "100"}});
    equipment(11104102, 1, "Weapon_1",
              {{"physicalDamageRate", "8"},
               {"magicDamageRate", "72"},
               {"hitDamageRate", "90"},
               {"impactDamageRate", "70"}});
    equipment(13104202, 6, "Weapon_1",
              {{"physicalDamageRate", "80"},
               {"magicDamageRate", "20"},
               {"hitDamageRate", "115"},
               {"impactDamageRate", "100"}});
    // Polish increase table 100100 (steps 1 and 2) belongs to the weapon 13102102 captured with
    // two equipped polishes; the necklace proves non-weapon items are not scaled.
    tables["ItemPolishIncreaseValue"].push_back(
        {{"id", "100100"}, {"polishStep", "1"}, {"polishIncreasePercent", "17.8"}});
    tables["ItemPolishIncreaseValue"].push_back(
        {{"id", "100100"}, {"polishStep", "2"}, {"polishIncreasePercent", "35.5"}});
    equipment(13102102, 6, "Weapon_2",
              {{"physicalDamageRate", "52"},
               {"magicDamageRate", "13"},
               {"hitDamageRate", "80"},
               {"impactDamageRate", "70"},
               {"FK_itemPolishIncreaseValueId", "100100"}});
    equipment(19000006, 1, "Necklace",
              {{"basicStat1", "PhysicalArmor"},
               {"basicStatRatio1", "16.1"},
               {"FK_itemPolishIncreaseValueId", "100100"}});
    // Polishable armor and a large basic-stat bonus, so a 35.5% step is visible after rounding.
    equipment(19000008, 1, "ArmorSuit",
              {{"physicalArmorRate", "70"}, {"FK_itemPolishIncreaseValueId", "100100"}});
    equipment(19000009, 1, "Necklace",
              {{"basicStat1", "PhysicalArmor"},
               {"basicStatRatio1", "300"},
               {"FK_itemPolishIncreaseValueId", "100100"}});
    equipment(11500101, 1, "Necklace",
              {{"basicStat1", "PhysicalArmor"}, {"basicStatRatio1", "16.1"}});
    equipment(19000001, 1, "Weapon_1",
              {{"windDamageRate", "30"},
               {"fireDamageRate", "40"},
               {"electricDamageRate", "20"},
               {"fogDamageRate", "60"},
               {"windArmorRate", "10"},
               {"tidalArmorRate", "20"}});
    equipment(19000002, 1, "Weapon_1", {{"hitArmorRate", "18"}, {"impactArmorRate", "10"}});
    equipment(19000003, 1, "Weapon_1", {{"hitArmorRate", "18.4"}});
    equipment(19000004, 1, "Necklace", {{"basicStat1", "Tal"}, {"basicStatRatio1", "16.1"}});
    equipment(19000005, 1, "Necklace", {{"basicStat1", "Tal"}, {"basicStatRatio1", "32.1"}});
    const std::array<const char *, 4> slots{
        {"ArmorHelmet", "ArmorSuit", "ArmorGloves", "ArmorShoes"}};
    const std::array<const char *, 4> cloth_physical{{"18", "31.5", "27", "13.5"}};
    const std::array<const char *, 4> cloth_magic{{"36", "63", "54", "27"}};
    const std::array<const char *, 4> heavy_physical{{"40", "70", "60", "30"}};
    const std::array<const char *, 4> heavy_magic{{"16.1", "28.1", "24.1", "12.1"}};
    for (int index = 0; index < 4; ++index) {
      equipment(
          11403101 + index * 100, 1, slots[index],
          {{"physicalArmorRate", cloth_physical[index]}, {"magicArmorRate", cloth_magic[index]}});
      equipment(
          11401101 + index * 100, 1, slots[index],
          {{"physicalArmorRate", heavy_physical[index]}, {"magicArmorRate", heavy_magic[index]}});
    }
    Json files = Json::array();
    for (const auto &[name, entries] : tables.items()) {
      std::string rows;
      for (const auto &entry : entries) {
        rows += entry.dump() + "\n";
      }
      const auto path = "Tables/" + name + ".jsonl";
      write(root_ / path, rows);
      files.push_back({{"path", path},
                       {"bytes", rows.size()},
                       {"records", entries.size()},
                       {"sha256", hash(rows)}});
    }
    const Json manifest{{"format_version", 1},
                        {"source_executable_dump_sha256", std::move(source_hash)},
                        {"table_count", tables.size()},
                        {"files", std::move(files)}};
    write(root_ / "manifest.json", manifest.dump());
  }

  ~StartingStatCatalog() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  std::shared_ptr<const CharacterStats> load() const {
    ClientCatalog catalog(root_);
    return CharacterStats::load(catalog);
  }

private:
  static std::string hash(const std::string &rows) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size{};
    if (EVP_Digest(rows.data(), rows.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) !=
        1) {
      throw std::runtime_error("Cannot hash starting stat fixture");
    }
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string hash;
    for (unsigned int index = 0; index < digest_size; ++index) {
      hash.push_back(kHexDigits[digest[index] >> 4]);
      hash.push_back(kHexDigits[digest[index] & 15]);
    }

    return hash;
  }

  static void write(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream output(path, std::ios::binary);
    output << contents;
    if (!output) {
      throw std::runtime_error("Cannot write starting stat fixture");
    }
  }

  std::filesystem::path root_;
};

TEST(CharacterStats, MatchesCaptureSupportedClassVitalsAndPower) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  struct Expected {
    int class_id;
    int hp;
    int sp;
    int physical_power;
    int magic_power;
  };
  const std::array<Expected, 4> classes{{
      {1, 309, 540, 190, 76},
      {2, 248, 567, 117, 99},
      {3, 363, 651, 114, 30},
      {5, 234, 540, 77, 197},
  }};
  for (const auto &expected : classes) {
    Character character;
    character.id = expected.class_id;
    character.class_id = expected.class_id;
    const auto stats = definitions->stats(character);
    EXPECT_EQ(stats.at("18"), expected.hp);
    EXPECT_EQ(stats.at("69"), expected.hp);
    EXPECT_EQ(stats.at("19"), expected.sp);
    EXPECT_EQ(stats.at("72"), expected.sp);
    EXPECT_EQ(stats.at("22"), expected.physical_power);
    EXPECT_EQ(stats.at("17"), expected.magic_power);
    EXPECT_EQ(stats.at("84"), 1000);
    EXPECT_GT(stats.at("52").get<int>(), 0);
    EXPECT_GT(stats.at("53").get<int>(), 0);
  }
}

TEST(CharacterStats, NuiBlessingAddsCumulativeCatalogRewardsAndExposesStoredBalances) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Character character;
  character.id = 1;
  const auto base = definitions->stats(character);

  character.nui_blessing_level = 2;
  character.dedicated_nui_tears = 123;
  const auto blessed = definitions->stats(character);
  EXPECT_EQ(blessed.at("18").get<int>(), base.at("18").get<int>() + 55);
  EXPECT_EQ(blessed.at("19").get<int>(), base.at("19").get<int>() + 7);
  EXPECT_EQ(blessed.at("69"), blessed.at("18"));
  EXPECT_EQ(blessed.at("72"), blessed.at("19"));
  const auto notification = definitions->base_notification(character);
  EXPECT_EQ(notification.at("nui_blessing_level"), 2);
  EXPECT_EQ(notification.at("dedicated_nui_tears"), 123);

  character.nui_blessing_level = 30;
  EXPECT_EQ(definitions->stats(character).at("18").get<int>(),
            base.at("18").get<int>() + 615);
  for (const int invalid_level : {-1, 31}) {
    character.nui_blessing_level = invalid_level;
    EXPECT_THROW(definitions->stats(character), std::invalid_argument);
    EXPECT_THROW(definitions->base_notification(character), std::invalid_argument);
  }
  character.nui_blessing_level = 0;
  for (const int invalid_balance : {-1, kMaximumDedicatedNuiTears + 1}) {
    character.dedicated_nui_tears = invalid_balance;
    EXPECT_THROW(definitions->base_notification(character), std::invalid_argument);
    EXPECT_THROW(definitions->stats(character), std::invalid_argument);
  }
  character.dedicated_nui_tears = kMaximumDedicatedNuiTears;
  EXPECT_EQ(definitions->base_notification(character).at("dedicated_nui_tears"), 100000);
}

TEST(CharacterStats, RejectsMalformedAttributesUnknownBuildAndInvalidCharacter) {
  for (const auto *attributes :
       {"Str:15,Int:12,Con:8,End:11,Dex:27,Tal:-1", "Str:15,Int:12,Con:8,End:11,Dex:27,Str:17",
        "Str:15,Int:12,Con:8,End:11,Dex:27,Tal:17,",
        "Str:15,Int:12,Con:8,End:11,Dex:27,Tal:9999999999999"}) {
    StartingStatCatalog invalid(attributes);
    EXPECT_THROW(invalid.load(), std::runtime_error);
  }
  StartingStatCatalog unknown_build("Str:15,Int:12,Con:8,End:11,Dex:27,Tal:17", "unknown");
  EXPECT_THROW(unknown_build.load(), std::runtime_error);

  StartingStatCatalog valid;
  const auto definitions = valid.load();
  Character character;
  character.id = 1;
  character.class_id = 4;
  EXPECT_THROW(definitions->stats(character), std::invalid_argument);
  character.class_id = 2;
  character.experience = -1;
  EXPECT_THROW(definitions->base_notification(character), std::invalid_argument);
}

CharacterItem equipped_item(int cms_id, int slot, int durability = 10000) {
  CharacterItem item;
  item.id = cms_id;
  item.cms_id = cms_id;
  item.equip_slot = slot;
  item.durability = durability;
  return item;
}

TEST(CharacterStats, MatchesCapturedEquipmentDefenseCriticalAndElementalRatings) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  struct Expected {
    int class_id;
    int weapon;
    int physical_armor;
    int magic_armor;
    int physical_amplification;
    int magic_amplification;
    int critical;
    int physical_critical_resist;
    int magic_critical_resist;
    int physical_penetration;
    int magic_penetration;
    int wind;
  };
  const std::array<Expected, 4> cases{{
      {1, 11101102, 45, 30, 35, 9, 22, 12, 12, 13, 8, 1},
      {2, 11102302, 57, 41, 17, 14, 37, 6, 6, 18, 12, 2},
      {3, 11103102, 84, 68, 28, 6, 9, 18, 19, 6, 7, 1},
      {5, 11104102, 33, 45, 5, 37, 23, 4, 5, 12, 15, 2},
  }};
  for (const auto &expected : cases) {
    SCOPED_TRACE(expected.class_id);
    Character character;
    character.id = expected.class_id;
    character.class_id = expected.class_id;
    character.items.push_back(equipped_item(expected.weapon, 0));
    for (int index = 0; index < 4; ++index) {
      const auto helmet = expected.class_id == 5 ? 11403101 : 11401101;
      character.items.push_back(equipped_item(helmet + index * 100, 19 + index));
    }
    if (expected.class_id == 2 || expected.class_id == 3) {
      character.items.push_back(equipped_item(11500101, 11));
    }
    const auto stats = definitions->stats(character);
    EXPECT_EQ(stats.at("52"), expected.physical_armor);
    EXPECT_EQ(stats.at("47"), expected.magic_armor);
    EXPECT_EQ(stats.at("53"), expected.physical_amplification);
    EXPECT_EQ(stats.at("48"), expected.magic_amplification);
    EXPECT_EQ(stats.at("54"), expected.critical);
    EXPECT_EQ(stats.at("49"), expected.critical);
    EXPECT_EQ(stats.at("55"), expected.physical_critical_resist);
    EXPECT_EQ(stats.at("50"), expected.magic_critical_resist);
    EXPECT_EQ(stats.at("56"), expected.physical_penetration);
    EXPECT_EQ(stats.at("51"), expected.magic_penetration);
    EXPECT_EQ(stats.at("66"), expected.wind);
    for (const auto *key : {"30", "33", "36", "45", "63"}) {
      EXPECT_EQ(stats.at(key), 1);
    }
    for (const auto *key : {"29", "32", "35", "44", "62", "65", "82", "83", "89", "90"}) {
      EXPECT_EQ(stats.at(key), 0);
    }
  }
}

TEST(CharacterStats, ActiveWeaponAndDurabilityUseSeparateEquipmentContributions) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Character character;
  character.id = 1;
  character.class_id = 5;
  character.items = {equipped_item(11104102, 0), equipped_item(13104202, 1)};
  character.items[1].grade = 1;
  auto stats = definitions->stats(character);
  EXPECT_EQ(stats.at("48"), 37);
  EXPECT_EQ(stats.at("53"), 5);
  EXPECT_EQ(stats.at("39"), 90);
  stats = definitions->stats(character, 1);
  EXPECT_EQ(stats.at("48"), 31);
  EXPECT_EQ(stats.at("53"), 38);
  EXPECT_EQ(stats.at("39"), 115);
  EXPECT_EQ(stats.at("42"), 100);

  character.items[0].durability = 1000;
  stats = definitions->stats(character);
  EXPECT_EQ(stats.at("48"), 30);
  EXPECT_EQ(stats.at("53"), 4);
  EXPECT_EQ(stats.at("39"), 90);
  character.items[0].durability = 1001;
  EXPECT_EQ(definitions->stats(character).at("48"), 37);
  character.items[0].durability = 0;
  EXPECT_EQ(definitions->stats(character).at("48"), 30);
  character.items[0].equip_slot.reset();
  EXPECT_EQ(definitions->stats(character).at("48"), 22);
}

// Weapon 13102102 (Grand, level 6, Weapon_2) with the captured equipped-polish frames. The Wild
// attributes are chosen so the catalog power equals the physical/magic power the official server
// reported in the same session (ChangeStatNoti stats 22 and 17); only the attack-power result is
// compared. Observed totals: step 0 = 34/12, step 1 = 38/13, step 2 = 43/15 (stats 53/48).
TEST(CharacterStats, EquippedWeaponPolishReproducesCapturedAttackPowerFrames) {
  struct Frame {
    const char *wild_attributes;
    int polish_step;
    int physical_attack;
    int magic_attack;
  };
  const std::array<Frame, 4> frames{{
      {"Str:18,Int:12,Con:8,End:11,Dex:30,Tal:17", 0, 34, 12},
      {"Str:18,Int:12,Con:8,End:11,Dex:30,Tal:17", 1, 38, 13},
      {"Str:18,Int:12,Con:8,End:11,Dex:36,Tal:17", 1, 39, 14},
      {"Str:18,Int:12,Con:8,End:11,Dex:39,Tal:17", 2, 43, 15},
  }};
  for (const auto &frame : frames) {
    SCOPED_TRACE(frame.polish_step);
    StartingStatCatalog catalog(frame.wild_attributes);
    const auto definitions = catalog.load();
    Character character;
    character.id = 1;
    character.class_id = 2;
    character.items = {equipped_item(13102102, 1)};
    character.items[0].grade = 1;
    character.items[0].polish_step = frame.polish_step;

    const auto stats = definitions->stats(character, 1);
    EXPECT_EQ(stats.at("53"), frame.physical_attack);
    EXPECT_EQ(stats.at("48"), frame.magic_attack);
    EXPECT_EQ(stats.at("39"), 80);
    EXPECT_EQ(stats.at("42"), 70);
  }
}

TEST(CharacterStats, WeaponPolishAppliesToTheActiveWeaponOnly) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Character character;
  character.id = 1;
  character.class_id = 2;
  character.items = {equipped_item(13102102, 1)};
  character.items[0].grade = 1;
  const auto unpolished_inactive = definitions->stats(character, 0);

  character.items[0].polish_step = 2;
  const auto polished_inactive = definitions->stats(character, 0);
  EXPECT_EQ(polished_inactive.at("53"), unpolished_inactive.at("53"));
  EXPECT_EQ(polished_inactive.at("48"), unpolished_inactive.at("48"));
  EXPECT_GT(definitions->stats(character, 1).at("53"), unpolished_inactive.at("53"));
}

// Static: AAC_Equipment_CalculatePhysicalArmor (like every curve-based helper) and
// AAC_Equipment_CalculateBasicStatBonus multiply by float(polish + 1); no capture covers it.
TEST(CharacterStats, PolishScalesArmorAndBasicStatBonusesLikeTheClient) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Character character;
  character.id = 1;
  character.class_id = 2;
  const auto base_armor = definitions->stats(character, 0).at("52").get<int>();

  for (const auto &[cms_id, slot] : {std::pair{19000008, 20}, std::pair{19000009, 11}}) {
    SCOPED_TRACE(cms_id);
    character.items = {equipped_item(cms_id, slot)};
    const auto unpolished = definitions->stats(character, 0).at("52").get<int>() - base_armor;
    character.items[0].polish_step = 2;
    const auto polished = definitions->stats(character, 0).at("52").get<int>() - base_armor;

    EXPECT_GT(unpolished, 5);
    EXPECT_GT(polished, unpolished);
    // Step 2 of table 100100 is +35.5%; the rounded term moves at most one point from it.
    EXPECT_NEAR(polished, unpolished * 1.355, 1.0);
  }
}

// The client reads polish 0 for a step outside the item's increase table
// (AAC_Equipment_Calculate*), so such a step adds nothing; a negative step is invalid.
TEST(CharacterStats, PolishStepsOutsideTheIncreaseTableAddNothing) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Character character;
  character.id = 1;
  character.class_id = 2;
  character.items = {equipped_item(13102102, 1)};
  character.items[0].grade = 1;
  const auto unpolished = definitions->stats(character, 1);

  character.items[0].polish_step = 3;
  EXPECT_EQ(definitions->stats(character, 1), unpolished);
  character.items[0].polish_step = -1;
  EXPECT_THROW(definitions->stats(character, 1), std::invalid_argument);
}

TEST(CharacterStats, RejectsUnknownDuplicateAndMisplacedEquipment) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Character character;
  character.id = 1;
  character.class_id = 5;
  EXPECT_THROW(definitions->stats(character, 2), std::invalid_argument);
  for (const auto &item :
       {equipped_item(99, 0), equipped_item(11104102, 19), equipped_item(19000003, 0),
        equipped_item(11104102, 0, -1), equipped_item(11104102, 0, 10001)}) {
    character.items = {item};
    EXPECT_THROW(definitions->stats(character), std::invalid_argument);
  }
  character.items = {equipped_item(11104102, 0), equipped_item(13104202, 0)};
  EXPECT_THROW(definitions->stats(character), std::invalid_argument);
}

TEST(CharacterStats, ElementalEquipmentUsesGradeAndSeparateAttackArmorCurves) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Character character;
  character.id = 1;
  character.class_id = 5;
  character.items = {equipped_item(19000001, 0)};
  auto stats = definitions->stats(character);
  EXPECT_EQ(stats.at("66"), 8);
  EXPECT_EQ(stats.at("33"), 9);
  EXPECT_EQ(stats.at("30"), 5);
  EXPECT_EQ(stats.at("36"), 14);
  EXPECT_EQ(stats.at("65"), 1);
  EXPECT_EQ(stats.at("62"), 3);
  character.items.front().grade = 2;
  stats = definitions->stats(character);
  EXPECT_EQ(stats.at("66"), 12);
  EXPECT_EQ(stats.at("33"), 9);
  character.items = {equipped_item(19000002, 0)};
  stats = definitions->stats(character);
  EXPECT_EQ(stats.at("38"), 18);
  EXPECT_EQ(stats.at("41"), 10);
}

TEST(CharacterStats, TalentBonusesUseDistinctCapturedElementalThresholds) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Character character;
  character.id = 1;
  character.class_id = 5;
  character.items = {equipped_item(19000004, 11)};
  auto stats = definitions->stats(character);
  EXPECT_EQ(stats.at("6"), 21);
  EXPECT_EQ(stats.at("33"), 2);
  EXPECT_EQ(stats.at("36"), 1);
  EXPECT_EQ(stats.at("66"), 3);
  EXPECT_EQ(stats.at("51"), 16);

  character.items = {equipped_item(19000005, 11)};
  stats = definitions->stats(character);
  EXPECT_EQ(stats.at("6"), 22);
  EXPECT_EQ(stats.at("33"), 2);
  EXPECT_EQ(stats.at("36"), 2);
  EXPECT_EQ(stats.at("66"), 3);
  EXPECT_EQ(stats.at("51"), 17);
}

TEST(CharacterStats, BootstrapMetadataAndNubUseSameOwnedCharacter) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol, true, definitions);
  Character character;
  character.id = 42;
  character.class_id = 2;
  character.name = "LocalArcher";
  character.experience = 123;
  ASSERT_TRUE(handshake.configure_bootstrap_character(character));
  ASSERT_TRUE(handshake.receive(protocol.encode(20001, Json::object(), 0), 100, 100));

  const auto clock = handshake.take_pending_notification();
  ASSERT_TRUE(clock);
  EXPECT_EQ(clock->opcode(), 1);
  const auto base = handshake.take_pending_notification();
  ASSERT_TRUE(base);
  EXPECT_EQ(base->opcode(), 125);
  const auto metadata = protocol.decode(*base).at("message");
  EXPECT_EQ(metadata.at("char_id"), "42");
  EXPECT_EQ(metadata.at("name"), "LocalArcher");
  EXPECT_EQ(metadata.at("exp"), "123");
  const auto expertise = handshake.take_pending_notification();
  ASSERT_TRUE(expertise);
  EXPECT_EQ(expertise->opcode(), 128);
  const auto inventory = handshake.take_pending_notification();
  ASSERT_TRUE(inventory);
  EXPECT_EQ(inventory->opcode(), 130);
  const auto learned_crafts = handshake.take_pending_notification();
  ASSERT_TRUE(learned_crafts);
  EXPECT_EQ(learned_crafts->opcode(), 132);
  const auto perks = handshake.take_pending_notification();
  ASSERT_TRUE(perks);
  EXPECT_EQ(perks->opcode(), 135);
  const auto perk_message = protocol.decode(*perks).at("message");
  ASSERT_EQ(perk_message.at("perks").size(), 1);
  EXPECT_EQ(perk_message.at("perks").at(0).at("cms_id"), 1210101);
  EXPECT_EQ(perk_message.at("perks").at(0).at("level"), 1);
  EXPECT_FALSE(handshake.take_pending_notification());

  const auto complete = handshake.receive(protocol.encode(20085, Json::object(), 1), 100, 100);
  ASSERT_TRUE(complete);
  const auto player = protocol.decode(*complete).at("message").at("nub").at("player");
  EXPECT_EQ(player.at("stats").at("69"), 248);
  EXPECT_EQ(player.at("stats").at("2"), 27);
  const auto snapshot = handshake.presence_snapshot();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->player_stats, definitions->stats(character));
}

TEST(CharacterStats, MagicEntryGrantsAutoRegisteredStaffAndBasicAttackPerks) {
  StartingStatCatalog catalog;
  const auto definitions = catalog.load();
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldHandshake handshake(protocol, true, definitions);
  Character character;
  character.id = 42;
  character.class_id = 5;
  character.name = "LocalMage";
  ASSERT_TRUE(handshake.configure_bootstrap_character(character));
  ASSERT_TRUE(handshake.receive(protocol.encode(20001, Json::object(), 0), 100, 100));

  const auto clock = handshake.take_pending_notification();
  const auto base = handshake.take_pending_notification();
  const auto expertise = handshake.take_pending_notification();
  const auto inventory = handshake.take_pending_notification();
  const auto learned_crafts = handshake.take_pending_notification();
  const auto perk_packet = handshake.take_pending_notification();
  ASSERT_TRUE(clock);
  ASSERT_TRUE(base);
  ASSERT_TRUE(expertise);
  ASSERT_TRUE(inventory);
  ASSERT_TRUE(learned_crafts);
  ASSERT_TRUE(perk_packet);
  EXPECT_EQ(perk_packet->opcode(), 135);
  const auto perks = protocol.decode(*perk_packet).at("message").at("perks");
  ASSERT_EQ(perks.size(), 3);
  EXPECT_EQ(perks.at(0), (Json{{"cms_id", 1425101}, {"level", 1}}));
  EXPECT_EQ(perks.at(1), (Json{{"cms_id", 1425102}, {"level", 1}}));
  EXPECT_FALSE(handshake.take_pending_notification());
}

TEST(CharacterStats, ComparesBaseAttributeSnapshotsInOptionalPrivateCapture) {
  const auto *capture_path = std::getenv("CHRONICLES_CHARACTER_STAT_CAPTURE");
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!capture_path || !catalog_path) {
    GTEST_SKIP() << "Set private capture and catalog paths to check recorded values";
  }

  ClientCatalog catalog(catalog_path);
  const auto definitions = CharacterStats::load(catalog);
  std::ifstream capture(capture_path);
  ASSERT_TRUE(capture);
  std::map<int, int> checked_classes;
  std::string line;
  while (std::getline(capture, line)) {
    const auto packet = Json::parse(line);
    const auto type = packet.value("packet", std::string{});
    if (type != "PS2C_MapMoveCompleteNoti" && type != "PS2C_UnitAddNoti") {
      continue;
    }
    const auto &nub = packet.at("message").at("nub");
    if (!nub.contains("player")) {
      continue;
    }
    const auto &player = nub.at("player");
    const auto class_name = player.at("char_class").get<std::string>();
    int class_id{};
    if (class_name == "EX5CharacterClass_Fight") {
      class_id = 1;
    } else if (class_name == "EX5CharacterClass_Wild") {
      class_id = 2;
    } else if (class_name == "EX5CharacterClass_Adamant") {
      class_id = 3;
    } else if (class_name == "EX5CharacterClass_Magic") {
      class_id = 5;
    } else {
      continue;
    }
    if (checked_classes.contains(class_id) || !player.contains("stats")) {
      continue;
    }

    Character local;
    local.id = class_id;
    local.class_id = class_id;
    const auto expected = definitions->stats(local);
    const auto &observed = player.at("stats");
    bool base_attributes_match = true;
    for (const auto *key : {"1", "2", "3", "4", "5", "6"}) {
      base_attributes_match =
          base_attributes_match && observed.value(key, -1) == expected.at(key).get<int>();
    }
    if (!base_attributes_match) {
      continue;
    }

    // Current HP/SP belong to the captured action. Two-weapon nubs do not expose
    // active selection. Later MaxHP can also include permanent RewardStat grants.
    bool ambiguous_weapon = false;
    for (const auto &nub_item : player.value("equipments", Json::array())) {
      const auto slot_name = nub_item.value("slot", std::string("EX5EquipSlotType_Weapon_1"));
      x5::cms::EX5EquipSlotType slot;
      x5::cms::EX5ItemGrade grade;
      ASSERT_TRUE(x5::cms::EX5EquipSlotType_Parse(slot_name, &slot));
      ASSERT_TRUE(x5::cms::EX5ItemGrade_Parse(
          nub_item.value("grade", std::string("EX5ItemGrade_Normal")), &grade));
      if (slot == x5::cms::EX5EquipSlotType_Weapon_2) {
        ambiguous_weapon = true;
      }
      auto item = equipped_item(nub_item.at("item_cms_id").get<int>(), static_cast<int>(slot));
      item.grade = static_cast<int>(grade);
      local.items.push_back(item);
    }
    if (ambiguous_weapon) {
      continue;
    }
    for (const auto *key : {"15", "17", "19", "22", "25"}) {
      EXPECT_EQ(observed.at(key), expected.at(key)) << "class=" << class_id << " stat=" << key;
    }
    if (player.value("level", 1) <= 2) {
      EXPECT_EQ(observed.at("18"), expected.at("18")) << "class=" << class_id;
    }
    const auto equipped = definitions->stats(local);
    for (int type = 29; type <= 67; ++type) {
      const auto key = std::to_string(type);
      EXPECT_EQ(observed.at(key), equipped.at(key)) << "class=" << class_id << " stat=" << key;
    }
    ++checked_classes[class_id];
  }
  EXPECT_FALSE(checked_classes.empty());
  RecordProperty("verified_class_count", static_cast<int>(checked_classes.size()));
}

} // namespace
} // namespace chronicles
