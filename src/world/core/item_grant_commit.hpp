#pragma once

#include "world/core/progress_access.hpp"
#include "world/core/session_event_bus.hpp"

#include <functional>
#include <memory>

namespace chronicles {

// The two grant-only native consumers use this exact post-commit cache sequence.
// Queries resolve current containing-session state; they must not retain combat pointers.
struct ItemGrantCacheAccess {
  std::function<bool()> has_tutorial;
  std::function<const nlohmann::json *()> player_stats;
};

using ItemGrantCommit = std::function<std::optional<std::vector<CharacterItem>>(
    const WorldProgress &, const WorldProgress &, const std::vector<CharacterItem> &)>;

// The captured ProgressAccess owns the original immutable handoff subject. The containing
// player session owns the bus and the state borrowed by cache queries, outliving the consumer.
ItemGrantCommit make_item_grant_commit(std::shared_ptr<ProgressAccess> progress,
                                       ItemGrantCacheAccess cache, SessionEventBus &events);

} // namespace chronicles
