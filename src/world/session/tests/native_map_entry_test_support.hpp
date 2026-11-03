#pragma once

// Shared unchanged map-entry fixtures used by NPC integration and generic channel travel.
// The accepted base fixture remains the single TLS/catalog/client implementation.
#include "world/features/npc/npc_sight.hpp"
#include "world/features/shop/tests/native_shop_test_support.hpp"
#include "world/session/tests/native_world_test_support.hpp"

namespace chronicles::native_testing {

// Map-9012 entry and cross-session fixtures.
//
// Everything is observed on the wire. Real data: the tutorial profile (CHRONICLES_TUTORIAL_TEST_PROFILE)
// and the full map-9012 placement file (CHRONICLES_NPC_PLACEMENTS_9012). A skipped test is not proof.

// Merchant NPC 1001121 (interaction 212, shop 1001) stands here; the persisted position of the
// overworld test character is set to it because the server uses a persisted position verbatim.
constexpr int kMerchantNpcCmsId = 1001121;

struct AnnouncedNpcFrame {
  std::uint64_t unit_id{};
  int npc_cms_id{};
  int x_mm{};
  int y_mm{};
  int z_mm{};
};

// Everything a client receives from the key exchange until the pong of a ping sent right after
// MapLoadComplete. The pong is queued after every notification the entry produced.
struct EntryObservation {
  std::vector<std::uint16_t> opcodes;
  std::vector<AnnouncedNpcFrame> npcs;
  std::vector<int> map_ids;  // map_id of opcodes 175 and 176, in arrival order
  // Every decoded message of the entry, in arrival order, for checks the fields above omit.
  std::vector<std::pair<std::uint16_t, nlohmann::json>> messages;
};

// Same lifetime layout as TwoClientWorldServer plus the optional overworld population.
class MapEntryServer {
public:
  MapEntryServer(AuthService &auth, const TemporaryWorldCertificate &certificate,
                 std::shared_ptr<const TutorialData> tutorial_data,
                 std::shared_ptr<const CharacterStats> character_stats,
                 std::shared_ptr<ClientCatalog> client_catalog, std::filesystem::path catalog_root,
                 std::shared_ptr<const NpcPopulation> population)
      : workers_(2),
        listener_(start_native_world(
            io_context_, workers_, auth, tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            CHRONICLES_PROTOCOL_DIR, certificate.certificate(), certificate.private_key(),
            std::chrono::milliseconds::zero(), false, false, true, std::move(tutorial_data),
            std::move(character_stats), std::move(client_catalog), {}, {}, {}, catalog_root,
            std::move(population))),
        io_thread_([this] {
          io_context_.run();
        }) {
  }

  ~MapEntryServer() {
    drain_world_for_test(listener_);
    io_context_.stop();
    io_thread_.join();
    workers_.join();
  }

  unsigned short port() const {
    return listener_->port();
  }

private:
  asio::thread_pool workers_;
  asio::io_context io_context_;
  std::shared_ptr<NativeWorldListener> listener_;
  std::thread io_thread_;
};

// The entry helpers send time sync (0), map-complete (1) and a ping (2); the server then expects
// sequence 3 from a fresh connection.
constexpr std::uint32_t kFirstSequenceAfterEntry = 3;

class MapEntryWorld {
public:
  struct Character {
    std::string access_token;
    std::int64_t account_id{};
    std::int64_t character_id{};
    std::uint32_t sequence{kFirstSequenceAfterEntry};
  };

  MapEntryWorld(const char *profile_path, const char *placements_path, bool load_placements)
      : definitions(TutorialData::load(profile_path)),
        catalog_root(std::filesystem::path(profile_path).parent_path().parent_path() / "catalog"),
        catalog(std::make_shared<ClientCatalog>(catalog_root)),
        stats(CharacterStats::load(*catalog)),
        protocol(CHRONICLES_PROTOCOL_DIR) {
    if (load_placements) {
      population = load_npc_population_file(placements_path, kMaxPopulatedNpcUnits);
    }
    config.game_address = "127.0.0.1:8081";
    auth = std::make_unique<AuthService>(repositories.character_repositories(), config);
    auth->acquire_monster_reward_owner_epoch();
    server = std::make_unique<MapEntryServer>(*auth, certificate, definitions, stats, catalog,
                                              catalog_root, population);
  }

  // Creates an account and a Fight character persisted at `position` (a persisted position is
  // the only server state that places a character on a map).
  Character add_character(const std::string &name, const WorldPosition &position) {
    const std::string account = "entry_" + name;
    EXPECT_TRUE(auth->create_account(account, "map-entry-fixture-password"));
    const auto login = auth->login(account, "map-entry-fixture-password");
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(1);
    auth->enter(login.access_token(), enter);
    ::chronicles::Character created;
    created.account_id = login.account_id();
    created.name = name;
    created.world_position = position;
    created = repositories.characters->create(created);
    repositories.characters->save_world_position(created.account_id, created.id, 1, position);
    Character result;
    result.access_token = login.access_token();
    result.account_id = login.account_id();
    result.character_id = created.id;
    return result;
  }

  WorldPosition tutorial_position() const {
    const auto &point = definitions->document.at("objects").at(0).at("position");
    return WorldPosition{8000, point.at(0), point.at(1), point.at(2), 0};
  }

  WorldPosition overworld_position() const {
    return WorldPosition{kOverworldMapId, -1242227, 2993829, -647306, 0};
  }

  WorldProgress progress(const Character &character) const {
    return repositories.character_state->load_world_progress(character.account_id,
                                                             character.character_id, 1);
  }

  // Key exchange, time sync, MapLoadComplete and a ping, then everything up to the pong.
  EntryObservation enter(const Character &character, WorldTestClient &client) {
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(character.character_id);
    const auto selected = auth->select(character.access_token, select);
    client.send_key(selected.packet_key());
    EntryObservation observation;
    record(observation, client.read());
    client.send(protocol.encode(20001, nlohmann::json::object(), 0));
    client.send(protocol.encode(20085, nlohmann::json::object(), 1));
    client.send(protocol.encode(20003, nlohmann::json::object(), 2));
    for (;;) {
      const auto frame = client.read();
      record(observation, frame);
      if (frame.opcode() == 4) {
        return observation;
      }
    }
  }

  // The server allows one connection per character and releases it only after it has processed
  // the previous connection's close, which can lag a client-side drop that left frames queued.
  // A reconnect in that window is refused by the server, so retry within a bounded time. The
  // product behaviour is unchanged; a persistent refusal still fails the test.
  EntryObservation enter_when_character_is_free(const Character &character,
                                                std::unique_ptr<WorldTestClient> &client) {
    constexpr int kMaximumAttempts = 100;
    constexpr auto kRetryDelay = std::chrono::milliseconds(100);
    for (int attempt = 1;; ++attempt) {
      client = std::make_unique<WorldTestClient>(server->port());
      try {
        return enter(character, *client);
      } catch (const std::exception &) {
        client.reset();
        if (attempt == kMaximumAttempts) {
          throw;
        }
        std::this_thread::sleep_for(kRetryDelay);
      }
    }
  }

  // Entry for a character the server refuses to place: only the key-exchange answer and the
  // time-sync reply are expected (no map move), then the pong.
  EntryObservation enter_and_observe_refusal(const Character &character, WorldTestClient &client) {
    return enter(character, client);
  }

  void record(EntryObservation &observation, const Frame &frame) {
    observation.opcodes.push_back(frame.opcode());
    try {
      observation.messages.emplace_back(frame.opcode(), protocol.decode(frame).at("message"));
    } catch (const std::exception &) {
      // Frames without a recovered schema stay out of the message list.
    }
    if (frame.opcode() != 10 && frame.opcode() != 175 && frame.opcode() != 176) {
      return;
    }
    const auto decoded = protocol.decode(frame);
    const auto &message = decoded.at("message");
    if (frame.opcode() != 10) {
      observation.map_ids.push_back(message.at("map_id"));
      return;
    }
    if (!message.contains("nub") || !message.at("nub").contains("npc")) {
      return;
    }
    const auto &pos = message.at("nub").at("pos");
    observation.npcs.push_back(
        {std::stoull(message.at("unit_id").get<std::string>()),
         message.at("nub").at("npc").at("npc_cms_id").get<int>(), pos.at("millimeter_x").get<int>(),
         pos.at("millimeter_y").get<int>(), pos.at("millimeter_z").get<int>()});
  }

  MerchantRequestPacer shop_pacer;

  void send_shop(const Character &character_state, WorldTestClient &client,
                 const std::string &target, int interaction_id, const nlohmann::json &shop,
                 std::uint32_t &sequence) {
    (void)character_state;
    shop_pacer.wait();
    client.send(protocol.encode(
        20068,
        {{"target_unit_id", target},
         {"param", {{"interaction_id", interaction_id}, {"shop", shop}}}},
        sequence++));
  }

  Frame read_reply(WorldTestClient &client) {
    auto frame = client.read();
    // Shared-map patrol and sight notifications can interleave with paced shop replies.
    while (frame.opcode() == 177 || frame.opcode() == 10 || frame.opcode() == 11) {
      frame = client.read();
    }
    return frame;
  }

  // Exactly one error reply, then the pong; independent patrol movement may interleave.
  void expect_one_error_then_pong(WorldTestClient &client, std::uint32_t &sequence,
                                  const std::string &expected_ec, std::uint16_t expected_opcode = 2) {
    const auto reply = read_reply(client);
    ASSERT_EQ(reply.opcode(), expected_opcode);
    EXPECT_EQ(protocol.decode(reply).at("message").at("ec"), expected_ec);
    client.send(protocol.encode(20003, nlohmann::json::object(), sequence++));
    EXPECT_EQ(read_reply(client).opcode(), 4);
  }

  std::shared_ptr<const TutorialData> definitions;
  std::filesystem::path catalog_root;
  std::shared_ptr<ClientCatalog> catalog;
  std::shared_ptr<const CharacterStats> stats;
  MemoryRepositories repositories;
  Protocol protocol;
  std::shared_ptr<const NpcPopulation> population;
  Config config;
  TemporaryWorldCertificate certificate;
  std::unique_ptr<AuthService> auth;
  std::unique_ptr<MapEntryServer> server;
};

inline std::vector<std::uint64_t> sorted_unit_ids(const std::vector<AnnouncedNpcFrame> &npcs) {
  std::vector<std::uint64_t> ids;
  for (const auto &npc : npcs) {
    ids.push_back(npc.unit_id);
  }
  std::ranges::sort(ids);
  return ids;
}

} // namespace chronicles::native_testing
