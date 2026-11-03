#include "persistence/postgres/character_rows.hpp"
#include "world/features/company/company.hpp"
#include "persistence/memory_quest_service_coordinator.hpp"
#include "persistence/quest_service_effect_guards.hpp"

#include <pqxx/pqxx>

#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

void validate_stored_record(const CompanyRecord &record) {
  if (record.char_id <= 0 || record.company_type < kCompanyNone ||
      record.company_type > kMaximumCompanyType || record.contribution < 0 ||
      record.unlocked_position_level < 0) {
    throw std::invalid_argument("Company record is outside the persisted value ranges");
  }
}

void validate_permit_transition(const CompanyRecord &before, const CompanyRecord &after) {
  validate_stored_record(before);
  validate_stored_record(after);
  auto expected = before;
  expected.permit_expiry_epoch_ticks = after.permit_expiry_epoch_ticks;
  if (expected != after || after.permit_expiry_epoch_ticks <= before.permit_expiry_epoch_ticks) {
    throw std::invalid_argument("Invalid company permit transition");
  }
}

} // namespace

MemoryCompanyStore::MemoryCompanyStore(
    std::shared_ptr<MemoryCharacterStateRepository> progress_store,
    std::shared_ptr<MemoryQuestServiceCoordinator> quest_services)
    : progress_store_(std::move(progress_store)), quest_services_(std::move(quest_services)) {}

std::vector<CompanyRecord> MemoryCompanyStore::load(int) {
  std::lock_guard lock(mutex_);
  std::vector<CompanyRecord> records;
  records.reserve(records_.size());
  for (const auto &[char_id, record] : records_) {
    records.push_back(record);
  }
  return records;
}

void MemoryCompanyStore::save(const CompanyRecord &record) {
  validate_stored_record(record);
  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    if (quest_owner_epoch_.load() <= 0 ||
        quest_services_->owner_epoch != quest_owner_epoch_.load()) {
      throw std::runtime_error("Company store owner epoch is stale or missing");
    }
    if (quest_services_->company_effect_pending(record.char_id)) {
      throw std::runtime_error("Company update waits for a pending quest reward");
    }
  }
  std::lock_guard lock(mutex_);
  auto memberships = quest_services_ ? quest_services_->company_memberships
                                     : std::map<std::int64_t, int>{};
  auto generations = quest_services_ ? quest_services_->company_generations
                                     : std::map<std::int64_t, std::int64_t>{};
  if (quest_services_) {
    const auto previous = memberships.find(record.char_id);
    const int previous_type = previous == memberships.end() ? 0 : previous->second;
    if (previous_type != record.company_type) {
      advance_quest_membership_generation(generations, record.char_id);
    }
    memberships[record.char_id] = record.company_type;
  }
  records_[record.char_id] = record;
  if (quest_services_) {
    quest_services_->company_memberships.swap(memberships);
    quest_services_->company_generations.swap(generations);
  }
}

bool MemoryCompanyStore::commit_permit(std::int64_t account_id, int world_id,
                                       const CompanyRecord &before_company,
                                       const CompanyRecord &after_company,
                                       const WorldProgress &before_progress,
                                       const WorldProgress &after_progress) {
  validate_permit_transition(before_company, after_company);
  if (!progress_store_) {
    return false;
  }

  std::unique_lock<std::mutex> coordinator_lock;
  if (quest_services_) {
    coordinator_lock = std::unique_lock(quest_services_->mutex);
    if (quest_owner_epoch_.load() <= 0 ||
        quest_services_->owner_epoch != quest_owner_epoch_.load()) {
      throw std::runtime_error("Company store owner epoch is stale or missing");
    }
    if (quest_services_->company_effect_pending(before_company.char_id)) {
      throw std::runtime_error("Company permit waits for a pending quest reward");
    }
  }
  std::lock_guard lock(mutex_);
  const auto found = records_.find(before_company.char_id);
  if (found == records_.end() || found->second != before_company) {
    return false;
  }
  return progress_store_->commit_company_permit(account_id, world_id, found->second, after_company,
                                                before_progress, after_progress);
}

PostgresCompanyStore::PostgresCompanyStore(std::string connection)
    : connection_(std::move(connection)) {}

std::vector<CompanyRecord> PostgresCompanyStore::load(int world_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(
      "SELECT r.character_id,r.company_type,r.contribution,r.join_epoch_ticks,"
      "r.activity_epoch_ticks,r.permit_expiry_epoch_ticks,r.unlocked_position_level "
      "FROM emulator_character_company r "
      "JOIN emulator_characters c ON c.id=r.character_id WHERE c.world_id=$1",
      pqxx::params{world_id});

  std::vector<CompanyRecord> records;
  records.reserve(rows.size());
  for (const auto &row : rows) {
    CompanyRecord record;
    record.char_id = row[0].as<std::int64_t>();
    record.company_type = row[1].as<int>();
    record.contribution = row[2].as<std::int64_t>();
    record.join_epoch_ticks = row[3].as<std::int64_t>();
    record.activity_epoch_ticks = row[4].as<std::int64_t>();
    record.permit_expiry_epoch_ticks = row[5].as<std::int64_t>();
    record.unlocked_position_level = row[6].as<int>();
    validate_stored_record(record);
    records.push_back(record);
  }
  return records;
}

void PostgresCompanyStore::save(const CompanyRecord &record) {
  validate_stored_record(record);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, quest_owner_epoch_.load());
  detail::lock_quest_service_character(transaction, record.char_id);
  detail::require_quest_company_write_allowed(transaction, record.char_id);
  const auto current = transaction.exec(
      "SELECT company_type FROM emulator_character_company WHERE character_id=$1 FOR UPDATE",
      pqxx::params{record.char_id});
  const int previous_type = current.empty() ? 0 : current.front()[0].as<int>();
  if (previous_type != record.company_type) {
    detail::advance_quest_company_generation(transaction, record.char_id);
  }
  const auto saved = transaction.exec(
      "INSERT INTO emulator_character_company(character_id,company_type,contribution,"
      "join_epoch_ticks,activity_epoch_ticks,permit_expiry_epoch_ticks,unlocked_position_level) "
      "VALUES ($1,$2,$3,$4,$5,$6,$7) ON CONFLICT (character_id) DO UPDATE SET "
      "company_type=EXCLUDED.company_type,contribution=EXCLUDED.contribution,"
      "join_epoch_ticks=EXCLUDED.join_epoch_ticks,"
      "activity_epoch_ticks=EXCLUDED.activity_epoch_ticks,"
      "permit_expiry_epoch_ticks=EXCLUDED.permit_expiry_epoch_ticks,"
      "unlocked_position_level=EXCLUDED.unlocked_position_level RETURNING character_id",
      pqxx::params{record.char_id, record.company_type, record.contribution,
                   record.join_epoch_ticks, record.activity_epoch_ticks,
                   record.permit_expiry_epoch_ticks, record.unlocked_position_level});
  if (saved.empty()) {
    throw std::runtime_error("Company record was not stored");
  }
  transaction.commit();
}

bool PostgresCompanyStore::commit_permit(std::int64_t account_id, int world_id,
                                         const CompanyRecord &before_company,
                                         const CompanyRecord &after_company,
                                         const WorldProgress &before_progress,
                                         const WorldProgress &after_progress) {
  validate_permit_transition(before_company, after_company);
  detail::validate_world_progress_transition(before_progress, after_progress);

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  detail::require_reward_owner_epoch(transaction, quest_owner_epoch_.load());
  detail::lock_quest_service_character(transaction, before_company.char_id);
  detail::require_quest_company_write_allowed(transaction, before_company.char_id);
  const auto owner = transaction.exec(
      "SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3 "
      "FOR UPDATE",
      pqxx::params{account_id, before_company.char_id, world_id});
  if (owner.empty()) {
    return false;
  }

  const auto rows = transaction.exec(
      "SELECT company_type,contribution,join_epoch_ticks,activity_epoch_ticks,"
      "permit_expiry_epoch_ticks,unlocked_position_level FROM emulator_character_company "
      "WHERE character_id=$1 FOR UPDATE",
      pqxx::params{before_company.char_id});
  if (rows.empty()) {
    return false;
  }

  const auto &row = rows.front();
  if (row[0].as<int>() != before_company.company_type ||
      row[1].as<std::int64_t>() != before_company.contribution ||
      row[2].as<std::int64_t>() != before_company.join_epoch_ticks ||
      row[3].as<std::int64_t>() != before_company.activity_epoch_ticks ||
      row[4].as<std::int64_t>() != before_company.permit_expiry_epoch_ticks ||
      row[5].as<int>() != before_company.unlocked_position_level) {
    return false;
  }

  if (!detail::write_world_progress_transition(transaction, account_id, before_company.char_id,
                                               world_id, before_progress, after_progress)) {
    return false;
  }
  transaction.exec("UPDATE emulator_character_company SET permit_expiry_epoch_ticks=$2 "
                   "WHERE character_id=$1",
                   pqxx::params{before_company.char_id, after_company.permit_expiry_epoch_ticks});
  transaction.commit();
  return true;
}

} // namespace chronicles
