#pragma once

// GM-only map reveal shared by the world-session GM command (/gm reveal_map). Kept out of
// AuthService because the auth layer may not include feature headers; the session checks the
// handoff's GM flag and the repository performs the merge.

#include "world/features/fog/fog_repository.hpp"

#include <cstddef>
#include <optional>

namespace chronicles {

class ClientCatalog;
struct Handoff;

// GM-only, issuing character only. Merges every verified catalog fog area, without changing
// gameplay revisions. The client reloads the saved set after character selection.
std::optional<std::size_t> reveal_world_map(FogRepository &fog, const Handoff &handoff,
                                            ClientCatalog &catalog);

} // namespace chronicles
