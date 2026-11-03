#include "world/core/map_channels.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t kMaximumDefinitionBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaximumMapRows = 10000;
constexpr std::size_t kMaximumBoundaries = 100000;
constexpr double kMaximumGeometryMagnitudeCm = 100000000;
constexpr double kRequestRangeMm = 5000;
// Local tolerance for movement polling and capsule overlap, not official collision policy.
constexpr double kBoundaryToleranceCm = 100;
constexpr auto kArcheWorld = "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld";
constexpr int kSpatialCellWidthCm = 5100;
// Map row 1 is the whole ArcheWorld ("main world"); no public channel uses it directly.
constexpr int kMainWorldMapId = 1;
// Bound authored grid loading separately from the physical travel/range policy.
constexpr std::size_t kMaximumSpatialGrids = 1000;
constexpr int kMaximumSpatialGridDimension = 1000;
constexpr int kMaximumSpatialCellMagnitude = 20000;
constexpr std::size_t kMaximumOccupiedSpatialCells = 100000;
constexpr int kMaximumSpatialDepth = 8;

int integer(const Json &value) {
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() &&
       value.get<std::uint64_t>() > std::numeric_limits<int>::max())) {
    throw std::runtime_error("Map channel integer is invalid");
  }
  const auto number = value.get<std::int64_t>();
  if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
    throw std::runtime_error("Map channel integer is outside its range");
  }
  return static_cast<int>(number);
}

int map_identifier(const Json &value) {
  if (!value.is_string()) {
    throw std::runtime_error("Map channel catalog ID must be decimal text");
  }
  const auto text = value.get<std::string>();
  int result{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size() || result <= 0) {
    throw std::runtime_error("Map channel catalog ID is invalid");
  }
  return result;
}

std::string wire_identifier(const Json &value) {
  if (!value.is_string()) {
    throw std::runtime_error("Map channel trigger ID must be decimal text");
  }
  const auto text = value.get<std::string>();
  std::uint64_t result{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::runtime_error("Map channel trigger ID is invalid");
  }
  return std::to_string(result);
}

double number(const Json &value) {
  if (!value.is_number()) {
    throw std::runtime_error("Map channel geometry must be numeric");
  }
  const double result = value.get<double>();
  if (!std::isfinite(result) || std::abs(result) > kMaximumGeometryMagnitudeCm) {
    throw std::runtime_error("Map channel geometry is outside its range");
  }
  return result;
}

std::array<double, 3> vector(const Json &value) {
  return {number(value.at("x")), number(value.at("y")), number(value.at("z"))};
}

std::array<std::array<double, 3>, 3> rotation_axes(const Json &rotation) {
  return unreal_rotation_axes(number(rotation.at("pitch")), number(rotation.at("yaw")),
                              number(rotation.at("roll")));
}

WorldPosition requested_position(const WorldPosition &current, const Json &request) {
  const auto &position = request.at("pos");
  if (!position.is_object()) {
    throw std::runtime_error("Map channel request position is invalid");
  }
  WorldPosition result{current.map_id, integer(position.value("millimeter_x", Json(0))),
                       integer(position.value("millimeter_y", Json(0))),
                       integer(position.value("millimeter_z", Json(0))), current.yaw};
  if (request.contains("body_yaw")) {
    number(request.at("body_yaw"));
  }
  return result;
}

} // namespace

std::array<std::array<double, 3>, 3> unreal_rotation_axes(double pitch_degrees,
                                                          double yaw_degrees,
                                                          double roll_degrees) {
  const double radians = std::numbers::pi / 180;
  const double pitch = pitch_degrees * radians;
  const double yaw = yaw_degrees * radians;
  const double roll = roll_degrees * radians;
  const double cp = std::cos(pitch);
  const double sp = std::sin(pitch);
  const double cy = std::cos(yaw);
  const double sy = std::sin(yaw);
  const double cr = std::cos(roll);
  const double sr = std::sin(roll);

  return {{{cp * cy, cp * sy, sp},
           {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
           {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}}};
}

std::optional<MapChannels> MapChannels::load(const std::filesystem::path &catalog_root,
                                             const std::vector<Json> &map_rows) {
  if (catalog_root.empty()) {
    return std::nullopt;
  }
  const auto path = catalog_root.parent_path() / "triggers" / "ArcheWorld-level-triggers.json";
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  if (std::filesystem::file_size(path) > kMaximumDefinitionBytes) {
    throw std::runtime_error("Map channel definitions exceed the size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open map channel definitions");
  }
  Json document;
  input >> document;
  Json spatial = Json::object();
  const auto spatial_path = path.parent_path() / "ArcheWorld-zone-channels.json";
  if (std::filesystem::exists(spatial_path)) {
    if (std::filesystem::file_size(spatial_path) > kMaximumDefinitionBytes) {
      throw std::runtime_error("Spatial channel definitions exceed the size limit");
    }
    std::ifstream spatial_input(spatial_path);
    if (!spatial_input) {
      throw std::runtime_error("Cannot open spatial channel definitions");
    }
    spatial_input >> spatial;
  }
  return MapChannels(document, map_rows, spatial);
}

MapChannels::MapChannels(const Json &document, const std::vector<Json> &map_rows,
                         const Json &spatial_channels) {
  if (document.at("format_version") != 1 || document.at("world") != "ArcheWorld" ||
      !document.at("volumes").is_array() || document.at("volumes").size() > kMaximumBoundaries ||
      map_rows.size() > kMaximumMapRows) {
    throw std::runtime_error("Unsupported map channel definitions");
  }

  for (const auto &row : map_rows) {
    const int map_id = map_identifier(row.at("id"));
    const auto type = row.value("type", std::string{});
    const bool archeworld = row.value("world", std::string{}) == kArcheWorld;
    const bool gated = !row.value("FK_enteranceConditionId", std::string{}).empty() ||
                       !row.value("FK_exitConditionId", std::string{}).empty();
    const bool supported = archeworld &&
                           (type == "Field" || type == "Instance" || type == "PointOfInterest") &&
                           !gated;
    if (!supported_maps_.emplace(map_id, supported).second) {
      throw std::runtime_error("Duplicate map channel catalog ID");
    }
    if (archeworld && (type == "Field" || type == "Instance" || type == "PointOfInterest" ||
                       type == "HousingArea")) {
      const auto parent = row.value("parentMapId", std::string{});
      zone_maps_.emplace(map_id,
                         ZoneMap{type, parent.empty() ? 0 : map_identifier(row.at("parentMapId")),
                                 gated});
    }
    // The same public zone channels as tools/build_zone_channels.py: an ungated top-level
    // ArcheWorld field with a zone key and an authored area.
    if (supported && type == "Field" && row.value("parentMapId", std::string{}).empty() &&
        !row.value("FK_zoneKey", std::string{}).empty() &&
        !row.value("area", std::string{}).empty()) {
      const auto area = Json::parse("{" + row.at("area").get<std::string>() + "}");
      RegionArea region;
      region.min_cm = {number(area.at("min").at("x")), number(area.at("min").at("y"))};
      region.max_cm = {number(area.at("max").at("x")), number(area.at("max").at("y"))};
      if (region.min_cm[0] > region.max_cm[0] || region.min_cm[1] > region.max_cm[1]) {
        throw std::runtime_error("Inverted public map area");
      }
      region_areas_.emplace(map_id, region);
    }
  }

  if (!spatial_channels.empty()) {
    load_spatial_grids(spatial_channels);
  }

  std::set<std::string> guids;
  std::set<std::array<std::string, 2>> wire_ids;
  for (const auto &row : document.at("volumes")) {
    const auto guid = row.at("trigger_guid").get<std::string>();
    const auto &wire = row.at("trigger_id_wire");
    if (guid.empty() || guid.size() > 128 || !wire.is_array() || wire.size() != 2) {
      throw std::runtime_error("Invalid map channel trigger identity");
    }
    const std::array<std::string, 2> trigger_wire{wire_identifier(wire.at(0)),
                                                  wire_identifier(wire.at(1))};
    if (!guids.insert(guid).second || !wire_ids.insert(trigger_wire).second) {
      throw std::runtime_error("Duplicate map channel trigger identity");
    }
    if (!row.at("is_activate").get<bool>() || row.at("kind") != "channelSession" ||
        row.at("shape") != "box" || row.at("spawn_position_id") != 0) {
      continue;
    }
    const int destination = integer(row.at("destination_map_id"));
    if (destination <= 0) {
      throw std::runtime_error("Map channel destination must be positive");
    }

    const auto &geometry = row.at("geometry");
    Boundary boundary;
    boundary.destination_map_id = destination;
    boundary.trigger_wire = trigger_wire;
    boundary.center_cm = vector(geometry.at("center"));
    boundary.extent_cm = vector(geometry.at("extent"));
    boundary.axes = rotation_axes(geometry.at("rotation"));
    for (const double extent : boundary.extent_cm) {
      if (extent <= 0) {
        throw std::runtime_error("Map channel box extent must be positive");
      }
    }
    // Keep gated and unsupported targets: their overlap makes an unlabelled
    // request ambiguous even when another nearby target would be eligible.
    boundaries_.push_back(std::move(boundary));
  }
}

bool MapChannels::supports_map(int map_id) const {
  const auto found = supported_maps_.find(map_id);
  return found != supported_maps_.end() && found->second;
}

const MapChannels::ZoneMap *MapChannels::zone_map(int map_id) const {
  const auto found = zone_maps_.find(map_id);
  return found == zone_maps_.end() ? nullptr : &found->second;
}

// Format 1 grids are top-level fields; format 2 adds nested zones (villages, points of
// interest, housing areas) with their nesting depth. A deeper zone's cell takes precedence over
// its parent's; zones of equal depth never overlap.
void MapChannels::load_spatial_grids(const Json &spatial_channels) {
  const auto format = spatial_channels.at("format_version");
  if ((format != 1 && format != 2) || spatial_channels.at("world") != "ArcheWorld" ||
      spatial_channels.at("cell_width_cm") != kSpatialCellWidthCm ||
      !spatial_channels.at("grids").is_array() ||
      spatial_channels.at("grids").size() > kMaximumSpatialGrids) {
    throw std::runtime_error("Unsupported spatial channel definitions");
  }

  for (const auto &grid : spatial_channels.at("grids")) {
    const int map_id = integer(grid.at("map_id"));
    const int depth = integer(grid.value("depth", Json(0)));
    const int min_x = integer(grid.at("min_cell_x"));
    const int min_y = integer(grid.at("min_cell_y"));
    const int width = integer(grid.at("width"));
    const int height = integer(grid.at("height"));
    const auto mask = grid.at("mask").get<std::string>();
    const auto *target = zone_map(map_id);
    const bool depth_matches = target && (depth == 0) == (target->parent_map_id == 0);
    if (!target || !depth_matches || depth < 0 || depth > kMaximumSpatialDepth ||
        std::abs(static_cast<double>(min_x)) > kMaximumSpatialCellMagnitude ||
        std::abs(static_cast<double>(min_y)) > kMaximumSpatialCellMagnitude || width <= 0 ||
        height <= 0 || width > kMaximumSpatialGridDimension ||
        height > kMaximumSpatialGridDimension ||
        mask.size() != static_cast<std::size_t>(width) * height) {
      throw std::runtime_error("Invalid spatial channel grid");
    }

    for (std::size_t index = 0; index < mask.size(); ++index) {
      if (mask[index] == '0') {
        continue;
      }
      if (mask[index] != '1' || spatial_channels_.size() >= kMaximumOccupiedSpatialCells) {
        throw std::runtime_error("Invalid spatial channel cell");
      }
      const auto cell = std::pair{min_x + static_cast<int>(index % width),
                                  min_y + static_cast<int>(index / width)};
      const auto [existing, inserted] = spatial_channels_.emplace(cell, SpatialCell{map_id, depth});
      if (inserted) {
        continue;
      }
      if (existing->second.depth == depth) {
        throw std::runtime_error("Overlapping spatial channel cell");
      }
      if (existing->second.depth < depth) {
        existing->second = SpatialCell{map_id, depth};
      }
    }
  }
}

std::optional<int> MapChannels::region_map_for(const WorldPosition &destination,
                                               bool keep_public_channel) const {
  if (destination.map_id != kMainWorldMapId && keep_public_channel) {
    return std::nullopt;
  }

  const auto requested = region_areas_.find(destination.map_id);
  if (destination.map_id != kMainWorldMapId && requested == region_areas_.end()) {
    return std::nullopt;
  }
  if (requested != region_areas_.end() && requested->second.contains(destination)) {
    return std::nullopt;
  }
  return nearest_region(destination, destination.map_id);
}

std::optional<int> MapChannels::crossing_region(int current_map_id,
                                                const WorldPosition &position) const {
  const auto *current = zone_map(current_map_id);
  const int current_parent = current ? current->parent_map_id : 0;

  // The authored occupied cells are exact; the deepest zone of a cell decides first.
  const auto cell =
      std::pair{static_cast<int>(std::floor(position.x_mm / (10.0 * kSpatialCellWidthCm))),
                static_cast<int>(std::floor(position.y_mm / (10.0 * kSpatialCellWidthCm)))};
  const auto channel = spatial_channels_.find(cell);
  if (channel != spatial_channels_.end()) {
    const int target = channel->second.map_id;
    if (target == current_map_id) {
      return std::nullopt;
    }
    // Leaving a nested zone, the client asks for the current map's parent; a zone nested in
    // the current map is entered directly.
    if (current_parent != 0 && zone_map(target)->parent_map_id != current_map_id) {
      return current_parent;
    }
    return target;
  }

  if (current_parent != 0) {
    return current_parent;
  }
  // Outside every occupied cell, another public region whose authored area holds the
  // position receives the crossing (the client asks only after leaving its own zone).
  return nearest_region(position, current_map_id);
}

std::optional<int> MapChannels::nearest_region(const WorldPosition &position,
                                               int excluded_map_id) const {
  // Authored Map.area envelopes overlap along zone borders; the nearest centre wins there.
  std::optional<int> selected;
  double selected_distance = std::numeric_limits<double>::infinity();
  for (const auto &[map_id, area] : region_areas_) {
    if (map_id == excluded_map_id || !area.contains(position)) {
      continue;
    }
    const double distance = area.centre_distance_squared(position);
    if (distance < selected_distance) {
      selected = map_id;
      selected_distance = distance;
    }
  }
  return selected;
}

bool MapChannels::RegionArea::contains(const WorldPosition &position) const {
  const double x_cm = position.x_mm / 10.0;
  const double y_cm = position.y_mm / 10.0;
  return min_cm[0] <= x_cm && x_cm <= max_cm[0] && min_cm[1] <= y_cm && y_cm <= max_cm[1];
}

double MapChannels::RegionArea::centre_distance_squared(const WorldPosition &position) const {
  const double dx = position.x_mm / 10.0 - (min_cm[0] + max_cm[0]) / 2;
  const double dy = position.y_mm / 10.0 - (min_cm[1] + max_cm[1]) / 2;
  return dx * dx + dy * dy;
}

bool MapChannels::Boundary::contains(const WorldPosition &position) const {
  const std::array<double, 3> displacement{position.x_mm / 10.0 - center_cm[0],
                                           position.y_mm / 10.0 - center_cm[1],
                                           position.z_mm / 10.0 - center_cm[2]};
  for (std::size_t axis = 0; axis < axes.size(); ++axis) {
    double local_coordinate{};
    for (std::size_t component = 0; component < displacement.size(); ++component) {
      local_coordinate += displacement[component] * axes[axis][component];
    }
    if (std::abs(local_coordinate) > extent_cm[axis] + kBoundaryToleranceCm) {
      return false;
    }
  }
  return true;
}

std::optional<WorldPosition> MapChannels::resolve(const WorldPosition &current,
                                                  const Json &request) const {
  if (!zone_map(current.map_id) || !std::isfinite(current.yaw)) {
    return std::nullopt;
  }
  try {
    const auto reported = requested_position(current, request);
    const double dx = static_cast<double>(current.x_mm) - reported.x_mm;
    const double dy = static_cast<double>(current.y_mm) - reported.y_mm;
    const double dz = static_cast<double>(current.z_mm) - reported.z_mm;
    if (dx * dx + dy * dy + dz * dz > kRequestRangeMm * kRequestRangeMm) {
      return std::nullopt;
    }

    std::optional<std::array<std::string, 2>> requested_trigger;
    if (request.contains("trigger_id_x") || request.contains("trigger_id_y")) {
      requested_trigger = std::array<std::string, 2>{wire_identifier(request.at("trigger_id_x")),
                                                     wire_identifier(request.at("trigger_id_y"))};
    }
    const Boundary *selected{};
    for (const auto &boundary : boundaries_) {
      if (boundary.destination_map_id == current.map_id ||
          (requested_trigger && boundary.trigger_wire != *requested_trigger) ||
          !boundary.contains(current) || !boundary.contains(reported)) {
        continue;
      }
      if (selected && selected->destination_map_id != boundary.destination_map_id) {
        return std::nullopt;
      }
      selected = &boundary;
    }
    if (selected && !supports_map(selected->destination_map_id)) {
      return std::nullopt;
    }
    // The client expects the spawn point where it asked from (the seamless check allows about
    // half a metre for a parent return), so the bounded reported point replaces the older
    // tracked one; the tracked yaw is kept.
    auto destination = reported;
    destination.yaw = current.yaw;
    if (selected) {
      destination.map_id = selected->destination_map_id;
    } else {
      // Seamless field crossings send the same request without entering a trigger box.
      // Its position was bounded against the authoritative position above.
      const auto crossed = crossing_region(current.map_id, reported);
      if (!crossed) {
        return std::nullopt;
      }
      destination.map_id = *crossed;
    }
    return destination;
  } catch (const Json::exception &) {
    return std::nullopt;
  } catch (const std::runtime_error &) {
    return std::nullopt;
  }
}

} // namespace chronicles
