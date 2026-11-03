#include "world/features/gathering/gathering_recovery.hpp"

#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace chronicles {
namespace {

constexpr std::int64_t kTick = 1000000;

GatheringCommitPlan plan() {
  GatheringCommitPlan result;
  result.receipt_id = std::string(32, 'a');
  result.account_id = 1;
  result.character_id = 2;
  result.world_id = 1;
  result.source_scope = "private-test";
  result.node = {9012, GatheringNodeKind::Placement, 3, 4};
  result.after = result.before;
  result.after.revision = 1;
  result.after.stackables[123] = 5;
  return result;
}

TEST(GatheringRecovery, FingerprintBindsSourceTradeQuestAndEntireTransition) {
  const auto original = plan();
  const auto fingerprint = gathering_plan_fingerprint(original);
  EXPECT_EQ(fingerprint.size(), 64U);
  auto changed = original;
  changed.channel = 7;
  EXPECT_NE(fingerprint, gathering_plan_fingerprint(changed));
  changed = original;
  changed.after.stackables[123] = 6;
  EXPECT_NE(fingerprint, gathering_plan_fingerprint(changed));
  changed = original;
  changed.trade.durability_updates.push_back({42, 10, 9});
  EXPECT_NE(fingerprint, gathering_plan_fingerprint(changed));
  changed = original;
  changed.quest_events.push_back({QuestEventKind::DoodadInteracted, 10, 2, 1});
  EXPECT_NE(fingerprint, gathering_plan_fingerprint(changed));
  EXPECT_NE(gathering_receipt_id(), gathering_receipt_id());
}

TEST(GatheringRecovery, LostAcknowledgementRetainsSourceAndExactRetryAcrossOwnerRelease) {
  auto nodes = std::make_shared<GatheringNodes>();
  auto input = plan();
  const auto token = nodes->reserve_placement(input.node, kTick, 100);
  ASSERT_TRUE(token);
  int calls = 0;
  std::string fingerprint;
  auto commit = [&](const GatheringCommitPlan &request, const std::string &current_fingerprint) {
    EXPECT_EQ(request.receipt_id, input.receipt_id);
    if (calls++ == 0) {
      fingerprint = current_fingerprint;
      throw std::runtime_error("Synthetic committed transaction with lost acknowledgement");
    }
    EXPECT_EQ(current_fingerprint, fingerprint);
    return LootClaimResult{LootClaimStatus::already_claimed_by_same_character, {}};
  };
  auto live_owner = std::make_shared<GatheringRecoveryOperation>(input, commit, [] { return kTick + 1000; },
                                                                 nodes, token);
  auto registry_owner = live_owner;
  EXPECT_THROW(live_owner->attempt(), std::runtime_error);
  live_owner.reset();
  EXPECT_EQ(registry_owner->status(), GatheringRecoveryStatus::pending);
  EXPECT_FALSE(nodes->reserve_placement(input.node, kTick + 100000, 100));
  EXPECT_TRUE(nodes->collect_respawns(kTick + 100000).empty());
  EXPECT_EQ(registry_owner->attempt(), GatheringRecoveryStatus::committed);
  EXPECT_EQ(registry_owner->attempt(), GatheringRecoveryStatus::committed);
  EXPECT_EQ(calls, 2);
  EXPECT_FALSE(nodes->is_active(input.node, kTick + 1099));
  EXPECT_TRUE(nodes->is_active(input.node, kTick + 1100));
}

TEST(GatheringRecovery, CorpseLeaseSurvivesLostAcknowledgementAndOwnerTeardown) {
  auto handling = std::make_shared<CorpseHandlingState>(std::string(32, 'b'));
  const auto token = handling->reserve();
  ASSERT_TRUE(token);
  auto input = plan();
  input.corpse_unit_id = 42;
  input.corpse_incarnation = handling->incarnation();
  input.receipt_id = gathering_corpse_receipt_id(*input.corpse_incarnation);
  EXPECT_EQ(input.receipt_id, gathering_corpse_receipt_id(*input.corpse_incarnation));
  EXPECT_NE(input.receipt_id, *input.corpse_incarnation);

  int attempts = 0;
  std::weak_ptr<CorpseHandlingState> weak = handling;
  auto operation = std::make_shared<GatheringRecoveryOperation>(
      input, [&](const GatheringCommitPlan &request, const std::string &) {
        EXPECT_EQ(request.receipt_id, input.receipt_id);
        if (++attempts == 1) {
          throw std::runtime_error("Synthetic lost acknowledgement");
        }
        return LootClaimResult{LootClaimStatus::already_claimed_by_same_character, {}};
      }, [] { return kTick; }, nullptr, std::nullopt, GatheringRecoveryOperation::FinalizeIntent{},
      GatheringRecoveryOperation::FinalizeReceipt{}, GatheringRecoveryOperation::Clock{},
      GatheringRecoveryOperation::PublishSource{}, GatheringRecoveryOperation::FenceSource{},
      [handling, token = *token](bool committed) {
        if (!handling->finish(token, committed)) {
          throw std::runtime_error("Corpse reservation changed");
        }
      });
  EXPECT_THROW(operation->attempt(), std::runtime_error);
  EXPECT_FALSE(handling->reserve());
  handling.reset();
  ASSERT_FALSE(weak.expired());
  EXPECT_EQ(operation->attempt(), GatheringRecoveryStatus::committed);
  EXPECT_FALSE(weak.lock()->available());
  EXPECT_EQ(attempts, 2);
  operation.reset();
  EXPECT_TRUE(weak.expired());
}

TEST(GatheringRecovery, ConflictKeepsSharedSourceReservedUntilFencedReadbackExists) {
  auto nodes = std::make_shared<GatheringNodes>();
  auto input = plan();
  input.source_scope = "shared";
  const auto token = nodes->reserve_placement(input.node, kTick, 100);
  ASSERT_TRUE(token);
  GatheringRecoveryOperation operation(
      input, [](const GatheringCommitPlan &, const std::string &) {
        return LootClaimResult{LootClaimStatus::conflict, {}};
      }, [] { return kTick + 10; }, nodes, token, {},
      [](bool) { return GatheringFinalizeResult{GatheringFinalizeStatus::finalized}; },
      [] { return 4900; });
  EXPECT_THROW(operation.attempt(), std::runtime_error);
  EXPECT_EQ(operation.status(), GatheringRecoveryStatus::pending);
  EXPECT_FALSE(nodes->is_active(input.node, kTick + 1000));
  EXPECT_FALSE(nodes->reserve_placement(input.node, kTick + 1000, 100));
}

TEST(GatheringRecovery, PrivateConflictReleasesOnlyItsOwnReservation) {
  auto nodes = std::make_shared<GatheringNodes>();
  const auto input = plan();
  const auto token = nodes->reserve_placement(input.node, kTick, 100);
  ASSERT_TRUE(token);

  GatheringRecoveryOperation operation(
      input, [](const GatheringCommitPlan &, const std::string &) {
        return LootClaimResult{LootClaimStatus::conflict, {}};
      }, [] { return kTick + 10; }, nodes, token);

  EXPECT_EQ(operation.attempt(), GatheringRecoveryStatus::rejected);
  const auto successor = nodes->reserve_placement(input.node, kTick + 10, 100);
  ASSERT_TRUE(successor);
  EXPECT_FALSE(nodes->finish_reservation(input.node, *token, false, kTick + 10));
  EXPECT_FALSE(nodes->is_active(input.node, kTick + 1000));
}

TEST(GatheringRecovery, DefinitiveSourceConflictInstallsFencedSnapshotBeforeRelease) {
  auto nodes = std::make_shared<GatheringNodes>();
  auto input = plan();
  input.source_scope = "shared";
  input.shared_source = SharedGatheringSourceTransition{};
  const auto token = nodes->reserve_placement(input.node, kTick, 100);
  ASSERT_TRUE(token);
  StoredSharedGatheringSource current;
  current.generation = 2;
  current.active = false;
  current.next_transition_epoch_ticks = 5000;
  GatheringRecoveryOperation operation(
      input, [current](const GatheringCommitPlan &, const std::string &) {
        return LootClaimResult{LootClaimStatus::source_conflict, {}, current};
      }, [] { return kTick; }, nodes, token, {},
      [](bool) { return GatheringFinalizeResult{GatheringFinalizeStatus::finalized}; },
      [] { return 4900; }, {},
      [current](const SharedGatheringSourceTransition &) {
        return SharedGatheringSourceUpdateResult{true, current};
      });
  EXPECT_EQ(operation.attempt(), GatheringRecoveryStatus::rejected);
  ASSERT_TRUE(nodes->shared_source(input.node));
  EXPECT_EQ(nodes->shared_source(input.node)->generation, 2);
  EXPECT_FALSE(nodes->is_active(input.node, kTick + 1000));
  EXPECT_FALSE(nodes->reserve_placement(input.node, kTick + 1000, 100));
}

TEST(GatheringRecovery, RejectedSharedClaimWaitsForGenerationFence) {
  auto nodes = std::make_shared<GatheringNodes>();
  auto input = plan();
  input.source_scope = "shared";
  input.shared_source = SharedGatheringSourceTransition{};
  const auto token = nodes->reserve_placement(input.node, kTick, 100);
  ASSERT_TRUE(token);

  int commits = 0;
  int fences = 0;
  StoredSharedGatheringSource current;
  current.generation = 1;
  current.active = true;
  GatheringRecoveryOperation operation(
      input, [&](const GatheringCommitPlan &, const std::string &) {
        ++commits;
        return LootClaimResult{LootClaimStatus::conflict, {}};
      }, [] { return kTick; }, nodes, token, {},
      [](bool) { return GatheringFinalizeResult{GatheringFinalizeStatus::finalized}; },
      [] { return 4900; }, {},
      [&](const SharedGatheringSourceTransition &) {
        ++fences;
        return SharedGatheringSourceUpdateResult{fences > 1, current};
      });

  EXPECT_THROW(operation.attempt(), std::runtime_error);
  EXPECT_FALSE(nodes->reserve_placement(input.node, kTick + 1000, 100));
  EXPECT_EQ(operation.attempt(), GatheringRecoveryStatus::rejected);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(fences, 2);
  ASSERT_TRUE(nodes->shared_source(input.node));
  EXPECT_EQ(nodes->shared_source(input.node)->generation, 1);
  EXPECT_TRUE(nodes->reserve_placement(input.node, kTick + 1000, 100));
}

TEST(GatheringRecovery, LinkedCommitInstallsBothSourcesBeforeOnePublication) {
  auto nodes = std::make_shared<GatheringNodes>();
  auto input = plan();
  input.source_scope = "shared";
  input.shared_source = SharedGatheringSourceTransition{};
  input.shared_source->linked = SharedGatheringLinkedTransition{};
  const GatheringNodeKey barrier{30104, GatheringNodeKind::Doodad, 171 | (289 << 14), 2};
  const auto primary_token = nodes->reserve_placement(input.node, kTick, 100);
  const auto barrier_token = nodes->reserve_cliffs_cannon_barrier(barrier);
  ASSERT_TRUE(primary_token);
  ASSERT_TRUE(barrier_token);
  StoredSharedGatheringSource primary;
  primary.generation = 1;
  primary.active = false;
  primary.next_transition_epoch_ticks = 5000;
  StoredSharedGatheringSource linked;
  linked.generation = 1;
  linked.phase = kDespawnedDoodadPhase;
  int commits = 0;
  int publications = 0;
  GatheringRecoveryOperation operation(
      input, [&](const GatheringCommitPlan &, const std::string &) {
        ++commits;
        return LootClaimResult{LootClaimStatus::committed, {}};
      }, [] { return kTick; }, nodes, primary_token, {},
      [primary, linked](bool) {
        return GatheringFinalizeResult{GatheringFinalizeStatus::finalized, 5000, primary, linked};
      }, [] { return 4900; },
      [&](const std::vector<GatheringStateChange> &changes) {
        ++publications;
        ASSERT_EQ(changes.size(), 2U);
        ASSERT_TRUE(nodes->shared_source(input.node));
        ASSERT_TRUE(nodes->shared_source(barrier));
        EXPECT_EQ(changes[0].node, input.node);
        EXPECT_EQ(changes[1].node, barrier);
      }, {}, {}, barrier, barrier_token);

  EXPECT_EQ(operation.attempt(), GatheringRecoveryStatus::committed);
  EXPECT_EQ(operation.attempt(), GatheringRecoveryStatus::committed);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(publications, 1);
}

TEST(GatheringRecovery, LinkedConflictKeepsBothOwnersUntilBothSourcesAreFenced) {
  auto nodes = std::make_shared<GatheringNodes>();
  auto input = plan();
  input.source_scope = "shared";
  input.shared_source = SharedGatheringSourceTransition{};
  input.shared_source->linked = SharedGatheringLinkedTransition{};
  const GatheringNodeKey barrier{30104, GatheringNodeKind::Doodad, 171 | (289 << 14), 2};
  const auto primary_token = nodes->reserve_placement(input.node, kTick, 100);
  const auto barrier_token = nodes->reserve_cliffs_cannon_barrier(barrier);
  ASSERT_TRUE(primary_token);
  ASSERT_TRUE(barrier_token);
  StoredSharedGatheringSource primary;
  primary.generation = 2;
  primary.active = true;
  StoredSharedGatheringSource linked;
  linked.generation = 2;
  linked.phase = kDespawnedDoodadPhase;
  int fences = 0;
  GatheringRecoveryOperation operation(
      input, [](const GatheringCommitPlan &, const std::string &) {
        return LootClaimResult{LootClaimStatus::conflict, {}};
      }, [] { return kTick; }, nodes, primary_token, {},
      [](bool) { return GatheringFinalizeResult{GatheringFinalizeStatus::finalized}; },
      [] { return 4900; }, {},
      [&](const SharedGatheringSourceTransition &) {
        ++fences;
        if (fences == 1) {
          return SharedGatheringSourceUpdateResult{true, primary};
        }
        return SharedGatheringSourceUpdateResult{true, primary, linked};
      }, {}, barrier, barrier_token);

  EXPECT_THROW(operation.attempt(), std::runtime_error);
  EXPECT_FALSE(nodes->reserve_placement(input.node, kTick + 1000, 100));
  EXPECT_FALSE(nodes->reserve_cliffs_cannon_barrier(barrier));
  EXPECT_EQ(operation.attempt(), GatheringRecoveryStatus::rejected);
  ASSERT_TRUE(nodes->shared_source(input.node));
  ASSERT_TRUE(nodes->shared_source(barrier));
  EXPECT_EQ(fences, 2);
}

TEST(GatheringRecovery, ReceiptIdentityErrorsKeepTheSourceQuarantined) {
  for (const auto failure : {LootClaimStatus::claimed_by_other_character,
                             LootClaimStatus::mismatch, LootClaimStatus::source_conflict}) {
    auto nodes = std::make_shared<GatheringNodes>();
    const auto input = plan();
    const auto token = nodes->reserve_placement(input.node, kTick, 100);
    ASSERT_TRUE(token);
    GatheringRecoveryOperation operation(
        input, [failure](const GatheringCommitPlan &, const std::string &) {
          return LootClaimResult{failure, {}};
        }, [] { return kTick; }, nodes, token);
    EXPECT_THROW(operation.attempt(), std::runtime_error);
    EXPECT_EQ(operation.status(), GatheringRecoveryStatus::pending);
    EXPECT_FALSE(nodes->is_active(input.node, kTick + 10000));
  }
}

TEST(GatheringRecovery, IntentAndReceiptFailuresDoNotReplayAConfirmedAward) {
  auto nodes = std::make_shared<GatheringNodes>();
  auto input = plan();
  input.quest_events.push_back({QuestEventKind::DoodadInteracted, 10, 1, 1});
  const auto token = nodes->reserve_placement(input.node, kTick, 100);
  ASSERT_TRUE(token);
  int commits = 0;
  int intent_attempts = 0;
  int retire_attempts = 0;
  GatheringRecoveryOperation operation(
      input, [&](const GatheringCommitPlan &, const std::string &) {
        ++commits;
        return LootClaimResult{LootClaimStatus::committed, {}};
      }, [] { return kTick; }, nodes, token,
      [&](bool committed) {
        EXPECT_TRUE(committed);
        if (++intent_attempts == 1) {
          throw std::runtime_error("Synthetic quest intent finalization failure");
        }
      }, [&](bool) -> GatheringFinalizeResult {
        if (++retire_attempts == 1) {
          throw std::runtime_error("Synthetic receipt finalization acknowledgement failure");
        }
        return {GatheringFinalizeStatus::finalized, {}};
      });
  EXPECT_THROW(operation.attempt(), std::runtime_error);
  EXPECT_FALSE(nodes->is_active(input.node, kTick + 1000));
  EXPECT_THROW(operation.attempt(), std::runtime_error);
  EXPECT_EQ(operation.status(), GatheringRecoveryStatus::pending);
  EXPECT_FALSE(nodes->is_active(input.node, kTick + 1000));
  EXPECT_EQ(operation.attempt(), GatheringRecoveryStatus::committed);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(intent_attempts, 2);
  EXPECT_EQ(retire_attempts, 2);
}

TEST(GatheringRecovery, PublishesThePersistedDeadlineWithoutExtendingIt) {
  auto nodes = std::make_shared<GatheringNodes>();
  const auto input = plan();
  const auto token = nodes->reserve_placement(input.node, kTick, 1000);
  ASSERT_TRUE(token);
  GatheringRecoveryOperation operation(
      input, [](const GatheringCommitPlan &, const std::string &) {
        return LootClaimResult{LootClaimStatus::committed, {}};
      }, [] { return kTick; }, nodes, token, {},
      [](bool committed) -> GatheringFinalizeResult {
        EXPECT_TRUE(committed);
        return {GatheringFinalizeStatus::finalized, 5000};
      }, [] { return 4900; });
  ASSERT_EQ(operation.attempt(), GatheringRecoveryStatus::committed);
  EXPECT_FALSE(nodes->is_active(input.node, kTick + 99));
  EXPECT_TRUE(nodes->is_active(input.node, kTick + 100));
}

TEST(GatheringRecovery, NotificationRetryDoesNotRepeatCommittedAward) {
  auto nodes = std::make_shared<GatheringNodes>();
  const auto input = plan();
  const auto token = nodes->reserve_placement(input.node, kTick, 100);
  ASSERT_TRUE(token);
  int commits = 0;
  int publications = 0;
  GatheringRecoveryOperation operation(
      input, [&](const GatheringCommitPlan &, const std::string &) {
        ++commits;
        return LootClaimResult{LootClaimStatus::committed, {}};
      }, [] { return kTick; }, nodes, token, {}, {}, {},
      [&](const std::vector<GatheringStateChange> &changes) {
        ASSERT_EQ(changes.size(), 1U);
        EXPECT_EQ(changes.front().node, input.node);
        if (++publications == 1) {
          throw std::runtime_error("Synthetic source publication failure");
        }
      });
  EXPECT_THROW(operation.attempt(), std::runtime_error);
  EXPECT_EQ(operation.status(), GatheringRecoveryStatus::pending);
  EXPECT_EQ(operation.attempt(), GatheringRecoveryStatus::committed);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(publications, 2);
}

TEST(GatheringRecovery, ConcurrentAttemptsSerializeAndAwardOnce) {
  auto input = plan();
  std::promise<void> entered;
  std::promise<void> release;
  auto ready = entered.get_future();
  auto proceed = release.get_future().share();
  int calls = 0;
  auto operation = std::make_shared<GatheringRecoveryOperation>(
      input, [&](const GatheringCommitPlan &, const std::string &) {
        ++calls;
        entered.set_value();
        proceed.wait();
        return LootClaimResult{LootClaimStatus::committed, {}};
      }, [] { return kTick; });
  GatheringRecoveryStatus first = GatheringRecoveryStatus::pending;
  GatheringRecoveryStatus second = GatheringRecoveryStatus::pending;
  std::thread first_thread([&] { first = operation->attempt(); });
  ready.wait();
  std::thread second_thread([&] { second = operation->attempt(); });
  release.set_value();
  first_thread.join();
  second_thread.join();
  EXPECT_EQ(first, GatheringRecoveryStatus::committed);
  EXPECT_EQ(second, GatheringRecoveryStatus::committed);
  EXPECT_EQ(calls, 1);
}

} // namespace
} // namespace chronicles
