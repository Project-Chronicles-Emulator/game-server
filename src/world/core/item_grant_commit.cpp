#include "world/core/item_grant_commit.hpp"

#include <stdexcept>
#include <utility>

namespace chronicles {

ItemGrantCommit make_item_grant_commit(std::shared_ptr<ProgressAccess> progress,
                                       ItemGrantCacheAccess cache, SessionEventBus &events) {
  if (!progress || !cache.has_tutorial || !cache.player_stats) {
    throw std::invalid_argument("Item grant commit requires progress and current cache queries");
  }

  return [progress = std::move(progress), cache = std::move(cache),
          &events](const WorldProgress &before, const WorldProgress &after,
                   const std::vector<CharacterItem> &granted_items)
             -> std::optional<std::vector<CharacterItem>> {
    auto committed = progress->commit(before, after, ItemTrade{granted_items, {}, {}});
    if (!committed) {
      return std::nullopt;
    }

    if (cache.has_tutorial()) {
      events.publish(PlayerProgressCommitted{after});
    }
    if (!committed->empty()) {
      if (const auto live = progress->character()) {
        // Existing grant consumers refresh the item cache without recomputing combat.
        events.publish(PlayerInventorySnapshotChanged{*live, cache.player_stats()});
      }
    }
    return committed;
  };
}

} // namespace chronicles
