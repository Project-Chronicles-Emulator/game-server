#pragma once

#include <cstddef>
#include <utility>

namespace chronicles {

// No write is started during insertion. If allocation or the readiness callback fails,
// restore the queue exactly before the caller closes the connection.
template <typename Queue, typename Batch, typename Ready>
void append_frame_batch_atomically(Queue &queue, Batch &batch, Ready &&ready) {
  const auto previous_size = queue.size();
  try {
    for (auto &frame : batch) {
      queue.push_back(std::move(frame));
    }
    std::forward<Ready>(ready)();
  } catch (...) {
    while (queue.size() > previous_size) {
      queue.pop_back();
    }
    throw;
  }
}

} // namespace chronicles
