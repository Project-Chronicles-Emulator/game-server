// Behavioral tests for summoned mounts, vehicles, house mates and trade runs:
// src/data/companion_catalog.cpp, companion_world.cpp, companion_session.cpp,
// trade_catalog.cpp, trade_run.cpp and trade_session.cpp.
//
// Every outbound payload is encoded and decoded with the recovered descriptors, so field names
// and wire types are schema-checked. Session tests commit through the memory
// repositories.

#include "data/client_catalog.hpp"
#include "p_a2c.ws.pb.h"
#include "p_c2a.ws.pb.h"
#include "p_c2f.rest.pb.h"
#include "p_c2f.ws.pb.h"
#include "p_f2c.ws.pb.h"
#include "protocol/protocol.hpp"
#include "testing/memory_repositories.hpp"
#include "world/core/progress_access.hpp"
#include "world/features/companion/tests/companion_test_support.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/features/trade/trade_session.hpp"
#include "world/session/local_packet_memory.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string_view>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace chronicles {
namespace {

using namespace companion_test_support;

// ---------------------------------------------------------------------------------------------
// Catalogs

TEST(CompanionCatalog, IndexesSummonsRegistrationItemsVehiclesRepairAndMates) {
  const auto catalog = companion_catalog();

  ASSERT_NE(catalog->summon(kSummonId), nullptr);
  EXPECT_EQ(catalog->summon(kSummonId)->npc_cms_id, 2001);
  EXPECT_TRUE(catalog->summon(kSummonId)->rideable);
  EXPECT_EQ(catalog->summon_registered_by_item(kSummonItem), kSummonId);
  EXPECT_FALSE(catalog->summon_registered_by_item(kRepairKit));

  const auto *wagon = catalog->vehicle(kWagonId);
  ASSERT_NE(wagon, nullptr);
  EXPECT_EQ(wagon->kind, companion::VehicleKind::trade_wagon);
  EXPECT_EQ(wagon->max_load_capacity, 7);
  EXPECT_EQ(wagon->price, 100);
  EXPECT_EQ(wagon->seat_count, 1);

  const auto *repair = catalog->repair_task(62);
  ASSERT_NE(repair, nullptr);
  EXPECT_EQ(repair->add_durability, 1500);
  EXPECT_EQ(repair->consumed_item_cms_id, kRepairKit);
  EXPECT_EQ(repair->consumed_item_count, 1);

  EXPECT_EQ(catalog->mate(kMateId)->npc_cms_id, 35);
  EXPECT_EQ(catalog->mate_intimacy_level(kMateId, 0), 1);
  EXPECT_EQ(catalog->mate_intimacy_level(kMateId, 100), 2);
  EXPECT_EQ(catalog->mate_intimacy_level(kMateId, 299), 2);
  EXPECT_EQ(catalog->mate_intimacy_level(kMateId, 300), 3);
  EXPECT_EQ(catalog->mate_intimacy_limit(kMateId), 1000);
  ASSERT_NE(catalog->mate_doodad_interaction(160021), nullptr);
  EXPECT_EQ(catalog->mate_doodad_interaction(160021)->probability_basis_points, 10000);
}

TEST(CompanionCatalog, RejectsASummonWhoseNpcDoesNotExist) {
  auto tables = companion_tables();
  tables.npcs = {{{"id", "35"}}};
  EXPECT_THROW(companion::CompanionCatalog{tables}, std::runtime_error);
}

TEST(CompanionCatalog, DefaultSummonIsTheLowestRideableSummon) {
  auto tables = companion_tables();
  constexpr int kHigherSummonId = 2005;
  tables.summon_npcs.push_back(
      {{"id", std::to_string(kHigherSummonId)}, {"ability", "Mount"}, {"FK_npcId", "2001"}});

  const companion::CompanionCatalog catalog{tables};

  EXPECT_EQ(catalog.default_summon_id(), kSummonId);
}

// ---------------------------------------------------------------------------------------------
// CompanionWorld

class CompanionWorldTest : public testing::Test {
protected:
  CompanionWorldTest()
      : world_(companion_catalog(), companion::CompanionWorldConfig{}) {}

  companion::CompanionWorld world_;
};

TEST_F(CompanionWorldTest, LiveAudienceUpdatesCarryTheirCapturedSourceView) {
  auto owner = participant(100, 1, at(0, 0));
  owner.view = {4, kMap, 0};
  auto viewer = participant(200, 2, at(1000, 0));
  viewer.view = {10, kMap, 0};
  world_.attach(owner);
  world_.attach(viewer);

  std::size_t checked = 0;
  const auto check_bindings = [&](const companion::CompanionUpdate &update) {
    for (const auto &packet : update.packets) {
      ASSERT_TRUE(packet.source);
      EXPECT_EQ(packet.source->connection_id, packet.connection_id);
      if (packet.connection_id == 100) {
        EXPECT_EQ(packet.source->view.epoch, 4U);
      } else if (packet.connection_id == 200) {
        EXPECT_EQ(packet.source->view.epoch, 10U);
      } else {
        ADD_FAILURE() << "Unexpected companion recipient";
      }
      ++checked;
    }
  };

  const auto spawned = world_.spawn_vehicle(100, kWagonId, at(0, 0), 4000, false, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);
  check_bindings(spawned.update);
  for (const auto &packet : spawned.update.packets) {
    if (packet.opcode != companion::kUnitAddNotification) {
      continue;
    }
    ASSERT_TRUE(packet.source);
    const auto snapshot = world_.snapshot_unit_add(*packet.source);
    ASSERT_TRUE(snapshot);
    const auto guard = world_.acquire_source_queue_guard(*packet.source, {}, &*snapshot);
    ASSERT_TRUE(guard);
    guard->mark_ready();
  }
  check_bindings(world_.ride(100, spawned.unit_id, 0, kNow).update);
  check_bindings(world_.move(100, at(2000, 0), nullptr));
  check_bindings(world_.damage_vehicle(spawned.unit_id, 100, kNow).update);
  check_bindings(world_.repair_vehicle(100, spawned.unit_id, 100, kNow).update);
  check_bindings(world_.get_off(100, std::nullopt, kNow).update);
  companion::CompanionUpdate removed;
  ASSERT_TRUE(world_.remove_vehicle(1, companion::DismountReason::user, kNow, removed));
  check_bindings(removed);
  EXPECT_GT(checked, 2U);
}

TEST(CompanionSeatCount, BoundsPassengerAllocationBeforeVehicleSpawn) {
  EXPECT_EQ(companion::checked_companion_seat_count(-1), 1U);
  EXPECT_EQ(companion::checked_companion_seat_count(0), 1U);
  EXPECT_EQ(companion::checked_companion_seat_count(1), 1U);
  EXPECT_EQ(companion::checked_companion_seat_count(64), 64U);
  EXPECT_FALSE(companion::checked_companion_seat_count(65));
  EXPECT_FALSE(companion::checked_companion_seat_count((std::numeric_limits<int>::max)()));
}

TEST(CompanionSeatCount, OversizedCatalogVehicleIsRefusedBeforeSpawning) {
  // The fixture owns a mutable catalog; production exposes definitions as const.
  auto catalog = std::make_shared<companion::CompanionCatalog>(companion_tables());
  auto *vehicle = const_cast<companion::VehicleDefinition *>(catalog->vehicle(kWagonId));
  ASSERT_NE(vehicle, nullptr);
  vehicle->seat_count = 65;
  companion::CompanionWorld world(catalog, companion::CompanionWorldConfig{});
  world.attach(participant(100, 1, at(0, 0)));
  const auto refused = world.spawn_vehicle(100, kWagonId, at(0, 0), 4000, true, kNow);
  EXPECT_EQ(refused.error, x5::ec::Trade_WagonSpawnFailed);
  EXPECT_EQ(world.unit_count(), 0U);
  EXPECT_FALSE(world.ridden_unit(100));

  vehicle->seat_count = 64;
  const auto allowed = world.spawn_vehicle(100, kWagonId, at(0, 0), 4000, true, kNow);
  EXPECT_EQ(allowed.error, x5::ec::Success);
  EXPECT_EQ(world.ridden_unit(100), allowed.unit_id);
}

TEST_F(CompanionWorldTest, SummonedMountSeatsItsOwnerAndIsAnnouncedToNearbyPlayers) {
  world_.attach(participant(100, 1, at(0, 0)));
  world_.attach(participant(200, 2, at(5000, 0)));
  world_.attach(participant(300, 3, at(500000, 0)));

  const auto spawned = world_.spawn_mount(100, kSummonId, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);
  EXPECT_EQ(spawned.unit_id & 0xF, companion::kNpcUnitTypeNibble);
  EXPECT_TRUE(companion::is_companion_unit_id(spawned.unit_id));

  for (const std::uint64_t viewer : {100u, 200u}) {
    const auto adds = addressed(spawned.update, viewer, companion::kUnitAddNotification);
    ASSERT_EQ(adds.size(), 1u) << viewer;
    const auto &npc = adds.front().at("nub").at("npc");
    EXPECT_EQ(npc.at("npc_cms_id"), 2001);
    EXPECT_EQ(npc.at("owner_char_id"), "1");
    // EX5Faction_Player is the protobuf default (0), so the decoded wire omits it.
    EXPECT_FALSE(npc.contains("faction_type"));
    EXPECT_EQ(npc.at("summon").at("summon_npc_cms_id"), kSummonId);
    // Neutral MoveSpeedRate, as the NPC population announces it.
    EXPECT_EQ(npc.at("stats").at("84"), 1000);
    // Actor loading can finish after MountUnitNoti. Its initial passenger snapshot
    // must let the client's ready callback recover the same rider and seat.
    ASSERT_TRUE(npc.contains("mount"));
    EXPECT_EQ(npc.at("mount").value("passengers", Json::array()),
              Json::array({std::to_string(player_wire_unit_id(1))}));

    const auto mounts = addressed(spawned.update, viewer, companion::kMountUnitNotification);
    ASSERT_EQ(mounts.size(), 1u) << viewer;
    EXPECT_EQ(mounts.front().at("unit_id"), std::to_string(spawned.unit_id));
    EXPECT_EQ(mounts.front().at("passenger_unit_id"), std::to_string(player_wire_unit_id(1)));
    EXPECT_EQ(mounts.front().at("slot"), 0);
  }
  EXPECT_TRUE(addressed(spawned.update, 300, companion::kUnitAddNotification).empty());
  EXPECT_EQ(world_.ridden_unit(100), spawned.unit_id);
  EXPECT_EQ(world_.spawn_mount(100, kSummonId, kNow).error, x5::ec::Summon_AlreadyRiding);
}

// The client's MountUnitNoti/MoveNoti dispatchers find the addressed actor by the low 32 bits of
// its unit id, so companion ids must leave the high dword empty and stay clear of the other
// server-owned id ranges.
TEST_F(CompanionWorldTest, CompanionUnitIdsFitTheClientThirtyTwoBitLookup) {
  constexpr std::uint64_t kGmSpawnedNpcIdBase = 0xF0000000ULL;
  const std::uint64_t last_population_id = kFirstNpcUnitId + kMaxPopulatedNpcUnits - 1;

  std::set<std::uint64_t> seen;
  for (std::uint64_t connection = 100; connection < 110; ++connection) {
    const auto character_id = static_cast<std::int64_t>(connection);
    world_.attach(participant(connection, character_id, at(0, 0)));

    const auto mount = world_.spawn_mount(connection, kSummonId, kNow);
    const auto wagon = world_.spawn_vehicle(connection, kWagonId, at(1000, 0), 5000, false, kNow);
    ASSERT_EQ(mount.error, x5::ec::Success);
    ASSERT_EQ(wagon.error, x5::ec::Success);

    for (const auto unit_id : {mount.unit_id, wagon.unit_id}) {
      EXPECT_EQ(unit_id >> 32, 0u) << unit_id;
      EXPECT_TRUE(companion::is_companion_unit_id(unit_id)) << unit_id;
      EXPECT_GT(unit_id, last_population_id) << unit_id;
      EXPECT_LT(unit_id, kGmSpawnedNpcIdBase) << unit_id;
      EXPECT_TRUE(seen.insert(unit_id).second) << unit_id;
    }
    EXPECT_EQ(mount.unit_id & 0xF, companion::kNpcUnitTypeNibble);
    EXPECT_EQ(wagon.unit_id & 0xF, companion::kOperableUnitTypeNibble);
  }

  EXPECT_FALSE(companion::is_companion_unit_id(player_wire_unit_id(1)));
  EXPECT_FALSE(companion::is_companion_unit_id(kFirstNpcUnitId));
  EXPECT_FALSE(companion::is_companion_unit_id(kGmSpawnedNpcIdBase | 2));
  EXPECT_FALSE(companion::is_companion_unit_id((1ULL << 62) | 0xE0000014ULL));
}

TEST_F(CompanionWorldTest, RiderMovementMovesTheMountAndShowsItToPlayersItApproaches) {
  world_.attach(participant(100, 1, at(0, 0)));
  world_.attach(participant(200, 2, at(5000, 0)));
  world_.attach(participant(300, 3, at(150000, 0)));
  const auto mount = world_.spawn_mount(100, kSummonId, kNow).unit_id;

  const Json movement = request(20087, {{"tick", "1"},
                                        {"mode", "MoveMode_Normal"},
                                        {"detail", "MoveDetail_Run"},
                                        {"pos", {{"millimeter_x", 90000},
                                                 {"millimeter_y", 0},
                                                 {"millimeter_z", 0}}}});
  const auto moved = world_.move(100, at(90000, 0), &movement);

  const auto seen_by_near = addressed(moved, 200, companion::kMoveNotification);
  ASSERT_EQ(seen_by_near.size(), 1u);
  EXPECT_EQ(seen_by_near.front().at("unit_id"), std::to_string(mount));
  EXPECT_TRUE(addressed(moved, 100, companion::kMoveNotification).empty());

  // Player 3 now sees the mount with its rider bound at seat 0.
  const auto late = addressed(moved, 300, companion::kUnitAddNotification);
  ASSERT_EQ(late.size(), 1u);
  EXPECT_EQ(late.front().at("nub").at("npc").at("mount").at("passengers"),
            Json::array({std::to_string(player_wire_unit_id(1))}));

  // A player attaching later also learns of the mount and its rider.
  const auto joined = world_.attach(participant(400, 4, at(90000, 1000)));
  ASSERT_EQ(addressed(joined, 400, companion::kUnitAddNotification).size(), 1u);
}

TEST_F(CompanionWorldTest, GettingOffAMountDismountsAtThePointAndRemovesTheMount) {
  world_.attach(participant(100, 1, at(0, 0)));
  world_.attach(participant(200, 2, at(5000, 0)));
  const auto mount = world_.spawn_mount(100, kSummonId, kNow).unit_id;

  auto point = at(300, 400);
  point.yaw = 90.0F;
  const auto off = world_.get_off(100, point, kNow);
  ASSERT_EQ(off.error, x5::ec::Success);

  const auto dismounts = addressed(off.update, 200, companion::kMountUnitNotification);
  ASSERT_EQ(dismounts.size(), 1u);
  EXPECT_FALSE(dismounts.front().contains("slot"));
  EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_User");
  EXPECT_EQ(dismounts.front().at("dismount_point").at("pos").at("millimeter_x"), 300);
  const auto removals = addressed(off.update, 200, companion::kUnitRemoveNotification);
  ASSERT_EQ(removals.size(), 1u);
  EXPECT_EQ(removals.front().at("unit_ids"), Json::array({std::to_string(mount)}));
  EXPECT_FALSE(world_.ridden_unit(100));
  EXPECT_EQ(world_.unit_count(), 0u);
  EXPECT_EQ(world_.get_off(100, std::nullopt, kNow).error, x5::ec::InvalidState);
}

TEST_F(CompanionWorldTest, VehicleBoardingChecksOwnerSlotRangeAndOccupancy) {
  world_.attach(participant(100, 1, at(0, 0)));
  world_.attach(participant(200, 2, at(1000, 0)));
  const auto spawned = world_.spawn_vehicle(100, kWagonId, at(2000, 0), 5000, false, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);
  EXPECT_EQ(spawned.unit_id & 0xF, companion::kOperableUnitTypeNibble);
  const auto adds = addressed(spawned.update, 200, companion::kUnitAddNotification);
  ASSERT_EQ(adds.size(), 1u);
  const auto &vehicle = adds.front().at("nub").at("operable").at("vehicle");
  EXPECT_EQ(vehicle.at("vehicle_cms_id"), kWagonId);
  EXPECT_EQ(vehicle.at("owner_char_id"), "1");
  EXPECT_FALSE(vehicle.contains("passengers") && !vehicle.at("passengers").empty());

  EXPECT_EQ(world_.ride(200, spawned.unit_id, 0, kNow).error, x5::ec::Riding_NotOwner);
  EXPECT_EQ(world_.ride(100, spawned.unit_id, 1, kNow).error, x5::ec::Riding_InvalidSlot);
  EXPECT_EQ(world_.ride(100, 12345, 0, kNow).error, x5::ec::Riding_TargetNotFound);
  world_.move(100, at(50000, 0), nullptr);
  EXPECT_EQ(world_.ride(100, spawned.unit_id, 0, kNow).error, x5::ec::Trade_WagonTooFar);
  world_.move(100, at(1500, 0), nullptr);

  const auto boarded = world_.ride(100, spawned.unit_id, 0, kNow);
  ASSERT_EQ(boarded.error, x5::ec::Success);
  EXPECT_EQ(addressed(boarded.update, 200, companion::kMountUnitNotification).size(), 1u);
  EXPECT_EQ(world_.ride(100, spawned.unit_id, 0, kNow).error, x5::ec::Summon_AlreadyRiding);

  // The vehicle stays where its driver gets off.
  ASSERT_EQ(world_.get_off(100, std::nullopt, kNow).error, x5::ec::Success);
  ASSERT_TRUE(world_.vehicle_of(1));
  EXPECT_FALSE(world_.vehicle_of(1)->owner_riding);
}

TEST_F(CompanionWorldTest, DismountCannotTeleportAndDisconnectUnbindsBeforeRemoval) {
  world_.attach(participant(100, 1, at(0, 0)));
  world_.attach(participant(200, 2, at(1000, 0)));
  world_.spawn_mount(100, kSummonId, kNow);

  const auto off = world_.get_off(100, at(1000000, 0), kNow);
  const auto dismounts = addressed(off.update, 100, companion::kMountUnitNotification);
  ASSERT_EQ(dismounts.size(), 1u);
  EXPECT_EQ(dismounts.front().at("dismount_point").at("pos").value("millimeter_x", 0), 0);

  world_.spawn_mount(100, kSummonId, kNow);
  const auto detached = world_.detach(100);
  ASSERT_EQ(detached.update.packets.size(), 2u);
  EXPECT_EQ(detached.update.packets[0].connection_id, 200);
  EXPECT_EQ(detached.update.packets[0].opcode, companion::kMountUnitNotification);
  EXPECT_EQ(detached.update.packets[0].message.at("dismount_reason"),
            "EX5DismountReason_RiderRemoved");
  EXPECT_EQ(detached.update.packets[1].opcode, companion::kUnitRemoveNotification);
  EXPECT_FALSE(world_.ridden_unit(100));
}

TEST_F(CompanionWorldTest, DrivingMovesTheVehicleAndDetachReportsItsLastState) {
  world_.attach(participant(100, 1, at(0, 0)));
  const auto spawned = world_.spawn_vehicle(100, kWagonId, at(0, 0), 4000, true, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);
  EXPECT_EQ(world_.ridden_unit(100), spawned.unit_id);
  const auto adds = addressed(spawned.update, 100, companion::kUnitAddNotification);
  ASSERT_EQ(adds.size(), 1);
  EXPECT_EQ(adds.front().at("nub").at("operable").at("vehicle").value("passengers", Json::array()),
            Json::array({std::to_string(player_wire_unit_id(1))}));

  world_.move(100, at(7000, 8000, 10), nullptr);
  const auto detached = world_.detach(100);
  ASSERT_EQ(detached.vehicles.size(), 1u);
  EXPECT_EQ(detached.vehicles.front().position.x_mm, 7000);
  EXPECT_EQ(detached.vehicles.front().health, 4000);
  EXPECT_TRUE(detached.vehicles.front().owner_riding);
  EXPECT_EQ(world_.unit_count(), 0u);
}

TEST_F(CompanionWorldTest, FailedDetachPreparationPreservesWagonAndRestagesItsLatestState) {
  world_.attach(participant(100, 1, at(0, 0)));
  world_.attach(participant(200, 2, at(1000, 0)));
  const auto spawned = world_.spawn_vehicle(100, kWagonId, at(0, 0), 4000, true, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);
  world_.move(100, at(7000, 8000, 10), nullptr);

  EXPECT_THROW(world_.detach_staged(100, [&](const auto &vehicles) {
    ASSERT_EQ(vehicles.size(), 1U);
    EXPECT_EQ(vehicles.front().position.x_mm, 7000);
    throw std::runtime_error("injected wagon preparation failure");
  }), std::runtime_error);
  ASSERT_TRUE(world_.vehicle_of(1));
  EXPECT_EQ(world_.vehicle_of(1)->unit_id, spawned.unit_id);
  EXPECT_EQ(world_.ridden_unit(100), spawned.unit_id);
  EXPECT_EQ(world_.unit_count(), 1U);

  world_.move(100, at(8000, 8000, 10), nullptr);
  int prepared_position = 0;
  const auto detached = world_.detach_staged(100, [&](const auto &vehicles) {
    ASSERT_EQ(vehicles.size(), 1U);
    prepared_position = vehicles.front().position.x_mm;
  });
  EXPECT_EQ(prepared_position, 8000);
  ASSERT_EQ(detached.vehicles.size(), 1U);
  EXPECT_EQ(detached.vehicles.front().position.x_mm, 8000);
  EXPECT_TRUE(detached.vehicles.front().owner_riding);
  EXPECT_EQ(addressed(detached.update, 200, companion::kMountUnitNotification).size(), 1U);
  EXPECT_EQ(addressed(detached.update, 200, companion::kUnitRemoveNotification).size(), 1U);
  EXPECT_EQ(world_.unit_count(), 0U);
  EXPECT_TRUE(world_.detach(100).update.packets.empty());
}

TEST_F(CompanionWorldTest, RetainedDetachDropsUnqueuedAddAndKeepsQueuedViewIdentity) {
  auto owner = participant(100, 1, at(0, 0));
  auto ready_viewer = participant(200, 2, at(1000, 0));
  auto pending_viewer = participant(300, 3, at(1000, 0));
  ready_viewer.view = {10, kMap, 0};
  pending_viewer.view = {20, kMap, 0};
  world_.attach(owner);
  world_.attach(ready_viewer);
  world_.attach(pending_viewer);
  const auto spawned = world_.spawn_mount(100, kSummonId, kNow);
  const auto find_add = [&](std::uint64_t connection) {
    return std::ranges::find_if(spawned.update.packets, [&](const auto &packet) {
      return packet.connection_id == connection && packet.opcode == companion::kUnitAddNotification;
    });
  };
  const auto ready_add = find_add(200);
  const auto pending_add = find_add(300);
  ASSERT_NE(ready_add, spawned.update.packets.end());
  ASSERT_NE(pending_add, spawned.update.packets.end());
  ASSERT_TRUE(ready_add->source);
  ASSERT_TRUE(pending_add->source);
  {
    const auto snapshot = world_.snapshot_unit_add(*ready_add->source);
    ASSERT_TRUE(snapshot);
    const auto guard = world_.acquire_source_queue_guard(*ready_add->source, {}, &*snapshot);
    ASSERT_TRUE(guard);
    guard->mark_ready();
  }

  std::shared_ptr<companion::CompanionWorld::RetainedDetach> retained;
  world_.detach_retained(100, [&](const auto &, const auto &operation) {
    retained = operation;
    EXPECT_THROW(operation->page({}, 1), std::invalid_argument);
  });
  ASSERT_TRUE(retained);
  EXPECT_FALSE(world_.acquire_source_queue_guard(*pending_add->source));
  const auto page = retained->page({}, 1);
  ASSERT_EQ(page.recipients.size(), 1U);
  EXPECT_EQ(page.recipients[0].connection_id, 200U);
  EXPECT_EQ(page.recipients[0].view, ready_viewer.view);
  EXPECT_EQ(page.recipients[0].source.unit_id, spawned.unit_id);
  EXPECT_EQ(page.recipients[0].source.generation, ready_add->source->generation);
  EXPECT_TRUE(page.recipients[0].source.terminal);
  EXPECT_TRUE(world_.acquire_source_queue_guard(page.recipients[0].source));
  ASSERT_EQ(page.recipients[0].packets->size(), 2U);
  EXPECT_EQ(page.recipients[0].packets->at(0).opcode, companion::kMountUnitNotification);
  EXPECT_EQ(page.recipients[0].packets->at(1).opcode, companion::kUnitRemoveNotification);
  EXPECT_TRUE(retained->page(page.next, 1).complete);
}

TEST_F(CompanionWorldTest, RetainedDetachPagesEveryReadyObserverAtCapacityBoundary) {
  world_.attach(participant(1, 1, at(0, 0)));
  constexpr std::uint64_t kLastObserver = 4098;
  for (std::uint64_t connection = 2; connection <= kLastObserver; ++connection) {
    world_.attach(participant(connection, static_cast<std::int64_t>(connection), at(0, 0)));
  }
  ASSERT_EQ(world_.spawn_mount(1, kSummonId, kNow).error, x5::ec::Success);
  std::shared_ptr<companion::CompanionWorld::RetainedDetach> retained;
  world_.detach_retained(1, [&](const auto &, const auto &operation) {
    retained = operation;
  });
  ASSERT_TRUE(retained);
  const auto first = retained->page({}, 4096);
  ASSERT_EQ(first.recipients.size(), 4096U);
  EXPECT_FALSE(first.complete);
  const auto second = retained->page(first.next, 4096);
  ASSERT_EQ(second.recipients.size(), 1U);
  EXPECT_TRUE(second.complete);
  EXPECT_EQ(first.recipients.front().connection_id, 2U);
  EXPECT_EQ(second.recipients.front().connection_id, kLastObserver);
  EXPECT_EQ(first.recipients.front().packets, second.recipients.front().packets);
  EXPECT_THROW(retained->page({}, 4097), std::invalid_argument);
}

TEST_F(CompanionWorldTest, CompanionAnnouncementsGrowPacketStorageGeometrically) {
  constexpr std::uint64_t kConnections = 4097;
  for (std::uint64_t connection = 1; connection <= kConnections; ++connection) {
    world_.attach(participant(connection, static_cast<std::int64_t>(connection), at(0, 0)));
  }

  const auto mate = world_.spawn_mate(1, kMateId, MateRecord{});
  ASSERT_EQ(mate.error, x5::ec::Success);
  ASSERT_EQ(mate.update.packets.size(), kConnections);
  EXPECT_GT(mate.update.packets.capacity(), mate.update.packets.size());
  EXPECT_LE(mate.update.packets.capacity(), 2 * mate.update.packets.size());
}

TEST_F(CompanionWorldTest, RetainedDetachPreparationFailureLeavesWagonAndSeat) {
  world_.attach(participant(100, 1, at(0, 0)));
  world_.attach(participant(200, 2, at(1000, 0)));
  const auto wagon = world_.spawn_vehicle(100, kWagonId, at(0, 0), 4000, true, kNow);
  ASSERT_EQ(wagon.error, x5::ec::Success);
  EXPECT_THROW(world_.detach_retained(100, [](const auto &, const auto &) {
    throw std::runtime_error("Injected retention failure");
  }), std::runtime_error);
  EXPECT_EQ(world_.ridden_unit(100), wagon.unit_id);
  ASSERT_TRUE(world_.vehicle_of(1));
  EXPECT_EQ(world_.vehicle_of(1)->health, 4000);
  std::shared_ptr<companion::CompanionWorld::RetainedDetach> retained;
  const auto result = world_.detach_retained(100, [&](const auto &, const auto &operation) {
    retained = operation;
  });
  ASSERT_TRUE(retained);
  ASSERT_EQ(result.vehicles.size(), 1U);
  const auto page = retained->page({}, 10);
  ASSERT_EQ(page.recipients.size(), 1U);
  EXPECT_EQ(page.recipients.front().connection_id, 200U);
}

TEST(CompanionDetachBudget, ContentionDoesNotMutateAndReleaseAllowsRetry) {
  auto budget = std::make_shared<companion::CompanionDetachBudget>(50000);
  companion::CompanionWorld first(companion_catalog(), {}, budget);
  companion::CompanionWorld second(companion_catalog(), {}, budget);
  first.attach(participant(100, 1, at(0, 0)));
  first.attach(participant(200, 2, at(1000, 0)));
  second.attach(participant(300, 3, at(0, 0)));
  second.attach(participant(400, 4, at(1000, 0)));
  const auto first_mount = first.spawn_mount(100, kSummonId, kNow);
  const auto second_mount = second.spawn_mount(300, kSummonId, kNow);
  ASSERT_EQ(first_mount.error, x5::ec::Success);
  ASSERT_EQ(second_mount.error, x5::ec::Success);

  std::shared_ptr<companion::CompanionWorld::RetainedDetach> first_retained;
  first.detach_retained(100, [&](const auto &, const auto &operation) {
    first_retained = operation;
  });
  ASSERT_TRUE(first_retained);
  EXPECT_GT(budget->used_bytes(), 0U);

  bool second_prepared = false;
  EXPECT_THROW(second.detach_retained(300, [&](const auto &, const auto &) {
    second_prepared = true;
  }), std::runtime_error);
  EXPECT_FALSE(second_prepared);
  EXPECT_EQ(second.unit_count(), 1U);
  EXPECT_EQ(second.ridden_unit(300), second_mount.unit_id);

  first_retained.reset();
  EXPECT_EQ(budget->used_bytes(), 0U);
  std::shared_ptr<companion::CompanionWorld::RetainedDetach> second_retained;
  second.detach_retained(300, [&](const auto &, const auto &operation) {
    second_retained = operation;
  });
  ASSERT_TRUE(second_retained);
  EXPECT_EQ(second.unit_count(), 0U);
  second_retained.reset();
  EXPECT_EQ(budget->used_bytes(), 0U);
}

TEST(CompanionDetachBudget, FailedPreparationReleasesReservation) {
  auto budget = std::make_shared<companion::CompanionDetachBudget>(50000);
  companion::CompanionWorld world(companion_catalog(), {}, budget);
  world.attach(participant(100, 1, at(0, 0)));
  const auto mount = world.spawn_mount(100, kSummonId, kNow);
  ASSERT_EQ(mount.error, x5::ec::Success);

  EXPECT_THROW(world.detach_retained(100, [](const auto &, const auto &) {
    throw std::runtime_error("Injected retention failure");
  }), std::runtime_error);
  EXPECT_EQ(budget->used_bytes(), 0U);
  EXPECT_EQ(world.unit_count(), 1U);
  EXPECT_EQ(world.ridden_unit(100), mount.unit_id);
  EXPECT_FALSE(budget->reserve(50001));
  EXPECT_EQ(budget->used_bytes(), 0U);
}

#ifdef _WIN32
// `_heapwalk` measures outstanding CRT allocations at a point in time. It does
// not measure transient peak allocations or allocations from other heaps.
std::optional<std::size_t> occupied_crt_heap_bytes() {
  _HEAPINFO entry{};
  std::size_t bytes = 0;
  int status = _HEAPOK;
  while ((status = _heapwalk(&entry)) == _HEAPOK) {
    if (entry._useflag != _USEDENTRY) {
      continue;
    }
    if (entry._size > (std::numeric_limits<std::size_t>::max)() - bytes) {
      return std::nullopt;
    }
    bytes += entry._size;
  }
  if (status != _HEAPEND && status != _HEAPEMPTY) {
    return std::nullopt;
  }
  return bytes;
}

TEST(CompanionSeatCount, MaximumSeatSnapshotFitsReconstructionWorkspace) {
  constexpr std::size_t kSnapshotLimit = 1024 * 1024;
  constexpr std::size_t kWorkspaceLimit = 3 * kSnapshotLimit;
  auto catalog = std::make_shared<companion::CompanionCatalog>(companion_tables());
  auto *vehicle = const_cast<companion::VehicleDefinition *>(catalog->vehicle(kWagonId));
  ASSERT_NE(vehicle, nullptr);
  vehicle->seat_count = 64;

  companion::CompanionWorld world(catalog, {});
  world.attach(participant(100, 1, at(0, 0)));
  auto viewer = participant(200, 2, at(1000, 0));
  viewer.view = {10, kMap, 0};
  world.attach(viewer);
  const auto spawned = world.spawn_vehicle(100, kWagonId, at(0, 0), 4000, false, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);
  const auto add = std::ranges::find_if(spawned.update.packets, [](const auto &packet) {
    return packet.connection_id == 200 && packet.opcode == companion::kUnitAddNotification;
  });
  ASSERT_NE(add, spawned.update.packets.end());
  ASSERT_TRUE(add->source);
  ASSERT_EQ(world.ride(100, spawned.unit_id, 63, kNow).error, x5::ec::Success);

  const auto before_cold = occupied_crt_heap_bytes();
  ASSERT_TRUE(before_cold);
  {
    const auto cold_snapshot = world.snapshot_unit_add(*add->source);
    ASSERT_TRUE(cold_snapshot);
    const auto cold_frame = protocol().encode(companion::kUnitAddNotification, *cold_snapshot);
    EXPECT_FALSE(cold_frame.bytes.empty());
  }
  const auto before = occupied_crt_heap_bytes();
  ASSERT_TRUE(before);
  const auto snapshot = world.snapshot_unit_add(*add->source);
  ASSERT_TRUE(snapshot);
  ASSERT_EQ(snapshot->at("nub").at("operable").at("vehicle").at("passengers").size(), 64U);
  std::size_t json_charge = 0;
  charge_local_json(*snapshot, 0, json_charge);
  const auto encoded = protocol().encode(companion::kUnitAddNotification, *snapshot);
  auto second_snapshot = *snapshot;
  const auto guard = world.acquire_source_queue_guard(*add->source, {}, &second_snapshot);
  ASSERT_TRUE(guard);
  const auto after = occupied_crt_heap_bytes();
  ASSERT_TRUE(after);

  const auto growth = *after > *before ? *after - *before : 0;
  const auto cold_growth = *before > *before_cold ? *before - *before_cold : 0;
  RecordProperty("cold_codec_outstanding_growth_bytes", std::to_string(cold_growth));
  RecordProperty("snapshot_json_charge_bytes", std::to_string(json_charge));
  RecordProperty("encoded_bytes", std::to_string(encoded.bytes.size()));
  RecordProperty("crt_outstanding_growth_bytes", std::to_string(growth));
  EXPECT_LE(json_charge, kSnapshotLimit);
  EXPECT_LE(encoded.bytes.size(), kSnapshotLimit);
  EXPECT_LE(growth, kWorkspaceLimit);
}

TEST(CompanionDetachBudget, MaximumAudienceConstructionUsesReservedMemory) {
  const char *enabled = std::getenv("CHRONICLES_COMPANION_CAPACITY_STRESS");
  if (!enabled || std::string_view(enabled) != "1") {
    GTEST_SKIP() << "Set CHRONICLES_COMPANION_CAPACITY_STRESS=1 for the capacity stress";
  }

  constexpr std::uint64_t kConnections = 100000;
  auto budget = std::make_shared<companion::CompanionDetachBudget>();
  auto catalog = std::make_shared<companion::CompanionCatalog>(companion_tables());
  auto *vehicle = const_cast<companion::VehicleDefinition *>(catalog->vehicle(kWagonId));
  ASSERT_NE(vehicle, nullptr);
  vehicle->seat_count = 64;
  companion::CompanionWorld world(catalog, {}, budget);
  for (std::uint64_t connection = 1; connection <= kConnections; ++connection) {
    world.attach(participant(connection, static_cast<std::int64_t>(connection), at(0, 0)));
  }
  {
    const auto mount = world.spawn_mount(1, kSummonId, kNow);
    ASSERT_EQ(mount.error, x5::ec::Success);
  }
  {
    const auto wagon = world.spawn_vehicle(1, kWagonId, at(0, 0), 4000, false, kNow);
    ASSERT_EQ(wagon.error, x5::ec::Success);
  }
  {
    const auto mate = world.spawn_mate(1, kMateId, MateRecord{});
    ASSERT_EQ(mate.error, x5::ec::Success);
  }
  ASSERT_EQ(world.unit_count(), 3U);

  auto held = budget->reserve(companion::CompanionDetachBudget::kDefaultLimitBytes - 1);
  ASSERT_TRUE(held);
  bool prepared = false;
  EXPECT_THROW(world.detach_retained(1, [&](const auto &, const auto &) {
    prepared = true;
  }), std::runtime_error);
  EXPECT_FALSE(prepared);
  EXPECT_EQ(world.unit_count(), 3U);
  EXPECT_TRUE(world.ridden_unit(1));
  held.reset();
  ASSERT_EQ(budget->used_bytes(), 0U);

  const auto before = occupied_crt_heap_bytes();
  ASSERT_TRUE(before);
  std::optional<std::size_t> during;
  std::size_t reserved = 0;
  std::shared_ptr<companion::CompanionWorld::RetainedDetach> retained;
  world.detach_retained(1, [&](const auto &, const auto &operation) {
    during = occupied_crt_heap_bytes();
    reserved = budget->used_bytes();
    retained = operation;
  });
  ASSERT_TRUE(during);
  ASSERT_TRUE(retained);
  const auto growth = *during > *before ? *during - *before : 0;
  RecordProperty("crt_heap_growth_bytes", std::to_string(growth));
  RecordProperty("reserved_bytes", std::to_string(reserved));
  EXPECT_GT(reserved, 0U);
  EXPECT_LE(growth, reserved);
  EXPECT_EQ(world.unit_count(), 0U);

  companion::CompanionDetachCursor cursor;
  std::size_t recipients = 0;
  bool complete = false;
  while (!complete) {
    const auto page = retained->page(cursor, 4096);
    recipients += page.recipients.size();
    cursor = page.next;
    complete = page.complete;
  }
  EXPECT_EQ(recipients, 3 * (kConnections - 1));
  retained.reset();
  EXPECT_EQ(budget->used_bytes(), 0U);
}
#endif

TEST_F(CompanionWorldTest, ReentryReplacesObserverGenerationBeforeDetachedSnapshotCanQueue) {
  world_.attach(participant(100, 1, at(0, 0)));
  auto viewer = participant(200, 2, at(1000, 0));
  viewer.view = {10, kMap, 0};
  world_.attach(viewer);
  const auto spawned = world_.spawn_mount(100, kSummonId, kNow);
  const auto old_add = std::ranges::find_if(spawned.update.packets, [](const auto &packet) {
    return packet.connection_id == 200 && packet.opcode == companion::kUnitAddNotification;
  });
  ASSERT_NE(old_add, spawned.update.packets.end());
  ASSERT_TRUE(old_add->source);
  {
    const auto snapshot = world_.snapshot_unit_add(*old_add->source);
    ASSERT_TRUE(snapshot);
    const auto guard = world_.acquire_source_queue_guard(*old_add->source, {}, &*snapshot);
    ASSERT_TRUE(guard);
    guard->mark_ready();
  }
  const companion::CompanionViewToken new_view{11, kMap, 0};
  const auto reentered = world_.reenter_region(200, at(1000, 0), kNow, new_view);
  const auto new_add = std::ranges::find_if(reentered.packets, [](const auto &packet) {
    return packet.connection_id == 200 && packet.opcode == companion::kUnitAddNotification;
  });
  ASSERT_NE(new_add, reentered.packets.end());
  ASSERT_TRUE(new_add->source);
  EXPECT_NE(new_add->source->generation, old_add->source->generation);
  EXPECT_FALSE(world_.acquire_source_queue_guard(*old_add->source));

  std::shared_ptr<companion::CompanionWorld::RetainedDetach> retained;
  world_.detach_retained(100, [&](const auto &, const auto &operation) {
    retained = operation;
  });
  ASSERT_TRUE(retained);
  EXPECT_TRUE(retained->page({}, 5).recipients.empty());
  EXPECT_FALSE(world_.acquire_source_queue_guard(*new_add->source));
}

TEST_F(CompanionWorldTest, TerminalDismountAndRemovalStayValidAfterTheMountIsErased) {
  world_.attach(participant(100, 1, at(0, 0)));
  auto viewer = participant(200, 2, at(1000, 0));
  viewer.view = {10, kMap, 0};
  world_.attach(viewer);
  const auto spawned = world_.spawn_mount(100, kSummonId, kNow);
  const auto added = std::ranges::find_if(spawned.update.packets, [](const auto &packet) {
    return packet.connection_id == 200 && packet.opcode == companion::kUnitAddNotification;
  });
  ASSERT_NE(added, spawned.update.packets.end());
  ASSERT_TRUE(added->source);
  {
    const auto snapshot = world_.snapshot_unit_add(*added->source);
    ASSERT_TRUE(snapshot);
    const auto guard = world_.acquire_source_queue_guard(*added->source, {}, &*snapshot);
    ASSERT_TRUE(guard);
    guard->mark_ready();
  }
  const auto result = world_.get_off(100, std::nullopt, kNow);
  std::vector<companion::OutboundPacket> outgoing;
  for (const auto &packet : result.update.packets) {
    if (packet.connection_id == 200) {
      outgoing.push_back(packet);
    }
  }
  ASSERT_EQ(outgoing.size(), 2U);
  EXPECT_EQ(outgoing[0].opcode, companion::kMountUnitNotification);
  EXPECT_EQ(outgoing[1].opcode, companion::kUnitRemoveNotification);
  for (const auto &packet : outgoing) {
    ASSERT_TRUE(packet.source);
    EXPECT_TRUE(packet.source->terminal);
    EXPECT_TRUE(world_.acquire_source_queue_guard(*packet.source));
  }
}

TEST_F(CompanionWorldTest, InitialSnapshotMustMatchCurrentSourceBeforeItCanBeQueued) {
  world_.attach(participant(100, 1, at(0, 0)));
  auto viewer = participant(200, 2, at(1000, 0));
  viewer.view = {10, kMap, 0};
  world_.attach(viewer);
  const auto spawned = world_.spawn_vehicle(100, kWagonId, at(0, 0), 4000, true, kNow);
  const auto add = std::ranges::find_if(spawned.update.packets, [](const auto &packet) {
    return packet.connection_id == 200 && packet.opcode == companion::kUnitAddNotification;
  });
  ASSERT_NE(add, spawned.update.packets.end());
  ASSERT_TRUE(add->source);
  const auto original = world_.snapshot_unit_add(*add->source);
  ASSERT_TRUE(original);
  world_.move(100, at(4000, 0), nullptr);
  EXPECT_FALSE(world_.acquire_source_queue_guard(*add->source, {}, &*original));
  const auto current = world_.snapshot_unit_add(*add->source);
  ASSERT_TRUE(current);
  EXPECT_NE(*current, *original);
  const auto guard = world_.acquire_source_queue_guard(*add->source, {}, &*current);
  ASSERT_TRUE(guard);
  guard->mark_ready();
}

TEST_F(CompanionWorldTest, OldSightRemovalCannotEraseAReenteredRelation) {
  world_.attach(participant(100, 1, at(0, 0)));
  auto viewer = participant(200, 2, at(1000, 0));
  viewer.view = {10, kMap, 0};
  world_.attach(viewer);
  const auto spawned = world_.spawn_mount(100, kSummonId, kNow);
  const auto add = std::ranges::find_if(spawned.update.packets, [](const auto &packet) {
    return packet.connection_id == 200 && packet.opcode == companion::kUnitAddNotification;
  });
  ASSERT_NE(add, spawned.update.packets.end());
  ASSERT_TRUE(add->source);
  {
    const auto snapshot = world_.snapshot_unit_add(*add->source);
    ASSERT_TRUE(snapshot);
    const auto guard = world_.acquire_source_queue_guard(*add->source, {}, &*snapshot);
    ASSERT_TRUE(guard);
    guard->mark_ready();
  }
  const auto departed = world_.move(200, at(500000, 0), nullptr);
  const auto removal = std::ranges::find_if(departed.packets, [](const auto &packet) {
    return packet.connection_id == 200 && packet.opcode == companion::kUnitRemoveNotification;
  });
  ASSERT_NE(removal, departed.packets.end());
  ASSERT_TRUE(removal->source);
  EXPECT_TRUE(removal->source->terminal);
  world_.move(200, at(1000, 0), nullptr);
  EXPECT_FALSE(world_.acquire_source_queue_guard(*removal->source));
}

TEST_F(CompanionWorldTest, RetainedRemovalCannotEraseAReusedWireUnitId) {
  world_.attach(participant(100, 1, at(0, 0)));
  auto viewer = participant(200, 2, at(1000, 0));
  viewer.view = {10, kMap, 0};
  world_.attach(viewer);
  const auto first = world_.spawn_mount(100, kSummonId, kNow);
  const auto add = std::ranges::find_if(first.update.packets, [](const auto &packet) {
    return packet.connection_id == 200 && packet.opcode == companion::kUnitAddNotification;
  });
  ASSERT_NE(add, first.update.packets.end());
  ASSERT_TRUE(add->source);
  {
    const auto snapshot = world_.snapshot_unit_add(*add->source);
    ASSERT_TRUE(snapshot);
    const auto guard = world_.acquire_source_queue_guard(*add->source, {}, &*snapshot);
    ASSERT_TRUE(guard);
    guard->mark_ready();
  }
  std::shared_ptr<companion::CompanionWorld::RetainedDetach> retained;
  world_.detach_retained(100, [&](const auto &, const auto &operation) {
    retained = operation;
  });
  ASSERT_TRUE(retained);
  const auto page = retained->page({}, 1);
  ASSERT_EQ(page.recipients.size(), 1U);
  EXPECT_TRUE(world_.acquire_source_queue_guard(page.recipients.front().source));

  world_.attach(participant(300, 3, at(0, 0)));
  world_.set_next_unit_sequence_for_test(1);
  const auto second = world_.spawn_mount(300, kSummonId, kNow);
  ASSERT_EQ(second.unit_id, first.unit_id);
  EXPECT_FALSE(world_.acquire_source_queue_guard(page.recipients.front().source));
}

TEST_F(CompanionWorldTest, DamageBreaksTheVehicleAndRepairStopsAtFullHealth) {
  world_.attach(participant(100, 1, at(0, 0)));
  const auto unit = world_.spawn_vehicle(100, kWagonId, at(0, 0), 5000, true, kNow).unit_id;

  EXPECT_EQ(world_.repair_vehicle(100, unit, 1500, kNow).error,
            x5::ec::Interaction_ConditionsNotMatch);
  const auto damaged = world_.damage_vehicle(unit, 2000, kNow);
  ASSERT_EQ(damaged.error, x5::ec::Success);
  EXPECT_EQ(damaged.health, 3000);
  const auto stat = addressed(damaged.update, 100, companion::kChangeStatNotification);
  ASSERT_EQ(stat.size(), 1u);
  EXPECT_EQ(stat.front().at("stat_changes").at(0).at("result"), 3000);

  const auto repaired = world_.repair_vehicle(100, unit, 1500, kNow);
  ASSERT_EQ(repaired.error, x5::ec::Success);
  EXPECT_EQ(repaired.health, 4500);
  EXPECT_EQ(world_.repair_vehicle(100, unit, 1500, kNow).health, 5000);

  const auto broken = world_.damage_vehicle(unit, 999999, kNow);
  EXPECT_TRUE(broken.destroyed);
  const auto dismounts = addressed(broken.update, 100, companion::kMountUnitNotification);
  ASSERT_EQ(dismounts.size(), 1u);
  EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_MountBroken");
  EXPECT_FALSE(world_.vehicle_of(1));
}

TEST_F(CompanionWorldTest, UnitsInAnotherMapOrChannelStayInvisible) {
  world_.attach(participant(100, 1, at(0, 0)));
  world_.attach({200, 2, 2, at(0, 0)});
  WorldPosition other_map = at(0, 0);
  other_map.map_id = 8000;
  world_.attach(participant(300, 3, other_map));

  const auto spawned = world_.spawn_mount(100, kSummonId, kNow);
  EXPECT_TRUE(addressed(spawned.update, 200, companion::kUnitAddNotification).empty());
  EXPECT_TRUE(addressed(spawned.update, 300, companion::kUnitAddNotification).empty());
}

TEST_F(CompanionWorldTest, ARiderEnteringAnotherCopyTakesItsMountAlong) {
  world_.attach(participant(100, 1, at(0, 0)));
  world_.attach(participant(200, 2, at(0, 0)));
  world_.attach({300, 3, 7, at(0, 0)});
  const auto spawned = world_.spawn_mount(100, kSummonId, kNow);
  ASSERT_EQ(addressed(spawned.update, 200, companion::kUnitAddNotification).size(), 1u);

  // The rider and its mount enter copy 7: the shared-map viewer loses them, copy 7 gains them.
  const auto moved = world_.set_channel(100, 7);
  EXPECT_FALSE(addressed(moved, 200, companion::kUnitRemoveNotification).empty());
  EXPECT_EQ(addressed(moved, 300, companion::kUnitAddNotification).size(), 1u);
  EXPECT_TRUE(world_.set_channel(100, 7).packets.empty());
}

TEST_F(CompanionWorldTest, RegionalReentryRetainsTheRiddenMountAndReannouncesIt) {
  world_.attach(participant(100, 1, at(0, 0)));
  const auto spawned = world_.spawn_mount(100, kSummonId, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);

  auto destination = at(5000, 0);
  destination.map_id += 1;
  const auto moved = world_.reenter_region(100, destination, kNow);
  const auto units = addressed(moved, 100, companion::kUnitAddNotification);
  const auto mounts = addressed(moved, 100, companion::kMountUnitNotification);
  ASSERT_EQ(units.size(), 1U);
  ASSERT_EQ(mounts.size(), 1U);
  EXPECT_EQ(units.front().at("unit_id"), std::to_string(spawned.unit_id));
  EXPECT_EQ(mounts.front().at("unit_id"), std::to_string(spawned.unit_id));
  EXPECT_EQ(world_.ridden_unit(100), spawned.unit_id);
  EXPECT_EQ(world_.unit_count(), 1U);
}

TEST_F(CompanionWorldTest, RegionalReentryRetainsTheDrivenWagon) {
  world_.attach(participant(100, 1, at(0, 0)));
  const auto spawned = world_.spawn_vehicle(100, kWagonId, at(0, 0), 5000, true, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);

  auto destination = at(5000, 0);
  destination.map_id += 1;
  const auto moved = world_.reenter_region(100, destination, kNow);
  const auto units = addressed(moved, 100, companion::kUnitAddNotification);
  const auto mounts = addressed(moved, 100, companion::kMountUnitNotification);
  ASSERT_EQ(units.size(), 1U);
  ASSERT_EQ(mounts.size(), 1U);
  const auto wagon = world_.vehicle_of(1);
  ASSERT_TRUE(wagon);
  EXPECT_EQ(wagon->unit_id, spawned.unit_id);
  EXPECT_EQ(wagon->position.map_id, destination.map_id);
  EXPECT_EQ(wagon->health, 5000);
  EXPECT_TRUE(wagon->owner_riding);
  EXPECT_EQ(world_.unit_count(), 1U);
}

TEST_F(CompanionWorldTest, RegionalLoadingHidesTheRiddenUnitUntilArrival) {
  const auto origin = at(0, 0);
  auto destination = origin;
  destination.map_id += 1;
  world_.attach(participant(100, 1, origin));
  world_.attach(participant(200, 2, origin));
  world_.attach(participant(300, 3, destination));
  const auto spawned = world_.spawn_mount(100, kSummonId, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);
  ASSERT_EQ(addressed(spawned.update, 200, companion::kUnitAddNotification).size(), 1U);

  const auto suspended = world_.suspend_region(100, destination);
  EXPECT_EQ(addressed(suspended, 200, companion::kUnitRemoveNotification).size(), 1U);
  EXPECT_TRUE(addressed(suspended, 300, companion::kUnitAddNotification).empty());
  EXPECT_EQ(world_.ridden_unit(100), spawned.unit_id);

  const auto arrived = world_.reenter_region(100, destination, kNow);
  EXPECT_EQ(addressed(arrived, 300, companion::kUnitAddNotification).size(), 1U);
  EXPECT_EQ(addressed(arrived, 100, companion::kMountUnitNotification).size(), 1U);
  EXPECT_EQ(world_.ridden_unit(100), spawned.unit_id);
}

TEST_F(CompanionWorldTest, DisconnectDuringRegionalLoadingKeepsDestinationWagonPosition) {
  const auto origin = at(0, 0);
  auto destination = at(5000, 0);
  destination.map_id += 1;
  world_.attach(participant(100, 1, origin));
  const auto spawned = world_.spawn_vehicle(100, kWagonId, origin, 5000, true, kNow);
  ASSERT_EQ(spawned.error, x5::ec::Success);

  world_.suspend_region(100, destination);
  const auto detached = world_.detach(100);
  ASSERT_EQ(detached.vehicles.size(), 1U);
  EXPECT_EQ(detached.vehicles.front().unit_id, spawned.unit_id);
  EXPECT_EQ(detached.vehicles.front().position, destination);
}

// ---------------------------------------------------------------------------------------------
// Sessions

class CountingProgressAccess final : public ProgressAccess {
public:
  explicit CountingProgressAccess(std::unique_ptr<ProgressAccess> source)
      : source_(std::move(source)) {}

  WorldProgress load() override {
    ++load_count;
    return source_->load();
  }

  std::optional<Character> character() override {
    return source_->character();
  }

  std::optional<std::vector<CharacterItem>>
  commit(const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) override {
    return source_->commit(before, after, trade);
  }

  std::size_t load_count{};

private:
  std::unique_ptr<ProgressAccess> source_;
};

TEST_F(SessionTest, RegistrationItemRegistersTheSummonOnceAndConsumesOneItem) {
  change_progress([](WorldProgress &progress) { progress.stackables[kSummonItem] = 2; });
  companions_->enter_world(participant(100, character_.id, at(0, 0)));

  const auto use = request(companion::kItemUseRequest, {{"item_cms_id", kSummonItem},
                                                        {"count", 1}});
  ASSERT_TRUE(companions_->handles(companion::kItemUseRequest, use));
  const auto used = companions_->receive(companion::kItemUseRequest, use, 4, kNow);
  const auto registered = own(used.own, companion::kRegisterSummonNpcResponse);
  ASSERT_EQ(registered.size(), 1u);
  EXPECT_EQ(registered.front().at("summon_npc_cms_id"), kSummonId);
  const auto items = own(used.own, companion::kItemsUpdateNotification);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items.front().at("stackables").at(0).at("quantity"), 1);
  EXPECT_EQ(stored().registered_summons, std::set<int>{kSummonId});

  const auto again = companions_->receive(companion::kItemUseRequest, use, 5, kNow);
  ASSERT_EQ(own(again.own, companion::kCommonErrorResponse).size(), 1u);
  EXPECT_EQ(stored().stackables.at(kSummonItem), 1);
  EXPECT_FALSE(
      companions_->handles(companion::kItemUseRequest,
                           request(companion::kItemUseRequest, {{"item_cms_id", kRepairKit}})));
}

WorldGameplayRequest registration_request(std::uint32_t sequence = 4) {
  return {20069, sequence,
          request(20069, {{"interaction_anim_id", 1013},
                          {"param", {{"use_item", {{"item_cms_id", kSummonItem}}}}},
                          {"pos", Json::object()},
                          {"packet_seq", {{"seq", "400"}}}})};
}

TEST_F(SessionTest, RegistrationAnimationUsesServerDeadlineAndCommitsOnlyOnce) {
  constexpr std::int64_t started = 100'000'000;
  change_progress([](WorldProgress &progress) { progress.stackables[kSummonItem] = 2; });
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  const auto start = registration_request();
  ASSERT_TRUE(companions_->handles_registration_animation(start.opcode, start.message));
  const auto begun =
      companions_->receive_registration_animation(start, at(0, 0), 305, started, false);
  ASSERT_EQ(own(begun.own, 166).size(), 1);
  EXPECT_TRUE(companions_->registration_busy());
  EXPECT_EQ(stored().stackables.at(kSummonItem), 2);

  companions_->receive_registration_animation(start, at(0, 0), 305, started + 10'000'000, false);
  EXPECT_EQ(companions_->registration_deadline(), started + 3408 * 10000LL);
  const WorldGameplayRequest execute{20070, 5, request(20070, {{"interaction_anim_id", 1013}})};
  companions_->receive_registration_animation(execute, at(0, 0), 305, started + 1, false);
  EXPECT_EQ(stored().stackables.at(kSummonItem), 2);

  const auto applied = companions_->advance_registration(started + 3408 * 10000LL, at(0, 0), false);
  ASSERT_EQ(own(applied.own, companion::kRegisterSummonNpcResponse).size(), 1);
  ASSERT_EQ(own(applied.own, companion::kItemsUpdateNotification).size(), 1);
  EXPECT_EQ(stored().stackables.at(kSummonItem), 1);
  EXPECT_EQ(stored().registered_summons, std::set<int>{kSummonId});
  const auto committed_revision = stored().revision;
  EXPECT_TRUE(
      companions_->advance_registration(started + 3500 * 10000LL, at(0, 0), false).own.empty());
  const auto ended = companions_->advance_registration(started + 4400 * 10000LL, at(0, 0), false);
  ASSERT_EQ(own(ended.own, 167).size(), 1);
  EXPECT_FALSE(companions_->registration_busy());
  EXPECT_FALSE(companions_->registration_deadline());
  EXPECT_EQ(stored().revision, committed_revision);
}

TEST_F(SessionTest, CancelingOrMovingBeforeRegistrationDoesNotSpendTheItem) {
  constexpr std::int64_t started = 100'000'000;
  change_progress([](WorldProgress &progress) { progress.stackables[kSummonItem] = 1; });
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  companions_->receive_registration_animation(registration_request(), at(0, 0), 305, started,
                                              false);
  const WorldGameplayRequest end{
      20071, 5, request(20071, {{"end_interaction_anim", {{"interaction_anim_id", 1013}}}})};
  EXPECT_EQ(
      own(companions_->receive_registration_animation(end, at(0, 0), 305, started + 1, false).own,
          167)
          .size(),
      1);
  EXPECT_FALSE(companions_->registration_busy());
  EXPECT_EQ(stored().stackables.at(kSummonItem), 1);

  companions_->receive_registration_animation(registration_request(), at(0, 0), 305, started,
                                              false);
  EXPECT_EQ(
      own(companions_->advance_registration(started + 3408 * 10000LL, at(2000, 0), false).own, 167)
          .size(),
      1);
  EXPECT_FALSE(companions_->registration_busy());
  EXPECT_TRUE(stored().registered_summons.empty());
}

TEST_F(SessionTest, DirectItemUseCannotBypassAPendingRegistrationAnimation) {
  constexpr std::int64_t started = 100'000'000;
  change_progress([](WorldProgress &progress) { progress.stackables[kSummonItem] = 2; });
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  companions_->receive_registration_animation(registration_request(), at(0, 0), 305, started,
                                              false);
  ASSERT_TRUE(companions_->registration_busy());

  const auto use =
      request(companion::kItemUseRequest, {{"item_cms_id", kSummonItem}, {"count", 1}});
  const auto refused = companions_->receive(companion::kItemUseRequest, use, 6, kNow);
  const auto errors = own(refused.own, companion::kCommonErrorResponse);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors.front().at("ec"), "InvalidState");
  EXPECT_EQ(stored().stackables.at(kSummonItem), 2);
  EXPECT_TRUE(stored().registered_summons.empty());

  // The animation still owns the registration and completes it exactly once.
  const auto applied = companions_->advance_registration(started + 3408 * 10000LL, at(0, 0), false);
  ASSERT_EQ(own(applied.own, companion::kRegisterSummonNpcResponse).size(), 1u);
  EXPECT_EQ(stored().stackables.at(kSummonItem), 1);
  EXPECT_EQ(stored().registered_summons, std::set<int>{kSummonId});

  // Late execute/end reports after completion belong to no pending registration.
  companions_->advance_registration(started + 4400 * 10000LL, at(0, 0), false);
  const WorldGameplayRequest late_end{
      20071, 7, request(20071, {{"end_interaction_anim", {{"interaction_anim_id", 1013}}}})};
  EXPECT_FALSE(companions_->handles_registration_animation(late_end.opcode, late_end.message));
}

TEST_F(SessionTest, RegistrationRechecksOwnershipAndLifeAtExecution) {
  constexpr std::int64_t started = 100'000'000;
  change_progress([](WorldProgress &progress) { progress.stackables[kSummonItem] = 1; });
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  companions_->receive_registration_animation(registration_request(), at(0, 0), 305, started,
                                              false);
  change_progress([](WorldProgress &progress) { progress.stackables[kSummonItem] = 0; });
  const auto result = companions_->advance_registration(started + 3408 * 10000LL, at(0, 0), false);
  const auto errors = own(result.own, 167);
  ASSERT_EQ(errors.size(), 1);
  EXPECT_EQ(errors.front().at("ec"), "ItemCountError");
  EXPECT_FALSE(companions_->registration_busy());
  EXPECT_TRUE(stored().registered_summons.empty());
}

TEST_F(SessionTest, RegistrationRefusesForgedAnimationAndLeavesNoActionAfterDisconnect) {
  constexpr std::int64_t started = 100'000'000;
  change_progress([](WorldProgress &progress) { progress.stackables[kSummonItem] = 1; });
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  auto invalid = registration_request();
  invalid.message["interaction_anim_id"] = 100;
  const auto refused =
      companions_->receive_registration_animation(invalid, at(0, 0), 305, started, false);
  EXPECT_EQ(own(refused.own, 165).size(), 1);
  EXPECT_FALSE(companions_->registration_busy());
  companions_->receive_registration_animation(registration_request(), at(0, 0), 305, started,
                                              false);
  companions_->leave();
  EXPECT_FALSE(companions_->registration_busy());
  EXPECT_TRUE(companions_->advance_registration(started + 50'000'000, at(0, 0), false).own.empty());
  EXPECT_EQ(stored().stackables.at(kSummonItem), 1);
}

TEST_F(SessionTest, OnlyARegisteredSummonCanBeSummonedAndWatchersSeeTheRider) {
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  watcher_->enter_world(participant(200, other_.id, at(3000, 0)));
  const auto summon = request(companion::kSpawnSummonNpcRequest,
                              {{"summon_npc_cms_id", kSummonId}});

  const auto refused = companions_->receive(companion::kSpawnSummonNpcRequest, summon, 1, kNow);
  const auto refusal = own(refused.own, companion::kSpawnSummonNpcResponse);
  ASSERT_EQ(refusal.size(), 1u);
  EXPECT_EQ(refusal.front().at("ec"), "Summon_NotRegistered");

  change_progress([](WorldProgress &progress) { progress.registered_summons.insert(kSummonId); });
  const auto summoned = companions_->receive(companion::kSpawnSummonNpcRequest, summon, 2, kNow);
  const auto response = own(summoned.own, companion::kSpawnSummonNpcResponse);
  ASSERT_EQ(response.size(), 1u);
  EXPECT_FALSE(response.front().contains("ec"));
  EXPECT_EQ(addressed(summoned.broadcast, 200, companion::kMountUnitNotification).size(), 1u);
  EXPECT_TRUE(companions_->riding());

  const auto off = companions_->receive(companion::kGetOffUnitRequest,
                                        request(companion::kGetOffUnitRequest, Json::object()),
                                        3, kNow);
  EXPECT_EQ(addressed(off.broadcast, 200, companion::kUnitRemoveNotification).size(), 1u);
  EXPECT_FALSE(companions_->riding());
}

TEST_F(SessionTest, PlayerMountLendsTheDefaultHorseAndPlayerDismountRemovesIt) {
  EXPECT_NE(companions_->player_mount(kNow).reply.find("Enter the game world"), std::string::npos);

  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  watcher_->enter_world(participant(200, other_.id, at(3000, 0)));
  EXPECT_EQ(companions_->player_dismount(kNow).reply, "You are not riding anything.");

  // A character without a registered summon borrows the catalog default; nothing is registered.
  const auto mounted = companions_->player_mount(kNow);
  EXPECT_EQ(mounted.reply, "Your horse is ready. Type !dismount to get off.");
  EXPECT_TRUE(companions_->riding());
  const auto seats = addressed(mounted.result.broadcast, 200, companion::kMountUnitNotification);
  ASSERT_EQ(seats.size(), 1u);
  EXPECT_EQ(seats.front().at("slot"), 0);
  EXPECT_TRUE(stored().registered_summons.empty());

  const auto again = companions_->player_mount(kNow);
  EXPECT_NE(again.reply.find("already riding"), std::string::npos);
  EXPECT_TRUE(again.result.broadcast.packets.empty());

  const auto dismounted = companions_->player_dismount(kNow);
  EXPECT_EQ(dismounted.reply, "You got off.");
  EXPECT_FALSE(companions_->riding());
  const auto unseats =
      addressed(dismounted.result.broadcast, 200, companion::kMountUnitNotification);
  ASSERT_EQ(unseats.size(), 1u);
  EXPECT_EQ(unseats.front().at("dismount_reason"), "EX5DismountReason_User");
  EXPECT_EQ(addressed(dismounted.result.broadcast, 200, companion::kUnitRemoveNotification).size(),
            1u);
}

TEST_F(SessionTest, PlayerMountRefusesDeadCharactersAndDismountLeavesAWagonParked) {
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  watcher_->enter_world(participant(200, other_.id, at(3000, 0)));

  const auto vehicle = world_->spawn_vehicle(100, kWagonId, at(0, 0), 5000, true, kNow);
  ASSERT_EQ(vehicle.error, x5::ec::Success);
  ASSERT_TRUE(companions_->riding());
  const auto off_wagon = companions_->player_dismount(kNow);
  EXPECT_EQ(off_wagon.reply, "You got off.");
  EXPECT_FALSE(companions_->riding());
  EXPECT_TRUE(
      addressed(off_wagon.result.broadcast, 200, companion::kUnitRemoveNotification).empty());
  EXPECT_TRUE(world_->vehicle_of(character_.id).has_value());

  ASSERT_TRUE(store_.character_state->commit_world_death(kAccountId, character_.id, kWorldId,
                                                         stored().revision, {}));
  companions_->observe_committed_life(true);
  const auto dead = companions_->player_mount(kNow);
  EXPECT_EQ(dead.reply, "You cannot mount while dead.");
  EXPECT_TRUE(dead.result.broadcast.packets.empty());
  EXPECT_FALSE(companions_->riding());
}

TEST_F(SessionTest, DeathDismountsOnceAndDeadPlayersCannotBoard) {
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  watcher_->enter_world(participant(200, other_.id, at(3000, 0)));
  change_progress([](WorldProgress &progress) {
    progress.registered_summons.insert(kSummonId);
  });
  companions_->receive(
      companion::kSpawnSummonNpcRequest,
      request(companion::kSpawnSummonNpcRequest, {{"summon_npc_cms_id", kSummonId}}), 1, kNow);
  ASSERT_TRUE(companions_->riding());
  EXPECT_TRUE(companions_->dismount_if_dead(kNow).broadcast.packets.empty());

  ASSERT_TRUE(store_.character_state->commit_world_death(kAccountId, character_.id, kWorldId,
                                                         stored().revision, {}));
  companions_->observe_committed_life(true);
  const auto death = companions_->dismount_if_dead(kNow);
  const auto dismounts = addressed(death.broadcast, 200, companion::kMountUnitNotification);
  ASSERT_EQ(dismounts.size(), 1u);
  EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_RiderKilled");
  EXPECT_FALSE(companions_->riding());
  EXPECT_TRUE(companions_->dismount_if_dead(kNow).broadcast.packets.empty());

  const auto refused = companions_->receive(companion::kRideUnitRequest,
                                            {{"mount_unit_id", "1"}, {"slot", 0}}, 2, kNow);
  ASSERT_EQ(refused.own.size(), 1u);
  EXPECT_EQ(refused.own.front().message.at("ec"), "InvalidState");
  EXPECT_TRUE(refused.broadcast.packets.empty());
}

TEST_F(SessionTest, DrivingAndRepeatedDeathChecksDoNotReloadProgress) {
  auto progress =
      std::make_unique<CountingProgressAccess>(std::make_unique<RepositoryProgressAccess>(
          *store_.characters, *store_.character_state, kAccountId, character_.id, kWorldId));
  const auto *loads = progress.get();
  companions_ = std::make_unique<companion::CompanionSession>(
      companion::CompanionServices{world_, companion_catalog(), item_catalog()},
      std::move(progress), 100, character_.id);
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  ASSERT_EQ(loads->load_count, 1U);
  const auto vehicle = world_->spawn_vehicle(100, kWagonId, at(0, 0), 5000, true, kNow);
  ASSERT_EQ(vehicle.error, x5::ec::Success);
  ASSERT_TRUE(companions_->riding());

  // Frame completion and combat timers both perform this check. With PostgreSQL, each old
  // check opened a connection and queried the entire character, even for an ignored move.
  for (int step = 0; step < 1000; ++step) {
    companions_->move(at(step, 0), nullptr);
    EXPECT_TRUE(companions_->dismount_if_dead(kNow + step).broadcast.packets.empty());
    EXPECT_TRUE(companions_->dismount_if_dead(kNow + step).broadcast.packets.empty());
  }
  EXPECT_EQ(loads->load_count, 1U);
  EXPECT_TRUE(companions_->riding());

  ASSERT_TRUE(store_.character_state->commit_world_death(kAccountId, character_.id, kWorldId,
                                                         stored().revision, {}));
  companions_->observe_committed_life(true);
  const auto death = companions_->dismount_if_dead(kNow);
  const auto dismounts = addressed(death.broadcast, 100, companion::kMountUnitNotification);
  ASSERT_EQ(dismounts.size(), 1U);
  EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_RiderKilled");
  EXPECT_FALSE(companions_->riding());
  EXPECT_TRUE(companions_->dismount_if_dead(kNow).broadcast.packets.empty());
  EXPECT_EQ(loads->load_count, 1U);

  ASSERT_TRUE(store_.character_state->commit_world_revival(kAccountId, character_.id, kWorldId,
                                                           stored().revision, at(999, 0)));
  companions_->observe_committed_life(false);
  ASSERT_EQ(world_->ride(100, vehicle.unit_id, 0, kNow).error, x5::ec::Success);
  EXPECT_TRUE(companions_->dismount_if_dead(kNow).broadcast.packets.empty());
  EXPECT_TRUE(companions_->riding());
  EXPECT_EQ(loads->load_count, 1U);
}

TEST_F(SessionTest, ReentryRefreshesCommittedLifeFromTheNewEntrySnapshot) {
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  companions_->leave();
  ASSERT_TRUE(store_.character_state->commit_world_death(kAccountId, character_.id, kWorldId,
                                                         stored().revision, {}));
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  const auto vehicle = world_->spawn_vehicle(100, kWagonId, at(0, 0), 5000, true, kNow);
  ASSERT_EQ(vehicle.error, x5::ec::Success);

  const auto death = companions_->dismount_if_dead(kNow);
  const auto dismounts = addressed(death.broadcast, 100, companion::kMountUnitNotification);
  ASSERT_EQ(dismounts.size(), 1U);
  EXPECT_EQ(dismounts.front().at("dismount_reason"), "EX5DismountReason_RiderKilled");
  EXPECT_FALSE(companions_->riding());
}

TEST_F(SessionTest, EntrySnapshotsListRegisteredSummonsAndMatesAsAway) {
  change_progress([](WorldProgress &progress) {
    progress.registered_summons.insert(kSummonId);
    progress.mates[kMateId] = MateRecord{static_cast<int>(companion::MateState::spawned), 0, 120};
  });

  const auto entered = companions_->enter_world(participant(100, character_.id, at(0, 0)));
  const auto summons = own(entered.own, companion::kWorldCharSummonNpcsNotification);
  ASSERT_EQ(summons.size(), 1u);
  EXPECT_EQ(summons.front().at("summon_npc_ids"), Json::array({kSummonId}));
  const auto mates = own(entered.own, companion::kWorldCharMatesNotification);
  ASSERT_EQ(mates.size(), 1u);
  EXPECT_EQ(mates.front().at("mate_infos").at(0).at("state_type"), "EX5MateStateType_BeAway");
  EXPECT_EQ(mates.front().at("mate_infos").at(0).at("intimate"), 120);
  EXPECT_EQ(stored().mates.at(kMateId).state, static_cast<int>(companion::MateState::be_away));
}

TEST_F(SessionTest, MateLifecycleSpawnsInvitesDespawnsAndSettlesOnExit) {
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  const auto spawn = request(companion::kSpawnMateNpcRequest,
                             {{"mate_npc_cms_id", kMateId}, {"housing_lot_number", 0}});

  const auto unregistered = companions_->receive(companion::kSpawnMateNpcRequest, spawn, 1, kNow);
  ASSERT_EQ(own(unregistered.own, companion::kSystemMessageNotification).size(), 1u);
  EXPECT_EQ(own(unregistered.own, companion::kSystemMessageNotification).front().at("key"),
            "ESystemMessage_SM_HOUSEMATE_SPAWN_FAIL_NOMATE");

  const auto commands = companion::parse_companion_gm_request(
      20009, request(20009, {{"text", "register_mate_npc 2"}}));
  ASSERT_TRUE(commands);
  const auto registered = companions_->gm_command(*commands, at(0, 0), kNow);
  ASSERT_EQ(own(registered.own, companion::kRegisterMateNpcResponse).size(), 1u);

  const auto in_house = companions_->receive(
      companion::kSpawnMateNpcRequest,
      request(companion::kSpawnMateNpcRequest,
              {{"mate_npc_cms_id", kMateId}, {"housing_lot_number", 101}}),
      2, kNow);
  EXPECT_EQ(own(in_house.own, companion::kSystemMessageNotification).front().at("key"),
            "ESystemMessage_SM_HOUSEMATE_SPAWN_FAIL_SPACE");

  const auto spawned = companions_->receive(companion::kSpawnMateNpcRequest, spawn, 3, kNow);
  const auto state = own(spawned.own, companion::kChangedMateStateResponse);
  ASSERT_EQ(state.size(), 1u);
  EXPECT_EQ(state.front().at("state_type"), 2);
  const auto mate_unit = state.front().at("mate_npc_unit_id").get<std::string>();
  const auto adds = addressed(spawned.broadcast, 100, companion::kUnitAddNotification);
  ASSERT_EQ(adds.size(), 1u);
  EXPECT_EQ(adds.front().at("nub").at("npc").at("mate").at("mate_npc_cms_id"), kMateId);
  EXPECT_EQ(stored().mates.at(kMateId).state, 2);

  const auto limit = companions_->receive(companion::kSpawnMateNpcRequest, spawn, 4, kNow);
  EXPECT_EQ(own(limit.own, companion::kSystemMessageNotification).front().at("key"),
            "ESystemMessage_SM_HOUSEMATE_SPAWN_FAIL_LIMIT");

  const auto invite = request(companion::kInviteMateNpcRequest, {{"mate_npc_unit_id", mate_unit}});
  const auto invited = companions_->receive(companion::kInviteMateNpcRequest, invite, 5, kNow);
  EXPECT_EQ(own(invited.own, companion::kChangedMateStateResponse).front().at("state_type"), 3);
  const auto twice = companions_->receive(companion::kInviteMateNpcRequest, invite, 6, kNow);
  EXPECT_EQ(own(twice.own, companion::kCommonErrorResponse).front().at("ec"),
            "Housing_AlreadyInvitedMate");

  const auto intimacy = companion::parse_companion_gm_request(
      20008, request(20008, {{"cmds", Json::array({{{"add_mate_intimate",
                                                     {{"cms_id", kMateId}, {"value", 5000}}}}})}}));
  ASSERT_TRUE(intimacy);
  const auto raised = companions_->gm_command(*intimacy, at(0, 0), kNow);
  EXPECT_EQ(own(raised.own, companion::kChangedMateIntimateResponse).front().at("result_intimate"),
            1000);

  const auto despawned = companions_->receive(
      companion::kDespawnMateNpcRequest,
      request(companion::kDespawnMateNpcRequest, {{"mate_npc_unit_id", mate_unit}}), 7, kNow);
  EXPECT_EQ(own(despawned.own, companion::kChangedMateStateResponse).front().at("state_type"), 1);
  EXPECT_EQ(addressed(despawned.broadcast, 100, companion::kUnitRemoveNotification).size(), 1u);

  companions_->receive(companion::kSpawnMateNpcRequest, spawn, 8, kNow);
  companions_->leave();
  EXPECT_EQ(stored().mates.at(kMateId).state, 1);
  EXPECT_EQ(world_->unit_count(), 0u);
}

TEST_F(SessionTest, RepairingTheOwnedWagonSpendsTheKitAndRestoresHealth) {
  change_progress([](WorldProgress &progress) { progress.stackables[kRepairKit] = 1; });
  companions_->enter_world(participant(100, character_.id, at(0, 0)));
  const auto unit = world_->spawn_vehicle(100, kWagonId, at(1000, 0), 2000, false, kNow).unit_id;

  const auto repair = request(companion::kInteractUnitRequest,
                              {{"target_unit_id", std::to_string(unit)},
                               {"param", {{"interaction_id", 62}}}});
  ASSERT_TRUE(companions_->handles(companion::kInteractUnitRequest, repair));
  const auto repaired = companions_->receive(companion::kInteractUnitRequest, repair, 9, kNow);
  const auto reply = own(repaired.own, companion::kInteractUnitResponse);
  ASSERT_EQ(reply.size(), 1u);
  EXPECT_FALSE(reply.front().contains("ec"));
  EXPECT_EQ(world_->vehicle_of(character_.id)->health, 3500);
  EXPECT_EQ(stored().stackables.at(kRepairKit), 0);

  const auto no_kit = companions_->receive(companion::kInteractUnitRequest, repair, 10, kNow);
  EXPECT_EQ(own(no_kit.own, companion::kInteractUnitResponse).front().at("ec"),
            "Interaction_ConditionsNotMatch");
}

TEST(CompanionGm, ParsesOnlyRequestsMadeEntirelyOfCompanionCommands) {
  const auto text = companion::parse_companion_gm_request(
      20009, request(20009, {{"text", "spawn_operable 200"}}));
  ASSERT_TRUE(text);
  EXPECT_EQ(text->front().kind, companion::CompanionGmKind::spawn_operable);
  EXPECT_EQ(text->front().cms_id, 200);

  EXPECT_FALSE(
      companion::parse_companion_gm_request(20009, request(20009, {{"text", "item_add 1 1"}})));
  EXPECT_FALSE(companion::parse_companion_gm_request(
      20008,
      request(20008, {{"cmds", Json::array({{{"register_summon_npc", {{"cms_id", 2001}}}},
                                            {{"item_add", {{"cms_id", 1}, {"count", 1}}}}})}})));
}

TEST(CompanionGm, CarIsTheOnlyCommandEveryPlayerMayRun) {
  const auto car = companion::parse_companion_gm_request(
      20009, request(20009, {{"text", "car"}}));
  ASSERT_TRUE(car);
  ASSERT_EQ(car->size(), 1U);
  EXPECT_EQ(car->front().kind, companion::CompanionGmKind::spawn_operable);
  EXPECT_EQ(car->front().cms_id, companion::kDefaultCarVehicleId);
  EXPECT_TRUE(car->front().player_command);

  // The long form and every other companion command stay game-master only.
  const auto vehicle = companion::parse_companion_gm_request(
      20009, request(20009, {{"text", "spawn_vehicle 200"}}));
  ASSERT_TRUE(vehicle);
  EXPECT_FALSE(vehicle->front().player_command);
  const auto summon = companion::parse_companion_gm_request(
      20009, request(20009, {{"text", "register_summon_npc 2001"}}));
  ASSERT_TRUE(summon);
  EXPECT_FALSE(summon->front().player_command);

  // A car command takes no argument, so it cannot be abused to pick another vehicle.
  EXPECT_FALSE(companion::parse_companion_gm_request(
      20009, request(20009, {{"text", "car 300"}})));
}

// ---------------------------------------------------------------------------------------------
// Trade runs



















// ---------------------------------------------------------------------------------------------
// Persistence

TEST(CompanionStorage, ProgressRecordsRoundTripAndRegistrationsCannotBeRemoved) {
  MemoryRepositories store;
  Character created;
  created.account_id = kAccountId;
  created.name = "Keeper";
  const auto character = store.characters->create(created);
  const auto before =
      store.character_state->load_world_progress(kAccountId, character.id, kWorldId);

  auto after = before;
  after.revision = before.revision + 1;
  after.registered_summons = {kSummonId};
  after.mates[kMateId] = MateRecord{1, 0, 10};
  after.trade_run.phase = TradeRunPhase::running;
  after.trade_run.goods = {{kItemRewardGood, kOriginPost, kNow}};
  after.trade_run.wagon_position = WorldPosition{kMap, 1, 2, 3, 4.0F};
  ASSERT_TRUE(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, before,
                                                         after));
  EXPECT_EQ(store.character_state->load_world_progress(kAccountId, character.id, kWorldId), after);

  auto removed = after;
  removed.revision = after.revision + 1;
  removed.registered_summons.clear();
  EXPECT_THROW(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, after,
                                                          removed),
               std::invalid_argument);

  auto idle_with_goods = after;
  idle_with_goods.revision = after.revision + 1;
  idle_with_goods.trade_run.phase = TradeRunPhase::idle;
  EXPECT_THROW(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, after,
                                                          idle_with_goods),
               std::invalid_argument);
}

// Needs the disposable cluster migrated to schema version 19 (never port 5433).
TEST(CompanionStorage, PostgresSummonsMatesAndTradeRunsRoundTripAndConflictsRollBack) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  auto store = make_postgres_character_repositories(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account owner;
  owner.name = "rider_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(store.accounts->add_account(owner));
  const auto account = store.accounts->account(owner.name);
  ASSERT_TRUE(account);
  Character created;
  created.account_id = account->id;
  created.name = "Rider" + suffix.substr(suffix.size() - 8);
  const auto character = store.characters->create(created);

  const auto before =
      store.character_state->load_world_progress(account->id, character.id, kWorldId);
  auto after = before;
  after.revision = before.revision + 1;
  after.gold = 500;
  after.registered_summons = {kSummonId, 101};
  after.mates[kMateId] = MateRecord{2, 0, 40};
  after.trade_run.phase = TradeRunPhase::parked;
  after.trade_run.start_post_id = kOriginPost;
  after.trade_run.parked_post_id = kDestinationPost;
  after.trade_run.vehicle_cms_id = kWagonId;
  after.trade_run.goods = {{kItemRewardGood, kOriginPost, kNow},
                           {kItemRewardGood, kOriginPost, kNow + 1}};
  after.trade_run.wagon_position = WorldPosition{kMap, -848599, 3700201, -628329, 12.5F};
  after.trade_run.wagon_health = 4321;
  ASSERT_TRUE(store.character_state->save_world_progress(account->id, character.id, kWorldId,
                                                         before, after));
  EXPECT_EQ(store.character_state->load_world_progress(account->id, character.id, kWorldId), after);

  // A stale revision writes nothing, including the trade run row.
  auto stale = before;
  stale.revision = before.revision + 1;
  stale.trade_run = TradeRunRecord{};
  EXPECT_FALSE(store.character_state->save_world_progress(account->id, character.id, kWorldId,
                                                          before, stale));
  EXPECT_EQ(store.character_state->load_world_progress(account->id, character.id, kWorldId), after);

  // Ending the run clears the goods and the wagon in place.
  auto ended = after;
  ended.revision = after.revision + 1;
  ended.trade_run = TradeRunRecord{};
  ended.trade_run.last_wagon_spawn_epoch_ticks = kNow;
  ended.mates[kMateId].state = 1;
  ASSERT_TRUE(store.character_state->save_world_progress(account->id, character.id, kWorldId, after,
                                                         ended));
  EXPECT_EQ(store.character_state->load_world_progress(account->id, character.id, kWorldId), ended);
}

// ---------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------
// Exported catalog (opt-in: set CHRONICLES_CLIENT_CATALOG to data/generated/catalog). A skip is
// not evidence.

TEST(CompanionCatalog, ExportedCatalogDefinesTheDemoMountsWagonAndTradeRoutes) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }
  ClientCatalog catalog(catalog_path);
  const auto companions = companion::CompanionCatalog::load(catalog);
  EXPECT_EQ(companions->summon_count(), 10u);
  EXPECT_EQ(companions->summon_registered_by_item(2001), 2001);
  EXPECT_EQ(companions->summon_registered_by_item(125), 101);
  ASSERT_NE(companions->vehicle(200), nullptr);
  EXPECT_EQ(companions->vehicle(200)->max_load_capacity, 7);
  EXPECT_EQ(companions->vehicle(200)->max_health, 5000000);
  ASSERT_NE(companions->repair_task(62), nullptr);
  EXPECT_EQ(companions->repair_task(62)->consumed_item_cms_id, 140);
  EXPECT_EQ(companions->mate_count(), 2u);

  const auto trade = trade::TradeCatalog::load(catalog, catalog_path);
  ASSERT_NE(trade->post(1000), nullptr);
  EXPECT_TRUE(trade->post(1000)->placed);
  EXPECT_TRUE(trade->post(1000)->map_ids.contains(9012));
  EXPECT_TRUE(trade->post(1100)->map_ids.contains(9012));
  EXPECT_FALSE(trade->post(4000)->placed);
  EXPECT_TRUE(trade->is_trade_management_dialogue(100097));
  EXPECT_EQ(trade->settings().voucher_own_limit, 10);
  EXPECT_EQ(trade->settings().wagon_respawn_cooldown_ms, 30000);
  const auto *route = trade->purchase_reward(1000, 1102, 1100);
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(route->item_cms_id, 60007);
  EXPECT_EQ(route->item_amount, 6);
  const auto message = wire(trade::kTradeMarketPriceResponse,
                            trade::market_price_message(*trade, kNow));
  EXPECT_GE(message.at("trading_posts").size(), 2u);
}

} // namespace
} // namespace chronicles
