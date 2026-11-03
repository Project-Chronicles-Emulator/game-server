#pragma once

#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"

#include <optional>
#include <string>

namespace chronicles {

struct GlidingConditions {
  bool dead{};
  bool riding{};
  bool using_skill{};
  bool interacting{};
  // Server-modelled crowd control only (the stun ailment status); see player_stunned().
  bool crowd_controlled{};
};

// The client predicts gliding and only has a failure notification (180). Accepted starts
// change session state without inventing a success packet; ordinary MoveReq replicates flight.
class GlidingSession {
public:
  void observe_movement(const nlohmann::json &movement);
  void reset();
  bool active() const noexcept;

  std::optional<GameplayPacket> start(const nlohmann::json &request,
                                      const std::optional<WorldPosition> &position,
                                      GlidingConditions conditions, std::int64_t now);

private:
  std::string movement_mode_{"MoveMode_Normal"};
  bool active_{};
};

} // namespace chronicles
