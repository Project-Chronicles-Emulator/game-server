#pragma once

// Shared runtime of summoned mounts, vehicles (trade wagons) and house mates.
//
// CompanionWorld owns every companion unit in the world: its unit id, owner, map instance,
// position, seats and health, and which connections currently see it. It never touches
// sockets, sessions, storage or the protocol codec; callers persist progress, encode the
// returned payloads and deliver them. Thread-safe: every public method takes one internal lock.
// The staged detach callback is the sole exception: it runs under that lock, must not reenter
// CompanionWorld, block on an executor or perform database I/O.
//
// Client contract (static analysis, out/vehicle-research/mounts-findings.md):
// - Unit id low nibble is the client unit type: 2 NPC (mounts and mates), 4 operable (vehicles).
// - A mount is announced with UnitAddNoti{npc{npc_cms_id, owner_char_id = raw character id,
//   faction Player, summon{summon_npc_cms_id}, mount{passengers}}}. The client never asks to
//   ride a horse; the server seats the rider with MountUnitNoti{unit, passenger, slot}.
// - A vehicle is UnitAddNoti{operable{vehicle{vehicle_cms_id, base_unit_id, owner_char_id,
//   passengers}}}; the driver's RideUnitReq always names slot 0.
// - MountUnitNoti with slot binds a rider; without slot (plus dismount_reason/point) unbinds.
// - PC2S_MoveReq has no unit id. While riding, the rider's movement moves the ridden unit and
//   the server broadcasts MoveNoti for that unit (Candidate: not for the rider).
// - Remote late joiners bind riders from MountNub/VehicleNub.passengers (slot = index).

#include "data/companion_catalog.hpp"
#include "ec.generated.pb.h"
#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chronicles::companion {

inline constexpr std::uint16_t kSpawnMateNpcRequest = 20024;
inline constexpr std::uint16_t kDespawnMateNpcRequest = 20025;
inline constexpr std::uint16_t kInviteMateNpcRequest = 20026;
inline constexpr std::uint16_t kSpawnSummonNpcRequest = 20027;
inline constexpr std::uint16_t kRideUnitRequest = 20030;
inline constexpr std::uint16_t kGetOffUnitRequest = 20031;

inline constexpr std::uint16_t kCommonErrorResponse = 2;
inline constexpr std::uint16_t kUnitAddNotification = 10;
inline constexpr std::uint16_t kUnitRemoveNotification = 11;
inline constexpr std::uint16_t kChangeStatNotification = 47;
inline constexpr std::uint16_t kMountUnitNotification = 56;
inline constexpr std::uint16_t kRegisterSummonNpcResponse = 57;
inline constexpr std::uint16_t kRegisterMateNpcResponse = 58;
inline constexpr std::uint16_t kChangedMateIntimateResponse = 59;
inline constexpr std::uint16_t kChangedMateStateResponse = 60;
inline constexpr std::uint16_t kSpawnSummonNpcResponse = 102;
inline constexpr std::uint16_t kRideUnitResponse = 103;
inline constexpr std::uint16_t kWorldCharMatesNotification = 133;
inline constexpr std::uint16_t kWorldCharSummonNpcsNotification = 137;
inline constexpr std::uint16_t kMoveNotification = 177;

// Client unit-type nibbles (low four bits of a unit id).
inline constexpr std::uint64_t kNpcUnitTypeNibble = 2;
inline constexpr std::uint64_t kOperableUnitTypeNibble = 4;

// Companion unit ids are kCompanionUnitIdBase | (sequence << 4) | type nibble and always fit in
// 32 bits. The client's per-unit packet dispatchers (MountUnitNoti, MoveNoti and the other
// unit-addressed notifications) look the actor up by the LOW 32 BITS of the wire id
// (client 0x146092DB0), while UnitAddNoti registers the actor under its full id, so a unit whose
// id sets any high-dword bit is never found by those packets. The block [0xE0000000, 0xEFFFFFFF]
// lies above the NPC population ids (from 0x80000000, at most kMaxPopulatedNpcUnits of them) and
// below GM-spawned NPC ids (0xF0000000 and above); player ids have nibble 1 and stay below
// 0x80000000.
inline constexpr std::uint64_t kCompanionUnitIdBase = 0xE0000000ULL;
inline constexpr std::uint64_t kCompanionUnitIdBlockMask = 0xFFFFFFFFF0000000ULL;

// Sequence numbers occupy the 24 bits between the block nibble and the type nibble.
inline constexpr std::uint64_t kCompanionUnitSequenceCount = 1ULL << 24;

// True when the id lies in the companion unit block (any type nibble).
inline constexpr bool is_companion_unit_id(std::uint64_t unit_id) {
  return (unit_id & kCompanionUnitIdBlockMask) == kCompanionUnitIdBase;
}

// x5.cms.EX5MateStateType.
enum class MateState : int {
  be_away = 1,
  spawned = 2,
  invited = 3,
  follow = 4,
};

// x5.cms.EX5DismountReason values the server sends.
enum class DismountReason : int {
  user = 1,
  rider_removed = 2,
  rider_killed = 3,
  mount_removed = 5,
  mount_broken = 7,
  clear_trade = 9,
};

enum class UnitKind {
  mount,
  vehicle,
  mate,
};

// Bounds passenger snapshots before constructing their JSON or seat vectors.
inline constexpr std::size_t kMaximumCompanionSeats = 64;
std::optional<std::size_t> checked_companion_seat_count(int authored_count) noexcept;

// Opaque recipient-view identity supplied by the native session. The companion
// channel key is separate: private-copy channel keys contain an extra type bit.
struct CompanionViewToken {
  std::uint64_t epoch{};
  std::optional<int> map_id;
  std::uint64_t copy_channel{};

  bool operator==(const CompanionViewToken &) const = default;
};

struct CompanionSourceBinding {
  std::uint64_t connection_id{};
  std::uint64_t unit_id{};
  std::uint64_t generation{};
  CompanionViewToken view;
  bool initial_add{};
  // A removal or its preceding dismount remains valid after the relation ends,
  // unless that recipient has since acquired a newer relation for the same unit.
  bool terminal{};
};

// One decoded payload addressed to a world connection.
struct OutboundPacket {
  std::uint64_t connection_id{};
  std::uint16_t opcode{};
  nlohmann::json message;
  std::optional<CompanionSourceBinding> source;
};

struct CompanionUpdate {
  std::vector<OutboundPacket> packets;

  void append(CompanionUpdate other);
};

// What a session publishes about its own character. Two participants share an instance only
// when map and channel key match (private tutorial instances use the character id; shared maps
// use zero).
struct Participant {
  std::uint64_t connection_id{};
  std::int64_t character_id{};
  std::uint64_t channel_key{};
  WorldPosition position;
  CompanionViewToken view;
};

// Sight radii shared with player presence: units enter view inside 100 m and leave beyond 120 m.
struct CompanionWorldConfig {
  std::int64_t sight_enter_radius_mm{100000};
  std::int64_t sight_leave_radius_mm{120000};
  // Three-dimensional reach for boarding a vehicle, the NPC interaction radius.
  std::int64_t board_range_mm{7000};
};

// A vehicle's state when it leaves the world, so the owner's trade run can persist it.
struct VehicleSnapshot {
  std::uint64_t unit_id{};
  int vehicle_cms_id{};
  WorldPosition position;
  int health{};
  bool owner_riding{};
};

struct SpawnResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  std::uint64_t unit_id{};
  CompanionUpdate update;
};

struct ActionResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  CompanionUpdate update;
};

struct DetachResult {
  CompanionUpdate update;
  std::vector<VehicleSnapshot> vehicles;
};

struct CompanionDetachRecipient {
  std::uint64_t connection_id{};
  CompanionViewToken view;
  CompanionSourceBinding source;
  std::shared_ptr<const std::vector<GameplayPacket>> packets;
};

struct CompanionDetachCursor {
  std::size_t unit{};
  std::uint64_t connection_id{};
};

struct CompanionDetachPage {
  std::vector<CompanionDetachRecipient> recipients;
  CompanionDetachCursor next;
  bool complete{};
};

// Process-wide admission for detached source metadata. A retained operation keeps
// its lease until its last notification page is settled or discarded on shutdown.
class CompanionDetachBudget final : public std::enable_shared_from_this<CompanionDetachBudget> {
public:
  static constexpr std::size_t kDefaultLimitBytes = 256 * 1024 * 1024;

  explicit CompanionDetachBudget(std::size_t limit_bytes = kDefaultLimitBytes);
  std::shared_ptr<void> reserve(std::size_t bytes);
  std::size_t used_bytes() const noexcept;

private:
  class Lease;
  const std::size_t limit_bytes_;
  std::atomic<std::size_t> used_bytes_{};
};

// Health change of a vehicle unit (damage or repair).
struct VehicleHealthResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  int health{};
  bool destroyed{};
  std::int64_t owner_character_id{};
  CompanionUpdate update;
};

class CompanionWorld {
public:
  class QueueGuard;
  class RetainedDetach;
  CompanionWorld(std::shared_ptr<const CompanionCatalog> catalog, CompanionWorldConfig config,
                 std::shared_ptr<CompanionDetachBudget> detach_budget = {});

  // Registers an in-world session and announces every companion unit it can see.
  CompanionUpdate attach(const Participant &participant);

  // A participant moved. A rider's movement also moves its ridden unit; `movement` is the
  // decoded PC2S_MoveReq, reused as the ridden unit's MoveNoti (null for teleports).
  CompanionUpdate move(std::uint64_t connection_id, const WorldPosition &position,
                       const nlohmann::json *movement);

  // Reannounce visible companions after a regional map load, retaining the rider and unit IDs.
  CompanionUpdate reenter_region(std::uint64_t connection_id, const WorldPosition &position,
                                 std::int64_t tick,
                                 const CompanionViewToken &view = {});

  // For an initial add, snapshot the source, encode it outside the lock, then
  // acquire the guard using that exact JSON. If it changed, rebuild or close the
  // recipient view; silently discarding an owed initial add is not sufficient.
  // Callers must bound snapshot construction and encoded frame sizes. Lock source
  // first, then the recipient's native view guard. Keep both through insertion;
  // mark_ready is called only after the add actually enters the transport queue.
  std::optional<nlohmann::json> snapshot_unit_add(const CompanionSourceBinding &binding) const;
  std::shared_ptr<QueueGuard> acquire_source_queue_guard(
      const CompanionSourceBinding &binding, std::shared_ptr<void> lifetime = {},
      const nlohmann::json *encoded_snapshot = nullptr);

  CompanionUpdate suspend_region(std::uint64_t connection_id, const WorldPosition &destination);

  // The participant entered another instance copy (or a shared map, key 0). Its ridden unit goes
  // with it; units of other copies leave its view and its own units leave theirs.
  CompanionUpdate set_channel(std::uint64_t connection_id, std::uint64_t channel_key);

  // Session closed. Removes the participant and every unit it owns; vehicles are reported so
  // their transform and health can be persisted.
  DetachResult detach(std::uint64_t connection_id);
  DetachResult detach_staged(
      std::uint64_t connection_id,
      const std::function<void(const std::vector<VehicleSnapshot> &)> &before_commit);

  // The callback retains the operation and wagon state before any world mutation.
  // It must not perform I/O or reenter this world. The operation is immutable once
  // this method returns; its pages preserve each recipient's packet order.
  DetachResult detach_retained(
      std::uint64_t connection_id,
      const std::function<void(const std::vector<VehicleSnapshot> &,
                               const std::shared_ptr<RetainedDetach> &)> &before_commit);

  // The unit this connection's character rides, if any.
  std::optional<std::uint64_t> ridden_unit(std::uint64_t connection_id) const;

  // Spawns the owner's mount for a registered summon at the owner's position and seats the
  // owner in slot 0. Fails with Summon_AlreadyRiding while riding.
  SpawnResult spawn_mount(std::uint64_t connection_id, int summon_npc_cms_id,
                          std::int64_t tick);

  // Spawns a vehicle owned by the connection at `position`. When `seat_owner` the owner is
  // seated in slot 0 right away (trade departure).
  SpawnResult spawn_vehicle(std::uint64_t connection_id, int vehicle_cms_id,
                            const WorldPosition &position, int health, bool seat_owner,
                            std::int64_t tick);

  // PC2S_RideUnitReq: boards a vehicle seat. Errors are RideUnitRes codes.
  ActionResult ride(std::uint64_t connection_id, std::uint64_t unit_id, std::uint32_t slot,
                    std::int64_t tick);

  // PC2S_GetOffUnitReq: leaves the ridden unit at `dismount_point` (or the unit's position).
  // A mount is removed after its rider gets off; a vehicle stays where it is.
  ActionResult get_off(std::uint64_t connection_id,
                       const std::optional<WorldPosition> &dismount_point, std::int64_t tick,
                       DismountReason reason = DismountReason::user);

  // Removes the owner's mount (MountRemoved dismount, then UnitRemoveNoti).
  CompanionUpdate despawn_mount(std::uint64_t connection_id, std::int64_t tick);

  // Removes the character's vehicle, unseating riders with `reason`. Returns the final state.
  std::optional<VehicleSnapshot> remove_vehicle(std::int64_t owner_character_id,
                                                DismountReason reason, std::int64_t tick,
                                                CompanionUpdate &update);

  std::optional<VehicleSnapshot> vehicle_of(std::int64_t owner_character_id) const;

  // Damages a vehicle; at zero health it breaks (MountBroken) and is removed.
  VehicleHealthResult damage_vehicle(std::uint64_t unit_id, int amount, std::int64_t tick);

  // RepairWagon: adds health up to the vehicle's maximum. Fails while undamaged.
  VehicleHealthResult repair_vehicle(std::uint64_t connection_id, std::uint64_t unit_id,
                                     int amount, std::int64_t tick);

  // True when the vehicle exists, is owned by the connection's character and lies within the
  // boarding range of the connection's position.
  bool vehicle_in_reach(std::uint64_t connection_id, std::uint64_t unit_id) const;

  // Spawns a registered mate beside its owner.
  SpawnResult spawn_mate(std::uint64_t connection_id, int mate_npc_cms_id,
                         const MateRecord &record);

  // Removes the owner's mate unit; returns the mate id that was removed.
  std::optional<int> despawn_mate(std::uint64_t connection_id, std::uint64_t unit_id,
                                  CompanionUpdate &update);

  // The owner's spawned mate unit for a mate id.
  std::optional<std::uint64_t> mate_unit(std::uint64_t connection_id, int mate_npc_cms_id) const;

  // The mate id behind an owned mate unit.
  std::optional<int> owned_mate(std::uint64_t connection_id, std::uint64_t unit_id) const;

  std::size_t unit_count() const;

  // Exercise wire-ID reuse without allocating the entire 24-bit sequence in tests.
  void set_next_unit_sequence_for_test(std::uint64_t sequence);

private:
  struct Observer {
    CompanionViewToken view;
    std::uint64_t generation{};
    bool ready{};
  };

  struct Unit {
    std::uint64_t unit_id{};
    UnitKind kind{UnitKind::mount};
    std::int64_t owner_character_id{};
    std::uint64_t owner_connection_id{};
    std::uint64_t channel_key{};
    WorldPosition position;
    int npc_cms_id{};
    int summon_npc_cms_id{};
    int vehicle_cms_id{};
    int mate_npc_cms_id{};
    MateRecord mate;
    int health{};
    int max_health{};
    bool region_suspended{};
    // Seated player connections by seat index; zero is an empty seat.
    std::vector<std::uint64_t> seats;
    std::map<std::uint64_t, Observer> observers;
  };

  struct Member {
    Participant participant;
    bool region_suspended{};
    // Unit and seat the member rides.
    std::optional<std::pair<std::uint64_t, std::uint32_t>> riding;
  };

  std::uint64_t allocate_unit_id(UnitKind kind);
  bool same_instance(const Unit &unit, const Member &member) const;
  bool sees(const Unit &unit, const Member &member, bool already_visible) const;
  nlohmann::json unit_add_message(const Unit &unit) const;
  nlohmann::json mount_message(const Unit &unit, std::uint64_t rider_connection,
                               std::uint32_t slot, std::int64_t tick) const;
  nlohmann::json dismount_message(const Unit &unit, std::uint64_t rider_connection,
                                  DismountReason reason,
                                  const std::optional<WorldPosition> &point,
                                  std::int64_t tick) const;
  std::uint64_t rider_wire_id(std::uint64_t connection_id) const;
  // Announces a new unit to every member that can see it.
  void announce(Unit &unit, CompanionUpdate &update);
  void add_observer(Unit &unit, const Member &member, CompanionUpdate &update);
  CompanionSourceBinding source_binding(const Unit &unit, std::uint64_t connection_id,
                                        bool initial_add) const;
  // Recomputes which members see `unit` after it moved.
  void refresh_unit_visibility(Unit &unit, CompanionUpdate &update);
  // Recomputes which units `member` sees after it moved.
  void refresh_member_visibility(const Member &member, CompanionUpdate &update);
  void send_to_audience(const Unit &unit, std::uint16_t opcode, const nlohmann::json &message,
                        CompanionUpdate &update) const;
  void unseat_all(Unit &unit, DismountReason reason, std::int64_t tick, CompanionUpdate &update);
  void remove_unit(std::uint64_t unit_id, CompanionUpdate &update);
  std::optional<std::uint64_t> owned_unit(std::uint64_t connection_id, UnitKind kind) const;
  VehicleSnapshot snapshot(const Unit &unit) const;
  nlohmann::json health_message(const Unit &unit, int delta, const char *source,
                                std::int64_t tick) const;

  mutable std::mutex mutex_;
  std::shared_ptr<const CompanionCatalog> catalog_;
  std::shared_ptr<CompanionDetachBudget> detach_budget_;
  CompanionWorldConfig config_;
  std::unordered_map<std::uint64_t, Member> members_;
  std::map<std::uint64_t, Unit> units_;
  std::uint64_t next_unit_sequence_{1};
  std::uint64_t next_observer_generation_{1};
};

class CompanionWorld::QueueGuard final {
public:
  QueueGuard(std::unique_lock<std::mutex> lock, bool *ready, std::shared_ptr<void> lifetime)
      : lifetime_(std::move(lifetime)), lock_(std::move(lock)), ready_(ready) {}

  void mark_ready() noexcept {
    if (ready_) {
      *ready_ = true;
    }
  }

private:
  std::shared_ptr<void> lifetime_;
  std::unique_lock<std::mutex> lock_;
  bool *ready_{};
};

class CompanionWorld::RetainedDetach final {
public:
  CompanionDetachPage page(CompanionDetachCursor cursor, std::size_t limit) const;

private:
  friend class CompanionWorld;
  struct DetachedUnit {
    std::uint64_t unit_id{};
    std::map<std::uint64_t, Observer> observers;
    std::shared_ptr<const std::vector<GameplayPacket>> packets;
  };
  std::vector<DetachedUnit> units_;
  std::shared_ptr<void> budget_lease_;
  std::uint64_t owner_connection_{};
  bool committed_{};
};

} // namespace chronicles::companion
