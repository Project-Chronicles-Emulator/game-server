#include "world/features/events/events_session.hpp"

#include "common/clock_ticks.hpp"
#include "data/client_catalog.hpp"
#include "world/features/events/preorder_bonus.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chronicles::events {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr int kParentMap = 9012;
constexpr int kRaidMap = 10103;
constexpr int kRankingId = 1000001;
constexpr int kOfferingItem = 100194;
constexpr std::uint64_t kAltarUnit = 21533402654060550ULL;

WorldPosition parent_position() {
  return {kParentMap, -2450974, 3854433, -604092, 37};
}

Json tribute_request(int count) {
  return {{"item_cms_id", kOfferingItem},
          {"count", count},
          {"target_unit_id", std::to_string(kAltarUnit)},
          {"packet_seq", {{"seq", "31"}}}};
}

Json raid_move_request() {
  const auto position = parent_position();
  return {{"map_id", kRaidMap},
          {"pos",
           {{"millimeter_x", position.x_mm},
            {"millimeter_y", position.y_mm},
            {"millimeter_z", position.z_mm}}}};
}

EventsDefinitions definitions(std::chrono::milliseconds countdown = 0ms) {
  EventsDefinitions result;
  result.rankings = std::make_shared<const ranking::RankingCatalog>(
      std::vector<Json>{{{"id", std::to_string(kRankingId)},
                         {"rankingType", "Solo"},
                         {"resetType", "Monthly"},
                         {"FK_mapId", std::to_string(kRaidMap)},
                         {"FK_channelEventId", "101015"}}},
      ranking::RankingLimits{});
  auto raids = std::make_shared<const raid::RaidCatalog>(
      std::vector<Json>{{{"id", "1"}, {"FK_raidMapId", std::to_string(kRaidMap)}}},
      std::vector<Json>{{{"id", std::to_string(kRaidMap)},
                         {"type", "Raid"},
                         {"parentMapId", std::to_string(kParentMap)},
                         {"chMaxCount", "3"},
                         {"timeLimit", "540"}},
                        {{"id", std::to_string(kParentMap)}, {"type", "Field"}}},
      countdown);
  result.raids = std::make_shared<raid::RaidMatchmaker>(raids, raid::RaidMatchmakerConfig{});
  result.channel_events = std::make_shared<const channel_event::ChannelEventData>(
      Json{{"format_version", 1},
           {"channel_events",
            Json::array(
                {{{"map_id", kParentMap}, {"channel_event_id", 101015}, {"reward_id", nullptr}}})},
           {"tribute_altars", Json::array({{{"map_id", kParentMap},
                                            {"unit_id", std::to_string(kAltarUnit)},
                                            {"doodad_cms_id", 49054},
                                            {"item_cms_id", kOfferingItem},
                                            {"position", {-2450974, 3854433, -604092}}}})}});
  return result;
}

class CountingRankingRepository final : public RankingRepository {
public:
  bool submit_ranking_record(const RankingRecord &record) override {
    records.push_back(record);
    return true;
  }

  RankingStanding ranking_standing(int, int, std::int64_t, std::size_t, std::int64_t) override {
    ++standing_queries;
    if (records.empty()) {
      return {};
    }
    return {records, records.back(), 1};
  }

  std::optional<std::int64_t> first_ranking_period(int, int) override {
    return std::nullopt;
  }

  unsigned standing_queries{};
  std::vector<RankingRecord> records;
};

class ProbeContext final : public EventsSessionContext {
public:
  std::uint64_t events_connection_id() const override {
    return connection;
  }

  bool events_closed() const override {
    return closed;
  }

  std::optional<EventsCharacter> events_character() const override {
    return character;
  }

  const party::MemberPresence *events_party_identity() const override {
    return display ? &*display : nullptr;
  }

  std::int64_t events_monotonic_ticks() const override {
    return monotonic;
  }

  std::int64_t events_epoch_ticks() const override {
    return epoch;
  }

  world_metrics::TimedMutex &events_protocol_mutex() const override {
    return protocol_mutex;
  }

  std::optional<WorldPosition> events_position_locked() const override {
    return position;
  }

  std::optional<WorldPosition> events_pending_map_destination() const override {
    return pending;
  }

  bool events_has_tutorial() const override {
    return tutorial;
  }

  bool events_in_housing() const override {
    return housing;
  }

  bool events_other_action_busy() const override {
    return busy;
  }

  bool events_has_gm_units() const override {
    return has_gm_units;
  }

  bool events_player_dead() const override {
    return dead;
  }

  bool events_begin_map_move(const WorldPosition &destination, bool preserve_combat) override {
    events.push_back("begin_map_move");
    EXPECT_TRUE(preserve_combat);
    if (begin_move) {
      return begin_move(destination);
    }
    return allow_move;
  }

  void events_stage_map_destination(const WorldPosition &destination) override {
    events.push_back("stage_destination");
    pending = destination;
    has_gm_units = false;
  }

  const MapChannels *events_map_channels() const override {
    return nullptr;
  }

  bool events_zone_channel_move(const WorldGameplayRequest &,
                                const std::optional<WorldPosition> &) override {
    events.push_back("zone_channel_move");
    return true;
  }

  WorldProgress events_world_progress() const override {
    events.push_back("load_progress");
    return progress;
  }

  bool events_save_world_progress(const WorldProgress &before,
                                  const WorldProgress &after) override {
    events.push_back("commit_progress");
    EXPECT_EQ(before, progress);
    if (throw_commit) {
      throw std::runtime_error("synthetic ambiguous inventory commit");
    }
    if (commit_succeeds) {
      durable_progress = after;
      if (!tutorial) {
        progress = after;
      }
    }
    return commit_succeeds;
  }

  void events_adopt_committed_progress(const WorldProgress &after) override {
    events.push_back("adopt_progress");
    progress = after;
  }

  void events_deliver_observers(const std::vector<GameplayPacket> &packets) override {
    events.push_back("deliver_observers");
    observers = packets;
    if (on_observers) {
      on_observers();
    }
  }

  void events_gm_reply(const std::string &message) override {
    events.push_back("gm_reply");
    replies.push_back(message);
  }

  std::uint64_t connection{10};
  bool closed{};
  std::optional<EventsCharacter> character{EventsCharacter{1, 7}};
  std::optional<party::MemberPresence> display{party::MemberPresence{7, 10, "Eventkeeper", 8, 2}};
  std::int64_t monotonic{10 * kEpochTicksPerSecond};
  std::int64_t epoch{17928731954228216};
  mutable world_metrics::TimedMutex protocol_mutex;
  std::optional<WorldPosition> position{parent_position()};
  std::optional<WorldPosition> pending;
  bool tutorial{};
  bool housing{};
  bool busy{};
  bool has_gm_units{};
  bool dead{};
  bool allow_move{true};
  bool commit_succeeds{true};
  bool throw_commit{};
  WorldProgress progress;
  WorldProgress durable_progress;
  std::function<bool(const WorldPosition &)> begin_move;
  std::function<void()> on_observers;
  mutable std::vector<std::string> events;
  std::vector<GameplayPacket> observers;
  std::vector<std::string> replies;
};

struct Harness {
  explicit Harness(std::chrono::milliseconds countdown = 0ms)
      : input(definitions(countdown)),
        feature(std::make_shared<EventsFeature>(
            boost::asio::make_strand(io), input,
            std::make_shared<party::PartyWorld>(party::PartyWorldConfig{}))),
        session(feature->create_session(context)) {
    feature->set_ranking_repository(rankings);
    feature->set_raid_delivery(
        [this](const raid::RaidUpdate &update) { deliveries.push_back(update); });
  }

  FrameContext frame(std::uint16_t opcode, Json message = Json::object()) const {
    FrameContext result(opcode, WorldGameplayRequest{opcode, 31, std::move(message)});
    result.gameplay_position = context.position;
    result.party_position = context.position;
    return result;
  }

  void match() {
    const auto matched = session->execute_gm_command(
        {true,
         event_gm::EventCommand{event_gm::EventCommandKind::raid_channel_enter, kRaidMap},
         {}});
    ASSERT_FALSE(matched.empty());
    ASSERT_EQ(matched.front().opcode, raid::kRaidChannelMatchedNotification);
    context.events.clear();
  }

  boost::asio::io_context io;
  EventsDefinitions input;
  ProbeContext context;
  std::shared_ptr<CountingRankingRepository> rankings{
      std::make_shared<CountingRankingRepository>()};
  std::shared_ptr<EventsFeature> feature;
  std::unique_ptr<EventsSession> session;
  std::vector<raid::RaidUpdate> deliveries;
};

TEST(EventsSession, UnhandledOpcodePreservesPositionsAndDoesNoFeatureWork) {
  Harness harness;
  auto frame = harness.frame(20002);
  raid::RaidUpdate update;

  EXPECT_FALSE(harness.session->handle(frame, update));
  EXPECT_EQ(frame.gameplay_position, harness.context.position);
  EXPECT_EQ(frame.party_position, harness.context.position);
  EXPECT_TRUE(frame.session_packets.empty());
  EXPECT_TRUE(update.packets.empty());
  EXPECT_TRUE(harness.context.events.empty());
}

TEST(EventsSession, PreorderQueriesAndRepeatedClaimsPreserveInventory) {
  Harness harness;
  harness.context.progress.gold = 123;
  const auto before = harness.context.progress;
  raid::RaidUpdate update;

  for (const auto opcode : {preorder_bonus::kStateRequest, preorder_bonus::kClaimRequest,
                            preorder_bonus::kClaimRequest}) {
    auto frame = harness.frame(opcode, {{"packet_seq", {{"seq", "7654321"}}}});
    ASSERT_TRUE(harness.session->handle(frame, update));
    ASSERT_EQ(frame.session_packets.size(), 1);
    const auto &reply = frame.session_packets.front();
    const bool claim = opcode == preorder_bonus::kClaimRequest;
    EXPECT_EQ(reply.opcode,
              claim ? preorder_bonus::kClaimResponse : preorder_bonus::kStateResponse);
    EXPECT_EQ(reply.message.at("ec"), claim ? "PreorderBonus_NotEligible" : "Success");
    EXPECT_EQ(reply.message.at("state"), "PreorderBonusState_NotEligible");
    EXPECT_EQ(reply.message.at("packet_seq").at("seq"), "7654321");
  }

  EXPECT_EQ(harness.context.progress, before);
  EXPECT_TRUE(harness.context.events.empty());
  EXPECT_TRUE(harness.context.observers.empty());
}

TEST(EventsSession, PreorderWithoutCharacterRefusesBothRequests) {
  Harness harness;
  harness.context.character.reset();
  raid::RaidUpdate update;

  for (const auto opcode : {preorder_bonus::kStateRequest, preorder_bonus::kClaimRequest}) {
    auto frame = harness.frame(opcode);
    ASSERT_TRUE(harness.session->handle(frame, update));
    ASSERT_EQ(frame.session_packets.size(), 1);
    EXPECT_EQ(frame.session_packets.front().message.at("ec"), "InvalidState");
  }
  EXPECT_TRUE(harness.context.events.empty());
}

TEST(EventsSession, RankingBudgetIsPerSessionAndResetsBeforeStorageAccess) {
  Harness harness;
  auto other_session = harness.feature->create_session(harness.context);
  const Json query{{"ranking_cms_id", kRankingId},
                   {"ranking_record_type", "ERankingRecordType_Server"}};
  raid::RaidUpdate update;
  for (unsigned index = 0; index < 32; ++index) {
    auto frame = harness.frame(ranking::kRankingListRequest, query);
    ASSERT_TRUE(harness.session->handle(frame, update));
    ASSERT_EQ(frame.session_packets.front().opcode, ranking::kRankingListResponse);
  }
  EXPECT_EQ(harness.rankings->standing_queries, 32);

  auto refused = harness.frame(ranking::kRankingListRequest, query);
  ASSERT_TRUE(harness.session->handle(refused, update));
  ASSERT_EQ(refused.session_packets.size(), 1);
  EXPECT_EQ(refused.session_packets.front().message.at("ec"), "BadRequest");
  EXPECT_EQ(harness.rankings->standing_queries, 32);

  auto independent = harness.frame(ranking::kRankingListRequest, query);
  EXPECT_TRUE(other_session->handle(independent, update));
  EXPECT_EQ(harness.rankings->standing_queries, 33);
  harness.context.monotonic += kEpochTicksPerSecond;
  auto recovered = harness.frame(ranking::kRankingListRequest, query);
  EXPECT_TRUE(harness.session->handle(recovered, update));
  EXPECT_EQ(harness.rankings->standing_queries, 34);
}

TEST(EventsSession, MissingIdentityRefusesRankingWithoutOpeningTheRepository) {
  Harness harness;
  harness.context.character.reset();
  auto frame = harness.frame(ranking::kRankingListRequest, {{"ranking_cms_id", kRankingId}});
  raid::RaidUpdate update;

  ASSERT_TRUE(harness.session->handle(frame, update));
  ASSERT_EQ(frame.session_packets.size(), 1);
  EXPECT_EQ(frame.session_packets.front().message.at("ec"), "Ranking_InvalidCmsId");
  EXPECT_EQ(harness.rankings->standing_queries, 0);
}

TEST(EventsSession, RankingRunUsesTheAuthenticatedFinisherAndServerClocks) {
  Harness harness;
  const auto started = harness.session->execute_gm_command(
      {true, event_gm::EventCommand{event_gm::EventCommandKind::ranking_start, kRankingId}, {}});
  ASSERT_EQ(started.size(), 1);
  EXPECT_EQ(started.front().opcode, ranking::kRankingRecordStartResponse);
  harness.context.monotonic += 25 * (kEpochTicksPerSecond / 10);
  const auto finished = harness.session->execute_gm_command(
      {true, event_gm::EventCommand{event_gm::EventCommandKind::ranking_finish, kRankingId}, {}});

  ASSERT_EQ(finished.size(), 1);
  EXPECT_EQ(finished.front().opcode, ranking::kRankingRecordResponse);
  EXPECT_FALSE(finished.front().message.contains("ec"));
  ASSERT_EQ(harness.rankings->records.size(), 1);
  const auto &record = harness.rankings->records.front();
  EXPECT_EQ(record.world_id, 1);
  EXPECT_EQ(record.score, 2500);
  EXPECT_EQ(record.created_epoch_ticks, harness.context.epoch);
  ASSERT_EQ(record.members.size(), 1);
  EXPECT_EQ(record.members.front().character_id, 7);
  EXPECT_EQ(record.members.front().name, "Eventkeeper");
}

TEST(EventsSession, TributeCommitAndTutorialAdoptionPrecedeClientAndObserverResults) {
  Harness harness;
  harness.context.tutorial = true;
  harness.context.progress.revision = 4;
  harness.context.progress.stackables[kOfferingItem] = 10;
  auto frame = harness.frame(channel_event::kItemTributeRequest, tribute_request(10));
  harness.context.on_observers = [&] {
    ASSERT_EQ(frame.session_packets.size(), 3);
    EXPECT_EQ(harness.context.progress.stackables.at(kOfferingItem), 0);
  };
  raid::RaidUpdate update;

  ASSERT_TRUE(harness.session->handle(frame, update));
  ASSERT_EQ(frame.session_packets.size(), 3);
  EXPECT_EQ(frame.session_packets[0].opcode, channel_event::kItemsUpdateNotification);
  EXPECT_EQ(frame.session_packets[1].opcode, channel_event::kTributeCountNotification);
  EXPECT_EQ(frame.session_packets[2].opcode, channel_event::kChangeChannelEventStatusNotification);
  EXPECT_EQ(harness.context.progress.revision, 5);
  EXPECT_EQ(harness.context.durable_progress, harness.context.progress);
  EXPECT_EQ(harness.context.events,
            (std::vector<std::string>{"load_progress", "commit_progress", "adopt_progress",
                                      "deliver_observers"}));
  ASSERT_EQ(harness.context.observers.size(), 2);
  EXPECT_EQ(harness.context.observers[0].opcode, channel_event::kTributeCountNotification);
  EXPECT_EQ(harness.context.observers[1].opcode,
            channel_event::kChangeChannelEventStatusNotification);
}

TEST(EventsSession, AmbiguousTributeCommitDoesNotPublishOrAdoptAResult) {
  Harness harness;
  harness.context.tutorial = true;
  harness.context.progress.revision = 4;
  harness.context.progress.stackables[kOfferingItem] = 10;
  harness.context.throw_commit = true;
  auto frame = harness.frame(channel_event::kItemTributeRequest, tribute_request(10));
  raid::RaidUpdate update;

  ASSERT_TRUE(harness.session->handle(frame, update));
  ASSERT_EQ(frame.session_packets.size(), 1);
  EXPECT_EQ(frame.session_packets.front().message.at("ec"), "BadRequest");
  EXPECT_EQ(harness.context.progress.revision, 4);
  EXPECT_EQ(harness.context.events, (std::vector<std::string>{"load_progress", "commit_progress"}));
  EXPECT_TRUE(harness.context.observers.empty());
  auto count = harness.frame(channel_event::kTributeCountRequest);
  ASSERT_TRUE(harness.session->handle(count, update));
  EXPECT_EQ(count.session_packets.front().message.at("count"), 0);
}

TEST(EventsSession, RaidEntryInstallsTheReturnPointBeforeMapMoveAndInvalidatesPositions) {
  Harness harness;
  harness.match();
  auto destination = parent_position();
  destination.map_id = kRaidMap;
  harness.context.begin_move = [&](const WorldPosition &target) {
    EXPECT_EQ(target, destination);
    EXPECT_TRUE(harness.session->authorize_server_map_move(target));
    EXPECT_EQ(harness.session->persisted_position(target), parent_position());
    return true;
  };
  auto frame = harness.frame(raid::kRaidChannelMoveRequest, raid_move_request());
  raid::RaidUpdate update;

  ASSERT_TRUE(harness.session->handle(frame, update));
  EXPECT_FALSE(frame.gameplay_position);
  EXPECT_FALSE(frame.party_position);
  EXPECT_EQ(harness.context.pending, destination);
  EXPECT_EQ(harness.context.events,
            (std::vector<std::string>{"begin_map_move", "stage_destination"}));
  EXPECT_EQ(harness.session->housing_return_origin(std::nullopt), parent_position());
}

TEST(EventsSession, RefusedRaidMapMoveRollsBackItsReturnPointWithoutInvalidatingPositions) {
  Harness harness;
  harness.match();
  harness.context.allow_move = false;
  auto frame = harness.frame(raid::kRaidChannelMoveRequest, raid_move_request());
  raid::RaidUpdate update;

  ASSERT_TRUE(harness.session->handle(frame, update));
  EXPECT_EQ(frame.gameplay_position, harness.context.position);
  EXPECT_EQ(frame.party_position, harness.context.position);
  EXPECT_FALSE(harness.session->housing_return_origin(std::nullopt));
  EXPECT_FALSE(harness.context.pending);
  EXPECT_EQ(frame.session_packets.front().message.at("ec"), "Move_ChannelMoveFailed");
}

TEST(EventsSession, EvictionKeepsItsCopiedOriginWhenTheMapAdapterRunsDepartureInsideTheMove) {
  Harness harness;
  harness.match();
  auto frame = harness.frame(raid::kRaidChannelMoveRequest, raid_move_request());
  raid::RaidUpdate update;
  ASSERT_TRUE(harness.session->handle(frame, update));
  auto leave = harness.frame(raid::kRaidChannelLeaveRequest, {{"map_id", kRaidMap}});
  ASSERT_TRUE(harness.session->handle(leave, update));
  harness.context.position->map_id = kRaidMap;
  harness.context.pending.reset();
  harness.context.begin_move = [&](const WorldPosition &destination) {
    EXPECT_EQ(destination, parent_position());
    // The real begin_server_map_move performs this callback before it returns success.
    harness.session->on_map_departure(destination);
    EXPECT_FALSE(harness.session->housing_return_origin(std::nullopt));
    return true;
  };

  harness.session->on_raid_eviction(kRaidMap);
  EXPECT_EQ(harness.context.pending, parent_position());
  EXPECT_FALSE(harness.session->housing_return_origin(std::nullopt));
  EXPECT_EQ(harness.input.raids->membership(7).state, raid::RaidState::none);
}

TEST(EventsSession, CountdownRefusalDoesNotStartTravelOrInstallAReturnPoint) {
  Harness harness(10000ms);
  harness.match();
  auto frame = harness.frame(raid::kRaidChannelMoveRequest, raid_move_request());
  raid::RaidUpdate update;

  ASSERT_TRUE(harness.session->handle(frame, update));
  EXPECT_TRUE(harness.context.events.empty());
  EXPECT_FALSE(harness.session->housing_return_origin(std::nullopt));
  EXPECT_EQ(frame.session_packets.front().message.at("ec"), "Raid_TimeUnavailable");
}

TEST(EventsSession, EvictionDuringLoadRetriesOnArrivalAndOnlyThenReleasesTheMap) {
  Harness harness;
  harness.match();
  auto frame = harness.frame(raid::kRaidChannelMoveRequest, raid_move_request());
  raid::RaidUpdate update;
  ASSERT_TRUE(harness.session->handle(frame, update));
  auto leave = harness.frame(raid::kRaidChannelLeaveRequest, {{"map_id", kRaidMap}});
  ASSERT_TRUE(harness.session->handle(leave, update));
  ASSERT_FALSE(update.evictions.empty());
  harness.context.allow_move = false;
  harness.session->on_raid_eviction(kRaidMap);
  EXPECT_EQ(harness.session->housing_return_origin(std::nullopt), parent_position());

  const raid::RaidApplicant other{8, 20, "Nextkeeper", 8, 2};
  const auto reserved = harness.input.raids->force_match(other, kRaidMap, raid::Clock::now());
  ASSERT_FALSE(reserved.packets.empty());
  EXPECT_EQ(reserved.packets.front().opcode, raid::kCommonErrorResponse);

  harness.context.position->map_id = kRaidMap;
  harness.context.pending.reset();
  harness.context.allow_move = true;
  harness.session->on_map_load_complete();
  EXPECT_EQ(harness.context.pending, parent_position());
  EXPECT_FALSE(harness.session->housing_return_origin(std::nullopt));
  const auto reusable = harness.input.raids->force_match(other, kRaidMap, raid::Clock::now());
  ASSERT_FALSE(reusable.packets.empty());
  EXPECT_EQ(reusable.packets.front().opcode, raid::kRaidChannelMatchedNotification);
}

TEST(EventsSession, GenericTravelDeliversDetachBeforeClearingTheSoleReturnPoint) {
  Harness harness;
  harness.match();
  auto frame = harness.frame(raid::kRaidChannelMoveRequest, raid_move_request());
  raid::RaidUpdate update;
  ASSERT_TRUE(harness.session->handle(frame, update));
  bool delivered{};
  harness.feature->set_raid_delivery([&](const raid::RaidUpdate &leaving) {
    delivered = true;
    EXPECT_EQ(harness.session->housing_return_origin(std::nullopt), parent_position());
    harness.deliveries.push_back(leaving);
  });

  harness.session->on_map_departure(parent_position());
  EXPECT_TRUE(delivered);
  EXPECT_FALSE(harness.session->housing_return_origin(std::nullopt));
  EXPECT_EQ(harness.input.raids->membership(7).state, raid::RaidState::none);
  ASSERT_FALSE(harness.deliveries.empty());
  EXPECT_TRUE(harness.deliveries.back().evictions.empty());
  const auto reusable =
      harness.input.raids->force_match({8, 20, "Nextkeeper", 8, 2}, kRaidMap, raid::Clock::now());
  EXPECT_EQ(reusable.packets.front().opcode, raid::kRaidChannelMatchedNotification);
}

TEST(EventsSession, LeavingTheWorldDetachesThroughTheExistingDeliveryBoundary) {
  Harness harness;
  harness.match();

  harness.session->on_leave_world();
  EXPECT_EQ(harness.input.raids->membership(7).state, raid::RaidState::none);
  ASSERT_EQ(harness.deliveries.size(), 1);
  EXPECT_TRUE(harness.deliveries.front().evictions.empty());
}

TEST(EventsFeature, RaidTimerPostsDoNotKeepTheFeatureAlive) {
  boost::asio::io_context io;
  auto feature = std::make_shared<EventsFeature>(
      boost::asio::make_strand(io), definitions(),
      std::make_shared<party::PartyWorld>(party::PartyWorldConfig{}));
  std::weak_ptr<EventsFeature> weak = feature;
  feature->schedule_raid_tick();
  feature.reset();

  EXPECT_TRUE(weak.expired());
  io.run();
  EXPECT_TRUE(weak.expired());
}

TEST(EventsFeature, RepeatedSchedulingUsesOneRaidTimerOnTheExistingStrand) {
  boost::asio::io_context io;
  const auto strand = boost::asio::make_strand(io);
  auto input = definitions();
  auto catalog = std::make_shared<const raid::RaidCatalog>(input.raids->catalog());
  input.raids = std::make_shared<raid::RaidMatchmaker>(catalog, raid::RaidMatchmakerConfig{1s, 1});
  auto feature = std::make_shared<EventsFeature>(
      strand, input, std::make_shared<party::PartyWorld>(party::PartyWorldConfig{}));
  unsigned deliveries{};
  feature->set_raid_delivery([&](const raid::RaidUpdate &update) {
    EXPECT_TRUE(strand.running_in_this_thread());
    ++deliveries;
    ASSERT_FALSE(update.packets.empty());
    EXPECT_EQ(update.packets.front().opcode, raid::kRaidChannelMatchedNotification);
  });
  input.raids->receive({7, 10, "Eventkeeper", 8, 2}, raid::kRaidChannelRegisterRequest,
                       {{"map_id", kRaidMap}}, 4, true, raid::Clock::now());
  ASSERT_EQ(input.raids->membership(7).state, raid::RaidState::waiting);
  feature->schedule_raid_tick();
  feature->schedule_raid_tick();
  feature->schedule_raid_tick();

  io.run_for(1500ms);
  EXPECT_EQ(deliveries, 1);
  feature.reset();
  io.restart();
  io.poll();
}

class TemporaryProfiles {
public:
  TemporaryProfiles() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::path(EVENTS_TEST_FIXTURE_ROOT) / std::to_string(stamp);
    std::filesystem::create_directories(root);
  }

  ~TemporaryProfiles() {
    std::error_code ignored;
    std::filesystem::remove(root / "channel-events" / "definitions.json", ignored);
    std::filesystem::remove(root / "channel-events", ignored);
    std::filesystem::remove(root / "catalog" / "manifest.json", ignored);
    std::filesystem::remove(root / "catalog", ignored);
    std::filesystem::remove(root, ignored);
  }

  std::filesystem::path root;
};

TEST(EventsFeature, MissingOptionalCatalogTablesAndProfileDisableOnlyTheirRequests) {
  TemporaryProfiles profiles;
  std::filesystem::create_directory(profiles.root / "catalog");
  {
    std::ofstream manifest(profiles.root / "catalog" / "manifest.json");
    manifest << Json{{"format_version", 1}, {"table_count", 0}, {"files", Json::array()}};
  }
  ClientCatalog catalog(profiles.root / "catalog");
  boost::asio::io_context io;
  auto feature = std::make_shared<EventsFeature>(
      boost::asio::make_strand(io), &catalog,
      std::make_shared<party::PartyWorld>(party::PartyWorldConfig{}));
  EXPECT_NO_THROW(feature->load_channel_event_data(profiles.root / "catalog"));
  EXPECT_FALSE(feature->has_raid_service());
  ProbeContext context;
  auto session = feature->create_session(context);
  FrameContext frame(channel_event::kTributeCountRequest,
                     WorldGameplayRequest{channel_event::kTributeCountRequest, 4, Json::object()});
  frame.party_position = parent_position();
  raid::RaidUpdate update;

  ASSERT_TRUE(session->handle(frame, update));
  EXPECT_EQ(frame.session_packets.front().message.at("ec"), "Tribute_InvalidTarget");
}

TEST(EventsFeature, PresentMalformedOptionalProfileStillStopsStartup) {
  TemporaryProfiles profiles;
  std::filesystem::create_directory(profiles.root / "channel-events");
  {
    std::ofstream definitions_file(profiles.root / "channel-events" / "definitions.json");
    definitions_file << Json{{"format_version", 999}};
  }
  boost::asio::io_context io;
  auto feature = std::make_shared<EventsFeature>(
      boost::asio::make_strand(io), nullptr,
      std::make_shared<party::PartyWorld>(party::PartyWorldConfig{}));

  EXPECT_THROW(feature->load_channel_event_data(profiles.root / "catalog"), std::runtime_error);
}

} // namespace
} // namespace chronicles::events
