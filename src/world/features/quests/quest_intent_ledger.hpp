#pragma once

#include "persistence/character_state_types.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace chronicles::quests {

// Orders quest mutations and kill snapshots for one character without holding a mutex across
// database work. An unresolved mutation is deliberately not assumed to have failed: an
// ambiguous database acknowledgement must be reconciled before captures can pass it.
class QuestIntentLedger final : public std::enable_shared_from_this<QuestIntentLedger> {
public:
  using Snapshot = std::map<int, MonsterRewardQuestAttempt>;
  struct PreparedSnapshotData {
    Snapshot quests;
    std::size_t entries{};
  };
  using PreparedSnapshot = std::shared_ptr<const PreparedSnapshotData>;
  using Sequence = std::uint64_t;

  struct Capture {
    Sequence id{};
    Sequence cutoff{};
  };

  class CaptureLease final {
  public:
    CaptureLease(std::shared_ptr<QuestIntentLedger> ledger, Capture capture)
        : ledger_(std::move(ledger)), capture_(capture) {
    }

    CaptureLease(const CaptureLease &) = delete;
    CaptureLease &operator=(const CaptureLease &) = delete;
    CaptureLease(CaptureLease &&) = delete;
    CaptureLease &operator=(CaptureLease &&) = delete;

    ~CaptureLease() {
      ledger_->release(capture_);
    }

    std::optional<Snapshot> resolve() const {
      return ledger_->resolve(capture_);
    }

    std::optional<Snapshot> resolve_filtered(const std::vector<int> &quest_ids) const {
      return ledger_->resolve_filtered(capture_, quest_ids);
    }

  private:
    std::shared_ptr<QuestIntentLedger> ledger_;
    Capture capture_;
  };

  explicit QuestIntentLedger(const WorldProgress &initial) {
    versions_.emplace(0, prepare(initial));
  }

  std::optional<Sequence> begin_mutation(const WorldProgress &proposed) {
    return begin_prepared(prepare(proposed));
  }

  // Build outside the cross-character ordering mutex; reserving an intent below is the
  // linearization point and performs no database work.
  static PreparedSnapshot prepare(const WorldProgress &progress) {
    auto quests = snapshot(progress);
    const auto entries = snapshot_entries(quests);
    return std::make_shared<const PreparedSnapshotData>(
        PreparedSnapshotData{std::move(quests), entries});
  }

  std::optional<Sequence> begin_prepared(PreparedSnapshot prepared) {
    std::lock_guard lock(mutex_);
    prune();
    const auto &previous = versions_.rbegin()->second;
    if (pending_intent_ || versions_.size() >= kMaximumVersions || !prepared || !previous ||
        retained_entries() + prepared->entries > kMaximumEntries ||
        next_sequence_ == std::numeric_limits<Sequence>::max()) {
      return std::nullopt;
    }
    const auto sequence = ++next_sequence_;
    versions_.emplace(sequence, nullptr);
    pending_previous_ = previous;
    pending_proposal_ = std::move(prepared);
    pending_intent_ = sequence;
    return sequence;
  }

  // Passing nullopt means a definitively failed mutation. Unknown outcomes must remain pending.
  void finish_mutation(Sequence intent, const std::optional<WorldProgress> &committed) {
    auto reconciled = committed ? prepare(*committed) : PreparedSnapshot{};
    std::lock_guard lock(mutex_);
    if (!pending_intent_ || *pending_intent_ != intent) {
      throw std::logic_error("Unknown quest mutation intent");
    }
    auto entry = versions_.find(intent);
    if (entry == versions_.end() || entry == versions_.begin()) {
      throw std::logic_error("Quest mutation intent has no previous version");
    }
    const auto previous = std::prev(entry);
    if (!previous->second) {
      throw std::logic_error("Previous quest mutation is unresolved");
    }
    if (!committed) {
      entry->second = pending_previous_;
    } else if (reconciled->quests == pending_proposal_->quests) {
      // The ordinary successful path moves a snapshot allocated before the DB mutation.
      entry->second = pending_proposal_;
    } else if (reconciled->quests == pending_previous_->quests) {
      // A fenced readback of a write that did not commit is already held by the intent.
      entry->second = pending_previous_;
    } else {
      // Already-processed or independently reconciled outcomes may differ from the proposal.
      // Allocation failure leaves the intent unresolved instead of guessing its result.
      if (retained_entries() + reconciled->entries > kMaximumEntries) {
        throw std::runtime_error("Quest intent history is full during reconciliation");
      }
      entry->second = std::move(reconciled);
    }
    pending_previous_.reset();
    pending_proposal_.reset();
    pending_intent_.reset();
    prune();
  }

  // Resolves the common known-success/known-conflict path without copying progress or allocating
  // after the database has returned: both outcomes were prepared before the mutation started.
  void finish_prepared(Sequence intent, bool committed) {
    std::lock_guard lock(mutex_);
    if (!pending_intent_ || *pending_intent_ != intent || !pending_previous_ ||
        !pending_proposal_) {
      throw std::logic_error("Unknown quest mutation intent");
    }
    auto entry = versions_.find(intent);
    if (entry == versions_.end()) {
      throw std::logic_error("Quest mutation intent has no version");
    }
    if (committed) {
      entry->second = pending_proposal_;
    } else {
      entry->second = pending_previous_;
    }
    pending_previous_.reset();
    pending_proposal_.reset();
    pending_intent_.reset();
    prune();
  }

  std::optional<Capture> capture() {
    std::lock_guard lock(mutex_);
    if (captures_.size() >= kMaximumCaptures ||
        next_capture_id_ == std::numeric_limits<Sequence>::max()) {
      return std::nullopt;
    }
    const auto id = ++next_capture_id_;
    captures_.emplace(id, next_sequence_);
    return Capture{id, next_sequence_};
  }

  std::shared_ptr<CaptureLease> acquire_capture() {
    const auto captured = capture();
    if (!captured) {
      return {};
    }
    try {
      return std::make_shared<CaptureLease>(shared_from_this(), *captured);
    } catch (...) {
      release(*captured);
      throw;
    }
  }

  // nullopt means an earlier intent is still unresolved, or this capture is no longer valid.
  std::optional<Snapshot> resolve(const Capture &capture) const {
    std::lock_guard lock(mutex_);
    const auto held = captures_.find(capture.id);
    if (held == captures_.end() || held->second != capture.cutoff) {
      return std::nullopt;
    }
    const auto after = versions_.upper_bound(capture.cutoff);
    if (after == versions_.begin()) {
      return std::nullopt;
    }
    const auto version = std::prev(after);
    if (!version->second) {
      return std::nullopt;
    }
    return version->second->quests;
  }

  std::optional<Snapshot> resolve_filtered(const Capture &capture,
                                           const std::vector<int> &quest_ids) const {
    std::lock_guard lock(mutex_);
    const auto held = captures_.find(capture.id);
    if (held == captures_.end() || held->second != capture.cutoff) {
      return std::nullopt;
    }
    const auto after = versions_.upper_bound(capture.cutoff);
    if (after == versions_.begin()) {
      return std::nullopt;
    }
    const auto version = std::prev(after);
    if (!version->second) {
      return std::nullopt;
    }

    Snapshot result;
    for (const int id : quest_ids) {
      const auto quest = version->second->quests.find(id);
      if (quest != version->second->quests.end()) {
        result.emplace(*quest);
      }
    }
    return result;
  }

  void release(const Capture &capture) {
    std::lock_guard lock(mutex_);
    const auto held = captures_.find(capture.id);
    if (held != captures_.end() && held->second == capture.cutoff) {
      captures_.erase(held);
    }
    prune();
  }

private:
  static constexpr std::size_t kMaximumVersions = 256;
  static constexpr std::size_t kMaximumCaptures = 1024;
  // Includes prepared/previous snapshots as well as retained versions.
  static constexpr std::size_t kMaximumEntries = 1048576;

  std::size_t retained_entries() const {
    std::size_t count = 0;
    for (const auto &[sequence, version] : versions_) {
      if (version) {
        count += version->entries;
      }
    }
    if (pending_proposal_) {
      count += pending_proposal_->entries;
    }
    return count;
  }

  static std::size_t snapshot_entries(const Snapshot &value) {
    std::size_t count = value.size();
    for (const auto &[id, quest] : value) {
      count += quest.objectives.size();
    }
    return count;
  }

  static Snapshot snapshot(const WorldProgress &progress) {
    Snapshot result;
    for (const auto &[id, quest] : progress.quests) {
      result.emplace(id, MonsterRewardQuestAttempt{quest.attempt, quest.started_epoch_ms,
                                                   quest.complete, quest.abandoned,
                                                   quest.objectives});
    }
    return result;
  }

  // Keep the newest version at or before the oldest outstanding capture, and everything after
  // it. With no captures, retain the newest completed version and any unresolved intent.
  void prune() {
    Sequence oldest = next_sequence_;
    for (const auto &[id, cutoff] : captures_) {
      oldest = std::min(oldest, cutoff);
    }
    auto anchor = versions_.upper_bound(oldest);
    if (anchor == versions_.begin()) {
      return;
    }
    --anchor;
    while (anchor != versions_.begin() && !anchor->second) {
      --anchor;
    }
    versions_.erase(versions_.begin(), anchor);
  }

  mutable std::mutex mutex_;
  std::map<Sequence, PreparedSnapshot> versions_;
  std::map<Sequence, Sequence> captures_;
  Sequence next_sequence_{};
  Sequence next_capture_id_{};
  std::optional<Sequence> pending_intent_;
  PreparedSnapshot pending_previous_;
  PreparedSnapshot pending_proposal_;
};

} // namespace chronicles::quests
