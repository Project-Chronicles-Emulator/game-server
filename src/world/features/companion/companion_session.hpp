#pragma once

// One world connection's mounts, vehicle boarding and house mates.
//
// CompanionSession decodes the companion requests of an authenticated non-tutorial session,
// checks ownership against committed progress, persists registrations and mate states, and
// drives the shared CompanionWorld. Packets for this connection come back in `own` (in send
// order, after the frame's response); packets for other connections, and the shared unit
// announcements for this one, come back in `broadcast`. Confined to the session's worker strand.
//
// Requests (opcodes from protocol/opcodes.json):
//   20027 SpawnSummonNpcReq   -> SpawnSummonNpcRes(102); mount UnitAddNoti + MountUnitNoti
//   20030 RideUnitReq         -> RideUnitRes(103); MountUnitNoti on success
//   20031 GetOffUnitReq       -> MountUnitNoti without slot (no typed reply exists)
//   20024 SpawnMateNpcReq     -> mate UnitAddNoti, ChangedMateStateRes(60), SystemMessageNoti
//   20025 DespawnMateNpcReq   -> ChangedMateStateRes(BeAway), UnitRemoveNoti
//   20026 InviteMateNpcReq    -> ChangedMateStateRes(Invited)
//   20077 ItemUseReq          -> only for consumables whose skill effect registers a summon:
//                                ItemsUpdateNoti(use) + RegisterSummonNpcRes(57)
//   20068 InteractUnitReq     -> only RepairWagon interactions on an owned vehicle
// PC2S_CreateOperableReq, ChangeSummonStateReq and DespawnSummonNpcReq have no client sender in
// this build and are not handled.

#include "data/shop_catalog.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/core/progress_access.hpp"
#include "world/core/world_gameplay_request.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/companion/companion_world.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <functional>

namespace chronicles::companion {

inline constexpr std::uint16_t kItemUseRequest = world_request::kItemUseRequest;
inline constexpr std::uint16_t kInteractUnitRequest = world_request::kInteractionUnitRequest;
inline constexpr std::uint16_t kInteractUnitResponse = 163;
inline constexpr std::uint16_t kItemsUpdateNotification = 169;
inline constexpr std::uint16_t kSystemMessageNotification = 3;

// ESystemMessage keys the client shows for house-mate summons.
inline constexpr int kMateSpawnSuccessMessage = 38;
inline constexpr int kMateSpawnNoMateMessage = 39;
inline constexpr int kMateSpawnLimitMessage = 40;
inline constexpr int kMateSpawnSpaceMessage = 41;

// Emulator policy: one spawned mate per character at a time (SM_HOUSEMATE_SPAWN_FAIL_LIMIT).
inline constexpr std::size_t kMaximumSpawnedMates = 1;

struct CompanionServices {
  std::shared_ptr<CompanionWorld> world;
  std::shared_ptr<const CompanionCatalog> catalog;
  // Item definitions; optional (registration consumables are stacks by Item.category).
  std::shared_ptr<const ShopCatalog> items;
};

struct SessionResult {
  // Packets for this connection, sent right after the frame's response.
  std::vector<GameplayPacket> own;
  // Shared unit packets for any connection, this one included.
  CompanionUpdate broadcast;
  // Packets for this connection that must follow its broadcast frames (for example
  // TradeStartNoti after the wagon's UnitAddNoti and MountUnitNoti).
  std::vector<GameplayPacket> trailing;
};

// A companion or trade GM command parsed from GmCmdReq / GmCmdTextReq.
enum class CompanionGmKind {
  register_summon,
  spawn_summon,
  despawn_summon,
  register_mate,
  add_mate_intimacy,
  spawn_mate,
  despawn_mate,
  invite_mate,
  spawn_operable,
  damage_vehicle,
  trade_voucher_add,
};

struct CompanionGmCommand {
  CompanionGmKind kind{CompanionGmKind::register_summon};
  int cms_id{};
  int value{};
  std::uint64_t unit_id{};
  // True for the few commands that every player may run, not only game masters. Only `car`
  // sets it; everything else stays game-master only.
  bool player_command{false};
};

// The only vehicle of the client catalog (Vehicle id 200, a TradeWagon) is what `car` spawns.
inline constexpr int kDefaultCarVehicleId = 200;

// Result of a player chat command (`!mount`, `!dismount`): the unit packets to deliver and the
// private chat line that tells the player what happened.
struct PlayerCompanionOutcome {
  SessionResult result;
  std::string reply;
};

// Parses a GM request when every command in it is a companion or trade command; nullopt for
// any other request (the general GM handler then owns it).
std::optional<std::vector<CompanionGmCommand>>
parse_companion_gm_request(std::uint16_t opcode, const nlohmann::json &message);

class CompanionSession {
public:
  CompanionSession(CompanionServices services, std::unique_ptr<ProgressAccess> progress,
                   std::uint64_t connection_id, std::int64_t character_id);

  // True for requests this module owns. ItemUseReq and InteractUnitReq are owned only when they
  // name a summon-registering consumable or a RepairWagon interaction.
  bool handles(std::uint16_t opcode, const nlohmann::json &message) const;

  bool handles_registration_animation(std::uint16_t opcode, const nlohmann::json &message) const;
  SessionResult receive_registration_animation(const WorldGameplayRequest &request,
                                               const std::optional<WorldPosition> &position,
                                               std::uint64_t player_unit_id, std::int64_t now,
                                               bool blocked);
  SessionResult advance_registration(std::int64_t now, const WorldPosition &position, bool blocked);
  std::optional<std::int64_t> registration_deadline() const;
  bool registration_busy() const noexcept;

  SessionResult receive(std::uint16_t opcode, const nlohmann::json &message, std::uint32_t sequence,
                        std::int64_t now_epoch_ticks);

  // Joins the shared companion world once the client entered the map, and sends the owned
  // summon and mate lists.
  SessionResult enter_world(const Participant &participant);

  SessionResult reenter_region(const WorldPosition &position, std::int64_t tick,
                               const CompanionViewToken &view = {});
  SessionResult suspend_region(const WorldPosition &destination);

  // Mirrors a client move; `movement` is the decoded MoveReq (null for server teleports).
  SessionResult move(const WorldPosition &position, const nlohmann::json *movement);

  // The character entered another instance copy (InstanceCopy::channel_key) or a shared map (0).
  SessionResult set_channel(std::uint64_t channel_key);

  bool entered() const;

  bool riding() const;

  // Receive durable death/revival transitions from the session's life event. Ordinary combat
  // snapshots are not enough: dismounting must follow the successful persistence commit.
  void observe_committed_life(bool dead) noexcept;

  // Called after combat/death persistence on the session strand, before packet delivery.
  // This hot path uses the entry snapshot and committed life events, never storage reads.
  SessionResult dismount_if_dead(std::int64_t tick);

  // Leaves the shared world: owned units are removed, spawned mates are recorded as away.
  // Returns the vehicles that left so the trade run can persist them.
  DetachResult leave(
      const std::function<void(const std::vector<VehicleSnapshot> &)> &before_commit = {});
  DetachResult leave_retained(
      const std::function<void(const std::vector<VehicleSnapshot> &,
                               const std::shared_ptr<CompanionWorld::RetainedDetach> &)>
          &before_commit);

  // Executes authorized GM companion commands (the caller checked the game-master role) at the
  // GM's current position. Wagon damage and voucher grants belong to the trade session.
  SessionResult gm_command(const std::vector<CompanionGmCommand> &commands,
                           const WorldPosition &position, std::int64_t now_epoch_ticks);

  // `!mount`, open to every character: summons and seats the character's horse. A registered
  // summon is preferred; a character without one borrows the catalog's default horse without
  // registering it. Refused while riding, dead or outside the shared world.
  PlayerCompanionOutcome player_mount(std::int64_t tick);

  // `!dismount`, open to every character: gets off the ridden horse or vehicle at the unit's
  // position, exactly like a client GetOffUnitReq without a landing point.
  PlayerCompanionOutcome player_dismount(std::int64_t tick);

  // The owned-summon and owned-mate snapshots for `progress`.
  static std::vector<GameplayPacket> snapshot_packets(const WorldProgress &progress);

private:
  DetachResult finish_leave(DetachResult detached);
  SessionResult spawn_summon(const nlohmann::json &message, std::int64_t tick);
  SessionResult ride(const nlohmann::json &message, std::int64_t tick);
  SessionResult get_off(const nlohmann::json &message, std::int64_t tick);
  SessionResult use_item(const nlohmann::json &message, std::uint32_t sequence);
  SessionResult repair(const nlohmann::json &message, std::uint32_t sequence, std::int64_t tick);
  SessionResult spawn_mate(const nlohmann::json &message);
  SessionResult despawn_mate(const nlohmann::json &message, std::uint32_t sequence);
  SessionResult invite_mate(const nlohmann::json &message, std::uint32_t sequence);

  SessionResult register_summon(int summon_npc_cms_id);
  SessionResult register_mate(int mate_npc_cms_id);
  SessionResult add_mate_intimacy(int mate_npc_cms_id, int value);

  // The summon `!mount` uses: the lowest registered rideable summon, else the catalog default.
  std::optional<int> player_mount_summon_id(const WorldProgress &progress) const;

  // Commits a mate state change; false when storage refused it.
  bool commit_mate_state(int mate_npc_cms_id, MateState state, int housing_lot_number);

  std::optional<std::vector<CharacterItem>> commit(const WorldProgress &before,
                                                   const WorldProgress &after,
                                                   const ItemTrade &trade = {});

  CompanionServices services_;
  struct RegistrationAnimation {
    int item_cms_id{};
    std::uint32_t sequence{};
    nlohmann::json packet_sequence;
    std::uint64_t player_unit_id{};
    WorldPosition position;
    std::int64_t started{};
    bool executed{};
  };

  GameplayPacket registration_started() const;
  GameplayPacket registration_finished(const char *error = "Success") const;
  std::optional<RegistrationAnimation> registration_;
  std::unique_ptr<ProgressAccess> progress_;
  std::uint64_t connection_id_{};
  std::int64_t character_id_{};
  bool entered_{};
  bool committed_dead_{};
};

GameplayPacket common_error(std::uint32_t sequence, x5::ec::EErrorCode code);

GameplayPacket system_message(int key);

} // namespace chronicles::companion
