#pragma once

// Mechanically shared native-world fixtures. Their protocol/teardown timeout policies are
// unchanged; feature tests and the retained native integration tests use one definition.
#include "data/client_catalog.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/features/processing/processing.hpp"
#include "world/session/native_world.hpp"

#include "ec.generated.pb.h"
#include "p_character.pb.h"

#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/system_error.hpp>
#include <gtest/gtest.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace chronicles::native_testing {

namespace asio = boost::asio;

inline void drain_world_for_test(const std::shared_ptr<NativeWorldListener> &listener) {
  if (!listener->shutdown_started()) {
    listener->shutdown_when_drained([] {});
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!listener->shutdown_finished()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      std::fputs("Native world fixture could not drain retained owners within 20 seconds\n",
                 stderr);
      std::abort();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
namespace beast = boost::beast;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

class TemporaryWorldCertificate {
public:
  TemporaryWorldCertificate()
      : directory_(std::filesystem::temp_directory_path() /
                   ("chronicles-world-test-" + random_hex(8))),
        certificate_(directory_ / "certificate.pem"), private_key_(directory_ / "private-key.pem") {
    std::filesystem::create_directory(directory_);

    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!key_context || EVP_PKEY_keygen_init(key_context.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) != 1) {
      throw std::runtime_error("Cannot prepare temporary world test key");
    }

    EVP_PKEY *generated_key = nullptr;
    if (EVP_PKEY_keygen(key_context.get(), &generated_key) != 1) {
      throw std::runtime_error("Cannot generate temporary world test key");
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generated_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) ||
        X509_set_pubkey(certificate.get(), key.get()) != 1) {
      throw std::runtime_error("Cannot prepare temporary world test certificate");
    }

    auto *subject = X509_get_subject_name(certificate.get());
    if (!subject ||
        X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>("localhost"), -1, -1,
                                   0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
      throw std::runtime_error("Cannot sign temporary world test certificate");
    }

    std::unique_ptr<BIO, decltype(&BIO_free)> certificate_output(
        BIO_new_file(certificate_.string().c_str(), "w"), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> key_output(
        BIO_new_file(private_key_.string().c_str(), "w"), BIO_free);
    if (!certificate_output || !key_output ||
        PEM_write_bio_X509(certificate_output.get(), certificate.get()) != 1 ||
        PEM_write_bio_PrivateKey(key_output.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                 nullptr) != 1) {
      throw std::runtime_error("Cannot write temporary world test certificate");
    }
  }

  ~TemporaryWorldCertificate() {
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  const std::filesystem::path &certificate() const {
    return certificate_;
  }

  const std::filesystem::path &private_key() const {
    return private_key_;
  }

private:
  std::filesystem::path directory_;
  std::filesystem::path certificate_;
  std::filesystem::path private_key_;
};

class TwoClientWorldServer {
public:
  TwoClientWorldServer(AuthService &auth, const TemporaryWorldCertificate &certificate,
                       std::shared_ptr<const TutorialData> tutorial_data = {},
                       std::shared_ptr<const CharacterStats> character_stats = {},
                       std::shared_ptr<ClientCatalog> client_catalog = {},
                       std::filesystem::path catalog_root = {},
                       std::shared_ptr<const NpcPopulation> npc_population = {},
                       std::shared_ptr<GuildService> guilds = {},
                       std::shared_ptr<housing::HousingHub> housing = {},
                       std::shared_ptr<const LootData> loot_data = {},
                       std::shared_ptr<const GatheringData> gathering_data = {},
                       std::shared_ptr<const ProcessingData> processing_data = {},
                       WorldSocialServices social = {})
      : workers_(2),
        listener_(start_native_world(
            io_context_, workers_, auth, tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            CHRONICLES_PROTOCOL_DIR, certificate.certificate(), certificate.private_key(),
            std::chrono::milliseconds::zero(), false, false, true, std::move(tutorial_data),
            std::move(character_stats), std::move(client_catalog), std::move(gathering_data),
            std::move(loot_data), std::move(processing_data), catalog_root,
            std::move(npc_population), std::move(guilds), std::move(housing),
            kDefaultMaximumWorldConnections, std::move(social))),
        io_thread_([this] { io_context_.run(); }) {
  }

  ~TwoClientWorldServer() {
    drain_world_for_test(listener_);
    io_context_.stop();
    io_thread_.join();
    workers_.join();
  }

  unsigned short port() const {
    return listener_->port();
  }

  NativeWorldListener &listener() {
    return *listener_;
  }

  // Runs a handler on the single stream-executor thread. Tests block that thread to order a
  // queued world-admission completion after a concurrent revocation or reselection.
  void post_to_io(std::function<void()> handler) {
    asio::post(io_context_, std::move(handler));
  }

private:
  // The pool must outlive the io_context: pending timer handlers hold WorldSession objects whose
  // strands live in the pool's execution context, and io_context destruction destroys them.
  asio::thread_pool workers_;
  asio::io_context io_context_;
  std::shared_ptr<NativeWorldListener> listener_;
  std::thread io_thread_;
};

class WorldTestClient {
public:
  explicit WorldTestClient(unsigned short port)
      : context_(ssl::context::tls_client), stream_(io_context_, context_) {
    context_.set_verify_mode(ssl::verify_none);
    beast::get_lowest_layer(stream_).connect(tcp::endpoint(asio::ip::address_v4::loopback(), port));
    stream_.handshake(ssl::stream_base::client);
  }

  void send_key(const std::string &packet_key) {
    asio::write(stream_, asio::buffer(packet_key));
  }

  void send(const Frame &frame) {
    asio::write(stream_, asio::buffer(frame.bytes));
  }

  Frame read() {
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(5));
    std::array<std::uint8_t, 2> length_bytes{};
    read_exact(asio::buffer(length_bytes));
    const auto frame_length = static_cast<std::size_t>(length_bytes[0]) |
                              (static_cast<std::size_t>(length_bytes[1]) << 8);
    if (frame_length < 4) {
      throw std::runtime_error("World test received an invalid frame length");
    }

    Frame frame;
    frame.bytes.resize(frame_length);
    std::copy(length_bytes.begin(), length_bytes.end(), frame.bytes.begin());
    read_exact(asio::buffer(frame.bytes.data() + 2, frame_length - 2));
    return frame;
  }

  void disconnect() {
    beast::error_code ignored;
    beast::get_lowest_layer(stream_).socket().close(ignored);
  }

private:
  void read_exact(asio::mutable_buffer buffer) {
    // Beast deadlines apply to asynchronous operations. A synchronous read can
    // otherwise hang forever when the expected gameplay notification is absent.
    beast::error_code read_error;
    io_context_.restart();
    asio::async_read(stream_, buffer,
                     [&read_error](beast::error_code error, std::size_t) { read_error = error; });
    io_context_.run();
    if (read_error) {
      throw boost::system::system_error(read_error, "World test frame read failed");
    }
  }

  asio::io_context io_context_;
  ssl::context context_;
  beast::ssl_stream<beast::tcp_stream> stream_;
};

inline Frame read_entry_inventory(WorldTestClient &client) {
  EXPECT_EQ(client.read().opcode(), kWorldCharExpertiseSkillsNotification);

  auto inventory = client.read();
  EXPECT_EQ(inventory.opcode(), 130);

  EXPECT_EQ(client.read().opcode(), kWorldCharItemCraftsNotification);
  return inventory;
}

inline void expect_pre_playing_combat_bootstrap(WorldTestClient &client, Protocol &protocol) {
  const auto first_gauge = client.read();
  const auto second_gauge = client.read();
  EXPECT_EQ(first_gauge.opcode(), 78);
  EXPECT_EQ(second_gauge.opcode(), 78);
  EXPECT_EQ(protocol.decode(first_gauge).at("message"), nlohmann::json::object());
  EXPECT_EQ(protocol.decode(second_gauge).at("message"), nlohmann::json::object());

  const auto initial_attributes = client.read();
  EXPECT_EQ(initial_attributes.opcode(), 33);
  EXPECT_EQ(protocol.decode(initial_attributes).at("message").at("attributes").at("2"), 1);
  const auto traits = client.read();
  EXPECT_EQ(traits.opcode(), 115);
  EXPECT_EQ(protocol.decode(traits).at("message").at("unitTraits").at("1").at("type"),
            "EX5UnitTraitValueType_PerMille");
  const auto changed_attributes = client.read();
  EXPECT_EQ(changed_attributes.opcode(), 33);
  EXPECT_EQ(protocol.decode(changed_attributes).at("message").at("attributes").at("2"), 2);
}

inline void
add_default_merchant_tables(std::map<std::string, std::vector<nlohmann::json>> &tables) {
  for (const char *name : {"Npc", "Interaction", "InteractionCondition", "ShopItemCondition",
                           "ItemRepairCost", "VoucherGroup", "VoucherCondition", "TradeGood"}) {
    tables[name];
  }
  tables["Money"] = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  auto &variables = tables["GameVariable"];
  variables.push_back({{"name", "ItemMaxStack"}, {"value", "1000"}});
  variables.push_back({{"name", "ResetBaseDateTime"}, {"value", "2023-01-02 05:00:00"}});
}

class TemporaryWorldCatalog {
public:
  TemporaryWorldCatalog() {
    const auto unique_number = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("chronicles-world-catalog-test-" + std::to_string(unique_number)) / "catalog";
    if (!std::filesystem::create_directories(root_ / "Tables")) {
      throw std::runtime_error("Cannot create temporary world catalog directory");
    }
  }

  ~TemporaryWorldCatalog() {
    if (root_.parent_path().parent_path() == std::filesystem::temp_directory_path()) {
      std::error_code ignored_error;
      std::filesystem::remove_all(root_.parent_path(), ignored_error);
    }
  }

  using ExtraTables = std::vector<std::pair<std::string, std::vector<nlohmann::json>>>;

  void write_items(const std::vector<nlohmann::json> &rows, const ExtraTables &extra_tables = {},
                   const std::string &source_sha256 = {}) {
    std::map<std::string, std::vector<nlohmann::json>> tables(extra_tables.begin(),
                                                              extra_tables.end());
    tables["Item"] = rows;
    write_tables(std::move(tables), source_sha256);
  }

  // Writes the given tables plus the BagDefaultSafeWeight row InventoryCapacity requires.
  void write_tables(std::map<std::string, std::vector<nlohmann::json>> tables,
                    const std::string &source_sha256 = {}) {
    tables["GameVariable"].push_back({{"name", "BagDefaultSafeWeight"}, {"value", "50000"}});
    if (tables.contains("Shop")) {
      add_default_merchant_tables(tables);
    }
    // The world service now requires the same verified raw-layout contract for tutorial
    // revival. Keep this small catalog fixture self-contained alongside its shrine data.
    if (!tables.contains("Map")) {
      tables["Map"] = {
          {{"id", "8000"},
           {"world", "/Game/Maps/Test/Test.Test"},
           {"area",
            R"("min":{"x":-1000000,"y":-1000000,"z":-1000000},"max":{"x":1000000,"y":1000000,"z":1000000})"}}};
    }
    const auto raw_file =
        root_.parent_path() /
        "raw/X5/Content/Data/BuiltData/Maps/Test/DataLayers/Runtime/spot_object.json";
    std::filesystem::create_directories(raw_file.parent_path());
    std::ofstream raw(raw_file, std::ios::binary);
    raw << R"({"sectors":[{"spotObjects":[{"types":[{"adventureObject":{"nuiIdol":{"spawnVolume":{"spawnVolumeSphere":{"spawnPosition":{"x":0,"y":0,"z":0,"yaw":0}}}}}}]}]}]})";
    raw.close();

    const auto write_table = [&](const std::string &name, const std::vector<nlohmann::json> &rows) {
      std::string data;
      for (const auto &row : rows) {
        data += row.dump() + "\n";
      }

      const auto path = "Tables/" + name + ".jsonl";
      std::ofstream output(root_ / path, std::ios::binary);
      output << data;
      output.close();

      unsigned char digest[EVP_MAX_MD_SIZE]{};
      unsigned int digest_length{};
      if (EVP_Digest(data.data(), data.size(), digest, &digest_length, EVP_sha256(), nullptr) !=
          1) {
        throw std::runtime_error("Cannot hash temporary world catalog table");
      }
      std::string sha256;
      for (unsigned int index = 0; index < digest_length; ++index) {
        constexpr char kHexDigits[] = "0123456789abcdef";
        sha256.push_back(kHexDigits[digest[index] >> 4]);
        sha256.push_back(kHexDigits[digest[index] & 15]);
      }
      return nlohmann::json{
          {"path", path}, {"bytes", data.size()}, {"records", rows.size()}, {"sha256", sha256}};
    };

    nlohmann::json files = nlohmann::json::array();
    for (const auto &[name, rows] : tables) {
      files.push_back(write_table(name, rows));
    }
    nlohmann::json manifest = {
        {"format_version", 1}, {"table_count", tables.size()}, {"files", std::move(files)}};
    if (!source_sha256.empty()) {
      manifest["source_executable_dump_sha256"] = source_sha256;
    }
    std::ofstream manifest_output(root_ / "manifest.json", std::ios::binary);
    manifest_output << manifest.dump();
  }

  std::shared_ptr<ClientCatalog> open() {
    return std::make_shared<ClientCatalog>(root_);
  }

  const std::filesystem::path &root() const {
    return root_;
  }

private:
  std::filesystem::path root_;
};

inline Frame read_until_opcode(WorldTestClient &client, std::uint16_t opcode,
                               int maximum_skipped_frames = 32) {
  for (int skipped = 0; skipped < maximum_skipped_frames; ++skipped) {
    auto frame = client.read();
    if (frame.opcode() == opcode) {
      return frame;
    }
  }
  throw std::runtime_error("Expected world frame did not arrive");
}

inline x5::pb::frontwebsocket::CharSelectRes
select_world_test_character(AuthService &auth, const std::string &account_name,
                            const std::string &character_name, int class_id) {
  constexpr const char *kPassword = "test-world-password-123";
  if (!auth.create_account(account_name, kPassword)) {
    throw std::runtime_error("Cannot create world test account");
  }

  const auto login = auth.login(account_name, kPassword);
  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  if (auth.enter(login.access_token(), world_request).error_code() != x5::ec::Success) {
    throw std::runtime_error("Cannot enter world test realm");
  }

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name(character_name);
  create_request.set_char_class(static_cast<x5::cms::EX5CharacterClass>(class_id));
  const auto created = auth.create(login.access_token(), create_request);
  if (created.ec() != x5::ec::Success) {
    throw std::runtime_error("Cannot create world test character");
  }

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(created.char_().char_id());
  const auto selected = auth.select(login.access_token(), select_request);
  if (selected.ec() != x5::ec::Success || selected.packet_key().size() != 32) {
    throw std::runtime_error("Cannot select world test character");
  }
  return selected;
}

inline void enter_shared_world(WorldTestClient &client, Protocol &protocol,
                               const std::string &packet_key) {
  client.send_key(packet_key);
  EXPECT_EQ(client.read().opcode(), 5);

  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  read_entry_inventory(client);
  EXPECT_EQ(client.read().opcode(), 126);

  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  const auto map_complete = client.read();
  EXPECT_EQ(map_complete.opcode(), 176);
  EXPECT_EQ(protocol.decode(map_complete).at("status"), "decoded");
  expect_pre_playing_combat_bootstrap(client, protocol);
}

inline constexpr int kOverworldMapId = 9012;
inline constexpr std::uint64_t kFirstServerNpcUnitId = 0x80000000ULL;
inline constexpr std::uint64_t kLastServerNpcUnitId = 0xFFFFFFFFULL;
inline constexpr std::uint16_t kUnitAddOpcode = 10;
inline constexpr std::uint16_t kPongOpcode = 4;
inline constexpr std::size_t kTestPlacementBound = 64;
inline constexpr int kOverworldSpawnXmm = -1242000;
inline constexpr int kOverworldSpawnYmm = 2993000;
inline constexpr int kOverworldSpawnZmm = -647000;

inline nlohmann::json overworld_placement_json(int npc_cms_id, int offset_mm, bool active) {
  return {{"spawner_guid", "00000000-0000-0000-0000-000000000000"},
          {"spawner_guid_wire", {std::to_string(offset_mm), "7"}},
          {"npc_cms_id", npc_cms_id},
          {"behavior", "Civilian"},
          {"faction", "TownNpc"},
          {"interaction_group_id", 219},
          {"interactions", {{{"interaction_id", 219}, {"task_case", "Shop"}, {"shop_id", 3002}}}},
          {"position_mm",
           {{"x", kOverworldSpawnXmm + offset_mm},
            {"y", kOverworldSpawnYmm},
            {"z", kOverworldSpawnZmm}}},
          {"yaw_degrees", 90.5},
          {"active", active}};
}

inline std::shared_ptr<const NpcPopulation> small_overworld_population() {
  const nlohmann::json document{{"format_version", 1},
                                {"map_id", kOverworldMapId},
                                {"layers_missing", nlohmann::json::array()},
                                {"placements",
                                 {overworld_placement_json(1001121, 1000, true),
                                  overworld_placement_json(5001015, 2000, true),
                                  overworld_placement_json(1001671, 3000, true),
                                  overworld_placement_json(1001672, 4000, false)}}};
  return std::make_shared<const NpcPopulation>(
      NpcPlacementSet::from_json(document, kTestPlacementBound), kTestPlacementBound);
}

struct OverworldEntry {
  nlohmann::json map_start;
  nlohmann::json map_complete;
  std::vector<nlohmann::json> unit_adds;
};

class OverworldSessions {
public:
  using StateWrapper = std::function<std::shared_ptr<CharacterStateRepository>(
      std::shared_ptr<CharacterStateRepository>)>;
  // Builds realm services (companies, company dialogues) over the fixture's repositories.
  using SocialFactory = std::function<WorldSocialServices(MemoryRepositories &)>;

  struct Session {
    std::string access_token;
    std::int64_t account_id{};
    std::int64_t character_id{};
    std::unique_ptr<WorldTestClient> client;
  };

  OverworldSessions(std::shared_ptr<const NpcPopulation> population,
                    const std::vector<std::optional<WorldPosition>> &saved_positions,
                    std::shared_ptr<ClientCatalog> client_catalog = {},
                    std::filesystem::path catalog_root = {},
                    std::shared_ptr<const TutorialData> tutorial_data = {},
                    std::shared_ptr<const CharacterStats> character_stats = {},
                    std::shared_ptr<const LootData> loot_data = {},
                    std::shared_ptr<const GatheringData> gathering_data = {},
                    std::shared_ptr<const ProcessingData> processing_data = {},
                    StateWrapper state_wrapper = {}, SocialFactory social_factory = {})
      : protocol(CHRONICLES_PROTOCOL_DIR) {
    config.game_address = "127.0.0.1:8081";
    auto backing = repositories.character_repositories();
    if (state_wrapper) {
      backing.character_state = state_wrapper(backing.character_state);
    }
    auth = std::make_unique<AuthService>(std::move(backing), config);
    // Production acquires its reward epoch before starting the world listener. This fixture
    // constructs the listener directly, so initialize its isolated memory backend here.
    auth->acquire_monster_reward_owner_epoch();
    for (std::size_t index = 0; index < saved_positions.size(); ++index) {
      const std::string suffix = std::to_string(index);
      const std::string account_name = "overworld_sessions" + suffix;
      auth->create_account(account_name, "test-world-password-123");
      const auto login = auth->login(account_name, "test-world-password-123");
      x5::pb::authwebsocket::WorldEnterReq world_request;
      world_request.set_world_id(1);
      if (auth->enter(login.access_token(), world_request).error_code() != x5::ec::Success) {
        throw std::runtime_error("Overworld fixture cannot enter the world");
      }
      x5::pb::ws::front::CharCreateReq create_request;
      create_request.set_slot(0);
      create_request.set_char_name("Overworld" + suffix);
      create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
      const auto created = auth->create(login.access_token(), create_request);
      if (created.ec() != x5::ec::Success) {
        throw std::runtime_error("Overworld fixture cannot create a character");
      }
      auto &session = sessions.emplace_back();
      session.access_token = login.access_token();
      session.account_id = login.account_id();
      session.character_id = created.char_().char_id();
      if (saved_positions[index]) {
        if (!repositories.characters->save_world_position(session.account_id, session.character_id,
                                                          1, *saved_positions[index])) {
          throw std::runtime_error("Overworld fixture cannot save the world position");
        }
      }
    }
    auto social = social_factory ? social_factory(repositories) : WorldSocialServices{};
    server = std::make_unique<TwoClientWorldServer>(
        *auth, certificate, std::move(tutorial_data), std::move(character_stats),
        std::move(client_catalog), std::move(catalog_root), std::move(population), nullptr, nullptr,
        std::move(loot_data), std::move(gathering_data), std::move(processing_data),
        std::move(social));
  }

  void connect(Session &session) {
    x5::pb::ws::front::CharSelectReq select_request;
    select_request.set_char_id(session.character_id);
    const auto selected = auth->select(session.access_token, select_request);
    if (selected.ec() != x5::ec::Success) {
      throw std::runtime_error("Overworld fixture cannot select the character");
    }
    session.client = std::make_unique<WorldTestClient>(server->port());
    session.client->send_key(selected.packet_key());
    EXPECT_EQ(session.client->read().opcode(), 5);
  }

  // Frames up to and including the pong of a ping sent at `sequence`; the pong is a barrier that
  // proves nothing else was queued behind it.
  std::vector<Frame> read_until_pong(Session &session, std::uint32_t sequence) {
    session.client->send(protocol.encode(20003, nlohmann::json::object(), sequence));
    std::vector<Frame> frames;
    for (;;) {
      auto frame = session.client->read();
      const bool is_pong = frame.opcode() == kPongOpcode;
      frames.push_back(std::move(frame));
      if (is_pong) {
        return frames;
      }
    }
  }

  // Full entry: map start, snapshot, map load complete, bootstrap frames, population adds.
  OverworldEntry enter(Session &session) {
    connect(session);
    return enter_after_key_exchange(session);
  }

  // Complete map entry after a caller has supplied the key and read its response.
  OverworldEntry enter_after_key_exchange(Session &session) {
    OverworldEntry entry;
    session.client->send(protocol.encode(20001, nlohmann::json::object(), 0));
    const auto map_start = session.client->read();
    EXPECT_EQ(map_start.opcode(), 175);
    entry.map_start = protocol.decode(map_start).at("message");
    // Catalog-backed entries add snapshots (for example WorldCharPerkNoti 135) between the
    // fixed bootstrap frames, so the fixed frames are checked as an ordered subsequence.
    const auto read_through = [&](std::uint16_t expected) {
      for (int skipped = 0; skipped < 16; ++skipped) {
        if (session.client->read().opcode() == expected) {
          return;
        }
      }
      ADD_FAILURE() << "Entry frame " << expected << " not received";
    };

    for (const std::uint16_t opcode : {1, 130, 126}) {

      read_through(opcode);
    }

    session.client->send(protocol.encode(20085, nlohmann::json::object(), 1));

    auto map_complete = session.client->read();

    for (int skipped = 0; skipped < 16 && map_complete.opcode() != 176; ++skipped) {
      map_complete = session.client->read();
    }
    EXPECT_EQ(map_complete.opcode(), 176);
    entry.map_complete = protocol.decode(map_complete).at("message");
    for (const std::uint16_t opcode : {78, 78, 33, 115, 33}) {
      read_through(opcode);
    }

    for (const auto &frame : read_until_pong(session, 2)) {
      if (frame.opcode() == kUnitAddOpcode) {
        entry.unit_adds.push_back(protocol.decode(frame).at("message"));
      }
    }
    return entry;
  }

  // Opcodes the server sends in answer to the time sync and map-load-complete of a refused entry.
  std::vector<std::uint16_t> refused_entry_opcodes(Session &session) {
    connect(session);
    session.client->send(protocol.encode(20001, nlohmann::json::object(), 0));
    session.client->send(protocol.encode(20085, nlohmann::json::object(), 1));
    std::vector<std::uint16_t> opcodes;
    for (const auto &frame : read_until_pong(session, 2)) {
      opcodes.push_back(frame.opcode());
    }
    return opcodes;
  }

  static std::set<std::uint64_t> unit_ids(const OverworldEntry &entry) {
    std::set<std::uint64_t> ids;
    for (const auto &add : entry.unit_adds) {
      ids.insert(std::stoull(add.at("unit_id").get<std::string>()));
    }
    return ids;
  }

  MemoryRepositories repositories;
  Config config;
  Protocol protocol;
  std::unique_ptr<AuthService> auth;
  TemporaryWorldCertificate certificate;
  std::unique_ptr<TwoClientWorldServer> server;
  std::vector<Session> sessions;
};

inline const WorldPosition kOverworldSpawn{kOverworldMapId, kOverworldSpawnXmm, kOverworldSpawnYmm,
                                           kOverworldSpawnZmm, 0.0F};

inline void expect_overworld_entry(const OverworldEntry &entry, std::size_t expected_units) {
  EXPECT_EQ(entry.map_start.at("map_id"), kOverworldMapId);
  const auto &spawn = entry.map_start.at("spawn_point").at("pos");
  EXPECT_EQ(spawn.at("millimeter_x"), kOverworldSpawn.x_mm);
  EXPECT_EQ(spawn.at("millimeter_y"), kOverworldSpawn.y_mm);
  EXPECT_EQ(spawn.at("millimeter_z"), kOverworldSpawn.z_mm);
  EXPECT_EQ(entry.map_complete.at("map_id"), kOverworldMapId);

  ASSERT_EQ(entry.unit_adds.size(), expected_units);
  const auto ids = OverworldSessions::unit_ids(entry);
  EXPECT_EQ(ids.size(), expected_units);
  for (const auto id : ids) {
    EXPECT_GE(id, kFirstServerNpcUnitId);
    EXPECT_LE(id, kLastServerNpcUnitId);
  }
}

inline std::vector<nlohmann::json> decoded_frames(Protocol &protocol,
                                                  const std::vector<Frame> &frames,
                                                  const std::vector<int> &opcodes) {
  std::vector<nlohmann::json> selected;
  for (const auto &frame : frames) {
    if (std::ranges::find(opcodes, static_cast<int>(frame.opcode())) != opcodes.end()) {
      selected.push_back(protocol.decode(frame));
    }
  }
  return selected;
}

struct DecodedWorldFrame {
  std::uint16_t opcode{};
  nlohmann::json message;
};

// Drives one overworld session with ordered client sequences and a ping barrier after each
// request, so every frame the server queued for this connection is collected.
class VehicleClient {
public:
  VehicleClient(OverworldSessions &world, OverworldSessions::Session &session,
                std::uint32_t next_sequence)
      : world_(world), session_(session), next_sequence_(next_sequence) {
  }

  std::vector<DecodedWorldFrame> send(std::uint16_t opcode, const nlohmann::json &body) {
    session_.client->send(world_.protocol.encode(opcode, body, next_sequence_++));
    return drain();
  }

  void send_without_barrier(std::uint16_t opcode, const nlohmann::json &body) {
    session_.client->send(world_.protocol.encode(opcode, body, next_sequence_++));
  }

  std::vector<DecodedWorldFrame> drain() {
    std::vector<DecodedWorldFrame> decoded;
    for (const auto &frame : world_.read_until_pong(session_, next_sequence_++)) {
      if (frame.opcode() != kPongOpcode) {
        decoded.push_back({frame.opcode(), world_.protocol.decode(frame).at("message")});
      }
    }
    return decoded;
  }

  nlohmann::json move_to(const WorldPosition &position) {
    nlohmann::json body{{"tick", "1"},
                        {"mode", "MoveMode_Normal"},
                        {"detail", "MoveDetail_Run"},
                        {"pos",
                         {{"millimeter_x", position.x_mm},
                          {"millimeter_y", position.y_mm},
                          {"millimeter_z", position.z_mm}}},
                        {"body_yaw", 0.0}};
    send(20087, body);
    return body;
  }

private:
  OverworldSessions &world_;
  OverworldSessions::Session &session_;
  std::uint32_t next_sequence_{};
};

inline std::vector<nlohmann::json> frames_of(const std::vector<DecodedWorldFrame> &frames,
                                             std::uint16_t opcode) {
  std::vector<nlohmann::json> matches;
  for (const auto &frame : frames) {
    if (frame.opcode == opcode) {
      matches.push_back(frame.message);
    }
  }
  return matches;
}

} // namespace chronicles::native_testing
