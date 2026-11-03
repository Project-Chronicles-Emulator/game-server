// Guild housing areas, GM housing commands and contract expiry for HousingService.
// The personal housing rules live in housing_service.cpp.

#include "world/features/housing/housing_service.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <spdlog/spdlog.h>

namespace chronicles::housing {
namespace {

// GameVariable GuildChannelNameUnitLengthMin/Max: an area name has 2 to 14 characters.
constexpr std::size_t kMinimumAreaNameCharacters = 2;
constexpr std::size_t kMaximumAreaNameCharacters = 14;
constexpr std::size_t kMaximumAreaNameBytes = 64;
constexpr int kFirstGuildChannel = 1;
constexpr std::int64_t kTicksPerMinute = kEpochTicksPerSecond * 60;

bool is_success(ErrorCode code) {
  return code == x5::ec::Success;
}

std::size_t utf8_character_count(const std::string &text) {
  std::size_t count = 0;
  for (const auto byte : text) {
    if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

bool valid_area_name(const std::string &name) {
  if (name.size() > kMaximumAreaNameBytes) {
    return false;
  }
  const auto characters = utf8_character_count(name);
  if (characters < kMinimumAreaNameCharacters || characters > kMaximumAreaNameCharacters) {
    return false;
  }
  return std::ranges::none_of(name, [](char character) {
    return static_cast<unsigned char>(character) < 0x20;
  });
}

// Lower grade numbers hold more authority (x5.cms.EX5GuildMemberGrade order).
bool grade_at_least(GuildGrade grade, GuildGrade lowest_allowed) {
  return grade != GuildGrade::none && static_cast<int>(grade) <= static_cast<int>(lowest_allowed);
}

} // namespace

ErrorCode HousingService::guild_permission(const HousingActor &actor, GuildId guild_id,
                                           GuildGrade lowest_allowed,
                                           GuildMembership &membership) const {
  const auto found = guilds_->membership(actor.character_id);
  if (!found) {
    return x5::ec::Guild_NotJoined;
  }
  if (guild_id != 0 && found->guild_id != guild_id) {
    return x5::ec::Guild_InvalidReqParam;
  }
  if (!grade_at_least(found->grade, lowest_allowed)) {
    return x5::ec::Guild_NoPermission;
  }
  membership = *found;
  return x5::ec::Success;
}

GuildAreaResult HousingService::guild_contract(const HousingActor &actor, GuildId guild_id,
                                               int zone_key, ContractType contract_type,
                                               const std::string &area_name,
                                               const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  GuildAreaResult result;
  GuildMembership membership;
  result.ec = guild_permission(actor, guild_id, GuildGrade::master, membership);
  if (!is_success(result.ec)) {
    return result;
  }

  const auto *zone = catalog_->zone(zone_key);
  if (!zone || !zone->map_id) {
    result.ec = x5::ec::Housing_InvalidZoneKey;
    return result;
  }
  if (contract_type != ContractType::monthly && contract_type != ContractType::periodic) {
    result.ec = x5::ec::Housing_InvalidContractType;
    return result;
  }
  if (guild_area_locked(membership.guild_id)) {
    result.ec = x5::ec::Guild_HasHousingContract;
    return result;
  }
  if (!valid_area_name(area_name)) {
    result.ec = x5::ec::Guild_ChannelNameLengthInvalid;
    return result;
  }
  if (open_channel_count_locked(zone_key, true) >= policy_.guild_channel_open_limit) {
    result.ec = x5::ec::Housing_ChannelLimitReached;
    return result;
  }

  const auto *cost = catalog_->rent_cost(AuthorityType::guild, SiteType::none);
  if (!cost) {
    result.ec = x5::ec::Housing_RentCostNotFound;
    return result;
  }

  const auto before = load_progress_locked(actor, access);
  auto after = before;
  auto character = characters_.contains(actor.character_id)
                       ? characters_.at(actor.character_id)
                       : CharacterHousingRecord{actor.character_id};
  const auto previous_character_version = character.version;
  const bool needs_unlock = cost->unlock_value > 0 && !character.guild_unlocked;
  if (needs_unlock) {
    result.ec = charge_unlock(*cost, after);
    if (!is_success(result.ec)) {
      return result;
    }
    character.guild_unlocked = true;
  }
  result.ec = charge_rent(*cost, after);
  if (!is_success(result.ec)) {
    return result;
  }
  after.revision = before.revision + 1;

  // Guild areas use channel numbers no personal channel uses.
  int channel_number = kFirstGuildChannel;
  while (channels_.contains({zone_key, channel_number})) {
    ++channel_number;
  }

  const auto now = now_();
  ChannelRecord channel;
  channel.zone_key = zone_key;
  channel.channel = channel_number;
  channel.version = 1;
  channel.guild_id = membership.guild_id;
  channel.guild_name = membership.guild_name;
  channel.area_name = area_name;
  channel.contract_type = contract_type;
  channel.expiry_epoch_ticks = now + rent_duration_ticks(AuthorityType::guild);

  HousingCommit commit;
  commit.progress.push_back({actor.account_id, actor.character_id, actor.world_id, before, after});
  commit.channels.push_back({channel, 0, false});
  if (needs_unlock) {
    character.version = previous_character_version + 1;
    commit.characters.push_back({character, previous_character_version, false});
  }
  if (!commit_locked(std::move(commit), access)) {
    result.ec = x5::ec::Housing_RetryContract;
    return result;
  }

  result.channel = channel_number;
  result.expiry_epoch_ticks = channel.expiry_epoch_ticks;
  result.contract_type = contract_type;
  result.area_name = area_name;
  result.zone_key = zone_key;
  result.outcome.guild_areas.push_back({membership.guild_id, zone_key, channel_number, false});
  result.outcome.progress_changes.push_back({actor.character_id, before, after});
  spdlog::info("Guild housing area contracted guild={} zone={} channel={}", membership.guild_id,
               zone_key, channel_number);
  return result;
}

GuildAreaResult HousingService::guild_extend(const HousingActor &actor, GuildId guild_id,
                                             ContractType contract_type,
                                             const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  GuildAreaResult result;
  GuildMembership membership;
  result.ec = guild_permission(actor, guild_id, GuildGrade::master, membership);
  if (!is_success(result.ec)) {
    return result;
  }

  const auto area = guild_area_locked(membership.guild_id);
  if (!area) {
    result.ec = x5::ec::Housing_ChannelNotFound;
    return result;
  }
  if (contract_type == ContractType::none) {
    result.ec = x5::ec::Housing_InvalidContractType;
    return result;
  }

  auto channel = *area;
  HousingCommit commit;
  if (contract_type == ContractType::close) {
    channel.contract_type = ContractType::close;
  } else {
    const auto now = now_();
    const auto imminent =
        static_cast<std::int64_t>(catalog_->rules().area_rent_imminent_days) * kEpochTicksPerDay;
    if (channel.expiry_epoch_ticks - now > imminent) {
      result.ec = x5::ec::Housing_FailedContractExtension;
      return result;
    }

    const auto *cost = catalog_->rent_cost(AuthorityType::guild, SiteType::none);
    if (!cost) {
      result.ec = x5::ec::Housing_RentCostNotFound;
      return result;
    }
    const auto before = load_progress_locked(actor, access);
    auto after = before;
    result.ec = charge_rent(*cost, after);
    if (!is_success(result.ec)) {
      return result;
    }
    after.revision = before.revision + 1;
    commit.progress.push_back(
        {actor.account_id, actor.character_id, actor.world_id, before, after});
    result.outcome.progress_changes.push_back({actor.character_id, before, after});

    channel.contract_type = contract_type;
    channel.expiry_epoch_ticks = std::max(channel.expiry_epoch_ticks, now) +
                                 rent_duration_ticks(AuthorityType::guild);
  }

  channel.version = area->version + 1;
  commit.channels.push_back({channel, area->version, false});
  // Guild sites expire with their area.
  for (const auto &[key, site] : sites_) {
    if (key.zone_key != channel.zone_key || key.channel != channel.channel) {
      continue;
    }
    auto updated = site;
    updated.contract_type = channel.contract_type;
    updated.expiry_epoch_ticks = channel.expiry_epoch_ticks;
    updated.version = site.version + 1;
    commit.sites.push_back({updated, site.version, false});
    result.outcome.contracts_changed.push_back(key);
  }

  if (!commit_locked(std::move(commit), access)) {
    result = GuildAreaResult{};
    result.ec = x5::ec::Housing_FailedContractExtension;
    return result;
  }

  result.channel = channel.channel;
  result.expiry_epoch_ticks = channel.expiry_epoch_ticks;
  result.contract_type = channel.contract_type;
  result.area_name = channel.area_name;
  result.zone_key = channel.zone_key;
  result.outcome.guild_areas.push_back(
      {membership.guild_id, channel.zone_key, channel.channel, true});
  return result;
}

GuildResidentsResult HousingService::guild_board(const HousingActor &actor, GuildId guild_id) {
  std::lock_guard lock(mutex_);
  GuildResidentsResult result;
  GuildMembership membership;
  result.ec = guild_permission(actor, guild_id, GuildGrade::member, membership);
  if (!is_success(result.ec)) {
    return result;
  }

  const auto area = guild_area_locked(membership.guild_id);
  if (!area) {
    result.ec = x5::ec::Housing_ChannelNotFound;
    return result;
  }
  for (const auto &[lot_number, resident] : area->residents) {
    result.residents.push_back(resident);
  }
  return result;
}

GuildResidentsResult HousingService::guild_allow(const HousingActor &actor, GuildId guild_id,
                                                 const std::vector<GuildResident> &allow,
                                                 const std::vector<GuildResident> &remove) {
  std::lock_guard lock(mutex_);
  GuildResidentsResult result;
  GuildMembership membership;
  result.ec = guild_permission(actor, guild_id, GuildGrade::sub_master, membership);
  if (!is_success(result.ec)) {
    return result;
  }

  const auto area = guild_area_locked(membership.guild_id);
  if (!area) {
    result.ec = x5::ec::Housing_ChannelNotFound;
    return result;
  }

  const auto members = guilds_->members(membership.guild_id);
  const auto member_of = [&](CharacterId character_id) -> const GuildMember * {
    const auto found = std::ranges::find_if(
        members, [&](const GuildMember &member) { return member.character_id == character_id; });
    return found == members.end() ? nullptr : &*found;
  };

  const auto now = now_();
  const auto cooldown_ticks =
      static_cast<std::int64_t>(catalog_->rules().guild_site_edit_cooldown.count()) *
      kTicksPerMinute;
  auto channel = *area;
  HousingCommit commit;
  const auto next_package_before = next_package_id_;

  for (const auto &removed : remove) {
    const auto resident = channel.residents.find(removed.lot_number);
    if (resident == channel.residents.end()) {
      result.ec = x5::ec::Housing_NotRegisteredHousingSite;
      next_package_id_ = next_package_before;
      return result;
    }
    const auto site = sites_.find(SiteKey{channel.zone_key, channel.channel, removed.lot_number});
    if (site != sites_.end()) {
      pack_owned_site_locked(site->second, PackageReason::guild_shrink, commit, result.outcome);
    }
    result.outcome.guild_sites.push_back(
        {resident->second.character_id,
         SiteKey{channel.zone_key, channel.channel, removed.lot_number},
         GuildHousingAction::remove});
    // Keep the lot's cooldown so a freed lot cannot be reassigned at once.
    GuildResident freed;
    freed.lot_number = removed.lot_number;
    freed.allow_cooldown_start_ticks = now;
    channel.residents[removed.lot_number] = freed;
  }

  for (const auto &allowed : allow) {
    const auto *definition = catalog_->site(allowed.lot_number);
    if (!definition || definition->zone_key != channel.zone_key) {
      result.ec = x5::ec::Housing_InvalidArea;
      next_package_id_ = next_package_before;
      return result;
    }
    const auto *member = member_of(allowed.character_id);
    if (!member) {
      result.ec = x5::ec::Guild_NotMember;
      next_package_id_ = next_package_before;
      return result;
    }
    // GameVariable GuildHousingSite{S,M,L}positionLv: the resident's company position must
    // reach the lot size's level.
    const auto required_position = catalog_->rules().guild_position_level.find(definition->type);
    if (required_position != catalog_->rules().guild_position_level.end() &&
        member->company_position_level < required_position->second) {
      result.ec = x5::ec::Housing_InsufficientCompanyPosition;
      next_package_id_ = next_package_before;
      return result;
    }
    const SiteKey key{channel.zone_key, channel.channel, allowed.lot_number};
    const bool lot_taken = sites_.contains(key) && std::ranges::none_of(commit.sites, [&](auto &w) {
                             return w.erase && w.record.key == key;
                           });
    if (lot_taken) {
      result.ec = x5::ec::Housing_AlreadyUsedArea;
      next_package_id_ = next_package_before;
      return result;
    }
    const bool member_has_site = std::ranges::any_of(sites_, [&](const auto &entry) {
      return entry.second.authority == AuthorityType::guild &&
             entry.second.owner_character_id == allowed.character_id;
    });
    if (member_has_site) {
      result.ec = x5::ec::Housing_CanHaveOnlyOneHousingSite;
      next_package_id_ = next_package_before;
      return result;
    }
    const auto previous = channel.residents.find(allowed.lot_number);
    if (previous != channel.residents.end() &&
        previous->second.allow_cooldown_start_ticks != 0 &&
        now - previous->second.allow_cooldown_start_ticks < cooldown_ticks) {
      result.ec = x5::ec::Housing_GuildSiteAssignCooldown;
      next_package_id_ = next_package_before;
      return result;
    }

    HousingActor resident_actor;
    resident_actor.account_id = member->account_id;
    resident_actor.character_id = member->character_id;
    resident_actor.world_id = world_id_;
    resident_actor.name = member->name;
    auto site = new_site_locked(resident_actor, key, *definition, channel.contract_type, now);
    site.authority = AuthorityType::guild;
    site.guild_id = membership.guild_id;
    site.expiry_epoch_ticks = channel.expiry_epoch_ticks;
    site.return_position.reset();
    site.version = 1;
    commit.sites.push_back({site, 0, false});

    GuildResident resident;
    resident.lot_number = allowed.lot_number;
    resident.character_id = member->character_id;
    resident.name = member->name;
    resident.allow_cooldown_start_ticks = now;
    channel.residents[allowed.lot_number] = resident;
    result.outcome.sites_added.push_back(key);
    result.outcome.guild_sites.push_back({member->character_id, key, GuildHousingAction::allow});
  }

  channel.version = area->version + 1;
  commit.channels.push_back({channel, area->version, false});
  if (!commit_locked(std::move(commit), nullptr)) {
    next_package_id_ = next_package_before;
    return GuildResidentsResult{x5::ec::Housing_RetryContract, {}, {}};
  }

  for (const auto &[lot_number, resident] : channel.residents) {
    if (resident.character_id != 0) {
      result.residents.push_back(resident);
    }
  }
  result.outcome.guild_areas.push_back(
      {membership.guild_id, channel.zone_key, channel.channel, false});
  return result;
}

GuildAssignmentResult HousingService::guild_assignment(const HousingActor &actor,
                                                       GuildId guild_id) {
  std::lock_guard lock(mutex_);
  GuildAssignmentResult result;
  GuildMembership membership;
  result.ec = guild_permission(actor, guild_id, GuildGrade::sub_master, membership);
  if (!is_success(result.ec)) {
    return result;
  }

  for (const auto &member : guilds_->members(membership.guild_id)) {
    const bool has_site = std::ranges::any_of(sites_, [&](const auto &entry) {
      return entry.second.authority == AuthorityType::guild &&
             entry.second.owner_character_id == member.character_id;
    });
    if (has_site) {
      result.characters_with_sites.push_back(member.character_id);
    }
  }
  return result;
}

SimpleResult HousingService::guild_member_left(GuildId guild_id, CharacterId character_id,
                                               const ProgressAccess *access, bool kicked) {
  std::lock_guard lock(mutex_);
  // Departures are delivered after the guild change commits. A character who is a member
  // again (a rejoin, or a replayed departure) keeps the site assigned since.
  const auto membership = guilds_->membership(character_id);
  if (membership && membership->guild_id == guild_id) {
    return SimpleResult{};
  }

  const auto action = kicked ? GuildHousingAction::remove : GuildHousingAction::leave;
  const auto area = guild_area_locked(guild_id);
  if (!area) {
    return release_unlisted_guild_site_locked(guild_id, character_id, action, access);
  }
  const auto resident = std::ranges::find_if(area->residents, [&](const auto &entry) {
    return entry.second.character_id == character_id;
  });
  if (resident == area->residents.end()) {
    return release_unlisted_guild_site_locked(guild_id, character_id, action, access);
  }

  SimpleResult result;
  HousingCommit commit;
  const auto next_package_before = next_package_id_;
  const SiteKey key{area->zone_key, area->channel, resident->first};
  const auto site = sites_.find(key);
  if (site != sites_.end()) {
    if (site->second.authority != AuthorityType::guild || site->second.guild_id != guild_id ||
        site->second.owner_character_id != character_id) {
      return SimpleResult{x5::ec::Housing_RetryContract, {}};
    }
    HousingActor owner;
    owner.account_id = site->second.owner_account_id;
    owner.character_id = site->second.owner_character_id;
    owner.world_id = world_id_;
    const auto before = load_progress_locked(owner, access);
    auto after = before;
    pack_site_locked(site->second, PackageReason::guild_shrink, commit, after, true,
                     result.outcome);
    if (after != before) {
      after.revision = before.revision + 1;
      commit.progress.push_back(
          {owner.account_id, owner.character_id, world_id_, before, after});
      result.outcome.progress_changes.push_back({owner.character_id, before, after});
    }
  }

  result.outcome.guild_sites.push_back({character_id, key, action});
  auto channel = *area;
  GuildResident freed;
  freed.lot_number = resident->first;
  freed.allow_cooldown_start_ticks = now_();
  channel.residents[resident->first] = freed;
  channel.version = area->version + 1;
  commit.channels.push_back({channel, area->version, false});
  if (!commit_locked(std::move(commit), access)) {
    next_package_id_ = next_package_before;
    return SimpleResult{x5::ec::Housing_RetryContract, {}};
  }

  edit_sessions_.erase(character_id);
  result.outcome.guild_areas.push_back({guild_id, channel.zone_key, channel.channel, false});
  return result;
}

SimpleResult HousingService::release_unlisted_guild_site_locked(GuildId guild_id,
                                                                CharacterId character_id,
                                                                GuildHousingAction action,
                                                                const ProgressAccess *access) {
  // An earlier partial cleanup, or the area's own removal, can leave a guild site that no
  // resident entry lists. It still belongs to the departed character and must be packed.
  const auto unlisted = std::ranges::find_if(sites_, [&](const auto &entry) {
    const auto &site = entry.second;
    return site.authority == AuthorityType::guild && site.guild_id == guild_id &&
           site.owner_character_id == character_id;
  });
  if (unlisted == sites_.end()) {
    return SimpleResult{};
  }

  const auto key = unlisted->first;
  auto result = pack_and_commit_locked(unlisted->second, PackageReason::guild_shrink, access);
  if (!is_success(result.ec)) {
    return result;
  }

  result.outcome.guild_sites.push_back({character_id, key, action});
  const auto area = guild_area_locked(guild_id);
  if (area && area->zone_key == key.zone_key && area->channel == key.channel) {
    result.outcome.guild_areas.push_back({guild_id, key.zone_key, key.channel, false});
  }
  return result;
}

SimpleResult HousingService::guild_disbanded(GuildId guild_id) {
  std::lock_guard lock(mutex_);
  // A replayed or misdirected disband must not remove a live guild's area.
  if (guilds_->guild_exists(guild_id).value_or(false)) {
    return SimpleResult{};
  }

  const auto area = guild_area_locked(guild_id);
  if (!area) {
    return SimpleResult{};
  }

  SimpleResult result;
  HousingCommit commit;
  const auto next_package_before = next_package_id_;
  for (const auto &[key, site] : sites_) {
    if (key.zone_key != area->zone_key || key.channel != area->channel) {
      continue;
    }
    pack_owned_site_locked(site, PackageReason::guild_shrink, commit, result.outcome);
    result.outcome.guild_sites.push_back(
        {site.owner_character_id, key, GuildHousingAction::disband});
  }
  commit.channels.push_back({*area, area->version, true});
  if (!commit_locked(std::move(commit), nullptr)) {
    next_package_id_ = next_package_before;
    return SimpleResult{x5::ec::Housing_RetryContract, {}};
  }
  spdlog::info("Guild housing area removed with its guild guild={} zone={} channel={}",
               guild_id, area->zone_key, area->channel);
  return result;
}

HousingService::StaleGuildHousing HousingService::stale_guild_housing() const {
  std::lock_guard lock(mutex_);
  std::map<GuildId, std::optional<bool>> existence;
  const auto guild_exists = [&](GuildId guild_id) {
    auto known = existence.find(guild_id);
    if (known == existence.end()) {
      known = existence.emplace(guild_id, guilds_->guild_exists(guild_id)).first;
    }
    return known->second;
  };

  std::set<DepartedGuildResident> departed;
  const auto check_member = [&](GuildId guild_id, CharacterId character_id) {
    const auto membership = guilds_->membership(character_id);
    if (!membership || membership->guild_id != guild_id) {
      departed.insert({guild_id, character_id});
    }
  };

  StaleGuildHousing stale;
  for (const auto &[key, channel] : channels_) {
    if (channel.guild_id == 0) {
      continue;
    }

    const auto exists = guild_exists(channel.guild_id);
    if (!exists.has_value()) {
      continue;
    }
    if (!*exists) {
      stale.disbanded_guilds.push_back(channel.guild_id);
      continue;
    }
    for (const auto &[lot_number, resident] : channel.residents) {
      if (resident.character_id != 0) {
        check_member(channel.guild_id, resident.character_id);
      }
    }
  }

  for (const auto &[key, site] : sites_) {
    if (site.authority != AuthorityType::guild || site.guild_id == 0) {
      continue;
    }

    const auto exists = guild_exists(site.guild_id);
    if (!exists.has_value()) {
      continue;
    }
    if (*exists) {
      check_member(site.guild_id, site.owner_character_id);
    } else if (!guild_area_locked(site.guild_id)) {
      // Sites inside a disbanded guild's area are packed with the area; this one has none.
      departed.insert({site.guild_id, site.owner_character_id});
    }
  }

  stale.departed_residents.assign(departed.begin(), departed.end());
  return stale;
}

SimpleResult HousingService::gm_release(int lot_number, int channel) {
  std::lock_guard lock(mutex_);
  const auto *definition = catalog_->site(lot_number);
  if (!definition) {
    return SimpleResult{x5::ec::Housing_InvalidArea, {}};
  }
  const auto site = sites_.find(SiteKey{definition->zone_key, channel, lot_number});
  if (site == sites_.end()) {
    return SimpleResult{x5::ec::Housing_NotRegisteredHousingSite, {}};
  }
  return pack_and_commit_locked(site->second, PackageReason::cheat, nullptr);
}

SimpleResult HousingService::gm_set_unlock(const HousingActor &actor, SiteType type, bool guild,
                                           bool grant) {
  std::lock_guard lock(mutex_);
  auto character = characters_.contains(actor.character_id)
                       ? characters_.at(actor.character_id)
                       : CharacterHousingRecord{actor.character_id};
  const auto previous_version = character.version;
  if (guild) {
    character.guild_unlocked = grant;
  } else if (type == SiteType::none) {
    return SimpleResult{x5::ec::Housing_InvalidArea, {}};
  } else if (grant) {
    character.unlocked_site_types.insert(type);
  } else {
    character.unlocked_site_types.erase(type);
  }

  character.version = previous_version + 1;
  HousingCommit commit;
  commit.characters.push_back({character, previous_version, false});
  if (!commit_locked(std::move(commit), nullptr)) {
    return SimpleResult{x5::ec::Housing_RetryContract, {}};
  }
  return SimpleResult{};
}

SimpleResult HousingService::gm_set_remaining_minutes(const HousingActor &actor, int minutes,
                                                      ContractType contract_type) {
  std::lock_guard lock(mutex_);
  const auto *current = personal_site_locked(actor.character_id);
  if (!current) {
    return SimpleResult{x5::ec::Housing_NotRegisteredHousingSite, {}};
  }
  if (minutes < 0) {
    return SimpleResult{x5::ec::BadRequest, {}};
  }

  auto site = *current;
  site.expiry_epoch_ticks = now_() + static_cast<std::int64_t>(minutes) * kTicksPerMinute;
  if (contract_type != ContractType::none) {
    site.contract_type = contract_type;
  }
  site.version = current->version + 1;
  HousingCommit commit;
  commit.sites.push_back({site, current->version, false});
  if (!commit_locked(std::move(commit), nullptr)) {
    return SimpleResult{x5::ec::Housing_RetryContract, {}};
  }

  SimpleResult result;
  result.outcome.contracts_changed.push_back(site.key);
  result.outcome.owner_sites_changed.push_back(actor.character_id);
  return result;
}

SimpleResult HousingService::gm_contract_guild(const HousingActor &actor, int lot_number) {
  GuildId guild_id{};
  {
    std::lock_guard lock(mutex_);
    GuildMembership membership;
    const auto error = guild_permission(actor, 0, GuildGrade::member, membership);
    if (!is_success(error)) {
      return SimpleResult{error, {}};
    }
    guild_id = membership.guild_id;
  }

  // A GM assignment acts as the guild master's allow request for the GM's own character.
  GuildResident resident;
  resident.lot_number = lot_number;
  resident.character_id = actor.character_id;
  resident.name = actor.name;
  HousingActor master = actor;
  for (const auto &member : guilds_->members(guild_id)) {
    if (member.grade == GuildGrade::master) {
      master.account_id = member.account_id;
      master.character_id = member.character_id;
      master.name = member.name;
      break;
    }
  }
  auto allowed = guild_allow(master, guild_id, {resident}, {});
  return SimpleResult{allowed.ec, std::move(allowed.outcome)};
}

SimpleResult HousingService::gm_release_guild(int lot_number, int channel) {
  std::lock_guard lock(mutex_);
  const auto *definition = catalog_->site(lot_number);
  if (!definition) {
    return SimpleResult{x5::ec::Housing_InvalidArea, {}};
  }
  const auto site = sites_.find(SiteKey{definition->zone_key, channel, lot_number});
  if (site == sites_.end() || site->second.authority != AuthorityType::guild) {
    return SimpleResult{x5::ec::Housing_NotRegisteredHousingSite, {}};
  }
  return pack_and_commit_locked(site->second, PackageReason::cheat, nullptr);
}

std::vector<HousingService::DueSite> HousingService::due_sites() const {
  std::lock_guard lock(mutex_);
  const auto now = now_();
  std::vector<DueSite> due;
  for (const auto &[key, site] : sites_) {
    // Guild sites follow their area.
    if (site.authority == AuthorityType::individual && site.expiry_epoch_ticks <= now) {
      due.push_back({key, site.owner_character_id, site.owner_account_id});
    }
  }
  return due;
}

std::vector<HousingService::DueArea> HousingService::due_areas() const {
  std::lock_guard lock(mutex_);
  const auto now = now_();
  std::vector<DueArea> due;
  for (const auto &[key, channel] : channels_) {
    if (channel.guild_id == 0 || channel.expiry_epoch_ticks > now) {
      continue;
    }
    DueArea area{key.first, key.second, channel.guild_id};
    for (const auto &member : guilds_->members(channel.guild_id)) {
      if (member.grade == GuildGrade::master) {
        area.master_character_id = member.character_id;
        area.master_account_id = member.account_id;
        break;
      }
    }
    due.push_back(area);
  }
  return due;
}

SimpleResult HousingService::expire_site(const SiteKey &key, const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  const auto found = sites_.find(key);
  if (found == sites_.end() || found->second.expiry_epoch_ticks > now_()) {
    return SimpleResult{};
  }

  const auto site = found->second;
  if (site.contract_type == ContractType::periodic) {
    const auto *definition = catalog_->site(key.lot_number);
    const auto *cost =
        definition ? catalog_->rent_cost(AuthorityType::individual, definition->type) : nullptr;
    HousingActor owner;
    owner.account_id = site.owner_account_id;
    owner.character_id = site.owner_character_id;
    owner.world_id = world_id_;
    if (cost) {
      const auto before = load_progress_locked(owner, access);
      auto after = before;
      if (is_success(charge_rent(*cost, after))) {
        after.revision = before.revision + 1;
        auto renewed = site;
        renewed.expiry_epoch_ticks =
            site.expiry_epoch_ticks + rent_duration_ticks(AuthorityType::individual);
        renewed.version = site.version + 1;
        HousingCommit commit;
        commit.progress.push_back(
            {owner.account_id, owner.character_id, world_id_, before, after});
        commit.sites.push_back({renewed, site.version, false});
        if (commit_locked(std::move(commit), access)) {
          SimpleResult result;
          result.outcome.contracts_changed.push_back(key);
          result.outcome.owner_sites_changed.push_back(owner.character_id);
          result.outcome.progress_changes.push_back({owner.character_id, before, after});
          return result;
        }
        // A revision conflict is not proof that the owner cannot pay. Keep the lease and
        // retry against fresh progress; packing here could destroy a renewable contract.
        return SimpleResult{x5::ec::Housing_RetryContract, {}};
      }
    }
  }

  return pack_and_commit_locked(site, PackageReason::lease_expired, access);
}

SimpleResult HousingService::expire_area(int zone_key, int channel_number,
                                         const ProgressAccess *access) {
  std::lock_guard lock(mutex_);
  const auto found = channels_.find({zone_key, channel_number});
  if (found == channels_.end() || found->second.guild_id == 0 ||
      found->second.expiry_epoch_ticks > now_()) {
    return SimpleResult{};
  }

  const auto area = found->second;
  if (area.contract_type == ContractType::periodic) {
    const auto *cost = catalog_->rent_cost(AuthorityType::guild, SiteType::none);
    std::optional<GuildMember> master;
    for (const auto &member : guilds_->members(area.guild_id)) {
      if (member.grade == GuildGrade::master) {
        master = member;
        break;
      }
    }
    if (cost && master) {
      HousingActor payer;
      payer.account_id = master->account_id;
      payer.character_id = master->character_id;
      payer.world_id = world_id_;
      const auto before = load_progress_locked(payer, access);
      auto after = before;
      if (is_success(charge_rent(*cost, after))) {
        after.revision = before.revision + 1;
        auto renewed = area;
        renewed.expiry_epoch_ticks =
            area.expiry_epoch_ticks + rent_duration_ticks(AuthorityType::guild);
        renewed.version = area.version + 1;
        HousingCommit commit;
        commit.progress.push_back(
            {payer.account_id, payer.character_id, world_id_, before, after});
        commit.channels.push_back({renewed, area.version, false});
        SimpleResult result;
        for (const auto &[key, site] : sites_) {
          if (key.zone_key != zone_key || key.channel != channel_number) {
            continue;
          }
          auto updated = site;
          updated.expiry_epoch_ticks = renewed.expiry_epoch_ticks;
          updated.version = site.version + 1;
          commit.sites.push_back({updated, site.version, false});
          result.outcome.contracts_changed.push_back(key);
        }
        if (commit_locked(std::move(commit), access)) {
          result.outcome.progress_changes.push_back({payer.character_id, before, after});
          result.outcome.guild_areas.push_back({area.guild_id, zone_key, channel_number, true});
          return result;
        }
        // A conflicting renewal must not be treated as an unpaid guild contract.
        return SimpleResult{x5::ec::Housing_RetryContract, {}};
      }
    }
  }

  // The area ends: every guild site in it is packed for its resident.
  SimpleResult result;
  HousingCommit commit;
  const auto next_package_before = next_package_id_;
  for (const auto &[key, site] : sites_) {
    if (key.zone_key != zone_key || key.channel != channel_number) {
      continue;
    }
    pack_owned_site_locked(site, PackageReason::lease_expired, commit, result.outcome);
    result.outcome.guild_sites.push_back({site.owner_character_id, key,
                                          GuildHousingAction::expired});
  }
  commit.channels.push_back({area, area.version, true});
  if (!commit_locked(std::move(commit), nullptr)) {
    next_package_id_ = next_package_before;
    return SimpleResult{x5::ec::Housing_RetryContract, {}};
  }
  result.outcome.guild_areas.push_back({area.guild_id, zone_key, channel_number, false});
  spdlog::info("Guild housing area expired guild={} zone={} channel={}", area.guild_id,
               zone_key, channel_number);
  return result;
}

} // namespace chronicles::housing
