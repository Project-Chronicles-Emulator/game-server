#pragma once

// Every storage backend the server uses, built once from --storage. Replaces create_store and
// the create_*_store helpers; each service receives only the members it uses.

#include "persistence/character_repositories.hpp"
#include "world/features/company/company.hpp"
#include "world/features/events/ranking_repository.hpp"
#include "world/features/fog/fog_repository.hpp"
#include "world/features/friends/friends.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/hotkeys/hotkey_repository.hpp"
#include "world/features/housing/housing_state.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "world/features/market/market_repository.hpp"

#include <memory>
#include <optional>
#include <string>

namespace chronicles {

struct Repositories {
  CharacterRepositories character;
  std::shared_ptr<MarketRepository> market;
  std::shared_ptr<MailRepository> mail;
  std::shared_ptr<RankingRepository> ranking;
  std::shared_ptr<HotKeyRepository> hotkeys;
  std::shared_ptr<FogRepository> fog;
  std::shared_ptr<GuildStore> guilds;
  std::shared_ptr<FriendStore> friends;
  std::shared_ptr<CompanyStore> companies;
  std::shared_ptr<housing::HousingStore> housing;
};

// "memory" or "postgres" (CHRONICLES_DATABASE_URL; --migrate applied first). Throws
// std::invalid_argument for an unknown backend or a missing database URL, with the same
// messages create_store produced.
Repositories create_repositories(const std::string &backend,
                                 const std::optional<std::string> &migration_file);

// Environment variable value, or empty when unset.
std::string environment_value(const char *name);

} // namespace chronicles
