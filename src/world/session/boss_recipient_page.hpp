#pragma once

#include "world/session/retained_frame_delivery.hpp"
#include "world/features/gathering/construction_budget.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace chronicles {

struct BossRecipientPage {
  std::vector<std::uint64_t> connection_ids;
  std::uint64_t end{};
  bool last{};
};

class BossPublicationOversize final : public std::runtime_error {
public:
  BossPublicationOversize() : std::runtime_error("Boss publication cannot fit one recipient") {
  }
};

inline std::size_t boss_recipient_page_limit(std::size_t frame_bytes,
                                             std::size_t frame_count,
                                             std::size_t shared_bytes) {
  using gathering::ConstructionBudget;
  if (frame_count == 0 || shared_bytes >= ConstructionBudget::kMaximumBatchBytes ||
      frame_bytes > RetainedFrameDelivery::kMaximumBytes ||
      frame_bytes > ConstructionBudget::kMaximumBatchBytes - ConstructionBudget::kNodeBytes) {
    throw BossPublicationOversize();
  }
  const auto per_recipient = frame_bytes + ConstructionBudget::kNodeBytes;
  const auto construction_limit =
      (ConstructionBudget::kMaximumBatchBytes - shared_bytes) / per_recipient;
  const auto tracker_limit = frame_bytes == 0 ? RetainedFrameDelivery::kMaximumTargets
                                               : RetainedFrameDelivery::kMaximumBytes / frame_bytes;
  const auto frame_limit = RetainedFrameDelivery::kMaximumFrames / frame_count;
  const auto limit = (std::min)({RetainedFrameDelivery::kMaximumTargets, construction_limit,
                                 tracker_limit, frame_limit});
  if (limit == 0) {
    throw BossPublicationOversize();
  }
  return limit;
}

class BossDeliveryCompletion final {
public:
  void finish(RetainedFrameDelivery &delivery, const RetainedFrameDelivery::Attempt &attempt,
              FrameEnqueueStatus status, bool same_view) noexcept {
    try {
      const auto outcome = status == FrameEnqueueStatus::closed && same_view
                               ? FrameEnqueueStatus::failed
                               : status;
      if (!delivery.finish(attempt, outcome, same_view) && same_view) {
        invariant_failed_.store(true);
      }
    } catch (...) {
      invariant_failed_.store(true);
    }
  }

  bool complete(const RetainedFrameDelivery &delivery) const {
    return !invariant_failed_.load() && delivery.complete();
  }

  bool invariant_failed() const noexcept {
    return invariant_failed_.load();
  }

private:
  std::atomic<bool> invariant_failed_{};
};

// The caller holds the session registry lock and reserves memory before invoking this.
// A fixed high-water mark makes concurrent new connections rely on their fresh snapshot.
template <typename SessionRegistry>
BossRecipientPage select_boss_recipient_page(const SessionRegistry &sessions,
                                             std::uint64_t cursor,
                                             std::uint64_t high_water,
                                             std::size_t limit) {
  if (limit == 0 || cursor > high_water) {
    throw std::invalid_argument("Invalid boss recipient page boundary");
  }
  BossRecipientPage page;
  page.connection_ids.reserve(limit);
  for (const auto &[connection, unused] : sessions) {
    static_cast<void>(unused);
    if (connection <= cursor || connection > high_water) {
      continue;
    }
    if (page.connection_ids.size() < limit) {
      page.connection_ids.push_back(connection);
      std::push_heap(page.connection_ids.begin(), page.connection_ids.end());
    } else if (connection < page.connection_ids.front()) {
      std::pop_heap(page.connection_ids.begin(), page.connection_ids.end());
      page.connection_ids.back() = connection;
      std::push_heap(page.connection_ids.begin(), page.connection_ids.end());
    }
  }
  std::sort(page.connection_ids.begin(), page.connection_ids.end());
  page.end = page.connection_ids.empty() ? high_water : page.connection_ids.back();
  page.last = page.connection_ids.size() < limit || page.end == high_water;
  return page;
}

} // namespace chronicles
