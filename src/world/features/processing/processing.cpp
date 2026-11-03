#include "world/features/processing/processing.hpp"

#include "cms.generated.pb.h"
#include "ec.generated.pb.h"
#include "world/core/inventory_slots.hpp"
#include "world/core/progress_packets.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using x5::ec::EErrorCode;

constexpr int kFormatVersion = 1;
constexpr char kBuildId[] = "40367e24241c9481bfc07fbf9ef1919f25fb0eaa";

// Native world ticks are 100 ns units (see NativeTicks in native_world.cpp).
constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr std::int64_t kTicksPerSecond = 1000 * kTicksPerMillisecond;
// Polling/network allowance for the client's execute request, as for gathering.
constexpr int kExecuteToleranceMs = 100;
// An unfinished craft can be replaced after this long, as for gathering.
constexpr std::int64_t kActionTimeoutTicks = 30 * kTicksPerSecond;
constexpr double kRequestPositionToleranceMm = 5000;
constexpr unsigned kMaximumRequestsPerSecond = 32;
// Storage accepts at most 64 new item rows per transaction; equipment crafts stay below.
constexpr int kMaximumGrantedInstances = 32;

// Spot doodad unit IDs are (sector_id << 32) | (local_id << 12) | 6.
constexpr std::uint64_t kSpotKindMask = 0xFFF;
constexpr std::uint64_t kSpotDoodadKind = 6;

constexpr std::uint16_t kCommonError = 2;
constexpr std::uint16_t kDialogueStartResponse = 161;
constexpr std::uint16_t kDialogueEndNotification = 162;
constexpr std::uint16_t kStartInteractionAnimResponse = 165;
constexpr std::uint16_t kStartInteractionAnimNotification = 166;
constexpr std::uint16_t kInteractionAnimResult = 167;
constexpr std::uint16_t kItemsUpdateNotification = 169;

constexpr char kSheathedWeaponState[] = "EX5WeaponStateType_Sheathed";
constexpr char kItemGradePrefix[] = "EX5ItemGrade_";

struct ProcessingError {
  EErrorCode code;
};

void require(bool condition, EErrorCode code) {
  if (!condition) {
    throw ProcessingError{code};
  }
}

int checked_int(const Json &value, const char *field) {
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string("Processing profile field is not an integer: ") + field);
  }
  const auto number = value.get<std::int64_t>();
  if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Processing profile field is out of range: ") + field);
  }
  return static_cast<int>(number);
}

int checked_positive_int(const Json &value, const char *field) {
  const int number = checked_int(value, field);
  if (number <= 0) {
    throw std::runtime_error(std::string("Processing profile field must be positive: ") + field);
  }
  return number;
}

std::uint64_t parse_profile_unit_id(const Json &value) {
  if (!value.is_string()) {
    throw std::runtime_error("Processing placement unit ID must be a decimal string");
  }
  const auto &text = value.get_ref<const std::string &>();
  std::size_t parsed_characters{};
  const auto unit_id = std::stoull(text, &parsed_characters);
  if (parsed_characters != text.size() || (unit_id & kSpotKindMask) != kSpotDoodadKind) {
    throw std::runtime_error("Processing placement unit ID is not a spot doodad: " + text);
  }
  return unit_id;
}

int parse_grade(const Json &value) {
  x5::cms::EX5ItemGrade grade{};
  const auto name = std::string(kItemGradePrefix) + value.get<std::string>();
  if (!x5::cms::EX5ItemGrade_Parse(name, &grade)) {
    throw std::runtime_error("Processing profile has an unknown item grade: " + name);
  }
  return static_cast<int>(grade);
}

ProcessingRecipe parse_recipe(const Json &entry) {
  ProcessingRecipe recipe;
  recipe.id = checked_positive_int(entry.at("item_craft_id"), "item_craft_id");
  recipe.requires_learning = entry.value("requires_learning", false);
  recipe.product_item_cms_id =
      checked_positive_int(entry.at("product_item_cms_id"), "product_item_cms_id");
  recipe.product_count = checked_positive_int(entry.at("product_count"), "product_count");
  recipe.gold_cost = checked_int(entry.at("gold_cost"), "gold_cost");
  if (recipe.gold_cost < 0) {
    throw std::runtime_error("Negative craft cost " + std::to_string(recipe.id));
  }

  const auto storage = entry.at("product_storage").get<std::string>();
  if (storage == "stackable") {
    recipe.product_storage = ProcessingProductStorage::stackable;
  } else if (storage == "instance") {
    recipe.product_storage = ProcessingProductStorage::instance;
    recipe.product_max_durability =
        checked_positive_int(entry.at("product_max_durability"), "product_max_durability");
    recipe.product_grade = parse_grade(entry.at("product_grade"));
  } else if (storage == "housing") {
    recipe.product_storage = ProcessingProductStorage::housing;
  } else {
    throw std::runtime_error("Unknown craft product storage " + storage);
  }

  std::set<int> material_ids;
  for (const auto &material_entry : entry.at("materials")) {
    ProcessingMaterial material;
    material.item_cms_id = checked_positive_int(material_entry.at("item_cms_id"), "item_cms_id");
    material.count = checked_positive_int(material_entry.at("count"), "count");
    if (!material_ids.insert(material.item_cms_id).second ||
        material.item_cms_id == recipe.product_item_cms_id) {
      throw std::runtime_error("Craft repeats a material or consumes its product " +
                               std::to_string(recipe.id));
    }
    recipe.materials.push_back(material);
  }
  if (recipe.materials.empty()) {
    throw std::runtime_error("Craft has no materials " + std::to_string(recipe.id));
  }
  return recipe;
}

ProcessingMapArea parse_map(const Json &entry) {
  ProcessingMapArea area;
  area.map_id = checked_positive_int(entry.at("map_id"), "map_id");
  for (std::size_t axis = 0; axis < area.min_mm.size(); ++axis) {
    area.min_mm[axis] = checked_int(entry.at("area_min_mm").at(axis), "area_min_mm");
    area.max_mm[axis] = checked_int(entry.at("area_max_mm").at(axis), "area_max_mm");
    if (area.min_mm[axis] > area.max_mm[axis]) {
      throw std::runtime_error("Inverted processing map area " + std::to_string(area.map_id));
    }
  }
  return area;
}

ProcessingPlacement parse_placement(const Json &entry) {
  constexpr std::size_t kPlacementFieldCount = 5;
  if (!entry.is_array() || entry.size() != kPlacementFieldCount) {
    throw std::runtime_error("Processing placement must have five fields");
  }

  ProcessingPlacement placement;
  placement.unit_id = parse_profile_unit_id(entry.at(0));
  placement.doodad_cms_id = checked_positive_int(entry.at(1), "doodad_cms_id");
  placement.x_mm = checked_int(entry.at(2), "x_mm");
  placement.y_mm = checked_int(entry.at(3), "y_mm");
  placement.z_mm = checked_int(entry.at(4), "z_mm");
  return placement;
}

double squared_distance_mm(const WorldPosition &position, double x_mm, double y_mm, double z_mm) {
  const double dx = position.x_mm - x_mm;
  const double dy = position.y_mm - y_mm;
  const double dz = position.z_mm - z_mm;
  return dx * dx + dy * dy + dz * dz;
}

bool within_station_range(const WorldPosition &position, const ProcessingPlacement &placement) {
  const double range_mm = placement.range_mm;
  return squared_distance_mm(position, placement.x_mm, placement.y_mm, placement.z_mm) <=
         range_mm * range_mm;
}

bool request_position_matches(const Json &request, const WorldPosition &position) {
  const auto pos = request.find("pos");
  if (pos == request.end() || !pos->is_object()) {
    return false;
  }

  // Proto3 JSON omits zero coordinates.
  const double x_mm = pos->value("millimeter_x", 0);
  const double y_mm = pos->value("millimeter_y", 0);
  const double z_mm = pos->value("millimeter_z", 0);
  const double tolerance = kRequestPositionToleranceMm * kRequestPositionToleranceMm;
  return squared_distance_mm(position, x_mm, y_mm, z_mm) <= tolerance;
}

std::optional<std::uint64_t> parse_target_unit_id(const Json &value) {
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(signed_value);
  }
  if (!value.is_string()) {
    return std::nullopt;
  }

  const auto &text = value.get_ref<const std::string &>();
  if (text.empty() || !std::ranges::all_of(text, [](char digit) {
        return digit >= '0' && digit <= '9';
      })) {
    return std::nullopt;
  }
  try {
    return std::stoull(text);
  } catch (const std::out_of_range &) {
    return std::nullopt;
  }
}

std::optional<std::uint64_t> request_target(const Json &request) {
  const auto target = request.find("target_unit_id");
  if (target == request.end()) {
    return std::nullopt;
  }
  return parse_target_unit_id(*target);
}

// The client turns toward the station before sending its request yaw.
float request_yaw(const Json &request, const WorldPosition &position) {
  const auto yaw = request.find("yaw");
  if (yaw != request.end() && yaw->is_number() && std::isfinite(yaw->get<double>())) {
    return yaw->get<float>();
  }
  return position.yaw;
}

Json position_json(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

Json sequence_json(std::uint32_t sequence) {
  return {{"seq", std::to_string(sequence)}};
}

GameplayPacket animation_result(std::uint64_t player_unit_id) {
  return {kInteractionAnimResult, {{"unit_id", std::to_string(player_unit_id)}}};
}

const Json *execute_craft_param(const Json &request) {
  const auto param = request.find("param");
  if (param == request.end() || !param->is_object()) {
    return nullptr;
  }
  const auto interaction = param->find("interaction");
  if (interaction == param->end() || !interaction->is_object()) {
    return nullptr;
  }
  const auto craft = interaction->find("execute_craft");
  if (craft == interaction->end() || !craft->is_object()) {
    return nullptr;
  }
  return &*craft;
}

std::optional<int> request_animation_id(const Json &message) {
  const auto animation = message.find("interaction_anim_id");
  if (animation == message.end() || !animation->is_number_integer()) {
    return std::nullopt;
  }
  return animation->get<int>();
}

std::optional<std::int64_t> checked_multiply(std::int64_t left, std::int64_t right) {
  if (left < 0 || right < 0) {
    return std::nullopt;
  }
  if (left != 0 && right > std::numeric_limits<std::int64_t>::max() / left) {
    return std::nullopt;
  }
  return left * right;
}

int checked_quantity(std::int64_t per_craft, int count) {
  const auto total = checked_multiply(per_craft, count);
  require(total && *total <= std::numeric_limits<int>::max(), x5::ec::ItemCountError);
  return static_cast<int>(*total);
}

std::vector<int> free_inventory_slots(const Character &character) {
  std::set<int> occupied;
  for (const auto &item : character.items) {
    occupied.insert(item.inventory_slot);
  }

  std::vector<int> free_slots;
  for (int slot = 0; slot <= kMaximumInventorySlot; ++slot) {
    if (!occupied.contains(slot)) {
      free_slots.push_back(slot);
    }
  }
  return free_slots;
}

// One validated craft ready for an atomic storage commit.
struct CraftPlan {
  WorldProgress next;
  std::vector<CharacterItem> granted_items;
  // Product first, then materials, matching the captured ItemsUpdateNoti order.
  std::vector<int> changed_stackables;
  std::vector<int> changed_housing_items;
  std::int64_t gold_change{};
  int produced_quantity{};
};

void plan_material_use(const ProcessingRecipe &recipe, int count, CraftPlan &plan) {
  for (const auto &material : recipe.materials) {
    const int needed = checked_quantity(material.count, count);
    const auto owned = plan.next.stackables.find(material.item_cms_id);
    require(owned != plan.next.stackables.end(), x5::ec::Craft_MaterialNotEnough);

    // Utility-bag slots reserve part of a stack; the bag must stay covered by the stack.
    const int reserved = reserved_utility_count(plan.next, material.item_cms_id);
    require(owned->second - reserved >= needed, x5::ec::Craft_MaterialNotEnough);

    // A depleted stack keeps its row at zero; storage never deletes progress rows.
    owned->second -= needed;
    plan.changed_stackables.push_back(material.item_cms_id);
  }
}

void plan_product(const ProcessingRecipe &recipe, const Character &character, CraftPlan &plan) {
  if (recipe.product_storage == ProcessingProductStorage::stackable) {
    auto &stackables = plan.next.stackables;
    require(stackables.contains(recipe.product_item_cms_id) ||
                stackables.size() < kMaximumStackableKinds,
            x5::ec::ItemSlotLimit);
    auto &owned = stackables[recipe.product_item_cms_id];
    require(owned <= std::numeric_limits<int>::max() - plan.produced_quantity,
            x5::ec::ItemCountError);
    owned += plan.produced_quantity;
    plan.changed_stackables.insert(plan.changed_stackables.begin(), recipe.product_item_cms_id);
    return;
  }

  if (recipe.product_storage == ProcessingProductStorage::housing) {
    auto &housing_items = plan.next.housing_items;
    require(housing_items.contains(recipe.product_item_cms_id) ||
                housing_items.size() < kMaximumHousingItemKinds,
            x5::ec::ItemSlotLimit);
    auto &owned = housing_items[recipe.product_item_cms_id];
    require(owned.quantity <= std::numeric_limits<int>::max() - plan.produced_quantity,
            x5::ec::ItemCountError);
    owned.quantity += plan.produced_quantity;
    plan.changed_housing_items.push_back(recipe.product_item_cms_id);
    return;
  }

  require(plan.produced_quantity <= kMaximumGrantedInstances, x5::ec::Craft_ProductingCountExceed);
  const auto free_slots = free_inventory_slots(character);
  require(free_slots.size() >= static_cast<std::size_t>(plan.produced_quantity),
          x5::ec::ItemSlotLimit);
  for (int index = 0; index < plan.produced_quantity; ++index) {
    CharacterItem granted;
    granted.cms_id = recipe.product_item_cms_id;
    granted.inventory_slot = free_slots[static_cast<std::size_t>(index)];
    granted.durability = recipe.product_max_durability;
    granted.grade = recipe.product_grade;
    plan.granted_items.push_back(granted);
  }
}

// Plans count crafts of one recipe against owned progress. No state changes.
CraftPlan plan_craft(const ProcessingRecipe &recipe, int count, const Character &character,
                     const WorldProgress &before) {
  require(!recipe.requires_learning || before.learned_crafts.contains(recipe.id),
          x5::ec::Craft_SkillNotLearn);
  require(before.revision < std::numeric_limits<std::int64_t>::max(),
          x5::ec::Craft_FailCraftAction);

  CraftPlan plan;
  plan.next = before;
  ++plan.next.revision;

  const auto cost = checked_multiply(recipe.gold_cost, count);
  require(cost.has_value(), x5::ec::Craft_FailCraftAction);
  require(plan.next.gold >= *cost, x5::ec::MoneyNotEnough);
  plan.next.gold -= *cost;
  plan.gold_change = -*cost;

  plan_material_use(recipe, count, plan);
  plan.produced_quantity = checked_quantity(recipe.product_count, count);
  plan_product(recipe, character, plan);
  return plan;
}

std::vector<GameplayPacket> craft_result_packets(const ProcessingRecipe &recipe,
                                                 const CraftPlan &plan,
                                                 const std::vector<CharacterItem> &committed,
                                                 std::uint64_t player_unit_id) {
  // ItemStackable.quantity is the resulting total, as in the captured craft updates.
  Json stackables = Json::array();
  for (const int item_cms_id : plan.changed_stackables) {
    stackables.push_back(
        {{"item_cms_id", item_cms_id}, {"quantity", plan.next.stackables.at(item_cms_id)}});
  }

  // Official craft notifications carried an empty PacketSeq.
  Json update{{"craft_action", Json::object()},
              {"packet_seq", Json::object()},
              {"stackables", std::move(stackables)}};
  if (plan.gold_change != 0) {
    update["money_changes"] = Json::array({{{"change", std::to_string(plan.gold_change)},
                                            {"result", std::to_string(plan.next.gold)}}});
  }
  if (!committed.empty()) {
    Json items = Json::array();
    for (const auto &item : committed) {
      items.push_back({{"item_id", std::to_string(item.id)},
                       {"item_cms_id", item.cms_id},
                       {"durability", item.durability},
                       {"grade", item.grade}});
    }
    update["items"] = std::move(items);
  }

  if (!plan.changed_housing_items.empty()) {
    Json housing_items = Json::array();
    for (const int item_id : plan.changed_housing_items) {
      const auto &owned = plan.next.housing_items.at(item_id);
      housing_items.push_back(housing_item_count_json(item_id, owned));
    }
    update["housing_items"] = std::move(housing_items);
  }

  Json produced{{"item_cms_id", recipe.product_item_cms_id},
                {"quantity", plan.produced_quantity}};
  if (recipe.product_storage == ProcessingProductStorage::instance) {
    produced["grade"] = recipe.product_grade;
  }

  return {{kItemsUpdateNotification, std::move(update)},
          {kProcessingCraftResultNotification,
           {{"item_craft_id", recipe.id}, {"items", Json::array({std::move(produced)})}}},
          animation_result(player_unit_id)};
}

GameplayPacket rejection_packet(std::uint16_t opcode, EErrorCode code, std::uint32_t sequence,
                                const Json &request) {
  const auto name = x5::ec::EErrorCode_Name(code);
  if (opcode == kProcessingDialogueStartRequest) {
    Json response{{"ec", name}};
    if (const auto target = request_target(request)) {
      response["target_unit_id"] = std::to_string(*target);
    }
    return {kDialogueStartResponse, std::move(response)};
  }
  if (opcode == kProcessingStartInteractionRequest) {
    // The one captured rejected start used StartInteractionAnimRes with an error code.
    return {kStartInteractionAnimResponse,
            {{"ec", name},
             {"weapon_state_type", kSheathedWeaponState},
             {"packet_seq", sequence_json(sequence)}}};
  }
  return {kCommonError, {{"ec", name}, {"packet_seq", sequence_json(sequence)}}};
}

} // namespace

nlohmann::json expertise_skills_entry_snapshot() {
  // Every official snapshot listed all sixteen enum values in enum order, each at Lv1,
  // including TradeItem, which has no ExpertiseSkill catalog row.
  const auto &starting_level = x5::cms::EX5ExpertiseLvType_Name(x5::cms::EX5ExpertiseLvType_Lv1);

  auto skills = Json::array();
  for (int type = x5::cms::EX5ExpertiseType_MIN; type <= x5::cms::EX5ExpertiseType_MAX; ++type) {
    if (!x5::cms::EX5ExpertiseType_IsValid(type)) {
      continue;
    }

    const auto expertise_type = static_cast<x5::cms::EX5ExpertiseType>(type);
    skills.push_back(
        {{"type", x5::cms::EX5ExpertiseType_Name(expertise_type)}, {"level", starting_level}});
  }

  return {{"skills", std::move(skills)}};
}

std::shared_ptr<const ProcessingData> ProcessingData::load(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open processing profile " + path.string());
  }

  try {
    return std::make_shared<const ProcessingData>(Json::parse(input));
  } catch (const Json::exception &error) {
    throw std::runtime_error("Invalid processing profile " + path.string() + ": " + error.what());
  }
}

ProcessingData::ProcessingData(const Json &document) {
  if (document.at("format_version") != kFormatVersion || document.at("build_id") != kBuildId) {
    throw std::runtime_error("Processing profile belongs to an unsupported format or client build");
  }
  maximum_craft_amount_ =
      checked_positive_int(document.at("maximum_craft_amount"), "maximum_craft_amount");

  for (const auto &entry : document.at("maps")) {
    maps_.push_back(parse_map(entry));
  }

  for (const auto &entry : document.at("animations")) {
    ProcessingAnimation animation;
    animation.id = checked_positive_int(entry.at("interaction_anim_id"), "interaction_anim_id");
    animation.execute_time_ms =
        checked_positive_int(entry.at("execute_time_ms"), "execute_time_ms");
    animation.duration_ms = checked_positive_int(entry.at("duration_ms"), "duration_ms");
    if (animation.execute_time_ms >= animation.duration_ms ||
        !animations_.emplace(animation.id, animation).second) {
      throw std::runtime_error("Invalid or duplicate craft animation " +
                               std::to_string(animation.id));
    }
  }

  for (const auto &entry : document.at("interactions")) {
    ProcessingInteraction interaction;
    interaction.id = checked_positive_int(entry.at("interaction_id"), "interaction_id");
    interaction.group_id = checked_positive_int(entry.at("group_id"), "group_id");
    interaction.animation_id =
        checked_positive_int(entry.at("interaction_anim_id"), "interaction_anim_id");
    if (!animations_.contains(interaction.animation_id) ||
        !interactions_.emplace(interaction.id, interaction).second) {
      throw std::runtime_error("Invalid or duplicate craft interaction " +
                               std::to_string(interaction.id));
    }
  }

  for (const auto &entry : document.at("crafts")) {
    auto recipe = parse_recipe(entry);
    const auto id = recipe.id;
    if (!recipes_.emplace(id, std::move(recipe)).second) {
      throw std::runtime_error("Duplicate craft " + std::to_string(id));
    }
  }

  for (const auto &entry : document.at("unsupported_crafts")) {
    const auto id = checked_positive_int(entry.at("item_craft_id"), "item_craft_id");
    if (recipes_.contains(id) || !unsupported_recipes_.insert(id).second) {
      throw std::runtime_error("Conflicting unsupported craft " + std::to_string(id));
    }
  }

  for (const auto &entry : document.at("stations")) {
    ProcessingStation station;
    station.doodad_cms_id = checked_positive_int(entry.at("doodad_cms_id"), "doodad_cms_id");
    station.interaction_group_id =
        checked_positive_int(entry.at("interaction_group_id"), "interaction_group_id");
    for (const auto &recipe_id : entry.at("craft_ids")) {
      const int id = checked_positive_int(recipe_id, "craft_ids");
      if (!recipes_.contains(id) && !unsupported_recipes_.contains(id)) {
        throw std::runtime_error("Station references unknown craft " + std::to_string(id));
      }
      station.recipe_ids.insert(id);
    }
    const bool has_interaction = std::ranges::any_of(interactions_, [&](const auto &entry_pair) {
      return entry_pair.second.group_id == station.interaction_group_id;
    });
    const auto doodad_id = station.doodad_cms_id;
    if (!has_interaction || !stations_.emplace(doodad_id, std::move(station)).second) {
      throw std::runtime_error("Invalid or duplicate craft station " + std::to_string(doodad_id));
    }
  }

  for (const auto &entry : document.at("placements")) {
    const auto placement = parse_placement(entry);
    if (!stations_.contains(placement.doodad_cms_id) ||
        !placements_.emplace(placement.unit_id, placement).second) {
      throw std::runtime_error("Invalid or duplicate craft station placement " +
                               std::to_string(placement.unit_id));
    }
  }

  if (maps_.empty() || placements_.empty() || recipes_.empty()) {
    throw std::runtime_error("Processing profile has no usable maps, stations or recipes");
  }
}

const ProcessingPlacement *ProcessingData::placement(std::uint64_t unit_id) const {
  const auto found = placements_.find(unit_id);
  return found == placements_.end() ? nullptr : &found->second;
}

bool ProcessingData::map_contains(int map_id, const ProcessingPlacement &placement) const {
  const std::array<int, 3> coordinates{placement.x_mm, placement.y_mm, placement.z_mm};
  return std::ranges::any_of(maps_, [&](const auto &area) {
    if (area.map_id != map_id) {
      return false;
    }
    for (std::size_t axis = 0; axis < coordinates.size(); ++axis) {
      if (coordinates[axis] < area.min_mm[axis] || coordinates[axis] > area.max_mm[axis]) {
        return false;
      }
    }
    return true;
  });
}

const ProcessingStation *ProcessingData::station(int doodad_cms_id) const {
  const auto found = stations_.find(doodad_cms_id);
  return found == stations_.end() ? nullptr : &found->second;
}

const ProcessingInteraction *ProcessingData::interaction(int interaction_id) const {
  const auto found = interactions_.find(interaction_id);
  return found == interactions_.end() ? nullptr : &found->second;
}

const ProcessingAnimation *ProcessingData::animation(int animation_id) const {
  const auto found = animations_.find(animation_id);
  return found == animations_.end() ? nullptr : &found->second;
}

const ProcessingRecipe *ProcessingData::recipe(int recipe_id) const {
  const auto found = recipes_.find(recipe_id);
  return found == recipes_.end() ? nullptr : &found->second;
}

bool ProcessingData::is_unsupported_recipe(int recipe_id) const {
  return unsupported_recipes_.contains(recipe_id);
}

int ProcessingData::maximum_craft_amount() const {
  return maximum_craft_amount_;
}

std::size_t ProcessingData::placement_count() const {
  return placements_.size();
}

std::size_t ProcessingData::recipe_count() const {
  return recipes_.size();
}

ProcessingSession::ProcessingSession(std::shared_ptr<const ProcessingData> data,
                                     std::uint64_t player_unit_id,
                                     ProcessingPersistence persistence,
                                     ProcessingStationLocator placed_stations)
    : data_(std::move(data)), player_unit_id_(player_unit_id),
      persistence_(std::move(persistence)), placed_stations_(std::move(placed_stations)) {
  if (!data_ || player_unit_id_ == 0 || !persistence_.load_character ||
      !persistence_.load_progress || !persistence_.commit) {
    throw std::invalid_argument("Processing session requires data, unit and storage");
  }
}

bool ProcessingSession::handles(std::uint16_t opcode, const Json &request,
                                const std::optional<WorldPosition> &position) const {
  if (!request.is_object()) {
    return false;
  }

  switch (opcode) {
  case kProcessingDialogueStartRequest: {
    const auto target = request_target(request);
    return target && position && station_on_map(*target, position->map_id).has_value();
  }
  case kProcessingDialogueEndRequest:
    return dialogue_station_.has_value();
  case kProcessingStartInteractionRequest:
    return execute_craft_param(request) != nullptr;
  case kProcessingExecuteInteractionRequest:
    return pending_ && request_animation_id(request) == pending_->animation_id;
  case kProcessingEndInteractionRequest: {
    const auto ended = request.find("end_interaction_anim");
    return pending_ && ended != request.end() && ended->is_object() &&
           request_animation_id(*ended) == pending_->animation_id;
  }
  default:
    return false;
  }
}

bool ProcessingSession::busy() const {
  return pending_.has_value() && !pending_->executed;
}

std::vector<GameplayPacket> ProcessingSession::receive(std::uint16_t opcode, const Json &request,
                                                       std::uint32_t sequence,
                                                       const WorldPosition &position,
                                                       std::int64_t server_ticks,
                                                       bool other_action_busy) {
  try {
    require(server_ticks > 0 &&
                server_ticks <= std::numeric_limits<std::int64_t>::max() - kActionTimeoutTicks,
            x5::ec::BadRequest);
    require(within_request_limit(server_ticks), x5::ec::BadRequest);

    switch (opcode) {
    case kProcessingDialogueStartRequest:
      return open_station(request, position);
    case kProcessingDialogueEndRequest:
      return close_station();
    case kProcessingStartInteractionRequest:
      return start(request, position, server_ticks, other_action_busy);
    case kProcessingExecuteInteractionRequest:
      return execute(request, position, server_ticks);
    case kProcessingEndInteractionRequest:
      return end(request);
    default:
      return {};
    }
  } catch (const ProcessingError &error) {
    return {rejection_packet(opcode, error.code, sequence, request)};
  } catch (const Json::exception &) {
    return {rejection_packet(opcode, x5::ec::BadRequest, sequence, request)};
  } catch (const std::invalid_argument &) {
    return {rejection_packet(opcode, x5::ec::BadRequest, sequence, request)};
  } catch (const std::out_of_range &) {
    return {rejection_packet(opcode, x5::ec::BadRequest, sequence, request)};
  }
}

std::vector<GameplayPacket> ProcessingSession::open_station(const Json &request,
                                                            const WorldPosition &position) {
  const auto target = request_target(request);
  const auto placement =
      target ? station_on_map(*target, position.map_id) : std::optional<ProcessingPlacement>{};
  require(placement.has_value(), x5::ec::Dialogue_InvalidTarget);
  require(request_position_matches(request, position), x5::ec::Dialogue_InvalidPos);
  require(within_station_range(position, *placement), x5::ec::TooFar);

  dialogue_station_ = placement->unit_id;

  // The official server attached the character at its validated request pose.
  return {{kDialogueStartResponse,
           {{"target_unit_id", std::to_string(placement->unit_id)},
            {"attach_pos", request.at("pos")},
            {"attach_yaw", request_yaw(request, position)}}}};
}

std::vector<GameplayPacket> ProcessingSession::close_station() {
  dialogue_station_.reset();
  if (pending_ && !pending_->executed) {
    // Leaving the station cancels an unexecuted craft; nothing was consumed.
    pending_.reset();
  }

  // Captured station dialogues ended with DialogueEndNoti naming the player.
  return {{kDialogueEndNotification, {{"player_unit_id", player_unit_id_}}}};
}

std::vector<GameplayPacket> ProcessingSession::start(const Json &request,
                                                     const WorldPosition &position,
                                                     std::int64_t server_ticks,
                                                     bool other_action_busy) {
  require(!other_action_busy && (!busy() || pending_expired(server_ticks)),
          x5::ec::InteractionAnim_UsingSkill);

  const auto target = request_target(request);
  const auto placement =
      target ? station_on_map(*target, position.map_id) : std::optional<ProcessingPlacement>{};
  require(placement.has_value(), x5::ec::Craft_NotFoundCraftingTable);
  // Every captured craft followed a DialogueStartReq on the same station.
  require(dialogue_station_ == placement->unit_id, x5::ec::Craft_InvalidCraftingTable);
  const auto &station = *data_->station(placement->doodad_cms_id);

  const auto &interaction_param = request.at("param").at("interaction");
  const auto *interaction = data_->interaction(interaction_param.at("interaction_id").get<int>());
  require(interaction != nullptr && interaction->group_id == station.interaction_group_id,
          x5::ec::Interaction_GroupIdNotMatch);
  require(request.at("interaction_anim_id").get<int>() == interaction->animation_id,
          x5::ec::InteractionAnim_Mismatch);

  const auto &craft = *execute_craft_param(request);
  const int recipe_id = craft.at("item_craft_id").get<int>();
  require(station.recipe_ids.contains(recipe_id), x5::ec::Craft_CraftPackNotMatch);
  require(!data_->is_unsupported_recipe(recipe_id), x5::ec::Craft_Restricted);
  const auto *recipe = data_->recipe(recipe_id);
  require(recipe != nullptr, x5::ec::Craft_InvalidCraftCmsId);

  // Proto3 omits a zero count; every captured craft sent count 1.
  const auto count_entry = craft.find("count");
  require(count_entry != craft.end() && count_entry->is_number_integer(),
          x5::ec::Craft_InvalidReqCountNumber);
  const auto requested_count = count_entry->get<std::int64_t>();
  require(requested_count >= 1 && requested_count <= data_->maximum_craft_amount(),
          x5::ec::Craft_InvalidReqCountNumber);
  const int count = static_cast<int>(requested_count);

  // Instance-material recipes are excluded from the profile, so no material item is valid.
  const auto material_items = craft.find("material_item_ids");
  require(material_items == craft.end() ||
              (material_items->is_array() && material_items->empty()),
          x5::ec::Craft_WrongAddMaterial);

  require(request_position_matches(request, position) &&
              within_station_range(position, *placement),
          x5::ec::TooFar);

  // Report missing materials, gold or space now; execute plans again from fresh state.
  const auto character = persistence_.load_character();
  require(character.has_value(), x5::ec::Craft_FailCraftAction);
  const auto preview = plan_craft(*recipe, count, *character, persistence_.load_progress());
  static_cast<void>(preview);

  pending_ = PendingCraft{placement->unit_id, recipe_id, count, interaction->animation_id,
                          server_ticks, false};

  const Json param{{"interaction",
                    {{"interaction_id", interaction->id},
                     {"execute_craft", {{"item_craft_id", recipe_id}, {"count", count}}}}}};
  return {{kStartInteractionAnimNotification,
           {{"unit_id", std::to_string(player_unit_id_)},
            {"interaction_anim_id", interaction->animation_id},
            {"param", param},
            {"pos", position_json(position)},
            {"yaw", request_yaw(request, position)},
            {"tick", std::to_string(server_ticks)},
            {"weapon_state_type", kSheathedWeaponState},
            {"packet_seq", Json::object()}}}};
}

std::vector<GameplayPacket> ProcessingSession::execute(const Json &request,
                                                       const WorldPosition &position,
                                                       std::int64_t server_ticks) {
  require(pending_ && request_animation_id(request) == pending_->animation_id,
          x5::ec::InteractionAnim_Mismatch);
  if (pending_->executed) {
    // A retried execute never crafts a second time.
    return {animation_result(player_unit_id_)};
  }

  const auto &animation = *data_->animation(pending_->animation_id);
  const auto earliest_ticks =
      pending_->started_ticks +
      static_cast<std::int64_t>(animation.execute_time_ms - kExecuteToleranceMs) *
          kTicksPerMillisecond;
  require(server_ticks >= earliest_ticks && !pending_expired(server_ticks),
          x5::ec::InteractionAnim_TimeNotMatch);

  const auto placement = station_on_map(pending_->station_unit_id, position.map_id);
  require(placement.has_value() && within_station_range(position, *placement), x5::ec::TooFar);

  const auto &recipe = *data_->recipe(pending_->recipe_id);
  const auto count = pending_->count;
  const auto character = persistence_.load_character();
  const auto before = persistence_.load_progress();
  CraftPlan plan;
  try {
    require(character.has_value(), x5::ec::Craft_FailCraftAction);
    plan = plan_craft(recipe, count, *character, before);
  } catch (const ProcessingError &) {
    // Materials or gold changed since the start; the craft is abandoned.
    pending_.reset();
    throw;
  }

  std::optional<std::vector<CharacterItem>> committed;
  try {
    committed = persistence_.commit(before, plan.next, plan.granted_items);
  } catch (const std::exception &error) {
    // Storage failures are reported as a failed craft; nothing was written.
    spdlog::error("Craft commit failed for recipe {}: {}", recipe.id, error.what());
  }
  if (!committed) {
    pending_.reset();
    throw ProcessingError{x5::ec::Craft_FailCraftAction};
  }

  pending_->executed = true;
  quest_events_.push_back({QuestEventKind::CraftSucceeded, recipe.id, 0, count});
  return craft_result_packets(recipe, plan, *committed, player_unit_id_);
}

QuestEvents ProcessingSession::take_quest_events() {
  auto events = std::move(quest_events_);
  quest_events_.clear();
  return events;
}

std::vector<GameplayPacket> ProcessingSession::end(const Json &request) {
  const auto &ended = request.at("end_interaction_anim");
  require(pending_ && request_animation_id(ended) == pending_->animation_id,
          x5::ec::InteractionAnim_Mismatch);

  // Ending before execution cancels the craft; nothing was consumed.
  pending_.reset();
  return {animation_result(player_unit_id_)};
}

std::optional<ProcessingPlacement> ProcessingSession::station_on_map(std::uint64_t unit_id,
                                                                    int map_id) const {
  const auto *placement = data_->placement(unit_id);
  if (placement != nullptr) {
    if (!data_->map_contains(map_id, *placement)) {
      return std::nullopt;
    }
    return *placement;
  }

  if (!placed_stations_) {
    return std::nullopt;
  }
  auto placed = placed_stations_(unit_id, map_id);
  // Only doodads the profile defines as stations offer crafting; the locator decides where
  // they stand and who may use them.
  if (!placed || placed->unit_id != unit_id || data_->station(placed->doodad_cms_id) == nullptr) {
    return std::nullopt;
  }
  return placed;
}

bool ProcessingSession::pending_expired(std::int64_t server_ticks) const {
  return pending_ && server_ticks > pending_->started_ticks + kActionTimeoutTicks;
}

bool ProcessingSession::within_request_limit(std::int64_t server_ticks) {
  if (server_ticks - request_window_ticks_ >= kTicksPerSecond) {
    request_window_ticks_ = server_ticks;
    requests_in_window_ = 0;
  }
  ++requests_in_window_;
  return requests_in_window_ <= kMaximumRequestsPerSecond;
}

} // namespace chronicles
