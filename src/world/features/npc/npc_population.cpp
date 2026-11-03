#include "world/features/npc/npc_population.hpp"

#include "world/features/npc/npc_patrol.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <tuple>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr int kSupportedFormatVersion = 1;
constexpr std::string_view kFactionPrefix = "EX5Faction_";
constexpr std::size_t kMaximumFactionLength = 64;
// Matches the tutorial profile default for NPCs that carry no explicit faction.
constexpr const char *kDefaultFaction = "EX5Faction_TownNpc";
// EX5StatType_MoveSpeedRate uses per-mille rates, as in CharacterStats and NpcAi.
constexpr int kMoveSpeedRateStat = 84;
constexpr int kNeutralMoveSpeedRate = 1000;

[[noreturn]] void reject(const std::string &field) {
  throw std::runtime_error("Invalid NPC placement field: " + field);
}

std::int64_t integer_value(const Json &value, std::int64_t minimum, std::int64_t maximum,
                           const std::string &field) {
  if (!value.is_number_integer()) {
    reject(field);
  }
  if (value.is_number_unsigned()) {
    const auto unsigned_value = value.get<std::uint64_t>();
    if (unsigned_value > static_cast<std::uint64_t>(maximum) ||
        static_cast<std::int64_t>(unsigned_value) < minimum) {
      reject(field);
    }
    return static_cast<std::int64_t>(unsigned_value);
  }
  const auto signed_value = value.get<std::int64_t>();
  if (signed_value < minimum || signed_value > maximum) {
    reject(field);
  }
  return signed_value;
}

const Json &required_member(const Json &object, const char *key, const std::string &prefix) {
  const auto found = object.find(key);
  if (found == object.end()) {
    reject(prefix + key);
  }
  return *found;
}

std::uint64_t guid_half(const Json &value, const std::string &field) {
  if (!value.is_string()) {
    reject(field);
  }
  const auto &text = value.get_ref<const std::string &>();
  std::uint64_t half{};
  const char *const end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, half);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
    reject(field);
  }
  return half;
}

int positive_int(const Json &value, const std::string &field) {
  return static_cast<int>(integer_value(value, 1, std::numeric_limits<int>::max(), field));
}

NpcInteraction parse_interaction(const Json &entry, const std::string &field) {
  if (!entry.is_object()) {
    reject(field);
  }
  NpcInteraction interaction;
  interaction.interaction_id = positive_int(required_member(entry, "interaction_id", field + "."),
                                            field + ".interaction_id");
  const auto &task_case = required_member(entry, "task_case", field + ".");
  if (!task_case.is_string() || task_case.get_ref<const std::string &>().empty()) {
    reject(field + ".task_case");
  }
  interaction.task_case = task_case.get<std::string>();
  if (const auto shop = entry.find("shop_id"); shop != entry.end()) {
    interaction.shop_id = positive_int(*shop, field + ".shop_id");
  }
  return interaction;
}

NpcPlacement parse_placement(const Json &entry, std::size_t index) {
  const std::string prefix = "placements[" + std::to_string(index) + "].";
  if (!entry.is_object()) {
    reject("placements[" + std::to_string(index) + "]");
  }

  NpcPlacement placement;
  placement.npc_cms_id =
      positive_int(required_member(entry, "npc_cms_id", prefix), prefix + "npc_cms_id");

  const auto &position = required_member(entry, "position_mm", prefix);
  if (!position.is_object()) {
    reject(prefix + "position_mm");
  }
  constexpr std::int64_t kMinimumCoordinate = std::numeric_limits<int>::min();
  constexpr std::int64_t kMaximumCoordinate = std::numeric_limits<int>::max();
  const auto coordinate = [&](const char *axis) {
    const std::string field = prefix + "position_mm." + axis;
    return static_cast<int>(integer_value(required_member(position, axis, prefix + "position_mm."),
                                          kMinimumCoordinate, kMaximumCoordinate, field));
  };
  placement.x_mm = coordinate("x");
  placement.y_mm = coordinate("y");
  placement.z_mm = coordinate("z");

  const auto &yaw = required_member(entry, "yaw_degrees", prefix);
  if (!yaw.is_number() || !std::isfinite(yaw.get<double>())) {
    reject(prefix + "yaw_degrees");
  }
  placement.yaw = yaw.get<float>();

  if (const auto faction = entry.find("faction"); faction != entry.end()) {
    // The file stores the faction without the wire prefix.
    if (!faction->is_string()) {
      reject(prefix + "faction");
    }
    const auto &text = faction->get_ref<const std::string &>();
    if (text.empty() || text.size() + kFactionPrefix.size() > kMaximumFactionLength ||
        std::string_view(text).substr(0, kFactionPrefix.size()) == kFactionPrefix) {
      reject(prefix + "faction");
    }
    placement.faction = std::string(kFactionPrefix) + text;
  } else {
    placement.faction = kDefaultFaction;
  }

  if (const auto behavior = entry.find("behavior"); behavior != entry.end()) {
    if (!behavior->is_string()) {
      reject(prefix + "behavior");
    }
    placement.behavior = behavior->get<std::string>();
  }

  if (const auto guid = entry.find("spawner_guid_wire"); guid != entry.end()) {
    if (!guid->is_array() || guid->size() != 2) {
      reject(prefix + "spawner_guid_wire");
    }
    placement.spawner_guid = std::pair{guid_half(guid->at(0), prefix + "spawner_guid_wire[0]"),
                                       guid_half(guid->at(1), prefix + "spawner_guid_wire[1]")};
  }

  if (const auto stance = entry.find("anim_stance_key"); stance != entry.end()) {
    placement.anim_stance_key = static_cast<int>(
        integer_value(*stance, 0, std::numeric_limits<int>::max(), prefix + "anim_stance_key"));
  }

  if (const auto group = entry.find("interaction_group_id"); group != entry.end()) {
    placement.interaction_group_id = static_cast<int>(
        integer_value(*group, 0, std::numeric_limits<int>::max(), prefix + "interaction_group_id"));
  }
  if (const auto interactions = entry.find("interactions"); interactions != entry.end()) {
    if (!interactions->is_array()) {
      reject(prefix + "interactions");
    }
    for (std::size_t item = 0; item < interactions->size(); ++item) {
      placement.interactions.push_back(parse_interaction(
          interactions->at(item), prefix + "interactions[" + std::to_string(item) + "]"));
    }
  }

  if (const auto active = entry.find("active"); active != entry.end()) {
    if (!active->is_boolean()) {
      reject(prefix + "active");
    }
    placement.active = active->get<bool>();
  }
  if (const auto patrol = entry.find("patrol"); patrol != entry.end()) {
    placement.patrol = std::make_shared<const NpcPatrolRoute>(NpcPatrolRoute::from_json(*patrol));
  }
  return placement;
}

// Fixed allocation order. A guid-less placement sorts after every guid placement.
auto allocation_key(const NpcPlacement &placement) {
  const auto guid = placement.spawner_guid.value_or(std::pair<std::uint64_t, std::uint64_t>{});
  return std::tuple{!placement.spawner_guid.has_value(),
                    guid.first,
                    guid.second,
                    placement.npc_cms_id,
                    placement.x_mm,
                    placement.y_mm,
                    placement.z_mm};
}

Json unit_added_message(const PopulatedNpc &unit, std::int64_t elapsed_ticks) {
  const auto &placement = unit.placement;
  Json npc{{"npc_cms_id", placement.npc_cms_id},
           {"base_unit_id", std::to_string(unit.unit_id)},
           {"state", "EX5UnitStateType_Idle"},
           {"faction_type", placement.faction}};
  if (placement.spawner_guid) {
    npc["spawner_id_x"] = std::to_string(placement.spawner_guid->first);
    npc["spawner_id_y"] = std::to_string(placement.spawner_guid->second);
  }
  if (placement.anim_stance_key) {
    npc["anim_stance_key"] = *placement.anim_stance_key;
  }
  auto position = UnitPositionMm{placement.x_mm, placement.y_mm, placement.z_mm};
  auto yaw = placement.yaw;
  if (placement.patrol) {
    const auto pose = placement.patrol->sample(elapsed_ticks);
    position = pose.position;
    yaw = pose.yaw;
    npc["move_mode"] = "MoveMode_Normal";
    npc["move_detail"] = pose.moving ? pose.move_detail : "MoveDetail_Stop";
    npc["stats"] = {{std::to_string(kMoveSpeedRateStat), kNeutralMoveSpeedRate}};
    if (pose.stance) {
      npc["anim_stance_key"] = *pose.stance;
    }
  }
  return {{"unit_id", std::to_string(unit.unit_id)},
          {"by_sight", true},
          {"nub",
           {{"pos",
             {{"millimeter_x", position.x_mm},
              {"millimeter_y", position.y_mm},
              {"millimeter_z", position.z_mm}}},
            {"yaw", yaw},
            {"npc", std::move(npc)}}}};
}

} // namespace


NpcPlacementSet NpcPlacementSet::from_json(const Json &document, std::size_t max_placements) {
  if (!document.is_object()) {
    reject("document");
  }
  integer_value(required_member(document, "format_version", ""), kSupportedFormatVersion,
                kSupportedFormatVersion, "format_version");

  NpcPlacementSet result;
  result.map_id = static_cast<int>(integer_value(required_member(document, "map_id", ""), 1,
                                                 std::numeric_limits<int>::max(), "map_id"));
  if (const auto missing = document.find("layers_missing"); missing != document.end()) {
    if (!missing->is_array()) {
      reject("layers_missing");
    }
    result.missing_layers = missing->size();
  }
  const auto &placements = required_member(document, "placements", "");
  if (!placements.is_array() || placements.size() > max_placements) {
    reject("placements");
  }

  std::set<std::pair<std::uint64_t, std::uint64_t>> guids;
  result.placements.reserve(placements.size());
  for (std::size_t index = 0; index < placements.size(); ++index) {
    auto placement = parse_placement(placements[index], index);
    if (placement.spawner_guid && !guids.insert(*placement.spawner_guid).second) {
      reject("placements[" + std::to_string(index) + "].spawner_guid (duplicate)");
    }
    result.placements.push_back(std::move(placement));
  }
  return result;
}

NpcPopulation::NpcPopulation(NpcPlacementSet placement_set, std::size_t max_units)
    : map_id_(placement_set.map_id) {
  constexpr std::uint64_t kIdCount = (kLastNpcUnitId - kFirstTypedNpcUnitId) / kNpcUnitIdStride + 1;
  if (max_units == 0) {
    throw std::invalid_argument("NPC population needs a non-zero unit bound");
  }
  if (map_id_ <= 0) {
    throw std::invalid_argument("NPC population map id must be positive");
  }
  const auto inactive = std::ranges::count_if(
      placement_set.placements, [](const auto &placement) { return !placement.active; });
  skipped_inactive_ = static_cast<std::size_t>(inactive);
  std::erase_if(placement_set.placements, [](const auto &placement) { return !placement.active; });
  if (placement_set.placements.size() > max_units || placement_set.placements.size() > kIdCount) {
    throw std::invalid_argument("NPC population exceeds its unit bound");
  }

  std::ranges::stable_sort(placement_set.placements, [](const auto &first, const auto &second) {
    return allocation_key(first) < allocation_key(second);
  });
  units_.reserve(placement_set.placements.size());
  std::uint64_t next_unit_id = kFirstTypedNpcUnitId;
  for (auto &placement : placement_set.placements) {
    units_.push_back({next_unit_id, std::move(placement)});
    next_unit_id += kNpcUnitIdStride;
  }
}

int NpcPopulation::map_id() const {
  return map_id_;
}

const std::vector<PopulatedNpc> &NpcPopulation::units() const {
  return units_;
}

std::size_t NpcPopulation::skipped_inactive() const {
  return skipped_inactive_;
}

std::vector<GameplayPacket> NpcPopulation::unit_added_packets(std::int64_t elapsed_ticks) const {
  std::vector<GameplayPacket> packets;
  packets.reserve(units_.size());
  for (const auto &unit : units_) {
    packets.push_back(unit_added_packet(unit, elapsed_ticks));
  }
  return packets;
}

GameplayPacket NpcPopulation::unit_added_packet(const PopulatedNpc &unit,
                                                std::int64_t elapsed_ticks) {
  return {kUnitAddedOpcode, unit_added_message(unit, elapsed_ticks)};
}

UnitPositionMm NpcPopulation::unit_position(const PopulatedNpc &unit,
                                            std::int64_t elapsed_ticks) {
  if (unit.placement.patrol) {
    return unit.placement.patrol->sample(elapsed_ticks).position;
  }
  return {unit.placement.x_mm, unit.placement.y_mm, unit.placement.z_mm};
}

PopulationAnnounceResult NpcPopulation::announce_to(UnitRegistry &registry,
                                                    std::string_view instance_key,
                                                    std::int64_t elapsed_ticks) const {
  PopulationAnnounceResult result;
  for (const auto &unit : units_) {
    AnnouncedUnit announced;
    announced.unit_id = unit.unit_id;
    announced.npc_template_id = unit.placement.npc_cms_id;
    announced.map_id = map_id_;
    announced.instance_key = std::string(instance_key);
    announced.position = unit_position(unit, elapsed_ticks);
    if (registry.find(unit.unit_id) != nullptr) {
      // Refuse before touching the registry so the existing owner of the id is left unchanged.
      ++result.collisions;
      ++result.rejected;
      continue;
    }
    if (registry.announce(std::move(announced)) == UnitAnnounceResult::Added) {
      ++result.added;
    } else {
      ++result.rejected;
    }
  }
  return result;
}

bool NpcPopulation::has_patrols() const {
  return std::ranges::any_of(
      units_, [](const auto &unit) { return static_cast<bool>(unit.placement.patrol); });
}

std::size_t NpcPopulation::despawn_from(UnitRegistry &registry) const {
  std::size_t removed = 0;
  for (const auto &unit : units_) {
    if (registry.despawn(unit.unit_id)) {
      ++removed;
    }
  }
  return removed;
}

std::shared_ptr<const NpcPopulation> load_npc_population_file(const std::filesystem::path &path,
                                                              std::size_t max_units) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot read NPC placement file: " + path.string());
  }
  Json document;
  try {
    document = Json::parse(input);
  } catch (const Json::parse_error &error) {
    throw std::runtime_error("NPC placement file is not valid JSON: " + std::string(error.what()));
  }
  return std::make_shared<const NpcPopulation>(NpcPlacementSet::from_json(document, max_units),
                                               max_units);
}

} // namespace chronicles
