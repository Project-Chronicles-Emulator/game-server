#include "world/features/combat/revival_spawns.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace chronicles {
namespace {

constexpr std::size_t kMaximumSpotFiles = 4096;
constexpr std::uintmax_t kMaximumSpotFileBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumSpotObjects = 1'000'000;
constexpr std::size_t kMaximumShrines = 100'000;
constexpr std::size_t kMaximumMappedSpawns = 1'000'000;
constexpr std::size_t kMaximumMapRows = 4096;

struct Bounds {
  double min_x;
  double min_y;
  double min_z;
  double max_x;
  double max_y;
  double max_z;

  bool contains(double x, double y, double z) const {
    return x >= min_x && x <= max_x && y >= min_y && y <= max_y &&
           z >= min_z && z <= max_z;
  }
};

struct MapSource {
  int id;
  Bounds bounds;
};

struct IndexedShrine {
  double x_cm;
  double y_cm;
  double z_cm;
  int x_mm;
  int y_mm;
  int z_mm;
  float yaw;
  bool flame;
};

double finite_number(const nlohmann::json &value) {
  if (!value.is_number()) {
    throw std::runtime_error("Invalid revival raw numeric field");
  }

  const double number = value.get<double>();
  if (!std::isfinite(number)) {
    throw std::runtime_error("Non-finite revival raw numeric field");
  }
  return number;
}

Bounds parse_bounds(const nlohmann::json &row) {
  const auto &area = row.at("area");
  if (!area.is_string()) {
    throw std::runtime_error("Invalid Map area for revival index");
  }

  const auto bounds = nlohmann::json::parse("{" + area.get<std::string>() + "}");
  const auto &minimum = bounds.at("min");
  const auto &maximum = bounds.at("max");
  Bounds result{
      finite_number(minimum.at("x")), finite_number(minimum.at("y")),
      finite_number(minimum.at("z")), finite_number(maximum.at("x")),
      finite_number(maximum.at("y")), finite_number(maximum.at("z"))};
  if (result.min_x > result.max_x || result.min_y > result.max_y ||
      result.min_z > result.max_z) {
    throw std::runtime_error("Invalid Map area bounds for revival index");
  }
  return result;
}

int parse_map_id(const nlohmann::json &row) {
  const std::string id = row.at("id").get<std::string>();
  int map_id{};
  const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), map_id);
  if (error != std::errc{} || end != id.data() + id.size() || map_id <= 0) {
    throw std::runtime_error("Invalid Map id for revival index");
  }
  return map_id;
}

std::string map_source_directory(const nlohmann::json &row,
                                 const std::filesystem::path &maps_root) {
  constexpr std::string_view kWorldPrefix = "/Game/Maps/";
  const std::string world = row.at("world").get<std::string>();
  if (!world.starts_with(kWorldPrefix)) {
    throw std::runtime_error("Invalid Map world path for revival index");
  }

  const std::string_view relative(world.data() + kWorldPrefix.size(),
                                  world.size() - kWorldPrefix.size());
  const auto separator = relative.find('/');
  if (separator == std::string_view::npos || separator == 0 ||
      relative.substr(0, separator).find('\\') != std::string_view::npos ||
      relative.substr(0, separator) == "." ||
      relative.substr(0, separator) == "..") {
    throw std::runtime_error("Invalid Map world directory for revival index");
  }
  const auto map_directory = relative.substr(0, separator);
  const auto asset = relative.substr(separator + 1);
  const auto dot = asset.find('.');
  if (dot == std::string_view::npos || dot == 0 ||
      asset.substr(dot + 1) != asset.substr(0, dot) ||
      asset.find('/') != std::string_view::npos ||
      asset.find('\\') != std::string_view::npos) {
    throw std::runtime_error("Invalid Map world asset for revival index");
  }
  std::string source(map_directory);
  const std::string nested = source + "/" + std::string(asset.substr(0, dot));
  std::error_code error;
  const bool nested_directory = std::filesystem::is_directory(maps_root / nested / "DataLayers", error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw std::runtime_error("Cannot inspect revival raw map directory");
  }
  if (nested_directory) {
    return nested;
  }
  return source;

} // namespace
int centimeters_to_millimeters(double centimeters) {
  constexpr double kScale = 10.0;
  const double millimeters = centimeters * kScale;
  // Check before rounding and narrowing; the rounded value must fit an int.
  if (!std::isfinite(millimeters) ||
      millimeters <= static_cast<double>(std::numeric_limits<int>::min()) - 0.5 ||
      millimeters >= static_cast<double>(std::numeric_limits<int>::max()) + 0.5) {
    throw std::runtime_error("Revival spawn coordinate exceeds millimeter range");
  }
  return static_cast<int>(std::llround(millimeters));
}

IndexedShrine parse_shrine(const nlohmann::json &shrine, bool flame) {
  const auto &point = shrine.at("spawnVolume").at("spawnVolumeSphere").at("spawnPosition");
  const double x_cm = finite_number(point.at("x"));
  const double y_cm = finite_number(point.at("y"));
  const double z_cm = finite_number(point.at("z"));
  const double yaw = finite_number(point.at("yaw"));
  if (std::abs(yaw) > std::numeric_limits<float>::max()) {
    throw std::runtime_error("Revival spawn yaw exceeds float range");
  }
  return {x_cm, y_cm, z_cm, centimeters_to_millimeters(x_cm),
          centimeters_to_millimeters(y_cm), centimeters_to_millimeters(z_cm),
          static_cast<float>(yaw), flame};
}

std::string read_spot_file(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > kMaximumSpotFileBytes) {
    throw std::runtime_error("Revival spot_object.json is unreadable or too large");
  }

  std::string data(static_cast<std::size_t>(size), '\0');
  std::ifstream input(path, std::ios::binary);
  if (!input || !input.read(data.data(), static_cast<std::streamsize>(size))) {
    throw std::runtime_error("Cannot read revival spot_object.json");
  }
  return data;
}

} // namespace

RevivalSpawns::RevivalSpawns(const std::filesystem::path &catalog_root,
                             const std::vector<nlohmann::json> &map_rows) {
  if (map_rows.empty() || map_rows.size() > kMaximumMapRows || catalog_root.empty()) {
    throw std::runtime_error("Revival index requires catalog root and Map rows");
  }

  const auto maps_root = catalog_root.parent_path() / "raw" / "X5" / "Content" /
                         "Data" / "BuiltData" / "Maps";
  std::error_code error;
  if (!std::filesystem::is_directory(maps_root, error) || error) {
    throw std::runtime_error("Revival raw Maps directory is missing beside catalog");
  }

  std::unordered_map<std::string, std::vector<MapSource>> map_sources;
  try {
    for (const auto &row : map_rows) {
      const int map_id = parse_map_id(row);
      const Bounds bounds = parse_bounds(row);
      const auto directory = map_source_directory(row, maps_root);
      map_sources[directory].push_back({map_id, bounds});
      by_map_.try_emplace(map_id);
    }
  } catch (const nlohmann::json::exception &) {
    throw std::runtime_error("Malformed Map row for revival index");
  }
  if (by_map_.size() != map_rows.size()) {
    throw std::runtime_error("Duplicate Map id for revival index");
  }

  std::vector<std::filesystem::path> spot_files;
  try {
    for (const auto &entry : std::filesystem::recursive_directory_iterator(maps_root)) {
      if (entry.path().filename() == "spot_object.json" && entry.is_regular_file()) {
        if (spot_files.size() == kMaximumSpotFiles) {
          throw std::runtime_error("Too many revival spot_object.json files");
        }
        spot_files.push_back(entry.path());
      }
    }
  } catch (const std::filesystem::filesystem_error &) {
    throw std::runtime_error("Cannot scan revival raw Maps directory");
  }
  if (spot_files.empty()) {
    throw std::runtime_error("No revival spot_object.json files in raw Maps");
  }
  std::sort(spot_files.begin(), spot_files.end());

  std::size_t objects_seen{};
  std::size_t shrines_seen{};
  std::size_t mapped_spawns{};
  for (std::size_t file_index = 0; file_index < spot_files.size(); ++file_index) {
    const auto &file = spot_files[file_index];
    try {
      const auto document = nlohmann::json::parse(read_spot_file(file));
      const auto &sectors = document.at("sectors");
      if (!sectors.is_array()) {
        throw std::runtime_error("Invalid revival sectors");
      }

      // A dungeon can live under Maps/MazeDungeons/<asset>/DataLayers;
      // other worlds use Maps/<world>/DataLayers. Do not mix dungeons.
      const auto relative = file.lexically_relative(maps_root).generic_string();
      const auto layer = relative.find("/DataLayers/");
      const auto source = layer == std::string::npos
                              ? map_sources.end()
                              : map_sources.find(relative.substr(0, layer));
      const auto add_shrine = [&](const nlohmann::json &shrine, bool flame) {
        if (shrines_seen == kMaximumShrines) {
          throw std::runtime_error("Too many revival shrine spawns");
        }
        ++shrines_seen;
        const auto position = parse_shrine(shrine, flame);

        if (source == map_sources.end()) {
          return;
        }
        for (const auto &map : source->second) {
          if (!map.bounds.contains(position.x_cm, position.y_cm, position.z_cm)) {
            continue;
          }
          if (mapped_spawns == kMaximumMappedSpawns) {
            throw std::runtime_error("Too many mapped revival shrine spawns");
          }
          ++mapped_spawns;
          auto &spawns = by_map_.at(map.id);
          auto &destinations = flame ? spawns.flames : spawns.idols;
          destinations.push_back({map.id, position.x_mm, position.y_mm,
                                  position.z_mm, position.yaw});
        }
      };

      for (const auto &sector : sectors) {
        const auto &objects = sector.at("spotObjects");
        if (!objects.is_array() || objects.size() > kMaximumSpotObjects - objects_seen) {
          throw std::runtime_error("Too many or invalid revival spot objects");
        }
        objects_seen += objects.size();

        for (const auto &object : objects) {
          const auto &types = object.at("types");
          if (!types.is_array()) {
            throw std::runtime_error("Invalid revival spot object types");
          }
          for (const auto &type : types) {
            if (!type.is_object()) {
              throw std::runtime_error("Invalid revival spot object type");
            }
            const auto idol = type.find("adventureObject");
            if (idol != type.end()) {
              if (!idol->is_object()) {
                throw std::runtime_error("Invalid revival adventure object");
              }
              const auto candidate = idol->find("nuiIdol");
              if (candidate != idol->end()) {
                add_shrine(*candidate, false);
              }
            }
            const auto flame = type.find("nuiFlameObject");
            if (flame != type.end()) {
              add_shrine(*flame, true);
            }
          }
        }
      }
    } catch (const nlohmann::json::exception &) {
      throw std::runtime_error("Malformed revival spot_object.json file #" +
                               std::to_string(file_index + 1));
    }
  }
  if (shrines_seen == 0) {
    throw std::runtime_error("No shrine spawn spheres in revival raw Maps");
  }
}

std::optional<WorldPosition> RevivalSpawns::nearest(const WorldPosition &current,
                                                     bool to_nui_flame) const {
  const auto map = by_map_.find(current.map_id);
  if (map == by_map_.end()) {
    return std::nullopt;
  }
  const auto &candidates = to_nui_flame ? map->second.flames : map->second.idols;
  if (candidates.empty()) {
    return std::nullopt;
  }

  const WorldPosition *nearest_spawn = nullptr;
  double best_distance = std::numeric_limits<double>::infinity();
  for (const auto &spawn : candidates) {
    const double x = static_cast<double>(spawn.x_mm) - current.x_mm;
    const double y = static_cast<double>(spawn.y_mm) - current.y_mm;
    const double z = static_cast<double>(spawn.z_mm) - current.z_mm;
    const double distance = x * x + y * y + z * z;
    if (distance < best_distance) {
      nearest_spawn = &spawn;
      best_distance = distance;
    }
  }
  return *nearest_spawn;
}

} // namespace chronicles
