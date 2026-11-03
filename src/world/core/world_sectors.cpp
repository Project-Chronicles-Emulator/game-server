#include "world/core/world_sectors.hpp"

#include "data/client_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr double kMillimetresPerCentimetre = 10.0;
constexpr std::uint32_t kSectorAxisMask = kSectorAxisCount - 1;
constexpr int kSectorAxisBits = 14;

struct AreaMinimumMaximum {
  double min_x_cm{};
  double min_y_cm{};
  double max_x_cm{};
  double max_y_cm{};
};

double finite_number(const Json &value, const char *field) {
  if (!value.is_number()) {
    throw std::runtime_error(std::string("Sector grid field is not a number: ") + field);
  }

  const double number = value.get<double>();
  if (!std::isfinite(number)) {
    throw std::runtime_error(std::string("Sector grid field is not finite: ") + field);
  }
  return number;
}

// Catalog areas are stored as an object body without braces: "min":{...},"max":{...}.
AreaMinimumMaximum parse_area(const Json &row) {
  const auto &area_text = row.at("area");
  if (!area_text.is_string()) {
    throw std::runtime_error("Sector grid area is not a fragment string");
  }

  const auto area = Json::parse("{" + area_text.get<std::string>() + "}");
  AreaMinimumMaximum bounds{finite_number(area.at("min").at("x"), "area.min.x"),
                            finite_number(area.at("min").at("y"), "area.min.y"),
                            finite_number(area.at("max").at("x"), "area.max.x"),
                            finite_number(area.at("max").at("y"), "area.max.y")};
  if (bounds.min_x_cm > bounds.max_x_cm || bounds.min_y_cm > bounds.max_y_cm) {
    throw std::runtime_error("Sector grid area is inverted");
  }
  return bounds;
}

int parse_map_id(const Json &row) {
  const auto &text = row.at("id").get_ref<const std::string &>();
  int map_id{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), map_id);
  if (error != std::errc{} || end != text.data() + text.size() || map_id <= 0) {
    throw std::runtime_error("Sector grid has an invalid Map id: " + text);
  }
  return map_id;
}

double parse_sector_width(const Json &row) {
  const auto &text = row.at("sectorWidth").get_ref<const std::string &>();
  std::size_t parsed_characters{};
  const double width = std::stod(text, &parsed_characters);
  if (parsed_characters != text.size() || !std::isfinite(width) || width <= 0) {
    throw std::runtime_error("Sector grid has an invalid sectorWidth: " + text);
  }
  return width;
}

bool has_area(const Json &row) {
  const auto area = row.find("area");
  return area != row.end() && area->is_string() && !area->get_ref<const std::string &>().empty();
}

bool valid_axis_index(std::int64_t index) {
  return index >= 1 && index < kSectorAxisCount;
}

std::int32_t clamp_axis_index(std::int32_t index) {
  return std::clamp<std::int32_t>(index, 1, kSectorAxisCount - 1);
}

} // namespace

std::uint32_t pack_sector_id(SectorCoordinate sector) {
  if (!valid_axis_index(sector.x) || !valid_axis_index(sector.y)) {
    throw std::invalid_argument("Sector coordinate is outside the 14-bit sector range");
  }
  return static_cast<std::uint32_t>(sector.x) |
         (static_cast<std::uint32_t>(sector.y) << kSectorAxisBits);
}

SectorCoordinate unpack_sector_id(std::uint32_t sector_id) {
  return {static_cast<std::int32_t>(sector_id & kSectorAxisMask),
          static_cast<std::int32_t>((sector_id >> kSectorAxisBits) & kSectorAxisMask)};
}

std::optional<WorldSectorGrid> WorldSectorGrid::load(ClientCatalog &catalog) {
  if (!catalog.has_table("OriginMap") || !catalog.has_table("Map")) {
    return std::nullopt;
  }
  return WorldSectorGrid(catalog.rows("OriginMap"), catalog.rows("Map"));
}

WorldSectorGrid::WorldSectorGrid(const std::vector<Json> &origin_rows,
                                 const std::vector<Json> &map_rows) {
  std::unordered_map<std::string, WorldOrigin> origins;
  for (const auto &row : origin_rows) {
    if (!has_area(row)) {
      continue;
    }

    const auto bounds = parse_area(row);
    const WorldOrigin origin{bounds.min_x_cm, bounds.min_y_cm, parse_sector_width(row)};
    const auto world = row.at("world").get<std::string>();
    if (!origins.emplace(world, origin).second) {
      throw std::runtime_error("Sector grid has a duplicate OriginMap world: " + world);
    }
  }

  for (const auto &row : map_rows) {
    const auto world = row.find("world");
    if (world == row.end() || !world->is_string() || !has_area(row)) {
      continue;
    }
    const auto origin = origins.find(world->get<std::string>());
    if (origin == origins.end()) {
      continue;
    }

    // Observed official clipping (maps 10109 and 30104): the block keeps one sector below the
    // sector holding the area minimum and stops at the sector holding the area maximum.
    const auto bounds = parse_area(row);
    const auto minimum = index_of(origin->second, bounds.min_x_cm, bounds.min_y_cm);
    const auto maximum = index_of(origin->second, bounds.max_x_cm, bounds.max_y_cm);
    MapGrid grid{origin->second,
                 {clamp_axis_index(minimum.x - 1), clamp_axis_index(minimum.y - 1)},
                 {clamp_axis_index(maximum.x), clamp_axis_index(maximum.y)}};

    const int map_id = parse_map_id(row);
    if (!maps_.emplace(map_id, grid).second) {
      throw std::runtime_error("Sector grid has a duplicate Map id " + std::to_string(map_id));
    }
  }
}

bool WorldSectorGrid::has_map(int map_id) const {
  return maps_.contains(map_id);
}

std::optional<SectorCoordinate> WorldSectorGrid::sector_at(int map_id, double x_mm,
                                                           double y_mm) const {
  const auto grid = maps_.find(map_id);
  if (grid == maps_.end() || !std::isfinite(x_mm) || !std::isfinite(y_mm)) {
    return std::nullopt;
  }

  const auto sector = index_of(grid->second.origin, x_mm / kMillimetresPerCentimetre,
                               y_mm / kMillimetresPerCentimetre);
  if (!valid_axis_index(sector.x) || !valid_axis_index(sector.y)) {
    return std::nullopt;
  }
  return sector;
}

std::vector<std::uint32_t> WorldSectorGrid::sight_window(int map_id,
                                                         SectorCoordinate center) const {
  const auto grid = maps_.find(map_id);
  if (grid == maps_.end()) {
    return {};
  }

  const auto &first = grid->second.first;
  const auto &last = grid->second.last;
  const auto first_x = std::max(first.x, center.x - kSectorSightRadius);
  const auto last_x = std::min(last.x, center.x + kSectorSightRadius);
  const auto first_y = std::max(first.y, center.y - kSectorSightRadius);
  const auto last_y = std::min(last.y, center.y + kSectorSightRadius);

  std::vector<std::uint32_t> sectors;
  for (auto y = first_y; y <= last_y; ++y) {
    for (auto x = first_x; x <= last_x; ++x) {
      sectors.push_back(pack_sector_id({x, y}));
    }
  }
  return sectors;
}

std::size_t WorldSectorGrid::map_count() const {
  return maps_.size();
}

SectorCoordinate WorldSectorGrid::index_of(const WorldOrigin &origin, double x_cm, double y_cm) {
  const auto axis = [&](double position_cm, double origin_cm) {
    const double index = std::floor((position_cm - origin_cm) / origin.sector_width_cm) + 1.0;
    // Keep far-out-of-range positions representable; callers reject them by range.
    return static_cast<std::int32_t>(
        std::clamp(index, -static_cast<double>(kSectorAxisCount),
                   static_cast<double>(kSectorAxisCount)));
  };
  return {axis(x_cm, origin.min_x_cm), axis(y_cm, origin.min_y_cm)};
}

void SectorSightTracker::arm(int map_id) {
  armed_map_ = map_id;
  center_.reset();
  visible_.clear();
}

void SectorSightTracker::clear() {
  armed_map_.reset();
  center_.reset();
  visible_.clear();
}

std::vector<std::uint32_t> SectorSightTracker::update(const WorldSectorGrid &grid, int map_id,
                                                      double x_mm, double y_mm) {
  if (armed_map_ != map_id) {
    // A map change ends the visit; nothing is announced until the next map load completes.
    if (armed_map_) {
      clear();
    }
    return {};
  }

  const auto center = grid.sector_at(map_id, x_mm, y_mm);
  if (!center || center == center_) {
    return {};
  }
  center_ = center;

  const auto window = grid.sight_window(map_id, *center);
  std::vector<std::uint32_t> entered;
  for (const auto sector_id : window) {
    if (!visible_.contains(sector_id)) {
      entered.push_back(sector_id);
    }
  }
  visible_ = std::set<std::uint32_t>(window.begin(), window.end());
  return entered;
}

GameplayPacket sector_update_packet(std::uint32_t sector_id,
                                    const std::vector<std::uint32_t> &deactivated_gathering) {
  Json message{{"sector_id", sector_id}};
  if (!deactivated_gathering.empty()) {
    message["gathering_deactivated_indices"] = deactivated_gathering;
  }
  return {kSectorUpdateNotification, std::move(message)};
}

} // namespace chronicles
