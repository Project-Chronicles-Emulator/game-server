#pragma once

// Server-authoritative party membership service.
//
// Protocol evidence (see docs/protocol-feature-reference.md "Parties" section and
// ClientData_2026-10-23/Protocol/proto/p_c2s.party.proto, p_s2c.party.proto,
// p_party.proto):
//   PC2S_PartyInvitePopupReq  20097  {1: target_char_id sfixed64}
//   PC2S_PartyAcceptInviteReq 20098  {1: leader_char_id, 2: target_char_id, 3: party_id}
//   PC2S_PartyRejectInviteReq 20099  {1: leader_char_id, 2: target_char_id}
//   PC2S_PartyKickReq         20094  {1: target_char_id sfixed64}
//   PC2S_PartyLeaveReq        20095  {} (empty body)
//   PC2S_PartyChangeLeaderReq 20096  {1: target_char_id sfixed64}
//   PS2C/WSF2C PartyJoinNoti         {party_id, leader_char_id, members[]}
//   PS2C/WSF2C PartyLeaveNoti        {party_id, kicked}
//   PS2C/WSF2C PartyMemberUpdateNoti {party_id, members[], leave_char_ids[],
//                                     kicked_char_ids[], leader_char_id,
//                                     disband_panding}
//   PS2C/WSF2C PartyInvitePopupNoti  {party_id, leader, target}
//   PS2C/WSF2C PartyRejectInviteNoti {party_id, target}
//   PS2C/WSF2C PartyRemoveTemporaryNoti {party_id}
//   PartyMember fields: char_id, name, level, char_class, connection_state,
//                       map_cms_id, zone_key. connection_state uses
//   EX5PartyMemberConnStateType: None=0, Connected=1, Retain=2 (disconnected,
//   membership retained).
//
// Capture evidence (protocol/demo-capture-observations.json): PartyInvitePopupReq
// observed 0/2/1 and PartyChannelInvite 0/0/1 (C2S); ChangePartyUnitNoti 0/35/16
// (S2C). Accept/leave/kick/leader transitions are CANDIDATE: uncaptured,
// schema- and ErrorCode-name-derived only. Recovered ErrorCode names in
// Catalog/Tables/ErrorCode.jsonl (3300-3360) are the contract.
//
// Bounds: Catalog/Tables/GameVariable.jsonl PartyMaxMemberNum=4 is the only
// evidenced member cap and PartyInviteTimeoutMillis=20000 the evidenced invite
// lifetime; both are runtime options, not hardcoded. ErrorCode 3309
// (LimitRequestCount) proves rate limiting exists; its window/count and the
// offline-seat retention time are SERVER POLICY CONSTANTS, not observed values.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chronicles::party {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using CharacterId = std::int64_t;
using PartyId = std::int64_t;

// Mirror of EX5PartyMemberConnStateType (cms.generated.proto). Retain means the
// member is offline but keeps the party seat; reconnect restores it.
enum class ConnectionState : std::int32_t {
  kNone = 0,
  kConnected = 1,
  kRetain = 2,
};

// Failure codes mirror recovered names in Catalog/Tables/ErrorCode.jsonl.
// The numeric value is the catalog id; these are evidence-derived constants,
// not invented codes. kNone is a local success sentinel (no catalog row).
enum class PartyError : std::int32_t {
  kNone = 0,
  kNotJoined = 3300,            // Party_NotJoined
  kLeaderPrivilege = 3301,      // Party_LeaderPrivilege
  kInvitationDeclined = 3302,   // Party_InvitationDeclined
  kInAnotherParty = 3303,       // Party_InAnotherParty
  kNotExistsInvitee = 3304,     // Party_NotExistsInvitee
  kNotExistsMember = 3305,      // Party_NotExistsMember
  kAlreadyLeader = 3306,        // Party_AlreadyLeader
  kFullCapacity = 3307,         // Party_FullCapacity
  kAlreadyInvited = 3308,       // Party_AlreadyInvited
  kLimitRequestCount = 3309,    // Party_LimitRequestCount
  kInviteRequestNotFound = 3310,// Party_InviteRequestNotFound
  kInviteRequestExpired = 3311, // Party_InviteRequestExpired
  kInviteRequestLeaderLeft = 3312, // Party_InviteRequestLeaderLeft
  kNotExistsInviter = 3313,     // Party_NotExistsInviter
  kNotExistParty = 3315,        // Party_NotExistParty
  // Party_UnlockPartyContentRequired; enforced by the world layer against
  // GameVariable PartyUnlockQuestId, never by PartyService itself.
  kUnlockPartyContentRequired = 3357,
  kSameParty = 3360,            // Party_SameParty
};

// Fields of x5.pb.party.PartyMember. Optional fields are left unset by the
// resolver when unknown; the transport layer omits unset fields.
struct MemberDescriptor {
  CharacterId char_id = 0;
  std::string name;
  std::int32_t level = 0;
  std::int32_t char_class = 0;
  std::int32_t map_cms_id = 0;
  std::int64_t zone_key = 0;
  ConnectionState connection_state = ConnectionState::kConnected;
};

// Which PS2C_/WSF2C_ notification an event maps to. The transport chooses the
// world (PS2C) or front-websocket (WSF2C) framing; the payloads are identical.
enum class EventKind {
  kJoinNoti,           // PartyJoinNoti -> single recipient (the new member)
  kLeaveNoti,          // PartyLeaveNoti -> single recipient (the departed member)
  kMemberUpdateNoti,   // PartyMemberUpdateNoti -> every CONNECTED remaining
                       // member; recipients are filtered by the service so
                       // transport never needs a session liveness lookup
  kInvitePopupNoti,    // PartyInvitePopupNoti -> the invite target
  kRejectInviteNoti,   // PartyRejectInviteNoti -> addressed to the invitee
                       // whose popup must clear; also used (Candidate) to
                       // signal invite cancellation on expiry, leader leave
                       // and leader disconnect
  kRemoveTemporaryNoti,// PartyRemoveTemporaryNoti -> members of a dissolved
                       // temporary party
};

struct PartyEvent {
  EventKind kind = EventKind::kMemberUpdateNoti;
  PartyId party_id = 0;
  // Recipients in server terms (character ids). RECIPIENT CONTRACT: the
  // service only ever addresses members whose connection_state is Connected
  // (plus non-member parties such as invitees and leavers, which the caller
  // just acted on). The transport therefore only needs "is this character's
  // world session attached" — never a second party-liveness rule.
  std::vector<CharacterId> recipients;
  // PartyJoinNoti payload.
  CharacterId leader_char_id = 0;
  std::vector<MemberDescriptor> members;
  // PartyMemberUpdateNoti payload.
  std::vector<CharacterId> leave_char_ids;
  std::vector<CharacterId> kicked_char_ids;
  bool disband_pending = false;
  // PartyLeaveNoti payload.
  bool kicked = false;
  // PartyInvitePopupNoti / PartyRejectInviteNoti payload.
  MemberDescriptor leader;
  MemberDescriptor target;
};

struct PartyConfig {
  // Catalog GameVariable PartyMaxMemberNum=4. Kept configurable so tests and a
  // future catalog refresh can adjust without a code change.
  std::size_t max_members = 4;
  // Invite expiry: Party_InviteRequestExpired (3311) proves invites expire and
  // Catalog GameVariable PartyInviteTimeoutMillis=20000 supplies the duration.
  std::chrono::milliseconds invite_ttl{20000};
  // How long a disconnected member keeps a Retain seat before it is released
  // as an ordinary leave. EX5PartyMemberConnStateType_Retain proves seats
  // survive disconnects; no captured duration exists, so this is operator
  // policy that bounds parties whose members never return.
  std::chrono::seconds retain_ttl{300};
  // Outbound invite rate limit per inviter: Party_LimitRequestCount (3309)
  // proves limiting; the window/count are operator settings.
  std::chrono::seconds invite_rate_window{60};
  std::size_t invite_rate_max = 5;
  // Bound on simultaneously tracked parties/invites so a hostile client cannot
  // grow server state without limit.
  std::size_t max_parties = 4096;
  std::size_t max_pending_invites_per_target = 8;
};

// Resolves display fields of PartyMember (name/level/class/map/zone). The
// service never reads character storage itself; native_world owns character
// state and supplies this lookup.
using MemberResolver = std::function<std::optional<MemberDescriptor>(CharacterId)>;

struct PartyResult {
  PartyError error = PartyError::kNone;
  std::vector<PartyEvent> events;

  bool ok() const { return error == PartyError::kNone; }
};

// Server-owned party registry. All methods take `now` so tests control invite
// expiry and rate windows deterministically. NOT internally synchronized:
// callers must serialize on the owning session/world worker, matching the
// project rule that packet and timer mutations share one worker.
class PartyService {
public:
  explicit PartyService(PartyConfig config, MemberResolver resolver);

  // PC2S_PartyInvitePopupReq. `inviter` may be party-less: inviting creates a
  // temporary party so the popup can carry party_id (schema field 1). The
  // temporary party disbands when its last invite resolves without an accept;
  // PartyRemoveTemporaryNoti covers that transition. A Retain (offline)
  // leader cannot invite: the popup names a leader the target cannot reach.
  PartyResult invite(CharacterId inviter, CharacterId target, TimePoint now);

  // PC2S_PartyAcceptInviteReq. Validates the (leader, target, party_id) triple
  // against a live invite. On success the target joins; a temporary party with
  // no other pending invites becomes permanent.
  PartyResult accept(CharacterId accepter, CharacterId leader_char_id,
                     CharacterId target_char_id, PartyId party_id, TimePoint now);

  // PC2S_PartyRejectInviteReq. Keyed by (leader, target) only per schema; the
  // leader's party id is resolved internally.
  PartyResult reject(CharacterId rejecter, CharacterId leader_char_id,
                     CharacterId target_char_id, TimePoint now);

  // PC2S_PartyLeaveReq. Empty request body: the leaver is implicit. Leaders may
  // leave; leadership transfers to the lowest connected char_id (lowest
  // retained seat when nobody is connected; no evidence selects a specific
  // successor). A leaving leader's
  // pending invites are cancelled and each invitee is notified so no popup
  // outlives its party. The last member leaving disbands the party.
  PartyResult leave(CharacterId leaver, TimePoint now);

  // PC2S_PartyKickReq. Leader-only (Party_LeaderPrivilege 3301). Kicking the
  // last other member leaves a solo leader, which the client models as a
  // normal party (ChangePartyUnitNoti traffic shows solo parties exist).
  PartyResult kick(CharacterId requester, CharacterId target, TimePoint now);

  // PC2S_PartyChangeLeaderReq. Leader-only; target must already be a member.
  PartyResult change_leader(CharacterId requester, CharacterId target,
                            TimePoint now);

  // Reconnect contract: a member that reconnects calls restore(); the result
  // carries a kJoinNoti event addressed to that member so native_world can
  // resend the PartyJoinNoti snapshot. Members who never left get
  // Party_NotJoined. Sessions dropping call disconnect() instead of leave():
  // the seat is retained with connection_state=Retain and no roster mutation.
  // A disconnecting LEADER cancels that party's pending invites (Candidate:
  // kRejectInviteNoti to each invitee clears the popup); a disconnecting
  // temporary leader destroys the temporary party outright. `now` starts the
  // retain_ttl clock used by expire_retained().
  PartyResult disconnect(CharacterId member, TimePoint now);
  // The callback must retain any required output before the disconnect is published.
  // It runs under the caller's serialization and must not reenter this service.
  PartyResult disconnect_staged(CharacterId member, TimePoint now,
                                const std::function<void(const PartyResult &)> &before_commit);
  PartyResult restore(CharacterId member);

  // Releases Retain seats older than retain_ttl through the normal leave path,
  // so successors, roster updates and empty-party disbanding stay identical to
  // an explicit PartyLeaveReq. Call from the world tick.
  std::vector<PartyEvent> expire_retained(TimePoint now);

  // Read-only queries for transport/interest code.
  std::optional<PartyId> party_of(CharacterId member) const;
  // Like party_of(), but ignores temporary parties that exist only to carry a
  // pending invite. Unit party identity (PlayerNub.party_id and
  // ChangePartyUnitNoti) uses this so a lone inviter is not shown as grouped.
  std::optional<PartyId> established_party_of(CharacterId member) const;
  bool is_leader(CharacterId member) const;
  std::vector<MemberDescriptor> members_of(CharacterId member) const;
  std::size_t party_count() const;
  // Rate-limit bookkeeping size; a lapsed inviter leaves no entry, proving
  // empty deques are not retained (verifier finding 2).
  std::size_t tracked_inviter_count() const;


  // Drop invites whose TTL elapsed; call from the world tick. Returns
  // PartyRemoveTemporaryNoti / RejectInviteNoti events for expired invites so
  // clients stop showing stale popups.
  std::vector<PartyEvent> expire_invites(TimePoint now);

private:
  struct Invite {
    PartyId party_id = 0;
    CharacterId inviter = 0;
    CharacterId target = 0;
    TimePoint expires_at{};
  };

  struct Party {
    PartyId id = 0;
    CharacterId leader = 0;
    std::vector<CharacterId> members; // leader first is NOT guaranteed
    std::vector<Invite> invites;
    bool temporary = false;
  };

  MemberDescriptor describe(CharacterId id, ConnectionState state) const;
  std::vector<MemberDescriptor> roster(const Party &party) const;
  bool member_index(const Party &party, CharacterId id, std::size_t &index) const;
  void remove_member(Party &party, CharacterId id);
  PartyResult fail(PartyError error) const;
  PartyEvent update_event(const Party &party, std::vector<CharacterId> left,
                          std::vector<CharacterId> kicked,
                          bool disband_pending) const;
  void destroy_party(PartyId id);
  void rate_record(CharacterId inviter, TimePoint now);
  bool rate_allows(CharacterId inviter, TimePoint now);
  ConnectionState state_of(CharacterId id) const;
  // Members of `party` that can actually receive a packet right now.
  std::vector<CharacterId> connected_recipients(const Party &party) const;
  // Pending invites cancelled because their leader can no longer honor them
  // (leave/disconnect/expiry). Emits a kRejectInviteNoti per cancelled invite
  // addressed to the invitee so the client popup clears. Candidate: the
  // schema has no dedicated cancellation noti; RejectInviteNoti carries the
  // cancelled target's descriptor which is the evidenced shape.
  std::vector<PartyEvent> cancel_invites(const Party &party,
                                         std::vector<Invite> cancelled) const;

  PartyConfig config_;
  MemberResolver resolver_;
  PartyId next_party_id_ = 1;
  std::unordered_map<PartyId, Party> parties_;
  std::unordered_map<CharacterId, PartyId> membership_;
  // Parallel connection state for seated members: Connected while a world
  // session is attached, Retain after disconnect until restore() or leave().
  std::unordered_map<CharacterId, ConnectionState> conn_state_;
  // Disconnect time of each Retain seat; erased on restore or removal.
  std::unordered_map<CharacterId, TimePoint> retained_since_;
  // invite rate limiting: per-inviter timestamps inside the window.
  std::unordered_map<CharacterId, std::deque<TimePoint>> invite_times_;
};

} // namespace chronicles::party
