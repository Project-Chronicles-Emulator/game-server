#include "world/features/party/party.hpp"

#include <algorithm>
#include <stdexcept>

namespace chronicles::party {

PartyService::PartyService(PartyConfig config, MemberResolver resolver)
    : config_(config), resolver_(std::move(resolver)) {}

PartyResult PartyService::fail(PartyError error) const {
  PartyResult result;
  result.error = error;
  return result;
}

MemberDescriptor PartyService::describe(CharacterId id,
                                        ConnectionState state) const {
  MemberDescriptor descriptor;
  descriptor.char_id = id;
  descriptor.connection_state = state;
  if (resolver_) {
    if (auto resolved = resolver_(id)) {
      descriptor.name = std::move(resolved->name);
      descriptor.level = resolved->level;
      descriptor.char_class = resolved->char_class;
      descriptor.map_cms_id = resolved->map_cms_id;
      descriptor.zone_key = resolved->zone_key;
    }
  }
  return descriptor;
}

std::vector<MemberDescriptor> PartyService::roster(const Party &party) const {
  std::vector<MemberDescriptor> roster;
  roster.reserve(party.members.size());
  for (const CharacterId id : party.members) {
    // Retained seats keep their roster slot so reconnect restores them without
    // a roster diff; only the connection_state field changes.
    roster.push_back(describe(id, state_of(id)));
  }
  return roster;
}

ConnectionState PartyService::state_of(CharacterId id) const {
  const auto it = conn_state_.find(id);
  return it == conn_state_.end() ? ConnectionState::kConnected : it->second;
}

bool PartyService::member_index(const Party &party, CharacterId id,
                                std::size_t &index) const {
  const auto it = std::find(party.members.begin(), party.members.end(), id);
  if (it == party.members.end()) {
    return false;
  }
  index = static_cast<std::size_t>(it - party.members.begin());
  return true;
}

void PartyService::remove_member(Party &party, CharacterId id) {
  std::size_t index = 0;
  if (member_index(party, id, index)) {
    party.members.erase(party.members.begin() + static_cast<std::ptrdiff_t>(index));
  }
  membership_.erase(id);
  conn_state_.erase(id);
  retained_since_.erase(id);
}

void PartyService::destroy_party(PartyId id) {
  const auto it = parties_.find(id);
  if (it == parties_.end()) {
    return;
  }
  for (const CharacterId member : it->second.members) {
    membership_.erase(member);
    conn_state_.erase(member);
    retained_since_.erase(member);
  }
  parties_.erase(it);
}

PartyEvent PartyService::update_event(const Party &party,
                                      std::vector<CharacterId> left,
                                      std::vector<CharacterId> kicked,
                                      bool disband_pending) const {
  PartyEvent event;
  event.kind = EventKind::kMemberUpdateNoti;
  event.party_id = party.id;
  // Recipient contract: only Connected members are addressed. Retain seats
  // still appear in the roster payload — the client learns their state from
  // the member list, not from a message they cannot receive.
  event.recipients = connected_recipients(party);
  event.leader_char_id = party.leader;
  event.members = roster(party);
  event.leave_char_ids = std::move(left);
  event.kicked_char_ids = std::move(kicked);
  event.disband_pending = disband_pending;
  return event;
}

std::vector<CharacterId>
PartyService::connected_recipients(const Party &party) const {
  std::vector<CharacterId> recipients;
  recipients.reserve(party.members.size());
  for (const CharacterId id : party.members) {
    if (state_of(id) == ConnectionState::kConnected) {
      recipients.push_back(id);
    }
  }
  return recipients;
}

std::vector<PartyEvent>
PartyService::cancel_invites(const Party &party,
                             std::vector<Invite> cancelled) const {
  std::vector<PartyEvent> events;
  events.reserve(cancelled.size());
  for (const Invite &invite : cancelled) {
    PartyEvent notice;
    notice.kind = EventKind::kRejectInviteNoti;
    notice.party_id = party.id;
    notice.recipients.push_back(invite.target);
    notice.target = describe(invite.target, ConnectionState::kConnected);
    events.push_back(std::move(notice));
  }
  return events;
}

void PartyService::rate_record(CharacterId inviter, TimePoint now) {
  auto &times = invite_times_[inviter];
  times.push_back(now);
  while (!times.empty() && now - times.front() > config_.invite_rate_window) {
    times.pop_front();
  }
}

bool PartyService::rate_allows(CharacterId inviter, TimePoint now) {
  const auto it = invite_times_.find(inviter);
  if (it == invite_times_.end()) {
    return true;
  }
  auto &times = it->second;
  while (!times.empty() && now - times.front() > config_.invite_rate_window) {
    times.pop_front();
  }
  if (times.empty()) {
    // Do not retain empty per-inviter deques: a long-lived world with many
    // one-time inviters must not grow bookkeeping without bound.
    invite_times_.erase(it);
    return true;
  }
  if (config_.invite_rate_max == 0) {
    return true;
  }
  return times.size() < config_.invite_rate_max;
}

PartyResult PartyService::invite(CharacterId inviter, CharacterId target,
                                 TimePoint now) {
  if (inviter == target) {
    return fail(PartyError::kNotExistsInvitee);
  }
  if (resolver_ && !resolver_(target).has_value()) {
    // Party_NotExistsInvitee (3304): the invitee is not a resolvable
    // character on this server.
    return fail(PartyError::kNotExistsInvitee);
  }
  if (membership_.count(target) != 0) {
    return fail(PartyError::kInAnotherParty);
  }
  if (!rate_allows(inviter, now)) {
    return fail(PartyError::kLimitRequestCount);
  }

  // Lazily expire before selecting a host so stale invites neither reserve
  // seats nor block this target; events ride along with the result so
  // invitees learn their popup died even outside the world tick.
  PartyResult result;
  result.events = expire_invites(now);

  // The inviter must be able to host a party: either already in one, or we
  // create a temporary party so the popup noti carries a real party_id.
  Party *party = nullptr;
  // True only when THIS call created the host party; a pre-existing temporary
  // party (earlier pending invite) must not be abandoned on failure below.
  bool created_temporary = false;
  const auto membership_it = membership_.find(inviter);
  if (membership_it != membership_.end()) {
    party = &parties_.at(membership_it->second);
    if (party->leader != inviter) {
      return fail(PartyError::kLeaderPrivilege);
    }
    if (state_of(inviter) != ConnectionState::kConnected) {
      // An offline leader cannot honor the popup's leader field; refusing
      // avoids presenting an invite whose leader is unreachable.
      return fail(PartyError::kLeaderPrivilege);
    }
  } else {
    if (parties_.size() >= config_.max_parties) {
      // No catalog row for server capacity; refusing with the closest
      // evidenced code keeps the bound without inventing a client message.
      return fail(PartyError::kLimitRequestCount);
    }
    const PartyId id = next_party_id_++;
    Party fresh;
    fresh.id = id;
    fresh.leader = inviter;
    fresh.members.push_back(inviter);
    fresh.temporary = true;
    party = &parties_.emplace(id, std::move(fresh)).first->second;
    membership_[inviter] = id;
    conn_state_[inviter] = ConnectionState::kConnected;
    created_temporary = true;
  }
  const PartyId host_party_id = party->id;
  // Failures below this point must unroll a fresh temporary party so a refused
  // invite never leaks a solo seat.
  const auto abandon_temporary = [&]() {
    if (created_temporary) {
      destroy_party(host_party_id);
    }
  };

  // Duplicate invite guard (Party_AlreadyInvited 3308) and the per-target
  // pending bound scan all parties; parties are few and invites short-lived.
  std::size_t pending_for_target = 0;
  for (const auto &[other_id, other] : parties_) {
    for (const Invite &existing : other.invites) {
      if (existing.target != target) {
        continue;
      }
      ++pending_for_target;
      if (existing.party_id == host_party_id) {
        abandon_temporary();
        return fail(PartyError::kAlreadyInvited);
      }
    }
  }
  if (pending_for_target >= config_.max_pending_invites_per_target) {
    abandon_temporary();
    return fail(PartyError::kLimitRequestCount);
  }
  if (party->members.size() + party->invites.size() + 1 > config_.max_members) {
    // Reserving a seat per invite keeps a 4-member party from accepting a
    // fifth member through simultaneous accepts (Party_FullCapacity 3307).
    abandon_temporary();
    return fail(PartyError::kFullCapacity);
  }

  Invite invite;
  invite.party_id = host_party_id;
  invite.inviter = inviter;
  invite.target = target;
  invite.expires_at = now + config_.invite_ttl;
  party->invites.push_back(invite);
  rate_record(inviter, now);

  PartyEvent popup;
  popup.kind = EventKind::kInvitePopupNoti;
  popup.party_id = host_party_id;
  popup.recipients.push_back(target);
  popup.leader = describe(inviter, state_of(inviter));
  popup.target = describe(target, ConnectionState::kConnected);
  result.events.push_back(std::move(popup));
  return result;
}

PartyResult PartyService::accept(CharacterId accepter, CharacterId leader_char_id,
                                 CharacterId target_char_id, PartyId party_id,
                                 TimePoint now) {
  // The request carries (leader, target, party_id); the accepter must equal
  // target_char_id or the request is spoofing someone else's invite.
  if (accepter != target_char_id) {
    return fail(PartyError::kNotExistsInvitee);
  }
  const auto party_it = parties_.find(party_id);
  if (party_it == parties_.end()) {
    return fail(PartyError::kNotExistParty);
  }
  Party &party = party_it->second;
  if (party.leader != leader_char_id) {
    return fail(PartyError::kInviteRequestLeaderLeft);
  }
  if (membership_.count(accepter) != 0) {
    if (membership_.at(accepter) == party_id) {
      return fail(PartyError::kSameParty);
    }
    return fail(PartyError::kInAnotherParty);
  }

  auto invite_it =
      std::find_if(party.invites.begin(), party.invites.end(),
                   [&](const Invite &invite) { return invite.target == accepter; });
  if (invite_it == party.invites.end()) {
    return fail(PartyError::kInviteRequestNotFound);
  }
  if (now >= invite_it->expires_at) {
    party.invites.erase(invite_it);
    return fail(PartyError::kInviteRequestExpired);
  }
  if (party.members.size() >= config_.max_members) {
    return fail(PartyError::kFullCapacity);
  }

  party.invites.erase(invite_it);
  party.members.push_back(accepter);
  membership_[accepter] = party.id;
  conn_state_[accepter] = ConnectionState::kConnected;
  party.temporary = false;

  PartyResult result;
  PartyEvent join;
  join.kind = EventKind::kJoinNoti;
  join.party_id = party.id;
  join.recipients.push_back(accepter);
  join.leader_char_id = party.leader;
  join.members = roster(party);
  result.events.push_back(std::move(join));
  result.events.push_back(update_event(party, {}, {}, false));
  return result;
}

PartyResult PartyService::reject(CharacterId rejecter, CharacterId leader_char_id,
                                 CharacterId target_char_id, TimePoint now) {
  (void)now;
  if (rejecter != target_char_id) {
    return fail(PartyError::kNotExistsInvitee);
  }
  const auto membership_it = membership_.find(leader_char_id);
  if (membership_it == membership_.end()) {
    return fail(PartyError::kNotExistsInviter);
  }
  Party &party = parties_.at(membership_it->second);
  if (party.leader != leader_char_id) {
    return fail(PartyError::kInviteRequestLeaderLeft);
  }
  const auto invite_it =
      std::find_if(party.invites.begin(), party.invites.end(),
                   [&](const Invite &invite) { return invite.target == rejecter; });
  if (invite_it == party.invites.end()) {
    return fail(PartyError::kInviteRequestNotFound);
  }
  party.invites.erase(invite_it);

  PartyResult result;
  PartyEvent notice;
  notice.kind = EventKind::kRejectInviteNoti;
  notice.party_id = party.id;
  notice.recipients.push_back(leader_char_id);
  notice.target = describe(rejecter, ConnectionState::kConnected);
  result.events.push_back(std::move(notice));

  // Temporary party with no pending invites left: the leader never acquired a
  // second member, so the seat is removed and PartyRemoveTemporaryNoti fires.
  if (party.temporary && party.invites.empty()) {
    PartyEvent remove;
    remove.kind = EventKind::kRemoveTemporaryNoti;
    remove.party_id = party.id;
    remove.recipients = connected_recipients(party);
    result.events.push_back(std::move(remove));
    destroy_party(party.id);
  }
  return result;
}

PartyResult PartyService::leave(CharacterId leaver, TimePoint now) {
  (void)now;
  const auto membership_it = membership_.find(leaver);
  if (membership_it == membership_.end()) {
    return fail(PartyError::kNotJoined);
  }
  Party &party = parties_.at(membership_it->second);
  const PartyId party_id = party.id;
  const bool was_leader = (party.leader == leaver);
  PartyResult result;
  // A leaving leader's pending invites die with leadership
  // (Party_InviteRequestLeaderLeft 3312 models that binding); each invitee is
  // told so no popup outlives the party that issued it.
  if (was_leader && !party.invites.empty()) {
    auto cancelled = cancel_invites(party, party.invites);
    party.invites.clear();
    for (auto &event : cancelled) {
      result.events.push_back(std::move(event));
    }
  }
  remove_member(party, leaver);

  PartyEvent leave_notice;
  leave_notice.kind = EventKind::kLeaveNoti;
  leave_notice.party_id = party_id;
  leave_notice.recipients.push_back(leaver);
  leave_notice.kicked = false;
  result.events.push_back(std::move(leave_notice));

  if (party.members.empty()) {
    destroy_party(party_id);
    return result;
  }
  if (was_leader) {
    // Deterministic successor: lowest connected char_id, falling back to the
    // lowest retained seat. No capture fixes the choice; preferring a
    // connected member keeps leader-only actions available to the party.
    party.leader = *std::min_element(party.members.begin(), party.members.end(),
                                     [this](CharacterId left, CharacterId right) {
                                       const bool left_connected =
                                           state_of(left) == ConnectionState::kConnected;
                                       const bool right_connected =
                                           state_of(right) == ConnectionState::kConnected;
                                       if (left_connected != right_connected) {
                                         return left_connected;
                                       }
                                       return left < right;
                                     });
  }
  result.events.push_back(update_event(party, {leaver}, {}, false));
  return result;
}

PartyResult PartyService::kick(CharacterId requester, CharacterId target,
                               TimePoint now) {
  (void)now;
  const auto membership_it = membership_.find(requester);
  if (membership_it == membership_.end()) {
    return fail(PartyError::kNotJoined);
  }
  Party &party = parties_.at(membership_it->second);
  if (party.leader != requester) {
    return fail(PartyError::kLeaderPrivilege);
  }
  if (requester == target) {
    // Self-kick is equivalent to leave; refuse so the leader keeps an explicit
    // leave path (PartyLeaveReq has no target and is unambiguous).
    return fail(PartyError::kAlreadyLeader);
  }
  std::size_t index = 0;
  if (!member_index(party, target, index)) {
    return fail(PartyError::kNotExistsMember);
  }
  const PartyId party_id = party.id;
  remove_member(party, target);
  // Kick revokes any pending invite that target held for this party.
  party.invites.erase(std::remove_if(party.invites.begin(), party.invites.end(),
                                     [&](const Invite &invite) {
                                       return invite.target == target;
                                     }),
                      party.invites.end());

  PartyResult result;
  PartyEvent leave_notice;
  leave_notice.kind = EventKind::kLeaveNoti;
  leave_notice.party_id = party_id;
  leave_notice.recipients.push_back(target);
  leave_notice.kicked = true;
  result.events.push_back(std::move(leave_notice));
  result.events.push_back(update_event(party, {}, {target}, false));
  return result;
}

PartyResult PartyService::change_leader(CharacterId requester, CharacterId target,
                                        TimePoint now) {
  (void)now;
  const auto membership_it = membership_.find(requester);
  if (membership_it == membership_.end()) {
    return fail(PartyError::kNotJoined);
  }
  Party &party = parties_.at(membership_it->second);
  if (party.leader != requester) {
    return fail(PartyError::kLeaderPrivilege);
  }
  if (requester == target) {
    return fail(PartyError::kAlreadyLeader);
  }
  std::size_t index = 0;
  if (!member_index(party, target, index)) {
    return fail(PartyError::kNotExistsMember);
  }
  party.leader = target;

  PartyResult result;
  result.events.push_back(update_event(party, {}, {}, false));
  return result;
}

PartyResult PartyService::disconnect(CharacterId member, TimePoint now) {
  return disconnect_staged(member, now, {});
}

PartyResult PartyService::disconnect_staged(
    CharacterId member, TimePoint now,
    const std::function<void(const PartyResult &)> &before_commit) {
  // Seat retained; the roster entry stays with connection_state=Retain and the
  // remaining members get a MemberUpdateNoti reflecting it.
  const auto membership_it = membership_.find(member);
  if (membership_it == membership_.end()) {
    return fail(PartyError::kNotJoined);
  }
  Party &party = parties_.at(membership_it->second);
  const PartyId party_id = party.id;
  PartyResult result;

  if (party.leader == member) {
    // An offline leader cannot honor pending invites; cancel them and clear
    // each invitee's popup (Candidate signal, RejectInviteNoti shape).
    if (!party.invites.empty()) {
      auto cancelled = cancel_invites(party, party.invites);
      for (auto &event : cancelled) {
        result.events.push_back(std::move(event));
      }
    }
    // A temporary party is definitionally "invite in flight": its only member
    // going offline dissolves it. The leader seat is freed, not retained —
    // nothing remains for restore() to resume.
    if (party.temporary) {
      PartyEvent remove;
      remove.kind = EventKind::kRemoveTemporaryNoti;
      remove.party_id = party_id;
      remove.recipients.push_back(member);
      result.events.push_back(std::move(remove));
      if (before_commit) {
        before_commit(result);
      }
      destroy_party(party_id);
      return result;
    }
  }

  const auto connection = conn_state_.find(member);
  if (connection == conn_state_.end()) {
    throw std::logic_error("Party member has no connection state");
  }
  auto update = update_event(party, {}, {}, false);
  std::erase(update.recipients, member);
  for (auto &descriptor : update.members) {
    if (descriptor.char_id == member) {
      descriptor.connection_state = ConnectionState::kRetain;
    }
  }
  result.events.push_back(std::move(update));
  // Prepare the node before invoking the output staging callback. The caller
  // serializes this service, so this provisional entry cannot be observed.
  const auto [retained, inserted] = retained_since_.try_emplace(member, now);
  try {
    if (before_commit) {
      before_commit(result);
    }
  } catch (...) {
    if (inserted) {
      retained_since_.erase(retained);
    }
    throw;
  }
  if (!inserted) {
    retained->second = now;
  }
  connection->second = ConnectionState::kRetain;
  if (party.leader == member) {
    party.invites.clear();
  }
  return result;
}

PartyResult PartyService::restore(CharacterId member) {
  const auto membership_it = membership_.find(member);
  if (membership_it == membership_.end()) {
    return fail(PartyError::kNotJoined);
  }
  Party &party = parties_.at(membership_it->second);
  const bool was_retained = state_of(member) == ConnectionState::kRetain;
  conn_state_[member] = ConnectionState::kConnected;
  retained_since_.erase(member);

  PartyResult result;
  PartyEvent join;
  join.kind = EventKind::kJoinNoti;
  join.party_id = party.id;
  join.recipients.push_back(member);
  join.leader_char_id = party.leader;
  join.members = roster(party);
  result.events.push_back(std::move(join));
  if (was_retained) {
    // Others learn the seat is Connected again through a roster update.
    result.events.push_back(update_event(party, {}, {}, false));
  }
  return result;
}


std::optional<PartyId> PartyService::party_of(CharacterId member) const {
  const auto it = membership_.find(member);
  if (it == membership_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<PartyId> PartyService::established_party_of(CharacterId member) const {
  const auto it = membership_.find(member);
  if (it == membership_.end()) {
    return std::nullopt;
  }

  if (parties_.at(it->second).temporary) {
    return std::nullopt;
  }

  return it->second;
}

std::vector<PartyEvent> PartyService::expire_retained(TimePoint now) {
  // Collect first: leave() mutates retained_since_ while it removes the seat.
  std::vector<CharacterId> expired_members;
  for (const auto &[member, since] : retained_since_) {
    if (now - since >= config_.retain_ttl) {
      expired_members.push_back(member);
    }
  }

  // Deterministic order keeps leader succession stable when several seats of
  // one party expire in the same tick.
  std::sort(expired_members.begin(), expired_members.end());

  std::vector<PartyEvent> events;
  for (const CharacterId member : expired_members) {
    auto released = leave(member, now);
    for (auto &event : released.events) {
      events.push_back(std::move(event));
    }
  }
  return events;
}

bool PartyService::is_leader(CharacterId member) const {
  const auto it = membership_.find(member);
  if (it == membership_.end()) {
    return false;
  }
  return parties_.at(it->second).leader == member;
}

std::vector<MemberDescriptor> PartyService::members_of(CharacterId member) const {
  const auto it = membership_.find(member);
  if (it == membership_.end()) {
    return {};
  }
  return roster(parties_.at(it->second));
}

std::size_t PartyService::party_count() const { return parties_.size(); }

std::size_t PartyService::tracked_inviter_count() const {
  return invite_times_.size();
}

std::vector<PartyEvent> PartyService::expire_invites(TimePoint now) {
  std::vector<PartyEvent> events;
  std::vector<PartyId> to_destroy;
  for (auto &[id, party] : parties_) {
    std::vector<Invite> expired;
    party.invites.erase(
        std::remove_if(party.invites.begin(), party.invites.end(),
                       [&](const Invite &invite) {
                         if (now >= invite.expires_at) {
                           expired.push_back(invite);
                           return true;
                         }
                         return false;
                       }),
        party.invites.end());
    if (expired.empty()) {
      continue;
    }
    // Every expired invite tells its invitee: the popup is dead.
    auto cancelled = cancel_invites(party, std::move(expired));
    for (auto &event : cancelled) {
      events.push_back(std::move(event));
    }
    if (party.temporary && party.invites.empty()) {
      PartyEvent remove;
      remove.kind = EventKind::kRemoveTemporaryNoti;
      remove.party_id = id;
      remove.recipients = connected_recipients(party);
      events.push_back(std::move(remove));
      to_destroy.push_back(id);
    }
  }
  for (const PartyId id : to_destroy) {
    destroy_party(id);
  }
  return events;
}

} // namespace chronicles::party
