#include "world/core/retained_owner_supervisor.hpp"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace chronicles {

namespace {

constexpr std::size_t kMaximumRetainedOwners = 100000;
constexpr std::size_t kMinimumWatchdogPageSize = 4096;

} // namespace

struct RetainedOwnerSupervisor::State : std::enable_shared_from_this<State> {
  using Clock = std::chrono::steady_clock;

  struct KeyLess {
    bool operator()(RetainedOwnerKey left, RetainedOwnerKey right) const noexcept {
      if (left.character_id != right.character_id) {
        return left.character_id < right.character_id;
      }
      return left.generation < right.generation;
    }
  };

  struct Entry {
    Clock::time_point next_attempt{Clock::now()};
    Clock::time_point started{};
    std::chrono::milliseconds backoff{};
    std::uint64_t attempt{};
    bool in_flight{};
    bool submitting{};
    bool uncertain{};
    bool overdue_reported{};
    bool active{};
  };

  State(Options configured, Submit post, Alarm notify)
      : options(configured), submit(std::move(post)), alarm(std::move(notify)),
        heartbeat(Clock::now()) {}

  void report(RetainedOwnerAlarm reason, RetainedOwnerKey key) noexcept {
    {
      std::lock_guard lock(mutex);
      if (active_alarms++ == 0) {
        first_alarm_started = Clock::now();
      }
    }
    bool failed = false;
    try {
      alarm(reason, key);
    } catch (...) {
      failed = true;
    }
    {
      std::lock_guard lock(mutex);
      --active_alarms;
      alarm_delivery_failed = alarm_delivery_failed || failed;
    }
  }

  bool finish(RetainedOwnerKey key, std::uint64_t attempt, RetainedOwnerOutcome outcome) noexcept {
    std::lock_guard lock(mutex);
    const auto found = entries.find(key);
    if (found == entries.end() || found->second.attempt != attempt || !found->second.in_flight) {
      return false;
    }
    if (outcome == RetainedOwnerOutcome::settled) {
      active.erase(key);
      entries.erase(found);
    } else {
      auto &entry = found->second;
      entry.in_flight = false;
      entry.uncertain = false;
      entry.overdue_reported = false;
      entry.next_attempt = Clock::now() + entry.backoff;
      if (entry.backoff >= options.maximum_backoff / 2) {
        entry.backoff = options.maximum_backoff;
      } else {
        entry.backoff *= 2;
      }
    }
    condition.notify_all();
    return true;
  }

  void mark_uncertain(RetainedOwnerKey key, std::uint64_t attempt) noexcept {
    bool report_unknown = false;
    {
      std::lock_guard lock(mutex);
      const auto found = entries.find(key);
      if (found != entries.end() && found->second.attempt == attempt && found->second.in_flight) {
        found->second.submitting = false;
        found->second.uncertain = true;
        report_unknown = true;
      }
    }
    if (report_unknown) {
      report(RetainedOwnerAlarm::post_outcome_unknown, key);
    }
  }

  void run_scheduler() noexcept {
    std::size_t scanned_since_wait = 0;
    for (;;) {
      std::optional<RetainedOwnerKey> selected;
      std::uint64_t attempt = 0;
      try {
        std::unique_lock lock(mutex);
        heartbeat = Clock::now();
        stalled_reported = false;
        if (stopping) {
          return;
        }
        if (inject_wait_failure) {
          inject_wait_failure = false;
          throw std::system_error(std::make_error_code(std::errc::io_error));
        }
        if (active.empty() || scanned_since_wait >= active.size()) {
          condition.wait_for(lock, options.poll_interval);
          scanned_since_wait = 0;
          heartbeat = Clock::now();
          if (stopping) {
            return;
          }
        }
        auto it = cursor ? active.upper_bound(*cursor) : active.begin();
        if (it == active.end()) {
          it = active.begin();
        }
        const auto count = std::min(options.page_size, active.size());
        for (std::size_t index = 0; index < count; ++index) {
          if (it == active.end()) {
            it = active.begin();
          }
          const auto key = *it;
          auto &entry = entries.at(key);
          ++it;
          ++scanned_since_wait;
          cursor = key;
          if (entry.in_flight || entry.next_attempt > Clock::now()) {
            continue;
          }
          if (entry.attempt == std::numeric_limits<std::uint64_t>::max()) {
            entry.in_flight = true;
            entry.uncertain = true;
            entry.started = Clock::now();
            selected = key;
            break;
          }
          ++entry.attempt;
          entry.in_flight = true;
          entry.submitting = true;
          entry.started = Clock::now();
          selected = key;
          attempt = entry.attempt;
          break;
        }
      } catch (...) {
        {
          std::lock_guard lock(mutex);
          wait_failure_seen = true;
        }
        report(RetainedOwnerAlarm::wait_failed, {});
        std::this_thread::sleep_for(options.poll_interval);
        continue;
      }
      if (!selected) {
        continue;
      }
      if (attempt == 0) {
        report(RetainedOwnerAlarm::internal_failure, *selected);
        continue;
      }

      try {
        const std::weak_ptr<State> weak = shared_from_this();
        Completion completed = [weak, key = *selected, attempt](RetainedOwnerOutcome outcome) {
          if (const auto state = weak.lock()) {
            state->finish(key, attempt, outcome);
          }
        };
        const auto result = submit(*selected, std::move(completed));
        {
          std::lock_guard lock(mutex);
          const auto found = entries.find(*selected);
          if (found != entries.end() && found->second.attempt == attempt) {
            found->second.submitting = false;
          }
        }
        if (result == RetainedOwnerSubmit::definitely_not_posted) {
          if (!finish(*selected, attempt, RetainedOwnerOutcome::retry)) {
            report(RetainedOwnerAlarm::internal_failure, *selected);
          }
        }
      } catch (...) {
        mark_uncertain(*selected, attempt);
      }
    }
  }

  void run_watchdog() noexcept {
    for (;;) {
      std::optional<std::pair<RetainedOwnerAlarm, RetainedOwnerKey>> notification;
      try {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, options.poll_interval);
        if (stopping) {
          return;
        }
        const auto now = Clock::now();
        if (!stalled_reported && now - heartbeat >= options.scheduler_stall_after) {
          stalled_reported = true;
          notification = std::pair{RetainedOwnerAlarm::scheduler_stalled, RetainedOwnerKey{}};
        } else {
          auto it = watchdog_cursor ? active.upper_bound(*watchdog_cursor) : active.begin();
          if (it == active.end()) {
            it = active.begin();
          }
          const auto count = std::min(active.size(),
                                      std::max(options.page_size, kMinimumWatchdogPageSize));
          for (std::size_t index = 0; index < count; ++index) {
            if (it == active.end()) {
              it = active.begin();
            }
            const auto key = *it;
            auto &entry = entries.at(key);
            ++it;
            watchdog_cursor = key;
            if (entry.in_flight && !entry.overdue_reported &&
                now - entry.started >= options.overdue_after) {
              entry.overdue_reported = true;
              notification = std::pair{RetainedOwnerAlarm::attempt_overdue, key};
              break;
            }
          }
        }
      } catch (...) {
        {
          std::lock_guard lock(mutex);
          wait_failure_seen = true;
        }
        report(RetainedOwnerAlarm::wait_failed, {});
        std::this_thread::sleep_for(options.poll_interval);
        continue;
      }
      if (notification) {
        report(notification->first, notification->second);
      }
    }
  }

  Options options;
  Submit submit;
  Alarm alarm;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::map<RetainedOwnerKey, Entry> entries;
  std::set<RetainedOwnerKey, KeyLess> dormant;
  std::set<RetainedOwnerKey, KeyLess> active;
  std::optional<RetainedOwnerKey> cursor;
  std::optional<RetainedOwnerKey> watchdog_cursor;
  Clock::time_point heartbeat;
  bool stopping{};
  bool stalled_reported{};
  bool alarm_delivery_failed{};
  bool wait_failure_seen{};
  bool inject_wait_failure{};
  std::size_t active_alarms{};
  Clock::time_point first_alarm_started{};
};

RetainedOwnerSupervisor::RetainedOwnerSupervisor(Options options, Submit submit, Alarm alarm) {
  if (!submit || !alarm || options.capacity == 0 || options.capacity > kMaximumRetainedOwners ||
      options.page_size == 0 || options.page_size > 1024 ||
      options.page_size > options.capacity || options.poll_interval.count() <= 0 ||
      options.initial_backoff.count() <= 0 || options.maximum_backoff < options.initial_backoff ||
      options.maximum_backoff > std::chrono::hours(1) ||
      options.poll_interval > std::chrono::hours(1) ||
      options.overdue_after > std::chrono::hours(24) ||
      options.scheduler_stall_after > std::chrono::hours(24) ||
      options.overdue_after <= options.poll_interval ||
      options.scheduler_stall_after <= options.poll_interval) {
    throw std::invalid_argument("Invalid retained owner supervisor configuration");
  }
  state_ = std::make_shared<State>(options, std::move(submit), std::move(alarm));
  scheduler_ = std::thread([state = state_] { state->run_scheduler(); });
  try {
    watchdog_ = std::thread([state = state_] { state->run_watchdog(); });
  } catch (...) {
    {
      std::lock_guard lock(state_->mutex);
      state_->stopping = true;
    }
    state_->condition.notify_all();
    scheduler_.join();
    throw;
  }
}

RetainedOwnerSupervisor::~RetainedOwnerSupervisor() {
  {
    std::unique_lock lock(state_->mutex);
    state_->condition.wait(lock, [&] { return state_->entries.empty(); });
  }
  stop();
}

bool RetainedOwnerSupervisor::reserve_owner(RetainedOwnerKey key) {
  if (key.character_id <= 0 || key.generation == 0) {
    return false;
  }
  std::lock_guard lock(state_->mutex);
  if (state_->stopping) {
    return false;
  }
  if (state_->entries.contains(key)) {
    return false;
  }
  if (state_->entries.size() >= state_->options.capacity) {
    return false;
  }
  State::Entry entry;
  entry.backoff = state_->options.initial_backoff;
  const auto [inserted, success] = state_->entries.emplace(key, entry);
  if (!success) {
    return false;
  }
  try {
    if (!state_->dormant.insert(key).second) {
      state_->entries.erase(inserted);
      return false;
    }
  } catch (...) {
    state_->entries.erase(inserted);
    throw;
  }
  return true;
}

bool RetainedOwnerSupervisor::activate_owner(RetainedOwnerKey key) noexcept {
  try {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->entries.find(key);
    if (state_->stopping || found == state_->entries.end()) {
      return false;
    }
    if (found->second.active) {
      return true;
    }
    auto node = state_->dormant.extract(key);
    if (node.empty()) {
      return false;
    }
    auto inserted = state_->active.insert(std::move(node));
    if (!inserted.inserted) {
      state_->dormant.insert(std::move(inserted.node));
      return false;
    }
    found->second.active = true;
    found->second.next_attempt = State::Clock::now();
    state_->condition.notify_all();
    return true;
  } catch (...) {
    return false;
  }
}

bool RetainedOwnerSupervisor::cancel_reservation(RetainedOwnerKey key) noexcept {
  try {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->entries.find(key);
    if (found == state_->entries.end() || found->second.active) {
      return false;
    }
    if (state_->dormant.erase(key) != 1) {
      return false;
    }
    state_->entries.erase(found);
    state_->condition.notify_all();
    return true;
  } catch (...) {
    return false;
  }
}

bool RetainedOwnerSupervisor::confirm_not_posted(RetainedOwnerKey key) {
  std::lock_guard lock(state_->mutex);
  const auto found = state_->entries.find(key);
  if (found == state_->entries.end() || !found->second.in_flight || found->second.submitting ||
      (!found->second.uncertain && !found->second.overdue_reported)) {
    return false;
  }
  auto &entry = found->second;
  entry.in_flight = false;
  entry.uncertain = false;
  entry.overdue_reported = false;
  entry.next_attempt = State::Clock::now();
  state_->condition.notify_all();
  return true;
}

RetainedOwnerSupervisor::Health RetainedOwnerSupervisor::health() const {
  std::lock_guard lock(state_->mutex);
  Health result;
  const auto now = State::Clock::now();
  result.retained = state_->entries.size();
  result.dormant = state_->dormant.size();
  result.alarm_delivery_failed = state_->alarm_delivery_failed;
  result.scheduler_stalled = now - state_->heartbeat >= state_->options.scheduler_stall_after;
  result.alarm_stalled = state_->active_alarms != 0 &&
                         now - state_->first_alarm_started >= state_->options.scheduler_stall_after;
  result.wait_failure_seen = state_->wait_failure_seen;
  for (const auto &key : state_->active) {
    const auto &entry = state_->entries.at(key);
    result.in_flight += entry.in_flight ? 1 : 0;
    result.uncertain += entry.uncertain ? 1 : 0;
    result.overdue += entry.in_flight && now - entry.started >= state_->options.overdue_after ? 1 : 0;
  }
  return result;
}

bool RetainedOwnerSupervisor::stop() {
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->entries.empty()) {
      return false;
    }
    state_->stopping = true;
  }
  state_->condition.notify_all();
  if (scheduler_.joinable()) {
    scheduler_.join();
  }
  if (watchdog_.joinable()) {
    watchdog_.join();
  }
  return true;
}

void RetainedOwnerSupervisor::inject_wait_failure_for_testing() {
  std::lock_guard lock(state_->mutex);
  state_->inject_wait_failure = true;
  state_->condition.notify_all();
}

} // namespace chronicles
