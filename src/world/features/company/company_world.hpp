#pragma once

// World-transport packets and progress charges for companies.
//
// - PS2C_JoinCompanyRes (92) after a company join or change, PS2C_CompanyUpdateNoti (93) when
//   contribution or position changes, PS2C_RegisterPioneerPermitRes (94) after a permit
//   registration or extension. Each carries x5.pb.WorldCharCompany.
// - A refused company dialogue function answers PS2C_CommonErrorRes (2) with category
//   EErrorCategory_Company and the request's sequence, like other refused world requests.
// - A pioneer permit charges the position's PioneerPermitMoneyType from progress and reports
//   it as ItemsUpdateNoti (169) with the company_pioneer_permit reason.

#include "world/features/company/company.hpp"
#include "persistence/character_state_types.hpp"
#include "persistence/company_record.hpp"
#include "protocol/gameplay_packet.hpp"

#include <cstdint>
#include <optional>

namespace chronicles {

namespace company_world {

inline constexpr std::uint16_t kDialogueFunctionRequest = 20067;
inline constexpr std::uint16_t kCommonErrorResponse = 2;
inline constexpr std::uint16_t kJoinCompanyResponse = 92;
inline constexpr std::uint16_t kCompanyUpdateNotification = 93;
inline constexpr std::uint16_t kRegisterPioneerPermitResponse = 94;

} // namespace company_world

GameplayPacket company_packet(std::uint16_t opcode, const CompanyRecord &record);

GameplayPacket company_error_packet(std::uint32_t request_sequence, x5::ec::EErrorCode code);

struct PermitCharge {
  x5::ec::EErrorCode error{x5::ec::Success};
  // The next revision, with the permit cost deducted when applicable.
  WorldProgress after;
  std::optional<GameplayPacket> items_update;
};

// Deducts a planned permit cost. PioneerPermit_NotEnoughMoneyForRegister when the balance is
// short; the input progress is never modified.
PermitCharge charge_permit(const WorldProgress &before, const PermitPlan &plan);

} // namespace chronicles
