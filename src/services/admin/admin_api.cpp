#include "services/admin/admin_api.hpp"
#include "common/clock_ticks.hpp"
#include "world/features/company/company.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/mail/mail.hpp"
#include "auth/password_policy.hpp"
#include "world/features/chat/server_messages.hpp"
#include "data/shop_catalog.hpp"

#include "common/currency.hpp"
#include "common/json_fields.hpp"

#include <google/protobuf/wire_format_lite.h>
#include <openssl/crypto.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace chronicles {
namespace {

using Json = nlohmann::json;
namespace fields = json_fields;

constexpr std::string_view kBearerPrefix = "Bearer ";
constexpr std::string_view kDefaultNoticeSender = "[Server]";
constexpr int kSettingsFileVersion = 1;
constexpr std::size_t kMaximumSettingsFileBytes = 65536;
constexpr std::size_t kDefaultLogPage = 200;
constexpr std::size_t kDefaultChatPage = 200;
constexpr std::string_view kShutdownConfirmation = "shutdown";

// An expected failure with an HTTP status and a stable machine-readable code.
class AdminError : public std::runtime_error {
public:
  AdminError(unsigned status, std::string code, const std::string &message)
      : std::runtime_error(message), status_(status), code_(std::move(code)) {}

  unsigned status() const {
    return status_;
  }

  const std::string &code() const {
    return code_;
  }

private:
  unsigned status_{};
  std::string code_;
};

AdminError not_found(const std::string &what) {
  return AdminError(404, "not_found", what + " was not found");
}

AdminError unavailable(const std::string &service) {
  return AdminError(503, "service_unavailable", service + " is not running in this server");
}

AdminHttpResponse error_response(unsigned status, const std::string &code,
                                 const std::string &message) {
  return {status, Json{{"error", code}, {"message", message}}};
}

std::vector<std::string_view> split_path(std::string_view path) {
  std::vector<std::string_view> segments;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto end = path.find('/', start);
    const auto length = (end == std::string_view::npos ? path.size() : end) - start;
    if (length > 0) {
      segments.push_back(path.substr(start, length));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return segments;
}

int hex_value(char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

// application/x-www-form-urlencoded decoding: '+' is a space and %XX a byte.
std::string decode_query_component(std::string_view text) {
  std::string decoded;
  decoded.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (character == '+') {
      decoded.push_back(' ');
      continue;
    }

    const bool escape = character == '%' && index + 2 < text.size();
    if (escape) {
      const auto high = hex_value(text[index + 1]);
      const auto low = hex_value(text[index + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        continue;
      }
    }
    decoded.push_back(character);
  }
  return decoded;
}

std::map<std::string, std::string> parse_query(std::string_view query) {
  std::map<std::string, std::string> values;
  std::size_t start = 0;
  while (start < query.size()) {
    auto end = query.find('&', start);
    if (end == std::string_view::npos) {
      end = query.size();
    }

    const auto pair = query.substr(start, end - start);
    const auto separator = pair.find('=');
    if (!pair.empty()) {
      const auto key = pair.substr(0, separator);
      const auto value =
          separator == std::string_view::npos ? std::string_view{} : pair.substr(separator + 1);
      values[decode_query_component(key)] = decode_query_component(value);
    }
    start = end + 1;
  }
  return values;
}

template <typename Integer> Integer parse_identifier(std::string_view text, const char *what) {
  Integer value{};
  const auto *begin = text.data();
  const auto *end = text.data() + text.size();
  const auto [parsed_end, error] = std::from_chars(begin, end, value);
  if (error != std::errc() || parsed_end != end || value <= 0) {
    throw AdminError(400, "invalid_identifier", std::string(what) + " must be a positive integer");
  }
  return value;
}

// Optional non-negative integer query parameter, such as a poll cursor or page size.
template <typename Integer>
Integer query_unsigned(const std::map<std::string, std::string> &query, const char *key,
                       Integer fallback) {
  const auto found = query.find(key);
  if (found == query.end() || found->second.empty()) {
    return fallback;
  }

  Integer value{};
  const auto &text = found->second;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) {
    throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
  }
  return value;
}

std::string lowercase(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return text;
}

// Recovered enum names without their prefix, lowercased: ServerLabel_New -> "new".
std::string enum_short_name(const std::string &full_name, std::string_view prefix) {
  if (full_name.starts_with(prefix)) {
    return lowercase(full_name.substr(prefix.size()));
  }
  return lowercase(full_name);
}

std::string label_name(x5::pb::ServerLabel label) {
  return enum_short_name(x5::pb::ServerLabel_Name(label), "ServerLabel_");
}

x5::pb::ServerLabel parse_label(const std::string &name) {
  const auto *descriptor = x5::pb::ServerLabel_descriptor();
  const auto wanted = lowercase(name);
  for (int index = 0; index < descriptor->value_count(); ++index) {
    const auto *value = descriptor->value(index);
    if (enum_short_name(value->name(), "ServerLabel_") == wanted) {
      return static_cast<x5::pb::ServerLabel>(value->number());
    }
  }
  throw std::invalid_argument("label must be none, new or recommended");
}

std::string class_name(int class_id) {
  if (!x5::cms::EX5CharacterClass_IsValid(class_id)) {
    return "unknown";
  }
  const auto character_class = static_cast<x5::cms::EX5CharacterClass>(class_id);
  return enum_short_name(x5::cms::EX5CharacterClass_Name(character_class), "EX5CharacterClass_");
}

Json position_json(const std::optional<WorldPosition> &position) {
  if (!position) {
    return nullptr;
  }
  return {{"map_id", position->map_id},
          {"x_mm", position->x_mm},
          {"y_mm", position->y_mm},
          {"z_mm", position->z_mm},
          {"yaw", position->yaw}};
}

// Operator text reaches clients as protobuf strings: valid UTF-8, no control characters.
void validate_notice_text(const std::string &text) {
  if (text.empty() || text.size() > kMaximumAdminNoticeBytes) {
    throw std::invalid_argument("Notice text must be 1-" +
                                std::to_string(kMaximumAdminNoticeBytes) + " bytes");
  }

  using WireFormat = google::protobuf::internal::WireFormatLite;
  if (!WireFormat::VerifyUtf8String(text.data(), static_cast<int>(text.size()), WireFormat::PARSE,
                                    "admin.notice")) {
    throw std::invalid_argument("Notice text must be valid UTF-8");
  }
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7F) {
      throw std::invalid_argument("Notice text must not contain control characters");
    }
  }
}

std::optional<SystemNoticeStyle> parse_notice_style(const std::string &name) {
  if (name == "none" || name.empty()) {
    return std::nullopt;
  }
  if (name == "text") {
    return SystemNoticeStyle::text;
  }
  if (name == "banner") {
    return SystemNoticeStyle::banner;
  }
  if (name == "popup") {
    return SystemNoticeStyle::popup;
  }
  throw std::invalid_argument("system_notice must be none, text, banner or popup");
}

// spdlog's names, restricted to the documented set so a typo is not read as "off".
std::optional<spdlog::level::level_enum> parse_log_level(const std::string &name) {
  static const std::map<std::string, spdlog::level::level_enum> kLevels{
      {"trace", spdlog::level::trace}, {"debug", spdlog::level::debug},
      {"info", spdlog::level::info},   {"warning", spdlog::level::warn},
      {"error", spdlog::level::err},   {"critical", spdlog::level::critical},
      {"off", spdlog::level::off}};
  const auto found = kLevels.find(lowercase(name));
  if (found == kLevels.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::string log_level_name(spdlog::level::level_enum level) {
  const auto name = spdlog::level::to_string_view(level);
  return std::string(name.data(), name.size());
}

bool contains_case_insensitive(const std::string &text, const std::string &lowered_needle) {
  return lowered_needle.empty() || lowercase(text).find(lowered_needle) != std::string::npos;
}

Json account_summary_json(const AccountSummary &account, unsigned lobby_sessions,
                          unsigned online_characters) {
  return {{"id", account.id},
          {"name", account.name},
          {"role", std::string(account_role_name(account.role))},
          {"banned", account.banned},
          {"character_count", account.character_count},
          {"created_at", account.created_at},
          {"lobby_sessions", lobby_sessions},
          {"online_characters", online_characters}};
}

Json character_summary_json(const Character &character, const std::string &account_name,
                            const WorldConnectionSummary *connection) {
  Json summary{{"id", character.id},
               {"name", character.name},
               {"account_id", character.account_id},
               {"account_name", account_name},
               {"world_id", character.world_id},
               {"slot", character.slot},
               {"class_id", character.class_id},
               {"class_name", class_name(character.class_id)},
               {"level", character.level},
               {"experience", character.experience},
               {"saved_position", position_json(character.world_position)},
               {"online", connection != nullptr},
               {"connection_id", nullptr}};
  if (connection) {
    summary["connection_id"] = connection->connection_id;
  }
  return summary;
}

Json item_json(const CharacterItem &item) {
  Json equip_slot = nullptr;
  if (item.equip_slot) {
    equip_slot = *item.equip_slot;
  }
  return {{"id", item.id},
          {"cms_id", item.cms_id},
          {"inventory_slot", item.inventory_slot},
          {"equip_slot", equip_slot},
          {"quantity", item.quantity},
          {"durability", item.durability},
          {"grade", item.grade}};
}

Json progress_json(const WorldProgress &progress) {
  auto stackables = Json::array();
  for (const auto &[cms_id, count] : progress.stackables) {
    stackables.push_back({{"cms_id", cms_id}, {"count", count}});
  }

  int completed_quests = 0;
  int quests_in_progress = 0;
  for (const auto &[quest_id, quest] : progress.quests) {
    if (quest.complete) {
      ++completed_quests;
    } else if (!quest.abandoned) {
      ++quests_in_progress;
    }
  }

  return {{"revision", progress.revision},
          {"dead", progress.dead},
          {"gold", progress.gold},
          {"hersander_token", progress.hersander_token},
          {"stackables", std::move(stackables)},
          {"quests_completed", completed_quests},
          {"quests_in_progress", quests_in_progress},
          {"adventure_objects", progress.adventure_objects.size()}};
}

Json connection_json(const WorldConnectionSummary &connection, std::int64_t now_unix) {
  return {{"connection_id", connection.connection_id},
          {"remote_address", connection.remote_address},
          {"connected_at_unix", connection.connected_at_unix},
          {"connected_seconds", std::max<std::int64_t>(0, now_unix - connection.connected_at_unix)},
          {"authenticated", connection.authenticated},
          {"account_id", connection.account_id},
          {"character_id", connection.character_id},
          {"game_master", connection.game_master},
          {"position", position_json(connection.position)}};
}

std::int64_t validated_money(const Json &body, const char *field, std::int64_t current) {
  const auto value = fields::optional_integer<std::int64_t>(body, field);
  if (!value) {
    return current;
  }
  if (*value < 0 || *value > kMoneyLimit) {
    throw std::invalid_argument(std::string(field) + " must be between 0 and " +
                                std::to_string(kMoneyLimit));
  }
  return *value;
}

Json parse_body(std::string_view body) {
  if (body.size() > kMaximumAdminBodyBytes) {
    throw AdminError(413, "body_too_large", "Request body exceeds the admin API limit");
  }
  if (body.empty()) {
    return Json::object();
  }

  auto document = Json::parse(body);
  if (!document.is_object()) {
    throw AdminError(400, "invalid_json", "Request body must be a JSON object");
  }
  return document;
}

} // namespace

nlohmann::json realm_settings_json(const RealmSettings &settings) {
  return {{"world_name", settings.world_name},
          {"site_display_name", settings.site_display_name},
          {"capacity", settings.capacity},
          {"label", label_name(settings.label)},
          {"maintenance", settings.maintenance},
          {"char_creation_restricted", settings.char_creation_restricted}};
}

RealmSettings realm_settings_from_json(const nlohmann::json &document, RealmSettings base) {
  if (!document.is_object()) {
    throw std::invalid_argument("Realm settings must be a JSON object");
  }

  if (const auto name = fields::optional_string(document, "world_name")) {
    base.world_name = *name;
  }
  if (const auto display = fields::optional_string(document, "site_display_name")) {
    base.site_display_name = *display;
  }
  if (const auto capacity = fields::optional_integer<unsigned>(document, "capacity")) {
    base.capacity = *capacity;
  }
  if (const auto label = fields::optional_string(document, "label")) {
    base.label = parse_label(*label);
  }
  if (const auto maintenance = fields::optional_bool(document, "maintenance")) {
    base.maintenance = *maintenance;
  }
  if (const auto restricted = fields::optional_bool(document, "char_creation_restricted")) {
    base.char_creation_restricted = *restricted;
  }

  validate_realm_settings(base);
  return base;
}

std::string load_or_create_admin_token(const std::filesystem::path &file) {
  constexpr std::size_t kTokenHexLength = kAdminTokenBytes * 2;

  if (std::filesystem::exists(file)) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
      throw std::runtime_error("Admin token file cannot be read");
    }

    std::string token;
    std::getline(input, token);
    while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) {
      token.pop_back();
    }
    const bool hex = std::ranges::all_of(token, [](char character) {
      return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
    if (token.size() != kTokenHexLength || !hex) {
      throw std::invalid_argument("Admin token file must contain 64 lowercase hex digits");
    }
    return token;
  }

  if (file.has_parent_path()) {
    std::filesystem::create_directories(file.parent_path());
  }

  auto token = random_hex(kAdminTokenBytes);
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  output << token << '\n';
  output.close();
  if (!output) {
    throw std::runtime_error("Admin token file cannot be written");
  }
  return token;
}

AdminApi::AdminApi(AdminServices services, std::string token, bool allow_remote)
    : services_(std::move(services)), token_(std::move(token)), allow_remote_(allow_remote) {
  if (token_.size() != kAdminTokenBytes * 2) {
    throw std::invalid_argument("Admin token must be 64 hex digits");
  }
}

bool AdminApi::handles(std::string_view target) {
  return target.starts_with(kAdminRoutePrefix);
}

bool AdminApi::authorized(std::string_view authorization) const {
  if (!authorization.starts_with(kBearerPrefix)) {
    return false;
  }

  const auto presented = authorization.substr(kBearerPrefix.size());
  if (presented.size() != token_.size()) {
    return false;
  }
  return CRYPTO_memcmp(presented.data(), token_.data(), token_.size()) == 0;
}

AdminHttpResponse AdminApi::handle(const AdminHttpRequest &request) {
  if (!request.peer_loopback && !allow_remote_) {
    return error_response(403, "admin_remote_disabled",
                          "The admin API only accepts loopback connections");
  }
  if (!authorized(request.authorization)) {
    return error_response(401, "admin_authentication_required",
                          "A valid admin bearer token is required");
  }

  const auto target = request.target;
  const auto query_start = target.find('?');
  const auto path = target.substr(0, query_start);
  const auto query_text =
      query_start == std::string_view::npos ? std::string_view{} : target.substr(query_start + 1);

  try {
    const auto segments = split_path(path.substr(kAdminRoutePrefix.size()));
    const auto query = parse_query(query_text);
    const auto body = parse_body(request.body);
    // Mutations, and the settings file they rewrite, run one at a time. Reads stay concurrent
    // (the services lock internally), so log and dashboard polls never wait behind a slow
    // password hash.
    std::unique_lock mutation_lock(request_mutex_, std::defer_lock);
    if (request.method != "GET") {
      mutation_lock.lock();
    }
    return route(request.method, segments, query, body);
  } catch (const AdminError &error) {
    return error_response(error.status(), error.code(), error.what());
  } catch (const Json::exception &) {
    return error_response(400, "invalid_json", "Request body is not valid JSON");
  } catch (const std::invalid_argument &error) {
    return error_response(400, "invalid_request", error.what());
  } catch (const std::exception &) {
    // Storage errors can quote player data; keep their text out of the response and the log.
    spdlog::error("Admin API request failed: {} {}", std::string(request.method),
                  std::string(path.substr(0, 128)));
    return error_response(500, "service_error", "The server could not complete the request");
  }
}

AdminHttpResponse AdminApi::route(std::string_view method, const Segments &segments,
                                  const Query &query, const nlohmann::json &body) {
  const bool get = method == "GET";
  const bool post = method == "POST";
  const auto count = segments.size();
  const auto resource = count > 0 ? segments[0] : std::string_view{};

  if (resource == "overview" && count == 1 && get) {
    return {200, overview()};
  }
  if (resource == "realm" && count == 1) {
    if (get) {
      return {200, realm_settings_json(services_.auth.realm_settings())};
    }
    if (post) {
      return {200, update_realm(body)};
    }
  }

  if (resource == "accounts") {
    if (count == 1 && get) {
      return {200, list_accounts(query)};
    }
    if (count == 1 && post) {
      return {201, create_account(body)};
    }
    if (count >= 2) {
      const auto account_id = parse_identifier<std::int64_t>(segments[1], "Account ID");
      if (count == 2 && get) {
        return {200, account_details(account_id)};
      }
      if (count == 3 && post) {
        return {200, update_account(account_id, segments[2], body)};
      }
    }
  }

  if (resource == "characters") {
    if (count == 1 && get) {
      return {200, list_characters(query)};
    }
    if (count >= 2) {
      const auto character_id = parse_identifier<std::int64_t>(segments[1], "Character ID");
      if (count == 2 && get) {
        return {200, character_details(character_id)};
      }
      if (count == 3 && post) {
        return {200, update_character(character_id, segments[2], body)};
      }
    }
  }

  if (resource == "players") {
    if (count == 1 && get) {
      return {200, list_players()};
    }
    if (count == 3 && post && segments[2] == "kick") {
      return {200, kick_player(parse_identifier<std::uint64_t>(segments[1], "Connection ID"))};
    }
  }

  if (resource == "announce" && count == 1 && post) {
    return {200, announce(body)};
  }

  if (resource == "server-messages" && count == 1 && (get || post)) {
    if (!services_.server_messages) {
      throw unavailable("Server messages");
    }
    if (get) {
      return {200, services_.server_messages->configuration()};
    }

    services_.server_messages->replace_configuration(body);
    auto result = services_.server_messages->configuration();
    result["settings_saved"] = save_settings();
    return {200, std::move(result)};
  }

  if (resource == "mail" && count == 1 && post) {
    if (!services_.mail_catalog) {
      throw unavailable("Mail item catalog");
    }
    const auto mail = parse_mail_delivery(body, unix_epoch_ticks_now());
    if (mail.world_id != services_.world_id) {
      throw std::invalid_argument("Mail must address this server's realm");
    }
    validate_mail_catalog(mail, *services_.mail_catalog);
    const auto delivered = services_.mail->deliver_mail(mail);
    if (!delivered) {
      throw AdminError(409, "mail_delivery_conflict",
                       "Recipient is unavailable, delivery key conflicts, or mailbox is full");
    }
    std::size_t notified{};
    if (services_.world && !delivered->deleted && delivered->claimed_epoch_ticks == 0 &&
        delivered->expiry_epoch_ticks > unix_epoch_ticks_now()) {
      notified = services_.world->notify_mail(*delivered);
    }
    return {201, {{"mail_id", delivered->id}, {"notified_connections", notified}}};
  }

  if (resource == "settings" && count == 1) {
    if (get) {
      return {200, rates_json()};
    }
    if (post) {
      return {200, update_rates(body)};
    }
  }

  if (resource == "events") {
    if (count == 1 && get) {
      return {200, rates_json()};
    }
    if (count == 1 && post) {
      return {201, create_event(body)};
    }
    if (count >= 2 && post) {
      const auto event_id = parse_identifier<std::int64_t>(segments[1], "Event ID");
      if (count == 2) {
        return {200, update_event(event_id, body)};
      }
      if (count == 3 && segments[2] == "delete") {
        return {200, delete_event(event_id)};
      }
    }
  }

  if (resource == "logs") {
    if (count == 1 && get) {
      return {200, read_logs(query)};
    }
    if (count == 2 && post && segments[1] == "level") {
      return {200, set_log_level(body)};
    }
  }

  if (resource == "chat" && count == 1 && get) {
    return {200, read_chat(query)};
  }

  if (resource == "server" && count == 2 && post && segments[1] == "shutdown") {
    return {202, shutdown(body)};
  }

  throw AdminError(404, "route_not_found", "Unknown admin route or method");
}

std::vector<WorldConnectionSummary> AdminApi::world_connections() const {
  if (!services_.world) {
    return {};
  }
  return services_.world->connections();
}

nlohmann::json AdminApi::overview() {
  const auto now = current_unix_seconds();
  const auto status = services_.auth.world_status();
  const auto realm = services_.auth.realm_settings();
  const auto connections = world_connections();
  const auto lobby_sessions = services_.auth.lobby_session_counts();
  const auto accounts = services_.repositories.accounts->list_accounts();

  unsigned total_lobby_sessions = 0;
  for (const auto &[account_id, sessions] : lobby_sessions) {
    total_lobby_sessions += sessions;
  }

  int character_total = 0;
  for (const auto &account : accounts) {
    character_total += account.character_count;
  }

  unsigned in_world = 0;
  for (const auto &connection : connections) {
    if (connection.authenticated) {
      ++in_world;
    }
  }

  const auto &info = services_.info;
  Json server{{"client_version", info.client_version},
              {"storage_backend", info.storage_backend},
              {"listen_address", info.listen_address},
              {"public_address", info.public_address},
              {"http_port", info.http_port},
              {"world_port", info.world_port},
              {"world_listener", services_.world != nullptr},
              {"native_client", info.native_client},
              {"require_launch_grant", info.require_launch_grant},
              {"world_bootstrap_diagnostic", info.world_bootstrap_diagnostic},
              {"client_catalog_loaded", info.client_catalog_loaded},
              {"started_at_unix", info.started_at_unix},
              {"uptime_seconds", std::max<std::int64_t>(0, now - info.started_at_unix)},
              {"server_time_unix", now},
              {"settings_persisted", !services_.settings_file.empty()},
              {"log_level", log_level_name(spdlog::get_level())},
              {"logs_available", services_.logs != nullptr},
              {"chat_available", services_.chat != nullptr}};

  auto realm_json = realm_settings_json(realm);
  realm_json["world_id"] = status.world_id;
  realm_json["online"] = status.online;
  realm_json["state"] = std::string(world_state_label(status.state));

  Json population{{"world_connections", connections.size()},
                  {"in_world", in_world},
                  {"lobby_sessions", total_lobby_sessions},
                  {"accounts", accounts.size()},
                  {"characters", character_total}};

  return {{"server", std::move(server)},
          {"realm", std::move(realm_json)},
          {"population", std::move(population)},
          {"rates", rates_json()}};
}

nlohmann::json AdminApi::update_realm(const nlohmann::json &body) {
  const auto updated = realm_settings_from_json(body, services_.auth.realm_settings());
  services_.auth.update_realm_settings(updated);
  spdlog::info("Admin: realm settings updated (capacity={} maintenance={} "
               "char_creation_restricted={})",
               updated.capacity, updated.maintenance, updated.char_creation_restricted);

  auto result = realm_settings_json(updated);
  result["settings_saved"] = save_settings();
  return result;
}

nlohmann::json AdminApi::list_accounts(const Query &query) {
  const auto search_entry = query.find("search");
  const auto search = lowercase(search_entry == query.end() ? std::string{} : search_entry->second);

  const auto lobby_sessions = services_.auth.lobby_session_counts();
  std::map<std::int64_t, unsigned> online_characters;
  for (const auto &connection : world_connections()) {
    if (connection.authenticated) {
      ++online_characters[connection.account_id];
    }
  }

  const auto accounts = services_.repositories.accounts->list_accounts();
  auto rows = Json::array();
  for (const auto &account : accounts) {
    if (!contains_case_insensitive(account.name, search)) {
      continue;
    }
    if (rows.size() >= kMaximumAdminListRows) {
      break;
    }

    const auto sessions = lobby_sessions.find(account.id);
    const auto online = online_characters.find(account.id);
    rows.push_back(account_summary_json(account,
                                        sessions == lobby_sessions.end() ? 0 : sessions->second,
                                        online == online_characters.end() ? 0 : online->second));
  }

  return {{"accounts", std::move(rows)}, {"total", accounts.size()}};
}

nlohmann::json AdminApi::create_account(const nlohmann::json &body) {
  const auto name = fields::required_string(body, "account");
  const auto password = fields::required_string(body, "password");
  const auto role_name = fields::optional_string(body, "role").value_or("player");
  const auto role = parse_account_role(role_name);
  if (!role) {
    throw std::invalid_argument("role must be player or gm");
  }

  if (!services_.auth.create_account(name, password)) {
    throw AdminError(409, "account_exists", "An account with this name already exists");
  }
  if (*role != AccountRole::player && !services_.auth.set_account_role(name, *role)) {
    throw std::runtime_error("Created account role could not be applied");
  }

  const auto created = services_.repositories.accounts->account(ascii_lower(name));
  if (!created) {
    throw std::runtime_error("Created account is missing");
  }
  spdlog::info("Admin: account {} created with role {}", created->id, account_role_name(*role));
  return {{"id", created->id}, {"name", created->name}, {"role", account_role_name(*role)}};
}

nlohmann::json AdminApi::account_details(std::int64_t account_id) {
  const auto account = services_.repositories.accounts->account_by_id(account_id);
  if (!account) {
    throw not_found("Account");
  }

  std::unordered_map<std::int64_t, WorldConnectionSummary> connections_by_character;
  unsigned online_characters = 0;
  for (const auto &connection : world_connections()) {
    if (connection.authenticated && connection.account_id == account_id) {
      connections_by_character[connection.character_id] = connection;
      ++online_characters;
    }
  }

  auto characters = Json::array();
  int character_count = 0;
  for (const auto &character : services_.repositories.characters->list_characters()) {
    if (character.account_id != account_id) {
      continue;
    }
    ++character_count;
    const auto connection = connections_by_character.find(character.id);
    const auto *online =
        connection == connections_by_character.end() ? nullptr : &connection->second;
    characters.push_back(character_summary_json(character, account->name, online));
  }

  const auto lobby_sessions = services_.auth.lobby_session_counts();
  const auto sessions = lobby_sessions.find(account_id);

  AccountSummary summary;
  summary.id = account->id;
  summary.name = account->name;
  summary.role = account->role;
  summary.banned = account->banned;
  summary.character_count = character_count;
  auto result = account_summary_json(
      summary, sessions == lobby_sessions.end() ? 0 : sessions->second, online_characters);
  result["characters"] = std::move(characters);
  return result;
}

nlohmann::json AdminApi::update_account(std::int64_t account_id, std::string_view action,
                                        const nlohmann::json &body) {
  std::optional<std::string> replacement_password;
  if (action == "password") {
    replacement_password = fields::required_string(body, "password");
    if (!is_valid_new_password(*replacement_password)) {
      throw std::invalid_argument("Password must be 8-256 bytes");
    }
  }
  const bool fail_closed_on_lookup = action == "kick" || action == "password" ||
      (action == "ban" && fields::optional_bool(body, "banned").value_or(true));
  std::optional<Account> account;
  try {
    account = services_.repositories.accounts->account_by_id(account_id);
  } catch (...) {
    if (fail_closed_on_lookup) {
      try {
        services_.auth.quarantine_account_sessions(account_id);
      } catch (...) {
        spdlog::error("Admin: could not quarantine account {} after lookup failed", account_id);
      }
      if (services_.world) {
        try {
          services_.world->disconnect_account(account_id);
        } catch (...) {
          spdlog::error("Admin: could not disconnect account {} after lookup failed", account_id);
        }
      }
    }
    throw;
  }
  if (!account) {
    throw not_found("Account");
  }

  if (action == "password") {
    bool updated{};
    try {
      updated = services_.auth.set_account_password(account_id, *replacement_password);
    } catch (...) {
      // The password mutation may have committed. Preserve the error while
      // requesting closure of already admitted world sessions.
      if (services_.world) {
        try {
          services_.world->disconnect_account(account_id);
        } catch (...) {
          spdlog::error("Admin: could not disconnect account {} after uncertain password reset",
                        account_id);
        }
      }
      throw;
    }
    if (!updated) {
      throw not_found("Account");
    }
    const auto disconnected = services_.world ? services_.world->disconnect_account(account_id) : 0;
    spdlog::info("Admin: account {} password reset; world connections closed={}", account_id,
                 disconnected);
    return {{"id", account_id}, {"password_changed", true}, {"disconnected", disconnected}};
  }

  if (action == "role") {
    const auto role = parse_account_role(fields::required_string(body, "role"));
    if (!role) {
      throw std::invalid_argument("role must be player or gm");
    }
    if (!services_.auth.set_account_role(account->name, *role)) {
      throw not_found("Account");
    }
    spdlog::info("Admin: account {} role set to {}", account_id, account_role_name(*role));
    // Sessions keep the role they were issued with; the change applies at the next login.
    return {
        {"id", account_id}, {"role", account_role_name(*role)}, {"applies_at_next_login", true}};
  }

  if (action == "ban") {
    const auto banned = fields::optional_bool(body, "banned").value_or(true);
    bool updated{};
    try {
      updated = services_.auth.set_account_banned(account_id, banned);
    } catch (...) {
      // A failed COMMIT response does not prove that the ban rolled back. The
      // auth service has already invalidated in-memory credentials; also ask
      // the world listener to close any active sessions before reporting error.
      if (banned && services_.world) {
        try {
          services_.world->disconnect_account(account_id);
        } catch (...) {
          spdlog::error("Admin: could not disconnect account {} after uncertain ban", account_id);
        }
      }
      throw;
    }
    if (!updated) {
      throw not_found("Account");
    }

    std::size_t disconnected = 0;
    if (banned) {
      if (services_.world) {
        disconnected = services_.world->disconnect_account(account_id);
      }
    }
    spdlog::info("Admin: account {} {}; world connections closed={}", account_id,
                 banned ? "banned" : "unbanned", disconnected);
    return {{"id", account_id}, {"banned", banned}, {"disconnected", disconnected}};
  }

  if (action == "kick") {
    try {
      services_.auth.revoke_account_sessions(account_id);
    } catch (...) {
      // Even when durable revocation is uncertain, disconnect any currently
      // visible world sessions and preserve the error for the operator.
      if (services_.world) {
        try {
          services_.world->disconnect_account(account_id);
        } catch (...) {
          spdlog::error("Admin: could not disconnect account {} after uncertain kick", account_id);
        }
      }
      throw;
    }
    const auto disconnected = services_.world ? services_.world->disconnect_account(account_id) : 0;
    spdlog::info("Admin: account {} signed out; world connections closed={}", account_id,
                 disconnected);
    return {{"id", account_id}, {"sessions_revoked", true}, {"disconnected", disconnected}};
  }

  throw AdminError(404, "route_not_found", "Unknown account action");
}

nlohmann::json AdminApi::list_characters(const Query &query) {
  const auto search_entry = query.find("search");
  const auto search = lowercase(search_entry == query.end() ? std::string{} : search_entry->second);

  std::unordered_map<std::int64_t, std::string> account_names;
  for (const auto &account : services_.repositories.accounts->list_accounts()) {
    account_names[account.id] = account.name;
  }

  std::unordered_map<std::int64_t, WorldConnectionSummary> connections_by_character;
  for (const auto &connection : world_connections()) {
    if (connection.authenticated) {
      connections_by_character[connection.character_id] = connection;
    }
  }

  const auto characters = services_.repositories.characters->list_characters();
  auto rows = Json::array();
  for (const auto &character : characters) {
    const auto &account_name = account_names[character.account_id];
    const bool matches = contains_case_insensitive(character.name, search) ||
                         contains_case_insensitive(account_name, search);
    if (!matches) {
      continue;
    }
    if (rows.size() >= kMaximumAdminListRows) {
      break;
    }

    const auto connection = connections_by_character.find(character.id);
    const auto *online =
        connection == connections_by_character.end() ? nullptr : &connection->second;
    rows.push_back(character_summary_json(character, account_name, online));
  }

  return {{"characters", std::move(rows)}, {"total", characters.size()}};
}

Character AdminApi::require_character(std::int64_t character_id) {
  auto character = services_.repositories.characters->character_by_id(character_id);
  if (!character) {
    throw not_found("Character");
  }
  return std::move(*character);
}

nlohmann::json AdminApi::character_details(std::int64_t character_id) {
  const auto character = require_character(character_id);
  const auto account = services_.repositories.accounts->account_by_id(character.account_id);
  const auto progress = services_.repositories.character_state->load_world_progress(
      character.account_id, character.id, character.world_id);

  std::optional<WorldConnectionSummary> connection;
  for (const auto &candidate : world_connections()) {
    if (candidate.authenticated && candidate.character_id == character_id) {
      connection = candidate;
    }
  }

  auto result = character_summary_json(character, account ? account->name : std::string{},
                                       connection ? &*connection : nullptr);
  if (connection) {
    result["live_position"] = position_json(connection->position);
  }

  auto items = Json::array();
  for (const auto &item : character.items) {
    items.push_back(item_json(item));
  }
  result["items"] = std::move(items);
  result["progress"] = progress_json(progress);
  return result;
}

nlohmann::json AdminApi::update_character(std::int64_t character_id, std::string_view action,
                                          const nlohmann::json &body) {
  std::unique_ptr<OfflineCharacterReservation> reservation;
  if (services_.world) {
    reservation = services_.world->reserve_offline_character(character_id);
    if (!reservation) {
      throw AdminError(409, "character_online",
                       "The character is active or has an unresolved operation");
    }
  }

  if (!services_.world) {
    const auto quarantines = services_.repositories.character_state->offline_admin_quarantines();
    if (std::find(quarantines.begin(), quarantines.end(), character_id) != quarantines.end()) {
      throw AdminError(409, "character_quarantined", "The character needs operator recovery");
    }
  }

  const auto character = require_character(character_id);
  const auto quarantine_token = random_hex(16);
  const auto begin_write = [&] {
    if (reservation) {
      reservation->begin_write();
    }
    if (!services_.repositories.character_state->create_offline_admin_quarantine(
            character_id, quarantine_token)) {
      throw AdminError(409, "character_quarantined", "The character needs operator recovery");
    }
  };
  const auto finish_write = [&] {
    if (!services_.repositories.character_state->clear_offline_admin_quarantine(
            character_id, quarantine_token)) {
      throw std::runtime_error("Offline admin quarantine could not be cleared");
    }
    if (reservation) {
      reservation->finish_write();
    }
  };

  const auto account_id = character.account_id;
  const auto world_id = character.world_id;

  if (action == "progress") {
    const auto level = fields::optional_integer<int>(body, "level").value_or(character.level);
    const auto experience =
        fields::optional_integer<std::int64_t>(body, "experience").value_or(character.experience);
    validate_progress(level, experience);
    begin_write();
    const bool saved = services_.repositories.characters->save_progress(
        account_id, character_id, world_id, level, experience);
    finish_write();
    if (!saved) {
      throw not_found("Character");
    }
    spdlog::info("Admin: character {} progress set to level={} experience={}", character_id, level,
                 experience);
    return {{"id", character_id}, {"level", level}, {"experience", experience}};
  }

  if (action == "currency") {
    const auto before =
        services_.repositories.character_state->load_world_progress(account_id, character_id,
                                                                    world_id);
    auto after = before;
    after.gold = validated_money(body, "gold", before.gold);
    after.hersander_token = validated_money(body, "hersander_token", before.hersander_token);
    // Storage accepts a progress transition only when the revision advances by exactly one.
    ++after.revision;
    begin_write();
    const bool saved = services_.repositories.character_state->save_world_progress(
        account_id, character_id, world_id, before, after);
    finish_write();
    if (!saved) {
      throw AdminError(409, "revision_conflict", "The character changed; reload and retry");
    }
    spdlog::info("Admin: character {} currency set (gold={} hersander_token={})", character_id,
                 after.gold, after.hersander_token);
    return {{"id", character_id}, {"gold", after.gold}, {"hersander_token", after.hersander_token}};
  }

  if (action == "company") {
    // Operator company assignment, the path to companies while no company NPC is reachable
    // in the live profiles. company_type 0 removes the company.
    if (!services_.companies) {
      throw AdminError(404, "route_not_found", "Companies are not enabled");
    }
    auto record = services_.companies->status(character_id).record;
    const auto company_type =
        fields::optional_integer<int>(body, "company_type").value_or(record.company_type);
    const bool company_changed = company_type != record.company_type;
    if (company_changed && services_.guilds &&
        services_.guilds->status(character_id).guild_id != 0) {
      throw AdminError(409, "guild_membership_exists",
                       "Leave the character's guild before changing its company");
    }
    if (company_changed) {
      const auto now = unix_epoch_ticks_now();
      record = CompanyRecord{};
      record.char_id = character_id;
      record.company_type = company_type;
      record.join_epoch_ticks = now;
      record.activity_epoch_ticks = now;
    }
    if (const auto position = fields::optional_integer<int>(body, "position_level")) {
      const auto threshold =
          services_.companies->contribution_for_position(company_type, *position);
      if (!threshold) {
        throw std::invalid_argument("position_level is not a level of this company");
      }
      record.contribution = *threshold;
      record.unlocked_position_level = std::max(record.unlocked_position_level, *position);
    }
    if (const auto contribution = fields::optional_integer<std::int64_t>(body, "contribution")) {
      record.contribution = *contribution;
    }
    services_.companies->validate_record(record);
    begin_write();
    const auto result = services_.companies->set_record(record);
    if (result.error != x5::ec::Success) {
      throw AdminError(500, "storage_error", "The company record outcome is unknown");
    }
    finish_write();
    spdlog::info("Admin: character {} company set to {} position {}", character_id,
                 company_type, result.status.position_level);
    return {{"id", character_id},
            {"company_type", result.status.record.company_type},
            {"contribution", result.status.record.contribution},
            {"position_level", result.status.position_level}};
  }

  if (action == "position") {
    WorldPosition position;
    position.map_id = fields::required_integer<int>(body, "map_id");
    position.x_mm = fields::required_integer<int>(body, "x_mm");
    position.y_mm = fields::required_integer<int>(body, "y_mm");
    position.z_mm = fields::required_integer<int>(body, "z_mm");
    position.yaw = static_cast<float>(fields::optional_number(body, "yaw").value_or(0.0));
    validate_world_position(position);
    begin_write();
    const bool saved = services_.repositories.characters->save_world_position(
        account_id, character_id, world_id, position);
    finish_write();
    if (!saved) {
      throw not_found("Character");
    }
    spdlog::info("Admin: character {} position set to map {}", character_id, position.map_id);
    return {{"id", character_id}, {"position", position_json(position)}};
  }

  if (action == "rename") {
    const auto name = fields::required_string(body, "name");
    if (!is_valid_character_name(name)) {
      throw std::invalid_argument("Character names are 3-16 ASCII letters or digits");
    }
    begin_write();
    const auto result = services_.repositories.characters->rename_character(character_id, name);
    finish_write();
    switch (result) {
    case CharacterRenameResult::renamed:
      break;
    case CharacterRenameResult::not_found:
      throw not_found("Character");
    case CharacterRenameResult::name_used:
      throw AdminError(409, "name_used", "Another character already uses this name");
    }
    spdlog::info("Admin: character {} renamed", character_id);
    return {{"id", character_id}, {"name", name}};
  }

  if (action == "delete") {
    // The typed name guards against deleting the wrong row from a stale table.
    const auto confirmation = fields::required_string(body, "confirm_name");
    if (confirmation != character.name) {
      throw AdminError(400, "confirmation_mismatch", "confirm_name must equal the character name");
    }
    if (services_.guilds && services_.guilds->status(character_id).guild_id != 0) {
      throw AdminError(409, "guild_membership_exists",
                       "Leave or disband the character's guild before deleting it");
    }
    begin_write();
    const bool deleted = services_.repositories.characters->delete_character(character_id);
    finish_write();
    if (!deleted) {
      throw not_found("Character");
    }
    spdlog::info("Admin: character {} of account {} deleted", character_id, account_id);
    return {{"id", character_id}, {"deleted", true}};
  }

  throw AdminError(404, "route_not_found", "Unknown character action");
}

nlohmann::json AdminApi::list_players() {
  if (!services_.world) {
    return {{"players", Json::array()}, {"world_listener", false}};
  }

  std::unordered_map<std::int64_t, std::string> account_names;
  for (const auto &account : services_.repositories.accounts->list_accounts()) {
    account_names[account.id] = account.name;
  }
  std::unordered_map<std::int64_t, Character> characters;
  for (auto &character : services_.repositories.characters->list_characters()) {
    characters[character.id] = std::move(character);
  }

  const auto now = current_unix_seconds();
  auto players = Json::array();
  for (const auto &connection : services_.world->connections()) {
    auto row = connection_json(connection, now);
    row["account_name"] = account_names[connection.account_id];

    const auto character = characters.find(connection.character_id);
    if (connection.authenticated && character != characters.end()) {
      row["character_name"] = character->second.name;
      row["level"] = character->second.level;
      row["class_name"] = class_name(character->second.class_id);
    } else {
      row["character_name"] = nullptr;
      row["level"] = nullptr;
      row["class_name"] = nullptr;
    }
    players.push_back(std::move(row));
  }

  return {{"players", std::move(players)}, {"world_listener", true}};
}

nlohmann::json AdminApi::kick_player(std::uint64_t connection_id) {
  if (!services_.world) {
    throw unavailable("The world listener");
  }
  if (!services_.world->disconnect(connection_id)) {
    throw not_found("Connection");
  }

  spdlog::info("Admin: world connection {} kicked", connection_id);
  return {{"connection_id", connection_id}, {"disconnected", true}};
}

nlohmann::json AdminApi::announce(const nlohmann::json &body) {
  const auto text = fields::required_string(body, "text");
  validate_notice_text(text);

  const auto send_chat = fields::optional_bool(body, "chat").value_or(true);
  const auto sender =
      fields::optional_string(body, "chat_sender").value_or(std::string(kDefaultNoticeSender));
  const auto style =
      parse_notice_style(fields::optional_string(body, "system_notice").value_or(""));
  if (!send_chat && !style) {
    throw std::invalid_argument("Choose chat, a system notice style, or both");
  }

  Json result{{"chat_recipients", nullptr}, {"world_recipients", nullptr}};
  if (send_chat) {
    if (!services_.chat) {
      throw unavailable("Front chat");
    }
    result["chat_recipients"] = services_.chat->broadcast_notice(services_.world_id, sender, text);
  }
  if (style) {
    if (!services_.world) {
      throw unavailable("The world listener");
    }
    result["world_recipients"] = services_.world->broadcast_system_notice(text, *style);
  }

  // Record that a notice went out, not its text.
  spdlog::info("Admin: notice sent (bytes={} chat_recipients={} world_recipients={})", text.size(),
               result["chat_recipients"].dump(), result["world_recipients"].dump());
  return result;
}

nlohmann::json AdminApi::rates_json() {
  const auto now = current_unix_seconds();
  const auto snapshot = services_.rates.snapshot();

  auto events = Json::array();
  for (const auto &event : snapshot.events) {
    auto row = server_event_json(event);
    row["active"] = event.active_at(now);
    events.push_back(std::move(row));
  }

  return {{"experience_rate_percent", snapshot.base_experience_percent},
          {"effective_experience_percent", services_.rates.effective_experience_percent(now)},
          {"maximum_rate_percent", kMaximumRatePercent},
          {"server_time_unix", now},
          {"events", std::move(events)}};
}

nlohmann::json AdminApi::update_rates(const nlohmann::json &body) {
  const auto percent = fields::required_integer<int>(body, "experience_rate_percent");
  services_.rates.set_base_experience_percent(percent);
  spdlog::info("Admin: base experience rate set to {}%", percent);

  auto result = rates_json();
  result["settings_saved"] = save_settings();
  return result;
}

nlohmann::json AdminApi::create_event(const nlohmann::json &body) {
  const auto created = services_.rates.add_event(server_event_from_json(body));
  spdlog::info("Admin: event {} scheduled (+{}% experience)", created.id,
               created.experience_bonus_percent);

  auto result = rates_json();
  result["event"] = server_event_json(created);
  result["settings_saved"] = save_settings();
  return result;
}

nlohmann::json AdminApi::update_event(std::int64_t event_id, const nlohmann::json &body) {
  auto event = server_event_from_json(body);
  event.id = event_id;
  if (!services_.rates.update_event(event)) {
    throw not_found("Event");
  }
  spdlog::info("Admin: event {} updated (+{}% experience enabled={})", event_id,
               event.experience_bonus_percent, event.enabled);

  auto result = rates_json();
  result["settings_saved"] = save_settings();
  return result;
}

nlohmann::json AdminApi::delete_event(std::int64_t event_id) {
  if (!services_.rates.remove_event(event_id)) {
    throw not_found("Event");
  }
  spdlog::info("Admin: event {} deleted", event_id);

  auto result = rates_json();
  result["settings_saved"] = save_settings();
  return result;
}

nlohmann::json AdminApi::read_logs(const Query &query) {
  if (!services_.logs) {
    throw unavailable("The log buffer");
  }

  const auto after = query_unsigned<std::uint64_t>(query, "after", 0);
  const auto limit = query_unsigned<std::size_t>(query, "limit", kDefaultLogPage);

  auto minimum_level = spdlog::level::trace;
  if (const auto found = query.find("level"); found != query.end() && !found->second.empty()) {
    const auto parsed = parse_log_level(found->second);
    if (!parsed) {
      throw std::invalid_argument("level must be trace, debug, info, warning, error or critical");
    }
    minimum_level = *parsed;
  }

  const auto page = services_.logs->read_after(after, limit, minimum_level);
  auto entries = Json::array();
  for (const auto &entry : page.entries) {
    entries.push_back({{"sequence", entry.sequence},
                       {"unix_ms", entry.unix_milliseconds},
                       {"level", entry.level},
                       {"message", entry.message}});
  }

  return {{"entries", std::move(entries)},
          {"latest_sequence", page.latest_sequence},
          {"next_after", page.next_after_sequence},
          {"gap", page.gap},
          {"server_level", log_level_name(spdlog::get_level())}};
}

nlohmann::json AdminApi::read_chat(const Query &query) {
  if (!services_.chat) {
    throw unavailable("Front chat");
  }

  const auto after = query_unsigned<std::uint64_t>(query, "after", 0);
  const auto limit = query_unsigned<std::size_t>(query, "limit", kDefaultChatPage);
  const auto page = services_.chat->world_chat_history(after, limit);

  auto messages = Json::array();
  for (const auto &record : page.records) {
    // The history covers every realm on the service; the console shows this server's realm.
    if (record.world_id != services_.world_id) {
      continue;
    }
    messages.push_back({{"sequence", record.sequence},
                        {"unix_ms", record.unix_milliseconds},
                        {"character_id", record.character_id},
                        {"sender_name", record.sender_name},
                        {"message", record.message},
                        {"operator_notice", record.operator_notice}});
  }

  std::uint64_t next_after = after;
  if (!page.records.empty()) {
    next_after = page.records.back().sequence;
  } else if (page.gap) {
    next_after = 0;
  }

  return {{"messages", std::move(messages)},
          {"latest_sequence", page.latest_sequence},
          {"next_after", next_after},
          {"gap", page.gap}};
}

nlohmann::json AdminApi::set_log_level(const nlohmann::json &body) {
  const auto name = fields::required_string(body, "level");
  const auto level = parse_log_level(name);
  if (!level) {
    throw std::invalid_argument(
        "level must be trace, debug, info, warning, error, critical or off");
  }

  spdlog::set_level(*level);
  spdlog::warn("Admin: server log level set to {}", log_level_name(*level));
  return {{"server_level", log_level_name(*level)}};
}

nlohmann::json AdminApi::shutdown(const nlohmann::json &body) {
  if (!services_.request_shutdown) {
    throw unavailable("Remote shutdown");
  }
  if (fields::required_string(body, "confirm") != kShutdownConfirmation) {
    throw AdminError(400, "confirmation_mismatch", "confirm must be \"shutdown\"");
  }

  spdlog::warn("Admin: shutdown requested from the operator console");
  services_.request_shutdown();
  return {{"shutting_down", true}};
}

bool AdminApi::save_settings() {
  if (services_.settings_file.empty()) {
    return false;
  }

  Json document{{"version", kSettingsFileVersion},
                {"realm", realm_settings_json(services_.auth.realm_settings())},
                {"rates", services_.rates.to_json()}};
  if (services_.server_messages) {
    document["server_messages"] = services_.server_messages->configuration();
  }

  // Write a sibling file, then rename it over the old one, so a crash never leaves half a file.
  auto temporary = services_.settings_file;
  temporary += ".tmp";
  try {
    if (services_.settings_file.has_parent_path()) {
      std::filesystem::create_directories(services_.settings_file.parent_path());
    }
    {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      output << document.dump(2) << '\n';
      output.close();
      if (!output) {
        throw std::runtime_error("write failed");
      }
    }
    std::filesystem::rename(temporary, services_.settings_file);
    return true;
  } catch (const std::exception &error) {
    spdlog::warn("Admin settings could not be saved: {}", error.what());
    return false;
  }
}

void AdminApi::load_saved_settings() {
  const auto &file = services_.settings_file;
  if (file.empty() || !std::filesystem::exists(file)) {
    return;
  }
  if (std::filesystem::file_size(file) > kMaximumSettingsFileBytes) {
    throw std::invalid_argument("Admin settings file is too large");
  }

  std::ifstream input(file, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Admin settings file cannot be read");
  }
  const auto document = Json::parse(input);
  if (!document.is_object() ||
      fields::optional_integer<int>(document, "version").value_or(0) != kSettingsFileVersion) {
    throw std::invalid_argument("Admin settings file has an unsupported version");
  }

  // Validate both parts before applying either, so a bad file changes nothing.
  std::optional<RealmSettings> realm;
  if (const auto stored_realm = document.find("realm"); stored_realm != document.end()) {
    realm = realm_settings_from_json(*stored_realm, services_.auth.realm_settings());
  }
  GameRates validated_rates;
  const auto stored_rates = document.find("rates");
  if (stored_rates != document.end()) {
    validated_rates.load_json(*stored_rates);
  }
  const auto stored_messages = document.find("server_messages");
  if (stored_messages != document.end() && services_.server_messages) {
    parse_server_messages_config(*stored_messages);
  }

  if (realm) {
    services_.auth.update_realm_settings(*realm);
  }
  if (stored_rates != document.end()) {
    services_.rates.load_json(*stored_rates);
  }
  if (stored_messages != document.end() && services_.server_messages) {
    services_.server_messages->replace_configuration(*stored_messages);
  }
  spdlog::info("Admin settings loaded from the console settings file");
}

} // namespace chronicles
