#pragma once

#include "persistence/character_state_types.hpp"

#include <nlohmann/json.hpp>
#include <optional>

namespace chronicles {

// Pursuit treats a target this close beyond its stopping distance as reached.
// Combat uses the same value so pursuit never settles outside attack reach.
inline constexpr double kNpcPursuitSettleToleranceMm = 1.0;

// Local policy, not a recovered official rule: an engaged monster that has not attacked, been
// hit or had its target within attack reach for this long gives up and walks home. Without it a
// player who outruns a monster stays hunted until the catalog leash (returnStartDist), which is
// 50 m for most placements and up to 500 m for some.
inline constexpr std::int64_t kNpcGiveUpWithoutContactMs = 10000;

enum class NpcAiState {
  Idle,
  Engaged,
  Returning,
};

struct NpcAiDefinition {
  std::int64_t search_circle_range_mm{};
  std::int64_t search_cone_range_mm{};
  double search_half_cone_angle{};
  std::int64_t hearing_range_mm{};
  // Npc.returnStartDist: neither the monster nor its target may be farther than this from the
  // monster's spawn point while it stays engaged.
  std::int64_t leash_range_mm{};
  std::int64_t reset_delay_ms{};
  std::int64_t combat_distance_min_mm{};
  std::int64_t combat_distance_max_mm{};
  double run_speed_mm_per_second{};
  int move_speed_rate{};

  static std::optional<NpcAiDefinition> from_json(const nlohmann::json &document);
};

struct NpcAiInput {
  WorldPosition player_position;
  std::int64_t ticks{};
  bool player_alive{true};
  bool monster_active{true};
  bool monster_alive{true};
  bool monster_hostile{true};
  bool action_active{};
  bool damaged{};
  // Monsters without an executable attack may retaliate when hit, but must not
  // proactively chase a player they cannot attack.
  bool perception_enabled{true};
  std::int64_t engagement_range_mm{};
  std::int64_t minimum_engagement_range_mm{};
};

struct NpcAiMotionStart {
  WorldPosition start_position;
  WorldPosition destination;
  std::int64_t end_tick{};
  // The target can be beyond the stopping radius; this is the actual endpoint.
  WorldPosition end_position;
};

struct NpcAiUpdate {
  bool acquired{};
  // Any loss of an engaged target, including death. The client receives the
  // captured aggro/AI-phase clears for this.
  bool aggro_released{};
  // A living, still-active monster disengaged and needs a new visible state.
  bool target_cleared{};
  bool cancel_action{};
  bool reset_health{};
  bool moved{};
  std::optional<NpcAiMotionStart> motion_start;
  std::optional<std::int64_t> motion_complete_tick;
};

// Transport-free tutorial AI. Calls and mutations are serialized by CombatSession.
class NpcAi {
public:
  NpcAi(NpcAiDefinition definition, WorldPosition home_position);

  NpcAiUpdate advance(const NpcAiInput &input);

  NpcAiState state() const;
  const WorldPosition &position() const;
  bool can_retarget(const WorldPosition &target) const;
  bool moving() const;
  // Stops a route and starts a fresh idle state at the new position (GM placement/pause).
  void reset_at(const WorldPosition &position);

  // Stop the current route at the interruption and resume thinking at the given deadline.
  void pause_until(std::int64_t ticks, std::int64_t resume_tick);

  // An early removal of the disabling status releases the scheduled pause.
  void resume_after_pause(std::int64_t ticks);

  // Moves the unit without a route (an attack lunge owned by the combat session). The caller
  // keeps the AI and the monster position identical.
  void place(const WorldPosition &position);
  // Authoritative position on the running route at the given tick, clamped to the route end; none
  // while no route runs. It is the same interpolation advance() uses, so it never leaves the path.
  std::optional<WorldPosition> route_position_at(std::int64_t ticks) const;
  // Endpoint of the running route; none while no route runs.
  std::optional<WorldPosition> route_end() const;
  const NpcAiDefinition &definition() const;
  std::optional<std::int64_t> next_action_tick() const;
  // True only when advance() for this tick and player map would return an empty update without
  // changing any AI state: Idle (not Engaged or Returning), a pending deadline that has not
  // arrived, and a player on the home map (so the reset branch cannot fire). The caller must
  // additionally know the monster is active, alive and hostile, which advance() also checks.
  bool idle_and_not_due(std::int64_t ticks, int player_map_id) const;

private:
  struct MotionPlan {
    NpcAiMotionStart start;
    std::int64_t start_tick{};
    std::int64_t stop_distance_mm{};
  };

  bool can_detect(const WorldPosition &player_position) const;
  std::int64_t engagement_range(const NpcAiInput &input) const;
  void record_contact(const NpcAiInput &input);
  bool should_disengage(const NpcAiInput &input) const;
  void begin_return(std::int64_t ticks, NpcAiUpdate &update);
  void start_motion(const WorldPosition &destination, std::int64_t stop_distance_mm,
                    std::int64_t ticks, NpcAiUpdate &update);
  void complete_motion(NpcAiUpdate &update,
                       std::optional<std::int64_t> interrupted_tick = std::nullopt);
  void advance_motion(std::int64_t ticks, NpcAiUpdate &update);
  void schedule(std::int64_t ticks, std::int64_t interval_ms);

  NpcAiDefinition definition_;
  WorldPosition home_position_;
  WorldPosition position_;
  NpcAiState state_{NpcAiState::Idle};
  std::optional<std::int64_t> next_action_tick_;
  std::optional<std::int64_t> last_advance_tick_;
  std::optional<std::int64_t> reset_tick_;
  // Last tick an engaged monster attacked, was hit or had its target within reach.
  std::optional<std::int64_t> last_contact_tick_;
  std::optional<MotionPlan> motion_;
};

} // namespace chronicles
