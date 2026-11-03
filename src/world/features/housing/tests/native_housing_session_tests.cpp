#include "testing/housing_test_support.hpp"
#include "world/features/housing/housing_world.hpp"
#include "world/features/processing/processing.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace chronicles {
namespace {

using namespace native_testing;

// Housing over real world sockets: a front-style contract runs on the owner's session strand,
// a visit moves the client into the housing area on its own connection, the area's sites are
// announced on arrival, the owner edits its site, a second character sees it, and leaving the
// area returns the owner to where it came from.
TEST(NativeWorld, HousingContractVisitEditAndExitOverTls) {
  using namespace housing;
  using namespace housing::testing_support;
  constexpr std::uint16_t kStartPlayingRequest = 20086;
  constexpr std::uint16_t kStartPlayingResponse = 183;
  constexpr std::uint16_t kMapMoveStart = 175;
  constexpr std::uint16_t kMapMoveComplete = 176;

  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  const auto owner = select_world_test_character(auth, "housing_owner_test", "HouseOwner", 1);
  const auto visitor = select_world_test_character(auth, "housing_visit_test", "HouseVisitor", 3);
  const auto owner_id = owner.character().char_id();
  const auto visitor_id = visitor.character().char_id();

  // Rent items before entry; the chair is granted after entry so the bootstrap sequence
  // (without WorldCharHousingItemsNoti) matches enter_shared_world.
  const auto owner_row = repositories.characters->character_by_id(owner_id);
  ASSERT_TRUE(owner_row.has_value());
  const auto before =
      repositories.character_state->load_world_progress(owner_row->account_id, owner_id, 1);
  auto after = before;
  after.revision = before.revision + 1;
  after.stackables[kPermitItem] = 60;
  ASSERT_TRUE(repositories.character_state->save_world_progress(owner_row->account_id, owner_id, 1,
                                                                before, after));

  auto service = std::make_shared<HousingService>(
      housing_catalog(), std::make_shared<MemoryHousingStore>(repositories.character_state), 1, [] {
        return std::int64_t{638'000'000'000'000'000};
      });
  auto hub = std::make_shared<HousingHub>(service);
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, {}, {}, {}, {}, hub);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient owner_client(server.port());
  enter_shared_world(owner_client, protocol, owner.packet_key());
  owner_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(read_until_opcode(owner_client, kStartPlayingResponse).opcode(),
            kStartPlayingResponse);
  {
    const auto entered =
        repositories.character_state->load_world_progress(owner_row->account_id, owner_id, 1);
    auto stocked = entered;
    stocked.revision = entered.revision + 1;
    stocked.housing_items[kChairItem] = {1, 0};
    ASSERT_TRUE(repositories.character_state->save_world_progress(owner_row->account_id, owner_id,
                                                                  1, entered, stocked));
  }

  // The contract runs where a front request would run it: on the owner's session strand.
  HousingActor actor{owner_row->account_id, owner_id, 1, "HouseOwner",
                     WorldPosition{8000, 100, 200, 300, 0.0F}};
  ContractResult contract;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(hub->run_for(owner_id, [&](const ProgressAccess *access) {
    ASSERT_NE(access, nullptr);
    contract = service->contract(actor, SiteType::small, kZoneKey, ContractType::monthly, access);
  }));
  ASSERT_EQ(contract.ec, x5::ec::Success);
  hub->publish(contract.outcome, owner_id);
  const auto own_site =
      protocol.decode(read_until_opcode(owner_client, kCharSiteUpdateNotification)).at("message");
  EXPECT_EQ(own_site.at("housing_site").at("lot_number"), kSmallLot);
  const auto charged =
      protocol.decode(read_until_opcode(owner_client, kItemsUpdateNotification)).at("message");
  // Protobuf JSON leaves the spent stack's zero quantity out.
  EXPECT_EQ(charged.at("stackables").at(0).at("item_cms_id"), kPermitItem);
  EXPECT_EQ(charged.at("stackables").at(0).value("quantity", 0), 0);

  // Visiting moves the client into map 20101 on this connection.
  const auto visit = service->visit(actor, kZoneKey, 1);
  ASSERT_EQ(visit.ec, x5::ec::Success);
  ASSERT_TRUE(hub->travel(owner_id, *visit.destination));
  const auto move =
      protocol.decode(read_until_opcode(owner_client, kMapMoveStart)).at("message");
  EXPECT_EQ(move.at("map_id"), kHousingMapId);
  EXPECT_FALSE(move.contains("game_address"));
  owner_client.send(protocol.encode(20085, nlohmann::json::object(), 3));
  const auto arrived =
      protocol.decode(read_until_opcode(owner_client, kMapMoveComplete)).at("message");
  EXPECT_EQ(arrived.at("map_id"), kHousingMapId);
  const auto house =
      protocol.decode(read_until_opcode(owner_client, kHousingAddNotification)).at("message");
  EXPECT_EQ(house.at("nub").at("lot_number"), kSmallLot);
  EXPECT_EQ(house.at("nub").at("owner_char_id"), std::to_string(owner_id));
  ASSERT_EQ(hub->location_of(owner_id), (HousingLocation{kZoneKey, 1}));

  // Edit mode and one storage object.
  owner_client.send(protocol.encode(kEditModeRequest, nlohmann::json::object(), 4));
  const auto edit =
      protocol.decode(read_until_opcode(owner_client, kEditModeNotification)).at("message");
  EXPECT_EQ(edit.value("ec", std::string("Success")), "Success");
  owner_client.send(protocol.encode(
      kUpdateHousingRequest,
      {{"housing_lot_number", kSmallLot},
       {"housing_doodad_nubs",
        nlohmann::json::array({{{"doodad_id", 9},
                                {"cms_id", kChairDoodad},
                                {"pos", {{"millimeter_x", 100}, {"millimeter_y", 100}}}}})}},
      5));
  const auto saved =
      protocol.decode(read_until_opcode(owner_client, kUpdateHousingResponse)).at("message");
  EXPECT_EQ(saved.at("lot_number"), kSmallLot);
  ASSERT_EQ(saved.at("housing_doodad_nubs").size(), 1u);
  EXPECT_EQ(saved.at("housing_doodad_nubs").at(0).at("cms_id"), kChairDoodad);
  EXPECT_EQ(repositories.character_state->load_world_progress(owner_row->account_id, owner_id, 1)
                .housing_items.at(kChairItem),
            (HousingItemRecord{1, 1}));

  // A second character visiting the same channel sees the decorated site.
  WorldTestClient visitor_client(server.port());
  enter_shared_world(visitor_client, protocol, visitor.packet_key());
  visitor_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(read_until_opcode(visitor_client, kStartPlayingResponse).opcode(),
            kStartPlayingResponse);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto visitor_row = repositories.characters->character_by_id(visitor_id);
  HousingActor visiting{visitor_row->account_id, visitor_id, 1, "HouseVisitor",
                        WorldPosition{8000, 1, 2, 3, 0.0F}};
  const auto second_visit = service->visit(visiting, kZoneKey, 1);
  ASSERT_EQ(second_visit.ec, x5::ec::Success);
  ASSERT_TRUE(hub->travel(visitor_id, *second_visit.destination));
  read_until_opcode(visitor_client, kMapMoveStart);
  visitor_client.send(protocol.encode(20085, nlohmann::json::object(), 3));
  const auto seen =
      protocol.decode(read_until_opcode(visitor_client, kHousingAddNotification)).at("message");
  EXPECT_EQ(seen.at("nub").at("housing_doodads").size(), 2u);

  // Crossing the area boundary returns the owner to its origin on map 8000.
  owner_client.send(protocol.encode(20090, nlohmann::json::object(), 6));
  const auto back =
      protocol.decode(read_until_opcode(owner_client, kMapMoveStart)).at("message");
  EXPECT_EQ(back.at("map_id"), 8000);
  owner_client.send(protocol.encode(20085, nlohmann::json::object(), 7));
  EXPECT_EQ(protocol.decode(read_until_opcode(owner_client, kMapMoveComplete))
                .at("message")
                .at("map_id"),
            8000);
  EXPECT_FALSE(hub->location_of(owner_id).has_value());
}

// A crafting station placed on a lot. Live 2026-11-02: the Kitchen (doodad 85002) saved on
// a lot answered "Unable to find target" because processing knew only map stations. The world
// session now resolves the housing doodad unit ID through the lot the character stands on.
// Opcodes: 20065 DialogueStartReq -> 161, 20069 craft -> 166, 20070 execute -> 169/147/167.
TEST(NativeWorld, CraftingStationPlacedOnAHousingLotOpensAndCraftsOverTls) {
  using namespace housing;
  using namespace housing::testing_support;
  constexpr std::uint16_t kStartPlayingRequest = 20086;
  constexpr std::uint16_t kStartPlayingResponse = 183;
  constexpr std::uint16_t kMapMoveStart = 175;
  constexpr std::uint16_t kMapMoveComplete = 176;
  constexpr std::uint16_t kDialogueStartRequest = 20065;
  constexpr std::uint16_t kDialogueStartResponse = 161;
  constexpr std::uint16_t kStartInteractionRequest = 20069;
  constexpr std::uint16_t kStartInteractionNotification = 166;
  constexpr std::uint16_t kExecuteInteractionRequest = 20070;
  constexpr std::uint16_t kCraftResultNotification = 147;
  constexpr int kKitchenInteraction = 603;
  constexpr int kKitchenAnimation = 60005;
  constexpr int kKitchenExecuteMs = 150;
  constexpr int kRecipe = 60204301;
  constexpr int kMaterial = 70102001;
  constexpr int kProduct = 40101101;
  constexpr std::uint32_t kMainBuildingHouse = 1;
  constexpr std::uint32_t kStationObject = 11;

  // The Kitchen as the exporter writes it, plus one unrelated map station (a profile needs one).
  const nlohmann::json profile{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"maximum_craft_amount", 999},
      {"maps", nlohmann::json::array({{{"map_id", 9012},
                                       {"area_min_mm", {-1000, -1000, -1000}},
                                       {"area_max_mm", {1000, 1000, 1000}}}})},
      {"animations", nlohmann::json::array({{{"interaction_anim_id", kKitchenAnimation},
                                             {"execute_time_ms", kKitchenExecuteMs},
                                             {"duration_ms", 1000}}})},
      {"interactions", nlohmann::json::array({{{"interaction_id", kKitchenInteraction},
                                               {"group_id", kKitchenInteraction},
                                               {"interaction_anim_id", kKitchenAnimation}}})},
      {"stations", nlohmann::json::array({{{"doodad_cms_id", kStationDoodad},
                                           {"interaction_group_id", kKitchenInteraction},
                                           {"craft_ids", {kRecipe}}}})},
      {"crafts", nlohmann::json::array({{{"item_craft_id", kRecipe},
                                         {"product_item_cms_id", kProduct},
                                         {"product_count", 1},
                                         {"product_storage", "stackable"},
                                         {"gold_cost", 0},
                                         {"materials", nlohmann::json::array(
                                                           {{{"item_cms_id", kMaterial},
                                                             {"count", 2}}})}}})},
      {"unsupported_crafts", nlohmann::json::array()},
      {"placements", nlohmann::json::array({{"20477965980667910", kStationDoodad, 0, 0, 0}})}};

  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  const auto owner = select_world_test_character(auth, "housing_kitchen_test", "Cook", 1);
  const auto owner_id = owner.character().char_id();
  const auto owner_row = repositories.characters->character_by_id(owner_id);
  ASSERT_TRUE(owner_row.has_value());
  const auto account_id = owner_row->account_id;
  {
    const auto before = repositories.character_state->load_world_progress(account_id, owner_id, 1);
    auto after = before;
    after.revision = before.revision + 1;
    after.stackables[kPermitItem] = 60;
    ASSERT_TRUE(repositories.character_state->save_world_progress(account_id, owner_id, 1, before,
                                                                  after));
  }

  auto service = std::make_shared<HousingService>(
      housing_catalog(), std::make_shared<MemoryHousingStore>(repositories.character_state), 1, [] {
        return std::int64_t{638'000'000'000'000'000};
      });
  auto hub = std::make_shared<HousingHub>(service);
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, {}, {}, {}, {}, hub, {}, {},
                              std::make_shared<const ProcessingData>(profile));
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient client(server.port());
  enter_shared_world(client, protocol, owner.packet_key());
  client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(read_until_opcode(client, kStartPlayingResponse).opcode(), kStartPlayingResponse);
  {
    // Stocked after entry so the bootstrap sequence matches enter_shared_world.
    const auto entered = repositories.character_state->load_world_progress(account_id, owner_id, 1);
    auto stocked = entered;
    stocked.revision = entered.revision + 1;
    stocked.housing_items[kFacilityItem] = {1, 0};
    stocked.stackables[kMaterial] = 5;
    ASSERT_TRUE(repositories.character_state->save_world_progress(account_id, owner_id, 1, entered,
                                                                  stocked));
  }

  HousingActor actor{account_id, owner_id, 1, "Cook", WorldPosition{8000, 100, 200, 300, 0.0F}};
  ContractResult contract;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(hub->run_for(owner_id, [&](const ProgressAccess *access) {
    ASSERT_NE(access, nullptr);
    contract = service->contract(actor, SiteType::small, kZoneKey, ContractType::monthly, access);
  }));
  ASSERT_EQ(contract.ec, x5::ec::Success);
  hub->publish(contract.outcome, owner_id);

  const auto visit = service->visit(actor, kZoneKey, 1);
  ASSERT_EQ(visit.ec, x5::ec::Success);
  const auto spawn = visit.destination->spawn;
  ASSERT_TRUE(hub->travel(owner_id, *visit.destination));
  read_until_opcode(client, kMapMoveStart);
  client.send(protocol.encode(20085, nlohmann::json::object(), 3));
  read_until_opcode(client, kMapMoveComplete);
  read_until_opcode(client, kHousingAddNotification);

  // The stock client saves the Kitchen as doodad 85002 attached to the preset main building.
  client.send(protocol.encode(kEditModeRequest, nlohmann::json::object(), 4));
  read_until_opcode(client, kEditModeNotification);
  client.send(protocol.encode(
      kUpdateHousingRequest,
      {{"housing_lot_number", kSmallLot},
       {"housing_doodad_nubs",
        nlohmann::json::array({{{"doodad_id", kStationObject},
                                {"house_id", kMainBuildingHouse},
                                {"cms_id", kStationDoodad},
                                {"pos", {{"millimeter_x", 300}, {"millimeter_y", 400}}}}})}},
      5));
  const auto saved =
      protocol.decode(read_until_opcode(client, kUpdateHousingResponse)).at("message");
  ASSERT_EQ(saved.value("ec", std::string("Success")), "Success");

  const auto station_unit = std::to_string(
      housing_doodad_unit_id(kSmallLot, kMainBuildingHouse, kStationObject));
  const nlohmann::json request_position{{"millimeter_x", spawn.x_mm},
                                        {"millimeter_y", spawn.y_mm},
                                        {"millimeter_z", spawn.z_mm}};
  client.send(protocol.encode(kDialogueStartRequest,
                              {{"target_unit_id", station_unit},
                               {"pos", request_position},
                               {"yaw", spawn.yaw}},
                              6));
  const auto opened =
      protocol.decode(read_until_opcode(client, kDialogueStartResponse)).at("message");
  EXPECT_FALSE(opened.contains("ec")) << opened.dump();
  EXPECT_EQ(opened.at("target_unit_id"), station_unit);

  client.send(protocol.encode(
      kStartInteractionRequest,
      {{"interaction_anim_id", kKitchenAnimation},
       {"target_unit_id", station_unit},
       {"pos", request_position},
       {"yaw", spawn.yaw},
       {"param",
        {{"interaction",
          {{"interaction_id", kKitchenInteraction},
           {"execute_craft", {{"item_craft_id", kRecipe}, {"count", 1}}}}}}}},
      7));
  const auto started =
      protocol.decode(read_until_opcode(client, kStartInteractionNotification)).at("message");
  EXPECT_EQ(started.at("interaction_anim_id"), kKitchenAnimation);

  std::this_thread::sleep_for(std::chrono::milliseconds(kKitchenExecuteMs + 200));
  client.send(protocol.encode(kExecuteInteractionRequest,
                              {{"interaction_anim_id", kKitchenAnimation}}, 8));
  const auto crafted =
      protocol.decode(read_until_opcode(client, kCraftResultNotification)).at("message");
  EXPECT_EQ(crafted.at("item_craft_id"), kRecipe);

  const auto progress = repositories.character_state->load_world_progress(account_id, owner_id, 1);
  EXPECT_EQ(progress.stackables.at(kMaterial), 3);
  EXPECT_EQ(progress.stackables.at(kProduct), 1);
  client.disconnect();
}

} // namespace
} // namespace chronicles
