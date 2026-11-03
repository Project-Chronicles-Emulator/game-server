#pragma once

namespace chronicles {

// x5.cms.EX5MarketListingState, also persisted in emulator_market.state.
inline constexpr int kMarketOnSale = 0;
inline constexpr int kMarketSold = 1;
inline constexpr int kMarketRevoked = 2;
inline constexpr int kMarketExpired = 3;

} // namespace chronicles
