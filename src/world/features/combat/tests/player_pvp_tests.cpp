#include "world/features/combat/player_pvp.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

const WorldPosition kSharedPosition{9001, 100, 200, 300, 0.0F};

TEST(PlayerPvp, ToggleUsesRecoveredFactionAndRejectsDeadOrBusyChanges) {
  PlayerPvpRegistry registry;
  ASSERT_TRUE(registry.enter(1, 11, kSharedPosition));
  EXPECT_FALSE(registry.enabled(1));
  EXPECT_EQ(registry.set_enabled(1, true, false).status, PlayerPvpStatus::player_dead);
  EXPECT_EQ(registry.set_enabled(1, true, true, true).status, PlayerPvpStatus::player_in_combat);
  EXPECT_FALSE(registry.enabled(1));

  const auto enabled = registry.set_enabled(1, true);
  ASSERT_EQ(enabled.status, PlayerPvpStatus::changed);
  ASSERT_TRUE(enabled.notification.has_value());
  EXPECT_EQ(enabled.notification->opcode, 82);
  EXPECT_EQ(enabled.notification->message.at("unit_id"), "11");
  EXPECT_EQ(enabled.notification->message.at("type"), "EX5Faction_PvP_MODE");
  EXPECT_TRUE(enabled.enabled);
  EXPECT_TRUE(registry.enabled(1));
  EXPECT_FALSE(registry.set_enabled(1, true).notification.has_value());

  const auto disabled = registry.set_enabled(1, false);
  ASSERT_TRUE(disabled.notification.has_value());
  EXPECT_EQ(disabled.notification->message.at("type"), "EX5Faction_Player");
  EXPECT_FALSE(registry.enabled(1));
}

TEST(PlayerPvp, BothPlayersMustOptInAndShareMapAndInstance) {
  PlayerPvpRegistry registry;
  ASSERT_TRUE(registry.enter(1, 11, kSharedPosition));
  ASSERT_TRUE(registry.enter(2, 22, kSharedPosition));
  EXPECT_FALSE(registry.can_attack(1, 2));
  registry.set_enabled(1, true);
  EXPECT_FALSE(registry.can_attack(1, 2));
  registry.set_enabled(2, true);
  EXPECT_TRUE(registry.can_attack(1, 2));
  EXPECT_TRUE(registry.can_attack(2, 1));
  EXPECT_FALSE(registry.can_attack(1, 1));
  EXPECT_FALSE(registry.can_attack(1, 9));

  const auto changed_instance = registry.update_position(2, kSharedPosition, 22);
  EXPECT_EQ(changed_instance.status, PlayerPvpStatus::changed);
  EXPECT_FALSE(changed_instance.enabled);
  registry.set_enabled(2, true);
  EXPECT_FALSE(registry.can_attack(1, 2));

  auto other_map = kSharedPosition;
  ++other_map.map_id;
  registry.update_position(2, other_map);
  registry.set_enabled(2, true);
  EXPECT_FALSE(registry.can_attack(1, 2));
}

TEST(PlayerPvp, MovementPreservesFlagButMapChangeAndReconnectResetIt) {
  PlayerPvpRegistry registry;
  ASSERT_TRUE(registry.enter(1, 11, kSharedPosition));
  registry.set_enabled(1, true);
  auto moved = kSharedPosition;
  moved.x_mm += 1000;
  const auto ordinary_move = registry.update_position(1, moved);
  EXPECT_EQ(ordinary_move.status, PlayerPvpStatus::unchanged);
  EXPECT_TRUE(ordinary_move.enabled);
  EXPECT_FALSE(ordinary_move.notification.has_value());

  ++moved.map_id;
  const auto map_move = registry.update_position(1, moved);
  ASSERT_TRUE(map_move.notification.has_value());
  EXPECT_EQ(map_move.notification->message.at("type"), "EX5Faction_Player");
  EXPECT_FALSE(registry.enabled(1));
  EXPECT_FALSE(registry.update_position(1, moved).notification.has_value());

  registry.set_enabled(1, true);
  registry.leave(1);
  EXPECT_FALSE(registry.enabled(1));
  EXPECT_EQ(registry.set_enabled(1, true).status, PlayerPvpStatus::unknown_player);
  ASSERT_TRUE(registry.enter(2, 11, moved));
  EXPECT_FALSE(registry.enabled(2));
}

TEST(PlayerPvp, RegistrationRefusesDuplicateOrInvalidIdentityWithoutChangingExistingFlag) {
  PlayerPvpRegistry registry;
  EXPECT_FALSE(registry.enter(0, 11, kSharedPosition));
  EXPECT_FALSE(registry.enter(1, 0, kSharedPosition));
  EXPECT_FALSE(registry.enter(1, 11, WorldPosition{}));
  ASSERT_TRUE(registry.enter(1, 11, kSharedPosition));
  registry.set_enabled(1, true);
  EXPECT_FALSE(registry.enter(1, 22, kSharedPosition));
  EXPECT_FALSE(registry.enter(2, 11, kSharedPosition));
  EXPECT_TRUE(registry.enabled(1));
  EXPECT_FALSE(registry.enabled(2));
  EXPECT_EQ(registry.set_enabled(2, true).status, PlayerPvpStatus::unknown_player);
}

TEST(PlayerPvp, LateObserverSnapshotUsesCurrentFactionAndPreservesOtherUnits) {
  PlayerPvpRegistry registry;
  ASSERT_TRUE(registry.enter(1, 11, kSharedPosition));
  registry.set_enabled(1, true);

  nlohmann::json message{{"unit_id", "11"}, {"nub", {{"player", {{"name", "fixture"}}}}}};
  registry.decorate_unit_add(message);
  const auto &player = message.at("nub").at("player");
  EXPECT_EQ(player.at("faction_type"), "EX5Faction_PvP_MODE");
  EXPECT_FALSE(player.at("pk_channel_activated").get<bool>());
  EXPECT_EQ(player.at("name"), "fixture");
  registry.set_enabled(1, false);
  registry.decorate_unit_add(message);
  EXPECT_EQ(player.at("faction_type"), "EX5Faction_Player");

  for (const auto &unit_id : {"22", "-1", "18446744073709551616", "11x"}) {
    nlohmann::json unknown{{"unit_id", unit_id},
                           {"nub", {{"player", {{"faction_type", "EX5Faction_PvP_RED"}}}}}};
    const auto before = unknown;
    registry.decorate_unit_add(unknown);
    EXPECT_EQ(unknown, before);
  }

  nlohmann::json npc{{"unit_id", "11"},
                     {"nub", {{"npc", {{"faction_type", "EX5Faction_Bandit"}}}}}};
  const auto before = npc;
  registry.decorate_unit_add(npc);
  EXPECT_EQ(npc, before);
}

TEST(PlayerPvp, DirectSnapshotDecorationAndMissingPlayersFailClosed) {
  PlayerPvpRegistry registry;
  ASSERT_TRUE(registry.enter(1, 11, kSharedPosition));
  nlohmann::json player = nlohmann::json::object();
  registry.decorate_player_nub(1, player);
  EXPECT_EQ(player.at("faction_type"), "EX5Faction_Player");
  EXPECT_FALSE(player.at("pk_channel_activated").get<bool>());

  nlohmann::json unknown{{"custom", 7}};
  registry.decorate_player_nub(8, unknown);
  EXPECT_EQ(unknown, nlohmann::json({{"custom", 7}}));
  nlohmann::json malformed = nlohmann::json::array();
  registry.decorate_player_nub(1, malformed);
  EXPECT_TRUE(malformed.is_array());
}

TEST(PlayerPvp, FlagAndLateObserverSnapshotsEncodeUsingRecoveredNativeSchemas) {
  PlayerPvpRegistry registry;
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  ASSERT_TRUE(registry.enter(1, 11, kSharedPosition));
  const auto enabled = registry.set_enabled(1, true);
  ASSERT_TRUE(enabled.notification.has_value());
  const auto notification =
      protocol.decode(protocol.encode(enabled.notification->opcode, enabled.notification->message));
  EXPECT_EQ(notification.at("packet"), "PS2C_ChangeFactionNoti");
  EXPECT_EQ(notification.at("message").at("unit_id"), "11");
  EXPECT_EQ(notification.at("message").at("type"), "EX5Faction_PvP_MODE");

  nlohmann::json unit_add{{"unit_id", "11"},
                          {"nub", {{"player", {{"char_id", "11"}, {"name", "fixture"}}}}}};
  registry.decorate_unit_add(unit_add);
  const auto decoded_add = protocol.decode(protocol.encode(10, unit_add));
  EXPECT_EQ(decoded_add.at("packet"), "PS2C_UnitAddNoti");
  EXPECT_EQ(decoded_add.at("message").at("nub").at("player").at("faction_type"),
            "EX5Faction_PvP_MODE");
}

} // namespace
} // namespace chronicles
