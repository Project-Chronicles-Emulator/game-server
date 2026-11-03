#pragma once

#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/core/world_gameplay_request.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace chronicles {

class FrameRouter;

// State shared by frame observers and request handlers. Encoded handshake frames and
// feature-specific delivery aggregates stay owned by the session adapter during extraction.
class FrameContext {
public:
  FrameContext(std::uint16_t incoming_opcode, std::optional<WorldGameplayRequest> request)
      : incoming_opcode_(incoming_opcode), request_(std::move(request)) {
  }

  std::uint16_t incoming_opcode() const noexcept {
    return incoming_opcode_;
  }

  // Observers can inspect the request, but only the router consumes it after a handler
  // returns true. No callback can replace its opcode and invalidate the compiled route.
  const WorldGameplayRequest *request() const noexcept {
    if (!request_) {
      return nullptr;
    }

    return &*request_;
  }

  void invalidate_positions() noexcept {
    gameplay_position.reset();
    party_position.reset();
  }

  // Valid while the router exists. On failure this identifies the throwing operation;
  // the caller's single catch retains the original exception and incoming opcode.
  std::string_view active_step() const noexcept {
    return active_step_;
  }

  std::optional<WorldPosition> gameplay_position;
  std::optional<WorldPosition> party_position;
  // Internal monotonic deadline in 100 ns ticks, independent of wire timestamp units.
  std::optional<std::int64_t> action_deadline;
  std::vector<GameplayPacket> session_packets;

private:
  friend class FrameRouter;

  const std::uint16_t incoming_opcode_;
  std::optional<WorldGameplayRequest> request_;
  std::string_view active_step_;
};

} // namespace chronicles
