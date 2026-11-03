#pragma once

// Durable housing records and their storage interface.
//
// Housing sites, guild areas, per-character unlocks and move packages are world-scoped records
// shared between characters, unlike WorldProgress. Every record carries a version: a write names
// the version it read and fails, leaving all rows unchanged, when another writer moved it first.
// A commit may also carry character progress transitions (rent items, Hersander Tokens and
// housing storage counts), so a payment and the site it buys succeed or fail together.

#include "world/features/housing/housing_catalog.hpp"
#include "persistence/character_state_repository.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace chronicles::housing {

using CharacterId = std::int64_t;
using AccountId = std::int64_t;
using GuildId = std::int64_t;

// HouseNub.attach_index of a building that is not attached to another one (a main building).
// The stock client leaves the optional field unset for such buildings.
inline constexpr int kUnattachedHouseIndex = -1;

// A placed building. Positions are site-local (HouseNub.pos).
struct HouseRecord {
  std::uint32_t house_id{};
  int attach_index{kUnattachedHouseIndex};
  int housing_id{};
  LocalPosition position;
  float yaw{};
  // Open/closed bit per door or window attach index (HousePartsUpdateRes.parts_state).
  std::uint64_t parts_state{};
  // True when one housing storage item was installed for this building. Preset buildings that
  // came with the lot are not owned items and return nothing when removed.
  bool from_storage{};

  bool operator==(const HouseRecord &) const = default;
};

// A placed object. house_id 0 is a site-level object; otherwise it belongs to that building.
struct DoodadRecord {
  std::uint32_t doodad_id{};
  std::uint32_t house_id{};
  int cms_id{};
  int phase{};
  std::int64_t phase_begin_tick{};
  LocalPosition position;
  float pitch{};
  float yaw{};
  float roll{};
  int interaction_count{};
  std::uint32_t parent_doodad_id{};
  bool from_storage{};

  bool operator==(const DoodadRecord &) const = default;
};

// (house_id, doodad_id): doodad IDs are unique within one building or the site level.
using DoodadKey = std::pair<std::uint32_t, std::uint32_t>;

struct SiteContents {
  std::string house_part_custom_ids;
  std::map<std::uint32_t, HouseRecord> houses;
  std::map<DoodadKey, DoodadRecord> doodads;

  bool operator==(const SiteContents &) const = default;
};

struct SiteKey {
  int zone_key{};
  int channel{};
  int lot_number{};

  auto operator<=>(const SiteKey &) const = default;
};

struct SiteRecord {
  SiteKey key;
  std::int64_t version{};
  AuthorityType authority{AuthorityType::individual};
  CharacterId owner_character_id{};
  AccountId owner_account_id{};
  std::string owner_name;
  // Guild sites are assigned to a member inside the guild's area.
  GuildId guild_id{};
  ContractType contract_type{ContractType::none};
  std::int64_t expiry_epoch_ticks{};
  std::int64_t contracted_epoch_ticks{};
  // Where an owner returns when leaving the site (HousingSite.return_pos), world millimetres.
  std::optional<WorldPosition> return_position;
  SiteContents contents;
  std::int64_t edit_save_cooldown_start_ticks{};

  bool operator==(const SiteRecord &) const = default;
};

// A channel copy of a housing zone. Personal channels have guild_id 0 and no contract; a guild
// area is a channel contracted by one guild.
struct GuildResident {
  int lot_number{};
  CharacterId character_id{};
  std::string name;
  std::int64_t allow_cooldown_start_ticks{};

  bool operator==(const GuildResident &) const = default;
};

struct ChannelRecord {
  int zone_key{};
  int channel{};
  std::int64_t version{};
  GuildId guild_id{};
  std::string guild_name;
  std::string area_name;
  ContractType contract_type{ContractType::none};
  std::int64_t expiry_epoch_ticks{};
  std::map<int, GuildResident> residents;

  bool operator==(const ChannelRecord &) const = default;
};

struct CharacterHousingRecord {
  CharacterId character_id{};
  std::int64_t version{};
  std::set<SiteType> unlocked_site_types;
  bool guild_unlocked{};

  bool operator==(const CharacterHousingRecord &) const = default;
};

// x5.pb.HousingMovePackageSourceReason.
enum class PackageReason : int { none = 0, lease_expired = 1, guild_shrink = 2, cheat = 3 };

struct MovePackageRecord {
  std::int64_t package_id{};
  std::int64_t version{};
  CharacterId owner_character_id{};
  SiteType site_type{SiteType::none};
  std::int64_t created_epoch_ticks{};
  int zone_key{};
  int original_lot_number{};
  int original_channel{};
  PackageReason reason{PackageReason::none};
  SiteContents contents;

  bool operator==(const MovePackageRecord &) const = default;
};

struct HousingSnapshot {
  std::vector<SiteRecord> sites;
  std::vector<ChannelRecord> channels;
  std::vector<CharacterHousingRecord> characters;
  std::vector<MovePackageRecord> packages;
  std::int64_t next_package_id{1};
};

// A write names the version it replaced: zero creates the row, which must not exist yet. erase
// deletes the row at that version.
template <typename Record> struct VersionedWrite {
  Record record;
  std::int64_t expected_version{};
  bool erase{};
};

struct ProgressTransition {
  AccountId account_id{};
  CharacterId character_id{};
  int world_id{};
  WorldProgress before;
  WorldProgress after;
};

struct HousingCommit {
  int world_id{};
  std::vector<ProgressTransition> progress;
  std::vector<VersionedWrite<SiteRecord>> sites;
  std::vector<VersionedWrite<ChannelRecord>> channels;
  std::vector<VersionedWrite<CharacterHousingRecord>> characters;
  std::vector<VersionedWrite<MovePackageRecord>> packages;
  std::int64_t next_package_id{};

  bool empty() const;
};

// Bounds every persisted record before it is written or after it is read.
void validate_site_contents(const SiteContents &contents);

class HousingStore {
public:
  virtual ~HousingStore() = default;

  virtual HousingSnapshot load(int world_id) = 0;

  // Reads the stored progress of a character that may be offline.
  virtual WorldProgress load_progress(AccountId account_id, CharacterId character_id,
                                      int world_id) = 0;

  // Applies every write and progress transition together. Returns false, changing nothing, when
  // a version or revision does not match.
  virtual bool commit(const HousingCommit &commit) = 0;
};

// Development and test storage. Progress transitions go through the character-state
// repository's revision-checked save; housing rows live in memory. The housing service
// serializes its own commits, so a successful progress save is always followed by the housing
// writes.
class MemoryHousingStore final : public HousingStore {
public:
  explicit MemoryHousingStore(std::shared_ptr<CharacterStateRepository> characters);

  HousingSnapshot load(int world_id) override;

  WorldProgress load_progress(AccountId account_id, CharacterId character_id,
                              int world_id) override;

  bool commit(const HousingCommit &commit) override;

private:
  struct WorldRows {
    std::map<SiteKey, SiteRecord> sites;
    std::map<std::pair<int, int>, ChannelRecord> channels;
    std::map<CharacterId, CharacterHousingRecord> characters;
    std::map<std::int64_t, MovePackageRecord> packages;
    std::int64_t next_package_id{1};
  };

  bool versions_match(const WorldRows &rows, const HousingCommit &commit) const;

  std::shared_ptr<CharacterStateRepository> characters_;
  std::mutex mutex_;
  std::map<int, WorldRows> worlds_;
};

// PostgreSQL storage (schema version 18). Housing rows and progress transitions share one
// transaction.
class PostgresHousingStore final : public HousingStore {
public:
  explicit PostgresHousingStore(std::string connection);

  HousingSnapshot load(int world_id) override;

  WorldProgress load_progress(AccountId account_id, CharacterId character_id,
                              int world_id) override;

  bool commit(const HousingCommit &commit) override;

private:
  std::string connection_;
};

// JSON encodings used by PostgreSQL rows; exposed for round-trip tests.
nlohmann::json site_contents_json(const SiteContents &contents);
SiteContents site_contents_from_json(const nlohmann::json &document);

} // namespace chronicles::housing
