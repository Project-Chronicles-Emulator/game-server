#include "world/features/fog/fog_reveal.hpp"

#include "auth/auth.hpp"
#include "data/client_catalog.hpp"

#include <stdexcept>

namespace chronicles {

std::optional<std::size_t> reveal_world_map(FogRepository &fog, const Handoff &handoff,
                                            ClientCatalog &catalog) {
  if (!handoff.game_master) {
    return std::nullopt;
  }

  const auto areas = catalog.fog_area_ids();
  if (areas.size() > kMaximumFogAreas) {
    throw std::runtime_error("FogArea catalog exceeds supported exploration bounds");
  }
  if (!fog.discover_fog_areas(handoff.account_id, handoff.character_id, handoff.world_id,
                              areas)) {
    return std::nullopt;
  }
  return areas.size();
}

} // namespace chronicles
