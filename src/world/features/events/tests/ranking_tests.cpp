#include "world/features/events/ranking.hpp"
#include "world/features/events/ranking_repository.hpp"

#include "common/clock_ticks.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/postgres_repositories.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>

namespace chronicles::ranking {
namespace {

using Json = nlohmann::json;

constexpr int kWorldId = 1;
constexpr int kSoloRanking = 1000001;
constexpr int kPartyRanking = 6000201;
constexpr std::int64_t kTicksPerSecond = kEpochTicksPerSecond;
constexpr std::int64_t kTicksPerMillisecond = kEpochTicksPerSecond / 1000;

// 2026-10-24T20:19:55Z, inside the captured P2 session.
constexpr std::int64_t kCapturedNow = 17928731954228216;
// 2026-11-01T10:00:00Z, the captured next_reset_epochtime.
constexpr std::int64_t kCapturedNextReset = 17935272000000000;
// 2026-10-01T10:00:00Z.
constexpr std::int64_t kOctoberReset = 17908488000000000;

Json row(int id, const char *type) {
  return {{"id", std::to_string(id)}, {"rankingType", type},           {"resetType", "Monthly"},
          {"FK_mapId", "7001"},       {"FK_channelEventId", "700001"}, {"FK_iconId", "463"}};
}

RankingCatalog catalog(int list_size = 100) {
  RankingLimits limits;
  limits.solo_list_size = list_size;
  limits.party_list_size = list_size;
  return RankingCatalog({row(kSoloRanking, "Solo"), row(kPartyRanking, "Party")}, limits);
}

RankingMember member(std::int64_t id, const char *name) {
  return {id, name, 8, 2};
}

RankingRecord record(std::int64_t character_id, std::int64_t score, std::int64_t created) {
  RankingRecord entry;
  entry.world_id = kWorldId;
  entry.ranking_cms_id = kSoloRanking;
  entry.period_start_epoch_ticks = kOctoberReset;
  entry.members = {member(character_id, "Holder")};
  entry.holder_key = holder_key(RankingType::solo, entry.members);
  entry.score = score;
  entry.created_epoch_ticks = created;
  return entry;
}

TEST(RankingPeriods, MonthlyPeriodMatchesCapturedResetBoundary) {
  const auto period = ranking_period(ResetType::monthly, kCapturedNow);
  EXPECT_EQ(period.start_epoch_ticks, kOctoberReset);
  EXPECT_EQ(period.next_reset_epoch_ticks, kCapturedNextReset);

  // One second before the November boundary still belongs to October; the boundary itself
  // opens November.
  EXPECT_EQ(
      ranking_period(ResetType::monthly, kCapturedNextReset - kTicksPerSecond).start_epoch_ticks,
      kOctoberReset);
  EXPECT_EQ(ranking_period(ResetType::monthly, kCapturedNextReset).start_epoch_ticks,
            kCapturedNextReset);
  EXPECT_EQ(periods_between(ResetType::monthly, kOctoberReset, kCapturedNextReset), 1);
}

TEST(RankingPeriods, DailyAndWeeklyBoundariesUseTenUtc) {
  const auto day = ranking_period(ResetType::daily, kCapturedNow);
  // 2026-10-24T10:00:00Z to 2026-10-25T10:00:00Z.
  EXPECT_EQ(day.start_epoch_ticks, 17928360000000000);
  EXPECT_EQ(day.next_reset_epoch_ticks - day.start_epoch_ticks, 86400 * kTicksPerSecond);

  const auto week = ranking_period(ResetType::weekly, kCapturedNow);
  // Monday 2026-10-19T10:00:00Z.
  EXPECT_EQ(week.start_epoch_ticks, 17924040000000000);
  EXPECT_EQ(week.next_reset_epoch_ticks - week.start_epoch_ticks, 7 * 86400 * kTicksPerSecond);
  EXPECT_EQ(periods_between(ResetType::weekly, week.start_epoch_ticks, week.next_reset_epoch_ticks),
            1);
}

TEST(RankingCatalogTest, RejectsMalformedRows) {
  EXPECT_THROW(RankingCatalog({row(1, "Guild")}, {}), std::runtime_error);
  EXPECT_THROW(RankingCatalog({row(1, "Solo"), row(1, "Solo")}, {}), std::runtime_error);
  EXPECT_EQ(catalog().size(), 2U);
  EXPECT_EQ(catalog().find(kPartyRanking)->type, RankingType::party);
}

TEST(RankingList, EmptyBoardMatchesCapturedResponseShape) {
  MemoryRankingRepository rankings{std::make_shared<MemoryDatabase>()};
  const auto packet = list_response(
      catalog(), rankings, kWorldId, 42,
      {{"ranking_cms_id", kSoloRanking}, {"ranking_record_type", "ERankingRecordType_Server"}},
      kCapturedNow);

  ASSERT_EQ(packet.opcode, kRankingListResponse);
  EXPECT_FALSE(packet.message.contains("ec"));
  EXPECT_FALSE(packet.message.contains("my_ranking"));
  EXPECT_TRUE(packet.message.at("rankings").empty());
  EXPECT_EQ(packet.message.at("ranking_cms_id"), kSoloRanking);
  EXPECT_EQ(packet.message.at("reset_counter"), "1");
  EXPECT_EQ(packet.message.at("next_reset_epochtime"), std::to_string(kCapturedNextReset));
  EXPECT_EQ(packet.message.at("next_timestamp"),
            std::to_string(kCapturedNow + 10 * kTicksPerSecond));
}

TEST(RankingList, UnknownRankingIsRefused) {
  MemoryRankingRepository rankings{std::make_shared<MemoryDatabase>()};
  const auto packet =
      list_response(catalog(), rankings, kWorldId, 42, {{"ranking_cms_id", 5}}, kCapturedNow);
  EXPECT_EQ(packet.opcode, kRankingListResponse);
  EXPECT_EQ(packet.message.at("ec"), "Ranking_InvalidCmsId");
}

TEST(RankingList, OrdersByTimeThenEarlierRecordAndReportsOwnRank) {
  MemoryRankingRepository repository{std::make_shared<MemoryDatabase>()};
  ASSERT_TRUE(repository.submit_ranking_record(record(1, 90000, kCapturedNow)));
  ASSERT_TRUE(repository.submit_ranking_record(record(2, 60000, kCapturedNow + 5)));
  ASSERT_TRUE(repository.submit_ranking_record(record(3, 60000, kCapturedNow + 1)));

  const auto packet = list_response(catalog(2), repository, kWorldId, 1,
                                    {{"ranking_cms_id", kSoloRanking}}, kCapturedNow + 10);
  const auto &rankings = packet.message.at("rankings");
  ASSERT_EQ(rankings.size(), 2U);
  EXPECT_EQ(rankings[0].at("core_info").at("char_id"), "3");
  EXPECT_EQ(rankings[0].at("rank"), "1");
  EXPECT_EQ(rankings[1].at("core_info").at("char_id"), "2");
  EXPECT_EQ(rankings[1].at("score"), "60000");

  // The requester is outside the two-entry list but still learns its own rank.
  EXPECT_EQ(packet.message.at("my_ranking").at("rank"), "3");
  EXPECT_EQ(packet.message.at("my_ranking").at("core_info").at("world_server_id"), kWorldId);
}

TEST(RankingStore, KeepsOnlyTheBestEntryOfAHolder) {
  MemoryRankingRepository rankings{std::make_shared<MemoryDatabase>()};
  ASSERT_TRUE(rankings.submit_ranking_record(record(1, 50000, kCapturedNow)));
  EXPECT_FALSE(rankings.submit_ranking_record(record(1, 70000, kCapturedNow + 1)));
  EXPECT_FALSE(rankings.submit_ranking_record(record(1, 50000, kCapturedNow + 2)));
  EXPECT_TRUE(rankings.submit_ranking_record(record(1, 40000, kCapturedNow + 3)));

  const auto standing = rankings.ranking_standing(kWorldId, kSoloRanking, kOctoberReset, 10, 1);
  ASSERT_EQ(standing.top.size(), 1U);
  EXPECT_EQ(standing.top.front().score, 40000);
  EXPECT_EQ(standing.own_rank, 1);
}

TEST(RankingStore, RejectsInvalidRecords) {
  MemoryRankingRepository rankings{std::make_shared<MemoryDatabase>()};
  auto invalid = record(1, -1, kCapturedNow);
  EXPECT_THROW(rankings.submit_ranking_record(invalid), std::invalid_argument);

  invalid = record(1, 1, kCapturedNow);
  invalid.members.clear();
  EXPECT_THROW(rankings.submit_ranking_record(invalid), std::invalid_argument);
}

TEST(RankingList, ResetCounterAdvancesFromTheFirstStoredPeriod) {
  MemoryRankingRepository rankings{std::make_shared<MemoryDatabase>()};
  auto september = record(1, 1000, kOctoberReset - kTicksPerSecond);
  september.period_start_epoch_ticks =
      ranking_period(ResetType::monthly, kOctoberReset - 1).start_epoch_ticks;
  ASSERT_TRUE(rankings.submit_ranking_record(september));

  const auto packet = list_response(catalog(), rankings, kWorldId, 1,
                                    {{"ranking_cms_id", kSoloRanking}}, kCapturedNow);
  EXPECT_EQ(packet.message.at("reset_counter"), "2");
  // September's entry belongs to a closed period.
  EXPECT_TRUE(packet.message.at("rankings").empty());
}

TEST(RankingRunsTest, ServerTimedSoloRunRecordsKillTime) {
  MemoryRankingRepository rankings{std::make_shared<MemoryDatabase>()};
  RankingRuns runs;
  const auto ranking_catalog = catalog();
  const std::int64_t started = 1'000'000'000;

  const auto start = runs.start(ranking_catalog, kSoloRanking, started, kCapturedNow);
  ASSERT_EQ(start.opcode, kRankingRecordStartResponse);
  EXPECT_FALSE(start.message.contains("ec"));
  EXPECT_EQ(start.message.at("start_tick"), wire_monotonic_tick(started));

  const auto finished = runs.finish(
      ranking_catalog, rankings, kWorldId, kSoloRanking,
      {member(7, "Runner"), member(8, "Ignored")},
      started + 95'250 * kTicksPerMillisecond, kCapturedNow + 95 * kTicksPerSecond);
  ASSERT_EQ(finished.packet.opcode, kRankingRecordResponse);
  EXPECT_TRUE(finished.recorded);
  EXPECT_EQ(finished.packet.message.at("time_record_milli"), "95250");
  const auto &info = finished.packet.message.at("party_ranking");
  EXPECT_EQ(info.at("rank"), "1");
  EXPECT_FALSE(info.at("core_info").contains("party_members"));
  EXPECT_FALSE(runs.active().has_value());
}

TEST(RankingRunsTest, PartyRunListsEveryMemberUnderOneHolder) {
  MemoryRankingRepository rankings{std::make_shared<MemoryDatabase>()};
  RankingRuns runs;
  const auto ranking_catalog = catalog();

  runs.start(ranking_catalog, kPartyRanking, 0, kCapturedNow);
  const auto finished =
      runs.finish(ranking_catalog, rankings, kWorldId, kPartyRanking,
                  {member(9, "Leader"), member(4, "Second")},
                  60'000 * kTicksPerMillisecond, kCapturedNow + 60 * kTicksPerSecond);
  ASSERT_TRUE(finished.recorded);
  const auto &core = finished.packet.message.at("party_ranking").at("core_info");
  EXPECT_EQ(core.at("char_id"), "9");
  ASSERT_EQ(core.at("party_members").size(), 1U);

  const auto standing = rankings.ranking_standing(kWorldId, kPartyRanking, kOctoberReset, 10, 4);
  ASSERT_TRUE(standing.own.has_value());
  EXPECT_EQ(standing.own->holder_key, "p4.9");
}

TEST(RankingRunsTest, RefusesFinishWithoutStartAndAcrossAReset) {
  MemoryRankingRepository rankings{std::make_shared<MemoryDatabase>()};
  RankingRuns runs;
  const auto ranking_catalog = catalog();

  const auto without_start = runs.finish(ranking_catalog, rankings, kWorldId, kSoloRanking,
                                         {member(1, "A")}, 10, kCapturedNow);
  EXPECT_EQ(without_start.packet.message.at("ec"), "Ranking_FailedToUpdateRank");

  runs.start(ranking_catalog, kSoloRanking, 0, kCapturedNextReset - kTicksPerSecond);
  const auto across_reset =
      runs.finish(ranking_catalog, rankings, kWorldId, kSoloRanking, {member(1, "A")},
                  2 * kTicksPerSecond * 1000, kCapturedNextReset + kTicksPerSecond);
  EXPECT_EQ(across_reset.packet.message.at("ec"), "Ranking_ResetCounterMismatch");
  EXPECT_FALSE(across_reset.recorded);

  const auto unknown = runs.start(ranking_catalog, 77, 0, kCapturedNow);
  EXPECT_EQ(unknown.message.at("ec"), "Ranking_InvalidCmsId");
}

TEST(RankingRunsTest, ConsumesAttemptWhenStoreValidationRejectsTheRecord) {
  const auto definitions = catalog();
  MemoryRankingRepository rankings{std::make_shared<MemoryDatabase>()};
  RankingRuns runs;
  runs.start(definitions, kSoloRanking, 0, kCapturedNow);
  const auto rejected = runs.finish(definitions, rankings, kWorldId, kSoloRanking,
                                    {{1, "", 8, 1}}, kTicksPerMillisecond, kCapturedNow);
  EXPECT_EQ(rejected.packet.message.at("ec"), "Ranking_FailedToUpdateRank");
  EXPECT_FALSE(runs.active().has_value());
}

// The PostgreSQL repository keeps the same best-entry, ordering and own-rank contract, and
// concurrent finishes of one holder keep only the best time. Gated like the other PostgreSQL
// tests.
TEST(RankingStorage, PostgresKeepsBestEntriesAndOrdersTheBoard) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires isolated PostgreSQL test cluster";
  }

  std::ifstream migration(std::filesystem::path(CHRONICLES_PROJECT_ROOT) /
                          "migrations/001_accounts_characters.sql");
  ASSERT_TRUE(migration);
  const std::string sql(std::istreambuf_iterator<char>{migration}, {});
  migrate_postgres(connection, sql);
  migrate_postgres(connection, sql);
  PostgresRankingRepository rankings(connection);

  // A world ID unique to this run keeps repeated runs on one cluster independent.
  const auto world_id = static_cast<int>(
      std::chrono::steady_clock::now().time_since_epoch().count() % 1000000 + 1000);
  const auto entry = [&](std::int64_t character_id, std::int64_t score, std::int64_t created) {
    auto result = record(character_id, score, created);
    result.world_id = world_id;
    return result;
  };

  auto slower = std::async(std::launch::async, [&] {
    PostgresRankingRepository writer(connection);
    return writer.submit_ranking_record(entry(1, 70000, kCapturedNow));
  });
  auto faster = std::async(std::launch::async, [&] {
    PostgresRankingRepository writer(connection);
    return writer.submit_ranking_record(entry(1, 50000, kCapturedNow + 1));
  });
  slower.get();
  faster.get();
  ASSERT_TRUE(rankings.submit_ranking_record(entry(2, 60000, kCapturedNow)));
  ASSERT_TRUE(rankings.submit_ranking_record(entry(3, 60000, kCapturedNow - 1)));
  EXPECT_FALSE(rankings.submit_ranking_record(entry(2, 60000, kCapturedNow + 9)));

  PostgresRankingRepository reopened(connection);
  const auto standing = reopened.ranking_standing(world_id, kSoloRanking, kOctoberReset, 2, 2);
  ASSERT_EQ(standing.top.size(), 2U);
  EXPECT_EQ(standing.top[0].members.front().character_id, 1);
  EXPECT_EQ(standing.top[0].score, 50000);
  EXPECT_EQ(standing.top[1].members.front().character_id, 3);
  ASSERT_TRUE(standing.own.has_value());
  EXPECT_EQ(standing.own_rank, 3);
  EXPECT_EQ(standing.own->members.front().name, "Holder");
  EXPECT_EQ(reopened.first_ranking_period(world_id, kSoloRanking), kOctoberReset);
  EXPECT_FALSE(reopened.first_ranking_period(world_id, kPartyRanking).has_value());

  // Top entries and the owner's rank must describe the same snapshot while new holders
  // finish. A read-committed transaction can count a holder absent from its earlier top list.
  std::promise<void> begin_writes;
  auto writes_started = begin_writes.get_future();
  auto additions = std::async(std::launch::async, [&] {
    PostgresRankingRepository writer(connection);
    begin_writes.set_value();
    for (std::int64_t character_id = 100; character_id < 164; ++character_id) {
      writer.submit_ranking_record(entry(character_id, 1000, kCapturedNow));
    }
  });
  writes_started.wait();
  for (int query = 0; query < 100; ++query) {
    const auto snapshot = reopened.ranking_standing(world_id, kSoloRanking, kOctoberReset, 1000, 2);
    ASSERT_TRUE(snapshot.own.has_value());
    const auto own = std::ranges::find_if(snapshot.top, [&](const RankingRecord &candidate) {
      return candidate.holder_key == snapshot.own->holder_key;
    });
    ASSERT_NE(own, snapshot.top.end());
    EXPECT_EQ(snapshot.own_rank, std::distance(snapshot.top.begin(), own) + 1);
  }
  additions.get();
}

} // namespace
} // namespace chronicles::ranking
