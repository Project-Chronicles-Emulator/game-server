#pragma once

#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"

#include <cstdint>
#include <map>
#include <optional>

namespace chronicles::trade {

// Non-tutorial voucher state belongs to Trade. Generic handshake sequencing chooses when
// to encode the optional packet; this cache retains the existing unredeemed-only filter.
class TradeBootstrap final {
public:
  void configure(std::optional<WorldProgress> &non_tutorial_progress);
  std::optional<GameplayPacket> snapshot() const;

private:
  std::map<std::int64_t, TradeVoucherRecord> vouchers_;
};

} // namespace chronicles::trade
