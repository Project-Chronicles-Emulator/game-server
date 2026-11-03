#pragma once

#include "persistence/character_state_types.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

// Unreal FRotator basis for pitch/yaw/roll in degrees: the rows are the local box axes, so a
// world displacement projected onto row i gives the local coordinate along axis i.
std::array<std::array<double, 3>, 3> unreal_rotation_axes(double pitch_degrees,
                                                          double yaw_degrees,
                                                          double roll_degrees);

// Immutable authored channel boundaries and spatial zone cells. This resolves same-world
// handoffs; it does not grant quests or derive unresolved spawn-position or regional-channel
// rules. Entrance and exit conditions of a resolved destination are left to the caller
// (see `zone_map(...)->gated`).
class MapChannels {
public:
  // An ArcheWorld map that zone channel requests may leave or reach.
  struct ZoneMap {
    std::string type;
    int parent_map_id{};
    bool gated{};
  };

  static std::optional<MapChannels> load(const std::filesystem::path &catalog_root,
                                         const std::vector<nlohmann::json> &map_rows);

  MapChannels(const nlohmann::json &document, const std::vector<nlohmann::json> &map_rows,
              const nlohmann::json &spatial_channels = nlohmann::json::object());

  // Ungated ArcheWorld fields, instances and points of interest.
  bool supports_map(int map_id) const;

  const ZoneMap *zone_map(int map_id) const;

  // The destination of PC2S_ZoneChannelMoveReq at the request's reported position (bounded to
  // five metres of `current`), keeping the current yaw. The client checks the destination's
  // own position against the MapMoveStartNoti spawn point to keep the move seamless, so the
  // reported point is used rather than the older tracked one.
  std::optional<WorldPosition> resolve(const WorldPosition &current,
                                       const nlohmann::json &request) const;

  // The public field channel (authored Map.area of an ungated zone field) that contains an
  // open-world destination, when the destination names the main world map or a public field
  // whose own area does not contain it. A teleport then loads that region's channel (its NPCs,
  // monsters and services). A nested field outside its area may return to its authored public
  // parent if no exit condition applies. Instances and other maps keep their identity: nullopt. With
  // `keep_public_channel` only the main world map is routed; a named public field is kept.
  std::optional<int> region_map_for(const WorldPosition &destination,
                                    bool keep_public_channel = false) const;

private:
  // A seamless crossing's destination: the deepest occupied cell's zone (leaving a nested zone
  // first returns to its parent, as the client asks), else another public region whose authored
  // area holds the position.
  std::optional<int> crossing_region(int current_map_id, const WorldPosition &position) const;
  std::optional<int> nearest_region(const WorldPosition &position, int excluded_map_id) const;
  void load_spatial_grids(const nlohmann::json &spatial_channels);

  struct SpatialCell {
    int map_id{};
    int depth{};
  };

  struct Boundary {
    int destination_map_id{};
    std::array<double, 3> center_cm{};
    std::array<double, 3> extent_cm{};
    std::array<std::array<double, 3>, 3> axes{};
    std::array<std::string, 2> trigger_wire;

    bool contains(const WorldPosition &position) const;
  };

  struct RegionArea {
    std::array<double, 2> min_cm{};
    std::array<double, 2> max_cm{};

    bool contains(const WorldPosition &position) const;
    double centre_distance_squared(const WorldPosition &position) const;
  };

  std::map<int, bool> supported_maps_;
  std::map<int, ZoneMap> zone_maps_;
  std::map<std::pair<int, int>, SpatialCell> spatial_channels_;
  std::map<int, RegionArea> region_areas_;
  std::map<int, RegionArea> nested_field_areas_;
  std::vector<Boundary> boundaries_;
};

} // namespace chronicles
