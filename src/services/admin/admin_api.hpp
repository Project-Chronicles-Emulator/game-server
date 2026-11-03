#pragma once

// Operator console HTTP API (/admin/v1/*). Emulator-owned management interface: it is not a
// recovered official service and no game client calls it. See docs/admin-console.md.

#include "services/admin/admin_log_buffer.hpp"
#include "auth/auth.hpp"
#include "persistence/character_repositories.hpp"
#include "world/core/game_rates.hpp"
#include "world/features/chat/native_chat.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "world/session/native_world.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles {

class CompanyService;
class GuildService;
class ServerMessages;
class ShopCatalog;

inline constexpr std::string_view kAdminRoutePrefix = "/admin/v1/";
inline constexpr std::size_t kAdminTokenBytes = 32;
inline constexpr std::size_t kMaximumAdminBodyBytes = 16384;
inline constexpr std::size_t kMaximumAdminNoticeBytes = 512;
inline constexpr std::size_t kMaximumAdminListRows = 5000;

// Facts about this server process shown on the console dashboard. Contains no secrets.
struct AdminServerInfo {
  // Native client build the server answers (CHRONICLES_CLIENT_VERSION or the default).
  std::string client_version;
  std::string storage_backend;
  std::string listen_address;
  std::string public_address;
  unsigned http_port{};
  unsigned world_port{};
  bool native_client{};
  bool require_launch_grant{};
  bool world_bootstrap_diagnostic{};
  bool client_catalog_loaded{};
  std::int64_t started_at_unix{};
};

// Everything the API manages. Optional services may be null; their routes then report
// `service_unavailable` instead of failing.
struct AdminServices {
  AuthService &auth;
  CharacterRepositories repositories;
  std::shared_ptr<MailRepository> mail;
  GameRates &rates;
  std::shared_ptr<AdminLogBuffer> logs;
  std::shared_ptr<NativeWorldListener> world;
  NativeChatService *chat{};
  // Requests a graceful process stop, as SIGINT does.
  std::function<void()> request_shutdown;
  // Realm and rate changes are saved here; empty keeps them until restart only.
  std::filesystem::path settings_file;
  AdminServerInfo info;
  int world_id{1};
  std::shared_ptr<GuildService> guilds;
  std::shared_ptr<ServerMessages> server_messages;
  std::shared_ptr<const ShopCatalog> mail_catalog;
  std::shared_ptr<CompanyService> companies;
};

struct AdminHttpRequest {
  std::string_view method;
  // Path with an optional query string.
  std::string_view target;
  // Raw Authorization header value.
  std::string_view authorization;
  std::string_view body;
  bool peer_loopback{};
};

struct AdminHttpResponse {
  unsigned status{200};
  nlohmann::json body;
};

class AdminApi {
public:
  // `token` is the 64-hex operator bearer token. Unless allow_remote is set, requests from a
  // non-loopback peer are refused even with a valid token.
  AdminApi(AdminServices services, std::string token, bool allow_remote);

  static bool handles(std::string_view target);

  // Never throws: every failure becomes a JSON error response.
  AdminHttpResponse handle(const AdminHttpRequest &request);

  // Applies realm and rate values saved by an earlier process. A missing file is not an
  // error; an unreadable or invalid file throws, so a broken file is never silently ignored.
  void load_saved_settings();

private:
  using Query = std::map<std::string, std::string>;
  using Segments = std::vector<std::string_view>;

  AdminHttpResponse route(std::string_view method, const Segments &segments, const Query &query,
                          const nlohmann::json &body);

  nlohmann::json overview();

  nlohmann::json update_realm(const nlohmann::json &body);

  nlohmann::json list_accounts(const Query &query);

  nlohmann::json create_account(const nlohmann::json &body);

  nlohmann::json account_details(std::int64_t account_id);

  nlohmann::json update_account(std::int64_t account_id, std::string_view action,
                                const nlohmann::json &body);

  nlohmann::json list_characters(const Query &query);

  nlohmann::json character_details(std::int64_t character_id);

  nlohmann::json update_character(std::int64_t character_id, std::string_view action,
                                  const nlohmann::json &body);

  nlohmann::json list_players();

  nlohmann::json kick_player(std::uint64_t connection_id);

  nlohmann::json announce(const nlohmann::json &body);

  nlohmann::json rates_json();

  nlohmann::json update_rates(const nlohmann::json &body);

  nlohmann::json create_event(const nlohmann::json &body);

  nlohmann::json update_event(std::int64_t event_id, const nlohmann::json &body);

  nlohmann::json delete_event(std::int64_t event_id);

  nlohmann::json read_logs(const Query &query);

  // Recent World chat of this realm (player messages and operator notices), from memory.
  nlohmann::json read_chat(const Query &query);

  nlohmann::json set_log_level(const nlohmann::json &body);

  nlohmann::json shutdown(const nlohmann::json &body);

  bool authorized(std::string_view authorization) const;

  std::vector<WorldConnectionSummary> world_connections() const;

  Character require_character(std::int64_t character_id);

  // Writes realm settings and rates; returns false (and logs) when the file cannot be saved.
  bool save_settings();

  // Serializes mutating requests, including the settings snapshot and its atomic file
  // replacement, across HTTP workers. GET requests do not take it.
  std::mutex request_mutex_;
  AdminServices services_;
  std::string token_;
  bool allow_remote_{};
};

// Returns the token stored in `file`, or creates the file (and its directory) with a new
// random token. Throws when an existing file does not hold exactly 64 lowercase hex digits.
std::string load_or_create_admin_token(const std::filesystem::path &file);

nlohmann::json realm_settings_json(const RealmSettings &settings);

// Applies the fields present in `document` over `base` and validates the result.
RealmSettings realm_settings_from_json(const nlohmann::json &document, RealmSettings base);

} // namespace chronicles
