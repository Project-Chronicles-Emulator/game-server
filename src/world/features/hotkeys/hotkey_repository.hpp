#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chronicles {

struct MemoryDatabase;

// One hot-key quick slot (x5.pb.rest.CharHotKey). The client's quick-slot manager starts a
// summoned mount only from a slot of kind summon_npc. Kinds follow x5.cms.EX5HotKeyCategory.
enum class HotKeyKind : int {
  action = 1,
  flight_tool = 2,
  item = 3,
  summon_npc = 4,
};

struct CharacterHotKey {
  int slot{};
  HotKeyKind kind{HotKeyKind::action};
  // EX5HotKeyAction for actions, otherwise the FlightTool, Item or SummonNpc CMS id.
  int value{};

  bool operator==(const CharacterHotKey &) const = default;
};

// Highest quick-slot index stored. The HUD shows four slots (keys 5-8); the bound leaves room
// for other layouts without accepting arbitrary indexes.
inline constexpr int kMaximumHotKeySlot = 63;

// Hot-key quick slots of owned characters; client preferences outside the world-progress
// revision.
// Implementations: MemoryHotKeyRepository and PostgresHotKeyRepository.
class HotKeyRepository {
public:
  virtual ~HotKeyRepository() = default;

  // Hot-key quick slots of an owned character (REST CharHotKeyLoadReq/CharHotKeySaveReq). They
  // are client preferences, outside the world-progress revision.
  virtual std::vector<CharacterHotKey> character_hotkeys(std::int64_t account_id,
                                                         std::int64_t character_id,
                                                         int world_id) = 0;

  // Stores one slot, or clears it when hotkey is empty. Returns false when the character is not
  // owned by the account. Throws std::invalid_argument for a slot outside [0, kMaximumHotKeySlot].
  virtual bool save_character_hotkey(std::int64_t account_id, std::int64_t character_id,
                                     int world_id, int slot,
                                     const std::optional<CharacterHotKey> &hotkey) = 0;
};

// Memory implementation of HotKeyRepository. The repository holds the shared MemoryDatabase
// by shared_ptr and owns the hot-key table; every access locks database_->mutex like the
// former in-memory member. Character deletion does not erase hot-key rows: the former
// memory store never erased them either (PostgreSQL's ON DELETE CASCADE does), and this
// repository preserves that difference unchanged.
class MemoryHotKeyRepository final : public HotKeyRepository {
public:
  explicit MemoryHotKeyRepository(std::shared_ptr<MemoryDatabase> database)
      : database_(std::move(database)) {}

  // A copy would share the database while pretending to own the hot-key table, so copying is
  // forbidden.
  MemoryHotKeyRepository(const MemoryHotKeyRepository &) = delete;
  MemoryHotKeyRepository &operator=(const MemoryHotKeyRepository &) = delete;

  std::vector<CharacterHotKey> character_hotkeys(std::int64_t account_id,
                                                 std::int64_t character_id,
                                                 int world_id) override;

  bool save_character_hotkey(std::int64_t account_id, std::int64_t character_id, int world_id,
                             int slot, const std::optional<CharacterHotKey> &hotkey) override;

private:
  std::shared_ptr<MemoryDatabase> database_;
  // Character id -> slot -> hot key.
  std::unordered_map<std::int64_t, std::map<int, CharacterHotKey>> hotkeys_;
};

// PostgreSQL implementation of HotKeyRepository. The repository keeps its own copy of the
// connection string and opens one pqxx::connection per call; the character row lock
// serializes slot writes per character.
class PostgresHotKeyRepository final : public HotKeyRepository {
public:
  explicit PostgresHotKeyRepository(std::string connection)
      : connection_(std::move(connection)) {}

  std::vector<CharacterHotKey> character_hotkeys(std::int64_t account_id,
                                                 std::int64_t character_id,
                                                 int world_id) override;

  bool save_character_hotkey(std::int64_t account_id, std::int64_t character_id, int world_id,
                             int slot, const std::optional<CharacterHotKey> &hotkey) override;

private:
  std::string connection_;
};

} // namespace chronicles
