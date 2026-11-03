#include "world/features/combat/death_reconciliation.hpp"

#include <gtest/gtest.h>

namespace chronicles::combat {
namespace {

TEST(DeathReconciliation, FenceDistinguishesCommittedAndUncommittedWear) {
  WorldProgress before;
  before.revision = 10;
  CharacterItem item;
  item.id = 1;
  item.cms_id = 100;
  item.equip_slot = 2;
  item.durability = 50;
  const std::vector<CharacterItem> equipped{item};
  const std::vector<EquippedDurabilityChange> changes{{1, 100, 2, 50, 47}};

  auto fenced = before;
  fenced.revision = 11;
  EXPECT_EQ(classify_fenced_death(before, equipped, changes, fenced, equipped),
            DeathReconciliationOutcome::not_committed);

  fenced.revision = 12;
  fenced.dead = true;
  auto worn = item;
  worn.durability = 47;
  EXPECT_EQ(classify_fenced_death(before, equipped, changes, fenced, {worn}),
            DeathReconciliationOutcome::committed);
  EXPECT_EQ(classify_fenced_death(before, equipped, changes, fenced, equipped),
            DeathReconciliationOutcome::conflict);
}

TEST(DeathReconciliation, EmptyAndUnchangedEquipmentUseTheDeadFlagAndConfirmedFence) {
  WorldProgress before;
  before.revision = 5;
  auto fenced = before;
  fenced.revision = 6;
  EXPECT_EQ(classify_fenced_death(before, {}, {}, fenced, {}),
            DeathReconciliationOutcome::not_committed);
  fenced.dead = true;
  EXPECT_EQ(classify_fenced_death(before, {}, {}, fenced, {}),
            DeathReconciliationOutcome::conflict);
  fenced.revision = 7;
  EXPECT_EQ(classify_fenced_death(before, {}, {}, fenced, {}),
            DeathReconciliationOutcome::committed);

  CharacterItem item;
  item.id = 1;
  item.cms_id = 100;
  item.equip_slot = 2;
  item.durability = 0;
  const std::vector<EquippedDurabilityChange> unchanged{{1, 100, 2, 0, 0}};
  EXPECT_EQ(classify_fenced_death(before, {item}, unchanged, fenced, {item}),
            DeathReconciliationOutcome::committed);
}

TEST(DeathReconciliation, UnexpectedProgressEquipmentOrPlanFailsClosed) {
  WorldProgress before;
  before.revision = 1;
  CharacterItem item;
  item.id = 1;
  item.cms_id = 100;
  item.equip_slot = 2;
  item.durability = 50;
  const std::vector<EquippedDurabilityChange> changes{{1, 100, 2, 50, 47}};
  auto fenced = before;
  fenced.revision = 3;
  fenced.dead = true;
  auto worn = item;
  worn.durability = 47;

  ++fenced.gold;
  EXPECT_EQ(classify_fenced_death(before, {item}, changes, fenced, {worn}),
            DeathReconciliationOutcome::conflict);
  --fenced.gold;
  worn.equip_slot = 3;
  EXPECT_EQ(classify_fenced_death(before, {item}, changes, fenced, {worn}),
            DeathReconciliationOutcome::conflict);
  EXPECT_EQ(classify_fenced_death(before, {item, item}, {changes[0], changes[0]}, fenced,
                                  {worn, worn}),
            DeathReconciliationOutcome::conflict);
  EXPECT_EQ(classify_fenced_death(before, {item}, changes, fenced, {}),
            DeathReconciliationOutcome::conflict);
}

} // namespace
} // namespace chronicles::combat
