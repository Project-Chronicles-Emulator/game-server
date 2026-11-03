#include "world/core/unit_ids.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

TEST(PlayerWireUnitId, EncodesCharacterIdsWithThePlayerTypeNibbleAndRoundTrips) {
  EXPECT_EQ(player_wire_unit_id(1), 0x11u);
  EXPECT_EQ(player_wire_unit_id(17), 0x111u);
  EXPECT_EQ(player_wire_unit_id(19), 0x131u);
  for (const std::int64_t character_id : {1, 2, 17, 19, 4096, 0x07FFFFFF}) {
    const auto unit_id = player_wire_unit_id(character_id);
    EXPECT_EQ(unit_id & 0xF, 1u) << character_id;
    EXPECT_LE(unit_id, kLastCharacterUnitId) << character_id;
    // Our NPC wire ids carry bit 61 (0x2000...); a player's wire id never does.
    EXPECT_EQ(unit_id & (1ULL << 61), 0u) << character_id;
    // The client rebuilds the local id as (id & 0xFFFFFFF0) | 1 from a uint32: ours is a fixed
    // point of that transform and fits in 32 bits.
    EXPECT_LT(unit_id, 1ULL << 32) << character_id;
    EXPECT_EQ((unit_id & 0xFFFFFFF0ULL) | 1ULL, unit_id) << character_id;
    EXPECT_FALSE(is_server_npc_unit_id(unit_id)) << character_id;
    ASSERT_TRUE(character_id_from_wire_unit_id(unit_id).has_value()) << character_id;
    EXPECT_EQ(*character_id_from_wire_unit_id(unit_id), character_id);
  }
}

TEST(PlayerWireUnitId, RejectsWrongTypeNibblesNpcRangeAndOutOfRangeCharacterIds) {
  // Raw database ids are not wire ids: 19 has nibble 3 and 2 is the NPC type.
  EXPECT_FALSE(character_id_from_wire_unit_id(19).has_value());
  EXPECT_FALSE(character_id_from_wire_unit_id(0x132).has_value());
  EXPECT_FALSE(character_id_from_wire_unit_id(0x4).has_value());
  EXPECT_FALSE(character_id_from_wire_unit_id(kFirstNpcUnitId + 1).has_value());
  EXPECT_FALSE(character_id_from_wire_unit_id(0x2000000000000001ULL).has_value());

  EXPECT_FALSE(is_supported_character_id(0));
  EXPECT_FALSE(is_supported_character_id(-1));
  EXPECT_TRUE(is_supported_character_id(1));
  EXPECT_TRUE(is_supported_character_id(kMaxCharacterId));
  EXPECT_FALSE(is_supported_character_id(kMaxCharacterId + 1));

  // Distinct character ids never share a wire id.
  EXPECT_NE(player_wire_unit_id(1), player_wire_unit_id(17));
  EXPECT_NE(player_wire_unit_id(17), player_wire_unit_id(19));
}

} // namespace
} // namespace chronicles
