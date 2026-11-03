#include "world/core/world_presence.hpp"

#include "world/core/player_snapshots.hpp"
#include "world/core/unit_ids.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace chronicles {
namespace {

constexpr std::int64_t kSightEnterRadiusMm = 100000;
constexpr std::int64_t kSightLeaveRadiusMm = 120000;

static_assert(WorldPresence::kCellMm >= kSightLeaveRadiusMm,
              "the presence grid cell must cover the sight leave radius");

bool within_sight(const WorldPosition &first, const WorldPosition &second, std::int64_t radius_mm) {
  if (first.map_id != second.map_id) {
    return false;
  }

  const auto delta_x = static_cast<std::int64_t>(first.x_mm) - second.x_mm;
  const auto delta_y = static_cast<std::int64_t>(first.y_mm) - second.y_mm;
  const auto delta_z = static_cast<std::int64_t>(first.z_mm) - second.z_mm;
  if (std::abs(delta_x) > radius_mm || std::abs(delta_y) > radius_mm ||
      std::abs(delta_z) > radius_mm) {
    return false;
  }

  return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z <= radius_mm * radius_mm;
}

WorldPresenceEvent make_add_event(std::uint64_t recipient_connection_id,
                                  const WorldPresenceSnapshot &subject) {
  return {recipient_connection_id,
          WorldPresenceEventKind::add,
          {{"unit_id", std::to_string(player_wire_unit_id(subject.character.id))},
           {"nub", player_unit_nub(subject.character, subject.position, subject.player_stats,
                                   subject.dead)},
           {"by_sight", true}}};
}

WorldPresenceEvent make_remove_event(std::uint64_t recipient_connection_id,
                                     std::int64_t character_id) {
  return {recipient_connection_id,
          WorldPresenceEventKind::remove,
          {{"unit_ids", nlohmann::json::array({std::to_string(player_wire_unit_id(character_id))})},
           {"by_sight", true}}};
}

std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
  auto quotient = value / divisor;
  if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
    --quotient;
  }
  return quotient;
}

} // namespace

std::size_t WorldPresence::CellHash::operator()(const CellKey &key) const noexcept {
  std::uint64_t hash = static_cast<std::uint64_t>(static_cast<std::int64_t>(key.map_id));
  hash = hash * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(key.cx);
  hash = hash * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(key.cy);
  hash ^= hash >> 29;
  return static_cast<std::size_t>(hash);
}

WorldPresence::CellKey WorldPresence::cell_for(const WorldPosition &position) noexcept {
  return {position.map_id, floor_div(position.x_mm, kCellMm), floor_div(position.y_mm, kCellMm)};
}

void WorldPresence::index_insert(std::uint64_t connection_id, Participant &participant) {
  participant.cell = cell_for(participant.snapshot.position);
  auto &members = cells_[participant.cell];
  participant.cell_slot = members.size();
  members.push_back(connection_id);
}

void WorldPresence::index_remove(const Participant &participant) {
  const auto cell = cells_.find(participant.cell);
  if (cell == cells_.end()) {
    return;
  }

  auto &members = cell->second;
  const auto last = members.size() - 1;
  if (participant.cell_slot != last) {
    const auto moved_id = members.back();
    members[participant.cell_slot] = moved_id;
    participants_.at(moved_id).cell_slot = participant.cell_slot;
  }
  members.pop_back();
  if (members.empty()) {
    cells_.erase(cell);
  }
}

void WorldPresence::index_move(std::uint64_t connection_id, Participant &participant) {
  if (cell_for(participant.snapshot.position) == participant.cell) {
    return;
  }

  index_remove(participant);
  index_insert(connection_id, participant);
}

void WorldPresence::collect_neighbourhood(const CellKey &center, std::uint64_t skip_connection_id,
                                          const std::unordered_set<std::uint64_t> &already_listed,
                                          std::vector<std::uint64_t> &out) const {
  for (std::int64_t delta_x = -1; delta_x <= 1; ++delta_x) {
    for (std::int64_t delta_y = -1; delta_y <= 1; ++delta_y) {
      const auto cell = cells_.find({center.map_id, center.cx + delta_x, center.cy + delta_y});
      if (cell == cells_.end()) {
        continue;
      }
      for (const auto id : cell->second) {
        if (id != skip_connection_id && !already_listed.contains(id)) {
          out.push_back(id);
        }
      }
    }
  }
}

void WorldPresence::enter(std::uint64_t connection_id, WorldPresenceSnapshot snapshot,
                          std::vector<WorldPresenceEvent> &events) {
  // A character id is the player's unit id; ids at or above kFirstNpcUnitId belong to the server's
  // NPC units and must never be a player.
  if (!is_supported_character_id(snapshot.character.id)) {
    throw std::invalid_argument("World character ID is outside the supported unit range");
  }

  std::lock_guard lock(mutex_);
  if (participants_.contains(connection_id)) {
    throw std::runtime_error("World connection is already present");
  }
  if (connection_by_character_.contains(snapshot.character.id)) {
    throw std::runtime_error("World character is already connected");
  }

  const auto character_id = snapshot.character.id;
  auto [entry, inserted] =
      participants_.emplace(connection_id, Participant{std::move(snapshot), {}});
  if (!inserted) {
    throw std::runtime_error("World presence registration failed");
  }
  connection_by_character_.emplace(character_id, connection_id);
  index_insert(connection_id, entry->second);

  std::vector<std::uint64_t> candidates;
  collect_neighbourhood(entry->second.cell, connection_id, {}, candidates);
  for (const auto other_connection_id : candidates) {
    auto &other = participants_.at(other_connection_id);
    if (!within_sight(entry->second.snapshot.position, other.snapshot.position,
                      kSightEnterRadiusMm)) {
      continue;
    }

    entry->second.visible_peers.insert(other_connection_id);
    other.visible_peers.insert(connection_id);
    // Visibility stays symmetric for range tracking; only a hidden unit's own add is withheld.
    if (!other.snapshot.hidden) {
      events.push_back(make_add_event(connection_id, other.snapshot));
    }
    if (!entry->second.snapshot.hidden) {
      events.push_back(make_add_event(other_connection_id, entry->second.snapshot));
    }
  }
}

void WorldPresence::refresh_player_life(std::uint64_t connection_id, bool dead,
                                        const nlohmann::json &stats) {
  std::lock_guard lock(mutex_);
  const auto participant = participants_.find(connection_id);
  if (participant == participants_.end()) {
    return;
  }

  participant->second.snapshot.dead = dead;
  participant->second.snapshot.player_stats = stats;
}

void WorldPresence::refresh_equipment(std::uint64_t connection_id, const Character &character,
                                      const nlohmann::json *stats) {
  std::lock_guard lock(mutex_);
  const auto participant = participants_.find(connection_id);
  if (participant == participants_.end() ||
      participant->second.snapshot.character.id != character.id) {
    return;
  }
  auto &snapshot = participant->second.snapshot;
  snapshot.character.items = character.items;
  snapshot.character.build.active_weapon_slot = character.build.active_weapon_slot;
  if (stats) {
    snapshot.player_stats = *stats;
  }
}

std::vector<WorldPresenceEvent> WorldPresence::move(std::uint64_t connection_id,
                                                    const WorldPosition &position,
                                                    const nlohmann::json &movement) {
  std::vector<WorldPresenceEvent> events;
  // Cached lazily inside the loop; identical MoveNoti JSON for all peers.
  nlohmann::json move_notification;
  const world_metrics::ScopedTimer move_timer(world_metrics::HistogramId::PresenceMoveUs);
  std::lock_guard lock(mutex_);
  const auto mover = participants_.find(connection_id);
  if (mover == participants_.end() || mover->second.leaving) {
    return events;
  }

  mover->second.snapshot.position = position;
  index_move(connection_id, mover->second);

  // Every pair that can change state is either already visible (kept in visible_peers) or lies in
  // the 3x3 cells around the new position. The loop body erases from visible_peers, so the
  // candidates are snapshotted first; the buffer is reused to avoid a per-move allocation.
  thread_local std::vector<std::uint64_t> candidates;
  candidates.assign(mover->second.visible_peers.begin(), mover->second.visible_peers.end());
  collect_neighbourhood(mover->second.cell, connection_id, mover->second.visible_peers,
                        candidates);
  for (const auto other_connection_id : candidates) {
    const auto other_entry = participants_.find(other_connection_id);
    if (other_entry == participants_.end()) {
      continue;
    }
    auto &other = other_entry->second;
    if (other.leaving) {
      continue;
    }

    const bool visible = mover->second.visible_peers.contains(other_connection_id);
    const auto radius = visible ? kSightLeaveRadiusMm : kSightEnterRadiusMm;
    const bool in_range = within_sight(position, other.snapshot.position, radius);
    const bool mover_hidden = mover->second.snapshot.hidden;
    if (!visible && in_range) {
      mover->second.visible_peers.insert(other_connection_id);
      other.visible_peers.insert(connection_id);
      if (!other.snapshot.hidden) {
        events.push_back(make_add_event(connection_id, other.snapshot));
      }
      if (!mover_hidden) {
        events.push_back(make_add_event(other_connection_id, mover->second.snapshot));
      }
      continue;
    }
    if (visible && !in_range) {
      mover->second.visible_peers.erase(other_connection_id);
      other.visible_peers.erase(connection_id);
      if (!other.snapshot.hidden) {
        events.push_back(make_remove_event(connection_id, other.snapshot.character.id));
      }
      if (!mover_hidden) {
        events.push_back(
            make_remove_event(other_connection_id, mover->second.snapshot.character.id));
      }
      continue;
    }
    if (visible && !mover_hidden) {
      // Build the subject-derived movement body once; each observer event owns
      // its JSON value copy.
      if (move_notification.is_null()) {
        move_notification = movement;
        move_notification["unit_id"] =
            std::to_string(player_wire_unit_id(mover->second.snapshot.character.id));
      }
      events.push_back(
          {other_connection_id, WorldPresenceEventKind::move, move_notification});
    }
  }
  world_metrics::record(world_metrics::HistogramId::PresenceCandidates, candidates.size());
  world_metrics::record(world_metrics::HistogramId::PresenceEventsPerMove, events.size());
  return events;
}

std::vector<WorldPresenceEvent> WorldPresence::leave(std::uint64_t connection_id,
                                                     std::size_t maximum_events) {
  std::vector<WorldPresenceEvent> events;
  std::lock_guard lock(mutex_);
  const auto subject = participants_.find(connection_id);
  if (subject == participants_.end()) {
    return events;
  }

  if (subject->second.visible_peers.size() > maximum_events) {
    throw std::runtime_error("Presence leave exceeds its recipient limit");
  }
  events.reserve(subject->second.visible_peers.size());
  for (const auto peer_id : subject->second.visible_peers) {
    const auto peer = participants_.find(peer_id);
    if (peer == participants_.end()) {
      continue;
    }
    if (!subject->second.snapshot.hidden) {
      events.push_back(make_remove_event(peer_id, subject->second.snapshot.character.id));
    }
  }
  for (const auto peer_id : subject->second.visible_peers) {
    const auto peer = participants_.find(peer_id);
    if (peer != participants_.end()) {
      peer->second.visible_peers.erase(connection_id);
    }
  }
  connection_by_character_.erase(subject->second.snapshot.character.id);
  if (!subject->second.leaving) {
    index_remove(subject->second);
  }
  participants_.erase(subject);
  return events;
}

void WorldPresence::begin_leave(std::uint64_t connection_id) {
  std::lock_guard lock(mutex_);
  const auto subject = participants_.find(connection_id);
  if (subject == participants_.end() || subject->second.leaving) {
    return;
  }
  index_remove(subject->second);
  subject->second.leaving = true;
}

WorldPresenceLeavePage WorldPresence::leave_page(std::uint64_t connection_id,
                                                 std::size_t maximum_events,
    const std::function<void(const WorldPresenceEvent &)> &before_commit) {
  if (maximum_events == 0) {
    throw std::invalid_argument("Presence leave page must be nonempty");
  }
  std::lock_guard lock(mutex_);
  const auto subject = participants_.find(connection_id);
  if (subject == participants_.end()) {
    return {};
  }
  if (!subject->second.leaving) {
    throw std::logic_error("Presence leave was not started");
  }
  const auto count = std::min(maximum_events, subject->second.visible_peers.size());
  std::vector<std::uint64_t> selected;
  WorldPresenceLeavePage result;
  selected.reserve(count);
  result.events.reserve(count);
  for (const auto peer_id : subject->second.visible_peers) {
    if (selected.size() == count) {
      break;
    }
    selected.push_back(peer_id);
    if (participants_.contains(peer_id) && !subject->second.snapshot.hidden) {
      result.events.push_back(make_remove_event(peer_id, subject->second.snapshot.character.id));
    }
  }
  if (before_commit) {
    for (const auto &event : result.events) {
      before_commit(event);
    }
  }
  for (const auto peer_id : selected) {
    const auto peer = participants_.find(peer_id);
    if (peer != participants_.end()) {
      peer->second.visible_peers.erase(connection_id);
    }
    subject->second.visible_peers.erase(peer_id);
  }
  result.more = !subject->second.visible_peers.empty();
  if (!result.more) {
    connection_by_character_.erase(subject->second.snapshot.character.id);
    participants_.erase(subject);
  }
  return result;
}

std::vector<WorldPresenceEvent> WorldPresence::set_hidden(std::uint64_t connection_id,
                                                          bool hidden) {
  std::vector<WorldPresenceEvent> events;
  std::lock_guard lock(mutex_);
  const auto subject = participants_.find(connection_id);
  if (subject == participants_.end() || subject->second.leaving ||
      subject->second.snapshot.hidden == hidden) {
    return events;
  }

  subject->second.snapshot.hidden = hidden;
  for (const auto peer_id : subject->second.visible_peers) {
    if (hidden) {
      events.push_back(make_remove_event(peer_id, subject->second.snapshot.character.id));
    } else {
      events.push_back(make_add_event(peer_id, subject->second.snapshot));
    }
  }
  return events;
}

std::vector<std::uint64_t> WorldPresence::observers(std::uint64_t connection_id) {
  std::lock_guard lock(mutex_);
  const auto subject = participants_.find(connection_id);
  if (subject == participants_.end() || subject->second.leaving || subject->second.snapshot.hidden) {
    return {};
  }

  // Visibility is symmetric: every visible peer also sees this connection's unit.
  return {subject->second.visible_peers.begin(), subject->second.visible_peers.end()};
}

} // namespace chronicles
