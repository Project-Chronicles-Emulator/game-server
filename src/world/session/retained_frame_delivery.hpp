#pragma once

#include "net/world_connection.hpp"
#include "world/session/gathering_view_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chronicles {

// Callers reserve the source batch's construction/storage budget before building targets.
// This owner never treats a posted handler as delivered and never retries an uncertain
// in-flight send: after timeout the recipient must close or leave before it can be resolved.
class RetainedFrameDelivery final {
public:
  static constexpr std::size_t kMaximumTargets = 4096;
  static constexpr std::size_t kMaximumFrames = 65536;
  static constexpr std::size_t kMaximumBytes = 64 * 1024 * 1024;
  static constexpr std::size_t kFrameOverhead = 1024;

  static std::optional<std::size_t> frame_bytes(const Frame &frame) {
    const auto wire = frame.shared ? frame.shared->size() : frame.bytes.size();
    if (wire > kMaximumBytes || frame.local_retained_bytes > kMaximumBytes - wire ||
        kFrameOverhead > kMaximumBytes - wire - frame.local_retained_bytes) {
      return std::nullopt;
    }
    return wire + frame.local_retained_bytes + kFrameOverhead;
  }

  struct Target {
    std::uint64_t connection{};
    GatheringViewIdentity view;
    std::vector<Frame> frames;
  };

  struct Attempt {
    std::size_t target{};
    std::uint64_t sequence{};
    std::size_t frame_index{};
    std::uint64_t connection{};
    GatheringViewIdentity view;
    Frame frame;
  };

  explicit RetainedFrameDelivery(std::string event_id, std::vector<Target> targets)
      : event_id_(std::move(event_id)) {
    if (event_id_.empty() || event_id_.size() > 64 || targets.size() > kMaximumTargets) {
      throw std::invalid_argument("Invalid retained frame delivery");
    }
    std::unordered_set<std::uint64_t> connections;
    std::size_t frames = 0;
    entries_.reserve(targets.size());
    for (auto &target : targets) {
      if (target.connection == 0 || !connections.insert(target.connection).second ||
          target.frames.size() > kMaximumFrames - frames) {
        throw std::invalid_argument("Invalid retained delivery target");
      }
      frames += target.frames.size();
      for (const auto &frame : target.frames) {
        const auto bytes = frame_bytes(frame);
        if (!bytes || *bytes > kMaximumBytes - retained_bytes_) {
          throw std::invalid_argument("Retained delivery byte limit exceeded");
        }
        retained_bytes_ += *bytes;
      }
      entries_.push_back(Entry{std::move(target)});
    }
  }

  const std::string &event_id() const noexcept {
    return event_id_;
  }

  std::size_t target_count() const noexcept {
    return entries_.size();
  }

  std::optional<Attempt> begin(std::size_t target) {
    std::lock_guard lock(mutex_);
    if (target >= entries_.size()) {
      return std::nullopt;
    }
    auto &entry = entries_[target];
    if (entry.departed || entry.close_required || entry.needs_rebuild || entry.in_flight ||
        entry.next_frame == entry.target.frames.size()) {
      return std::nullopt;
    }
    if (next_sequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
      throw std::overflow_error("Retained delivery sequence exhausted");
    }
    Attempt attempt{target, next_sequence_, entry.next_frame, entry.target.connection,
                    entry.target.view, entry.target.frames[entry.next_frame]};
    ++next_sequence_;
    entry.in_flight = attempt.sequence;
    return attempt;
  }

  bool finish(const Attempt &attempt, FrameEnqueueStatus status, bool same_view) {
    std::lock_guard lock(mutex_);
    if (attempt.target >= entries_.size()) {
      return false;
    }
    auto &entry = entries_[attempt.target];
    if (entry.departed || entry.in_flight != attempt.sequence ||
        entry.next_frame != attempt.frame_index || entry.target.connection != attempt.connection ||
        entry.target.view != attempt.view) {
      return false;
    }
    entry.in_flight.reset();
    if (status == FrameEnqueueStatus::inserted) {
      ++entry.next_frame;
    } else if (status == FrameEnqueueStatus::closed ||
               (status == FrameEnqueueStatus::stale && !same_view)) {
      entry.departed = true;
    } else if (status == FrameEnqueueStatus::stale) {
      entry.needs_rebuild = true;
    } else {
      entry.close_required = true;
    }
    return true;
  }

  // The caller builds a fresh, pre-reserved current-state frame for this exact view.
  // Earlier inserted frames are never replaced or replayed.
  bool replace_stale(std::size_t target, Frame replacement) {
    std::lock_guard lock(mutex_);
    if (target >= entries_.size()) {
      return false;
    }
    auto &entry = entries_[target];
    if (!entry.needs_rebuild || entry.in_flight || entry.departed || entry.close_required ||
        entry.next_frame == entry.target.frames.size()) {
      return false;
    }
    const auto previous = frame_bytes(entry.target.frames[entry.next_frame]);
    const auto next = frame_bytes(replacement);
    if (!previous || !next || *previous > retained_bytes_ ||
        *next > kMaximumBytes - (retained_bytes_ - *previous)) {
      return false;
    }
    static_assert(std::is_nothrow_move_assignable_v<Frame>);
    entry.target.frames[entry.next_frame] = std::move(replacement);
    retained_bytes_ = retained_bytes_ - *previous + *next;
    entry.needs_rebuild = false;
    return true;
  }

  bool timeout(std::size_t target) {
    std::lock_guard lock(mutex_);
    if (target >= entries_.size() || !entries_[target].in_flight || entries_[target].departed) {
      return false;
    }
    entries_[target].close_required = true;
    return true;
  }

  bool confirm_departed(std::size_t target) {
    std::lock_guard lock(mutex_);
    if (target >= entries_.size()) {
      return false;
    }
    entries_[target].departed = true;
    entries_[target].in_flight.reset();
    return true;
  }

  bool close_required(std::size_t target) const {
    std::lock_guard lock(mutex_);
    return target < entries_.size() && entries_[target].close_required &&
           !entries_[target].departed;
  }

  bool needs_rebuild(std::size_t target) const {
    std::lock_guard lock(mutex_);
    return target < entries_.size() && entries_[target].needs_rebuild &&
           !entries_[target].departed;
  }

  bool complete() const {
    std::lock_guard lock(mutex_);
    for (const auto &entry : entries_) {
      if (!entry.departed && entry.next_frame != entry.target.frames.size()) {
        return false;
      }
    }
    return true;
  }


private:
  struct Entry {
    Target target;
    std::size_t next_frame{};
    std::optional<std::uint64_t> in_flight;
    bool close_required{};
    bool needs_rebuild{};
    bool departed{};
  };

  const std::string event_id_;
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  std::size_t retained_bytes_{};
  std::uint64_t next_sequence_{1};
};

} // namespace chronicles
