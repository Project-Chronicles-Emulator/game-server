#include "world/core/item_grant_commit.hpp"

#include "auth/auth.hpp"
#include "persistence/character_repositories.hpp"
#include "world/core/tests/economy_test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <set>

namespace chronicles {
namespace {

using namespace economy_testing;

std::shared_ptr<RecordingProgress> captured_progress(EconomyFixture &fixture) {
  return std::make_shared<RecordingProgress>(fixture.repositories, fixture.actor, fixture.trace);
}

std::vector<CharacterItem> requested_instance() {
  return {CharacterItem{0, kEquipment, 5, std::nullopt, 1, 10000, 0}};
}

ItemGrantCacheAccess cache_queries(EconomyFixture &fixture) {
  return {[&fixture] {
            fixture.trace.push_back("tutorial-query");
            return true;
          },
          [&fixture] {
            fixture.trace.push_back("stats-query");
            return &fixture.prepared_stats;
          }};
}

TEST(ItemGrantCommit, DurableGrantPrecedesAdoptionActorReloadAndSnapshotWithoutStatRecompute) {
  EconomyFixture fixture;
  auto commit =
      make_item_grant_commit(captured_progress(fixture), cache_queries(fixture), *fixture.events);
  const auto before = fixture.stored_progress();
  auto after = before;
  ++after.revision;
  after.gold -= 7;

  const auto result = commit(before, after, requested_instance());
  ASSERT_TRUE(result);
  ASSERT_EQ(result->size(), 1U);
  EXPECT_GT(result->front().id, 0);
  EXPECT_EQ(fixture.stored_progress(), after);
  EXPECT_EQ(fixture.progress->cached, after);
  ASSERT_TRUE(fixture.last_snapshot);
  EXPECT_EQ(fixture.last_snapshot->items.size(), fixture.actor.items.size() + 1);
  EXPECT_EQ(fixture.trace, (std::vector<std::string>{"commit", "tutorial-query", "adopt",
                                                     "character", "stats-query", "snapshot"}));
  EXPECT_FALSE(fixture.prepared_stats.at("refreshed").get<bool>());
}

TEST(ItemGrantCommit, ProgressOnlyCommitAdoptsWithoutReloadingOrPublishingAnInventorySnapshot) {
  EconomyFixture fixture;
  auto commit =
      make_item_grant_commit(captured_progress(fixture), cache_queries(fixture), *fixture.events);
  const auto before = fixture.stored_progress();
  auto after = before;
  ++after.revision;
  after.gold -= 1;

  const auto result = commit(before, after, {});
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->empty());
  EXPECT_EQ(fixture.trace, (std::vector<std::string>{"commit", "tutorial-query", "adopt"}));
  EXPECT_FALSE(fixture.last_snapshot);
}

TEST(ItemGrantCommit, RevisionConflictDoesNotQueryOrMutateAnySessionCache) {
  EconomyFixture fixture;
  auto progress = captured_progress(fixture);
  progress->reject_commit = true;
  auto commit = make_item_grant_commit(progress, cache_queries(fixture), *fixture.events);
  const auto before = fixture.stored_progress();
  auto after = before;
  ++after.revision;

  EXPECT_FALSE(commit(before, after, requested_instance()));
  EXPECT_EQ(fixture.stored_progress(), before);
  EXPECT_EQ(fixture.trace, (std::vector<std::string>{"commit"}));
}

TEST(ItemGrantCommit, StorageFailureKeepsTheOriginalExceptionAndPublishesNoSuccess) {
  EconomyFixture fixture;
  auto progress = captured_progress(fixture);
  progress->throw_commit = true;
  auto commit = make_item_grant_commit(progress, cache_queries(fixture), *fixture.events);
  const auto before = fixture.stored_progress();

  EXPECT_THROW(commit(before, before, requested_instance()), std::runtime_error);
  EXPECT_EQ(fixture.trace, (std::vector<std::string>{"commit"}));
  EXPECT_FALSE(fixture.last_snapshot);
}

TEST(ItemGrantCommit, PostCommitReloadFailurePropagatesWithTheDurableGrantAlreadySaved) {
  EconomyFixture fixture;
  auto progress = captured_progress(fixture);
  progress->throw_character = true;
  auto commit = make_item_grant_commit(progress, cache_queries(fixture), *fixture.events);
  const auto before = fixture.stored_progress();
  auto after = before;
  ++after.revision;

  EXPECT_THROW(commit(before, after, requested_instance()), std::runtime_error);
  EXPECT_EQ(fixture.stored_progress(), after);
  EXPECT_EQ(fixture.progress->cached, after);
  EXPECT_EQ(fixture.trace,
            (std::vector<std::string>{"commit", "tutorial-query", "adopt", "character"}));
  EXPECT_FALSE(fixture.last_snapshot);
}

TEST(ItemGrantCommit, SnapshotQueriesTheCurrentPreparedStatsWithoutRetainingTheOldPointer) {
  EconomyFixture fixture;
  Json replacement = {{"generation", 2}};
  const Json *current = &fixture.prepared_stats;
  auto cache = cache_queries(fixture);
  cache.player_stats = [&current] {
    return current;
  };
  auto commit =
      make_item_grant_commit(captured_progress(fixture), std::move(cache), *fixture.events);
  fixture.on_snapshot = [&](const PlayerInventorySnapshotChanged &event) {
    EXPECT_EQ(event.prepared_stats, &replacement);
  };
  current = &replacement;
  const auto before = fixture.stored_progress();
  auto after = before;
  ++after.revision;

  EXPECT_TRUE(commit(before, after, requested_instance()));
}

TEST(ItemGrantCommit, PendingPersistenceOwnsItsCapturedSubjectUntilTheConsumerIsDestroyed) {
  EconomyFixture fixture;
  auto progress = captured_progress(fixture);
  std::weak_ptr<ProgressAccess> weak = progress;
  auto commit = make_item_grant_commit(progress, cache_queries(fixture), *fixture.events);
  progress.reset();
  ASSERT_FALSE(weak.expired());
  const auto before = fixture.stored_progress();
  auto after = before;
  ++after.revision;

  EXPECT_TRUE(commit(before, after, {}));
  commit = {};
  EXPECT_TRUE(weak.expired());
}

void exercise_loot_owner_takeover(CharacterRepositories old_repositories,
                                  CharacterRepositories new_repositories) {
  const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  Account account;
  account.name = "loot_epoch_" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  ASSERT_TRUE(old_repositories.accounts->add_account(account));
  const auto owner = old_repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "LootEpoch" + suffix;
  character = old_repositories.characters->create(character);

  // A direct repository without a world owner remains usable for trusted offline tools.
  RepositoryProgressAccess legacy_access(*old_repositories.characters,
                                         *old_repositories.character_state,
                                         owner->id, character.id, 1);
  const auto legacy_before = legacy_access.load();
  auto legacy_after = legacy_before;
  ++legacy_after.revision;
  ++legacy_after.gold;
  const auto legacy = legacy_access.commit_loot_claim(std::string(31, '6') + "d",
                                                       std::string(64, 'c'), legacy_before,
                                                       legacy_after, ItemTrade{});
  ASSERT_EQ(legacy.status, LootClaimStatus::committed);

  AuthService old_auth(old_repositories, Config{});
  AuthService new_auth(new_repositories, Config{});
  const auto old_epoch = old_auth.acquire_monster_reward_owner_epoch();
  const auto new_epoch = new_auth.acquire_monster_reward_owner_epoch();
  ASSERT_GT(new_epoch, old_epoch);
  Handoff handoff;
  handoff.account_id = owner->id;
  handoff.character_id = character.id;
  handoff.world_id = 1;
  HandoffProgressAccess old_access(old_auth, handoff);
  HandoffProgressAccess new_access(new_auth, handoff);

  const auto before = old_access.load();
  auto after = before;
  ++after.revision;
  after.gold += 7;
  std::set<int> used_slots;
  for (const auto &item : character.items) {
    used_slots.insert(item.inventory_slot);
  }
  int free_slot = 0;
  while (used_slots.contains(free_slot)) {
    ++free_slot;
  }
  CharacterItem item;
  item.cms_id = 901;
  item.inventory_slot = free_slot;
  ItemTrade trade;
  trade.granted_items.push_back(item);
  const std::string receipt = std::string(31, '7') + "d";
  const std::string fingerprint(64, 'b');

  EXPECT_THROW(old_access.commit_loot_claim(receipt, fingerprint, before, after, trade),
               std::runtime_error);
  EXPECT_EQ(old_access.load(), before);
  EXPECT_EQ(old_auth.lookup_world_loot_claim(handoff, receipt, fingerprint).status,
            LootClaimStatus::not_found);
  const auto unchanged = old_repositories.characters->character_by_id(character.id);
  ASSERT_TRUE(unchanged);
  EXPECT_EQ(unchanged->items, character.items);

  const auto committed = new_access.commit_loot_claim(receipt, fingerprint, before, after, trade);
  ASSERT_EQ(committed.status, LootClaimStatus::committed);
  ASSERT_EQ(committed.granted_items.size(), 1U);
  EXPECT_THROW(old_access.commit_loot_claim(receipt, fingerprint, before, after, trade),
               std::runtime_error);
  const auto replay = new_access.commit_loot_claim(receipt, fingerprint, before, after, trade);
  EXPECT_EQ(replay.status, LootClaimStatus::already_claimed_by_same_character);
  EXPECT_EQ(replay.granted_items, committed.granted_items);
  EXPECT_EQ(new_access.load(), after);
  const auto restored = new_repositories.characters->character_by_id(character.id);
  ASSERT_TRUE(restored);
  EXPECT_EQ(std::ranges::count(restored->items, committed.granted_items.front()), 1);
}

TEST(ItemGrantCommit, MemoryHandoffLootClaimRejectsStaleOwnerEpoch) {
  MemoryRepositories repositories;
  exercise_loot_owner_takeover(repositories.character_repositories(),
                               make_memory_character_repositories(repositories.database));
}

TEST(ItemGrantCommit, PostgresHandoffLootClaimRejectsStaleOwnerEpoch) {
  const auto *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL test cluster";
  }
  exercise_loot_owner_takeover(make_postgres_character_repositories(connection),
                               make_postgres_character_repositories(connection));
}

} // namespace
} // namespace chronicles
