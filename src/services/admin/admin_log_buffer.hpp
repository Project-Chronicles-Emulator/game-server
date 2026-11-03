#pragma once

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace chronicles {

inline constexpr std::size_t kDefaultAdminLogCapacity = 5000;
inline constexpr std::size_t kMaximumAdminLogPage = 1000;

struct AdminLogEntry {
  // Increases by one per record for the process lifetime; clients poll with the last seen value.
  std::uint64_t sequence{};
  std::int64_t unix_milliseconds{};
  std::string level;
  std::string message;
};

struct AdminLogPage {
  std::vector<AdminLogEntry> entries;
  // Sequence of the newest retained record; zero before anything has been logged.
  std::uint64_t latest_sequence{};
  // Pass this as the next after_sequence. It covers records skipped by the level filter, so a
  // filtered poll still advances.
  std::uint64_t next_after_sequence{};
  // True when records after the requested sequence were already evicted from the ring.
  bool gap{};
};

// Bounded ring of recent server log records for the operator console. The log text already
// excludes credentials and packet payloads by the server's logging policy; this sink does not
// add any other data. The base sink uses null_mutex because buffer_mutex_ guards everything.
class AdminLogBuffer final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
  explicit AdminLogBuffer(std::size_t capacity = kDefaultAdminLogCapacity);

  // Records with sequence > after_sequence, oldest first, at most `limit` (bounded by
  // kMaximumAdminLogPage). A minimum level filters by spdlog level name ("info", "warning"...).
  AdminLogPage read_after(std::uint64_t after_sequence, std::size_t limit,
                          spdlog::level::level_enum minimum_level = spdlog::level::trace) const;

protected:
  void sink_it_(const spdlog::details::log_msg &message) override;

  void flush_() override;

private:
  struct StoredEntry {
    AdminLogEntry entry;
    spdlog::level::level_enum level{};
  };

  const std::size_t capacity_;
  mutable std::mutex buffer_mutex_;
  std::deque<StoredEntry> entries_;
  std::uint64_t next_sequence_{1};
};

// Adds a buffer to the default logger's sinks. Call once at startup, before other threads log:
// spdlog does not synchronize changes to a logger's sink list.
std::shared_ptr<AdminLogBuffer>
install_admin_log_buffer(std::size_t capacity = kDefaultAdminLogCapacity);

} // namespace chronicles
