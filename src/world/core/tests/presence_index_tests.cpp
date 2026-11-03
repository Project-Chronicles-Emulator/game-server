// Differential tests of the WorldPresence spatial index against the previous full-scan
// implementation, kept here as the oracle.
#include "world/core/world_presence.hpp"
#include "world/core/unit_ids.hpp"
#include "common/world_metrics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chronicles {

// Friend of WorldPresence: checks that the grid, the character index and the stored cells agree.
struct WorldPresenceTestAccess {
  static void connect_star(WorldPresence &presence, std::uint64_t subject,
                           std::uint64_t first, std::uint64_t count) {
    auto &owner = presence.participants_.at(subject);
    for (std::uint64_t index = 0; index < count; ++index) {
      const auto peer = first + index;
      owner.visible_peers.insert(peer);
      presence.participants_.at(peer).visible_peers.insert(subject);
    }
  }

  static std::size_t remaining_peers(const WorldPresence &presence, std::uint64_t subject) {
    return presence.participants_.at(subject).visible_peers.size();
  }

  static bool consistent(const WorldPresence &presence) {
    std::size_t indexed = 0;
    for (const auto &[key, members] : presence.cells_) {
      if (members.empty()) {
        return false;
      }
      for (std::size_t slot = 0; slot < members.size(); ++slot) {
        const auto found = presence.participants_.find(members[slot]);
        if (found == presence.participants_.end() || found->second.cell_slot != slot ||
            !(found->second.cell == key) ||
            !(WorldPresence::cell_for(found->second.snapshot.position) == key)) {
          return false;
        }
        ++indexed;
      }
    }
    if (indexed != presence.participants_.size() ||
        presence.connection_by_character_.size() != presence.participants_.size()) {
      return false;
    }
    for (const auto &[connection_id, participant] : presence.participants_) {
      const auto character = presence.connection_by_character_.find(participant.snapshot.character.id);
      if (character == presence.connection_by_character_.end() ||
          character->second != connection_id) {
        return false;
      }
    }
    return true;
  }
};

namespace {

constexpr std::int64_t kEnterMm = 100000;
constexpr std::int64_t kLeaveMm = 120000;

bool reference_within_sight(const WorldPosition &first, const WorldPosition &second,
                            std::int64_t radius) {
  if (first.map_id != second.map_id) {
    return false;
  }
  const auto dx = static_cast<std::int64_t>(first.x_mm) - second.x_mm;
  const auto dy = static_cast<std::int64_t>(first.y_mm) - second.y_mm;
  const auto dz = static_cast<std::int64_t>(first.z_mm) - second.z_mm;
  // Same per-axis early exit as production: int32 extremes make dx*dx overflow int64 otherwise.
  if (std::abs(dx) > radius || std::abs(dy) > radius || std::abs(dz) > radius) {
    return false;
  }
  return dx * dx + dy * dy + dz * dz <= radius * radius;
}

// The pre-index algorithm: every participant is a candidate.
class ReferencePresence {
public:
  void enter(std::uint64_t id, WorldPresenceSnapshot snapshot, std::vector<WorldPresenceEvent> &events) {
    if (participants_.contains(id)) {
      throw std::runtime_error("World connection is already present");
    }
    for (const auto &[other_id, participant] : participants_) {
      if (participant.snapshot.character.id == snapshot.character.id) {
        throw std::runtime_error("World character is already connected");
      }
    }
    auto &entry = participants_.emplace(id, Participant{std::move(snapshot), {}}).first->second;
    for (auto &[other_id, other] : participants_) {
      if (other_id == id ||
          !reference_within_sight(entry.snapshot.position, other.snapshot.position, kEnterMm)) {
        continue;
      }
      entry.visible.insert(other_id);
      other.visible.insert(id);
      if (!other.snapshot.hidden) {
        events.push_back(add(id, other.snapshot));
      }
      if (!entry.snapshot.hidden) {
        events.push_back(add(other_id, entry.snapshot));
      }
    }
  }

  std::vector<WorldPresenceEvent> move(std::uint64_t id, const WorldPosition &position,
                                       const nlohmann::json &movement) {
    std::vector<WorldPresenceEvent> events;
    const auto mover = participants_.find(id);
    if (mover == participants_.end()) {
      return events;
    }
    mover->second.snapshot.position = position;
    for (auto &[other_id, other] : participants_) {
      if (other_id == id) {
        continue;
      }
      const bool visible = mover->second.visible.contains(other_id);
      const bool in_range = reference_within_sight(position, other.snapshot.position,
                                                   visible ? kLeaveMm : kEnterMm);
      const bool hidden = mover->second.snapshot.hidden;
      if (!visible && in_range) {
        mover->second.visible.insert(other_id);
        other.visible.insert(id);
        if (!other.snapshot.hidden) {
          events.push_back(add(id, other.snapshot));
        }
        if (!hidden) {
          events.push_back(add(other_id, mover->second.snapshot));
        }
      } else if (visible && !in_range) {
        mover->second.visible.erase(other_id);
        other.visible.erase(id);
        if (!other.snapshot.hidden) {
          events.push_back(remove(id, other.snapshot.character.id));
        }
        if (!hidden) {
          events.push_back(remove(other_id, mover->second.snapshot.character.id));
        }
      } else if (visible && !hidden) {
        auto notification = movement;
        notification["unit_id"] =
            std::to_string(player_wire_unit_id(mover->second.snapshot.character.id));
        events.push_back({other_id, WorldPresenceEventKind::move, notification});
      }
    }
    return events;
  }

  std::vector<WorldPresenceEvent> leave(std::uint64_t id) {
    std::vector<WorldPresenceEvent> events;
    const auto subject = participants_.find(id);
    if (subject == participants_.end()) {
      return events;
    }
    for (const auto peer_id : subject->second.visible) {
      participants_.at(peer_id).visible.erase(id);
      if (!subject->second.snapshot.hidden) {
        events.push_back(remove(peer_id, subject->second.snapshot.character.id));
      }
    }
    participants_.erase(subject);
    return events;
  }

  std::vector<WorldPresenceEvent> set_hidden(std::uint64_t id, bool hidden) {
    std::vector<WorldPresenceEvent> events;
    const auto subject = participants_.find(id);
    if (subject == participants_.end() || subject->second.snapshot.hidden == hidden) {
      return events;
    }
    subject->second.snapshot.hidden = hidden;
    for (const auto peer_id : subject->second.visible) {
      events.push_back(hidden ? remove(peer_id, subject->second.snapshot.character.id)
                              : add(peer_id, subject->second.snapshot));
    }
    return events;
  }

  std::vector<std::uint64_t> observers(std::uint64_t id) {
    const auto subject = participants_.find(id);
    if (subject == participants_.end() || subject->second.snapshot.hidden) {
      return {};
    }
    return {subject->second.visible.begin(), subject->second.visible.end()};
  }

private:
  struct Participant {
    WorldPresenceSnapshot snapshot;
    std::unordered_set<std::uint64_t> visible;
  };

  static WorldPresenceEvent add(std::uint64_t recipient, const WorldPresenceSnapshot &subject) {
    return {recipient,
            WorldPresenceEventKind::add,
            {{"unit_id", std::to_string(player_wire_unit_id(subject.character.id))},
             {"nub",
              {{"pos",
                {{"millimeter_x", subject.position.x_mm},
                 {"millimeter_y", subject.position.y_mm},
                 {"millimeter_z", subject.position.z_mm}}}}},
             {"by_sight", true}}};
  }

  static WorldPresenceEvent remove(std::uint64_t recipient, std::int64_t character_id) {
    return {recipient,
            WorldPresenceEventKind::remove,
            {{"unit_ids", nlohmann::json::array({std::to_string(player_wire_unit_id(character_id))})},
             {"by_sight", true}}};
  }

  std::unordered_map<std::uint64_t, Participant> participants_;
};

using EventKey = std::tuple<std::uint64_t, int, std::string>;

std::vector<EventKey> canonical(const std::vector<WorldPresenceEvent> &events) {
  std::vector<EventKey> keys;
  for (const auto &event : events) {
    // The oracle cannot rebuild the full nub (internal to native_world.cpp), so an add event is
    // compared by recipient, unit id and announced position.
    auto message = event.message;
    if (event.kind == WorldPresenceEventKind::add) {
      message = {{"unit_id", event.message.at("unit_id")},
                 {"pos", event.message.at("nub").at("pos")}};
    }
    keys.emplace_back(event.recipient_connection_id, static_cast<int>(event.kind), message.dump());
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::vector<std::uint64_t> sorted(std::vector<std::uint64_t> ids) {
  std::sort(ids.begin(), ids.end());
  return ids;
}

Character character(std::int64_t id) {
  Character result;
  result.id = id;
  result.class_id = 2;
  result.level = 1;
  result.name = "P" + std::to_string(id);
  return result;
}

// Runs both implementations side by side and fails at the first observable difference.
class Pair {
public:
  void enter(std::uint64_t id, std::int64_t character_id, const WorldPosition &position,
             bool hidden = false) {
    WorldPresenceSnapshot snapshot{character(character_id), position};
    snapshot.hidden = hidden;
    std::vector<WorldPresenceEvent> actual;
    std::vector<WorldPresenceEvent> expected;
    std::string actual_error;
    std::string expected_error;
    try {
      index_.enter(id, snapshot, actual);
    } catch (const std::exception &error) {
      actual_error = error.what();
    }
    try {
      oracle_.enter(id, snapshot, expected);
    } catch (const std::exception &error) {
      expected_error = error.what();
    }
    ASSERT_EQ(actual_error, expected_error);
    ASSERT_EQ(canonical(actual), canonical(expected));
    check_all(id);
  }

  void move(std::uint64_t id, const WorldPosition &position) {
    const nlohmann::json movement = {{"unit_id", "x"}, {"tick", 7}};
    ASSERT_EQ(canonical(index_.move(id, position, movement)),
              canonical(oracle_.move(id, position, movement)));
    check_all(id);
  }

  void leave(std::uint64_t id) {
    ASSERT_EQ(canonical(index_.leave(id)), canonical(oracle_.leave(id)));
    check_all(id);
  }

  void set_hidden(std::uint64_t id, bool hidden) {
    ASSERT_EQ(canonical(index_.set_hidden(id, hidden)), canonical(oracle_.set_hidden(id, hidden)));
    check_all(id);
  }

  void check_all(std::uint64_t probe) {
    ASSERT_TRUE(WorldPresenceTestAccess::consistent(index_));
    ASSERT_EQ(sorted(index_.observers(probe)), sorted(oracle_.observers(probe)));
    for (std::uint64_t id = 1; id <= 64; ++id) {
      ASSERT_EQ(sorted(index_.observers(id)), sorted(oracle_.observers(id))) << id;
    }
  }

  WorldPresence index_;
  ReferencePresence oracle_;
};

WorldPosition at(int map, int x, int y, int z = 0) {
  return WorldPosition{map, x, y, z, 0.0F};
}

TEST(WorldPresenceIndex, BoundedLeaveFailurePreservesAllVisibilityForRetry) {
  WorldPresence presence;
  std::vector<WorldPresenceEvent> events;
  presence.enter(1, {character(1), at(8000, 0, 0)}, events);
  presence.enter(2, {character(2), at(8000, 0, 0)}, events);
  presence.enter(3, {character(3), at(8000, 0, 0)}, events);
  EXPECT_THROW(presence.leave(1, 1), std::runtime_error);
  EXPECT_EQ(presence.observers(1).size(), 2U);
  EXPECT_EQ(presence.observers(2).size(), 2U);
  const auto removed = presence.leave(1, 2);
  EXPECT_EQ(removed.size(), 2U);
  EXPECT_TRUE(presence.observers(1).empty());
  EXPECT_EQ(presence.observers(2).size(), 1U);
}

TEST(WorldPresenceIndex, PagedLeaveKeepsAllRecipientsAcrossFailureAndNewEntry) {
  WorldPresence presence;
  std::vector<WorldPresenceEvent> events;
  presence.enter(1, {character(1), at(8000, 0, 0)}, events);
  constexpr std::uint64_t kPeers = 4097;
  for (std::uint64_t index = 0; index < kPeers; ++index) {
    const auto id = index + 2;
    presence.enter(id, {character(static_cast<std::int64_t>(id)),
                        at(8000, static_cast<int>((index + 1) * 250000), 0)}, events);
  }
  WorldPresenceTestAccess::connect_star(presence, 1, 2, kPeers);
  presence.begin_leave(1);
  events.clear();
  presence.enter(kPeers + 2, {character(static_cast<std::int64_t>(kPeers + 2)),
                             at(8000, 0, 0)}, events);
  EXPECT_TRUE(events.empty());

  EXPECT_THROW(presence.leave_page(1, 4096, [](const WorldPresenceEvent &) {
    throw std::bad_alloc();
  }), std::bad_alloc);
  EXPECT_EQ(WorldPresenceTestAccess::remaining_peers(presence, 1), kPeers);
  const auto first = presence.leave_page(1, 4096);
  EXPECT_EQ(first.events.size(), 4096U);
  EXPECT_TRUE(first.more);
  const auto last = presence.leave_page(1, 4096);
  EXPECT_EQ(last.events.size(), 1U);
  EXPECT_FALSE(last.more);
  EXPECT_TRUE(presence.observers(1).empty());
}

TEST(PresenceIndex, CellEdgeCoversTheLeaveRadius) {
  static_assert(WorldPresence::kCellMm >= kLeaveMm);
  SUCCEED();
}

TEST(PresenceIndex, HysteresisAndCellBoundaryCrossingMatchTheOracle) {
  // A peer exactly on a cell edge; the mover walks across cell edges on every axis and diagonal.
  Pair pair;
  pair.enter(1, 1, at(8000, 0, 0));
  pair.enter(2, 2, at(8000, 119999, 119999));
  for (const int x : {99999, 100000, 100001, 119999, 120000, 120001, 110000, 99999, -1, -120001}) {
    pair.move(2, at(8000, x, 0));
    pair.move(2, at(8000, x, x));
    pair.move(1, at(8000, -x, 119999 - x));
  }
}

// Kills a stale grid cell after a move: the mover jumps several cells, then a peer appears within
// the enter radius of the NEW position but outside the 3x3 cells around the OLD one. The index
// finds that peer only through the mover's current cell.
TEST(PresenceIndex, PeersNearAMoversNewPositionAreFoundAfterItJumpedSeveralCells) {
  const auto observers_of = [](Pair &pair, std::uint64_t id) {
    return sorted(pair.index_.observers(id));
  };

  // Peer enters after the jump.
  Pair entering;
  entering.enter(1, 1, at(8000, 0, 0));
  entering.move(1, at(8000, 500000, 500000));
  entering.enter(2, 2, at(8000, 550000, 500000));
  EXPECT_EQ(observers_of(entering, 1), (std::vector<std::uint64_t>{2}));
  EXPECT_EQ(observers_of(entering, 2), (std::vector<std::uint64_t>{1}));
  // A second jump across negative cells, again onto a peer far from every earlier cell.
  entering.move(1, at(8000, -700001, -600000));
  entering.enter(3, 3, at(8000, -740000, -600000));
  EXPECT_EQ(observers_of(entering, 1), (std::vector<std::uint64_t>{3}));

  // Peer is already there and the mover jumps next to it.
  Pair moving;
  moving.enter(1, 1, at(8000, 0, 0));
  moving.enter(2, 2, at(8000, 900000, -900000));
  moving.move(1, at(8000, 870000, -900000));
  EXPECT_EQ(observers_of(moving, 1), (std::vector<std::uint64_t>{2}));
  // The peer jumps away and back next to the mover through different cells.
  moving.move(2, at(8000, -900000, 900000));
  EXPECT_TRUE(observers_of(moving, 1).empty());
  moving.move(2, at(8000, 880000, -870000));
  EXPECT_EQ(observers_of(moving, 1), (std::vector<std::uint64_t>{2}));
  // Remove events after both left the old cells: the leave radius still applies.
  moving.move(1, at(8000, 0, 0));
  EXPECT_TRUE(observers_of(moving, 1).empty());
}

TEST(PresenceIndex, BoundaryAndExtremeCoordinatesMatchTheOracle) {
  Pair pair;
  std::vector<int> coordinates{std::numeric_limits<int>::min(), std::numeric_limits<int>::max()};
  for (int k = -3; k <= 3; ++k) {
    for (const int delta : {-1, 0, 1}) {
      coordinates.push_back(k * 120000 + delta);
      coordinates.push_back(k * 100000 + delta);
    }
  }
  std::uint64_t id = 1;
  for (const int x : coordinates) {
    if (id > 40) {
      break;
    }
    pair.enter(id, static_cast<std::int64_t>(id), at(8000, x, x / 2, (id % 3) == 0 ? 200000 : 0));
    ++id;
  }
  for (std::uint64_t mover = 1; mover < id; ++mover) {
    for (const int x : coordinates) {
      // Negating INT_MIN is undefined behaviour; map it to the other extreme instead.
      const int y = x == std::numeric_limits<int>::min() ? std::numeric_limits<int>::max() : -x;
      pair.move(mover, at(8000, x, y));
    }
  }
}

TEST(PresenceIndex, MapChangesAndHousingMapsMatchTheOracle) {
  Pair pair;
  pair.enter(1, 1, at(8000, 0, 0));
  pair.enter(2, 2, at(8000, 1000, 0));
  pair.enter(3, 3, at(-12, 0, 0));
  pair.move(2, at(-12, 500, 0));
  pair.move(2, at(8000, 500, 0));
  pair.move(1, at(-12, 0, 100000));
  pair.leave(3);
  pair.move(1, at(8000, 0, 0));
}

TEST(PresenceIndex, DuplicateConnectionAndCharacterThrowLikeTheOracle) {
  Pair pair;
  pair.enter(1, 1, at(8000, 0, 0));
  pair.enter(1, 2, at(8000, 0, 0));
  pair.enter(2, 1, at(8000, 0, 0));
  pair.leave(1);
  pair.enter(2, 1, at(8000, 0, 0));
}

TEST(PresenceIndex, RandomOperationSequencesMatchTheOracleAndKeepTheIndexConsistent) {
  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    std::mt19937_64 random(seed);
    Pair pair;
    const auto draw = [&random]() {
      const std::int64_t cluster = static_cast<std::int64_t>(random() % 4) * 130000;
      switch (random() % 6) {
      case 0:
        return at(8000, static_cast<int>(random() % 600000) - 300000,
                  static_cast<int>(random() % 600000) - 300000);
      case 1:
        return at(-12, static_cast<int>(random() % 200000), static_cast<int>(random() % 200000));
      case 2: {
        const int k = static_cast<int>(random() % 7) - 3;
        return at(8000, k * 120000 + static_cast<int>(random() % 3) - 1,
                  k * 100000 + static_cast<int>(random() % 3) - 1);
      }
      default:
        return at(8000, static_cast<int>(cluster + static_cast<std::int64_t>(random() % 60000)),
                  static_cast<int>(random() % 60000), static_cast<int>(random() % 3) * 50000);
      }
    };
    // A third of the positions land within 90 m of a position handed out earlier, which is usually
    // many cells away from where the chosen connection currently is: the jump that exposes a
    // stale grid cell (the other 5/6 cases rarely put a peer near the new position).
    std::vector<WorldPosition> recent;
    const auto position = [&random, &recent, &draw]() {
      WorldPosition result = draw();
      if (!recent.empty() && random() % 3 == 0) {
        result = recent[random() % recent.size()];
        result.x_mm += static_cast<int>(random() % 180001) - 90000;
        result.y_mm += static_cast<int>(random() % 180001) - 90000;
      }
      recent.push_back(result);
      if (recent.size() > 48) {
        recent.erase(recent.begin());
      }
      return result;
    };
    for (int step = 0; step < 300 && !::testing::Test::HasFailure(); ++step) {
      const std::uint64_t id = random() % 40 + 1;
      switch (random() % 10) {
      case 0:
      case 1:
        pair.enter(id, static_cast<std::int64_t>(random() % 48 + 1), position(), random() % 8 == 0);
        break;
      case 2:
        pair.leave(id);
        break;
      case 3:
        pair.set_hidden(id, random() % 2 == 0);
        break;
      default:
        pair.move(id, position());
        break;
      }
    }
    ASSERT_FALSE(::testing::Test::HasFailure()) << "seed " << seed;
  }
}

TEST(PresenceIndex, PresenceMutexContentionIsCountedSeparatelyFromTheProtocolMutex) {
  world_metrics::enable();
  const auto acquisitions = [] {
    const auto document = nlohmann::json::parse(world_metrics::snapshot_json());
    return std::make_pair(
        document.at("counters").at("presence_mutex_acquisitions").get<std::uint64_t>(),
        document.at("counters").at("protocol_mutex_acquisitions").get<std::uint64_t>());
  };
  const auto before = acquisitions();
  WorldPresence presence;
  std::vector<WorldPresenceEvent> events;
  presence.enter(1, {character(1), at(8000, 0, 0)}, events);
  presence.move(1, at(8000, 10, 0), nlohmann::json::object());
  presence.leave(1);
  const auto after = acquisitions();
  EXPECT_GE(after.first, before.first + 3);
  EXPECT_EQ(after.second, before.second);
}

} // namespace
} // namespace chronicles
