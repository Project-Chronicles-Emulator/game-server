#include "world/features/combat/movement_vitals.hpp"

#include "data/client_catalog.hpp"
#include "common/clock_ticks.hpp"
#include "world/features/combat/combat.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr std::int64_t kTicksPerSecond = 10000000;
constexpr int kMaximumCost = 1000000;
constexpr int kMaximumIntervalMs = 60000;
constexpr std::uint16_t kSprintEndNotification = 55;

void load_integer(ClientCatalog &catalog, std::string_view name, int &value, int minimum,
                  int maximum) {
  if (const auto loaded =
          game_variable_integer(catalog, name, minimum, maximum, "Movement vitals")) {
    value = static_cast<int>(*loaded);
  }
}

} // namespace

MovementVitalsRules MovementVitalsRules::load(ClientCatalog &catalog) {
  MovementVitalsRules rules;
  load_integer(catalog, "SprintConsumeSPPerTick", rules.sprint_cost, 0, kMaximumCost);
  load_integer(catalog, "SprintConsumeTick", rules.sprint_interval_ms, 1, kMaximumIntervalMs);
  load_integer(catalog, "SprintRequireSP", rules.sprint_required_stamina, 0, kMaximumCost);
  load_integer(catalog, "SwimConsumeSPPerTick", rules.swim_cost, 0, kMaximumCost);
  load_integer(catalog, "SwimSprintConsumeSPPerTick", rules.swim_sprint_cost, 0, kMaximumCost);
  load_integer(catalog, "SwimConsumeTick", rules.swim_interval_ms, 1, kMaximumIntervalMs);
  rules.validate();
  return rules;
}

void MovementVitalsRules::validate() const {
  if (sprint_cost < 0 || sprint_cost > kMaximumCost || swim_cost < 0 || swim_cost > kMaximumCost ||
      swim_sprint_cost < 0 || swim_sprint_cost > kMaximumCost || sprint_required_stamina < 0 ||
      sprint_required_stamina > kMaximumCost || sprint_interval_ms <= 0 ||
      sprint_interval_ms > kMaximumIntervalMs || swim_interval_ms <= 0 ||
      swim_interval_ms > kMaximumIntervalMs || submerged_damage_percent <= 0 ||
      submerged_damage_percent > 100) {
    throw std::invalid_argument("Invalid movement stamina or drowning rules");
  }
}

MovementVitals::MovementVitals(MovementVitalsRules rules) : rules_(rules) {
  rules_.validate();
}

void MovementVitals::reset() {
  activity_ = Activity::Rest;
  drain_elapsed_.fill(0);
  submerged_elapsed_ = 0;
  last_tick_.reset();
  map_id_.reset();
  submerged_ = false;
  sprint_requested_ = false;
  sprint_blocked_ = false;
  swimming_flow_.reset();
  swimming_flow_activity_ = Activity::Rest;
}

void MovementVitals::continue_on_map(int map_id, std::int64_t ticks) {
  validate_tick(ticks);
  map_id_ = map_id;
  if (last_tick_) {
    // The player cannot control movement during a map load. Retain accumulated partial
    // intervals but do not charge the time spent waiting for the new map.
    last_tick_ = ticks;
  }
}

bool MovementVitals::submerged() const {
  return submerged_;
}

std::optional<GameplayPacket> MovementVitals::stop_swimming(CombatSession &combat,
                                                            std::int64_t ticks) {
  if (!swimming_flow_) {
    return std::nullopt;
  }
  auto stopped = combat.end_swimming_stamina_flow(*swimming_flow_, ticks, false);
  swimming_flow_.reset();
  swimming_flow_activity_ = Activity::Rest;
  return stopped;
}

void MovementVitals::validate_tick(std::int64_t ticks) const {
  // Leave enough headroom for every bounded deadline and the nanosecond wire conversion.
  constexpr auto maximum_tick =
      std::numeric_limits<std::int64_t>::max() / 100 - kMaximumIntervalMs * kTicksPerMillisecond;
  if (ticks < 0 || ticks > maximum_tick || (last_tick_ && ticks < *last_tick_)) {
    throw std::invalid_argument("Invalid or decreasing server movement-vitals tick");
  }
}

std::int64_t MovementVitals::interval_ticks() const {
  const auto interval_ms =
      activity_ == Activity::Sprint ? rules_.sprint_interval_ms : rules_.swim_interval_ms;
  return static_cast<std::int64_t>(interval_ms) * kTicksPerMillisecond;
}

int MovementVitals::stamina_cost() const {
  switch (activity_) {
  case Activity::Sprint:
    return rules_.sprint_cost;
  case Activity::Swim:
    return rules_.swim_cost;
  case Activity::SwimSprint:
    return rules_.swim_sprint_cost;
  default:
    return 0;
  }
}

void MovementVitals::update_recovery(CombatSession &combat, std::int64_t ticks) const {
  combat.set_movement_recovery_blocked(activity_ != Activity::Rest, submerged_, ticks);
}

std::vector<GameplayPacket> MovementVitals::update_swimming_flow(CombatSession &combat,
                                                                 std::int64_t ticks) {
  std::vector<GameplayPacket> packets;
  const bool swimming = activity_ == Activity::Swim || activity_ == Activity::SwimSprint;
  const bool has_stamina = combat.player_stats().value("72", 0) > 0;
  const bool unlimited = combat.movement_stamina_unlimited();
  if (swimming_flow_ &&
      (!swimming || unlimited || (has_stamina && swimming_flow_activity_ != activity_))) {
    packets.push_back(combat.end_swimming_stamina_flow(*swimming_flow_, ticks,
                                                        activity_ != Activity::Rest && !unlimited));
    swimming_flow_.reset();
    swimming_flow_activity_ = Activity::Rest;
  }
  if (swimming && !unlimited && !swimming_flow_ && stamina_cost() > 0) {
    const double rate = -static_cast<double>(stamina_cost()) * 1000.0 / rules_.swim_interval_ms;
    auto started = combat.begin_swimming_stamina_flow(rate, ticks);
    if (started) {
      swimming_flow_ = started->first;
      swimming_flow_activity_ = activity_;
      packets.push_back(std::move(started->second));
    }
  }
  return packets;
}

GameplayPacket MovementVitals::sprint_end(const CombatSession &combat, std::int64_t ticks) const {
  return {kSprintEndNotification,
          {{"sprint_failed", true},
           {"stamina", combat.player_stats().value("72", 0)},
           {"tick", wire_monotonic_tick(ticks)}}};
}

std::vector<GameplayPacket> MovementVitals::move(const nlohmann::json &movement,
                                                 const WorldPosition &position,
                                                 CombatSession &combat, std::int64_t ticks) {
  validate_tick(ticks);
  std::vector<GameplayPacket> packets;
  if (map_id_ && *map_id_ != position.map_id) {
    if (auto stopped = stop_swimming(combat, ticks)) {
      packets.push_back(std::move(*stopped));
    }
    reset();
    combat.set_movement_recovery_blocked(false, false, ticks);
  }
  map_id_ = position.map_id;
  const auto mode = movement.value("mode", std::string("MoveMode_Normal"));
  const auto detail = movement.value("detail", std::string("MoveDetail_Stop"));
  const bool swimming = mode == "MoveMode_Swimming";
  const bool surfaced = !swimming || detail == "MoveDetail_SurfaceIdle";
  if (surfaced) {
    // A queued, late timer cannot charge old underwater time after accepted surfacing.
    submerged_ = false;
    submerged_elapsed_ = 0;
  }
  auto elapsed_packets = advance_to(position, combat, ticks, !surfaced);
  packets.insert(packets.end(), std::make_move_iterator(elapsed_packets.begin()),
                 std::make_move_iterator(elapsed_packets.end()));
  if (combat.player_dead()) {
    return packets;
  }

  if (swimming && detail == "MoveDetail_SW_Dive") {
    submerged_ = true;
  }
  Activity requested = Activity::Rest;
  if (swimming && detail != "MoveDetail_SurfaceIdle") {
    requested = detail == "MoveDetail_Sprint" ? Activity::SwimSprint : Activity::Swim;
  } else if (mode == "MoveMode_Normal" && detail == "MoveDetail_Sprint") {
    requested = Activity::Sprint;
  }
  const bool requests_sprint = requested == Activity::Sprint || requested == Activity::SwimSprint;
  if (!requests_sprint) {
    sprint_blocked_ = false;
  } else if (!sprint_blocked_ && (!sprint_requested_ || (requested == Activity::Sprint &&
                                                         activity_ != Activity::Sprint))) {
    const int required = requested == Activity::Sprint ? rules_.sprint_required_stamina : 1;
    if (combat.player_stats().value("72", 0) < required) {
      sprint_blocked_ = true;
      packets.push_back(sprint_end(combat, ticks));
    }
  }
  sprint_requested_ = requests_sprint;
  activity_ = requested;
  if (sprint_blocked_) {
    activity_ = swimming ? Activity::Swim : Activity::Rest;
  }
  update_recovery(combat, ticks);
  auto flow_packets = update_swimming_flow(combat, ticks);
  packets.insert(packets.end(), std::make_move_iterator(flow_packets.begin()),
                 std::make_move_iterator(flow_packets.end()));
  return packets;
}

std::vector<GameplayPacket> MovementVitals::advance(const WorldPosition &position,
                                                    CombatSession &combat, std::int64_t ticks) {
  return advance_to(position, combat, ticks, true);
}

std::vector<GameplayPacket> MovementVitals::advance_to(const WorldPosition &position,
                                                       CombatSession &combat, std::int64_t ticks,
                                                       bool apply_drowning) {
  validate_tick(ticks);
  std::vector<GameplayPacket> packets;
  if (map_id_ && *map_id_ != position.map_id) {
    if (auto stopped = stop_swimming(combat, ticks)) {
      packets.push_back(std::move(*stopped));
    }
    reset();
    combat.set_movement_recovery_blocked(false, false, ticks);
  }
  map_id_ = position.map_id;
  if (combat.player_dead()) {
    if (auto stopped = stop_swimming(combat, ticks)) {
      packets.push_back(std::move(*stopped));
    }
    reset();
    combat.set_movement_recovery_blocked(false, false, ticks);
    return packets;
  }
  const auto elapsed = last_tick_ ? ticks - *last_tick_ : 0;
  last_tick_ = ticks;
  const int cost = stamina_cost();
  if (cost > 0) {
    auto &accrued = drain_elapsed_[static_cast<std::size_t>(activity_)];
    accrued += elapsed;
    const auto interval = interval_ticks();
    const auto cycles = accrued / interval;
    accrued %= interval;
    if (cycles > 0) {
      const auto available = combat.player_stats().value("72", 0);
      const auto bounded_cycles = std::min<std::int64_t>(cycles, available / cost + 1);
      const int spent = static_cast<int>(std::min<std::int64_t>(available, bounded_cycles * cost));
      const auto source = activity_ == Activity::Sprint ? "SCS_SprintFlow" : "SCS_SwimFlow";
      auto changes = combat.change_movement_stamina(-spent, source, ticks, !swimming_flow_);
      packets.insert(packets.end(), std::make_move_iterator(changes.begin()),
                     std::make_move_iterator(changes.end()));
      if (combat.player_stats().value("72", 0) == 0 && sprint_requested_ && !sprint_blocked_) {
        sprint_blocked_ = true;
        packets.push_back(sprint_end(combat, ticks));
        activity_ = activity_ == Activity::Sprint ? Activity::Rest : Activity::Swim;
      }
    }
  }
  if (submerged_ && apply_drowning) {
    submerged_elapsed_ += elapsed;
    const auto cycles = submerged_elapsed_ / kTicksPerSecond;
    submerged_elapsed_ %= kTicksPerSecond;
    if (cycles > 0) {
      const auto maximum_health = combat.player_stats().value("18", 0);
      const auto health = combat.player_stats().value("69", 0);
      const auto damage_per_tick = std::max<std::int64_t>(
          1,
          (static_cast<std::int64_t>(maximum_health) * rules_.submerged_damage_percent + 99) / 100);
      const auto bounded_cycles = std::min<std::int64_t>(cycles, health / damage_per_tick + 1);
      const auto damage =
          static_cast<int>(std::min<std::int64_t>(health, bounded_cycles * damage_per_tick));
      auto impact = combat.apply_drowning_damage(damage, position, ticks);
      packets.insert(packets.end(), std::make_move_iterator(impact.begin()),
                     std::make_move_iterator(impact.end()));
    }
  }
  if (combat.player_dead()) {
    if (auto stopped = stop_swimming(combat, ticks)) {
      packets.push_back(std::move(*stopped));
    }
    reset();
  }
  update_recovery(combat, ticks);
  auto flow_packets = update_swimming_flow(combat, ticks);
  packets.insert(packets.end(), std::make_move_iterator(flow_packets.begin()),
                 std::make_move_iterator(flow_packets.end()));
  return packets;
}

std::optional<std::int64_t> MovementVitals::next_action_tick() const {
  if (!last_tick_) {
    return std::nullopt;
  }
  std::optional<std::int64_t> deadline;
  if (stamina_cost() > 0) {
    deadline = *last_tick_ + interval_ticks() - drain_elapsed_[static_cast<std::size_t>(activity_)];
  }
  if (submerged_) {
    const auto drowning = *last_tick_ + kTicksPerSecond - submerged_elapsed_;
    if (!deadline || drowning < *deadline) {
      deadline = drowning;
    }
  }
  return deadline;
}

} // namespace chronicles
