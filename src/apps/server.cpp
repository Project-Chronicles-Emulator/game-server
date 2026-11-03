#include "apps/repositories.hpp"
#include "apps/auth_heartbeat.hpp"
#include "apps/postgres_server_lease.hpp"
#include "apps/quest_effect_timer_policy.hpp"
#include "apps/startup_reward_recovery.hpp"
#include "auth/auth.hpp"
#include "auth/auth_authority.hpp"
#include "auth/auth_admission.hpp"
#include "auth/native_auth.hpp"
#include "auth/password_policy.hpp"
#include "common/clock_ticks.hpp"
#include "common/world_metrics.hpp"
#include "data/client_catalog.hpp"
#include "data/shop_catalog.hpp"
#include "data/starter_gear.hpp"
#include "distribution/distribution.hpp"
#include "net/world_worker_threads.hpp"
#include "services/admin/admin_api.hpp"
#include "services/agent_engine/agent_engine.hpp"
#include "services/agent_engine/agent_engine_http.hpp"
#include "services/agent_engine/agent_engine_rest.hpp"
#include "services/discord/discord_oauth.hpp"
#include "services/front/native_ws.hpp"
#include "services/news/news.hpp"
#include "services/portal_storage/portal_store.hpp"
#include "services/social/character_social.hpp"
#include "world/core/quest_effect_dispatcher.hpp"
#include "world/features/chat/player_commands.hpp"
#include "world/features/chat/server_messages.hpp"
#include "world/features/company/company.hpp"
#include "world/features/company/company_dialogues.hpp"
#include "world/features/fog/fog_rest.hpp"
#include "world/features/friends/friends.hpp"
#include "world/features/gathering/gathering.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/hotkeys/hotkey_rest.hpp"
#include "world/features/housing/guild_housing.hpp"
#include "world/features/housing/housing_service.hpp"
#include "world/features/housing/housing_world.hpp"
#include "world/features/housing/native_housing.hpp"
#include "world/features/loot/loot.hpp"
#include "world/features/mail/mail_rest.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/features/processing/processing.hpp"
#include "world/session/native_world.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <charconv>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string_view>
#include <thread>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <spdlog/spdlog.h>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;
using Json = nlohmann::json;
using namespace chronicles;

namespace {

constexpr unsigned kMaximumPort = 65535;
#ifdef CHRONICLES_REQUIRE_SEPARATE_LOGIN
constexpr bool kDefaultSeparateLogin = true;
#else
constexpr bool kDefaultSeparateLogin = false;
#endif
constexpr std::size_t kMaximumStartupRewardRecovery = 1'000'000;
constexpr auto kQuestEffectRetryInterval = std::chrono::seconds(2);
// A character returning from character selection may reconnect before its previous world
// session has released it; the entry waits up to this long instead of being refused.
constexpr auto kWorldEntryAdmissionWait = std::chrono::seconds(10);
constexpr unsigned kAgentEngineWorkerThreadCount = 2;
constexpr unsigned kAuthWorkerThreadCount = 4;
// In-world clients hold long-lived front (and auth) WebSockets on this listener, so the
// default must cover two sockets per player at the 500-player realm target plus launcher
// and download traffic. This is a resource bound, not a measured capacity.
constexpr unsigned kDefaultMaximumHttpConnections = 1280;
constexpr unsigned kMaximumHttpConnections = 100000;
constexpr std::size_t kMessageBodyLimit = 65536;
constexpr std::uint32_t kHttpHeaderLimit = 8192;
constexpr std::size_t kBearerPrefixLength = 7;
constexpr std::size_t kMaximumBearerTokenLength = 4096;
constexpr std::size_t kMaximumRegistrationBodyBytes = 4096;
constexpr auto kHttpTimeout = std::chrono::seconds(15);
constexpr auto kWebSocketIdleTimeout = std::chrono::seconds(30);
constexpr auto kChatDeliveryInterval = std::chrono::milliseconds(100);
constexpr std::size_t kMaximumWebSocketQueuedMessages = 64;
constexpr std::string_view kDefaultWorldAdvertisedHost = "127.0.0.1";
constexpr std::size_t kMaximumHostNameLength = 253;
constexpr std::string_view kSupportedClientVersion = "hotfix-26.10.18.734";
constexpr std::string_view kSupportedClientProtocolHex =
    "b0fa233f08edc242480aa097e76541d4e256bd7641c856c424907eb2ecd91e15";

class ServerMessageTimer : public std::enable_shared_from_this<ServerMessageTimer> {
public:
  ServerMessageTimer(asio::io_context &context, std::shared_ptr<ServerMessages> messages)
      : timer_(context), messages_(std::move(messages)) {}

  void run() {
    timer_.expires_after(std::chrono::seconds(1));
    timer_.async_wait([self = shared_from_this()](beast::error_code error) {
      if (error) {
        return;
      }
      try {
        self->messages_->tick(Clock::now());
      } catch (const std::exception &) {
        spdlog::error("Scheduled server message delivery failed");
      }
      self->run();
    });
  }

private:
  asio::steady_timer timer_;
  std::shared_ptr<ServerMessages> messages_;
};

// A single background pass runs at a time. Database I/O stays off the network executor,
// and failed or ambiguous settlements are retried using the same dispatcher quarantine.
class QuestEffectTimer : public std::enable_shared_from_this<QuestEffectTimer> {
public:
  QuestEffectTimer(asio::io_context &context, asio::thread_pool &workers,
                   std::shared_ptr<QuestEffectDispatcher> dispatcher, int world_id)
      : context_(context), timer_(context), workers_(workers), dispatcher_(std::move(dispatcher)),
        world_id_(world_id) {
  }

  void run() {
    if (state_.stopping()) {
      return;
    }
    try_schedule_quest_effect_step(
        [self = shared_from_this()] {
          self->timer_.expires_after(kQuestEffectRetryInterval);
          self->timer_.async_wait([self](beast::error_code error) {
            if (!quest_effect_wait_can_continue(
                    static_cast<bool>(error), error == asio::error::operation_aborted,
                    self->state_.stopping(), [self] { self->fail_closed(); })) {
              return;
            }
            self->schedule_dispatch();
          });
        },
        [this] { fail_closed(); });
  }

  bool failed() const noexcept {
    return state_.failed();
  }

  int exit_code() const noexcept {
    return state_.exit_code();
  }

  void stop() {
    state_.stop();
  }

private:
  void schedule_dispatch() {
    try_schedule_quest_effect_step(
        [self = shared_from_this()] {
#if defined(CHRONICLES_TEST_QUEST_SCHEDULER_FAULT)
          // Only the isolated test executable contains this hook. It holds dispatch until
          // the harness commits a pending event, then injects a worker-post failure.
          if (const char *trigger = std::getenv("CHRONICLES_TEST_QUEST_SCHEDULER_TRIGGER")) {
            if (!std::filesystem::exists(trigger)) {
              self->run();
              return;
            }
            throw std::runtime_error("Injected quest scheduler worker-post failure");
          }
#endif
          asio::post(self->workers_, [self] {
            try {
              const auto result = self->dispatcher_->dispatch(
                  self->world_id_, std::nullopt, kQuestServiceEffectPageSize);
              if (result.retry_needed) {
                spdlog::warn("Quest service settlement will be retried");
              }
            } catch (const std::exception &error) {
              spdlog::error("Quest service settlement failed: {}", error.what());
            }
            if (self->state_.stopping()) {
              return;
            }
            try_schedule_quest_effect_step(
                [self] {
                  asio::post(self->timer_.get_executor(), [self] { self->run(); });
                },
                [self] { self->fail_closed(); });
          });
        },
        [this] { fail_closed(); });
  }

  void fail_closed() noexcept {
    state_.fail();
    try {
      spdlog::critical("Quest service scheduler failed; stopping the server for recovery");
    } catch (...) {
      // Stopping the server must not depend on the logging backend.
    }
    context_.stop();
  }

  asio::io_context &context_;
  asio::steady_timer timer_;
  asio::thread_pool &workers_;
  std::shared_ptr<QuestEffectDispatcher> dispatcher_;
  int world_id_{};
  QuestEffectSchedulerState state_;
};

std::string bearer_token(const Request &request) {
  auto value = request[http::field::authorization];
  std::string text(value.data(), value.size());
  if (text.starts_with("Bearer ") && text.size() > kBearerPrefixLength &&
      text.size() <= kBearerPrefixLength + kMaximumBearerTokenLength) {
    return text.substr(kBearerPrefixLength);
  }
  return {};
}

Response error_response(http::status status, const std::string &code) {
  Response response{status, 11};
  response.set(http::field::content_type, "application/json");
  response.body() = Json{{"error", code}}.dump();
  response.prepare_payload();

  return response;
}

template <class Message> Message parse_request_body(const Request &request) {
  Message message;
  if (request[http::field::content_type] == "application/x-protobuf") {
    if (!message.ParseFromString(request.body())) {
      throw std::invalid_argument("Invalid protobuf");
    }
  } else {
    parse_json(request.body().empty() ? Json::object() : Json::parse(request.body()), message);
  }

  return message;
}

template <class Message> Response make_reply(const Request &request, const Message &message) {
  Response response{http::status::ok, 11};
  if (request[http::field::accept] == "application/x-protobuf") {
    response.set(http::field::content_type, "application/x-protobuf");
    response.body() = message.SerializeAsString();
  } else {
    response.set(http::field::content_type, "application/json");
    response.body() = message_json(message).dump();
  }
  response.set(http::field::cache_control, "no-store");
  response.prepare_payload();

  return response;
}

class Router {
public:
  Router(AuthService &service, NativeAuth &native_auth, bool native_enabled, bool world_enabled,
         bool require_launch_grant, std::string public_address,
         std::shared_ptr<distribution::PatchCatalog> patches, NewsStore &news, PortalStore &content,
         HotKeyRepository &hotkeys, FogRepository &fog, std::shared_ptr<GuildService> guilds,
         std::shared_ptr<housing::HousingHub> housing = {},
         ClientCatalog *catalog = nullptr,
         std::shared_ptr<FriendService> friends = {},
         unsigned world_connection_limit = kDefaultMaximumWorldConnections,
         std::shared_ptr<const CompanyService> companies = {},
         std::shared_ptr<GuildHousingRequests> guild_housing = {})
      : service_(service), native_auth_(native_auth), hotkeys_(service, hotkeys),
        native_ws_(service, std::move(guilds), std::move(housing), std::move(friends),
                   std::move(companies), std::move(guild_housing)),
        native_enabled_(native_enabled),
        world_enabled_(world_enabled), require_launch_grant_(require_launch_grant),
        public_address_(std::move(public_address)), patches_(std::move(patches)), news_(news),
        content_(content), fog_(service, fog, catalog),
        // Effective accepted-connection cap enforced by the world listener, when enabled.
        // Reported on /health as max_world_connections; the realm world.capacity summary
        // (default kDefaultWorldCapacity=500) is unchanged and not the socket cap.
        world_connection_limit_(world_connection_limit),
        discord_oauth_(service, DiscordOAuthConfig::from_environment()) {}

  std::shared_ptr<AuthAdmission::Permit> try_auth_work(std::string_view peer) {
    return admission_.try_acquire(peer);
  }

  std::optional<http::response<http::file_body>> patch_blob(const Request &request) const {
    const auto target = std::string_view(request.target().data(), request.target().size());
    if (!patches_ || request.method() != http::verb::get ||
        !target.starts_with(distribution::kBlobRoute)) {
      return std::nullopt;
    }
    try {
      const auto path = patches_->blob(std::string(target.substr(distribution::kBlobRoute.size())));
      http::response<http::file_body> response{http::status::ok, 11};
      beast::error_code error;
      response.body().open(path.string().c_str(), beast::file_mode::scan, error);
      if (error) {
        return std::nullopt;
      }
      response.set(http::field::content_type, "application/octet-stream");
      response.set(http::field::cache_control, "public, max-age=31536000, immutable");
      response.set("X-Content-Type-Options", "nosniff");
      response.content_length(response.body().size());
      return response;
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }

  Response handle(const Request &request, bool peer_loopback = false,
                  const std::string &peer_address = "unknown") {
    try {
      std::string path(request.target().data(), request.target().size());
      if (path == "/emulator/v1/auth-options" || path.starts_with("/emulator/v1/discord/")) {
        return discord_response(request, path);
      }
      if (path == "/internal/metrics" && request.method() == http::verb::get) {
        const auto reply =
            world_metrics::metrics_http_reply(world_metrics::enabled(), peer_loopback);
        Response response{static_cast<http::status>(reply.status), 11};
        response.set(http::field::content_type, "application/json");
        response.body() = reply.body;
        response.prepare_payload();
        return response;
      }
      if (AdminApi::handles(path)) {
        return admin_response(request, peer_loopback);
      }
      if (path == "/launcher/v1/content") {
        if (request.method() != http::verb::get) {
          return error_response(http::status::method_not_allowed, "content_read_only");
        }
        Response response{http::status::ok, 11};
        response.set(http::field::content_type, "application/json");
        response.set(http::field::cache_control, "no-store");
        response.set("X-Content-Type-Options", "nosniff");
        response.body() =
            Json{{"store", content_.list("store")}, {"community", content_.list("community")}}
                .dump();
        response.prepare_payload();
        return response;
      }
      if (path == "/launcher/v1/news") {
        if (request.method() != http::verb::get) {
          return error_response(http::status::method_not_allowed, "news_read_only");
        }
        Response response{http::status::ok, 11};
        response.set(http::field::content_type, "application/json");
        response.set(http::field::cache_control, "no-store");
        response.set("X-Content-Type-Options", "nosniff");
        response.body() = Json{{"articles", news_.list()}}.dump();
        response.prepare_payload();
        return response;
      }
      if (path.starts_with("/distribution/v1/")) {
        constexpr std::string_view prefix = "/distribution/v1/channels/";
        if (patches_ && request.method() == http::verb::get && path.starts_with(prefix)) {
          try {
            Response response{http::status::ok, 11};
            response.set(http::field::content_type, "application/json");
            response.set(http::field::cache_control, "no-store");
            response.body() = patches_->manifest(path.substr(prefix.size()));
            response.prepare_payload();
            return response;
          } catch (const std::exception &) {
            return error_response(http::status::not_found, "release_unavailable");
          }
        }
        return error_response(http::status::not_found, "distribution_resource_not_found");
      }
      if (path == "/health" && request.method() == http::verb::get) {
        // Realm population is public summary data, like a realm list; it names no player.
        const auto world = service_.world_status();
        const Json world_summary{{"id", world.world_id},
                                 {"name", world.name},
                                 {"online", world.online},
                                 {"capacity", world.capacity},
                                 {"state", world_state_label(world.state)},
                                 {"char_creation_restricted", world.char_creation_restricted}};

        auto response = error_response(http::status::ok, "ok");
        response.body() = Json{
            {"status", "ok"},
            {"native_login_adapter", native_enabled_},
            {"native_require_launch_grant", require_launch_grant_},
            {"patch_service", patches_ != nullptr},
            {"native_websockets", true},
            {"world_listener", world_enabled_},
            {"max_world_connections", world_connection_limit_},
            {"world",
             world_summary}}.dump();
        response.prepare_payload();
        return response;
      }
      const auto method = request.method_string();
      const auto authorization = request[http::field::authorization];
      const auto content_type = request[http::field::content_type];
      if (FogRest::handles(path)) {
        auto fog_response = fog_.handle(
            std::string_view(method.data(), method.size()), path,
            std::string_view(authorization.data(), authorization.size()),
            std::string_view(content_type.data(), content_type.size()), request.body());
        Response response{static_cast<http::status>(fog_response.status), 11};
        response.set(http::field::content_type, fog_response.content_type);
        response.set(http::field::cache_control, "no-store");
        response.body() = std::move(fog_response.body);
        response.prepare_payload();
        spdlog::info("Front REST {} exploration -> {}", std::string(method), fog_response.status);
        return response;
      }
      // Front REST hot-key quick slots (src/world/features/hotkeys/hotkey_rest.hpp), on the address
      // WorldEnterRes names.
      if (HotKeyRest::handles(path)) {
        auto hotkey_response = hotkeys_.handle(
            std::string_view(method.data(), method.size()), path,
            std::string_view(authorization.data(), authorization.size()),
            std::string_view(content_type.data(), content_type.size()), request.body());
        Response response{static_cast<http::status>(hotkey_response.status), 11};
        response.set(http::field::content_type, hotkey_response.content_type);
        response.set(http::field::cache_control, "no-store");
        response.body() = std::move(hotkey_response.body);
        response.prepare_payload();
        spdlog::info("Front REST {} hot keys -> {}", std::string(method), hotkey_response.status);
        return response;
      }
      auto native_response = native_auth_.handle(
          std::string_view(method.data(), method.size()), path,
          std::string_view(authorization.data(), authorization.size()),
          std::string_view(content_type.data(), content_type.size()), request.body());
      if (native_response) {
        Response response{static_cast<http::status>(native_response->status), 11};
        response.set(http::field::content_type, native_response->content_type);
        response.set(http::field::cache_control, "no-store");
        response.body() = std::move(native_response->body);
        response.prepare_payload();
        // Log route and status only: platform tickets and bearer tokens stay out of logs.
        const auto logged_path = path.starts_with("/launch/")
                                     ? std::string("/launch/[redacted]")
                                     : path.substr(0, path.find('?')).substr(0, 128);
        spdlog::info("Native HTTP {} {} -> {}", std::string(method), logged_path,
                     response.result_int());
        return response;
      }
      if (path == "/emulator/v1/register") {
        if (request.method() != http::verb::post) {
          return error_response(http::status::method_not_allowed, "registration_requires_post");
        }
        if (request.body().size() > kMaximumRegistrationBodyBytes) {
          return error_response(http::status::payload_too_large, "registration_body_too_large");
        }
        if (!request[http::field::content_type].starts_with("application/json")) {
          return error_response(http::status::unsupported_media_type, "registration_requires_json");
        }
        if (!admission_.allow_registration(peer_address)) {
          auto response = error_response(http::status::too_many_requests, "registration_rate_limit");
          response.set(http::field::retry_after, "60");
          return response;
        }

        const auto document = Json::parse(request.body());
        const auto account = document.at("account").get<std::string>();
        const auto password = document.at("password").get<std::string>();
        if (!service_.create_account(account, password)) {
          return error_response(http::status::conflict, "account_unavailable");
        }

        Response response{http::status::created, 11};
        response.set(http::field::content_type, "application/json");
        response.set(http::field::cache_control, "no-store");
        response.body() = Json{{"created", true}}.dump();
        response.prepare_payload();
        return response;
      }
      if (path == "/emulator/v1/login" && request.method() == http::verb::post) {
        auto json = Json::parse(request.body());
        const auto account = json.at("account").get<std::string>();
        const auto normalized = normalized_account(account);
        if (!admission_.password_allowed(peer_address, normalized)) {
          auto response = error_response(http::status::too_many_requests, "login_rate_limit");
          response.set(http::field::retry_after, "60");
          return response;
        }

        bool credential_mismatch = false;
        auto result = service_.login(account, json.at("password").get<std::string>(),
                                     json.value("client", std::string{}) == "launcher",
                                     &credential_mismatch);
        if (credential_mismatch && !normalized.empty()) {
          admission_.bad_password(peer_address, normalized);
        }
        return make_reply(request, result);
      }

      auto token = bearer_token(request);
      if (!service_.authenticated(token)) {
        return error_response(http::status::unauthorized, "authentication_required");
      }
      if (path == "/launcher/v1/characters") {
        if (request.method() != http::verb::get) {
          return error_response(http::status::method_not_allowed, "roster_read_only");
        }
        Response response{http::status::ok, 11};
        response.set(http::field::content_type, "application/json");
        response.set(http::field::cache_control, "no-store");
        response.body() = service_.launcher_characters(token).dump();
        response.prepare_payload();
        return response;
      }
      if (path == "/emulator/v1/launch-grant" && request.method() == http::verb::post) {
        const auto grant = service_.issue_launch_grant(token);
        if (!grant) {
          return error_response(http::status::unauthorized, "authentication_required");
        }

        Response response{http::status::ok, 11};
        response.set(http::field::content_type, "application/json");
        response.set(http::field::cache_control, "no-store");
        response.body() = Json{{"bootstrap_url", public_address_ + "/launch/" + *grant + "/"},
                               {"expires_in_seconds", kLaunchGrantTtl.count()}}
                              .dump();
        response.prepare_payload();
        return response;
      }
      if (path == "/emulator/v1/logout" && request.method() == http::verb::post) {
        service_.logout(token);
        Response response{http::status::no_content, 11};
        response.set(http::field::cache_control, "no-store");
        return response;
      }
      if (path == "/emulator/v1/worlds" && request.method() == http::verb::get) {
        return make_reply(request, service_.worlds(token));
      }
      if (path == "/emulator/v1/world/enter" && request.method() == http::verb::post) {
        const auto world_request =
            parse_request_body<x5::pb::authwebsocket::WorldEnterReq>(request);
        return make_reply(request, service_.enter(token, world_request));
      }
      if (path == "/emulator/v1/characters" && request.method() == http::verb::get) {
        return make_reply(request, service_.list(token));
      }
      if (path == "/emulator/v1/characters/create" && request.method() == http::verb::post) {
        const auto character_request =
            parse_request_body<x5::pb::ws::front::CharCreateReq>(request);
        return make_reply(request, service_.create(token, character_request));
      }
      if (path == "/emulator/v1/characters/select" && request.method() == http::verb::post) {
        const auto character_request =
            parse_request_body<x5::pb::ws::front::CharSelectReq>(request);
        return make_reply(request, service_.select(token, character_request));
      }

      return error_response(http::status::not_found, "route_not_found");
    } catch (const Unauthorized &) {
      return error_response(http::status::unauthorized, "world_entry_or_authentication_required");
    } catch (const Json::exception &) {
      return error_response(http::status::bad_request, "invalid_json_payload");
    } catch (const std::invalid_argument &) {
      return error_response(http::status::bad_request, "invalid_payload");
    } catch (const std::exception &) {
      spdlog::error("Request failed in auth/lobby service");
      return error_response(http::status::internal_server_error, "service_error");
    }
  }

  Json rpc(const std::string &token, const Json &request) {
    Json response{{"request_id", request.value("request_id", Json())}};
    try {
      auto type = request.at("type").get<std::string>();
      auto payload = request.value("payload", Json::object());
      if (type == "CharListReq") {
        x5::pb::ws::front::CharListReq message;
        parse_json(payload, message);
        response["type"] = "CharListRes";
        response["payload"] = message_json(service_.list(token));
      } else if (type == "CharCreateReq") {
        x5::pb::ws::front::CharCreateReq message;
        parse_json(payload, message);
        response["type"] = "CharCreateRes";
        response["payload"] = message_json(service_.create(token, message));
      } else if (type == "CharSelectReq") {
        x5::pb::ws::front::CharSelectReq message;
        parse_json(payload, message);
        response["type"] = "CharSelectRes";
        response["payload"] = message_json(service_.select(token, message));
      } else {
        response["error"] = "unknown_rpc_type";
      }
    } catch (const Unauthorized &) {
      response["error"] = "authentication_or_world_entry_required";
    } catch (const Json::exception &) {
      response["error"] = "invalid_json_payload";
    } catch (const std::invalid_argument &) {
      response["error"] = "invalid_payload";
    } catch (const std::exception &) {
      spdlog::error("Lobby RPC failed");
      response["error"] = "service_error";
    }
    return response;
  }

  bool authenticated(const std::string &token, bool native_session = false) {
    if (service_.authenticated(token)) {
      return true;
    }
    if (native_session) {
      return native_auth_.authenticate_session(token);
    }
    return false;
  }

  std::string native_rpc(NativeWsChannel channel, const std::string &token,
                         std::string_view request) {
    return native_ws_.handle(channel, token, request);
  }

  NativeChatService &native_chat() {
    return native_ws_.chat();
  }

  void set_player_commands(std::shared_ptr<PlayerCommands> commands) {
    native_ws_.set_player_commands(std::move(commands));
  }

  void set_local_chat_audience(NativeChatService::ChatAudience audience) {
    native_ws_.set_local_chat_audience(std::move(audience));
  }

  void set_mail(std::unique_ptr<MailRest> mail) {
    mail_ = std::move(mail);
  }

  bool handle_mail_async(const Request &request, std::function<void(Response)> reply) {
    const std::string_view target(request.target().data(), request.target().size());
    if (!native_enabled_ || !mail_ || !MailRest::handles(target)) {
      return false;
    }
    mail_->handle_async(
        std::string_view(request.method_string().data(), request.method_string().size()), target,
        std::string_view(request[http::field::authorization].data(),
                         request[http::field::authorization].size()),
        std::string_view(request[http::field::content_type].data(),
                         request[http::field::content_type].size()),
        request.body(), [reply = std::move(reply)](NativeHttpResponse result) mutable {
          Response response{static_cast<http::status>(result.status), 11};
          response.set(http::field::content_type, result.content_type);
          response.set(http::field::cache_control, "no-store");
          response.body() = std::move(result.body);
          response.prepare_payload();
          reply(std::move(response));
        });
    return true;
  }

  // The operator console is optional; without it /admin/v1/* answers route_not_found.
  void set_admin(AdminApi *admin) {
    admin_ = admin;
  }

private:
  Response admin_response(const Request &request, bool peer_loopback) {
    if (!admin_) {
      return error_response(http::status::not_found, "route_not_found");
    }

    const auto method = request.method_string();
    const auto target = request.target();
    const auto authorization = request[http::field::authorization];
    const AdminHttpRequest admin_request{
        std::string_view(method.data(), method.size()),
        std::string_view(target.data(), target.size()),
        std::string_view(authorization.data(), authorization.size()), request.body(),
        peer_loopback};
    const auto result = admin_->handle(admin_request);

    Response response{static_cast<http::status>(result.status), 11};
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.set("X-Content-Type-Options", "nosniff");
    response.body() = result.body.dump();
    response.prepare_payload();
    return response;
  }

  std::string normalized_account(const std::string &account) const {
    // Invalid oversized names do not enter the bounded failure table.
    if (account.size() > 128) {
      return {};
    }
    std::string normalized = account;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    return normalized;
  }

  Response discord_response(const Request &request, const std::string &target) {
    const auto separator = target.find('?');
    const auto path = target.substr(0, separator);
    Response response{http::status::ok, 11};
    response.set(http::field::cache_control, "no-store");
    response.set("X-Content-Type-Options", "nosniff");
    response.set("Referrer-Policy", "no-referrer");
    response.set(http::field::content_type, "application/json");
    Json body;
    if (path == "/emulator/v1/auth-options" && request.method() == http::verb::get) {
      body = {{"discord", discord_oauth_.enabled()},
               {"minimum_password_bytes", kMinimumPasswordBytes}};
    } else if (!discord_oauth_.enabled()) {
      return error_response(http::status::service_unavailable, "discord_not_configured");
    } else if (path == "/emulator/v1/discord/callback" && request.method() == http::verb::get) {
      const auto query = oauth_query(separator == std::string::npos ? std::string_view{} :
                                     std::string_view(target).substr(separator + 1));
      if (!query.contains("state")) {
        return error_response(http::status::bad_request, "invalid_discord_callback");
      }
      const auto destination = discord_oauth_.callback(
          query.at("state"), query.contains("code") ? query.at("code") : "",
          query.contains("iss") ? query.at("iss") : "", query.contains("error"));
      response.result(http::status::see_other);
      response.set(http::field::location, destination);
      response.body().clear();
      response.prepare_payload();
      return response;
    } else if (request.method() == http::verb::post &&
               (path == "/emulator/v1/discord/start" || path == "/emulator/v1/discord/poll")) {
      if (request.body().size() > 2048 ||
          !request[http::field::content_type].starts_with("application/json")) {
        return error_response(http::status::bad_request, "invalid_discord_request");
      }
      const auto document = Json::parse(request.body());
      if (path.ends_with("/start")) {
        const auto token = document.value("link", false) ? bearer_token(request) : std::string{};
        if (document.value("link", false) && token.empty()) {
          return error_response(http::status::unauthorized, "authentication_required");
        }
        const auto port = document.at("loopback_port").get<std::int64_t>();
        if (port < 1024 || port > 65535) {
          return error_response(http::status::bad_request, "invalid_discord_callback_port");
        }
        body = discord_oauth_.begin(static_cast<unsigned short>(port), token);
      } else {
        body = discord_oauth_.poll(document.at("request_id").get<std::string>(),
                                   document.at("poll_secret").get<std::string>(),
                                   document.value("browser_proof", std::string{}));
      }
    } else {
      return error_response(http::status::method_not_allowed, "invalid_discord_method");
    }
    response.body() = body.dump();
    response.prepare_payload();
    return response;
  }

  AuthService &service_;
  NativeAuth &native_auth_;
  // Front REST quick slots; session checks stay in AuthService, storage in the repository.
  HotKeyRest hotkeys_;
  std::unique_ptr<MailRest> mail_;
  NativeWsDispatcher native_ws_;
  bool native_enabled_;
  bool world_enabled_;
  // Effective world socket cap; world.capacity above stays the advertised realm size.
  unsigned world_connection_limit_;
  bool require_launch_grant_;
  std::string public_address_;
  std::shared_ptr<distribution::PatchCatalog> patches_;
  NewsStore &news_;
  PortalStore &content_;
  FogRest fog_;
  DiscordOAuth discord_oauth_;
  AdminApi *admin_{};
  AuthAdmission admission_{16, 4, 12, 8192};
};

struct Connections {
  std::atomic_uint count{};
};

template <class Stream>
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession<Stream>> {
public:
  WebSocketSession(Stream stream, Router &router, asio::thread_pool &workers, std::string token,
                   std::shared_ptr<Connections> connections,
                   std::optional<NativeWsChannel> native_channel = std::nullopt)
      : websocket_(std::move(stream)), chat_timer_(websocket_.get_executor()), router_(router),
        workers_(workers), token_(std::move(token)), connections_(std::move(connections)),
        native_channel_(native_channel) {}

  ~WebSocketSession() {
    // I/O shutdown may destroy pending sessions after Router. Normal connection
    // teardown unregisters in stop(); remaining subscriptions die with the service.
    --connections_->count;
  }

  void run(Request request) {
    beast::get_lowest_layer(websocket_).expires_never();
    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::server);
    timeout.idle_timeout = kWebSocketIdleTimeout;
    websocket_.set_option(timeout);
    websocket_.read_message_max(kMessageBodyLimit);
    request_ = std::move(request);

    websocket_.async_accept(request_, [self = this->shared_from_this()](beast::error_code error) {
      if (!error) {
        if (self->native_channel_ == NativeWsChannel::front) {
          try {
            self->chat_connection_ = self->router_.native_chat().connect(self->token_);
            self->schedule_chat_delivery();
          } catch (const std::exception &) {
            self->close_invalid_native_request();
            return;
          }
        }
        self->read();
      }
    });
  }

private:
  void read() {
    if (stopped_ || closing_) {
      return;
    }
    websocket_.async_read(buffer_,
                          [self = this->shared_from_this()](beast::error_code error, std::size_t) {
                            self->on_read(error);
                          });
  }

  void on_read(beast::error_code error) {
    if (error) {
      stop();
      return;
    }

    auto request_text = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    if (native_channel_ && websocket_.got_text()) {
      close_invalid_native_request();
      return;
    }
    asio::post(workers_, [self = this->shared_from_this(), text = std::move(request_text)] {
      self->dispatch_rpc(text);
    });
  }

  void dispatch_rpc(const std::string &request_text) {
    if (native_channel_) {
      try {
        auto result = router_.native_rpc(*native_channel_, token_, request_text);
        asio::post(websocket_.get_executor(),
                   [self = this->shared_from_this(), response = std::move(result)] {
                     self->write(response);
                   });
      } catch (const std::exception &) {
        asio::post(websocket_.get_executor(),
                   [self = this->shared_from_this()] { self->close_invalid_native_request(); });
      }
      return;
    }

    Json result;
    try {
      result = router_.rpc(token_, Json::parse(request_text));
    } catch (const std::exception &) {
      result = {{"error", "invalid_rpc_envelope"}};
    }

    asio::post(websocket_.get_executor(),
               [self = this->shared_from_this(), text = result.dump()] { self->write(text); });
  }

  void write(const std::string &response_text, bool resume_read = true) {
    if (stopped_ || closing_) {
      return;
    }
    // ChatReq has no separate acknowledgement. Its echo arrives as ChatNoti.
    if (response_text.empty()) {
      if (resume_read) {
        read();
      }
      return;
    }
    if (outgoing_.size() >= kMaximumWebSocketQueuedMessages ||
        response_text.size() > native_ws::kMaximumMessageBytes - outgoing_bytes_) {
      close_invalid_native_request();
      return;
    }
    outgoing_.push_back({response_text, resume_read});
    outgoing_bytes_ += response_text.size();
    if (!writing_) {
      write_next();
    }
  }

  void write_next() {
    writing_ = true;
    websocket_.text(!native_channel_.has_value());
    websocket_.async_write(asio::buffer(outgoing_.front().text),
                           [self = this->shared_from_this()](beast::error_code error, std::size_t) {
                             self->writing_ = false;
                             if (error) {
                               self->stop();
                               return;
                             }
                             const bool resume_read = self->outgoing_.front().resume_read;
                             self->outgoing_bytes_ -= self->outgoing_.front().text.size();
                             self->outgoing_.pop_front();
                             if (self->closing_) {
                               self->begin_close();
                               return;
                             }
                             if (self->stopped_) {
                               return;
                             }
                             if (resume_read) {
                               self->read();
                             }
                             if (!self->outgoing_.empty()) {
                               self->write_next();
                             }
                           });
  }

  void schedule_chat_delivery() {
    chat_timer_.expires_after(kChatDeliveryInterval);
    chat_timer_.async_wait([self = this->shared_from_this()](beast::error_code error) {
      if (error || self->stopped_ || self->closing_ || !self->chat_connection_) {
        return;
      }
      const auto connection_id = *self->chat_connection_;
      asio::post(self->workers_, [self, connection_id] {
        try {
          auto messages = self->router_.native_chat().drain(connection_id);
          asio::post(self->websocket_.get_executor(), [self, messages = std::move(messages)] {
            if (self->stopped_ || self->closing_) {
              return;
            }
            for (const auto &message : messages) {
              self->write(message, false);
            }
            if (!self->closing_) {
              self->schedule_chat_delivery();
            }
          });
        } catch (const std::exception &) {
          asio::post(self->websocket_.get_executor(),
                     [self] { self->close_invalid_native_request(); });
        }
      });
    });
  }

  void disconnect_chat() {
    if (chat_connection_) {
      router_.native_chat().disconnect(*chat_connection_);
      chat_connection_.reset();
    }
  }

  void stop() {
    stopped_ = true;
    chat_timer_.cancel();
    disconnect_chat();
    beast::error_code ignored;
    beast::get_lowest_layer(websocket_).socket().close(ignored);
  }

  void close_invalid_native_request() {
    if (stopped_ || closing_) {
      return;
    }
    closing_ = true;
    chat_timer_.cancel();
    disconnect_chat();
    if (!writing_) {
      begin_close();
    }
  }

  void begin_close() {
    websocket_.async_close(websocket::close_code::policy_error,
                           [self = this->shared_from_this()](beast::error_code) { self->stop(); });
  }

  struct OutgoingMessage {
    std::string text;
    bool resume_read{};
  };

  websocket::stream<Stream> websocket_;
  asio::steady_timer chat_timer_;
  beast::flat_buffer buffer_;
  Request request_;
  std::deque<OutgoingMessage> outgoing_;
  std::size_t outgoing_bytes_{};
  bool writing_{};
  bool closing_{};
  bool stopped_{};
  std::optional<std::uint64_t> chat_connection_;
  Router &router_;
  asio::thread_pool &workers_;
  std::string token_;
  std::shared_ptr<Connections> connections_;
  std::optional<NativeWsChannel> native_channel_;
};

bool requires_auth_work(std::string_view path) {
  return path == "/emulator/v1/login" || path == "/emulator/v1/register" ||
         path == "/emulator/v1/launch-grant" || path.starts_with("/emulator/v1/discord/") ||
         path.starts_with("/launch/") || path.starts_with("/api/auth/") ||
         path == "/native/bootstrap";
}

template <class Stream>
class HttpSession : public std::enable_shared_from_this<HttpSession<Stream>> {
public:
  HttpSession(Stream stream, Router &router, asio::thread_pool &workers,
              asio::thread_pool &auth_workers, std::shared_ptr<Connections> connections)
      : stream_(std::move(stream)), router_(router), workers_(workers),
        auth_workers_(auth_workers), connections_(std::move(connections)) {
    parser_.body_limit(kMessageBodyLimit);
    parser_.header_limit(kHttpHeaderLimit);
  }

  ~HttpSession() {
    if (connections_) {
      --connections_->count;
    }
  }

  void run() {
    beast::get_lowest_layer(stream_).expires_after(kHttpTimeout);
    if constexpr (std::is_same_v<Stream, beast::tcp_stream>) {
      read_request();
    } else {
      stream_.async_handshake(asio::ssl::stream_base::server,
                              [self = this->shared_from_this()](beast::error_code error) {
                                if (!error) {
                                  self->read_request();
                                }
                              });
    }
  }

private:
  void read_request() {
    beast::get_lowest_layer(stream_).expires_after(kHttpTimeout);
    http::async_read(stream_, buffer_, parser_,
                     [self = this->shared_from_this()](beast::error_code error, std::size_t) {
                       self->on_read(error);
                     });
  }

  void on_read(beast::error_code error) {
    if (error) {
      return;
    }

    auto request = parser_.release();
    const auto target = request.target();
    auto path = std::string_view(target.data(), target.size());
    path = path.substr(0, path.find('?'));
    beast::error_code endpoint_error;
    const auto peer = beast::get_lowest_layer(stream_).socket().remote_endpoint(endpoint_error);
    const bool peer_loopback = !endpoint_error && peer.address().is_loopback();
    const auto peer_address = endpoint_error ? std::string("unknown") : peer.address().to_string();
    const bool native_auth = path == "/ws/auth";
    const bool native_front = path == "/ws/front";
    if (websocket::is_upgrade(request) &&
        (path == "/emulator/v1/lobby/ws" || native_auth || native_front)) {
      std::optional<NativeWsChannel> native_channel;
      if (native_auth) {
        native_channel = NativeWsChannel::auth;
      } else if (native_front) {
        native_channel = NativeWsChannel::front;
      }

      upgrade(std::move(request), native_channel, peer_address);
      return;
    }

    const bool expensive_auth = requires_auth_work(path);
    auto permit = expensive_auth ? router_.try_auth_work(peer_address) : nullptr;
    if (expensive_auth && !permit) {
      auto response = error_response(http::status::service_unavailable, "authentication_busy");
      response.set(http::field::retry_after, "1");
      write(std::move(response));
      return;
    }
    auto &pool = expensive_auth ? auth_workers_ : workers_;
    asio::post(pool, [self = this->shared_from_this(), request = std::move(request),
                      peer_loopback, peer_address, permit = std::move(permit)] {
      if (auto patch = self->router_.patch_blob(request)) {
        asio::post(self->stream_.get_executor(), [self, response = std::move(*patch)]() mutable {
          self->write(std::move(response));
        });
        return;
      }
      const auto reply = [self](Response response) {
        asio::post(self->stream_.get_executor(), [self, response = std::move(response)]() mutable {
          self->write(std::move(response));
        });
      };
      if (self->router_.handle_mail_async(request, reply)) {
        return;
      }
      auto response = self->router_.handle(request, peer_loopback, peer_address);
      asio::post(self->stream_.get_executor(), [self, response = std::move(response)]() mutable {
        self->write(std::move(response));
      });
    });
  }

  void upgrade(Request request, std::optional<NativeWsChannel> native_channel,
               const std::string &peer_address) {
    auto permit = router_.try_auth_work(peer_address);
    if (!permit) {
      auto response = error_response(http::status::service_unavailable, "authentication_busy");
      response.set(http::field::retry_after, "1");
      write(std::move(response));
      return;
    }
    auto token = bearer_token(request);
    // Local session handoff can verify a password; keep that work off the network executor.
    asio::post(auth_workers_, [self = this->shared_from_this(), request = std::move(request),
                               token = std::move(token), native_channel,
                               permit = std::move(permit)]() mutable {
      bool authenticated{};
      try {
        authenticated = self->router_.authenticated(token, native_channel.has_value());
      } catch (const std::exception &) {
        spdlog::error("WebSocket session authentication failed");
      }

      asio::post(self->stream_.get_executor(), [self, request = std::move(request),
                                                token = std::move(token), native_channel,
                                                authenticated]() mutable {
        if (!authenticated) {
          self->write(error_response(http::status::unauthorized, "authentication_required"));
          return;
        }

        // Transfer the connection count to the upgraded session.
        std::make_shared<WebSocketSession<Stream>>(std::move(self->stream_), self->router_,
                                                   self->workers_, std::move(token),
                                                   std::move(self->connections_), native_channel)
            ->run(std::move(request));
      });
    });
  }

  template <class Body> void write(http::response<Body> response) {
    response.keep_alive(false);
    auto outgoing = std::make_shared<http::response<Body>>(std::move(response));
    auto serializer = std::make_shared<http::response_serializer<Body>>(*outgoing);
    write_chunk(std::move(outgoing), std::move(serializer));
  }

  template <class Body>
  void write_chunk(std::shared_ptr<http::response<Body>> outgoing,
                   std::shared_ptr<http::response_serializer<Body>> serializer) {
    // Reset the deadline after progress, so large patch blobs do not need an unbounded timeout.
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(60));
    http::async_write_some(
        stream_, *serializer,
        [self = this->shared_from_this(), outgoing, serializer](beast::error_code error,
                                                                std::size_t) {
          if (error) {
            return;
          }
          if (!serializer->is_done()) {
            self->write_chunk(outgoing, serializer);
            return;
          }
          if constexpr (std::is_same_v<Stream, beast::tcp_stream>) {
            beast::error_code shutdown_error;
            beast::get_lowest_layer(self->stream_)
                .socket()
                .shutdown(tcp::socket::shutdown_send, shutdown_error);
          } else {
            beast::get_lowest_layer(self->stream_).expires_after(std::chrono::seconds(5));
            self->stream_.async_shutdown([self](beast::error_code) {});
          }
        });
  }

  Stream stream_;
  beast::flat_buffer buffer_;
  http::request_parser<http::string_body> parser_;
  Router &router_;
  asio::thread_pool &workers_;
  asio::thread_pool &auth_workers_;
  std::shared_ptr<Connections> connections_;
};

class Listener : public std::enable_shared_from_this<Listener> {
public:
  Listener(asio::io_context &io, tcp::endpoint endpoint, Router &router, asio::thread_pool &workers,
           asio::thread_pool &auth_workers, std::shared_ptr<asio::ssl::context> tls_context,
           unsigned max_connections)
      : acceptor_(io), router_(router), workers_(workers), auth_workers_(auth_workers),
        connections_(std::make_shared<Connections>()), tls_context_(std::move(tls_context)),
        max_connections_(max_connections) {
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
  }

  unsigned short port() const {
    return acceptor_.local_endpoint().port();
  }

  void run() {
    acceptor_.async_accept(
        asio::make_strand(acceptor_.get_executor()),
        [self = this->shared_from_this()](beast::error_code error, tcp::socket socket) {
          if (!error) {
            if (self->connections_->count.fetch_add(1) < self->max_connections_) {
              if (self->tls_context_) {
                using SecureStream = beast::ssl_stream<beast::tcp_stream>;
                auto stream = SecureStream(std::move(socket), *self->tls_context_);
                std::make_shared<HttpSession<SecureStream>>(std::move(stream), self->router_,
                                                            self->workers_, self->auth_workers_,
                                                            self->connections_)
                    ->run();
              } else {
                std::make_shared<HttpSession<beast::tcp_stream>>(
                    beast::tcp_stream(std::move(socket)), self->router_, self->workers_,
                    self->auth_workers_, self->connections_)
                    ->run();
              }
            } else {
              --self->connections_->count;
            }
          }
          if (self->acceptor_.is_open()) {
            self->run();
          }
        });
  }

private:
  tcp::acceptor acceptor_;
  Router &router_;
  asio::thread_pool &workers_;
  asio::thread_pool &auth_workers_;
  std::shared_ptr<Connections> connections_;
  std::shared_ptr<asio::ssl::context> tls_context_;
  unsigned max_connections_;
};

struct ServerOptions {
  std::string storage_backend{"postgres"};
  std::string listen_address{"127.0.0.1"};
  std::string public_address;
  std::string http_certificate;
  std::string http_private_key;
  std::string patch_directory;
  std::string patch_public_key;
  std::string migration_file;
  std::string auth_authority{kDefaultSeparateLogin ? "http://127.0.0.1:8090" : ""};
  std::string auth_service_key_file;
  unsigned port{kDefaultSeparateLogin ? 8082U : 8080U};
  unsigned max_http_connections{kDefaultMaximumHttpConnections};
  unsigned world_port{kDefaultSeparateLogin ? 8081u : 0u};
  std::chrono::milliseconds world_handshake_delay{};
  unsigned world_worker_threads{kDefaultWorldWorkerThreads};
  bool world_tls12_only{};
  bool world_credential_diagnostic{};
  bool world_bootstrap_diagnostic{};
  // Host part of the game_address sent in CharSelectRes. The listener itself always binds
  // to listen_address; this only changes how the client is told to reach it.
  std::string world_advertised_host{kDefaultWorldAdvertisedHost};
  // Realm presentation and admission policy. Empty text keeps the Config defaults.
  std::string world_name;
  std::string site_display_name;
  unsigned world_capacity{kDefaultWorldCapacity};
  // Validated --max-world-connections value; kDefaultMaximumWorldConnections when absent.
  unsigned max_world_connections{kDefaultMaximumWorldConnections};
  x5::pb::ServerLabel world_label{x5::pb::ServerLabel_None};
  bool world_maintenance{};
  // Opt-in loopback-only GET /internal/metrics; hooks cost one atomic load while disabled.
  bool metrics{};
  bool world_char_creation_restricted{};
  std::string certificate_file;
  std::string private_key_file;
  std::string protocol_directory{CHRONICLES_DEFAULT_PROTOCOL_DIR};
  std::string client_data_directory;
  // Full stationary placement file (tools/build_stationary_npc_placements.py) for one map.
  std::string npc_placements_file;
  bool native_client{kDefaultSeparateLogin};
  // CUSTOM (guessed, not official) monster loot rules. "--loot-custom-rules off" keeps only the
  // evidence-backed behavior; --loot-rules-file supplies {"custom_rules": {...}} switches and
  // guessed details (see docs/monster-loot.md).
  bool loot_custom_rules_enabled{true};
  std::string loot_rules_file;
  // Test kit for characters entering the overworld below the level cap (docs/graduation-grant.md).
  // "--graduation-grant off" turns it off.
  bool graduation_grant_enabled{true};
  bool native_identity_accounts{};
  bool native_require_launch_grant{kDefaultSeparateLogin};
  // Operator console (/admin/v1). Disabled unless a token file is supplied.
  std::string admin_token_file;
  std::string admin_settings_file;
  std::string server_messages_file;
  bool admin_allow_remote{};
  // Agent engine (AI chat REST; docs/agent-engine.md). Disabled while the port is 0.
  unsigned agent_engine_port{};
  std::string agent_engine_data_directory{CHRONICLES_DEFAULT_AGENT_ENGINE_DIR};
  bool agent_engine_allow_remote{};
  // Optional OpenAI-compatible chat-completions endpoint; empty keeps the scripted model.
  std::string agent_engine_model_url;
  std::string agent_engine_model_name;
  bool show_help{};
};

// Accepts DNS names and dotted IPv4 literals: letters, digits, '-' and '.', without empty
// labels. The value is copied into game_address, so ':' and other separators are rejected.
bool is_valid_advertised_host(std::string_view host) {
  if (host.empty() || host.size() > kMaximumHostNameLength) {
    return false;
  }
  if (host.front() == '.' || host.back() == '.' || host.find("..") != std::string_view::npos) {
    return false;
  }

  for (const char character : host) {
    const bool is_letter =
        (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    const bool is_digit = character >= '0' && character <= '9';
    if (!is_letter && !is_digit && character != '-' && character != '.') {
      return false;
    }
  }
  return true;
}

// Realm text is copied into protobuf strings rendered by the client. Multibyte UTF-8 is
// allowed; control characters and unbounded lengths are not.
std::string parse_realm_text(std::string_view option, const std::string &value) {
  validate_realm_text(option, value);
  return value;
}

unsigned parse_world_capacity(std::string_view value) {
  unsigned capacity{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), capacity);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || capacity == 0 ||
      capacity > kMaximumWorldCapacity) {
    throw std::invalid_argument("--world-capacity must be between 1 and " +
                                std::to_string(kMaximumWorldCapacity));
  }
  return capacity;
}

x5::pb::ServerLabel parse_world_label(std::string_view value) {
  if (value == "none") {
    return x5::pb::ServerLabel_None;
  }
  if (value == "new") {
    return x5::pb::ServerLabel_New;
  }
  if (value == "recommended") {
    return x5::pb::ServerLabel_Recommended;
  }
  throw std::invalid_argument("--world-label must be none, new or recommended");
}

ServerOptions parse_options(int argc, char **argv) {
  // Expand one JSON config before normal parsing; explicit CLI options retain precedence.
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) != "--config") {
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("--config requires a JSON file");
    }
    const auto document = Json::parse(distribution::read_bounded_file(argv[index + 1], 65536));
    if (!document.is_object() || document.contains("config")) {
      throw std::invalid_argument("Backend config must be a flat option object");
    }
    std::vector<std::string> arguments{argv[0]};
    for (const auto &[key, value] : document.items()) {
      if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) {
        continue;
      }
      if (!value.is_string() && !value.is_number_unsigned() && !value.is_boolean()) {
        throw std::invalid_argument("Backend config values must be strings, integers or booleans");
      }
      if (value.is_boolean() && !value.get<bool>()) {
        continue;
      }
      auto option = key;
      std::replace(option.begin(), option.end(), '_', '-');
      arguments.push_back("--" + option);
      arguments.push_back(value.is_string() ? value.get<std::string>() : value.dump());
    }
    for (int argument = 1; argument < argc; ++argument) {
      if (argument == index) {
        ++argument;
        continue;
      }
      if (std::string_view(argv[argument]) == "--config") {
        throw std::invalid_argument("Only one backend config file may be supplied");
      }
      arguments.emplace_back(argv[argument]);
    }
    std::vector<char *> expanded;
    for (auto &argument : arguments) {
      expanded.push_back(argument.data());
    }
    return parse_options(static_cast<int>(expanded.size()), expanded.data());
  }
  ServerOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help") {
      options.show_help = true;
      return options;
    }
    if (argument == "--metrics") {
      options.metrics = true;
      // A backend config file expands "metrics": true into "--metrics true".
      if (i + 1 < argc && std::string_view(argv[i + 1]) == "true") {
        ++i;
      }
      continue;
    }

    if (i + 1 >= argc) {
      throw std::invalid_argument("Missing option value");
    }
    const auto value = std::string(argv[++i]);
    if (argument == "--storage") {
      options.storage_backend = value;
    } else if (argument == "--bind-address") {
      asio::ip::make_address(value);
      options.listen_address = value;
    } else if (argument == "--public-address") {
      options.public_address = distribution::parse_service_origin(value).url;
    } else if (argument == "--auth-authority") {
      options.auth_authority = distribution::parse_service_origin(value).url;
    } else if (argument == "--auth-service-key-file") {
      options.auth_service_key_file = value;
    } else if (argument == "--http-tls-cert") {
      options.http_certificate = value;
    } else if (argument == "--http-tls-key") {
      options.http_private_key = value;
    } else if (argument == "--patch-dir") {
      options.patch_directory = value;
    } else if (argument == "--patch-public-key") {
      options.patch_public_key = value;
    } else if (argument == "--port" || argument == "--world-port" ||
               argument == "--agent-engine-port") {
      std::size_t consumed{};
      const auto parsed_port = std::stoul(value, &consumed);
      if (consumed != value.size() || parsed_port == 0 || parsed_port > kMaximumPort) {
        throw std::invalid_argument("Invalid port");
      }
      if (argument == "--port") {
        options.port = static_cast<unsigned>(parsed_port);
      } else if (argument == "--world-port") {
        options.world_port = static_cast<unsigned>(parsed_port);
      } else {
        options.agent_engine_port = static_cast<unsigned>(parsed_port);
      }
    } else if (argument == "--agent-engine-data") {
      options.agent_engine_data_directory = value;
    } else if (argument == "--agent-engine-allow-remote") {
      if (value != "true") {
        throw std::invalid_argument("--agent-engine-allow-remote requires true");
      }
      options.agent_engine_allow_remote = true;
    } else if (argument == "--agent-engine-model-url") {
      agent_engine::parse_chat_completion_url(value);
      options.agent_engine_model_url = value;
    } else if (argument == "--agent-engine-model") {
      options.agent_engine_model_name = value;
    } else if (argument == "--world-handshake-delay-ms") {
      std::chrono::milliseconds::rep delay_ms{};
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), delay_ms);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || delay_ms < 0 ||
          delay_ms > kMaximumWorldHandshakeDelay.count()) {
        throw std::invalid_argument("--world-handshake-delay-ms must be between 0 and 10000");
      }
      options.world_handshake_delay = std::chrono::milliseconds(delay_ms);
    } else if (argument == "--world-worker-threads") {
      options.world_worker_threads = parse_world_worker_threads(value);
    } else if (argument == "--world-advertised-host") {
      if (!is_valid_advertised_host(value)) {
        throw std::invalid_argument("--world-advertised-host must be a DNS name or IPv4 address");
      }
      options.world_advertised_host = value;
    } else if (argument == "--world-name") {
      options.world_name = parse_realm_text(argument, value);
    } else if (argument == "--site-display-name") {
      options.site_display_name = parse_realm_text(argument, value);
    } else if (argument == "--max-http-connections") {
      unsigned limit{};
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), limit);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || limit == 0 ||
          limit > kMaximumHttpConnections) {
        throw std::invalid_argument("--max-http-connections must be between 1 and " +
                                    std::to_string(kMaximumHttpConnections));
      }
      options.max_http_connections = limit;
    } else if (argument == "--world-capacity") {
      options.world_capacity = parse_world_capacity(value);
    } else if (argument == "--max-world-connections") {
      options.max_world_connections = parse_world_connection_limit(value);
    } else if (argument == "--world-label") {
      options.world_label = parse_world_label(value);
    } else if (argument == "--world-maintenance") {
      if (value != "true") {
        throw std::invalid_argument("--world-maintenance requires true");
      }
      options.world_maintenance = true;
    } else if (argument == "--world-char-creation-restricted") {
      if (value != "true") {
        throw std::invalid_argument("--world-char-creation-restricted requires true");
      }
      options.world_char_creation_restricted = true;
    } else if (argument == "--world-tls12-only") {
      if (value != "true") {
        throw std::invalid_argument("--world-tls12-only requires true");
      }
      options.world_tls12_only = true;
    } else if (argument == "--world-credential-diagnostic") {
      if (value != "true") {
        throw std::invalid_argument("--world-credential-diagnostic requires true");
      }
      options.world_credential_diagnostic = true;
    } else if (argument == "--world-bootstrap-diagnostic") {
      if (value != "true") {
        throw std::invalid_argument("--world-bootstrap-diagnostic requires true");
      }
      options.world_bootstrap_diagnostic = true;
    } else if (argument == "--migrate") {
      options.migration_file = value;
    } else if (argument == "--native-client" && value == "local") {
      options.native_client = true;
    } else if (argument == "--native-identity-accounts" && value == "true") {
      options.native_identity_accounts = true;
    } else if (argument == "--native-require-launch-grant" && value == "true") {
      options.native_require_launch_grant = true;
    } else if (argument == "--tls-cert") {
      options.certificate_file = value;
    } else if (argument == "--tls-key") {
      options.private_key_file = value;
    } else if (argument == "--protocol-dir") {
      options.protocol_directory = value;
    } else if (argument == "--client-data-dir") {
      options.client_data_directory = value;
    } else if (argument == "--npc-placements") {
      options.npc_placements_file = value;
    } else if (argument == "--loot-custom-rules") {
      if (value != "on" && value != "off") {
        throw std::invalid_argument("--loot-custom-rules must be on or off");
      }
      options.loot_custom_rules_enabled = value == "on";
    } else if (argument == "--loot-rules-file") {
      options.loot_rules_file = value;
    } else if (argument == "--graduation-grant") {
      if (value != "on" && value != "off") {
        throw std::invalid_argument("--graduation-grant must be on or off");
      }
      options.graduation_grant_enabled = value == "on";
    } else if (argument == "--admin-token-file") {
      options.admin_token_file = value;
    } else if (argument == "--admin-settings-file") {
      options.admin_settings_file = value;
    } else if (argument == "--server-messages-file") {
      options.server_messages_file = value;
    } else if (argument == "--admin-allow-remote") {
      if (value != "true") {
        throw std::invalid_argument("--admin-allow-remote requires true");
      }
      options.admin_allow_remote = true;
    } else {
      throw std::invalid_argument("Unknown option");
    }
  }

  if (options.world_port != 0 &&
      (options.certificate_file.empty() || options.private_key_file.empty())) {
    throw std::invalid_argument("World listener requires --tls-cert and --tls-key");
  }
  if (options.world_port == 0 &&
      (!options.certificate_file.empty() || !options.private_key_file.empty())) {
    throw std::invalid_argument("TLS certificate options require --world-port");
  }
  if (options.world_port == 0 &&
      options.world_handshake_delay != std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("--world-handshake-delay-ms requires --world-port");
  }
  if (options.world_port == 0 && options.world_advertised_host != kDefaultWorldAdvertisedHost) {
    throw std::invalid_argument("--world-advertised-host requires --world-port");
  }
  if (options.world_port == 0 && options.world_tls12_only) {
    throw std::invalid_argument("--world-tls12-only requires --world-port");
  }
  if (options.world_port == 0 && options.world_credential_diagnostic) {
    throw std::invalid_argument("--world-credential-diagnostic requires --world-port");
  }
  if (options.world_port == 0 && options.world_bootstrap_diagnostic) {
    throw std::invalid_argument("--world-bootstrap-diagnostic requires --world-port");
  }
  if (options.world_port == 0 && !options.npc_placements_file.empty()) {
    throw std::invalid_argument("--npc-placements requires --world-port");
  }
  if (options.world_port == 0 &&
      options.max_world_connections != kDefaultMaximumWorldConnections) {
    throw std::invalid_argument("--max-world-connections requires --world-port");
  }
  if (options.native_identity_accounts && !options.native_client) {
    throw std::invalid_argument("--native-identity-accounts requires --native-client local");
  }
  if (options.native_require_launch_grant && !options.native_client) {
    throw std::invalid_argument("--native-require-launch-grant requires --native-client local");
  }
  if (options.native_require_launch_grant && options.native_identity_accounts) {
    throw std::invalid_argument("Named-account grants cannot be combined with identity mapping");
  }
  if (options.admin_token_file.empty() &&
      (!options.admin_settings_file.empty() || options.admin_allow_remote)) {
    throw std::invalid_argument("Admin console options require --admin-token-file");
  }
  if (options.agent_engine_port == 0 &&
      (options.agent_engine_allow_remote || !options.agent_engine_model_url.empty())) {
    throw std::invalid_argument("Agent engine options require --agent-engine-port");
  }
  if (options.agent_engine_model_url.empty() != options.agent_engine_model_name.empty()) {
    throw std::invalid_argument(
        "--agent-engine-model-url and --agent-engine-model must be given together");
  }
  if (options.agent_engine_port != 0 &&
      (options.agent_engine_port == options.port ||
       options.agent_engine_port == options.world_port)) {
    throw std::invalid_argument("--agent-engine-port must differ from the other listeners");
  }

  if (options.http_certificate.empty() != options.http_private_key.empty()) {
    throw std::invalid_argument("HTTP TLS requires both certificate and private key");
  }
  if (options.patch_directory.empty() != options.patch_public_key.empty()) {
    throw std::invalid_argument("Patch service requires --patch-dir and --patch-public-key");
  }
  const bool tls = !options.http_certificate.empty();
  if (options.public_address.empty()) {
    options.public_address = std::string(tls ? "https://127.0.0.1:" : "http://127.0.0.1:") +
                             std::to_string(options.port);
  }
  const auto origin = distribution::parse_service_origin(options.public_address);
  if (origin.secure != tls) {
    throw std::invalid_argument("Advertised API scheme must match the HTTP listener TLS mode");
  }
  const bool remote_bind = !asio::ip::make_address(options.listen_address).is_loopback();
  if (remote_bind && origin.loopback) {
    throw std::invalid_argument("Remote listeners require a non-loopback advertised API origin");
  }
  if (remote_bind || !origin.loopback) {
    if (!tls || !options.native_client || !options.native_require_launch_grant ||
        options.native_identity_accounts) {
      throw std::invalid_argument("Remote listeners require HTTPS and strict named-account grants");
    }
    if (options.world_port && options.world_advertised_host == kDefaultWorldAdvertisedHost) {
      throw std::invalid_argument("Remote world listener requires an advertised world host");
    }
    if (options.world_credential_diagnostic || options.world_tls12_only ||
        options.world_handshake_delay != std::chrono::milliseconds::zero()) {
      throw std::invalid_argument("Local world diagnostics are not permitted on remote listeners");
    }
  }
  if (options.admin_allow_remote && !tls) {
    // The bearer token would otherwise cross the network in clear text.
    throw std::invalid_argument("--admin-allow-remote requires HTTPS (--http-tls-cert)");
  }
  return options;
}

// The agent engine's memories follow the account storage backend (schema version 26). Agents
// come from the client catalog and the agent-engine data directory.
std::unique_ptr<agent_engine::AgentEngine> create_agent_engine(const ServerOptions &options,
                                                               ClientCatalog *catalog) {
  auto data = agent_engine::load_agent_engine_data(catalog, options.agent_engine_data_directory);

  std::shared_ptr<agent_engine::AgentMemoryStore> memories;
  if (options.storage_backend == "postgres") {
    memories = std::make_shared<agent_engine::PostgresAgentMemoryStore>(
        environment_value("CHRONICLES_DATABASE_URL"));
  } else {
    memories = std::make_shared<agent_engine::InMemoryAgentMemoryStore>();
  }

  std::unique_ptr<agent_engine::DialogueModel> model =
      std::make_unique<agent_engine::ScriptedDialogueModel>();
  if (!options.agent_engine_model_url.empty()) {
    agent_engine::ChatCompletionConfig config;
    config.endpoint = agent_engine::parse_chat_completion_url(options.agent_engine_model_url);
    config.model = options.agent_engine_model_name;
    config.api_key = environment_value("CHRONICLES_AGENT_MODEL_API_KEY");
    model = std::make_unique<agent_engine::ChatCompletionModel>(std::move(config),
                                                                std::move(model));
  }

  auto engine = std::make_unique<agent_engine::AgentEngine>(
      std::move(data.agents), agent_engine::KnowledgeIndex(std::move(data.knowledge)),
      std::move(memories), std::move(model));
  spdlog::info("Agent engine: agents={} knowledge_entries={} model={}", engine->agent_count(),
               engine->knowledge_count(),
               options.agent_engine_model_url.empty() ? "scripted" : "chat-completions");
  if (engine->agent_count() == 0) {
    spdlog::warn("Agent engine has no agents; pass --client-data-dir or add agents.json");
  }
  return engine;
}

// Housing needs the verified client catalog. Its persistence follows the account backend; the
// housing tables are part of the startup migration (schema version 18). Returns null, leaving
// every housing request unsupported, when the catalog is absent or lacks a housing table.
// The optional doodad profile (tools/build_housing_data.py) enables gardens and interactive
// decorations; without it their interactions are refused.
std::shared_ptr<housing::HousingHub>
create_housing_hub(const std::shared_ptr<ClientCatalog> &catalog,
                   const Repositories &repositories,
                   const std::shared_ptr<GuildService> &guilds, int world_id,
                   const std::filesystem::path &doodad_profile_path) {
  if (!catalog) {
    spdlog::warn("Housing disabled: no client catalog is loaded");
    return {};
  }
  for (const auto &table : housing::HousingCatalog::required_tables()) {
    if (!catalog->has_table(table)) {
      spdlog::warn("Housing disabled: client catalog table {} is absent", table);
      return {};
    }
  }

  const auto epoch_ticks = [] {
    using EpochTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    return std::chrono::duration_cast<EpochTicks>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  auto service = std::make_shared<housing::HousingService>(
      housing::HousingCatalog::load(*catalog), repositories.housing, world_id, epoch_ticks,
      housing::HousingPolicy{},
      std::make_shared<GuildServiceDirectory>(guilds, repositories.character.characters));
  const auto zones = service->catalog().rentable_zones();
  spdlog::info("Housing service: rentable_zones={}", zones.size());
  if (std::filesystem::exists(doodad_profile_path)) {
    auto profile = housing::HousingDoodadProfile::load(doodad_profile_path);
    spdlog::info("Housing doodad profile loaded: doodads={} animations={}",
                 profile->doodad_count(), profile->animation_count());
    service->set_doodad_profile(std::move(profile), std::random_device{}());
  } else {
    spdlog::warn("Housing doodad profile absent: gardens and interactive decorations are "
                 "unavailable");
  }
  return std::make_shared<housing::HousingHub>(std::move(service));
}

void bootstrap_account(AuthService &service) {
  const auto account = environment_value("CHRONICLES_BOOTSTRAP_ACCOUNT");
  if (account.empty()) {
    return;
  }

  const auto password = environment_value("CHRONICLES_BOOTSTRAP_PASSWORD");
  if (password.empty()) {
    throw std::invalid_argument("Bootstrap password environment variable is required");
  }

  if (service.create_account(account, password)) {
    spdlog::info("Bootstrap account created");
  } else {
    spdlog::info("Bootstrap account already exists; password unchanged");
  }

  // Optional: lets a disposable memory-store server start with a game master account.
  // The role is only changed when this variable is set, so a persisted role is kept otherwise.
  const auto requested_role = environment_value("CHRONICLES_BOOTSTRAP_ACCOUNT_ROLE");
  if (requested_role.empty()) {
    return;
  }

  const auto role = parse_account_role(requested_role);
  if (!role) {
    throw std::invalid_argument("CHRONICLES_BOOTSTRAP_ACCOUNT_ROLE must be player or gm");
  }
  if (!service.set_account_role(account, *role)) {
    throw std::runtime_error("Bootstrap account role could not be applied");
  }
  spdlog::info("Bootstrap account role set to {}", account_role_name(*role));
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto options = parse_options(argc, argv);
    if (options.metrics) {
      world_metrics::enable();
    }
    if (options.show_help) {
      std::cout << "chronicles-server [--storage postgres|memory] [--port 8080] [--migrate "
                   "SQL_FILE] [--native-client local] [--world-port PORT --tls-cert PEM "
                   "--tls-key PEM] [--world-handshake-delay-ms 0..10000] "
                   "[--world-worker-threads 1..64] "
                   "[--world-advertised-host HOST] [--world-tls12-only true] [--protocol-dir DIR]"
                   " [--client-data-dir DIR] [--npc-placements FILE]"
                   " [--loot-custom-rules on|off] [--loot-rules-file JSON]"
                   " [--graduation-grant on|off]"
                   " [--world-credential-diagnostic true] [--world-bootstrap-diagnostic true]"
                   " [--native-identity-accounts true]"
                   " [--native-require-launch-grant true]"
                   " [--config JSON] [--bind-address IP] [--public-address ORIGIN]"
                   " [--http-tls-cert PEM --http-tls-key PEM]"
                   " [--auth-authority http://127.0.0.1:PORT --auth-service-key-file FILE]"
                   " [--patch-dir DIR --patch-public-key HEX]"
                   " [--world-name TEXT] [--site-display-name TEXT] [--world-capacity 1..100000]"
                   " [--max-world-connections 1..100000]"
                   " [--max-http-connections 1..100000]"
                   " [--world-label none|new|recommended] [--world-maintenance true]"
                   " [--metrics]"
                   " [--world-char-creation-restricted true]"
                   " [--admin-token-file FILE [--admin-settings-file FILE]"
                   " [--admin-allow-remote true]]"
                   " [--server-messages-file JSON]"
                   " [--agent-engine-port PORT [--agent-engine-data DIR]"
                   " [--agent-engine-allow-remote true]"
                   " [--agent-engine-model-url http://HOST:PORT[/v1] --agent-engine-model NAME]]"
                   "\nEnvironment: CHRONICLES_DATABASE_URL, "
                   "CHRONICLES_BOOTSTRAP_ACCOUNT, CHRONICLES_BOOTSTRAP_PASSWORD, "
                   "CHRONICLES_AGENT_MODEL_API_KEY\n";
      return 0;
    }

    // This lifetime spans migrations, recovery, listeners and the final worker joins.
    // PostgreSQL writers must also use the durable owner epoch for cross-connection fencing.
    std::unique_ptr<HostServerLease> host_lease;
    std::unique_ptr<PostgresServerLease> server_lease;
    if (options.storage_backend == "postgres") {
      try {
        server_lease = std::make_unique<PostgresServerLease>(
            environment_value("CHRONICLES_DATABASE_URL"));
        host_lease = std::make_unique<HostServerLease>(server_lease->database_identity());
      } catch (const std::exception &) {
        spdlog::error("Exclusive server lease unavailable or database unreachable");
        throw;
      }
      spdlog::info("PostgreSQL exclusive server lease acquired");
    }

    const auto started_at_unix = current_unix_seconds();
    // The console's live log view needs records from startup on, so this precedes loading.
    std::shared_ptr<AdminLogBuffer> admin_logs;
    if (!options.admin_token_file.empty()) {
      admin_logs = install_admin_log_buffer();
    }

    std::shared_ptr<ClientCatalog> client_catalog;
    std::shared_ptr<const TutorialData> tutorial_data;
    std::shared_ptr<const GatheringData> gathering_data;
    std::shared_ptr<const LootData> loot_data;
    std::shared_ptr<const ProcessingData> processing_data;
    std::shared_ptr<const CharacterStats> character_stats;
    if (!options.client_data_directory.empty()) {
      client_catalog = std::make_shared<ClientCatalog>(options.client_data_directory);
      const auto item_count = client_catalog->row_count("Item");
      const auto character_count = client_catalog->row_count("Character");
      character_stats = CharacterStats::load(*client_catalog);

      constexpr std::array<std::pair<int, std::string_view>, 4> starter_classes{{
          {1, "Fight"},
          {2, "Wild"},
          {3, "Adamant"},
          {5, "Magic"},
      }};
      for (const auto &[class_id, class_name] : starter_classes) {
        if (!client_catalog->find_row("Character", "class", std::string(class_name))) {
          throw std::runtime_error("Client catalog is missing the " + std::string(class_name) +
                                   " character definition");
        }

        for (const auto &starter_item : starter_gear_for_class(class_id)) {
          if (!client_catalog->find_row("Item", "id", std::to_string(starter_item.cms_id))) {
            throw std::runtime_error("Client catalog is missing starter item " +
                                     std::to_string(starter_item.cms_id));
          }
        }
      }

      spdlog::info("Client catalog loaded: tables={} items={} character_definitions={}",
                   client_catalog->table_count(), item_count, character_count);
      const auto tutorial_path =
          std::filesystem::path(options.client_data_directory).parent_path() / "tutorial" /
          "definitions.json";
      if (options.world_bootstrap_diagnostic && std::filesystem::exists(tutorial_path)) {
        tutorial_data = TutorialData::load(tutorial_path);
        spdlog::info("Tutorial definitions loaded: objects={} quests={} private_instances=true",
                     tutorial_data->objects.size(), tutorial_data->quests.size());
      }

      // Built by tools/build_gathering_data.py; absent profiles leave gathering disabled.
      const auto gathering_path =
          std::filesystem::path(options.client_data_directory).parent_path() / "gathering" /
          "definitions.json";
      if (options.world_bootstrap_diagnostic && std::filesystem::exists(gathering_path)) {
        gathering_data = GatheringData::load(gathering_path);
        spdlog::info("Gathering definitions loaded: points={} supported_definitions={} "
                     "doodad_points={}",
                     gathering_data->point_count(), gathering_data->definition_count(),
                     gathering_data->doodad_point_count());
      }

      // Built by tools/build_loot_data.py; absent profiles leave monster corpses unlootable.
      const auto loot_path = std::filesystem::path(options.client_data_directory).parent_path() /
                             "loot" / "definitions.json";
      if (options.world_bootstrap_diagnostic && std::filesystem::exists(loot_path)) {
        // CUSTOM (guessed, not official) rules are on by default; see docs/monster-loot.md.
        auto loot_rules = LootCustomRules::all_enabled();
        if (!options.loot_custom_rules_enabled) {
          loot_rules = LootCustomRules::evidence_only();
        } else if (!options.loot_rules_file.empty()) {
          loot_rules = LootCustomRules::load(options.loot_rules_file);
        }
        loot_data = LootData::load(loot_path, loot_rules);
        spdlog::info("Loot definitions loaded: npcs={} packs={} withheld_groups={}",
                     loot_data->npc_count(), loot_data->pack_count(),
                     loot_data->withheld_group_count());
        spdlog::info("Loot {}", loot_rules.describe());
      }

      // Built by tools/build_processing_data.py; absent profiles leave crafting stations
      // (processing and campfire cooking) unhandled.
      const auto processing_path =
          std::filesystem::path(options.client_data_directory).parent_path() / "processing" /
          "definitions.json";
      if (options.world_bootstrap_diagnostic && std::filesystem::exists(processing_path)) {
        processing_data = ProcessingData::load(processing_path);
        spdlog::info("Processing definitions loaded: station_placements={} recipes={}",
                     processing_data->placement_count(), processing_data->recipe_count());
      }
    }

    std::shared_ptr<const NpcPopulation> npc_population;
    if (!options.npc_placements_file.empty()) {
      npc_population = load_npc_population_file(options.npc_placements_file, kMaxPopulatedNpcUnits);
      spdlog::info("NPC placements loaded: map={} units={} skipped_inactive={}",
                   npc_population->map_id(), npc_population->units().size(),
                   npc_population->skipped_inactive());
    }

    auto repositories = create_repositories(options.storage_backend, options.migration_file);
    NewsStore news(options.storage_backend == "postgres"
                       ? environment_value("CHRONICLES_DATABASE_URL")
                       : std::string{});
    PortalStore content(options.storage_backend == "postgres"
                            ? environment_value("CHRONICLES_DATABASE_URL")
                            : std::string{});
    Config config;
    if (!options.auth_authority.empty()) {
      if (!server_lease || options.storage_backend != "postgres") {
        throw std::invalid_argument("Separate login/game services require PostgreSQL storage");
      }
      config.authority_database_identity = server_lease->database_identity();
    }
    if (!options.world_name.empty()) {
      config.world_name = options.world_name;
    }
    if (!options.site_display_name.empty()) {
      config.site_display_name = options.site_display_name;
    }
    config.world_capacity = options.world_capacity;
    config.world_label = options.world_label;
    config.world_maintenance = options.world_maintenance;
    config.char_creation_restricted = options.world_char_creation_restricted;
    const auto local_address = options.public_address;
    config.lobby_address = local_address;
    if (!options.native_client) {
      config.lobby_address += "/emulator/v1";
    }
    if (options.world_port != 0) {
      config.game_address =
          options.world_advertised_host + ":" + std::to_string(options.world_port);
    }
    AuthService service(repositories.character, config);
    if (!options.auth_authority.empty()) {
      const auto key_file = options.auth_service_key_file.empty()
                                ? environment_value("CHRONICLES_AUTH_SERVICE_KEY_FILE")
                                : options.auth_service_key_file;
      auto authority = make_local_auth_authority(options.auth_authority,
                                                   read_auth_service_key(key_file),
                                                   config.authority_database_identity);
      const auto status = authority->call("service_status", Json::object());
      if (status.at("protocol_version").get<int>() != 1 ||
          status.at("service").get<std::string>() != "login" ||
          status.at("world_id").get<int>() != config.world_id ||
          status.at("database_identity").get<std::string>() != config.authority_database_identity) {
        throw std::invalid_argument("Login authority does not match this database and world");
      }
      service.set_auth_authority(std::move(authority));
      service.update_realm_settings(service.realm_settings());
      spdlog::info("Separate login authority verified for this game server");
    }
    const auto reward_owner_epoch = service.acquire_monster_reward_owner_epoch();
    const auto recovered_rewards = recover_startup_rewards(
        kMonsterRewardRecoveryPageSize, kMaximumStartupRewardRecovery,
        [&service](std::size_t limit) {
          return service.recover_staged_monster_rewards(limit);
        },
        [&service] {
          return service.has_staged_monster_rewards();
        });
    spdlog::info("Monster reward recovery completed: epoch={} staged={}", reward_owner_epoch,
                 recovered_rewards);
    // Finalize committed gathering receipts under the acquired owner epoch before any
    // listener can expose their source or let a character enter the world.
    const auto recovered_gathering = recover_startup_rewards(
        kMonsterRewardRecoveryPageSize, kMaximumStartupRewardRecovery,
        [&repositories](std::size_t limit) {
          return repositories.character.character_state->recover_gathering_claims(
              limit, unix_epoch_ticks_now());
        },
        [&repositories] {
          return repositories.character.character_state->has_unfinished_gathering_claims();
        });
    spdlog::info("Gathering claim recovery completed: staged={}", recovered_gathering);
    // Resolve committed shared boss scripts before hydration and player admission.
    // Private copies from the previous process no longer exist under the exclusive
    // lifetime lease, so their intents are retired without affecting a new copy.
    std::size_t recovered_boss_scripts = 0;
    std::optional<std::string> boss_cursor;
    while (true) {
      const auto remaining = kMaximumStartupRewardRecovery - recovered_boss_scripts;
      const auto requested = std::min(kMonsterRewardRecoveryPageSize, remaining + 1);
      const auto pending = service.pending_boss_scripts(config.world_id, boss_cursor, requested);
      if (pending.size() > remaining) {
        throw std::runtime_error("Pending boss script recovery exceeds the startup limit");
      }
      if (pending.empty()) {
        break;
      }
      for (const auto &intent : pending) {
        if (boss_cursor && intent.event_id <= *boss_cursor) {
          throw std::runtime_error("Boss script recovery page is not ordered");
        }
        if (intent.copy_scope == "shared") {
          if (!gathering_data || !gathering_data->has_map(intent.map_id)) {
            throw std::runtime_error("Boss script recovery requires its gathering profile");
          }
          if (!service.settle_shared_boss_script(intent).committed) {
            throw std::runtime_error("Shared boss script recovery needs reconciliation");
          }
        } else if (!service.retire_private_boss_script(intent)) {
          throw std::runtime_error("Private boss script recovery needs reconciliation");
        }
        boss_cursor = intent.event_id;
        ++recovered_boss_scripts;
      }
      if (pending.size() < requested) {
        break;
      }
    }
    spdlog::info("Boss script recovery completed: staged={}", recovered_boss_scripts);
    bootstrap_account(service);

    // Company and guild persistence must share the explicitly acquired reward owner epoch
    // before their constructors can repair, persist, or recover any service state.
    repositories.companies->adopt_quest_owner_epoch(reward_owner_epoch);
    repositories.guilds->adopt_quest_service_owner_epoch(reward_owner_epoch);

    const auto company_rules =
        client_catalog ? load_company_rules(*client_catalog) : CompanyRules{};
    auto companies = std::make_shared<CompanyService>(repositories.companies, company_rules,
                                                      config.world_id);
    const auto company_dialogues =
        client_catalog ? CompanyDialogueIndex::load(*client_catalog) : nullptr;
    spdlog::info("Company service: companies={} dialogue_selections={}",
                 company_rules.companies.size(),
                 company_dialogues ? company_dialogues->selection_count() : 0);

    auto guild_rules = client_catalog ? load_guild_rules(*client_catalog) : GuildRules{};
    // Without a company catalog nobody can hold a company, so guilds skip those checks.
    guild_rules.companies_enabled = !company_rules.companies.empty();
    auto guilds = std::make_shared<GuildService>(repositories.guilds, guild_rules,
                                                 config.world_id);
    spdlog::info("Guild service: name_length={}..{} sub_masters={} levels={} "
                 "invite_timeout_ms={} creatable_company_position={} companies_enabled={}",
                 guild_rules.name_min_length, guild_rules.name_max_length,
                 guild_rules.max_sub_masters, guild_rules.levels.size(),
                 guild_rules.invite_timeout.count(), guild_rules.creatable_company_position,
                 guild_rules.companies_enabled);
    // Startup settlement runs before any listener admits players. A completed prior-process
    // event is already reflected in the service snapshots loaded by these constructors.
    auto quest_listener = std::make_shared<std::weak_ptr<NativeWorldListener>>();
    QuestEffectComponent company_effects;
    company_effects.settle = [companies](const QuestServiceEffectEvent &event) {
      return companies->settle_quest_effect(event).status;
    };
    company_effects.reconcile_uncertain = [companies]() -> std::optional<QuestEffectReconciliation> {
      const auto result = companies->reconcile_uncertain_quest_effect();
      if (!result) {
        return std::nullopt;
      }
      return QuestEffectReconciliation{result->status, result->record.char_id,
                                       result->effect_id, result->event_completed};
    };
    company_effects.resynchronize = [quest_listener](std::int64_t character_id) {
      if (const auto listener = quest_listener->lock()) {
        listener->resynchronize_quest_company(character_id);
      }
    };
    QuestEffectComponent guild_effects;
    guild_effects.settle = [guilds](const QuestServiceEffectEvent &event) {
      return guilds->settle_quest_effect(event);
    };
    guild_effects.reconcile_uncertain = [guilds]() -> std::optional<QuestEffectReconciliation> {
      if (!guilds->reconcile_uncertain_progression()) {
        return QuestEffectReconciliation{QuestEffectSettlementStatus::retry};
      }
      return guilds->reconcile_uncertain_quest_effect();
    };
    guild_effects.resynchronize = [guilds](std::int64_t character_id) {
      guilds->resynchronize_quest_effect(character_id);
    };
    auto quest_dispatcher = std::make_shared<QuestEffectDispatcher>(
        repositories.character.character_state, std::move(company_effects),
        std::move(guild_effects));
    const auto recovered_quest_effects = quest_dispatcher->recover_startup(
        config.world_id, kMaximumStartupRewardRecovery);
    spdlog::info("Quest service recovery completed: events={}", recovered_quest_effects);
    const auto housing_hub = create_housing_hub(
        client_catalog, repositories, guilds, config.world_id,
        std::filesystem::path(options.client_data_directory).parent_path() / "housing" /
            "doodads.json");
    if (housing_hub) {
      // Repairs guild housing whose cleanup was lost before the last stop. No world session
      // exists yet, so every release runs directly against storage.
      const auto failed_cleanups = housing_hub->reconcile_guild_housing();
      if (failed_cleanups != 0) {
        spdlog::warn("Guild housing reconciliation left {} cleanups for the housing sweep",
                     failed_cleanups);
      }
    }

    const auto friend_rules = client_catalog ? load_friend_rules(*client_catalog) : FriendRules{};
    auto friends = std::make_shared<FriendService>(
        repositories.friends,
        std::make_shared<CharacterFriendDirectory>(repositories.character.characters, guilds),
        friend_rules, config.world_id);
    spdlog::info("Friend service: max_friends={}", friend_rules.max_friends);
    std::shared_ptr<housing::HousingService> housing;
    std::shared_ptr<GuildHousingRequests> guild_housing_view;
    if (housing_hub) {
      housing = std::shared_ptr<housing::HousingService>(housing_hub, &housing_hub->service());
      // Read-only GuildRes housing view. HousingHub alone owns requests and guild events.
      guild_housing_view = std::make_shared<GuildHousingCoordinator>(housing, guilds);
    }
    service.set_character_social_state(
        std::make_shared<CharacterSocialDirectory>(companies, guilds, housing));

    NativeAuthConfig native_config;
    native_config.enabled = options.native_client;
    native_config.public_address = local_address;
    const auto client_region = environment_value("CHRONICLES_CLIENT_REGION");
    if (!client_region.empty()) {
      native_config.region = client_region;
    }
    native_config.local_account = environment_value("CHRONICLES_BOOTSTRAP_ACCOUNT");
    native_config.local_password = environment_value("CHRONICLES_BOOTSTRAP_PASSWORD");
    native_config.separate_accounts_by_identity = options.native_identity_accounts;
    native_config.require_launch_grant = options.native_require_launch_grant;
    native_config.client_version = environment_value("CHRONICLES_CLIENT_VERSION");
    if (native_config.client_version.empty()) {
      native_config.client_version = kSupportedClientVersion;
    }
    auto client_protocol_hex = environment_value("CHRONICLES_CLIENT_PROTOCOL_HEX");
    if (client_protocol_hex.empty()) {
      client_protocol_hex = kSupportedClientProtocolHex;
    }
    native_config.client_protocol = hex_decode(client_protocol_hex);
    const auto native_config_client_version = native_config.client_version;
    NativeAuth native_auth(service, std::move(native_config));

    // Created before the worker pools so it outlives every queued agent-engine request.
    std::unique_ptr<agent_engine::AgentEngine> agent_engine_service;
    std::unique_ptr<agent_engine::AgentEngineRest> agent_engine_rest;
    if (options.agent_engine_port != 0) {
      agent_engine_service = create_agent_engine(options, client_catalog.get());
      agent_engine_rest = std::make_unique<agent_engine::AgentEngineRest>(*agent_engine_service);
    }

    // Handlers still queued in io_context own sessions whose strands belong to workers, so the
    // pool is declared first and destroyed last.
    spdlog::info("World worker pool: {} threads", options.world_worker_threads);
    asio::thread_pool workers(options.world_worker_threads);
    // Guild cleanup may wait on world-session strands. Keep those waits off workers.
    asio::thread_pool housing_events(1);
    // Bound potentially blocking authentication separately from gameplay work.
    asio::thread_pool auth_workers(kAuthWorkerThreadCount);
    // Agent-engine requests may wait seconds for a language model; they never block `workers`.
    asio::thread_pool agent_engine_workers(kAgentEngineWorkerThreadCount);
    asio::io_context io_context;
    std::shared_ptr<AuthHeartbeat> auth_heartbeat;
    if (!options.auth_authority.empty() && options.world_port != 0) {
      auth_heartbeat = std::make_shared<AuthHeartbeat>(io_context, auth_workers, service);
    }
    if (housing_hub) {
      // Guild subscribers must not block. A separate event executor waits for a departing
      // member's session strand without consuming the world worker needed to run it.
      std::weak_ptr<housing::HousingHub> weak_housing = housing_hub;
      guilds->subscribe([weak_housing, &housing_events](const std::vector<GuildEvent> &events) {
        for (const auto &event : events) {
          if (const auto *left = std::get_if<GuildMemberLeft>(&event)) {
            asio::post(housing_events, [weak_housing, guild_id = left->guild_id, char_id = left->char_id,
                                 kicked = left->kicked] {
              if (const auto hub = weak_housing.lock()) {
                hub->guild_member_left(guild_id, char_id, kicked);
              }
            });
          } else if (const auto *disbanded = std::get_if<GuildDisbanded>(&event)) {
            asio::post(housing_events, [weak_housing, guild_id = disbanded->guild_id] {
              if (const auto hub = weak_housing.lock()) {
                hub->guild_disbanded(guild_id);
              }
            });
          }
        }
      });
    }
    std::shared_ptr<asio::ssl::context> http_tls;
    if (!options.http_certificate.empty()) {
      http_tls = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
      http_tls->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                            asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                            asio::ssl::context::no_tlsv1_1);
      http_tls->use_certificate_chain_file(options.http_certificate);
      http_tls->use_private_key_file(options.http_private_key, asio::ssl::context::pem);
      if (SSL_CTX_check_private_key(http_tls->native_handle()) != 1) {
        throw std::invalid_argument("HTTP certificate and private key do not match");
      }
    }
    std::shared_ptr<distribution::PatchCatalog> patches;
    if (!options.patch_directory.empty()) {
      patches = std::make_shared<distribution::PatchCatalog>(
          options.patch_directory, options.patch_public_key, options.public_address);
    }
    auto world_listener =
        options.world_port == 0
            ? std::shared_ptr<NativeWorldListener>()
            : start_native_world(io_context, workers, service,
                                 tcp::endpoint(asio::ip::make_address(options.listen_address),
                                               static_cast<unsigned short>(options.world_port)),
                                 options.protocol_directory, options.certificate_file,
                                 options.private_key_file, options.world_handshake_delay,
                                 options.world_tls12_only, options.world_credential_diagnostic,
                                 options.world_bootstrap_diagnostic, tutorial_data, character_stats,
                                 client_catalog, gathering_data, loot_data, processing_data,
                                 options.client_data_directory, npc_population, guilds,
                                 housing_hub, options.max_world_connections,
                                 WorldSocialServices{guilds, companies, company_dialogues, {}});
    *quest_listener = world_listener;
    Router router(service, native_auth, options.native_client, bool(world_listener),
                  options.native_require_launch_grant, local_address, patches, news, content,
                  *repositories.hotkeys, *repositories.fog,
                  guilds, housing_hub, client_catalog.get(), friends,
                  options.world_port != 0 ? options.max_world_connections : 0u,
                  companies, guild_housing_view);
    if (world_listener) {
      world_listener->set_repositories({repositories.mail, repositories.market,
                                        repositories.ranking, repositories.fog});
      if (options.graduation_grant_enabled) {
        world_listener->enable_graduation_grant();
      }
      world_listener->set_entry_admission_wait(kWorldEntryAdmissionWait);
    }
    router.set_mail(std::make_unique<MailRest>(
        service, *repositories.mail,
        [weak_world = std::weak_ptr<NativeWorldListener>(world_listener)](
            const Character &character, std::int64_t mail_id, MailRest::ClaimReply reply) {
          const auto world = weak_world.lock();
          if (!world || !world->claim_mail(character, mail_id, reply)) {
            reply(MailClaimStatus::conflict);
          }
        },
        unix_epoch_ticks_now));

    auto messages_config = options.server_messages_file.empty()
                               ? ServerMessagesConfig{}
                               : load_server_messages_config(options.server_messages_file);
    auto server_messages = std::make_shared<ServerMessages>(
        std::move(messages_config),
        [&router, weak_world = std::weak_ptr<NativeWorldListener>(world_listener),
         world_id = config.world_id](const std::string &sender, const ServerMessage &message) {
          if (message.type == ServerMessageType::chat) {
            router.native_chat().broadcast_notice(world_id, sender, message.text);
          } else if (const auto world = weak_world.lock()) {
            world->broadcast_system_notice(message.text, SystemNoticeStyle::banner);
          }
        },
        [&router](const ChatIdentity &recipient, const std::string &sender,
                  const std::string &message) {
          router.native_chat().private_notice(recipient.world_id, recipient.character_id, sender,
                                              message);
        });
    if (world_listener) {
      world_listener->set_world_entry_handler(
          [server_messages, friends](const ChatIdentity &identity) {
            server_messages->world_entered(identity);
            try {
              friends->character_entered_world(identity.world_id, identity.character_id);
            } catch (const std::exception &error) {
              spdlog::error("Friend presence entry failed: {}", error.what());
            }
          });
      // Friends see the character offline again, with its departure as the last login.
      world_listener->set_world_exit_handler([friends](const ChatIdentity &identity) {
        friends->character_left_world(identity.world_id, identity.character_id);
      });
    }
    // A queued player command reports its outcome later as a private "[Server]" chat line.
    const auto server_reply = [&router](const ChatIdentity &identity) {
      return [&router, identity](std::string result) {
        try {
          router.native_chat().private_notice(identity.world_id, identity.character_id, "[Server]",
                                              result);
        } catch (const std::exception &) {
          spdlog::warn("Player command reply could not be queued");
        }
      };
    };
    auto player_commands = std::make_shared<PlayerCommands>(
        [weak_world = std::weak_ptr<NativeWorldListener>(world_listener),
         server_reply](const ChatIdentity &identity, std::optional<bool> enabled) -> std::string {
          const auto world = weak_world.lock();
          if (!world) {
            return "PvP is unavailable while the world service is offline.";
          }
          if (!world->player_pvp(identity, enabled, server_reply(identity))) {
            return "Enter the game world before using /pvp.";
          }
          return "PvP request queued.";
        },
        Clock::now,
        [weak_world = std::weak_ptr<NativeWorldListener>(world_listener),
         server_reply](const ChatIdentity &identity, PlayerMountAction action) -> std::string {
          const auto world = weak_world.lock();
          if (!world) {
            return "Mounts are unavailable while the world service is offline.";
          }
          if (!world->player_mount(identity, action, server_reply(identity))) {
            return "Enter the game world before using mount commands.";
          }
          // Queued: the world session sends the outcome line.
          return {};
        },
        [weak_world = std::weak_ptr<NativeWorldListener>(world_listener),
         server_reply](const ChatIdentity &identity) -> std::string {
          const auto world = weak_world.lock();
          if (!world) {
            return "Unstuck is unavailable while the world service is offline.";
          }
          if (!world->player_unstuck(identity, server_reply(identity))) {
            return "Enter the game world before using !unstuck.";
          }
          return {};
        });
    router.set_player_commands(player_commands);
    if (world_listener) {
      // Local chat reaches the speaker and the characters that currently see the speaker.
      router.set_local_chat_audience(
          [weak_world = std::weak_ptr<NativeWorldListener>(world_listener)](
              const ChatIdentity &speaker) -> std::vector<std::int64_t> {
            const auto world = weak_world.lock();
            if (!world) {
              return {};
            }
            return world->local_chat_audience(speaker);
          });
    }
    if (world_listener) {
      // GM command replies arrive as a private "[GM]" chat line; the client discards DebugText.
      world_listener->set_gm_reply_handler(
          [&router](const ChatIdentity &identity, const std::string &text) {
            router.native_chat().private_notice(identity.world_id, identity.character_id, "[GM]",
                                                text);
          });
      // GM announcements reuse the operator notice path: one World-channel line per character.
      world_listener->set_gm_announce_handler(
          [&router](const ChatIdentity &identity, const std::string &text) {
            return router.native_chat().broadcast_notice(identity.world_id, "[GM]", text);
          });
    }
    if (world_listener) {
      world_listener->set_player_command_handler(
          [player_commands, &router](const ChatIdentity &identity, const std::string &text) {
            try {
              const auto result = player_commands->execute(identity, text);
              for (const auto &reply : result.replies) {
                router.native_chat().private_notice(identity.world_id, identity.character_id,
                                                    "[Server]", reply);
              }
              return result.handled;
            } catch (const std::length_error &) {
              // A command flood is consumed privately; it must never become party chat.
              return !text.empty() && (text.front() == '/' || text.front() == '!');
            }
          });
    }
    auto quest_effect_timer = std::make_shared<QuestEffectTimer>(
        io_context, workers, quest_dispatcher, config.world_id);
    quest_effect_timer->run();
    auto message_timer = std::make_shared<ServerMessageTimer>(io_context, server_messages);
    message_timer->run();

    // Both SIGINT/SIGTERM and the console's shutdown action stop the server this way.
    bool shutdown_requested = false;
    auto shutdown_completed = std::make_shared<std::atomic<bool>>(false);
    asio::steady_timer shutdown_retry_timer(io_context);
    unsigned shutdown_start_failures = 0;
    std::function<void()> stop_server;
    stop_server = [&] {
      if (auth_heartbeat) {
        auth_heartbeat->stop();
      }
      try_start_drained_shutdown(
          shutdown_requested,
          [&] {
            if (world_listener) {
              world_listener->shutdown_when_drained(
                  [&io_context, shutdown_completed, quest_effect_timer] {
                    quest_effect_timer->stop();
                    shutdown_completed->store(true);
                    io_context.stop();
                  });
              return;
            }
            quest_effect_timer->stop();
            shutdown_completed->store(true);
            io_context.stop();
          },
          [&] {
            spdlog::critical("World shutdown could not start; retained owners remain fenced "
                             "and the drain will be retried");
            if (shutdown_start_failures < 5) {
              ++shutdown_start_failures;
            }
            const auto seconds = std::min(1U << shutdown_start_failures, 30U);
            try {
              shutdown_retry_timer.expires_after(std::chrono::seconds(seconds));
              shutdown_retry_timer.async_wait([&](const boost::system::error_code &error) {
                if (!error) {
                  stop_server();
                } else if (error != asio::error::operation_aborted) {
                  spdlog::critical("World shutdown retry timer failed: {}", error.message());
                }
              });
            } catch (...) {
              spdlog::critical("World shutdown retry could not be scheduled; "
                               "retained owners remain fenced");
            }
          });
    };

    // The console's shutdown waits briefly so its HTTP 202 reply can still be written.
    constexpr auto kConsoleShutdownDelay = std::chrono::milliseconds(500);
    asio::steady_timer console_shutdown_timer(io_context);
    std::unique_ptr<AdminApi> admin;
    if (!options.admin_token_file.empty()) {
      AdminServerInfo info;
      info.client_version = native_config_client_version;
      info.storage_backend = options.storage_backend;
      info.listen_address = options.listen_address;
      info.public_address = local_address;
      info.http_port = options.port;
      info.world_port = options.world_port;
      info.native_client = options.native_client;
      info.require_launch_grant = options.native_require_launch_grant;
      info.world_bootstrap_diagnostic = options.world_bootstrap_diagnostic;
      info.client_catalog_loaded = client_catalog != nullptr;
      info.started_at_unix = started_at_unix;

      AdminServices services{service,
                             repositories.character,
                             repositories.mail,
                             game_rates(),
                             admin_logs,
                             world_listener,
                             &router.native_chat(),
                             [&] {
                               asio::post(io_context, [&] {
                                 console_shutdown_timer.expires_after(kConsoleShutdownDelay);
                                 console_shutdown_timer.async_wait(
                                     [&](beast::error_code) { stop_server(); });
                               });
                             },
                             options.admin_settings_file,
                             info,
                             config.world_id};
      services.guilds = guilds;
      services.server_messages = server_messages;
      if (client_catalog &&
          std::ranges::all_of(ShopCatalog::required_tables(), [&](const auto &table) {
            return client_catalog->has_table(table);
          })) {
        services.mail_catalog = ShopCatalog::load(*client_catalog);
      }
      services.companies = companies;
      admin = std::make_unique<AdminApi>(std::move(services),
                                         load_or_create_admin_token(options.admin_token_file),
                                         options.admin_allow_remote);
      admin->load_saved_settings();
      router.set_admin(admin.get());
      spdlog::info("Admin console API enabled ({}; token file supplied)",
                   options.admin_allow_remote ? "remote HTTPS allowed" : "loopback only");
    }

    auto listener =
        std::make_shared<Listener>(io_context,
                                   tcp::endpoint(asio::ip::make_address(options.listen_address),
                                                 static_cast<unsigned short>(options.port)),
                                   router, workers, auth_workers, http_tls,
                                   options.max_http_connections);
    spdlog::info("HTTP and WebSocket connection limit: {}", options.max_http_connections);
    listener->run();
    if (auth_heartbeat) {
      auth_heartbeat->start();
    }

    std::shared_ptr<agent_engine::AgentEngineListener> agent_engine_listener;
    if (agent_engine_rest) {
      agent_engine_listener = std::make_shared<agent_engine::AgentEngineListener>(
          io_context,
          tcp::endpoint(asio::ip::make_address(options.listen_address),
                        static_cast<unsigned short>(options.agent_engine_port)),
          *agent_engine_rest, agent_engine_workers, options.agent_engine_allow_remote);
      agent_engine_listener->run();
      spdlog::info("Agent engine listening on {}:{} ({})", options.listen_address,
                   agent_engine_listener->port(),
                   options.agent_engine_allow_remote ? "remote peers allowed, no authentication"
                                                     : "loopback peers only");
    }

    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](beast::error_code, int) { stop_server(); });
    spdlog::info("Auth/lobby listening on {}:{}; native local login {}", options.listen_address,
                 listener->port(), options.native_client ? "enabled" : "disabled");
    world_metrics::register_io_thread();
    try {
      io_context.run();
    } catch (const std::exception &error) {
      spdlog::critical("Server I/O stopped unexpectedly: {}; draining world owners", error.what());
    } catch (...) {
      spdlog::critical("Server I/O stopped unexpectedly; draining world owners");
    }
    if (!shutdown_completed->load()) {
      // A stopped context can contain accepted, unrun recovery callbacks. Restart it and
      // retain a work guard until the listener explicitly confirms all owners have drained.
      auto drain_guard = asio::make_work_guard(io_context);
      stop_server();
      while (!shutdown_completed->load()) {
        io_context.restart();
        try {
          io_context.run();
        } catch (const std::exception &error) {
          spdlog::critical("Server I/O failed during world drain: {}", error.what());
        } catch (...) {
          spdlog::critical("Server I/O failed during world drain");
        }
      }
      drain_guard.reset();
    }
    // Drain cleanup while world-session workers can still run the queued strand tasks.
    housing_events.join();
    workers.join();
    auth_workers.join();
    agent_engine_workers.join();
    return quest_effect_timer->exit_code();
  } catch (const std::invalid_argument &error) {
    spdlog::error("Configuration error: {}", error.what());
    return 1;
  } catch (const std::exception &) {
    spdlog::error("Server startup/runtime failure (check database availability and schema)");
    return 1;
  }
}
