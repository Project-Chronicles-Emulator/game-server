#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

// Operator-controlled reward rates (emulator policy, not a recovered official service).
// Percentages: 100 means the catalog amount, 200 doubles it. The defaults leave every reward
// unchanged, so code and tests that never configure rates keep their exact amounts.
inline constexpr int kDefaultRatePercent = 100;
inline constexpr int kMaximumRatePercent = 1000;
inline constexpr int kMaximumEventBonusPercent = 1000;
inline constexpr std::size_t kMaximumServerEvents = 64;
inline constexpr std::size_t kMaximumEventNameBytes = 64;
inline constexpr std::size_t kMaximumEventDescriptionBytes = 256;

// A scheduled bonus. While enabled and inside [starts_at, ends_at), its bonus is added to the
// base rate: base 100 plus one +100 event yields 200 (2x).
struct ServerEvent {
  std::int64_t id{};
  std::string name;
  std::string description;
  int experience_bonus_percent{};
  // Unix seconds. Zero start means "already started"; zero end means "no end".
  std::int64_t starts_at_unix{};
  std::int64_t ends_at_unix{};
  bool enabled{true};

  bool active_at(std::int64_t now_unix) const;
};

struct GameRatesSnapshot {
  int base_experience_percent{kDefaultRatePercent};
  std::vector<ServerEvent> events;
};

class GameRates {
public:
  GameRatesSnapshot snapshot() const;

  // Throws std::invalid_argument outside 0..kMaximumRatePercent.
  void set_base_experience_percent(int percent);

  // Validates and stores a new event with a fresh ID; returns the stored copy.
  ServerEvent add_event(ServerEvent event);

  // Replaces the event with the same ID. Returns false when it does not exist.
  bool update_event(const ServerEvent &event);

  bool remove_event(std::int64_t event_id);

  // Base rate plus every active event bonus, capped at kMaximumRatePercent.
  int effective_experience_percent(std::int64_t now_unix) const;

  // Scales a server-derived EXP amount by the effective rate. Saturates instead of
  // overflowing; a zero or negative amount is returned unchanged.
  std::int64_t scale_experience(std::int64_t amount, std::int64_t now_unix) const;

  nlohmann::json to_json() const;

  // Replaces every rate and event. Throws std::invalid_argument for an invalid document and
  // leaves the current values unchanged.
  void load_json(const nlohmann::json &document);

private:
  mutable std::mutex mutex_;
  int base_experience_percent_{kDefaultRatePercent};
  std::vector<ServerEvent> events_;
  std::int64_t next_event_id_{1};
};

// Throws std::invalid_argument when an event field is outside its accepted range.
void validate_server_event(const ServerEvent &event);

nlohmann::json server_event_json(const ServerEvent &event);

// Parses the operator-facing JSON shape; `id` is taken from the document when present.
ServerEvent server_event_from_json(const nlohmann::json &document);

// The server process's rates. Reward paths (tutorial quests and adventure objects) consult
// this one instance, and the operator console edits it. It is process-wide because the rate
// is one operator policy for the whole server, and threading it through every session
// constructor would couple unrelated gameplay modules to the admin service.
GameRates &game_rates();

// Scales a server-derived EXP reward with game_rates() at the current wall-clock time.
std::int64_t scale_experience_reward(std::int64_t amount);

std::int64_t current_unix_seconds();

} // namespace chronicles
