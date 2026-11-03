#include "world/features/companion/companion_world.hpp"

#include "world/features/npc/npc_population.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace chronicles::companion {
namespace {

using Json = nlohmann::json;

constexpr char kMaxHealthStat[] = "18";
constexpr int kHealthStatType = 69;

// EX5StatType_MoveSpeedRate in per mille. The client scales a ridden X5Character's speed by
// this stat (AAC_Unit_Candidate_RefreshMoveSpeedFromStat), so mounts carry the same neutral
// rate the NPC population announces.
constexpr char kMoveSpeedRateStat[] = "84";
constexpr int kNeutralMoveSpeedRate = 1000;
constexpr char kIdleState[] = "EX5UnitStateType_Idle";
constexpr char kPlayerFaction[] = "EX5Faction_Player";
constexpr char kTownNpcFaction[] = "EX5Faction_TownNpc";
constexpr char kStandByState[] = "EX5SummonStateType_StandBy";

// Same three-dimensional sphere test player presence uses.
bool within_radius(const WorldPosition &first, const WorldPosition &second,
                   std::int64_t radius_mm) {
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

Json world_pos(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

std::string decimal(std::uint64_t value) {
  return std::to_string(value);
}

const char *dismount_reason_name(DismountReason reason) {
  switch (reason) {
  case DismountReason::user:
    return "EX5DismountReason_User";
  case DismountReason::rider_removed:
    return "EX5DismountReason_RiderRemoved";
  case DismountReason::rider_killed:
    return "EX5DismountReason_RiderKilled";
  case DismountReason::mount_removed:
    return "EX5DismountReason_MountRemoved";
  case DismountReason::mount_broken:
    return "EX5DismountReason_MountBroken";
  case DismountReason::clear_trade:
    return "EX5DismountReason_ClearTrade";
  }
  return "EX5DismountReason_None";
}

std::shared_ptr<CompanionDetachBudget> default_detach_budget() {
  static auto budget = std::make_shared<CompanionDetachBudget>();
  return budget;
}

} // namespace

class CompanionDetachBudget::Lease final {
public:
  Lease(std::shared_ptr<CompanionDetachBudget> budget, std::size_t bytes)
      : budget_(std::move(budget)), bytes_(bytes) {}

  ~Lease() {
    budget_->used_bytes_.fetch_sub(bytes_);
  }

private:
  std::shared_ptr<CompanionDetachBudget> budget_;
  std::size_t bytes_{};
};

CompanionDetachBudget::CompanionDetachBudget(std::size_t limit_bytes) : limit_bytes_(limit_bytes) {
  if (limit_bytes == 0 || limit_bytes > kDefaultLimitBytes) {
    throw std::invalid_argument("Invalid companion detach budget");
  }
}

std::shared_ptr<void> CompanionDetachBudget::reserve(std::size_t bytes) {
  if (bytes == 0 || bytes > limit_bytes_) {
    return {};
  }
  auto used = used_bytes_.load();
  do {
    if (used > limit_bytes_ - bytes) {
      return {};
    }
  } while (!used_bytes_.compare_exchange_weak(used, used + bytes));

  try {
    return std::make_shared<Lease>(shared_from_this(), bytes);
  } catch (...) {
    used_bytes_.fetch_sub(bytes);
    throw;
  }
}

std::size_t CompanionDetachBudget::used_bytes() const noexcept {
  return used_bytes_.load();
}

void CompanionUpdate::append(CompanionUpdate other) {
  packets.insert(packets.end(), std::make_move_iterator(other.packets.begin()),
                 std::make_move_iterator(other.packets.end()));
}

std::optional<std::size_t> checked_companion_seat_count(int authored_count) noexcept {
  if (authored_count > static_cast<int>(kMaximumCompanionSeats)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(std::max(authored_count, 1));
}

CompanionWorld::CompanionWorld(std::shared_ptr<const CompanionCatalog> catalog,
                               CompanionWorldConfig config,
                               std::shared_ptr<CompanionDetachBudget> detach_budget)
    : catalog_(std::move(catalog)),
      detach_budget_(detach_budget ? std::move(detach_budget) : default_detach_budget()),
      config_(config) {
  if (!catalog_) {
    throw std::invalid_argument("CompanionWorld requires a companion catalog");
  }
  if (config_.sight_enter_radius_mm <= 0 ||
      config_.sight_leave_radius_mm < config_.sight_enter_radius_mm ||
      config_.board_range_mm <= 0) {
    throw std::invalid_argument("Invalid CompanionWorld radii");
  }
}

CompanionUpdate CompanionWorld::attach(const Participant &participant) {
  if (!is_supported_character_id(participant.character_id)) {
    throw std::invalid_argument("Companion participant has an unsupported character id");
  }

  std::lock_guard lock(mutex_);
  if (members_.contains(participant.connection_id)) {
    throw std::runtime_error("Companion participant is already attached");
  }

  auto &member = members_[participant.connection_id];
  member.participant = participant;
  CompanionUpdate update;
  refresh_member_visibility(member, update);
  return update;
}

CompanionUpdate CompanionWorld::move(std::uint64_t connection_id, const WorldPosition &position,
                                     const nlohmann::json *movement) {
  std::lock_guard lock(mutex_);
  CompanionUpdate update;
  const auto member = members_.find(connection_id);
  if (member == members_.end()) {
    return update;
  }

  member->second.participant.position = position;
  if (member->second.riding) {
    const auto unit = units_.find(member->second.riding->first);
    if (unit != units_.end()) {
      unit->second.position = position;
      if (movement && movement->is_object()) {
        Json notification = *movement;
        notification["unit_id"] = decimal(unit->second.unit_id);
        for (const auto &[observer, relationship] : unit->second.observers) {
          static_cast<void>(relationship);
          if (observer != connection_id) {
            update.packets.push_back({observer, kMoveNotification, notification,
                                      source_binding(unit->second, observer, false)});
          }
        }
      }
      refresh_unit_visibility(unit->second, update);
    }
  }

  refresh_member_visibility(member->second, update);
  return update;
}

CompanionUpdate CompanionWorld::set_channel(std::uint64_t connection_id,
                                            std::uint64_t channel_key) {
  std::lock_guard lock(mutex_);
  CompanionUpdate update;
  const auto member = members_.find(connection_id);
  if (member == members_.end() || member->second.participant.channel_key == channel_key) {
    return update;
  }

  member->second.participant.channel_key = channel_key;
  if (member->second.riding) {
    const auto unit = units_.find(member->second.riding->first);
    if (unit != units_.end()) {
      unit->second.channel_key = channel_key;
      refresh_unit_visibility(unit->second, update);
    }
  }
  refresh_member_visibility(member->second, update);
  return update;
}

CompanionUpdate CompanionWorld::reenter_region(std::uint64_t connection_id,
                                                const WorldPosition &position,
                                                std::int64_t tick,
                                                const CompanionViewToken &view) {
  std::lock_guard lock(mutex_);
  CompanionUpdate update;
  const auto member = members_.find(connection_id);
  if (member == members_.end()) {
    return update;
  }

  member->second.region_suspended = false;
  member->second.participant.position = position;
  member->second.participant.view = view;
  if (member->second.riding) {
    const auto unit = units_.find(member->second.riding->first);
    if (unit != units_.end()) {
      unit->second.region_suspended = false;
      unit->second.position = position;
      refresh_unit_visibility(unit->second, update);
    }
  }
  refresh_member_visibility(member->second, update);

  // The map load discards the client's unit actors even when server visibility did not change.
  // Remove newly queued adds for this client, then send one current snapshot for each unit.
  std::erase_if(update.packets, [connection_id](const auto &packet) {
    return packet.connection_id == connection_id && packet.opcode == kUnitAddNotification;
  });
  for (auto &[unit_id, unit] : units_) {
    if (unit.observers.contains(connection_id)) {
      auto &relationship = unit.observers.at(connection_id);
      if (next_observer_generation_ == (std::numeric_limits<std::uint64_t>::max)()) {
        throw std::overflow_error("Companion observer generation exhausted");
      }
      relationship = {view, next_observer_generation_++, view.epoch == 0};
      update.packets.push_back({connection_id, kUnitAddNotification, unit_add_message(unit),
                                source_binding(unit, connection_id, true)});
    }
  }
  if (member->second.riding) {
    const auto [unit_id, slot] = *member->second.riding;
    const auto unit = units_.find(unit_id);
    if (unit != units_.end() && unit->second.observers.contains(connection_id)) {
      update.packets.push_back({connection_id, kMountUnitNotification,
                                mount_message(unit->second, connection_id, slot, tick),
                                source_binding(unit->second, connection_id, false)});
    }
  }
  return update;
}

CompanionUpdate CompanionWorld::suspend_region(std::uint64_t connection_id,
                                                const WorldPosition &destination) {
  std::lock_guard lock(mutex_);
  CompanionUpdate update;
  const auto member = members_.find(connection_id);
  if (member == members_.end()) {
    return update;
  }

  member->second.region_suspended = true;
  member->second.participant.position = destination;
  if (member->second.riding) {
    const auto unit = units_.find(member->second.riding->first);
    if (unit != units_.end()) {
      unit->second.region_suspended = true;
      unit->second.position = destination;
      refresh_unit_visibility(unit->second, update);
    }
  }
  refresh_member_visibility(member->second, update);
  return update;
}

DetachResult CompanionWorld::detach(std::uint64_t connection_id) {
  return detach_staged(connection_id, {});
}

DetachResult CompanionWorld::detach_staged(
    std::uint64_t connection_id,
    const std::function<void(const std::vector<VehicleSnapshot> &)> &before_commit) {
  std::lock_guard lock(mutex_);
  DetachResult result;
  const auto member = members_.find(connection_id);
  if (member == members_.end()) {
    return result;
  }

  std::vector<std::uint64_t> owned;
  for (const auto &[unit_id, unit] : units_) {
    if (unit.owner_connection_id == connection_id) {
      owned.push_back(unit_id);
    }
  }
  // Construct every packet and vehicle snapshot before changing the world. The callback
  // can retain the wagon plan without allowing a concurrent movement or damage update.
  for (const auto unit_id : owned) {
    const auto &unit = units_.at(unit_id);
    if (unit.kind == UnitKind::vehicle) {
      result.vehicles.push_back(snapshot(unit));
    }
    for (const auto seat : unit.seats) {
      if (seat == 0) {
        continue;
      }
      const auto message = dismount_message(unit, seat, DismountReason::rider_removed,
                                            unit.position, 0);
      for (const auto &[observer, relationship] : unit.observers) {
        static_cast<void>(relationship);
        if (observer != connection_id) {
          result.update.packets.push_back({observer, kMountUnitNotification, message});
        }
      }
    }
    const Json removed{{"unit_ids", Json::array({decimal(unit_id)})}};
    for (const auto &[observer, relationship] : unit.observers) {
      static_cast<void>(relationship);
      if (observer != connection_id) {
        result.update.packets.push_back({observer, kUnitRemoveNotification, removed});
      }
    }
  }
  if (before_commit) {
    before_commit(result.vehicles);
  }

  // All potentially allocating work is complete. Erases and optional resets do not allocate.
  for (const auto unit_id : owned) {
    const auto unit = units_.find(unit_id);
    for (const auto seat : unit->second.seats) {
      const auto rider = members_.find(seat);
      if (seat != 0 && rider != members_.end()) {
        rider->second.riding.reset();
      }
    }
    units_.erase(unit);
  }

  // A rider of another member's unit simply vacates the seat.
  if (member->second.riding) {
    const auto ridden = units_.find(member->second.riding->first);
    if (ridden != units_.end()) {
      for (auto &seat : ridden->second.seats) {
        if (seat == connection_id) {
          seat = 0;
        }
      }
    }
  }

  for (auto &[unit_id, unit] : units_) {
    unit.observers.erase(connection_id);
  }
  members_.erase(member);
  return result;
}

DetachResult CompanionWorld::detach_retained(
    std::uint64_t connection_id,
    const std::function<void(const std::vector<VehicleSnapshot> &,
                             const std::shared_ptr<RetainedDetach> &)> &before_commit) {
  std::lock_guard lock(mutex_);
  DetachResult result;
  const auto member = members_.find(connection_id);
  if (member == members_.end()) {
    return result;
  }

  constexpr std::size_t kMaximumRetainedUnits = 4096;
  constexpr std::size_t kBaseConstructionBytes = 16 * 1024;
  constexpr std::size_t kBytesPerSource = 8192;
  constexpr std::size_t kBytesPerSeat = 4096;
  constexpr std::size_t kBytesPerObserver = 256;
  std::size_t owned_count = 0;
  std::size_t estimated_bytes = kBaseConstructionBytes;
  const auto charge = [&](std::size_t count, std::size_t bytes_each) {
    if (count > (CompanionDetachBudget::kDefaultLimitBytes - estimated_bytes) / bytes_each) {
      throw std::runtime_error("Companion detach exceeds its construction budget");
    }
    estimated_bytes += count * bytes_each;
  };
  for (const auto &[unit_id, unit] : units_) {
    if (unit.owner_connection_id != connection_id) {
      continue;
    }
    if (++owned_count > kMaximumRetainedUnits || unit.seats.size() > kMaximumCompanionSeats) {
      throw std::runtime_error("Companion detach exceeds its retained source limit");
    }
    charge(1, kBytesPerSource);
    charge(unit.seats.size(), kBytesPerSeat);
    charge(unit.observers.size(), kBytesPerObserver);
  }
  auto budget_lease = detach_budget_->reserve(estimated_bytes);
  if (!budget_lease) {
    throw std::runtime_error("Companion detach construction budget is exhausted");
  }

  std::vector<std::uint64_t> owned;
  owned.reserve(owned_count);
  for (const auto &[unit_id, unit] : units_) {
    if (unit.owner_connection_id == connection_id) {
      owned.push_back(unit_id);
    }
  }
  auto retained = std::make_shared<RetainedDetach>();
  retained->budget_lease_ = std::move(budget_lease);
  retained->owner_connection_ = connection_id;
  retained->units_.reserve(owned.size());
  result.vehicles.reserve(owned.size());
  for (const auto unit_id : owned) {
    const auto &unit = units_.at(unit_id);
    if (unit.kind == UnitKind::vehicle) {
      result.vehicles.push_back(snapshot(unit));
    }
    auto packets = std::make_shared<std::vector<GameplayPacket>>();
    packets->reserve(unit.seats.size() + 1);
    for (const auto seat : unit.seats) {
      if (seat != 0) {
        packets->push_back({kMountUnitNotification,
                            dismount_message(unit, seat, DismountReason::rider_removed,
                                             unit.position, 0)});
      }
    }
    packets->push_back({kUnitRemoveNotification,
                        {{"unit_ids", Json::array({decimal(unit_id)})}}});
    retained->units_.push_back({unit_id, {}, std::move(packets)});
  }
  if (before_commit) {
    before_commit(result.vehicles, retained);
  }

  for (std::size_t index = 0; index < owned.size(); ++index) {
    const auto unit = units_.find(owned[index]);
    for (const auto seat : unit->second.seats) {
      const auto rider = members_.find(seat);
      if (seat != 0 && rider != members_.end()) {
        rider->second.riding.reset();
      }
    }
    retained->units_[index].observers.swap(unit->second.observers);
    units_.erase(unit);
  }
  if (member->second.riding) {
    const auto ridden = units_.find(member->second.riding->first);
    if (ridden != units_.end()) {
      for (auto &seat : ridden->second.seats) {
        if (seat == connection_id) {
          seat = 0;
        }
      }
    }
  }
  for (auto &[unit_id, unit] : units_) {
    unit.observers.erase(connection_id);
  }
  members_.erase(member);
  retained->committed_ = true;
  return result;
}

CompanionDetachPage CompanionWorld::RetainedDetach::page(CompanionDetachCursor cursor,
                                                           std::size_t limit) const {
  constexpr std::size_t kMaximumPageSize = 4096;
  if (!committed_ || limit == 0 || limit > kMaximumPageSize || cursor.unit > units_.size()) {
    throw std::invalid_argument("Invalid retained companion detach page");
  }
  CompanionDetachPage result;
  result.next = cursor;
  result.recipients.reserve(limit);
  while (result.next.unit < units_.size()) {
    const auto &unit = units_[result.next.unit];
    auto observer = unit.observers.upper_bound(result.next.connection_id);
    while (observer != unit.observers.end() && result.recipients.size() < limit) {
      result.next.connection_id = observer->first;
      if (observer->first != owner_connection_ && observer->second.ready) {
        CompanionSourceBinding source{observer->first, unit.unit_id, observer->second.generation,
                                      observer->second.view, false, true};
        result.recipients.push_back({observer->first, observer->second.view, source, unit.packets});
      }
      ++observer;
    }
    if (observer != unit.observers.end()) {
      return result;
    }
    ++result.next.unit;
    result.next.connection_id = 0;
    // A page addresses only one source unit, so recipients cannot occur twice.
    if (!result.recipients.empty()) {
      break;
    }
  }
  result.complete = result.next.unit == units_.size();
  return result;
}

std::optional<std::uint64_t> CompanionWorld::ridden_unit(std::uint64_t connection_id) const {
  std::lock_guard lock(mutex_);
  const auto member = members_.find(connection_id);
  if (member == members_.end() || !member->second.riding) {
    return std::nullopt;
  }
  return member->second.riding->first;
}

SpawnResult CompanionWorld::spawn_mount(std::uint64_t connection_id, int summon_npc_cms_id,
                                        std::int64_t tick) {
  std::lock_guard lock(mutex_);
  SpawnResult result;
  const auto *summon = catalog_->summon(summon_npc_cms_id);
  if (!summon) {
    result.error = x5::ec::Summon_InvalidSummonNpcCmsId;
    return result;
  }
  if (!summon->rideable) {
    result.error = x5::ec::Summon_MountRestricted;
    return result;
  }

  const auto member = members_.find(connection_id);
  if (member == members_.end()) {
    result.error = x5::ec::InvalidState;
    return result;
  }
  if (member->second.riding) {
    result.error = x5::ec::Summon_AlreadyRiding;
    return result;
  }

  // One mount per owner: a previous, unridden mount is replaced.
  if (const auto previous = owned_unit(connection_id, UnitKind::mount)) {
    remove_unit(*previous, result.update);
  }

  Unit unit;
  unit.unit_id = allocate_unit_id(UnitKind::mount);
  unit.kind = UnitKind::mount;
  unit.owner_character_id = member->second.participant.character_id;
  unit.owner_connection_id = connection_id;
  unit.channel_key = member->second.participant.channel_key;
  unit.position = member->second.participant.position;
  unit.npc_cms_id = summon->npc_cms_id;
  unit.summon_npc_cms_id = summon->id;
  unit.seats.assign(1, connection_id);

  auto &stored = units_.emplace(unit.unit_id, std::move(unit)).first->second;
  member->second.riding = std::pair{stored.unit_id, 0U};
  // The client retries seating from MountNub.passengers when asynchronous actor loading
  // finishes. Publish the committed seat in the first nub as well as MountUnitNoti.
  announce(stored, result.update);
  send_to_audience(stored, kMountUnitNotification, mount_message(stored, connection_id, 0, tick),
                   result.update);
  result.unit_id = stored.unit_id;
  return result;
}

SpawnResult CompanionWorld::spawn_vehicle(std::uint64_t connection_id, int vehicle_cms_id,
                                          const WorldPosition &position, int health,
                                          bool seat_owner, std::int64_t tick) {
  std::lock_guard lock(mutex_);
  SpawnResult result;
  const auto *vehicle = catalog_->vehicle(vehicle_cms_id);
  if (!vehicle) {
    result.error = x5::ec::Trade_InvalidWagon;
    return result;
  }
  const auto seat_count = checked_companion_seat_count(vehicle->seat_count);
  if (!seat_count) {
    result.error = x5::ec::Trade_WagonSpawnFailed;
    return result;
  }

  const auto member = members_.find(connection_id);
  if (member == members_.end()) {
    result.error = x5::ec::InvalidState;
    return result;
  }
  if (seat_owner && member->second.riding) {
    result.error = x5::ec::Summon_AlreadyRiding;
    return result;
  }
  if (owned_unit(connection_id, UnitKind::vehicle)) {
    result.error = x5::ec::Trade_WagonSpawnFailed;
    return result;
  }

  Unit unit;
  unit.unit_id = allocate_unit_id(UnitKind::vehicle);
  unit.kind = UnitKind::vehicle;
  unit.owner_character_id = member->second.participant.character_id;
  unit.owner_connection_id = connection_id;
  unit.channel_key = member->second.participant.channel_key;
  unit.position = position;
  unit.vehicle_cms_id = vehicle->id;
  unit.max_health = vehicle->max_health;
  unit.health = std::clamp(health, 1, vehicle->max_health);
  unit.seats.assign(*seat_count, 0);
  if (seat_owner) {
    unit.seats[0] = connection_id;
  }

  auto &stored = units_.emplace(unit.unit_id, std::move(unit)).first->second;
  if (seat_owner) {
    member->second.riding = std::pair{stored.unit_id, 0U};
    member->second.participant.position = stored.position;
  }
  announce(stored, result.update);

  if (seat_owner) {
    send_to_audience(stored, kMountUnitNotification,
                     mount_message(stored, connection_id, 0, tick), result.update);
  }
  result.unit_id = stored.unit_id;
  return result;
}

ActionResult CompanionWorld::ride(std::uint64_t connection_id, std::uint64_t unit_id,
                                  std::uint32_t slot, std::int64_t tick) {
  std::lock_guard lock(mutex_);
  ActionResult result;
  const auto member = members_.find(connection_id);
  const auto unit = units_.find(unit_id);
  if (member == members_.end() || unit == units_.end() ||
      !same_instance(unit->second, member->second)) {
    result.error = x5::ec::Riding_TargetNotFound;
    return result;
  }
  if (unit->second.kind == UnitKind::mate) {
    result.error = x5::ec::Riding_InvalidTargetUnitType;
    return result;
  }
  // Mounts are seated by the server when summoned; the client boards only vehicles.
  if (unit->second.kind != UnitKind::vehicle) {
    result.error = x5::ec::Riding_NotRidableTarget;
    return result;
  }
  if (unit->second.owner_character_id != member->second.participant.character_id) {
    result.error = x5::ec::Riding_NotOwner;
    return result;
  }
  if (slot >= unit->second.seats.size()) {
    result.error = x5::ec::Riding_InvalidSlot;
    return result;
  }
  if (member->second.riding) {
    result.error = x5::ec::Summon_AlreadyRiding;
    return result;
  }
  if (unit->second.seats[slot] != 0) {
    result.error = x5::ec::Riding_AlreadyOccupiedSlot;
    return result;
  }
  if (!within_radius(member->second.participant.position, unit->second.position,
                     config_.board_range_mm)) {
    result.error = x5::ec::Trade_WagonTooFar;
    return result;
  }

  unit->second.seats[slot] = connection_id;
  member->second.riding = std::pair{unit_id, slot};
  member->second.participant.position = unit->second.position;
  send_to_audience(unit->second, kMountUnitNotification,
                   mount_message(unit->second, connection_id, slot, tick), result.update);
  return result;
}

ActionResult CompanionWorld::get_off(std::uint64_t connection_id,
                                     const std::optional<WorldPosition> &dismount_point,
                                     std::int64_t tick, DismountReason reason) {
  std::lock_guard lock(mutex_);
  ActionResult result;
  const auto member = members_.find(connection_id);
  if (member == members_.end() || !member->second.riding) {
    result.error = x5::ec::InvalidState;
    return result;
  }

  const auto [unit_id, slot] = *member->second.riding;
  const auto unit = units_.find(unit_id);
  member->second.riding.reset();
  if (unit == units_.end()) {
    return result;
  }

  auto point = dismount_point.value_or(unit->second.position);
  point.map_id = unit->second.position.map_id;
  // Candidate server policy: accept a nearby landing point, never a client-requested teleport.
  if (!within_radius(point, unit->second.position, config_.board_range_mm) ||
      !std::isfinite(point.yaw)) {
    point = unit->second.position;
  }
  if (slot < unit->second.seats.size()) {
    unit->second.seats[slot] = 0;
  }
  member->second.participant.position = point;
  send_to_audience(unit->second, kMountUnitNotification,
                   dismount_message(unit->second, connection_id, reason, point, tick),
                   result.update);

  // HorseTable.DisMountActorRemoveDelay is one second: the summoned horse does not stay out
  // after its rider gets off (Candidate). Vehicles stay where they stopped.
  if (unit->second.kind == UnitKind::mount) {
    remove_unit(unit_id, result.update);
  }
  return result;
}

CompanionUpdate CompanionWorld::despawn_mount(std::uint64_t connection_id, std::int64_t tick) {
  std::lock_guard lock(mutex_);
  CompanionUpdate update;
  const auto mount = owned_unit(connection_id, UnitKind::mount);
  if (!mount) {
    return update;
  }

  unseat_all(units_.at(*mount), DismountReason::mount_removed, tick, update);
  remove_unit(*mount, update);
  return update;
}

std::optional<VehicleSnapshot> CompanionWorld::remove_vehicle(std::int64_t owner_character_id,
                                                              DismountReason reason,
                                                              std::int64_t tick,
                                                              CompanionUpdate &update) {
  std::lock_guard lock(mutex_);
  for (auto &[unit_id, unit] : units_) {
    if (unit.kind != UnitKind::vehicle || unit.owner_character_id != owner_character_id) {
      continue;
    }

    const auto final_state = snapshot(unit);
    unseat_all(unit, reason, tick, update);
    remove_unit(unit_id, update);
    return final_state;
  }
  return std::nullopt;
}

std::optional<VehicleSnapshot> CompanionWorld::vehicle_of(std::int64_t owner_character_id) const {
  std::lock_guard lock(mutex_);
  for (const auto &[unit_id, unit] : units_) {
    if (unit.kind == UnitKind::vehicle && unit.owner_character_id == owner_character_id) {
      return snapshot(unit);
    }
  }
  return std::nullopt;
}

VehicleHealthResult CompanionWorld::damage_vehicle(std::uint64_t unit_id, int amount,
                                                   std::int64_t tick) {
  std::lock_guard lock(mutex_);
  VehicleHealthResult result;
  const auto unit = units_.find(unit_id);
  if (unit == units_.end() || unit->second.kind != UnitKind::vehicle || amount <= 0) {
    result.error = x5::ec::InvalidUnit;
    return result;
  }

  const int delta = -std::min(amount, unit->second.health);
  unit->second.health += delta;
  result.health = unit->second.health;
  result.owner_character_id = unit->second.owner_character_id;
  send_to_audience(unit->second, kChangeStatNotification,
                   health_message(unit->second, delta, "SCS_Damage", tick), result.update);
  if (unit->second.health == 0) {
    result.destroyed = true;
    unseat_all(unit->second, DismountReason::mount_broken, tick, result.update);
    remove_unit(unit_id, result.update);
  }
  return result;
}

VehicleHealthResult CompanionWorld::repair_vehicle(std::uint64_t connection_id,
                                                   std::uint64_t unit_id, int amount,
                                                   std::int64_t tick) {
  std::lock_guard lock(mutex_);
  VehicleHealthResult result;
  const auto member = members_.find(connection_id);
  const auto unit = units_.find(unit_id);
  if (member == members_.end() || unit == units_.end() ||
      unit->second.kind != UnitKind::vehicle || !same_instance(unit->second, member->second)) {
    result.error = x5::ec::Interaction_TargetNotFound;
    return result;
  }
  if (!within_radius(member->second.participant.position, unit->second.position,
                     config_.board_range_mm)) {
    result.error = x5::ec::Interaction_InvalidArea;
    return result;
  }
  // InteractionCondition 62 requires the target's durability below its threshold; the threshold
  // value is unobserved, so a vehicle at full health cannot be repaired (Candidate).
  if (amount <= 0 || unit->second.health >= unit->second.max_health) {
    result.error = x5::ec::Interaction_ConditionsNotMatch;
    return result;
  }

  const int delta = std::min(amount, unit->second.max_health - unit->second.health);
  unit->second.health += delta;
  result.health = unit->second.health;
  result.owner_character_id = unit->second.owner_character_id;
  send_to_audience(unit->second, kChangeStatNotification,
                   health_message(unit->second, delta, "SCS_InteractionFlow", tick),
                   result.update);
  return result;
}

bool CompanionWorld::vehicle_in_reach(std::uint64_t connection_id, std::uint64_t unit_id) const {
  std::lock_guard lock(mutex_);
  const auto member = members_.find(connection_id);
  const auto unit = units_.find(unit_id);
  return member != members_.end() && unit != units_.end() &&
         unit->second.kind == UnitKind::vehicle &&
         unit->second.owner_character_id == member->second.participant.character_id &&
         within_radius(member->second.participant.position, unit->second.position,
                       config_.board_range_mm);
}

SpawnResult CompanionWorld::spawn_mate(std::uint64_t connection_id, int mate_npc_cms_id,
                                       const MateRecord &record) {
  std::lock_guard lock(mutex_);
  SpawnResult result;
  const auto *mate = catalog_->mate(mate_npc_cms_id);
  const auto member = members_.find(connection_id);
  if (!mate || member == members_.end()) {
    result.error = x5::ec::Housing_NotInvitedMate;
    return result;
  }
  for (const auto &[unit_id, unit] : units_) {
    if (unit.kind == UnitKind::mate && unit.owner_connection_id == connection_id &&
        unit.mate_npc_cms_id == mate_npc_cms_id) {
      result.error = x5::ec::Housing_AlreadyInvitedMate;
      return result;
    }
  }

  Unit unit;
  unit.unit_id = allocate_unit_id(UnitKind::mate);
  unit.kind = UnitKind::mate;
  unit.owner_character_id = member->second.participant.character_id;
  unit.owner_connection_id = connection_id;
  unit.channel_key = member->second.participant.channel_key;
  unit.position = member->second.participant.position;
  unit.npc_cms_id = mate->npc_cms_id;
  unit.mate_npc_cms_id = mate->id;
  unit.mate = record;

  auto &stored = units_.emplace(unit.unit_id, std::move(unit)).first->second;
  announce(stored, result.update);
  result.unit_id = stored.unit_id;
  return result;
}

std::optional<int> CompanionWorld::despawn_mate(std::uint64_t connection_id,
                                                std::uint64_t unit_id, CompanionUpdate &update) {
  std::lock_guard lock(mutex_);
  const auto unit = units_.find(unit_id);
  if (unit == units_.end() || unit->second.kind != UnitKind::mate ||
      unit->second.owner_connection_id != connection_id) {
    return std::nullopt;
  }

  const auto mate_id = unit->second.mate_npc_cms_id;
  remove_unit(unit_id, update);
  return mate_id;
}

std::optional<std::uint64_t> CompanionWorld::mate_unit(std::uint64_t connection_id,
                                                       int mate_npc_cms_id) const {
  std::lock_guard lock(mutex_);
  for (const auto &[unit_id, unit] : units_) {
    if (unit.kind == UnitKind::mate && unit.owner_connection_id == connection_id &&
        unit.mate_npc_cms_id == mate_npc_cms_id) {
      return unit_id;
    }
  }
  return std::nullopt;
}

std::optional<int> CompanionWorld::owned_mate(std::uint64_t connection_id,
                                              std::uint64_t unit_id) const {
  std::lock_guard lock(mutex_);
  const auto unit = units_.find(unit_id);
  if (unit == units_.end() || unit->second.kind != UnitKind::mate ||
      unit->second.owner_connection_id != connection_id) {
    return std::nullopt;
  }
  return unit->second.mate_npc_cms_id;
}

std::size_t CompanionWorld::unit_count() const {
  std::lock_guard lock(mutex_);
  return units_.size();
}

std::uint64_t CompanionWorld::allocate_unit_id(UnitKind kind) {
  const auto nibble = kind == UnitKind::vehicle ? kOperableUnitTypeNibble : kNpcUnitTypeNibble;

  // The 24-bit sequence wraps around; an id whose earlier unit is still alive is skipped.
  for (std::uint64_t attempt = 0; attempt < kCompanionUnitSequenceCount; ++attempt) {
    const auto sequence = next_unit_sequence_ % kCompanionUnitSequenceCount;
    ++next_unit_sequence_;

    const auto unit_id = kCompanionUnitIdBase | (sequence << 4) | nibble;
    if (!units_.contains(unit_id)) {
      return unit_id;
    }
  }
  throw std::runtime_error("No free companion unit id");
}

void CompanionWorld::set_next_unit_sequence_for_test(std::uint64_t sequence) {
  std::lock_guard lock(mutex_);
  next_unit_sequence_ = sequence;
}

bool CompanionWorld::same_instance(const Unit &unit, const Member &member) const {
  return unit.position.map_id == member.participant.position.map_id &&
         unit.channel_key == member.participant.channel_key;
}

bool CompanionWorld::sees(const Unit &unit, const Member &member, bool already_visible) const {
  if (unit.region_suspended || member.region_suspended || !same_instance(unit, member)) {
    return false;
  }
  const auto radius =
      already_visible ? config_.sight_leave_radius_mm : config_.sight_enter_radius_mm;
  return within_radius(unit.position, member.participant.position, radius);
}

std::uint64_t CompanionWorld::rider_wire_id(std::uint64_t connection_id) const {
  const auto member = members_.find(connection_id);
  if (member == members_.end()) {
    return 0;
  }
  return player_wire_unit_id(member->second.participant.character_id);
}

Json CompanionWorld::unit_add_message(const Unit &unit) const {
  auto passengers = Json::array();
  for (const auto seat : unit.seats) {
    passengers.push_back(decimal(seat == 0 ? 0 : rider_wire_id(seat)));
  }
  // Trailing empty seats carry no information; the client skips zero ids anyway.
  while (!passengers.empty() && passengers.back() == "0") {
    passengers.erase(passengers.end() - 1);
  }

  Json nub{{"pos", world_pos(unit.position)}, {"yaw", unit.position.yaw}};
  const auto owner = std::to_string(unit.owner_character_id);
  if (unit.kind == UnitKind::vehicle) {
    nub["operable"] = {
        {"state", kIdleState},
        {"stats",
         {{kMaxHealthStat, unit.max_health}, {std::to_string(kHealthStatType), unit.health}}},
        {"faction_type", kPlayerFaction},
        {"vehicle",
         {{"vehicle_cms_id", unit.vehicle_cms_id},
          // base_unit_id is not read by the client's vehicle builder; the unit itself is the base.
          {"base_unit_id", decimal(unit.unit_id)},
          {"owner_char_id", owner},
          {"passengers", passengers}}}};
  } else {
    Json npc{{"npc_cms_id", unit.npc_cms_id},
             {"base_unit_id", decimal(unit.unit_id)},
             {"owner_char_id", owner},
             {"state", kIdleState}};
    if (unit.kind == UnitKind::mount) {
      npc["faction_type"] = kPlayerFaction;
      npc["stats"] = {{kMoveSpeedRateStat, kNeutralMoveSpeedRate}};
      npc["summon"] = {{"summon_npc_cms_id", unit.summon_npc_cms_id},
                       {"summon_state_type", kStandByState}};
      npc["mount"] = {{"passengers", passengers}};
    } else {
      npc["faction_type"] = kTownNpcFaction;
      npc["mate"] = {{"mate_npc_cms_id", unit.mate_npc_cms_id},
                     {"state_type", unit.mate.state},
                     {"housing_lot_number", unit.mate.housing_lot_number},
                     {"intimate", unit.mate.intimacy}};
    }
    nub["npc"] = std::move(npc);
  }
  return {{"unit_id", decimal(unit.unit_id)}, {"nub", std::move(nub)}, {"spawned_unit", true}};
}

Json CompanionWorld::mount_message(const Unit &unit, std::uint64_t rider_connection,
                                   std::uint32_t slot, std::int64_t tick) const {
  return {{"unit_id", decimal(unit.unit_id)},
          {"passenger_unit_id", decimal(rider_wire_id(rider_connection))},
          {"slot", slot},
          {"tick", std::to_string(tick)}};
}

Json CompanionWorld::dismount_message(const Unit &unit, std::uint64_t rider_connection,
                                      DismountReason reason,
                                      const std::optional<WorldPosition> &point,
                                      std::int64_t tick) const {
  Json message{{"unit_id", decimal(unit.unit_id)},
               {"passenger_unit_id", decimal(rider_wire_id(rider_connection))},
               {"dismount_reason", dismount_reason_name(reason)},
               {"tick", std::to_string(tick)}};
  if (point) {
    message["dismount_point"] = {{"pos", world_pos(*point)}, {"yaw", point->yaw}};
  }
  return message;
}

Json CompanionWorld::health_message(const Unit &unit, int delta, const char *source,
                                    std::int64_t tick) const {
  return {{"unit_id", decimal(unit.unit_id)},
          {"tick", std::to_string(tick)},
          {"stat_changes",
           Json::array({{{"stat_type", kHealthStatType},
                         {"source", source},
                         {"delta", delta},
                         {"result", unit.health}}})}};
}

void CompanionWorld::announce(Unit &unit, CompanionUpdate &update) {
  for (const auto &[connection_id, member] : members_) {
    if (sees(unit, member, false)) {
      add_observer(unit, member, update);
    }
  }
}

void CompanionWorld::add_observer(Unit &unit, const Member &member, CompanionUpdate &update) {
  if (next_observer_generation_ == (std::numeric_limits<std::uint64_t>::max)()) {
    throw std::overflow_error("Companion observer generation exhausted");
  }
  const auto connection = member.participant.connection_id;
  const auto &view = member.participant.view;
  const auto generation = next_observer_generation_;
  OutboundPacket packet{connection, kUnitAddNotification, unit_add_message(unit),
                        CompanionSourceBinding{connection, unit.unit_id, generation, view, true}};
  // Reserve before installing the relationship so allocation failure leaves no
  // observer without an add. Grow geometrically to avoid quadratic relocation.
  if (update.packets.size() == update.packets.capacity()) {
    const auto capacity = update.packets.capacity();
    const auto maximum = update.packets.max_size();
    if (capacity == maximum) {
      throw std::length_error("Companion notification capacity exhausted");
    }
    const auto increment = std::min(std::max<std::size_t>(capacity, 1), maximum - capacity);
    update.packets.reserve(capacity + increment);
  }
  const bool inserted = unit.observers.emplace(
      connection, Observer{view, generation, view.epoch == 0}).second;
  if (!inserted) {
    throw std::logic_error("Companion observer already exists");
  }
  ++next_observer_generation_;
  update.packets.push_back(std::move(packet));
}

CompanionSourceBinding CompanionWorld::source_binding(const Unit &unit,
                                                       std::uint64_t connection_id,
                                                       bool initial_add) const {
  const auto &relationship = unit.observers.at(connection_id);
  return {connection_id, unit.unit_id, relationship.generation, relationship.view, initial_add};
}

std::optional<nlohmann::json> CompanionWorld::snapshot_unit_add(
    const CompanionSourceBinding &binding) const {
  std::lock_guard lock(mutex_);
  const auto unit = units_.find(binding.unit_id);
  if (!binding.initial_add || binding.terminal || unit == units_.end()) {
    return std::nullopt;
  }
  const auto observer = unit->second.observers.find(binding.connection_id);
  if (observer == unit->second.observers.end() ||
      observer->second.generation != binding.generation || observer->second.view != binding.view) {
    return std::nullopt;
  }
  return unit_add_message(unit->second);
}

std::shared_ptr<CompanionWorld::QueueGuard> CompanionWorld::acquire_source_queue_guard(
    const CompanionSourceBinding &binding, std::shared_ptr<void> lifetime,
    const nlohmann::json *encoded_snapshot) {
  std::unique_lock lock(mutex_);
  const auto unit = units_.find(binding.unit_id);
  if (binding.terminal) {
    if (unit != units_.end() && unit->second.observers.contains(binding.connection_id)) {
      return {};
    }
    return std::make_shared<QueueGuard>(std::move(lock), nullptr, std::move(lifetime));
  }
  if (unit == units_.end()) {
    return {};
  }
  const auto observer = unit->second.observers.find(binding.connection_id);
  if (observer == unit->second.observers.end() ||
      observer->second.generation != binding.generation || observer->second.view != binding.view ||
      (!binding.initial_add && !observer->second.ready)) {
    return {};
  }
  if (binding.initial_add && binding.view.epoch != 0 &&
      (!encoded_snapshot || unit_add_message(unit->second) != *encoded_snapshot)) {
    return {};
  }
  return std::make_shared<QueueGuard>(std::move(lock),
                                      binding.initial_add ? &observer->second.ready : nullptr,
                                      std::move(lifetime));
}

void CompanionWorld::refresh_unit_visibility(Unit &unit, CompanionUpdate &update) {
  for (const auto &[connection_id, member] : members_) {
    const bool visible = unit.observers.contains(connection_id);
    const bool in_view = sees(unit, member, visible);
    if (in_view && !visible) {
      add_observer(unit, member, update);
    } else if (!in_view && visible) {
      const auto relationship = unit.observers.at(connection_id);
      if (relationship.ready) {
        auto binding = source_binding(unit, connection_id, false);
        binding.terminal = true;
        update.packets.push_back(
            {connection_id, kUnitRemoveNotification,
             {{"unit_ids", Json::array({decimal(unit.unit_id)})}, {"by_sight", true}}, binding});
      }
      unit.observers.erase(connection_id);
    }
  }
}

void CompanionWorld::refresh_member_visibility(const Member &member, CompanionUpdate &update) {
  const auto connection_id = member.participant.connection_id;
  for (auto &[unit_id, unit] : units_) {
    const bool visible = unit.observers.contains(connection_id);
    const bool in_view = sees(unit, member, visible);
    if (in_view && !visible) {
      add_observer(unit, member, update);
    } else if (!in_view && visible) {
      const auto relationship = unit.observers.at(connection_id);
      if (relationship.ready) {
        auto binding = source_binding(unit, connection_id, false);
        binding.terminal = true;
        update.packets.push_back(
            {connection_id, kUnitRemoveNotification,
             {{"unit_ids", Json::array({decimal(unit_id)})}, {"by_sight", true}}, binding});
      }
      unit.observers.erase(connection_id);
    }
  }
}

void CompanionWorld::send_to_audience(const Unit &unit, std::uint16_t opcode,
                                      const nlohmann::json &message,
                                      CompanionUpdate &update) const {
  for (const auto &[observer, relationship] : unit.observers) {
    static_cast<void>(relationship);
    update.packets.push_back({observer, opcode, message, source_binding(unit, observer, false)});
  }
}

void CompanionWorld::unseat_all(Unit &unit, DismountReason reason, std::int64_t tick,
                                CompanionUpdate &update) {
  for (auto &seat : unit.seats) {
    if (seat == 0) {
      continue;
    }

    send_to_audience(unit, kMountUnitNotification,
                     dismount_message(unit, seat, reason, unit.position, tick), update);
    const auto rider = members_.find(seat);
    if (rider != members_.end()) {
      rider->second.riding.reset();
    }
    seat = 0;
  }
}

void CompanionWorld::remove_unit(std::uint64_t unit_id, CompanionUpdate &update) {
  const auto unit = units_.find(unit_id);
  if (unit == units_.end()) {
    return;
  }

  for (const auto seat : unit->second.seats) {
    const auto rider = members_.find(seat);
    if (seat != 0 && rider != members_.end()) {
      rider->second.riding.reset();
    }
  }
  const Json message{{"unit_ids", Json::array({decimal(unit_id)})}};
  for (const auto &[connection_id, relationship] : unit->second.observers) {
    if (!relationship.ready) {
      continue;
    }
    auto binding = source_binding(unit->second, connection_id, false);
    binding.terminal = true;
    for (auto &packet : update.packets) {
      if (packet.connection_id == connection_id && packet.opcode == kMountUnitNotification &&
          packet.source && packet.source->unit_id == unit_id &&
          packet.source->generation == relationship.generation) {
        packet.source->terminal = true;
      }
    }
    update.packets.push_back({connection_id, kUnitRemoveNotification, message, binding});
  }
  units_.erase(unit);
}

std::optional<std::uint64_t> CompanionWorld::owned_unit(std::uint64_t connection_id,
                                                        UnitKind kind) const {
  for (const auto &[unit_id, unit] : units_) {
    if (unit.kind == kind && unit.owner_connection_id == connection_id) {
      return unit_id;
    }
  }
  return std::nullopt;
}

VehicleSnapshot CompanionWorld::snapshot(const Unit &unit) const {
  VehicleSnapshot result;
  result.unit_id = unit.unit_id;
  result.vehicle_cms_id = unit.vehicle_cms_id;
  result.position = unit.position;
  result.health = unit.health;
  result.owner_riding = !unit.seats.empty() && unit.seats[0] == unit.owner_connection_id;
  return result;
}

} // namespace chronicles::companion
