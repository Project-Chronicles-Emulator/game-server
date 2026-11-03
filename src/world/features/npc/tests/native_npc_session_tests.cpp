#include "world/features/npc/npc_population.hpp"
#include "world/features/npc/npc_sight.hpp"
#include "world/session/tests/native_map_entry_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <set>
#include <thread>

namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace native_testing;

// Stationary population units inside the NPC enter radius of `viewer`: exactly the stationary units
// a player standing there must be told about at map entry. Patrol units are left out because their
// pose depends on the moment of entry.
std::set<std::uint64_t> stationary_units_in_entry_sight(const NpcPopulation &population,
                                                        const WorldPosition &viewer) {
  std::set<std::uint64_t> ids;
  for (const auto &unit : population.units()) {
    if (unit.placement.patrol) {
      continue;
    }
    const auto delta_x = static_cast<std::int64_t>(unit.placement.x_mm) - viewer.x_mm;
    const auto delta_y = static_cast<std::int64_t>(unit.placement.y_mm) - viewer.y_mm;
    if (delta_x * delta_x + delta_y * delta_y <=
        kNpcSightEnterRadiusMm * kNpcSightEnterRadiusMm) {
      ids.insert(unit.unit_id);
    }
  }
  return ids;
}

TEST(NativeWorld, CivilianPatrolAdvancesWithoutPlayerInputAndLateJoinersShareItsClock) {
  auto placement = overworld_placement_json(1001121, 1, true);
  placement["position_mm"] = {
      {"x", kOverworldSpawn.x_mm}, {"y", kOverworldSpawn.y_mm}, {"z", kOverworldSpawn.z_mm}};
  const auto node = [](int x_mm) {
    return nlohmann::json{
        {"position_mm", {{"x", x_mm}, {"y", kOverworldSpawn.y_mm}, {"z", kOverworldSpawn.z_mm}}},
        {"speed_mm_per_second", 1000},
        {"move_detail", "MoveDetail_Walk"},
        {"wait_ms", 0}};
  };
  placement["patrol"] = {
      {"mode", "circle_forward"},
      {"nodes", {node(kOverworldSpawn.x_mm), node(kOverworldSpawn.x_mm + 100000)}}};
  const nlohmann::json document{
      {"format_version", 1}, {"map_id", kOverworldMapId}, {"placements", {placement}}};
  auto population = std::make_shared<const NpcPopulation>(
      NpcPlacementSet::from_json(document, kTestPlacementBound), kTestPlacementBound);
  OverworldSessions world(population, {kOverworldSpawn, kOverworldSpawn});
  auto &first = world.sessions[0];
  auto &second = world.sessions[1];
  const auto first_entry = world.enter(first);
  ASSERT_EQ(first_entry.unit_adds.size(), 1);

  const auto start = [&](OverworldSessions::Session &session) {
    session.client->send(world.protocol.encode(20086, nlohmann::json::object(), 3));
    for (int index = 0; index < 32; ++index) {
      if (session.client->read().opcode() == 183) {
        return;
      }
    }
    FAIL() << "StartPlayingRes was not received";
  };
  const auto read_move = [&](OverworldSessions::Session &session) {
    for (int index = 0; index < 32; ++index) {
      const auto frame = session.client->read();
      if (frame.opcode() == 177) {
        return world.protocol.decode(frame).at("message");
      }
    }
    throw std::runtime_error("Civilian patrol MoveNoti was not received");
  };
  start(first);
  const auto initial = read_move(first);
  // No further client packet is sent before this timer-driven notification.
  const auto continued = read_move(first);
  EXPECT_EQ(continued.at("detail"), "MoveDetail_Walk");
  EXPECT_GT(continued.at("pos").at("millimeter_x").get<int>(),
            initial.at("pos").at("millimeter_x").get<int>());
  EXPECT_TRUE(continued.contains("pred_pos"));
  EXPECT_GE(continued.at("pred_milli").get<int>(), 16);

  const auto second_entry = world.enter(second);
  const auto npc_id = first_entry.unit_adds[0].at("unit_id");
  const auto joined_npc = std::ranges::find_if(
      second_entry.unit_adds, [&](const auto &unit) { return unit.at("unit_id") == npc_id; });
  ASSERT_NE(joined_npc, second_entry.unit_adds.end());
  EXPECT_EQ(std::ranges::count_if(second_entry.unit_adds,
                                  [&](const auto &unit) { return unit.at("unit_id") == npc_id; }),
            1);
  start(second);
  const auto joined = read_move(second);
  const auto first_tick = std::stoll(continued.at("tick").get<std::string>());
  const auto second_tick = std::stoll(joined.at("tick").get<std::string>());
  const auto expected_delta_mm = static_cast<double>(second_tick - first_tick) / 1000000000 * 1000;
  const auto observed_delta_mm = joined.at("pos").at("millimeter_x").get<int>() -
                                 continued.at("pos").at("millimeter_x").get<int>();
  EXPECT_NEAR(observed_delta_mm, expected_delta_mm, 2);

  second.client->send(world.protocol.encode(20086, nlohmann::json::object(), 4));
  const auto repeated = world.read_until_pong(second, 5);
  EXPECT_TRUE(std::ranges::none_of(
      repeated, [](const Frame &frame) { return frame.opcode() == kUnitAddOpcode; }));
}

TEST(NativeWorld, CharacterSavedOnTheOverworldMapEntersItAndReceivesTheActivePopulation) {
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn});
  const auto entry = world.enter(world.sessions[0]);

  expect_overworld_entry(entry, 3);
  std::set<int> template_ids;
  for (const auto &add : entry.unit_adds) {
    template_ids.insert(add.at("nub").at("npc").at("npc_cms_id").get<int>());
  }
  EXPECT_EQ(template_ids, (std::set<int>{1001121, 5001015, 1001671}));
}

TEST(NativeWorld, ReconnectingToTheOverworldReannouncesTheSameUnitIds) {
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn});
  auto &session = world.sessions[0];
  const auto first = world.enter(session);
  session.client->disconnect();
  const auto second = world.enter(session);

  expect_overworld_entry(first, 3);
  expect_overworld_entry(second, 3);
  EXPECT_EQ(OverworldSessions::unit_ids(first), OverworldSessions::unit_ids(second));
}

TEST(NativeWorld, OverworldEntryIsRefusedWhenNoPlacementsAreLoaded) {
  OverworldSessions world(nullptr, {kOverworldSpawn});
  const auto opcodes = world.refused_entry_opcodes(world.sessions[0]);

  for (const auto opcode : opcodes) {
    EXPECT_NE(opcode, 175);
    EXPECT_NE(opcode, 176);
    EXPECT_NE(opcode, kUnitAddOpcode);
  }
}

TEST(NativeWorld, CharacterSavedOnAnotherMapIsRefusedEvenWithPlacementsLoaded) {
  const WorldPosition other_map{10116, 0, 0, 0, 0.0F};
  OverworldSessions world(small_overworld_population(), {other_map});
  const auto opcodes = world.refused_entry_opcodes(world.sessions[0]);

  for (const auto opcode : opcodes) {
    EXPECT_NE(opcode, 175);
    EXPECT_NE(opcode, 176);
    EXPECT_NE(opcode, kUnitAddOpcode);
  }
}

// A character without a persisted position keeps the existing tutorial-map start. It is never
// placed on the overworld and receives no population, and the server invents no overworld spawn.
TEST(NativeWorld, CharacterWithoutAPersistedPositionNeverGetsTheOverworldPopulation) {
  OverworldSessions world(small_overworld_population(), {std::nullopt});
  const auto entry = world.enter(world.sessions[0]);

  EXPECT_EQ(entry.map_start.at("map_id"), 8000);
  EXPECT_EQ(entry.map_complete.at("map_id"), 8000);
  EXPECT_TRUE(entry.unit_adds.empty());
}

TEST(NativeWorld, TutorialMapCharacterIsUnaffectedByLoadedPlacementsAndOtherSessions) {
  const WorldPosition tutorial_position{8000, -795146, 712194, -698778, 0.0F};
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn, tutorial_position});
  const auto overworld = world.enter(world.sessions[0]);
  const auto tutorial = world.enter(world.sessions[1]);

  expect_overworld_entry(overworld, 3);
  EXPECT_EQ(tutorial.map_start.at("map_id"), 8000);
  EXPECT_EQ(tutorial.map_complete.at("map_id"), 8000);
  EXPECT_TRUE(tutorial.unit_adds.empty());
}

// Needs the full placement file (CHRONICLES_NPC_PLACEMENTS_9012); a skip is NOT proof.
TEST(NativeWorld, FullMap9012PlacementFileIsAnnouncedByNpcSightThroughTheRealEntryPath) {
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!placements_path) {
    GTEST_SKIP() << "Set CHRONICLES_NPC_PLACEMENTS_9012 to the full map-9012.json";
  }
  std::shared_ptr<const NpcPopulation> population =
      load_npc_population_file(placements_path, kMaxPopulatedNpcUnits);
  const auto expected_stationary = stationary_units_in_entry_sight(*population, kOverworldSpawn);
  std::set<std::string> patrol_ids;
  std::set<std::uint64_t> stationary_ids;
  for (const auto &unit : population->units()) {
    if (unit.placement.patrol) {
      patrol_ids.insert(std::to_string(unit.unit_id));
    } else {
      stationary_ids.insert(unit.unit_id);
    }
  }
  OverworldSessions world(population, {kOverworldSpawn});
  const auto entry = world.enter(world.sessions[0]);

  // Only the units near the spawn are announced, never the whole map.
  ASSERT_FALSE(expected_stationary.empty());
  EXPECT_LT(entry.unit_adds.size(), population->units().size());
  std::set<std::uint64_t> announced_stationary;
  for (const auto id : OverworldSessions::unit_ids(entry)) {
    if (stationary_ids.contains(id)) {
      announced_stationary.insert(id);
    }
  }
  EXPECT_EQ(announced_stationary, expected_stationary);

  bool found_merchant = false;
  for (const auto &add : entry.unit_adds) {
    const auto unit_id = std::stoull(add.at("unit_id").get<std::string>());
    EXPECT_EQ(unit_id & 0xFULL, kNpcUnitTypeNibble);
    const auto &npc = add.at("nub").at("npc");
    const auto &position = add.at("nub").at("pos");
    const auto delta_x = position.at("millimeter_x").get<std::int64_t>() - kOverworldSpawn.x_mm;
    const auto delta_y = position.at("millimeter_y").get<std::int64_t>() - kOverworldSpawn.y_mm;
    EXPECT_LE(delta_x * delta_x + delta_y * delta_y,
              kNpcSightEnterRadiusMm * kNpcSightEnterRadiusMm);
    if (patrol_ids.contains(add.at("unit_id").get<std::string>())) {
      // Without this rate, the client can reject route velocity against a zero speed cap.
      ASSERT_TRUE(npc.contains("stats"));
      ASSERT_TRUE(npc.at("stats").contains("84"));
      EXPECT_EQ(npc.at("stats").at("84"), 1000);
    }

    if (npc.at("npc_cms_id") != 1001121) {
      continue;
    }
    found_merchant = found_merchant || (position.at("millimeter_x") == -1242227 &&
                                        position.at("millimeter_y") == 2993829 &&
                                        position.at("millimeter_z") == -647306);
  }
  EXPECT_TRUE(found_merchant);
}

// A distant NPC is announced when the player walks into its sight and removed when the player
// walks away again, without a map change.
TEST(NativeWorld, OverworldNpcEntersAndLeavesSightAsThePlayerMoves) {
  constexpr int kFarOffsetMm = 400000;
  const nlohmann::json document{
      {"format_version", 1},
      {"map_id", kOverworldMapId},
      {"placements",
       {overworld_placement_json(1001121, 1000, true),
        overworld_placement_json(1001748, kFarOffsetMm, true)}}};
  auto population = std::make_shared<const NpcPopulation>(
      NpcPlacementSet::from_json(document, kTestPlacementBound), kTestPlacementBound);
  std::string near_id;
  std::string far_id;
  for (const auto &unit : population->units()) {
    if (unit.placement.npc_cms_id == 1001748) {
      far_id = std::to_string(unit.unit_id);
    } else {
      near_id = std::to_string(unit.unit_id);
    }
  }
  OverworldSessions world(population, {kOverworldSpawn});
  auto &session = world.sessions[0];

  const auto entry = world.enter(session);
  ASSERT_EQ(entry.unit_adds.size(), 1u);
  EXPECT_EQ(entry.unit_adds[0].at("unit_id"), near_id);

  std::uint32_t sequence = 3;
  const auto move_to = [&](int x_mm) {
    session.client->send(world.protocol.encode(20087,
                                               {{"mode", "MoveMode_Normal"},
                                                {"pos",
                                                 {{"millimeter_x", x_mm},
                                                  {"millimeter_y", kOverworldSpawn.y_mm},
                                                  {"millimeter_z", kOverworldSpawn.z_mm}}}},
                                               sequence++));
    const auto frames = world.read_until_pong(session, sequence++);
    std::vector<nlohmann::json> sight;
    for (const auto &frame : frames) {
      if (frame.opcode() == kUnitAddOpcode || frame.opcode() == kNpcSightUnitRemovedOpcode) {
        auto decoded = world.protocol.decode(frame).at("message");
        decoded["opcode"] = frame.opcode();
        sight.push_back(std::move(decoded));
      }
    }
    return sight;
  };

  const auto arrived = move_to(kOverworldSpawn.x_mm + kFarOffsetMm - 10000);
  ASSERT_EQ(arrived.size(), 2u);
  EXPECT_EQ(arrived[0].at("opcode"), kUnitAddOpcode);
  EXPECT_EQ(arrived[0].at("unit_id"), far_id);
  EXPECT_EQ(arrived[1].at("opcode"), kNpcSightUnitRemovedOpcode);
  EXPECT_EQ(arrived[1].at("unit_ids"), nlohmann::json::array({near_id}));

  EXPECT_TRUE(move_to(kOverworldSpawn.x_mm + kFarOffsetMm - 20000).empty());

  const auto returned = move_to(kOverworldSpawn.x_mm);
  ASSERT_EQ(returned.size(), 2u);
  EXPECT_EQ(returned[0].at("unit_id"), near_id);
  EXPECT_EQ(returned[1].at("unit_ids"), nlohmann::json::array({far_id}));
  session.client->disconnect();
}

// Tutorial and map-9012 characters on one server. The 9012 session is told map 9012, receives
// exactly the loaded population under server-owned ids, and is refused for every target the tutorial
// announced; the tutorial session's entry is unchanged by the other session and refuses the 9012
// ids with its own error. The merchant opens (an announced NPC offering interaction 212) but
// buy and sell stay refused with progress untouched.
TEST(NativeWorld, OverworldAndTutorialSessionsStayIndependentOnOneServer) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs the tutorial profile and the full map-9012 placement file";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  ASSERT_NE(world.population, nullptr);
  auto tutorial = world.add_character("TutorialIndep", world.tutorial_position());
  auto overworld = world.add_character("OverworldIndep", world.overworld_position());

  WorldTestClient tutorial_client(world.server->port());
  const auto tutorial_first = world.enter(tutorial, tutorial_client);
  ASSERT_FALSE(tutorial_first.npcs.empty());
  for (const auto &npc : tutorial_first.npcs) {
    EXPECT_GT(npc.unit_id, kLastServerNpcUnitId) << "tutorial ids stay outside the server NPC range";
  }

  WorldTestClient overworld_client(world.server->port());
  const auto entered = world.enter(overworld, overworld_client);
  ASSERT_EQ(entered.map_ids.size(), 2u);
  EXPECT_EQ(entered.map_ids[0], kOverworldMapId);
  EXPECT_EQ(entered.map_ids[1], kOverworldMapId);
  // The session is told about the population units in its sight, under server-owned ids.
  std::set<std::uint64_t> stationary_ids;
  for (const auto &unit : world.population->units()) {
    if (!unit.placement.patrol) {
      stationary_ids.insert(unit.unit_id);
    }
  }
  const auto expected_ids = sorted_unit_ids(entered.npcs);
  std::set<std::uint64_t> announced_stationary;
  for (const auto id : expected_ids) {
    if (stationary_ids.contains(id)) {
      announced_stationary.insert(id);
    }
  }
  EXPECT_EQ(announced_stationary,
            stationary_units_in_entry_sight(*world.population, world.overworld_position()));
  const AnnouncedNpcFrame *merchant = nullptr;
  for (const auto &npc : entered.npcs) {
    EXPECT_GE(npc.unit_id, kFirstServerNpcUnitId);
    EXPECT_LE(npc.unit_id, kLastServerNpcUnitId);
    EXPECT_TRUE(world.catalog->find_row("Npc", "id", std::to_string(npc.npc_cms_id)));
    if (npc.npc_cms_id == kMerchantNpcCmsId) {
      merchant = &npc;
    }
  }
  ASSERT_NE(merchant, nullptr);
  EXPECT_EQ(merchant->x_mm, -1242227);
  EXPECT_EQ(merchant->y_mm, 2993829);
  // z is announced as 10 * placement z, which differs from the captured wire z by up to ~0.6 m.

  // The two ranges never mix: no id of one session is announced to the other.
  const auto tutorial_ids = sorted_unit_ids(tutorial_first.npcs);
  for (const auto id : expected_ids) {
    EXPECT_FALSE(std::ranges::binary_search(tutorial_ids, id));
  }

  // The overworld session refuses every target the tutorial announced.
  const auto before_overworld = world.progress(overworld);
  for (const auto &tutorial_npc : tutorial_first.npcs) {
    SCOPED_TRACE("tutorial unit " + std::to_string(tutorial_npc.unit_id));
    for (const auto &shop :
         {nlohmann::json::object(), buy_list(133, 1), sell_list(72000016, 4)}) {
      world.send_shop(overworld, overworld_client, std::to_string(tutorial_npc.unit_id), 212, shop,
                      overworld.sequence);
      world.expect_one_error_then_pong(overworld_client, overworld.sequence,
                                       x5::ec::EErrorCode_Name(x5::ec::Interaction_TargetNotFound));
    }
  }
  EXPECT_EQ(world.progress(overworld), before_overworld);

  // The tutorial shop authorizer rejects NPCs belonging to the overworld session.
  const auto before_tutorial = world.progress(tutorial);
  for (const auto id : expected_ids) {
    world.send_shop(tutorial, tutorial_client, std::to_string(id), 212, nlohmann::json::object(),
                    tutorial.sequence);
    world.expect_one_error_then_pong(tutorial_client, tutorial.sequence,
                                     "Interaction_TargetNotFound");
    if (id > expected_ids.front() + 7) {
      break;  // a sample is enough; the reply does not depend on the id
    }
  }
  EXPECT_EQ(world.progress(tutorial), before_tutorial);

  // The merchant opens for the session that was told about it. Open grants nothing: buy and sell
  // are refused and progress is unchanged. (The order 163 then 171 is a Candidate.)
  const auto merchant_id = std::to_string(merchant->unit_id);
  world.send_shop(overworld, overworld_client, merchant_id, 212, nlohmann::json::object(),
                  overworld.sequence);
  const auto opened = world.read_reply(overworld_client);
  EXPECT_EQ(opened.opcode(), 163);
  EXPECT_EQ(world.protocol.decode(world.read_reply(overworld_client)).at("message").at("shop_id"),
            1001);
  for (const auto &shop : {buy_list(133, 1), sell_list(72000016, 4)}) {
    world.send_shop(overworld, overworld_client, merchant_id, 212, shop, overworld.sequence);
    world.expect_one_error_then_pong(overworld_client, overworld.sequence,
                                     x5::ec::EErrorCode_Name(shop.contains("buy_list") ? x5::ec::MoneyNotEnough
                                                                                    : x5::ec::ItemCountError));
  }
  EXPECT_EQ(world.progress(overworld), before_overworld);

  // Leaving sight removes the client actor, not the session's authoritative interaction target.
  // Returning must allow the same shop request without requiring a reconnect.
  for (const int offset_mm : {400000, 0}) {
    auto position = world.overworld_position();
    position.x_mm += offset_mm;
    overworld_client.send(world.protocol.encode(20087,
                                                {{"mode", "MoveMode_Normal"},
                                                 {"pos",
                                                  {{"millimeter_x", position.x_mm},
                                                   {"millimeter_y", position.y_mm},
                                                   {"millimeter_z", position.z_mm}}}},
                                                overworld.sequence++));
    overworld_client.send(
        world.protocol.encode(20003, nlohmann::json::object(), overworld.sequence++));
    while (overworld_client.read().opcode() != kPongOpcode) {
    }
  }
  world.send_shop(overworld, overworld_client, merchant_id, 212, nlohmann::json::object(),
                  overworld.sequence);
  EXPECT_EQ(world.read_reply(overworld_client).opcode(), 163);
  EXPECT_EQ(world.protocol.decode(world.read_reply(overworld_client)).at("message").at("shop_id"),
            1001);
  EXPECT_EQ(world.progress(overworld), before_overworld);

  // The tutorial session is unaffected: dropping and re-entering it, while the overworld session
  // stays connected, yields the identical frame sequence and ids.
  tutorial_client.disconnect();
  WorldTestClient tutorial_again(world.server->port());
  const auto tutorial_second = world.enter(tutorial, tutorial_again);
  EXPECT_EQ(tutorial_second.opcodes, tutorial_first.opcodes);
  EXPECT_EQ(sorted_unit_ids(tutorial_second.npcs), tutorial_ids);
  // overworld.sequence already points at the next unsent packet: the shop and ping helpers above
  // advance it.
  overworld_client.send(world.protocol.encode(20003, nlohmann::json::object(), overworld.sequence));
  EXPECT_EQ(world.read_reply(overworld_client).opcode(), 4);
}

// Two reconnects of the overworld character, each dropped with shop requests still in flight: the
// server stays responsive, every reconnect re-announces the identical ids in the identical order
// (nothing is announced before MapLoadComplete), and progress never changes.
TEST(NativeWorld, OverworldReconnectReannouncesTheSameIdsAfterDropsWithFramesInFlight) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *placements_path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!profile_path || !placements_path) {
    GTEST_SKIP() << "Needs the tutorial profile and the full map-9012 placement file";
  }
  MapEntryWorld world(profile_path, placements_path, true);
  auto overworld = world.add_character("OverworldReconnect", world.overworld_position());
  const auto before = world.progress(overworld);
  const auto expect_preserved_progress = [&](std::int64_t expected_revision) {
    auto actual = world.progress(overworld);
    EXPECT_EQ(actual.revision, expected_revision);
    actual.revision = before.revision;
    EXPECT_EQ(actual, before);
  };
  const auto expected_stationary =
      stationary_units_in_entry_sight(*world.population, world.overworld_position());

  // Patrol units can cross the sight boundary between rounds, so identity and order are
  // compared on the stationary units only.
  const auto stationary_order = [&](const auto &npcs) {
    std::vector<std::uint64_t> order;
    for (const auto &npc : npcs) {
      if (expected_stationary.contains(npc.unit_id)) {
        order.push_back(npc.unit_id);
      }
    }
    return order;
  };

  std::vector<std::uint64_t> first_order;
  for (int round = 0; round < 3; ++round) {
    SCOPED_TRACE("round " + std::to_string(round));
    std::unique_ptr<WorldTestClient> client_owner;
    const auto observed = world.enter_when_character_is_free(overworld, client_owner);
    auto &client = *client_owner;
    const auto entry_revision = before.revision + round + 1;
    expect_preserved_progress(entry_revision);
    const auto order = stationary_order(observed.npcs);
    ASSERT_EQ(order.size(), expected_stationary.size());
    if (round == 0) {
      first_order = order;
    } else {
      EXPECT_EQ(order, first_order);
    }
    // Requests the server never answers on this connection: queue them and drop.
    overworld.sequence = kFirstSequenceAfterEntry;
    for (int request = 0; request < 24; ++request) {
      const std::string target = request % 2 == 0 ? std::to_string(order.front()) : "999999";
      world.send_shop(overworld, client, target, 212, request % 3 == 0 ? buy_list(133, 1)
                                                                       : nlohmann::json::object(),
                      overworld.sequence);
    }
    client.disconnect();
    expect_preserved_progress(entry_revision);
  }
  std::unique_ptr<WorldTestClient> last_owner;
  const auto final_entry = world.enter_when_character_is_free(overworld, last_owner);
  EXPECT_EQ(stationary_order(final_entry.npcs), first_order);
  expect_preserved_progress(before.revision + 4);
}

// A character persisted on 9012 on a server started without placements is not placed on any map:
// no MapMoveStart/Complete, no NPC announcements, and the persisted position and progress stay.
TEST(NativeWorld, OverworldEntryWithoutPlacementsKeepsPersistedPositionAndProgress) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the generated local tutorial profile";
  }
  MapEntryWorld world(profile_path, nullptr, false);
  ASSERT_EQ(world.population, nullptr);
  auto overworld = world.add_character("OverworldNoData", world.overworld_position());
  const auto before = world.progress(overworld);

  WorldTestClient client(world.server->port());
  const auto observed = world.enter_and_observe_refusal(overworld, client);
  EXPECT_TRUE(observed.map_ids.empty());
  EXPECT_TRUE(observed.npcs.empty());
  EXPECT_EQ(std::ranges::count(observed.opcodes, 176), 0);
  auto after_entry = before;
  ++after_entry.revision;
  EXPECT_EQ(world.progress(overworld), after_entry);
  const auto characters = world.repositories.characters->characters(overworld.account_id, 1);
  ASSERT_EQ(characters.size(), 1u);
  ASSERT_TRUE(characters.front().world_position.has_value());
  EXPECT_EQ(characters.front().world_position->map_id, kOverworldMapId);
}

} // namespace
} // namespace chronicles
