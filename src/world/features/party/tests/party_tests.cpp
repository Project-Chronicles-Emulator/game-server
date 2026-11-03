// Behavioral tests for src/world/features/party/party.cpp (project gtest style).
// Evidence anchors: opcodes 20094-20099, PS2C/WSF2C notifications,
// PartyMaxMemberNum=4 (GameVariable.jsonl), ErrorCode names 3300-3360
// (ErrorCode.jsonl), EX5PartyMemberConnStateType Connected=1/Retain=2.
// Accept/leave/kick/leader transitions are Candidate-grade (schema +
// recovered error names only); InvitePopup/ChannelInvite C2S and
// ChangePartyUnitNoti S2C are the only captured frames.

#include "world/features/party/party.hpp"

#include <gtest/gtest.h>

#include <map>
#include <new>

namespace chronicles::party {
namespace {

const TimePoint T0 = Clock::time_point{};

std::map<CharacterId, MemberDescriptor> make_known() {
  std::map<CharacterId, MemberDescriptor> known;
  for (CharacterId id = 1; id <= 10; ++id) {
    MemberDescriptor descriptor;
    descriptor.char_id = id;
    descriptor.name = "char" + std::to_string(id);
    descriptor.level = 8;
    known[id] = descriptor;
  }
  return known;
}

class PartyServiceTest : public testing::Test {
protected:
  // Declaration order matters: resolver_ reads known_, service_ binds both.
  PartyConfig config_ = [] {
    PartyConfig config;
    config.max_members = 4; // PartyMaxMemberNum=4
    config.invite_ttl = std::chrono::seconds(30);
    config.invite_rate_window = std::chrono::seconds(60);
    config.invite_rate_max = 5;
    config.max_parties = 16;
    config.max_pending_invites_per_target = 8;
    return config;
  }();
  std::map<CharacterId, MemberDescriptor> known_ = make_known();
  MemberResolver resolver_ =
      [this](CharacterId id) -> std::optional<MemberDescriptor> {
        const auto it = known_.find(id);
        if (it == known_.end()) {
          return std::nullopt;
        }
        return it->second;
      };
  PartyService service_{config_, resolver_};
};

// Invite -> accept produces JoinNoti to the accepter and MemberUpdate to all.
// Accept is Candidate: PartyAcceptInviteReq is uncaptured, schema-derived.
TEST_F(PartyServiceTest, InviteAcceptEmitsPopupJoinAndRosterUpdate) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  ASSERT_EQ(invited.events.size(), 1u);
  EXPECT_EQ(invited.events[0].kind, EventKind::kInvitePopupNoti);
  EXPECT_EQ(invited.events[0].recipients, std::vector<CharacterId>({2}));
  const PartyId party = invited.events[0].party_id;
  EXPECT_NE(party, 0);

  const auto accepted = service_.accept(2, 1, 2, party, T0);
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(accepted.events.size(), 2u);
  EXPECT_EQ(accepted.events[0].kind, EventKind::kJoinNoti);
  EXPECT_EQ(accepted.events[0].recipients, std::vector<CharacterId>({2}));
  EXPECT_EQ(accepted.events[0].leader_char_id, 1);
  EXPECT_EQ(accepted.events[0].members.size(), 2u);
  EXPECT_EQ(accepted.events[1].kind, EventKind::kMemberUpdateNoti);
  EXPECT_EQ(accepted.events[1].recipients.size(), 2u);
  EXPECT_TRUE(service_.is_leader(1));
  EXPECT_EQ(service_.party_of(2), party);
}

// Reject path: notification to inviter, temporary party removed.
TEST_F(PartyServiceTest, RejectNotifiesInviterAndRemovesTemporary) {
  ASSERT_TRUE(service_.invite(1, 2, T0).ok());
  const auto rejected = service_.reject(2, 1, 2, T0);
  ASSERT_TRUE(rejected.ok());
  ASSERT_EQ(rejected.events.size(), 2u);
  EXPECT_EQ(rejected.events[0].kind, EventKind::kRejectInviteNoti);
  EXPECT_EQ(rejected.events[0].recipients, std::vector<CharacterId>({1}));
  EXPECT_EQ(rejected.events[1].kind, EventKind::kRemoveTemporaryNoti);
  EXPECT_FALSE(service_.party_of(1).has_value());
}

// Expiry via tick: invite_ttl (server policy constant) elapsed ->
// invitee gets a popup-clearing noti and the temporary party is removed;
// a late accept gets InviteRequestExpired.
TEST_F(PartyServiceTest, ExpiredInviteNotifiesAndRemovesTemporary) {
  ASSERT_TRUE(service_.invite(1, 2, T0).ok());
  const auto events = service_.expire_invites(T0 + std::chrono::seconds(31));
  ASSERT_EQ(events.size(), 2u);
  // First: the invitee learns the popup died (verifier finding 1).
  EXPECT_EQ(events[0].kind, EventKind::kRejectInviteNoti);
  EXPECT_EQ(events[0].recipients, std::vector<CharacterId>({2}));
  EXPECT_EQ(events[0].target.char_id, 2);
  // Second: the temporary party dissolves for its remaining (leader) seat.
  EXPECT_EQ(events[1].kind, EventKind::kRemoveTemporaryNoti);
  EXPECT_EQ(events[1].recipients, std::vector<CharacterId>({1}));
  EXPECT_FALSE(service_.party_of(1).has_value());

  ASSERT_TRUE(service_.invite(3, 4, T0).ok());
  const PartyId party = service_.party_of(3).value();
  const auto late = service_.accept(4, 3, 4, party, T0 + std::chrono::seconds(31));
  EXPECT_EQ(late.error, PartyError::kInviteRequestExpired);
}

// PartyMaxMemberNum=4: fourth member joins, fifth invite refuses FullCapacity.
TEST_F(PartyServiceTest, CatalogCapOfFourRefusesFifthInvite) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;
  ASSERT_TRUE(service_.accept(2, 1, 2, party, T0).ok());
  ASSERT_TRUE(service_.invite(1, 3, T0 + std::chrono::seconds(1)).ok());
  ASSERT_TRUE(service_.accept(3, 1, 3, party, T0 + std::chrono::seconds(1)).ok());
  ASSERT_TRUE(service_.invite(1, 4, T0 + std::chrono::seconds(2)).ok());
  ASSERT_TRUE(service_.accept(4, 1, 4, party, T0 + std::chrono::seconds(2)).ok());
  EXPECT_EQ(service_.members_of(1).size(), 4u);
  EXPECT_EQ(service_.invite(1, 5, T0 + std::chrono::seconds(3)).error,
            PartyError::kFullCapacity);
}

// Rate limit (3309 Party_LimitRequestCount): burst beyond the configured
// server-policy count refuses; the window reset permits again.
TEST_F(PartyServiceTest, InviteRateLimitRefusesBurstAndResets) {
  PartyConfig config = config_;
  config.invite_rate_max = 3;
  service_ = PartyService(config, resolver_);

  EXPECT_TRUE(service_.invite(1, 2, T0).ok());
  EXPECT_TRUE(service_.invite(1, 3, T0).ok());
  EXPECT_TRUE(service_.invite(1, 4, T0).ok());
  EXPECT_EQ(service_.invite(1, 5, T0).error, PartyError::kLimitRequestCount);
  EXPECT_TRUE(service_.invite(1, 5, T0 + std::chrono::seconds(61)).ok());
}

// Non-leader invite/kick/change-leader refused LeaderPrivilege (3301).
TEST_F(PartyServiceTest, NonLeaderActionsRefuseLeaderPrivilege) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;
  ASSERT_TRUE(service_.accept(2, 1, 2, party, T0).ok());

  EXPECT_EQ(service_.invite(2, 3, T0).error, PartyError::kLeaderPrivilege);
  EXPECT_EQ(service_.kick(2, 1, T0).error, PartyError::kLeaderPrivilege);
  EXPECT_EQ(service_.change_leader(2, 1, T0).error,
            PartyError::kLeaderPrivilege);
}

// Leader transfer (Candidate: no captured leader transition). Explicit
// change-leader moves leadership; leader leaving promotes lowest char_id.
TEST_F(PartyServiceTest, LeaderTransferExplicitAndOnLeave) {
  const auto invited = service_.invite(5, 3, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;
  ASSERT_TRUE(service_.accept(3, 5, 3, party, T0).ok());
  ASSERT_TRUE(service_.invite(5, 2, T0 + std::chrono::seconds(1)).ok());
  ASSERT_TRUE(service_.accept(2, 5, 2, party, T0 + std::chrono::seconds(1)).ok());

  EXPECT_TRUE(service_.change_leader(5, 3, T0).ok());
  EXPECT_TRUE(service_.is_leader(3));
  EXPECT_EQ(service_.change_leader(5, 5, T0).error,
            PartyError::kLeaderPrivilege);

  const auto left = service_.leave(3, T0);
  ASSERT_TRUE(left.ok());
  EXPECT_TRUE(service_.is_leader(2));
  ASSERT_EQ(left.events.size(), 2u);
  EXPECT_EQ(left.events[0].kind, EventKind::kLeaveNoti);
  EXPECT_EQ(left.events[0].recipients, std::vector<CharacterId>({3}));
  EXPECT_EQ(left.events[1].kind, EventKind::kMemberUpdateNoti);
  EXPECT_EQ(left.events[1].leave_char_ids, std::vector<CharacterId>({3}));
}

// Kick (Candidate): target gets kicked LeaveNoti; update carries kicked list.
TEST_F(PartyServiceTest, KickEmitsKickedLeaveAndRosterUpdate) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;
  ASSERT_TRUE(service_.accept(2, 1, 2, party, T0).ok());

  const auto kicked = service_.kick(1, 2, T0);
  ASSERT_TRUE(kicked.ok());
  ASSERT_EQ(kicked.events.size(), 2u);
  EXPECT_EQ(kicked.events[0].kind, EventKind::kLeaveNoti);
  EXPECT_TRUE(kicked.events[0].kicked);
  EXPECT_EQ(kicked.events[0].recipients, std::vector<CharacterId>({2}));
  EXPECT_EQ(kicked.events[1].kind, EventKind::kMemberUpdateNoti);
  EXPECT_EQ(kicked.events[1].kicked_char_ids, std::vector<CharacterId>({2}));
  EXPECT_FALSE(service_.party_of(2).has_value());
  EXPECT_EQ(service_.kick(1, 9, T0).error, PartyError::kNotExistsMember);
}

// Reconnect contract: disconnect retains the seat with Retain state; restore
// returns a fresh JoinNoti snapshot to the reconnecting member.
TEST_F(PartyServiceTest, DisconnectRetainsSeatAndRestoreResendsSnapshot) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;
  ASSERT_TRUE(service_.accept(2, 1, 2, party, T0).ok());

  ASSERT_TRUE(service_.disconnect(2, T0).ok());
  EXPECT_EQ(service_.party_of(2), party);
  const auto roster = service_.members_of(1);
  ASSERT_EQ(roster.size(), 2u);
  EXPECT_EQ(roster[1].connection_state, ConnectionState::kRetain);

  const auto restored = service_.restore(2);
  ASSERT_TRUE(restored.ok());
  ASSERT_GE(restored.events.size(), 1u);
  EXPECT_EQ(restored.events[0].kind, EventKind::kJoinNoti);
  EXPECT_EQ(restored.events[0].recipients, std::vector<CharacterId>({2}));
  EXPECT_EQ(restored.events[0].members.size(), 2u);
  EXPECT_EQ(service_.restore(9).error, PartyError::kNotJoined);
  EXPECT_EQ(service_.disconnect(9, T0).error, PartyError::kNotJoined);
}

TEST_F(PartyServiceTest, FailedDisconnectStagingPreservesSeatAndPendingInvites) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const auto party = invited.events[0].party_id;
  ASSERT_TRUE(service_.accept(2, 1, 2, party, T0).ok());
  ASSERT_TRUE(service_.invite(1, 3, T0).ok());

  EXPECT_THROW(service_.disconnect_staged(1, T0, [](const PartyResult &result) {
    ASSERT_EQ(result.events.size(), 2U);
    throw std::bad_alloc();
  }), std::bad_alloc);

  const auto roster = service_.members_of(2);
  ASSERT_EQ(roster.size(), 2U);
  EXPECT_EQ(roster[0].connection_state, ConnectionState::kConnected);
  EXPECT_TRUE(service_.accept(3, 1, 3, party, T0).ok());
  EXPECT_TRUE(service_.disconnect(1, T0).ok());
  EXPECT_EQ(service_.members_of(2)[0].connection_state, ConnectionState::kRetain);
}

TEST_F(PartyServiceTest, FailedTemporaryDisconnectStagingKeepsTheInvite) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const auto party = invited.events[0].party_id;

  EXPECT_THROW(service_.disconnect_staged(1, T0, [](const PartyResult &) {
    throw std::bad_alloc();
  }), std::bad_alloc);

  EXPECT_EQ(service_.party_of(1), party);
  EXPECT_TRUE(service_.accept(2, 1, 2, party, T0).ok());
}

// Validation failures: spoofed accept, unknown party, bad leader id,
// already-invited, in-another-party, self-invite, unresolvable invitee.
TEST_F(PartyServiceTest, RefusalsDoNotMutateState) {
  EXPECT_EQ(service_.invite(1, 1, T0).error, PartyError::kNotExistsInvitee);
  EXPECT_EQ(service_.invite(1, 99, T0).error, PartyError::kNotExistsInvitee);

  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;
  EXPECT_EQ(service_.invite(1, 2, T0).error, PartyError::kAlreadyInvited);
  EXPECT_EQ(service_.accept(2, 1, 2, party + 999, T0).error,
            PartyError::kNotExistParty);
  EXPECT_EQ(service_.accept(3, 1, 2, party, T0).error,
            PartyError::kNotExistsInvitee);
  EXPECT_EQ(service_.accept(2, 9, 2, party, T0).error,
            PartyError::kInviteRequestLeaderLeft);
  EXPECT_EQ(service_.reject(9, 1, 2, T0).error, PartyError::kNotExistsInvitee);

  ASSERT_TRUE(service_.accept(2, 1, 2, party, T0).ok());
  ASSERT_TRUE(service_.invite(6, 7, T0).ok());
  EXPECT_EQ(service_.invite(2, 8, T0).error, PartyError::kLeaderPrivilege);
  EXPECT_EQ(service_.invite(6, 2, T0 + std::chrono::seconds(1)).error,
            PartyError::kInAnotherParty);
  EXPECT_EQ(service_.leave(9, T0).error, PartyError::kNotJoined);
}

// Verifier finding 3a: a disconnecting temporary leader dissolves the party —
// invites cancelled, invitee popup cleared, leader seat freed.
TEST_F(PartyServiceTest, DisconnectingTemporaryLeaderDissolves) {
  ASSERT_TRUE(service_.invite(1, 2, T0).ok());
  const auto result = service_.disconnect(1, T0);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.events.size(), 2u);
  EXPECT_EQ(result.events[0].kind, EventKind::kRejectInviteNoti);
  EXPECT_EQ(result.events[0].recipients, std::vector<CharacterId>({2}));
  EXPECT_EQ(result.events[1].kind, EventKind::kRemoveTemporaryNoti);
  EXPECT_FALSE(service_.party_of(1).has_value());
  // Nothing retained: a later restore cannot resume a dissolved invite shell.
  EXPECT_EQ(service_.restore(1).error, PartyError::kNotJoined);
  // The stale invite cannot be accepted anymore.
  EXPECT_EQ(service_.accept(2, 1, 2, result.events[0].party_id, T0).error,
            PartyError::kNotExistParty);
}

// Verifier finding 3b: a disconnecting permanent leader retains the seat but
// the party's pending invites are cancelled and invitees notified; offline
// members are filtered from update recipients (finding 4).
TEST_F(PartyServiceTest, DisconnectingLeaderCancelsInvitesAndFiltersRecipients) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;
  ASSERT_TRUE(service_.accept(2, 1, 2, party, T0).ok());
  ASSERT_TRUE(service_.invite(1, 3, T0 + std::chrono::seconds(1)).ok());

  const auto dropped = service_.disconnect(1, T0);
  ASSERT_TRUE(dropped.ok());
  ASSERT_EQ(dropped.events.size(), 2u);
  // Invite 1->3 cancelled; invitee popup cleared.
  EXPECT_EQ(dropped.events[0].kind, EventKind::kRejectInviteNoti);
  EXPECT_EQ(dropped.events[0].recipients, std::vector<CharacterId>({3}));
  // Roster update addressed only to still-connected members (2), never to
  // the Retain leader (1).
  EXPECT_EQ(dropped.events[1].kind, EventKind::kMemberUpdateNoti);
  EXPECT_EQ(dropped.events[1].recipients, std::vector<CharacterId>({2}));
  EXPECT_EQ(dropped.events[1].members.size(), 2u);

  // Invitees cannot accept against a cancelled request.
  EXPECT_EQ(service_.accept(3, 1, 3, party, T0).error,
            PartyError::kInviteRequestNotFound);
  // Retain leaders cannot issue new invites.
  EXPECT_EQ(service_.invite(1, 4, T0).error, PartyError::kLeaderPrivilege);

  // Restore brings the seat back; the leader must re-invite explicitly.
  ASSERT_TRUE(service_.restore(1).ok());
  EXPECT_TRUE(service_.invite(1, 3, T0 + std::chrono::seconds(2)).ok());
}

// Verifier finding 1, second path: an unrelated invite also flushes expiry
// events so a stale popup clears before any new invite evaluation.
TEST_F(PartyServiceTest, LazyExpiryEmitsNotificationDuringInvite) {
  ASSERT_TRUE(service_.invite(1, 2, T0).ok());
  const auto fresh = service_.invite(6, 8, T0 + std::chrono::seconds(31));
  ASSERT_TRUE(fresh.ok());
  ASSERT_EQ(fresh.events.size(), 3u);
  EXPECT_EQ(fresh.events[0].kind, EventKind::kRejectInviteNoti);
  EXPECT_EQ(fresh.events[0].recipients, std::vector<CharacterId>({2}));
  EXPECT_EQ(fresh.events[1].kind, EventKind::kRemoveTemporaryNoti);
  EXPECT_EQ(fresh.events[2].kind, EventKind::kInvitePopupNoti);
  EXPECT_FALSE(service_.party_of(1).has_value());
}

// Verifier finding 2: rate-limit bookkeeping drops empty deques. The cleanup
// is observable when a later call drains a lapsed deque and then fails before
// rate_record could repopulate it — a Retain leader's invite does exactly
// that (rate check passes, privilege check fails).
TEST_F(PartyServiceTest, RateBookkeepingDropsEmptyDeques) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  ASSERT_TRUE(service_.accept(2, 1, 2, invited.events[0].party_id, T0).ok());
  const auto second = service_.invite(5, 6, T0);
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(service_.accept(6, 5, 6, second.events[0].party_id, T0).ok());
  EXPECT_EQ(service_.tracked_inviter_count(), 2u);
  // Leader 5 goes offline; the seat is retained but invites are refused.
  ASSERT_TRUE(service_.disconnect(5, T0).ok());
  // At +61s both deques are stale. rate_allows(5) empties and drops 5's entry,
  // then the Retain-leader guard refuses — so no entry is re-recorded.
  EXPECT_EQ(service_.invite(5, 7, T0 + std::chrono::seconds(61)).error,
            PartyError::kLeaderPrivilege);
  EXPECT_EQ(service_.tracked_inviter_count(), 1u);
}

// A retained seat older than retain_ttl is released through the leave path:
// leadership moves to the connected member and the roster update omits it.
TEST_F(PartyServiceTest, ExpiredRetainedSeatLeavesAndTransfersLeadership) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;
  ASSERT_TRUE(service_.accept(2, 1, 2, party, T0).ok());

  ASSERT_TRUE(service_.disconnect(1, T0).ok());
  EXPECT_TRUE(service_.expire_retained(T0 + std::chrono::seconds(299)).empty());
  EXPECT_TRUE(service_.is_leader(1));

  const auto released = service_.expire_retained(T0 + std::chrono::seconds(300));
  ASSERT_EQ(released.size(), 2u);
  EXPECT_EQ(released[0].kind, EventKind::kLeaveNoti);
  EXPECT_EQ(released[1].kind, EventKind::kMemberUpdateNoti);
  EXPECT_EQ(released[1].leave_char_ids, std::vector<CharacterId>({1}));
  EXPECT_EQ(released[1].recipients, std::vector<CharacterId>({2}));
  EXPECT_EQ(released[1].leader_char_id, 2);
  EXPECT_FALSE(service_.party_of(1).has_value());
  EXPECT_TRUE(service_.is_leader(2));
}

// Restoring a seat cancels its retention clock.
TEST_F(PartyServiceTest, RestoreStopsRetainedSeatExpiry) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  ASSERT_TRUE(service_.accept(2, 1, 2, invited.events[0].party_id, T0).ok());

  ASSERT_TRUE(service_.disconnect(2, T0).ok());
  ASSERT_TRUE(service_.restore(2).ok());

  EXPECT_TRUE(service_.expire_retained(T0 + std::chrono::hours(1)).empty());
  EXPECT_EQ(service_.members_of(1).size(), 2u);
}

// A leaving leader hands leadership to a connected member before a lower
// char_id that is offline, so leader-only actions remain usable.
TEST_F(PartyServiceTest, LeaderSuccessionPrefersConnectedMember) {
  const auto invited = service_.invite(3, 1, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;
  ASSERT_TRUE(service_.accept(1, 3, 1, party, T0).ok());
  ASSERT_TRUE(service_.invite(3, 5, T0).ok());
  ASSERT_TRUE(service_.accept(5, 3, 5, party, T0).ok());

  ASSERT_TRUE(service_.disconnect(1, T0).ok());
  ASSERT_TRUE(service_.leave(3, T0).ok());

  EXPECT_TRUE(service_.is_leader(5));
}

// Temporary invite-carrier parties are not established membership.
TEST_F(PartyServiceTest, EstablishedPartyIgnoresTemporaryInviteCarrier) {
  const auto invited = service_.invite(1, 2, T0);
  ASSERT_TRUE(invited.ok());
  const PartyId party = invited.events[0].party_id;

  EXPECT_EQ(service_.party_of(1), party);
  EXPECT_FALSE(service_.established_party_of(1).has_value());

  ASSERT_TRUE(service_.accept(2, 1, 2, party, T0).ok());

  EXPECT_EQ(service_.established_party_of(1), party);
  EXPECT_EQ(service_.established_party_of(2), party);
}

} // namespace
} // namespace chronicles::party
