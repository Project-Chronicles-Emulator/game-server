#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace chronicles {

struct MemoryDatabase;

// One character named by a leaderboard record (src/world/features/events/ranking.hpp). Display
// fields are copied when the record is made, so a later rename or deletion does not rewrite past
// results.
struct RankingMember {
  std::int64_t character_id{};
  std::string name;
  int level{};
  // x5.cms.EX5CharacterClass value.
  int char_class{};

  bool operator==(const RankingMember &) const = default;
};

// A leaderboard entry of one ranking period. The score is a boss kill time in milliseconds,
// so a lower score ranks higher. Party records list every member, the finisher first.
struct RankingRecord {
  int world_id{};
  int ranking_cms_id{};
  // Epoch ticks (100 ns since the Unix epoch) of the period's last reset boundary.
  std::int64_t period_start_epoch_ticks{};
  // Stable identity of the record holder within the period (see ranking::holder_key).
  std::string holder_key;
  std::int64_t score{};
  std::int64_t created_epoch_ticks{};
  std::vector<RankingMember> members;

  bool operator==(const RankingRecord &) const = default;
};

// Best-first leaderboard slice and the requesting character's own best entry.
struct RankingStanding {
  std::vector<RankingRecord> top;
  std::optional<RankingRecord> own;
  // One-based rank of `own`; zero when the character has no entry.
  int own_rank{};
};

// Party records name at most PartyMaxMemberNum (4) members; the bound leaves room for larger
// groups without accepting unbounded rows.
inline constexpr std::size_t kMaximumRankingMembers = 12;
inline constexpr std::size_t kMaximumRankingHolderKeyBytes = 200;
inline constexpr std::size_t kMaximumRankingNameBytes = 64;

// Throws std::invalid_argument when a record is outside the persisted value ranges.
void validate_ranking_record(const RankingRecord &record);

// Event leaderboards: best-entry submission, period standings and the oldest period lookup.
// Implementations: MemoryRankingRepository and PostgresRankingRepository.
class RankingRepository {
public:
  virtual ~RankingRepository() = default;

  // Leaderboards (src/world/features/events/ranking_storage.cpp). A holder keeps only its best
  // entry per period: the record replaces a stored one only with a lower score, or an equal
  // score set earlier. Returns true when `record` is now the stored entry. Throws
  // std::invalid_argument for an invalid record.
  virtual bool submit_ranking_record(const RankingRecord &record) = 0;

  // Entries ordered by score, then creation time, then holder key. `own` is the best entry
  // naming `character_id` as a member.
  virtual RankingStanding ranking_standing(int world_id, int ranking_cms_id,
                                           std::int64_t period_start_epoch_ticks,
                                           std::size_t limit, std::int64_t character_id) = 0;

  // Start of the oldest period holding any entry of this ranking.
  virtual std::optional<std::int64_t> first_ranking_period(int world_id, int ranking_cms_id) = 0;
};

// Memory implementation of RankingRepository. The repository holds the shared MemoryDatabase
// by shared_ptr and owns the leaderboard table; every access locks database_->mutex like the
// former in-memory member. Records name deleted characters by their copied display fields,
// so no deletion hook is needed (PostgreSQL retains the rows as well).
class MemoryRankingRepository final : public RankingRepository {
public:
  explicit MemoryRankingRepository(std::shared_ptr<MemoryDatabase> database)
      : database_(std::move(database)) {}

  // Copies would clone the ranking table and diverge from the shared database.
  MemoryRankingRepository(const MemoryRankingRepository &) = delete;
  MemoryRankingRepository &operator=(const MemoryRankingRepository &) = delete;

  bool submit_ranking_record(const RankingRecord &record) override;

  RankingStanding ranking_standing(int world_id, int ranking_cms_id,
                                   std::int64_t period_start_epoch_ticks, std::size_t limit,
                                   std::int64_t character_id) override;

  std::optional<std::int64_t> first_ranking_period(int world_id, int ranking_cms_id) override;

private:
  std::shared_ptr<MemoryDatabase> database_;
  // (world, ranking, period start, holder key) -> best record.
  std::map<std::tuple<int, int, std::int64_t, std::string>, RankingRecord> ranking_records_;
};

// PostgreSQL implementation of RankingRepository. The repository keeps its own copy of the
// connection string and opens one pqxx::connection per call.
class PostgresRankingRepository final : public RankingRepository {
public:
  explicit PostgresRankingRepository(std::string connection) : connection_(std::move(connection)) {}

  bool submit_ranking_record(const RankingRecord &record) override;

  RankingStanding ranking_standing(int world_id, int ranking_cms_id,
                                   std::int64_t period_start_epoch_ticks, std::size_t limit,
                                   std::int64_t character_id) override;

  std::optional<std::int64_t> first_ranking_period(int world_id, int ranking_cms_id) override;

private:
  std::string connection_;
};

} // namespace chronicles
