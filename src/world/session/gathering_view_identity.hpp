#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>

namespace chronicles {

// An authenticated recipient's map and copy are published as one versioned value. The
// generation changes on departures as well as arrivals, so leaving and returning to the same
// map/copy cannot make a previously queued notification valid again.
struct GatheringViewIdentity {
  std::uint64_t epoch{};
  std::optional<int> map_id;
  std::uint64_t copy_channel{};

  bool operator==(const GatheringViewIdentity &) const = default;
};

class GatheringViewState final {
public:
  class QueueGuard final {
  public:
    QueueGuard(GatheringViewState &state, std::unique_lock<std::mutex> lock,
               std::shared_ptr<void> owner, std::optional<std::uint32_t> sector)
        : state_(state), owner_(std::move(owner)), lock_(std::move(lock)), sector_(sector) {
    }

    void mark_ready() {
      if (sector_) {
        state_.sectors_.at(*sector_).ready = true;
      }
    }

    void mark_all_ready() {
      state_.all_sectors_ready_ = true;
    }

  private:
    GatheringViewState &state_;
    std::shared_ptr<void> owner_;
    std::unique_lock<std::mutex> lock_;
    std::optional<std::uint32_t> sector_;
  };

  std::shared_ptr<QueueGuard> acquire_queue_guard(
      const GatheringViewIdentity &expected, std::optional<std::uint32_t> sector,
      std::optional<std::uint64_t> generation, bool require_ready, std::shared_ptr<void> owner) {
    std::unique_lock lock(mutex_);
    if (sealed_for_close_ || current_ != expected || !current_.map_id) {
      return {};
    }
    if (sector) {
      const auto found = sectors_.find(*sector);
      const bool all_ready = all_sectors_ready_ && generation && *generation == 0;
      if (!all_ready && (!generation || found == sectors_.end() ||
                         found->second.generation != *generation ||
                         (require_ready && !found->second.ready))) {
        return {};
      }
    }
    return std::make_shared<QueueGuard>(*this, std::move(lock), std::move(owner), sector);
  }

  GatheringViewIdentity snapshot() const {
    std::lock_guard lock(mutex_);
    return current_;
  }

  // A partial fanout must not close a later view after leave/re-entry. Sealing and
  // matching use the same mutex as view changes; no transport work runs under it.
  bool seal_for_close(const GatheringViewIdentity &expected) {
    std::lock_guard lock(mutex_);
    if (current_ != expected || !current_.map_id) {
      return false;
    }
    sealed_for_close_ = true;
    return true;
  }

  bool matches(const GatheringViewIdentity &expected) const {
    std::lock_guard lock(mutex_);
    return current_ == expected && current_.map_id.has_value();
  }

  bool sector_ready(const GatheringViewIdentity &expected, std::uint32_t sector) const {
    std::lock_guard lock(mutex_);
    const auto found = sectors_.find(sector);
    return current_ == expected && current_.map_id.has_value() &&
           (all_sectors_ready_ || (found != sectors_.end() && found->second.ready));
  }

  std::optional<std::uint64_t> sector_generation(const GatheringViewIdentity &expected,
                                                  std::uint32_t sector) const {
    std::lock_guard lock(mutex_);
    if (current_ != expected || !current_.map_id) {
      return std::nullopt;
    }
    const auto found = sectors_.find(sector);
    if (found == sectors_.end()) {
      if (all_sectors_ready_) {
        return 0;
      }
      return std::nullopt;
    }
    return found->second.generation;
  }

  bool sector_matches(const GatheringViewIdentity &expected, std::uint32_t sector,
                      std::uint64_t generation, bool require_ready) const {
    std::lock_guard lock(mutex_);
    const auto found = sectors_.find(sector);
    const bool valid_sector = (all_sectors_ready_ && generation == 0) ||
        (found != sectors_.end() && found->second.generation == generation &&
         (!require_ready || found->second.ready));
    return current_ == expected && current_.map_id && valid_sector;
  }

  bool mark_sector_ready(const GatheringViewIdentity &expected, std::uint32_t sector,
                         std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    const auto found = sectors_.find(sector);
    if (current_ != expected || !current_.map_id || found == sectors_.end() ||
        found->second.generation != generation) {
      return false;
    }
    found->second.ready = true;
    return true;
  }

  void synchronize_sectors(const GatheringViewIdentity &expected,
                           const std::set<std::uint32_t> &visible) {
    if (visible.size() > kMaximumReadySectors) {
      throw std::runtime_error("Gathering recipient sector window is too large");
    }
    std::lock_guard lock(mutex_);
    if (current_ != expected || !current_.map_id) {
      return;
    }
    auto next = sectors_;
    auto next_generation = next_sector_generation_;
    for (auto iterator = next.begin(); iterator != next.end();) {
      if (!visible.contains(iterator->first)) {
        iterator = next.erase(iterator);
      } else {
        ++iterator;
      }
    }
    for (const auto sector : visible) {
      if (!next.contains(sector)) {
        if (next_generation == (std::numeric_limits<std::uint64_t>::max)()) {
          throw std::overflow_error("Gathering recipient sector generation is exhausted");
        }
        next.emplace(sector, SectorState{++next_generation, false});
      }
    }
    sectors_.swap(next);
    next_sector_generation_ = next_generation;
  }

  bool mark_all_sectors_ready(const GatheringViewIdentity &expected) {
    std::lock_guard lock(mutex_);
    if (current_ != expected || !current_.map_id) {
      return false;
    }
    all_sectors_ready_ = true;
    return true;
  }

  void publish(std::optional<int> map_id, std::uint64_t copy_channel) {
    if (!map_id) {
      copy_channel = 0;
    }
    std::lock_guard lock(mutex_);
    if (sealed_for_close_) {
      throw std::runtime_error("Recipient view is sealed for close");
    }
    if (current_.map_id == map_id && current_.copy_channel == copy_channel) {
      return;
    }
    advance_epoch();
    current_.map_id = map_id;
    current_.copy_channel = copy_channel;
    sectors_.clear();
    all_sectors_ready_ = false;
  }

  // A departure always advances the epoch, even if no map is currently ready.
  void invalidate() {
    std::lock_guard lock(mutex_);
    if (sealed_for_close_) {
      throw std::runtime_error("Recipient view is sealed for close");
    }
    invalidate_locked();
  }

  // The transport close path must invalidate even a view already sealed by a failure.
  void invalidate_for_close() {
    std::lock_guard lock(mutex_);
    invalidate_locked();
  }

private:
  void invalidate_locked() {
    advance_epoch();
    current_.map_id.reset();
    current_.copy_channel = 0;
    sectors_.clear();
    all_sectors_ready_ = false;
  }

  static constexpr std::size_t kMaximumReadySectors = 4096;
  struct SectorState {
    std::uint64_t generation{};
    bool ready{};
  };
  bool sealed_for_close_{};
  void advance_epoch() {
    if (current_.epoch == (std::numeric_limits<std::uint64_t>::max)()) {
      throw std::overflow_error("Gathering recipient view epoch is exhausted");
    }
    ++current_.epoch;
  }

  mutable std::mutex mutex_;
  GatheringViewIdentity current_;
  std::map<std::uint32_t, SectorState> sectors_;
  std::uint64_t next_sector_generation_{};
  bool all_sectors_ready_{};
};

} // namespace chronicles
