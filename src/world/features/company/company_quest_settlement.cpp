#include "world/features/company/company.hpp"
#include "world/features/company/company_quest_settlement.hpp"
#include "persistence/memory_quest_service_coordinator.hpp"
#include "persistence/quest_service_effect_codec.hpp"
#include "persistence/quest_service_effect_guards.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <tuple>

namespace chronicles {
namespace {

bool same_event(const QuestServiceEffectEvent &stored, const QuestServiceEffectEvent &expected) {
  return stored.enqueue_sequence == expected.enqueue_sequence &&
         stored.account_id == expected.account_id && stored.character_id == expected.character_id &&
         stored.world_id == expected.world_id && stored.effect_id == expected.effect_id &&
         stored.mutation_fingerprint == expected.mutation_fingerprint &&
         stored.monster_receipt == expected.monster_receipt && stored.effects == expected.effects &&
         stored.granted_items == expected.granted_items;
}

CompanyRecord memory_company_record(const std::map<std::int64_t, CompanyRecord> &records,
                                    std::int64_t character_id) {
  const auto entry = records.find(character_id);
  if (entry != records.end()) {
    return entry->second;
  }
  CompanyRecord empty;
  empty.char_id = character_id;
  return empty;
}

bool memory_membership_matches(const MemoryQuestServiceCoordinator &coordinator,
                               const QuestServiceEffectEvent &event, const CompanyRecord &record) {
  const auto membership = coordinator.company_memberships.find(event.character_id);
  const auto generation = coordinator.company_generations.find(event.character_id);
  const int current_company = membership == coordinator.company_memberships.end()
                                  ? 0 : membership->second;
  const std::int64_t current_generation = generation == coordinator.company_generations.end()
                                              ? 0 : generation->second;
  return record.company_type == current_company &&
         current_company == event.effects.expected_company_type &&
         current_generation == event.effects.expected_company_generation;
}

void validate_company_quest_transition(const QuestServiceEffectEvent &event,
                                       const CompanyRecord &before, const CompanyRecord &after,
                                       std::int64_t activity_contribution) {
  auto allowed = before;
  allowed.contribution = after.contribution;
  allowed.unlocked_position_level = after.unlocked_position_level;
  allowed.activity_epoch_ticks = after.activity_epoch_ticks;
  if (allowed != after || before.contribution < 0 || after.contribution < before.contribution ||
      event.effects.company_contribution < 0 ||
      activity_contribution < 0 ||
      (after.contribution - before.contribution > event.effects.company_contribution &&
       after.contribution - before.contribution - event.effects.company_contribution >
           activity_contribution) ||
      after.unlocked_position_level < before.unlocked_position_level ||
      (event.effects.company_contribution == 0 && activity_contribution == 0 &&
       after.activity_epoch_ticks != before.activity_epoch_ticks)) {
    throw std::invalid_argument("Invalid company quest settlement transition");
  }
}

struct CompanyActivityConversion {
  std::int64_t points{};
  std::int64_t remainder{};
};

CompanyActivityConversion convert_company_activity(const QuestServiceEffects &effects,
                                                   std::int64_t previous_remainder) {
  if (effects.activity_experience < 0) {
    throw std::invalid_argument("Negative company activity experience");
  }
  if (effects.activity_experience == 0) {
    return {0, previous_remainder};
  }
  const auto rate = effects.activity_company_rate;
  if (rate <= 0 || previous_remainder < 0 || previous_remainder >= rate) {
    throw std::invalid_argument("Invalid company activity conversion state");
  }
  const auto whole = effects.activity_experience / rate;
  const auto fractional = effects.activity_experience % rate;
  const bool carry = fractional >= rate - previous_remainder;
  const auto remainder = carry ? fractional - (rate - previous_remainder)
                               : fractional + previous_remainder;
  return {whole + (carry ? 1 : 0), remainder};
}

CompanyQuestSettlementResult memory_company_effect(
    MemoryQuestServiceCoordinator &coordinator, std::map<std::int64_t, CompanyRecord> &records,
    const QuestServiceEffectEvent &event, const CompanyRecord *before,
    const CompanyQuestPlanner *planner) {
  const auto found = coordinator.events.find({event.character_id, event.effect_id});
  const auto record = memory_company_record(records, event.character_id);
  if (found == coordinator.events.end() || !same_event(found->second, event)) {
    return {QuestEffectSettlementStatus::identity_mismatch, record};
  }
  if (found->second.company_applied) {
    return {QuestEffectSettlementStatus::already_applied, record, found->second.guild_applied};
  }
  if (!memory_membership_matches(coordinator, event, record)) {
    return {QuestEffectSettlementStatus::identity_mismatch, record};
  }
  if (!before || !planner) {
    return {QuestEffectSettlementStatus::pending, record};
  }
  if (!coordinator.can_mark_component(found->second, QuestServiceComponent::company) ||
      record != *before) {
    return {QuestEffectSettlementStatus::retry, record};
  }
  const auto stored = records.find(event.character_id);
  if (stored == records.end()) {
    return {QuestEffectSettlementStatus::identity_mismatch, record};
  }
  const auto key = std::make_tuple(event.world_id, event.character_id,
                                   QuestServiceComponent::company,
                                   event.effects.expected_company_generation,
                                   event.effects.activity_company_rate);
  const auto remainder_entry = coordinator.activity_remainders.find(key);
  const auto previous_remainder = remainder_entry == coordinator.activity_remainders.end()
                                      ? 0 : remainder_entry->second;
  const auto conversion = convert_company_activity(event.effects, previous_remainder);
  const auto after = (*planner)(conversion.points);
  if (after.char_id != event.character_id ||
      after.company_type != event.effects.expected_company_type) {
    return {QuestEffectSettlementStatus::identity_mismatch, record};
  }
  validate_company_quest_transition(event, *before, after, conversion.points);
  // Allocation and the planner complete before publishing any component state.
  if (event.effects.activity_experience > 0) {
    auto entry = coordinator.activity_remainders.try_emplace(key, previous_remainder).first;
    entry->second = conversion.remainder;
  }
  stored->second = after;
  coordinator.mark_component_applied(found->second, QuestServiceComponent::company);
  return {QuestEffectSettlementStatus::applied, after, found->second.guild_applied};
}

struct LockedCompanyQuest {
  QuestEffectSettlementStatus status{QuestEffectSettlementStatus::retry};
  CompanyRecord record;
  bool guild_applied{};
};

LockedCompanyQuest lock_company_quest(pqxx::work &transaction,
                                       const QuestServiceEffectEvent &event, bool for_write) {
  detail::lock_quest_service_character(transaction, event.character_id);
  const auto events = transaction.exec(
      "SELECT enqueue_sequence,account_id,world_id,mutation_fingerprint,monster_receipt,"
      "effect_payload,expected_company_type,expected_company_generation,expected_guild_id,"
      "expected_guild_generation,granted_items,company_applied,guild_applied "
      "FROM emulator_quest_service_effects WHERE character_id=$1 AND effect_id=$2 FOR UPDATE",
      pqxx::params{event.character_id, event.effect_id});
  if (events.empty()) {
    return {QuestEffectSettlementStatus::identity_mismatch, {}};
  }
  const auto &row = events.front();
  if (row[0].as<std::uint64_t>() != event.enqueue_sequence ||
      row[1].as<std::int64_t>() != event.account_id || row[2].as<int>() != event.world_id ||
      row[3].as<std::string>() != event.mutation_fingerprint ||
      row[4].as<bool>() != event.monster_receipt ||
      decode_quest_service_effects(nlohmann::json::parse(row[5].as<std::string>())) != event.effects ||
      row[6].as<int>() != event.effects.expected_company_type ||
      row[7].as<std::int64_t>() != event.effects.expected_company_generation ||
      row[8].as<std::int64_t>() != event.effects.expected_guild_id ||
      row[9].as<std::int64_t>() != event.effects.expected_guild_generation ||
      decode_quest_granted_items(nlohmann::json::parse(row[10].as<std::string>())) !=
          event.granted_items) {
    return {QuestEffectSettlementStatus::identity_mismatch, {}};
  }
  if (for_write && !row[11].as<bool>()) {
    detail::require_quest_effect_is_first(transaction, event.character_id, event.effect_id);
    detail::require_quest_company_write_allowed(transaction, event.character_id, event.effect_id);
  }
  const auto owners = transaction.exec(
      "SELECT id FROM emulator_characters WHERE id=$1 AND account_id=$2 AND world_id=$3",
      pqxx::params{event.character_id, event.account_id, event.world_id});
  if (owners.empty()) {
    return {QuestEffectSettlementStatus::identity_mismatch, {}};
  }
  const auto companies = transaction.exec(
      "SELECT company_type,contribution,join_epoch_ticks,activity_epoch_ticks,"
      "permit_expiry_epoch_ticks,unlocked_position_level FROM emulator_character_company "
      "WHERE character_id=$1 FOR UPDATE", pqxx::params{event.character_id});
  CompanyRecord record;
  record.char_id = event.character_id;
  if (!companies.empty()) {
    const auto &company = companies.front();
    record.company_type = company[0].as<int>();
    record.contribution = company[1].as<std::int64_t>();
    record.join_epoch_ticks = company[2].as<std::int64_t>();
    record.activity_epoch_ticks = company[3].as<std::int64_t>();
    record.permit_expiry_epoch_ticks = company[4].as<std::int64_t>();
    record.unlocked_position_level = company[5].as<int>();
  }
  if (row[11].as<bool>()) {
    return {QuestEffectSettlementStatus::already_applied, record, row[12].as<bool>()};
  }
  const auto generations = transaction.exec(
      "SELECT company_generation FROM emulator_quest_service_character_guards "
      "WHERE character_id=$1", pqxx::params{event.character_id});
  if (generations.empty() || record.company_type != event.effects.expected_company_type ||
      generations.front()[0].as<std::int64_t>() != event.effects.expected_company_generation) {
    return {QuestEffectSettlementStatus::identity_mismatch, record};
  }
  return {QuestEffectSettlementStatus::pending, record, row[12].as<bool>()};
}

} // namespace

void MemoryCompanyStore::adopt_quest_owner_epoch(std::int64_t epoch) {
  if (epoch <= 0) {
    throw std::invalid_argument("Invalid company quest owner epoch");
  }
  quest_owner_epoch_.store(epoch);
}

void PostgresCompanyStore::adopt_quest_owner_epoch(std::int64_t epoch) {
  if (epoch <= 0) {
    throw std::invalid_argument("Invalid company quest owner epoch");
  }
  quest_owner_epoch_.store(epoch);
}

CompanyQuestSettlementResult MemoryCompanyStore::read_quest_effect(
    const QuestServiceEffectEvent &event) {
  if (!quest_services_) {
    throw std::runtime_error("Company quest settlement coordinator is unavailable");
  }
  std::lock_guard coordinator_lock(quest_services_->mutex);
  const auto epoch = quest_owner_epoch_.load();
  if (epoch <= 0 || quest_services_->owner_epoch != epoch) {
    throw std::runtime_error("Company quest settlement owner was superseded");
  }
  std::lock_guard store_lock(mutex_);
  return memory_company_effect(*quest_services_, records_, event, nullptr, nullptr);
}

CompanyQuestSettlementResult MemoryCompanyStore::settle_quest_effect(
    const QuestServiceEffectEvent &event, const CompanyRecord &before,
    const CompanyQuestPlanner &planner) {
  if (!quest_services_) {
    throw std::runtime_error("Company quest settlement coordinator is unavailable");
  }
  std::lock_guard coordinator_lock(quest_services_->mutex);
  const auto epoch = quest_owner_epoch_.load();
  if (epoch <= 0 || quest_services_->owner_epoch != epoch) {
    throw std::runtime_error("Company quest settlement owner was superseded");
  }
  std::lock_guard store_lock(mutex_);
  return memory_company_effect(*quest_services_, records_, event, &before, &planner);
}

CompanyQuestSettlementResult PostgresCompanyStore::read_quest_effect(
    const QuestServiceEffectEvent &event) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, quest_owner_epoch_.load());
  const auto result = lock_company_quest(transaction, event, false);
  transaction.commit();
  return {result.status, result.record,
          result.status == QuestEffectSettlementStatus::already_applied && result.guild_applied};
}

CompanyQuestSettlementResult PostgresCompanyStore::settle_quest_effect(
    const QuestServiceEffectEvent &event, const CompanyRecord &before,
    const CompanyQuestPlanner &planner) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, quest_owner_epoch_.load());
  const auto current = lock_company_quest(transaction, event, true);
  if (current.status != QuestEffectSettlementStatus::pending) {
    transaction.commit();
    return {current.status, current.record, current.guild_applied};
  }
  if (current.record != before) {
    transaction.commit();
    return {QuestEffectSettlementStatus::retry, current.record, false};
  }
  std::int64_t previous_remainder = 0;
  if (event.effects.activity_experience > 0) {
    const auto rows = transaction.exec(
        "SELECT remainder FROM emulator_quest_activity_remainders "
        "WHERE world_id=$1 AND character_id=$2 AND component=0 "
        "AND membership_generation=$3 AND conversion_rate=$4 FOR UPDATE",
        pqxx::params{event.world_id, event.character_id,
                     event.effects.expected_company_generation,
                     event.effects.activity_company_rate});
    if (!rows.empty()) {
      previous_remainder = rows.front()[0].as<std::int64_t>();
    }
  }
  const auto conversion = convert_company_activity(event.effects, previous_remainder);
  const auto after = planner(conversion.points);
  validate_company_quest_transition(event, before, after, conversion.points);
  if (event.effects.activity_experience > 0) {
    transaction.exec(
        "INSERT INTO emulator_quest_activity_remainders "
        "(world_id,character_id,component,membership_generation,conversion_rate,remainder) "
        "VALUES ($1,$2,0,$3,$4,$5) ON CONFLICT "
        "(world_id,character_id,component,membership_generation,conversion_rate) "
        "DO UPDATE SET remainder=EXCLUDED.remainder",
        pqxx::params{event.world_id, event.character_id,
                     event.effects.expected_company_generation,
                     event.effects.activity_company_rate, conversion.remainder});
  }
  const auto saved = transaction.exec(
      "UPDATE emulator_character_company SET contribution=$2,join_epoch_ticks=$3,"
      "activity_epoch_ticks=$4,permit_expiry_epoch_ticks=$5,unlocked_position_level=$6 "
      "WHERE character_id=$1 AND company_type=$7 RETURNING character_id",
      pqxx::params{event.character_id, after.contribution, after.join_epoch_ticks,
                   after.activity_epoch_ticks, after.permit_expiry_epoch_ticks,
                   after.unlocked_position_level, after.company_type});
  if (saved.empty()) {
    return {QuestEffectSettlementStatus::identity_mismatch, current.record};
  }
  const auto marked = transaction.exec(
      "UPDATE emulator_quest_service_effects SET company_applied=true "
      "WHERE character_id=$1 AND effect_id=$2 AND NOT company_applied RETURNING character_id",
      pqxx::params{event.character_id, event.effect_id});
  if (marked.empty()) {
    throw std::runtime_error("Company quest settlement marker changed under its guard");
  }
  transaction.commit();
  return {QuestEffectSettlementStatus::applied, after, current.guild_applied};
}

void CompanyService::adopt_quest_owner_epoch(std::int64_t epoch) {
  std::lock_guard lock(mutex_);
  if (uncertain_quest_effect_) {
    throw std::runtime_error("Cannot replace a company owner with an uncertain settlement");
  }
  store_->adopt_quest_owner_epoch(epoch);
}

void CompanyService::ensure_quest_cache_available_locked(std::int64_t character_id) const {
  if (blocked_quest_characters_.contains(character_id) ||
      (uncertain_quest_effect_ && uncertain_quest_effect_->character_id == character_id)) {
    throw std::runtime_error("Company state awaits quest settlement reconciliation");
  }
}

std::optional<CompanyQuestSettlementResult> CompanyService::reconcile_uncertain_quest_effect() {
  std::lock_guard lock(mutex_);
  if (!uncertain_quest_effect_) {
    return std::nullopt;
  }
  const auto event = *uncertain_quest_effect_;
  auto result = store_->read_quest_effect(event);
  if (result.status != QuestEffectSettlementStatus::pending &&
      result.status != QuestEffectSettlementStatus::already_applied) {
    return result;
  }
  const auto found = records_.find(event.character_id);
  if (found == records_.end() || result.record.char_id != event.character_id) {
    throw std::runtime_error("Reconciled company cache entry is unavailable");
  }
  found->second = result.record;
  result.effect_id = event.effect_id;
  blocked_quest_characters_.erase(event.character_id);
  uncertain_quest_effect_.reset();
  return result;
}

CompanyQuestSettlementResult CompanyService::settle_quest_effect(
    const QuestServiceEffectEvent &event) {
  std::lock_guard lock(mutex_);
  if (uncertain_quest_effect_) {
    return {QuestEffectSettlementStatus::retry, {}};
  }
  if (event.world_id != world_id_) {
    return {QuestEffectSettlementStatus::identity_mismatch, {}};
  }
  if (blocked_quest_characters_.contains(event.character_id)) {
    // A definitive pre-write mismatch only quarantines its own character. Retrying first
    // performs a fenced read so an operator repair can restore its authoritative cache.
    const auto current = store_->read_quest_effect(event);
    if (current.status != QuestEffectSettlementStatus::pending &&
        current.status != QuestEffectSettlementStatus::already_applied) {
      return current;
    }
    if (current.record.char_id != event.character_id) {
      throw std::runtime_error("Company repair returned the wrong character");
    }
    records_.insert_or_assign(event.character_id, current.record);
    blocked_quest_characters_.erase(event.character_id);
    if (current.status == QuestEffectSettlementStatus::already_applied) {
      return current;
    }
  }
  const auto found = records_.find(event.character_id);
  if (found == records_.end() ||
      found->second.company_type != event.effects.expected_company_type) {
    blocked_quest_characters_.insert(event.character_id);
    spdlog::error("Company quest state mismatch for character {}; this character is blocked "
                  "until authoritative state is repaired", event.character_id);
    return {QuestEffectSettlementStatus::identity_mismatch, {}};
  }

  const auto before = found->second;
  const CompanyQuestPlanner planner = [&](std::int64_t activity_contribution) {
    return plan_quest_company_effect(before, event.effects, activity_contribution);
  };
  // Retain the event before an ambiguous write can start. The affected character's cache
  // remains unavailable until a guarded readback proves the transaction's actual outcome.
  // Reserve actor quarantine before any potentially ambiguous database write.
  blocked_quest_characters_.insert(event.character_id);
  uncertain_quest_effect_ = event;
  auto result = store_->settle_quest_effect(event, before, planner);
  if (result.status == QuestEffectSettlementStatus::identity_mismatch) {
    // A returned identity mismatch is a definitive no-write result from the store.
    uncertain_quest_effect_.reset();
    spdlog::error("Company quest identity mismatch for character {}; this character is blocked "
                  "until authoritative state is repaired", event.character_id);
    return result;
  }
  if (result.status == QuestEffectSettlementStatus::applied ||
      result.status == QuestEffectSettlementStatus::already_applied ||
      result.status == QuestEffectSettlementStatus::retry) {
    if (result.record.char_id != event.character_id) {
      throw std::runtime_error("Company settlement returned the wrong character");
    }
    found->second = result.record;
  }
  blocked_quest_characters_.erase(event.character_id);
  uncertain_quest_effect_.reset();
  return result;
}

} // namespace chronicles
