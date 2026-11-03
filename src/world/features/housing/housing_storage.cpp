#include "world/features/housing/housing_state.hpp"

#include "persistence/postgres/character_rows.hpp"
#include "persistence/postgres_repositories.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <pqxx/pqxx>
#include <stdexcept>

namespace chronicles::housing {
namespace {

using Json = nlohmann::json;

// Upper bounds for one persisted site or package. The largest exported preset carries 565
// objects; the catalog object limit is 200 per site, so these bounds are only a safety net.
constexpr std::size_t kMaximumStoredHouses = 1024;
constexpr std::size_t kMaximumStoredDoodads = 8192;
constexpr std::size_t kMaximumStoredCustomIdBytes = 256;
constexpr std::size_t kMaximumStoredResidents = 4096;
constexpr std::size_t kMaximumStoredNameBytes = 128;

constexpr char kHexDigits[] = "0123456789abcdef";

std::string hex_encode(const std::string &bytes) {
  std::string text;
  text.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    const auto value = static_cast<unsigned char>(byte);
    text.push_back(kHexDigits[value >> 4]);
    text.push_back(kHexDigits[value & 0x0F]);
  }
  return text;
}

int hex_value(char digit) {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  throw std::runtime_error("Stored housing customization is not lowercase hexadecimal");
}

std::string hex_decode(const std::string &text) {
  if (text.size() % 2 != 0) {
    throw std::runtime_error("Stored housing customization has an odd length");
  }
  std::string bytes;
  bytes.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    bytes.push_back(static_cast<char>((hex_value(text[index]) << 4) | hex_value(text[index + 1])));
  }
  return bytes;
}

Json position_json(const LocalPosition &position) {
  return Json::array({position.x_mm, position.y_mm, position.z_mm});
}

LocalPosition position_from_json(const Json &value) {
  if (!value.is_array() || value.size() != 3) {
    throw std::runtime_error("Stored housing position must hold three integers");
  }
  return LocalPosition{value.at(0).get<int>(), value.at(1).get<int>(), value.at(2).get<int>()};
}

Json world_position_json(const std::optional<WorldPosition> &position) {
  if (!position) {
    return Json();
  }
  return Json{{"map_id", position->map_id}, {"x_mm", position->x_mm}, {"y_mm", position->y_mm},
              {"z_mm", position->z_mm}, {"yaw", position->yaw}};
}

std::optional<WorldPosition> world_position_from_json(const Json &value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  return WorldPosition{value.at("map_id").get<int>(), value.at("x_mm").get<int>(),
                       value.at("y_mm").get<int>(), value.at("z_mm").get<int>(),
                       value.at("yaw").get<float>()};
}

Json residents_json(const std::map<int, GuildResident> &residents) {
  auto array = Json::array();
  for (const auto &[lot_number, resident] : residents) {
    array.push_back({{"lot", resident.lot_number},
                     {"character_id", resident.character_id},
                     {"name", resident.name},
                     {"cooldown", resident.allow_cooldown_start_ticks}});
  }
  return array;
}

std::map<int, GuildResident> residents_from_json(const Json &value) {
  if (!value.is_array() || value.size() > kMaximumStoredResidents) {
    throw std::runtime_error("Stored guild residents are invalid");
  }
  std::map<int, GuildResident> residents;
  for (const auto &entry : value) {
    GuildResident resident;
    resident.lot_number = entry.at("lot").get<int>();
    resident.character_id = entry.at("character_id").get<CharacterId>();
    resident.name = entry.at("name").get<std::string>();
    resident.allow_cooldown_start_ticks = entry.at("cooldown").get<std::int64_t>();
    if (resident.name.size() > kMaximumStoredNameBytes) {
      throw std::runtime_error("Stored guild resident name is too long");
    }
    residents[resident.lot_number] = resident;
  }
  return residents;
}

Json site_types_json(const std::set<SiteType> &types) {
  auto array = Json::array();
  for (const auto type : types) {
    array.push_back(static_cast<int>(type));
  }
  return array;
}

std::set<SiteType> site_types_from_json(const Json &value) {
  std::set<SiteType> types;
  for (const auto &entry : value) {
    const int type = entry.get<int>();
    if (type < static_cast<int>(SiteType::small) || type > static_cast<int>(SiteType::large)) {
      throw std::runtime_error("Stored housing unlock names an unknown site type");
    }
    types.insert(static_cast<SiteType>(type));
  }
  return types;
}

template <typename Enum> Enum stored_enum(int value, int maximum, const char *name) {
  if (value < 0 || value > maximum) {
    throw std::runtime_error(std::string("Stored housing ") + name + " is out of range");
  }
  return static_cast<Enum>(value);
}

SiteRecord site_from_row(const pqxx::row &row) {
  SiteRecord site;
  site.key = SiteKey{row["zone_key"].as<int>(), row["channel"].as<int>(),
                     row["lot_number"].as<int>()};
  site.version = row["version"].as<std::int64_t>();
  site.authority = stored_enum<AuthorityType>(row["authority"].as<int>(), 2, "authority");
  site.owner_character_id = row["owner_character_id"].as<CharacterId>();
  site.owner_account_id = row["owner_account_id"].as<AccountId>();
  site.owner_name = row["owner_name"].as<std::string>();
  site.guild_id = row["guild_id"].as<GuildId>();
  site.contract_type =
      stored_enum<ContractType>(row["contract_type"].as<int>(), 3, "contract type");
  site.expiry_epoch_ticks = row["expiry_epoch_ticks"].as<std::int64_t>();
  site.contracted_epoch_ticks = row["contracted_epoch_ticks"].as<std::int64_t>();
  if (!row["return_position"].is_null()) {
    site.return_position =
        world_position_from_json(Json::parse(row["return_position"].as<std::string>()));
  }
  site.contents = site_contents_from_json(Json::parse(row["contents"].as<std::string>()));
  site.edit_save_cooldown_start_ticks = row["edit_save_cooldown_start_ticks"].as<std::int64_t>();
  return site;
}

ChannelRecord channel_from_row(const pqxx::row &row) {
  ChannelRecord channel;
  channel.zone_key = row["zone_key"].as<int>();
  channel.channel = row["channel"].as<int>();
  channel.version = row["version"].as<std::int64_t>();
  channel.guild_id = row["guild_id"].as<GuildId>();
  channel.guild_name = row["guild_name"].as<std::string>();
  channel.area_name = row["area_name"].as<std::string>();
  channel.contract_type =
      stored_enum<ContractType>(row["contract_type"].as<int>(), 3, "contract type");
  channel.expiry_epoch_ticks = row["expiry_epoch_ticks"].as<std::int64_t>();
  channel.residents = residents_from_json(Json::parse(row["residents"].as<std::string>()));
  return channel;
}

MovePackageRecord package_from_row(const pqxx::row &row) {
  MovePackageRecord package;
  package.package_id = row["package_id"].as<std::int64_t>();
  package.version = row["version"].as<std::int64_t>();
  package.owner_character_id = row["owner_character_id"].as<CharacterId>();
  package.site_type = stored_enum<SiteType>(row["site_type"].as<int>(), 3, "site type");
  package.created_epoch_ticks = row["created_epoch_ticks"].as<std::int64_t>();
  package.zone_key = row["zone_key"].as<int>();
  package.original_lot_number = row["original_lot_number"].as<int>();
  package.original_channel = row["original_channel"].as<int>();
  package.reason = stored_enum<PackageReason>(row["reason"].as<int>(), 3, "package reason");
  package.contents = site_contents_from_json(Json::parse(row["contents"].as<std::string>()));
  return package;
}

// Each versioned statement must touch exactly one row; anything else means another writer won.
bool touched_one(const pqxx::result &result) {
  return result.affected_rows() == 1;
}

bool write_site(pqxx::work &transaction, int world_id, const VersionedWrite<SiteRecord> &write) {
  const auto &site = write.record;
  if (write.erase) {
    return touched_one(transaction.exec(
        "DELETE FROM emulator_housing_sites WHERE world_id=$1 AND zone_key=$2 AND channel=$3 "
        "AND lot_number=$4 AND version=$5",
        pqxx::params{world_id, site.key.zone_key, site.key.channel, site.key.lot_number,
                     write.expected_version}));
  }

  validate_site_contents(site.contents);
  const auto contents = site_contents_json(site.contents).dump();
  const auto return_position = site.return_position
                                   ? std::optional<std::string>(
                                         world_position_json(site.return_position).dump())
                                   : std::nullopt;
  if (write.expected_version == 0) {
    return touched_one(transaction.exec(
        "INSERT INTO emulator_housing_sites(world_id,zone_key,channel,lot_number,version,"
        "authority,owner_character_id,owner_account_id,owner_name,guild_id,contract_type,"
        "expiry_epoch_ticks,contracted_epoch_ticks,return_position,contents,"
        "edit_save_cooldown_start_ticks) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,"
        "$14::jsonb,$15::jsonb,$16) ON CONFLICT DO NOTHING",
        pqxx::params{world_id, site.key.zone_key, site.key.channel, site.key.lot_number,
                     site.version, static_cast<int>(site.authority), site.owner_character_id,
                     site.owner_account_id, site.owner_name, site.guild_id,
                     static_cast<int>(site.contract_type), site.expiry_epoch_ticks,
                     site.contracted_epoch_ticks, return_position, contents,
                     site.edit_save_cooldown_start_ticks}));
  }

  return touched_one(transaction.exec(
      "UPDATE emulator_housing_sites SET version=$6,authority=$7,owner_character_id=$8,"
      "owner_account_id=$9,owner_name=$10,guild_id=$11,contract_type=$12,"
      "expiry_epoch_ticks=$13,contracted_epoch_ticks=$14,return_position=$15::jsonb,"
      "contents=$16::jsonb,edit_save_cooldown_start_ticks=$17 WHERE world_id=$1 AND "
      "zone_key=$2 AND channel=$3 AND lot_number=$4 AND version=$5",
      pqxx::params{world_id, site.key.zone_key, site.key.channel, site.key.lot_number,
                   write.expected_version, site.version, static_cast<int>(site.authority),
                   site.owner_character_id, site.owner_account_id, site.owner_name,
                   site.guild_id, static_cast<int>(site.contract_type),
                   site.expiry_epoch_ticks, site.contracted_epoch_ticks, return_position,
                   contents, site.edit_save_cooldown_start_ticks}));
}

bool write_channel(pqxx::work &transaction, int world_id,
                   const VersionedWrite<ChannelRecord> &write) {
  const auto &channel = write.record;
  if (write.erase) {
    return touched_one(transaction.exec(
        "DELETE FROM emulator_housing_channels WHERE world_id=$1 AND zone_key=$2 AND "
        "channel=$3 AND version=$4",
        pqxx::params{world_id, channel.zone_key, channel.channel, write.expected_version}));
  }

  const auto residents = residents_json(channel.residents).dump();
  if (write.expected_version == 0) {
    return touched_one(transaction.exec(
        "INSERT INTO emulator_housing_channels(world_id,zone_key,channel,version,guild_id,"
        "guild_name,area_name,contract_type,expiry_epoch_ticks,residents) VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10::jsonb) ON CONFLICT DO NOTHING",
        pqxx::params{world_id, channel.zone_key, channel.channel, channel.version,
                     channel.guild_id, channel.guild_name, channel.area_name,
                     static_cast<int>(channel.contract_type), channel.expiry_epoch_ticks,
                     residents}));
  }

  return touched_one(transaction.exec(
      "UPDATE emulator_housing_channels SET version=$5,guild_id=$6,guild_name=$7,area_name=$8,"
      "contract_type=$9,expiry_epoch_ticks=$10,residents=$11::jsonb WHERE world_id=$1 AND "
      "zone_key=$2 AND channel=$3 AND version=$4",
      pqxx::params{world_id, channel.zone_key, channel.channel, write.expected_version,
                   channel.version, channel.guild_id, channel.guild_name, channel.area_name,
                   static_cast<int>(channel.contract_type), channel.expiry_epoch_ticks,
                   residents}));
}

bool write_character(pqxx::work &transaction, int world_id,
                     const VersionedWrite<CharacterHousingRecord> &write) {
  const auto &character = write.record;
  if (write.erase) {
    return touched_one(transaction.exec(
        "DELETE FROM emulator_housing_characters WHERE world_id=$1 AND character_id=$2 AND "
        "version=$3",
        pqxx::params{world_id, character.character_id, write.expected_version}));
  }

  const auto unlocked = site_types_json(character.unlocked_site_types).dump();
  if (write.expected_version == 0) {
    return touched_one(transaction.exec(
        "INSERT INTO emulator_housing_characters(world_id,character_id,version,"
        "unlocked_site_types,guild_unlocked) VALUES ($1,$2,$3,$4::jsonb,$5) "
        "ON CONFLICT DO NOTHING",
        pqxx::params{world_id, character.character_id, character.version, unlocked,
                     character.guild_unlocked}));
  }

  return touched_one(transaction.exec(
      "UPDATE emulator_housing_characters SET version=$4,unlocked_site_types=$5::jsonb,"
      "guild_unlocked=$6 WHERE world_id=$1 AND character_id=$2 AND version=$3",
      pqxx::params{world_id, character.character_id, write.expected_version, character.version,
                   unlocked, character.guild_unlocked}));
}

bool write_package(pqxx::work &transaction, int world_id,
                   const VersionedWrite<MovePackageRecord> &write) {
  const auto &package = write.record;
  if (write.erase) {
    return touched_one(transaction.exec(
        "DELETE FROM emulator_housing_packages WHERE world_id=$1 AND package_id=$2 AND "
        "version=$3",
        pqxx::params{world_id, package.package_id, write.expected_version}));
  }
  if (write.expected_version != 0) {
    throw std::invalid_argument("Housing move packages are created or deleted, never changed");
  }

  validate_site_contents(package.contents);
  return touched_one(transaction.exec(
      "INSERT INTO emulator_housing_packages(world_id,package_id,version,owner_character_id,"
      "site_type,created_epoch_ticks,zone_key,original_lot_number,original_channel,reason,"
      "contents) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11::jsonb) ON CONFLICT DO NOTHING",
      pqxx::params{world_id, package.package_id, package.version, package.owner_character_id,
                   static_cast<int>(package.site_type), package.created_epoch_ticks,
                   package.zone_key, package.original_lot_number, package.original_channel,
                   static_cast<int>(package.reason),
                   site_contents_json(package.contents).dump()}));
}

template <typename Key, typename Record>
bool version_matches(const std::map<Key, Record> &rows, const Key &key,
                     const VersionedWrite<Record> &write) {
  const auto found = rows.find(key);
  if (write.expected_version == 0) {
    return found == rows.end() && !write.erase;
  }
  return found != rows.end() && found->second.version == write.expected_version;
}

} // namespace

bool HousingCommit::empty() const {
  return progress.empty() && sites.empty() && channels.empty() && characters.empty() &&
         packages.empty();
}

void validate_site_contents(const SiteContents &contents) {
  if (contents.house_part_custom_ids.size() > kMaximumStoredCustomIdBytes ||
      contents.houses.size() > kMaximumStoredHouses ||
      contents.doodads.size() > kMaximumStoredDoodads) {
    throw std::invalid_argument("Housing site contents exceed storage limits");
  }

  for (const auto &[house_id, house] : contents.houses) {
    if (house_id == 0 || house_id > kMaximumObjectId || house.house_id != house_id ||
        house.housing_id <= 0 || !std::isfinite(house.yaw)) {
      throw std::invalid_argument("Invalid housing building record");
    }
  }
  for (const auto &[key, doodad] : contents.doodads) {
    if (key.second == 0 || key.second > kMaximumObjectId || key.first > kMaximumObjectId ||
        doodad.house_id != key.first || doodad.doodad_id != key.second || doodad.cms_id <= 0 ||
        !std::isfinite(doodad.pitch) || !std::isfinite(doodad.yaw) ||
        !std::isfinite(doodad.roll)) {
      throw std::invalid_argument("Invalid housing object record");
    }
  }
}

Json site_contents_json(const SiteContents &contents) {
  auto houses = Json::array();
  for (const auto &[house_id, house] : contents.houses) {
    houses.push_back({{"id", house.house_id},
                      {"attach", house.attach_index},
                      {"cms", house.housing_id},
                      {"pos", position_json(house.position)},
                      {"yaw", house.yaw},
                      {"parts", house.parts_state},
                      {"owned", house.from_storage}});
  }

  auto doodads = Json::array();
  for (const auto &[key, doodad] : contents.doodads) {
    doodads.push_back({{"id", doodad.doodad_id},
                       {"house", doodad.house_id},
                       {"cms", doodad.cms_id},
                       {"phase", doodad.phase},
                       {"phase_tick", doodad.phase_begin_tick},
                       {"pos", position_json(doodad.position)},
                       {"rot", Json::array({doodad.pitch, doodad.yaw, doodad.roll})},
                       {"interactions", doodad.interaction_count},
                       {"parent", doodad.parent_doodad_id},
                       {"owned", doodad.from_storage}});
  }

  return Json{{"custom_ids", hex_encode(contents.house_part_custom_ids)},
              {"houses", std::move(houses)},
              {"doodads", std::move(doodads)}};
}

SiteContents site_contents_from_json(const Json &document) {
  SiteContents contents;
  contents.house_part_custom_ids = hex_decode(document.at("custom_ids").get<std::string>());

  const auto &houses = document.at("houses");
  const auto &doodads = document.at("doodads");
  if (!houses.is_array() || !doodads.is_array() || houses.size() > kMaximumStoredHouses ||
      doodads.size() > kMaximumStoredDoodads) {
    throw std::runtime_error("Stored housing contents are invalid");
  }

  for (const auto &entry : houses) {
    HouseRecord house;
    house.house_id = entry.at("id").get<std::uint32_t>();
    house.attach_index = entry.at("attach").get<int>();
    house.housing_id = entry.at("cms").get<int>();
    house.position = position_from_json(entry.at("pos"));
    house.yaw = entry.at("yaw").get<float>();
    house.parts_state = entry.at("parts").get<std::uint64_t>();
    house.from_storage = entry.at("owned").get<bool>();
    if (!contents.houses.emplace(house.house_id, house).second) {
      throw std::runtime_error("Stored housing contents repeat a building");
    }
  }

  for (const auto &entry : doodads) {
    DoodadRecord doodad;
    doodad.doodad_id = entry.at("id").get<std::uint32_t>();
    doodad.house_id = entry.at("house").get<std::uint32_t>();
    doodad.cms_id = entry.at("cms").get<int>();
    doodad.phase = entry.at("phase").get<int>();
    doodad.phase_begin_tick = entry.at("phase_tick").get<std::int64_t>();
    doodad.position = position_from_json(entry.at("pos"));
    const auto &rotation = entry.at("rot");
    if (!rotation.is_array() || rotation.size() != 3) {
      throw std::runtime_error("Stored housing object rotation is invalid");
    }
    doodad.pitch = rotation.at(0).get<float>();
    doodad.yaw = rotation.at(1).get<float>();
    doodad.roll = rotation.at(2).get<float>();
    doodad.interaction_count = entry.at("interactions").get<int>();
    doodad.parent_doodad_id = entry.at("parent").get<std::uint32_t>();
    doodad.from_storage = entry.at("owned").get<bool>();
    if (!contents.doodads.emplace(DoodadKey{doodad.house_id, doodad.doodad_id}, doodad).second) {
      throw std::runtime_error("Stored housing contents repeat an object");
    }
  }

  validate_site_contents(contents);
  return contents;
}

MemoryHousingStore::MemoryHousingStore(std::shared_ptr<CharacterStateRepository> characters)
    : characters_(std::move(characters)) {
  if (!characters_) {
    throw std::invalid_argument("Memory housing storage needs the character-state repository");
  }
}

HousingSnapshot MemoryHousingStore::load(int world_id) {
  std::lock_guard lock(mutex_);
  HousingSnapshot snapshot;
  const auto found = worlds_.find(world_id);
  if (found == worlds_.end()) {
    return snapshot;
  }

  const auto &rows = found->second;
  for (const auto &[key, site] : rows.sites) {
    snapshot.sites.push_back(site);
  }
  for (const auto &[key, channel] : rows.channels) {
    snapshot.channels.push_back(channel);
  }
  for (const auto &[character_id, character] : rows.characters) {
    snapshot.characters.push_back(character);
  }
  for (const auto &[package_id, package] : rows.packages) {
    snapshot.packages.push_back(package);
  }
  snapshot.next_package_id = rows.next_package_id;
  return snapshot;
}

WorldProgress MemoryHousingStore::load_progress(AccountId account_id, CharacterId character_id,
                                                int world_id) {
  return characters_->load_world_progress(account_id, character_id, world_id);
}

bool MemoryHousingStore::versions_match(const WorldRows &rows,
                                        const HousingCommit &commit) const {
  for (const auto &write : commit.sites) {
    if (!version_matches(rows.sites, write.record.key, write)) {
      return false;
    }
  }
  for (const auto &write : commit.channels) {
    if (!version_matches(rows.channels, std::pair{write.record.zone_key, write.record.channel},
                         write)) {
      return false;
    }
  }
  for (const auto &write : commit.characters) {
    if (!version_matches(rows.characters, write.record.character_id, write)) {
      return false;
    }
  }
  for (const auto &write : commit.packages) {
    if (!version_matches(rows.packages, write.record.package_id, write)) {
      return false;
    }
  }
  return true;
}

bool MemoryHousingStore::commit(const HousingCommit &commit) {
  for (const auto &write : commit.sites) {
    validate_site_contents(write.record.contents);
  }

  std::lock_guard lock(mutex_);
  auto &rows = worlds_[commit.world_id];
  if (!versions_match(rows, commit)) {
    return false;
  }

  // Every transition is checked before any is saved, so a stale revision changes nothing.
  for (const auto &transition : commit.progress) {
    detail::validate_world_progress_transition(transition.before, transition.after);
    const auto stored = characters_->load_world_progress(
        transition.account_id, transition.character_id, transition.world_id);
    if (stored.revision != transition.before.revision) {
      return false;
    }
  }
  for (const auto &transition : commit.progress) {
    if (!characters_->save_world_progress(transition.account_id, transition.character_id,
                                          transition.world_id, transition.before,
                                          transition.after)) {
      throw std::runtime_error("Housing progress changed during a serialized commit");
    }
  }

  for (const auto &write : commit.sites) {
    if (write.erase) {
      rows.sites.erase(write.record.key);
    } else {
      rows.sites[write.record.key] = write.record;
    }
  }
  for (const auto &write : commit.channels) {
    const auto key = std::pair{write.record.zone_key, write.record.channel};
    if (write.erase) {
      rows.channels.erase(key);
    } else {
      rows.channels[key] = write.record;
    }
  }
  for (const auto &write : commit.characters) {
    if (write.erase) {
      rows.characters.erase(write.record.character_id);
    } else {
      rows.characters[write.record.character_id] = write.record;
    }
  }
  for (const auto &write : commit.packages) {
    if (write.erase) {
      rows.packages.erase(write.record.package_id);
    } else {
      rows.packages[write.record.package_id] = write.record;
    }
  }
  rows.next_package_id = std::max(rows.next_package_id, commit.next_package_id);
  return true;
}

PostgresHousingStore::PostgresHousingStore(std::string connection)
    : connection_(std::move(connection)) {}

HousingSnapshot PostgresHousingStore::load(int world_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  transaction.exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");

  HousingSnapshot snapshot;
  for (const auto &row :
       transaction.exec("SELECT zone_key,channel,lot_number,version,authority,"
                        "owner_character_id,owner_account_id,owner_name,guild_id,contract_type,"
                        "expiry_epoch_ticks,contracted_epoch_ticks,return_position::text AS "
                        "return_position,contents::text AS contents,"
                        "edit_save_cooldown_start_ticks FROM emulator_housing_sites "
                        "WHERE world_id=$1",
                        pqxx::params{world_id})) {
    snapshot.sites.push_back(site_from_row(row));
  }
  for (const auto &row : transaction.exec(
           "SELECT zone_key,channel,version,guild_id,guild_name,area_name,contract_type,"
           "expiry_epoch_ticks,residents::text AS residents FROM emulator_housing_channels "
           "WHERE world_id=$1",
           pqxx::params{world_id})) {
    snapshot.channels.push_back(channel_from_row(row));
  }
  for (const auto &row : transaction.exec(
           "SELECT character_id,version,unlocked_site_types::text AS unlocked_site_types,"
           "guild_unlocked FROM emulator_housing_characters WHERE world_id=$1",
           pqxx::params{world_id})) {
    CharacterHousingRecord character;
    character.character_id = row["character_id"].as<CharacterId>();
    character.version = row["version"].as<std::int64_t>();
    character.unlocked_site_types =
        site_types_from_json(Json::parse(row["unlocked_site_types"].as<std::string>()));
    character.guild_unlocked = row["guild_unlocked"].as<bool>();
    snapshot.characters.push_back(character);
  }
  for (const auto &row : transaction.exec(
           "SELECT package_id,version,owner_character_id,site_type,created_epoch_ticks,"
           "zone_key,original_lot_number,original_channel,reason,contents::text AS contents "
           "FROM emulator_housing_packages WHERE world_id=$1",
           pqxx::params{world_id})) {
    snapshot.packages.push_back(package_from_row(row));
  }
  const auto next = transaction.exec(
      "SELECT next_package_id FROM emulator_housing_worlds WHERE world_id=$1",
      pqxx::params{world_id});
  if (!next.empty()) {
    snapshot.next_package_id = next.front()[0].as<std::int64_t>();
  }
  for (const auto &package : snapshot.packages) {
    snapshot.next_package_id = std::max(snapshot.next_package_id, package.package_id + 1);
  }
  return snapshot;
}

WorldProgress PostgresHousingStore::load_progress(AccountId account_id, CharacterId character_id,
                                                  int world_id) {
  PostgresCharacterStateRepository characters(connection_);
  return characters.load_world_progress(account_id, character_id, world_id);
}

bool PostgresHousingStore::commit(const HousingCommit &commit) {
  for (const auto &transition : commit.progress) {
    detail::validate_world_progress_transition(transition.before, transition.after);
  }

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  // Returning before commit() rolls back every statement of this transaction.
  for (const auto &transition : commit.progress) {
    if (!detail::write_world_progress_transition(transaction, transition.account_id,
                                                 transition.character_id, transition.world_id,
                                                 transition.before, transition.after)) {
      return false;
    }
  }
  for (const auto &write : commit.sites) {
    if (!write_site(transaction, commit.world_id, write)) {
      return false;
    }
  }
  for (const auto &write : commit.channels) {
    if (!write_channel(transaction, commit.world_id, write)) {
      return false;
    }
  }
  for (const auto &write : commit.characters) {
    if (!write_character(transaction, commit.world_id, write)) {
      return false;
    }
  }
  for (const auto &write : commit.packages) {
    if (!write_package(transaction, commit.world_id, write)) {
      return false;
    }
  }
  transaction.exec(
      "INSERT INTO emulator_housing_worlds(world_id,next_package_id) VALUES ($1,$2) "
      "ON CONFLICT (world_id) DO UPDATE SET next_package_id="
      "GREATEST(emulator_housing_worlds.next_package_id,EXCLUDED.next_package_id)",
      // Zero means this mutation did not allocate a package. PostgreSQL checks the
      // proposed insert before ON CONFLICT, so keep that row valid as well as the update.
      pqxx::params{commit.world_id, std::max<std::int64_t>(1, commit.next_package_id)});
  transaction.commit();
  return true;
}

} // namespace chronicles::housing
