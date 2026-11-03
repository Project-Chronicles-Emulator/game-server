#pragma once

// Transport binding of HousingService.
//
// HousingHub connects the transport-free service to world sessions and front WebSocket
// connections. It keeps the directory of in-world characters (their session port, player unit
// ID and housing channel), decodes PC2S housing requests, builds the PS2C payloads, and fans an
// operation's HousingOutcome out to the characters that must see it. It never touches sockets
// or the protocol codec: sessions encode and send the WorldPackets they are given.
//
// Evidence (dumps/ida_research/housing-20261026, protocol/opcodes.json):
// - World opcodes: PC2S 20040 HousingEditModeReq, 20042 UpdateHousingSiteContractExtensionReq,
//   20043 HousingMovePackageUnpackReq, 20044 HousingMovePackageRecoverToStorageReq,
//   20045 UpdateHousingReq, 20046 HousingItemDiscardReq, 20083 GoHomeReq; PS2C 83-89, 131,
//   199 HousingSiteContractNoti, 200 HousingAddNoti, 202 UpdateHousingRes,
//   203 HousePartsUpdateRes.
// - AAC_PS2C_HousingAddNoti_Handle creates the housing actor from the client's static
//   HousingSite row for nub.lot_number, so the nub needs no world position.
// - The client completes a channel visit only after both WSF2C_HousingChannelVisitRes and a
//   MapMoveCompleteNoti into a map of type HousingArea (AAC_Housing_OnMapMoveComplete), so a
//   visit moves the character with an in-connection map move.
// - UpdateHousingRes with house_part_custom_ids set applies only the customization
//   (AAC_PS2C_UpdateHousingRes_Handle); objects travel in a separate response.

#include "world/features/housing/housing_service.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles::housing {

inline constexpr std::uint16_t kEditModeRequest = 20040;
inline constexpr std::uint16_t kContractExtensionRequest = 20042;
inline constexpr std::uint16_t kPackageUnpackRequest = 20043;
inline constexpr std::uint16_t kPackageRecoverRequest = 20044;
inline constexpr std::uint16_t kUpdateHousingRequest = 20045;
inline constexpr std::uint16_t kItemDiscardRequest = 20046;
inline constexpr std::uint16_t kGoHomeRequest = 20083;

inline constexpr std::uint16_t kCommonErrorResponse = 2;
inline constexpr std::uint16_t kUnitRemoveNotification = 11;
inline constexpr std::uint16_t kEditModeNotification = 83;
inline constexpr std::uint16_t kCharSiteUpdateNotification = 84;
inline constexpr std::uint16_t kContractExtensionResponse = 85;
inline constexpr std::uint16_t kPackageUnpackResponse = 86;
inline constexpr std::uint16_t kPackageRecoverResponse = 87;
inline constexpr std::uint16_t kPackageCreatedNotification = 88;
inline constexpr std::uint16_t kGuildSiteUpdateNotification = 89;
inline constexpr std::uint16_t kWorldCharHousingItemsNotification = 131;
inline constexpr std::uint16_t kItemsUpdateNotification = 169;
inline constexpr std::uint16_t kSiteContractNotification = 199;
inline constexpr std::uint16_t kHousingAddNotification = 200;
inline constexpr std::uint16_t kUpdateHousingResponse = 202;
inline constexpr std::uint16_t kHousePartsUpdateResponse = 203;
inline constexpr std::uint16_t kStartInteractionAnimResponse = 165;
inline constexpr std::uint16_t kStartInteractionAnimNotification = 166;
inline constexpr std::uint16_t kInteractionAnimResponse = 167;
inline constexpr std::uint16_t kChangeDoodadPhaseNotification = 194;

struct WorldPacket {
  std::uint16_t opcode{};
  nlohmann::json message;
};

// HousingNub and its children (x5.HousingNub, x5.HouseNub, x5.HousingDoodadNub).
nlohmann::json house_nub_json(const HouseRecord &house);
nlohmann::json doodad_nub_json(const DoodadRecord &doodad);
nlohmann::json housing_nub_json(const SiteRecord &site);

// x5.pb.HousingSite as the owner sees it in CharHousingSiteUpdateNoti and WorldCharacter.
nlohmann::json housing_site_json(const SiteRecord &site);

WorldPacket housing_add_packet(const SiteRecord &site, const HousingCatalog &catalog);
WorldPacket site_contract_packet(const SiteRecord &site, const HousingCatalog &catalog);
WorldPacket site_removed_packet(const SiteKey &site);
WorldPacket site_update_packet(const SiteUpdateNotice &notice);
WorldPacket parts_update_packet(const PartsUpdateNotice &notice);
// An absent site reports authority None, clearing the client's site record.
WorldPacket owner_site_packet(const std::optional<SiteRecord> &site);
WorldPacket guild_site_packet(const SiteRecord &site, GuildHousingAction action);
WorldPacket package_created_packet(PackageReason reason);
// ItemsUpdateNoti (reason housing) with the stack, money and housing storage rows that changed.
std::optional<WorldPacket> progress_update_packet(const ProgressNotice &notice);
// WorldCharHousingItemsNoti with every owned housing storage row (entry snapshot).
std::optional<WorldPacket> housing_items_packet(const WorldProgress &progress);
std::optional<WorldPacket> housing_items_packet(const std::map<int, HousingItemRecord> &items);
WorldPacket error_packet(ErrorCode code, std::uint32_t sequence);

// HousingAddNoti for every occupied lot of a channel, in lot order.
std::vector<WorldPacket> channel_snapshot(const HousingService &service, int zone_key,
                                          int channel);

// Decodes a PC2S UpdateHousingReq payload (protobuf JSON). Throws std::invalid_argument when a
// field has the wrong type or range.
UpdateRequest parse_update_request(const nlohmann::json &message);

// A world session as the hub sees it. Implementations post to the session's own executors and
// never block the caller.
class SessionPort {
public:
  virtual ~SessionPort() = default;

  // Queues packets for this session's client in order.
  virtual void deliver(std::vector<WorldPacket> packets) = 0;

  // Runs `task` on the session's worker strand with its live progress. Returns false when the
  // session can no longer run tasks.
  virtual bool post(std::function<void(const ProgressAccess &)> task) = 0;

  // Starts an in-connection map move to `destination`. Called on the session's worker strand.
  virtual void travel(const HousingDestination &destination) = 0;
};

// The outcome of the housing GM commands in one GM request.
struct GmHousingReply {
  // Packets for the requesting GM in order: refusals and its own notifications.
  std::vector<WorldPacket> packets;
  // One line per executed or refused command. No GM reply packet was recovered, so the world
  // session relays these lines as private [GM] chat.
  std::vector<std::string> messages;
};

// The housing channel a character currently stands in.
struct HousingLocation {
  int zone_key{};
  int channel{};

  bool operator==(const HousingLocation &) const = default;
};

// An object placed on a lot of the character's current housing channel, as other world
// services (crafting stations) need it. Positions are world millimetres.
struct HousingPlacedDoodad {
  int doodad_cms_id{};
  int lot_centre_x_mm{};
  int lot_centre_y_mm{};
  int lot_centre_z_mm{};
  // Horizontal distance from the lot centre to a lot corner. Every object of the lot stands
  // within it.
  int lot_radius_mm{};
};

// The SP drain announced for one garden interaction; same fields as the gathering flow. Wire
// ticks use the client's nanosecond monotonic clock.
struct HousingStaminaFlow {
  int cost{};
  std::int64_t wire_start_ns{};
  int execute_ms{};
  // SP regeneration resumes after the animation plus SPStatRegenDelayMilli.
  int regen_resume_ms{};
  std::int64_t server_ticks{};
};

// Player vitals owned by the world session. Every callback is optional: without them a garden
// interaction runs without SP checks or drains.
struct HousingVitals {
  std::function<bool()> dead;
  // Returns the packets announcing the drain (possibly none), or nullopt when the player lacks
  // the SP.
  std::function<std::optional<std::vector<WorldPacket>>(const HousingStaminaFlow &)>
      begin_stamina;
  // Deducts the announced cost once at execution.
  std::function<void(std::int64_t server_ticks)> spend_stamina;
  // Stops an unexecuted drain and returns the correcting packet, if one is needed.
  std::function<std::optional<WorldPacket>(std::int64_t server_ticks)> cancel_stamina;
};

class HousingHub {
public:
  using GuildAreaListener = std::function<void(const GuildAreaNotice &notice)>;

  explicit HousingHub(std::shared_ptr<HousingService> service,
                      std::chrono::milliseconds session_task_timeout = std::chrono::seconds(5));

  HousingService &service();

  const HousingCatalog &catalog() const;

  // Set once at startup by the front adapter; guild-area changes reach members through it.
  void set_guild_area_listener(GuildAreaListener listener);

  // A world session entered the world (StartPlaying) or moved. Location is empty outside a
  // housing area. Stale connection IDs never replace a newer session.
  void attach(const HousingActor &actor, std::uint64_t connection_id, std::uint64_t unit_id,
              std::weak_ptr<SessionPort> port);

  void detach(CharacterId character_id, std::uint64_t connection_id);

  void set_location(CharacterId character_id, std::uint64_t connection_id,
                    std::optional<HousingLocation> location);

  std::optional<HousingLocation> location_of(CharacterId character_id) const;

  // The object that a housing doodad unit ID names on a lot of the character's current channel,
  // when that channel's HousingArea map is map_id. Returns nothing for other unit IDs, other
  // channels and objects that no longer exist.
  std::optional<HousingPlacedDoodad> placed_doodad(CharacterId character_id,
                                                   std::uint64_t unit_id, int map_id) const;

  // True for the PC2S opcodes handled by receive().
  static bool handles(std::uint16_t opcode);

  // Handles one PC2S housing request on the requester's session strand. Returns the packets for
  // the requester in order; packets for other characters are delivered through their ports.
  std::vector<WorldPacket> receive(const HousingActor &actor, std::uint64_t unit_id,
                                   std::uint16_t opcode, const nlohmann::json &message,
                                   std::uint32_t sequence, const ProgressAccess &access);

  // StartInteractionAnimReq.param.change_house_part_state. Returns nothing when the request is
  // not a housing door or window toggle.
  std::optional<std::vector<WorldPacket>> receive_part_toggle(const HousingActor &actor,
                                                              const nlohmann::json &message,
                                                              std::uint32_t sequence);

  // Furniture poses such as sitting on a chair. StartInteractionAnimReq (20069) aimed at a
  // placed housing doodad answers StartInteractionAnimNoti to the requester and the channel;
  // a refusal answers StartInteractionAnimRes with the error. The EndInteractionAnimReq (20071)
  // that finishes the active pose answers InteractionAnimRes to the requester and the channel.
  // Returns nothing for any other target or animation, so the statue, processing and gathering
  // handlers still receive those requests.
  //
  // Candidate wire shape: no official furniture capture exists. The packets mirror the captured
  // gathering flow (Noti echoes the request's param, position, yaw and tick) and add the
  // furniture as target_unit_id so viewers can attach the pose to it.
  std::optional<std::vector<WorldPacket>> receive_furniture_pose(const HousingActor &actor,
                                                                 std::uint16_t opcode,
                                                                 const nlohmann::json &message,
                                                                 std::uint32_t sequence);

  // Garden plots and interactive decorations (HousingDoodadProfile): StartInteractionAnimReq
  // (20069) on a profiled housing doodad, the ExecuteInteractionReq (20070) that completes it
  // and the EndInteractionAnimReq (20071) that finishes or cancels it. The flow mirrors
  // captured doodad gathering: the start answers [SP flow] StartInteractionAnimNoti; the
  // execute answers ItemsUpdateNoti (rewards and spent seeds), ChangeDoodadPhaseNoti and
  // InteractionAnimRes; refusals answer StartInteractionAnimRes or the common error.
  // `monotonic_ticks` is the session's 100 ns monotonic clock. Returns nothing for other
  // targets and for execute/end without an open garden action.
  std::optional<std::vector<WorldPacket>>
  receive_doodad_use(const HousingActor &actor, std::uint16_t opcode,
                     const nlohmann::json &message, std::uint32_t sequence,
                     const ProgressAccess &access, const HousingVitals &vitals,
                     std::int64_t monotonic_ticks);

  // A player who is still seated keeps the pose for viewers that see its unit later: given the
  // UnitAddNoti message of a player unit, returns that player's StartInteractionAnimNoti.
  std::optional<WorldPacket> late_viewer_pose(const nlohmann::json &unit_add_message) const;

  // The character's latest position. A seated character that moved away from its seat has left
  // it, so its held pose is no longer replayed.
  void observe_movement(CharacterId character_id, const WorldPosition &position);

  // GM housing commands from PC2S_GmCmdReq (20008: GmCmd fields 56, 74, 96, 103, 104, 115,
  // 116, 123, 126) or PC2S_GmCmdTextReq (20009) lines named after those fields. The text form
  // also accepts the emulator-defined `housing_contract <small|medium|large>
  // [monthly|periodic]`, which rents a personal lot like WSC2F_HousingSiteContractReq.
  // Returns nothing when the request holds no housing command, leaving it to the generic GM
  // parser. The caller authorizes the GM role.
  std::optional<GmHousingReply> receive_gm(const HousingActor &actor, std::uint16_t opcode,
                                           const nlohmann::json &message,
                                           std::uint32_t sequence,
                                           const ProgressAccess &access);

  // Runs `operation` for a character: on its session strand with live progress when it is in
  // the world, otherwise directly against storage. Returns false when the session did not run
  // the operation before the task timeout; the operation is then skipped. Once a task starts,
  // the caller waits for its actual result to keep captured references alive.
  bool run_for(CharacterId character_id,
               const std::function<void(const ProgressAccess *)> &operation);

  // Moves an in-world character to `destination`. Returns false when it is not in the world.
  bool travel(CharacterId character_id, const HousingDestination &destination);

  // Fans an outcome out. `adopted_character` is the character whose session already adopted
  // its progress during the operation; other changed characters' sessions adopt asynchronously.
  // Packets addressed to `local_character` are appended to `local` (when given) so the
  // requester sees them after its own response.
  void publish(const HousingOutcome &outcome, CharacterId adopted_character = 0,
               CharacterId local_character = 0, std::vector<WorldPacket> *local = nullptr);

  // Renews or packs every due site and guild area, after first reconciling guild housing with
  // the guild registry. Call about once a minute.
  void expire_due();

  // GuildService events, delivered off the guild thread: a member who left loses the guild
  // site, a disbanded guild loses its area. May wait for the owners' session strands.
  // Returns false when the cleanup did not complete; the failure is logged and
  // reconcile_guild_housing() retries it. Never throws.
  bool guild_member_left(GuildId guild_id, CharacterId character_id, bool kicked = false);

  bool guild_disbanded(GuildId guild_id);

  // Releases guild housing whose guild membership has already ended: sites of departed
  // members and areas of disbanded guilds. Guild changes commit before their housing cleanup
  // runs, so a cleanup lost to a storage failure, a session timeout or a server stop is
  // repaired here from the guild registry. Idempotent; call at startup and periodically.
  // Returns the number of cleanups that still failed. Never throws.
  std::size_t reconcile_guild_housing();

private:
  // An open garden or decoration interaction between its start and its end.
  struct DoodadUse {
    DoodadUnitKey target;
    std::uint64_t target_unit_id{};
    int interaction_id{};
    int animation_id{};
    int phase{};
    std::int64_t started_ticks{};
    bool executed{};
    bool stamina_flow{};
  };

  struct Member {
    HousingActor actor;
    std::uint64_t connection_id{};
    std::uint64_t unit_id{};
    std::weak_ptr<SessionPort> port;
    std::optional<HousingLocation> location;
    // Animation of the furniture pose whose EndInteractionAnimReq is still expected.
    std::optional<int> pose_animation_id;
    // The StartInteractionAnimNoti of a pose that holds after its end (sitting), replayed to
    // viewers who see this player later.
    std::optional<WorldPacket> held_pose;
    std::optional<DoodadUse> doodad_use;
  };

  std::vector<WorldPacket> dispatch(const HousingActor &actor, std::uint64_t unit_id,
                                    std::uint16_t opcode, const nlohmann::json &message,
                                    std::uint32_t sequence, const ProgressAccess &access);

  std::optional<std::vector<WorldPacket>> start_furniture_pose(const HousingActor &actor,
                                                               const nlohmann::json &message,
                                                               std::uint32_t sequence);

  std::optional<std::vector<WorldPacket>> end_furniture_pose(const HousingActor &actor,
                                                             const nlohmann::json &message);

  std::vector<WorldPacket> start_doodad_use(const HousingActor &actor, const DoodadUnitKey &target,
                                            std::uint64_t target_unit_id,
                                            const nlohmann::json &message,
                                            std::uint32_t sequence, const ProgressAccess &access,
                                            const HousingVitals &vitals,
                                            std::int64_t monotonic_ticks);

  std::optional<std::vector<WorldPacket>>
  execute_doodad_use(const HousingActor &actor, const nlohmann::json &message,
                     std::uint32_t sequence, const ProgressAccess &access,
                     const HousingVitals &vitals, std::int64_t monotonic_ticks);

  std::optional<std::vector<WorldPacket>> end_doodad_use(const HousingActor &actor,
                                                         const nlohmann::json &message,
                                                         const HousingVitals &vitals,
                                                         std::int64_t monotonic_ticks);

  // The player's InteractionAnimRes, sent to the player and the channel.
  WorldPacket interaction_result(std::uint64_t player_unit_id) const;

  // ChangeDoodadPhaseNoti for every notice, delivered to the notice's channel.
  void publish_doodad_phases(const std::vector<DoodadPhaseNotice> &notices,
                             CharacterId local_character, std::vector<WorldPacket> *local);

  // Sends `packet` to every other character standing in `location`.
  void send_to_channel(const HousingLocation &location, CharacterId except,
                       const WorldPacket &packet);

  SimpleResult execute_gm(const HousingActor &actor, const std::string &command,
                          const nlohmann::json &arguments, const ProgressAccess &access);

  // The chat line for a GM housing command that succeeded.
  std::string gm_success_message(const HousingActor &actor, const std::string &command) const;

  std::vector<CharacterId> viewers(const SiteKey &site) const;

  std::shared_ptr<SessionPort> port_of(CharacterId character_id) const;

  void send(CharacterId character_id, std::vector<WorldPacket> packets,
            CharacterId local_character, std::vector<WorldPacket> *local);

  std::shared_ptr<HousingService> service_;
  std::chrono::milliseconds session_task_timeout_;
  mutable std::mutex mutex_;
  std::unordered_map<CharacterId, Member> members_;
  GuildAreaListener guild_area_listener_;
};

} // namespace chronicles::housing
