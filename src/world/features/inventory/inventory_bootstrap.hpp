#pragma once

#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"

#include <cstdint>
#include <map>
#include <optional>

namespace chronicles {

// Existing money.values projection, shared by tutorial rewards and non-tutorial entry.
// Gold (key "0") is always sent. The nonzero-only HersanderToken (key "1") rule remains
// a bounded candidate from one capture; Blue Salt is a separate field.
nlohmann::json world_money_values_json(std::int64_t gold, std::int64_t hersander_token);

namespace inventory {

// Owns the non-tutorial entry cache and item/money packet policy. The handshake retains
// sequence, codec access and the original interleaving with other entry notifications.
class InventoryBootstrap final {
public:
  void configure(std::optional<WorldProgress> &non_tutorial_progress);
  GameplayPacket items_snapshot(const Character &character) const;
  std::optional<GameplayPacket> money_snapshot() const;

private:
  std::optional<std::int64_t> gold_;
  std::int64_t hersander_token_{};
  std::map<int, int> stackables_;
};

} // namespace inventory
} // namespace chronicles
