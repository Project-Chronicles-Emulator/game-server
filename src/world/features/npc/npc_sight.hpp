#pragma once

#include "protocol/gameplay_packet.hpp"
#include "world/core/unit_registry.hpp"
#include "world/features/npc/npc_reactions.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace chronicles {

class NpcPopulation;

// Server-owned NPCs are announced by sight, as the official world server does.
//
// Evidence (local official captures, build 40367e24): on arrival in a Shining Shore channel the
// official server sent only the NPCs within about 123 m of the player (24 of them), and a longer
// shared-map session paired 328 NPC UnitAddNoti with 328 UnitRemoveNoti as the player moved.
// Announcing all 261 map-9012 placements at map entry left distant NPCs missing in the client.
//
// The radii are a local Candidate policy, not recovered official values: the enter radius covers
// the largest observed arrival distance and the wider leave radius keeps a unit on the boundary
// from being removed and re-added on every step. Distances are horizontal because placement
// heights are only approximate (z_candidate in the placement file).
inline constexpr std::int64_t kNpcSightEnterRadiusMm = 130000;
inline constexpr std::int64_t kNpcSightLeaveRadiusMm = 150000;

// PS2C_UnitRemoveNoti, which carries the unit ids leaving the client's view.
inline constexpr std::uint16_t kNpcSightUnitRemovedOpcode = 11;

// Per-session record of which population units the client currently knows. Confined to the
// session worker strand, like the session's UnitRegistry.
class NpcSightTracker {
public:
  // Brings the client's view up to date for a viewer at `viewer`, with every unit at its shared
  // timeline pose after elapsed_ticks. Returns one UnitAddNoti per entering unit (population id
  // order), then a single UnitRemoveNoti listing the leaving units, or nothing when unchanged.
  std::vector<GameplayPacket> update(const NpcPopulation &population, const UnitPositionMm &viewer,
                                     std::int64_t elapsed_ticks,
                                     const npc::ReactionTimeline &timeline = {},
                                     std::int64_t epoch_ticks = 0);

  bool visible(std::uint64_t unit_id) const;

  std::size_t visible_count() const;

  // Drops packets addressed (message "unit_id") to a server NPC the client does not currently
  // know, so timeline updates for distant units never reach the client. Other packets pass.
  std::vector<GameplayPacket> visible_only(std::vector<GameplayPacket> packets) const;

  // Forgets every unit, for a map change: the client discards the old map's units itself.
  void clear();

private:
  std::unordered_set<std::uint64_t> visible_;
};

} // namespace chronicles
