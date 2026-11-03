#pragma once

#include "protocol/protocol.hpp"
#include "world/features/combat/combat_timeline.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace chronicles {

// I/O-executor owned. One received frame stays here until its world timeline admits it;
// it is never replayed after the worker has started the session pipeline.
class CombatInputAdmission final {
public:
  struct Snapshot {
    std::shared_ptr<combat::CombatTimeline> timeline;
    combat::CombatTimeline::Owner owner;
    // Must name the enclosing session. It is weak while idle; accepted input retains that
    // session until settlement or cancellation, including while a claim is being executed.
    std::weak_ptr<void> owner_lifetime;
  };

  enum class SubmitStatus { accepted, unavailable, busy, rejected };

  CombatInputAdmission() = default;
  CombatInputAdmission(const CombatInputAdmission &) = delete;
  CombatInputAdmission &operator=(const CombatInputAdmission &) = delete;
  CombatInputAdmission(CombatInputAdmission &&) = delete;
  CombatInputAdmission &operator=(CombatInputAdmission &&) = delete;

  bool publish(std::optional<Snapshot> snapshot) {
    if (pending_) {
      return false;
    }
    snapshot_ = std::move(snapshot);
    return true;
  }

  SubmitStatus submit(Frame frame, std::int64_t ingress_tick) {
    if (pending_) {
      return SubmitStatus::busy;
    }
    if (!snapshot_ || !snapshot_->timeline) {
      return SubmitStatus::unavailable;
    }
    auto owner_lifetime = snapshot_->owner_lifetime.lock();
    if (!owner_lifetime) {
      return SubmitStatus::unavailable;
    }
    static_assert(std::is_nothrow_move_constructible_v<Frame>);
    static_assert(std::is_nothrow_copy_constructible_v<Snapshot>);
    static_assert(std::is_nothrow_move_constructible_v<Pending>);
    const auto result = snapshot_->timeline->enqueue(
        snapshot_->owner, ingress_tick, combat::CombatTimeline::Kind::input);
    if (result.status != combat::CombatTimeline::EnqueueStatus::accepted || !result.token) {
      return SubmitStatus::rejected;
    }
    pending_.emplace(Pending{*snapshot_, *result.token, std::move(frame),
                             std::move(owner_lifetime), false});
    return SubmitStatus::accepted;
  }

  combat::CombatTimeline::ClaimResult claim(std::int64_t now_tick) {
    if (!pending_ || pending_->claimed) {
      return {combat::CombatTimeline::ClaimStatus::stale, std::nullopt};
    }
    const auto result = pending_->snapshot.timeline->try_claim(pending_->token, now_tick);
    if (result.status == combat::CombatTimeline::ClaimStatus::ready) {
      pending_->claimed = true;
    }
    return result;
  }

  Frame take_claimed_frame() {
    if (!pending_ || !pending_->claimed || !pending_->frame) {
      throw std::logic_error("Combat input is not available for dispatch");
    }
    auto frame = std::move(*pending_->frame);
    pending_->frame.reset();
    return frame;
  }

  // The worker has settled the input's owned outcomes and supplied the replacement deadline.
  // A rejected or throwing replacement keeps the same claimed input and old milestone.
  combat::CombatTimeline::EnqueueResult complete_with_milestone(
      std::optional<combat::CombatTimeline::Token> previous_milestone,
      std::optional<std::int64_t> next_tick) {
    if (!pending_ || !pending_->claimed || pending_->frame) {
      return {combat::CombatTimeline::EnqueueStatus::invalid, std::nullopt};
    }
    const auto result = pending_->snapshot.timeline->complete_input_with_milestone(
        pending_->token, previous_milestone, next_tick);
    if (result.status == combat::CombatTimeline::EnqueueStatus::accepted) {
      auto owner_lifetime = pending_->owner_lifetime;
      pending_.reset();
      static_cast<void>(owner_lifetime);
      return result;
    }
    return result;
  }

  // An executing claim is deliberately retained for its owner to settle, including on close.
  bool cancel_queued() {
    if (!pending_) {
      return true;
    }
    if (pending_->claimed || !pending_->snapshot.timeline->cancel_queued(pending_->token)) {
      return false;
    }
    auto owner_lifetime = pending_->owner_lifetime;
    pending_.reset();
    static_cast<void>(owner_lifetime);
    return true;
  }

private:
  struct Pending {
    Snapshot snapshot;
    combat::CombatTimeline::Token token;
    std::optional<Frame> frame;
    std::shared_ptr<void> owner_lifetime;
    bool claimed{};
  };

  std::optional<Snapshot> snapshot_;
  std::optional<Pending> pending_;
};

} // namespace chronicles
