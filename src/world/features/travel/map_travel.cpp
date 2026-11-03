#include "world/features/travel/map_travel.hpp"

#include "common/clock_ticks.hpp"
#include "world/core/map_channels.hpp"
#include "world/features/quests/quest_rules.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace chronicles::travel {
namespace {

using Json = nlohmann::json;

constexpr int kFormatVersion = 2;
constexpr std::uintmax_t kMaximumProfileBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaximumMaps = 10000;
constexpr std::size_t kMaximumVolumes = 100000;
// The reported request point must lie near the server's tracked position (same bound as the
// zone channel route), and the tracked position inside the volume with this capsule margin.
constexpr double kRequestRangeMm = 5000;
constexpr double kVolumeToleranceCm = 150;
constexpr std::uint64_t kCutSceneVisibleChannel = 6;
constexpr std::uint64_t kDefaultVisibleChannel = 3;
// Client Map table type of the housing overlay maps (map 20101).
constexpr std::string_view kHousingAreaMapType = "HousingArea";

std::array<double, 3> triple(const Json &value) {
  if (!value.is_array() || value.size() != 3) {
    throw std::runtime_error("Travel profile vector must have three numbers");
  }
  std::array<double, 3> result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = value.at(index).get<double>();
    if (!std::isfinite(result[index])) {
      throw std::runtime_error("Travel profile vector is not finite");
    }
  }
  return result;
}

std::string wire_text(const Json &value) {
  // fixed64 trigger halves arrive as decimal text; profiles also store them as text.
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<std::uint64_t>());
  }
  const auto text = value.get<std::string>();
  std::uint64_t parsed{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::runtime_error("Travel trigger wire id is invalid");
  }
  return std::to_string(parsed);
}

// The request point of a LevelTriggerReq or ChannelTriggerEntryReq, when present.
std::optional<WorldPosition> reported_position(const Json &request, int map_id) {
  const auto found = request.find("pos");
  if (found == request.end() || !found->is_object()) {
    return std::nullopt;
  }
  WorldPosition position{map_id, found->value("millimeter_x", 0), found->value("millimeter_y", 0),
                         found->value("millimeter_z", 0), 0.0};
  return position;
}

bool near(const WorldPosition &first, const WorldPosition &second, double range_mm) {
  const double dx = static_cast<double>(first.x_mm) - second.x_mm;
  const double dy = static_cast<double>(first.y_mm) - second.y_mm;
  const double dz = static_cast<double>(first.z_mm) - second.z_mm;
  return dx * dx + dy * dy + dz * dz <= range_mm * range_mm;
}

Json position_json(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

TravelPlan refused(std::string reason, bool client_waits) {
  TravelPlan plan;
  plan.handled = true;
  plan.client_waits_for_move = client_waits;
  plan.refusal = std::move(reason);
  return plan;
}

} // namespace

std::shared_ptr<const Catalog> Catalog::load(const std::filesystem::path &path) {
  if (std::filesystem::file_size(path) > kMaximumProfileBytes) {
    throw std::runtime_error("Travel profile exceeds the size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open the travel profile");
  }
  Json document;
  input >> document;
  return std::make_shared<const Catalog>(document);
}

Catalog::Catalog(const Json &document) {
  if (document.at("format_version") != kFormatVersion ||
      document.at("maps").size() > kMaximumMaps ||
      document.at("volumes").size() > kMaximumVolumes) {
    throw std::runtime_error("Unsupported travel profile");
  }

  for (const auto &row : document.at("maps")) {
    MapInfo map;
    map.map_id = row.at("map_id");
    map.type = row.at("type");
    map.world = row.at("world");
    map.parent_map_id = row.at("parent_map_id");
    map.origin_map_id = row.at("origin_map_id");
    if (row.contains("area_cm")) {
      map.area_min_cm = triple(row.at("area_cm").at("min"));
      map.area_max_cm = triple(row.at("area_cm").at("max"));
    }
    if (row.contains("translation_cm")) {
      map.translation_cm = triple(row.at("translation_cm"));
    }
    map.entrance_conditions = row.at("entrance_conditions");
    map.exit_conditions = row.at("exit_conditions");
    if (map.map_id <= 0 || !map.entrance_conditions.is_array() ||
        !map.exit_conditions.is_array()) {
      throw std::runtime_error("Invalid travel map row");
    }
    const int map_id = map.map_id;
    if (!maps_.emplace(map_id, std::move(map)).second) {
      throw std::runtime_error("Duplicate travel map row");
    }
  }

  for (const auto &row : document.at("volumes")) {
    if (!row.at("active").get<bool>()) {
      continue;
    }
    Volume volume;
    volume.wire = {wire_text(row.at("trigger_id_wire").at(0)),
                   wire_text(row.at("trigger_id_wire").at(1))};
    volume.world = row.at("world");
    volume.maps = row.at("maps").get<std::vector<int>>();
    volume.kind = row.at("kind");
    volume.center_cm = triple(row.at("center_cm"));
    if (row.at("shape") == "sphere") {
      volume.sphere = true;
      volume.radius_cm = row.at("radius_cm");
    } else {
      volume.extent_cm = triple(row.at("extent_cm"));
      const auto rotation = triple(row.at("rotation_deg"));
      volume.axes = unreal_rotation_axes(rotation[0], rotation[1], rotation[2]);
    }
    volume.destination_map_id = row.value("destination_map_id", 0);
    volume.portal_id = row.value("portal_id", 0);
    volume.rules = row.value("rules", Json::array());
    if (volume.kind != "channelSession" && volume.kind != "channel" && volume.kind != "portal" &&
        volume.kind != "rules") {
      throw std::runtime_error("Unknown travel volume kind");
    }
    if (!volumes_by_wire_.emplace(volume.wire, volumes_.size()).second) {
      throw std::runtime_error("Duplicate travel volume wire id");
    }
    volumes_.push_back(std::move(volume));
  }

  for (const auto &row : document.at("portals")) {
    Portal portal{row.at("portal_id"), row.at("from_map_id"), row.at("to_map_id"), std::nullopt};
    if (row.contains("arrival_mm")) {
      if (portal.to_map_id != portal.from_map_id) {
        throw std::runtime_error("Only same-map portals carry a recovered arrival");
      }
      portal.arrival_mm = row.at("arrival_mm").get<std::array<int, 3>>();
    }
    portals_.push_back(portal);
  }
  for (const auto &row : document.at("map_move_cut_scenes")) {
    map_move_cut_scenes_.push_back(
        {row.at("from_map_id"), row.at("to_map_id"), row.at("cut_scene_id"), row.at("conditions")});
  }
  for (const auto &row : document.at("cut_scenes")) {
    CutScene cut_scene{row.at("invisible").get<bool>(), row.at("client_only").get<bool>(),
                       std::nullopt};
    if (row.contains("end_point")) {
      const auto &end = row.at("end_point");
      CutSceneEndPoint end_point;
      end_point.map_id = end.at("map_id");
      end_point.end_point_id = end.at("end_point_id");
      if (!maps_.contains(end_point.map_id)) {
        throw std::runtime_error("Cutscene end point names an unknown map");
      }
      if (end.contains("position_mm")) {
        const auto position = end.at("position_mm").get<std::array<int, 3>>();
        end_point.position = WorldPosition{end_point.map_id, position[0], position[1],
                                           position[2], end.value("yaw", 0.0f)};
      }
      cut_scene.end_point = std::move(end_point);
    }
    cut_scenes_.emplace(row.at("cut_scene_id").get<int>(), std::move(cut_scene));
  }

  if (document.contains("cliffs_gate")) {
    const auto &row = document.at("cliffs_gate");
    const auto position = row.at("position_mm").get<std::array<int, 3>>();
    const auto destination = row.at("destination_mm").get<std::array<int, 3>>();
    CutSceneGate gate;
    gate.unit_id = wire_text(row.at("unit_id"));
    gate.interaction_id = row.at("interaction_id");
    gate.source_map_id = row.at("source_map_id");
    gate.position = {gate.source_map_id, position[0], position[1], position[2], 0.0};
    gate.required_quest_id = row.at("required_quest_id");
    gate.cut_scene_id = row.at("cut_scene_id");
    gate.destination = {row.at("destination_map_id").get<int>(), destination[0], destination[1],
                        destination[2], row.at("destination_yaw").get<float>()};
    if (gate.interaction_id <= 0 || gate.required_quest_id <= 0 ||
        !maps_.contains(gate.source_map_id) || !maps_.contains(gate.destination.map_id) ||
        !server_cut_scene(gate.cut_scene_id) || !std::isfinite(gate.destination.yaw)) {
      throw std::runtime_error("Invalid Cliffs entrance profile");
    }
    cliffs_gate_ = std::move(gate);
  }
}

const MapInfo *Catalog::map(int map_id) const {
  const auto found = maps_.find(map_id);
  return found == maps_.end() ? nullptr : &found->second;
}

std::size_t Catalog::map_count() const noexcept {
  return maps_.size();
}

std::size_t Catalog::volume_count() const noexcept {
  return volumes_.size();
}

bool Catalog::resumable(int map_id) const {
  const auto *info = map(map_id);
  if (!info || info->origin_map_id != 1) {
    return false;
  }
  return info->type == "Field" || info->type == "Instance" || info->type == "Dungeon" ||
         info->type == "PointOfInterest";
}

bool Catalog::conditions_pass(const Json &conditions, int from_map_id, int to_map_id,
                              const TravelContext &context) const {
  return quest_rules::grouped_conditions_pass(conditions, [&](const Json &row) {
    const auto kind = row.at("kind").get<std::string>();
    if (kind == "ClientChecked") {
      // CUSTOM: kinds the server does not track were checked by the client before it asked.
      return true;
    }
    if (kind == "PartyState") {
      const auto state = row.at("state").get<std::string>();
      if (state == "NoParty") {
        return context.party_role == PartyRole::none;
      }
      if (state == "LeaderOnly") {
        return context.party_role == PartyRole::leader;
      }
      return context.party_role == PartyRole::member;
    }
    if (kind == "MapLeave") {
      return row.at("map_id").get<int>() == from_map_id;
    }
    if (kind == "MapEnter") {
      return row.at("map_id").get<int>() == to_map_id;
    }
    return context.evaluate && context.evaluate(row);
  });
}

bool Catalog::travel_allowed(int from_map_id, int to_map_id, const TravelContext &context) const {
  const auto *from = map(from_map_id);
  const auto *to = map(to_map_id);
  if (!from || !to) {
    return false;
  }
  return conditions_pass(from->exit_conditions, from_map_id, to_map_id, context) &&
         conditions_pass(to->entrance_conditions, from_map_id, to_map_id, context);
}

std::optional<WorldPosition> Catalog::parent_arrival(const WorldPosition &current) const {
  const auto *info = map(current.map_id);
  if (!info || info->parent_map_id <= 0 || info->parent_map_id == current.map_id) {
    return std::nullopt;
  }
  return arrival(current, info->parent_map_id);
}

std::optional<int> Catalog::housing_area_parent(int map_id) const {
  const auto *info = map(map_id);
  if (!info || info->type != kHousingAreaMapType || info->parent_map_id <= 0 ||
      info->parent_map_id == map_id) {
    return std::nullopt;
  }
  return info->parent_map_id;
}

bool Catalog::Volume::contains(const WorldPosition &position, double tolerance_cm) const {
  const std::array<double, 3> displacement{position.x_mm / 10.0 - center_cm[0],
                                           position.y_mm / 10.0 - center_cm[1],
                                           position.z_mm / 10.0 - center_cm[2]};
  if (sphere) {
    const double distance_squared = displacement[0] * displacement[0] +
                                    displacement[1] * displacement[1] +
                                    displacement[2] * displacement[2];
    const double limit = radius_cm + tolerance_cm;
    return distance_squared <= limit * limit;
  }
  for (std::size_t axis = 0; axis < axes.size(); ++axis) {
    double local = 0;
    for (std::size_t component = 0; component < displacement.size(); ++component) {
      local += displacement[component] * axes[axis][component];
    }
    if (std::abs(local) > extent_cm[axis] + tolerance_cm) {
      return false;
    }
  }
  return true;
}

bool Catalog::volume_on_map(const Volume &volume, int map_id) const {
  const auto *info = map(map_id);
  if (!info || info->world != volume.world) {
    return false;
  }
  // Static layers list no map: they belong to every map of their world.
  return volume.maps.empty() || std::ranges::find(volume.maps, map_id) != volume.maps.end();
}

bool Catalog::area_contains(const MapInfo &info, const WorldPosition &position) const {
  if (!info.area_min_cm || !info.area_max_cm) {
    return false;
  }
  const double x_cm = position.x_mm / 10.0;
  const double y_cm = position.y_mm / 10.0;
  return (*info.area_min_cm)[0] <= x_cm && x_cm <= (*info.area_max_cm)[0] &&
         (*info.area_min_cm)[1] <= y_cm && y_cm <= (*info.area_max_cm)[1];
}

std::optional<WorldPosition> Catalog::arrival(const WorldPosition &current, int to_map_id) const {
  const auto *destination = map(to_map_id);
  if (!destination) {
    return std::nullopt;
  }
  WorldPosition result = current;
  result.map_id = to_map_id;
  // A destination without an authored area (the main world, public fields) has no bound.
  if (!destination->area_min_cm || area_contains(*destination, current)) {
    return result;
  }
  if (destination->translation_cm) {
    result.x_mm = static_cast<int>(std::lround((*destination->translation_cm)[0] * 10.0));
    result.y_mm = static_cast<int>(std::lround((*destination->translation_cm)[1] * 10.0));
    result.z_mm = static_cast<int>(std::lround((*destination->translation_cm)[2] * 10.0));
    return result;
  }
  return std::nullopt;
}

int Catalog::map_move_cut_scene(int from_map_id, int to_map_id,
                                const TravelContext &context) const {
  for (const auto &row : map_move_cut_scenes_) {
    // From map 0 is the login entry into the tutorial, which the tutorial instance owns.
    if (row.from_map_id != from_map_id || row.to_map_id != to_map_id ||
        !cut_scenes_.contains(row.cut_scene_id)) {
      continue;
    }
    if (conditions_pass(row.conditions, from_map_id, to_map_id, context)) {
      return row.cut_scene_id;
    }
  }
  return 0;
}

bool Catalog::server_cut_scene(int cut_scene_id) const {
  const auto found = cut_scenes_.find(cut_scene_id);
  return found != cut_scenes_.end() && !found->second.client_only;
}

bool Catalog::invisible_cut_scene(int cut_scene_id) const {
  const auto found = cut_scenes_.find(cut_scene_id);
  return found != cut_scenes_.end() && found->second.invisible;
}

std::set<int> Catalog::copy_maps() const {
  std::set<int> result;
  for (const auto &[map_id, info] : maps_) {
    if (info.type == "Instance" || info.type == "Dungeon") {
      result.insert(map_id);
    }
  }
  return result;
}

std::vector<std::uint64_t> Catalog::volume_doodads(int map_id) const {
  std::set<std::uint64_t> units;
  for (const auto &volume : volumes_) {
    if (volume.rules.empty() || !volume_on_map(volume, map_id)) {
      continue;
    }
    for (const auto &rule : volume.rules) {
      for (const auto &task : rule.at("tasks")) {
        if (task.at("kind") != "ChangeDoodadPhase" || !task.contains("unit_id")) {
          continue;
        }
        units.insert(std::stoull(task.at("unit_id").get<std::string>()));
      }
    }
  }
  return {units.begin(), units.end()};
}

std::optional<CutSceneEndPoint> Catalog::cut_scene_end_point(int cut_scene_id) const {
  const auto found = cut_scenes_.find(cut_scene_id);
  if (found == cut_scenes_.end()) {
    return std::nullopt;
  }
  return found->second.end_point;
}

std::optional<WorldPosition> Catalog::cut_scene_end_destination(int cut_scene_id,
                                                                const WorldPosition &current) const {
  const auto end_point = cut_scene_end_point(cut_scene_id);
  if (!end_point) {
    return std::nullopt;
  }
  const auto *info = map(end_point->map_id);
  if (!info || info->type == "Raid") {
    return std::nullopt;
  }

  std::optional<WorldPosition> destination = end_point->position;
  if (!destination) {
    // CUSTOM: the end point's spawn table is not in the export; travel's arrival rule applies.
    destination = arrival(current, end_point->map_id);
  }
  if (!destination) {
    return std::nullopt;
  }
  if (destination->map_id == current.map_id && destination->x_mm == current.x_mm &&
      destination->y_mm == current.y_mm && destination->z_mm == current.z_mm) {
    return std::nullopt;
  }
  if (!end_point->position) {
    destination->yaw = current.yaw;
  }
  return destination;
}

TravelPlan Catalog::move_to(const WorldPosition &current, int to_map_id,
                            const TravelContext &context, bool client_waits) const {
  if (to_map_id == current.map_id) {
    TravelPlan plan;
    plan.handled = true;
    return plan;
  }
  if (!travel_allowed(current.map_id, to_map_id, context)) {
    return refused("map " + std::to_string(current.map_id) + " -> " + std::to_string(to_map_id) +
                       ": entrance or exit conditions are not met",
                   client_waits);
  }
  const auto destination = arrival(current, to_map_id);
  if (!destination) {
    return refused("map " + std::to_string(to_map_id) + " has no arrival position",
                   client_waits);
  }

  TravelPlan plan;
  plan.handled = true;
  plan.client_waits_for_move = client_waits;
  plan.destination = destination;
  plan.cut_scene_id = map_move_cut_scene(current.map_id, to_map_id, context);
  return plan;
}

TravelPlan Catalog::run_rules(const Volume &volume, const WorldPosition &current, bool leave,
                              const TravelContext &context) const {
  TravelPlan plan;
  plan.handled = true;
  const auto trigger_type = leave ? "Leave" : "Enter";
  for (const auto &rule : volume.rules) {
    if (rule.at("trigger_type") != trigger_type ||
        !conditions_pass(rule.at("conditions"), current.map_id, current.map_id, context)) {
      continue;
    }
    for (const auto &task : rule.at("tasks")) {
      const auto kind = task.at("kind").get<std::string>();
      if (kind == "PlayCutScene") {
        const int cut_scene_id = task.at("cut_scene_id");
        // The client watches one server cutscene at a time.
        if (plan.cut_scenes.empty() && server_cut_scene(cut_scene_id)) {
          plan.cut_scenes.push_back(cut_scene_id);
        }
      } else if (kind == "MapMove" && !plan.destination) {
        const auto &position = task.at("position_mm");
        WorldPosition destination{task.at("map_id").get<int>(), position.at(0).get<int>(),
                                  position.at(1).get<int>(), position.at(2).get<int>(),
                                  current.yaw};
        if (!map(destination.map_id)) {
          plan.refusal = "rule map move names unknown map " + std::to_string(destination.map_id);
          continue;
        }
        plan.destination = destination;
        plan.cut_scene_id = map_move_cut_scene(current.map_id, destination.map_id, context);
      } else if (kind == "ChangeDoodadPhase" && task.contains("unit_id")) {
        plan.doodad_phases.push_back(
            {task.at("unit_id").get<std::string>(), task.at("phase").get<int>()});
      }
      // ClientPlayDialogue runs on the client.
    }
  }
  return plan;
}

TravelPlan Catalog::level_trigger(const WorldPosition &current, const Json &request,
                                  const TravelContext &context) const {
  if (!request.contains("trigger_id_x") || !request.contains("trigger_id_y")) {
    return {};
  }
  std::array<std::string, 2> wire;
  try {
    wire = {wire_text(request.at("trigger_id_x")), wire_text(request.at("trigger_id_y"))};
  } catch (const std::exception &) {
    return {};
  }
  const auto found = volumes_by_wire_.find(wire);
  if (found == volumes_by_wire_.end()) {
    return {};
  }
  const auto &volume = volumes_[found->second];
  const bool leave = request.value("leave", false);
  const bool travel_volume = volume.kind != "rules";
  // A travel volume naming another map left the client waiting for a map move.
  const bool client_waits =
      travel_volume && !leave &&
      (volume.kind == "portal" || volume.destination_map_id != current.map_id);

  if (!volume_on_map(volume, current.map_id)) {
    return refused("volume is not placed on map " + std::to_string(current.map_id), client_waits);
  }
  const auto reported = reported_position(request, current.map_id);
  if (reported && !near(*reported, current, kRequestRangeMm)) {
    return refused("request point is far from the tracked position", client_waits);
  }
  if (!leave && !volume.contains(current, kVolumeToleranceCm)) {
    return refused("tracked position is outside the volume", client_waits);
  }

  if (volume.kind == "rules") {
    return run_rules(volume, current, leave, context);
  }
  if (leave) {
    TravelPlan plan;
    plan.handled = true;
    return plan;
  }
  if (volume.kind == "portal") {
    const auto portal = std::ranges::find_if(portals_, [&](const Portal &row) {
      return row.portal_id == volume.portal_id && row.from_map_id == current.map_id;
    });
    if (portal == portals_.end()) {
      return refused("portal " + std::to_string(volume.portal_id) + " has no row from map " +
                         std::to_string(current.map_id),
                     client_waits);
    }
    if (portal->to_map_id == current.map_id) {
      // Same-map dungeon portals (GoBoss/GoExit) jump to the destination of the dungeon's own
      // boss or exit doodad interaction (Candidate). Rows without one stay refused.
      if (!portal->arrival_mm) {
        return refused("portal " + std::to_string(volume.portal_id) +
                           " moves within the map to an unrecovered spawn point",
                       client_waits);
      }
      TravelPlan plan;
      plan.handled = true;
      plan.client_waits_for_move = client_waits;
      plan.destination = WorldPosition{current.map_id, (*portal->arrival_mm)[0],
                                       (*portal->arrival_mm)[1], (*portal->arrival_mm)[2],
                                       current.yaw};
      return plan;
    }
    return move_to(current, portal->to_map_id, context, client_waits);
  }

  int destination = volume.destination_map_id;
  if (destination == 0) {
    const auto *info = map(current.map_id);
    destination = info ? info->parent_map_id : 0;
  }
  if (destination == 0) {
    return refused("volume names no destination map", client_waits);
  }
  return move_to(current, destination, context, client_waits);
}

TravelPlan Catalog::channel_entry(const WorldPosition &current, const Json &request,
                                  const TravelContext &context) const {
  // proto3 omits a zero map_id, which asks for the parent map.
  const auto found = request.find("map_id");
  if (found != request.end() && !found->is_number_integer()) {
    return refused("channel entry names no map", true);
  }
  const auto *info = map(current.map_id);
  if (!info) {
    return refused("current map is unknown", true);
  }
  int destination = found == request.end() ? 0 : found->get<int>();
  if (destination == 0) {
    destination = info->parent_map_id;
  }
  const auto *target = map(destination);
  if (!target || destination == current.map_id) {
    return refused("channel entry names no other known map", true);
  }

  const auto reported = reported_position(request, current.map_id);
  if (reported && !near(*reported, current, kRequestRangeMm)) {
    return refused("request point is far from the tracked position", true);
  }

  // Leaving to the parent map, entering a child whose entry volume holds the character, or
  // entering a child whose authored area holds the character.
  const bool to_parent = destination == info->parent_map_id;
  bool at_entry = to_parent;
  if (!at_entry && target->parent_map_id == current.map_id) {
    at_entry = area_contains(*target, current) ||
               std::ranges::any_of(volumes_, [&](const Volume &volume) {
                 return volume.destination_map_id == destination &&
                        volume_on_map(volume, current.map_id) &&
                        volume.contains(current, kVolumeToleranceCm);
               });
  }
  if (!at_entry) {
    return refused("the character is not at an entry of map " + std::to_string(destination),
                   true);
  }
  return move_to(current, destination, context, true);
}

TravelPlan Catalog::cliffs_gate(const WorldPosition &current, const Json &request,
                                const TravelContext &context) const {
  if (!cliffs_gate_ || !request.contains("target_unit_id") || !request.contains("param") ||
      !request.at("param").is_object()) {
    return {};
  }

  const auto interaction = request.at("param").find("interaction_id");
  if (interaction == request.at("param").end() || !interaction->is_number_integer() ||
      interaction->get<int>() != cliffs_gate_->interaction_id) {
    return {};
  }

  try {
    if (wire_text(request.at("target_unit_id")) != cliffs_gate_->unit_id) {
      return {};
    }
  } catch (const std::exception &) {
    return {};
  }

  if (current.map_id != cliffs_gate_->source_map_id ||
      !near(current, cliffs_gate_->position, kRequestRangeMm)) {
    return refused("Cliffs entrance is outside interaction range", false);
  }
  const Json prerequisite{{"kind", "Quest"},
                          {"quest_id", cliffs_gate_->required_quest_id},
                          {"requirement", "QuestComplete"}};
  if (!context.evaluate || !context.evaluate(prerequisite) ||
      context.party_role == PartyRole::member ||
      !travel_allowed(current.map_id, cliffs_gate_->destination.map_id, context)) {
    return refused("Cliffs entrance conditions are not met", false);
  }

  TravelPlan plan;
  plan.handled = true;
  plan.actor_doodad_phases.push_back({cliffs_gate_->unit_id, 0});
  plan.cut_scenes.push_back(cliffs_gate_->cut_scene_id);
  plan.cut_scene_destination = cliffs_gate_->destination;
  return plan;
}

TravelSession::TravelSession(std::shared_ptr<const Catalog> catalog, std::uint64_t player_unit_id)
    : catalog_(std::move(catalog)), player_unit_id_(player_unit_id) {
  if (!catalog_) {
    throw std::invalid_argument("A travel session needs its catalog");
  }
}

const Catalog &TravelSession::catalog() const {
  return *catalog_;
}

bool TravelSession::cut_scene_active() const noexcept {
  return active_cut_scene_.has_value();
}

std::vector<GameplayPacket> TravelSession::play(const TravelPlan &plan,
                                                const WorldPosition &position,
                                                std::int64_t monotonic_ticks) {
  std::vector<GameplayPacket> packets;
  for (const auto &change : plan.actor_doodad_phases) {
    packets.push_back({kDoodadPhaseChangedNotification,
                       {{"unit_id", change.unit_id},
                        {"phase_id", change.phase},
                        {"phase_begin_tick", wire_monotonic_tick(monotonic_ticks)}}});
  }
  if (plan.cut_scenes.empty() || active_cut_scene_) {
    return packets;
  }
  const int cut_scene_id = plan.cut_scenes.front();
  if (catalog_->invisible_cut_scene(cut_scene_id)) {
    packets.push_back({kVisibleChannelNotification,
                       {{"unit_id", std::to_string(player_unit_id_)},
                        {"visible_channel", std::to_string(kCutSceneVisibleChannel)}}});
  }
  packets.push_back({kCutScenePlayNotification,
                     {{"cut_scene_id", cut_scene_id},
                      {"unit_id", std::to_string(player_unit_id_)},
                      {"pos", position_json(position)},
                      {"yaw", position.yaw}}});
  active_cut_scene_ = cut_scene_id;
  // A captured gate names its destination; otherwise the CutScene row's end point applies. The
  // character watches the scene where it stands, so the arrival rule uses this position.
  pending_cut_scene_destination_ = plan.cut_scene_destination
                                       ? plan.cut_scene_destination
                                       : catalog_->cut_scene_end_destination(cut_scene_id,
                                                                             position);
  return packets;
}

std::optional<std::vector<GameplayPacket>>
TravelSession::end(const WorldGameplayRequest &request) {
  const int cut_scene_id = request.message.value("cut_scene_id", 0);
  if (!active_cut_scene_ || *active_cut_scene_ != cut_scene_id) {
    return std::nullopt;
  }
  active_cut_scene_.reset();

  completed_cut_scene_destination_ = pending_cut_scene_destination_;
  pending_cut_scene_destination_.reset();

  std::vector<GameplayPacket> packets;
  if (!completed_cut_scene_destination_ && catalog_->invisible_cut_scene(cut_scene_id)) {
    packets.push_back({kVisibleChannelNotification,
                       {{"unit_id", std::to_string(player_unit_id_)},
                        {"visible_channel", std::to_string(kDefaultVisibleChannel)}}});
  }
  return packets;
}

std::optional<WorldPosition> TravelSession::take_cut_scene_destination() {
  auto destination = completed_cut_scene_destination_;
  completed_cut_scene_destination_.reset();
  return destination;
}

} // namespace chronicles::travel
