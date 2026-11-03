#include "world/features/combat/combat_timeline.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronicles::combat {

CombatTimeline::CombatTimeline(std::size_t capacity, bool require_registered_owner)
    : capacity_(capacity), require_registered_owner_(require_registered_owner) {
  if (capacity == 0) {
    throw std::invalid_argument("Combat timeline capacity must be positive");
  }
}

std::optional<CombatTimeline::Owner> CombatTimeline::register_owner(std::uint64_t owner_id) {
  std::lock_guard lock(mutex_);
  if (!require_registered_owner_ || owner_id == 0 || registered_owners_.contains(owner_id) ||
      registered_owners_.size() >= capacity_ ||
      next_owner_generation_ == (std::numeric_limits<std::uint64_t>::max)()) {
    return std::nullopt;
  }
  const Owner owner{owner_id, next_owner_generation_};
  registered_owners_.emplace(owner_id, OwnerRegistration{owner});
  ++next_owner_generation_;
  return owner;
}

bool CombatTimeline::seal_owner(Owner owner) {
  std::lock_guard lock(mutex_);
  const auto found = registered_owners_.find(owner.id);
  if (!require_registered_owner_ || found == registered_owners_.end() ||
      found->second.owner != owner) {
    return false;
  }
  found->second.sealed = true;
  return true;
}

bool CombatTimeline::retire_owner_if_idle(Owner owner) {
  std::lock_guard lock(mutex_);
  const auto found = registered_owners_.find(owner.id);
  if (!require_registered_owner_ || found == registered_owners_.end() ||
      found->second.owner != owner || !found->second.sealed ||
      owner_milestones_.contains(owner) ||
      std::ranges::any_of(entries_, [&](const auto &entry) {
        return entry.second.owner == owner;
      })) {
    return false;
  }
  registered_owners_.erase(found);
  return true;
}

std::optional<CombatTimeline::Token> CombatTimeline::insert_locked(Owner owner,
                                                                  std::int64_t tick,
                                                                  Kind kind,
                                                                  std::optional<Token> replaced) {
  if (next_sequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
    return std::nullopt;
  }
  if (before_insert_hook_for_test_) {
    before_insert_hook_for_test_();
  }
  const Token token{tick, next_sequence_, owner};
  auto existing = owner_milestones_.find(owner);
  if (kind == Kind::milestone &&
      ((existing != owner_milestones_.end() && (!replaced || existing->second != *replaced)) ||
       (existing == owner_milestones_.end() && replaced))) {
    throw std::logic_error("Combat owner milestone index does not match replacement");
  }
  const auto [entry, inserted] =
      entries_.emplace(Key{tick, next_sequence_}, Entry{owner, kind, false, false});
  if (!inserted) {
    throw std::logic_error("Combat timeline sequence was reused");
  }
  try {
    if (after_insert_hook_for_test_) {
      after_insert_hook_for_test_();
    }
    if (kind == Kind::milestone) {
      if (existing == owner_milestones_.end()) {
        owner_milestones_.emplace(owner, token);
      } else {
        existing->second = token;
      }
    }
  } catch (...) {
    entries_.erase(entry);
    throw;
  }
  ++next_sequence_;
  return token;
}

CombatTimeline::EnqueueResult CombatTimeline::enqueue(Owner owner, std::int64_t tick, Kind kind) {
  std::lock_guard lock(mutex_);
  if (owner.id == 0 || owner.generation == 0 || tick < 0) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (frontier_tick_ && tick < *frontier_tick_) {
    return {EnqueueStatus::stale_tick, std::nullopt};
  }
  auto registration = registered_owners_.end();
  if (require_registered_owner_) {
    registration = registered_owners_.find(owner.id);
    if (registration == registered_owners_.end() || registration->second.owner != owner ||
        (registration->second.sealed && kind != Kind::close) ||
        (kind == Kind::close && registration->second.close_submitted)) {
      return {EnqueueStatus::invalid, std::nullopt};
    }
  }
  if (kind == Kind::milestone && owner_milestones_.contains(owner)) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (entries_.size() >= capacity_) {
    return {EnqueueStatus::full, std::nullopt};
  }
  const auto token = insert_locked(owner, tick, kind);
  if (!token) {
    return {EnqueueStatus::sequence_exhausted, std::nullopt};
  }
  if (require_registered_owner_ && kind == Kind::close) {
    registration->second.sealed = true;
    registration->second.close_submitted = true;
  }
  return {EnqueueStatus::accepted, token};
}

CombatTimeline::ClaimResult CombatTimeline::try_claim(const Token &token, std::int64_t now_tick) {
  std::lock_guard lock(mutex_);
  const auto entry = entries_.find(Key{token.tick, token.sequence});
  if (entry == entries_.end() || entry->second.owner != token.owner) {
    return {ClaimStatus::stale, std::nullopt};
  }
  const auto &head = *entries_.begin();
  if (claimed_ || entry != entries_.begin()) {
    return {ClaimStatus::waiting, head.second.owner};
  }
  if (token.tick > now_tick) {
    return {ClaimStatus::not_due, head.second.owner};
  }
  claimed_ = token;
  frontier_tick_ = token.tick;
  return {ClaimStatus::ready, token.owner};
}

bool CombatTimeline::is_claimed(const Token &token) const {
  std::lock_guard lock(mutex_);
  return claimed_ && *claimed_ == token;
}

bool CombatTimeline::begin_milestone(const Token &token) {
  std::lock_guard lock(mutex_);
  if (!claimed_ || *claimed_ != token) {
    return false;
  }
  const auto entry = entries_.find(Key{token.tick, token.sequence});
  if (entry == entries_.end() || entry->second.kind != Kind::milestone ||
      entry->second.executing || entry->second.staged) {
    return false;
  }
  entry->second.executing = true;
  return true;
}

bool CombatTimeline::mark_milestone_staged(const Token &token) {
  std::lock_guard lock(mutex_);
  if (!claimed_ || *claimed_ != token) {
    return false;
  }
  const auto entry = entries_.find(Key{token.tick, token.sequence});
  if (entry == entries_.end() || entry->second.kind != Kind::milestone ||
      !entry->second.executing || entry->second.staged) {
    return false;
  }
  entry->second.staged = true;
  return true;
}

CombatTimeline::EnqueueResult CombatTimeline::replace_unstarted_milestone(
    const Token &token, std::optional<Successor> successor) {
  std::lock_guard lock(mutex_);
  if (!claimed_ || *claimed_ != token) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  const auto current = entries_.find(Key{token.tick, token.sequence});
  if (current == entries_.end() || current->second.kind != Kind::milestone ||
      current->second.executing || current->second.staged) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  const auto indexed = owner_milestones_.find(token.owner);
  if (indexed == owner_milestones_.end() || indexed->second != token) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (successor && successor->kind != Kind::milestone) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (successor && successor->tick < token.tick) {
    return {EnqueueStatus::stale_tick, std::nullopt};
  }
  std::optional<Token> next;
  if (successor) {
    next = insert_locked(token.owner, successor->tick, successor->kind,
                         successor->kind == Kind::milestone ? std::optional<Token>(token)
                                                            : std::nullopt);
    if (!next) {
      return {EnqueueStatus::sequence_exhausted, std::nullopt};
    }
  }
  if (!successor || successor->kind != Kind::milestone) {
    owner_milestones_.erase(indexed);
  }
  entries_.erase(current);
  claimed_.reset();
  return {EnqueueStatus::accepted, next};
}

CombatTimeline::EnqueueResult CombatTimeline::complete(
    const Token &token, std::optional<Successor> successor) {
  std::lock_guard lock(mutex_);
  if (!claimed_ || *claimed_ != token) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  const auto current = entries_.find(Key{token.tick, token.sequence});
  if (current == entries_.end() || current->second.kind == Kind::input ||
      (current->second.kind == Kind::milestone && !current->second.staged)) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  const auto indexed = owner_milestones_.find(token.owner);
  if (current->second.kind == Kind::close &&
      (indexed != owner_milestones_.end() || successor)) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (successor && successor->kind != Kind::milestone) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (current->second.kind == Kind::milestone &&
      (indexed == owner_milestones_.end() || indexed->second != token)) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (current->second.kind != Kind::milestone && successor &&
      successor->kind == Kind::milestone && indexed != owner_milestones_.end()) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (successor && successor->tick < token.tick) {
    return {EnqueueStatus::stale_tick, std::nullopt};
  }
  std::optional<Token> next;
  if (successor) {
    // Insert before removing the head. An allocation failure leaves the claimed work
    // intact, so its owner can retry publication without applying the effect again.
    next = insert_locked(token.owner, successor->tick, successor->kind,
                         current->second.kind == Kind::milestone &&
                                 successor->kind == Kind::milestone
                             ? std::optional<Token>(token)
                             : std::nullopt);
    if (!next) {
      return {EnqueueStatus::sequence_exhausted, std::nullopt};
    }
  }
  if (current->second.kind == Kind::milestone &&
      (!successor || successor->kind != Kind::milestone)) {
    owner_milestones_.erase(indexed);
  }
  entries_.erase(current);
  claimed_.reset();
  return {EnqueueStatus::accepted, next};
}

CombatTimeline::EnqueueResult CombatTimeline::complete_input_with_milestone(
    const Token &input, std::optional<Token> previous_milestone,
    std::optional<std::int64_t> next_tick) {
  std::lock_guard lock(mutex_);
  if (!claimed_ || *claimed_ != input) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  const auto current = entries_.find(Key{input.tick, input.sequence});
  if (current == entries_.end() || current->second.owner != input.owner ||
      current->second.kind != Kind::input) {
    return {EnqueueStatus::invalid, std::nullopt};
  }

  auto previous = entries_.end();
  const auto indexed = owner_milestones_.find(input.owner);
  if (!previous_milestone && indexed != owner_milestones_.end()) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (previous_milestone &&
      (indexed == owner_milestones_.end() || indexed->second != *previous_milestone)) {
    return {EnqueueStatus::invalid, std::nullopt};
  }
  if (previous_milestone) {
    previous = entries_.find(Key{previous_milestone->tick, previous_milestone->sequence});
    if (previous_milestone->owner != input.owner || previous == entries_.end() ||
        previous->second.owner != input.owner || previous->second.kind != Kind::milestone ||
        previous->second.executing || previous->second.staged || previous == current) {
      return {EnqueueStatus::invalid, std::nullopt};
    }
  }
  if (next_tick && *next_tick < input.tick) {
    return {EnqueueStatus::stale_tick, std::nullopt};
  }

  std::optional<Token> next;
  const bool keep_previous = previous_milestone && next_tick &&
                             previous_milestone->tick == *next_tick;
  if (keep_previous) {
    next = previous_milestone;
  } else if (next_tick) {
    next = insert_locked(input.owner, *next_tick, Kind::milestone, previous_milestone);
    if (!next) {
      return {EnqueueStatus::sequence_exhausted, std::nullopt};
    }
  }
  if (previous_milestone && !keep_previous) {
    entries_.erase(previous);
    if (!next_tick) {
      owner_milestones_.erase(indexed);
    }
  }
  entries_.erase(current);
  claimed_.reset();
  return {EnqueueStatus::accepted, next};
}

void CombatTimeline::set_before_insert_hook_for_test(std::function<void()> hook) {
  std::lock_guard lock(mutex_);
  before_insert_hook_for_test_ = std::move(hook);
}

void CombatTimeline::set_after_insert_hook_for_test(std::function<void()> hook) {
  std::lock_guard lock(mutex_);
  after_insert_hook_for_test_ = std::move(hook);
}

bool CombatTimeline::cancel_queued(const Token &token) {
  std::lock_guard lock(mutex_);
  if (claimed_ && *claimed_ == token) {
    return false;
  }
  const auto entry = entries_.find(Key{token.tick, token.sequence});
  if (entry == entries_.end() || entry->second.owner != token.owner ||
      entry->second.kind != Kind::input) {
    return false;
  }
  entries_.erase(entry);
  return true;
}

bool CombatTimeline::cancel_milestone_after_close(const Token &close, const Token &milestone) {
  std::lock_guard lock(mutex_);
  if (!claimed_ || *claimed_ != close || close.owner != milestone.owner) {
    return false;
  }
  const auto close_entry = entries_.find(Key{close.tick, close.sequence});
  const auto target = entries_.find(Key{milestone.tick, milestone.sequence});
  if (close_entry == entries_.end() || close_entry->second.kind != Kind::close ||
      target == entries_.end() || target->second.kind != Kind::milestone ||
      target->second.owner != milestone.owner ||
      !(close_entry->first < target->first)) {
    return false;
  }
  const auto indexed = owner_milestones_.find(milestone.owner);
  if (indexed == owner_milestones_.end() || indexed->second != milestone) {
    return false;
  }
  owner_milestones_.erase(indexed);
  entries_.erase(target);
  return true;
}

std::size_t CombatTimeline::size() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

std::optional<CombatTimeline::Owner> CombatTimeline::head_owner() const {
  std::lock_guard lock(mutex_);
  if (entries_.empty()) {
    return std::nullopt;
  }
  return entries_.begin()->second.owner;
}

bool CombatTimeline::owner_has_work(Owner owner) const {
  std::lock_guard lock(mutex_);
  return std::ranges::any_of(entries_, [&](const auto &entry) {
    return entry.second.owner == owner;
  });
}

} // namespace chronicles::combat
