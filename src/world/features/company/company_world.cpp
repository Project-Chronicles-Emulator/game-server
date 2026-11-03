#include "world/features/company/company_world.hpp"

#include "auth/auth.hpp"
#include "common/currency.hpp"
#include "p_system.pb.h"
#include "world/core/progress_packets.hpp"

#include <limits>

namespace chronicles {

GameplayPacket company_packet(std::uint16_t opcode, const CompanyRecord &record) {
  // JoinCompanyRes, CompanyUpdateNoti and RegisterPioneerPermitRes share field 1 `company`.
  return {opcode, {{"company", message_json(world_char_company(record))}}};
}

GameplayPacket company_error_packet(std::uint32_t request_sequence, x5::ec::EErrorCode code) {
  x5::pb::CommonErrorRes response;
  response.set_ec(code);
  response.set_category(x5::pb::EErrorCategory_Company);
  response.mutable_packet_seq()->set_seq(request_sequence);
  return {company_world::kCommonErrorResponse, message_json(response)};
}

PermitCharge charge_permit(const WorldProgress &before, const PermitPlan &plan) {
  PermitCharge charge;
  charge.after = before;
  if (before.revision == std::numeric_limits<std::int64_t>::max() ||
      plan.money_value < 0 ||
      (plan.money_type != static_cast<int>(MoneyType::Gold) &&
       plan.money_type != static_cast<int>(MoneyType::HersanderToken))) {
    charge.error = x5::ec::PioneerPermit_Invalid;
    return charge;
  }

  CurrencyBalances balances;
  balances.gold = before.gold;
  balances.hersander_token = before.hersander_token;
  const auto applied =
      apply_money_change(balances, static_cast<MoneyType>(plan.money_type), -plan.money_value);
  if (!applied.ok) {
    charge.error = x5::ec::PioneerPermit_NotEnoughMoneyForRegister;
    return charge;
  }

  // Storage accepts a transition only when the revision advances by exactly one.
  charge.after.gold = balances.gold;
  charge.after.hersander_token = balances.hersander_token;
  ++charge.after.revision;
  if (plan.money_value != 0) {
    charge.items_update = progress_change_packet(before, charge.after, "company_pioneer_permit");
  }
  return charge;
}

} // namespace chronicles
