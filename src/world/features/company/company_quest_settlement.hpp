#pragma once

#include "persistence/company_record.hpp"
#include "persistence/quest_effect_settlement_status.hpp"

#include <string>

namespace chronicles {

// Returned only after the store has fenced earlier attempts and read the authoritative row.
struct CompanyQuestSettlementResult {
  QuestEffectSettlementStatus status{QuestEffectSettlementStatus::retry};
  CompanyRecord record;
  bool event_completed{};
  std::string effect_id;
};

} // namespace chronicles
