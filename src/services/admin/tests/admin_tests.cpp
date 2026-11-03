#include "services/admin/admin_api.hpp"
#include "services/admin/admin_log_buffer.hpp"
#include "common/clock_ticks.hpp"
#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "testing/memory_repositories.hpp"
#include "world/core/game_rates.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "world/session/native_world.hpp"
#include "world/session/tests/native_world_test_support.hpp"
#include "world/session/tests/offline_quarantine_test_server.hpp"
#include "world/features/trade/tests/pausing_region_checkpoint_store.hpp"
#include "world/features/company/company.hpp"
#include "protocol/protocol.hpp"
#include "world/features/chat/server_messages.hpp"
#include "data/shop_catalog.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr char kAdminToken[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr char kPassword[] = "correct-horse-battery";

// ---------------------------------------------------------------------------------------------
// Game rates

TEST(GameRates, DefaultRateKeepsCatalogAmounts) {
  GameRates rates;

  EXPECT_EQ(rates.effective_experience_percent(0), kDefaultRatePercent);
  EXPECT_EQ(rates.scale_experience(1234, 0), 1234);
  EXPECT_EQ(rates.scale_experience(0, 0), 0);
}

TEST(GameRates, BaseRateAndActiveEventsAdd) {
  GameRates rates;
  rates.set_base_experience_percent(200);

  ServerEvent weekend;
  weekend.name = "Weekend bonus";
  weekend.experience_bonus_percent = 100;
  weekend.starts_at_unix = 1000;
  weekend.ends_at_unix = 2000;
  rates.add_event(weekend);

  EXPECT_EQ(rates.scale_experience(100, 999), 200);
  EXPECT_EQ(rates.scale_experience(100, 1000), 300);
  EXPECT_EQ(rates.scale_experience(100, 1999), 300);
  // The end time is exclusive.
  EXPECT_EQ(rates.scale_experience(100, 2000), 200);
}

TEST(GameRates, DisabledEventsAndCapAreRespected) {
  GameRates rates;
  rates.set_base_experience_percent(900);

  ServerEvent disabled;
  disabled.name = "Off";
  disabled.experience_bonus_percent = 500;
  disabled.enabled = false;
  rates.add_event(disabled);
  EXPECT_EQ(rates.effective_experience_percent(10), 900);

  ServerEvent large;
  large.name = "Large";
  large.experience_bonus_percent = 500;
  rates.add_event(large);
  EXPECT_EQ(rates.effective_experience_percent(10), kMaximumRatePercent);
}

TEST(GameRates, ScalingSaturatesInsteadOfOverflowing) {
  GameRates rates;
  rates.set_base_experience_percent(kMaximumRatePercent);

  const auto largest = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(rates.scale_experience(largest, 0), largest);
}

TEST(GameRates, RejectsInvalidValues) {
  GameRates rates;

  EXPECT_THROW(rates.set_base_experience_percent(-1), std::invalid_argument);
  EXPECT_THROW(rates.set_base_experience_percent(kMaximumRatePercent + 1), std::invalid_argument);

  ServerEvent unnamed;
  EXPECT_THROW(rates.add_event(unnamed), std::invalid_argument);

  ServerEvent backwards;
  backwards.name = "Backwards";
  backwards.starts_at_unix = 50;
  backwards.ends_at_unix = 10;
  EXPECT_THROW(rates.add_event(backwards), std::invalid_argument);
}

TEST(GameRates, JsonRoundTripKeepsEventIds) {
  GameRates original;
  original.set_base_experience_percent(150);
  ServerEvent event;
  event.name = "Launch";
  event.experience_bonus_percent = 50;
  const auto stored = original.add_event(event);

  GameRates restored;
  restored.load_json(original.to_json());
  const auto snapshot = restored.snapshot();
  ASSERT_EQ(snapshot.events.size(), 1U);
  EXPECT_EQ(snapshot.base_experience_percent, 150);
  EXPECT_EQ(snapshot.events.front().id, stored.id);

  // New events continue after the largest restored ID.
  ServerEvent another;
  another.name = "Another";
  EXPECT_GT(restored.add_event(another).id, stored.id);
}

TEST(GameRates, InvalidDocumentLeavesValuesUnchanged) {
  GameRates rates;
  rates.set_base_experience_percent(300);

  EXPECT_THROW(rates.load_json(Json{{"experience_rate_percent", "300"}}), std::invalid_argument);
  EXPECT_EQ(rates.snapshot().base_experience_percent, 300);
}

// ---------------------------------------------------------------------------------------------
// Log buffer

std::shared_ptr<spdlog::logger> buffered_logger(const std::shared_ptr<AdminLogBuffer> &buffer) {
  auto logger = std::make_shared<spdlog::logger>("admin-test", buffer);
  logger->set_level(spdlog::level::trace);
  return logger;
}

TEST(AdminLogBuffer, ReturnsRecordsAfterTheCursor) {
  auto buffer = std::make_shared<AdminLogBuffer>(10);
  auto logger = buffered_logger(buffer);
  logger->info("first");
  logger->warn("second");

  const auto all = buffer->read_after(0, 100);
  ASSERT_EQ(all.entries.size(), 2U);
  EXPECT_EQ(all.entries[0].message, "first");
  EXPECT_EQ(all.entries[1].level, "warning");
  EXPECT_EQ(all.latest_sequence, 2U);
  EXPECT_FALSE(all.gap);

  const auto newer = buffer->read_after(1, 100);
  ASSERT_EQ(newer.entries.size(), 1U);
  EXPECT_EQ(newer.entries[0].message, "second");
}

TEST(AdminLogBuffer, ReportsEvictionGapsAndStaleCursors) {
  auto buffer = std::make_shared<AdminLogBuffer>(2);
  auto logger = buffered_logger(buffer);
  logger->info("one");
  logger->info("two");
  logger->info("three");

  const auto evicted = buffer->read_after(0, 100);
  EXPECT_TRUE(evicted.gap);
  ASSERT_EQ(evicted.entries.size(), 2U);
  EXPECT_EQ(evicted.entries[0].message, "two");

  // A cursor from an earlier process restarts at the oldest retained record.
  const auto stale = buffer->read_after(99, 100);
  EXPECT_TRUE(stale.gap);
  EXPECT_EQ(stale.entries.size(), 2U);
}

TEST(AdminLogBuffer, LevelFilterStillAdvancesTheCursor) {
  auto buffer = std::make_shared<AdminLogBuffer>(10);
  auto logger = buffered_logger(buffer);
  logger->info("quiet");
  logger->info("quiet again");

  const auto page = buffer->read_after(0, 100, spdlog::level::err);
  EXPECT_TRUE(page.entries.empty());
  EXPECT_EQ(page.next_after_sequence, 2U);
}

// ---------------------------------------------------------------------------------------------
// Admin API

class AdminApiTest : public ::testing::Test {
protected:
  void SetUp() override {
    settings_file_ = std::filesystem::temp_directory_path() /
                     ("chronicles-admin-test-" + random_hex(8) + ".json");
    mail_ = std::make_shared<MemoryMailRepository>(repositories_.database);
    auth_ = std::make_unique<AuthService>(repositories_.character_repositories());
    api_ = make_api(*auth_, rates_);
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove(settings_file_, ignored);
  }

  std::unique_ptr<AdminApi> make_api(AuthService &auth, GameRates &rates,
                                     std::shared_ptr<GuildService> guilds = {},
                                     std::shared_ptr<ServerMessages> server_messages = {},
                                     std::shared_ptr<const ShopCatalog> mail_catalog = {}) {
    AdminServices services{auth,
                           repositories_.character_repositories(),
                           mail_,
                           rates,
                           nullptr,
                           nullptr,
                           nullptr,
                           [this] { ++shutdown_requests_; },
                           settings_file_};
    services.info.storage_backend = "memory";
    services.guilds = std::move(guilds);
    services.server_messages = std::move(server_messages);
    services.mail_catalog = std::move(mail_catalog);
    return std::make_unique<AdminApi>(std::move(services), kAdminToken, false);
  }

  AdminHttpResponse call(std::string_view method, std::string_view target,
                         const Json &body = Json::object(), bool loopback = true) {
    const auto text = body.dump();
    const std::string authorization = std::string("Bearer ") + kAdminToken;
    return api_->handle({method, target, authorization, text, loopback});
  }

  std::int64_t create_account(const std::string &name) {
    const auto response =
        call("POST", "/admin/v1/accounts", {{"account", name}, {"password", kPassword}});
    EXPECT_EQ(response.status, 201U) << response.body.dump();
    return response.body.at("id").get<std::int64_t>();
  }

  Character create_character(std::int64_t account_id, const std::string &name, int slot = 0) {
    Character character;
    character.account_id = account_id;
    character.world_id = 1;
    character.slot = slot;
    character.class_id = 2;
    character.name = name;
    return repositories_.characters->create(character);
  }

  std::filesystem::path settings_file_;
  MemoryRepositories repositories_;
  std::shared_ptr<MemoryMailRepository> mail_;
  std::unique_ptr<AuthService> auth_;
  GameRates rates_;
  std::unique_ptr<AdminApi> api_;
  int shutdown_requests_{};
};

TEST_F(AdminApiTest, MailDeliveryIsAuthorizedIdempotentAndChecksItsCatalogAndRecipient) {
  ShopCatalogTables tables;
  tables.items = {{{"id", "701"}, {"category", "CraftMaterial"}, {"grade", "Normal"}}};
  tables.game_variables = {{{"name", "ItemMaxStack"}, {"value", "999"}},
                           {{"name", "ResetBaseDateTime"}, {"value", "2025-01-01 00:00:00"}}};
  tables.money = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  api_ = make_api(*auth_, rates_, {}, {}, std::make_shared<const ShopCatalog>(tables));
  const auto owner = create_character(create_account("mail_admin"), "MailAdmin");
  const auto stranger = create_character(create_account("mail_other"), "MailOther");
  Json delivery{{"account_id", owner.account_id},
                {"character_id", owner.id},
                {"world_id", 1},
                {"delivery_key", "test-reward"},
                {"title", "Reward"},
                {"text", "Claim your items."},
                {"retention_seconds", 3600},
                {"attachments", {{{"kind", "item"}, {"cms_id", 701}, {"count", 3}}}}};
  const auto delivered = call("POST", "/admin/v1/mail", delivery);
  ASSERT_EQ(delivered.status, 201U) << delivered.body.dump();
  const auto retry = call("POST", "/admin/v1/mail", delivery);
  ASSERT_EQ(retry.status, 201U) << retry.body.dump();
  EXPECT_EQ(retry.body.at("mail_id"), delivered.body.at("mail_id"));
  EXPECT_EQ(mail_->mailbox(owner.account_id, owner.id, 1, unix_epoch_ticks_now()).size(), 1U);

  delivery["delivery_key"] = "invalid-target";
  delivery["character_id"] = stranger.id;
  EXPECT_EQ(call("POST", "/admin/v1/mail", delivery).status, 409U);
  delivery["character_id"] = owner.id;
  delivery["attachments"][0]["cms_id"] = 123456789;
  EXPECT_EQ(call("POST", "/admin/v1/mail", delivery).status, 400U);
  EXPECT_EQ(api_->handle({"POST", "/admin/v1/mail", "", "{}", true}).status, 401U);
}

TEST_F(AdminApiTest, RejectsMissingTokensAndRemotePeers) {
  const auto anonymous = api_->handle({"GET", "/admin/v1/overview", "", "", true});
  EXPECT_EQ(anonymous.status, 401U);

  const auto wrong =
      api_->handle({"GET", "/admin/v1/overview", "Bearer " + std::string(64, 'f'), "", true});
  EXPECT_EQ(wrong.status, 401U);

  const auto remote = call("GET", "/admin/v1/overview", Json::object(), false);
  EXPECT_EQ(remote.status, 403U);

  EXPECT_EQ(call("GET", "/admin/v1/overview").status, 200U);
  EXPECT_EQ(call("GET", "/admin/v1/nothing-here").status, 404U);
}

std::shared_ptr<ServerMessages> admin_message_service() {
  return std::make_shared<ServerMessages>(
      ServerMessagesConfig{}, [](const auto &, const auto &) {},
      [](const auto &, const auto &, const auto &) {});
}

TEST_F(AdminApiTest, ServerMessageSettingsArePrivateValidatedAndPersisted) {
  auto messages = admin_message_service();
  api_ = make_api(*auth_, rates_, {}, messages);
  const Json desired{{"enabled", true},
                     {"sender_name", "[Realm]"},
                     {"interval_seconds", 120},
                     {"messages", {{{"type", "chat"}, {"text", "Configured recurring message"}}}},
                     {"welcome", {{"enabled", true}, {"messages", {"Configured welcome"}}}}};

  const auto anonymous = api_->handle({"GET", "/admin/v1/server-messages", "", "", true});
  EXPECT_EQ(anonymous.status, 401U);
  const auto response = call("POST", "/admin/v1/server-messages", desired);
  ASSERT_EQ(response.status, 200U) << response.body.dump();
  EXPECT_TRUE(response.body.at("settings_saved").get<bool>());
  EXPECT_EQ(call("GET", "/admin/v1/server-messages").body, desired);

  AuthService restarted(repositories_.character_repositories());
  GameRates restarted_rates;
  auto restored_messages = admin_message_service();
  auto restarted_api = make_api(restarted, restarted_rates, {}, restored_messages);
  restarted_api->load_saved_settings();
  EXPECT_EQ(restored_messages->configuration(), desired);
}

TEST_F(AdminApiTest, InvalidServerMessageUpdateChangesNeitherMemoryNorSavedSettings) {
  auto messages = admin_message_service();
  api_ = make_api(*auth_, rates_, {}, messages);
  const auto before = messages->configuration();
  ASSERT_EQ(call("POST", "/admin/v1/server-messages", before).status, 200U);
  std::ifstream saved_before(settings_file_, std::ios::binary);
  const std::string before_bytes((std::istreambuf_iterator<char>(saved_before)), {});
  saved_before.close();

  auto invalid = before;
  invalid["enabled"] = true;
  invalid["interval_seconds"] = 0;
  EXPECT_EQ(call("POST", "/admin/v1/server-messages", invalid).status, 400U);
  EXPECT_EQ(messages->configuration(), before);
  std::ifstream saved_after(settings_file_, std::ios::binary);
  const std::string after_bytes((std::istreambuf_iterator<char>(saved_after)), {});
  EXPECT_EQ(after_bytes, before_bytes);
}

TEST_F(AdminApiTest, InvalidSavedMessageSettingsDoNotPartiallyApplyRealmOrRates) {
  auto messages = admin_message_service();
  api_ = make_api(*auth_, rates_, {}, messages);
  const auto realm_before = auth_->realm_settings();
  const auto messages_before = messages->configuration();
  auto invalid = messages_before;
  invalid["enabled"] = true;
  {
    std::ofstream saved(settings_file_);
    saved << Json{{"version", 1},
                  {"realm", {{"world_name", "Must not apply"}}},
                  {"rates", {{"experience_rate_percent", 200}}},
                  {"server_messages", invalid}}
                 .dump();
  }
  EXPECT_THROW(api_->load_saved_settings(), std::invalid_argument);
  EXPECT_EQ(auth_->realm_settings().world_name, realm_before.world_name);
  EXPECT_EQ(rates_.snapshot().base_experience_percent, kDefaultRatePercent);
  EXPECT_EQ(messages->configuration(), messages_before);
}

TEST_F(AdminApiTest, RealmChangesApplyAndPersist) {
  const auto response = call("POST", "/admin/v1/realm",
                             {{"world_name", "Test Realm"},
                              {"capacity", 25},
                              {"label", "recommended"},
                              {"maintenance", true}});
  ASSERT_EQ(response.status, 200U) << response.body.dump();
  EXPECT_TRUE(response.body.at("settings_saved").get<bool>());

  const auto status = auth_->world_status();
  EXPECT_EQ(status.name, "Test Realm");
  EXPECT_EQ(status.capacity, 25U);
  EXPECT_EQ(status.state, x5::pb::ServerState_GmOnly);
  EXPECT_EQ(auth_->public_worlds().worlds(0).label(), x5::pb::ServerLabel_Recommended);

  // A fresh process loads the saved realm settings.
  AuthService restarted(repositories_.character_repositories());
  GameRates restarted_rates;
  auto restarted_api = make_api(restarted, restarted_rates);
  restarted_api->load_saved_settings();
  EXPECT_EQ(restarted.realm_settings().world_name, "Test Realm");
  EXPECT_TRUE(restarted.realm_settings().maintenance);
}

TEST_F(AdminApiTest, RejectsInvalidRealmValuesWithoutChangingThem) {
  const auto before = auth_->realm_settings();

  EXPECT_EQ(call("POST", "/admin/v1/realm", {{"capacity", 0}}).status, 400U);
  EXPECT_EQ(call("POST", "/admin/v1/realm", {{"world_name", ""}}).status, 400U);
  EXPECT_EQ(call("POST", "/admin/v1/realm", {{"label", "shiny"}}).status, 400U);

  EXPECT_EQ(auth_->realm_settings().world_name, before.world_name);
  EXPECT_EQ(auth_->realm_settings().capacity, before.capacity);
}

class BanCommitFailureStore : public MemoryAccountRepository {
public:
  explicit BanCommitFailureStore(std::shared_ptr<MemoryDatabase> database)
      : MemoryAccountRepository(std::move(database)) {}

  bool fail_lookup{};
  bool fail_revocation{};
  bool fail_password_after_commit{};

  bool set_account_password(std::int64_t account_id, const std::string &salt,
                            const std::string &hash, int iterations) override {
    const bool updated =
        MemoryAccountRepository::set_account_password(account_id, salt, hash, iterations);
    if (updated && fail_password_after_commit) {
      throw std::runtime_error("synthetic lost password COMMIT response");
    }
    return updated;
  }

  std::optional<Account> account_by_id(std::int64_t account_id) override {
    if (fail_lookup) {
      throw std::runtime_error("synthetic account lookup failure");
    }
    return MemoryAccountRepository::account_by_id(account_id);
  }

  void revoke_reconnect_credentials(std::int64_t account_id) override {
    if (fail_revocation) {
      throw std::runtime_error("synthetic revocation failure");
    }
    MemoryAccountRepository::revoke_reconnect_credentials(account_id);
  }

  bool set_account_banned(std::int64_t account_id, bool banned) override {
    const bool updated = MemoryAccountRepository::set_account_banned(account_id, banned);
    if (updated && banned) {
      throw std::runtime_error("synthetic lost ban COMMIT response");
    }
    return updated;
  }
};

class RecordingWorldListener : public NativeWorldListener {
public:
  explicit RecordingWorldListener(std::int64_t account_id) : account_id_(account_id) {}

  unsigned short port() const override {
    return 0;
  }

  void stop() override {}

  std::vector<WorldConnectionSummary> connections() override {
    return {};
  }

  bool disconnect(std::uint64_t) override {
    return false;
  }

  void set_repositories(WorldRepositories) override {}

  std::size_t disconnect_account(std::int64_t account_id) override {
    if (account_id != account_id_ || !connected_) {
      return 0;
    }
    connected_ = false;
    return 1;
  }

  std::size_t broadcast_system_notice(const std::string &, SystemNoticeStyle) override {
    return 0;
  }

  bool connected() const {
    return connected_;
  }

private:
  std::int64_t account_id_;
  bool connected_{true};
};

TEST(AdminUncertainBan, RequestsWorldDisconnectAndReportsErrorAfterCommittedMutation) {
  MemoryRepositories repositories;
  auto store = std::make_shared<BanCommitFailureStore>(repositories.database);
  repositories.accounts = store;
  AuthService auth(repositories.character_repositories());
  GameRates rates;
  ASSERT_TRUE(auth.create_account("alice", kPassword));
  const auto login = auth.login("alice", kPassword, true);
  ASSERT_EQ(login.ec(), x5::ec::Success);
  auto world = std::make_shared<RecordingWorldListener>(login.account_id());
  AdminServices services{auth, repositories.character_repositories(),
                         std::make_shared<MemoryMailRepository>(repositories.database),
                         rates, nullptr, world, nullptr, [] {}, {}};
  AdminApi api(std::move(services), kAdminToken, false);
  const auto path = "/admin/v1/accounts/" + std::to_string(login.account_id()) + "/ban";
  const auto authorization = std::string("Bearer ") + kAdminToken;
  const auto response = api.handle({"POST", path, authorization, R"({"banned":true})", true});

  EXPECT_EQ(response.status, 500U);
  EXPECT_FALSE(world->connected());
  EXPECT_FALSE(auth.authenticated(login.access_token()));
  ASSERT_TRUE(store->account_by_id(login.account_id()));
  EXPECT_TRUE(store->account_by_id(login.account_id())->banned);
}

TEST(AdminPasswordReset, DisconnectsActiveWorldAfterSuccessOrUncertainCommit) {
  for (const bool uncertain : {false, true}) {
    SCOPED_TRACE(uncertain ? "uncertain commit" : "success");
    MemoryRepositories repositories;
    auto store = std::make_shared<BanCommitFailureStore>(repositories.database);
    repositories.accounts = store;
    AuthService auth(repositories.character_repositories());
    GameRates rates;
    ASSERT_TRUE(auth.create_account("alice", kPassword));
    const auto login = auth.login("alice", kPassword, true);
    auto world = std::make_shared<RecordingWorldListener>(login.account_id());
    AdminServices services{auth, repositories.character_repositories(),
                           std::make_shared<MemoryMailRepository>(repositories.database),
                           rates, nullptr, world, nullptr, [] {}, {}};
    AdminApi api(std::move(services), kAdminToken, false);
    const auto path = "/admin/v1/accounts/" + std::to_string(login.account_id()) + "/password";
    const auto authorization = std::string("Bearer ") + kAdminToken;
    const auto invalid = api.handle({"POST", path, authorization, R"({"password":"short"})", true});
    EXPECT_EQ(invalid.status, 400U);
    EXPECT_TRUE(world->connected());
    EXPECT_TRUE(auth.authenticated(login.access_token()));

    store->fail_password_after_commit = uncertain;
    const auto response = api.handle(
        {"POST", path, authorization, R"({"password":"new-alice-password"})", true});
    EXPECT_EQ(response.status, uncertain ? 500U : 200U);
    EXPECT_FALSE(world->connected());
    EXPECT_FALSE(auth.authenticated(login.access_token()));
    if (!uncertain) {
      EXPECT_EQ(response.body.at("disconnected"), 1);
    }
  }
}

TEST(AdminUncertainKick, DisconnectsAndQuarantinesWhenStorageRevocationFails) {
  MemoryRepositories repositories;
  auto store = std::make_shared<BanCommitFailureStore>(repositories.database);
  repositories.accounts = store;
  AuthService auth(repositories.character_repositories());
  GameRates rates;
  ASSERT_TRUE(auth.create_account("alice", kPassword));
  const auto login = auth.login("alice", kPassword, true);
  auto grant = auth.issue_launch_grant(login.access_token());
  ASSERT_TRUE(grant);
  ASSERT_TRUE(auth.consume_launch_grant(*grant));
  auto world = std::make_shared<RecordingWorldListener>(login.account_id());
  AdminServices services{auth, repositories.character_repositories(),
                         std::make_shared<MemoryMailRepository>(repositories.database),
                         rates, nullptr, world, nullptr, [] {}, {}};
  AdminApi api(std::move(services), kAdminToken, false);
  const auto path = "/admin/v1/accounts/" + std::to_string(login.account_id()) + "/kick";
  const auto authorization = std::string("Bearer ") + kAdminToken;
  store->fail_revocation = true;
  const auto response = api.handle({"POST", path, authorization, "{}", true});

  EXPECT_EQ(response.status, 500U);
  EXPECT_FALSE(world->connected());
  EXPECT_FALSE(auth.authenticated(login.access_token()));
  EXPECT_FALSE(auth.reconnect_launch_grant(*grant));
}

TEST(AdminUncertainKick, DisconnectsAndQuarantinesWhenInitialLookupFails) {
  MemoryRepositories repositories;
  auto store = std::make_shared<BanCommitFailureStore>(repositories.database);
  repositories.accounts = store;
  AuthService auth(repositories.character_repositories());
  GameRates rates;
  ASSERT_TRUE(auth.create_account("alice", kPassword));
  const auto login = auth.login("alice", kPassword, true);
  auto grant = auth.issue_launch_grant(login.access_token());
  ASSERT_TRUE(grant);
  ASSERT_TRUE(auth.consume_launch_grant(*grant));
  auto world = std::make_shared<RecordingWorldListener>(login.account_id());
  AdminServices services{auth, repositories.character_repositories(),
                         std::make_shared<MemoryMailRepository>(repositories.database),
                         rates, nullptr, world, nullptr, [] {}, {}};
  AdminApi api(std::move(services), kAdminToken, false);
  const auto path = "/admin/v1/accounts/" + std::to_string(login.account_id()) + "/kick";
  const auto authorization = std::string("Bearer ") + kAdminToken;
  store->fail_lookup = true;
  const auto response = api.handle({"POST", path, authorization, "{}", true});

  EXPECT_EQ(response.status, 500U);
  EXPECT_FALSE(world->connected());
  EXPECT_FALSE(auth.authenticated(login.access_token()));
  EXPECT_FALSE(auth.reconnect_launch_grant(*grant));
}

TEST_F(AdminApiTest, BanRefusesLoginAndRevokesSessions) {
  const auto account_id = create_account("banme");
  const auto login = auth_->login("banme", kPassword);
  ASSERT_EQ(login.ec(), x5::ec::Success);
  ASSERT_TRUE(auth_->authenticated(login.access_token()));

  const auto banned =
      call("POST", "/admin/v1/accounts/" + std::to_string(account_id) + "/ban", {{"banned", true}});
  ASSERT_EQ(banned.status, 200U) << banned.body.dump();
  EXPECT_FALSE(auth_->authenticated(login.access_token()));
  EXPECT_EQ(auth_->login("banme", kPassword).ec(), x5::ec::LoginDenyRestrictedAccount);

  call("POST", "/admin/v1/accounts/" + std::to_string(account_id) + "/ban", {{"banned", false}});
  EXPECT_EQ(auth_->login("banme", kPassword).ec(), x5::ec::Success);
}

TEST_F(AdminApiTest, PasswordResetReplacesTheOldPassword) {
  const auto account_id = create_account("resetme");
  const auto response =
      call("POST", "/admin/v1/accounts/" + std::to_string(account_id) + "/password",
           {{"password", "a-brand-new-password"}});
  ASSERT_EQ(response.status, 200U) << response.body.dump();

  EXPECT_NE(auth_->login("resetme", kPassword).ec(), x5::ec::Success);
  EXPECT_EQ(auth_->login("resetme", "a-brand-new-password").ec(), x5::ec::Success);

  const auto too_short =
      call("POST", "/admin/v1/accounts/" + std::to_string(account_id) + "/password",
           {{"password", "short"}});
  EXPECT_EQ(too_short.status, 400U);
}

TEST_F(AdminApiTest, ListsAccountsAndCharactersWithSearch) {
  const auto first = create_account("alpha");
  create_account("bravo");
  create_character(first, "Archer");

  const auto accounts = call("GET", "/admin/v1/accounts?search=alp");
  ASSERT_EQ(accounts.status, 200U);
  ASSERT_EQ(accounts.body.at("accounts").size(), 1U);
  EXPECT_EQ(accounts.body.at("accounts")[0].at("character_count"), 1);

  const auto characters = call("GET", "/admin/v1/characters?search=arch");
  ASSERT_EQ(characters.status, 200U);
  ASSERT_EQ(characters.body.at("characters").size(), 1U);
  EXPECT_EQ(characters.body.at("characters")[0].at("account_name"), "alpha");
  EXPECT_EQ(characters.body.at("characters")[0].at("class_name"), "wild");

  const auto details = call("GET", "/admin/v1/accounts/" + std::to_string(first));
  ASSERT_EQ(details.status, 200U);
  EXPECT_EQ(details.body.at("characters").size(), 1U);
}

TEST_F(AdminApiTest, EditsOfflineCharacters) {
  const auto account_id = create_account("editor");
  const auto character = create_character(account_id, "Editme");
  const auto base = "/admin/v1/characters/" + std::to_string(character.id);

  ASSERT_EQ(call("POST", base + "/progress", {{"level", 7}, {"experience", 42}}).status, 200U);
  ASSERT_EQ(call("POST", base + "/currency", {{"gold", 5000}}).status, 200U);
  ASSERT_EQ(call("POST", base + "/position",
                 {{"map_id", 8000}, {"x_mm", 1}, {"y_mm", 2}, {"z_mm", 3}, {"yaw", 90.0}})
                .status,
            200U);

  const auto details = call("GET", base);
  ASSERT_EQ(details.status, 200U) << details.body.dump();
  EXPECT_EQ(details.body.at("level"), 7);
  EXPECT_EQ(details.body.at("experience"), 42);
  EXPECT_EQ(details.body.at("progress").at("gold"), 5000);
  EXPECT_EQ(details.body.at("saved_position").at("map_id"), 8000);
  EXPECT_FALSE(details.body.at("items").empty());

  EXPECT_EQ(call("POST", base + "/currency", {{"gold", -1}}).status, 400U);
  EXPECT_EQ(call("POST", base + "/progress", {{"level", 0}}).status, 400U);
}

TEST_F(AdminApiTest, CharacterMutationsFailClosedWhenWorldCannotReserve) {
  const auto account_id = create_account("reserved_editor");
  const auto character = create_character(account_id, "Reservededit");
  const auto base = "/admin/v1/characters/" + std::to_string(character.id);
  auto world = std::make_shared<RecordingWorldListener>(account_id);
  AdminServices services{*auth_, repositories_.character_repositories(), mail_, rates_, nullptr,
                         world, nullptr, [this] { ++shutdown_requests_; }, settings_file_};
  api_ = std::make_unique<AdminApi>(std::move(services), kAdminToken, false);

  EXPECT_EQ(call("POST", base + "/progress", {{"level", 7}, {"experience", 42}}).status, 409U);
  EXPECT_EQ(call("POST", base + "/currency", {{"gold", 5000}}).status, 409U);
  EXPECT_EQ(call("POST", base + "/position",
                 {{"map_id", 8000}, {"x_mm", 1}, {"y_mm", 2}, {"z_mm", 3}}).status,
            409U);
  EXPECT_EQ(call("POST", base + "/company", {{"company_type", 0}}).status, 409U);
  EXPECT_EQ(call("POST", base + "/rename", {{"name", "Othername"}}).status, 409U);
  EXPECT_EQ(call("POST", base + "/delete", {{"confirm_name", character.name}}).status, 409U);

  const auto after = repositories_.characters->character_by_id(character.id);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->name, character.name);
  EXPECT_EQ(after->level, character.level);
  EXPECT_EQ(after->experience, character.experience);
  const auto progress = repositories_.character_state->load_world_progress(
      account_id, character.id, character.world_id);
  EXPECT_EQ(progress.gold, 0);
}

TEST(AdminCharacterValidation, InvalidEditsDoNotFenceWorldEntry) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("validation_player", kPassword));
  const auto login = auth.login("validation_player", kPassword);
  ASSERT_EQ(login.ec(), x5::ec::Success);

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);
  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("Validationplayer");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  const auto character_id = created.char_().char_id();

  native_testing::TemporaryWorldCertificate certificate;
  native_testing::TwoClientWorldServer server(auth, certificate);
  auto world = std::shared_ptr<NativeWorldListener>(&server.listener(), [](NativeWorldListener *) {});
  GameRates rates;
  auto mail = std::make_shared<MemoryMailRepository>(repositories.database);
  auto companies = std::make_shared<CompanyService>(std::make_shared<MemoryCompanyStore>(),
                                                     CompanyRules{}, 1);
  AdminServices services{auth, repositories.character_repositories(), mail, rates, nullptr,
                         world, nullptr, {}, {}};
  services.companies = companies;
  AdminApi api(std::move(services), kAdminToken, false);
  const std::string authorization = std::string("Bearer ") + kAdminToken;
  const auto base = "/admin/v1/characters/" + std::to_string(character_id);
  const auto invalid_edit = [&](std::string_view action, const Json &body) {
    const auto path = base + std::string(action);
    const auto text = body.dump();
    EXPECT_EQ(api.handle({"POST", path, authorization, text, true}).status, 400U);
    auto reservation = server.listener().reserve_offline_character(character_id);
    EXPECT_NE(reservation, nullptr);
  };

  invalid_edit("/progress", {{"level", 0}});
  invalid_edit("/position", {{"map_id", 0}, {"x_mm", 1}, {"y_mm", 2}, {"z_mm", 3}});
  invalid_edit("/company", {{"company_type", 999}});
  invalid_edit("/company", {{"contribution", -1}});

  x5::pb::ws::front::CharSelectReq selection;
  selection.set_char_id(character_id);
  const auto selected = auth.select(login.access_token(), selection);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  native_testing::WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);
}

class LostProgressAcknowledgementStore final : public CharacterRepository {
public:
  explicit LostProgressAcknowledgementStore(std::shared_ptr<CharacterRepository> inner)
      : inner_(std::move(inner)) {
  }

  std::vector<Character> characters(std::int64_t account_id, int world_id) override {
    return inner_->characters(account_id, world_id);
  }
  Character create(Character character) override {
    return inner_->create(std::move(character));
  }
  bool save_world_position(std::int64_t account_id, std::int64_t character_id, int world_id,
                           const WorldPosition &position) override {
    return inner_->save_world_position(account_id, character_id, world_id, position);
  }
  bool save_progress(std::int64_t account_id, std::int64_t character_id, int world_id,
                     int level, std::int64_t experience) override {
    if (!inner_->save_progress(account_id, character_id, world_id, level, experience)) {
      return false;
    }
    throw std::runtime_error("Synthetic lost progress commit acknowledgement");
  }
  std::vector<Character> list_characters() override {
    return inner_->list_characters();
  }
  std::optional<Character> character_by_id(std::int64_t character_id) override {
    return inner_->character_by_id(character_id);
  }
  CharacterRenameResult rename_character(std::int64_t character_id,
                                         const std::string &name) override {
    return inner_->rename_character(character_id, name);
  }
  bool delete_character(std::int64_t character_id) override {
    return inner_->delete_character(character_id);
  }
  std::optional<CharacterSummary> character_summary(std::int64_t character_id) override {
    return inner_->character_summary(character_id);
  }
  std::optional<CharacterSummary> character_summary_by_name(int world_id,
                                                             const std::string &name) override {
    return inner_->character_summary_by_name(world_id, name);
  }
  std::vector<CharacterSummary> account_character_summaries(std::int64_t account_id,
                                                             int world_id) override {
    return inner_->account_character_summaries(account_id, world_id);
  }

private:
  std::shared_ptr<CharacterRepository> inner_;
};

TEST(AdminCharacterQuarantine, LostMarkerAcknowledgementDoesNotStartTheMutation) {
  MemoryRepositories memory;
  auto repositories = memory.character_repositories();
  auto fault = std::make_shared<native_testing::PausingRegionCheckpointStore>(
      repositories.character_state);
  repositories.character_state = fault;
  AuthService auth(repositories);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("marker_fault", kPassword));
  const auto login = auth.login("marker_fault", kPassword);
  ASSERT_EQ(login.ec(), x5::ec::Success);
  Character character;
  character.account_id = login.account_id();
  character.name = "Markerfault";
  character = repositories.characters->create(character);

  native_testing::TemporaryWorldCertificate certificate;
  native_testing::OfflineQuarantineTestServer server(auth, certificate);
  auto world = std::shared_ptr<NativeWorldListener>(&server.listener(), [](NativeWorldListener *) {});
  GameRates rates;
  AdminServices services{auth, repositories, nullptr, rates, nullptr, world, nullptr, {}, {}};
  AdminApi api(std::move(services), kAdminToken, false);
  const std::string authorization = std::string("Bearer ") + kAdminToken;
  const auto path = "/admin/v1/characters/" + std::to_string(character.id) + "/progress";

  fault->lose_next_admin_quarantine_ack();
  EXPECT_EQ(api.handle({"POST", path, authorization, R"({"level":7})", true}).status, 500U);
  const auto after = repositories.characters->character_by_id(character.id);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->level, character.level);
  EXPECT_EQ(server.listener().reserve_offline_character(character.id), nullptr);
  EXPECT_EQ(repositories.character_state->offline_admin_quarantines(),
            std::vector<std::int64_t>{character.id});
  EXPECT_TRUE(server.shutdown_remains_blocked());
}

TEST(AdminCharacterQuarantine, StartupRefusesToListenWhenMarkerReadFails) {
  MemoryRepositories memory;
  auto repositories = memory.character_repositories();
  auto fault = std::make_shared<native_testing::PausingRegionCheckpointStore>(
      repositories.character_state);
  repositories.character_state = fault;
  AuthService auth(repositories);
  auth.acquire_monster_reward_owner_epoch();
  native_testing::TemporaryWorldCertificate certificate;
  fault->fail_admin_quarantine_load();
  EXPECT_THROW((native_testing::TwoClientWorldServer(auth, certificate)), std::runtime_error);
}

// The harness executes each stage in a separate process against a disposable loopback cluster.
TEST(AdminCharacterQuarantine, PostgresLostAcknowledgementSurvivesProcessRestart) {
  const char *url = std::getenv("CHRONICLES_ADMIN_ISOLATED_TEST_DATABASE_URL");
  const char *stage_value = std::getenv("CHRONICLES_ADMIN_QUARANTINE_TEST_STAGE");
  if (!url || !stage_value) {
    GTEST_SKIP() << "Requires the isolated admin quarantine PostgreSQL harness";
  }
  pqxx::connection verification(url);
  pqxx::read_transaction read(verification);
  const auto identity = read.exec1("SELECT current_database(), host(inet_server_addr())");
  ASSERT_EQ(identity[0].as<std::string>(), "codex_admin_isolated");
  ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  read.commit();
  verification.close();

  const std::string stage(stage_value);
  ASSERT_TRUE(stage == "write" || stage == "blocked" || stage == "recovered");
  auto repositories = make_postgres_character_repositories(url);
  if (stage == "write") {
    repositories.characters = std::make_shared<LostProgressAcknowledgementStore>(
        repositories.characters);
  }
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories, config);
  auth.acquire_monster_reward_owner_epoch();
  if (stage == "write") {
    ASSERT_TRUE(auth.create_account("quarantine_player", kPassword));
  }
  const auto login = auth.login("quarantine_player", kPassword);
  ASSERT_EQ(login.ec(), x5::ec::Success);
  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);
  if (stage == "write") {
    x5::pb::ws::front::CharCreateReq create_request;
    create_request.set_slot(0);
    create_request.set_char_name("Quarantinehero");
    create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
    ASSERT_EQ(auth.create(login.access_token(), create_request).ec(), x5::ec::Success);
  }
  const auto characters = repositories.characters->characters(login.account_id(), 1);
  ASSERT_EQ(characters.size(), 1U);
  const auto character_id = characters.front().id;

  native_testing::TemporaryWorldCertificate certificate;
  native_testing::TwoClientWorldServer server(auth, certificate);
  auto world = std::shared_ptr<NativeWorldListener>(&server.listener(), [](NativeWorldListener *) {});
  if (stage == "write") {
    GameRates rates;
    AdminServices services{auth, repositories, nullptr, rates, nullptr, world, nullptr, {}, {}};
    AdminApi api(std::move(services), kAdminToken, false);
    const std::string authorization = std::string("Bearer ") + kAdminToken;
    const auto path = "/admin/v1/characters/" + std::to_string(character_id) + "/progress";
    ASSERT_EQ(api.handle({"POST", path, authorization, R"({"level":7})", true}).status, 500U);
    ASSERT_EQ(repositories.characters->character_by_id(character_id)->level, 7);
    ASSERT_EQ(server.listener().reserve_offline_character(character_id), nullptr);
    ASSERT_EQ(repositories.character_state->offline_admin_quarantines().size(), 1U);
    std::cout << "QUARANTINE_CRASH_STAGE_READY" << std::endl;
    // Abrupt termination exercises restart without world listener or reservation destructors.
    std::_Exit(0);
  }

  x5::pb::ws::front::CharSelectReq selection;
  selection.set_char_id(character_id);
  const auto selected = auth.select(login.access_token(), selection);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  native_testing::WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  if (stage == "blocked") {
    EXPECT_EQ(server.listener().reserve_offline_character(character_id), nullptr);
    EXPECT_THROW(client.read(), boost::system::system_error);
  } else {
    EXPECT_EQ(client.read().opcode(), 5);
  }
}

TEST_F(AdminApiTest, RenameAndDeleteAreGuarded) {
  const auto account_id = create_account("owner");
  const auto first = create_character(account_id, "Firstone", 0);
  create_character(account_id, "Secondone", 1);
  const auto base = "/admin/v1/characters/" + std::to_string(first.id);

  EXPECT_EQ(call("POST", base + "/rename", {{"name", "secondONE"}}).status, 409U);
  EXPECT_EQ(call("POST", base + "/rename", {{"name", "no spaces"}}).status, 400U);
  ASSERT_EQ(call("POST", base + "/rename", {{"name", "Renamed"}}).status, 200U);

  EXPECT_EQ(call("POST", base + "/delete", {{"confirm_name", "Firstone"}}).status, 400U);
  ASSERT_EQ(call("POST", base + "/delete", {{"confirm_name", "Renamed"}}).status, 200U);
  EXPECT_EQ(call("GET", base).status, 404U);

  // Memory IDs are never reused after a deletion.
  const auto replacement = create_character(account_id, "Thirdone", 0);
  EXPECT_GT(replacement.id, first.id + 1);
}

TEST_F(AdminApiTest, DeletingAGuildMemberCannotLeaveTheLiveRegistryStale) {
  const auto account_id = create_account("guild_owner");
  const auto character = create_character(account_id, "GuildOwner", 0);

  // This test covers deletion protection, not company prerequisites, so the realm runs
  // without the company requirement that guild founding enforces by default.
  GuildRules rules;
  rules.companies_enabled = false;
  auto guilds = std::make_shared<GuildService>(std::make_shared<MemoryGuildStore>(), rules, 1);
  GuildActor owner;
  owner.world_id = 1;
  owner.char_id = character.id;
  owner.name = character.name;
  owner.level = character.level;
  owner.char_class = character.class_id;
  GuildCreateRequest request;
  request.name = "Owners";
  ASSERT_EQ(guilds->create(owner, request).error, x5::ec::Success);
  api_ = make_api(*auth_, rates_, guilds);
  const auto base = "/admin/v1/characters/" + std::to_string(character.id);
  const auto refused = call("POST", base + "/delete", {{"confirm_name", character.name}});
  EXPECT_EQ(refused.status, 409U);
  EXPECT_EQ(refused.body.at("error"), "guild_membership_exists");
  EXPECT_EQ(call("GET", base).status, 200U);
  EXPECT_GT(guilds->status(character.id).guild_id, 0);
}

TEST_F(AdminApiTest, RatesAndEventsAreEditableAndSaved) {
  ASSERT_EQ(call("POST", "/admin/v1/settings", {{"experience_rate_percent", 200}}).status, 200U);
  EXPECT_EQ(rates_.scale_experience(10, current_unix_seconds()), 20);

  const auto created =
      call("POST", "/admin/v1/events", {{"name", "Double up"}, {"experience_bonus_percent", 100}});
  ASSERT_EQ(created.status, 201U) << created.body.dump();
  EXPECT_EQ(created.body.at("effective_experience_percent"), 300);
  const auto event_id = created.body.at("event").at("id").get<std::int64_t>();

  const auto disabled =
      call("POST", "/admin/v1/events/" + std::to_string(event_id),
           {{"name", "Double up"}, {"experience_bonus_percent", 100}, {"enabled", false}});
  ASSERT_EQ(disabled.status, 200U);
  EXPECT_EQ(disabled.body.at("effective_experience_percent"), 200);

  ASSERT_EQ(call("POST", "/admin/v1/events/" + std::to_string(event_id) + "/delete").status, 200U);
  EXPECT_TRUE(rates_.snapshot().events.empty());

  std::ifstream saved(settings_file_);
  const auto document = Json::parse(saved);
  EXPECT_EQ(document.at("rates").at("experience_rate_percent"), 200);
}

TEST_F(AdminApiTest, ConcurrentEventWritesPersistEveryAcceptedEvent) {
  std::atomic<int> accepted{};
  std::vector<std::thread> workers;
  for (int index = 0; index < 8; ++index) {
    workers.emplace_back([&, index] {
      const auto result =
          call("POST", "/admin/v1/events",
               {{"name", "Event " + std::to_string(index)}, {"experience_bonus_percent", 1}});
      if (result.status == 201U && result.body.value("settings_saved", false)) {
        ++accepted;
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  ASSERT_EQ(accepted.load(), 8);
  std::ifstream saved(settings_file_);
  const auto document = Json::parse(saved);
  EXPECT_EQ(document.at("rates").at("events").size(), 8U);
  EXPECT_EQ(rates_.snapshot().events.size(), 8U);
}

TEST_F(AdminApiTest, OptionalServicesReportUnavailable) {
  EXPECT_EQ(call("GET", "/admin/v1/logs").status, 503U);
  EXPECT_EQ(call("GET", "/admin/v1/chat").status, 503U);
  EXPECT_EQ(call("POST", "/admin/v1/announce", {{"text", "hello"}}).status, 503U);
  EXPECT_EQ(call("POST", "/admin/v1/players/1/kick").status, 503U);

  const auto players = call("GET", "/admin/v1/players");
  ASSERT_EQ(players.status, 200U);
  EXPECT_FALSE(players.body.at("world_listener").get<bool>());
}

TEST_F(AdminApiTest, ShutdownNeedsExplicitConfirmation) {
  EXPECT_EQ(call("POST", "/admin/v1/server/shutdown", {{"confirm", "yes"}}).status, 400U);
  EXPECT_EQ(shutdown_requests_, 0);

  EXPECT_EQ(call("POST", "/admin/v1/server/shutdown", {{"confirm", "shutdown"}}).status, 202U);
  EXPECT_EQ(shutdown_requests_, 1);
}

// ---------------------------------------------------------------------------------------------
// World chat history for the console

TEST(WorldChatHistory, RecordsOperatorNoticesInOrder) {
  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories());
  NativeChatService chat(auth);

  // Nobody is connected, but the console still sees what the operator sent.
  EXPECT_EQ(chat.broadcast_notice(1, "[Server]", "first"), 0U);
  chat.broadcast_notice(1, "[Server]", "second");

  const auto page = chat.world_chat_history(0, 100);
  ASSERT_EQ(page.records.size(), 2U);
  EXPECT_EQ(page.records[0].message, "first");
  EXPECT_EQ(page.records[1].sender_name, "[Server]");
  EXPECT_TRUE(page.records[1].operator_notice);
  EXPECT_EQ(page.latest_sequence, 2U);

  const auto newer = chat.world_chat_history(1, 100);
  ASSERT_EQ(newer.records.size(), 1U);
  EXPECT_EQ(newer.records[0].message, "second");
}

TEST(WorldChatHistory, KeepsABoundedWindowAndReportsGaps) {
  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories());
  NativeChatService chat(auth);

  for (std::size_t index = 0; index < kWorldChatHistoryCapacity + 5; ++index) {
    chat.broadcast_notice(1, "[Server]", "message " + std::to_string(index));
  }

  const auto oldest = chat.world_chat_history(0, kMaximumWorldChatHistoryPage);
  EXPECT_TRUE(oldest.gap);
  EXPECT_EQ(oldest.records.front().message, "message 5");
  EXPECT_EQ(oldest.records.size(), kMaximumWorldChatHistoryPage);

  // A cursor from an earlier server process restarts from the oldest retained message.
  const auto stale = chat.world_chat_history(999999, 10);
  EXPECT_TRUE(stale.gap);
  EXPECT_EQ(stale.records.front().message, "message 5");
}

TEST(WorldChatHistory, AdminRouteReturnsThisRealmsMessages) {
  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories());
  NativeChatService chat(auth);
  GameRates rates;
  AdminServices services{auth, repositories.character_repositories(),
                         std::make_shared<MemoryMailRepository>(repositories.database),
                         rates, nullptr, nullptr, &chat, {}, {}};
  AdminApi api(std::move(services), kAdminToken, false);
  const std::string authorization = std::string("Bearer ") + kAdminToken;

  chat.broadcast_notice(1, "[Server]", "hello realm");
  chat.broadcast_notice(2, "[Server]", "another realm");

  const auto response = api.handle({"GET", "/admin/v1/chat?after=0", authorization, "", true});
  ASSERT_EQ(response.status, 200U) << response.body.dump();
  ASSERT_EQ(response.body.at("messages").size(), 1U);
  EXPECT_EQ(response.body.at("messages")[0].at("message"), "hello realm");
  EXPECT_EQ(response.body.at("next_after"), 2);

  const auto empty = api.handle({"GET", "/admin/v1/chat?after=2", authorization, "", true});
  EXPECT_TRUE(empty.body.at("messages").empty());
  EXPECT_EQ(empty.body.at("next_after"), 2);
}

// The operator on-screen notice is a free-text PS2C_SystemMessageNoti (opcode 3). This checks the
// wire encoding only; the original client's display of free text is unverified.
TEST(AdminNotice, SystemMessageNotificationEncodesFreeText) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const Json message{{"text", "Server restart in 5 minutes"},
                     {"type", "EX5SystemMessageType_SystemMessage_Banner"}};

  const auto frame = protocol.encode(3, message);
  const auto decoded = protocol.decode(frame);

  EXPECT_EQ(decoded.at("packet"), "PS2C_SystemMessageNoti");
  EXPECT_EQ(decoded.at("message").at("text"), "Server restart in 5 minutes");
  EXPECT_EQ(decoded.at("message").at("type"), "EX5SystemMessageType_SystemMessage_Banner");
}

TEST(AdminToken, CreatesAndReusesTheTokenFile) {
  const auto file = std::filesystem::temp_directory_path() /
                    ("chronicles-admin-token-" + random_hex(8)) / "admin.token";

  const auto created = load_or_create_admin_token(file);
  EXPECT_EQ(created.size(), kAdminTokenBytes * 2);
  EXPECT_EQ(load_or_create_admin_token(file), created);

  {
    std::ofstream corrupt(file, std::ios::trunc);
    corrupt << "not-a-token\n";
  }
  EXPECT_THROW(load_or_create_admin_token(file), std::invalid_argument);

  std::error_code ignored;
  std::filesystem::remove_all(file.parent_path(), ignored);
}

} // namespace
} // namespace chronicles
