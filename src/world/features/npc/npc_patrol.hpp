#pragma once

#include "protocol/gameplay_packet.hpp"
#include "world/core/unit_registry.hpp"
#include "world/features/npc/npc_reactions.hpp"
#include "world/features/npc/npc_sight.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chronicles {

class NpcPopulation;

struct NpcPatrolSample {
  UnitPositionMm position;
  float yaw{};
  bool moving{};
  std::string move_detail;
  std::int64_t phase_id{};
  std::int64_t phase_started{};
  std::int64_t remaining_ticks{};
  std::optional<int> stance;
  nlohmann::json motion;
};

// An immutable, closed authored route. Sampling a common elapsed clock makes a late joiner
// see the same worker as an existing player without copying a capture or running another AI.
class NpcPatrolRoute {
public:
  static NpcPatrolRoute from_json(const nlohmann::json &document);

  // The out-parameter overload lets a wake-up reuse one sample buffer instead of
  // copying a fresh NpcPatrolSample (with its json motion) per call.
  void sample(std::int64_t elapsed_ticks, NpcPatrolSample &out) const;
  NpcPatrolSample sample(std::int64_t elapsed_ticks) const;

private:
  NpcPatrolRoute() = default;

  struct Phase {
    UnitPositionMm from;
    UnitPositionMm to;
    float yaw{};
    bool moving{};
    std::string move_detail;
    std::int64_t starts{};
    std::int64_t ends{};
    std::optional<int> stance;
    nlohmann::json motion;
  };

  std::vector<Phase> phases_;
  std::int64_t cycle_ticks_{};
};

// Worker-strand state for delivering one shared route timeline to an entered session.
// It owns no rewards or NPC definitions and never accepts client movement for these units.
class NpcPatrolSession {
public:
  NpcPatrolSession(const NpcPopulation &population, std::int64_t epoch_tick,
                   npc::ReactionTimeline timeline = {});

  std::vector<GameplayPacket> start(std::int64_t ticks, UnitRegistry &registry,
                                    std::string_view instance_key,
                                    const NpcSightTracker *sight = nullptr);
  std::vector<GameplayPacket> advance(std::int64_t ticks, UnitRegistry &registry,
                                      std::string_view instance_key,
                                      const NpcSightTracker *sight = nullptr);
  void synchronize(std::int64_t ticks, UnitRegistry &registry, std::string_view instance_key) const;
  std::optional<std::int64_t> next_action_tick() const;

  // Brings an already scheduled wake-up forward, for example when a reaction pauses a route
  // now. The next advance then emits the stop and reaction together. A session that has not
  // started has no schedule and is left unchanged.
  void wake_at(std::int64_t ticks);

  // Per-advance accounting for the patrol hot path; read-only, no behavior role.
  struct Stats {
    std::int64_t route_samples{};
    std::int64_t position_updates{};
    std::int64_t packets_built{};
    std::int64_t packets_dropped_out_of_sight{};
  };

  const Stats &stats() const { return stats_; }

private:
  const NpcPopulation &population_;
  std::int64_t epoch_tick_{};
  npc::ReactionTimeline timeline_;
  bool started_{};
  std::optional<std::int64_t> next_tick_;
  std::unordered_map<std::uint64_t, std::int64_t> last_phases_;
  std::unordered_set<std::uint64_t> stopped_units_;
  std::unordered_map<std::uint64_t, std::int64_t> next_updates_;
  Stats stats_;
};

} // namespace chronicles
