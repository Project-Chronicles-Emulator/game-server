#pragma once

#include "protocol/gameplay_packet.hpp"

#include <cstdint>
#include <string>

namespace chronicles {

struct WorldPosition;

namespace combat {

// Packet policy only. Handshake keeps the original per-packet construction/encode/queue
// points, response flags and cached CharacterBootstrap life state.
GameplayPacket entry_weapon_gauge();
GameplayPacket entry_unit_attribute(std::uint32_t unit_id, int observed_value);
GameplayPacket entry_unit_traits(std::uint32_t unit_id);
GameplayPacket saved_player_demise(const std::string &unit_id, std::int64_t internal_tick);
GameplayPacket saved_player_death(const std::string &unit_id, const WorldPosition &position);

} // namespace combat
} // namespace chronicles
