// Hot-key quick-slot storage for the memory and PostgreSQL repositories.
//
// Quick slots are client preferences served by the REST CharHotKeyLoadReq/CharHotKeySaveReq
// routes (src/world/features/hotkeys/hotkey_rest.cpp). They are not part of the world-progress
// revision, so a slot change never conflicts with a concurrent gameplay commit.

#include "world/features/hotkeys/hotkey_repository.hpp"

#include "persistence/memory_database.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <stdexcept>

namespace chronicles {
namespace {

void validate_slot(int slot) {
  if (slot < 0 || slot > kMaximumHotKeySlot) {
    throw std::invalid_argument("Hot-key slot is outside the stored range");
  }
}

void validate_hotkey(const CharacterHotKey &hotkey) {
  const auto kind = static_cast<int>(hotkey.kind);
  if (kind < static_cast<int>(HotKeyKind::action) ||
      kind > static_cast<int>(HotKeyKind::summon_npc) || hotkey.value <= 0) {
    throw std::invalid_argument("Hot key needs a known kind and a positive value");
  }
}

} // namespace

std::vector<CharacterHotKey>
MemoryHotKeyRepository::character_hotkeys(std::int64_t account_id, std::int64_t character_id,
                                            int world_id) {
  std::lock_guard lock(database_->mutex);
  const bool owned = std::ranges::any_of(database_->characters, [&](const Character &character) {
    return character.id == character_id && character.account_id == account_id &&
           character.world_id == world_id;
  });
  if (!owned) {
    return {};
  }

  std::vector<CharacterHotKey> hotkeys;
  const auto stored = hotkeys_.find(character_id);
  if (stored != hotkeys_.end()) {
    for (const auto &[slot, hotkey] : stored->second) {
      hotkeys.push_back(hotkey);
    }
  }
  return hotkeys;
}

bool MemoryHotKeyRepository::save_character_hotkey(
    std::int64_t account_id, std::int64_t character_id, int world_id, int slot,
    const std::optional<CharacterHotKey> &hotkey) {
  validate_slot(slot);
  if (hotkey) {
    validate_hotkey(*hotkey);
  }

  std::lock_guard lock(database_->mutex);
  const bool owned = std::ranges::any_of(database_->characters, [&](const Character &character) {
    return character.id == character_id && character.account_id == account_id &&
           character.world_id == world_id;
  });
  if (!owned) {
    return false;
  }

  auto &slots = hotkeys_[character_id];
  if (!hotkey) {
    slots.erase(slot);
    return true;
  }
  auto stored = *hotkey;
  stored.slot = slot;
  slots[slot] = stored;
  return true;
}

std::vector<CharacterHotKey>
PostgresHotKeyRepository::character_hotkeys(std::int64_t account_id,
                                              std::int64_t character_id, int world_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(
      "SELECT h.slot,h.kind,h.value FROM emulator_character_hotkeys h "
      "JOIN emulator_characters c ON c.id=h.character_id "
      "WHERE c.account_id=$1 AND c.id=$2 AND c.world_id=$3 ORDER BY h.slot",
      pqxx::params{account_id, character_id, world_id});

  std::vector<CharacterHotKey> hotkeys;
  for (const auto &row : rows) {
    CharacterHotKey hotkey;
    hotkey.slot = row[0].as<int>();
    hotkey.kind = static_cast<HotKeyKind>(row[1].as<int>());
    hotkey.value = row[2].as<int>();
    validate_slot(hotkey.slot);
    validate_hotkey(hotkey);
    hotkeys.push_back(hotkey);
  }
  return hotkeys;
}

bool PostgresHotKeyRepository::save_character_hotkey(
    std::int64_t account_id, std::int64_t character_id, int world_id, int slot,
    const std::optional<CharacterHotKey> &hotkey) {
  validate_slot(slot);
  if (hotkey) {
    validate_hotkey(*hotkey);
  }

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  // Locking the character row proves ownership and serializes slot writes per character.
  const auto owner = transaction.exec(
      "SELECT id FROM emulator_characters WHERE account_id=$1 AND id=$2 AND world_id=$3 "
      "FOR UPDATE",
      pqxx::params{account_id, character_id, world_id});
  if (owner.empty()) {
    return false;
  }

  if (hotkey) {
    transaction.exec(
        "INSERT INTO emulator_character_hotkeys(character_id,slot,kind,value) "
        "VALUES ($1,$2,$3,$4) ON CONFLICT (character_id,slot) DO UPDATE SET "
        "kind=EXCLUDED.kind,value=EXCLUDED.value",
        pqxx::params{character_id, slot, static_cast<int>(hotkey->kind), hotkey->value});
  } else {
    transaction.exec("DELETE FROM emulator_character_hotkeys WHERE character_id=$1 AND slot=$2",
                     pqxx::params{character_id, slot});
  }
  transaction.commit();
  return true;
}

} // namespace chronicles
