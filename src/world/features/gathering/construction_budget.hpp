#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace chronicles::gathering {

// Admission is charged before snapshot allocations and retained until the packets' posted
// handlers have acquired their own reservations. The overlapping charge avoids a transfer gap.
class ConstructionBudget final {
public:
  static constexpr std::size_t kMaximumGlobalBytes = 256 * 1024 * 1024;
  static constexpr std::size_t kMaximumBatchBytes = 64 * 1024 * 1024;
  static constexpr std::size_t kSectorBytes = 16 * 1024;
  static constexpr std::size_t kNodeBytes = 4096;

  class Lease final {
  public:
    Lease() = default;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;

    ~Lease() {
      std::lock_guard lock(mutex());
      used() -= bytes_;
    }

    void charge(std::size_t bytes) {
      std::lock_guard lock(mutex());
      if (bytes > kMaximumBatchBytes - bytes_ || bytes > kMaximumGlobalBytes - used()) {
        throw std::runtime_error("Gathering snapshot construction budget exhausted");
      }
      bytes_ += bytes;
      used() += bytes;
    }

  private:
    std::size_t bytes_{};
  };

  static std::shared_ptr<Lease> acquire() {
    return std::make_shared<Lease>();
  }

private:
  static std::mutex &mutex() {
    static std::mutex budget_mutex;
    return budget_mutex;
  }

  static std::size_t &used() {
    static std::size_t used_bytes = 0;
    return used_bytes;
  }
};

} // namespace chronicles::gathering
