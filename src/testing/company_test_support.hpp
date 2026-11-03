#pragma once

// Company rules for unit tests, shaped like the exported BlueSalt and Antiqus rows: cumulative
// contribution thresholds, the 4000-point change requirement and Gold pioneer permits.

#include "world/features/company/company.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace chronicles::testing_support {

inline constexpr int kAntiqus = 1;
inline constexpr int kBlueSalt = 2;

// Contribution totals at which each Antiqus position level begins (levels 1 to 6).
inline constexpr std::int64_t kAntiqusThresholds[] = {0, 1, 3001, 8001, 20001, 45001};

inline CompanyPositionRule position_rule(int level, std::int64_t next_level_contribution,
                                         bool locks_advancement = false) {
  CompanyPositionRule rule;
  rule.level = level;
  rule.next_level_contribution = next_level_contribution;
  rule.locks_advancement = locks_advancement;
  rule.change_penalty_contribution = level >= 2 ? 3000 : 0;
  rule.pioneer_permit_duration = std::chrono::minutes(4320);
  rule.pioneer_permit_money_type = 0;
  rule.pioneer_permit_money_value = level >= 2 ? 10 : 0;
  return rule;
}

inline CompanyRules company_rules() {
  CompanyRules rules;

  CompanyDefinition antiqus;
  antiqus.type = kAntiqus;
  antiqus.name = "Antiqus";
  antiqus.required_contribution_to_change = 4000;
  antiqus.positions = {position_rule(1, 1),     position_rule(2, 3001),
                       position_rule(3, 8001),  position_rule(4, 20001),
                       position_rule(5, 45001), position_rule(6, 90001)};
  rules.companies.emplace(kAntiqus, antiqus);

  // BlueSalt caps levels 2 and 3 until a CompanyLevelUnlock reward lifts them.
  CompanyDefinition blue_salt;
  blue_salt.type = kBlueSalt;
  blue_salt.name = "BlueSalt";
  blue_salt.required_contribution_to_change = 4000;
  blue_salt.positions = {position_rule(1, 1), position_rule(2, 1501, true),
                         position_rule(3, 11501, true), position_rule(4, 31501)};
  rules.companies.emplace(kBlueSalt, blue_salt);
  return rules;
}

// Places a character in Antiqus at a position level, bypassing the dialogue path.
inline void seat_in_company(CompanyService &companies, std::int64_t char_id, int position_level,
                            int company_type = kAntiqus) {
  const auto threshold = companies.contribution_for_position(company_type, position_level);
  if (!threshold) {
    throw std::invalid_argument("Unknown test company position");
  }

  CompanyRecord record;
  record.char_id = char_id;
  record.company_type = company_type;
  record.contribution = *threshold;
  record.unlocked_position_level = position_level;
  if (companies.set_record(record).error != x5::ec::Success) {
    throw std::runtime_error("Cannot store test company record");
  }
}

} // namespace chronicles::testing_support
