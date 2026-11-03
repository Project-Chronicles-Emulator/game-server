#include "world/core/quest_effect_dispatcher.hpp"
#include "persistence/memory_quest_service_coordinator.hpp"
#include "testing/memory_repositories.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace chronicles {
namespace {

QuestServiceEffectEvent event_for(std::int64_t character_id, std::uint64_t sequence,
                                  char event_digit) {
  QuestServiceEffectEvent event;
  event.account_id = character_id;
  event.character_id = character_id;
  event.world_id = 1;
  event.effect_id = std::string(32, event_digit);
  event.enqueue_sequence = sequence;
  return event;
}

void add_event(MemoryRepositories &repositories, const QuestServiceEffectEvent &event) {
  std::lock_guard lock(repositories.database->quest_services->mutex);
  ASSERT_TRUE(repositories.database->quest_services->record_event(event));
}

TEST(QuestEffectDispatcher, ProcessesEachCharacterInEventOrderAcrossBothComponents) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  const auto first = event_for(1, 1, 'a');
  const auto second = event_for(1, 2, 'b');
  add_event(repositories, first);
  add_event(repositories, second);
  std::vector<std::string> order;
  QuestEffectComponent company;
  company.settle = [&](const QuestServiceEffectEvent &event) {
    order.push_back("company:" + event.effect_id);
    std::lock_guard lock(repositories.database->quest_services->mutex);
    auto &stored = repositories.database->quest_services->events.at(
        {event.character_id, event.effect_id});
    EXPECT_TRUE(repositories.database->quest_services->can_mark_component(
        stored, QuestServiceComponent::company));
    repositories.database->quest_services->mark_component_applied(
        stored, QuestServiceComponent::company);
    return QuestEffectSettlementStatus::applied;
  };
  QuestEffectComponent guild;
  guild.settle = [&](const QuestServiceEffectEvent &event) {
    order.push_back("guild:" + event.effect_id);
    std::lock_guard lock(repositories.database->quest_services->mutex);
    auto &stored = repositories.database->quest_services->events.at(
        {event.character_id, event.effect_id});
    EXPECT_TRUE(repositories.database->quest_services->can_mark_component(
        stored, QuestServiceComponent::guild));
    repositories.database->quest_services->mark_component_applied(
        stored, QuestServiceComponent::guild);
    return QuestEffectSettlementStatus::applied;
  };
  QuestEffectDispatcher dispatcher(repositories.character_state, company, guild);
  EXPECT_EQ(dispatcher.dispatch(1, {}, 10).completed, 1U);
  EXPECT_EQ(dispatcher.dispatch(1, {}, 10).completed, 1U);
  EXPECT_EQ(dispatcher.dispatch(1, {}, 10).completed, 0U);
  EXPECT_EQ(order, (std::vector<std::string>{"company:" + first.effect_id,
                                              "guild:" + first.effect_id,
                                              "company:" + second.effect_id,
                                              "guild:" + second.effect_id}));
}

TEST(QuestEffectDispatcher, ReconcilesLostAcknowledgementEvenAfterOutboxDisappears) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  auto event = event_for(1, 1, 'c');
  event.guild_applied = true;
  add_event(repositories, event);
  bool uncertain = false;
  int writes = 0;
  int resyncs = 0;
  QuestEffectComponent company;
  company.settle = [&](const QuestServiceEffectEvent &candidate) -> QuestEffectSettlementStatus {
    ++writes;
    {
      std::lock_guard lock(repositories.database->quest_services->mutex);
      auto &stored = repositories.database->quest_services->events.at(
          {candidate.character_id, candidate.effect_id});
      EXPECT_TRUE(repositories.database->quest_services->can_mark_component(
          stored, QuestServiceComponent::company));
      repositories.database->quest_services->mark_component_applied(
          stored, QuestServiceComponent::company);
    }
    uncertain = true;
    throw std::runtime_error("Synthetic lost acknowledgement");
  };
  company.reconcile_uncertain = [&]() -> std::optional<QuestEffectReconciliation> {
    if (!uncertain) {
      return std::nullopt;
    }
    uncertain = false;
    return QuestEffectReconciliation{QuestEffectSettlementStatus::already_applied, 1,
                                     event.effect_id, true};
  };
  company.resynchronize = [&](std::int64_t character_id) {
    EXPECT_EQ(character_id, 1);
    ++resyncs;
  };
  QuestEffectDispatcher dispatcher(repositories.character_state, company, {});
  EXPECT_TRUE(dispatcher.dispatch(1, {}, 10).retry_needed);
  EXPECT_TRUE(repositories.character_state->pending_quest_service_effects(1, {}, 10).empty());
  EXPECT_FALSE(dispatcher.dispatch(1, {}, 10).retry_needed);
  EXPECT_EQ(writes, 1);
  EXPECT_EQ(resyncs, 1);
}

TEST(QuestEffectDispatcher, CompletedCompanyReadbackSurvivesGuildReadbackFailure) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  auto event = event_for(1, 1, 'e');
  event.guild_applied = true;
  add_event(repositories, event);
  bool company_uncertain = false;
  bool guild_unavailable = false;
  int company_notices = 0;
  int guild_notices = 0;
  QuestEffectComponent company;
  company.settle = [&](const QuestServiceEffectEvent &candidate) -> QuestEffectSettlementStatus {
    {
      std::lock_guard lock(repositories.database->quest_services->mutex);
      auto &stored = repositories.database->quest_services->events.at(
          {candidate.character_id, candidate.effect_id});
      repositories.database->quest_services->mark_component_applied(
          stored, QuestServiceComponent::company);
    }
    company_uncertain = true;
    guild_unavailable = true;
    throw std::runtime_error("Synthetic lost company acknowledgement");
  };
  company.reconcile_uncertain = [&]() -> std::optional<QuestEffectReconciliation> {
    if (!company_uncertain) {
      return std::nullopt;
    }
    company_uncertain = false;
    return QuestEffectReconciliation{QuestEffectSettlementStatus::already_applied,
                                     1, event.effect_id, true};
  };
  company.resynchronize = [&](std::int64_t) { ++company_notices; };
  QuestEffectComponent guild;
  guild.reconcile_uncertain = [&]() -> std::optional<QuestEffectReconciliation> {
    if (guild_unavailable) {
      throw std::runtime_error("Synthetic unavailable guild readback");
    }
    return std::nullopt;
  };
  guild.resynchronize = [&](std::int64_t) { ++guild_notices; };
  QuestEffectDispatcher dispatcher(repositories.character_state, company, guild);
  EXPECT_TRUE(dispatcher.dispatch(1, {}, 10).retry_needed);
  guild_unavailable = true;
  EXPECT_TRUE(dispatcher.dispatch(1, {}, 10).retry_needed);
  EXPECT_EQ(company_notices, 0);
  EXPECT_EQ(guild_notices, 0);
  guild_unavailable = false;
  EXPECT_FALSE(dispatcher.dispatch(1, {}, 10).retry_needed);
  EXPECT_EQ(company_notices, 1);
  EXPECT_EQ(guild_notices, 1);
}

TEST(QuestEffectDispatcher, ActivityOnlyEventRetriesNotificationAfterBothComponentsCommit) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  auto event = event_for(1, 1, 'f');
  event.effects.expected_company_type = 1;
  event.effects.expected_guild_id = 7;
  event.effects.activity_experience = 25;
  event.effects.activity_company_rate = 20;
  event.effects.activity_guild_rate = 10;
  add_event(repositories, event);
  int company_writes = 0;
  int guild_writes = 0;
  int company_notices = 0;
  int guild_attempts = 0;
  auto apply = [&](const QuestServiceEffectEvent &candidate, QuestServiceComponent component) {
    std::lock_guard lock(repositories.database->quest_services->mutex);
    auto &stored = repositories.database->quest_services->events.at(
        {candidate.character_id, candidate.effect_id});
    EXPECT_TRUE(repositories.database->quest_services->can_mark_component(stored, component));
    repositories.database->quest_services->mark_component_applied(stored, component);
    return QuestEffectSettlementStatus::applied;
  };
  QuestEffectComponent company;
  company.settle = [&](const QuestServiceEffectEvent &candidate) {
    ++company_writes;
    return apply(candidate, QuestServiceComponent::company);
  };
  company.resynchronize = [&](std::int64_t) {
    EXPECT_EQ(guild_writes, 1);
    ++company_notices;
  };
  QuestEffectComponent guild;
  guild.settle = [&](const QuestServiceEffectEvent &candidate) {
    ++guild_writes;
    return apply(candidate, QuestServiceComponent::guild);
  };
  guild.resynchronize = [&](std::int64_t) {
    if (++guild_attempts == 1) {
      throw std::runtime_error("Synthetic notification failure");
    }
  };
  QuestEffectDispatcher dispatcher(repositories.character_state, company, guild);
  const auto first = dispatcher.dispatch(1, {}, 10);
  EXPECT_EQ(first.completed, 1U);
  EXPECT_TRUE(first.retry_needed);
  EXPECT_FALSE(dispatcher.dispatch(1, {}, 10).retry_needed);
  EXPECT_EQ(company_writes, 1);
  EXPECT_EQ(guild_writes, 1);
  EXPECT_EQ(company_notices, 1);
  EXPECT_EQ(guild_attempts, 2);
}

TEST(QuestEffectDispatcher, StartupRecoversQueuedEventsUntilEmptyAndEnforcesBound) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  add_event(repositories, event_for(1, 1, 'a'));
  add_event(repositories, event_for(1, 2, 'b'));
  auto component = [&](QuestServiceComponent kind) {
    QuestEffectComponent result;
    result.settle = [&, kind](const QuestServiceEffectEvent &candidate) {
      std::lock_guard lock(repositories.database->quest_services->mutex);
      auto &stored = repositories.database->quest_services->events.at(
          {candidate.character_id, candidate.effect_id});
      EXPECT_TRUE(repositories.database->quest_services->can_mark_component(stored, kind));
      repositories.database->quest_services->mark_component_applied(stored, kind);
      return QuestEffectSettlementStatus::applied;
    };
    return result;
  };
  QuestEffectDispatcher dispatcher(repositories.character_state,
                                   component(QuestServiceComponent::company),
                                   component(QuestServiceComponent::guild));
  EXPECT_THROW(dispatcher.recover_startup(1, 1), std::runtime_error);
  EXPECT_EQ(dispatcher.recover_startup(1, 10), 1U);
  EXPECT_TRUE(repositories.character_state->pending_quest_service_effects(1, {}, 1).empty());
}

TEST(QuestEffectDispatcher, RotatesPastManyBlockedCharactersAndReclaimsReservations) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  constexpr std::int64_t blocked_count = 1030;
  for (std::int64_t character_id = 1; character_id <= blocked_count; ++character_id) {
    auto event = event_for(character_id, character_id, 'a');
    event.guild_applied = true;
    add_event(repositories, event);
  }
  auto healthy = event_for(blocked_count + 1, blocked_count + 1, 'b');
  healthy.guild_applied = true;
  add_event(repositories, healthy);
  std::vector<std::int64_t> committed;
  QuestEffectComponent company;
  company.settle = [&](const QuestServiceEffectEvent &candidate) {
    if (candidate.character_id <= blocked_count) {
      return QuestEffectSettlementStatus::retry;
    }
    std::lock_guard lock(repositories.database->quest_services->mutex);
    auto &stored = repositories.database->quest_services->events.at(
        {candidate.character_id, candidate.effect_id});
    EXPECT_TRUE(repositories.database->quest_services->can_mark_component(
        stored, QuestServiceComponent::company));
    repositories.database->quest_services->mark_component_applied(
        stored, QuestServiceComponent::company);
    committed.push_back(candidate.character_id);
    return QuestEffectSettlementStatus::applied;
  };
  QuestEffectDispatcher dispatcher(repositories.character_state, company, {});
  for (int pass = 0; pass < 9; ++pass) {
    dispatcher.dispatch(1, {}, kQuestServiceEffectPageSize);
  }
  EXPECT_EQ(committed, (std::vector<std::int64_t>{healthy.character_id}));
  auto newcomer = event_for(blocked_count + 2, blocked_count + 2, 'c');
  newcomer.guild_applied = true;
  add_event(repositories, newcomer);
  dispatcher.dispatch(1, {}, kQuestServiceEffectPageSize);
  EXPECT_EQ(committed, (std::vector<std::int64_t>{healthy.character_id, newcomer.character_id}));
  // After wrapping, blocked entries remain durable and are tried again rather than dropped.
  EXPECT_TRUE(dispatcher.dispatch(1, {}, kQuestServiceEffectPageSize).retry_needed);
  EXPECT_EQ(repositories.character_state->pending_quest_service_effects(1, {}, 1).size(), 1U);
}

TEST(QuestEffectDispatcher, PendingReconciliationDoesNotReapplyOrSkipTheEvent) {
  MemoryRepositories repositories;
  repositories.character_state->acquire_monster_reward_owner_epoch();
  auto event = event_for(1, 1, 'd');
  event.guild_applied = true;
  add_event(repositories, event);
  int reconciliations = 0;
  int writes = 0;
  QuestEffectComponent company;
  company.reconcile_uncertain = [&]() -> std::optional<QuestEffectReconciliation> {
    ++reconciliations;
    if (reconciliations == 1) {
      return QuestEffectReconciliation{QuestEffectSettlementStatus::retry, 1};
    }
    return QuestEffectReconciliation{QuestEffectSettlementStatus::pending, 1};
  };
  company.settle = [&](const QuestServiceEffectEvent &candidate) {
    ++writes;
    std::lock_guard lock(repositories.database->quest_services->mutex);
    auto &stored = repositories.database->quest_services->events.at(
        {candidate.character_id, candidate.effect_id});
    EXPECT_TRUE(repositories.database->quest_services->can_mark_component(
        stored, QuestServiceComponent::company));
    repositories.database->quest_services->mark_component_applied(
        stored, QuestServiceComponent::company);
    return QuestEffectSettlementStatus::applied;
  };
  QuestEffectDispatcher dispatcher(repositories.character_state, company, {});
  EXPECT_TRUE(dispatcher.dispatch(1, {}, 10).retry_needed);
  EXPECT_EQ(writes, 0);
  EXPECT_EQ(dispatcher.dispatch(1, {}, 10).completed, 1U);
  EXPECT_EQ(writes, 1);
}

} // namespace
} // namespace chronicles
