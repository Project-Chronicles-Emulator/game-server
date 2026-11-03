#include "world/features/fog/fog_repository.hpp"

#include "persistence/memory_database.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

void validate_area_ids(const std::vector<int> &area_ids) {
  if (area_ids.size() > kMaximumFogAreas) {
    throw std::invalid_argument("Fog discoveries exceed the stored count");
  }
  for (const int area_id : area_ids) {
    if (area_id <= 0) {
      throw std::invalid_argument("Fog discoveries require positive ids");
    }
  }
}

} // namespace

MemoryFogRepository::MemoryFogRepository(std::shared_ptr<MemoryDatabase> database)
    : database_(std::move(database)) {
  // Reproduce the former in-memory deletion behavior: erasing a character removes its fog
  // rows, like PostgreSQL's ON DELETE CASCADE. The callback runs with database_->mutex held.
  MemoryDatabase::CharacterDeletionHook hook;
  hook.erase_rows = [this](std::int64_t character_id) {
    fog_areas_.erase(character_id);
  };
  std::lock_guard lock(database_->mutex);
  deletion_hook_index_ = database_->deletion_hooks.size();
  database_->deletion_hooks.push_back(std::move(hook));
}

MemoryFogRepository::~MemoryFogRepository() {
  // The database can outlive this repository through the shared accessor; neutralize the
  // hook so its callback never touches the fog table after it is gone.
  std::lock_guard lock(database_->mutex);
  if (deletion_hook_index_ < database_->deletion_hooks.size()) {
    database_->deletion_hooks[deletion_hook_index_].erase_rows = nullptr;
  }
}

std::vector<int> MemoryFogRepository::character_fog_areas(std::int64_t account_id,
                                                          std::int64_t character_id,
                                                          int world_id) {
  std::lock_guard lock(database_->mutex);
  const bool owned = std::ranges::any_of(database_->characters, [&](const Character &character) {
    return character.id == character_id && character.account_id == account_id &&
           character.world_id == world_id;
  });
  if (!owned) {
    return {};
  }

  const auto stored = fog_areas_.find(character_id);
  if (stored == fog_areas_.end()) {
    return {};
  }
  return {stored->second.begin(), stored->second.end()};
}

bool MemoryFogRepository::discover_fog_areas(std::int64_t account_id,
                                               std::int64_t character_id, int world_id,
                                               const std::vector<int> &area_ids) {
  validate_area_ids(area_ids);

  std::lock_guard lock(database_->mutex);
  const bool owned = std::ranges::any_of(database_->characters, [&](const Character &character) {
    return character.id == character_id && character.account_id == account_id &&
           character.world_id == world_id;
  });
  if (!owned) {
    return false;
  }

  auto merged = fog_areas_[character_id];
  merged.insert(area_ids.begin(), area_ids.end());
  if (merged.size() > kMaximumFogAreas) {
    throw std::invalid_argument("Character fog discoveries exceed the stored count");
  }
  fog_areas_[character_id] = std::move(merged);
  return true;
}

std::vector<int> PostgresFogRepository::character_fog_areas(std::int64_t account_id,
                                                            std::int64_t character_id,
                                                            int world_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows =
      transaction.exec("SELECT f.fog_area_id FROM emulator_character_fog_areas f "
                       "JOIN emulator_characters c ON c.id=f.character_id "
                       "WHERE c.account_id=$1 AND c.id=$2 AND c.world_id=$3 ORDER BY f.fog_area_id",
                       pqxx::params{account_id, character_id, world_id});

  std::vector<int> area_ids;
  area_ids.reserve(rows.size());
  for (const auto &row : rows) {
    area_ids.push_back(row[0].as<int>());
  }
  validate_area_ids(area_ids);
  return area_ids;
}

bool PostgresFogRepository::discover_fog_areas(std::int64_t account_id,
                                                 std::int64_t character_id, int world_id,
                                                 const std::vector<int> &area_ids) {
  validate_area_ids(area_ids);

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  // Serialize discoveries per character, including the capacity check and deletion.
  const auto owner = transaction.exec(
      "SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3 "
      "FOR UPDATE",
      pqxx::params{account_id, character_id, world_id});
  if (owner.empty()) {
    return false;
  }

  transaction.exec("INSERT INTO emulator_character_fog_areas(character_id,fog_area_id) "
                   "SELECT $1,value::integer FROM jsonb_array_elements_text($2::jsonb) "
                   "ON CONFLICT (character_id,fog_area_id) DO NOTHING",
                   pqxx::params{character_id, nlohmann::json(area_ids).dump()});
  const auto count =
      transaction.exec("SELECT count(*) FROM emulator_character_fog_areas WHERE character_id=$1",
                       pqxx::params{character_id});
  if (count.front()[0].as<std::size_t>() > kMaximumFogAreas) {
    throw std::invalid_argument("Character fog discoveries exceed the stored count");
  }

  transaction.commit();
  return true;
}

} // namespace chronicles
