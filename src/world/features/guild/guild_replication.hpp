#pragma once

#include "world/features/guild/guild_feature.hpp"

#include <cstdint>
#include <vector>

namespace chronicles::guild {

// Called synchronously while composition holds its original codec/registry lock. Each
// queue operation preserves its own encoding catch and partial-result behavior. The context
// retains strong recipients until composition sends the queued frames after unlocking.
class GuildReplicationContext {
public:
  virtual ~GuildReplicationContext() = default;

  virtual void queue_packet(std::uint64_t connection_id, std::uint16_t opcode,
                            const nlohmann::json &message) = 0;
  virtual std::vector<std::uint64_t> observers(std::uint64_t subject_connection_id) = 0;
  virtual void queue_shared_observers(std::uint16_t opcode, const nlohmann::json &message,
                                      const std::vector<std::uint64_t> &observers) = 0;
};

} // namespace chronicles::guild
