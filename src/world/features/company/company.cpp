#include "world/features/company/company.hpp"

#include "data/client_catalog.hpp"
#include "cms.generated.pb.h"
#include "common/currency.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

using x5::ec::EErrorCode;

constexpr char kCompanyConfiguration[] = "Company configuration";

constexpr std::int64_t kTicksPerMinute = 60LL * 10'000'000LL;

// Catalog plausibility bounds.
constexpr std::int64_t kMaximumCooldownMinutes = 60LL * 24 * 365;
constexpr std::int64_t kMaximumContribution = std::numeric_limits<std::int32_t>::max();
constexpr int kMaximumPositionLevel = 100;

std::optional<int> parse_company_type(const std::string &name) {
  x5::cms::EX5CompanyType type{};
  if (name.empty() || !x5::cms::EX5CompanyType_Parse("EX5CompanyType_" + name, &type) ||
      type == x5::cms::EX5CompanyType_None) {
    return std::nullopt;
  }
  return static_cast<int>(type);
}

std::optional<int> parse_money_type(const std::string &name) {
  x5::cms::EX5MoneyType type{};
  if (!x5::cms::EX5MoneyType_Parse("EX5MoneyType_" + name, &type)) {
    return std::nullopt;
  }
  return static_cast<int>(type);
}

std::optional<CompanyPositionRule> parse_position(const nlohmann::json &row) {
  const auto level = catalog_integer(row, "level");
  const auto next = catalog_integer(row, "nextLevelContribution");
  const auto penalty = catalog_integer(row, "changePenaltyContribution");
  const auto permit_minutes = catalog_integer(row, "pioneerPermitLifeValueMin");
  const auto permit_cost = catalog_integer(row, "pioneerPermitMoneyValue");
  const auto money_type = parse_money_type(catalog_text_field(row, "pioneerPermitMoneyType"));
  const bool level_valid = level && *level >= 1 && *level <= kMaximumPositionLevel;
  if (!level_valid || !next || *next < 0 || !penalty || *penalty < 0 || !permit_minutes ||
      *permit_minutes < 0 || !permit_cost || *permit_cost < 0 || !money_type) {
    return std::nullopt;
  }

  CompanyPositionRule rule;
  rule.level = static_cast<int>(*level);
  rule.next_level_contribution = *next;
  rule.locks_advancement = catalog_flag(row, "isMaxPositionLevel");
  rule.safe_contribution = catalog_flag(row, "safeContribution");
  rule.change_penalty_contribution = *penalty;
  rule.pioneer_permit_duration = std::chrono::minutes(*permit_minutes);
  rule.pioneer_permit_money_type = *money_type;
  rule.pioneer_permit_money_value = *permit_cost;
  return rule;
}

// Positions must be contiguous from level 1; a gap leaves the company without positions.
bool contiguous_positions(std::vector<CompanyPositionRule> &positions) {
  std::ranges::sort(positions, {}, &CompanyPositionRule::level);
  for (std::size_t index = 0; index < positions.size(); ++index) {
    if (positions[index].level != static_cast<int>(index) + 1) {
      return false;
    }
  }
  return !positions.empty();
}

std::int64_t saturating_add(std::int64_t value, std::int64_t amount) {
  if (amount > 0 && value > kMaximumContribution - amount) {
    return kMaximumContribution;
  }
  return std::max<std::int64_t>(0, value + amount);
}

} // namespace

CompanyRules load_company_rules(ClientCatalog &catalog) {
  CompanyRules rules;
  if (catalog.has_table("Company")) {
    for (const auto &row : catalog.rows("Company")) {
      const auto type = parse_company_type(catalog_text_field(row, "id"));
      const auto required = catalog_integer(row, "requireContributionToChangeCompany");
      if (!type || !required || *required < 0) {
        spdlog::warn("{}: skipped an invalid Company row", kCompanyConfiguration);
        continue;
      }

      CompanyDefinition definition;
      definition.type = *type;
      definition.name = catalog_text_field(row, "id");
      definition.required_contribution_to_change = *required;
      rules.companies.emplace(*type, std::move(definition));
    }
  }

  std::map<int, std::vector<CompanyPositionRule>> positions;
  if (catalog.has_table("CompanyPosition")) {
    for (const auto &row : catalog.rows("CompanyPosition")) {
      const auto type = parse_company_type(catalog_text_field(row, "company"));
      const auto position = parse_position(row);
      if (!type || !position) {
        spdlog::warn("{}: skipped an invalid CompanyPosition row", kCompanyConfiguration);
        continue;
      }
      positions[*type].push_back(*position);
    }
  }

  for (auto &[type, definition] : rules.companies) {
    auto &company_positions = positions[type];
    if (!contiguous_positions(company_positions)) {
      spdlog::warn("{}: company {} has no contiguous positions and is not joinable",
                   kCompanyConfiguration, definition.name);
      continue;
    }
    definition.positions = std::move(company_positions);
  }

  if (catalog.has_table("GameVariable")) {
    const auto cooldown = game_variable_integer(catalog, "CompanyChangeLimitMin", 0,
                                                kMaximumCooldownMinutes, kCompanyConfiguration);
    if (cooldown) {
      rules.change_cooldown = std::chrono::minutes(*cooldown);
    }

    const auto renewal =
        game_variable_integer(catalog, "RenewPioneerPermitDurationMinuteBeforeExpiry", 0,
                              kMaximumCooldownMinutes, kCompanyConfiguration);
    if (renewal) {
      rules.permit_renewal_window = std::chrono::minutes(*renewal);
    }
  }

  return rules;
}

CompanyService::CompanyService(std::shared_ptr<CompanyStore> store, CompanyRules rules,
                               int world_id, std::function<std::int64_t()> clock)
    : store_(std::move(store)), rules_(std::move(rules)), world_id_(world_id),
      clock_(std::move(clock)) {
  if (!store_ || !clock_) {
    throw std::invalid_argument("Company service requires a store and a clock");
  }
  if (rules_.character_experience_per_contribution <= 0) {
    throw std::invalid_argument("Company experience conversion must be positive");
  }

  for (auto &record : store_->load(world_id_)) {
    records_[record.char_id] = record;
  }
}

const CompanyRules &CompanyService::rules() const {
  return rules_;
}

CompanyStatus CompanyService::status(std::int64_t char_id) const {
  std::lock_guard lock(mutex_);
  return status_locked(record_locked(char_id));
}

int CompanyService::position_level(int company_type, std::int64_t contribution) const {
  const auto *company = definition(company_type);
  if (!company || company->positions.empty()) {
    return 0;
  }

  // Thresholds are cumulative contribution totals (Antiqus: 1, 3001, 8001, ...). The client
  // returns -1 past the last threshold; the emulator reports the last level instead.
  for (const auto &rule : company->positions) {
    if (contribution < rule.next_level_contribution) {
      return rule.level;
    }
  }
  return company->positions.back().level;
}

std::int64_t CompanyService::contribution_cap(const CompanyRecord &record) const {
  const auto *company = definition(record.company_type);
  if (!company) {
    return kMaximumContribution;
  }

  for (const auto &rule : company->positions) {
    if (rule.locks_advancement && record.unlocked_position_level < rule.level) {
      return std::max<std::int64_t>(0, rule.next_level_contribution - 1);
    }
  }
  return kMaximumContribution;
}

std::optional<std::int64_t> CompanyService::contribution_for_position(int company_type,
                                                                     int level) const {
  const auto *company = definition(company_type);
  if (!company || level < 1 || level > static_cast<int>(company->positions.size())) {
    return std::nullopt;
  }
  if (level == 1) {
    return 0;
  }
  return company->positions[static_cast<std::size_t>(level) - 2].next_level_contribution;
}

CompanyResult CompanyService::join(std::int64_t char_id, int company_type) {
  std::lock_guard lock(mutex_);
  const auto now_ticks = clock_();
  const auto *company = definition(company_type);
  if (char_id <= 0 || !company || company->positions.empty()) {
    return {x5::ec::Company_InvalidCmsId, {}};
  }

  auto record = record_locked(char_id);
  if (record.company_type != kCompanyNone) {
    return {x5::ec::Company_IsNotJoinable_AlreadyJoined, status_locked(record)};
  }

  record.company_type = company_type;
  record.contribution = 0;
  record.join_epoch_ticks = now_ticks;
  record.activity_epoch_ticks = now_ticks;
  record.permit_expiry_epoch_ticks = 0;
  record.unlocked_position_level = 0;
  return store_locked(record);
}

CompanyResult CompanyService::change(std::int64_t char_id, int company_type) {
  std::lock_guard lock(mutex_);
  const auto now_ticks = clock_();
  const auto *target = definition(company_type);
  if (char_id <= 0 || !target || target->positions.empty()) {
    return {x5::ec::Company_InvalidCmsId, {}};
  }

  auto record = record_locked(char_id);
  const auto current = status_locked(record);
  if (record.company_type == kCompanyNone) {
    return {x5::ec::Company_IsNotJoined, current};
  }
  if (record.company_type == company_type) {
    return {x5::ec::Company_IsNotJoinable_AlreadyJoined, current};
  }

  const auto cooldown_ticks = rules_.change_cooldown.count() * kTicksPerMinute;
  if (now_ticks < record.join_epoch_ticks + cooldown_ticks) {
    return {x5::ec::Company_IsNotJoinable_ChangeLimit, current};
  }
  if (record.contribution < target->required_contribution_to_change) {
    return {x5::ec::Company_InsufficientContribution, current};
  }

  const auto &current_rule =
      definition(record.company_type)->positions.at(current.position_level - 1);
  record.contribution = saturating_add(record.contribution,
                                       -current_rule.change_penalty_contribution);
  record.company_type = company_type;
  record.join_epoch_ticks = now_ticks;
  record.activity_epoch_ticks = now_ticks;
  record.permit_expiry_epoch_ticks = 0;
  record.unlocked_position_level = 0;
  return store_locked(record);
}

std::optional<ContributionGain> CompanyService::add_contribution(std::int64_t char_id,
                                                                 std::int64_t amount) {
  std::lock_guard lock(mutex_);
  return contribute_locked(char_id, amount);
}

std::optional<ContributionGain>
CompanyService::record_character_experience(std::int64_t char_id, std::int64_t experience) {
  std::lock_guard lock(mutex_);
  if (experience <= 0 || record_locked(char_id).company_type == kCompanyNone) {
    return std::nullopt;
  }

  auto &remainder = experience_remainders_[char_id];
  const auto per_point = rules_.character_experience_per_contribution;
  const auto total = std::min(experience, std::numeric_limits<std::int64_t>::max() - remainder) +
                     remainder;
  remainder = total % per_point;
  const auto points = total / per_point;
  if (points == 0) {
    return std::nullopt;
  }
  return contribute_locked(char_id, points);
}

CompanyResult CompanyService::unlock_position(std::int64_t char_id, int company_type,
                                              int level) {
  std::lock_guard lock(mutex_);
  auto record = record_locked(char_id);
  if (record.company_type == kCompanyNone) {
    return {x5::ec::Company_IsNotJoined, status_locked(record)};
  }
  if (record.company_type != company_type || level <= 0 || level > kMaximumPositionLevel) {
    return {x5::ec::Company_NotMatched, status_locked(record)};
  }
  if (record.unlocked_position_level >= level) {
    return {x5::ec::Success, status_locked(record)};
  }

  record.unlocked_position_level = level;
  return store_locked(record);
}

PermitPlan CompanyService::plan_permit(std::int64_t char_id, bool extend) const {
  std::lock_guard lock(mutex_);
  const auto now_ticks = clock_();
  const auto record = record_locked(char_id);
  PermitPlan plan;
  plan.expected_company = record;
  if (record.company_type == kCompanyNone) {
    plan.error = x5::ec::Company_IsNotJoined;
    return plan;
  }

  const auto status = status_locked(record);
  const auto &rule = definition(record.company_type)->positions.at(status.position_level - 1);
  const auto duration_ticks = rule.pioneer_permit_duration.count() * kTicksPerMinute;
  if (extend) {
    if (record.permit_expiry_epoch_ticks <= 0) {
      plan.error = x5::ec::PioneerPermit_NotExists;
      return plan;
    }
    if (!status.permit_valid) {
      plan.error = x5::ec::PioneerPermit_Expired;
      return plan;
    }

    const auto remaining = record.permit_expiry_epoch_ticks - now_ticks;
    if (remaining >= rules_.permit_renewal_window.count() * kTicksPerMinute) {
      plan.error = x5::ec::PioneerPermit_IsNotRenewableTime;
      return plan;
    }
    plan.new_expiry_epoch_ticks = record.permit_expiry_epoch_ticks + duration_ticks;
  } else {
    if (status.permit_valid) {
      plan.error = x5::ec::PioneerPermit_Valid;
      return plan;
    }
    plan.new_expiry_epoch_ticks = now_ticks + duration_ticks;
  }

  plan.money_type = rule.pioneer_permit_money_type;
  plan.money_value = rule.pioneer_permit_money_value;
  return plan;
}

CompanyResult CompanyService::commit_permit(std::int64_t char_id,
                                            std::int64_t expiry_epoch_ticks) {
  std::lock_guard lock(mutex_);
  auto record = record_locked(char_id);
  if (record.company_type == kCompanyNone) {
    return {x5::ec::Company_IsNotJoined, status_locked(record)};
  }

  record.permit_expiry_epoch_ticks = expiry_epoch_ticks;
  return store_locked(record);
}

CompanyResult CompanyService::commit_paid_permit(std::int64_t account_id,
                                                  const PermitPlan &plan,
                                                  const WorldProgress &before,
                                                  const WorldProgress &after) {
  std::lock_guard lock(mutex_);
  ensure_quest_cache_available_locked(plan.expected_company.char_id);
  const auto found = records_.find(plan.expected_company.char_id);
  if (found == records_.end() || found->second != plan.expected_company ||
      plan.error != x5::ec::Success || plan.money_value < 0 ||
      plan.new_expiry_epoch_ticks <= plan.expected_company.permit_expiry_epoch_ticks ||
      before.revision == std::numeric_limits<std::int64_t>::max()) {
    return {x5::ec::SystemError, status_locked(record_locked(plan.expected_company.char_id))};
  }

  const auto *company = definition(found->second.company_type);
  const auto current_status = status_locked(found->second);
  if (!company || current_status.position_level <= 0 ||
      static_cast<std::size_t>(current_status.position_level) > company->positions.size()) {
    return {x5::ec::PioneerPermit_Invalid, current_status};
  }
  const auto &rule = company->positions[static_cast<std::size_t>(current_status.position_level - 1)];
  if (plan.money_type != rule.pioneer_permit_money_type ||
      plan.money_value != rule.pioneer_permit_money_value) {
    return {x5::ec::PioneerPermit_Invalid, current_status};
  }

  auto expected_progress = before;
  if (plan.money_type != static_cast<int>(MoneyType::Gold) &&
      plan.money_type != static_cast<int>(MoneyType::HersanderToken)) {
    return {x5::ec::PioneerPermit_Invalid, status_locked(found->second)};
  }
  CurrencyBalances balances;
  balances.gold = before.gold;
  balances.hersander_token = before.hersander_token;
  const auto applied = apply_money_change(balances, static_cast<MoneyType>(plan.money_type),
                                          -plan.money_value);
  if (!applied.ok) {
    return {x5::ec::PioneerPermit_NotEnoughMoneyForRegister, status_locked(found->second)};
  }
  expected_progress.gold = balances.gold;
  expected_progress.hersander_token = balances.hersander_token;
  ++expected_progress.revision;
  if (expected_progress != after) {
    return {x5::ec::SystemError, status_locked(found->second)};
  }

  auto updated = found->second;
  updated.permit_expiry_epoch_ticks = plan.new_expiry_epoch_ticks;
  try {
    if (!store_->commit_permit(account_id, world_id_, found->second, updated, before, after)) {
      return {x5::ec::SystemError, status_locked(found->second)};
    }
  } catch (const std::exception &error) {
    spdlog::error("Permit transaction for character {} failed: {}", updated.char_id, error.what());
    return {x5::ec::SystemError, status_locked(found->second)};
  }
  found->second = updated;
  return {x5::ec::Success, status_locked(updated)};
}

CompanyRecord CompanyService::plan_quest_company_effect(const CompanyRecord &before,
                                                              const QuestServiceEffects &effects,
                                                              std::int64_t activity_contribution) const {
  auto after = before;
  for (const auto &unlock : effects.company_unlocks) {
    if (unlock.company_type == after.company_type && unlock.level > 0 &&
        unlock.level <= kMaximumPositionLevel) {
      after.unlocked_position_level = std::max(after.unlocked_position_level, unlock.level);
    }
  }
  const auto contribution = saturating_add(effects.company_contribution, activity_contribution);
  if (contribution > 0 && after.company_type != kCompanyNone) {
    const auto cap = std::max(after.contribution, contribution_cap(after));
    after.contribution = std::min(saturating_add(after.contribution, contribution),
                                  cap);
    after.activity_epoch_ticks = clock_();
  }
  return after;
}

void CompanyService::validate_record(const CompanyRecord &record) const {
  if (record.char_id <= 0 ||
      (record.company_type != kCompanyNone && !definition(record.company_type)) ||
      record.contribution < 0 || record.contribution > kMaximumContribution) {
    throw std::invalid_argument("Company record is outside the accepted ranges");
  }
}

CompanyResult CompanyService::set_record(const CompanyRecord &record) {
  std::lock_guard lock(mutex_);
  ensure_quest_cache_available_locked(record.char_id);
  validate_record(record);
  return store_locked(record);
}

const CompanyDefinition *CompanyService::definition(int company_type) const {
  const auto found = rules_.companies.find(company_type);
  return found == rules_.companies.end() ? nullptr : &found->second;
}

CompanyRecord CompanyService::record_locked(std::int64_t char_id) const {
  ensure_quest_cache_available_locked(char_id);
  const auto found = records_.find(char_id);
  if (found == records_.end()) {
    CompanyRecord record;
    record.char_id = char_id;
    return record;
  }
  return found->second;
}

CompanyStatus CompanyService::status_locked(const CompanyRecord &record) const {
  CompanyStatus status;
  status.record = record;
  if (record.company_type == kCompanyNone) {
    return status;
  }

  status.position_level = position_level(record.company_type, record.contribution);
  status.permit_valid = record.permit_expiry_epoch_ticks > clock_();

  // Mirrors AAC_Company_State_IsAtCappedLevelMaxContribution (dumps/ida_research/
  // company-requirement-20261030): the current level is flagged isMaxPositionLevel and the
  // contribution sits exactly one below its nextLevelContribution.
  const auto *company = definition(record.company_type);
  if (company && status.position_level > 0 &&
      status.position_level <= static_cast<int>(company->positions.size())) {
    const auto &rule = company->positions.at(status.position_level - 1);
    status.at_capped_level_ceiling =
        rule.locks_advancement && record.contribution == rule.next_level_contribution - 1;
  }
  return status;
}

CompanyResult CompanyService::store_locked(const CompanyRecord &record) {
  try {
    store_->save(record);
  } catch (const std::exception &error) {
    spdlog::error("Company record for character {} was not stored: {}", record.char_id,
                  error.what());
    return {x5::ec::SystemError, status_locked(record_locked(record.char_id))};
  }

  records_[record.char_id] = record;
  return {x5::ec::Success, status_locked(record)};
}

std::optional<ContributionGain> CompanyService::contribute_locked(std::int64_t char_id,
                                                                  std::int64_t amount) {
  auto record = record_locked(char_id);
  if (record.company_type == kCompanyNone || amount == 0) {
    return std::nullopt;
  }

  const auto before = status_locked(record);
  // A positive gain stops at the cap; contribution already above it (set by an operator) is
  // kept, and losses always apply.
  const auto added = saturating_add(record.contribution, amount);
  if (amount > 0) {
    const auto cap = std::max(record.contribution, contribution_cap(record));
    record.contribution = std::min(added, cap);
  } else {
    record.contribution = added;
  }
  record.activity_epoch_ticks = clock_();
  const auto stored = store_locked(record);
  if (stored.error != x5::ec::Success) {
    return std::nullopt;
  }

  ContributionGain gain;
  gain.contribution = stored.status.record.contribution - before.record.contribution;
  gain.position_changed = stored.status.position_level != before.position_level;
  gain.status = stored.status;
  return gain;
}

x5::pb::WorldCharCompany world_char_company(const CompanyRecord &record) {
  x5::pb::WorldCharCompany company;
  if (record.company_type == kCompanyNone) {
    return company;
  }

  company.set_type(static_cast<x5::cms::EX5CompanyType>(record.company_type));
  company.set_contribution(record.contribution);
  company.set_activity_time_since_epoch(record.activity_epoch_ticks);
  company.set_join_time_since_epoch(record.join_epoch_ticks);
  company.set_pioneer_permit_expire_time_since_epoch(record.permit_expiry_epoch_ticks);
  return company;
}

std::string company_type_name(int company_type) {
  if (company_type == kCompanyNone || !x5::cms::EX5CompanyType_IsValid(company_type)) {
    return {};
  }
  return x5::cms::EX5CompanyType_Name(static_cast<x5::cms::EX5CompanyType>(company_type));
}

} // namespace chronicles
