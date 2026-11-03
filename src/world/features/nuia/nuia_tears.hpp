#pragma once

#include "protocol/gameplay_packet.hpp"
#include "persistence/character_state_types.hpp"

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>

namespace chronicles {

struct NuiaTearResult {
  std::vector<GameplayPacket> packets;
  // The caller must commit this revision before publishing any success packets.
  std::optional<WorldProgress> next;
};

// Immutable, build-specific placement index. Collected CMS IDs remain character-owned
// across maps; a shared placement is never globally depleted for other characters.
class NuiaTears {
public:
  static NuiaTears load(const std::filesystem::path &path);
  explicit NuiaTears(const nlohmann::json &document);

  static bool handles(std::uint16_t opcode);
  bool handles(std::uint16_t opcode, const nlohmann::json &request,
               const WorldPosition &position) const;
  static GameplayPacket snapshot(const WorldProgress &progress);
  NuiaTearResult receive(std::uint16_t opcode, const nlohmann::json &request,
                         std::uint32_t sequence, const WorldPosition &position,
                         const WorldProgress &current) const;

private:
  struct Placement {
    int cms_id;
    nlohmann::json position;
    int quantity;
  };

  std::map<std::pair<int, std::uint64_t>, Placement> placements_;
};

} // namespace chronicles
