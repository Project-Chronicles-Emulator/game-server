#include "protocol/protocol.hpp"
#include "world/features/events/channel_event.hpp"
#include "world/features/events/preorder_bonus.hpp"
#include "world/features/events/raid_channel.hpp"
#include "world/features/events/ranking.hpp"

#include <gtest/gtest.h>

// Every server packet these services produce must encode against the recovered descriptors
// and decode back to the same field values, so a misspelled field or wrong scalar type fails
// here instead of being dropped by the client.
namespace chronicles {
namespace {

using Json = nlohmann::json;

class EventProtocolTest : public ::testing::Test {
protected:
  Json round_trip(const GameplayPacket &packet) {
    const auto frame = protocol.encode(packet.opcode, packet.message);
    EXPECT_EQ(frame.opcode(), packet.opcode);
    return protocol.decode(frame).at("message");
  }

  Protocol protocol{CHRONICLES_PROTOCOL_DIR};
};

TEST_F(EventProtocolTest, RankingPacketsEncode) {
  RankingRecord record;
  record.world_id = 1;
  record.ranking_cms_id = 6000201;
  record.period_start_epoch_ticks = 17908488000000000;
  record.members = {{9, "Leader", 8, 2}, {4, "Second", 7, 1}};
  record.holder_key = ranking::holder_key(ranking::RankingType::party, record.members);
  record.score = 61250;
  record.created_epoch_ticks = 17928731954228216;

  Json list{{"rankings", Json::array({ranking::ranking_info(record, 1, 1)})},
            {"next_timestamp", "17928732053300590"},
            {"reset_counter", "1"},
            {"ranking_cms_id", 6000201},
            {"ranking_record_type", "ERankingRecordType_World"},
            {"my_ranking", ranking::ranking_info(record, 1, 1)},
            {"next_reset_epochtime", "17935272000000000"}};
  const auto decoded = round_trip({ranking::kRankingListResponse, list});
  EXPECT_EQ(decoded.at("rankings")[0].at("core_info").at("party_members")[0].at("name"), "Second");
  EXPECT_EQ(decoded.at("my_ranking").at("score"), "61250");
  EXPECT_EQ(decoded.at("next_reset_epochtime"), "17935272000000000");

  round_trip({ranking::kRankingRecordStartResponse,
              {{"ranking_cms_id", 1000001}, {"start_tick", "298146235484000"}}});
  const auto record_res = round_trip({ranking::kRankingRecordResponse,
                                      {{"ranking_cms_id", 6000201},
                                       {"time_record_milli", "61250"},
                                       {"party_ranking", ranking::ranking_info(record, 3, 1)}}});
  EXPECT_EQ(record_res.at("party_ranking").at("rank"), "3");
  EXPECT_EQ(round_trip({ranking::kRankingListResponse,
                        {{"ec", "Ranking_InvalidCmsId"}, {"ranking_cms_id", 5}}})
                .at("ec"),
            "Ranking_InvalidCmsId");
}

TEST_F(EventProtocolTest, PreorderRepliesMatchRecoveredWireDefaultsAndCorrelation) {
  const auto state = preorder_bonus::respond_without_entitlement(
      {preorder_bonus::kStateRequest, 31, Json::object()}, true);
  const auto decoded_state = round_trip(state);
  EXPECT_EQ(decoded_state, Json({{"packet_seq", Json::object()}}));

  const auto claim = preorder_bonus::respond_without_entitlement(
      {preorder_bonus::kClaimRequest, 32, {{"packet_seq", {{"seq", "9876543210"}}}}}, true);
  const auto decoded_claim = round_trip(claim);
  EXPECT_EQ(decoded_claim.at("ec"), "PreorderBonus_NotEligible");
  EXPECT_EQ(decoded_claim.at("packet_seq").at("seq"), "9876543210");
}

TEST_F(EventProtocolTest, RaidPacketsEncode) {
  const Json members =
      Json::array({{{"char_id", "19"}, {"name", "Raider"}, {"level", 8}, {"char_class", 2}}});
  const auto matched = round_trip(
      {raid::kRaidChannelMatchedNotification, {{"map_id", 10103}, {"members", members}}});
  EXPECT_EQ(matched.at("members")[0].at("char_id"), "19");

  round_trip({raid::kRaidChannelRegisterNotification, {{"map_id", 10103}}});
  round_trip({raid::kRaidChannelCancelNotification, {{"map_id", 10103}}});
  round_trip({raid::kRaidChannelLeaveNotification, {{"map_id", 10103}, {"char_id", "19"}}});
  round_trip({raid::kRaidChannelDestroyNotification, {{"map_id", 10103}}});
  EXPECT_EQ(
      round_trip({raid::kCommonErrorResponse, raid::raid_error("Raid_AlreadyApplied", 4)}).at("ec"),
      "Raid_AlreadyApplied");
}

TEST_F(EventProtocolTest, ChannelEventPacketsEncode) {
  using namespace channel_event;
  EXPECT_EQ(round_trip(status_packet(EventStatus::success)).at("status_type"),
            "EX5ChannelEventStatusType_Success");
  EXPECT_EQ(round_trip(tribute_count_packet({3, 10})).at("target_count"), 10);

  TributeAltar altar{10125, 21533402654060550ULL, 49054, 100194, {0, 0, 0}};
  TributePlan plan;
  plan.altar = &altar;
  plan.count = 2;
  WorldProgress after;
  after.stackables[100194] = 1;
  const auto update =
      round_trip(tribute_items_packet(plan, after, {{"packet_seq", {{"seq", "8"}}}}, 8));
  EXPECT_TRUE(update.contains("tribute"));
  EXPECT_EQ(round_trip(common_error("Tribute_LimitReached", 8)).at("ec"), "Tribute_LimitReached");
}

} // namespace
} // namespace chronicles
