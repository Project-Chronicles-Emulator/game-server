#include "world/features/character/character_stats.hpp"

#include "cms.generated.pb.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr int kCoreAttributeCount = 6;
constexpr int kMaximumCharacterLevel = 40;
constexpr int kMaximumEquipmentLevel = 80;
constexpr int kNeutralRate = 1000;

struct InitialClass {
  int class_id;
  const char *class_name;
};

constexpr std::array<InitialClass, 4> kInitialClasses{{
    {1, "Fight"},
    {2, "Wild"},
    {3, "Adamant"},
    {5, "Magic"},
}};

struct EquipmentRate {
  int stat;
  const char *field;
  bool armor;
  bool fixed;
};

constexpr std::array<EquipmentRate, 22> kEquipmentRates{{
    {53, "physicalDamageRate", false, false}, {48, "magicDamageRate", false, false},
    {66, "windDamageRate", false, false},     {36, "fogDamageRate", false, false},
    {58, "quakeDamageRate", false, false},    {33, "fireDamageRate", false, false},
    {63, "tidalDamageRate", false, false},    {30, "electricDamageRate", false, false},
    {45, "ironDamageRate", false, false},     {39, "hitDamageRate", false, true},
    {42, "impactDamageRate", false, true},    {52, "physicalArmorRate", true, false},
    {47, "magicArmorRate", true, false},      {65, "windArmorRate", true, false},
    {35, "fogArmorRate", true, false},        {57, "quakeArmorRate", true, false},
    {32, "fireArmorRate", true, false},       {62, "tidalArmorRate", true, false},
    {29, "electricArmorRate", true, false},   {44, "ironArmorRate", true, false},
    {38, "hitArmorRate", true, true},         {41, "impactArmorRate", true, true},
}};

double number(const Json &row, const std::string &field, double maximum = 10000000) {
  const auto text = row.value(field, std::string{});
  if (text.empty()) {
    return 0;
  }
  double value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      !std::isfinite(value) || value < 0 || value > maximum) {
    throw std::runtime_error("Invalid character stat catalog field: " + field);
  }
  return value;
}

int integer(const Json &row, const std::string &field, int maximum = 10000000) {
  const auto value = number(row, field, maximum);
  if (value != std::floor(value)) {
    throw std::runtime_error("Nonintegral character stat catalog field: " + field);
  }
  return static_cast<int>(value);
}

// Polish scales every curve-based equipment term (attack, armor and elemental, on any slot) and
// every basic-stat bonus by (1 + polishIncreasePercent / 100); the fixed hit/impact terms are
// not scaled. Static: every AAC_Equipment_Calculate* helper except hit/impact, and
// AAC_Equipment_CalculateBasicStatBonus, multiply by float(polish + 1), with 0 polish for a
// step outside the item's increase table. Observed: equipped weapon polishes (steps 0 to 2)
// reproduce exactly on the unrounded physical/magic weapon term; armor, elemental and
// basic-stat polish frames were never captured.

// ItemPolishIncreaseValue rows grouped by increase id, then polish step.
using PolishIncreaseTable = std::map<std::string, std::map<int, double>>;

PolishIncreaseTable load_polish_increases(ClientCatalog &catalog) {
  PolishIncreaseTable table;
  for (const auto &row : catalog.rows("ItemPolishIncreaseValue")) {
    const auto id = row.value("id", std::string{});
    const auto step = static_cast<int>(number(row, "polishStep", 100));
    const auto percent = number(row, "polishIncreasePercent", 100000);
    if (id.empty() || step <= 0 || !table[id].emplace(step, percent).second) {
      throw std::runtime_error("Invalid or duplicate item polish increase row");
    }
  }
  return table;
}

std::map<int, double> resolve_polish_percents(ClientCatalog &catalog,
                                              std::optional<PolishIncreaseTable> &table,
                                              const Json &attributes) {
  const auto increase_id = attributes.value("FK_itemPolishIncreaseValueId", std::string{});
  if (increase_id.empty()) {
    return {};
  }
  if (!table) {
    table = load_polish_increases(catalog);
  }
  const auto found = table->find(increase_id);
  if (found == table->end()) {
    throw std::runtime_error("Missing item polish increase values: " + increase_id);
  }
  return found->second;
}

// Polish increase of an item at `polish_step` as a fraction (0.325 for 32.5%). Like the client,
// a step outside the item's increase table (including step 0) adds nothing.
double polish_fraction(const std::map<int, double> &percents, int polish_step) {
  const auto found = percents.find(polish_step);
  if (found == percents.end()) {
    return 0.0;
  }
  return found->second / 100.0;
}

int ceiling(double value) {
  // Decimal coefficient sums that are mathematically integral must not gain
  // one point from floating-point noise (e.g. .1*4 + .6*11).
  return static_cast<int>(std::ceil(value - 1e-9));
}

int nearest(double value) {
  if (!std::isfinite(value) || value < 0 || value > 10000000) {
    throw std::invalid_argument("Equipment stat contribution exceeds its supported range");
  }
  return static_cast<int>(std::floor(value + 0.5));
}

int core_power(int primary, int dexterity) {
  if (primary <= 70) {
    return primary * 6 + dexterity;
  }
  if (primary <= 120) {
    return primary * 3 + 210 + dexterity;
  }
  return ceiling(primary * 1.8 + 354 + dexterity);
}

int amplification(int power) {
  return ceiling(std::pow(std::log(power * 0.0307 + 1.5), 5) * 0.6248528243076684 + 0.1);
}

int elemental_power(int talent, double coefficient) {
  return ceiling(std::pow(std::log(talent * coefficient + 1), 4) * 0.27075605219327015);
}

int parse_attribute(std::string_view text) {
  int value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 0 ||
      value > 1000) {
    throw std::runtime_error("Invalid starting character attribute: " + std::string(text));
  }
  return value;
}

std::map<int, int> parse_attributes(std::string_view text) {
  std::map<int, int> attributes;
  while (!text.empty()) {
    const auto separator = text.find(',');
    const auto entry = text.substr(0, separator);
    const auto colon = entry.find(':');
    if (colon == std::string_view::npos) {
      throw std::runtime_error("Starting character attribute is missing its value");
    }

    const auto name = "EX5StatType_" + std::string(entry.substr(0, colon));
    x5::cms::EX5StatType type;
    if (!x5::cms::EX5StatType_Parse(name, &type) || type < x5::cms::EX5StatType_Con ||
        type > x5::cms::EX5StatType_Tal) {
      throw std::runtime_error("Unknown starting character attribute: " + name);
    }
    const auto value = parse_attribute(entry.substr(colon + 1));
    if (!attributes.emplace(static_cast<int>(type), value).second) {
      throw std::runtime_error("Duplicate starting character attribute: " + name);
    }

    if (separator == std::string_view::npos) {
      break;
    }
    text.remove_prefix(separator + 1);
    if (text.empty()) {
      throw std::runtime_error("Starting character attributes end with an empty entry");
    }
  }

  if (attributes.size() != kCoreAttributeCount) {
    throw std::runtime_error("Starting character definition must contain all six attributes");
  }
  return attributes;
}

void validate_character(const Character &character) {
  if (character.id <= 0 || character.level < 1 || character.level > kMaximumCharacterLevel ||
      character.experience < 0 || character.nui_blessing_level < 0 ||
      character.nui_blessing_level > kMaximumNuiBlessingLevel ||
      character.dedicated_nui_tears < 0 ||
      character.dedicated_nui_tears > kMaximumDedicatedNuiTears) {
    throw std::invalid_argument("Invalid tutorial character identity or progression");
  }
}

// PerkNode.json is a JSON object body without braces: "statType":"EX5StatType_...","statAdd":20.
std::pair<int, int> parse_perk_stat_effect(const Json &row) {
  const auto effect = Json::parse("{" + row.value("json", std::string{}) + "}");
  x5::cms::EX5StatType type;
  const auto name = effect.value("statType", std::string{});
  if (!x5::cms::EX5StatType_Parse(name, &type) || type == x5::cms::EX5StatType_None ||
      !effect.contains("statAdd") || !effect.at("statAdd").is_number_integer()) {
    throw std::runtime_error("Perk stat effect is malformed");
  }
  return {static_cast<int>(type), effect.at("statAdd").get<int>()};
}

} // namespace

std::shared_ptr<const CharacterStats> CharacterStats::load(ClientCatalog &catalog) {
  constexpr std::string_view kVerifiedSourceSha256 =
      "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6";
  if (catalog.source_executable_sha256() != kVerifiedSourceSha256) {
    throw std::runtime_error("Starting character stats belong to an unverified client build");
  }

  auto definitions = std::make_shared<CharacterStats>();
  // Minimal catalogs can omit blessing definitions, but a nonzero stored level then fails
  // closed during stat calculation. Missing rewards must never silently erase earned HP.
  definitions->nui_blessing_rewards_.emplace(0, std::pair{0, 0});
  if (catalog.has_table("NuiBlessingLevel")) {
    std::map<int, std::pair<int, int>> rewards;
    for (const auto &row : catalog.rows("NuiBlessingLevel")) {
      const auto level = integer(row, "level", kMaximumNuiBlessingLevel);
      const auto hp = integer(row, "maxHpReward");
      const auto sp = integer(row, "maxSpReward");
      if (level <= 0 || !rewards.emplace(level, std::pair{hp, sp}).second) {
        throw std::runtime_error("Invalid or duplicate Nui blessing level");
      }
    }
    int cumulative_hp{};
    int cumulative_sp{};
    for (int level = 1; level <= kMaximumNuiBlessingLevel; ++level) {
      const auto reward = rewards.find(level);
      if (reward == rewards.end()) {
        throw std::runtime_error("Nui blessing catalog is missing a reached-level reward");
      }
      cumulative_hp += reward->second.first;
      cumulative_sp += reward->second.second;
      definitions->nui_blessing_rewards_.emplace(
          level, std::pair{cumulative_hp, cumulative_sp});
    }
  }

  for (const auto &initial : kInitialClasses) {
    const auto row = catalog.find_row("CharacterBasicStat", "FK_character", initial.class_name);
    if (!row) {
      throw std::runtime_error("Missing starting attributes for " +
                               std::string(initial.class_name));
    }

    ClassDefinition definition;
    definition.attributes = parse_attributes(row->at("typeValue").get<std::string>());
    definitions->classes_.emplace(initial.class_id, std::move(definition));
  }

  for (const auto &row : catalog.rows("PerkNode")) {
    const int node_id = integer(row, "id", std::numeric_limits<int>::max());
    if (node_id <= 0) {
      throw std::runtime_error("Invalid perk CMS ID");
    }
    if (row.value("jsonCase", std::string{}) == "Stat") {
      definitions->perk_stats_.emplace(node_id, parse_perk_stat_effect(row));
    }

    if (row.value("autoRegistration", std::string{}) != "True") {
      continue;
    }

    const auto class_name = row.value("characterClass", std::string{});
    const auto found = std::ranges::find_if(kInitialClasses, [&](const InitialClass &entry) {
      return class_name == entry.class_name;
    });
    if (found == kInitialClasses.end()) {
      continue;
    }

    definitions->starting_perks_[found->class_id].push_back(node_id);
  }
  for (const auto &initial : kInitialClasses) {
    auto &nodes = definitions->starting_perks_[initial.class_id];
    std::ranges::sort(nodes);
    if (nodes.empty() || std::adjacent_find(nodes.begin(), nodes.end()) != nodes.end()) {
      throw std::runtime_error("Missing or duplicate auto-registered perk for " +
                               std::string(initial.class_name));
    }
  }

  for (const auto &row : catalog.rows("EquipmentGrade")) {
    x5::cms::EX5ItemGrade grade;
    if (!x5::cms::EX5ItemGrade_Parse("EX5ItemGrade_" + row.at("grade").get<std::string>(),
                                     &grade)) {
      throw std::runtime_error("Unknown equipment grade in character stat catalog");
    }
    GradeDefinition definition;
    for (const auto &rate : kEquipmentRates) {
      definition.rates[rate.stat] = static_cast<float>(number(row, rate.field) / 1000);
    }
    definition.basic_ratio = static_cast<float>(number(row, "basicStatRatio") / 1000);
    if (!definitions->grades_.emplace(static_cast<int>(grade), std::move(definition)).second) {
      throw std::runtime_error("Duplicate equipment grade in character stat catalog");
    }
  }

  // Loaded on demand: only items whose ItemAttribute row links an increase table need it.
  std::optional<PolishIncreaseTable> polish_table;
  for (const auto &row : catalog.rows("Item")) {
    const auto slot_name = row.value("equipSlot", std::string{});
    if (slot_name.empty()) {
      continue;
    }
    x5::cms::EX5EquipSlotType slot;
    if (!x5::cms::EX5EquipSlotType_Parse("EX5EquipSlotType_" + slot_name, &slot)) {
      throw std::runtime_error("Unknown equipment slot in character stat catalog");
    }
    EquipmentDefinition definition;
    definition.slot = static_cast<int>(slot);
    definition.level = integer(row, "level", kMaximumEquipmentLevel);
    definition.max_durability = integer(row, "maxDurability");
    const auto attribute_id = row.value("FK_itemAttribute", std::string{});
    if (!attribute_id.empty()) {
      const auto attributes = catalog.find_row("ItemAttribute", "id", attribute_id);
      if (!attributes) {
        throw std::runtime_error("Missing equipment stat attributes: " + attribute_id);
      }
      for (const auto &rate : kEquipmentRates) {
        definition.rates[rate.stat] = static_cast<float>(number(*attributes, rate.field) / 100);
      }
      for (int index = 1; index <= 4; ++index) {
        const auto suffix = std::to_string(index);
        const auto stat_name = attributes->value("basicStat" + suffix, std::string{});
        if (stat_name.empty()) {
          continue;
        }
        x5::cms::EX5StatType stat;
        if (!x5::cms::EX5StatType_Parse("EX5StatType_" + stat_name, &stat) || stat == 0) {
          throw std::runtime_error("Unknown equipment basic stat: " + stat_name);
        }
        definition.basic_ratios.emplace_back(
            static_cast<int>(stat),
            static_cast<float>(number(*attributes, "basicStatRatio" + suffix) / 100));
      }
      definition.polish_percents = resolve_polish_percents(catalog, polish_table, *attributes);
    }
    const auto cms_id = integer(row, "id", std::numeric_limits<int>::max());
    if (cms_id <= 0 || !definitions->equipment_.emplace(cms_id, std::move(definition)).second) {
      throw std::runtime_error("Invalid or duplicate equipment CMS ID");
    }
  }

  for (const auto &row : catalog.rows("Stat")) {
    x5::cms::EX5StatType type;
    if (!x5::cms::EX5StatType_Parse("EX5StatType_" + row.at("id").get<std::string>(), &type)) {
      throw std::runtime_error("Unknown stat limit in character stat catalog");
    }
    const auto minimum = integer(row, "min");
    const auto maximum = integer(row, "max");
    if (minimum > maximum ||
        !definitions->limits_.emplace(static_cast<int>(type), std::pair{minimum, maximum}).second) {
      throw std::runtime_error("Invalid or duplicate character stat limit");
    }
  }
  for (const auto &[name, target] : std::array<std::pair<const char *, float *>, 2>{{
           {"DurabilityDangerRatio", &definitions->durability_danger_ratio_},
           {"DurabilityPanaltyValue", &definitions->durability_penalty_},
       }}) {
    const auto row = catalog.find_row("GameVariable", "name", name);
    if (!row) {
      throw std::runtime_error("Missing equipment durability variable: " + std::string(name));
    }
    *target = static_cast<float>(number(*row, "value", 1));
  }
  return definitions;
}

std::map<int, int> CharacterStats::equipment_stats(const Character &character,
                                                   int active_weapon_slot) const {
  if (active_weapon_slot != 0 && active_weapon_slot != 1) {
    throw std::invalid_argument("Active weapon slot must be Weapon_1 or Weapon_2");
  }
  std::map<int, int> values;
  std::set<int> occupied_slots;
  if (character.items.size() > 1000) {
    throw std::invalid_argument("Character inventory exceeds the supported stat calculation bound");
  }
  for (const auto &item : character.items) {
    if (!item.equip_slot) {
      continue;
    }
    const auto slot = *item.equip_slot;
    const auto found = equipment_.find(item.cms_id);
    const auto grade = grades_.find(item.grade);
    if (found == equipment_.end() || grade == grades_.end() || slot < 0 || slot > 24 ||
        !occupied_slots.insert(slot).second || item.quantity != 1 || item.durability < 0 ||
        item.polish_step < 0) {
      throw std::invalid_argument("Invalid equipped item for character stat calculation");
    }
    const auto &definition = found->second;
    if ((slot > 1 && definition.slot != slot) || (slot <= 1 && definition.slot > 1)) {
      throw std::invalid_argument("Equipped item does not belong in its assigned slot");
    }
    if (slot <= 1 && slot != active_weapon_slot) {
      continue;
    }

    float durability = 1;
    if (definition.max_durability > 0) {
      if (item.durability > definition.max_durability) {
        throw std::invalid_argument("Equipped item durability exceeds its catalog maximum");
      }
      const auto ratio = static_cast<float>(item.durability) / definition.max_durability;
      if (ratio <= durability_danger_ratio_) {
        durability = std::max(1.0f - durability_penalty_, 0.0f);
      }
    }

    const auto level = definition.level;
    const auto attack_curve =
        std::log((level + 20.0) * (level + 20.0) / 300) * 52.26579203455843 + 1;
    const auto armor_curve =
        std::log((level + 26.0) * (level + 26.0) / 700) * 90.89702962531902 + 9;
    const auto polish = polish_fraction(definition.polish_percents, item.polish_step);
    for (const auto &rate : kEquipmentRates) {
      const auto attribute = definition.rates.find(rate.stat);
      if (attribute == definition.rates.end() || attribute->second == 0) {
        continue;
      }
      const auto grade_rate = grade->second.rates.at(rate.stat);
      const auto combined_rate = attribute->second * grade_rate;
      double value{};
      if (rate.fixed) {
        value = 100.0f * grade_rate * attribute->second;
        // Item detail retains one decimal for hit/impact armor. The capture
        // verifies integral rates only; do not guess its server wire conversion.
        if (rate.armor && std::abs(value - nearest(value)) > 1e-5) {
          throw std::invalid_argument("Fractional hit/impact armor needs verified wire rounding");
        }
      } else {
        // Applied to the unrounded term exactly as on the captured weapon polish frames.
        const auto curve = rate.armor ? armor_curve : attack_curve;
        value = durability * curve * combined_rate * (1.0 + polish);
      }
      values[rate.stat] += nearest(value);
    }

    // Client order: float(float(polish + 1) * gradeBasicRatio) * curve, then durability, ratio.
    const auto basic_polish = static_cast<float>(polish) + 1.0f;
    const auto basic_curve = std::log(static_cast<float>(level + 10) * 0.1f) * 33.830902f + 3.0f;
    for (const auto &[type, ratio] : definition.basic_ratios) {
      const auto bonus =
          durability * (basic_polish * grade->second.basic_ratio * basic_curve) * ratio;
      values[type] += nearest(bonus);
    }
  }
  return values;
}

Json CharacterStats::stats(const Character &character) const {
  return stats(character, character.build.active_weapon_slot);
}

Json CharacterStats::stats(const Character &character, int active_weapon_slot) const {
  validate_character(character);
  const auto found = classes_.find(character.class_id);
  if (found == classes_.end()) {
    throw std::invalid_argument("Character class has no verified tutorial starting attributes");
  }

  const auto &definition = found->second;
  auto attributes = definition.attributes;
  // Allocated LevelUpPoints raise the core attributes before every derived formula, as the
  // captured Str/Dex/Int allocations show (e.g. Str 15 -> 18 raises physical power by 18).
  for (const auto &[type, bonus] : character.build.stat_bonuses) {
    const auto attribute = attributes.find(type);
    if (attribute == attributes.end() || bonus < 0) {
      throw std::invalid_argument("Allocated attribute bonus is not a core attribute");
    }
    attribute->second += bonus;
  }
  auto equipment = equipment_stats(character, active_weapon_slot);
  for (auto &[type, value] : attributes) {
    value += equipment[type];
    equipment.erase(type);
    if (value < 0 || value > 1000) {
      throw std::invalid_argument("Equipment core attribute exceeds the supported stat range");
    }
  }
  Json values = Json::object();
  for (const auto &[type, value] : attributes) {
    values[std::to_string(type)] = value;
  }

  const auto set = [&](x5::cms::EX5StatType type, int value) {
    values[std::to_string(static_cast<int>(type))] = value;
  };
  const auto attribute = [&](x5::cms::EX5StatType type) {
    return attributes.at(static_cast<int>(type));
  };

  const auto strength = attribute(x5::cms::EX5StatType_Str);
  const auto intelligence = attribute(x5::cms::EX5StatType_Int);
  const auto dexterity = attribute(x5::cms::EX5StatType_Dex);
  const auto constitution = attribute(x5::cms::EX5StatType_Con);
  const auto endurance = attribute(x5::cms::EX5StatType_End);
  const auto talent = attribute(x5::cms::EX5StatType_Tal);
  const auto physical_power = core_power(strength, dexterity);
  const auto magic_power = core_power(intelligence, dexterity);
  const auto max_hp =
      200 + ceiling(std::pow(std::log(3.0 * constitution + 1), 4) * 0.44620614440569695);
  const auto max_sp =
      500 + ceiling(std::pow(std::log(3.0 * endurance + 1), 4) * 0.4278614158115875);
  set(x5::cms::EX5StatType_PhysicalPower, physical_power);
  set(x5::cms::EX5StatType_MagicPower, magic_power);
  const auto blessing = nui_blessing_rewards_.find(character.nui_blessing_level);
  if (blessing == nui_blessing_rewards_.end()) {
    throw std::invalid_argument("Character Nui blessing level has no verified catalog rewards");
  }
  set(x5::cms::EX5StatType_MaxHP, max_hp + blessing->second.first);
  set(x5::cms::EX5StatType_MaxSP, max_sp + blessing->second.second);
  for (int type = 29; type <= 67; ++type) {
    values[std::to_string(type)] = 0;
  }
  set(x5::cms::EX5StatType_PhysicalArmor, ceiling(2.1 * endurance + 0.35 * talent));
  set(x5::cms::EX5StatType_MagicArmor, ceiling(2.1 * endurance + 0.35 * talent));
  set(x5::cms::EX5StatType_PhysicalAttackPower, amplification(physical_power));
  set(x5::cms::EX5StatType_MagicAttackPower, amplification(magic_power));
  set(x5::cms::EX5StatType_PhysicalCritical, ceiling(1.35 * dexterity));
  set(x5::cms::EX5StatType_MagicCritical, ceiling(1.35 * dexterity));
  set(x5::cms::EX5StatType_PhysicalCriticalResist, nearest(0.7 * constitution));
  set(x5::cms::EX5StatType_MagicCriticalResist, ceiling(0.7 * constitution));
  set(x5::cms::EX5StatType_PhysicalIgnoreArmor, ceiling(0.1 * strength + 0.6 * dexterity));
  set(x5::cms::EX5StatType_MagicIgnoreArmor, ceiling(0.1 * intelligence + 0.6 * talent));
  const std::array<std::pair<x5::cms::EX5StatType, double>, 6> elemental_coefficients{{
      {x5::cms::EX5StatType_ElectricAttackPower, 0.09},
      {x5::cms::EX5StatType_FireAttackPower, 0.15},
      {x5::cms::EX5StatType_FogAttackPower, 0.14},
      {x5::cms::EX5StatType_IronAttackPower, 0.09},
      {x5::cms::EX5StatType_TidalAttackPower, 0.11},
      {x5::cms::EX5StatType_WindAttackPower, 0.2},
  }};
  for (const auto &[type, coefficient] : elemental_coefficients) {
    set(type, elemental_power(talent, coefficient));
  }
  for (const auto type :
       {x5::cms::EX5StatType_MagicCriticalPowerInc, x5::cms::EX5StatType_MagicCriticalRateInc,
        x5::cms::EX5StatType_PhysicalCriticalPowerInc, x5::cms::EX5StatType_PhysicalCriticalRateInc,
        x5::cms::EX5StatType_NamedNpcDamageInc, x5::cms::EX5StatType_NormalNpcDamageInc}) {
    set(type, 0);
  }
  set(x5::cms::EX5StatType_HPRegen, 1);
  set(x5::cms::EX5StatType_SPRegen, 120);
  set(x5::cms::EX5StatType_MaxSatiety, 100);
  set(x5::cms::EX5StatType_Satiety, 0);
  set(x5::cms::EX5StatType_SatietyRegen, 1);
  set(x5::cms::EX5StatType_IncomingHealRate, kNeutralRate);
  set(x5::cms::EX5StatType_MoveSpeedRate, kNeutralRate);
  set(x5::cms::EX5StatType_PotionHealRate, kNeutralRate);

  for (const auto &[type, contribution] : equipment) {
    const auto key = std::to_string(type);
    values[key] = values.value(key, 0) + contribution;
  }
  // Earned Stat perks add a flat amount per active level. Candidate: no captured frame
  // isolates a Stat perk, so the flat addition follows the PerkNode statAdd field only.
  for (const auto &[perk_id, perk_level] : character.build.perks) {
    const auto effect = perk_stats_.find(perk_id);
    if (effect == perk_stats_.end() || perk_level <= 0) {
      continue;
    }
    const auto key = std::to_string(effect->second.first);
    values[key] = values.value(key, 0) + effect->second.second * perk_level;
  }
  for (auto &[key, value] : values.items()) {
    const auto type = std::stoi(key);
    if (const auto limit = limits_.find(type); limit != limits_.end()) {
      value = std::clamp(value.get<int>(), limit->second.first, limit->second.second);
    }
  }
  set(x5::cms::EX5StatType_HP, values.at("18").get<int>());
  set(x5::cms::EX5StatType_SP, values.at("19").get<int>());
  return values;
}

Json CharacterStats::base_notification(const Character &character) const {
  validate_character(character);
  if (!classes_.contains(character.class_id)) {
    throw std::invalid_argument("Character class has no verified tutorial starting attributes");
  }
  return {{"char_id", std::to_string(character.id)},
          {"name", character.name},
          {"char_class", character.class_id},
          {"level", character.level},
          {"exp", std::to_string(character.experience)},
          {"world_level", 1},
          {"nui_blessing_level", character.nui_blessing_level},
          {"dedicated_nui_tears", character.dedicated_nui_tears}};
}

Json CharacterStats::starting_perks(int class_id) const {
  const auto found = starting_perks_.find(class_id);
  if (found == starting_perks_.end()) {
    throw std::invalid_argument("Character class has no auto-registered perks");
  }

  Json perks = Json::array();
  for (const int node_id : found->second) {
    perks.push_back({{"cms_id", node_id}, {"level", 1}});
  }
  // Earned nodes and unspent build points require durable progression support.
  return {{"perks", std::move(perks)},
          {"remaining_char_build_points", {{"1", 0}, {"2", 0}}}};
}

} // namespace chronicles
