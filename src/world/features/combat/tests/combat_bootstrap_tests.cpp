#include "world/features/combat/combat_bootstrap.hpp"

#include "persistence/character_state_types.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

namespace chronicles::combat {
namespace {

TEST(CombatEntryBootstrap, AllExistingPresentationFactoriesEncodeThroughTheRecoveredSchemas) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const WorldPosition position{9012, -100, 200, 300, 0};
  const std::vector<GameplayPacket> packets{entry_weapon_gauge(),
                                            entry_weapon_gauge(),
                                            entry_unit_attribute(305, 1),
                                            entry_unit_traits(305),
                                            entry_unit_attribute(305, 2),
                                            entry_unit_attribute(305, 1),
                                            saved_player_demise("305", 100000000),
                                            saved_player_death("305", position)};
  const std::vector<std::uint16_t> expected{78, 78, 33, 115, 33, 33, 32, 23};
  for (std::size_t index = 0; index < packets.size(); ++index) {
    EXPECT_EQ(packets[index].opcode, expected[index]);
    const auto decoded =
        protocol.decode(protocol.encode(packets[index].opcode, packets[index].message));
    EXPECT_EQ(decoded.at("status"), "decoded") << packets[index].opcode;
  }
  EXPECT_EQ(packets[6].message.at("tick"), "10000000000");
  EXPECT_EQ(packets[7].message.at("pos").at("millimeter_x"), -100);
}

} // namespace
} // namespace chronicles::combat
