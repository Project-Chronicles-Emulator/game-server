#pragma once

#include "persistence/character_state_types.hpp"

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles {

// Startup-only index of the matching raw export. Nearest-shrine selection is a
// local candidate policy, not a recovered official activation/range rule.
class RevivalSpawns {
public:
  RevivalSpawns(const std::filesystem::path &catalog_root,
                const std::vector<nlohmann::json> &map_rows);

  // No cross-map or cross-type fallback. Missing shrines return std::nullopt.
  std::optional<WorldPosition> nearest(const WorldPosition &current,
                                       bool to_nui_flame) const;

private:
  struct MapSpawns {
    std::vector<WorldPosition> idols;
    std::vector<WorldPosition> flames;
  };

  std::unordered_map<int, MapSpawns> by_map_;
};

} // namespace chronicles
