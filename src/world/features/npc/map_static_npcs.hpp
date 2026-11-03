#pragma once

#include "world/core/unit_registry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <utility>

namespace chronicles {

// Authored client-owned NPCs are interaction targets, never additional UnitAddNoti actors.
// Identity evidence: docs/npc-duplicate-spawns.md (sector, local static ID, unit kind 7).
class MapStaticNpcs {
public:
  static std::optional<MapStaticNpcs> load(const std::filesystem::path &catalog_root);

  explicit MapStaticNpcs(const nlohmann::json &document);

  // Returns only an exact authored identity on this map. Position/template come from the
  // verified export, never from the request. The caller still enforces interaction/range rules.
  const AnnouncedUnit *find(int map_id, std::uint64_t unit_id) const;

  std::size_t size() const noexcept {
    return targets_.size();
  }

private:
  std::map<std::pair<int, std::uint64_t>, AnnouncedUnit> targets_;
};

} // namespace chronicles
