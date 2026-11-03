#pragma once

// Boss time-trial leaderboards (client Ranking table) for the world transport.
//
// Evidence (build 40367e24...):
// - Opcodes from protocol/opcodes.json: PC2S 20006 RankingListReq; PS2C 7 RankingRecordStartRes,
//   8 RankingRecordRes and 9 RankingListRes.
// - Official capture P2 holds 16 RankingListReq {ranking_cms_id, ranking_record_type=Server}, one
//   per Ranking row, each answered by RankingListRes with ec omitted (Success), no rankings,
//   reset_counter 1, next_reset_epochtime 2026-11-01T10:00:00Z for the Monthly rows, and
//   next_timestamp about 9.9 s after the server time recovered from that session's TimeSyncRes,
//   which matches GameVariable RankingUpdateCycleSec = 10.
// - GameVariable SoloRankingMaxCount = PartyRankingMaxCount = 100 bound each list.
// - L10N TXT_RANKING_<id>_NAME names one boss per row ("Hereafter Gartez"), and
//   RankingRecordRes carries time_record_milli, so a score is a kill time: lower ranks higher.
// - No RankingRecordStartRes/RankingRecordRes was captured. Their use by RankingRuns, the start
//   tick clock and the party_ranking contents are Candidate. The official server starts and ends
//   runs from server-only channel-event tasks (rankingStart/rankingEnd), which the client data
//   does not contain, so only operator commands start and finish runs here.
// - Daily and weekly reset anchors are Candidate (every catalog row resets monthly): daily at
//   10:00 UTC, weekly on Monday 10:00 UTC.

#include "protocol/gameplay_packet.hpp"
#include "world/features/events/ranking_repository.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles {
class ClientCatalog;
} // namespace chronicles

namespace chronicles::ranking {

inline constexpr std::uint16_t kRankingListRequest = 20006;
inline constexpr std::uint16_t kRankingRecordStartResponse = 7;
inline constexpr std::uint16_t kRankingRecordResponse = 8;
inline constexpr std::uint16_t kRankingListResponse = 9;

// x5.cms.EX5RankingType.
enum class RankingType { solo = 0, party = 1 };

// x5.cms.EX5RankingResetType.
enum class ResetType { daily = 0, weekly = 1, monthly = 2 };

struct RankingDefinition {
  int id{};
  RankingType type{RankingType::solo};
  ResetType reset{ResetType::monthly};
  // Ranking.FK_mapId and FK_channelEventId: the boss room and its channel event.
  int map_id{};
  int channel_event_id{};
};

// One leaderboard period, in epoch ticks (100 ns since the Unix epoch).
struct RankingPeriod {
  std::int64_t start_epoch_ticks{};
  std::int64_t next_reset_epoch_ticks{};
};

// Period containing `now_epoch_ticks`. Boundaries fall at 10:00 UTC (observed monthly reset).
RankingPeriod ranking_period(ResetType reset, std::int64_t now_epoch_ticks);

// Reset boundaries crossed from `earlier_start` to `later_start` (both period starts).
std::int64_t periods_between(ResetType reset, std::int64_t earlier_start, std::int64_t later_start);

struct RankingLimits {
  // GameVariable SoloRankingMaxCount and PartyRankingMaxCount.
  int solo_list_size{100};
  int party_list_size{100};
  // GameVariable RankingUpdateCycleSec: RankingListRes.next_timestamp is this far ahead.
  std::chrono::seconds update_cycle{10};
};

class RankingCatalog {
public:
  static std::vector<std::string> required_tables();

  static std::shared_ptr<const RankingCatalog> load(ClientCatalog &catalog);

  // Throws std::runtime_error naming the field for a malformed or duplicate row.
  RankingCatalog(const std::vector<nlohmann::json> &ranking_rows, RankingLimits limits);

  const RankingDefinition *find(int ranking_cms_id) const;

  int list_size(RankingType type) const;

  std::chrono::seconds update_cycle() const;

  std::size_t size() const;

private:
  std::unordered_map<int, RankingDefinition> definitions_;
  RankingLimits limits_;
};

// Holder identity of a record: "c<id>" for one character, "p<id>.<id>..." (ascending member
// IDs) for a party, so one party composition keeps one best entry.
std::string holder_key(RankingType type, const std::vector<RankingMember> &members);

// PS2C_RankingListRes for one PC2S_RankingListReq. Unknown ranking IDs answer
// Ranking_InvalidCmsId; a storage failure answers SystemError.
GameplayPacket list_response(const RankingCatalog &catalog, RankingRepository &rankings,
                             int world_id, std::int64_t character_id,
                             const nlohmann::json &request, std::int64_t now_epoch_ticks);

// x5.pb.RankingInfo for one stored record.
nlohmann::json ranking_info(const RankingRecord &record, int rank, int world_server_id);

// A boss attempt timed by the server. Never derived from a client report.
struct RankingRun {
  int ranking_cms_id{};
  std::int64_t started_monotonic_ticks{};
  std::int64_t period_start_epoch_ticks{};
};

struct RunFinish {
  GameplayPacket packet;
  // True when the run produced a stored leaderboard entry (new or improved).
  bool recorded{};
};

// The attempt of one world session. Not thread-safe: the session worker strand owns it.
class RankingRuns {
public:
  // Starts (or restarts) the attempt and answers RankingRecordStartRes.
  GameplayPacket start(const RankingCatalog &catalog, int ranking_cms_id,
                       std::int64_t now_monotonic_ticks, std::int64_t now_epoch_ticks);

  // Ends the attempt, stores its kill time and answers RankingRecordRes with the holder's best
  // entry. `members` lists the finisher first; a solo ranking keeps only the finisher.
  // An accepted finish consumes the run even if storage fails, since COMMIT may be ambiguous.
  RunFinish finish(const RankingCatalog &catalog, RankingRepository &rankings, int world_id,
                   int ranking_cms_id, std::vector<RankingMember> members,
                   std::int64_t now_monotonic_ticks, std::int64_t now_epoch_ticks);

  // Drops the attempt without a record. Returns false when none was running.
  bool cancel();

  const std::optional<RankingRun> &active() const;

private:
  std::optional<RankingRun> active_;
};

} // namespace chronicles::ranking
