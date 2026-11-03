// Behavioral tests for src/world/features/party/party_world.cpp: request decoding, unlock gating,
// connection addressing, unit party identity and member world-state sync.
// Every request is produced by encoding a real PC2S frame and decoding it
// with the recovered descriptors, and every outbound payload must round-trip
// through the same codec, so field names and wire types are schema-checked.

#include "world/features/party/party_world.hpp"
#include "protocol/protocol.hpp"
#include "world/core/instance_copies.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/features/quests/quest_intent_ledger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <map>
#include <new>

namespace chronicles::party {

struct PartyWorldTestAccess {
  static void forget_announcement(PartyWorld &world, CharacterId member) {
    std::lock_guard lock(world.mutex_);
    world.announced_party_.erase(member);
  }
};

namespace {

constexpr int kUnlockQuestId = 90100011;
constexpr std::uint32_t kSequence = 7;

const TimePoint kStart = Clock::time_point{};

class PartyWorldTest : public testing::Test {
protected:
  PartyWorldTest() : world_(make_config()) {}

  static PartyWorldConfig make_config() {
    PartyWorldConfig config;
    config.unlock_quest_id = kUnlockQuestId;
    config.world_state_interval = std::chrono::seconds(10);
    return config;
  }

  static MemberPresence presence(CharacterId id, bool unlocked = true) {
    MemberPresence member;
    member.char_id = id;
    member.connection_id = static_cast<std::uint64_t>(id) * 100;
    member.name = "member" + std::to_string(id);
    member.level = 5;
    member.char_class = 2;
    member.position = WorldPosition{8000, 1000 * static_cast<int>(id), 0, 0, 0.0F};
    member.hp = 900;
    member.max_hp = 1000;
    member.party_unlocked = unlocked;
    return member;
  }

  // Encodes a client request frame and returns the decoded message, exactly
  // as WorldHandshake hands it to the gameplay layer.
  nlohmann::json request(std::uint16_t opcode, const nlohmann::json &body) {
    const auto decoded = protocol_.decode(protocol_.encode(opcode, body, 0));
    EXPECT_EQ(decoded.at("status"), "decoded");
    return decoded.at("message");
  }

  PartyUpdate send(CharacterId requester, std::uint16_t opcode,
                   const nlohmann::json &body = nlohmann::json::object(),
                   TimePoint now = kStart) {
    const auto connection_id = static_cast<std::uint64_t>(requester) * 100;
    return world_.receive(requester, connection_id, opcode, request(opcode, body), kSequence,
                          now);
  }

  // Round-trips one outbound payload through the codec.
  nlohmann::json wire(const OutboundPacket &packet) {
    const auto decoded = protocol_.decode(protocol_.encode(packet.opcode, packet.message));
    EXPECT_EQ(decoded.at("status"), "decoded");
    return decoded.at("message");
  }

  static std::vector<const OutboundPacket *> addressed(const PartyUpdate &update,
                                                       CharacterId member,
                                                       std::uint16_t opcode) {
    std::vector<const OutboundPacket *> matches;
    for (const auto &packet : update.packets) {
      if (packet.connection_id == static_cast<std::uint64_t>(member) * 100 &&
          packet.opcode == opcode) {
        matches.push_back(&packet);
      }
    }
    return matches;
  }

  PartyId form_party(CharacterId leader, CharacterId member) {
    const auto invited = send(leader, kPartyInvitePopupRequest,
                              {{"target_char_id", std::to_string(member)}});
    const auto popups = addressed(invited, member, kPartyInvitePopupNotification);
    EXPECT_EQ(popups.size(), 1u);
    const auto party_id = wire(*popups.at(0)).at("party_id").get<std::string>();

    const auto accepted = send(member, kPartyAcceptInviteRequest,
                               {{"leader_char_id", std::to_string(leader)},
                                {"target_char_id", std::to_string(member)},
                                {"party_id", party_id}});
    EXPECT_EQ(addressed(accepted, member, kPartyJoinNotification).size(), 1u);
    return std::stoll(party_id);
  }

  Protocol protocol_{CHRONICLES_PROTOCOL_DIR};
  PartyWorld world_;
};

TEST_F(PartyWorldTest, InviteAcceptDeliversSchemaValidRosterAndUnitIdentity) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);

  const auto invited =
      send(1, kPartyInvitePopupRequest, {{"target_char_id", "2"}});
  ASSERT_EQ(invited.packets.size(), 1u);
  const auto popup = wire(invited.packets[0]);
  EXPECT_EQ(invited.packets[0].connection_id, 200u);
  EXPECT_EQ(popup.at("leader").at("name"), "member1");
  EXPECT_EQ(popup.at("target").at("char_id"), "2");
  // A lone inviter's temporary party is not shown on its unit.
  EXPECT_TRUE(invited.unit_changes.empty());

  const auto accepted = send(2, kPartyAcceptInviteRequest,
                             {{"leader_char_id", "1"},
                              {"target_char_id", "2"},
                              {"party_id", popup.at("party_id")}});
  const auto joins = addressed(accepted, 2, kPartyJoinNotification);
  ASSERT_EQ(joins.size(), 1u);
  const auto join = wire(*joins[0]);
  EXPECT_EQ(join.at("leader_char_id"), "1");
  ASSERT_EQ(join.at("members").size(), 2u);
  EXPECT_EQ(join.at("members").at(0).at("connection_state"),
            "EX5PartyMemberConnStateType_Connected");
  EXPECT_EQ(join.at("members").at(1).at("map_cms_id"), 8000);
  ASSERT_EQ(addressed(accepted, 1, kPartyMemberUpdateNotification).size(), 1u);
  ASSERT_EQ(addressed(accepted, 2, kPartyMemberUpdateNotification).size(), 1u);
  wire(*addressed(accepted, 1, kPartyMemberUpdateNotification)[0]);

  ASSERT_EQ(accepted.unit_changes.size(), 2u);
  for (const auto &change : accepted.unit_changes) {
    EXPECT_EQ(change.party_id, std::stoll(popup.at("party_id").get<std::string>()));
    const auto message = wire({change.subject_connection_id, kChangePartyUnitNotification,
                               PartyWorld::change_party_unit_message(change)});
    EXPECT_EQ(message.at("party_id"), popup.at("party_id"));
  }
}

TEST_F(PartyWorldTest, FailedDetachStagingLeavesMemberOnlineAndCanBeRetried) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);
  world_.attach(presence(3), kStart);
  const auto party = form_party(1, 2);
  const auto invited = send(1, kPartyInvitePopupRequest, {{"target_char_id", "3"}});
  ASSERT_EQ(addressed(invited, 3, kPartyInvitePopupNotification).size(), 1U);

  EXPECT_THROW(world_.detach_staged(1, 100, kStart, [](const PartyUpdate &update) {
    ASSERT_FALSE(update.packets.empty());
    throw std::bad_alloc();
  }), std::bad_alloc);

  EXPECT_TRUE(world_.online_presence(1));
  const auto accepted = send(3, kPartyAcceptInviteRequest,
                             {{"leader_char_id", "1"},
                              {"target_char_id", "3"},
                              {"party_id", std::to_string(party)}});
  ASSERT_EQ(addressed(accepted, 3, kPartyJoinNotification).size(), 1U);
  const auto detached = world_.detach(1, 100, kStart);
  EXPECT_FALSE(world_.online_presence(1));
  EXPECT_EQ(addressed(detached, 2, kPartyMemberUpdateNotification).size(), 1U);
  EXPECT_EQ(addressed(detached, 3, kPartyMemberUpdateNotification).size(), 1U);
}

TEST_F(PartyWorldTest, TemporaryLeaderDetachStagesTheInviteRejection) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);
  const auto invited = send(1, kPartyInvitePopupRequest, {{"target_char_id", "2"}});
  ASSERT_EQ(addressed(invited, 2, kPartyInvitePopupNotification).size(), 1U);

  PartyUpdate staged;
  const auto detached = world_.detach_staged(1, 100, kStart, [&](const PartyUpdate &update) {
    staged = update;
  });

  ASSERT_EQ(addressed(staged, 2, kPartyRejectInviteNotification).size(), 1U);
  ASSERT_EQ(detached.packets.size(), staged.packets.size());
  EXPECT_EQ(detached.packets[0].connection_id, staged.packets[0].connection_id);
  EXPECT_EQ(detached.packets[0].opcode, staged.packets[0].opcode);
  EXPECT_EQ(detached.packets[0].message, staged.packets[0].message);
  EXPECT_TRUE(staged.unit_changes.empty());
  EXPECT_TRUE(detached.unit_changes.empty());
  EXPECT_FALSE(world_.online_presence(1));
}

TEST_F(PartyWorldTest, NonPartyDetachStagesAnEmptyUpdateBeforeRemoval) {
  world_.attach(presence(1), kStart);
  bool staged = false;
  const auto detached = world_.detach_staged(1, 100, kStart, [&](const PartyUpdate &update) {
    EXPECT_TRUE(update.packets.empty());
    EXPECT_TRUE(update.unit_changes.empty());
    staged = true;
  });

  EXPECT_TRUE(staged);
  EXPECT_TRUE(detached.packets.empty());
  EXPECT_TRUE(detached.unit_changes.empty());
  EXPECT_FALSE(world_.online_presence(1));
}

TEST_F(PartyWorldTest, FailedStagingRollsBackPreparedUnitAnnouncement) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);
  world_.attach(presence(3), kStart);
  const auto party = form_party(1, 2);
  PartyWorldTestAccess::forget_announcement(world_, 2);

  EXPECT_THROW(world_.detach_staged(3, 300, kStart, [&](const PartyUpdate &update) {
    ASSERT_EQ(update.unit_changes.size(), 1U);
    EXPECT_EQ(update.unit_changes[0].char_id, 2);
    EXPECT_EQ(update.unit_changes[0].party_id, party);
    throw std::bad_alloc();
  }), std::bad_alloc);

  EXPECT_TRUE(world_.online_presence(3));
  PartyUpdate staged;
  const auto detached = world_.detach_staged(3, 300, kStart, [&](const PartyUpdate &update) {
    staged = update;
  });
  ASSERT_EQ(staged.unit_changes.size(), 1U);
  ASSERT_EQ(detached.unit_changes.size(), 1U);
  EXPECT_EQ(detached.unit_changes[0].char_id, staged.unit_changes[0].char_id);
  EXPECT_EQ(detached.unit_changes[0].party_id, staged.unit_changes[0].party_id);
  EXPECT_FALSE(world_.online_presence(3));
  EXPECT_TRUE(world_.tick(kStart).unit_changes.empty());
}

TEST_F(PartyWorldTest, UnlockQuestGatesInviterInviteeAndAccepter) {
  world_.attach(presence(1, false), kStart);
  world_.attach(presence(2), kStart);
  world_.attach(presence(3, false), kStart);

  const auto locked_inviter = send(1, kPartyInvitePopupRequest, {{"target_char_id", "2"}});
  ASSERT_EQ(locked_inviter.packets.size(), 1u);
  EXPECT_EQ(locked_inviter.packets[0].connection_id, 100u);
  const auto error = wire(locked_inviter.packets[0]);
  EXPECT_EQ(error.at("ec"), "Party_UnlockPartyContentRequired");
  EXPECT_EQ(error.at("category"), "EErrorCategory_Party");
  EXPECT_EQ(error.at("packet_seq").at("seq"), std::to_string(kSequence));

  const auto locked_invitee = send(2, kPartyInvitePopupRequest, {{"target_char_id", "3"}});
  ASSERT_EQ(locked_invitee.packets.size(), 1u);
  EXPECT_EQ(wire(locked_invitee.packets[0]).at("ec"), "Party_UnlockPartyContentRequired");
}

TEST_F(PartyWorldTest, OfflineTargetAndUnattachedRequesterAreRefused) {
  world_.attach(presence(1), kStart);

  const auto offline = send(1, kPartyInvitePopupRequest, {{"target_char_id", "9"}});
  ASSERT_EQ(offline.packets.size(), 1u);
  EXPECT_EQ(wire(offline.packets[0]).at("ec"), "Party_NotExistsInvitee");

  const auto unattached = send(5, kPartyLeaveRequest);
  ASSERT_EQ(unattached.packets.size(), 1u);
  EXPECT_EQ(unattached.packets[0].connection_id, 500u);
  EXPECT_EQ(wire(unattached.packets[0]).at("ec"), "Party_NotJoined");

  const auto self_invite = send(1, kPartyInvitePopupRequest, {{"target_char_id", "1"}});
  ASSERT_EQ(self_invite.packets.size(), 1u);
  EXPECT_EQ(wire(self_invite.packets[0]).at("ec"), "Party_NotExistsInvitee");
}

TEST_F(PartyWorldTest, MalformedIdentifiersAreRefusedWithoutStateChange) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);

  const auto invite = world_.receive(1, 100, kPartyInvitePopupRequest,
                                     {{"target_char_id", "not-a-number"}}, kSequence, kStart);
  ASSERT_EQ(invite.packets.size(), 1u);
  EXPECT_EQ(wire(invite.packets[0]).at("ec"), "Party_NotExistsInvitee");

  const auto kick = world_.receive(1, 100, kPartyKickRequest, {{"target_char_id", true}},
                                   kSequence, kStart);
  ASSERT_EQ(kick.packets.size(), 1u);
  EXPECT_EQ(wire(kick.packets[0]).at("ec"), "Party_NotExistsMember");
  EXPECT_FALSE(world_.active());
}

TEST_F(PartyWorldTest, WorldStateSyncReportsVitalsPositionAndChannel) {
  auto leader = presence(1);
  auto member = presence(2);
  // Member 2 is in its own private tutorial instance.
  member.channel_key = 2;
  world_.attach(leader, kStart);
  world_.attach(member, kStart);
  form_party(1, 2);

  member.hp = 0;
  world_.refresh(member);

  // The roster change forces an immediate sync on the next tick.
  const auto synced = world_.tick(kStart + std::chrono::seconds(1));
  const auto to_leader = addressed(synced, 1, kPartyMemberWorldStateNotification);
  ASSERT_EQ(to_leader.size(), 1u);
  const auto state = wire(*to_leader[0]);
  EXPECT_EQ(state.at("member").at("char_id"), "2");
  EXPECT_EQ(state.at("member").at("max_hp"), 1000);
  EXPECT_TRUE(state.at("member").at("is_dead").get<bool>());
  EXPECT_EQ(state.at("member").at("pos").at("millimeter_x"), 2000);
  EXPECT_FALSE(state.value("same_channel", false));
  ASSERT_EQ(addressed(synced, 2, kPartyMemberWorldStateNotification).size(), 1u);

  // Nothing new until PartyMemberWorldStateSyncMillis elapses.
  const auto quiet = world_.tick(kStart + std::chrono::seconds(5));
  EXPECT_TRUE(addressed(quiet, 1, kPartyMemberWorldStateNotification).empty());
  const auto periodic = world_.tick(kStart + std::chrono::seconds(11));
  EXPECT_EQ(addressed(periodic, 1, kPartyMemberWorldStateNotification).size(), 1u);
}

TEST_F(PartyWorldTest, SharedChannelIsReportedForSameMapAndInstance) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);
  form_party(1, 2);

  const auto synced = world_.tick(kStart);
  const auto to_leader = addressed(synced, 1, kPartyMemberWorldStateNotification);
  ASSERT_EQ(to_leader.size(), 1u);
  EXPECT_TRUE(wire(*to_leader[0]).at("same_channel").get<bool>());
}

TEST_F(PartyWorldTest, NearbyPartyMembersFilterByInstanceMapRangeAndConnection) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);
  auto far_member = presence(3);
  far_member.position->x_mm = 900000;
  world_.attach(far_member, kStart);
  auto other_instance = presence(4);
  other_instance.channel_key = 77;
  world_.attach(other_instance, kStart);
  world_.attach(presence(5), kStart);
  form_party(1, 2);
  form_party(1, 3);
  form_party(1, 4);

  const WorldPosition kill{8000, 1000, 0, 0, 0.0F};
  const auto ids = [](const std::vector<PartyWorld::NearbyMember> &members) {
    std::vector<CharacterId> result;
    for (const auto &member : members) {
      result.push_back(member.char_id);
    }
    return result;
  };

  // Only member 2 is in the same instance and within 5 m; 3 is far and 4 is elsewhere.
  const auto close = world_.nearby_party_members(1, kill, 0, 5000.0);
  EXPECT_EQ(ids(close), (std::vector<CharacterId>{2}));
  EXPECT_EQ(close.at(0).connection_id, 200u);
  EXPECT_EQ(ids(world_.nearby_party_members(1, kill, 0, 2000000.0)),
            (std::vector<CharacterId>{2, 3}));
  EXPECT_EQ(ids(world_.nearby_party_members(1, kill, 77, 2000000.0)),
            (std::vector<CharacterId>{4}));

  // Another map, a character outside the party and a disconnected member yield nothing.
  const WorldPosition elsewhere{9000, 1000, 0, 0, 0.0F};
  EXPECT_TRUE(world_.nearby_party_members(1, elsewhere, 0, 2000000.0).empty());
  EXPECT_TRUE(world_.nearby_party_members(5, kill, 0, 2000000.0).empty());
  world_.detach(2, 200, kStart);
  EXPECT_EQ(ids(world_.nearby_party_members(1, kill, 0, 2000000.0)),
            (std::vector<CharacterId>{3}));
  // The asking member is never listed; the others in the party are.
  EXPECT_EQ(ids(world_.nearby_party_members(3, kill, 0, 2000000.0)),
            (std::vector<CharacterId>{1}));
}

TEST_F(PartyWorldTest, NearbyDungeonCreditSnapshotsThePartyCopyAndRecipientPosition) {
  constexpr std::uint64_t kCopySerial = 17;
  constexpr std::uint64_t kPartyChannel = kCopyChannelKeyBit | kCopySerial;
  auto killer = presence(1);
  killer.account_id = 11;
  killer.position = WorldPosition{10111, 1000, 0, 0, 0.0F};
  killer.channel_key = kPartyChannel;
  auto member = presence(2);
  member.account_id = 12;
  member.position = WorldPosition{10111, 2500, 0, 0, 0.0F};
  member.channel_key = kPartyChannel;
  world_.attach(killer, kStart);
  world_.attach(member, kStart);
  form_party(1, 2);

  const auto selected = world_.nearby_party_members(1, *killer.position, kPartyChannel, 30000.0);
  ASSERT_EQ(selected.size(), 1U);
  EXPECT_EQ(selected.front().account_id, 12);
  EXPECT_EQ(selected.front().char_id, 2);
  EXPECT_EQ(selected.front().position, *member.position);
  EXPECT_TRUE(selected.front().alive);
  EXPECT_TRUE(world_.nearby_party_members(1, *killer.position, kCopySerial, 30000.0).empty());

  // Later party departure cannot change the already captured eligibility or position.
  world_.detach(1, 100, kStart);
  EXPECT_EQ(selected.front().position, *member.position);
  EXPECT_EQ(selected.front().char_id, 2);
}

TEST_F(PartyWorldTest, EligibilityCaptureOrdersPartyDepartureAndQuestMutation) {
  auto killer = presence(1);
  auto member = presence(2);
  world_.attach(killer, kStart);
  world_.attach(member, kStart);
  form_party(1, 2);

  WorldProgress initial;
  initial.quests[501] = QuestProgress{};
  auto ledger = std::make_shared<quests::QuestIntentLedger>(initial);
  std::mutex quest_order;
  std::promise<void> capture_entered;
  std::promise<void> continue_capture;
  auto continue_future = continue_capture.get_future();
  std::shared_ptr<quests::QuestIntentLedger::CaptureLease> lease;
  std::size_t selected_count = 0;

  auto capture_task = std::async(std::launch::async, [&] {
    world_.capture_nearby_party_members(
        1, *killer.position, 0, 30000.0,
        [&](const std::optional<MemberPresence> &,
            const std::vector<PartyWorld::NearbyMember> &selected) {
          std::lock_guard lock(quest_order);
          lease = ledger->acquire_capture();
          selected_count = selected.size();
          capture_entered.set_value();
          continue_future.wait();
        });
  });
  capture_entered.get_future().wait();
  auto departure = std::async(std::launch::async, [&] {
    world_.detach(2, 200, kStart);
  });
  EXPECT_EQ(departure.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
  continue_capture.set_value();
  capture_task.get();
  departure.get();

  WorldProgress later = initial;
  later.quests[501].attempt = 1;
  {
    std::lock_guard lock(quest_order);
    const auto intent = ledger->begin_mutation(later);
    ASSERT_TRUE(intent);
    ledger->finish_prepared(*intent, true);
  }
  ASSERT_TRUE(lease);
  const auto snapshot = lease->resolve();
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(selected_count, 1U);
  EXPECT_EQ(snapshot->at(501).attempt, 0);
}

TEST_F(PartyWorldTest, ConnectionOfReportsOnlyOnlineMembers) {
  world_.attach(presence(1), kStart);

  EXPECT_EQ(world_.connection_of(1), 100u);
  EXPECT_FALSE(world_.connection_of(99).has_value());
  world_.detach(1, 100, kStart);
  EXPECT_FALSE(world_.connection_of(1).has_value());
}

TEST_F(PartyWorldTest, DisconnectRetainsSeatAndReconnectRestoresSnapshot) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);
  const auto party_id = form_party(1, 2);

  // A stale connection cannot detach the live session.
  EXPECT_TRUE(world_.detach(2, 999, kStart).packets.empty());

  const auto dropped = world_.detach(2, 200, kStart);
  const auto updates = addressed(dropped, 1, kPartyMemberUpdateNotification);
  ASSERT_EQ(updates.size(), 1u);
  const auto roster = wire(*updates[0]).at("members");
  const auto retained = std::find_if(roster.begin(), roster.end(), [](const auto &member) {
    return member.at("char_id") == "2";
  });
  ASSERT_NE(retained, roster.end());
  EXPECT_EQ(retained->at("connection_state"), "EX5PartyMemberConnStateType_Retain");
  EXPECT_EQ(retained->at("name"), "member2");
  EXPECT_EQ(world_.party_connections(1), std::vector<std::uint64_t>({100}));

  auto returning = presence(2);
  returning.connection_id = 201;
  const auto restored = world_.attach(returning, kStart + std::chrono::seconds(5));
  ASSERT_EQ(restored.unit_changes.size(), 1u);
  EXPECT_EQ(restored.unit_changes[0].subject_connection_id, 201u);
  EXPECT_EQ(restored.unit_changes[0].party_id, party_id);
  const auto rejoin = std::find_if(restored.packets.begin(), restored.packets.end(),
                                   [](const OutboundPacket &packet) {
                                     return packet.connection_id == 201 &&
                                            packet.opcode == kPartyJoinNotification;
                                   });
  ASSERT_NE(rejoin, restored.packets.end());
  EXPECT_EQ(wire(*rejoin).at("party_id"), std::to_string(party_id));

  auto connections = world_.party_connections(2);
  std::sort(connections.begin(), connections.end());
  EXPECT_EQ(connections, std::vector<std::uint64_t>({100, 201}));
}

TEST_F(PartyWorldTest, LeaveClearsUnitIdentityAndUnitAddDecoration) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);
  world_.attach(presence(3), kStart);
  const auto party_id = form_party(1, 2);

  nlohmann::json grouped{
      {"unit_id", std::to_string(player_wire_unit_id(2))},
      {"nub", {{"player", {{"char_id", "2"}}}}}};
  world_.decorate_unit_add(grouped);
  EXPECT_EQ(grouped.at("nub").at("player").at("party_id"), std::to_string(party_id));

  nlohmann::json solo{
      {"unit_id", std::to_string(player_wire_unit_id(3))},
      {"nub", {{"player", {{"char_id", "3"}}}}}};
  world_.decorate_unit_add(solo);
  EXPECT_FALSE(solo.at("nub").at("player").contains("party_id"));

  const auto left = send(2, kPartyLeaveRequest);
  ASSERT_EQ(addressed(left, 2, kPartyLeaveNotification).size(), 1u);
  ASSERT_EQ(left.unit_changes.size(), 1u);
  EXPECT_EQ(left.unit_changes[0].char_id, 2);
  EXPECT_EQ(left.unit_changes[0].party_id, 0);
  const auto cleared = PartyWorld::change_party_unit_message(left.unit_changes[0]);
  EXPECT_FALSE(cleared.contains("party_id"));
  EXPECT_EQ(cleared.at("unit_id"), std::to_string(player_wire_unit_id(2)));
  wire({200, kChangePartyUnitNotification, cleared});
  EXPECT_TRUE(world_.party_connections(2).empty());
}

TEST_F(PartyWorldTest, KickAndLeaderChangeRequireLeaderAndEncode) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);
  world_.attach(presence(3), kStart);
  const auto party_id = form_party(1, 2);
  ASSERT_EQ(form_party(1, 3), party_id);

  const auto refused = send(2, kPartyKickRequest, {{"target_char_id", "3"}});
  ASSERT_EQ(refused.packets.size(), 1u);
  EXPECT_EQ(wire(refused.packets[0]).at("ec"), "Party_LeaderPrivilege");

  const auto promoted = send(1, kPartyChangeLeaderRequest, {{"target_char_id", "2"}});
  const auto updates = addressed(promoted, 3, kPartyMemberUpdateNotification);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(wire(*updates[0]).at("leader_char_id"), "2");

  const auto kicked = send(2, kPartyKickRequest, {{"target_char_id", "3"}});
  const auto notices = addressed(kicked, 3, kPartyLeaveNotification);
  ASSERT_EQ(notices.size(), 1u);
  EXPECT_TRUE(wire(*notices[0]).at("kicked").get<bool>());
}

TEST_F(PartyWorldTest, RejectNotifiesLeaderAndExpiryClearsPopup) {
  world_.attach(presence(1), kStart);
  world_.attach(presence(2), kStart);
  world_.attach(presence(3), kStart);

  send(1, kPartyInvitePopupRequest, {{"target_char_id", "2"}});
  const auto rejected =
      send(2, kPartyRejectInviteRequest, {{"leader_char_id", "1"}, {"target_char_id", "2"}});
  const auto notices = addressed(rejected, 1, kPartyRejectInviteNotification);
  ASSERT_EQ(notices.size(), 1u);
  EXPECT_EQ(wire(*notices[0]).at("target").at("char_id"), "2");
  ASSERT_EQ(addressed(rejected, 1, kPartyRemoveTemporaryNotification).size(), 1u);
  EXPECT_FALSE(world_.active());

  send(1, kPartyInvitePopupRequest, {{"target_char_id", "3"}});
  EXPECT_TRUE(world_.active());
  // GameVariable PartyInviteTimeoutMillis = 20000.
  const auto expired = world_.tick(kStart + std::chrono::milliseconds(20000));
  ASSERT_EQ(addressed(expired, 3, kPartyRejectInviteNotification).size(), 1u);
  EXPECT_FALSE(world_.active());
}

TEST(PartyUnlockTest, RequiresCompletedUnlockQuest) {
  WorldProgress progress;
  EXPECT_TRUE(party_unlocked(progress, 0));
  EXPECT_FALSE(party_unlocked(progress, kUnlockQuestId));

  progress.quests[kUnlockQuestId].complete = false;
  EXPECT_FALSE(party_unlocked(progress, kUnlockQuestId));

  progress.quests[kUnlockQuestId].complete = true;
  EXPECT_TRUE(party_unlocked(progress, kUnlockQuestId));
}

TEST(PartyWorldStaticTest, HandlesOnlyMembershipRequests) {
  EXPECT_TRUE(PartyWorld::handles(kPartyKickRequest));
  EXPECT_TRUE(PartyWorld::handles(kPartyRejectInviteRequest));
  // PartyChannelInvite and later channel-travel requests stay unhandled.
  EXPECT_FALSE(PartyWorld::handles(20100));
  EXPECT_FALSE(PartyWorld::handles(20093));
}

} // namespace
} // namespace chronicles::party
