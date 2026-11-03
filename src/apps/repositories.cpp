#include "apps/repositories.hpp"

#include "persistence/character_repositories.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace chronicles {

std::string environment_value(const char *name) {
  auto *value = std::getenv(name);
  return value ? value : "";
}

Repositories create_repositories(const std::string &backend,
                                 const std::optional<std::string> &migration_file) {
  if (backend == "memory") {
    auto database = std::make_shared<MemoryDatabase>();
    spdlog::warn("Memory mode: accounts and characters disappear on shutdown");
    // The concrete memory repository backs the company-permit commit; the same instance
    // serves as the shared character-state repository over the one memory database.
    const auto company_progress = std::make_shared<MemoryCharacterStateRepository>(database);

    Repositories repositories;
    repositories.character = make_memory_character_repositories(database);
    repositories.character.character_state = company_progress;
    // The mail, hot-key, fog, market and ranking repositories share the same database; the
    // mail, market and fog repositories register their deletion hooks there, so a deleted
    // character's recipient mail still follows its removal, escrow still vetoes a deletion,
    // and a deleted character's areas still disappear.
    repositories.mail = std::make_shared<MemoryMailRepository>(database);
    repositories.hotkeys = std::make_shared<MemoryHotKeyRepository>(database);
    repositories.fog = std::make_shared<MemoryFogRepository>(database);
    repositories.market = std::make_shared<MemoryMarketRepository>(database);
    repositories.ranking = std::make_shared<MemoryRankingRepository>(database);
    repositories.companies =
        std::make_shared<MemoryCompanyStore>(std::move(company_progress), database->quest_services);
    repositories.guilds = std::make_shared<MemoryGuildStore>(database->quest_services);
    repositories.friends = std::make_shared<MemoryFriendStore>();
    repositories.housing =
        std::make_shared<housing::MemoryHousingStore>(repositories.character.character_state);
    return repositories;
  }

  if (backend == "postgres") {
    const auto connection_string = environment_value("CHRONICLES_DATABASE_URL");
    if (connection_string.empty()) {
      throw std::invalid_argument(
          "Set CHRONICLES_DATABASE_URL or explicitly choose --storage memory");
    }

    // The migration runs before any repository is created, so every table exists first.
    if (migration_file && !migration_file->empty()) {
      std::ifstream input(*migration_file);
      if (!input) {
        throw std::invalid_argument("Migration file cannot be opened");
      }
      migrate_postgres(connection_string,
                       std::string(std::istreambuf_iterator<char>(input), {}));
    }

    // Company, guild, friend and housing persistence follow the account storage backend;
    // their tables are part of the startup migration applied above.
    Repositories repositories;
    repositories.character = make_postgres_character_repositories(connection_string);
    repositories.mail = std::make_shared<PostgresMailRepository>(connection_string);
    repositories.hotkeys = std::make_shared<PostgresHotKeyRepository>(connection_string);
    repositories.fog = std::make_shared<PostgresFogRepository>(connection_string);
    repositories.market = std::make_shared<PostgresMarketRepository>(connection_string);
    repositories.ranking = std::make_shared<PostgresRankingRepository>(connection_string);
    repositories.companies = std::make_shared<PostgresCompanyStore>(connection_string);
    repositories.guilds = std::make_shared<PostgresGuildStore>(connection_string);
    repositories.friends = std::make_shared<PostgresFriendStore>(connection_string);
    repositories.housing = std::make_shared<housing::PostgresHousingStore>(connection_string);
    return repositories;
  }

  throw std::invalid_argument("Unknown storage backend");
}

} // namespace chronicles
