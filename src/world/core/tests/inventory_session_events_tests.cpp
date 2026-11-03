#include "world/core/session_event_bus.hpp"

#include "persistence/character_state_types.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace chronicles {
namespace {

TEST(InventorySessionEvents, EquipmentRevisionPublicationIsSynchronousAndKeepsSubscriberOrder) {
  std::vector<std::int64_t> revisions;
  SessionEventBus::Subscriptions subscriptions;
  subscriptions.equipment_revision.push_back([&](const PlayerEquipmentRevisionCommitted &event) {
    revisions.push_back(event.revision);
  });
  subscriptions.equipment_revision.push_back([&](const PlayerEquipmentRevisionCommitted &event) {
    revisions.push_back(event.revision + 1);
  });
  SessionEventBus bus(std::move(subscriptions));
  bus.publish(PlayerEquipmentRevisionCommitted{5});
  EXPECT_EQ(revisions, (std::vector<std::int64_t>{5, 6}));
}

TEST(InventorySessionEvents, WorldLootPublicationBorrowsTheCommittedRevisionWithoutMutation) {
  WorldProgress progress;
  progress.revision = 5;
  progress.utility_slots[0] = {3001, 2};
  const auto before = progress;
  bool called{};
  SessionEventBus::Subscriptions subscriptions;
  subscriptions.world_loot_progress.push_back([&](const PlayerWorldLootProgressCommitted &event) {
    EXPECT_EQ(&event.progress, &progress);
    EXPECT_EQ(event.progress.revision, 5);
    called = true;
  });
  SessionEventBus bus(std::move(subscriptions));
  bus.publish(PlayerWorldLootProgressCommitted{progress});
  EXPECT_TRUE(called);
  EXPECT_EQ(progress, before);
}

} // namespace
} // namespace chronicles
