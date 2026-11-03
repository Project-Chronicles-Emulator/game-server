#include "world/features/events/ranking.hpp"

#include "common/catalog_fields.hpp"
#include "data/client_catalog.hpp"
#include "common/clock_ticks.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace chronicles::ranking {
namespace {

using Json = nlohmann::json;
using EpochTicks = std::chrono::duration<std::int64_t, std::ratio<1, kEpochTicksPerSecond>>;

constexpr std::uint16_t kCommonErrorResponse = 2;
// Observed monthly reset: 2026-11-01T10:00:00Z (P2 RankingListRes.next_reset_epochtime).
constexpr std::chrono::hours kResetHourUtc{10};
constexpr std::int64_t kTicksPerMillisecond = kEpochTicksPerSecond / 1000;
constexpr char kServerRecordType[] = "ERankingRecordType_Server";
constexpr char kWorldRecordType[] = "ERankingRecordType_World";

std::chrono::sys_seconds to_time(std::int64_t epoch_ticks) {
  return std::chrono::floor<std::chrono::seconds>(
      std::chrono::sys_time<EpochTicks>(EpochTicks(epoch_ticks)));
}

std::int64_t to_ticks(std::chrono::sys_seconds time) {
  return std::chrono::duration_cast<EpochTicks>(time.time_since_epoch()).count();
}

std::chrono::sys_seconds monthly_boundary(std::chrono::year_month month) {
  const std::chrono::sys_days first_day{month / std::chrono::day{1}};
  return first_day + kResetHourUtc;
}

RankingPeriod monthly_period(std::chrono::sys_seconds now) {
  // Shifting by the reset hour makes the calendar month of `shifted` the current period.
  const auto shifted = std::chrono::floor<std::chrono::days>(now - kResetHourUtc);
  const std::chrono::year_month_day date{shifted};
  const std::chrono::year_month month{date.year(), date.month()};
  return {to_ticks(monthly_boundary(month)),
          to_ticks(monthly_boundary(month + std::chrono::months{1}))};
}

RankingPeriod fixed_period(std::chrono::sys_seconds now, std::chrono::days length,
                           std::chrono::days anchor_offset) {
  // Day boundaries are shifted by the reset hour; `anchor_offset` moves weekly periods from the
  // epoch's Thursday to Monday.
  const auto shifted_day = std::chrono::floor<std::chrono::days>(now - kResetHourUtc);
  const auto days_since_anchor = (shifted_day.time_since_epoch() - anchor_offset).count();
  const auto length_days = length.count();
  auto elapsed_in_period = days_since_anchor % length_days;
  if (elapsed_in_period < 0) {
    elapsed_in_period += length_days;
  }

  const auto start_day = shifted_day - std::chrono::days{elapsed_in_period};
  const std::chrono::sys_seconds start = start_day + kResetHourUtc;
  return {to_ticks(start), to_ticks(start + length)};
}

std::optional<int> request_integer(const Json &message, const char *key) {
  const auto found = message.find(key);
  if (found == message.end()) {
    return std::nullopt;
  }
  if (!found->is_number_integer()) {
    return std::nullopt;
  }

  const auto value = found->get<std::int64_t>();
  if (value <= 0 || value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

// The request's record type. One server has one leaderboard, so World and Server read the same
// entries; the type is echoed back because the client keys its cached lists by it.
std::optional<std::string> record_type(const Json &message) {
  const auto found = message.find("ranking_record_type");
  if (found == message.end()) {
    return std::string(kServerRecordType);
  }
  if (found->is_string()) {
    const auto text = found->get<std::string>();
    if (text == kServerRecordType || text == kWorldRecordType) {
      return text;
    }
    return std::nullopt;
  }
  if (found->is_number_integer()) {
    const auto value = found->get<std::int64_t>();
    if (value == 0) {
      return std::string(kServerRecordType);
    }
    if (value == 1) {
      return std::string(kWorldRecordType);
    }
  }
  return std::nullopt;
}

Json core_info(const RankingMember &member, int world_server_id) {
  return {{"char_id", std::to_string(member.character_id)},
          {"name", member.name},
          {"level", member.level},
          {"char_class", member.char_class},
          {"world_server_id", world_server_id}};
}

std::int64_t reset_counter(RankingRepository &rankings, const RankingDefinition &definition,
                           int world_id, const RankingPeriod &period) {
  // Capture P2 reported 1 for an empty board; each later reset since the first stored period
  // advances it by one.
  const auto first = rankings.first_ranking_period(world_id, definition.id);
  if (!first || *first > period.start_epoch_ticks) {
    return 1;
  }
  return 1 + periods_between(definition.reset, *first, period.start_epoch_ticks);
}

GameplayPacket record_failure(int ranking_cms_id, const char *code) {
  return {kRankingRecordResponse, {{"ec", code}, {"ranking_cms_id", ranking_cms_id}}};
}

ResetType parse_reset_type(const std::string &text) {
  if (text == "Daily") {
    return ResetType::daily;
  }
  if (text == "Weekly") {
    return ResetType::weekly;
  }
  if (text == "Monthly") {
    return ResetType::monthly;
  }
  throw std::runtime_error("Unknown Ranking.resetType: " + text);
}

RankingType parse_ranking_type(const std::string &text) {
  if (text == "Solo") {
    return RankingType::solo;
  }
  if (text == "Party") {
    return RankingType::party;
  }
  throw std::runtime_error("Unknown Ranking.rankingType: " + text);
}

} // namespace

RankingPeriod ranking_period(ResetType reset, std::int64_t now_epoch_ticks) {
  const auto now = to_time(now_epoch_ticks);
  switch (reset) {
  case ResetType::daily:
    return fixed_period(now, std::chrono::days{1}, std::chrono::days{0});
  case ResetType::weekly: {
    // 1970-01-01 was a Thursday; Monday 1970-01-05 anchors the weeks (Candidate).
    constexpr std::chrono::days kEpochToMonday{4};
    return fixed_period(now, std::chrono::days{7}, kEpochToMonday);
  }
  case ResetType::monthly:
    return monthly_period(now);
  }
  throw std::invalid_argument("Unknown ranking reset type");
}

std::int64_t periods_between(ResetType reset, std::int64_t earlier_start,
                             std::int64_t later_start) {
  if (later_start <= earlier_start) {
    return 0;
  }

  const auto earlier = to_time(earlier_start);
  const auto later = to_time(later_start);
  switch (reset) {
  case ResetType::daily:
    return std::chrono::floor<std::chrono::days>(later - earlier).count();
  case ResetType::weekly:
    return std::chrono::floor<std::chrono::days>(later - earlier).count() / 7;
  case ResetType::monthly: {
    const std::chrono::year_month_day first{std::chrono::floor<std::chrono::days>(earlier)};
    const std::chrono::year_month_day last{std::chrono::floor<std::chrono::days>(later)};
    const auto months = (last.year() - first.year()).count() * 12 +
                        (static_cast<int>(static_cast<unsigned>(last.month())) -
                         static_cast<int>(static_cast<unsigned>(first.month())));
    return months;
  }
  }
  throw std::invalid_argument("Unknown ranking reset type");
}

std::vector<std::string> RankingCatalog::required_tables() {
  return {"Ranking", "GameVariable"};
}

std::shared_ptr<const RankingCatalog> RankingCatalog::load(ClientCatalog &catalog) {
  constexpr std::int64_t kMaximumListSize = 1000;
  constexpr std::int64_t kMaximumUpdateCycleSeconds = 3600;

  RankingLimits limits;
  if (const auto solo =
          game_variable_integer(catalog, "SoloRankingMaxCount", 1, kMaximumListSize, "Rankings")) {
    limits.solo_list_size = static_cast<int>(*solo);
  }
  if (const auto party =
          game_variable_integer(catalog, "PartyRankingMaxCount", 1, kMaximumListSize, "Rankings")) {
    limits.party_list_size = static_cast<int>(*party);
  }
  if (const auto cycle = game_variable_integer(catalog, "RankingUpdateCycleSec", 1,
                                               kMaximumUpdateCycleSeconds, "Rankings")) {
    limits.update_cycle = std::chrono::seconds{*cycle};
  }
  return std::make_shared<const RankingCatalog>(catalog.rows("Ranking"), limits);
}

RankingCatalog::RankingCatalog(const std::vector<Json> &ranking_rows, RankingLimits limits)
    : limits_(limits) {
  if (limits_.solo_list_size <= 0 || limits_.party_list_size <= 0 ||
      limits_.update_cycle.count() <= 0) {
    throw std::invalid_argument("Ranking limits must be positive");
  }

  for (const auto &row : ranking_rows) {
    RankingDefinition definition;
    definition.id = catalog_fields::required_identifier(row, "id");
    definition.type = parse_ranking_type(catalog_fields::text(row, "rankingType"));
    definition.reset = parse_reset_type(catalog_fields::text(row, "resetType"));
    definition.map_id = catalog_fields::optional_identifier(row, "FK_mapId").value_or(0);
    definition.channel_event_id =
        catalog_fields::optional_identifier(row, "FK_channelEventId").value_or(0);
    if (!definitions_.emplace(definition.id, definition).second) {
      throw std::runtime_error("Duplicate Ranking.id " + std::to_string(definition.id));
    }
  }
}

const RankingDefinition *RankingCatalog::find(int ranking_cms_id) const {
  const auto found = definitions_.find(ranking_cms_id);
  return found == definitions_.end() ? nullptr : &found->second;
}

int RankingCatalog::list_size(RankingType type) const {
  return type == RankingType::party ? limits_.party_list_size : limits_.solo_list_size;
}

std::chrono::seconds RankingCatalog::update_cycle() const {
  return limits_.update_cycle;
}

std::size_t RankingCatalog::size() const {
  return definitions_.size();
}

std::string holder_key(RankingType type, const std::vector<RankingMember> &members) {
  if (members.empty()) {
    throw std::invalid_argument("A ranking holder needs at least one member");
  }
  if (type == RankingType::solo) {
    return "c" + std::to_string(members.front().character_id);
  }

  std::vector<std::int64_t> ids;
  for (const auto &member : members) {
    ids.push_back(member.character_id);
  }
  std::ranges::sort(ids);

  std::string key = "p";
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (index > 0) {
      key += '.';
    }
    key += std::to_string(ids[index]);
  }
  return key;
}

Json ranking_info(const RankingRecord &record, int rank, int world_server_id) {
  if (record.members.empty()) {
    throw std::invalid_argument("A ranking record needs at least one member");
  }

  // Candidate: the finisher is the core entry and the other members follow as party_members.
  auto core = core_info(record.members.front(), world_server_id);
  Json party_members = Json::array();
  for (std::size_t index = 1; index < record.members.size(); ++index) {
    party_members.push_back(core_info(record.members[index], world_server_id));
  }
  if (!party_members.empty()) {
    core["party_members"] = std::move(party_members);
  }

  // RankingInfo.rank is int64, which protobuf JSON carries as text.
  return {{"ranking_cms_id", std::to_string(record.ranking_cms_id)},
          {"score", std::to_string(record.score)},
          {"rank", std::to_string(rank)},
          {"core_info", std::move(core)},
          {"created_at", std::to_string(record.created_epoch_ticks)}};
}

GameplayPacket list_response(const RankingCatalog &catalog, RankingRepository &rankings,
                             int world_id, std::int64_t character_id, const Json &request,
                             std::int64_t now_epoch_ticks) {
  const auto ranking_cms_id = request_integer(request, "ranking_cms_id");
  const auto type = record_type(request);
  const auto *definition = ranking_cms_id ? catalog.find(*ranking_cms_id) : nullptr;
  if (!definition || !type) {
    return {kRankingListResponse,
            {{"ec", "Ranking_InvalidCmsId"}, {"ranking_cms_id", ranking_cms_id.value_or(0)}}};
  }

  const auto period = ranking_period(definition->reset, now_epoch_ticks);
  const auto next_refresh =
      now_epoch_ticks + std::chrono::duration_cast<EpochTicks>(catalog.update_cycle()).count();

  RankingStanding standing;
  std::int64_t counter{};
  try {
    const auto limit = static_cast<std::size_t>(catalog.list_size(definition->type));
    standing = rankings.ranking_standing(world_id, definition->id, period.start_epoch_ticks,
                                         limit, character_id);
    counter = reset_counter(rankings, *definition, world_id, period);
  } catch (const std::exception &error) {
    spdlog::error("Ranking list {} failed: {}", definition->id, error.what());
    return {kCommonErrorResponse, {{"ec", "SystemError"}}};
  }

  Json entries = Json::array();
  for (std::size_t index = 0; index < standing.top.size(); ++index) {
    entries.push_back(ranking_info(standing.top[index], static_cast<int>(index + 1), world_id));
  }

  // Field set and order follow the captured success responses; ec stays at its default.
  Json response{{"rankings", std::move(entries)},
                {"next_timestamp", std::to_string(next_refresh)},
                {"reset_counter", std::to_string(counter)},
                {"ranking_cms_id", definition->id},
                {"ranking_record_type", *type},
                {"is_settling", false},
                {"next_reset_epochtime", std::to_string(period.next_reset_epoch_ticks)}};
  if (standing.own) {
    response["my_ranking"] = ranking_info(*standing.own, standing.own_rank, world_id);
  }
  return {kRankingListResponse, std::move(response)};
}

GameplayPacket RankingRuns::start(const RankingCatalog &catalog, int ranking_cms_id,
                                  std::int64_t now_monotonic_ticks, std::int64_t now_epoch_ticks) {
  const auto *definition = catalog.find(ranking_cms_id);
  if (!definition) {
    return {kRankingRecordStartResponse,
            {{"ec", "Ranking_InvalidCmsId"}, {"ranking_cms_id", ranking_cms_id}}};
  }

  const auto period = ranking_period(definition->reset, now_epoch_ticks);
  active_ = RankingRun{definition->id, now_monotonic_ticks, period.start_epoch_ticks};
  return {kRankingRecordStartResponse,
          {{"ranking_cms_id", definition->id},
           {"start_tick", wire_monotonic_tick(now_monotonic_ticks)}}};
}

RunFinish RankingRuns::finish(const RankingCatalog &catalog, RankingRepository &rankings,
                              int world_id, int ranking_cms_id,
                              std::vector<RankingMember> members,
                              std::int64_t now_monotonic_ticks, std::int64_t now_epoch_ticks) {
  const auto *definition = catalog.find(ranking_cms_id);
  if (!definition) {
    return {record_failure(ranking_cms_id, "Ranking_InvalidCmsId"), false};
  }
  if (!active_ || active_->ranking_cms_id != ranking_cms_id || members.empty()) {
    return {record_failure(ranking_cms_id, "Ranking_FailedToUpdateRank"), false};
  }

  const auto run = *active_;
  // A storage error can leave COMMIT's outcome unknown. Consume an accepted finish rather
  // than retrying it later with a different elapsed time; operators must start a new attempt.
  active_.reset();

  // A reset during the attempt would file its time under a board the run never started on.
  const auto period = ranking_period(definition->reset, now_epoch_ticks);
  if (period.start_epoch_ticks != run.period_start_epoch_ticks) {
    return {record_failure(ranking_cms_id, "Ranking_ResetCounterMismatch"), false};
  }

  if (definition->type == RankingType::solo) {
    members.resize(1);
  }

  const auto elapsed_ms =
      std::max<std::int64_t>(0, now_monotonic_ticks - run.started_monotonic_ticks) /
      kTicksPerMillisecond;

  RankingRecord record;
  record.world_id = world_id;
  record.ranking_cms_id = definition->id;
  record.period_start_epoch_ticks = period.start_epoch_ticks;
  record.holder_key = holder_key(definition->type, members);
  record.score = elapsed_ms;
  record.created_epoch_ticks = now_epoch_ticks;
  record.members = std::move(members);

  bool recorded{};
  RankingStanding standing;
  try {
    recorded = rankings.submit_ranking_record(record);
    standing = rankings.ranking_standing(world_id, definition->id, period.start_epoch_ticks, 0,
                                         record.members.front().character_id);
  } catch (const std::exception &error) {
    spdlog::error("Ranking record {} failed: {}", definition->id, error.what());
    return {record_failure(ranking_cms_id, "Ranking_FailedToUpdateRank"), false};
  }

  Json response{{"ranking_cms_id", definition->id},
                {"time_record_milli", std::to_string(elapsed_ms)}};
  if (standing.own) {
    response["party_ranking"] = ranking_info(*standing.own, standing.own_rank, world_id);
  }
  return {{kRankingRecordResponse, std::move(response)}, recorded};
}

bool RankingRuns::cancel() {
  const bool had_run = active_.has_value();
  active_.reset();
  return had_run;
}

const std::optional<RankingRun> &RankingRuns::active() const {
  return active_;
}

} // namespace chronicles::ranking
