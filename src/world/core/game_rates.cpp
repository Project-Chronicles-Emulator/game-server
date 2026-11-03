#include "world/core/game_rates.hpp"

#include "common/json_fields.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

constexpr std::int64_t kPercentDenominator = 100;

void validate_rate_percent(const char *field, int percent, int maximum) {
  if (percent < 0 || percent > maximum) {
    throw std::invalid_argument(std::string(field) + " must be between 0 and " +
                                std::to_string(maximum));
  }
}

void validate_event_text(const char *field, const std::string &value, std::size_t maximum,
                         bool required) {
  if ((required && value.empty()) || value.size() > maximum) {
    throw std::invalid_argument(std::string(field) + " must be " + (required ? "1" : "0") + "-" +
                                std::to_string(maximum) + " bytes");
  }

  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7F) {
      throw std::invalid_argument(std::string(field) + " must not contain control characters");
    }
  }
}

// Optional fields fall back to a default; type errors still throw.
template <typename Integer>
Integer integer_field(const nlohmann::json &document, const char *field, Integer fallback) {
  return json_fields::optional_integer<Integer>(document, field).value_or(fallback);
}

std::string string_field(const nlohmann::json &document, const char *field) {
  return json_fields::optional_string(document, field).value_or(std::string{});
}

bool bool_field(const nlohmann::json &document, const char *field, bool fallback) {
  return json_fields::optional_bool(document, field).value_or(fallback);
}

int effective_percent(int base_percent, const std::vector<ServerEvent> &events,
                      std::int64_t now_unix) {
  std::int64_t total = base_percent;
  for (const auto &event : events) {
    if (event.active_at(now_unix)) {
      total += event.experience_bonus_percent;
    }
  }
  return static_cast<int>(std::min<std::int64_t>(total, kMaximumRatePercent));
}

} // namespace

bool ServerEvent::active_at(std::int64_t now_unix) const {
  if (!enabled) {
    return false;
  }

  const bool started = starts_at_unix == 0 || now_unix >= starts_at_unix;
  const bool ended = ends_at_unix != 0 && now_unix >= ends_at_unix;
  return started && !ended;
}

void validate_server_event(const ServerEvent &event) {
  validate_event_text("name", event.name, kMaximumEventNameBytes, true);
  validate_event_text("description", event.description, kMaximumEventDescriptionBytes, false);
  validate_rate_percent("experience_bonus_percent", event.experience_bonus_percent,
                        kMaximumEventBonusPercent);
  if (event.starts_at_unix < 0 || event.ends_at_unix < 0) {
    throw std::invalid_argument("Event times must be Unix seconds or 0");
  }
  if (event.ends_at_unix != 0 && event.ends_at_unix <= event.starts_at_unix) {
    throw std::invalid_argument("Event end must be after its start");
  }
}

nlohmann::json server_event_json(const ServerEvent &event) {
  return {{"id", event.id},
          {"name", event.name},
          {"description", event.description},
          {"experience_bonus_percent", event.experience_bonus_percent},
          {"starts_at_unix", event.starts_at_unix},
          {"ends_at_unix", event.ends_at_unix},
          {"enabled", event.enabled}};
}

ServerEvent server_event_from_json(const nlohmann::json &document) {
  if (!document.is_object()) {
    throw std::invalid_argument("Event must be a JSON object");
  }

  ServerEvent event;
  event.id = integer_field<std::int64_t>(document, "id", 0);
  event.name = string_field(document, "name");
  event.description = string_field(document, "description");
  event.experience_bonus_percent = integer_field<int>(document, "experience_bonus_percent", 0);
  event.starts_at_unix = integer_field<std::int64_t>(document, "starts_at_unix", 0);
  event.ends_at_unix = integer_field<std::int64_t>(document, "ends_at_unix", 0);
  event.enabled = bool_field(document, "enabled", true);
  return event;
}

GameRatesSnapshot GameRates::snapshot() const {
  std::lock_guard lock(mutex_);
  return {base_experience_percent_, events_};
}

void GameRates::set_base_experience_percent(int percent) {
  validate_rate_percent("experience_rate_percent", percent, kMaximumRatePercent);

  std::lock_guard lock(mutex_);
  base_experience_percent_ = percent;
}

ServerEvent GameRates::add_event(ServerEvent event) {
  validate_server_event(event);

  std::lock_guard lock(mutex_);
  if (events_.size() >= kMaximumServerEvents) {
    throw std::invalid_argument("At most " + std::to_string(kMaximumServerEvents) +
                                " events can be scheduled");
  }
  event.id = next_event_id_++;
  events_.push_back(event);
  return event;
}

bool GameRates::update_event(const ServerEvent &event) {
  validate_server_event(event);

  std::lock_guard lock(mutex_);
  const auto existing = std::ranges::find_if(
      events_, [&event](const ServerEvent &candidate) { return candidate.id == event.id; });
  if (existing == events_.end()) {
    return false;
  }

  *existing = event;
  return true;
}

bool GameRates::remove_event(std::int64_t event_id) {
  std::lock_guard lock(mutex_);
  const auto removed =
      std::erase_if(events_, [event_id](const ServerEvent &event) { return event.id == event_id; });
  return removed > 0;
}

int GameRates::effective_experience_percent(std::int64_t now_unix) const {
  std::lock_guard lock(mutex_);
  return effective_percent(base_experience_percent_, events_, now_unix);
}

std::int64_t GameRates::scale_experience(std::int64_t amount, std::int64_t now_unix) const {
  if (amount <= 0) {
    return amount;
  }

  const auto percent = static_cast<std::int64_t>(effective_experience_percent(now_unix));
  if (percent == kPercentDenominator) {
    return amount;
  }

  // Divide first when the product could overflow; the rounding difference is irrelevant at
  // amounts that large.
  constexpr auto kLargest = std::numeric_limits<std::int64_t>::max();
  if (percent != 0 && amount > kLargest / percent) {
    const auto quotient = amount / kPercentDenominator;
    if (quotient > kLargest / percent) {
      return kLargest;
    }
    return quotient * percent;
  }
  return amount * percent / kPercentDenominator;
}

nlohmann::json GameRates::to_json() const {
  std::lock_guard lock(mutex_);

  auto events = nlohmann::json::array();
  for (const auto &event : events_) {
    events.push_back(server_event_json(event));
  }
  return {{"experience_rate_percent", base_experience_percent_}, {"events", std::move(events)}};
}

void GameRates::load_json(const nlohmann::json &document) {
  if (!document.is_object()) {
    throw std::invalid_argument("Rates must be a JSON object");
  }

  const auto base_percent =
      integer_field<int>(document, "experience_rate_percent", kDefaultRatePercent);
  validate_rate_percent("experience_rate_percent", base_percent, kMaximumRatePercent);

  std::vector<ServerEvent> events;
  std::int64_t largest_id = 0;
  const auto stored_events = document.find("events");
  if (stored_events != document.end()) {
    if (!stored_events->is_array() || stored_events->size() > kMaximumServerEvents) {
      throw std::invalid_argument("events must be an array of at most " +
                                  std::to_string(kMaximumServerEvents) + " entries");
    }
    for (const auto &entry : *stored_events) {
      auto event = server_event_from_json(entry);
      validate_server_event(event);
      if (event.id <= 0) {
        throw std::invalid_argument("Stored events need a positive id");
      }
      largest_id = std::max(largest_id, event.id);
      events.push_back(std::move(event));
    }
  }

  std::lock_guard lock(mutex_);
  base_experience_percent_ = base_percent;
  events_ = std::move(events);
  next_event_id_ = largest_id + 1;
}

GameRates &game_rates() {
  static GameRates rates;
  return rates;
}

std::int64_t current_unix_seconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

std::int64_t scale_experience_reward(std::int64_t amount) {
  return game_rates().scale_experience(amount, current_unix_seconds());
}

} // namespace chronicles
