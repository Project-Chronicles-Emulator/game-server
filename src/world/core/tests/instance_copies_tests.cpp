#include "world/core/instance_copies.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>

namespace chronicles {
namespace {

constexpr int kDungeon = 10111;
constexpr int kField = 9012;

struct Counter {
  int value{};
};

TEST(InstanceCopies, SharedMapsHaveNoCopy) {
  InstanceCopies copies({kDungeon});

  EXPECT_FALSE(copies.copies(kField));
  EXPECT_EQ(copies.enter(kField, 7, std::nullopt), nullptr);
}

TEST(InstanceCopies, PartyMembersShareOneCopyAndSoloCharactersGetTheirOwn) {
  InstanceCopies copies({kDungeon});

  const auto leader = copies.enter(kDungeon, 7, 3);
  const auto member = copies.enter(kDungeon, 8, 3);
  const auto solo = copies.enter(kDungeon, 9, std::nullopt);
  const auto other_party = copies.enter(kDungeon, 10, 4);

  ASSERT_NE(leader, nullptr);
  EXPECT_EQ(leader, member);
  EXPECT_NE(leader, solo);
  EXPECT_NE(leader, other_party);
  EXPECT_EQ(leader->owner(), InstanceCopies::party_owner(3));
  EXPECT_EQ(solo->owner(), InstanceCopies::solo_owner(9));
  EXPECT_EQ(copies.live_copies(), 3U);

  // Different copies never share presence maps or channel keys.
  EXPECT_NE(leader->presence_map(), solo->presence_map());
  EXPECT_NE(leader->channel_key(), solo->channel_key());
  EXPECT_LT(leader->presence_map(), -kCopyPresenceMapBase + 1);
}

TEST(InstanceCopies, ACopyEndsWhenItsLastHolderLeavesAndTheNextEntryIsFresh) {
  InstanceCopies copies({kDungeon});

  auto first = copies.enter(kDungeon, 7, std::nullopt);
  const auto first_serial = first->serial();
  const auto first_presence = first->presence_map();
  first->resource<Counter>([] {
    return std::make_shared<Counter>();
  })->value = 5;

  first.reset();
  EXPECT_EQ(copies.live_copies(), 0U);

  const auto second = copies.enter(kDungeon, 7, std::nullopt);
  EXPECT_NE(second->serial(), first_serial);
  // The released presence slot is reused, but the serial is not.
  EXPECT_EQ(second->presence_map(), first_presence);
  EXPECT_EQ(second->resource<Counter>([] {
              return std::make_shared<Counter>();
            })->value,
            0);
}

TEST(InstanceCopies, ResourcesAreCreatedOncePerCopyAndType) {
  InstanceCopies copies({kDungeon});
  const auto copy = copies.enter(kDungeon, 7, std::nullopt);

  int created{};
  const auto make = [&] {
    ++created;
    return std::make_shared<Counter>();
  };
  const auto first = copy->resource<Counter>(make);
  const auto again = copy->resource<Counter>(make);

  EXPECT_EQ(first, again);
  EXPECT_EQ(created, 1);
}

TEST(InstanceCopies, ACopyReleasedAfterTheRegistryIsSafe) {
  std::shared_ptr<InstanceCopy> survivor;
  {
    InstanceCopies copies({kDungeon});
    survivor = copies.enter(kDungeon, 7, std::nullopt);
  }
  EXPECT_EQ(survivor->map_id(), kDungeon);
  survivor.reset();
}

TEST(InstanceCopies, RejectedAdmissionsLeaveExistingCopiesUsableAndSlotsReusable) {
  InstanceCopies copies({kDungeon}, 1);
  auto first = copies.enter(kDungeon, 7, std::nullopt);

  for (std::int64_t character = 8; character < 108; ++character) {
    EXPECT_THROW(copies.enter(kDungeon, character, std::nullopt), std::runtime_error);
  }
  EXPECT_EQ(copies.enter(kDungeon, 7, std::nullopt), first);
  EXPECT_EQ(copies.live_copies(), 1U);

  first.reset();
  const auto replacement = copies.enter(kDungeon, 8, std::nullopt);
  ASSERT_NE(replacement, nullptr);
  EXPECT_EQ(copies.live_copies(), 1U);
}

} // namespace
} // namespace chronicles
