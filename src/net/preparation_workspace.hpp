#pragma once

#include <atomic>
#include <memory>

namespace chronicles {

// The world executor normally has one I/O thread. Enforce that assumption for
// packet reconstruction as well, so a future multi-threaded executor cannot
// multiply the temporary JSON and encoder workspace without an explicit design.
class PreparationWorkspace final {
public:
  class Lease final {
  public:
    Lease() = default;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;

    ~Lease() {
      if (owns_workspace_) {
        active().store(false, std::memory_order_release);
      }
    }

  private:
    friend class PreparationWorkspace;
    bool owns_workspace_{};
  };

  static std::shared_ptr<Lease> try_acquire() {
    // Allocate before taking the global slot, so allocation failure cannot strand it.
    auto lease = std::make_shared<Lease>();
    bool expected = false;
    if (!active().compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return {};
    }
    lease->owns_workspace_ = true;
    return lease;
  }

private:
  static std::atomic<bool> &active() {
    static std::atomic<bool> workspace_active{false};
    return workspace_active;
  }
};

} // namespace chronicles
