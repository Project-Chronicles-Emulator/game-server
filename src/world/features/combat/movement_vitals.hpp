#pragma once

#include "protocol/gameplay_packet.hpp"
#include "persistence/character_state_types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace chronicles {

class ClientCatalog;
class CombatSession;

struct MovementVitalsRules {
  int sprint_cost{3};
  int sprint_interval_ms{600};
  int sprint_required_stamina{50};
  int swim_cost{10};
  int swim_sprint_cost{20};
  int swim_interval_ms{1000};
  // Explicit local policy: immediate periodic damage, without an invented oxygen resource.
  int submerged_damage_percent{5};

  static MovementVitalsRules load(ClientCatalog &catalog);
  void validate() const;
};

// One accepted movement stream, advanced on server time even without further packets.
// SW_Dive enters underwater state; SurfaceIdle or leaving Swimming ends it. Other
// swimming details retain that state, so stopping or swimming upward is not surfacing.
class MovementVitals {
public:
  explicit MovementVitals(MovementVitalsRules rules = {});

  std::vector<GameplayPacket> move(const nlohmann::json &movement, const WorldPosition &position,
                                   CombatSession &combat, std::int64_t ticks);
  std::vector<GameplayPacket> advance(const WorldPosition &position, CombatSession &combat,
                                      std::int64_t ticks);
  std::optional<std::int64_t> next_action_tick() const;
  std::optional<GameplayPacket> stop_swimming(CombatSession &combat, std::int64_t ticks);
  void continue_on_map(int map_id, std::int64_t ticks);
  void reset();
  bool submerged() const;

private:
  enum class Activity { Rest, Sprint, Swim, SwimSprint };

  std::vector<GameplayPacket> advance_to(const WorldPosition &position, CombatSession &combat,
                                         std::int64_t ticks, bool apply_drowning);
  void validate_tick(std::int64_t ticks) const;
  void update_recovery(CombatSession &combat, std::int64_t ticks) const;
  std::vector<GameplayPacket> update_swimming_flow(CombatSession &combat, std::int64_t ticks);
  GameplayPacket sprint_end(const CombatSession &combat, std::int64_t ticks) const;
  std::int64_t interval_ticks() const;
  int stamina_cost() const;

  MovementVitalsRules rules_;
  Activity activity_{Activity::Rest};
  // Preserve fractional drain intervals across toggles; repeated movement cannot reset cost.
  std::array<std::int64_t, 4> drain_elapsed_{};
  std::int64_t submerged_elapsed_{};
  std::optional<std::int64_t> last_tick_;
  std::optional<int> map_id_;
  bool submerged_{};
  bool sprint_requested_{};
  bool sprint_blocked_{};
  std::optional<std::uint64_t> swimming_flow_;
  Activity swimming_flow_activity_{Activity::Rest};
};

} // namespace chronicles
