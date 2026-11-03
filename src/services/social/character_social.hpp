#pragma once

// AuthService's view of a character's company, guild and guild housing.
//
// CharacterSocialDirectory fills the company and guild parts of the lobby list and of
// CharSelectRes.character (WorldCharacter.company, guild and housing_site), and reports the
// recovered deletion blockers: a guild Master cannot delete (CharDeleteGuildMaster, block reason
// GUILD_MASTER), a member must leave first (CharDeleteGuildMember), and a character holding a
// guild site must give it up (CharDeleteHavingHousing, block reason HOUSING).

#include "auth/auth.hpp"

#include <memory>

namespace chronicles {

class CompanyService;
class GuildService;

namespace housing {
class HousingService;
}

class CharacterSocialDirectory final : public CharacterSocialState {
public:
  // Each service is optional; an absent one contributes nothing.
  CharacterSocialDirectory(std::shared_ptr<const CompanyService> companies,
                           std::shared_ptr<GuildService> guilds,
                           std::shared_ptr<const housing::HousingService> housing);

  void decorate_lobby(std::int64_t char_id, x5::pb::LobbyCharacter &character) const override;

  void decorate_world(std::int64_t char_id, x5::pb::WorldCharacter &character) const override;

  CharacterDeletionCheck deletion_check(std::int64_t char_id) const override;

private:
  std::shared_ptr<const CompanyService> companies_;
  std::shared_ptr<GuildService> guilds_;
  std::shared_ptr<const housing::HousingService> housing_;
};

} // namespace chronicles
