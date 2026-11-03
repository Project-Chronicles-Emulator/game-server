#pragma once

#include "persistence/character_state_types.hpp"
#include "persistence/quest_service_effects.hpp"
#include "world/core/quest_events.hpp"
#include "world/features/gathering/gathering.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace chronicles {

// Immutable input owned independently of a world session. The receipt fingerprint covers
// every member; the detached commit must atomically persist the item and quest transition.
struct GatheringCommitPlan {
  std::string receipt_id;
  std::int64_t account_id{};
  std::int64_t character_id{};
  int world_id{};
  std::uint64_t channel{};
  // Shared maps use a stable scope; each private copy uses a newly minted incarnation.
  std::string source_scope;
  std::int64_t expected_incarnation{};
  int expected_phase{};
  int next_phase{};
  // Delay is applied only after a fenced result is confirmed, never at request start.
  std::optional<std::int64_t> respawn_delay_ticks;
  GatheringNodeKey node;
  std::optional<std::uint64_t> corpse_unit_id;
  std::optional<std::string> corpse_incarnation;
  std::optional<SharedGatheringSourceTransition> shared_source;
  WorldProgress before;
  WorldProgress after;
  ItemTrade trade;
  QuestServiceEffects effects;
  QuestEvents quest_events;
};

std::string gathering_receipt_id();
std::string gathering_corpse_receipt_id(const std::string &incarnation);
std::string gathering_plan_fingerprint(const GatheringCommitPlan &plan);

enum class GatheringRecoveryStatus { pending, committed, rejected };

// Register this object with character admission before the first attempt. Both the live
// session and detached worker can call attempt(); the mutex prevents overlapping retries.
class GatheringRecoveryOperation final {
public:
  using Commit = std::function<LootClaimResult(const GatheringCommitPlan &, const std::string &)>;
  using Clock = std::function<std::int64_t()>;
  // These detached callbacks must be idempotent and must not capture a session object.
  using FinalizeIntent = std::function<void(bool committed)>;
  using FinalizeCorpse = std::function<void(bool committed)>;
  // Returns the fenced receipt result and current source state after arming its deadline.
  using FinalizeReceipt = std::function<GatheringFinalizeResult(bool committed)>;
  using PublishSource = std::function<void(const std::vector<GatheringStateChange> &)>;
  // Bumps the durable source generation before a rejected reservation is released.
  using FenceSource = std::function<SharedGatheringSourceUpdateResult(
      const SharedGatheringSourceTransition &)>;

  GatheringRecoveryOperation(GatheringCommitPlan plan, Commit commit, Clock clock,
                             std::shared_ptr<GatheringNodes> nodes = {},
                             std::optional<std::uint64_t> reservation = {},
                             FinalizeIntent finalize_intent = {},
                             FinalizeReceipt finalize_receipt = {}, Clock epoch_clock = {},
                             PublishSource publish_source = {}, FenceSource fence_source = {},
                             FinalizeCorpse finalize_corpse = {},
                             std::optional<GatheringNodeKey> linked_node = {},
                             std::optional<std::uint64_t> linked_reservation = {});

  GatheringRecoveryStatus attempt();
  GatheringRecoveryStatus status() const;
  void set_publish_source(PublishSource publish_source);
  std::vector<CharacterItem> granted_items() const;
  std::vector<GatheringStateChange> source_changes() const;
  const GatheringCommitPlan &plan() const noexcept;

private:
  const GatheringCommitPlan plan_;
  const std::string fingerprint_;
  const Commit commit_;
  const Clock clock_;
  const std::shared_ptr<GatheringNodes> nodes_;
  const std::optional<std::uint64_t> reservation_;
  const FinalizeIntent finalize_intent_;
  const FinalizeReceipt finalize_receipt_;
  const Clock epoch_clock_;
  PublishSource publish_source_;
  const FenceSource fence_source_;
  const FinalizeCorpse finalize_corpse_;
  const std::optional<GatheringNodeKey> linked_node_;
  const std::optional<std::uint64_t> linked_reservation_;
  mutable std::mutex mutex_;
  std::optional<bool> committed_;
  bool intent_finalized_{};
  bool corpse_finalized_{};
  bool source_finalized_{};
  bool source_published_{};
  std::int64_t source_begin_tick_{};
  bool receipt_finalized_{};
  std::optional<std::int64_t> confirmed_deadline_;
  std::optional<StoredSharedGatheringSource> confirmed_source_;
  std::optional<StoredSharedGatheringSource> confirmed_linked_source_;
  GatheringRecoveryStatus status_{GatheringRecoveryStatus::pending};
  std::vector<CharacterItem> granted_items_;
  std::vector<GatheringStateChange> source_changes_;
};

} // namespace chronicles
