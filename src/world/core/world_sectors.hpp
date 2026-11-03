#pragma once

#include "protocol/gameplay_packet.hpp"

#include <compare>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace chronicles {

class ClientCatalog;

// PS2C_SectorUpdateNoti (p_s2c.sector.proto), build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa.
inline constexpr std::uint16_t kSectorUpdateNotification = 193;

// Sector IDs pack two 14-bit one-based axis indices as x | (y << 14).
inline constexpr std::int32_t kSectorAxisCount = 1 << 14;

// The official server announced a 5 x 5 block around the player's sector: two sectors on each
// side. The client creates a sector record only within two sectors of the player
// (AAC_Sector_Candidate_CreateRecordIfNearPlayer, RVA 0x5F54B70), so a wider block is ignored.
inline constexpr std::int32_t kSectorSightRadius = 2;

struct SectorCoordinate {
  std::int32_t x{};
  std::int32_t y{};

  auto operator<=>(const SectorCoordinate &) const = default;
};

std::uint32_t pack_sector_id(SectorCoordinate sector);

SectorCoordinate unpack_sector_id(std::uint32_t sector_id);

// Per-map sector geometry from the client catalog. A world's sector index is
// floor((position_cm - OriginMap.area.min) / OriginMap.sectorWidth) + 1 on each axis. For
// ArcheWorld this reproduces the authored sectorIndex of all 4,502 exported spot placements.
class WorldSectorGrid {
public:
  // Reads the OriginMap and Map tables. Returns nothing when either table is absent.
  static std::optional<WorldSectorGrid> load(ClientCatalog &catalog);

  WorldSectorGrid(const std::vector<nlohmann::json> &origin_rows,
                  const std::vector<nlohmann::json> &map_rows);

  bool has_map(int map_id) const;

  // The sector containing a world position in millimetres, or nothing for an unknown map or a
  // position outside the 14-bit sector range.
  std::optional<SectorCoordinate> sector_at(int map_id, double x_mm, double y_mm) const;

  // Sector IDs within kSectorSightRadius of `center`, limited to the map's sector range and
  // ordered by y, then x, as in the official map-entry bursts.
  std::vector<std::uint32_t> sight_window(int map_id, SectorCoordinate center) const;

  std::size_t map_count() const;

private:
  struct WorldOrigin {
    double min_x_cm{};
    double min_y_cm{};
    double sector_width_cm{};
  };

  struct MapGrid {
    WorldOrigin origin;
    SectorCoordinate first;
    SectorCoordinate last;
  };

  static SectorCoordinate index_of(const WorldOrigin &origin, double x_cm, double y_cm);

  std::unordered_map<int, MapGrid> maps_;
};

// The sectors one session has been told about on its current map. Updates are produced only
// after arm(), which the session calls on MapLoadCompleteReq: earlier announcements would be
// dropped by the client while its player still stands on the previous map.
class SectorSightTracker {
public:
  // Starts a new map visit; the next update announces the whole window.
  void arm(int map_id);

  void clear();

  // Sector IDs that entered the window since the last update, in window order. A sector that
  // left the window is announced again when it re-enters, as the official server did.
  std::vector<std::uint32_t> update(const WorldSectorGrid &grid, int map_id, double x_mm,
                                    double y_mm);

  const std::set<std::uint32_t> &visible() const noexcept {
    return visible_;
  }

private:
  std::optional<int> armed_map_;
  std::optional<SectorCoordinate> center_;
  std::set<std::uint32_t> visible_;
};

// One SectorUpdateNoti. Shared-world default-phase doodads are never listed by the official
// server, so only the sector's depleted gathering indices are reported.
GameplayPacket sector_update_packet(std::uint32_t sector_id,
                                    const std::vector<std::uint32_t> &deactivated_gathering);

} // namespace chronicles
