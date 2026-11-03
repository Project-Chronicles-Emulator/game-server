#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

namespace chronicles {

// Decoded world request transferred from the handshake to gameplay routing. The handshake
// validates the wire sequence before creating this value; handlers retain that sequence.
struct WorldGameplayRequest {
  std::uint16_t opcode{};
  std::uint32_t sequence{};
  nlohmann::json message;
};

} // namespace chronicles
