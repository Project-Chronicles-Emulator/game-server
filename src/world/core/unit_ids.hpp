#pragma once

#include <cstdint>
#include <optional>

namespace chronicles {

// Server-owned unit ids for populated NPCs.
//
// Official runtime ids are private and unstable, so none are copied. The low nibble
// encodes the client unit type: NPCs require 2. Allocate one typed slot per placement
// in the fixed guid/template/position sort order. This preserves identity across
// input ordering and reconnects while keeping NPCs in the reserved 32-bit range.
// The full range stays reserved for collision checks, including unused type slots.
// Runtime ids are not persisted.
inline constexpr std::uint64_t kFirstNpcUnitId = 0x80000000ULL;
inline constexpr std::uint64_t kLastNpcUnitId = 0xFFFFFFFFULL;
inline constexpr std::uint64_t kNpcUnitTypeNibble = 2;
inline constexpr std::uint64_t kNpcUnitIdStride = 16;
inline constexpr std::uint64_t kFirstTypedNpcUnitId = kFirstNpcUnitId | kNpcUnitTypeNibble;

// Character ids occupy [1, kLastCharacterUnitId]; server-owned NPC ids occupy
// [kFirstNpcUnitId, kLastNpcUnitId]. The two ranges never overlap.
inline constexpr std::uint64_t kLastCharacterUnitId = kFirstNpcUnitId - 1;

// True when the id lies in the range reserved for server-owned NPC units. World entry must refuse
// a character whose id satisfies this, because the character id is the player's unit id.
bool is_server_npc_unit_id(std::uint64_t unit_id);

// The client types every unit by the low nibble of its 64-bit id: 1 = player, 2 = NPC (confirmed
// statically; official local-player ids are 0x1001, 0x2001, ...). A player's WIRE unit id is
// therefore (character_id << 4) | 1, which is injective, always has nibble 1, never collides with
// NPC ids (nibble 2) and stays below kFirstNpcUnitId for character ids up to kMaxCharacterId.
// Persistence, instance keys and database rows keep the raw character id.
inline constexpr std::int64_t kMaxCharacterId = 0x07FFFFFF;
inline constexpr std::uint64_t kPlayerUnitTypeNibble = 1;

inline bool is_supported_character_id(std::int64_t character_id) {
  return character_id > 0 && character_id <= kMaxCharacterId;
}

inline std::uint64_t player_wire_unit_id(std::int64_t character_id) {
  return (static_cast<std::uint64_t>(character_id) << 4) | kPlayerUnitTypeNibble;
}

// The character id behind a player wire unit id, or nothing when the id is not a player's.
inline std::optional<std::int64_t> character_id_from_wire_unit_id(std::uint64_t unit_id) {
  if ((unit_id & 0xF) != kPlayerUnitTypeNibble || unit_id > kLastCharacterUnitId) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(unit_id >> 4);
}

} // namespace chronicles
