#pragma once

#include <utility>

namespace chronicles {

// The callback can mutate the party before delivery fails. Failure is therefore an
// uncertain close stage, not proof that the callback is safe to execute again.
template <typename DetachAndDeliver>
bool try_close_party_step(DetachAndDeliver &&detach_and_deliver) noexcept {
  try {
    std::forward<DetachAndDeliver>(detach_and_deliver)();
    return true;
  } catch (...) {
    return false;
  }
}

} // namespace chronicles
