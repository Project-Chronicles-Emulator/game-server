#include "world/features/shop/tests/native_shop_test_support.hpp"

#include "world/features/character/character_stats.hpp"
#include "world/features/npc/map_static_npcs.hpp"

#include <future>

namespace chronicles {
namespace {

using namespace native_testing;

TEST(NativeWorld, ExportedHarborMerchantSellsOwnedItemsAndPersistsGoldOverTls) {
  const auto *catalog_root = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_root) {
    GTEST_SKIP() << "Needs the verified catalog and regenerated static NPC index";
  }
  // Authored sector/local identity of the scheduled Harbor City general merchant.
  constexpr std::uint64_t kMerchant = 21252082296168455ULL;
  constexpr int kSoldMaterial = 70100005;
  const auto targets = MapStaticNpcs::load(catalog_root);
  ASSERT_TRUE(targets);
  const auto *merchant = targets->find(kOverworldMapId, kMerchant);
  ASSERT_NE(merchant, nullptr);
  EXPECT_EQ(merchant->npc_template_id, 11001007);

  const WorldPosition position{kOverworldMapId, merchant->position.x_mm, merchant->position.y_mm,
                               merchant->position.z_mm, 0.0F};
  auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  OverworldSessions world(small_overworld_population(), {position}, catalog, catalog_root);
  auto &session = world.sessions.front();
  auto before = world.repositories.character_state->load_world_progress(session.account_id,
                                                                        session.character_id, 1);
  auto stocked = before;
  stocked.gold = 100;
  stocked.stackables[kSoldMaterial] = 5;
  ++stocked.revision;
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      session.account_id, session.character_id, 1, before, stocked));
  world.enter(session);

  const auto shop_request = [](const nlohmann::json &shop) {
    return nlohmann::json{{"target_unit_id", std::to_string(kMerchant)},
                          {"param", {{"interaction_id", 212}, {"shop", shop}}}};
  };
  session.client->send(world.protocol.encode(20068, shop_request(nlohmann::json::object()), 3));
  const auto opened = world.read_until_pong(session, 4);
  EXPECT_TRUE(
      std::ranges::any_of(opened, [](const Frame &frame) { return frame.opcode() == 163; }));
  EXPECT_FALSE(std::ranges::any_of(opened, [](const Frame &frame) { return frame.opcode() == 2; }));

  const auto sale = shop_request(sell_list(kSoldMaterial, 5));
  session.client->send(world.protocol.encode(20068, sale, 5));
  const auto sold = world.read_until_pong(session, 6);
  ASSERT_EQ(sold.size(), 3);
  EXPECT_EQ(sold[0].opcode(), 169);
  EXPECT_EQ(sold[1].opcode(), 163);
  const auto after = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  const auto material = after.stackables.find(kSoldMaterial);
  EXPECT_TRUE(material == after.stackables.end() || material->second == 0);
  EXPECT_EQ(after.gold, 105); // Verified catalog refund: one gold per material.

  session.client->send(world.protocol.encode(20068, sale, 7));
  const auto exhausted = world.read_until_pong(session, 8);
  ASSERT_EQ(exhausted.size(), 2);
  EXPECT_EQ(world.protocol.decode(exhausted[0]).at("message").at("ec"), "ItemCountError");
  EXPECT_EQ(world.repositories.character_state->load_world_progress(session.account_id,
                                                                    session.character_id, 1),
            after);
  session.client->disconnect();
}

TEST(NativeWorld, TutorialMerchantServicesPersistAtomicallyAndKeepSessionRevision) {
  constexpr int kShopNpc = 1001007;
  constexpr int kBlacksmithNpc = 1001096;
  constexpr int kVoucherNpc = 1001425;
  constexpr int kShopInteraction = 212;
  constexpr int kRepairInteraction = 65;
  constexpr int kVoucherInteraction = 30;
  constexpr int kPotion = 15010403;
  constexpr int kPickaxe = 11200201;
  constexpr int kLeather = 72000016;
  constexpr int kTokenPotion = 40101105;
  constexpr int kChair = 1900000001;
  // Tutorial NPC unit IDs use namespace 1 and the low tag 2, as generated profiles do.
  const auto unit_id = [](std::uint64_t number) {
    return std::to_string(0x1000000000000002ULL + (number * 4096));
  };
  const std::string near_merchant = unit_id(1);
  const std::string far_merchant = unit_id(2);
  const std::string blacksmith = unit_id(3);
  const std::string voucher_npc = unit_id(4);
  // The diagnostic tutorial spawn is (-795146, 712194, -698778); every NPC but one is 2 m away.
  const auto civilian = [](const std::string &id, int npc, int x_mm) {
    return nlohmann::json{{"unit_id", id},
                          {"cms_id", npc},
                          {"position", {x_mm, 712194, -698778}},
                          {"yaw", 0.0}};
  };
  const nlohmann::json tutorial_profile = {
      {"format_version", 1},
      {"map_id", 8000},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"objects", nlohmann::json::array()},
      {"quests", nlohmann::json::array()},
      {"doodads", nlohmann::json::array()},
      {"sectors", nlohmann::json::array()},
      {"utility_items", {kPotion}},
      {"civilians",
       {civilian(near_merchant, kShopNpc, -793146), civilian(far_merchant, kShopNpc, -695146),
        civilian(blacksmith, kBlacksmithNpc, -793146),
        civilian(voucher_npc, kVoucherNpc, -793146)}}};
  const auto tutorial = std::make_shared<const TutorialData>(tutorial_profile);

  const auto item = [](int id, const char *category, const char *price_type, const char *price,
                       const char *refund) {
    return nlohmann::json{{"id", std::to_string(id)},
                          {"category", category},
                          {"priceType", price_type},
                          {"priceValue", price},
                          {"refundType", refund[0] != '\0' ? "Gold" : ""},
                          {"refundValue", refund}};
  };
  auto pickaxe = item(kPickaxe, "Equipment", "Gold", "400", "80");
  pickaxe["grade"] = "Normal";
  pickaxe["maxDurability"] = "2000";
  pickaxe["npcRepairable"] = "True";
  pickaxe["equipmentType"] = "Pick";
  pickaxe["equipSlot"] = "Tool_3";

  TemporaryWorldCatalog catalog;
  catalog.write_tables({
      {"Item",
       {item(kPotion, "Consumable", "Gold", "200", "40"), pickaxe,
        item(kLeather, "CraftMaterial", "Gold", "10", "1"),
        item(kTokenPotion, "Consumable", "HersanderToken", "5", ""),
        item(kChair, "HousingObject", "Gold", "100", "20")}},
      {"Shop",
       {{{"id", "1001"}, {"FK_itemId", std::to_string(kPotion)}},
        {{"id", "1001"}, {"FK_itemId", std::to_string(kPickaxe)}},
        {{"id", "1001"}, {"FK_itemId", std::to_string(kTokenPotion)}},
        {{"id", "1001"}, {"FK_itemId", std::to_string(kChair)}}}},
      {"Interaction",
       {{{"id", std::to_string(kShopInteraction)},
         {"groupId", std::to_string(kShopInteraction)},
         {"playerTaskCase", "Shop"},
         {"playerTask", "\"shopId\":1001"}},
        {{"id", std::to_string(kRepairInteraction)},
         {"groupId", "64"},
         {"playerTaskCase", "Repair"},
         {"playerTask", ""}},
        {{"id", std::to_string(kVoucherInteraction)},
         {"groupId", std::to_string(kVoucherInteraction)},
         {"playerTaskCase", "VoucherGroup"},
         {"playerTask", "\"voucherGroupId\":2"}}}},
      {"Npc",
       {{{"id", std::to_string(kShopNpc)}, {"interactionGroupId", "212"}},
        {{"id", std::to_string(kBlacksmithNpc)}, {"interactionGroupId", "64"}},
        {{"id", std::to_string(kVoucherNpc)}, {"interactionGroupId", "30"}}}},
      {"ItemRepairCost",
       {{{"category", "Equipment"},
         {"equipmentType", "Pick"},
         {"grade", "Normal"},
         {"equipType", "Tool_3"},
         {"costType", "Gold"},
         {"costValue", "10"}}}},
      {"VoucherGroup",
       {{{"VoucherGroupId", "2"}, {"FK_tradeGoodId", "100"}, {"FK_VoucherConditionId", ""}}}},
      {"TradeGood",
       {{{"id", "100"}, {"basicValue", "100"}, {"validPeriod", "100"}, {"company", "YokedIron"}}}},
  });

  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("shop_owner", "test-world-password-123"));
  const auto login = auth.login("shop_owner", "test-world-password-123");
  x5::pb::authwebsocket::WorldEnterReq enter_request;
  enter_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), enter_request).error_code(), x5::ec::Success);

  Character character;
  character.account_id = login.account_id();
  character.name = "ShopOwner";
  character = repositories.characters->create(character);
  const auto worn_pickaxe = repositories.character_state->grant_item(
      character.account_id, character.id, 1,
      CharacterItem{0, kPickaxe, 50, std::nullopt, 1, 1500, 0});
  ASSERT_TRUE(worn_pickaxe);
  const auto initial =
      repositories.character_state->load_world_progress(character.account_id, character.id, 1);
  auto stocked = initial;
  stocked.revision = initial.revision + 1;
  stocked.gold = 1000;
  stocked.hersander_token = 20;
  stocked.stackables[kLeather] = 4;
  ASSERT_TRUE(repositories.character_state->save_world_progress(character.account_id, character.id,
                                                                1, initial, stocked));

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(character.id);
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, tutorial, {}, catalog.open(), catalog.root());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  ASSERT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  read_until_opcode(client, 140);
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  EXPECT_EQ(client.read().opcode(), 176);
  expect_pre_playing_combat_bootstrap(client, protocol);
  for (int announced = 0; announced < 4; ++announced) {
    EXPECT_EQ(client.read().opcode(), 10);
  }

  std::uint32_t sequence = 2;
  const auto request = [&](const std::string &unit, const nlohmann::json &param) {
    client.send(protocol.encode(20068, {{"target_unit_id", unit}, {"param", param}}, sequence++));
  };
  const auto shop_request = [&](const std::string &unit, int interaction_id,
                                const nlohmann::json &shop) {
    request(unit, {{"interaction_id", interaction_id}, {"shop", shop}});
  };
  int reply_step = 0;
  const auto read_message = [&](std::uint16_t opcode) {
    const auto frame = client.read();
    const auto decoded = protocol.decode(frame);
    EXPECT_EQ(frame.opcode(), opcode) << "reply " << reply_step << ": " << decoded.dump();
    ++reply_step;
    return decoded.at("message");
  };
  const auto buy_one = [&](int cms_id, int count) {
    shop_request(near_merchant, kShopInteraction,
                 {{"buy_list", {{{"item_cms_id", cms_id}, {"count", count}}}}});
  };

  shop_request(near_merchant, kShopInteraction, nlohmann::json::object());
  EXPECT_FALSE(read_message(163).contains("ec"));
  EXPECT_EQ(read_message(171).at("shop_id"), 1001);

  buy_one(kPotion, 2);
  const auto potions = read_message(169);
  EXPECT_TRUE(potions.contains("shop"));
  EXPECT_EQ(potions.at("money_changes").at(0).at("change"), "-400");
  EXPECT_EQ(potions.at("stackables").at(0).at("quantity"), 2);
  EXPECT_EQ(read_message(163).at("interaction_id"), kShopInteraction);

  buy_one(kPickaxe, 1);
  const auto pickaxe_update = read_message(169);
  ASSERT_EQ(pickaxe_update.at("items").size(), 1U);
  EXPECT_EQ(pickaxe_update.at("items").at(0).at("durability"), 2000);
  EXPECT_TRUE(pickaxe_update.at("items").at(0).contains("created_epochtime"));
  const auto pickaxe_id = pickaxe_update.at("items").at(0).at("item_id").get<std::string>();
  read_message(163);

  shop_request(near_merchant, kShopInteraction,
               {{"sell_list", {{{"stackable", {{"item_cms_id", kLeather}, {"count", 4}}}}}}});
  EXPECT_EQ(read_message(169).at("money_changes").at(0).at("result"), "204");
  read_message(163);

  shop_request(near_merchant, kShopInteraction, {{"sell_list", {{{"item_id", pickaxe_id}}}}});
  EXPECT_EQ(read_message(169).at("money_changes").at(0).at("result"), "284");
  read_message(163);

  buy_one(kTokenPotion, 2);
  const auto token_update = read_message(169);
  EXPECT_EQ(token_update.at("money_changes").at(0).at("money_type"),
            "EX5MoneyType_HersanderToken");
  EXPECT_EQ(token_update.at("money_changes").at(0).at("result"), "10");
  read_message(163);

  buy_one(kChair, 1);
  const auto chair_update = read_message(169);
  EXPECT_EQ(chair_update.at("housing_items").at(0).at("item_cms_id"), kChair);
  EXPECT_EQ(chair_update.at("housing_items").at(0).at("quantity"), 1);
  read_message(163);

  // 10 gold per 2000 durability x 500 missing = 2.5, rounded up to 3 gold.
  request(blacksmith, {{"interaction_id", kRepairInteraction},
                       {"repair", {{"item_ids", {std::to_string(worn_pickaxe->id)}}}}});
  const auto repaired = read_message(169);
  EXPECT_EQ(repaired.at("items").at(0).at("durability"), 2000);
  EXPECT_EQ(repaired.at("money_changes").at(0).at("change"), "-3");
  read_message(163);

  // Eight merchant requests share one rate window. The ninth is refused atomically;
  // a later request in the next window can still complete the same valid voucher trade.
  const auto before_throttle =
      repositories.character_state->load_world_progress(character.account_id, character.id, 1);
  request(voucher_npc, {{"interaction_id", kVoucherInteraction},
                        {"voucher_group", {{"good_cms_id", 100}, {"count", 1}}}});
  EXPECT_EQ(read_message(2).at("ec"), "BadRequest");
  EXPECT_EQ(
      repositories.character_state->load_world_progress(character.account_id, character.id, 1),
      before_throttle);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  request(voucher_npc, {{"interaction_id", kVoucherInteraction},
                        {"voucher_group", {{"good_cms_id", 100}, {"count", 1}}}});
  const auto voucher_update = read_message(169);
  EXPECT_EQ(voucher_update.at("money_changes").at(0).at("change"), "-100");
  EXPECT_EQ(voucher_update.at("trade_voucher").at("vouchers").at(0).at("good_cms_id"), 100);
  read_message(163);

  // Rejections use the common error channel and change nothing.
  shop_request(far_merchant, kShopInteraction, nlohmann::json::object());
  EXPECT_EQ(read_message(2).at("ec"), "TooFar");
  shop_request(near_merchant, 213, nlohmann::json::object());
  EXPECT_EQ(read_message(2).at("ec"), "Interaction_GroupIdNotMatch");
  request(blacksmith, {{"interaction_id", kRepairInteraction},
                       {"repair", {{"item_ids", {std::to_string(worn_pickaxe->id)}}}}});
  EXPECT_EQ(read_message(2).at("ec"), "ItemNotRepairable");
  buy_one(kPotion, 5);
  EXPECT_EQ(read_message(2).at("ec"), "MoneyNotEnough");
  buy_one(kTokenPotion, 3);
  EXPECT_EQ(read_message(2).at("ec"), "MoneyNotEnough");

  // The tutorial session adopted the shop revision, so its next commit still succeeds.
  client.send(protocol.encode(20080, {{"slots", {{{"item_cms_id", kPotion}, {"count", 2}}}}},
                              sequence++));
  EXPECT_EQ(client.read().opcode(), 104);

  const auto progress =
      repositories.character_state->load_world_progress(character.account_id, character.id, 1);
  EXPECT_EQ(progress.gold, 81);
  EXPECT_EQ(progress.hersander_token, 10);
  EXPECT_EQ(progress.stackables.at(kPotion), 2);
  EXPECT_EQ(progress.stackables.at(kLeather), 0);
  EXPECT_EQ(progress.housing_items.at(kChair).quantity, 1);
  EXPECT_EQ(progress.trade_vouchers.size(), 1U);
  EXPECT_EQ(progress.utility_slots.at(0).count, 2);
  const auto persisted = repositories.characters->characters(character.account_id, 1).front().items;
  EXPECT_FALSE(std::ranges::any_of(persisted, [&](const CharacterItem &owned) {
    return std::to_string(owned.id) == pickaxe_id;
  }));
  EXPECT_TRUE(std::ranges::any_of(persisted, [&](const CharacterItem &owned) {
    return owned.id == worn_pickaxe->id && owned.durability == 2000;
  }));
}

TEST(NativeWorld, ShopRequestsAreAnsweredWhenTheShopCatalogIsDisabled) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("shop_absent", "test-world-password-123"));
  const auto login = auth.login("shop_absent", "test-world-password-123");
  x5::pb::authwebsocket::WorldEnterReq enter_request;
  enter_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), enter_request).error_code(), x5::ec::Success);
  Character character;
  character.account_id = login.account_id();
  character.name = "ShopAbsent";
  character = repositories.characters->create(character);
  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(character.id);
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  // Without the shop tables the catalog is disabled; the request is still answered.
  TemporaryWorldCatalog catalog;
  catalog.write_items({});
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  ASSERT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  read_until_opcode(client, 126);

  const nlohmann::json request{
      {"target_unit_id", "777001"},
      {"param", {{"interaction_id", 116}, {"shop", {{"buy_list", {{{"item_cms_id", 50}}}}}}}}};
  client.send(protocol.encode(20068, request, 1));
  const auto response = protocol.decode(client.read());
  EXPECT_EQ(response.at("opcode"), 2);
  EXPECT_EQ(response.at("message").at("ec"), "Interaction_Invalid");
}

TEST(NativeWorld, ShopOpenIsRefusedForAnUnannouncedTarget) {
  MerchantWorld world(100, {});
  const auto before = world.progress();
  world.expect_refusal(nlohmann::json::object(), x5::ec::Interaction_TargetNotFound);
  EXPECT_EQ(world.progress(), before);
}

TEST(NativeWorld, ShopBuyIsRefusedWithoutAuthoritativeTargetAndChangesNothing) {
  MerchantWorld world(1000, {});
  const auto before = world.progress();
  world.expect_refusal(buy_list(70109003, 2), x5::ec::Interaction_TargetNotFound);
  world.expect_refusal(buy_list(133, 1), x5::ec::Interaction_TargetNotFound);
  EXPECT_EQ(world.progress(), before);
}

TEST(NativeWorld, ShopSellIsRefusedWithoutAuthoritativeTargetAndChangesNothing) {
  MerchantWorld world(10, {{72000016, 4}});
  const auto before = world.progress();
  world.expect_refusal(sell_list(72000016, 4), x5::ec::Interaction_TargetNotFound);
  world.expect_refusal(sell_list(72000016, 99), x5::ec::Interaction_TargetNotFound);
  EXPECT_EQ(world.progress(), before);
}

TEST(NativeWorld, PostgresShopBuyAndSellRefusalLeavesPersistedStateUnchanged) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  // Character names are limited to 16 alphanumerics, so keep the unique suffix short.
  const auto suffix =
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
  MerchantWorld world(1000, {{72000016, 4}},
                      make_postgres_character_repositories(connection), suffix);
  auto reader = make_postgres_character_repositories(connection);
  const auto load = [&] {
    return reader.character_state->load_world_progress(world.account_id, world.character_id, 1);
  };
  const auto persisted = load();
  ASSERT_EQ(persisted.gold, 1000);
  ASSERT_EQ(persisted.stackables.at(72000016), 4);

  world.expect_refusal(buy_list(70109003, 2), x5::ec::Interaction_TargetNotFound);
  EXPECT_EQ(load(), persisted);
  world.expect_refusal(sell_list(72000016, 4), x5::ec::Interaction_TargetNotFound);
  EXPECT_EQ(load(), persisted);
}

TEST(NativeWorld, ShopBuyAndSellRefuseEveryTargetAndInteractionWithoutChange) {
  MerchantWorld world(1000, {{72000016, 4}});
  // target_unit_id is a uint64 on the wire, so the test protocol can only encode numeric strings;
  // malformed text is rejected by the encoder before it reaches the server.
  for (const std::string target : {"1001", "999999", "0", "18446744073709551615"}) {
    for (const int interaction_id : {212, 213, 999}) {
      expect_buy_and_sell_refused(world, target, interaction_id);
    }
  }
}

TEST(NativeWorld, ShopOpenIsRefusedAndNeverBecomesPermissionForALaterBuyOrSell) {
  MerchantWorld world(1000, {{72000016, 4}});
  const auto before = world.progress();

  world.expect_refusal(nlohmann::json::object(), x5::ec::Interaction_TargetNotFound);
  EXPECT_EQ(world.progress(), before);

  expect_buy_and_sell_refused(world, "1001", 212);
  EXPECT_EQ(world.progress(), before);
}

TEST(NativeWorld, ShopEquipmentBuyAndSellByItemIdAreRefusedWithoutAnAnnouncedMerchant) {
  MerchantWorld world(1000, {});
  const auto before = world.progress();
  world.expect_refusal(buy_list(11200201, 1), x5::ec::Interaction_TargetNotFound);
  world.expect_refusal(
      {{"sell_list", nlohmann::json::array({{{"item_id", "1"}}})}}, x5::ec::Interaction_TargetNotFound);
  EXPECT_EQ(world.progress(), before);
}

TEST(NativeWorld, ShopPurchaseCommitIsRejectedUnderStaleRevisionWithoutChange) {
  // The handler loads the revision immediately before committing, so a stale revision cannot
  // be staged over the socket; this exercises the exact save_world_progress call it makes.
  MerchantWorld world(1000, {});
  const auto stale_before = world.progress();
  world.set_progress(900, {});

  auto purchase = stale_before;
  purchase.revision = stale_before.revision + 1;
  purchase.gold = stale_before.gold - 500;
  purchase.stackables[133] = 1;
  EXPECT_FALSE(world.repositories.character_state->save_world_progress(
      world.account_id, world.character_id, 1, stale_before, purchase));
  const auto current = world.progress();
  EXPECT_EQ(current.gold, 900);
  EXPECT_FALSE(current.stackables.contains(133));
}

TEST(NativeWorld, ShopOpenRefusesEveryForgedTargetAndInteractionWithoutAnyShopFrame) {
  MerchantWorld world(1000, {{72000016, 4}});
  const auto before = world.progress();
  for (const std::string target : {"1001", "999999", "0", "18446744073709551615"}) {
    for (const int interaction_id : {212, 213, 999}) {
      SCOPED_TRACE("target " + target + " interaction " + std::to_string(interaction_id));
      send_shop_to_target(world, target, interaction_id, nlohmann::json::object());
      EXPECT_EQ(world.read_message(2).at("ec"),
                x5::ec::EErrorCode_Name(forged_target_error(target)));
      EXPECT_EQ(world.progress(), before);
    }
  }

  // The session is still in step after the refusals: the next reply belongs to the ping.
  world.client->send(world.protocol->encode(20003, nlohmann::json::object(), world.sequence++));
  EXPECT_EQ(world.client->read().opcode(), 4);
  EXPECT_EQ(world.progress(), before);
}

TEST(NativeWorld, ShopTrafficIsRefusedInAnyOrderForTheSameForgedTarget) {
  MerchantWorld world(1000, {{72000016, 4}});
  const auto before = world.progress();
  const std::string target = "1001";
  for (const int round : {0, 1}) {
    SCOPED_TRACE("round " + std::to_string(round));
    if (round == 0) {
      expect_buy_and_sell_refused(world, target, 212);
      send_shop_to_target(world, target, 212, nlohmann::json::object());
      EXPECT_EQ(world.read_message(2).at("ec"),
                x5::ec::EErrorCode_Name(x5::ec::Interaction_TargetNotFound));
    } else {
      send_shop_to_target(world, target, 212, nlohmann::json::object());
      EXPECT_EQ(world.read_message(2).at("ec"),
                x5::ec::EErrorCode_Name(x5::ec::Interaction_TargetNotFound));
      expect_buy_and_sell_refused(world, target, 212);
    }
    EXPECT_EQ(world.progress(), before);
  }
}

TEST(NativeWorld, TutorialSessionRejectsForgedShopTargetsWithoutChangingProgress) {
  const auto *profile_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the generated local tutorial profile";
  }
  const auto definitions = TutorialData::load(profile_path);
  const auto catalog_root = std::filesystem::path(profile_path).parent_path().parent_path() /
                            "catalog";
  auto catalog = std::make_shared<ClientCatalog>(catalog_root);
  const auto stats = CharacterStats::load(*catalog);
  const auto &point = definitions->document.at("objects").at(0).at("position");
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  ASSERT_TRUE(auth.create_account("tutorial_shop_bypass", "tutorial-world-fixture-password"));
  const auto login = auth.login("tutorial_shop_bypass", "tutorial-world-fixture-password");
  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  auth.enter(login.access_token(), world_request);
  Character character;
  character.account_id = login.account_id();
  character.name = "TutorialShopGate";
  character.world_position = WorldPosition{8000, point.at(0), point.at(1), point.at(2), 0};
  character = repositories.characters->create(character);
  repositories.characters->save_world_position(character.account_id, character.id, 1,
                                               *character.world_position);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(character.id);
  const auto selected = auth.select(login.access_token(), select);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, definitions, stats, catalog, catalog_root);
  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  EXPECT_EQ(client.read().opcode(), 125);
  read_entry_inventory(client);
  EXPECT_EQ(client.read().opcode(), 135);
  for (int index = 0; index < 5; ++index) {
    EXPECT_EQ(protocol.decode(client.read()).at("status"), "decoded");
  }
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  EXPECT_EQ(client.read().opcode(), 176);
  expect_pre_playing_combat_bootstrap(client, protocol);
  const auto active_npcs = std::ranges::count_if(definitions->npcs, [](const auto &entry) {
    return entry.second.value("initially_active", true);
  });
  const auto notifications = definitions->document.at("sectors").size() + active_npcs;
  for (std::size_t index = 0; index < notifications; ++index) {
    EXPECT_EQ(protocol.decode(client.read()).at("status"), "decoded");
  }

  const auto before =
      repositories.character_state->load_world_progress(character.account_id, character.id, 1);
  std::uint32_t sequence = 2;
  for (const auto &shop : {nlohmann::json::object(), buy_list(133, 1), sell_list(72000016, 4)}) {
    client.send(protocol.encode(
        20068,
        {{"target_unit_id", "999999"}, {"param", {{"interaction_id", 212}, {"shop", shop}}}},
        sequence++));
    const auto reply = client.read();
    ASSERT_EQ(reply.opcode(), 2);
    EXPECT_EQ(protocol.decode(reply).at("message").at("ec"), "Interaction_TargetNotFound");
    EXPECT_EQ(
        repositories.character_state->load_world_progress(character.account_id, character.id, 1),
        before);
  }
  client.send(protocol.encode(20003, nlohmann::json::object(), sequence++));
  EXPECT_EQ(client.read().opcode(), 4);
}

} // namespace
} // namespace chronicles
