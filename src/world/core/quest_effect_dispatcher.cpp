#include "world/core/quest_effect_dispatcher.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

constexpr std::size_t kMaximumPendingResynchronizations = 1024;

bool resolved(QuestEffectSettlementStatus status) {
  return status == QuestEffectSettlementStatus::applied ||
         status == QuestEffectSettlementStatus::already_applied;
}

} // namespace

QuestEffectDispatcher::QuestEffectDispatcher(std::shared_ptr<CharacterStateRepository> repository,
                                               QuestEffectComponent company,
                                               QuestEffectComponent guild)
    : repository_(std::move(repository)), company_(std::move(company)), guild_(std::move(guild)) {
  if (!repository_) {
    throw std::invalid_argument("Quest effect dispatcher requires character storage");
  }
  pending_resynchronizations_.reserve(kMaximumPendingResynchronizations);
}

bool QuestEffectDispatcher::resynchronize(const QuestEffectComponent &component,
                                           std::int64_t character_id) const {
  if (!component.resynchronize) {
    return true;
  }
  try {
    component.resynchronize(character_id);
    return true;
  } catch (const std::exception &) {
    spdlog::warn("Quest service state resynchronization could not be delivered");
    return false;
  }
}

QuestEffectDispatcher::PendingResynchronization &
QuestEffectDispatcher::reserve_resynchronization(std::int64_t character_id,
                                                  const std::string &effect_id) {
  const auto found = std::ranges::find_if(pending_resynchronizations_, [&](const auto &entry) {
    return entry.character_id == character_id && entry.effect_id == effect_id;
  });
  if (found != pending_resynchronizations_.end()) {
    return *found;
  }
  if (pending_resynchronizations_.size() == kMaximumPendingResynchronizations) {
    throw std::runtime_error("Quest state resynchronization queue is full");
  }
  pending_resynchronizations_.push_back({character_id, effect_id});
  return pending_resynchronizations_.back();
}

bool QuestEffectDispatcher::flush_resynchronizations() {
  for (auto &pending : pending_resynchronizations_) {
    if (!pending.ready) {
      continue;
    }
    if (pending.company_needed && resynchronize(company_, pending.character_id)) {
      pending.company_needed = false;
    }
    if (pending.guild_needed && resynchronize(guild_, pending.character_id)) {
      pending.guild_needed = false;
    }
  }
  std::erase_if(pending_resynchronizations_, [](const auto &pending) {
    return pending.ready && !pending.company_needed && !pending.guild_needed;
  });
  return std::ranges::none_of(pending_resynchronizations_, [](const auto &pending) {
    return pending.ready;
  });
}

bool QuestEffectDispatcher::reconcile(const QuestEffectComponent &component,
                                       std::vector<QuestEffectReconciliation> &completed) {
  if (!component.reconcile_uncertain) {
    return true;
  }
  auto result = component.reconcile_uncertain();
  if (!result) {
    return true;
  }
  if (result->status == QuestEffectSettlementStatus::pending) {
    return true;
  }
  if (!resolved(result->status)) {
    return false;
  }
  if (result->event_completed) {
    completed.push_back(std::move(*result));
  }
  return true;
}

bool QuestEffectDispatcher::settle(const QuestEffectComponent &component,
                                    const QuestServiceEffectEvent &event) {
  if (!component.settle) {
    return false;
  }
  const auto status = component.settle(event);
  if (!resolved(status)) {
    return false;
  }
  return true;
}

bool QuestEffectDispatcher::reconcile_reservations() {
  std::vector<QuestEffectReconciliation> reconciled;
  reconciled.reserve(2);
  bool company_ready = false;
  bool guild_ready = false;
  try {
    company_ready = reconcile(company_, reconciled);
  } catch (const std::exception &) {
    spdlog::warn("Company quest settlement readback will be retried");
  }
  try {
    guild_ready = reconcile(guild_, reconciled);
  } catch (const std::exception &) {
    spdlog::warn("Guild quest settlement readback will be retried");
  }
  // Preserve completed readbacks even if the other component is unavailable. No notices
  // are sent until both service caches can be inspected safely.
  for (const auto &entry : reconciled) {
    auto &pending = reserve_resynchronization(entry.character_id, entry.effect_id);
    pending.ready = true;
    pending.company_needed = true;
    pending.guild_needed = true;
  }
  if (!company_ready || !guild_ready) {
    return false;
  }
  // Once both uncertain operations are resolved, each unready event is still durable in
  // the outbox. Its temporary notice reservation is no longer needed until a later retry.
  std::erase_if(pending_resynchronizations_, [](const auto &pending) {
    return !pending.ready;
  });
  return true;
}

QuestEffectDispatchResult QuestEffectDispatcher::dispatch(
    int world_id, std::optional<std::int64_t> character_id, std::size_t limit) {
  if (world_id <= 0 || (character_id && *character_id <= 0) || limit == 0 ||
      limit > kQuestServiceEffectPageSize) {
    throw std::invalid_argument("Invalid quest service dispatch page");
  }
  std::lock_guard lock(mutex_);
  QuestEffectDispatchResult result;
  std::vector<QuestServiceEffectEvent> events;
  try {
    if (!reconcile_reservations()) {
      result.retry_needed = true;
      return result;
    }
    const auto after_sequence = character_id ? 0 : next_after_sequence_;
    events = repository_->pending_quest_service_effects(world_id, character_id, limit,
                                                        after_sequence);
    if (!character_id && events.empty() && next_after_sequence_ != 0) {
      next_after_sequence_ = 0;
      events = repository_->pending_quest_service_effects(world_id, character_id, limit, 0);
    }
  } catch (const std::exception &) {
    result.retry_needed = true;
    return result;
  }
  for (const auto &event : events) {
    if (!character_id) {
      next_after_sequence_ = event.enqueue_sequence;
    }
    bool incomplete = false;
    try {
      // Allocate a notice reservation before either component can commit.
      auto &pending = reserve_resynchronization(event.character_id, event.effect_id);
      if (!event.company_applied && !settle(company_, event)) {
        incomplete = true;
      } else if (!event.guild_applied && !settle(guild_, event)) {
        incomplete = true;
      } else {
        pending.ready = true;
        pending.company_needed = event.effects.expected_company_type != 0 &&
                                 (event.effects.company_contribution != 0 ||
                                  event.effects.activity_experience != 0 ||
                                  !event.effects.company_unlocks.empty());
        pending.guild_needed = event.effects.expected_guild_id != 0 &&
                               (event.effects.guild_experience != 0 ||
                                event.effects.activity_experience != 0);
        ++result.completed;
      }
    } catch (const std::exception &) {
      incomplete = true;
    }
    if (incomplete) {
      result.retry_needed = true;
      try {
        if (!reconcile_reservations()) {
          return result;
        }
      } catch (const std::exception &) {
        return result;
      }
    }
  }
  if (!flush_resynchronizations()) {
    result.retry_needed = true;
  }
  return result;
}

std::size_t QuestEffectDispatcher::recover_startup(int world_id, std::size_t maximum_events) {
  if (world_id <= 0 || maximum_events == 0) {
    throw std::invalid_argument("Invalid startup quest effect recovery bound");
  }
  std::size_t completed = 0;
  while (completed < maximum_events) {
    const auto page_size = std::min(kQuestServiceEffectPageSize, maximum_events - completed);
    const auto result = dispatch(world_id, std::nullopt, page_size);
    if (result.retry_needed) {
      throw std::runtime_error("Quest service effect recovery needs retry");
    }
    if (result.completed == 0) {
      return completed;
    }
    completed += result.completed;
  }
  if (!repository_->pending_quest_service_effects(world_id, std::nullopt, 1).empty()) {
    throw std::runtime_error("Quest service effect recovery exceeds its startup bound");
  }
  return completed;
}

} // namespace chronicles
