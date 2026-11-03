#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chronicles {

struct MemoryDatabase;

// Exploration is a monotonic set of FogArea CMS ids, independent of gameplay revisions.
inline constexpr std::size_t kMaximumFogAreas = 4096;

// Explored fog areas of owned characters; a monotonic set outside the world-progress revision.
// Implementations: MemoryFogRepository and PostgresFogRepository.
class FogRepository {
public:
  virtual ~FogRepository() = default;

  virtual std::vector<int> character_fog_areas(std::int64_t account_id,
                                             std::int64_t character_id, int world_id) = 0;

  // Merge discoveries atomically. Empty/repeated/stale batches never erase saved areas.
  virtual bool discover_fog_areas(std::int64_t account_id, std::int64_t character_id,
                                  int world_id, const std::vector<int> &area_ids) = 0;
};

// Memory implementation of FogRepository. The repository holds the shared MemoryDatabase by
// shared_ptr and owns the fog-area table; every access locks database_->mutex like the
// former in-memory member. The constructor registers a CharacterDeletionHook that erases
// a deleted character's areas, and the destructor neutralizes it so the callback never
// outlives the table.
class MemoryFogRepository final : public FogRepository {
public:
  explicit MemoryFogRepository(std::shared_ptr<MemoryDatabase> database);
  ~MemoryFogRepository() override;

  // The registered hook callback points at this repository's table, so copying is forbidden.
  MemoryFogRepository(const MemoryFogRepository &) = delete;
  MemoryFogRepository &operator=(const MemoryFogRepository &) = delete;

  std::vector<int> character_fog_areas(std::int64_t account_id, std::int64_t character_id,
                                       int world_id) override;

  bool discover_fog_areas(std::int64_t account_id, std::int64_t character_id, int world_id,
                          const std::vector<int> &area_ids) override;

private:
  std::shared_ptr<MemoryDatabase> database_;
  // Position of this repository's entry in database_->deletion_hooks. The destructor
  // neutralizes it so the callback never outlives the fog table.
  std::size_t deletion_hook_index_{};
  std::unordered_map<std::int64_t, std::set<int>> fog_areas_;
};

// PostgreSQL implementation of FogRepository. The repository keeps its own copy of the
// connection string and opens one pqxx::connection per call; the character row lock
// serializes discoveries per character.
class PostgresFogRepository final : public FogRepository {
public:
  explicit PostgresFogRepository(std::string connection) : connection_(std::move(connection)) {}

  std::vector<int> character_fog_areas(std::int64_t account_id, std::int64_t character_id,
                                       int world_id) override;

  bool discover_fog_areas(std::int64_t account_id, std::int64_t character_id, int world_id,
                          const std::vector<int> &area_ids) override;

private:
  std::string connection_;
};

} // namespace chronicles
