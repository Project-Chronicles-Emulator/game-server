#include "world/features/housing/housing_service.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace chronicles::housing {
namespace {

constexpr int kFirstChannel = kPrimaryPersonalChannel;
// Opaque client customization bytes (HousingNub.house_part_custom_ids). The preset carries six
// integers; this bound keeps a hostile client from storing large blobs.
constexpr std::size_t kMaximumCustomIdBytes = 256;
constexpr std::size_t kMaximumAreaNameBytes = 64;
constexpr std::uint32_t kMaximumPartAttachIndex = 63;
// Candidate spawn height above the site floor so the visitor does not start inside terrain.
constexpr int kSpawnHeightOffsetMillimetres = 1000;

bool is_success(ErrorCode code) {
  return code == x5::ec::Success;
}

std::string ascii_lower_copy(std::string text) {
  for (auto &character : text) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return text;
}

WorldPosition world_position_mm(int map_id, double x_cm, double y_cm, double z_cm,
                                int z_offset_mm, float yaw) {
  return WorldPosition{map_id, static_cast<int>(std::lround(x_cm * 10.0)),
                       static_cast<int>(std::lround(y_cm * 10.0)),
                       static_cast<int>(std::lround(z_cm * 10.0)) + z_offset_mm, yaw};
}

// Housing storage arithmetic on a progress copy. Keys are never erased: storage transitions
// preserve every row (validate_transition in world_storage.cpp).
class StorageLedger {
public:
  explicit StorageLedger(WorldProgress &progress) : progress_(progress) {}

  int available(int item_cms_id) const {
    const auto found = progress_.housing_items.find(item_cms_id);
    if (found == progress_.housing_items.end()) {
      return 0;
    }
    return found->second.quantity - found->second.installed_count;
  }

  bool install(int item_cms_id) {
    if (available(item_cms_id) < 1) {
      return false;
    }
    ++progress_.housing_items[item_cms_id].installed_count;
    return true;
  }

  // An installed object returns to storage (it stays owned).
  void uninstall(int item_cms_id) {
    auto &record = progress_.housing_items[item_cms_id];
    if (record.installed_count > 0) {
      --record.installed_count;
    }
  }

  // An installed object leaves storage entirely (packed into a move package or replaced by an
  // upgrade).
  void remove_installed(int item_cms_id) {
    auto &record = progress_.housing_items[item_cms_id];
    if (record.installed_count > 0) {
      --record.installed_count;
    }
    if (record.quantity > 0) {
      --record.quantity;
    }
  }

  // A packaged or upgraded object becomes owned storage again.
  bool add(int item_cms_id, int count, bool installed) {
    if (count <= 0) {
      return true;
    }
    if (!progress_.housing_items.contains(item_cms_id) &&
        progress_.housing_items.size() >= kMaximumHousingItemKinds) {
      return false;
    }

    auto &record = progress_.housing_items[item_cms_id];
    if (record.quantity > std::numeric_limits<int>::max() - count) {
      return false;
    }
    record.quantity += count;
    if (installed) {
      record.installed_count += count;
    }
    return true;
  }

  bool discard(int item_cms_id, int count) {
    if (count <= 0 || available(item_cms_id) < count) {
      return false;
    }
    progress_.housing_items[item_cms_id].quantity -= count;
    return true;
  }

private:
  WorldProgress &progress_;
};

std::optional<int> house_item(const HousingCatalog &catalog, int housing_id) {
  const auto *definition = catalog.housing(housing_id);
  if (!definition) {
    return std::nullopt;
  }
  return definition->item_cms_id;
}

std::size_t object_count(const SiteContents &contents) {
  return contents.houses.size() + contents.doodads.size();
}

bool has_main_building(const HousingCatalog &catalog, const SiteContents &contents) {
  return std::ranges::any_of(contents.houses, [&](const auto &entry) {
    const auto *definition = catalog.housing(entry.second.housing_id);
    return definition && definition->category == HousingCategory::main_building;
  });
}

// Site-local bounds: half the lot size on each horizontal axis and the design height.
bool within_site(const SiteDesign &design, const LocalPosition &position) {
  const double half_x_mm = design.size_x_cm * 10.0 / 2.0;
  const double half_y_mm = design.size_y_cm * 10.0 / 2.0;
  const double height_mm = design.max_height_cm * 10.0;
  return std::abs(position.x_mm) <= half_x_mm && std::abs(position.y_mm) <= half_y_mm &&
         std::abs(position.z_mm) <= height_mm;
}

bool finite_rotation(float pitch, float yaw, float roll) {
  return std::isfinite(pitch) && std::isfinite(yaw) && std::isfinite(roll);
}

// Adds the doodads a building model carries (for example a facility's crafting station).
void add_model_add_ons(const HousingCatalog &catalog, const HouseRecord &house,
                       SiteContents &contents, std::vector<DoodadRecord> *added) {
  const auto *definition = catalog.housing(house.housing_id);
  if (!definition) {
    return;
  }
  const auto *add_ons = catalog.model_add_ons(definition->housing_unit_model_id);
  if (!add_ons) {
    return;
  }

  for (const auto &add_on : *add_ons) {
    DoodadRecord doodad;
    doodad.doodad_id = add_on.doodad_id;
    doodad.house_id = house.house_id;
    doodad.cms_id = add_on.doodad_cms_id;
    doodad.phase = 1;
    doodad.position = add_on.position;
    doodad.yaw = add_on.yaw;
    doodad.from_storage = false;
    const auto [entry, inserted] =
        contents.doodads.try_emplace(DoodadKey{house.house_id, add_on.doodad_id}, doodad);
    if (inserted && added) {
      added->push_back(entry->second);
    }
  }
}

void remove_house_doodads(SiteContents &contents, std::uint32_t house_id) {
  std::erase_if(contents.doodads,
                [house_id](const auto &entry) { return entry.first.first == house_id; });
}

} // namespace

std::optional<GuildMembership> NoGuildDirectory::membership(CharacterId) const {
  return std::nullopt;
}

std::vector<GuildMember> NoGuildDirectory::members(GuildId) const {
  return {};
}

std::optional<bool> GuildDirectory::guild_exists(GuildId) const {
  return std::nullopt;
}

void HousingOutcome::append(HousingOutcome other) {
  const auto move_all = [](auto &target, auto &source) {
    target.insert(target.end(), std::make_move_iterator(source.begin()),
                  std::make_move_iterator(source.end()));
  };
  move_all(sites_added, other.sites_added);
  move_all(contracts_changed, other.contracts_changed);
  move_all(sites_removed, other.sites_removed);
  move_all(site_updates, other.site_updates);
  move_all(parts_updates, other.parts_updates);
  move_all(owner_sites_changed, other.owner_sites_changed);
  move_all(progress_changes, other.progress_changes);
  move_all(packages_created, other.packages_created);
  move_all(guild_sites, other.guild_sites);
  move_all(guild_areas, other.guild_areas);
  move_all(doodad_phases, other.doodad_phases);
}

bool HousingOutcome::empty() const {
  return sites_added.empty() && contracts_changed.empty() && sites_removed.empty() &&
         site_updates.empty() && parts_updates.empty() && owner_sites_changed.empty() &&
         progress_changes.empty() && packages_created.empty() && guild_sites.empty() &&
         guild_areas.empty() && doodad_phases.empty();
}

HousingService::HousingService(std::shared_ptr<const HousingCatalog> catalog,
                               std::shared_ptr<HousingStore> store, int world_id,
                               Clock now_epoch_ticks, HousingPolicy policy,
                               std::shared_ptr<const GuildDirectory> guilds)
    : catalog_(std::move(catalog)), store_(std::move(store)), world_id_(world_id),
      now_(std::move(now_epoch_ticks)), policy_(policy), guilds_(std::move(guilds)) {
  if (!catalog_ || !store_ || !now_) {
    throw std::invalid_argument("Housing service needs a catalog, a store and a clock");
  }
  if (!guilds_) {
    guilds_ = std::make_shared<NoGuildDirectory>();
  }

  auto snapshot = store_->load(world_id_);
  for (auto &site : snapshot.sites) {
    validate_site_contents(site.contents);
    sites_.emplace(site.key, std::move(site));
  }
  for (auto &channel : snapshot.channels) {
    channels_.emplace(std::pair{channel.zone_key, channel.channel}, std::move(channel));
  }
  for (auto &character : snapshot.characters) {
    characters_.emplace(character.character_id, std::move(character));
  }
  for (auto &package : snapshot.packages) {
    validate_site_contents(package.contents);
    packages_.emplace(package.package_id, std::move(package));
  }
  next_package_id_ = std::max<std::int64_t>(snapshot.next_package_id, 1);
  spdlog::info("Housing service world={} sites={} channels={} packages={}", world_id_,
               sites_.size(), channels_.size(), packages_.size());
}

const HousingCatalog &HousingService::catalog() const {
  return *catalog_;
}

const GuildDirectory &HousingService::guilds() const {
  return *guilds_;
}

int HousingService::world_id() const {
  return world_id_;
}

WorldProgress HousingService::load_progress_locked(const HousingActor &actor,
                                                   const ProgressAccess *access) const {
  if (access && access->load && access->character_id == actor.character_id) {
    return access->load();
  }
  return store_->load_progress(actor.account_id, actor.character_id, actor.world_id);
}

bool HousingService::commit_locked(HousingCommit commit, const ProgressAccess *access) {
  commit.world_id = world_id_;
  commit.next_package_id = next_package_id_;
  if (!store_->commit(commit)) {
    return false;
  }

  for (const auto &write : commit.sites) {
    if (write.erase) {
      sites_.erase(write.record.key);
    } else {
      sites_[write.record.key] = write.record;
    }
  }
  for (const auto &write : commit.channels) {
    const auto key = std::pair{write.record.zone_key, write.record.channel};
    if (write.erase) {
      channels_.erase(key);
    } else {
      channels_[key] = write.record;
    }
  }
  for (const auto &write : commit.characters) {
    if (write.erase) {
      characters_.erase(write.record.character_id);
    } else {
      characters_[write.record.character_id] = write.record;
    }
  }
  for (const auto &write : commit.packages) {
    if (write.erase) {
      packages_.erase(write.record.package_id);
    } else {
      packages_[write.record.package_id] = write.record;
    }
  }

  // The caller's own session caches its progress revision; adopt the committed state there.
  // Other characters' sessions adopt through the outcome's progress notices.
  if (access && access->adopt) {
    for (const auto &transition : commit.progress) {
      if (transition.character_id == access->character_id) {
        access->adopt(transition.after);
      }
    }
  }
  return true;
}

const SiteRecord *HousingService::personal_site_locked(CharacterId character_id) const {
  for (const auto &[key, site] : sites_) {
    if (site.authority == AuthorityType::individual && site.owner_character_id == character_id) {
      return &site;
    }
  }
  return nullptr;
}

std::int64_t HousingService::rent_duration_ticks(AuthorityType authority) const {
  const auto &rules = catalog_->rules();
  const int days = authority == AuthorityType::guild ? rules.area_rent_duration_days
                                                     : rules.site_rent_duration_days;
  return static_cast<std::int64_t>(days) * kEpochTicksPerDay;
}

ErrorCode HousingService::charge_unlock(const RentCost &cost, WorldProgress &progress) const {
  if (cost.unlock_value <= 0) {
    return x5::ec::Success;
  }

  switch (cost.unlock_money) {
  case MoneyKind::hersander_token:
    if (progress.hersander_token < cost.unlock_value) {
      return x5::ec::MoneyNotEnough;
    }
    progress.hersander_token -= cost.unlock_value;
    return x5::ec::Success;
  case MoneyKind::gold:
    if (progress.gold < cost.unlock_value) {
      return x5::ec::MoneyNotEnough;
    }
    progress.gold -= cost.unlock_value;
    return x5::ec::Success;
  case MoneyKind::none:
    break;
  }
  return x5::ec::Housing_RentCostNotFound;
}

ErrorCode HousingService::charge_rent(const RentCost &cost, WorldProgress &progress) const {
  if (cost.rent_item_count <= 0) {
    return x5::ec::Success;
  }

  const auto held = progress.stackables.find(cost.rent_item_cms_id);
  if (held == progress.stackables.end() || held->second < cost.rent_item_count) {
    return x5::ec::Housing_RentItemNotEnough;
  }
  held->second -= cost.rent_item_count;
  return x5::ec::Success;
}

int HousingService::open_channel_count_locked(int zone_key, bool guild) const {
  int count = 0;
  for (const auto &[key, channel] : channels_) {
    if (key.first == zone_key && (channel.guild_id != 0) == guild) {
      ++count;
    }
  }
  return count;
}

std::optional<SiteKey> HousingService::find_free_lot_locked(int zone_key, SiteType type) const {
  const auto lots = catalog_->sites_in_zone(zone_key);
  std::vector<int> personal_channels;
  for (const auto &[key, channel] : channels_) {
    if (key.first == zone_key && channel.guild_id == 0) {
      personal_channels.push_back(key.second);
    }
  }
  if (personal_channels.empty()) {
    personal_channels.push_back(kFirstChannel);
  }

  for (const int channel : personal_channels) {
    for (const auto *site : lots) {
      if (site->type != type) {
        continue;
      }
      const SiteKey key{zone_key, channel, site->lot_number};
      if (!sites_.contains(key)) {
        return key;
      }
    }
  }

  // Open the next channel when every personal channel is full and the policy allows it.
  const int open_channels = static_cast<int>(personal_channels.size());
  if (open_channels >= policy_.personal_channel_open_limit) {
    return std::nullopt;
  }

  int next_channel = kFirstChannel;
  while (channels_.contains({zone_key, next_channel})) {
    ++next_channel;
  }
  for (const auto *site : lots) {
    if (site->type == type) {
      return SiteKey{zone_key, next_channel, site->lot_number};
    }
  }
  return std::nullopt;
}

SiteContents HousingService::preset_contents(const SiteDefinition &definition) const {
  SiteContents contents;
  const auto *preset = catalog_->preset(definition.preset_id);
  if (!preset) {
    return contents;
  }

  for (const auto &house : preset->houses) {
    HouseRecord record;
    record.house_id = house.house_id;
    record.attach_index = house.attach_index;
    record.housing_id = house.housing_id;
    record.position = house.position;
    record.yaw = house.yaw;
    record.from_storage = false;
    contents.houses.emplace(record.house_id, record);
  }
  for (const auto &doodad : preset->doodads) {
    DoodadRecord record;
    record.doodad_id = doodad.doodad_id;
    record.house_id = doodad.house_id;
    record.cms_id = doodad.doodad_cms_id;
    record.phase = doodad.phase;
    record.position = doodad.position;
    record.yaw = doodad.yaw;
    record.from_storage = false;
    contents.doodads.emplace(DoodadKey{record.house_id, record.doodad_id}, record);
  }
  for (const auto &[house_id, house] : contents.houses) {
    add_model_add_ons(*catalog_, house, contents, nullptr);
  }
  return contents;
}

SiteRecord HousingService::new_site_locked(const HousingActor &actor, const SiteKey &key,
                                           const SiteDefinition &definition,
                                           ContractType contract_type, std::int64_t now) const {
  SiteRecord site;
  site.key = key;
  site.authority = AuthorityType::individual;
  site.owner_character_id = actor.character_id;
  site.owner_account_id = actor.account_id;
  site.owner_name = actor.name;
  site.contract_type = contract_type;
  site.contracted_epoch_ticks = now;
  site.expiry_epoch_ticks = now + rent_duration_ticks(AuthorityType::individual);
  site.return_position = actor.position;
  site.contents = preset_contents(definition);
  return site;
}

UnlockInfo HousingService::unlock_info(const HousingActor &actor, int zone_key) {
  std::lock_guard lock(mutex_);
  UnlockInfo info;
  const auto *zone = catalog_->zone(zone_key);
  if (!zone || !zone->map_id) {
    info.ec = x5::ec::Housing_InvalidZoneKey;
    return info;
  }

  const auto record = characters_.find(actor.character_id);
  std::set<SiteType> available_types;
  for (const auto *site : catalog_->sites_in_zone(zone_key)) {
    available_types.insert(site->type);
  }

  const bool owns_site = personal_site_locked(actor.character_id) != nullptr;
  for (const auto type : available_types) {
    const auto *cost = catalog_->rent_cost(AuthorityType::individual, type);
    if (!cost) {
      continue;
    }

    const bool unlocked =
        cost->unlock_value == 0 ||
        (record != characters_.end() && record->second.unlocked_site_types.contains(type));
    if (unlocked) {
      info.unlocked_site_types.push_back(type);
    }
    if (!owns_site) {
      info.rentable_site_types.push_back(type);
    }
  }

  const auto membership = guilds_->membership(actor.character_id);
  info.guild_unlocked = record != characters_.end() && record->second.guild_unlocked;
  info.guild_rentable = membership && membership->grade == GuildGrade::master &&
                        !guild_area_locked(membership->guild_id).has_value();
  info.personal_channel_open_limit = policy_.personal_channel_open_limit;
  info.guild_channel_open_limit = policy_.guild_channel_open_limit;
  info.region_rent_locked = owns_site;
  info.personal_site_rent_limit_per_region = policy_.personal_site_rent_limit_per_region;
  return info;
}

ContractResult HousingService::contract(const HousingActor &actor, SiteType type, int zone_key,
                                        ContractType contract_type,
                                        const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  ContractResult result;
  const auto *zone = catalog_->zone(zone_key);
  if (!zone || !zone->map_id) {
    result.ec = x5::ec::Housing_InvalidZoneKey;
    return result;
  }
  if (type == SiteType::none || !catalog_->design(type)) {
    result.ec = x5::ec::Housing_InvalidArea;
    return result;
  }
  if (contract_type != ContractType::monthly && contract_type != ContractType::periodic) {
    result.ec = x5::ec::Housing_InvalidContractType;
    return result;
  }
  if (personal_site_locked(actor.character_id)) {
    result.ec = x5::ec::Housing_CanHaveOnlyOneHousingSite;
    return result;
  }
  // GameVariable GeneralHousingSite{S,M,L}positionLv, the personal counterpart of the guild
  // lot rule. Candidate: the server checks the character's current company position.
  const auto required_position = catalog_->rules().general_position_level.find(type);
  if (actor.company_position_level &&
      required_position != catalog_->rules().general_position_level.end() &&
      *actor.company_position_level < required_position->second) {
    result.ec = x5::ec::Housing_InsufficientCompanyPosition;
    return result;
  }

  const auto *cost = catalog_->rent_cost(AuthorityType::individual, type);
  if (!cost) {
    result.ec = x5::ec::Housing_RentCostNotFound;
    return result;
  }

  const auto lot = find_free_lot_locked(zone_key, type);
  if (!lot) {
    const bool channels_full =
        open_channel_count_locked(zone_key, false) >= policy_.personal_channel_open_limit;
    result.ec = channels_full ? x5::ec::Housing_ChannelLimitReached
                              : x5::ec::Housing_NotFoundEmptyHousingSite;
    return result;
  }

  const auto before = load_progress_locked(actor, access);
  auto after = before;
  auto character = characters_.contains(actor.character_id)
                       ? characters_.at(actor.character_id)
                       : CharacterHousingRecord{actor.character_id};
  const auto previous_character_version = character.version;
  const bool needs_unlock = cost->unlock_value > 0 && !character.unlocked_site_types.contains(type);
  if (needs_unlock) {
    const auto unlock_error = charge_unlock(*cost, after);
    if (!is_success(unlock_error)) {
      result.ec = unlock_error;
      return result;
    }
    character.unlocked_site_types.insert(type);
  }

  const auto rent_error = charge_rent(*cost, after);
  if (!is_success(rent_error)) {
    result.ec = rent_error;
    return result;
  }
  after.revision = before.revision + 1;

  const auto now = now_();
  const auto *definition = catalog_->site(lot->lot_number);
  auto site = new_site_locked(actor, *lot, *definition, contract_type, now);
  site.version = 1;

  HousingCommit commit;
  commit.progress.push_back({actor.account_id, actor.character_id, actor.world_id, before, after});
  commit.sites.push_back({site, 0, false});
  if (!channels_.contains({lot->zone_key, lot->channel})) {
    ChannelRecord channel;
    channel.zone_key = lot->zone_key;
    channel.channel = lot->channel;
    channel.version = 1;
    commit.channels.push_back({channel, 0, false});
  }
  if (needs_unlock) {
    character.version = previous_character_version + 1;
    commit.characters.push_back({character, previous_character_version, false});
  }

  if (!commit_locked(std::move(commit), access)) {
    result.ec = x5::ec::Housing_RetryContract;
    return result;
  }

  result.lot_number = lot->lot_number;
  result.channel = lot->channel;
  result.expiry_epoch_ticks = site.expiry_epoch_ticks;
  result.contract_type = contract_type;
  result.outcome.sites_added.push_back(*lot);
  result.outcome.owner_sites_changed.push_back(actor.character_id);
  result.outcome.progress_changes.push_back({actor.character_id, before, after});
  spdlog::info("Housing contract character={} zone={} channel={} lot={} type={}",
               actor.character_id, lot->zone_key, lot->channel, lot->lot_number,
               site_type_name(type));
  return result;
}

ExtensionResult HousingService::extend(const HousingActor &actor, ContractType contract_type,
                                       const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  ExtensionResult result;
  const auto *current = personal_site_locked(actor.character_id);
  if (!current) {
    result.ec = x5::ec::Housing_NotRegisteredHousingSite;
    return result;
  }
  if (contract_type == ContractType::none) {
    result.ec = x5::ec::Housing_InvalidContractType;
    return result;
  }

  auto site = *current;
  const auto previous_version = site.version;
  HousingCommit commit;
  std::optional<ProgressNotice> progress_notice;

  if (contract_type == ContractType::close) {
    // Closing stops automatic renewal; the site stays until its current expiry.
    site.contract_type = ContractType::close;
  } else {
    const auto now = now_();
    const auto imminent =
        static_cast<std::int64_t>(catalog_->rules().site_rent_imminent_days) * kEpochTicksPerDay;
    // Candidate: rent can be prepaid only inside the imminent window before expiry.
    if (site.expiry_epoch_ticks - now > imminent) {
      result.ec = x5::ec::Housing_FailedContractExtension;
      return result;
    }

    const auto *definition = catalog_->site(site.key.lot_number);
    const auto *cost =
        definition ? catalog_->rent_cost(AuthorityType::individual, definition->type) : nullptr;
    if (!cost) {
      result.ec = x5::ec::Housing_RentCostNotFound;
      return result;
    }

    const auto before = load_progress_locked(actor, access);
    auto after = before;
    const auto rent_error = charge_rent(*cost, after);
    if (!is_success(rent_error)) {
      result.ec = rent_error;
      return result;
    }
    after.revision = before.revision + 1;
    commit.progress.push_back(
        {actor.account_id, actor.character_id, actor.world_id, before, after});
    progress_notice = ProgressNotice{actor.character_id, before, after};

    site.contract_type = contract_type;
    site.expiry_epoch_ticks =
        std::max(site.expiry_epoch_ticks, now) + rent_duration_ticks(AuthorityType::individual);
  }

  site.version = previous_version + 1;
  commit.sites.push_back({site, previous_version, false});
  if (!commit_locked(std::move(commit), access)) {
    result.ec = x5::ec::Housing_FailedContractExtension;
    return result;
  }

  result.expiry_epoch_ticks = site.expiry_epoch_ticks;
  result.contract_type = site.contract_type;
  result.outcome.contracts_changed.push_back(site.key);
  result.outcome.owner_sites_changed.push_back(actor.character_id);
  if (progress_notice) {
    result.outcome.progress_changes.push_back(*progress_notice);
  }
  return result;
}

bool HousingService::pack_site_locked(const SiteRecord &site, PackageReason reason,
                                      HousingCommit &commit, WorldProgress &owner_progress,
                                      bool owner_progress_loaded, HousingOutcome &outcome) {
  MovePackageRecord package;
  package.owner_character_id = site.owner_character_id;
  package.created_epoch_ticks = now_();
  package.zone_key = site.key.zone_key;
  package.original_lot_number = site.key.lot_number;
  package.original_channel = site.key.channel;
  package.reason = reason;
  const auto *definition = catalog_->site(site.key.lot_number);
  package.site_type = definition ? definition->type : SiteType::none;

  // Only owned objects travel in the package; preset objects belonged to the lot.
  StorageLedger ledger(owner_progress);
  for (const auto &[house_id, house] : site.contents.houses) {
    if (!house.from_storage) {
      continue;
    }
    package.contents.houses.emplace(house_id, house);
    if (owner_progress_loaded) {
      if (const auto item = house_item(*catalog_, house.housing_id)) {
        ledger.remove_installed(*item);
      }
    }
  }
  for (const auto &[key, doodad] : site.contents.doodads) {
    if (!doodad.from_storage) {
      continue;
    }
    // A doodad on a building that stays behind moves to the site level of the package.
    package.contents.doodads.emplace(key, doodad);
    if (owner_progress_loaded) {
      if (const auto item = catalog_->item_for_doodad(doodad.cms_id)) {
        ledger.remove_installed(*item);
      }
    }
  }

  commit.sites.push_back({site, site.version, true});
  outcome.sites_removed.push_back(site.key);
  outcome.owner_sites_changed.push_back(site.owner_character_id);

  if (package.contents.houses.empty() && package.contents.doodads.empty()) {
    return false;
  }

  package.package_id = next_package_id_++;
  package.version = 1;
  commit.packages.push_back({package, 0, false});
  outcome.packages_created.push_back({site.owner_character_id, reason});
  return true;
}

SimpleResult HousingService::release(const HousingActor &actor, PackageReason reason,
                                     const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  const auto *current = personal_site_locked(actor.character_id);
  if (!current) {
    return SimpleResult{x5::ec::Housing_NotRegisteredHousingSite, {}};
  }
  return pack_and_commit_locked(*current, reason, access);
}

SimpleResult HousingService::pack_and_commit_locked(const SiteRecord &current,
                                                    PackageReason reason,
                                                    const ProgressAccess *access) {
  SimpleResult result;
  const auto site = current;
  HousingActor owner;
  owner.account_id = site.owner_account_id;
  owner.character_id = site.owner_character_id;
  owner.world_id = world_id_;

  const auto before = load_progress_locked(owner, access);
  auto after = before;
  HousingCommit commit;
  const auto next_package_before = next_package_id_;
  pack_site_locked(site, reason, commit, after, true, result.outcome);
  if (after != before) {
    after.revision = before.revision + 1;
    commit.progress.push_back(
        {site.owner_account_id, site.owner_character_id, world_id_, before, after});
    result.outcome.progress_changes.push_back({site.owner_character_id, before, after});
  }

  // Guild residency ends with the site.
  const auto channel = channels_.find({site.key.zone_key, site.key.channel});
  if (channel != channels_.end() && channel->second.residents.contains(site.key.lot_number)) {
    auto updated_channel = channel->second;
    updated_channel.residents.erase(site.key.lot_number);
    updated_channel.version = channel->second.version + 1;
    commit.channels.push_back({updated_channel, channel->second.version, false});
  }

  if (!commit_locked(std::move(commit), access)) {
    next_package_id_ = next_package_before;
    return SimpleResult{x5::ec::Housing_RetryContract, {}};
  }
  edit_sessions_.erase(site.owner_character_id);
  spdlog::info("Housing site packed zone={} channel={} lot={} reason={}", site.key.zone_key,
               site.key.channel, site.key.lot_number, static_cast<int>(reason));
  return result;
}

void HousingService::pack_owned_site_locked(const SiteRecord &site, PackageReason reason,
                                            HousingCommit &commit, HousingOutcome &outcome) {
  const auto before =
      store_->load_progress(site.owner_account_id, site.owner_character_id, world_id_);
  auto after = before;
  pack_site_locked(site, reason, commit, after, true, outcome);
  if (after == before) {
    return;
  }

  after.revision = before.revision + 1;
  commit.progress.push_back(
      {site.owner_account_id, site.owner_character_id, world_id_, before, after});
  outcome.progress_changes.push_back({site.owner_character_id, before, after});
}

const SiteRecord *HousingService::owned_site_in_channel_locked(CharacterId character_id,
                                                               int zone_key,
                                                               int channel) const {
  for (const auto &[key, site] : sites_) {
    if (key.zone_key == zone_key && key.channel == channel &&
        site.owner_character_id == character_id) {
      return &site;
    }
  }
  return nullptr;
}

BoardResult HousingService::board(const HousingActor &, int zone_key, int channel) {
  std::lock_guard lock(mutex_);
  BoardResult result;
  if (!catalog_->zone(zone_key)) {
    result.ec = x5::ec::Housing_InvalidZoneKey;
    return result;
  }
  if (channel != kFirstChannel && !channels_.contains({zone_key, channel})) {
    result.ec = x5::ec::Housing_ChannelNotFound;
    return result;
  }

  for (const auto &[key, site] : sites_) {
    if (key.zone_key == zone_key && key.channel == channel) {
      result.residents.push_back({key.lot_number, site.owner_character_id, site.owner_name});
    }
  }
  return result;
}

ChannelEntry HousingService::channel_entry_locked(int zone_key, int channel,
                                                  CharacterId viewer) const {
  ChannelEntry entry;
  entry.channel = channel;
  const auto *zone = catalog_->zone(zone_key);
  const auto record = channels_.find({zone_key, channel});
  if (record != channels_.end() && record->second.guild_id != 0) {
    entry.area_name = record->second.area_name;
    entry.guild_id = record->second.guild_id;
    entry.guild_name = record->second.guild_name;
  } else if (zone) {
    entry.area_name = zone->display_name;
  }

  std::map<SiteType, SiteAssignment> assignments;
  for (const auto *site : catalog_->sites_in_zone(zone_key)) {
    auto &assignment = assignments[site->type];
    assignment.site_type = site->type;
    ++assignment.total;
    const auto occupied = sites_.find(SiteKey{zone_key, channel, site->lot_number});
    if (occupied != sites_.end()) {
      ++assignment.assigned;
      if (occupied->second.owner_character_id == viewer) {
        entry.is_residence = true;
      }
    }
  }
  for (const auto &[type, assignment] : assignments) {
    entry.assignments.push_back(assignment);
  }
  return entry;
}

ChannelListResult HousingService::channel_list(const HousingActor &actor, int zone_key,
                                               AuthorityType authority) {
  std::lock_guard lock(mutex_);
  ChannelListResult result;
  if (!catalog_->zone(zone_key)) {
    result.ec = x5::ec::Housing_InvalidZoneKey;
    return result;
  }
  if (authority != AuthorityType::individual && authority != AuthorityType::guild) {
    result.ec = x5::ec::Housing_InvalidAuthorityType;
    return result;
  }

  const bool guild = authority == AuthorityType::guild;
  bool first_listed = false;
  for (const auto &[key, channel] : channels_) {
    if (key.first != zone_key || (channel.guild_id != 0) != guild) {
      continue;
    }
    first_listed = first_listed || key.second == kFirstChannel;
    result.entries.push_back(channel_entry_locked(zone_key, key.second, actor.character_id));
  }
  // The first personal channel always exists, even before anyone rents there.
  if (!guild && !first_listed) {
    result.entries.insert(result.entries.begin(),
                          channel_entry_locked(zone_key, kFirstChannel, actor.character_id));
  }
  return result;
}

ChannelSearchResult HousingService::channel_search(const HousingActor &actor, int zone_key,
                                                   AuthorityType authority,
                                                   const std::string &text) {
  std::lock_guard lock(mutex_);
  ChannelSearchResult result;
  if (!catalog_->zone(zone_key)) {
    result.ec = x5::ec::Housing_InvalidZoneKey;
    return result;
  }
  if (text.empty() || text.size() > kMaximumAreaNameBytes) {
    result.ec = x5::ec::BadRequest;
    return result;
  }

  const auto needle = ascii_lower_copy(text);
  if (authority == AuthorityType::guild) {
    for (const auto &[key, channel] : channels_) {
      if (key.first == zone_key && channel.guild_id != 0 &&
          (ascii_lower_copy(channel.area_name) == needle ||
           ascii_lower_copy(channel.guild_name) == needle)) {
        result.entry = channel_entry_locked(zone_key, key.second, actor.character_id);
        return result;
      }
    }
  } else {
    for (const auto &[key, site] : sites_) {
      if (key.zone_key == zone_key && site.authority == AuthorityType::individual &&
          ascii_lower_copy(site.owner_name) == needle) {
        result.entry = channel_entry_locked(zone_key, key.channel, actor.character_id);
        return result;
      }
    }
  }

  result.ec = x5::ec::Housing_ChannelNotFound;
  return result;
}

VisitResult HousingService::visit(const HousingActor &actor, int zone_key, int channel) {
  std::lock_guard lock(mutex_);
  VisitResult result;
  const auto *zone = catalog_->zone(zone_key);
  if (!zone || !zone->map_id) {
    result.ec = x5::ec::Housing_InvalidZoneKey;
    return result;
  }
  if (channel <= 0 || (channel != kFirstChannel && !channels_.contains({zone_key, channel}))) {
    result.ec = x5::ec::Housing_ChannelNotFound;
    return result;
  }

  const auto lots = catalog_->sites_in_zone(zone_key);
  if (lots.empty()) {
    result.ec = x5::ec::Housing_InvalidArea;
    return result;
  }

  // Arrive at the visitor's own sign when it lives in this channel, else at the first lot.
  const SiteDefinition *arrival = lots.front();
  for (const auto *site : lots) {
    const auto owned = sites_.find(SiteKey{zone_key, channel, site->lot_number});
    if (owned != sites_.end() && owned->second.owner_character_id == actor.character_id) {
      arrival = site;
      break;
    }
  }

  HousingDestination destination;
  destination.zone_key = zone_key;
  destination.map_id = *zone->map_id;
  destination.channel = channel;
  destination.spawn = world_position_mm(*zone->map_id, arrival->sign_x_cm, arrival->sign_y_cm,
                                        arrival->z_cm, kSpawnHeightOffsetMillimetres,
                                        static_cast<float>(arrival->yaw_degrees));

  // Keep the first origin while a visitor hops between housing channels.
  if (actor.position && !catalog_->zone_for_map(actor.position->map_id)) {
    origins_[actor.character_id] = *actor.position;
  }
  const auto origin = origins_.find(actor.character_id);
  if (origin != origins_.end()) {
    destination.origin = origin->second;
  }

  pending_destinations_[actor.character_id] = destination;
  result.destination = destination;
  return result;
}

GoHomeResult HousingService::go_home(const HousingActor &actor) {
  std::optional<SiteKey> home;
  {
    std::lock_guard lock(mutex_);
    const auto *site = personal_site_locked(actor.character_id);
    if (!site) {
      return GoHomeResult{x5::ec::Housing_NotRegisteredHousingSite, std::nullopt};
    }

    // GameVariable GoHomeCooltimeSec. The client shows SM_POPUP_GOHOME_COOL itself; the server
    // refuses early repeats as a bad request.
    const auto now = now_();
    const auto cooldown_ticks =
        static_cast<std::int64_t>(catalog_->rules().go_home_cooldown.count()) *
        kEpochTicksPerSecond;
    const auto last = last_go_home_ticks_.find(actor.character_id);
    if (last != last_go_home_ticks_.end() && now - last->second < cooldown_ticks) {
      return GoHomeResult{x5::ec::BadRequest, std::nullopt};
    }
    last_go_home_ticks_[actor.character_id] = now;
    home = site->key;
  }
  auto visited = visit(actor, home->zone_key, home->channel);
  return GoHomeResult{visited.ec, visited.destination};
}

std::optional<HousingDestination> HousingService::take_destination(CharacterId character_id,
                                                                   int map_id) {
  std::lock_guard lock(mutex_);
  const auto pending = pending_destinations_.find(character_id);
  if (pending != pending_destinations_.end() && pending->second.map_id == map_id) {
    auto destination = pending->second;
    pending_destinations_.erase(pending);
    return destination;
  }

  // Re-entering a housing map without a visit (for example after a relog): use the
  // character's own channel there, otherwise the first channel.
  const auto *zone = catalog_->zone_for_map(map_id);
  if (!zone) {
    return std::nullopt;
  }

  HousingDestination destination;
  destination.zone_key = zone->zone_key;
  destination.map_id = map_id;
  destination.channel = kFirstChannel;
  const auto *site = personal_site_locked(character_id);
  if (site && site->key.zone_key == zone->zone_key) {
    destination.channel = site->key.channel;
  }
  const auto origin = origins_.find(character_id);
  if (origin != origins_.end()) {
    destination.origin = origin->second;
  }
  return destination;
}

std::optional<HousingDestination>
HousingService::walk_in_destination(CharacterId character_id, const WorldPosition &arrival) {
  std::lock_guard lock(mutex_);
  const auto *zone = catalog_->zone_for_map(arrival.map_id);
  if (!zone) {
    return std::nullopt;
  }

  HousingDestination destination;
  destination.zone_key = zone->zone_key;
  destination.map_id = arrival.map_id;
  destination.channel = kFirstChannel;
  const auto *site = personal_site_locked(character_id);
  if (site && site->key.zone_key == zone->zone_key) {
    destination.channel = site->key.channel;
  }
  destination.spawn = arrival;
  origins_.erase(character_id);
  return destination;
}

std::optional<WorldPosition> HousingService::origin_of(CharacterId character_id) const {
  std::lock_guard lock(mutex_);
  const auto found = origins_.find(character_id);
  if (found == origins_.end()) {
    return std::nullopt;
  }
  return found->second;
}

PackageListResult HousingService::packages(const HousingActor &actor) {
  std::lock_guard lock(mutex_);
  PackageListResult result;
  for (const auto &[package_id, package] : packages_) {
    if (package.owner_character_id != actor.character_id) {
      continue;
    }
    result.packages.push_back({package_id, package.site_type, package.created_epoch_ticks,
                               package.original_lot_number, package.original_channel});
  }
  return result;
}

PackageResult HousingService::unpack(const HousingActor &actor, std::int64_t package_id,
                                     int lot_number, const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  PackageResult result;
  const auto package = packages_.find(package_id);
  if (package == packages_.end() || package->second.owner_character_id != actor.character_id) {
    result.ec = x5::ec::Housing_MovePackageInvalidPackage;
    return result;
  }

  const auto *current = personal_site_locked(actor.character_id);
  if (!current) {
    result.ec = x5::ec::Housing_MovePackageNoHousingSite;
    return result;
  }
  if (current->key.lot_number != lot_number) {
    result.ec = x5::ec::Housing_NotOwner;
    return result;
  }

  const auto *definition = catalog_->site(lot_number);
  if (!definition || static_cast<int>(definition->type) <
                         static_cast<int>(package->second.site_type)) {
    result.ec = x5::ec::Housing_MovePackageNeedLargerSite;
    return result;
  }

  const bool has_owned_objects =
      std::ranges::any_of(current->contents.houses,
                          [](const auto &entry) { return entry.second.from_storage; }) ||
      std::ranges::any_of(current->contents.doodads,
                          [](const auto &entry) { return entry.second.from_storage; });
  if (has_owned_objects) {
    result.ec = x5::ec::Housing_MovePackageRequireEmptySite;
    return result;
  }

  // The package replaces the lot's preset content and returns as installed storage items.
  auto site = *current;
  const auto previous_version = site.version;
  site.contents.houses = package->second.contents.houses;
  site.contents.doodads = package->second.contents.doodads;
  for (const auto &[house_id, house] : site.contents.houses) {
    add_model_add_ons(*catalog_, house, site.contents, nullptr);
  }
  site.version = previous_version + 1;

  const auto before = load_progress_locked(actor, access);
  auto after = before;
  StorageLedger ledger(after);
  for (const auto &[house_id, house] : package->second.contents.houses) {
    if (const auto item = house_item(*catalog_, house.housing_id)) {
      if (!ledger.add(*item, 1, true)) {
        result.ec = x5::ec::ItemError;
        return result;
      }
    }
  }
  for (const auto &[key, doodad] : package->second.contents.doodads) {
    if (const auto item = catalog_->item_for_doodad(doodad.cms_id)) {
      if (!ledger.add(*item, 1, true)) {
        result.ec = x5::ec::ItemError;
        return result;
      }
    }
  }
  after.revision = before.revision + 1;

  HousingCommit commit;
  commit.progress.push_back({actor.account_id, actor.character_id, actor.world_id, before, after});
  commit.sites.push_back({site, previous_version, false});
  commit.packages.push_back({package->second, package->second.version, true});
  if (!commit_locked(std::move(commit), access)) {
    result.ec = x5::ec::Housing_RetryContract;
    return result;
  }

  result.package_ids.push_back(package_id);
  result.outcome.sites_added.push_back(site.key);
  result.outcome.progress_changes.push_back({actor.character_id, before, after});
  return result;
}

PackageResult HousingService::recover_to_storage(const HousingActor &actor,
                                                 const std::vector<std::int64_t> &package_ids,
                                                 bool all, const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  PackageResult result;
  std::vector<std::int64_t> selected;
  if (all) {
    for (const auto &[package_id, package] : packages_) {
      if (package.owner_character_id == actor.character_id) {
        selected.push_back(package_id);
      }
    }
  } else {
    std::set<std::int64_t> distinct;
    for (const auto package_id : package_ids) {
      const auto package = packages_.find(package_id);
      if (package == packages_.end() || package->second.owner_character_id != actor.character_id ||
          !distinct.insert(package_id).second) {
        result.ec = x5::ec::Housing_MovePackageInvalidPackage;
        return result;
      }
      selected.push_back(package_id);
    }
  }
  if (selected.empty()) {
    result.ec = x5::ec::Housing_MovePackageInvalidPackage;
    return result;
  }

  const auto before = load_progress_locked(actor, access);
  auto after = before;
  StorageLedger ledger(after);
  HousingCommit commit;
  for (const auto package_id : selected) {
    const auto &package = packages_.at(package_id);
    for (const auto &[house_id, house] : package.contents.houses) {
      if (const auto item = house_item(*catalog_, house.housing_id)) {
        if (!ledger.add(*item, 1, false)) {
          result.ec = x5::ec::ItemError;
          return result;
        }
      }
    }
    for (const auto &[key, doodad] : package.contents.doodads) {
      if (const auto item = catalog_->item_for_doodad(doodad.cms_id)) {
        if (!ledger.add(*item, 1, false)) {
          result.ec = x5::ec::ItemError;
          return result;
        }
      }
    }
    commit.packages.push_back({package, package.version, true});
  }
  after.revision = before.revision + 1;
  commit.progress.push_back({actor.account_id, actor.character_id, actor.world_id, before, after});

  if (!commit_locked(std::move(commit), access)) {
    result.ec = x5::ec::Housing_RetryContract;
    return result;
  }

  result.package_ids = std::move(selected);
  result.outcome.progress_changes.push_back({actor.character_id, before, after});
  return result;
}

EditModeResult HousingService::edit_mode(const HousingActor &actor, EditMode mode, bool end,
                                         int zone_key, int channel) {
  std::lock_guard lock(mutex_);
  EditModeResult result{x5::ec::Success, mode, end};
  if (end) {
    // Keep the ended session for the client's trailing save (see EditSession).
    const auto session = edit_sessions_.find(actor.character_id);
    if (session != edit_sessions_.end()) {
      session->second.ended_epoch_ticks = now_();
    }
    return result;
  }

  const auto *site = owned_site_in_channel_locked(actor.character_id, zone_key, channel);
  if (!site) {
    result.ec = x5::ec::Housing_NotOwner;
    return result;
  }
  if (mode != EditMode::build && !has_main_building(*catalog_, site->contents)) {
    result.ec = x5::ec::Housing_HouseNotFound;
    return result;
  }

  edit_sessions_[actor.character_id] = EditSession{mode, site->key, 0};
  return result;
}

const HousingService::EditSession *HousingService::save_session_locked(CharacterId character_id,
                                                                       const SiteKey &site) {
  const auto session = edit_sessions_.find(character_id);
  if (session == edit_sessions_.end()) {
    return nullptr;
  }

  const auto ended = session->second.ended_epoch_ticks;
  if (ended != 0 && now_() - ended > policy_.edit_save_window_ticks) {
    edit_sessions_.erase(session);
    return nullptr;
  }
  if (session->second.site != site) {
    return nullptr;
  }
  return &session->second;
}

ErrorCode HousingService::validate_update_locked(const SiteRecord &site,
                                                 const UpdateRequest &request,
                                                 WorldProgress &progress, SiteRecord &updated,
                                                 SiteUpdateNotice &notice) const {
  const auto *definition = catalog_->site(site.key.lot_number);
  const auto *design = definition ? catalog_->design(definition->type) : nullptr;
  if (!design) {
    return x5::ec::Housing_InvalidArea;
  }

  StorageLedger ledger(progress);
  auto &contents = updated.contents;

  // Removals first so a request can replace an object with another one using the same ID.
  for (const auto house_id : request.remove_house_ids) {
    const auto house = contents.houses.find(house_id);
    if (house == contents.houses.end()) {
      return x5::ec::Housing_HouseNotFound;
    }
    const bool has_owned_contents = std::ranges::any_of(contents.doodads, [&](const auto &entry) {
      return entry.first.first == house_id && entry.second.from_storage;
    });
    if (has_owned_contents) {
      return x5::ec::Housing_HouseNotEmpty;
    }
    if (house->second.from_storage) {
      if (const auto item = house_item(*catalog_, house->second.housing_id)) {
        ledger.uninstall(*item);
      }
    }
    remove_house_doodads(contents, house_id);
    contents.houses.erase(house);
    notice.removed_house_ids.push_back(house_id);
  }

  for (const auto unit_id : request.remove_doodad_unit_ids) {
    const auto key = parse_housing_doodad_unit_id(unit_id);
    if (!key || key->lot_number != site.key.lot_number) {
      return x5::ec::Housing_InvalidDoodadUpdateReq;
    }
    const auto doodad = contents.doodads.find(DoodadKey{key->house_id, key->doodad_id});
    if (doodad == contents.doodads.end()) {
      return x5::ec::Housing_DoodadNotFound;
    }
    if (doodad->second.from_storage) {
      if (const auto item = catalog_->item_for_doodad(doodad->second.cms_id)) {
        ledger.uninstall(*item);
      }
    }
    contents.doodads.erase(doodad);
    notice.removed_doodad_unit_ids.push_back(unit_id);
  }

  std::set<std::uint32_t> seen_houses;
  for (const auto &requested : request.houses) {
    if (requested.house_id == 0 || requested.house_id > kMaximumObjectId) {
      return x5::ec::Housing_OutOfBoundId;
    }
    if (!seen_houses.insert(requested.house_id).second) {
      return x5::ec::Housing_DuplicateHouse;
    }
    const auto *housing = catalog_->housing(requested.housing_id);
    if (!housing) {
      return x5::ec::Housing_InvalidCmsId;
    }
    const bool main_building = housing->category == HousingCategory::main_building;
    if ((main_building && requested.attach_index != kUnattachedHouseIndex) ||
        (!main_building && requested.attach_index < 0)) {
      return x5::ec::Housing_InvalidAttachIndex;
    }
    if (!within_site(*design, requested.position) || !std::isfinite(requested.yaw)) {
      return x5::ec::Housing_InvalidArea;
    }

    auto placed = requested;
    placed.parts_state = 0;
    placed.from_storage = true;
    const auto existing = contents.houses.find(requested.house_id);
    if (existing != contents.houses.end()) {
      placed.parts_state = existing->second.parts_state;
      if (existing->second.housing_id == requested.housing_id) {
        // A move keeps ownership; a preset building stays a preset building.
        placed.from_storage = existing->second.from_storage;
      } else {
        const auto *upgrade = catalog_->upgrade(existing->second.housing_id, requested.housing_id);
        if (!upgrade) {
          return x5::ec::Housing_NotAvailableUpgradeBuilding;
        }
        for (const auto &material : upgrade->materials) {
          const auto held = progress.stackables.find(material.item_cms_id);
          if (held == progress.stackables.end() || held->second < material.count) {
            return x5::ec::Craft_MaterialNotEnough;
          }
          held->second -= material.count;
        }
        if (existing->second.from_storage) {
          if (const auto item = house_item(*catalog_, existing->second.housing_id)) {
            ledger.remove_installed(*item);
          }
        }
        const auto upgraded_item = house_item(*catalog_, requested.housing_id);
        if (upgraded_item && !ledger.add(*upgraded_item, 1, true)) {
          return x5::ec::ItemError;
        }
        placed.from_storage = upgraded_item.has_value();
        // The new model may carry different built-in doodads.
        std::erase_if(contents.doodads, [&](const auto &entry) {
          return entry.first.first == requested.house_id && !entry.second.from_storage;
        });
      }
    } else {
      const auto item = housing->item_cms_id;
      if (!item || !ledger.install(*item)) {
        return x5::ec::ItemError;
      }
    }

    const bool is_new_model =
        existing == contents.houses.end() || existing->second.housing_id != placed.housing_id;
    contents.houses[placed.house_id] = placed;
    notice.houses.push_back(placed);
    if (is_new_model) {
      add_model_add_ons(*catalog_, placed, contents, &notice.doodads);
    }
  }

  if (!request.houses.empty()) {
    const bool needs_main = std::ranges::any_of(contents.houses, [](const auto &entry) {
      return entry.second.attach_index >= 0;
    });
    if (needs_main && !has_main_building(*catalog_, contents)) {
      return x5::ec::Housing_InvalidAttachIndex;
    }
  }

  std::set<DoodadKey> seen_doodads;
  for (const auto &requested : request.doodads) {
    if (requested.doodad_id == 0 || requested.doodad_id > kMaximumObjectId ||
        requested.house_id > kMaximumObjectId ||
        requested.parent_doodad_id > kMaximumObjectId) {
      return x5::ec::Housing_OutOfBoundId;
    }
    const DoodadKey key{requested.house_id, requested.doodad_id};
    if (!seen_doodads.insert(key).second) {
      return x5::ec::Housing_DuplicateHousingDoodad;
    }
    if (requested.house_id != 0 && !contents.houses.contains(requested.house_id)) {
      return x5::ec::Housing_HouseNotFound;
    }
    if (!within_site(*design, requested.position) ||
        !finite_rotation(requested.pitch, requested.yaw, requested.roll)) {
      return x5::ec::Housing_InvalidArea;
    }

    auto placed = requested;
    const auto existing = contents.doodads.find(key);
    if (existing != contents.doodads.end() && existing->second.cms_id == requested.cms_id) {
      // A move keeps phase, interaction state and ownership.
      placed.phase = existing->second.phase;
      placed.phase_begin_tick = existing->second.phase_begin_tick;
      placed.interaction_count = existing->second.interaction_count;
      placed.from_storage = existing->second.from_storage;
    } else {
      if (existing != contents.doodads.end() && existing->second.from_storage) {
        if (const auto item = catalog_->item_for_doodad(existing->second.cms_id)) {
          ledger.uninstall(*item);
        }
      }
      const auto item = catalog_->item_for_doodad(requested.cms_id);
      if (!item) {
        return x5::ec::Housing_InvalidCmsId;
      }
      if (!ledger.install(*item)) {
        return x5::ec::ItemError;
      }
      placed.phase = std::max(requested.phase, 1);
      placed.phase_begin_tick = 0;
      placed.interaction_count = 0;
      placed.from_storage = true;
    }
    contents.doodads[key] = placed;
    notice.doodads.push_back(placed);
  }

  if (request.house_part_custom_ids) {
    if (request.house_part_custom_ids->size() > kMaximumCustomIdBytes) {
      return x5::ec::Housing_InvalidHousingPartIds;
    }
    contents.house_part_custom_ids = *request.house_part_custom_ids;
  }

  const auto &rules = catalog_->rules();
  const auto limit = rules.object_limit.find(definition->type);
  if (limit != rules.object_limit.end() &&
      object_count(contents) > static_cast<std::size_t>(limit->second)) {
    return x5::ec::Housing_InvalidEditModeSaveObjectLimitCount;
  }
  const auto farm_limit = rules.farm_doodad_limit.find(definition->type);
  if (farm_limit != rules.farm_doodad_limit.end()) {
    const auto farms = std::ranges::count_if(contents.doodads, [&](const auto &entry) {
      return entry.second.cms_id == rules.farm_doodad_cms_id;
    });
    if (farms > farm_limit->second) {
      return x5::ec::Housing_InvalidEditModeSaveFarmDoodadCount;
    }
  }

  validate_site_contents(contents);
  return x5::ec::Success;
}

UpdateResult HousingService::update(const HousingActor &actor, const UpdateRequest &request,
                                    int zone_key, int channel, const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  UpdateResult result;
  const auto *current = owned_site_in_channel_locked(actor.character_id, zone_key, channel);
  if (!current) {
    result.ec = personal_site_locked(actor.character_id) ? x5::ec::Housing_NotOwner
                                                         : x5::ec::Housing_NotRegisteredHousingSite;
    return result;
  }
  if (current->key.lot_number != request.lot_number) {
    result.ec = x5::ec::Housing_NotOwner;
    return result;
  }
  const auto *session = save_session_locked(actor.character_id, current->key);
  if (!session) {
    result.ec = x5::ec::Housing_InvalidDoodadUpdateReq;
    return result;
  }

  // The save that follows HousingEditModeReq{end} is the session's last request whatever its
  // outcome: the client has already left edit mode.
  const bool closes_session = session->ended_epoch_ticks != 0;
  result = apply_update_locked(actor, request, *current, access);
  if (closes_session) {
    edit_sessions_.erase(actor.character_id);
  }
  return result;
}

UpdateResult HousingService::apply_update_locked(const HousingActor &actor,
                                                 const UpdateRequest &request,
                                                 const SiteRecord &current,
                                                 const ProgressAccess *access) {
  UpdateResult result;
  const std::size_t requested_objects = request.houses.size() + request.doodads.size() +
                                        request.remove_house_ids.size() +
                                        request.remove_doodad_unit_ids.size();
  if (requested_objects == 0 && !request.house_part_custom_ids) {
    result.ec = x5::ec::Housing_NoneChange;
    return result;
  }
  if (requested_objects > policy_.maximum_update_objects) {
    result.ec = x5::ec::Housing_InvalidEditModeSaveObjectLimitCount;
    return result;
  }

  const auto now = now_();
  const auto cooldown_ticks = catalog_->rules().edit_save_cooldown.count() * 10'000;
  if (current.edit_save_cooldown_start_ticks != 0 &&
      now - current.edit_save_cooldown_start_ticks < cooldown_ticks) {
    result.ec = x5::ec::Housing_InvalidEditModeSaveCooldown;
    return result;
  }

  const auto before = load_progress_locked(actor, access);
  auto after = before;
  auto updated = current;
  SiteUpdateNotice notice;
  notice.site = current.key;
  const auto error = validate_update_locked(current, request, after, updated, notice);
  if (!is_success(error)) {
    result.ec = error;
    return result;
  }

  updated.edit_save_cooldown_start_ticks = now;
  updated.version = current.version + 1;
  notice.edit_save_cooldown_start_ticks = now;

  // A newly placed object starts its first phase now, so timed phases (garden growth) count
  // from the placement rather than from the Unix epoch.
  for (auto &placed : notice.doodads) {
    if (placed.phase_begin_tick == 0) {
      placed.phase_begin_tick = now;
      updated.contents.doodads.at(DoodadKey{placed.house_id, placed.doodad_id})
          .phase_begin_tick = now;
    }
  }

  HousingCommit commit;
  commit.sites.push_back({updated, current.version, false});
  if (after != before) {
    after.revision = before.revision + 1;
    commit.progress.push_back(
        {actor.account_id, actor.character_id, actor.world_id, before, after});
  }
  if (!commit_locked(std::move(commit), access)) {
    result.ec = x5::ec::Housing_RetryContract;
    return result;
  }

  // The client applies only the customization when house_part_custom_ids is present
  // (AAC_PS2C_UpdateHousingRes_Handle), so a combined save is announced in two parts.
  if (request.house_part_custom_ids) {
    SiteUpdateNotice customization;
    customization.site = notice.site;
    customization.custom_ids_changed = true;
    customization.house_part_custom_ids = updated.contents.house_part_custom_ids;
    customization.edit_save_cooldown_start_ticks = now;
    result.outcome.site_updates.push_back(std::move(customization));
  }
  if (requested_objects > 0) {
    result.outcome.site_updates.push_back(std::move(notice));
  }
  if (after != before) {
    result.outcome.progress_changes.push_back({actor.character_id, before, after});
  }
  return result;
}

SimpleResult HousingService::set_house_part(const HousingActor &, int zone_key, int channel,
                                            int lot_number, std::uint32_t house_id,
                                            std::uint32_t part_attach_index, bool open) {
  std::lock_guard lock(mutex_);
  SimpleResult result;
  const auto found = sites_.find(SiteKey{zone_key, channel, lot_number});
  if (found == sites_.end()) {
    result.ec = x5::ec::Housing_NotRegisteredHousingSite;
    return result;
  }
  const auto house = found->second.contents.houses.find(house_id);
  if (house == found->second.contents.houses.end()) {
    result.ec = x5::ec::Housing_HouseNotFound;
    return result;
  }
  if (part_attach_index > kMaximumPartAttachIndex) {
    result.ec = x5::ec::Housing_InvalidHousingPartIds;
    return result;
  }

  auto site = found->second;
  auto &state = site.contents.houses.at(house_id).parts_state;
  const std::uint64_t bit = std::uint64_t{1} << part_attach_index;
  state = open ? (state | bit) : (state & ~bit);
  if (state == house->second.parts_state) {
    return result;
  }

  const auto previous_version = site.version;
  site.version = previous_version + 1;
  HousingCommit commit;
  commit.sites.push_back({site, previous_version, false});
  if (!commit_locked(std::move(commit), nullptr)) {
    result.ec = x5::ec::Housing_RetryContract;
    return result;
  }

  result.outcome.parts_updates.push_back({site.key, house_id, state});
  return result;
}

FurniturePoseCheck HousingService::furniture_pose(int zone_key, int channel,
                                                  const DoodadUnitKey &target, int animation_id,
                                                  std::optional<int> interaction_id) const {
  std::lock_guard lock(mutex_);
  FurniturePoseCheck result;
  const auto site = sites_.find(SiteKey{zone_key, channel, target.lot_number});
  if (site == sites_.end()) {
    result.ec = x5::ec::Housing_NotRegisteredHousingSite;
    return result;
  }

  const auto &doodads = site->second.contents.doodads;
  const auto doodad = doodads.find(DoodadKey{target.house_id, target.doodad_id});
  if (doodad == doodads.end()) {
    result.ec = x5::ec::Housing_DoodadNotFound;
    return result;
  }

  const auto *poses = catalog_->furniture_interactions(doodad->second.cms_id);
  if (!poses) {
    result.ec = x5::ec::Interaction_Invalid;
    return result;
  }
  const auto pose = std::ranges::find_if(*poses, [&](const FurnitureInteraction &candidate) {
    const bool interaction_matches =
        !interaction_id || candidate.interaction_id == *interaction_id;
    return candidate.animation_id == animation_id && interaction_matches;
  });
  if (pose == poses->end()) {
    result.ec = x5::ec::Interaction_Invalid;
    return result;
  }
  result.holds = pose->holds_pose;
  return result;
}

void HousingService::set_doodad_profile(std::shared_ptr<const HousingDoodadProfile> profile,
                                        std::uint32_t random_seed) {
  std::lock_guard lock(mutex_);
  doodad_profile_ = std::move(profile);
  doodad_random_.seed(random_seed);
}

const HousingDoodadProfile *HousingService::doodad_profile() const {
  std::lock_guard lock(mutex_);
  return doodad_profile_.get();
}

std::vector<DoodadPhaseNotice> HousingService::advance_site_doodads_locked(SiteRecord &site,
                                                                          std::int64_t now) {
  // A chain of timed phases that elapsed back to back (withering 800 ms then 1 ms) resolves
  // in one pass; the bound only stops a malformed cycle of zero-length timers.
  constexpr int kMaximumChainedPhases = 64;
  constexpr std::int64_t kTicksPerMillisecond = 10'000;

  std::vector<DoodadPhaseNotice> notices;
  if (!doodad_profile_) {
    return notices;
  }
  for (auto &[key, doodad] : site.contents.doodads) {
    if (!doodad_profile_->doodad(doodad.cms_id)) {
      continue;
    }
    if (doodad.phase_begin_tick == 0) {
      // Placed before phase timers existed: its current phase starts counting now.
      doodad.phase_begin_tick = now;
      notices.push_back({site.key, key.first, key.second, doodad.phase, now});
      continue;
    }

    bool changed = false;
    for (int step = 0; step < kMaximumChainedPhases; ++step) {
      const auto *timer = doodad_profile_->timer(doodad.cms_id, doodad.phase);
      if (!timer) {
        break;
      }
      const auto due = doodad.phase_begin_tick + timer->duration_ms * kTicksPerMillisecond;
      if (now < due) {
        break;
      }
      doodad.phase = choose_doodad_phase(timer->next, doodad_random_);
      doodad.phase_begin_tick = due;
      changed = true;
    }
    if (changed) {
      notices.push_back({site.key, key.first, key.second, doodad.phase, doodad.phase_begin_tick});
    }
  }
  return notices;
}

bool HousingService::commit_doodad_advance_locked(const SiteRecord &before, SiteRecord site,
                                                  const std::vector<DoodadPhaseNotice> &notices) {
  if (notices.empty()) {
    return true;
  }
  site.version = before.version + 1;
  HousingCommit commit;
  commit.sites.push_back({std::move(site), before.version, false});
  return commit_locked(std::move(commit), nullptr);
}

ErrorCode HousingService::doodad_conditions_hold(const DoodadTransition &transition,
                                                 const WorldProgress &progress) {
  for (const auto &condition : transition.conditions) {
    const auto held = progress.stackables.find(condition.item_cms_id);
    if (held == progress.stackables.end() || held->second < condition.count) {
      return x5::ec::Interaction_ConditionsNotMatch;
    }
  }
  return x5::ec::Success;
}

DoodadUseCheck HousingService::check_doodad_use(const HousingActor &actor, int zone_key,
                                                int channel, const DoodadUnitKey &target,
                                                int interaction_id, int animation_id,
                                                const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  DoodadUseCheck result;
  const auto found = sites_.find(SiteKey{zone_key, channel, target.lot_number});
  if (found == sites_.end()) {
    result.ec = x5::ec::Housing_NotRegisteredHousingSite;
    return result;
  }
  if (found->second.owner_character_id != actor.character_id) {
    result.ec = x5::ec::Housing_NotOwner;
    return result;
  }

  // Phases that elapsed since the last sweep are applied before the interaction is judged.
  auto advanced = found->second;
  auto notices = advance_site_doodads_locked(advanced, now_());
  if (!commit_doodad_advance_locked(found->second, advanced, notices)) {
    result.ec = x5::ec::Housing_RetryContract;
    return result;
  }
  result.outcome.doodad_phases = std::move(notices);

  const auto &site = sites_.at(SiteKey{zone_key, channel, target.lot_number});
  const auto doodad = site.contents.doodads.find(DoodadKey{target.house_id, target.doodad_id});
  if (doodad == site.contents.doodads.end()) {
    result.ec = x5::ec::Housing_DoodadNotFound;
    return result;
  }
  const auto *transition = doodad_profile_ ? doodad_profile_->interaction(
                                                 doodad->second.cms_id, doodad->second.phase,
                                                 interaction_id)
                                           : nullptr;
  if (!transition || transition->animation_id != animation_id) {
    result.ec = x5::ec::Interaction_Invalid;
    return result;
  }

  const auto progress = load_progress_locked(actor, access);
  result.ec = doodad_conditions_hold(*transition, progress);
  result.phase = doodad->second.phase;
  result.animation = doodad_profile_->animation(animation_id);
  return result;
}

DoodadUseResult HousingService::complete_doodad_use(const HousingActor &actor, int zone_key,
                                                    int channel, const DoodadUnitKey &target,
                                                    int interaction_id, int expected_phase,
                                                    const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  DoodadUseResult result;
  const SiteKey key{zone_key, channel, target.lot_number};
  const auto found = sites_.find(key);
  if (found == sites_.end()) {
    result.ec = x5::ec::Housing_NotRegisteredHousingSite;
    return result;
  }
  if (found->second.owner_character_id != actor.character_id) {
    result.ec = x5::ec::Housing_NotOwner;
    return result;
  }

  const auto now = now_();
  auto updated = found->second;
  auto notices = advance_site_doodads_locked(updated, now);
  const auto doodad = updated.contents.doodads.find(DoodadKey{target.house_id, target.doodad_id});
  if (doodad == updated.contents.doodads.end()) {
    result.ec = x5::ec::Housing_DoodadNotFound;
    return result;
  }
  // The phase may have moved on (a timer elapsed, another session used it) during the
  // animation; the interaction then no longer applies.
  const auto *transition =
      doodad_profile_ ? doodad_profile_->interaction(doodad->second.cms_id, expected_phase,
                                                     interaction_id)
                      : nullptr;
  if (!transition || doodad->second.phase != expected_phase) {
    result.ec = x5::ec::Interaction_Invalid;
    return result;
  }

  const auto before = load_progress_locked(actor, access);
  result.ec = doodad_conditions_hold(*transition, before);
  if (!is_success(result.ec)) {
    return result;
  }
  auto after = before;
  for (const auto &condition : transition->conditions) {
    if (condition.kind == DoodadCondition::Kind::consume) {
      after.stackables.at(condition.item_cms_id) -= condition.count;
    }
  }
  if (transition->reward) {
    result.rewards = roll_doodad_reward(*transition->reward, doodad_random_);
    result.looting_reason = transition->reward->looting_reason;
    for (const auto &[item_cms_id, quantity] : result.rewards) {
      auto &stack = after.stackables[item_cms_id];
      if (stack > std::numeric_limits<int>::max() - quantity) {
        result.ec = x5::ec::ItemCountError;
        return result;
      }
      stack += quantity;
    }
  }

  doodad->second.phase = choose_doodad_phase(transition->next, doodad_random_);
  doodad->second.phase_begin_tick = now;
  result.next_phase = doodad->second.phase;
  result.phase_begin_epoch_ticks = now;
  std::erase_if(notices, [&](const DoodadPhaseNotice &notice) {
    return notice.house_id == target.house_id && notice.doodad_id == target.doodad_id;
  });
  notices.push_back({key, target.house_id, target.doodad_id, result.next_phase, now});

  updated.version = found->second.version + 1;
  HousingCommit commit;
  commit.sites.push_back({updated, found->second.version, false});
  if (after != before) {
    after.revision = before.revision + 1;
    commit.progress.push_back(
        {actor.account_id, actor.character_id, actor.world_id, before, after});
  }
  if (!commit_locked(std::move(commit), access)) {
    result.ec = x5::ec::Housing_RetryContract;
    return result;
  }

  result.outcome.doodad_phases = std::move(notices);
  for (const auto &[item_cms_id, quantity] : after.stackables) {
    const auto previous = before.stackables.find(item_cms_id);
    if (previous == before.stackables.end() || previous->second != quantity) {
      result.changed_stacks[item_cms_id] = quantity;
    }
  }
  return result;
}

HousingOutcome HousingService::advance_due_doodads() {
  std::lock_guard lock(mutex_);
  HousingOutcome outcome;
  if (!doodad_profile_) {
    return outcome;
  }

  const auto now = now_();
  std::vector<SiteKey> keys;
  keys.reserve(sites_.size());
  for (const auto &[key, site] : sites_) {
    keys.push_back(key);
  }
  for (const auto &key : keys) {
    const auto found = sites_.find(key);
    if (found == sites_.end()) {
      continue;
    }
    auto advanced = found->second;
    auto notices = advance_site_doodads_locked(advanced, now);
    if (notices.empty()) {
      continue;
    }
    if (!commit_doodad_advance_locked(found->second, std::move(advanced), notices)) {
      // The next sweep retries from the stored state; nothing was announced.
      continue;
    }
    outcome.doodad_phases.insert(outcome.doodad_phases.end(), notices.begin(), notices.end());
  }
  return outcome;
}

SimpleResult HousingService::discard_item(const HousingActor &actor, int item_cms_id,
                                          int quantity, const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  SimpleResult result;
  if (!catalog_->is_discardable_storage_item(item_cms_id) || quantity <= 0) {
    result.ec = x5::ec::ItemError;
    return result;
  }

  const auto before = load_progress_locked(actor, access);
  auto after = before;
  StorageLedger ledger(after);
  if (!ledger.discard(item_cms_id, quantity)) {
    result.ec = x5::ec::ItemError;
    return result;
  }
  after.revision = before.revision + 1;

  HousingCommit commit;
  commit.progress.push_back({actor.account_id, actor.character_id, actor.world_id, before, after});
  if (!commit_locked(std::move(commit), access)) {
    result.ec = x5::ec::Housing_RetryContract;
    return result;
  }

  result.outcome.progress_changes.push_back({actor.character_id, before, after});
  return result;
}

SimpleResult HousingService::gm_reset_account(const HousingActor &actor,
                                              const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  SimpleResult result;
  const auto before = load_progress_locked(actor, access);
  auto after = before;
  StorageLedger ledger(after);
  HousingCommit commit;

  // Installed objects return to storage; packaged objects are recovered to storage, so a reset
  // never destroys items the character owns.
  for (const auto &[key, site] : sites_) {
    if (site.owner_character_id != actor.character_id) {
      continue;
    }
    for (const auto &[house_id, house] : site.contents.houses) {
      const auto item = house_item(*catalog_, house.housing_id);
      if (house.from_storage && item) {
        ledger.uninstall(*item);
      }
    }
    for (const auto &[doodad_key, doodad] : site.contents.doodads) {
      const auto item = catalog_->item_for_doodad(doodad.cms_id);
      if (doodad.from_storage && item) {
        ledger.uninstall(*item);
      }
    }
    commit.sites.push_back({site, site.version, true});
    result.outcome.sites_removed.push_back(key);
  }
  for (const auto &[package_id, package] : packages_) {
    if (package.owner_character_id != actor.character_id) {
      continue;
    }
    for (const auto &[house_id, house] : package.contents.houses) {
      if (const auto item = house_item(*catalog_, house.housing_id)) {
        ledger.add(*item, 1, false);
      }
    }
    for (const auto &[doodad_key, doodad] : package.contents.doodads) {
      if (const auto item = catalog_->item_for_doodad(doodad.cms_id)) {
        ledger.add(*item, 1, false);
      }
    }
    commit.packages.push_back({package, package.version, true});
  }
  const auto character = characters_.find(actor.character_id);
  if (character != characters_.end()) {
    commit.characters.push_back({character->second, character->second.version, true});
  }

  if (after != before) {
    after.revision = before.revision + 1;
    commit.progress.push_back(
        {actor.account_id, actor.character_id, actor.world_id, before, after});
    result.outcome.progress_changes.push_back({actor.character_id, before, after});
  }
  if (commit.empty()) {
    return result;
  }
  if (!commit_locked(std::move(commit), access)) {
    return SimpleResult{x5::ec::Housing_RetryContract, {}};
  }

  edit_sessions_.erase(actor.character_id);
  origins_.erase(actor.character_id);
  pending_destinations_.erase(actor.character_id);
  result.outcome.owner_sites_changed.push_back(actor.character_id);
  return result;
}

std::optional<SiteRecord> HousingService::site(const SiteKey &key) const {
  std::lock_guard lock(mutex_);
  const auto found = sites_.find(key);
  if (found == sites_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<SiteRecord> HousingService::personal_site(CharacterId character_id) const {
  std::lock_guard lock(mutex_);
  const auto *site = personal_site_locked(character_id);
  if (!site) {
    return std::nullopt;
  }
  return *site;
}

std::optional<SiteRecord> HousingService::guild_site_of(CharacterId character_id) const {
  std::lock_guard lock(mutex_);
  for (const auto &[key, site] : sites_) {
    if (site.authority == AuthorityType::guild && site.owner_character_id == character_id) {
      return site;
    }
  }
  return std::nullopt;
}

std::vector<SiteRecord> HousingService::sites_in_channel(int zone_key, int channel) const {
  std::lock_guard lock(mutex_);
  std::vector<SiteRecord> result;
  for (const auto &[key, site] : sites_) {
    if (key.zone_key == zone_key && key.channel == channel) {
      result.push_back(site);
    }
  }
  return result;
}

std::optional<ChannelRecord> HousingService::channel_record(int zone_key, int channel) const {
  std::lock_guard lock(mutex_);
  const auto found = channels_.find({zone_key, channel});
  if (found == channels_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<ChannelRecord> HousingService::guild_area(GuildId guild_id) const {
  std::lock_guard lock(mutex_);
  return guild_area_locked(guild_id);
}

std::optional<ChannelRecord> HousingService::guild_area_locked(GuildId guild_id) const {
  for (const auto &[key, channel] : channels_) {
    if (guild_id != 0 && channel.guild_id == guild_id) {
      return channel;
    }
  }
  return std::nullopt;
}

} // namespace chronicles::housing
