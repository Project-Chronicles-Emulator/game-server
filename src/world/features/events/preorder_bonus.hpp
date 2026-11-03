#pragma once

#include "protocol/gameplay_packet.hpp"
#include "world/core/world_gameplay_request.hpp"

namespace chronicles::preorder_bonus {

inline constexpr std::uint16_t kStateRequest = 20125;
inline constexpr std::uint16_t kClaimRequest = 20126;
inline constexpr std::uint16_t kStateResponse = 232;
inline constexpr std::uint16_t kClaimResponse = 233;

// This realm has no storefront entitlement issuer. An authenticated account is therefore
// NotEligible; never mint paid rewards from a client query or claim. Official captures contain
// 69 successful state replies with the default NotEligible state and an empty packet_seq.
GameplayPacket respond_without_entitlement(const WorldGameplayRequest &request, bool authenticated);

} // namespace chronicles::preorder_bonus
