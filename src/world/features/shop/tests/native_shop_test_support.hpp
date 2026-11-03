#pragma once

// Shop-specific scenarios reuse the single native TLS/server/catalog fixture. Retained
// session-isolation tests use these same merchant helpers instead of duplicating them.
#include "world/session/tests/native_world_test_support.hpp"

namespace chronicles::native_testing {

// Authorization fixtures avoid the separate eight-requests-per-second budget path.
struct MerchantRequestPacer {
  unsigned requests{};
  void wait() {
    if (requests == 8) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1100));
      requests = 0;
    }
    ++requests;
  }
};

class MerchantWorld {
public:
  // `backing_repositories` defaults to the in-memory tables. A PostgreSQL-backed run passes
  // its own repositories and a unique `name_suffix`, because that database outlives the test.
  MerchantWorld(std::int64_t gold, const std::map<int, int> &stackables,
                CharacterRepositories backing_repositories =
                    make_memory_character_repositories(std::make_shared<MemoryDatabase>()),
                const std::string &name_suffix = "")
      : repositories(std::move(backing_repositories)) {
    config.game_address = "127.0.0.1:8081";
    auth = std::make_unique<AuthService>(repositories, config);
    auth->acquire_monster_reward_owner_epoch();
    const std::string account_name = "merchant_test" + name_suffix;
    auth->create_account(account_name, "test-world-password-123");
    const auto login = auth->login(account_name, "test-world-password-123");

    x5::pb::authwebsocket::WorldEnterReq world_request;
    world_request.set_world_id(1);
    if (auth->enter(login.access_token(), world_request).error_code() != x5::ec::Success) {
      throw std::runtime_error("Merchant fixture cannot enter the world");
    }
    x5::pb::ws::front::CharCreateReq create_request;
    create_request.set_slot(0);
    create_request.set_char_name("Merchant" + name_suffix);
    create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
    const auto created = auth->create(login.access_token(), create_request);
    if (created.ec() != x5::ec::Success) {
      throw std::runtime_error("Merchant fixture cannot create a character");
    }
    account_id = login.account_id();
    character_id = created.char_().char_id();

    const auto price = [](const char *id, const char *category, const char *price_value,
                          const char *refund_value) {
      return nlohmann::json{{"id", id},
                            {"category", category},
                            {"priceType", "Gold"},
                            {"priceValue", price_value},
                            {"refundType", "Gold"},
                            {"refundValue", refund_value}};
    };
    catalog.write_items(
        {price("133", "Consumable", "500", "500"), price("70109003", "CraftMaterial", "5", "2"),
         price("72000016", "CraftMaterial", "10", "1"),
         price("11200201", "Equipment", "400", "80")},
        {{"Npc", {{{"id", "1001121"}, {"interactionGroupId", "32"}}}},
         {"Interaction",
          {{{"id", "212"},
            {"groupId", "32"},
            {"playerTaskCase", "Shop"},
            {"playerTask", "\"shopId\":1001"}}}},
         {"Shop",
          {{{"id", "1001"}, {"FK_itemId", "133"}, {"priceMultiplierPerMille", ""}},
           {{"id", "1001"}, {"FK_itemId", "70109003"}, {"priceMultiplierPerMille", "2000"}},
           {{"id", "1001"}, {"FK_itemId", "11200201"}, {"priceMultiplierPerMille", ""}}}}});

    set_progress(gold, stackables);

    x5::pb::ws::front::CharSelectReq select_request;
    select_request.set_char_id(character_id);
    const auto selected = auth->select(login.access_token(), select_request);
    if (selected.ec() != x5::ec::Success) {
      throw std::runtime_error("Merchant fixture cannot select the character");
    }

    server = std::make_unique<TwoClientWorldServer>(*auth, certificate, nullptr, nullptr,
                                                    catalog.open());
    protocol = std::make_unique<Protocol>(CHRONICLES_PROTOCOL_DIR);
    client = std::make_unique<WorldTestClient>(server->port());
    client->send_key(selected.packet_key());
    EXPECT_EQ(client->read().opcode(), 5);
    client->send(protocol->encode(20001, nlohmann::json::object(), 0));
    EXPECT_EQ(client->read().opcode(), 175);
    EXPECT_EQ(client->read().opcode(), 1);
    read_entry_inventory(*client);
    EXPECT_EQ(client->read().opcode(), 126);
    client->send(protocol->encode(20085, nlohmann::json::object(), 1));
    EXPECT_EQ(client->read().opcode(), 176);
    expect_pre_playing_combat_bootstrap(*client, *protocol);
  }

  void set_progress(std::int64_t gold, const std::map<int, int> &stackables) {
    const auto before =
        repositories.character_state->load_world_progress(account_id, character_id, 1);
    auto after = before;
    after.revision = before.revision + 1;
    after.gold = gold;
    after.stackables = stackables;
    ASSERT_TRUE(repositories.character_state->save_world_progress(account_id, character_id, 1,
                                                                  before, after));
  }

  WorldProgress progress() const {
    return repositories.character_state->load_world_progress(account_id, character_id, 1);
  }

  MerchantRequestPacer shop_pacer;

  void send_shop(const nlohmann::json &shop) {
    shop_pacer.wait();
    client->send(protocol->encode(
        20068, {{"target_unit_id", "1001"}, {"param", {{"interaction_id", 212}, {"shop", shop}}}},
        sequence++));
  }

  nlohmann::json read_message(std::uint16_t expected_opcode) {
    const auto frame = client->read();
    EXPECT_EQ(frame.opcode(), expected_opcode);
    return protocol->decode(frame).at("message");
  }

  void expect_refusal(const nlohmann::json &shop, x5::ec::EErrorCode code) {
    send_shop(shop);
    SCOPED_TRACE(x5::ec::EErrorCode_Name(code));
    EXPECT_EQ(read_message(2).at("ec"), x5::ec::EErrorCode_Name(code));
  }

  CharacterRepositories repositories;
  Config config;
  std::unique_ptr<AuthService> auth;
  TemporaryWorldCatalog catalog;
  TemporaryWorldCertificate certificate;
  std::unique_ptr<TwoClientWorldServer> server;
  std::unique_ptr<Protocol> protocol;
  std::unique_ptr<WorldTestClient> client;
  std::int64_t account_id{};
  std::int64_t character_id{};
  std::uint32_t sequence{2};
};

inline nlohmann::json buy_list(int cms_id, int count) {
  return {{"buy_list", nlohmann::json::array({{{"item_cms_id", cms_id}, {"count", count}}})}};
}

inline nlohmann::json sell_list(int cms_id, int count) {
  return {{"sell_list",
           nlohmann::json::array({{{"stackable", {{"item_cms_id", cms_id}, {"count", count}}}}})}};
}

inline void send_shop_to_target(MerchantWorld &world, const std::string &target, int interaction_id,
                                const nlohmann::json &shop) {
  world.shop_pacer.wait();
  world.client->send(world.protocol->encode(
      20068,
      {{"target_unit_id", target}, {"param", {{"interaction_id", interaction_id}, {"shop", shop}}}},
      world.sequence++));
}

inline x5::ec::EErrorCode forged_target_error(const std::string &target) {
  // Unit ids follow parse_unit_id_text: zero and values above INT64_MAX are malformed.
  return target == "0" || target == "18446744073709551615" ? x5::ec::BadRequest
                                                           : x5::ec::Interaction_TargetNotFound;
}

inline void expect_buy_and_sell_refused(MerchantWorld &world, const std::string &target,
                                        int interaction_id) {
  const auto before = world.progress();
  SCOPED_TRACE("target " + target + " interaction " + std::to_string(interaction_id));
  send_shop_to_target(world, target, interaction_id, buy_list(133, 1));
  EXPECT_EQ(world.read_message(2).at("ec"), x5::ec::EErrorCode_Name(forged_target_error(target)));
  send_shop_to_target(world, target, interaction_id, sell_list(72000016, 4));
  EXPECT_EQ(world.read_message(2).at("ec"), x5::ec::EErrorCode_Name(forged_target_error(target)));
  EXPECT_EQ(world.progress(), before);
}

} // namespace chronicles::native_testing
