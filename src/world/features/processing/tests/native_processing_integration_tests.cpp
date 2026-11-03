#include "world/features/processing/processing.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;

// The live-trial station: doodad 20251 at sector 4767898, local spot 1, unit kind 6.
constexpr std::uint64_t kOverworldStationUnitId = 20477965980667910ULL;
constexpr int kOverworldStationDoodad = 20251;
// The live trial opened the station from 1.24 m away.
constexpr int kOverworldStationOffsetMm = 1240;

// One captured cooking station placed next to the overworld spawn.
std::shared_ptr<const ProcessingData> overworld_processing_profile() {
  constexpr int kCookingInteraction = 10220;
  constexpr int kCookingAnimation = 60001;
  constexpr int kAreaHalfExtentMm = 100000;

  const nlohmann::json area_min{kOverworldSpawn.x_mm - kAreaHalfExtentMm,
                                kOverworldSpawn.y_mm - kAreaHalfExtentMm,
                                kOverworldSpawn.z_mm - kAreaHalfExtentMm};
  const nlohmann::json area_max{kOverworldSpawn.x_mm + kAreaHalfExtentMm,
                                kOverworldSpawn.y_mm + kAreaHalfExtentMm,
                                kOverworldSpawn.z_mm + kAreaHalfExtentMm};
  const nlohmann::json cooking_recipe{
      {"item_craft_id", 60190101},
      {"product_item_cms_id", 40101101},
      {"product_count", 5},
      {"product_storage", "stackable"},
      {"gold_cost", 125},
      {"materials", nlohmann::json::array({{{"item_cms_id", 70102001}, {"count", 9}}})}};
  const nlohmann::json placement{std::to_string(kOverworldStationUnitId),
                                 kOverworldStationDoodad,
                                 kOverworldSpawn.x_mm + kOverworldStationOffsetMm,
                                 kOverworldSpawn.y_mm,
                                 kOverworldSpawn.z_mm};

  const nlohmann::json profile{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"maximum_craft_amount", 999},
      {"maps", nlohmann::json::array({{{"map_id", kOverworldMapId},
                                       {"area_min_mm", area_min},
                                       {"area_max_mm", area_max}}})},
      {"animations", nlohmann::json::array({{{"interaction_anim_id", kCookingAnimation},
                                             {"execute_time_ms", 2933},
                                             {"duration_ms", 3000}}})},
      {"interactions", nlohmann::json::array({{{"interaction_id", kCookingInteraction},
                                               {"group_id", kCookingInteraction},
                                               {"interaction_anim_id", kCookingAnimation}}})},
      {"stations", nlohmann::json::array({{{"doodad_cms_id", kOverworldStationDoodad},
                                           {"interaction_group_id", kCookingInteraction},
                                           {"craft_ids", {60190101}}}})},
      {"crafts", nlohmann::json::array({cooking_recipe})},
      {"unsupported_crafts", nlohmann::json::array()},
      {"placements", nlohmann::json::array({placement})}};
  return std::make_shared<const ProcessingData>(profile);
}

// Live trial 2026-10-27: after DialogueEndReq on a crafting station the server's DialogueEndNoti
// named the raw character ID, so the original client never released its own character from the
// station and the character could not move. Official captures name the player's wire unit.
// Opcodes: 20065/20066 DialogueStart/EndReq, 161 DialogueStartRes, 162 DialogueEndNoti.
TEST(NativeWorld, CraftingStationDialogueEndReleasesThePlayersOwnWireUnit) {
  const std::vector<int> dialogue_opcodes{161, 162};

  OverworldSessions world(small_overworld_population(), {kOverworldSpawn}, {}, {}, {}, {}, {}, {},
                          overworld_processing_profile());
  auto &session = world.sessions.front();
  const auto entry = world.enter(session);
  const auto player_unit_id = player_wire_unit_id(session.character_id);
  ASSERT_NE(player_unit_id, static_cast<std::uint64_t>(session.character_id));
  ASSERT_EQ(entry.map_complete.at("player_unit_id"), player_unit_id);
  std::uint32_t sequence = 3;

  const nlohmann::json request_position{{"millimeter_x", kOverworldSpawn.x_mm},
                                        {"millimeter_y", kOverworldSpawn.y_mm},
                                        {"millimeter_z", kOverworldSpawn.z_mm}};
  const nlohmann::json dialogue_start{{"target_unit_id", std::to_string(kOverworldStationUnitId)},
                                      {"pos", request_position},
                                      {"yaw", -7.5}};
  session.client->send(world.protocol.encode(20065, dialogue_start, sequence++));
  auto frames = world.read_until_pong(session, sequence++);
  const auto opened = decoded_frames(world.protocol, frames, dialogue_opcodes);
  ASSERT_EQ(opened.size(), 1);
  EXPECT_EQ(opened.at(0).at("opcode"), 161);
  EXPECT_FALSE(opened.at(0).at("message").contains("ec"));
  EXPECT_EQ(opened.at(0).at("message").at("target_unit_id"),
            std::to_string(kOverworldStationUnitId));

  session.client->send(world.protocol.encode(20066, nlohmann::json::object(), sequence++));
  frames = world.read_until_pong(session, sequence++);
  const auto closed = decoded_frames(world.protocol, frames, dialogue_opcodes);
  ASSERT_EQ(closed.size(), 1);
  EXPECT_EQ(closed.at(0).at("opcode"), 162);
  EXPECT_EQ(closed.at(0).at("message").at("player_unit_id"), player_unit_id);

  session.client->disconnect();
}

} // namespace
} // namespace chronicles
