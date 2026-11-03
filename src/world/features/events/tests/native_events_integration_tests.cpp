#include "world/session/tests/native_world_test_support.hpp"

#include "testing/housing_test_support.hpp"
#include "world/features/events/channel_event.hpp"
#include "world/features/events/ranking_repository.hpp"
#include "world/features/fog/fog_repository.hpp"
#include "world/features/housing/housing_world.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "world/features/market/market_repository.hpp"

#include "ec.generated.pb.h"
#include "p_character.pb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;

TEST(NativeWorld, RankingsRaidChannelsAndTributesOverTls) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }
  constexpr char kPassword[] = "event-keeper-world-password";
  constexpr int kRanking = 1000001;
  constexpr int kRaidMap = 10103;

  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("event_keeper", kPassword));
  ASSERT_TRUE(auth.set_account_role("event_keeper", AccountRole::game_master));

  const auto login = auth.login("event_keeper", kPassword);
  x5::pb::authwebsocket::WorldEnterReq enter_request;
  enter_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), enter_request).error_code(), x5::ec::Success);
  Character keeper;
  keeper.account_id = login.account_id();
  keeper.name = "Eventkeeper";
  keeper = repositories.characters->create(keeper);
  // Authored tribute altar on the hosted parent map. No live player/session data is used.
  const WorldPosition origin{9012, -2450974, 3854433, -604092, 0.0F};
  ASSERT_TRUE(
      repositories.characters->save_world_position(keeper.account_id, keeper.id, 1, origin));
  const auto before =
      repositories.character_state->load_world_progress(keeper.account_id, keeper.id, 1);
  auto owned = before;
  ++owned.revision;
  owned.stackables[100194] = 3;
  ASSERT_TRUE(repositories.character_state->save_world_progress(keeper.account_id, keeper.id, 1,
                                                                before, owned));
  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(keeper.id);
  const auto selection = auth.select(login.access_token(), select_request);

  auto client_catalog = std::make_shared<ClientCatalog>(catalog_path);
  const auto stats = CharacterStats::load(*client_catalog);
  auto housing_service = std::make_shared<housing::HousingService>(
      housing::testing_support::housing_catalog(),
      std::make_shared<housing::MemoryHousingStore>(repositories.character_state), 1, [] {
        return std::int64_t{638'000'000'000'000'000};
      });
  auto housing_hub = std::make_shared<housing::HousingHub>(housing_service);
  // Keep this service trial independent of optional full-world monster/static-NPC profiles.
  // The catalog and event definitions remain the verified export, not synthetic packet data.
  TemporaryWorldCatalog event_profiles;
  const auto event_directory = event_profiles.root().parent_path() / "channel-events";
  std::filesystem::create_directories(event_directory);
  std::filesystem::copy_file(
      std::filesystem::path(catalog_path).parent_path() / "channel-events/definitions.json",
      event_directory / "definitions.json");
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, stats, client_catalog, event_profiles.root(),
                              small_overworld_population(), {}, housing_hub);
  const auto mail = std::make_shared<MemoryMailRepository>(repositories.database);
  server.listener().set_repositories(
      {mail, std::make_shared<MemoryMarketRepository>(repositories.database),
       std::make_shared<MemoryRankingRepository>(repositories.database),
       std::make_shared<MemoryFogRepository>(repositories.database)});
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient client(server.port());
  client.send_key(selection.packet_key());
  ASSERT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  read_until_opcode(client, 126);
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  read_until_opcode(client, 176);

  const nlohmann::json list_request{{"ranking_cms_id", kRanking},
                                    {"ranking_record_type", "ERankingRecordType_Server"}};
  client.send(protocol.encode(20006, list_request, 2));
  // A verified full-map profile can announce more units than the minimal socket fixtures.
  constexpr int kMaximumMapBootstrapFrames = 2048;
  const auto empty = protocol.decode(
      read_until_opcode(client, 9, kMaximumMapBootstrapFrames)).at("message");
  EXPECT_FALSE(empty.contains("ec"));
  EXPECT_FALSE(empty.contains("rankings"));
  EXPECT_EQ(empty.at("ranking_cms_id"), kRanking);
  EXPECT_EQ(empty.at("reset_counter"), "1");
  EXPECT_TRUE(empty.contains("next_reset_epochtime"));

  client.send(protocol.encode(20009, {{"text", "ranking_start 1000001"}}, 3));
  const auto started = protocol.decode(read_until_opcode(client, 7)).at("message");
  EXPECT_EQ(started.at("ranking_cms_id"), kRanking);
  EXPECT_TRUE(started.contains("start_tick"));

  client.send(protocol.encode(20009, {{"text", "ranking_finish 1000001"}}, 4));
  const auto finished = protocol.decode(read_until_opcode(client, 8)).at("message");
  EXPECT_FALSE(finished.contains("ec"));
  EXPECT_EQ(finished.at("party_ranking").at("rank"), "1");
  EXPECT_EQ(finished.at("party_ranking").at("core_info").at("name"), "Eventkeeper");

  client.send(protocol.encode(20006, list_request, 5));
  const auto listed = protocol.decode(read_until_opcode(client, 9)).at("message");
  ASSERT_EQ(listed.at("rankings").size(), 1U);
  EXPECT_EQ(listed.at("my_ranking").at("core_info").at("char_id"), std::to_string(keeper.id));

  client.send(protocol.encode(20110, {{"map_id", kRaidMap}}, 6));
  EXPECT_EQ(protocol.decode(read_until_opcode(client, 211)).at("message").at("map_id"),
            kRaidMap);
  client.send(protocol.encode(20111, {{"map_id", kRaidMap}}, 7));
  EXPECT_EQ(protocol.decode(read_until_opcode(client, 212)).at("message").at("map_id"),
            kRaidMap);
  client.send(protocol.encode(20112, {{"map_id", kRaidMap}}, 8));
  EXPECT_EQ(protocol.decode(read_until_opcode(client, 2)).at("message").at("ec"),
            "Raid_RequesterNotFound");

  client.send(protocol.encode(20009, {{"text", "raid_channel_enter 10103"}}, 9));
  const auto matched = protocol.decode(read_until_opcode(client, 213)).at("message");
  EXPECT_EQ(matched.at("map_id"), kRaidMap);
  ASSERT_EQ(matched.at("members").size(), 1U);
  EXPECT_EQ(matched.at("members")[0].at("name"), "Eventkeeper");

  client.send(protocol.encode(20062, nlohmann::json::object(), 10));
  const auto tribute_count = protocol.decode(read_until_opcode(client, 118)).at("message");
  EXPECT_EQ(tribute_count.value("count", 0), 0);
  EXPECT_EQ(tribute_count.at("target_count"), channel_event::kTributeTarget);

  // GM requests are limited to four per second.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  client.send(protocol.encode(20009, {{"text", "complete_channel_event_objective"}}, 11));
  EXPECT_EQ(protocol.decode(read_until_opcode(client, 117)).at("message").at("status_type"),
            "EX5ChannelEventStatusType_Success");
  client.send(protocol.encode(20009, {{"text", "channel_event_info"}}, 12));
  const auto info = protocol.decode(read_until_opcode(client, 159)).at("message");
  EXPECT_EQ(info.at("completed"), true);

  client.send(protocol.encode(20086, nlohmann::json::object(), 13));
  read_until_opcode(client, 183, kMaximumMapBootstrapFrames);
  const nlohmann::json move{{"map_id", kRaidMap},
                            {"pos", {{"millimeter_x", origin.x_mm},
                                     {"millimeter_y", origin.y_mm},
                                     {"millimeter_z", origin.z_mm}}}};
  // The server also enforces the catalog's 10-second matched countdown.
  std::this_thread::sleep_for(std::chrono::seconds(10));
  client.send(protocol.encode(20091, move, 14));
  EXPECT_EQ(protocol.decode(read_until_opcode(client, 175)).at("message").at("map_id"),
            kRaidMap);
  client.send(protocol.encode(20085, nlohmann::json::object(), 15));
  read_until_opcode(client, 176);
  client.send(protocol.encode(20086, nlohmann::json::object(), 16));
  read_until_opcode(client, 183, kMaximumMapBootstrapFrames);
  // Even while in the raid, durable position remains the parent-map return point.
  EXPECT_EQ(repositories.characters->character_by_id(keeper.id)->world_position, origin);

  // A housing visit from a raid must return to its public parent, never to a released seat.
  auto raid_position = origin;
  raid_position.map_id = kRaidMap;
  const housing::HousingActor housing_actor{
      keeper.account_id, keeper.id, 1, keeper.name, raid_position};
  const auto visit = housing_service->visit(housing_actor, housing::testing_support::kZoneKey, 1);
  ASSERT_EQ(visit.ec, x5::ec::Success);
  ASSERT_TRUE(housing_hub->travel(keeper.id, *visit.destination));
  EXPECT_EQ(protocol.decode(read_until_opcode(client, 175)).at("message").at("map_id"),
            housing::testing_support::kHousingMapId);
  client.send(protocol.encode(20085, nlohmann::json::object(), 17));
  read_until_opcode(client, 176, kMaximumMapBootstrapFrames);
  client.send(protocol.encode(20086, nlohmann::json::object(), 18));
  read_until_opcode(client, 183, kMaximumMapBootstrapFrames);
  client.send(protocol.encode(20090, nlohmann::json::object(), 19));
  EXPECT_EQ(protocol.decode(read_until_opcode(client, 175)).at("message").at("map_id"),
            origin.map_id);
  client.send(protocol.encode(20085, nlohmann::json::object(), 20));
  read_until_opcode(client, 176);
  client.send(protocol.encode(20086, nlohmann::json::object(), 21));
  read_until_opcode(client, 183, kMaximumMapBootstrapFrames);

  client.send(protocol.encode(20081,
      {{"item_cms_id", 100194}, {"count", 3},
       {"target_unit_id", "21533402654056454"}}, 22));
  const auto items = protocol.decode(read_until_opcode(client, 169)).at("message");
  EXPECT_TRUE(items.contains("tribute"));
  EXPECT_EQ(items.at("stackables")[0].value("quantity", 0), 0);
  EXPECT_EQ(protocol.decode(read_until_opcode(client, 118)).at("message").at("count"), 3);
  EXPECT_EQ(repositories.character_state->load_world_progress(keeper.account_id, keeper.id, 1)
                .stackables.at(100194), 0);

  // Leaving before entry must acknowledge eviction too, so the map can be reused without
  // waiting for the previous channel's timeout.
  client.send(protocol.encode(20009, {{"text", "raid_channel_enter 10103"}}, 23));
  read_until_opcode(client, 213);
  client.send(protocol.encode(20112, {{"map_id", kRaidMap}}, 24));
  read_until_opcode(client, 214);
  client.send(protocol.encode(20009, {{"text", "raid_channel_enter 10103"}}, 25));
  read_until_opcode(client, 213);
  client.send(protocol.encode(20112, {{"map_id", kRaidMap}}, 26));
  read_until_opcode(client, 214);

  // Generic GM map travel must not bypass raid admission or persist a seatless raid position.
  client.send(protocol.encode(20009, {{"text", "map 10103"}}, 27));
  client.send(protocol.encode(20006, list_request, 28));
  read_until_opcode(client, 9);
  const auto connections = server.listener().connections();
  ASSERT_EQ(connections.size(), 1U);
  EXPECT_EQ(connections.front().position, origin);
  EXPECT_EQ(repositories.characters->character_by_id(keeper.id)->world_position, origin);

  // A burst of authenticated board queries must be bounded before it reaches storage.
  constexpr std::uint32_t kQueryBurst = 96;
  for (std::uint32_t index = 0; index < kQueryBurst; ++index) {
    client.send(protocol.encode(20006, list_request, 29 + index));
  }
  bool rate_limited = false;
  for (std::uint32_t index = 0; index < kQueryBurst; ++index) {
    const auto response = client.read();
    ASSERT_TRUE(response.opcode() == 9 || response.opcode() == 2);
    if (response.opcode() == 2) {
      const auto refused = protocol.decode(response).at("message");
      EXPECT_EQ(refused.at("ec"), "BadRequest");
      EXPECT_EQ(refused.at("packet_seq").at("seq"), std::to_string(29 + index));
      rate_limited = true;
    }
  }
  EXPECT_TRUE(rate_limited);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  client.send(protocol.encode(20006, list_request, 29 + kQueryBurst));
  EXPECT_EQ(protocol.decode(read_until_opcode(client, 9)).at("message").at("rankings").size(), 1U);
  client.disconnect();
}

} // namespace
} // namespace chronicles
