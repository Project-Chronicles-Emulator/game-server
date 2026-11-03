#include "world/features/character/character_stats.hpp"
#include "world/features/inventory/inventory_capacity.hpp"
#include "world/features/inventory/inventory_rules.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>
namespace chronicles {
namespace {

using namespace native_testing;

// Catalog tables the character stat module needs for the Fight class, the starter gear and the
// polish fixture items, plus their polish increase tables. The increase percentages and attack
// rates are copied from ItemPolishIncreaseValue.jsonl / ItemAttribute.jsonl (13102102 and
// 11102302); the rest are fixture values.
constexpr const char *kStatsSourceSha256 =
    "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6";
constexpr int kStarterWeaponCmsId = 11101102;
constexpr int kMaxEntryFrames = 8;

TemporaryWorldCatalog::ExtraTables polish_world_stat_tables() {
  using Json = nlohmann::json;
  TemporaryWorldCatalog::ExtraTables tables;

  std::vector<Json> basic_stats{
      {{"FK_character", "Fight"}, {"typeValue", "Str:29,Int:10,Con:17,End:7,Dex:16,Tal:11"}},
      {{"FK_character", "Wild"}, {"typeValue", "Str:15,Int:12,Con:8,End:11,Dex:27,Tal:17"}},
      {{"FK_character", "Adamant"}, {"typeValue", "Str:18,Int:4,Con:26,End:25,Dex:6,Tal:11"}},
      {{"FK_character", "Magic"}, {"typeValue", "Str:10,Int:30,Con:6,End:7,Dex:17,Tal:20"}}};
  tables.emplace_back("CharacterBasicStat", std::move(basic_stats));

  std::vector<Json> perks;
  for (const auto &[class_name, node_id] : std::array<std::pair<const char *, int>, 6>{{
           {"Fight", 1010101},
           {"Wild", 1210101},
           {"Adamant", 1310101},
           {"Magic", 1425101},
           {"Magic", 1425102},
           {"Magic", 1425201}}}) {
    perks.push_back({{"characterClass", class_name},
                     {"id", std::to_string(node_id)},
                     {"autoRegistration", "True"}});
  }
  tables.emplace_back("PerkNode", std::move(perks));

  std::vector<Json> grades;
  for (const char *name : {"Normal", "Grand", "Rare", "Heroic", "Legendary", "Mythic"}) {
    Json grade{{"grade", name}, {"basicStatRatio", "1000"}};
    for (const char *field :
         {"physicalDamageRate", "magicDamageRate", "windDamageRate", "fogDamageRate",
          "quakeDamageRate", "fireDamageRate", "tidalDamageRate", "electricDamageRate",
          "ironDamageRate", "hitDamageRate", "impactDamageRate", "physicalArmorRate",
          "magicArmorRate", "windArmorRate", "fogArmorRate", "quakeArmorRate", "fireArmorRate",
          "tidalArmorRate", "electricArmorRate", "ironArmorRate", "hitArmorRate",
          "impactArmorRate"}) {
      grade[field] = "1000";
    }
    grades.push_back(std::move(grade));
  }
  tables.emplace_back("EquipmentGrade", std::move(grades));
  tables.emplace_back("Stat", std::vector<Json>{});
  tables.emplace_back("GameVariable",
                      std::vector<Json>{{{"name", "DurabilityDangerRatio"}, {"value", "0.1"}},
                                        {{"name", "DurabilityPanaltyValue"}, {"value", "0.5"}}});

  const auto attributes = [](const char *id, const char *physical, const char *magic,
                             const char *increase_id) {
    return Json{{"id", id},
                {"physicalDamageRate", physical},
                {"magicDamageRate", magic},
                {"hitDamageRate", "80"},
                {"impactDamageRate", "70"},
                {"FK_itemPolishIncreaseValueId", increase_id}};
  };
  std::vector<Json> item_attributes{
      attributes("13102102", "52", "13", "100100"), attributes("11102302", "44", "36", "100000"),
      attributes("11101102", "70", "30", "100000"), attributes("13102104", "44", "36", "100300"),
      attributes("13102105", "44", "36", "100301"),
      // Always-fail (Heroic) and always-destroyed (Legendary) synthetic weapons.
      attributes("13102106", "44", "36", "100100"), attributes("13102107", "44", "36", "100100"),
      {{"id", "12401201"}, {"physicalArmorRate", "70"}, {"FK_itemPolishIncreaseValueId", "100200"}},
      {{"id", "99001"}}, {{"id", "10130102"}}, {{"id", "10130103"}}, {{"id", "10130104"}},
      {{"id", "11700201"}}};
  tables.emplace_back("ItemAttribute", std::move(item_attributes));

  const auto increase = [](const char *id, const char *step, const char *percent) {
    return Json{{"id", id}, {"polishStep", step}, {"polishIncreasePercent", percent}};
  };
  tables.emplace_back(
      "ItemPolishIncreaseValue",
      std::vector<Json>{increase("100100", "1", "17.8"), increase("100100", "2", "35.5"),
                        increase("100000", "1", "32.5"), increase("100200", "1", "10"),
                        increase("100300", "1", "10"), increase("100301", "1", "10"),
                        increase("100301", "2", "20")});
  return tables;
}

// A non-tutorial world session with polishable items: a Grand weapon (13102102), a Normal
// weapon (11102302), a Grand armor suit (12401201) and a costume suit (not polishable), with
// the ItemPolish rows the captured exchanges used (copied from ItemPolish.jsonl).
class PolishWorld {
public:
  PolishWorld(std::int64_t gold, const std::map<int, int> &stackables,
              bool equip_armor_suit = false) {
    config.game_address = "127.0.0.1:8081";
    auth = std::make_unique<AuthService>(repositories.character_repositories(), config);
    auth->acquire_monster_reward_owner_epoch();
    auth->create_account("polish_world", "test-world-password-123");
    const auto login = auth->login("polish_world", "test-world-password-123");

    x5::pb::authwebsocket::WorldEnterReq world_request;
    world_request.set_world_id(1);
    if (auth->enter(login.access_token(), world_request).error_code() != x5::ec::Success) {
      throw std::runtime_error("Polish fixture cannot enter the world");
    }
    x5::pb::ws::front::CharCreateReq create_request;
    create_request.set_slot(0);
    create_request.set_char_name("PolishWorld");
    create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
    const auto created = auth->create(login.access_token(), create_request);
    if (created.ec() != x5::ec::Success) {
      throw std::runtime_error("Polish fixture cannot create a character");
    }
    account_id = login.account_id();
    character_id = created.char_().char_id();

    const auto item = [](const char *id, const char *category, const char *grade,
                         const char *equip_slot) {
      return nlohmann::json{{"id", id},
                            {"category", category},
                            {"grade", grade},
                            {"equipSlot", equip_slot},
                            {"level", "6"},
                            {"maxDurability", "10000"},
                            {"FK_itemAttribute", id}};
    };
    const auto row = [](const char *grade, const char *category, const char *step,
                        const char *success, const char *fail, const char *destruction,
                        const char *material, const char *material_count, const char *cost) {
      return nlohmann::json{{"grade", grade},
                            {"equipCategory", category},
                            {"polishStep", step},
                            {"successProbability", success},
                            {"failProbability", fail},
                            {"destructionProbability", destruction},
                            {"materialId01", material},
                            {"materialCnt01", material_count},
                            {"materialId02", ""},
                            {"materialCnt02", ""},
                            {"costType", "Gold"},
                            {"costValue", cost}};
    };
    const auto additive = [](const char *id, const char *grade, const char *effect,
                             const char *value) {
      return nlohmann::json{{"id", id},
                            {"category", "PolishingMaterial"},
                            {"grade", grade},
                            {"additiveMaterialEffect", effect},
                            {"additiveMaterialValue", value}};
    };
    const std::vector<nlohmann::json> items{
        item("13102102", "Equipment", "Grand", "Weapon_1"),
        item("11102302", "Equipment", "Normal", "Weapon_1"),
        item("12401201", "Equipment", "Grand", "ArmorSuit"),
        item("99001", "Equipment", "Grand", "CostumeSuit"),
        item("13102106", "Equipment", "Heroic", "Weapon_1"),
        item("13102107", "Equipment", "Legendary", "Weapon_1"),
        item("90211101", "PolishingMaterial", "Grand", ""),
        item("90211201", "PolishingMaterial", "Grand", ""),
        additive("90551001", "Legendary", "AddSuccessPercent", "100"),
        additive("90552001", "Legendary", "DestructionProhibition", "1")};
    // The first four rows are copied from ItemPolish.jsonl; the Heroic (always fail) and
    // Legendary (always destroyed) rows are synthetic so every outcome is deterministic.
    const std::vector<nlohmann::json> polish_rows{
        row("Grand", "Weapon", "1", "10000", "0", "0", "90211101", "1", "1000"),
        row("Grand", "Weapon", "2", "6000", "4000", "0", "90211101", "2", "1425"),
        row("Normal", "Weapon", "1", "10000", "0", "0", "90211101", "1", "200"),
        row("Grand", "Armor", "1", "10000", "0", "0", "90211201", "1", "1000"),
        row("Heroic", "Weapon", "1", "0", "10000", "0", "90211101", "1", "300"),
        row("Legendary", "Weapon", "1", "0", "0", "10000", "90211101", "1", "400")};
    // Equipment rows the stat module needs (level, durability, attributes) and the Fight starter
    // gear every created character wears; 13102104/13102105 differ only in increase table length.
    const auto equipment = [](const char *id, const char *grade, const char *slot) {
      return nlohmann::json{{"id", id},
                            {"category", "Equipment"},
                            {"grade", grade},
                            {"equipSlot", slot},
                            {"level", "6"},
                            {"maxDurability", "10000"},
                            {"FK_itemAttribute", id}};
    };
    const std::vector<nlohmann::json> stat_items{
        equipment("11101102", "Normal", "Weapon_1"),
        equipment("10130102", "Normal", "CostumeSuit"),
        equipment("10130103", "Normal", "CostumeGloves"),
        equipment("10130104", "Normal", "CostumeShoes"),
        equipment("11700201", "Normal", "Bag"),
        equipment("13102104", "Rare", "Weapon_1"),
        equipment("13102105", "Rare", "Weapon_1")};
    auto all_items = items;
    all_items.insert(all_items.end(), stat_items.begin(), stat_items.end());
    auto all_polish_rows = polish_rows;
    all_polish_rows.push_back(
        row("Rare", "Weapon", "1", "10000", "0", "0", "90211101", "1", "100"));
    all_polish_rows.push_back(
        row("Rare", "Weapon", "2", "10000", "0", "0", "90211101", "1", "100"));
    auto tables = polish_world_stat_tables();
    tables.emplace_back("ItemPolish", all_polish_rows);
    catalog.write_items(all_items, tables, kStatsSourceSha256);

    int slot = 600;
    for (const int cms_id :
         {13102102, 11102302, 12401201, 99001, 13102104, 13102105, 13102106, 13102107}) {
      // Optionally wear the armor suit so its polish also triggers the equipment refresh.
      const std::optional<int> worn =
          equip_armor_suit && cms_id == 12401201
              ? std::optional<int>(x5::cms::EX5EquipSlotType_ArmorSuit)
              : std::nullopt;
      const auto granted = repositories.character_state->grant_item(
          account_id, character_id, 1, CharacterItem{0, cms_id, slot++, worn, 1, 9908, 3});
      if (!granted) {
        throw std::runtime_error("Polish fixture cannot grant an item");
      }
      item_ids[cms_id] = granted->id;
    }
    set_progress(gold, stackables);

    x5::pb::ws::front::CharSelectReq select_request;
    select_request.set_char_id(character_id);
    const auto selected = auth->select(login.access_token(), select_request);
    if (selected.ec() != x5::ec::Success) {
      throw std::runtime_error("Polish fixture cannot select the character");
    }


    const auto client_catalog = catalog.open();
    const auto stats = CharacterStats::load(*client_catalog);
    server = std::make_unique<TwoClientWorldServer>(*auth, certificate, nullptr, stats,
                                                    client_catalog);
    protocol = std::make_unique<Protocol>(CHRONICLES_PROTOCOL_DIR);
    client = std::make_unique<WorldTestClient>(server->port());
    client->send_key(selected.packet_key());
    EXPECT_EQ(client->read().opcode(), 5);
    client->send(protocol->encode(20001, nlohmann::json::object(), 0));
    EXPECT_EQ(client->read().opcode(), 175);
    EXPECT_EQ(client->read().opcode(), 1);
    // With character stats loaded the entry also carries the character base notification.
    for (int reads = 0; reads < kMaxEntryFrames; ++reads) {
      if (client->read().opcode() == 126) {
        break;
      }
    }
    client->send(protocol->encode(20085, nlohmann::json::object(), 1));
    EXPECT_EQ(client->read().opcode(), 176);
    expect_pre_playing_combat_bootstrap(*client, *protocol);

    // The starter gear exists once the world entry has run; the polish tests wear its weapon.
    const auto entered = repositories.characters->characters(1, 1).front();
    for (const auto &owned : entered.items) {
      if (owned.cms_id == kStarterWeaponCmsId) {
        item_ids[owned.cms_id] = owned.id;
      }
    }
    if (!item_ids.contains(kStarterWeaponCmsId)) {
      throw std::runtime_error("Polish fixture character has no starter weapon");
    }
    stats_before_polish = stats->stats(entered);
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

  int polish_step(int cms_id) const {
    const auto characters = repositories.characters->characters(1, 1);
    for (const auto &item : characters.front().items) {
      if (item.id == item_ids.at(cms_id)) {
        return item.polish_step;
      }
    }
    return -1;
  }

  void send_polish(const std::string &item_id, int additive_cms_id = 0) {
    nlohmann::json message{{"item_id", item_id}};
    if (additive_cms_id != 0) {
      message["add_material_item_cms_id"] = additive_cms_id;
    }
    client->send(protocol->encode(20075, message, sequence++));
  }

  std::optional<CharacterItem> stored_item(std::int64_t item_id) const {
    // Keep the loaded characters alive for the whole loop.
    const auto characters = repositories.characters->characters(1, 1);
    for (const auto &item : characters.front().items) {
      if (item.id == item_id) {
        return item;
      }
    }
    return std::nullopt;
  }

  nlohmann::json read_message(std::uint16_t expected_opcode) {
    const auto frame = client->read();
    EXPECT_EQ(frame.opcode(), expected_opcode);
    return protocol->decode(frame).at("message");
  }

  // A refused polish answers with the common error frame and changes neither gold, stackables,
  // revision nor any polish_step.
  void expect_refused_unchanged(const std::string &item_id, x5::ec::EErrorCode code) {
    const auto before = progress();
    const auto steps = std::make_tuple(polish_step(13102102), polish_step(11102302),
                                       polish_step(12401201), polish_step(99001));
    send_polish(item_id);
    SCOPED_TRACE(x5::ec::EErrorCode_Name(code));
    EXPECT_EQ(read_message(2).at("ec"), x5::ec::EErrorCode_Name(code));
    EXPECT_EQ(progress(), before);
    EXPECT_EQ(steps, std::make_tuple(polish_step(13102102), polish_step(11102302),
                                     polish_step(12401201), polish_step(99001)));
  }

  MemoryRepositories repositories;
  Config config;
  std::unique_ptr<AuthService> auth;
  TemporaryWorldCatalog catalog;
  TemporaryWorldCertificate certificate;
  std::unique_ptr<TwoClientWorldServer> server;
  std::unique_ptr<Protocol> protocol;
  std::unique_ptr<WorldTestClient> client;
  std::map<int, std::int64_t> item_ids;
  std::int64_t account_id{};
  std::int64_t character_id{};
  std::uint32_t sequence{2};
  nlohmann::json stats_before_polish;
};

TEST(NativeWorld, CarriedWeightUsesEquippedBagCapacityAndStackQuantity) {
  TemporaryWorldCatalog catalog;
  catalog.write_items({
      {{"id", "90001"}, {"weight", "20"}},
      {{"id", "90002"}, {"weight", "100"}, {"safeWeight", "135000"}, {"equipSlot", "Bag"}},
      {{"id", "90003"}, {"weight", "10"}},
  });
  auto definitions = catalog.open();
  InventoryCapacity capacity(*definitions);

  Character character;
  character.items = {
      CharacterItem{1, 90001, 0, std::nullopt, 2},
      CharacterItem{2, 90002, 1, 24, 1},
  };
  WorldProgress progress;
  progress.stackables[90003] = 3;
  const auto equipped = capacity.measure(character, progress);
  EXPECT_EQ(equipped.total, 170);
  EXPECT_EQ(equipped.safe, 135000);

  character.items.at(1).equip_slot.reset();
  const auto bag_in_inventory = capacity.measure(character, progress);
  EXPECT_EQ(bag_in_inventory.total, 170);
  EXPECT_EQ(bag_in_inventory.safe, 50000);

  progress.stackables[90003] = -1;
  EXPECT_THROW(capacity.measure(character, progress), std::runtime_error);
}

TEST(NativeWorld, ItemDiscardValidatesBatchAndPersists) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  ASSERT_TRUE(auth.create_account("discard_owner", "test-world-password-123"));
  const auto login = auth.login("discard_owner", "test-world-password-123");
  x5::pb::authwebsocket::WorldEnterReq enter_request;
  enter_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), enter_request).error_code(), x5::ec::Success);

  Character character;
  character.account_id = login.account_id();
  character.name = "DiscardOwner";
  character = repositories.characters->create(character);
  const auto discardable = repositories.character_state->grant_item(
      character.account_id, character.id, 1,
      CharacterItem{0, 90001, 5, std::nullopt, 1, 10000, 0});
  const auto protected_item = repositories.character_state->grant_item(
      character.account_id, character.id, 1,
      CharacterItem{0, 90002, 6, std::nullopt, 1, 10000, 0});
  ASSERT_TRUE(discardable && protected_item);
  auto before =
      repositories.character_state->load_world_progress(character.account_id, character.id, 1);
  auto with_stack = before;
  with_stack.revision = before.revision + 1;
  with_stack.stackables[90003] = 5;
  ASSERT_TRUE(repositories.character_state->save_world_progress(character.account_id, character.id,
                                                                1, before, with_stack));

  ASSERT_TRUE(auth.create_account("discard_other", "test-world-password-123"));
  Character other;
  other.account_id = repositories.accounts->account("discard_other")->id;
  other.name = "DiscardOther";
  other = repositories.characters->create(other);

  TemporaryWorldCatalog catalog;
  catalog.write_items({
      {{"id", "90001"}, {"discardable", "True"}},
      {{"id", "90002"}, {"discardable", "False"}},
      {{"id", "90003"}, {"discardable", "True"}},
      {{"id", std::to_string(character.items.front().cms_id)}, {"discardable", "True"}},
  });
  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(character.id);
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  ASSERT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  read_entry_inventory(client);
  EXPECT_EQ(client.read().opcode(), 126);

  std::uint32_t sequence = 1;
  const auto request = [&](const nlohmann::json &identities) {
    client.send(protocol.encode(20072, {{"identity_list", identities}}, sequence++));
    return client.read();
  };
  const auto item = [](std::int64_t id) {
    return nlohmann::json{{"item_id", std::to_string(id)}};
  };
  const auto stack = [](int count) {
    return nlohmann::json{{"stackable", {{"item_cms_id", 90003}, {"count", count}}}};
  };
  const auto expect_error = [&](const nlohmann::json &identities, x5::ec::EErrorCode code) {
    const auto response = request(identities);
    ASSERT_EQ(response.opcode(), 2);
    EXPECT_EQ(protocol.decode(response).at("message").at("ec"),
              x5::ec::EErrorCode_Name(code));
  };
  expect_error(nlohmann::json::array({item(discardable->id), item(other.items.front().id)}),
               x5::ec::ItemIdInvalid);
  expect_error(nlohmann::json::array({stack(6)}), x5::ec::ItemCountError);
  expect_error(nlohmann::json::array({item(protected_item->id)}), x5::ec::ItemNonDiscardable);
  expect_error(nlohmann::json::array({item(character.items.front().id)}), x5::ec::ItemEquipped);
  ASSERT_EQ(repositories.characters->characters(character.account_id, 1).front().items.size(), 7);
  EXPECT_EQ(repositories.character_state->load_world_progress(character.account_id, character.id, 1)
                .stackables.at(90003),
            5);

  const auto update = request(nlohmann::json::array({item(discardable->id), stack(2)}));
  ASSERT_EQ(update.opcode(), 169);
  const auto message = protocol.decode(update).at("message");
  EXPECT_TRUE(message.contains("discard"));
  EXPECT_EQ(message.at("items").at(0).at("item_id"), std::to_string(discardable->id));
  EXPECT_EQ(message.at("stackables").at(0).at("quantity"), 3);
  EXPECT_EQ(repositories.character_state->load_world_progress(character.account_id, character.id, 1)
                .stackables.at(90003),
            3);
  const auto persisted = repositories.characters->characters(character.account_id, 1);
  ASSERT_EQ(persisted.size(), 1);
  EXPECT_EQ(std::ranges::count_if(persisted.front().items, [&](const CharacterItem &owned) {
              return owned.id == discardable->id;
            }), 0);

  client.disconnect();
  const auto reselected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(reselected.ec(), x5::ec::Success);
  WorldTestClient reconnected(server.port());
  reconnected.send_key(reselected.packet_key());
  EXPECT_EQ(reconnected.read().opcode(), 5);
  reconnected.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(reconnected.read().opcode(), 175);
  EXPECT_EQ(reconnected.read().opcode(), 1);
  const auto inventory = read_entry_inventory(reconnected);
  ASSERT_EQ(inventory.opcode(), 130);
  EXPECT_EQ(reconnected.read().opcode(), 126);
  const auto inventory_message = protocol.decode(inventory).at("message");
  const auto &reconnected_items = inventory_message.at("items");
  EXPECT_EQ(std::ranges::count_if(reconnected_items, [&](const nlohmann::json &owned) {
              return owned.at("item_id") == std::to_string(discardable->id);
            }), 0);
  const auto &reconnected_stacks = inventory_message.at("stackables");
  ASSERT_EQ(reconnected_stacks.size(), 1);
  EXPECT_EQ(reconnected_stacks.at(0).at("item_cms_id"), 90003);
  EXPECT_EQ(reconnected_stacks.at(0).at("quantity"), 3);
  EXPECT_EQ(repositories.character_state->load_world_progress(character.account_id, character.id, 1)
                .stackables.at(90003),
            3);
}

void verify_weapon_equip_and_reconnect(int equip_slot) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  auth.create_account("equip_test", "test-world-password-123");
  const auto login = auth.login("equip_test", "test-world-password-123");

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("Equiper");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  TemporaryWorldCatalog catalog;
  catalog.write_items({
      {{"id", "11101102"},
       {"equipSlot", "Weapon_1"},
       {"class", "Fight"},
       {"levelReq", "1"}},
      {{"id", "10130102"},
       {"equipSlot", "CostumeSuit"},
       {"class", "Fight"},
       {"levelReq", "1"}},
      {{"id", "90001"},
       {"equipSlot", "Weapon_1"},
       {"class", "Fight"},
       {"levelReq", "1"}},
  });

  // A second Fight weapon either swaps the first slot or occupies the second slot.
  const auto stored = repositories.characters->characters(1, 1);
  ASSERT_EQ(stored.size(), 1);
  const auto character_id = stored.front().id;
  const auto granted =
      repositories.character_state->grant_item(1, character_id, 1,
                        CharacterItem{0, 90001, 5, std::nullopt, 1, 10000, 0});
  ASSERT_TRUE(granted);

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(created.char_().char_id());
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  read_entry_inventory(client);
  EXPECT_EQ(client.read().opcode(), 126);
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  EXPECT_EQ(client.read().opcode(), 176);
  expect_pre_playing_combat_bootstrap(client, protocol);

  client.send(
      protocol.encode(20074, {{"item_id", std::to_string(granted->id)}, {"slot", equip_slot}}, 2));
  const auto update = client.read();
  ASSERT_EQ(update.opcode(), 169);
  const auto update_message = protocol.decode(update).at("message");
  ASSERT_TRUE(update_message.contains("equip"));
  ASSERT_EQ(update_message.at("items").size(), equip_slot == 0 ? 2 : 1);

  nlohmann::json vacated;
  nlohmann::json equipped;
  for (const auto &item : update_message.at("items")) {
    if (item.at("item_id") == std::to_string(granted->id)) {
      equipped = item;
    } else {
      vacated = item;
    }
  }
  if (equip_slot == 0) {
    ASSERT_FALSE(vacated.is_null());
    EXPECT_FALSE(vacated.contains("equip_slot"));
  } else {
    EXPECT_TRUE(vacated.is_null());
  }
  ASSERT_FALSE(equipped.is_null());
  const std::string slot_name =
      equip_slot == 0 ? "EX5EquipSlotType_Weapon_1" : "EX5EquipSlotType_Weapon_2";
  EXPECT_EQ(equipped.at("equip_slot"), slot_name);

  const auto visual = client.read();
  ASSERT_EQ(visual.opcode(), 168);
  const auto visual_message = protocol.decode(visual).at("message");
  EXPECT_EQ(visual_message.at("unit_id"), std::to_string(player_wire_unit_id(character_id)));
  ASSERT_EQ(visual_message.at("equipments").size(), 1);
  EXPECT_EQ(visual_message.at("equipments").at(0).at("item_cms_id"), 90001);

  const auto persisted = repositories.characters->characters(1, 1);
  ASSERT_EQ(persisted.size(), 1);
  const auto stored_weapon =
      std::ranges::find_if(persisted.front().items, [&](const CharacterItem &item) {
        return item.equip_slot == equip_slot;
      });
  ASSERT_NE(stored_weapon, persisted.front().items.end());
  EXPECT_EQ(stored_weapon->cms_id, 90001);
  client.send(protocol.encode(20078, {{"slot", equip_slot}}, 3));
  const auto activated = protocol.decode(read_until_opcode(client, 172)).at("message");
  EXPECT_EQ(activated.value("ec", std::string("Success")), "Success");
  EXPECT_EQ(activated.value("slot", std::string("EX5EquipSlotType_Weapon_1")), slot_name);
  client.disconnect();

  // Reconnecting replays the persisted equipment in the inventory snapshot.
  const auto reselect = auth.select(login.access_token(), select_request);
  ASSERT_EQ(reselect.ec(), x5::ec::Success);
  WorldTestClient reconnected(server.port());
  reconnected.send_key(reselect.packet_key());
  EXPECT_EQ(reconnected.read().opcode(), 5);
  reconnected.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(reconnected.read().opcode(), 175);
  EXPECT_EQ(reconnected.read().opcode(), 1);
  const auto inventory = read_entry_inventory(reconnected);
  ASSERT_EQ(inventory.opcode(), 130);
  EXPECT_EQ(reconnected.read().opcode(), 126);
  const auto inventory_message = protocol.decode(inventory).at("message");
  EXPECT_EQ(inventory_message.value("weapon_slot", std::string("EX5EquipSlotType_Weapon_1")),
            slot_name);
  const auto items = inventory_message.at("items");
  const auto reconnected_weapon = std::ranges::find_if(items, [&](const nlohmann::json &item) {
    return item.value("equip_slot", std::string()) == slot_name;
  });
  ASSERT_NE(reconnected_weapon, items.end());
  EXPECT_EQ(reconnected_weapon->at("item_cms_id"), 90001);
}

TEST(NativeWorld, ItemEquipSwapsWeaponAndPersistsAcrossReconnect) {
  verify_weapon_equip_and_reconnect(0);
}

TEST(NativeWorld, SecondWeaponEquipsActivatesAndPersistsAcrossReconnect) {
  verify_weapon_equip_and_reconnect(1);
}

TEST(NativeWorld, BareUnequipVacatesSlotAndEmptySlotIsSilentNoOp) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  auth.create_account("unequip_test", "test-world-password-123");
  const auto login = auth.login("unequip_test", "test-world-password-123");

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("Unequiper");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  TemporaryWorldCatalog catalog;
  catalog.write_items({
      {{"id", "10130102"},
       {"equipSlot", "CostumeSuit"},
       {"class", "Fight"},
       {"levelReq", "1"}},
  });

  // Precondition: the starter costume suit is equipped and no ring is worn.
  const auto stored = repositories.characters->characters(1, 1);
  ASSERT_EQ(stored.size(), 1);
  const auto character_id = stored.front().id;
  const auto suit_before = std::ranges::find_if(
      stored.front().items, [](const CharacterItem &item) { return item.cms_id == 10130102; });
  ASSERT_NE(suit_before, stored.front().items.end());
  ASSERT_EQ(suit_before->equip_slot, x5::cms::EX5EquipSlotType_CostumeSuit);
  ASSERT_TRUE(std::ranges::none_of(stored.front().items, [](const CharacterItem &item) {
    return item.equip_slot == x5::cms::EX5EquipSlotType_Ring;
  }));

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(created.char_().char_id());
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  read_entry_inventory(client);
  EXPECT_EQ(client.read().opcode(), 126);
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  EXPECT_EQ(client.read().opcode(), 176);
  expect_pre_playing_combat_bootstrap(client, protocol);

  // An empty slot produces no reply at all: the next frame read must belong to
  // the following request, and nothing is persisted.
  client.send(protocol.encode(20074, {{"slot", "EX5EquipSlotType_Ring"}}, 2));

  // Observed shape: slot only, item_id omitted.
  client.send(protocol.encode(20074, {{"slot", "EX5EquipSlotType_CostumeSuit"}}, 3));
  const auto update = client.read();
  ASSERT_EQ(update.opcode(), 169);
  const auto update_message = protocol.decode(update).at("message");
  ASSERT_TRUE(update_message.contains("equip"));
  ASSERT_EQ(update_message.at("items").size(), 1);
  EXPECT_EQ(update_message.at("items").at(0).at("item_cms_id"), 10130102);
  EXPECT_FALSE(update_message.at("items").at(0).contains("equip_slot"));

  const auto visual = client.read();
  ASSERT_EQ(visual.opcode(), 168);
  const auto visual_message = protocol.decode(visual).at("message");
  EXPECT_EQ(visual_message.at("unit_id"), std::to_string(player_wire_unit_id(character_id)));
  ASSERT_EQ(visual_message.at("equipments").size(), 1);
  EXPECT_EQ(visual_message.at("equipments").at(0).at("slot"), "EX5EquipSlotType_CostumeSuit");
  EXPECT_FALSE(visual_message.at("equipments").at(0).contains("item_cms_id"));

  const auto persisted = repositories.characters->characters(1, 1);
  ASSERT_EQ(persisted.size(), 1);
  const auto suit_after = std::ranges::find_if(
      persisted.front().items, [](const CharacterItem &item) { return item.cms_id == 10130102; });
  ASSERT_NE(suit_after, persisted.front().items.end());
  EXPECT_FALSE(suit_after->equip_slot.has_value());
}

TEST(NativeWorld, BareUnequipMutationIsRejectedUnderStaleRevisionWithoutChange) {
  // The handler reads the revision immediately before persisting, so a stale
  // revision cannot be staged through the socket; this exercises the exact
  // update_items call the handler makes with the InventoryRules mutation.
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  auth.create_account("unequip_stale", "test-world-password-123");
  const auto login = auth.login("unequip_stale", "test-world-password-123");

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("StaleUnequip");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  const auto stored = repositories.characters->characters(1, 1);
  ASSERT_EQ(stored.size(), 1);
  const auto &character = stored.front();
  constexpr int kSuitSlot = x5::cms::EX5EquipSlotType_CostumeSuit;

  VisualSlotClear visual_clear;
  const auto update = InventoryRules::bare_unequip(character.items, kSuitSlot, visual_clear);
  ASSERT_TRUE(update.vacated);
  auto cleared = *std::ranges::find_if(
      character.items, [&](const CharacterItem &item) { return item.id == update.item_id; });
  cleared.equip_slot.reset();

  const auto progress =
      repositories.character_state->load_world_progress(character.account_id, character.id, 1);
  EXPECT_FALSE(repositories.character_state->update_items(character.account_id, character.id, 1,
                                                          progress.revision + 1, {cleared}));
  const auto unchanged = repositories.characters->characters(1, 1);
  const auto still_worn = std::ranges::find_if(
      unchanged.front().items, [&](const CharacterItem &item) { return item.id == update.item_id; });
  ASSERT_NE(still_worn, unchanged.front().items.end());
  EXPECT_EQ(still_worn->equip_slot, kSuitSlot);

  EXPECT_TRUE(repositories.character_state->update_items(character.account_id, character.id, 1,
                                                         progress.revision, {cleared}));
  const auto vacated = repositories.characters->characters(1, 1);
  const auto now_stored = std::ranges::find_if(
      vacated.front().items, [&](const CharacterItem &item) { return item.id == update.item_id; });
  ASSERT_NE(now_stored, vacated.front().items.end());
  EXPECT_FALSE(now_stored->equip_slot.has_value());
}

TEST(NativeWorld, BagUnequipWithCarriedWeightAboveDefaultSafeWeightHasNoRefusal) {
  // Documents current behaviour only: unequipping the Bag slot is treated like
  // any other slot. The client blocks it locally; no server refusal is observed
  // or invented, and carried weight is not compared with capacity on this path.
  //
  // A Fight character is created with the starter Bag (11700201) already equipped in
  // slot 24 (starter_gear.hpp). Equipping a second Bag would be rejected by the store's
  // equip-slot uniqueness check, so this test uses the starter Bag itself.
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  auth.create_account("bag_unequip", "test-world-password-123");
  const auto login = auth.login("bag_unequip", "test-world-password-123");

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("BagUnequip");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  // Catalog weights for every item the fixture character carries: the five Fight
  // starter items plus one heavy stackable.
  constexpr int kStarterBag = 11700201;
  constexpr int kHeavyStackable = 90003;
  const std::map<int, std::int64_t> weights{{11101102, 1500}, {10130102, 100}, {10130103, 50},
                                            {10130104, 50},   {kStarterBag, 100},
                                            {kHeavyStackable, 60000}};
  std::vector<nlohmann::json> catalog_rows;
  for (const auto &[cms_id, weight] : weights) {
    nlohmann::json row{{"id", std::to_string(cms_id)}, {"weight", std::to_string(weight)}};
    if (cms_id == kStarterBag) {
      row["safeWeight"] = "135000";
      row["equipSlot"] = "Bag";
    }
    catalog_rows.push_back(std::move(row));
  }
  TemporaryWorldCatalog catalog;
  catalog.write_items(catalog_rows);

  const auto character_id = created.char_().char_id();
  const auto before_progress =
      repositories.character_state->load_world_progress(1, character_id, 1);
  auto heavy_progress = before_progress;
  heavy_progress.revision = before_progress.revision + 1;
  heavy_progress.stackables[kHeavyStackable] = 1;
  ASSERT_TRUE(repositories.character_state->save_world_progress(1, character_id, 1, before_progress,
                                                                heavy_progress));

  const auto stored_before = repositories.characters->characters(1, 1);
  ASSERT_EQ(stored_before.size(), 1);
  const auto starter_bag_before = std::ranges::find_if(
      stored_before.front().items,
      [&](const CharacterItem &item) { return item.cms_id == kStarterBag; });
  ASSERT_NE(starter_bag_before, stored_before.front().items.end());
  ASSERT_EQ(starter_bag_before->equip_slot, x5::cms::EX5EquipSlotType_Bag);

  // Expected carried weight counts every fixture item, not a hard-coded subset.
  std::int64_t expected_total = weights.at(kHeavyStackable);
  for (const auto &item : stored_before.front().items) {
    expected_total += weights.at(item.cms_id) * item.quantity;
  }

  InventoryCapacity capacity(*catalog.open());
  const auto progress_before =
      repositories.character_state->load_world_progress(1, character_id, 1);
  const auto worn = capacity.measure(stored_before.front(), progress_before);
  EXPECT_EQ(worn.safe, 135000);
  EXPECT_EQ(worn.total, expected_total);
  EXPECT_GT(worn.total, 50000);

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(character_id);
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  read_entry_inventory(client);
  EXPECT_EQ(client.read().opcode(), 126);
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  EXPECT_EQ(client.read().opcode(), 176);
  expect_pre_playing_combat_bootstrap(client, protocol);

  client.send(protocol.encode(20074, {{"slot", "EX5EquipSlotType_Bag"}}, 2));
  const auto update = client.read();
  ASSERT_EQ(update.opcode(), 169);
  const auto update_message = protocol.decode(update).at("message");
  ASSERT_EQ(update_message.at("items").size(), 1);
  EXPECT_EQ(update_message.at("items").at(0).at("item_cms_id"), kStarterBag);
  EXPECT_FALSE(update_message.at("items").at(0).contains("equip_slot"));
  const auto visual = client.read();
  ASSERT_EQ(visual.opcode(), 168);
  const auto visual_message = protocol.decode(visual).at("message");
  ASSERT_EQ(visual_message.at("equipments").size(), 1);
  EXPECT_EQ(visual_message.at("equipments").at(0).at("slot"), "EX5EquipSlotType_Bag");
  EXPECT_FALSE(visual_message.at("equipments").at(0).contains("item_cms_id"));

  // Capacity falls back to the default while the carried weight is unchanged and
  // now exceeds it; measuring reports both values and does not throw.
  const auto stored_after = repositories.characters->characters(1, 1);
  const auto starter_bag_after = std::ranges::find_if(
      stored_after.front().items,
      [&](const CharacterItem &item) { return item.cms_id == kStarterBag; });
  ASSERT_NE(starter_bag_after, stored_after.front().items.end());
  EXPECT_FALSE(starter_bag_after->equip_slot.has_value());

  const auto progress_after = repositories.character_state->load_world_progress(1, character_id, 1);
  const auto unworn = capacity.measure(stored_after.front(), progress_after);
  EXPECT_EQ(unworn.safe, 50000);
  EXPECT_EQ(unworn.total, expected_total);
  EXPECT_GT(unworn.total, unworn.safe);
}

TEST(NativeWorld, RepeatedBareUnequipOfSameSlotIsSilentNoOp) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  auth.create_account("unequip_twice", "test-world-password-123");
  const auto login = auth.login("unequip_twice", "test-world-password-123");

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("UnequipTwice");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  TemporaryWorldCatalog catalog;
  catalog.write_items({{{"id", "10130102"}, {"equipSlot", "CostumeSuit"}},
                       {{"id", "10130103"}, {"equipSlot", "CostumeGloves"}}});

  const auto find_equipped = [](const Character &character, int cms_id) {
    return std::ranges::find_if(character.items, [&](const CharacterItem &item) {
      return item.cms_id == cms_id;
    });
  };
  const auto stored = repositories.characters->characters(1, 1);
  ASSERT_EQ(stored.size(), 1);
  const auto suit = find_equipped(stored.front(), 10130102);
  const auto gloves = find_equipped(stored.front(), 10130103);
  ASSERT_NE(suit, stored.front().items.end());
  ASSERT_NE(gloves, stored.front().items.end());
  ASSERT_EQ(suit->equip_slot, x5::cms::EX5EquipSlotType_CostumeSuit);
  ASSERT_EQ(gloves->equip_slot, x5::cms::EX5EquipSlotType_CostumeGloves);

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(created.char_().char_id());
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  read_entry_inventory(client);
  EXPECT_EQ(client.read().opcode(), 126);
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  EXPECT_EQ(client.read().opcode(), 176);
  expect_pre_playing_combat_bootstrap(client, protocol);

  client.send(protocol.encode(20074, {{"slot", "EX5EquipSlotType_CostumeSuit"}}, 2));
  EXPECT_EQ(client.read().opcode(), 169);
  EXPECT_EQ(client.read().opcode(), 168);

  // The repeat produces no reply; the next frame belongs to the gloves request.
  client.send(protocol.encode(20074, {{"slot", "EX5EquipSlotType_CostumeSuit"}}, 3));
  client.send(protocol.encode(20074, {{"slot", "EX5EquipSlotType_CostumeGloves"}}, 4));
  const auto gloves_update = client.read();
  ASSERT_EQ(gloves_update.opcode(), 169);
  const auto gloves_message = protocol.decode(gloves_update).at("message");
  ASSERT_EQ(gloves_message.at("items").size(), 1);
  EXPECT_EQ(gloves_message.at("items").at(0).at("item_cms_id"), 10130103);
  EXPECT_EQ(client.read().opcode(), 168);

  const auto persisted = repositories.characters->characters(1, 1);
  EXPECT_FALSE(find_equipped(persisted.front(), 10130102)->equip_slot.has_value());
  EXPECT_FALSE(find_equipped(persisted.front(), 10130103)->equip_slot.has_value());
}

TEST(NativeWorld, UnequipAndEquipInOneRevisionAreAtomic) {
  // The swap handler persists the vacated item and the newly equipped item in one
  // update_items call. A stale revision must leave both rows unchanged;
  // the current revision must apply both.
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  auth.create_account("swap_atomic", "test-world-password-123");
  const auto login = auth.login("swap_atomic", "test-world-password-123");

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("SwapAtomic");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  ASSERT_EQ(auth.create(login.access_token(), create_request).ec(), x5::ec::Success);

  const auto stored = repositories.characters->characters(1, 1);
  ASSERT_EQ(stored.size(), 1);
  const auto character_id = stored.front().id;
  const auto spare = repositories.character_state->grant_item(
      1, character_id, 1, CharacterItem{0, 90001, 5, std::nullopt, 1, 10000, 0});
  ASSERT_TRUE(spare);

  const auto current = repositories.characters->characters(1, 1).front();
  const auto worn = std::ranges::find_if(current.items, [](const CharacterItem &item) {
    return item.equip_slot == x5::cms::EX5EquipSlotType_Weapon_1;
  });
  ASSERT_NE(worn, current.items.end());
  auto vacated = *worn;
  vacated.equip_slot.reset();
  auto equipped = *spare;
  equipped.equip_slot = x5::cms::EX5EquipSlotType_Weapon_1;
  const std::vector<CharacterItem> swap{vacated, equipped};

  const auto revision =
      repositories.character_state->load_world_progress(1, character_id, 1).revision;
  EXPECT_FALSE(repositories.character_state->update_items(1, character_id, 1, revision + 1, swap));
  {
    const auto unchanged = repositories.characters->characters(1, 1).front();
    const auto old_weapon = std::ranges::find_if(
        unchanged.items, [&](const CharacterItem &item) { return item.id == worn->id; });
    const auto new_weapon = std::ranges::find_if(
        unchanged.items, [&](const CharacterItem &item) { return item.id == spare->id; });
    ASSERT_NE(old_weapon, unchanged.items.end());
    ASSERT_NE(new_weapon, unchanged.items.end());
    EXPECT_EQ(old_weapon->equip_slot, x5::cms::EX5EquipSlotType_Weapon_1);
    EXPECT_FALSE(new_weapon->equip_slot.has_value());
  }

  ASSERT_TRUE(repositories.character_state->update_items(1, character_id, 1, revision, swap));
  const auto swapped = repositories.characters->characters(1, 1).front();
  const auto old_weapon = std::ranges::find_if(
      swapped.items, [&](const CharacterItem &item) { return item.id == worn->id; });
  const auto new_weapon = std::ranges::find_if(
      swapped.items, [&](const CharacterItem &item) { return item.id == spare->id; });
  ASSERT_NE(old_weapon, swapped.items.end());
  ASSERT_NE(new_weapon, swapped.items.end());
  EXPECT_FALSE(old_weapon->equip_slot.has_value());
  EXPECT_EQ(new_weapon->equip_slot, x5::cms::EX5EquipSlotType_Weapon_1);
}

TEST(NativeWorld, CarriedWeightOverflowThrowsInsteadOfWrapping) {
  // InventoryCapacity::measure reports overflow as std::overflow_error. Equipping
  // does not change carried weight and the equip path does not call measure, so
  // there is no equip-time rejection to test; this pins the measure boundary.
  TemporaryWorldCatalog catalog;
  catalog.write_items({
      {{"id", "90010"}, {"weight", "9223372036854775807"}},
      {{"id", "90011"}, {"weight", "4611686018427387904"}},
  });
  auto definitions = catalog.open();
  InventoryCapacity capacity(*definitions);

  WorldProgress progress;
  Character product_overflow;
  product_overflow.items = {CharacterItem{1, 90010, 0, std::nullopt, 2}};
  EXPECT_THROW(capacity.measure(product_overflow, progress), std::overflow_error);

  Character sum_overflow;
  sum_overflow.items = {CharacterItem{1, 90010, 0, std::nullopt, 1},
                        CharacterItem{2, 90011, 1, std::nullopt, 1}};
  EXPECT_THROW(capacity.measure(sum_overflow, progress), std::overflow_error);

  Character exact;
  exact.items = {CharacterItem{1, 90010, 0, std::nullopt, 1}};
  EXPECT_EQ(capacity.measure(exact, progress).total, std::numeric_limits<std::int64_t>::max());
}

TEST(NativeWorld, ItemPolishAppliesTheObservedAlwaysSucceedRows) {
  PolishWorld world(5000, {{90211101, 2}, {90211201, 1}});
  const auto revision = world.progress().revision;

  // Grand weapon 0 to 1: cost 1000, one 90211101, two materials to one left.
  world.send_polish(std::to_string(world.item_ids.at(13102102)));
  const auto grand = world.read_message(169);
  ASSERT_EQ(grand.at("items").size(), 1);
  EXPECT_EQ(grand.at("items").at(0).at("item_cms_id"), 13102102);
  EXPECT_EQ(grand.at("items").at(0).at("polish_step"), 1);
  EXPECT_EQ(grand.at("items").at(0).at("durability"), 9908);
  EXPECT_EQ(grand.at("money_changes").at(0).at("change"), "-1000");
  EXPECT_EQ(grand.at("money_changes").at(0).at("result"), "4000");
  EXPECT_EQ(grand.at("stackables").at(0).at("item_cms_id"), 90211101);
  EXPECT_EQ(grand.at("stackables").at(0).at("quantity"), 1);
  EXPECT_TRUE(grand.contains("polish"));

  // Normal weapon 0 to 1: cost 200, the last 90211101 is consumed (quantity absent).
  world.send_polish(std::to_string(world.item_ids.at(11102302)));
  const auto normal = world.read_message(169);
  EXPECT_EQ(normal.at("items").at(0).at("polish_step"), 1);
  EXPECT_EQ(normal.at("money_changes").at(0).at("change"), "-200");
  EXPECT_EQ(normal.at("stackables").at(0).value("quantity", 0), 0);

  // Grand armor 0 to 1 uses the other material.
  world.send_polish(std::to_string(world.item_ids.at(12401201)));
  const auto armor = world.read_message(169);
  EXPECT_EQ(armor.at("items").at(0).at("polish_step"), 1);
  EXPECT_EQ(armor.at("money_changes").at(0).at("change"), "-1000");
  EXPECT_EQ(armor.at("stackables").at(0).at("item_cms_id"), 90211201);

  EXPECT_EQ(world.polish_step(13102102), 1);
  EXPECT_EQ(world.polish_step(11102302), 1);
  EXPECT_EQ(world.polish_step(12401201), 1);
  EXPECT_EQ(world.progress().gold, 2800);
  EXPECT_EQ(world.progress().revision, revision + 3);
}

TEST(NativeWorld, ItemPolishRefusesUnknownAndUntemperableItemsWithTheirCodes) {
  PolishWorld world(5000, {{90211101, 4}});
  world.expect_refused_unchanged("424242", x5::ec::ItemIdInvalid);
  world.expect_refused_unchanged(std::to_string(world.item_ids.at(99001)),
                                 x5::ec::ItemPolish_Invalid);
}

TEST(NativeWorld, ItemPolishChanceStepRollsAndAlwaysChargesTheCatalogCost) {
  PolishWorld world(5000, {{90211101, 3}});
  const auto weapon_id = std::to_string(world.item_ids.at(13102102));
  world.send_polish(weapon_id);
  EXPECT_EQ(world.read_message(169).at("items").at(0).at("polish_step"), 1);

  world.send_polish(weapon_id);
  const auto update = world.read_message(169);
  EXPECT_EQ(update.at("money_changes").at(0).at("change"), "-1425");
  EXPECT_EQ(update.at("stackables").at(0).value("quantity", 0), 0);
  const auto step = update.at("items").at(0).value("polish_step", 0);
  if (step == 2) {
    EXPECT_FALSE(update.at("polish").contains("result"));
  } else {
    EXPECT_EQ(step, 1);
    EXPECT_EQ(update.at("polish").at("result"), "ItemRollResult_Fail");
  }
  EXPECT_EQ(world.polish_step(13102102), step);
  EXPECT_EQ(world.progress().gold, 2575);
}

TEST(NativeWorld, ItemPolishFailureKeepsTheStepAndChargesEverything) {
  PolishWorld world(5000, {{90211101, 2}});
  const auto revision = world.progress().revision;

  world.send_polish(std::to_string(world.item_ids.at(13102106)));
  const auto update = world.read_message(169);
  ASSERT_EQ(update.at("items").size(), 1);
  EXPECT_EQ(update.at("items").at(0).at("item_cms_id"), 13102106);
  EXPECT_EQ(update.at("items").at(0).value("polish_step", 0), 0);
  EXPECT_EQ(update.at("items").at(0).at("durability"), 9908);
  EXPECT_EQ(update.at("polish").at("result"), "ItemRollResult_Fail");
  EXPECT_EQ(update.at("money_changes").at(0).at("change"), "-300");
  EXPECT_EQ(update.at("stackables").at(0).at("quantity"), 1);

  EXPECT_EQ(world.polish_step(13102106), 0);
  EXPECT_EQ(world.progress().gold, 4700);
  EXPECT_EQ(world.progress().revision, revision + 1);
}

TEST(NativeWorld, ItemPolishDestructionDeletesTheItemWithAnItemIdOnlyUpdate) {
  PolishWorld world(5000, {{90211101, 2}});
  const auto doomed_id = world.item_ids.at(13102107);

  world.send_polish(std::to_string(doomed_id));
  const auto update = world.read_message(169);
  ASSERT_EQ(update.at("items").size(), 1);
  // Only item_id: the client removes the bag item (AAC_Inventory_ApplyItemUpdate).
  EXPECT_EQ(update.at("items").at(0), nlohmann::json({{"item_id", std::to_string(doomed_id)}}));
  EXPECT_EQ(update.at("polish").at("result"), "ItemRollResult_Destruction");
  EXPECT_EQ(update.at("money_changes").at(0).at("change"), "-400");
  EXPECT_EQ(update.at("stackables").at(0).at("quantity"), 1);
  EXPECT_FALSE(world.stored_item(doomed_id));
  EXPECT_EQ(world.progress().gold, 4600);

  // No follow-up frame: the next frame answers the next request, and the item is gone.
  world.expect_refused_unchanged(std::to_string(doomed_id), x5::ec::ItemIdInvalid);
}

TEST(NativeWorld, ItemPolishAdditivesChangeTheRollAndAreChargedOnce) {
  PolishWorld world(5000, {{90211101, 4}, {90551001, 1}, {90552001, 2}});

  // +100% success turns the always-fail row into a success.
  world.send_polish(std::to_string(world.item_ids.at(13102106)), 90551001);
  const auto boosted = world.read_message(169);
  EXPECT_EQ(boosted.at("items").at(0).at("polish_step"), 1);
  EXPECT_FALSE(boosted.at("polish").contains("result"));
  ASSERT_EQ(boosted.at("stackables").size(), 2);
  EXPECT_EQ(boosted.at("stackables").at(1).at("item_cms_id"), 90551001);
  EXPECT_EQ(boosted.at("stackables").at(1).value("quantity", 0), 0);

  // The safeguard turns the always-destroyed row into a failure; the item survives.
  const auto guarded_id = world.item_ids.at(13102107);
  world.send_polish(std::to_string(guarded_id), 90552001);
  const auto guarded = world.read_message(169);
  EXPECT_EQ(guarded.at("polish").at("result"), "ItemRollResult_Fail");
  EXPECT_EQ(guarded.at("stackables").at(1).at("quantity"), 1);
  EXPECT_TRUE(world.stored_item(guarded_id));

  EXPECT_EQ(world.progress().stackables.at(90551001), 0);
  EXPECT_EQ(world.progress().stackables.at(90552001), 1);
  EXPECT_EQ(world.progress().gold, 4300);
}

TEST(NativeWorld, ItemPolishRefusesInvalidOrMissingAdditivesWithoutChange) {
  PolishWorld world(5000, {{90211101, 4}, {90211201, 1}});
  const auto weapon_id = std::to_string(world.item_ids.at(13102102));

  // A tempering stone is not an additive.
  world.send_polish(weapon_id, 90211201);
  EXPECT_EQ(world.read_message(2).at("ec"),
            x5::ec::EErrorCode_Name(x5::ec::ItemPolish_InvalidAdditiveMaterial));
  // A real additive the character does not own.
  world.send_polish(weapon_id, 90551001);
  EXPECT_EQ(world.read_message(2).at("ec"), x5::ec::EErrorCode_Name(x5::ec::ItemCountError));

  EXPECT_EQ(world.polish_step(13102102), 0);
  EXPECT_EQ(world.progress().gold, 5000);
}

TEST(NativeWorld, ItemPolishRefusesDamagedItemsAndEquippedItemsThatCouldBeDestroyed) {
  PolishWorld world(5000, {{90211101, 4}});

  const auto damaged = world.repositories.character_state->grant_item(
      world.account_id, world.character_id, 1,
      CharacterItem{0, 11102302, 700, std::nullopt, 1, 0, 0});
  ASSERT_TRUE(damaged);
  world.expect_refused_unchanged(std::to_string(damaged->id), x5::ec::ItemPolish_Damaged);

  // Worn in Weapon_2: its only row can destroy it, so it must be unequipped first.
  const auto worn = world.repositories.character_state->grant_item(
      world.account_id, world.character_id, 1,
      CharacterItem{0, 13102107, 701, x5::cms::EX5EquipSlotType_Weapon_2, 1, 10000, 3});
  ASSERT_TRUE(worn);
  world.expect_refused_unchanged(std::to_string(worn->id), x5::ec::ItemPolish_RequireUnEquip);
  EXPECT_TRUE(world.stored_item(worn->id));
}

TEST(NativeWorld, ItemPolishRefusesShortMaterialAndShortGoldWithoutChange) {
  PolishWorld without_material(5000, {});
  without_material.expect_refused_unchanged(
      std::to_string(without_material.item_ids.at(13102102)), x5::ec::ItemCountError);

  PolishWorld short_gold(999, {{90211101, 1}});
  short_gold.expect_refused_unchanged(std::to_string(short_gold.item_ids.at(13102102)),
                                      x5::ec::MoneyNotEnough);

  // The exact cost is accepted.
  short_gold.set_progress(1000, {{90211101, 1}});
  short_gold.send_polish(std::to_string(short_gold.item_ids.at(13102102)));
  EXPECT_EQ(short_gold.read_message(169).at("money_changes").at(0).at("change"), "-1000");
  EXPECT_EQ(short_gold.progress().gold, 0);
}

TEST(NativeWorld, ItemPolishOfAnEquippedItemAlsoSendsStatsAndEquipmentUpdateButUnequippedDoesNot) {
  PolishWorld world(5000, {{90211101, 1}, {90211201, 1}}, true);

  // Equipped armor suit: ItemsUpdateNoti, then (Candidate for armor) its stat refresh, then
  // EquipmentsUpdateNoti (observed 4/4 equipped polishes).
  world.send_polish(std::to_string(world.item_ids.at(12401201)));
  const auto update = world.read_message(169);
  EXPECT_EQ(update.at("items").at(0).at("polish_step"), 1);
  EXPECT_EQ(update.at("items").at(0).at("equip_slot"), "EX5EquipSlotType_ArmorSuit");
  EXPECT_EQ(world.read_message(47).at("default_source"), "SCS_Equip");
  const auto visual = world.read_message(168);
  EXPECT_EQ(visual.at("unit_id"), std::to_string(player_wire_unit_id(world.character_id)));
  ASSERT_EQ(visual.at("equipments").size(), 1);
  EXPECT_EQ(visual.at("equipments").at(0).at("item_cms_id"), 12401201);
  EXPECT_EQ(visual.at("equipments").at(0).at("slot"), "EX5EquipSlotType_ArmorSuit");

  // Unequipped weapon (observed 0/3 for unequipped pieces): no follow-up frame, so the next
  // frame read belongs to the next request.
  world.send_polish(std::to_string(world.item_ids.at(13102102)));
  EXPECT_EQ(world.read_message(169).at("items").at(0).at("polish_step"), 1);
  world.expect_refused_unchanged("424242", x5::ec::ItemIdInvalid);
  EXPECT_EQ(world.polish_step(12401201), 1);
  EXPECT_EQ(world.polish_step(13102102), 1);
}

TEST(NativeWorld, ItemPolishOfTheEquippedWeaponSendsItemsStatsAndEquipmentUpdateInOrder) {
  PolishWorld world(5000, {{90211101, 1}});
  const auto weapon_id = world.item_ids.at(kStarterWeaponCmsId);
  const auto before = world.stats_before_polish;

  world.send_polish(std::to_string(weapon_id));
  const auto items = world.read_message(169);
  EXPECT_EQ(items.at("items").at(0).at("item_cms_id"), kStarterWeaponCmsId);
  EXPECT_EQ(items.at("items").at(0).at("polish_step"), 1);

  // Observed order after an equipped polish: 169, 47 (SCS_Equip), 168.
  const auto stats = world.read_message(47);
  EXPECT_EQ(stats.at("default_source"), "SCS_Equip");
  EXPECT_EQ(stats.at("unit_id"), std::to_string(player_wire_unit_id(world.character_id)));
  std::map<int, int> changed;
  for (const auto &change : stats.at("stat_changes")) {
    changed[change.at("stat_type").get<int>()] = change.value("result", 0);
  }
  // Fixture weapon: level 6, physical 70, magic 30; step 1 is +32.5%. The unrounded weapon
  // terms 30.42 and 13.04 become 40.31 and 17.27, so the attack stats rise by 10 and 4. Only
  // those attack stats and the vitals are listed.
  EXPECT_EQ(changed.at(53), before.at("53").get<int>() + 10);
  EXPECT_EQ(changed.at(48), before.at("48").get<int>() + 4);
  EXPECT_FALSE(changed.contains(54));
  EXPECT_EQ(changed.at(69), before.at("69").get<int>());
  EXPECT_EQ(changed.at(72), before.at("72").get<int>());

  const auto visual = world.read_message(168);
  EXPECT_EQ(visual.at("equipments").at(0).at("item_cms_id"), kStarterWeaponCmsId);
}

TEST(NativeWorld, ItemPolishRefusesTheStepBeyondTheItemsIncreaseTableWithoutChange) {
  // 13102104 has a one-row increase table, 13102105 a two-row table; both have always-succeed
  // ItemPolish rows for steps 1 and 2.
  PolishWorld world(5000, {{90211101, 4}});
  for (const int cms_id : {13102104, 13102105}) {
    world.send_polish(std::to_string(world.item_ids.at(cms_id)));
    EXPECT_EQ(world.read_message(169).at("items").at(0).at("polish_step"), 1);
  }

  world.expect_refused_unchanged(std::to_string(world.item_ids.at(13102104)),
                                 x5::ec::ItemPolish_StepIsMax);
  EXPECT_EQ(world.polish_step(13102104), 1);

  world.send_polish(std::to_string(world.item_ids.at(13102105)));
  EXPECT_EQ(world.read_message(169).at("items").at(0).at("polish_step"), 2);
}

TEST(NativeWorld, ItemEquipRejectsClassSlotAndForeignItems) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  auth.create_account("equip_owner", "test-world-password-123");
  const auto login = auth.login("equip_owner", "test-world-password-123");

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("Rejecter");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(login.access_token(), create_request);
  ASSERT_EQ(created.ec(), x5::ec::Success);

  TemporaryWorldCatalog catalog;
  catalog.write_items({
      {{"id", "11101102"},
       {"equipSlot", "Weapon_1"},
       {"class", "Fight"},
       {"levelReq", "1"}},
      {{"id", "10130102"},
       {"equipSlot", "CostumeSuit"},
       {"class", "Fight"},
       {"levelReq", "1"}},
      {{"id", "90002"},
       {"equipSlot", "Weapon_1"},
       {"class", "Wild"},
       {"levelReq", "1"}},
      {{"id", "90003"},
       {"equipSlot", "ArmorHelmet"},
       {"class", "Fight"},
       {"levelReq", "1"}},
  });

  const auto character_id = repositories.characters->characters(1, 1).front().id;
  const auto foreign = repositories.character_state->grant_item(1, character_id, 1,
                                         CharacterItem{0, 90002, 5, std::nullopt, 1, 10000, 0});
  const auto wrong_slot = repositories.character_state->grant_item(1, character_id, 1,
                                            CharacterItem{0, 90003, 6, std::nullopt, 1, 10000, 0});
  ASSERT_TRUE(foreign && wrong_slot);

  ASSERT_TRUE(auth.create_account("equip_other", "test-world-password-123"));
  const auto other_account = repositories.accounts->account("equip_other");
  ASSERT_TRUE(other_account);
  Character other_character;
  other_character.account_id = other_account->id;
  other_character.world_id = 1;
  other_character.class_id = 1;
  other_character.name = "OtherItem";
  const auto other_created = repositories.characters->create(other_character);
  ASSERT_FALSE(other_created.items.empty());
  const auto other_item_id = other_created.items.front().id;

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(created.char_().char_id());
  const auto selected = auth.select(login.access_token(), select_request);
  ASSERT_EQ(selected.ec(), x5::ec::Success);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient client(server.port());
  client.send_key(selected.packet_key());
  EXPECT_EQ(client.read().opcode(), 5);
  client.send(protocol.encode(20001, nlohmann::json::object(), 0));
  EXPECT_EQ(client.read().opcode(), 175);
  EXPECT_EQ(client.read().opcode(), 1);
  read_entry_inventory(client);
  EXPECT_EQ(client.read().opcode(), 126);
  client.send(protocol.encode(20085, nlohmann::json::object(), 1));
  EXPECT_EQ(client.read().opcode(), 176);
  expect_pre_playing_combat_bootstrap(client, protocol);

  std::uint32_t sequence = 2;
  const auto expect_error = [&](nlohmann::json request, x5::ec::EErrorCode code) {
    client.send(protocol.encode(20074, request, sequence++));
    const auto response = client.read();
    SCOPED_TRACE(x5::ec::EErrorCode_Name(code));
    ASSERT_EQ(response.opcode(), 2);
    EXPECT_EQ(protocol.decode(response).at("message").at("ec"),
              x5::ec::EErrorCode_Name(code));
  };

  // Wild-only weapon on a Fight character.
  expect_error({{"item_id", std::to_string(foreign->id)}}, x5::ec::ItemClassMismatch);
  // Requested slot does not match the catalog equipSlot.
  expect_error({{"slot", "EX5EquipSlotType_Weapon_1"},
                {"item_id", std::to_string(wrong_slot->id)}},
               x5::ec::ItemEquipInvalidSlot);
  // This valid item ID belongs to a different account.
  expect_error({{"item_id", std::to_string(other_item_id)}}, x5::ec::ItemIdInvalid);

  const auto persisted = repositories.characters->characters(1, 1);
  const auto equipped_count = std::ranges::count_if(
      persisted.front().items, [](const CharacterItem &item) { return item.equip_slot == 0; });
  EXPECT_EQ(equipped_count, 1);
}

TEST(NativeWorld, SharedWorldUtilityBagCommitsAndIsRestoredOnRejoin) {
  constexpr int kPotion = 40101101;
  constexpr int kIneligibleLeather = 72000016;
  TemporaryWorldCatalog catalog;
  catalog.write_items(
      {{{"id", std::to_string(kPotion)}, {"FK_utilityBagFullFillCnt", "1000"}},
       {{"id", std::to_string(kIneligibleLeather)}, {"FK_utilityBagFullFillCnt", ""}}});
  OverworldSessions world(small_overworld_population(), {kOverworldSpawn}, catalog.open(),
                          catalog.root());
  auto &session = world.sessions.front();
  const auto load = [&] {
    return world.repositories.character_state->load_world_progress(session.account_id,
                                                                   session.character_id, 1);
  };
  const auto initial = load();
  auto owned = initial;
  ++owned.revision;
  owned.stackables[kPotion] = 3;
  owned.stackables[kIneligibleLeather] = 2;
  ASSERT_TRUE(world.repositories.character_state->save_world_progress(
      session.account_id, session.character_id, 1, initial, owned));
  world.enter(session);
  auto after_entry = owned;
  ++after_entry.revision;
  EXPECT_EQ(load(), after_entry);

  const auto set_slot = [&](int slot, int item_cms_id, int count, std::uint32_t sequence) {
    nlohmann::json request = nlohmann::json::object();
    request["slots"] = nlohmann::json::array();
    request["slots"].push_back({{"slot", slot}, {"item_cms_id", item_cms_id}, {"count", count}});
    session.client->send(world.protocol.encode(20080, request, sequence));
    return world.read_until_pong(session, sequence + 1);
  };
  const auto decoded = [&](const std::vector<Frame> &frames, std::uint16_t opcode) {
    const auto found = std::ranges::find_if(
        frames, [opcode](const Frame &frame) { return frame.opcode() == opcode; });
    return found == frames.end() ? nlohmann::json() : world.protocol.decode(*found).at("message");
  };

  // Refusals answer CommonErrorRes and leave the stored bag untouched.
  const auto over_reserved = set_slot(0, kPotion, 4, 3);
  EXPECT_EQ(decoded(over_reserved, 2).value("ec", ""), "Interaction_ConditionsNotMatch");
  EXPECT_TRUE(decoded(over_reserved, 104).is_null());
  const auto ineligible = set_slot(0, kIneligibleLeather, 1, 5);
  EXPECT_EQ(decoded(ineligible, 2).value("ec", ""), "BadRequest");
  EXPECT_EQ(load(), after_entry);

  const auto accepted = set_slot(1, kPotion, 3, 7);
  const auto response = decoded(accepted, 104);
  ASSERT_TRUE(response.is_object());
  EXPECT_EQ(response.at("bag").at("item_cms_id2"), kPotion);
  EXPECT_EQ(response.at("bag").at("count2"), 3);
  const auto committed = load();
  EXPECT_EQ(committed.revision, after_entry.revision + 1);
  EXPECT_EQ(committed.utility_slots[1], (UtilitySlot{kPotion, 3}));
  EXPECT_EQ(committed.stackables.at(kPotion), 3);

  // Re-equipping the potion already in slot 2 sends that slot three times: cleared, its current
  // contents, then the new assignment. The hosted server refused this shape with BadRequest.
  nlohmann::json reequip = nlohmann::json::object();
  reequip["slots"] = nlohmann::json::array();
  reequip["slots"].push_back({{"slot", 1}});
  reequip["slots"].push_back({{"slot", 1}, {"item_cms_id", kPotion}, {"count", 3}});
  reequip["slots"].push_back({{"slot", 1}, {"item_cms_id", kPotion}, {"count", 3}});
  session.client->send(world.protocol.encode(20080, reequip, 9));
  const auto reequipped = world.read_until_pong(session, 10);
  EXPECT_TRUE(decoded(reequipped, 2).is_null());
  const auto reequip_response = decoded(reequipped, 104);
  ASSERT_TRUE(reequip_response.is_object());
  EXPECT_EQ(reequip_response.at("bag").at("item_cms_id2"), kPotion);
  EXPECT_EQ(reequip_response.at("bag").at("count2"), 3);
  EXPECT_EQ(load(), committed);

  session.client->disconnect();
  world.connect(session);
  session.client->send(world.protocol.encode(20001, nlohmann::json::object(), 0));
  const auto rejoin = world.read_until_pong(session, 1);
  const auto restored = decoded(rejoin, 140);
  ASSERT_TRUE(restored.is_object());
  EXPECT_EQ(restored.at("bag").at("item_cms_id2"), kPotion);
  EXPECT_EQ(restored.at("bag").at("count2"), 3);
  session.client->disconnect();
}

} // namespace
} // namespace chronicles
