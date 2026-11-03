#pragma once

#include "world/features/tutorial/tutorial.hpp"
#include "world/core/unit_registry.hpp"
#include "world/core/unit_ids.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chronicles {

class NpcPatrolRoute;

// Upper bound on NPC units announced to one session at once. A server memory/announce policy (map
// 9012 has 261 active server placements), not a game rule; the session registry must hold it.
inline constexpr std::size_t kMaxPopulatedNpcUnits = 1024;

// PS2C_UnitAddNoti, the same opcode TutorialSession::sectors() uses for NPC announcements.
inline constexpr std::uint16_t kUnitAddedOpcode = 10;

// One server-owned civilian placement, optionally carrying an authored patrol. Coordinates are
// millimeters as announced to the client (catalog centimeters x10); yaw is degrees.
// One Interaction row offered by the NPC's interaction group. Carried for the interaction index;
// this module does not evaluate it.
struct NpcInteraction {
  int interaction_id{};
  std::string task_case;
  std::optional<int> shop_id;
};

struct NpcPlacement {
  int npc_cms_id{};
  int x_mm{};
  int y_mm{};
  int z_mm{};
  float yaw{};
  // Full wire name, "EX5Faction_" + the placement file's faction.
  std::string faction;
  std::string behavior;
  // Placement guid as the two wire halves (spawner_id_x / spawner_id_y).
  std::optional<std::pair<std::uint64_t, std::uint64_t>> spawner_guid;
  // The placement data has no stance key; it is announced only when the data supplies one.
  std::optional<int> anim_stance_key;
  int interaction_group_id{};
  std::vector<NpcInteraction> interactions;
  // NpcSpawner.activationState as passed through by the placement file. Its meaning is Unknown;
  // NpcPopulation announces only active placements, like the tutorial's initially_active.
  bool active{true};
  std::shared_ptr<const NpcPatrolRoute> patrol;
};

struct NpcPlacementSet {
  int map_id{};
  // Layers the export lacks for this map; a non-zero count means the placements are partial.
  std::size_t missing_layers{};
  std::vector<NpcPlacement> placements;

  // Parses the FULL stationary placement file only (tools/build_stationary_npc_placements.py,
  // schema in out/agent-team-reports/npc-runtime/placements/schema.md): format_version 1, map_id,
  // optional layers_missing, placements[] with position_mm, yaw_degrees, spawner_guid_wire.
  // The lean runtime document is not accepted. Every malformed field throws std::runtime_error
  // naming the field; more than max_placements entries is rejected. Unrecognized members are
  // ignored.
  static NpcPlacementSet from_json(const nlohmann::json &document, std::size_t max_placements);
};

struct PopulatedNpc {
  std::uint64_t unit_id{};
  NpcPlacement placement;
};

struct PopulationAnnounceResult {
  std::size_t added{};
  // Units not recorded: ids already present in the registry (counted again in `collisions`),
  // invalid announcements, or a full registry. A non-zero value means the registry cannot
  // authorize those units and the caller must fail closed.
  std::size_t rejected{};
  // Ids that were already present in the registry when announce_to ran. The existing entry is
  // never replaced or modified.
  std::size_t collisions{};
};

// Immutable NPC definitions for one map, announced at their shared timeline's current pose.
class NpcPopulation {
public:
  // max_units has no default: the caller states its own announce bound.
  // Throws std::invalid_argument if max_units is zero, the placement count exceeds it or the
  // id range, or the map id is not positive.
  NpcPopulation(NpcPlacementSet placement_set, std::size_t max_units);

  int map_id() const;
  // Active placements only, in id order.
  const std::vector<PopulatedNpc> &units() const;
  // Placements left out because the data marks them inactive.
  std::size_t skipped_inactive() const;

  // One PS2C_UnitAddNoti per NPC with the fields the tutorial path sends.
  std::vector<GameplayPacket> unit_added_packets(std::int64_t elapsed_ticks = 0) const;

  // The PS2C_UnitAddNoti for one unit at its shared timeline's pose after elapsed_ticks.
  static GameplayPacket unit_added_packet(const PopulatedNpc &unit, std::int64_t elapsed_ticks);

  // Where one unit stands after elapsed_ticks: its placement, or its patrol pose.
  static UnitPositionMm unit_position(const PopulatedNpc &unit, std::int64_t elapsed_ticks);

  // Feeds the session's registry. Call before queueing unit_added_packets() so an announced
  // unit is never visible to the client without being resolvable by the server. An id that is
  // already present is refused and counted, regardless of which unit owns it.
  PopulationAnnounceResult announce_to(UnitRegistry &registry, std::string_view instance_key,
                                       std::int64_t elapsed_ticks = 0) const;

  bool has_patrols() const;

  // Removes this population's units from the registry; returns how many were present.
  std::size_t despawn_from(UnitRegistry &registry) const;

private:
  int map_id_{};
  std::size_t skipped_inactive_{};
  std::vector<PopulatedNpc> units_;
};

// Reads, parses and validates one full placement file and builds its population. Throws
// std::runtime_error on an unreadable file, invalid JSON or malformed placements and
// std::invalid_argument when the bound is exceeded.
std::shared_ptr<const NpcPopulation> load_npc_population_file(const std::filesystem::path &path,
                                                              std::size_t max_units);

} // namespace chronicles
