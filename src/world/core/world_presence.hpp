#pragma once

#include "common/world_metrics.hpp"
#include "persistence/character_repository.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chronicles {

struct WorldPresenceSnapshot {
  Character character;
  WorldPosition position;
  nlohmann::json player_stats = nlohmann::json::object();
  // A hidden participant (an invisible game master) still sees others, but no other
  // participant receives its add or move events.
  bool hidden{};
  bool dead{};
};

enum class WorldPresenceEventKind { add, remove, move };

struct WorldPresenceEvent {
  std::uint64_t recipient_connection_id{};
  WorldPresenceEventKind kind{};
  nlohmann::json message;
};

struct WorldPresenceLeavePage {
  std::vector<WorldPresenceEvent> events;
  bool more{};
};

// A single map's interested clients. Only clients in the same map and within
// sight distance are announced to one another. The registry is thread-safe;
// callers deliver returned events on each recipient's own I/O executor.
class WorldPresence {
public:
  // Cell edge of the per-map candidate grid. It must cover the leave radius so that every pair
  // that can produce an event shares a cell or touches a neighbouring cell (static_assert in
  // world_presence.cpp).
  static constexpr std::int64_t kCellMm = 120000;

  void enter(std::uint64_t connection_id, WorldPresenceSnapshot snapshot,
             std::vector<WorldPresenceEvent> &events);

  // Refreshes later sight-entry snapshots without adding or removing the participant.
  void refresh_player_life(std::uint64_t connection_id, bool dead,
                           const nlohmann::json &stats);
  void refresh_equipment(std::uint64_t connection_id, const Character &character,
                         const nlohmann::json *stats);

  std::vector<WorldPresenceEvent> move(std::uint64_t connection_id, const WorldPosition &position,
                                       const nlohmann::json &movement);

  std::vector<WorldPresenceEvent> leave(
      std::uint64_t connection_id,
      std::size_t maximum_events = (std::numeric_limits<std::size_t>::max)());

  // Remove the subject from new sight-entry candidates before paging existing observers.
  // Each page is fully prepared before any of its visibility links are removed. The optional
  // staging callback runs under the presence mutex and must never call back into WorldPresence.
  void begin_leave(std::uint64_t connection_id);
  WorldPresenceLeavePage leave_page(
      std::uint64_t connection_id, std::size_t maximum_events,
      const std::function<void(const WorldPresenceEvent &)> &before_commit = {});

  // Hides or reveals a present participant. Hiding sends a remove event to every peer that
  // currently sees it; revealing sends add events to those peers. Empty when unknown/unchanged.
  std::vector<WorldPresenceEvent> set_hidden(std::uint64_t connection_id, bool hidden);

  // Connections that currently see this connection's unit; empty when unknown.
  std::vector<std::uint64_t> observers(std::uint64_t connection_id);

private:
  friend struct WorldPresenceTestAccess;

  struct CellKey {
    int map_id{};
    std::int64_t cx{};
    std::int64_t cy{};

    bool operator==(const CellKey &) const = default;
  };

  struct CellHash {
    std::size_t operator()(const CellKey &key) const noexcept;
  };

  struct Participant {
    WorldPresenceSnapshot snapshot;
    std::unordered_set<std::uint64_t> visible_peers;
    CellKey cell;
    std::size_t cell_slot{};
    bool leaving{};
  };

  static CellKey cell_for(const WorldPosition &position) noexcept;
  void index_insert(std::uint64_t connection_id, Participant &participant);
  void index_remove(const Participant &participant);
  void index_move(std::uint64_t connection_id, Participant &participant);
  // Appends the connection ids in the 3x3 cells around `center`, excluding `skip_connection_id`
  // and the ids for which `already_listed` returns true.
  void collect_neighbourhood(const CellKey &center, std::uint64_t skip_connection_id,
                             const std::unordered_set<std::uint64_t> &already_listed,
                             std::vector<std::uint64_t> &out) const;

  world_metrics::TimedMutex mutex_{world_metrics::HistogramId::PresenceMutexWaitUs,
                                   world_metrics::CounterId::PresenceMutexAcquisitions,
                                   world_metrics::CounterId::PresenceMutexContended,
                                   world_metrics::HistogramId::PresenceMutexHoldUs,
                                   world_metrics::CounterId::PresenceMutexHoldSumUs};
  std::unordered_map<std::uint64_t, Participant> participants_;
  std::unordered_map<CellKey, std::vector<std::uint64_t>, CellHash> cells_;
  std::unordered_map<std::int64_t, std::uint64_t> connection_by_character_;
};

} // namespace chronicles
