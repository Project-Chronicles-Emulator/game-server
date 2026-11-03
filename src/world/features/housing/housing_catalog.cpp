#include "world/features/housing/housing_catalog.hpp"

#include "data/client_catalog.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace chronicles::housing {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kHousingAreaMapTypeName = "HousingArea";
constexpr std::string_view kHousingObjectCategory = "HousingObject";
constexpr std::string_view kHousingBuildingCategory = "HousingBuilding";
constexpr int kMaximumPresetObjects = 4096;
constexpr int kMaximumLotNumber = (1 << 20) - 1;
constexpr int kMaximumUpgradeMaterials = 5;
constexpr double kMillimetresPerCentimetre = 10.0;

const std::vector<std::string> kRequiredTables = {
    "Doodad",           "GameVariable",     "Housing",               "HousingModelPartAddOn",
    "HousingPreset",    "HousingPresetDoodad", "HousingRentCost",    "HousingSite",
    "HousingSiteDesign", "HousingUpgrade",  "Interaction",           "InteractionAnim",
    "Item",             "Map",              "WorldHousingLotNumber", "Zone"};

std::runtime_error row_error(const std::string &table, const char *field,
                             const std::string &problem) {
  return std::runtime_error(table + "." + field + " " + problem);
}

std::string text_field(const Json &row, const char *field) {
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

std::optional<std::int64_t> optional_integer(const std::string &table, const Json &row,
                                             const char *field) {
  const auto text = text_field(row, field);
  if (text.empty()) {
    return std::nullopt;
  }

  std::int64_t value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw row_error(table, field, "is not an integer");
  }
  return value;
}

int required_int(const std::string &table, const Json &row, const char *field, int minimum,
                 int maximum) {
  const auto value = optional_integer(table, row, field);
  if (!value) {
    throw row_error(table, field, "is required");
  }
  if (*value < minimum || *value > maximum) {
    throw row_error(table, field, "is out of range");
  }
  return static_cast<int>(*value);
}

std::optional<int> optional_positive_int(const std::string &table, const Json &row,
                                         const char *field) {
  const auto value = optional_integer(table, row, field);
  if (!value || *value == 0) {
    return std::nullopt;
  }
  if (*value < 0 || *value > std::numeric_limits<int>::max()) {
    throw row_error(table, field, "is out of range");
  }
  return static_cast<int>(*value);
}

double number_from_text(const std::string &table, const char *field, const std::string &text) {
  double value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      !std::isfinite(value)) {
    throw row_error(table, field, "is not a finite number");
  }
  return value;
}

double required_number(const std::string &table, const Json &row, const char *field) {
  const auto text = text_field(row, field);
  if (text.empty()) {
    throw row_error(table, field, "is required");
  }
  return number_from_text(table, field, text);
}

double optional_number(const std::string &table, const Json &row, const char *field) {
  const auto text = text_field(row, field);
  if (text.empty()) {
    return 0.0;
  }
  return number_from_text(table, field, text);
}

int centimetres_to_millimetres(const std::string &table, const char *field, double centimetres) {
  const double millimetres = std::round(centimetres * kMillimetresPerCentimetre);
  if (millimetres < std::numeric_limits<int>::min() ||
      millimetres > std::numeric_limits<int>::max()) {
    throw row_error(table, field, "is outside the millimetre range");
  }
  return static_cast<int>(millimetres);
}

// Catalog nested values are JSON fragments without their outer braces, for example
// `"x":577.5,"y":1132.5,"z":0`. Arrays and full objects are stored as complete JSON text.
Json parse_fragment(const std::string &table, const char *field, const std::string &text) {
  if (text.empty()) {
    return Json();
  }

  const bool complete = text.front() == '[' || text.front() == '{';
  try {
    return Json::parse(complete ? text : "{" + text + "}");
  } catch (const Json::exception &) {
    throw row_error(table, field, "is not valid JSON");
  }
}

double json_number(const std::string &table, const char *field, const Json &object,
                   const char *member) {
  const auto found = object.find(member);
  if (found == object.end()) {
    return 0.0;
  }
  if (!found->is_number() || !std::isfinite(found->get<double>())) {
    throw row_error(table, field, std::string("member ") + member + " is not a number");
  }
  return found->get<double>();
}

LocalPosition local_position_from_object(const std::string &table, const char *field,
                                         const Json &object) {
  if (!object.is_object()) {
    throw row_error(table, field, "is not a position object");
  }

  return LocalPosition{
      centimetres_to_millimetres(table, field, json_number(table, field, object, "x")),
      centimetres_to_millimetres(table, field, json_number(table, field, object, "y")),
      centimetres_to_millimetres(table, field, json_number(table, field, object, "z"))};
}

SiteType required_site_type(const std::string &table, const Json &row, const char *field,
                            bool allow_none) {
  const auto text = text_field(row, field);
  const auto parsed = parse_site_type(text);
  if (!parsed || (!allow_none && *parsed == SiteType::none)) {
    throw row_error(table, field, "names an unknown site type");
  }
  return *parsed;
}

AuthorityType required_authority(const std::string &table, const Json &row, const char *field) {
  const auto text = text_field(row, field);
  if (text == "Individual") {
    return AuthorityType::individual;
  }
  if (text == "Guild") {
    return AuthorityType::guild;
  }
  throw row_error(table, field, "names an unknown authority type");
}

MoneyKind optional_money(const std::string &table, const Json &row, const char *field) {
  const auto text = text_field(row, field);
  if (text.empty()) {
    return MoneyKind::none;
  }
  if (text == "Gold") {
    return MoneyKind::gold;
  }
  if (text == "HersanderToken") {
    return MoneyKind::hersander_token;
  }
  throw row_error(table, field, "names an unsupported money type");
}

HousingCategory required_category(const std::string &table, const Json &row, const char *field) {
  const auto text = text_field(row, field);
  if (text == "MainBuilding") {
    return HousingCategory::main_building;
  }
  if (text == "AttachedBuilding") {
    return HousingCategory::attached_building;
  }
  if (text == "FunctionFacility") {
    return HousingCategory::function_facility;
  }
  if (text == "HousingObject") {
    return HousingCategory::housing_object;
  }
  if (text == "Trophy") {
    return HousingCategory::trophy;
  }
  throw row_error(table, field, "names an unknown housing category");
}

std::uint32_t object_id(const std::string &table, const Json &row, const char *field,
                        bool allow_zero) {
  const int value = required_int(table, row, field, allow_zero ? 0 : 1,
                                 static_cast<int>(kMaximumObjectId));
  return static_cast<std::uint32_t>(value);
}

const std::vector<Json> &table_rows(const std::map<std::string, std::vector<Json>> &tables,
                                    const std::string &name) {
  const auto found = tables.find(name);
  if (found == tables.end()) {
    throw std::runtime_error("Housing catalog table " + name + " is absent");
  }
  return found->second;
}

// Indexes the pose interactions furniture offers (see FurnitureInteraction). Interaction groups
// with any other row kind are still indexed for their qualifying rows; the other rows (farm
// planting, conditional uses) stay unserved.
std::unordered_map<int, std::vector<FurnitureInteraction>>
load_furniture_interactions(const std::map<std::string, std::vector<Json>> &tables) {
  std::set<int> pose_animations;
  std::set<int> holding_animations;
  for (const auto &row : table_rows(tables, "InteractionAnim")) {
    const auto cost_type = text_field(row, "costType");
    const bool costs_nothing = cost_type.empty() || cost_type == "None";
    if (costs_nothing && text_field(row, "noExecuteNotify") == "True") {
      const int animation_id =
          required_int("InteractionAnim", row, "id", 1, std::numeric_limits<int>::max());
      pose_animations.insert(animation_id);
      if (text_field(row, "attachPolicy") == "HoldAfterEnd") {
        holding_animations.insert(animation_id);
      }
    }
  }

  std::unordered_map<int, std::vector<FurnitureInteraction>> by_group;
  for (const auto &row : table_rows(tables, "Interaction")) {
    if (text_field(row, "playerTaskCase") != "Use" || !text_field(row, "FK_conditionId").empty()) {
      continue;
    }
    const auto animation_id = optional_positive_int("Interaction", row, "FK_interactionAnimId");
    if (!animation_id || !pose_animations.contains(*animation_id)) {
      continue;
    }
    const auto group_id = optional_positive_int("Interaction", row, "groupId");
    if (!group_id) {
      continue;
    }
    by_group[*group_id].push_back(FurnitureInteraction{
        required_int("Interaction", row, "id", 1, std::numeric_limits<int>::max()),
        *animation_id, holding_animations.contains(*animation_id)});
  }

  std::unordered_map<int, std::vector<FurnitureInteraction>> by_doodad;
  for (const auto &row : table_rows(tables, "Doodad")) {
    const auto group_id = optional_positive_int("Doodad", row, "interactionGroupId");
    if (!group_id) {
      continue;
    }
    const auto group = by_group.find(*group_id);
    if (group == by_group.end()) {
      continue;
    }
    const int doodad_id = required_int("Doodad", row, "id", 1, std::numeric_limits<int>::max());
    by_doodad[doodad_id] = group->second;
  }
  return by_doodad;
}

int game_variable(const std::map<std::string, std::string> &variables, const std::string &name,
                  int fallback, int minimum, int maximum) {
  const auto found = variables.find(name);
  if (found == variables.end()) {
    return fallback;
  }

  int value{};
  const auto &text = found->second;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum ||
      value > maximum) {
    throw std::runtime_error("GameVariable " + name + " is outside its accepted range");
  }
  return value;
}

HousingRules load_rules(const std::vector<Json> &rows) {
  std::map<std::string, std::string> variables;
  for (const auto &row : rows) {
    variables[text_field(row, "name")] = text_field(row, "value");
  }

  HousingRules rules;
  rules.site_rent_duration_days =
      game_variable(variables, "HousingSiteRentDurationDays", rules.site_rent_duration_days, 1,
                    3650);
  rules.site_rent_imminent_days = game_variable(variables, "HousingSiteRentImminentDurationDays",
                                                rules.site_rent_imminent_days, 0, 3650);
  rules.area_rent_duration_days =
      game_variable(variables, "HousingAreaRentDurationDays", rules.area_rent_duration_days, 1,
                    3650);
  rules.area_rent_imminent_days = game_variable(variables, "HousingAreaRentImminentDurationDays",
                                                rules.area_rent_imminent_days, 0, 3650);
  rules.edit_save_cooldown = std::chrono::milliseconds(game_variable(
      variables, "HousingEditModeSaveCooldown",
      static_cast<int>(rules.edit_save_cooldown.count()), 0, 3'600'000));
  rules.guild_site_edit_cooldown = std::chrono::minutes(game_variable(
      variables, "HousingGuildHousingsiteEditCooldownMin",
      static_cast<int>(rules.guild_site_edit_cooldown.count()), 0, 525'600));
  rules.go_home_cooldown = std::chrono::seconds(
      game_variable(variables, "GoHomeCooltimeSec",
                    static_cast<int>(rules.go_home_cooldown.count()), 0, 86'400));

  const std::array<std::pair<SiteType, std::string>, 3> sizes = {
      std::pair{SiteType::small, std::string("S")}, std::pair{SiteType::medium, std::string("M")},
      std::pair{SiteType::large, std::string("L")}};
  for (const auto &[type, letter] : sizes) {
    rules.object_limit[type] =
        game_variable(variables, "HousingSite" + letter + "ObjectLimitCount",
                      rules.object_limit[type], 0, static_cast<int>(kMaximumObjectId));
    rules.farm_doodad_limit[type] =
        game_variable(variables, "HousingFarmDoodadSmallLimitInHousingSite" + letter,
                      rules.farm_doodad_limit[type], 0, static_cast<int>(kMaximumObjectId));
    rules.general_position_level[type] =
        game_variable(variables, "GeneralHousingSite" + letter + "positionLv",
                      rules.general_position_level[type], 0, 100);
    rules.guild_position_level[type] =
        game_variable(variables, "GuildHousingSite" + letter + "positionLv",
                      rules.guild_position_level[type], 0, 100);
  }
  rules.farm_doodad_cms_id = game_variable(variables, "HousingFarmDoodadSmall",
                                           rules.farm_doodad_cms_id, 1,
                                           std::numeric_limits<int>::max());
  return rules;
}

// One entry of HousingPreset.houses, for example
// {"houseId":1,"attachIndex":-1,"housingCmsId":10001,"relativePosition":{...},"relativeYaw":0}.
PresetHouse preset_house(const std::unordered_map<int, HousingDefinition> &housings,
                         const Json &house) {
  constexpr const char *kTable = "HousingPreset";
  constexpr const char *kField = "houses";
  if (!house.is_object()) {
    throw row_error(kTable, kField, "contains a non-object house");
  }

  const auto integer_member = [&](const char *member, int fallback) {
    const auto found = house.find(member);
    if (found == house.end()) {
      return fallback;
    }
    if (!found->is_number_integer()) {
      throw row_error(kTable, kField, std::string("member ") + member + " is not an integer");
    }
    return found->get<int>();
  };

  PresetHouse placed;
  const int house_id = integer_member("houseId", 0);
  placed.attach_index = integer_member("attachIndex", -1);
  placed.housing_id = integer_member("housingCmsId", 0);
  placed.position = local_position_from_object(
      kTable, kField, house.contains("relativePosition") ? house.at("relativePosition")
                                                         : Json::object());
  placed.yaw = static_cast<float>(json_number(kTable, kField, house, "relativeYaw"));

  if (house_id <= 0 || house_id > static_cast<int>(kMaximumObjectId) ||
      !housings.contains(placed.housing_id)) {
    throw row_error(kTable, kField, "names an invalid house");
  }
  placed.house_id = static_cast<std::uint32_t>(house_id);
  return placed;
}

} // namespace

std::uint64_t housing_unit_id(int lot_number) {
  if (lot_number <= 0 || lot_number > kMaximumLotNumber) {
    throw std::invalid_argument("Housing lot number is outside the unit ID layout");
  }
  return (static_cast<std::uint64_t>(lot_number) << 12) | kHousingUnitTypeNibble;
}

std::uint64_t housing_doodad_unit_id(int lot_number, std::uint32_t house_id,
                                     std::uint32_t doodad_id) {
  if (house_id > kMaximumObjectId || doodad_id == 0 || doodad_id > kMaximumObjectId) {
    throw std::invalid_argument("Housing doodad key is outside the unit ID layout");
  }

  const std::uint64_t object_key = (static_cast<std::uint64_t>(house_id) << 14) | doodad_id;
  return (object_key << 32) | housing_unit_id(lot_number);
}

std::optional<DoodadUnitKey> parse_housing_doodad_unit_id(std::uint64_t unit_id) {
  const auto low = static_cast<std::uint32_t>(unit_id & 0xFFFF'FFFFu);
  const auto high = static_cast<std::uint32_t>(unit_id >> 32);

  // Low half: type nibble 5, a zero flag nibble, then the 20-bit lot number.
  if ((low & 0xFFu) != kHousingUnitTypeNibble) {
    return std::nullopt;
  }

  const int lot_number = static_cast<int>(low >> 12);
  const std::uint32_t doodad_id = high & kMaximumObjectId;
  const std::uint32_t house_id = (high >> 14) & kMaximumObjectId;
  const std::uint32_t reserved = high >> 28;
  if (lot_number <= 0 || doodad_id == 0 || reserved != 0) {
    return std::nullopt;
  }
  return DoodadUnitKey{lot_number, house_id, doodad_id};
}

std::uint32_t sector_id_for_position(double x_centimetres, double y_centimetres) {
  if (!std::isfinite(x_centimetres) || !std::isfinite(y_centimetres)) {
    throw std::invalid_argument("Sector position must be finite");
  }

  const auto axis_index = [](double centimetres) {
    const double index = std::floor(centimetres / kSectorSizeCentimetres) + 1.0;
    const auto whole = static_cast<std::int64_t>(index);
    return static_cast<std::uint32_t>(whole) & kMaximumObjectId;
  };
  return axis_index(x_centimetres) | (axis_index(y_centimetres) << 14);
}

std::string site_type_name(SiteType type) {
  switch (type) {
  case SiteType::small:
    return "Small";
  case SiteType::medium:
    return "Medium";
  case SiteType::large:
    return "Large";
  case SiteType::none:
    break;
  }
  return "None";
}

std::optional<SiteType> parse_site_type(const std::string &text) {
  if (text == "Small") {
    return SiteType::small;
  }
  if (text == "Medium") {
    return SiteType::medium;
  }
  if (text == "Large") {
    return SiteType::large;
  }
  if (text == "None" || text.empty()) {
    return SiteType::none;
  }
  return std::nullopt;
}

const std::vector<std::string> &HousingCatalog::required_tables() {
  return kRequiredTables;
}

std::shared_ptr<const HousingCatalog> HousingCatalog::load(ClientCatalog &catalog) {
  std::map<std::string, std::vector<Json>> tables;
  for (const auto &name : kRequiredTables) {
    tables.emplace(name, catalog.rows(name));
  }
  return from_rows(tables);
}

std::shared_ptr<const HousingCatalog>
HousingCatalog::from_rows(const std::map<std::string, std::vector<Json>> &tables) {
  auto catalog = std::make_shared<HousingCatalog>();
  catalog->rules_ = load_rules(table_rows(tables, "GameVariable"));

  // Zone keys are referenced by name; only zones with lot ranges are housing zones.
  std::unordered_map<std::string, std::pair<int, std::string>> zone_ids;
  for (const auto &row : table_rows(tables, "Zone")) {
    const auto name = text_field(row, "zoneKey");
    if (name.empty()) {
      continue;
    }
    const int id = required_int("Zone", row, "id", 1, std::numeric_limits<int>::max());
    zone_ids[name] = {id, text_field(row, "name")};
  }

  for (const auto &row : table_rows(tables, "WorldHousingLotNumber")) {
    const auto name = text_field(row, "zoneKey");
    const auto zone = zone_ids.find(name);
    if (zone == zone_ids.end()) {
      throw row_error("WorldHousingLotNumber", "zoneKey", "names an unknown zone");
    }

    ZoneDefinition definition;
    definition.zone_key = zone->second.first;
    definition.zone_name = name;
    definition.display_name = zone->second.second;
    definition.min_lot_number = required_int("WorldHousingLotNumber", row,
                                             "minHousingLotNumber", 1, kMaximumLotNumber);
    definition.max_lot_number = required_int("WorldHousingLotNumber", row,
                                             "maxHousingLotNumber", 1, kMaximumLotNumber);
    if (definition.max_lot_number < definition.min_lot_number) {
      throw row_error("WorldHousingLotNumber", "maxHousingLotNumber", "is below the minimum");
    }
    if (!catalog->zones_.emplace(definition.zone_key, definition).second) {
      throw row_error("WorldHousingLotNumber", "zoneKey", "repeats a zone");
    }
  }

  for (const auto &row : table_rows(tables, "Map")) {
    if (text_field(row, "type") != kHousingAreaMapTypeName) {
      continue;
    }
    const auto zone = zone_ids.find(text_field(row, "FK_zoneKey"));
    if (zone == zone_ids.end()) {
      throw row_error("Map", "FK_zoneKey", "names an unknown housing zone");
    }
    const auto housing_zone = catalog->zones_.find(zone->second.first);
    if (housing_zone == catalog->zones_.end()) {
      throw row_error("Map", "FK_zoneKey", "names a zone without a lot range");
    }

    const int map_id = required_int("Map", row, "id", 1, std::numeric_limits<int>::max());
    housing_zone->second.map_id = map_id;
    catalog->zone_by_map_[map_id] = housing_zone->first;
  }

  for (const auto &row : table_rows(tables, "HousingSiteDesign")) {
    SiteDesign design;
    design.type = required_site_type("HousingSiteDesign", row, "siteType", false);
    design.size_x_cm = required_number("HousingSiteDesign", row, "sizeX");
    design.size_y_cm = required_number("HousingSiteDesign", row, "sizeY");
    design.max_height_cm = required_number("HousingSiteDesign", row, "maxHeight");
    if (design.size_x_cm <= 0 || design.size_y_cm <= 0 || design.max_height_cm <= 0) {
      throw row_error("HousingSiteDesign", "sizeX", "must describe a positive volume");
    }
    catalog->designs_[design.type] = design;
  }

  for (const auto &row : table_rows(tables, "HousingSite")) {
    SiteDefinition site;
    site.lot_number = required_int("HousingSite", row, "lotNumber", 1, kMaximumLotNumber);
    const auto zone = zone_ids.find(text_field(row, "zoneKey"));
    if (zone == zone_ids.end() || !catalog->zones_.contains(zone->second.first)) {
      throw row_error("HousingSite", "zoneKey", "names an unknown housing zone");
    }
    site.zone_key = zone->second.first;
    const auto &zone_definition = catalog->zones_.at(site.zone_key);
    if (site.lot_number < zone_definition.min_lot_number ||
        site.lot_number > zone_definition.max_lot_number) {
      throw row_error("HousingSite", "lotNumber", "is outside the zone lot range");
    }

    site.type = required_site_type("HousingSite", row, "siteType", false);
    site.x_cm = required_number("HousingSite", row, "x");
    site.y_cm = required_number("HousingSite", row, "y");
    site.z_cm = required_number("HousingSite", row, "z");
    site.size_x_cm = required_number("HousingSite", row, "sizeX");
    site.size_y_cm = required_number("HousingSite", row, "sizeY");
    site.yaw_degrees = optional_number("HousingSite", row, "yaw");
    site.sign_x_cm = optional_number("HousingSite", row, "signX");
    site.sign_y_cm = optional_number("HousingSite", row, "signY");
    site.preset_id = text_field(row, "fk_PresetId");
    site.sector_id = sector_id_for_position(site.x_cm, site.y_cm);
    if (!catalog->sites_.emplace(site.lot_number, site).second) {
      throw row_error("HousingSite", "lotNumber", "repeats a lot");
    }
  }

  for (const auto &row : table_rows(tables, "Housing")) {
    HousingDefinition housing;
    housing.id = required_int("Housing", row, "id", 1, std::numeric_limits<int>::max());
    housing.category = required_category("Housing", row, "category");
    housing.housing_unit_model_id = required_int("Housing", row, "housingUnitModelId", 0,
                                                 std::numeric_limits<int>::max());
    if (!catalog->housings_.emplace(housing.id, housing).second) {
      throw row_error("Housing", "id", "repeats a building");
    }
  }

  for (const auto &row : table_rows(tables, "Item")) {
    const auto category = text_field(row, "category");
    const bool building = category == kHousingBuildingCategory;
    if (!building && category != kHousingObjectCategory) {
      continue;
    }

    const int item_id = required_int("Item", row, "id", 1, std::numeric_limits<int>::max());
    catalog->storage_items_[item_id] = text_field(row, "discardable") == "True";
    if (building) {
      if (const auto housing_id = optional_positive_int("Item", row, "FK_housingId")) {
        const auto found = catalog->housings_.find(*housing_id);
        if (found == catalog->housings_.end()) {
          throw row_error("Item", "FK_housingId", "names an unknown building");
        }
        found->second.item_cms_id = item_id;
      }
    }

    // Function-facility buildings (the crafting stations) name both a Housing row and a
    // doodad. The stock client places them from housing storage as a doodad of that cms ID
    // (UpdateHousingReq.doodads), so the doodad must resolve to the building item as well.
    if (const auto doodad_id = optional_positive_int("Item", row, "FK_doodadId")) {
      catalog->item_by_doodad_.emplace(*doodad_id, item_id);
    }
  }

  for (const auto &row : table_rows(tables, "HousingRentCost")) {
    RentCost cost;
    cost.authority = required_authority("HousingRentCost", row, "authorityType");
    cost.site_type = required_site_type("HousingRentCost", row, "siteType", true);
    cost.unlock_money = optional_money("HousingRentCost", row, "unlockMoneyType");
    cost.unlock_value = required_int("HousingRentCost", row, "unlockValue", 0,
                                     std::numeric_limits<int>::max());
    cost.rent_item_cms_id = required_int("HousingRentCost", row, "rentItemId", 1,
                                         std::numeric_limits<int>::max());
    cost.rent_item_count = required_int("HousingRentCost", row, "rentItemCount", 0,
                                        std::numeric_limits<int>::max());
    if (cost.unlock_money == MoneyKind::none && cost.unlock_value != 0) {
      throw row_error("HousingRentCost", "unlockValue", "has no money type");
    }
    catalog->rent_costs_[{cost.authority, cost.site_type}] = cost;
  }

  for (const auto &row : table_rows(tables, "HousingUpgrade")) {
    UpgradeDefinition upgrade;
    upgrade.from_housing_id = required_int("HousingUpgrade", row, "FK_fromHousingId", 1,
                                           std::numeric_limits<int>::max());
    upgrade.to_housing_id = required_int("HousingUpgrade", row, "FK_toHousingId", 1,
                                         std::numeric_limits<int>::max());
    if (!catalog->housings_.contains(upgrade.from_housing_id) ||
        !catalog->housings_.contains(upgrade.to_housing_id)) {
      throw row_error("HousingUpgrade", "FK_toHousingId", "names an unknown building");
    }

    for (int index = 1; index <= kMaximumUpgradeMaterials; ++index) {
      const auto id_field = "materialId0" + std::to_string(index);
      const auto count_field = "materialCnt0" + std::to_string(index);
      const auto material = optional_positive_int("HousingUpgrade", row, id_field.c_str());
      if (!material) {
        continue;
      }
      const int count = required_int("HousingUpgrade", row, count_field.c_str(), 1,
                                     std::numeric_limits<int>::max());
      upgrade.materials.push_back({*material, count});
    }
    catalog->upgrades_[{upgrade.from_housing_id, upgrade.to_housing_id}] = upgrade;
  }

  for (const auto &row : table_rows(tables, "HousingPreset")) {
    Preset preset;
    preset.id = text_field(row, "id");
    if (preset.id.empty()) {
      throw row_error("HousingPreset", "id", "is required");
    }
    preset.site_type = required_site_type("HousingPreset", row, "siteType", false);

    const auto parts = parse_fragment("HousingPreset", "partCustomIds",
                                      text_field(row, "partCustomIds"));
    if (parts.is_array()) {
      for (const auto &part : parts) {
        if (!part.is_number_integer()) {
          throw row_error("HousingPreset", "partCustomIds", "contains a non-integer");
        }
        preset.part_custom_ids.push_back(part.get<int>());
      }
    }

    const auto houses = parse_fragment("HousingPreset", "houses", text_field(row, "houses"));
    if (houses.is_array()) {
      for (const auto &house : houses) {
        preset.houses.push_back(preset_house(catalog->housings_, house));
      }
    }
    catalog->presets_[preset.id] = std::move(preset);
  }

  for (const auto &row : table_rows(tables, "HousingPresetDoodad")) {
    const auto preset_id = text_field(row, "id");
    const auto preset = catalog->presets_.find(preset_id);
    if (preset == catalog->presets_.end()) {
      throw row_error("HousingPresetDoodad", "id", "names an unknown preset");
    }
    if (preset->second.doodads.size() >= kMaximumPresetObjects) {
      throw row_error("HousingPresetDoodad", "id", "exceeds the preset object limit");
    }

    PresetDoodad doodad;
    doodad.doodad_id = object_id("HousingPresetDoodad", row, "housingDoodadId", false);
    doodad.house_id = object_id("HousingPresetDoodad", row, "houseId", true);
    doodad.doodad_cms_id = required_int("HousingPresetDoodad", row, "FK_doodad", 1,
                                        std::numeric_limits<int>::max());
    doodad.phase = required_int("HousingPresetDoodad", row, "phase", 0,
                                std::numeric_limits<int>::max());
    doodad.position = local_position_from_object(
        "HousingPresetDoodad", "relativePosition",
        parse_fragment("HousingPresetDoodad", "relativePosition",
                       text_field(row, "relativePosition")));
    doodad.yaw = static_cast<float>(optional_number("HousingPresetDoodad", row, "relativeYaw"));
    preset->second.doodads.push_back(doodad);
  }

  for (const auto &row : table_rows(tables, "HousingModelPartAddOn")) {
    ModelAddOnDoodad add_on;
    const int model_id = required_int("HousingModelPartAddOn", row, "housingUnitModelId", 0,
                                      std::numeric_limits<int>::max());
    add_on.doodad_id = object_id("HousingModelPartAddOn", row, "housingDoodadId", false);
    add_on.doodad_cms_id = required_int("HousingModelPartAddOn", row, "doodadCmsId", 1,
                                        std::numeric_limits<int>::max());
    add_on.position = LocalPosition{
        centimetres_to_millimetres("HousingModelPartAddOn", "relativeX",
                                   optional_number("HousingModelPartAddOn", row, "relativeX")),
        centimetres_to_millimetres("HousingModelPartAddOn", "relativeY",
                                   optional_number("HousingModelPartAddOn", row, "relativeY")),
        centimetres_to_millimetres("HousingModelPartAddOn", "relativeZ",
                                   optional_number("HousingModelPartAddOn", row, "relativeZ"))};
    add_on.yaw = static_cast<float>(optional_number("HousingModelPartAddOn", row, "relativeYaw"));
    catalog->model_add_ons_[model_id].push_back(add_on);
  }

  catalog->furniture_interactions_ = load_furniture_interactions(tables);

  for (const auto &[lot_number, site] : catalog->sites_) {
    if (!site.preset_id.empty() && !catalog->presets_.contains(site.preset_id)) {
      throw std::runtime_error("HousingSite " + std::to_string(lot_number) +
                               " names an unknown preset");
    }
    if (!catalog->designs_.contains(site.type)) {
      throw std::runtime_error("HousingSite " + std::to_string(lot_number) +
                               " has no site design");
    }
  }

  return catalog;
}

const HousingRules &HousingCatalog::rules() const {
  return rules_;
}

const ZoneDefinition *HousingCatalog::zone(int zone_key) const {
  const auto found = zones_.find(zone_key);
  return found == zones_.end() ? nullptr : &found->second;
}

const ZoneDefinition *HousingCatalog::zone_for_map(int map_id) const {
  const auto found = zone_by_map_.find(map_id);
  return found == zone_by_map_.end() ? nullptr : zone(found->second);
}

std::vector<const ZoneDefinition *> HousingCatalog::rentable_zones() const {
  std::vector<const ZoneDefinition *> result;
  for (const auto &[zone_key, definition] : zones_) {
    if (definition.map_id) {
      result.push_back(&definition);
    }
  }
  return result;
}

const SiteDefinition *HousingCatalog::site(int lot_number) const {
  const auto found = sites_.find(lot_number);
  return found == sites_.end() ? nullptr : &found->second;
}

std::vector<const SiteDefinition *> HousingCatalog::sites_in_zone(int zone_key) const {
  std::vector<const SiteDefinition *> result;
  for (const auto &[lot_number, site] : sites_) {
    if (site.zone_key == zone_key) {
      result.push_back(&site);
    }
  }
  return result;
}

const SiteDesign *HousingCatalog::design(SiteType type) const {
  const auto found = designs_.find(type);
  return found == designs_.end() ? nullptr : &found->second;
}

const HousingDefinition *HousingCatalog::housing(int housing_id) const {
  const auto found = housings_.find(housing_id);
  return found == housings_.end() ? nullptr : &found->second;
}

const RentCost *HousingCatalog::rent_cost(AuthorityType authority, SiteType type) const {
  // The guild row is not split by site size (siteType None).
  const auto key = authority == AuthorityType::guild ? std::pair{authority, SiteType::none}
                                                      : std::pair{authority, type};
  const auto found = rent_costs_.find(key);
  return found == rent_costs_.end() ? nullptr : &found->second;
}

const UpgradeDefinition *HousingCatalog::upgrade(int from_housing_id, int to_housing_id) const {
  const auto found = upgrades_.find({from_housing_id, to_housing_id});
  return found == upgrades_.end() ? nullptr : &found->second;
}

const Preset *HousingCatalog::preset(const std::string &preset_id) const {
  const auto found = presets_.find(preset_id);
  return found == presets_.end() ? nullptr : &found->second;
}

const std::vector<ModelAddOnDoodad> *
HousingCatalog::model_add_ons(int housing_unit_model_id) const {
  const auto found = model_add_ons_.find(housing_unit_model_id);
  return found == model_add_ons_.end() ? nullptr : &found->second;
}

std::optional<int> HousingCatalog::item_for_doodad(int doodad_cms_id) const {
  const auto found = item_by_doodad_.find(doodad_cms_id);
  if (found == item_by_doodad_.end()) {
    return std::nullopt;
  }
  return found->second;
}

const std::vector<FurnitureInteraction> *
HousingCatalog::furniture_interactions(int doodad_cms_id) const {
  const auto found = furniture_interactions_.find(doodad_cms_id);
  return found == furniture_interactions_.end() ? nullptr : &found->second;
}

bool HousingCatalog::is_storage_item(int item_cms_id) const {
  return storage_items_.contains(item_cms_id);
}

bool HousingCatalog::is_discardable_storage_item(int item_cms_id) const {
  const auto found = storage_items_.find(item_cms_id);
  return found != storage_items_.end() && found->second;
}

} // namespace chronicles::housing
