#include "services/social/character_social.hpp"

#include "world/features/company/company.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/housing/guild_housing.hpp"
#include "world/features/housing/housing_service.hpp"

#include <utility>

namespace chronicles {

CharacterSocialDirectory::CharacterSocialDirectory(
    std::shared_ptr<const CompanyService> companies, std::shared_ptr<GuildService> guilds,
    std::shared_ptr<const housing::HousingService> housing)
    : companies_(std::move(companies)), guilds_(std::move(guilds)), housing_(std::move(housing)) {}

void CharacterSocialDirectory::decorate_lobby(std::int64_t char_id,
                                              x5::pb::LobbyCharacter &character) const {
  if (!companies_) {
    return;
  }

  const auto company = companies_->status(char_id);
  if (company.record.company_type == kCompanyNone) {
    return;
  }
  character.set_company_type(static_cast<x5::cms::EX5CompanyType>(company.record.company_type));
  character.set_company_contribution(company.record.contribution);
}

void CharacterSocialDirectory::decorate_world(std::int64_t char_id,
                                              x5::pb::WorldCharacter &character) const {
  if (companies_) {
    const auto company = companies_->status(char_id);
    if (company.record.company_type != kCompanyNone) {
      *character.mutable_company() = world_char_company(company.record);
    }
  }

  if (!guilds_) {
    return;
  }

  const auto status = guilds_->status(char_id);
  const bool has_history = status.last_leave_epoch_ticks != 0 ||
                           status.last_disband_epoch_ticks != 0;
  if (status.guild_id == 0 && !has_history) {
    return;
  }

  auto *guild = character.mutable_guild();
  guild->set_guild_id(status.guild_id);
  guild->set_last_guild_leave_epochtime(status.last_leave_epoch_ticks);
  guild->set_last_guild_disband_epochtime(status.last_disband_epoch_ticks);
  if (status.guild_id == 0) {
    return;
  }
  guild->set_member_grade(static_cast<x5::cms::EX5GuildMemberGrade>(status.grade));
  guild->set_guild_level(status.guild_level);
  guild->set_guild_name(status.guild_name);

  if (!housing_) {
    return;
  }
  const auto area = housing_->guild_area(status.guild_id);
  if (area && area->expiry_epoch_ticks > 0) {
    guild->set_housing_channel(area->channel);
  }
  const auto site = housing_->guild_site_of(char_id);
  if (site) {
    *character.mutable_housing_site() = guild_housing::guild_housing_site(site->key, site, area);
  }
}

CharacterDeletionCheck CharacterSocialDirectory::deletion_check(std::int64_t char_id) const {
  CharacterDeletionCheck check;
  if (housing_ && housing_->guild_site_of(char_id)) {
    check.block = x5::ec::CharDeleteHavingHousing;
    check.block_reasons.push_back(x5::pb::frontwebsocket::CHAR_DELETE_BLOCK_HOUSING);
  }

  if (guilds_) {
    const auto status = guilds_->status(char_id);
    if (status.grade == GuildGrade::master) {
      check.block = x5::ec::CharDeleteGuildMaster;
      check.block_reasons.push_back(x5::pb::frontwebsocket::CHAR_DELETE_BLOCK_GUILD_MASTER);
    } else if (status.guild_id != 0) {
      // Members have no block reason enum value; the deletion itself is refused.
      check.block = x5::ec::CharDeleteGuildMember;
    }
  }
  return check;
}

} // namespace chronicles
