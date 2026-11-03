#include "world/core/session_event_bus.hpp"

#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

template <typename Event>
void validate_subscribers(const std::vector<SessionEventBus::Subscriber<Event>> &subscribers) {
  for (const auto &subscriber : subscribers) {
    if (!subscriber) {
      throw std::invalid_argument("Session event subscriber is empty");
    }
  }
}

template <typename Event>
void publish_to(const std::vector<SessionEventBus::Subscriber<Event>> &subscribers,
                const Event &event) {
  for (const auto &subscriber : subscribers) {
    subscriber(event);
  }
}

} // namespace

SessionEventBus::SessionEventBus(Subscriptions subscriptions)
    : subscriptions_(std::move(subscriptions)) {
  validate_subscribers(subscriptions_.protection);
  validate_subscribers(subscriptions_.visibility);
  validate_subscribers(subscriptions_.player_state);
  validate_subscribers(subscriptions_.company);
  validate_subscribers(subscriptions_.activity_experience);
  validate_subscribers(subscriptions_.progress);
  validate_subscribers(subscriptions_.inventory_stats);
  validate_subscribers(subscriptions_.inventory_snapshot);
  validate_subscribers(subscriptions_.equipment_revision);
  validate_subscribers(subscriptions_.world_loot_progress);
  validate_subscribers(subscriptions_.progression_snapshot);
  validate_subscribers(subscriptions_.player_level);
  validate_subscribers(subscriptions_.player_life);
}

void SessionEventBus::publish(const PlayerProtectionChanged &event) const {
  publish_to(subscriptions_.protection, event);
}

void SessionEventBus::publish(const PlayerVisibilityChanged &event) const {
  publish_to(subscriptions_.visibility, event);
}

void SessionEventBus::publish(const PlayerCombatSnapshotChanged &event) const {
  publish_to(subscriptions_.player_state, event);
}

void SessionEventBus::publish(const CompanyMembershipChanged &event) const {
  publish_to(subscriptions_.company, event);
}

void SessionEventBus::publish(const CharacterActivityExperienceGained &event) const {
  publish_to(subscriptions_.activity_experience, event);
}

void SessionEventBus::publish(const PlayerProgressCommitted &event) const {
  publish_to(subscriptions_.progress, event);
}

void SessionEventBus::publish(const PlayerInventoryStatsChanged &event) const {
  publish_to(subscriptions_.inventory_stats, event);
}

void SessionEventBus::publish(const PlayerInventorySnapshotChanged &event) const {
  publish_to(subscriptions_.inventory_snapshot, event);
}

void SessionEventBus::publish(const PlayerEquipmentRevisionCommitted &event) const {
  publish_to(subscriptions_.equipment_revision, event);
}

void SessionEventBus::publish(const PlayerWorldLootProgressCommitted &event) const {
  publish_to(subscriptions_.world_loot_progress, event);
}

void SessionEventBus::publish(const PlayerProgressionSnapshotChanged &event) const {
  publish_to(subscriptions_.progression_snapshot, event);
}

void SessionEventBus::publish(const PlayerLevelChanged &event) const {
  publish_to(subscriptions_.player_level, event);
}

void SessionEventBus::publish(const PlayerLifeCommitted &event) const {
  publish_to(subscriptions_.player_life, event);
}

} // namespace chronicles
