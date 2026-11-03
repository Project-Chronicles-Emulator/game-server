#include "world/features/events/raid_channel.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace chronicles::raid {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr int kRaidMap = 10103;
constexpr int kOtherRaidMap = 20229;

std::vector<Json> raid_ui_rows() {
  return {{{"id", "1"}, {"FK_raidMapId", "10103"}, {"FK_icon", "99980002"}},
          {{"id", "2"}, {"FK_raidMapId", "20229"}, {"FK_icon", "99980000"}}};
}

std::vector<Json> map_rows(const char *capacity = "3") {
  return {{{"id", "10103"},
           {"type", "Raid"},
           {"parentMapId", "9012"},
           {"chMaxCount", capacity},
           {"timeLimit", "540"}},
          {{"id", "20229"},
           {"type", "Raid"},
           {"parentMapId", "9021"},
           {"chMaxCount", capacity},
           {"timeLimit", "1200"}},
          {{"id", "9012"}, {"type", "Field"}}};
}

std::shared_ptr<const RaidCatalog> catalog(const char *capacity = "3") {
  return std::make_shared<const RaidCatalog>(raid_ui_rows(), map_rows(capacity), 10000ms);
}

RaidApplicant applicant(std::int64_t char_id) {
  return {char_id, static_cast<std::uint64_t>(char_id) * 10, "Raider", 8,
          static_cast<int>(char_id % 3)};
}

Json map_request(int map_id) {
  return {{"map_id", map_id}};
}

std::vector<RaidPacket> addressed(const RaidUpdate &update, std::uint64_t connection_id,
                                  std::uint16_t opcode) {
  std::vector<RaidPacket> result;
  std::ranges::copy_if(update.packets, std::back_inserter(result), [&](const RaidPacket &packet) {
    return packet.connection_id == connection_id && packet.opcode == opcode;
  });
  return result;
}

class RaidMatchmakerTest : public ::testing::Test {
protected:
  RaidUpdate register_member(std::int64_t char_id, int map_id = kRaidMap, bool eligible = true) {
    return matchmaker.receive(applicant(char_id), kRaidChannelRegisterRequest, map_request(map_id),
                              7, eligible, now);
  }

  Clock::time_point now{};
  RaidMatchmakerConfig config{30s, 1};
  RaidMatchmaker matchmaker{catalog(), config};
};

TEST(RaidCatalogTest, ReadsRaidMapsAndRejectsNonRaidRows) {
  const auto raids = catalog();
  ASSERT_NE(raids->find(kRaidMap), nullptr);
  EXPECT_EQ(raids->find(kRaidMap)->parent_map_id, 9012);
  EXPECT_EQ(raids->find(kRaidMap)->time_limit, 540s);
  EXPECT_EQ(raids->auto_enter_delay(), 10000ms);
  EXPECT_EQ(raids->find(9012), nullptr);

  const std::vector<Json> field_raid{{{"id", "1"}, {"FK_raidMapId", "9012"}}};
  EXPECT_THROW(RaidCatalog(field_raid, map_rows(), 0ms), std::runtime_error);
}

TEST_F(RaidMatchmakerTest, RegistrationIsAcknowledgedAndValidated) {
  const auto registered = register_member(1);
  ASSERT_EQ(addressed(registered, 10, kRaidChannelRegisterNotification).size(), 1U);
  EXPECT_EQ(registered.packets.front().message, map_request(kRaidMap));
  EXPECT_EQ(matchmaker.membership(1).state, RaidState::waiting);
  EXPECT_EQ(matchmaker.membership(1).channel_id, 0u);

  const auto again = register_member(1, kOtherRaidMap);
  EXPECT_EQ(again.packets.front().message.at("ec"), "Raid_AlreadyApplied");
  EXPECT_EQ(again.packets.front().message.at("packet_seq").at("seq"), "7");

  EXPECT_EQ(register_member(2, 9012).packets.front().message.at("ec"), "Raid_InvalidChannelId");
  EXPECT_EQ(register_member(3, kRaidMap, false).packets.front().message.at("ec"),
            "Raid_RegisterDeclined");
}

TEST_F(RaidMatchmakerTest, FullChannelMatchesAtOnceWithEveryMember) {
  register_member(1);
  register_member(2);
  const auto third = register_member(3);

  for (const std::uint64_t connection : {10U, 20U, 30U}) {
    const auto matched = addressed(third, connection, kRaidChannelMatchedNotification);
    ASSERT_EQ(matched.size(), 1U);
    EXPECT_EQ(matched.front().message.at("map_id"), kRaidMap);
    EXPECT_EQ(matched.front().message.at("members").size(), 3U);
  }
  EXPECT_EQ(third.packets.front().opcode, kRaidChannelRegisterNotification);
  EXPECT_EQ(matchmaker.membership(2).state, RaidState::matched);
}

TEST_F(RaidMatchmakerTest, PartialChannelFormsAfterTheWait) {
  register_member(1);
  now += 29s;
  EXPECT_TRUE(matchmaker.tick(now).packets.empty());

  now += 1s;
  const auto formed = matchmaker.tick(now);
  ASSERT_EQ(addressed(formed, 10, kRaidChannelMatchedNotification).size(), 1U);
  EXPECT_EQ(matchmaker.membership(1).state, RaidState::matched);
}

TEST_F(RaidMatchmakerTest, CancelRemovesTheApplication) {
  register_member(1);
  const auto cancelled = matchmaker.receive(applicant(1), kRaidChannelCancelRequest,
                                            map_request(kRaidMap), 8, true, now);
  ASSERT_EQ(addressed(cancelled, 10, kRaidChannelCancelNotification).size(), 1U);
  EXPECT_EQ(matchmaker.membership(1).state, RaidState::none);

  const auto nothing = matchmaker.receive(applicant(1), kRaidChannelCancelRequest,
                                          map_request(kRaidMap), 9, true, now);
  EXPECT_EQ(nothing.packets.front().message.at("ec"), "Raid_NoChannelToCancel");
}

TEST_F(RaidMatchmakerTest, LeaveNotifiesEveryMemberAndEvictsTheLeaver) {
  register_member(1);
  register_member(2);
  register_member(3);

  const auto left = matchmaker.receive(applicant(2), kRaidChannelLeaveRequest,
                                       map_request(kRaidMap), 11, true, now);
  for (const std::uint64_t connection : {10U, 20U, 30U}) {
    const auto notices = addressed(left, connection, kRaidChannelLeaveNotification);
    ASSERT_EQ(notices.size(), 1U);
    EXPECT_EQ(notices.front().message.at("char_id"), "2");
  }
  ASSERT_EQ(left.evictions.size(), 1U);
  EXPECT_EQ(left.evictions.front().connection_id, 20U);
  EXPECT_EQ(matchmaker.membership(2).state, RaidState::none);

  const auto stranger = matchmaker.receive(applicant(2), kRaidChannelLeaveRequest,
                                           map_request(kRaidMap), 12, true, now);
  EXPECT_EQ(stranger.packets.front().message.at("ec"), "Raid_RequesterNotFound");
}

TEST_F(RaidMatchmakerTest, MoveIsAuthorizedOnlyForTheMatchedMap) {
  EXPECT_EQ(matchmaker.authorize_move(1, map_request(kRaidMap), 10, now + 10s).error,
            "Raid_RequesterNotFound");

  matchmaker.force_match(applicant(1), kRaidMap, now);
  const auto allowed = matchmaker.authorize_move(1, map_request(kRaidMap), 10, now + 10s);
  EXPECT_FALSE(allowed.error.has_value());
  ASSERT_NE(allowed.map, nullptr);
  EXPECT_EQ(allowed.map->map_id, kRaidMap);
  EXPECT_EQ(matchmaker.authorize_move(1, map_request(kOtherRaidMap), 10, now + 10s).error,
            "Raid_InvalidChannelId");
}

TEST_F(RaidMatchmakerTest, MoveRejectsEarlyExpiredAndStaleConnectionRequests) {
  matchmaker.force_match(applicant(1), kRaidMap, now);
  EXPECT_EQ(matchmaker.authorize_move(1, map_request(kRaidMap), 10, now).error,
            "Raid_TimeUnavailable");
  EXPECT_EQ(matchmaker.authorize_move(1, map_request(kRaidMap), 999, now + 10s).error,
            "Raid_RequesterNotFound");
  EXPECT_EQ(matchmaker.authorize_move(1, map_request(kRaidMap), 10, now + 550s).error,
            "Raid_TimeUnavailable");
}

TEST_F(RaidMatchmakerTest, ChannelClosesAfterCountdownAndTimeLimit) {
  matchmaker.force_match(applicant(1), kRaidMap, now);
  now += 10s + 539s;
  EXPECT_TRUE(matchmaker.tick(now).packets.empty());

  now += 1s;
  const auto closed = matchmaker.tick(now);
  ASSERT_EQ(addressed(closed, 10, kRaidChannelDestroyNotification).size(), 1U);
  ASSERT_EQ(closed.evictions.size(), 1U);
  EXPECT_TRUE(matchmaker.active());
  matchmaker.complete_eviction(1, 10, kRaidMap);
  EXPECT_FALSE(matchmaker.active());
}

TEST_F(RaidMatchmakerTest, ASecondGroupWaitsUntilTheFirstChannelCloses) {
  register_member(1);
  register_member(2);
  register_member(3);
  const auto first_channel = matchmaker.membership(1).channel_id;
  EXPECT_NE(first_channel, 0u);
  EXPECT_EQ(matchmaker.membership(2).channel_id, first_channel);
  register_member(4);
  register_member(5);
  const auto sixth = register_member(6);
  EXPECT_TRUE(addressed(sixth, 40, kRaidChannelMatchedNotification).empty());
  EXPECT_EQ(matchmaker.membership(4).state, RaidState::waiting);
  EXPECT_TRUE(matchmaker.tick(now + 30s).packets.empty());

  const auto closed = matchmaker.tick(now + 550s);
  EXPECT_EQ(addressed(closed, 10, kRaidChannelDestroyNotification).size(), 1U);
  matchmaker.complete_eviction(1, 10, kRaidMap);
  matchmaker.complete_eviction(2, 20, kRaidMap);
  EXPECT_TRUE(matchmaker.tick(now + 551s).packets.empty());
  matchmaker.complete_eviction(3, 30, kRaidMap);
  const auto next = matchmaker.tick(now + 552s);
  EXPECT_EQ(addressed(next, 40, kRaidChannelMatchedNotification).size(), 1U);
  EXPECT_NE(matchmaker.membership(4).channel_id, first_channel);
}

TEST_F(RaidMatchmakerTest, LastLeaveKeepsTheMapReservedUntilEvictionCompletes) {
  matchmaker.force_match(applicant(1), kRaidMap, now);
  const auto left = matchmaker.receive(applicant(1), kRaidChannelLeaveRequest,
                                       map_request(kRaidMap), 11, true, now);
  ASSERT_EQ(left.evictions.size(), 1U);

  const auto next = matchmaker.force_match(applicant(2), kRaidMap, now);
  ASSERT_EQ(next.packets.size(), 1U);
  EXPECT_EQ(next.packets.front().opcode, kCommonErrorResponse);
  EXPECT_EQ(matchmaker.membership(2).state, RaidState::none);

  // Acknowledgements from another connection/map cannot release the reservation.
  matchmaker.complete_eviction(1, 999, kRaidMap);
  matchmaker.complete_eviction(1, 10, kOtherRaidMap);
  EXPECT_EQ(matchmaker.force_match(applicant(2), kRaidMap, now).packets.front().opcode,
            kCommonErrorResponse);
  EXPECT_EQ(register_member(1).packets.front().message.at("ec"), "Raid_AlreadyApplied");

  matchmaker.complete_eviction(1, 10, kRaidMap);
  EXPECT_EQ(matchmaker.force_match(applicant(2), kRaidMap, now).packets.front().opcode,
            kRaidChannelMatchedNotification);
}

TEST_F(RaidMatchmakerTest, TimeoutKeepsQueuedGroupsWaitingUntilEvictionCompletes) {
  matchmaker.force_match(applicant(1), kRaidMap, now);
  register_member(2);
  const auto closed = matchmaker.tick(now + 550s);
  ASSERT_EQ(closed.evictions.size(), 1U);

  const auto next = matchmaker.tick(now + 551s);
  EXPECT_TRUE(addressed(next, 20, kRaidChannelMatchedNotification).empty());
  EXPECT_EQ(matchmaker.membership(2).state, RaidState::waiting);

  // Disconnect cleanup also releases a pending eviction, but only for its owning connection.
  matchmaker.detach(1, 999);
  EXPECT_TRUE(matchmaker.tick(now + 552s).packets.empty());
  matchmaker.detach(1, 10);
  EXPECT_EQ(addressed(matchmaker.tick(now + 553s), 20, kRaidChannelMatchedNotification).size(), 1U);
}

TEST_F(RaidMatchmakerTest, AStaleConnectionCannotCancelOrLeaveAnotherConnectionSeat) {
  register_member(1);
  auto stale = applicant(1);
  stale.connection_id = 999;
  const auto cancel =
      matchmaker.receive(stale, kRaidChannelCancelRequest, map_request(kRaidMap), 1, true, now);
  EXPECT_EQ(cancel.packets.front().opcode, kCommonErrorResponse);
  EXPECT_EQ(matchmaker.membership(1).state, RaidState::waiting);

  register_member(2);
  register_member(3);
  const auto leave =
      matchmaker.receive(stale, kRaidChannelLeaveRequest, map_request(kRaidMap), 2, true, now);
  EXPECT_EQ(leave.packets.front().opcode, kCommonErrorResponse);
  EXPECT_EQ(matchmaker.membership(1).state, RaidState::matched);
}

TEST_F(RaidMatchmakerTest, DetachIgnoresStaleConnections) {
  register_member(1);
  register_member(2);
  register_member(3);

  EXPECT_TRUE(matchmaker.detach(2, 999).packets.empty());
  EXPECT_EQ(matchmaker.membership(2).state, RaidState::matched);

  const auto detached = matchmaker.detach(2, 20);
  EXPECT_TRUE(addressed(detached, 20, kRaidChannelLeaveNotification).empty());
  EXPECT_EQ(addressed(detached, 10, kRaidChannelLeaveNotification).size(), 1U);
  EXPECT_TRUE(detached.evictions.empty());
}

} // namespace
} // namespace chronicles::raid
