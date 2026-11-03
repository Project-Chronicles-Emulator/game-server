#include "world/features/events/ranking_repository.hpp"

#include "persistence/memory_database.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <stdexcept>
#include <tuple>

namespace chronicles {
namespace {

using Json = nlohmann::json;

// Leaderboard order: the lower kill time first; an equal time set earlier ranks higher; the
// holder key makes the order total, so ranks never depend on container iteration order.
bool ranks_before(const RankingRecord &first, const RankingRecord &second) {
  return std::tie(first.score, first.created_epoch_ticks, first.holder_key) <
         std::tie(second.score, second.created_epoch_ticks, second.holder_key);
}

// A submitted run replaces the holder's stored entry only when it is strictly better.
bool improves(const RankingRecord &candidate, const RankingRecord &stored) {
  return std::tie(candidate.score, candidate.created_epoch_ticks) <
         std::tie(stored.score, stored.created_epoch_ticks);
}

bool names_member(const RankingRecord &record, std::int64_t character_id) {
  return std::ranges::any_of(record.members, [&](const RankingMember &member) {
    return member.character_id == character_id;
  });
}

Json members_json(const std::vector<RankingMember> &members) {
  Json result = Json::array();
  for (const auto &member : members) {
    result.push_back({{"character_id", member.character_id},
                      {"name", member.name},
                      {"level", member.level},
                      {"char_class", member.char_class}});
  }
  return result;
}

std::vector<RankingMember> parse_members(const std::string &text) {
  const auto document = Json::parse(text);
  if (!document.is_array()) {
    throw std::runtime_error("Stored ranking members are not an array");
  }

  std::vector<RankingMember> members;
  for (const auto &entry : document) {
    RankingMember member;
    member.character_id = entry.at("character_id").get<std::int64_t>();
    member.name = entry.at("name").get<std::string>();
    member.level = entry.at("level").get<int>();
    member.char_class = entry.at("char_class").get<int>();
    members.push_back(std::move(member));
  }
  return members;
}

std::string member_id_array(const std::vector<RankingMember> &members) {
  std::string array = "{";
  for (std::size_t index = 0; index < members.size(); ++index) {
    if (index > 0) {
      array += ',';
    }
    array += std::to_string(members[index].character_id);
  }
  array += '}';
  return array;
}

RankingRecord record_from_row(const pqxx::row &row, int world_id, int ranking_cms_id,
                              std::int64_t period_start) {
  RankingRecord record;
  record.world_id = world_id;
  record.ranking_cms_id = ranking_cms_id;
  record.period_start_epoch_ticks = period_start;
  record.holder_key = row["holder_key"].as<std::string>();
  record.score = row["score"].as<std::int64_t>();
  record.created_epoch_ticks = row["created"].as<std::int64_t>();
  record.members = parse_members(row["members"].as<std::string>());
  return record;
}

constexpr const char *kRecordColumns = "holder_key,score,created,members::text AS members";

} // namespace

void validate_ranking_record(const RankingRecord &record) {
  if (record.world_id <= 0 || record.ranking_cms_id <= 0 || record.period_start_epoch_ticks <= 0 ||
      record.created_epoch_ticks <= 0) {
    throw std::invalid_argument("Ranking record identity or time is invalid");
  }
  if (record.score < 0) {
    throw std::invalid_argument("Ranking record score is negative");
  }
  if (record.holder_key.empty() || record.holder_key.size() > kMaximumRankingHolderKeyBytes) {
    throw std::invalid_argument("Ranking record holder key length is invalid");
  }
  if (record.members.empty() || record.members.size() > kMaximumRankingMembers) {
    throw std::invalid_argument("Ranking record member count is invalid");
  }

  for (const auto &member : record.members) {
    if (member.character_id <= 0 || member.level < 0 || member.char_class < 0) {
      throw std::invalid_argument("Ranking record member is invalid");
    }
    if (member.name.empty() || member.name.size() > kMaximumRankingNameBytes) {
      throw std::invalid_argument("Ranking record member name length is invalid");
    }
  }
}

bool MemoryRankingRepository::submit_ranking_record(const RankingRecord &record) {
  validate_ranking_record(record);

  std::lock_guard lock(database_->mutex);
  const auto key = std::make_tuple(record.world_id, record.ranking_cms_id,
                                   record.period_start_epoch_ticks, record.holder_key);
  const auto stored = ranking_records_.find(key);
  if (stored != ranking_records_.end() && !improves(record, stored->second)) {
    return false;
  }

  ranking_records_[key] = record;
  return true;
}

RankingStanding MemoryRankingRepository::ranking_standing(int world_id, int ranking_cms_id,
                                                          std::int64_t period_start_epoch_ticks,
                                                          std::size_t limit,
                                                          std::int64_t character_id) {
  std::vector<RankingRecord> period;
  {
    std::lock_guard lock(database_->mutex);
    for (const auto &[key, record] : ranking_records_) {
      if (record.world_id == world_id && record.ranking_cms_id == ranking_cms_id &&
          record.period_start_epoch_ticks == period_start_epoch_ticks) {
        period.push_back(record);
      }
    }
  }
  std::ranges::sort(period, ranks_before);

  RankingStanding standing;
  const auto own = std::ranges::find_if(
      period, [&](const RankingRecord &record) { return names_member(record, character_id); });
  if (own != period.end()) {
    standing.own = *own;
    standing.own_rank = static_cast<int>(std::distance(period.begin(), own) + 1);
  }

  const auto kept = std::min(limit, period.size());
  standing.top.assign(period.begin(), period.begin() + static_cast<std::ptrdiff_t>(kept));
  return standing;
}

std::optional<std::int64_t> MemoryRankingRepository::first_ranking_period(int world_id,
                                                                        int ranking_cms_id) {
  std::lock_guard lock(database_->mutex);
  std::optional<std::int64_t> first;
  for (const auto &[key, record] : ranking_records_) {
    if (record.world_id != world_id || record.ranking_cms_id != ranking_cms_id) {
      continue;
    }
    if (!first || record.period_start_epoch_ticks < *first) {
      first = record.period_start_epoch_ticks;
    }
  }
  return first;
}

bool PostgresRankingRepository::submit_ranking_record(const RankingRecord &record) {
  validate_ranking_record(record);

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  // One statement keeps the best-entry comparison and the write atomic under concurrent
  // finishes of the same holder.
  const auto stored = transaction.exec(
      "INSERT INTO emulator_ranking_records"
      "(world_id,ranking_cms_id,period_start,holder_key,score,created,member_ids,members) "
      "VALUES ($1,$2,$3,$4,$5,$6,$7::bigint[],$8::jsonb) "
      "ON CONFLICT (world_id,ranking_cms_id,period_start,holder_key) DO UPDATE SET "
      "score=EXCLUDED.score,created=EXCLUDED.created,member_ids=EXCLUDED.member_ids,"
      "members=EXCLUDED.members "
      "WHERE (EXCLUDED.score,EXCLUDED.created) < "
      "(emulator_ranking_records.score,emulator_ranking_records.created) "
      "RETURNING 1",
      pqxx::params{record.world_id, record.ranking_cms_id, record.period_start_epoch_ticks,
                   record.holder_key, record.score, record.created_epoch_ticks,
                   member_id_array(record.members), members_json(record.members).dump()});
  transaction.commit();
  return !stored.empty();
}

RankingStanding PostgresRankingRepository::ranking_standing(int world_id, int ranking_cms_id,
                                                            std::int64_t period_start_epoch_ticks,
                                                            std::size_t limit,
                                                            std::int64_t character_id) {
  pqxx::connection connection(connection_);
  // The list, own entry and rank count must share one snapshot while finishes commit.
  pqxx::transaction<pqxx::isolation_level::repeatable_read, pqxx::write_policy::read_only>
      transaction(connection);

  RankingStanding standing;
  // Explicit C collation matches the memory repository's byte-wise holder-key tie-break in
  // any locale.
  const auto top =
      transaction.exec(std::string("SELECT ") + kRecordColumns +
                           " FROM emulator_ranking_records WHERE world_id=$1 AND ranking_cms_id=$2 "
                           "AND period_start=$3 ORDER BY score,created,holder_key COLLATE \"C\" "
                           "LIMIT $4",
                       pqxx::params{world_id, ranking_cms_id, period_start_epoch_ticks,
                                    static_cast<std::int64_t>(limit)});
  for (const auto &row : top) {
    standing.top.push_back(
        record_from_row(row, world_id, ranking_cms_id, period_start_epoch_ticks));
  }

  const auto own = transaction.exec(
      std::string("SELECT ") + kRecordColumns +
          " FROM emulator_ranking_records WHERE world_id=$1 AND ranking_cms_id=$2 "
          "AND period_start=$3 AND $4=ANY(member_ids) "
          "ORDER BY score,created,holder_key COLLATE \"C\" LIMIT 1",
      pqxx::params{world_id, ranking_cms_id, period_start_epoch_ticks, character_id});
  if (own.empty()) {
    return standing;
  }

  standing.own = record_from_row(own.front(), world_id, ranking_cms_id, period_start_epoch_ticks);
  const auto better = transaction.exec(
      "SELECT count(*) FROM emulator_ranking_records WHERE world_id=$1 AND ranking_cms_id=$2 "
      "AND period_start=$3 AND (score,created,holder_key COLLATE \"C\") < ($4,$5,$6)",
      pqxx::params{world_id, ranking_cms_id, period_start_epoch_ticks, standing.own->score,
                   standing.own->created_epoch_ticks, standing.own->holder_key});
  standing.own_rank = better.front()[0].as<int>() + 1;
  return standing;
}

std::optional<std::int64_t> PostgresRankingRepository::first_ranking_period(int world_id,
                                                                          int ranking_cms_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec("SELECT min(period_start) FROM emulator_ranking_records "
                                     "WHERE world_id=$1 AND ranking_cms_id=$2",
                                     pqxx::params{world_id, ranking_cms_id});
  if (rows.front()[0].is_null()) {
    return std::nullopt;
  }
  return rows.front()[0].as<std::int64_t>();
}

} // namespace chronicles
