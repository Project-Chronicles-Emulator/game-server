#include "services/admin/admin_log_buffer.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace chronicles {

AdminLogBuffer::AdminLogBuffer(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

void AdminLogBuffer::sink_it_(const spdlog::details::log_msg &message) {
  const auto since_epoch = message.time.time_since_epoch();

  StoredEntry stored;
  stored.level = message.level;
  stored.entry.unix_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
  const auto level_name = spdlog::level::to_string_view(message.level);
  stored.entry.level.assign(level_name.data(), level_name.size());
  stored.entry.message.assign(message.payload.data(), message.payload.size());

  std::lock_guard lock(buffer_mutex_);
  stored.entry.sequence = next_sequence_++;
  entries_.push_back(std::move(stored));
  if (entries_.size() > capacity_) {
    entries_.pop_front();
  }
}

void AdminLogBuffer::flush_() {}

AdminLogPage AdminLogBuffer::read_after(std::uint64_t after_sequence, std::size_t limit,
                                        spdlog::level::level_enum minimum_level) const {
  const auto page_limit = std::clamp<std::size_t>(limit, 1, kMaximumAdminLogPage);

  std::lock_guard lock(buffer_mutex_);
  AdminLogPage page;
  page.next_after_sequence = after_sequence;
  if (entries_.empty()) {
    return page;
  }

  page.latest_sequence = entries_.back().entry.sequence;
  const auto oldest_sequence = entries_.front().entry.sequence;
  // A cursor beyond the newest record came from an earlier server process; restart from the
  // oldest retained record and report the discontinuity.
  const bool stale_cursor = after_sequence > page.latest_sequence;
  if (stale_cursor) {
    after_sequence = 0;
  }
  page.gap = stale_cursor || after_sequence + 1 < oldest_sequence;

  // Sequences are contiguous inside the ring, so the first wanted record is found by offset.
  const auto first_wanted = std::max(after_sequence + 1, oldest_sequence);
  if (first_wanted > page.latest_sequence) {
    page.next_after_sequence = std::max(after_sequence, page.latest_sequence);
    return page;
  }

  for (auto index = static_cast<std::size_t>(first_wanted - oldest_sequence);
       index < entries_.size() && page.entries.size() < page_limit; ++index) {
    const auto &stored = entries_[index];
    page.next_after_sequence = stored.entry.sequence;
    if (stored.level >= minimum_level) {
      page.entries.push_back(stored.entry);
    }
  }
  return page;
}

std::shared_ptr<AdminLogBuffer> install_admin_log_buffer(std::size_t capacity) {
  auto buffer = std::make_shared<AdminLogBuffer>(capacity);
  spdlog::default_logger()->sinks().push_back(buffer);
  return buffer;
}

} // namespace chronicles
