#include "auth/auth.hpp"
#include "world/features/character/character_stats.hpp"
#include "data/client_catalog.hpp"
#include "world/features/gm/gm_commands.hpp"
#include "world/features/gm/gm_world_commands.hpp"
#include "world/session/native_world.hpp"
#include "testing/memory_repositories.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr int kPotion = 15010403;
constexpr int kPickaxe = 11200201;
constexpr int kHousingChair = 90001;
constexpr int kUnknownItem = 99999999;
constexpr int kItemMaxStack = 1000;
constexpr std::int64_t kGoldLimit = 1'000'000;
constexpr char kPassword[] = "game-master-fixture-password";

Json item_row(int cms_id, const std::string &category, const std::string &durability = "") {
  return {{"id", std::to_string(cms_id)}, {"category", category},       {"grade", "Normal"},
          {"priceType", "Gold"},          {"priceValue", "1"},          {"refundType", "Gold"},
          {"refundValue", "1"},           {"maxDurability", durability}};
}

const ShopCatalog &test_catalog() {
  static const ShopCatalog catalog([] {
    ShopCatalogTables tables;
    tables.items = {
        item_row(kPotion, "Consumable"),
        item_row(kPickaxe, "Equipment", "2000"),
        item_row(kHousingChair, "HousingObject"),
    };
    tables.game_variables = {
        {{"name", "ItemMaxStack"}, {"value", std::to_string(kItemMaxStack)}},
        {{"name", "ResetBaseDateTime"}, {"value", "2023-01-02 05:00:00"}},
    };
    tables.money = {{{"type", "Gold"}, {"limitUserMax", std::to_string(kGoldLimit)}}};
    return tables;
  }());
  return catalog;
}

Character character_with_two_items() {
  Character character;
  character.id = 7;
  character.class_id = 1;
  character.items = {
      CharacterItem{101, kPickaxe, 0, 0, 1, 2000, 1},
      CharacterItem{102, kPickaxe, 1, std::nullopt, 1, 2000, 1},
  };
  return character;
}

WorldProgress progress_with_gold(std::int64_t gold) {
  WorldProgress progress;
  progress.revision = 4;
  progress.gold = gold;
  progress.stackables[kPotion] = 3;
  return progress;
}

GmCommandParse parse_text(const std::string &text) {
  return parse_gm_request(kGmCommandTextRequestOpcode, {{"text", text}});
}

GmCommandParse parse_structured(const Json &commands) {
  return parse_gm_request(kGmCommandRequestOpcode, {{"cmds", commands}});
}

TEST(GameMasterAccount, RoleNamesRoundTrip) {
  EXPECT_EQ(parse_account_role("player"), AccountRole::player);
  EXPECT_EQ(parse_account_role("GM"), AccountRole::game_master);
  EXPECT_EQ(parse_account_role("game_master"), AccountRole::game_master);
  EXPECT_FALSE(parse_account_role("admin"));
  EXPECT_FALSE(parse_account_role(""));
  EXPECT_EQ(account_role_name(AccountRole::player), "player");
  EXPECT_EQ(account_role_name(AccountRole::game_master), "gm");
}

TEST(GameMasterAccount, MemoryStoreChangesOnlyExistingAccounts) {
  MemoryRepositories store;
  EXPECT_FALSE(store.accounts->set_account_role("missing", AccountRole::game_master));

  Account account;
  account.name = "keeper";
  ASSERT_TRUE(store.accounts->add_account(account));
  EXPECT_EQ(store.accounts->account("keeper")->role, AccountRole::player);

  ASSERT_TRUE(store.accounts->set_account_role("keeper", AccountRole::game_master));
  EXPECT_EQ(store.accounts->account("keeper")->role, AccountRole::game_master);
}

class GameMasterAuthTest : public ::testing::Test {
protected:
  MemoryRepositories store;

  static x5::pa2c::WorldEnterRes enter(AuthService &service, const std::string &token) {
    x5::pb::authwebsocket::WorldEnterReq request;
    request.set_world_id(1);
    return service.enter(token, request);
  }

  static std::optional<Handoff> select_new_character(AuthService &service, const std::string &token,
                                                     const std::string &name) {
    x5::pb::ws::front::CharCreateReq create_request;
    create_request.set_slot(0);
    create_request.set_char_name(name);
    create_request.set_char_class(static_cast<x5::cms::EX5CharacterClass>(1));
    const auto created = service.create(token, create_request);
    if (created.ec() != x5::ec::Success) {
      return std::nullopt;
    }

    x5::pb::ws::front::CharSelectReq select_request;
    select_request.set_char_id(created.char_().char_id());
    const auto selected = service.select(token, select_request);
    return service.consume_ticket(selected.char_token());
  }
};

TEST_F(GameMasterAuthTest, LoginReportsTheRoleAndHandoffCarriesIt) {
  AuthService service(store.character_repositories());
  ASSERT_TRUE(service.create_account("player_one", kPassword));
  ASSERT_TRUE(service.create_account("keeper_one", kPassword));
  EXPECT_FALSE(service.set_account_role("nobody_here", AccountRole::game_master));
  EXPECT_FALSE(service.set_account_role("bad name!", AccountRole::game_master));
  ASSERT_TRUE(service.set_account_role("Keeper_One", AccountRole::game_master));

  const auto player = service.login("player_one", kPassword);
  const auto keeper = service.login("keeper_one", kPassword);
  ASSERT_EQ(player.ec(), x5::ec::Success);
  ASSERT_EQ(keeper.ec(), x5::ec::Success);
  EXPECT_FALSE(player.gm());
  EXPECT_TRUE(keeper.gm());

  ASSERT_EQ(enter(service, player.access_token()).error_code(), x5::ec::Success);
  ASSERT_EQ(enter(service, keeper.access_token()).error_code(), x5::ec::Success);
  const auto player_handoff = select_new_character(service, player.access_token(), "Plainhero");
  const auto keeper_handoff = select_new_character(service, keeper.access_token(), "Keeperhero");
  ASSERT_TRUE(player_handoff);
  ASSERT_TRUE(keeper_handoff);
  EXPECT_FALSE(player_handoff->game_master);
  EXPECT_TRUE(keeper_handoff->game_master);
}

TEST_F(GameMasterAuthTest, RoleSurvivesLaunchGrantAndSessionRotation) {
  AuthService service(store.character_repositories());
  ASSERT_TRUE(service.create_account("keeper_two", kPassword));
  ASSERT_TRUE(service.set_account_role("keeper_two", AccountRole::game_master));

  const auto launcher = service.login("keeper_two", kPassword, true);
  const auto grant = service.issue_launch_grant(launcher.access_token());
  ASSERT_TRUE(grant);
  const auto native = service.consume_launch_grant(*grant);
  ASSERT_TRUE(native);
  EXPECT_TRUE(native->gm());

  const auto rotated = service.rotate_session(native->access_token());
  ASSERT_TRUE(rotated);
  EXPECT_TRUE(rotated->gm());
  ASSERT_EQ(enter(service, rotated->access_token()).error_code(), x5::ec::Success);
  const auto handoff = select_new_character(service, rotated->access_token(), "Rotatedgm");
  ASSERT_TRUE(handoff);
  EXPECT_TRUE(handoff->game_master);
}

TEST_F(GameMasterAuthTest, OnlyGameMastersEnterDuringMaintenanceOrAtCapacity) {
  Config maintenance;
  maintenance.world_maintenance = true;
  AuthService closed(store.character_repositories(), maintenance);
  ASSERT_TRUE(closed.create_account("player_three", kPassword));
  ASSERT_TRUE(closed.create_account("keeper_three", kPassword));
  ASSERT_TRUE(closed.set_account_role("keeper_three", AccountRole::game_master));

  const auto player = closed.login("player_three", kPassword).access_token();
  const auto keeper = closed.login("keeper_three", kPassword).access_token();
  EXPECT_EQ(enter(closed, player).error_code(), x5::ec::WorldIsUnderMaintenance);
  EXPECT_EQ(enter(closed, keeper).error_code(), x5::ec::Success);

  Config full;
  full.world_capacity = 1;
  AuthService crowded(store.character_repositories(), full);
  auto occupant = crowded.admit_world_character();
  const auto crowded_player = crowded.login("player_three", kPassword).access_token();
  const auto crowded_keeper = crowded.login("keeper_three", kPassword).access_token();
  EXPECT_EQ(enter(crowded, crowded_player).error_code(), x5::ec::ServerOverload);
  EXPECT_EQ(enter(crowded, crowded_keeper).error_code(), x5::ec::Success);
}

TEST(GameMasterCommands, WorldCommandBoundariesRejectOverflowAndUnsupportedExtras) {
  const auto parse_text_world = [](const std::string &text) {
    return gm::parse_world_command(20009, {{"text", text}});
  };
  EXPECT_TRUE(parse_text_world("monster_kill spawned").command);
  EXPECT_TRUE(parse_text_world("damage_monster spawned 25%").command);
  EXPECT_FALSE(parse_text_world("set_gm_stat str -9223372036854775808").command);
  EXPECT_FALSE(parse_text_world("gauge me stun -2147483648").command);
  const auto structured = [](const Json &body) {
    return gm::parse_world_command(20008, {{"cmds", Json::array({body})}});
  };
  EXPECT_FALSE(
      structured({{"add_status_effect", {{"status_effect_id", 1}, {"stack_count", 99}}}}).command);
  EXPECT_FALSE(
      structured({{"execute_skill_effect", {{"cms_id", 1}, {"target_id", "17"}}}}).command);
  EXPECT_FALSE(structured({{"add_gm_stat", {{"type", 5}, {"percent", 1e100}}}}).command);
  EXPECT_FALSE(structured({{"mail_send", {{"item_cms_id", 1}, {"money_amount", 1}}}}).command);
}

TEST(GameMasterCommands, StructuredWorldVariantsFailClosed) {
  const auto structured = [](const Json &body) {
    return gm::parse_world_command(20008, {{"cmds", Json::array({body})}});
  };
  EXPECT_FALSE(
      structured({{"set_gm_stat", {{"type", 5}, {"value", 10}, {"enable", false}}}}).command);
  EXPECT_FALSE(
      structured({{"add_gm_stat", {{"type", 5}, {"value", 10}, {"percent", 0.25}}}}).command);
  EXPECT_FALSE(
      structured({{"mail_send", {{"money_amount", 1}, {"money_type", 4'294'967'296ULL}}}}).command);
  EXPECT_TRUE(gm::unsupported_command_reason(20009, {{"text", "content_reset_counter"}}));
  const auto fraction = structured({{"add_gm_stat", {{"type", 5}, {"percent", 0.001}}}});
  ASSERT_TRUE(fraction.command);
  ASSERT_TRUE(fraction.command->percent);
  EXPECT_NEAR(*fraction.command->percent, 0.1, 1e-6);
}

TEST(GameMasterCommands, StructuredWorldCommandsRoundTripThroughRecoveredSchemas) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const Json commands[] = {
      {{"monster_kill", {{"unit_id", "4026531842"}}}},
      {{"damage_monster", {{"unit_id", "4026531842"}, {"hp_per", 25}}}},
      {{"add_status_effect", {{"status_effect_id", 1}, {"stack_count", 1}}}},
      {{"set_gm_stat", {{"type", 5}, {"value", 10}, {"enable", true}}}},
      {{"add_gm_stat", {{"type", 5}, {"percent", 0.25}}}},
      {{"move_unit", {{"pos", {1.0, 2.0, 3.0}}, {"rel", true}}}},
      {{"mail_send", {{"money_amount", 5}, {"money_type", "EX5MoneyType_Gold"}}}},
      {{"accept_quest", {{"cms_id", 1}}}},
  };
  for (const auto &command : commands) {
    const auto decoded =
        protocol.decode(protocol.encode(20008, {{"cmds", Json::array({command})}}, 0))
            .at("message");
    const auto parsed = gm::parse_world_command(20008, decoded);
    EXPECT_TRUE(parsed.command) << command.dump() << ": " << parsed.error;
  }
}

TEST(GameMasterCommands, BagClearPreservesEquipmentAndUtilityReservations) {
  const auto parsed = parse_text("itembag_clear");
  ASSERT_TRUE(parsed.error.empty());
  const auto character = character_with_two_items();
  auto before = progress_with_gold(10);
  before.utility_slots[0] = {kPotion, 1};
  const auto plan = plan_gm_command(test_catalog(), parsed.commands.front(), character, before);
  ASSERT_EQ(plan.error, x5::ec::Success);
  ASSERT_EQ(plan.trade.removed_items.size(), 1U);
  EXPECT_EQ(plan.trade.removed_items.front().id, 102);
  EXPECT_EQ(plan.trade.next.stackables.at(kPotion), 1);
  EXPECT_EQ(plan.trade.next.gold, before.gold);
  EXPECT_EQ(plan.trade.storage_changes().removed_item_ids, std::vector<std::int64_t>{102});
  EXPECT_FALSE(
      parse_structured(Json::array({{{"itembag_clear", {{"equip", true}}}}})).error.empty());
  const auto stat = parse_text("set_stat_bonus str 20");
  ASSERT_TRUE(stat.error.empty());
  EXPECT_EQ(stat.commands.front().stat_type, 5);
  EXPECT_FALSE(parse_text("set_stat_bonus maxhp 20").error.empty());
  EXPECT_TRUE(parse_text("learn_all_perks").error.empty());
}

TEST(GameMasterCommands, RecognizesOnlyTheTwoGmOpcodes) {
  EXPECT_TRUE(is_gm_command_opcode(20008));
  EXPECT_TRUE(is_gm_command_opcode(20009));
  EXPECT_FALSE(is_gm_command_opcode(20010));
  EXPECT_FALSE(is_gm_command_opcode(20039));
}

TEST(GameMasterCommands, ParsesTextCommandsAndAliases) {
  const auto item = parse_text("item_add 15010403 5");
  ASSERT_TRUE(item.error.empty()) << item.error;
  ASSERT_EQ(item.commands.size(), 1U);
  EXPECT_EQ(item.commands.front().kind, GmCommandKind::item_add);
  EXPECT_EQ(item.commands.front().item_cms_id, kPotion);
  EXPECT_EQ(item.commands.front().item_count, 5);

  const auto default_count = parse_text("  AddItem\t11200201  ");
  ASSERT_TRUE(default_count.error.empty()) << default_count.error;
  EXPECT_EQ(default_count.commands.front().item_cms_id, kPickaxe);
  EXPECT_EQ(default_count.commands.front().item_count, 1);

  const auto money = parse_text("GOLD -250");
  ASSERT_TRUE(money.error.empty()) << money.error;
  EXPECT_EQ(money.commands.front().kind, GmCommandKind::add_money);
  EXPECT_EQ(money.commands.front().money_amount, -250);

  EXPECT_TRUE(parse_text("add_money 9000000000").error.empty());
}

TEST(GameMasterCommands, RejectsMalformedTextCommands) {
  EXPECT_FALSE(parse_text("").error.empty());
  EXPECT_FALSE(parse_text("   ").error.empty());
  EXPECT_FALSE(parse_text("teleport 1 2 3").error.empty());
  EXPECT_FALSE(parse_text("item_add").error.empty());
  EXPECT_FALSE(parse_text("item_add 12abc").error.empty());
  EXPECT_FALSE(parse_text("item_add -5").error.empty());
  EXPECT_FALSE(parse_text("item_add 15010403 0").error.empty());
  EXPECT_FALSE(parse_text("item_add 15010403 2 extra").error.empty());
  EXPECT_FALSE(parse_text("add_money 0").error.empty());
  EXPECT_FALSE(parse_text("add_money 1e3").error.empty());
  EXPECT_FALSE(parse_text("add_money 99999999999999999999").error.empty());
  EXPECT_FALSE(parse_text("item_add 1 " + std::string(300, '1')).error.empty());
  EXPECT_FALSE(parse_gm_request(kGmCommandTextRequestOpcode, Json::object()).error.empty());
  EXPECT_FALSE(parse_gm_request(kGmCommandTextRequestOpcode, {{"text", 5}}).error.empty());
  EXPECT_FALSE(parse_gm_request(kGmCommandTextRequestOpcode, "text").error.empty());
}

TEST(GameMasterCommands, RejectionReasonsDoNotRepeatRequestText) {
  const auto rejected = parse_text("whisper secret-message-text");
  EXPECT_EQ(rejected.error.find("secret-message-text"), std::string::npos);
}

TEST(GameMasterCommands, ParsesStructuredGmCmdOneofs) {
  const auto parsed = parse_structured(Json::array({
      {{"item_add", {{"cms_id", kPotion}, {"count", 2}}}},
      {{"item_add", {{"cms_id", kPickaxe}}}},
      {{"add_money", {{"amount", "1200"}}}},
      {{"add_money", {{"amount", "-5"}, {"type", "EX5MoneyType_Gold"}}}},
  }));
  ASSERT_TRUE(parsed.error.empty()) << parsed.error;
  ASSERT_EQ(parsed.commands.size(), 4U);
  EXPECT_EQ(parsed.commands.at(0).item_count, 2);
  // An omitted ItemAdd.count (proto default 0) grants one item.
  EXPECT_EQ(parsed.commands.at(1).item_count, 1);
  EXPECT_EQ(parsed.commands.at(2).money_amount, 1200);
  EXPECT_EQ(parsed.commands.at(3).money_amount, -5);
}

TEST(GameMasterCommands, RejectsUnsupportedOrMalformedStructuredCommands) {
  // A GM's revive button sends GmCmd.revive; it is named, not silently ignored.
  const auto revive = parse_structured(Json::array({{{"revive", Json::object()}}}));
  EXPECT_EQ(revive.error, "unsupported GmCmd field revive");

  const auto token_money = parse_structured(
      Json::array({{{"add_money", {{"amount", "5"}, {"type", "EX5MoneyType_HersanderToken"}}}}}));
  EXPECT_FALSE(token_money.error.empty());

  // One unsupported entry rejects the whole request, including valid commands before it.
  const auto mixed = parse_structured(Json::array({
      {{"item_add", {{"cms_id", kPotion}}}},
      {{"suicide", Json::object()}},
  }));
  EXPECT_FALSE(mixed.error.empty());
  EXPECT_TRUE(mixed.commands.empty());

  Json too_many = Json::array();
  for (std::size_t index = 0; index <= kMaximumGmCommandsPerRequest; ++index) {
    too_many.push_back({{"item_add", {{"cms_id", kPotion}}}});
  }
  EXPECT_FALSE(parse_structured(too_many).error.empty());
  EXPECT_FALSE(parse_structured(Json::array()).error.empty());
  EXPECT_FALSE(parse_structured(Json::array({Json::object()})).error.empty());
  EXPECT_FALSE(parse_structured(Json::array({{{"item_add", {{"cms_id", kPotion}}},
                                              {"add_money", {{"amount", "5"}}}}}))
                   .error.empty());
  EXPECT_FALSE(
      parse_structured(Json::array({{{"item_add", {{"cms_id", "not a number"}}}}})).error.empty());
  EXPECT_FALSE(parse_structured(Json::array({{{"add_money", Json::object()}}})).error.empty());
}

TEST(GameMasterCommands, ItemGrantsFollowShopStorageRules) {
  const auto character = character_with_two_items();
  const auto before = progress_with_gold(10);

  GmCommand stack{GmCommandKind::item_add, kPotion, 7, 0};
  const auto stacked = plan_gm_command(test_catalog(), stack, character, before);
  ASSERT_EQ(stacked.error, x5::ec::Success);
  EXPECT_EQ(stacked.trade.next.revision, before.revision + 1);
  EXPECT_EQ(stacked.trade.next.stackables.at(kPotion), 10);
  EXPECT_EQ(stacked.trade.next.gold, 10);
  EXPECT_TRUE(stacked.trade.money_changes.empty());
  EXPECT_TRUE(stacked.trade.granted_items.empty());

  GmCommand instances{GmCommandKind::item_add, kPickaxe, 2, 0};
  const auto granted = plan_gm_command(test_catalog(), instances, character, before);
  ASSERT_EQ(granted.error, x5::ec::Success);
  ASSERT_EQ(granted.trade.granted_items.size(), 2U);
  EXPECT_EQ(granted.trade.granted_items.at(0).inventory_slot, 2);
  EXPECT_EQ(granted.trade.granted_items.at(1).inventory_slot, 3);
  EXPECT_EQ(granted.trade.granted_items.at(0).durability, 2000);
  EXPECT_EQ(granted.trade.granted_items.at(0).id, 0);
}

TEST(GameMasterCommands, ParsesWeaponDurabilityCommands) {
  const auto text = parse_text("set_weapon_durability 150");
  ASSERT_TRUE(text.error.empty()) << text.error;
  EXPECT_EQ(text.commands.front().kind, GmCommandKind::set_weapon_durability);
  EXPECT_EQ(text.commands.front().durability, 150);
  EXPECT_TRUE(parse_text("durability 0").error.empty());
  EXPECT_FALSE(parse_text("durability -1").error.empty());
  EXPECT_FALSE(parse_text("durability").error.empty());
  EXPECT_FALSE(parse_text("durability 99999999999").error.empty());

  const auto structured =
      parse_structured(Json::array({{{"set_weapon_durability", {{"durability", 75}}}}}));
  ASSERT_TRUE(structured.error.empty()) << structured.error;
  EXPECT_EQ(structured.commands.front().durability, 75);
  EXPECT_FALSE(is_progression_gm_command(GmCommandKind::set_weapon_durability));
}

TEST(GameMasterCommands, WeaponDurabilityChangesOnlyEquippedWeaponsWithinTheirMaximum) {
  const auto character = character_with_two_items();
  const auto before = progress_with_gold(10);
  GmCommand command;
  command.kind = GmCommandKind::set_weapon_durability;

  command.durability = 150;
  const auto worn = plan_gm_command(test_catalog(), command, character, before);
  ASSERT_EQ(worn.error, x5::ec::Success);
  // Only item 101 is equipped (Weapon_1); the unequipped pickaxe keeps its durability.
  ASSERT_EQ(worn.trade.repaired_items.size(), 1U);
  EXPECT_EQ(worn.trade.repaired_items.front().id, 101);
  EXPECT_EQ(worn.trade.repaired_items.front().durability, 150);
  ASSERT_EQ(worn.trade.durability_updates.size(), 1U);
  EXPECT_EQ(worn.trade.durability_updates.front().expected_durability, 2000);
  EXPECT_EQ(worn.trade.storage_changes().durability_updates.size(), 1U);

  // A value above the catalog maximum is capped; the weapon is already there, so nothing changes.
  command.durability = 50'000;
  EXPECT_EQ(plan_gm_command(test_catalog(), command, character, before).error, x5::ec::GmCmd_Fail);

  Character unarmed = character;
  unarmed.items.front().equip_slot.reset();
  command.durability = 10;
  EXPECT_EQ(plan_gm_command(test_catalog(), command, unarmed, before).error, x5::ec::GmCmd_Fail);

  const auto packet = gm_command_packet(worn.trade, {});
  EXPECT_EQ(packet.message.at("items").at(0).at("durability"), 150);
  EXPECT_TRUE(packet.message.contains("command"));
}

TEST(GameMasterCommands, RejectsInvalidItemGrants) {
  const auto character = character_with_two_items();
  const auto before = progress_with_gold(10);
  const auto plan_item = [&](int cms_id, int count) {
    return plan_gm_command(test_catalog(), {GmCommandKind::item_add, cms_id, count, 0}, character,
                           before)
        .error;
  };

  EXPECT_EQ(plan_item(kUnknownItem, 1), x5::ec::ItemCmsIdInvalid);
  EXPECT_EQ(plan_item(kHousingChair, 1), x5::ec::ItemError);
  EXPECT_EQ(plan_item(kPotion, kItemMaxStack + 1), x5::ec::ItemCountError);
  EXPECT_EQ(plan_item(kPickaxe, kMaximumGmGrantedInstances + 1), x5::ec::ItemCountError);

  Character full = character;
  full.items.clear();
  for (int slot = 0; slot <= kMaximumInventorySlot; ++slot) {
    full.items.push_back(CharacterItem{slot + 1, kPickaxe, slot, std::nullopt, 1, 2000, 1});
  }
  EXPECT_EQ(plan_gm_command(test_catalog(), {GmCommandKind::item_add, kPickaxe, 1, 0}, full, before)
                .error,
            x5::ec::ItemSlotLimit);
}

TEST(GameMasterCommands, MoneyStaysWithinZeroAndTheCatalogLimit) {
  const auto character = character_with_two_items();
  const auto before = progress_with_gold(100);
  const auto plan_money = [&](std::int64_t amount) {
    return plan_gm_command(test_catalog(), {GmCommandKind::add_money, 0, 0, amount}, character,
                           before);
  };

  const auto added = plan_money(900);
  ASSERT_EQ(added.error, x5::ec::Success);
  EXPECT_EQ(added.trade.next.gold, 1000);
  ASSERT_EQ(added.trade.money_changes.size(), 1U);
  EXPECT_EQ(added.trade.money_changes.front().change, 900);
  EXPECT_EQ(added.trade.next.revision, before.revision + 1);

  const auto removed = plan_money(-100);
  ASSERT_EQ(removed.error, x5::ec::Success);
  EXPECT_EQ(removed.trade.next.gold, 0);

  EXPECT_EQ(plan_money(-101).error, x5::ec::MoneyNotEnough);
  EXPECT_EQ(plan_money(kGoldLimit).error, x5::ec::GmCmd_Fail);
  EXPECT_EQ(plan_money(std::numeric_limits<std::int64_t>::max()).error, x5::ec::GmCmd_Fail);

  auto exhausted = before;
  exhausted.revision = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(
      plan_gm_command(test_catalog(), {GmCommandKind::add_money, 0, 0, 1}, character, exhausted)
          .error,
      x5::ec::GmCmd_Fail);
}

TEST(GameMasterCommands, NotificationUsesTheCommandReasonAndEncodes) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto encodes = [&protocol](const GameplayPacket &packet) {
    return protocol.decode(protocol.encode(packet.opcode, packet.message)).at("message");
  };
  const auto character = character_with_two_items();
  const auto before = progress_with_gold(100);

  const auto item_plan =
      plan_gm_command(test_catalog(), {GmCommandKind::item_add, kPickaxe, 1, 0}, character, before);
  ASSERT_EQ(item_plan.error, x5::ec::Success);
  auto committed = item_plan.trade.granted_items;
  committed.front().id = 900;
  const auto item_packet = gm_command_packet(item_plan.trade, committed);
  EXPECT_EQ(item_packet.opcode, kItemsUpdateNotificationOpcode);
  const auto item_update = encodes(item_packet);
  EXPECT_TRUE(item_update.contains("command"));
  EXPECT_FALSE(item_update.contains("shop"));
  EXPECT_FALSE(item_update.contains("money_changes"));
  EXPECT_EQ(item_update.at("items").at(0).at("item_id"), "900");
  EXPECT_EQ(item_update.at("items").at(0).at("item_cms_id"), kPickaxe);

  const auto stack_plan =
      plan_gm_command(test_catalog(), {GmCommandKind::item_add, kPotion, 2, 0}, character, before);
  const auto stack_update = encodes(gm_command_packet(stack_plan.trade, {}));
  EXPECT_EQ(stack_update.at("stackables").at(0).at("item_cms_id"), kPotion);
  EXPECT_EQ(stack_update.at("stackables").at(0).at("quantity"), 5);

  const auto money_plan =
      plan_gm_command(test_catalog(), {GmCommandKind::add_money, 0, 0, 50}, character, before);
  const auto money_update = encodes(gm_command_packet(money_plan.trade, {}));
  EXPECT_TRUE(money_update.contains("command"));
  EXPECT_EQ(money_update.at("money_changes").at(0).at("change"), "50");
  EXPECT_EQ(money_update.at("money_changes").at(0).at("result"), "150");
}

TEST(GameMasterCommands, StructuredRequestsDecodeFromTheWireSchema) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const Json request{{"cmds", Json::array({{{"item_add", {{"cms_id", kPotion}, {"count", 3}}}},
                                           {{"add_money", {{"amount", "75"}}}}})}};
  const auto decoded =
      protocol.decode(protocol.encode(kGmCommandRequestOpcode, request, 0)).at("message");
  const auto parsed = parse_gm_request(kGmCommandRequestOpcode, decoded);
  ASSERT_TRUE(parsed.error.empty()) << parsed.error;
  ASSERT_EQ(parsed.commands.size(), 2U);
  EXPECT_EQ(parsed.commands.at(0).item_count, 3);
  EXPECT_EQ(parsed.commands.at(1).money_amount, 75);

  const auto revive = protocol.decode(protocol.encode(
      kGmCommandRequestOpcode, {{"cmds", Json::array({{{"revive", Json::object()}}})}}, 1));
  EXPECT_EQ(parse_gm_request(kGmCommandRequestOpcode, revive.at("message")).error,
            "unsupported GmCmd field revive");

  const auto text = protocol.decode(
      protocol.encode(kGmCommandTextRequestOpcode, {{"text", "item_add 15010403"}}, 2));
  EXPECT_TRUE(parse_gm_request(kGmCommandTextRequestOpcode, text.at("message")).error.empty());
}

TEST(GameMasterWorld, BootstrapBaseNotificationCarriesTheGmFlag) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }

  ClientCatalog catalog(catalog_path);
  const auto stats = CharacterStats::load(catalog);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  MemoryRepositories store;
  Character character;
  character.class_id = 1;
  character.name = "Keeper";
  character = store.characters->create(character);

  const auto base_notification_gm = [&](bool game_master) {
    WorldHandshake handshake(protocol, true, stats);
    handshake.set_game_master(game_master);
    EXPECT_TRUE(handshake.configure_bootstrap_character(character));
    EXPECT_TRUE(handshake.receive(protocol.encode(20001, Json::object(), 0), 120000, 190000));
    while (auto notification = handshake.take_pending_notification()) {
      if (notification->opcode() == 125) {
        return protocol.decode(*notification).at("message").value("gm", false);
      }
    }
    ADD_FAILURE() << "No WorldCharBaseNoti was queued";
    return false;
  };

  EXPECT_TRUE(base_notification_gm(true));
  EXPECT_FALSE(base_notification_gm(false));
}

} // namespace
} // namespace chronicles
