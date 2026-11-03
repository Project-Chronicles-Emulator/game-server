#pragma once

#include "world/features/combat/combat_timeline.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

namespace chronicles {

// A wake callback must retain only a weak session reference and post to its executor.
// Missing owners and failed posts are reported to the coordinator; neither implies that
// queued or claimed combat work may be discarded. A posted callback must recheck its owner
// generation on that executor because unregistration can race a previously copied callback.
class CombatOwnerWakes final {
public:
  using Callback = std::function<bool()>;

  explicit CombatOwnerWakes(std::size_t capacity) : capacity_(capacity) {
  }

  bool register_owner(combat::CombatTimeline::Owner owner, Callback callback) {
    if (owner.id == 0 || owner.generation == 0 || !callback) {
      return false;
    }
    std::lock_guard lock(mutex_);
    const Key key{owner.id, owner.generation};
    if (owners_.contains(key) || owners_.size() >= capacity_) {
      return false;
    }
    owners_.emplace(key, std::move(callback));
    return true;
  }

  bool unregister_owner(combat::CombatTimeline::Owner owner) {
    std::lock_guard lock(mutex_);
    return owners_.erase(Key{owner.id, owner.generation}) != 0;
  }

  bool request(combat::CombatTimeline::Owner owner) const {
    Callback callback;
    {
      std::lock_guard lock(mutex_);
      const auto found = owners_.find(Key{owner.id, owner.generation});
      if (found == owners_.end()) {
        return false;
      }
      callback = found->second;
    }
    return callback();
  }

private:
  using Key = std::pair<std::uint64_t, std::uint64_t>;
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::map<Key, Callback> owners_;
};

} // namespace chronicles
