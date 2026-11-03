#pragma once

#include "world/features/market/market_states.hpp"

#include <cstdint>

namespace chronicles {

// Recovered demo world-transport IDs; these are separate from trading-post packets.
inline constexpr std::uint16_t kMarketRegisterRequest = 20116;
inline constexpr std::uint16_t kMarketRevokeRequest = 20117;
inline constexpr std::uint16_t kMarketPurchaseRequest = 20118;
inline constexpr std::uint16_t kMarketQueryRequest = 20119;
inline constexpr std::uint16_t kMarketClaimRequest = 20120;
inline constexpr std::uint16_t kMarketDetailsRequest = 20121;
inline constexpr std::uint16_t kMarketHistoryRequest = 20122;
inline constexpr std::uint16_t kMarketRegisterResponse = 221;
inline constexpr std::uint16_t kMarketQueryResponse = 224;
inline constexpr std::uint16_t kMarketDetailsResponse = 226;
inline constexpr std::uint16_t kMarketHistoryResponse = 227;
inline constexpr std::uint16_t kMarketItemsUpdate = 169;

} // namespace chronicles
