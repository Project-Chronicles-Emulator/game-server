#pragma once

#include <cstddef>
#include <cstdint>

namespace chronicles {

// Socket-free state of "when may a world session read its next request after a response".
//
// Drain mode (handshake, welcome, before presence is published): the read resumes when the
// outbound queue is empty. Barrier mode (in-world steady state): the read resumes as soon as the
// frames written so far cover every frame enqueued up to the park, provided the remaining backlog
// is within kBacklogFrames / kBacklogBytes. Drain stays the fallback in both modes. The session
// forwards to this class on its single I/O executor; nothing here is thread-safe.
class ReadResumeGate {
public:
  // Backlog guard: a barrier resume waits while the queue holds more than this (inclusive limit).
  static constexpr std::size_t kBacklogFrames = 64;
  static constexpr std::size_t kBacklogBytes = 64 * 1024;

  enum class Park { none, drain, barrier };
  enum class Resume { none, by_drain, by_barrier, blocked_by_backlog };

  // Park choice for a response-bearing request, evaluated after its last enqueue_frame.
  // No park on a closed connection; barrier only in the steady state and only when this request
  // really enqueued a frame (otherwise there is nothing to wait for).
  static constexpr Park choose_park(bool closed, bool presence_published,
                                    std::uint64_t frames_enqueued_by_request) noexcept {
    if (closed) {
      return Park::none;
    }
    return presence_published && frames_enqueued_by_request > 0 ? Park::barrier : Park::drain;
  }

  void on_enqueued() noexcept { ++enqueued_; }
  void on_written() noexcept { ++written_; }
  std::uint64_t enqueued() const noexcept { return enqueued_; }
  std::uint64_t written() const noexcept { return written_; }
  bool parked() const noexcept { return parked_; }

  void park_drain() noexcept {
    parked_ = true;
    barrier_mode_ = false;
  }

  // The barrier is every frame enqueued so far, i.e. this request's own frames and the backlog
  // before them, taken after the request's last enqueue. Later notifications do not extend it.
  void park_barrier() noexcept {
    parked_ = true;
    barrier_mode_ = true;
    barrier_ = enqueued_;
  }

  // Called after a frame completed (handshake exit/close and presence publication already
  // handled). Clears the parked state when it resumes, so the drain branch cannot resume again.
  Resume try_resume_by_barrier(std::size_t queued_frames, std::size_t queued_bytes) noexcept {
    if (!parked_ || !barrier_mode_ || written_ < barrier_) {
      return Resume::none;
    }
    if (queued_frames > kBacklogFrames || queued_bytes > kBacklogBytes) {
      return Resume::blocked_by_backlog;
    }
    parked_ = false;
    return Resume::by_barrier;
  }

  // Called when the outbound queue is empty and no write is in flight.
  Resume try_resume_by_drain() noexcept {
    if (!parked_) {
      return Resume::none;
    }
    parked_ = false;
    return Resume::by_drain;
  }

private:
  std::uint64_t enqueued_{};
  std::uint64_t written_{};
  std::uint64_t barrier_{};
  bool parked_{};
  bool barrier_mode_{};
};

} // namespace chronicles
