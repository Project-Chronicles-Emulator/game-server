#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace chronicles {

// Shared immutable allocations are charged once globally, while each recipient is charged
// for the full allocation and for its own posted handler. The default server limits keep stalled
// I/O bounded; callers embedding the transport can provide a smaller shared budget for tests.
class GlobalPendingFrameBudget final {
public:
  struct Allocation {
    std::shared_ptr<const void> owner;
    std::size_t bytes{};
  };

  GlobalPendingFrameBudget(std::size_t maximum_count, std::size_t maximum_bytes)
      : maximum_count_(maximum_count), maximum_bytes_(maximum_bytes) {
  }

private:
  friend class PendingFrameBudget;
  struct Entry {
    std::size_t bytes{};
    std::size_t references{};
  };
  std::mutex mutex_;
  std::size_t count_{};
  std::size_t bytes_{};
  const std::size_t maximum_count_;
  const std::size_t maximum_bytes_;
  std::map<std::shared_ptr<const void>, Entry, std::owner_less<std::shared_ptr<const void>>> shared_;
};

class PendingFrameBudget final {
  struct State {
    std::mutex mutex;
    std::size_t count{};
    std::size_t bytes{};
    std::size_t maximum_count{};
    std::size_t maximum_bytes{};
    std::shared_ptr<GlobalPendingFrameBudget> global;
  };

public:
  class Lease final {
  public:
    Lease(std::shared_ptr<State> state, std::size_t count, std::size_t local_bytes,
          std::size_t private_bytes, std::vector<GlobalPendingFrameBudget::Allocation> allocations)
        : state_(std::move(state)), count_(count), local_bytes_(local_bytes),
          private_bytes_(private_bytes), allocations_(std::move(allocations)) {
    }

    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;

    ~Lease() {
      if (!armed_) {
        return;
      }
      auto &global = *state_->global;
      std::lock_guard global_lock(global.mutex_);
      std::lock_guard local_lock(state_->mutex);
      if (state_->count < count_ || state_->bytes < local_bytes_ ||
          global.count_ < count_ || global.bytes_ < private_bytes_) {
        std::terminate();
      }
      state_->count -= count_;
      state_->bytes -= local_bytes_;
      global.count_ -= count_;
      global.bytes_ -= private_bytes_;
      for (const auto &allocation : allocations_) {
        const auto found = global.shared_.find(allocation.owner);
        if (found == global.shared_.end() || found->second.references == 0 ||
            global.bytes_ < found->second.bytes) {
          std::terminate();
        }
        if (--found->second.references == 0) {
          global.bytes_ -= found->second.bytes;
          global.shared_.erase(found);
        }
      }
    }

  private:
    friend class PendingFrameBudget;
    const std::shared_ptr<State> state_;
    const std::size_t count_;
    const std::size_t local_bytes_;
    const std::size_t private_bytes_;
    const std::vector<GlobalPendingFrameBudget::Allocation> allocations_;
    bool armed_{};
  };

  PendingFrameBudget(std::size_t maximum_count, std::size_t maximum_bytes,
                     std::shared_ptr<GlobalPendingFrameBudget> global)
      : state_(std::make_shared<State>()) {
    state_->maximum_count = maximum_count;
    state_->maximum_bytes = maximum_bytes;
    state_->global = std::move(global);
  }

  std::shared_ptr<Lease> acquire(
      std::size_t count, std::size_t private_bytes,
      std::vector<GlobalPendingFrameBudget::Allocation> allocations = {}) {
    if (!state_->global || count == 0 || private_bytes > state_->maximum_bytes) {
      return {};
    }
    auto less = std::owner_less<std::shared_ptr<const void>>{};
    std::ranges::sort(allocations, [&](const auto &left, const auto &right) {
      return less(left.owner, right.owner);
    });
    std::vector<GlobalPendingFrameBudget::Allocation> distinct;
    std::size_t local_bytes = private_bytes;
    for (const auto &allocation : allocations) {
      if (!allocation.owner || allocation.bytes > state_->maximum_bytes - local_bytes) {
        return {};
      }
      local_bytes += allocation.bytes;
      if (!distinct.empty() && !less(distinct.back().owner, allocation.owner) &&
          !less(allocation.owner, distinct.back().owner)) {
        if (distinct.back().bytes != allocation.bytes) {
          return {};
        }
      } else {
        distinct.push_back(allocation);
      }
    }
    auto lease = std::make_shared<Lease>(state_, count, local_bytes, private_bytes,
                                          std::move(distinct));
    auto &global = *state_->global;
    std::lock_guard global_lock(global.mutex_);
    std::lock_guard local_lock(state_->mutex);
    if (count > state_->maximum_count - state_->count ||
        local_bytes > state_->maximum_bytes - state_->bytes ||
        count > global.maximum_count_ - global.count_ ||
        private_bytes > global.maximum_bytes_ - global.bytes_) {
      return {};
    }
    auto available = global.maximum_bytes_ - global.bytes_ - private_bytes;
    for (const auto &allocation : lease->allocations_) {
      const auto found = global.shared_.find(allocation.owner);
      if (found != global.shared_.end()) {
        if (found->second.bytes != allocation.bytes) {
          return {};
        }
      } else {
        if (allocation.bytes > available) {
          return {};
        }
        available -= allocation.bytes;
      }
    }
    std::size_t inserted = 0;
    try {
      for (const auto &allocation : lease->allocations_) {
        auto [entry, created] = global.shared_.try_emplace(
            allocation.owner, GlobalPendingFrameBudget::Entry{allocation.bytes, 0});
        ++entry->second.references;
        if (created) {
          global.bytes_ += allocation.bytes;
        }
        ++inserted;
      }
    } catch (...) {
      for (std::size_t index = 0; index < inserted; ++index) {
        const auto found = global.shared_.find(lease->allocations_[index].owner);
        if (--found->second.references == 0) {
          global.bytes_ -= found->second.bytes;
          global.shared_.erase(found);
        }
      }
      throw;
    }
    state_->count += count;
    state_->bytes += local_bytes;
    global.count_ += count;
    global.bytes_ += private_bytes;
    lease->armed_ = true;
    return lease;
  }

private:
  const std::shared_ptr<State> state_;
};

} // namespace chronicles
