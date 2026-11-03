#pragma once

#include "world/features/combat/combat_timeline.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace chronicles {

// One native connection's I/O-owned admission state. The population timeline orders
// both requests and scheduled work; the existing session worker executes a claimed item.
class CombatWorkSchedule final {
public:
  using Timeline = combat::CombatTimeline;

  struct Work {
    Timeline::Token token;
    Timeline::Kind kind;
  };

  CombatWorkSchedule(std::shared_ptr<Timeline> timeline, std::uint64_t connection_id,
                     std::shared_ptr<void> lifetime)
      : timeline_(std::move(timeline)), lifetime_(std::move(lifetime)) {
    if (!timeline_ || !lifetime_) {
      throw std::invalid_argument("Combat schedule requires a population and an owner");
    }
    const auto owner = timeline_->register_owner(connection_id);
    if (!owner) {
      throw std::runtime_error("Cannot register native combat schedule");
    }
    owner_ = *owner;
  }

  const std::shared_ptr<Timeline> &timeline() const noexcept {
    return timeline_;
  }

  Timeline::Owner owner() const noexcept {
    return owner_;
  }

  const std::optional<Work> &active() const noexcept {
    return active_;
  }

  bool closing() const noexcept {
    return close_.has_value();
  }

  void initialize(std::optional<std::int64_t> deadline) {
    if (milestone_ || input_ || active_ || close_) {
      throw std::logic_error("Combat schedule is already initialized");
    }
    if (deadline) {
      milestone_ = accepted(timeline_->enqueue(owner_, *deadline, Timeline::Kind::milestone));
    }
  }

  void submit_input(std::int64_t ingress_tick) {
    if (input_ || close_) {
      throw std::logic_error("Combat schedule cannot accept another input");
    }
    input_ = accepted(timeline_->enqueue(owner_, ingress_tick, Timeline::Kind::input));
  }

  void submit_close(std::int64_t close_tick) {
    if (close_) {
      return;
    }
    if (input_ && (!active_ || active_->token != *input_)) {
      if (!timeline_->cancel_queued(*input_)) {
        throw std::logic_error("Cannot cancel unstarted combat input");
      }
      input_.reset();
    }
    close_ = accepted(timeline_->enqueue(owner_, close_tick, Timeline::Kind::close));
  }

  std::optional<Work> next() const {
    std::optional<Work> result;
    const auto include = [&](const std::optional<Timeline::Token> &token, Timeline::Kind kind) {
      if (token && (!result || std::tie(token->tick, token->sequence) <
                                   std::tie(result->token.tick, result->token.sequence))) {
        result = Work{*token, kind};
      }
    };
    include(input_, Timeline::Kind::input);
    include(milestone_, Timeline::Kind::milestone);
    include(close_, Timeline::Kind::close);
    return result;
  }

  Timeline::ClaimResult claim(std::int64_t now_tick) {
    const auto work = next();
    if (active_ || !work) {
      return {Timeline::ClaimStatus::stale, std::nullopt};
    }
    const auto result = timeline_->try_claim(work->token, now_tick);
    if (result.status != Timeline::ClaimStatus::ready) {
      return result;
    }
    active_ = work;
    active_staged_ = false;
    if (work->kind == Timeline::Kind::milestone && !timeline_->begin_milestone(work->token)) {
      throw std::logic_error("Cannot begin admitted combat milestone");
    }
    if (work->kind == Timeline::Kind::close && milestone_) {
      if (!timeline_->cancel_milestone_after_close(work->token, *milestone_)) {
        throw std::logic_error("Cannot retire future combat work at ordered close");
      }
      milestone_.reset();
    }
    return result;
  }

  // Call only after the worker result is retained. Failure leaves the same claim held;
  // callers retry this transition, never the gameplay operation.
  void complete(std::optional<std::int64_t> deadline) {
    if (!active_) {
      throw std::logic_error("Combat schedule has no completed work");
    }
    if (close_ && deadline && *deadline > close_->tick) {
      deadline.reset();
    }
    const auto work = *active_;
    if (work.kind == Timeline::Kind::input) {
      milestone_ =
          accepted(timeline_->complete_input_with_milestone(work.token, milestone_, deadline));
      input_.reset();
    } else {
      if (work.kind == Timeline::Kind::milestone && !active_staged_) {
        if (!timeline_->mark_milestone_staged(work.token)) {
          throw std::logic_error("Cannot stage completed combat milestone");
        }
        active_staged_ = true;
      }
      const auto successor = deadline ? std::optional<Timeline::Successor>(Timeline::Successor{
                                            *deadline, Timeline::Kind::milestone})
                                      : std::nullopt;
      const auto token = accepted(timeline_->complete(work.token, successor));
      if (work.kind == Timeline::Kind::milestone) {
        milestone_ = token;
      } else {
        close_.reset();
      }
    }
    active_.reset();
  }

  void retire() {
    if (active_ || input_ || milestone_ || close_ || !timeline_->seal_owner(owner_) ||
        !timeline_->retire_owner_if_idle(owner_)) {
      throw std::logic_error("Cannot retire native combat owner with unresolved work");
    }
    lifetime_.reset();
  }

private:
  static std::optional<Timeline::Token> accepted(const Timeline::EnqueueResult &result) {
    if (result.status != Timeline::EnqueueStatus::accepted) {
      throw std::runtime_error("Combat timeline rejected native work or its successor");
    }
    return result.token;
  }

  std::shared_ptr<Timeline> timeline_;
  std::shared_ptr<void> lifetime_;
  Timeline::Owner owner_;
  std::optional<Timeline::Token> input_;
  std::optional<Timeline::Token> milestone_;
  std::optional<Timeline::Token> close_;
  std::optional<Work> active_;
  bool active_staged_{};
};

} // namespace chronicles
