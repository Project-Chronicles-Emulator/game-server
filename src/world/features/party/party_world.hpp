#pragma once

// World-transport integration of chronicles::party::PartyService.
//
// PartyWorld binds the transport-free party registry to authenticated world
// sessions. It owns the online-member directory (character -> connection,
// display fields, live vitals), decodes the PC2S party requests, builds the
// PS2C payloads, and produces the periodic member world-state sync. It never
// touches sockets, sessions or the protocol codec: callers encode and deliver
// the returned packets. Thread-safe; every public method takes one internal
// lock. The bounded kill-snapshot and disconnect-staging callbacks run under that lock;
// they must not perform I/O or call back into PartyWorld.
//
// Evidence:
// - Opcodes from protocol/opcodes.json: PC2S 20094 Kick, 20095 Leave,
//   20096 ChangeLeader, 20097 InvitePopup, 20098 AcceptInvite,
//   20099 RejectInvite; PS2C 184 Join, 185 Leave, 186 MemberUpdate,
//   187 ChangePartyUnit, 188 InvitePopup, 189 RemoveTemporary,
//   190 RejectInvite, 192 MemberWorldState; PS2C 2 CommonErrorRes.
// - The client registers a world (PS2C) packet factory for PartyJoinNoti at
//   slot 184 (AAC_Net_WorldPacketFactory_RegisterPartyJoinNoti, see
//   dumps/ida_research/party-20261026). Official world captures contain no
//   roster notifications, so the official server may prefer the WSF2C
//   variants; the world transport is the emulator's Candidate choice.
// - Official captures show ChangePartyUnitNoti {unit_id, party_id} for the
//   player's own unit and for observed player units, with party_id omitted
//   when the unit has no party, and PlayerNub.party_id on grouped players.
// - Catalog GameVariable: PartyMaxMemberNum=4, PartyInviteTimeoutMillis=20000,
//   PartyMemberWorldStateSyncMillis=10000, PartyUnlockQuestId=90100011
//   (paired with ErrorCode Party_UnlockPartyContentRequired 3357).

#include "world/features/party/party.hpp"
#include "persistence/character_state_types.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles {
class ClientCatalog;
namespace quests {
class QuestIntentLedger;
}
} // namespace chronicles

namespace chronicles::party {

inline constexpr std::uint16_t kPartyKickRequest = 20094;
inline constexpr std::uint16_t kPartyLeaveRequest = 20095;
inline constexpr std::uint16_t kPartyChangeLeaderRequest = 20096;
inline constexpr std::uint16_t kPartyInvitePopupRequest = 20097;
inline constexpr std::uint16_t kPartyAcceptInviteRequest = 20098;
inline constexpr std::uint16_t kPartyRejectInviteRequest = 20099;

inline constexpr std::uint16_t kCommonErrorResponse = 2;
inline constexpr std::uint16_t kPartyJoinNotification = 184;
inline constexpr std::uint16_t kPartyLeaveNotification = 185;
inline constexpr std::uint16_t kPartyMemberUpdateNotification = 186;
inline constexpr std::uint16_t kChangePartyUnitNotification = 187;
inline constexpr std::uint16_t kPartyInvitePopupNotification = 188;
inline constexpr std::uint16_t kPartyRemoveTemporaryNotification = 189;
inline constexpr std::uint16_t kPartyRejectInviteNotification = 190;
inline constexpr std::uint16_t kPartyMemberWorldStateNotification = 192;

struct PartyWorldConfig {
  PartyConfig service;
  // GameVariable PartyUnlockQuestId. Inviting, being invited and accepting
  // require this quest to be complete. Zero disables the gate.
  int unlock_quest_id = 0;
  // GameVariable PartyMemberWorldStateSyncMillis: period of the
  // PartyMemberWorldStateNoti refresh for every connected member.
  std::chrono::milliseconds world_state_interval{10000};
};

// Reads the evidenced GameVariable rows. Missing or out-of-range rows keep the
// defaults above and are logged; the catalog is never required.
PartyWorldConfig load_party_world_config(ClientCatalog &catalog);

// True when `progress` records `quest_id` as complete. Zero means no gate.
bool party_unlocked(const WorldProgress &progress, int quest_id);

// Snapshot a world session publishes about its own character. Display fields
// come from the selected character, vitals from authoritative session state;
// nothing here is read from client request payloads.
struct MemberPresence {
  CharacterId char_id = 0;
  std::uint64_t connection_id = 0;
  std::string name;
  std::int32_t level = 0;
  std::int32_t char_class = 0;
  // Two members share a world instance only when both this key and the map
  // match. Private tutorial instances use the owning character ID; shared
  // maps use zero.
  std::uint64_t channel_key = 0;
  std::optional<WorldPosition> position;
  std::int32_t hp = 0;
  std::int32_t max_hp = 0;
  bool party_unlocked = false;
  std::int64_t account_id = 0;
  int world_id = 1;
  std::shared_ptr<quests::QuestIntentLedger> quest_ledger;
};

// One decoded payload addressed to a world connection.
struct OutboundPacket {
  std::uint64_t connection_id = 0;
  std::uint16_t opcode = 0;
  nlohmann::json message;
};

// A player unit whose visible party changed. The transport sends
// ChangePartyUnitNoti to the subject and to every connection observing it.
struct UnitPartyChange {
  std::uint64_t subject_connection_id = 0;
  CharacterId char_id = 0;
  // Zero means the unit no longer belongs to an established party.
  PartyId party_id = 0;
};

struct PartyUpdate {
  std::vector<OutboundPacket> packets;
  std::vector<UnitPartyChange> unit_changes;

  void append(PartyUpdate other);
};

class PartyWorld {
public:
  explicit PartyWorld(PartyWorldConfig config);

  // PC2S party membership requests handled by receive(). Channel invites
  // (20100-20103) are deliberately excluded: no channel travel exists.
  static bool handles(std::uint16_t opcode);

  int unlock_quest_id() const;

  // Registers an in-world session. A character holding a retained seat is
  // restored: it receives a fresh PartyJoinNoti and the others a roster
  // update showing it Connected again.
  PartyUpdate attach(const MemberPresence &presence, TimePoint now);

  // Replaces position, vitals and unlock state of an attached session.
  void refresh(const MemberPresence &presence);

  // Session closed. Party seats are retained (connection_state Retain) until
  // the member returns or PartyConfig::retain_ttl elapses. Ignores stale
  // connections so a newer session for the same character is not detached.
  PartyUpdate detach(CharacterId char_id, std::uint64_t connection_id, TimePoint now);
  // Retain the staged update in the callback; publish it only after this call succeeds.
  // The callback runs under the party lock and must not perform I/O or reenter PartyWorld.
  PartyUpdate detach_staged(CharacterId char_id, std::uint64_t connection_id, TimePoint now,
                            const std::function<void(const PartyUpdate &)> &before_commit);

  // Handles one PC2S party request from the authenticated requester. Refusals
  // produce a CommonErrorRes addressed to `connection_id`.
  PartyUpdate receive(CharacterId requester, std::uint64_t connection_id, std::uint16_t opcode,
                      const nlohmann::json &message, std::uint32_t sequence, TimePoint now);

  // Expires invites and retained seats and emits the periodic world-state
  // sync. Call about once per second while active().
  PartyUpdate tick(TimePoint now);

  // True while any party or pending invite needs tick().
  bool active() const;

  // Connections of the connected members of `member`'s established party,
  // including the member's own. Empty when not in an established party.
  std::vector<std::uint64_t> party_connections(CharacterId member) const;

  // Membership as map entrance conditions test it (EX5PartyStateType).
  enum class Membership { none, leader, member };
  Membership membership(CharacterId member) const;

  // The member's established party, which selects its instance and dungeon copies.
  std::optional<PartyId> established_party(CharacterId member) const;

  // A connected party member that shares a kill's instance.
  struct NearbyMember {
    CharacterId char_id = 0;
    std::uint64_t connection_id = 0;
    WorldPosition position;
    std::int64_t account_id = 0;
    int world_id = 1;
    bool alive = false;
    std::shared_ptr<quests::QuestIntentLedger> quest_ledger;
  };

  // Connected members of `member`'s established party, excluding `member`, whose last published
  // position is on `position`'s map, in the same instance (`channel_key`) and within `range_mm`.
  // Empty when `member` is not in an established party.
  std::vector<NearbyMember> nearby_party_members(CharacterId member, const WorldPosition &position,
                                                 std::uint64_t channel_key, double range_mm) const;

  // Capture membership and quest-order tokens as one ordered operation. The callback runs under
  // the party lock and must not perform I/O, wait on workers, or reenter this PartyWorld.
  void capture_nearby_party_members(
      CharacterId member, const WorldPosition &position, std::uint64_t channel_key,
      double range_mm,
      const std::function<void(const std::optional<MemberPresence> &,
                               const std::vector<NearbyMember> &)> &capture) const;

  // Connection of an online member, if any.
  std::optional<std::uint64_t> connection_of(CharacterId member) const;

  // Last published presence (display fields and vitals) of an online member, if any.
  std::optional<MemberPresence> online_presence(CharacterId member) const;

  // Sets nub.player.party_id on a PS2C_UnitAddNoti payload whose unit is a
  // grouped player (player unit IDs equal character IDs).
  void decorate_unit_add(nlohmann::json &unit_add_message) const;

  static nlohmann::json change_party_unit_message(const UnitPartyChange &change);

  // CommonErrorRes payload naming the recovered EErrorCode for `error`.
  static nlohmann::json error_message(PartyError error, std::uint32_t sequence);

private:
  friend struct PartyWorldTestAccess;

  struct KnownMember {
    MemberPresence presence;
    bool online = false;
  };

  PartyResult dispatch(CharacterId requester, std::uint16_t opcode,
                       const nlohmann::json &message, TimePoint now);
  PartyResult invite(CharacterId requester, const nlohmann::json &message, TimePoint now);
  PartyResult accept(CharacterId requester, const nlohmann::json &message, TimePoint now);
  bool unlocked(CharacterId member) const;
  const KnownMember *online_member(CharacterId member) const;
  std::vector<NearbyMember> nearby_party_members_locked(
      CharacterId member, const WorldPosition &position, std::uint64_t channel_key,
      double range_mm) const;
  std::optional<MemberDescriptor> describe(CharacterId member) const;
  void translate(const std::vector<PartyEvent> &events, PartyUpdate &update) const;
  void collect_unit_changes(PartyUpdate &update);
  void append_world_states(PartyUpdate &update) const;
  void prune_offline_members();

  mutable std::mutex mutex_;
  PartyWorldConfig config_;
  // Online members plus offline members that still hold a party seat, so a
  // retained roster entry keeps its name, level and class.
  std::unordered_map<CharacterId, KnownMember> known_;
  // Last party_id announced through ChangePartyUnitNoti per online member.
  std::unordered_map<CharacterId, PartyId> announced_party_;
  PartyService service_;
  TimePoint next_world_state_sync_{};
  // Set by roster changes so the next tick refreshes vitals immediately
  // instead of waiting for the periodic interval.
  bool world_state_sync_due_ = false;
};

} // namespace chronicles::party
