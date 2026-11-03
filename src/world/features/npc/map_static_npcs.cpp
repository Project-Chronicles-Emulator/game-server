#include "world/features/npc/map_static_npcs.hpp"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

constexpr std::uintmax_t kMaximumProfileBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaximumTargets = 100000;
constexpr auto kBuildId = "40367e24241c9481bfc07fbf9ef1919f25fb0eaa";
constexpr std::uint64_t kStaticNpcKind = 7;
constexpr std::uint64_t kLocalBitsMask = 0xFFF;

int integer(const nlohmann::json &value, int minimum) {
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() &&
       value.get<std::uint64_t>() > std::numeric_limits<int>::max())) {
    throw std::runtime_error("Static NPC profile integer is invalid");
  }
  const auto number = value.get<std::int64_t>();
  if (number < minimum || number > std::numeric_limits<int>::max()) {
    throw std::runtime_error("Static NPC profile integer is out of range");
  }
  return static_cast<int>(number);
}

} // namespace

std::optional<MapStaticNpcs> MapStaticNpcs::load(const std::filesystem::path &catalog_root) {
  const auto path = catalog_root.parent_path() / "static-npcs" / "definitions.json";
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  if (std::filesystem::file_size(path) > kMaximumProfileBytes) {
    throw std::runtime_error("Static NPC profile exceeds the size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open static NPC interaction profile");
  }
  return MapStaticNpcs(nlohmann::json::parse(input));
}

MapStaticNpcs::MapStaticNpcs(const nlohmann::json &document) {
  if (document.at("format_version") != 1 || document.at("build_id") != kBuildId ||
      !document.at("targets").is_array() || document.at("targets").size() > kMaximumTargets) {
    throw std::runtime_error("Unsupported static NPC interaction profile");
  }
  for (const auto &row : document.at("targets")) {
    AnnouncedUnit target;
    target.map_id = integer(row.at("map_id"), 1);
    target.npc_template_id = integer(row.at("npc_cms_id"), 1);
    const auto id = parse_unit_id_text(row.at("unit_id").get<std::string>());
    if (!id || (*id & kLocalBitsMask) != kStaticNpcKind ||
        (static_cast<std::uint32_t>(*id) >> 12) == 0 || (*id >> 60) != 0) {
      throw std::runtime_error("Invalid client static NPC unit identity");
    }
    target.unit_id = *id;
    const auto &position = row.at("position_mm");
    if (!position.is_array() || position.size() != 3) {
      throw std::runtime_error("Invalid static NPC placement position");
    }
    target.position = {integer(position.at(0), std::numeric_limits<int>::min()),
                       integer(position.at(1), std::numeric_limits<int>::min()),
                       integer(position.at(2), std::numeric_limits<int>::min())};
    if (!targets_.emplace(std::pair{target.map_id, target.unit_id}, target).second) {
      throw std::runtime_error("Duplicate map-scoped static NPC identity");
    }
  }
}

const AnnouncedUnit *MapStaticNpcs::find(int map_id, std::uint64_t unit_id) const {
  const auto found = targets_.find({map_id, unit_id});
  return found == targets_.end() ? nullptr : &found->second;
}

} // namespace chronicles
