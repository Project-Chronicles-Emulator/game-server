#include "world/features/combat/combat.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t kStart = 100000000;
constexpr std::int64_t kMillisecond = 10000;
constexpr int kPotionItem = 40101101;
constexpr int kPotionSkill = 1000;
constexpr int kPotionUseEffect = 100000141;
constexpr int kPotionRecoveryEffect = 100000142;
constexpr int kElixirItem = 40301201;
constexpr int kElixirSkill = 917;
constexpr int kMaxStaminaBuff = 100597;
constexpr int kNormalSkill = 200;
const WorldPosition kPosition{8000, 0, 0, 0};

Json self_effect(const std::string &kind, Json parameters, int chain = 0) {
  return {{"kind", kind},
          {"parameters", std::move(parameters)},
          {"targeting_kind", "refTarget"},
          {"targeting", {{"refType", "EX5ReferenceTargetType_Self"}}},
          {"chain", chain}};
}

Json item_skill(int skill_id, int duration_ms, Json events, int cooldown_ms = 0,
                int cancel_flags = 70) {
  const Json stage{{"index", 0},
                   {"duration_ms", duration_ms},
                   {"next_stage", -1},
                   {"events", std::move(events)},
                   {"notifies",
                    {{{"notifyType", "cancelable"},
                      {"startTime", 0},
                      {"endTime", duration_ms},
                      {"cancelable", {{"skillTypeFlags", cancel_flags}}}}}}};
  return {{"id", skill_id},
          {"classes", {1, 2, 3, 5}},
          {"npc", false},
          {"weapon", "None"},
          {"type", "Item"},
          {"costs", Json::object()},
          {"cooldown_ms", cooldown_ms},
          {"cooldown_group", 0},
          {"range_mm", 0},
          {"require_target", false},
          {"angle", 360},
          {"layers", {{{"index", 0}, {"stages", {stage}}}}}};
}

// The catalog potion (skill 1000, recovery row 4) and stamina elixir (skill 917, status 100597)
// reduced to the fields the server reads, plus one ordinary skill without an item.
Json item_fixture(int potion_cancel_flags = 70) {
  const Json normal_stage{{"index", 0},
                          {"duration_ms", 500},
                          {"next_stage", -1},
                          {"events", Json::array()},
                          {"notifies", Json::array()}};
  const Json normal{{"id", kNormalSkill},   {"classes", {1, 2, 3, 5}},
                    {"npc", false},         {"weapon", "None"},
                    {"type", "Normal"},     {"costs", Json::object()},
                    {"cooldown_ms", 0},     {"cooldown_group", 0},
                    {"range_mm", 5000},     {"require_target", false},
                    {"angle", 360},         {"layers", {{{"index", 0}, {"stages", {normal_stage}}}}}};
  const Json potion = item_skill(
      kPotionSkill, 2881, {{{"effect_id", kPotionUseEffect}, {"unique_id", 0}, {"time_ms", 435}}},
      0, potion_cancel_flags);
  const Json elixir = item_skill(kElixirSkill, 2833,
                                 {{{"effect_id", 101002}, {"unique_id", 0}, {"time_ms", 1343}},
                                  {{"effect_id", 91700}, {"unique_id", 1}, {"time_ms", 1600}}},
                                 4000);
  return {
      {"format_version", 1},
      {"skills", {normal, potion, elixir}},
      {"effects",
       {{std::to_string(kPotionUseEffect),
         self_effect("useItem", Json::object(), kPotionRecoveryEffect)},
        {std::to_string(kPotionRecoveryEffect),
         self_effect("statRecovery", {{"statRecoveryCmsId", 4}})},
        {"101002", self_effect("useItem", Json::object())},
        {"91700",
         self_effect("addStatusEffect", {{"statusEffectId", static_cast<double>(kMaxStaminaBuff)},
                                         {"stackCount", 1},
                                         {"rate", 100.0},
                                         {"level", 1}})}}},
      {"projectiles", Json::object()},
      {"npc_stats", Json::object()},
      {"npc_skills", Json::object()},
      {"weapon_items", Json::object()},
      {"item_skills",
       {{std::to_string(kPotionItem), kPotionSkill}, {std::to_string(kElixirItem), kElixirSkill}}},
      {"recoveries",
       {{"4",
         {{"stat", 69},
          {"interval_ms", 100},
          {"amount", 20.0},
          {"duration_ms", 1000},
          {"use_potion_heal_rate", true},
          {"cancel_type", "EndSkillStage"}}}}},
      {"status_effects",
       {{std::to_string(kMaxStaminaBuff),
         {{"duration_ms", 60000},
          {"stack_rule", "ResetDuration"},
          {"stat_modifiers", {{{"stat", "MaxSP"}, {"add", 200}, {"mul", 0}}}}}}}}};
}

std::shared_ptr<TutorialData> empty_tutorial() {
  return std::make_shared<TutorialData>(
      Json{{"format_version", 1},
           {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
           {"map_id", 8000},
           {"objects", Json::array()},
           {"doodads", Json::array()},
           {"utility_items", Json::array()},
           {"civilians", Json::array()},
           {"sectors", {1}},
           {"npc_spawns", Json::array()},
           {"quests", Json::array()}});
}

Character player() {
  Character result;
  result.id = 7;
  result.class_id = 1;
  return result;
}

// Regeneration stats are zero so that only item effects change the vitals.
Json player_stats() {
  return {{"18", 100}, {"19", 50}, {"69", 100}, {"72", 50},
          {"22", 60},  {"17", 10}, {"15", 0},   {"25", 0}};
}

// A stack inventory in memory whose spends are recorded like the tutorial session's commits.
struct TestInventory {
  std::map<int, std::int64_t> stacks;
  int spends{};

  ItemInventoryAccess access() {
    ItemInventoryAccess result;
    result.quantity = [this](int item_cms_id) {
      const auto stack = stacks.find(item_cms_id);
      return stack == stacks.end() ? std::int64_t{0} : stack->second;
    };
    result.consume_one = [this](int item_cms_id) {
      const auto remaining = --stacks.at(item_cms_id);
      ++spends;
      return std::vector<GameplayPacket>{
          {169,
           {{"stackables", {{{"item_cms_id", item_cms_id}, {"quantity", remaining}}}},
            {"utility_bag", Json::object()}}}};
    };
    return result;
  }
};

struct ItemUseHarness {
  explicit ItemUseHarness(Json definitions = item_fixture(),
                          std::shared_ptr<TutorialData> data = empty_tutorial(),
                          Json stats = player_stats())
      : tutorial_data(std::move(data)),
        tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; }),
        combat(std::make_shared<CombatData>(definitions), tutorial_data, player(),
               std::move(stats)) {
    combat.set_item_inventory(inventory.access());
  }

  // Raises the maxima without healing, so that the player has missing health and stamina.
  void wound(int maximum_health, int maximum_stamina) {
    auto stats = player_stats();
    stats["18"] = maximum_health;
    stats["19"] = maximum_stamina;
    combat.update_player_stats(stats);
  }

  std::vector<GameplayPacket> use(int skill_id, int item_cms_id, std::uint32_t instance,
                                  std::int64_t at) {
    Json request{{"start_skill",
                  {{"skill_id", skill_id},
                   {"instance_id", instance},
                   {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
                   {"rotation", {{"yaw", 0}}},
                   {"tick", "1"}}}};
    if (item_cms_id != 0) {
      request["use_item"] = {{"item_cms_id", item_cms_id}};
    }
    EXPECT_TRUE(combat.handles(20010, request));
    return combat.receive(20010, request, kPosition, at);
  }

  std::vector<GameplayPacket> advance_to(std::int64_t at) {
    return combat.advance(at, kPosition, tutorial);
  }

  int stat(int stat_type) const {
    return combat.player_stats().value(std::to_string(stat_type), 0);
  }

  std::shared_ptr<TutorialData> tutorial_data;
  TutorialSession tutorial;
  TestInventory inventory;
  CombatSession combat;
};

std::vector<std::uint16_t> opcodes(const std::vector<GameplayPacket> &packets) {
  std::vector<std::uint16_t> result;
  for (const auto &packet : packets) {
    result.push_back(packet.opcode);
  }
  return result;
}

const GameplayPacket *find_opcode(const std::vector<GameplayPacket> &packets,
                                  std::uint16_t opcode) {
  for (const auto &packet : packets) {
    if (packet.opcode == opcode) {
      return &packet;
    }
  }
  return nullptr;
}

std::string refusal(const std::vector<GameplayPacket> &packets) {
  const auto *failure = find_opcode(packets, 13);
  return failure ? failure->message.at("ec").get<std::string>() : std::string("accepted");
}

void verify_encoding(const std::vector<GameplayPacket> &packets) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : packets) {
    const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
    EXPECT_EQ(decoded.at("status"), "decoded") << packet.opcode << " " << packet.message.dump();
  }
}

} // namespace

TEST(CombatItemUse, PotionSpendsOneItemAtItsEventAndHealsThroughTheOfficialRecoveryFlow) {
  ItemUseHarness harness;
  harness.inventory.stacks[kPotionItem] = 2;
  harness.wound(400, 50);
  ASSERT_EQ(harness.stat(69), 100);

  const auto started = harness.use(kPotionSkill, kPotionItem, 1, kStart);
  EXPECT_EQ(opcodes(started), (std::vector<std::uint16_t>{53, 14}));
  EXPECT_EQ(harness.inventory.spends, 0) << "the item is spent at its effect, not at the start";
  EXPECT_TRUE(harness.advance_to(kStart + 434 * kMillisecond).empty());

  // Official order at the consumption event: the stack update, the flow, then the recovery.
  const auto consumed = harness.advance_to(kStart + 435 * kMillisecond);
  ASSERT_EQ(opcodes(consumed), (std::vector<std::uint16_t>{169, 54, 80}));
  EXPECT_EQ(harness.inventory.stacks.at(kPotionItem), 1);
  const auto &flow = consumed.at(1).message.at("flows").at(0);
  EXPECT_EQ(flow.at("source"), "SCS_StatRecoveryFlow");
  EXPECT_EQ(flow.at("stat_type"), 69);
  EXPECT_DOUBLE_EQ(flow.at("rate_per_sec").get<double>(), 200.0);
  EXPECT_EQ(flow.at("checkpoint").at("result"), 100);
  const auto &recovery = consumed.at(2).message;
  EXPECT_EQ(recovery.at("skill_effect_id"), kPotionRecoveryEffect);
  EXPECT_EQ(recovery.at("stat_recovery").at("cms_id"), 4);
  EXPECT_EQ(recovery.at("stat_recovery").at("instance_id"), flow.at("instance_id"));
  EXPECT_DOUBLE_EQ(recovery.at("stat_recovery").at("recovery_value_per_sec").get<double>(), 200.0);
  const auto start_ns = std::stoll(recovery.at("stat_recovery").at("start_tick").get<std::string>());
  const auto end_ns = std::stoll(recovery.at("stat_recovery").at("end_tick").get<std::string>());
  EXPECT_EQ(end_ns - start_ns, 1000LL * 1000000LL) << "catalog duration in nanoseconds";
  verify_encoding(consumed);

  // The client simulates the flow, so the server heals silently: half the flow after 500 ms.
  EXPECT_TRUE(find_opcode(harness.advance_to(kStart + 935 * kMillisecond), 47) == nullptr);
  EXPECT_EQ(harness.stat(69), 200);
  harness.advance_to(kStart + 1500 * kMillisecond);
  EXPECT_EQ(harness.stat(69), 300);
  harness.advance_to(kStart + 2000 * kMillisecond);
  EXPECT_EQ(harness.stat(69), 300) << "the recovery ends after its catalog duration";

  const auto ended = harness.advance_to(kStart + 2881 * kMillisecond);
  EXPECT_EQ(opcodes(ended), (std::vector<std::uint16_t>{15}));
  EXPECT_EQ(harness.inventory.spends, 1);
}

TEST(CombatItemUse, RecoveryNeverRaisesHealthAboveItsMaximum) {
  ItemUseHarness harness;
  harness.inventory.stacks[kPotionItem] = 1;
  harness.wound(150, 50);

  harness.use(kPotionSkill, kPotionItem, 1, kStart);
  harness.advance_to(kStart + 435 * kMillisecond);
  harness.advance_to(kStart + 2000 * kMillisecond);
  EXPECT_EQ(harness.stat(69), 150);
}

TEST(CombatItemUse, PotionHealRateScalesTheRecovery) {
  auto stats = player_stats();
  stats["18"] = 1000;
  stats["91"] = 1500;
  ItemUseHarness harness;
  harness.combat.update_player_stats(stats);
  harness.inventory.stacks[kPotionItem] = 1;

  harness.use(kPotionSkill, kPotionItem, 1, kStart);
  const auto consumed = harness.advance_to(kStart + 435 * kMillisecond);
  const auto *recovery = find_opcode(consumed, 80);
  ASSERT_NE(recovery, nullptr);
  EXPECT_DOUBLE_EQ(
      recovery->message.at("stat_recovery").at("recovery_value_per_sec").get<double>(), 300.0);
  harness.advance_to(kStart + 2000 * kMillisecond);
  EXPECT_EQ(harness.stat(69), 400);
}

TEST(CombatItemUse, ItemSkillsRequireTheirOwnOwnedStackableItem) {
  ItemUseHarness harness;
  harness.inventory.stacks[kPotionItem] = 0;
  harness.inventory.stacks[kElixirItem] = 1;

  EXPECT_EQ(refusal(harness.use(kPotionSkill, 0, 1, kStart)), "Skill_InvalidUseItem");
  EXPECT_EQ(refusal(harness.use(kPotionSkill, 12345, 2, kStart)), "Skill_UnusableItem");
  EXPECT_EQ(refusal(harness.use(kPotionSkill, kElixirItem, 3, kStart)), "Skill_UnusableItem");
  EXPECT_EQ(refusal(harness.use(kNormalSkill, kPotionItem, 4, kStart)), "Skill_UnusableItem");
  EXPECT_EQ(refusal(harness.use(kPotionSkill, kPotionItem, 5, kStart)), "Skill_NotFoundUseItem");

  harness.inventory.stacks[kPotionItem] = 1;
  Json instance_request{{"start_skill",
                         {{"skill_id", kPotionSkill},
                          {"instance_id", 6},
                          {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}}},
                          {"tick", "1"}}},
                        {"use_item", {{"item_cms_id", kPotionItem}, {"item_id", "99"}}}};
  EXPECT_EQ(refusal(harness.combat.receive(20010, instance_request, kPosition, kStart)),
            "Skill_InvalidUseItem");
  EXPECT_EQ(harness.inventory.spends, 0);
  EXPECT_EQ(refusal(harness.use(kPotionSkill, kPotionItem, 7, kStart)), "accepted");
}

TEST(CombatItemUse, SharedWorldRejectsEveryEmptyItemSkillBeforeStartingAnAction) {
  const auto definitions = std::make_shared<CombatData>(item_fixture());
  TestInventory inventory;
  CombatSession combat(definitions, nullptr, player(), player_stats());
  combat.set_item_inventory(inventory.access());
  combat.set_player_map(9012);
  const WorldPosition position{9012, 0, 0, 0};

  for (const auto &[item_id, skill_id] : definitions->item_skills) {
    SCOPED_TRACE(item_id);
    const Json request{{"start_skill", {{"skill_id", skill_id}, {"instance_id", 1}}},
                       {"use_item", {{"item_cms_id", item_id}}}};
    const auto packets = combat.receive(20010, request, position, kStart);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].opcode, 13);
    EXPECT_EQ(packets[0].message.at("ec"), "Skill_NotFoundUseItem");
    EXPECT_EQ(packets[0].message.at("instance_id"), 1);
    EXPECT_FALSE(combat.busy());
    EXPECT_EQ(inventory.spends, 0);
  }
}

TEST(CombatItemUse, EmptySharedMapConsumesTheLastPotionAndRefusesTheNextAnimation) {
  TestInventory inventory;
  inventory.stacks[kPotionItem] = 1;
  CombatSession combat(std::make_shared<CombatData>(item_fixture()), nullptr, player(),
                       player_stats());
  combat.set_item_inventory(inventory.access());
  combat.set_player_map(9012);
  const WorldPosition position{9012, 0, 0, 0};
  auto stats = player_stats();
  stats["18"] = 400;
  combat.update_player_stats(stats);
  Json request{
      {"start_skill",
       {{"skill_id", kPotionSkill}, {"instance_id", 1}, {"pos", Json::object()}, {"tick", "1"}}},
      {"use_item", {{"item_cms_id", kPotionItem}}}};

  EXPECT_EQ(refusal(combat.receive(20010, request, position, kStart)), "accepted");
  const auto consumed = combat.advance_private_encounter(kStart + 435 * kMillisecond, position);
  EXPECT_NE(find_opcode(consumed, 169), nullptr);
  EXPECT_NE(find_opcode(consumed, 80), nullptr);
  EXPECT_EQ(inventory.spends, 1);
  EXPECT_EQ(inventory.stacks[kPotionItem], 0);
  combat.advance_private_encounter(kStart + 3000 * kMillisecond, position);
  EXPECT_FALSE(combat.busy());
  EXPECT_EQ(combat.player_stats().at("69"), 300);

  request["start_skill"]["instance_id"] = 2;
  const auto refused = combat.receive(20010, request, position, kStart + 4000 * kMillisecond);
  EXPECT_EQ(opcodes(refused), (std::vector<std::uint16_t>{13}));
  EXPECT_EQ(refusal(refused), "Skill_NotFoundUseItem");
  EXPECT_EQ(inventory.spends, 1);
}

TEST(CombatItemUse, ItemUseStaysWithTheTutorialSessionUntilAnInventoryIsConnected) {
  const auto tutorial_data = empty_tutorial();
  CombatSession combat(std::make_shared<CombatData>(item_fixture()), tutorial_data, player(),
                       player_stats());
  const Json drink{{"start_skill", {{"skill_id", kPotionSkill}}},
                   {"use_item", {{"item_cms_id", kPotionItem}}}};
  const Json report{{"skill_effect", {{"effect_id", kPotionUseEffect}}}};
  EXPECT_FALSE(combat.handles(20010, drink));
  EXPECT_FALSE(combat.handles(20011, report));

  TestInventory inventory;
  combat.set_item_inventory(inventory.access());
  EXPECT_TRUE(combat.handles(20010, drink));
  EXPECT_TRUE(combat.handles(20011, report));
}

TEST(CombatItemUse, AnItemThatLeftTheInventoryEndsTheActionWithoutAnyEffect) {
  ItemUseHarness harness;
  harness.inventory.stacks[kPotionItem] = 1;
  harness.wound(400, 50);

  ASSERT_EQ(refusal(harness.use(kPotionSkill, kPotionItem, 1, kStart)), "accepted");
  harness.inventory.stacks[kPotionItem] = 0;
  const auto packets = harness.advance_to(kStart + 435 * kMillisecond);
  EXPECT_EQ(opcodes(packets), (std::vector<std::uint16_t>{15}));
  EXPECT_EQ(harness.inventory.spends, 0);
  harness.advance_to(kStart + 2000 * kMillisecond);
  EXPECT_EQ(harness.stat(69), 100);
  EXPECT_FALSE(harness.combat.busy());
}

TEST(CombatItemUse, CancellingTheDrinkStopsItsEndSkillStageRecovery) {
  // Let an ordinary skill cancel the drink (flag 1) to exercise the catalog cancelType.
  ItemUseHarness harness(item_fixture(1));
  harness.inventory.stacks[kPotionItem] = 1;
  harness.wound(400, 50);

  harness.use(kPotionSkill, kPotionItem, 1, kStart);
  harness.advance_to(kStart + 435 * kMillisecond);
  const auto cancelled = harness.use(kNormalSkill, 0, 2, kStart + 600 * kMillisecond);
  ASSERT_EQ(refusal(cancelled), "accepted");
  const auto *stop = find_opcode(cancelled, 54);
  ASSERT_NE(stop, nullptr);
  const auto &flow = stop->message.at("flows").at(0);
  EXPECT_FALSE(flow.contains("rate_per_sec"));
  EXPECT_EQ(flow.at("checkpoint").at("result"), 133);
  verify_encoding(cancelled);

  harness.advance_to(kStart + 2000 * kMillisecond);
  EXPECT_EQ(harness.stat(69), 133) << "33 points accrued in the 165 ms before the cancel";
}

TEST(CombatItemUse, StaminaElixirRaisesMaximumStaminaWhileItsStatusIsActive) {
  ItemUseHarness harness;
  harness.inventory.stacks[kElixirItem] = 2;

  harness.use(kElixirSkill, kElixirItem, 1, kStart);
  const auto spent = harness.advance_to(kStart + 1343 * kMillisecond);
  EXPECT_EQ(opcodes(spent), (std::vector<std::uint16_t>{169}));

  // Observed for status 100597: the stat change (maximum, then current stamina) precedes 49.
  const auto buffed = harness.advance_to(kStart + 1600 * kMillisecond);
  ASSERT_EQ(opcodes(buffed), (std::vector<std::uint16_t>{47, 49}));
  const auto &change = buffed.at(0).message;
  EXPECT_EQ(change.at("default_source"), "SCS_StatusEffect");
  EXPECT_EQ(change.at("stat_changes"),
            (Json{{{"stat_type", 19}, {"result", 250}}, {{"stat_type", 72}, {"result", 50}}}));
  EXPECT_EQ(buffed.at(1).message.at("status_effect").at("status_effect_id"), kMaxStaminaBuff);
  EXPECT_EQ(harness.stat(19), 250);
  verify_encoding(buffed);

  // The 4 s cooldown is enforced; a second elixir after it refreshes the ResetDuration status.
  harness.advance_to(kStart + 2833 * kMillisecond);
  EXPECT_EQ(refusal(harness.use(kElixirSkill, kElixirItem, 2, kStart + 3000 * kMillisecond)),
            "Skill_Cooldown");
  harness.use(kElixirSkill, kElixirItem, 3, kStart + 4100 * kMillisecond);
  harness.advance_to(kStart + 5443 * kMillisecond);
  const auto refreshed = harness.advance_to(kStart + 5700 * kMillisecond);
  ASSERT_EQ(opcodes(refreshed), (std::vector<std::uint16_t>{50}));
  EXPECT_EQ(harness.stat(19), 250) << "a refresh never stacks the modifier";
  verify_encoding(refreshed);
  EXPECT_EQ(opcodes(harness.advance_to(kStart + (4100 + 2833) * kMillisecond)),
            (std::vector<std::uint16_t>{15}));

  // At expiry the maximum returns first and the current stamina is clamped to it.
  const auto removed = harness.advance_to(kStart + (5700 + 60000) * kMillisecond);
  ASSERT_EQ(opcodes(removed), (std::vector<std::uint16_t>{47, 51}));
  EXPECT_EQ(removed.at(0).message.at("stat_changes"),
            (Json{{{"stat_type", 19}, {"result", 50}}, {{"stat_type", 72}, {"result", 50}}}));
  EXPECT_EQ(harness.stat(19), 50);
  EXPECT_EQ(harness.inventory.stacks.at(kElixirItem), 0);
}

TEST(CombatItemUse, EquipmentRefreshKeepsActiveStatusModifiers) {
  ItemUseHarness harness;
  harness.inventory.stacks[kElixirItem] = 1;
  harness.use(kElixirSkill, kElixirItem, 1, kStart);
  harness.advance_to(kStart + 1600 * kMillisecond);
  ASSERT_EQ(harness.stat(19), 250);

  auto stats = player_stats();
  stats["19"] = 80;
  harness.combat.update_player_stats(stats);
  EXPECT_EQ(harness.stat(19), 280);
}

TEST(CombatWeapons, SelectedSecondWeaponControlsSkillValidationAndNotifications) {
  auto definitions = item_fixture();
  definitions["weapon_items"] = {{"101", "Sword"}, {"102", "Bow"}};
  definitions["skills"][0]["weapon"] = "Bow";
  ItemUseHarness harness(definitions);
  auto character = player();
  character.items = {{1, 101, 0, 0}, {2, 102, 1, 1}};
  harness.combat.update_player_weapon(character);

  const auto refused = harness.use(kNormalSkill, 0, 1, kStart);
  ASSERT_EQ(opcodes(refused), (std::vector<std::uint16_t>{13}));
  EXPECT_EQ(refused.front().message.at("weapon_slot_type"), 0);

  character.build.active_weapon_slot = 1;
  harness.combat.update_player_weapon(character);
  const auto accepted = harness.use(kNormalSkill, 0, 2, kStart + kMillisecond);
  const auto *started = find_opcode(accepted, 14);
  ASSERT_NE(started, nullptr);
  EXPECT_EQ(started->message.at("weapon_slot_type"), 1);
  EXPECT_EQ(find_opcode(accepted, 13), nullptr);
  verify_encoding(accepted);

  harness.advance_to(kStart + 600 * kMillisecond);
  character.build.active_weapon_slot = 0;
  harness.combat.update_player_weapon(character);
  const auto refused_again = harness.use(kNormalSkill, 0, 3, kStart + 700 * kMillisecond);
  ASSERT_EQ(opcodes(refused_again), (std::vector<std::uint16_t>{13}));
  EXPECT_EQ(refused_again.front().message.at("weapon_slot_type"), 0);
}

TEST(CombatItemUse, ProfilesWithInvalidItemDataAreRejected) {
  {
    auto definitions = item_fixture();
    definitions["item_skills"][std::to_string(kPotionItem)] = kNormalSkill;
    EXPECT_THROW(CombatData{definitions}, std::runtime_error) << "an item names a Normal skill";
  }
  {
    auto definitions = item_fixture();
    definitions["item_skills"]["77"] = 4242;
    EXPECT_THROW(CombatData{definitions}, std::runtime_error) << "an item names no skill";
  }
  {
    auto definitions = item_fixture();
    definitions["status_effects"][std::to_string(kMaxStaminaBuff)]["stat_modifiers"][0]["stat"] =
        "NotAStat";
    EXPECT_THROW(CombatData{definitions}, std::runtime_error);
  }
  {
    auto definitions = item_fixture();
    definitions["recoveries"]["4"]["interval_ms"] = 0;
    EXPECT_THROW(CombatData{definitions}, std::runtime_error);
  }
  {
    // Profiles exported before item skills keep loading.
    auto definitions = item_fixture();
    definitions.erase("item_skills");
    definitions["recoveries"]["4"].erase("use_potion_heal_rate");
    definitions["recoveries"]["4"].erase("cancel_type");
    EXPECT_NO_THROW(CombatData{definitions});
  }
}

namespace {

constexpr std::uint64_t kMonster = 2305843009213698050;
constexpr int kImmunityPotionItem = 76;
constexpr int kImmunityPotionSkill = 911;
constexpr int kBalmItem = 40205301;
constexpr int kBalmSkill = 1055;
constexpr int kBurnSelfSkill = 300;
constexpr int kBurningStrikeSkill = 301;
constexpr int kAttackFoodItem = 90101003;
constexpr int kBurningAilment = 12;
constexpr int kBurningImmunity = 91100;
constexpr int kArmorBreak = 100557;
constexpr int kArea = 100556;

Json timed_skill(int skill_id, const std::string &type, Json events) {
  auto skill = item_skill(skill_id, 100, std::move(events));
  skill["type"] = type;
  skill["range_mm"] = 5000;
  return skill;
}

Json event_at(int effect_id, int unique_id, int time_ms) {
  return {{"effect_id", effect_id}, {"unique_id", unique_id}, {"time_ms", time_ms}};
}

// The catalog item effects the item tests exercise, reduced to the fields the server reads:
// a burning cure with immunity (item 76), a physical armor-break balm (item 40205301), a test
// skill that burns its user, a burning strike and the FGT attack food (item 90101003).
Json effect_fixture() {
  auto definitions = item_fixture();
  auto &skills = definitions["skills"];
  skills.push_back(timed_skill(kImmunityPotionSkill, "Item",
                               {event_at(101002, 0, 0), event_at(100770, 1, 10),
                                event_at(91100, 2, 20)}));
  skills.push_back(
      timed_skill(kBalmSkill, "Item", {event_at(100558, 0, 0), event_at(100556, 1, 10)}));
  skills.push_back(timed_skill(kBurnSelfSkill, "Normal", Json::array({event_at(9001, 0, 0)})));
  skills.push_back(
      timed_skill(kBurningStrikeSkill, "Normal", Json::array({event_at(9002, 0, 0)})));

  auto &effects = definitions["effects"];
  effects["100770"] = self_effect("addAbnormalGauge", {{"burningAdd", -20000.0}});
  effects["91100"] = self_effect("addStatusEffect", {{"statusEffectId", 91100.0}, {"rate", 100.0}});
  effects["100558"] = self_effect("useItem", Json::object());
  effects["100556"] = {{"kind", "spawnAreaOfEffect"},
                       {"parameters", {{"areaOfEffectId", 100556.0}}},
                       {"targeting_kind", "refTarget"},
                       {"targeting", {{"refType", "EX5ReferenceTargetType_SkillMain"}}},
                       {"targeting_purpose", {{"targetingPosType", "EX5TargetingPosType_Caster"}}},
                       {"chain", 0}};
  effects["100800"] = {{"kind", "damage"},
                       {"parameters", {{"physicalCoefficient", 0.0}}},
                       {"targeting_kind", "refTarget"},
                       {"targeting",
                        {{"maxTargetCount", 5.0},
                         {"refType", "EX5ReferenceTargetType_SkillObjectDriven"}}},
                       {"status_effect_id", kArmorBreak},
                       {"chain", 0}};
  effects["9001"] = self_effect("addAbnormalGauge", {{"burningAdd", 15000.0}});
  effects["9002"] = {{"kind", "damage"},
                     {"parameters",
                      {{"physicalCoefficient", 1.0}, {"abnormalStatusBurningAttack", 3000.0}}},
                     {"targeting_kind", "shapeCylinderSector"},
                     {"targeting",
                      {{"outerRadius", 300}, {"halfHeight", 200}, {"angle", 120},
                       {"maxTargetCount", 1}}},
                     {"chain", 0}};
  effects["7000"] = self_effect("addStatusEffect", {{"statusEffectId", 7000.0}, {"rate", 100.0}});

  auto &statuses = definitions["status_effects"];
  statuses[std::to_string(kBurningAilment)] = {{"duration_ms", 4000},
                                               {"stack_rule", "ResetDuration"}};
  statuses[std::to_string(kBurningImmunity)] = {{"duration_ms", 5000},
                                                {"stack_rule", "ResetDuration"},
                                                {"immune_types", Json::array({"Burning"})}};
  statuses[std::to_string(kArmorBreak)] = {
      {"duration_ms", 20000},
      {"stack_rule", "Stacking"},
      {"stat_modifiers", {{{"stat", "PhysicalArmor"}, {"add", -100}, {"mul", 0}}}}};
  statuses["7000"] = {
      {"duration_ms", 600000},
      {"stack_rule", "ResetDuration"},
      {"stat_modifiers",
       {{{"stat", "PhysicalAttackPower"}, {"add", 0}, {"mul", 200}},
        {{"stat", "MagicAttackPower"}, {"add", 0}, {"mul", 200}}}}};

  definitions["item_skills"][std::to_string(kImmunityPotionItem)] = kImmunityPotionSkill;
  definitions["item_skills"][std::to_string(kBalmItem)] = kBalmSkill;
  definitions["item_effect_lists"] = {{std::to_string(kAttackFoodItem), Json::array({7000})}};
  definitions["areas"] = {{std::to_string(kArea),
                           {{"duration_ms", 20000},
                            {"interval_ms", 1000},
                            {"interval_effect_id", 100800},
                            {"radius_mm", 3000},
                            {"half_height_mm", 1000},
                            {"max_targets", 0}}}};
  definitions["abnormal_gauges"] = {
      {"reduce_tick_ms", 100},
      {"types",
       {{"Burning", {{"maximum", 20000}, {"reduce_per_tick", 40}, {"status_effect_id", 12}}}}}};
  definitions["npc_stats"] = {{"5001015", {{"18", 5000}, {"69", 5000}, {"52", 30}, {"47", 30}}}};
  return definitions;
}

std::shared_ptr<TutorialData> tutorial_with_monster() {
  auto document = empty_tutorial()->document;
  document["npc_spawns"] = {{{"unit_id", std::to_string(kMonster)},
                             {"cms_id", 5001015},
                             {"behavior", "Monster"},
                             {"faction", "EX5Faction_Bandit"},
                             {"position", {1000, 0, 0}},
                             {"yaw", 0},
                             {"spawner_guid", {"1", "2"}},
                             {"anim_stance_key", 0},
                             {"initially_active", true}}};
  return std::make_shared<TutorialData>(document);
}

std::vector<GameplayPacket> with_opcode(const std::vector<GameplayPacket> &packets,
                                        std::uint16_t opcode) {
  std::vector<GameplayPacket> result;
  for (const auto &packet : packets) {
    if (packet.opcode == opcode) {
      result.push_back(packet);
    }
  }
  return result;
}

} // namespace

TEST(CombatItemEffects, AFullStunGaugeMarksThePlayerStunnedUntilTheAilmentExpires) {
  constexpr int kStunSelfSkill = 302;
  constexpr int kStunAilment = 13;
  constexpr int kStunAilmentMs = 2000;

  auto definitions = effect_fixture();
  definitions["skills"].push_back(
      timed_skill(kStunSelfSkill, "Normal", Json::array({event_at(9003, 0, 0)})));
  definitions["effects"]["9003"] = self_effect("addAbnormalGauge", {{"stunAdd", 20000.0}});
  definitions["status_effects"][std::to_string(kStunAilment)] = {{"duration_ms", kStunAilmentMs},
                                                                 {"stack_rule", "ResetDuration"}};
  definitions["abnormal_gauges"]["types"]["Stun"] = {
      {"maximum", 20000}, {"reduce_per_tick", 40}, {"status_effect_id", kStunAilment}};
  ItemUseHarness harness(definitions);

  EXPECT_FALSE(harness.combat.player_stunned(kStart));
  harness.use(kStunSelfSkill, 0, 1, kStart);
  const auto stunned = harness.advance_to(kStart);
  const auto applied = with_opcode(stunned, 49);
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_EQ(applied.front().message.at("status_effect").at("status_effect_id"), kStunAilment);
  EXPECT_TRUE(harness.combat.player_stunned(kStart + 100 * kMillisecond));

  const auto expired_at = kStart + (kStunAilmentMs + 100) * kMillisecond;
  harness.advance_to(expired_at);
  EXPECT_FALSE(harness.combat.player_stunned(expired_at));
}

TEST(CombatItemEffects, SmallAirBubbleAppliesItsAuthoredStaminaRecovery) {
  auto definitions = effect_fixture();
  definitions["effects"]["11152"] = {
      {"kind", "statRecovery"}, {"parameters", {{"statRecoveryCmsId", 42}}},
      {"targeting_kind", "refTarget"},
      {"targeting", {{"refType", "EX5ReferenceTargetType_InteractableUnit"}}}, {"chain", 0}};
  definitions["recoveries"]["42"] = {
      {"stat", 72}, {"interval_ms", 250}, {"amount", 150}, {"duration_ms", 250},
      {"use_potion_heal_rate", false}, {"cancel_type", "None"}};
  ItemUseHarness harness(definitions);
  harness.wound(500, 500);
  const int before = harness.stat(72);
  const auto packets = harness.combat.apply_item_effects({11152}, kPosition, kStart);
  EXPECT_NE(find_opcode(packets, 80), nullptr);
  harness.advance_to(kStart + 250 * kMillisecond);
  EXPECT_EQ(harness.stat(72), std::min(500, before + 150));
  EXPECT_TRUE(harness.combat.can_apply_stat_recovery(11152));
  for (int index = 0; index < 32; ++index) {
    harness.combat.apply_item_effects({11152}, kPosition, kStart + 250 * kMillisecond);
  }
  EXPECT_FALSE(harness.combat.can_apply_stat_recovery(11152));
}

TEST(CombatItemEffects, ImmunityPotionCuresItsGaugeAndBlocksNewBuildUpWhileItLasts) {
  ItemUseHarness harness(effect_fixture());
  harness.inventory.stacks[kImmunityPotionItem] = 1;
  const auto burning = combat_rules::AbnormalType::burning;

  harness.use(kBurnSelfSkill, 0, 1, kStart);
  const auto burned = with_opcode(harness.advance_to(kStart), 52);
  ASSERT_EQ(burned.size(), 1u);
  EXPECT_EQ(burned.front().message.at("gauges").at(0).at("type"), "EX5AbnormalStatusType_Burning");
  EXPECT_EQ(burned.front().message.at("gauges").at(0).at("value"), 15000);
  verify_encoding(burned);
  harness.advance_to(kStart + 200 * kMillisecond);
  // 40 points decay every 100 ms, as the client simulates it.
  EXPECT_EQ(harness.combat.abnormal_gauge(7, burning, kStart + 200 * kMillisecond), 14920);

  harness.use(kImmunityPotionSkill, kImmunityPotionItem, 2, kStart + 300 * kMillisecond);
  const auto cured = harness.advance_to(kStart + 330 * kMillisecond);
  const auto cleared = with_opcode(cured, 52);
  ASSERT_EQ(cleared.size(), 1u);
  EXPECT_FALSE(cleared.front().message.at("gauges").at(0).contains("value"));
  EXPECT_EQ(harness.combat.abnormal_gauge(7, burning, kStart + 330 * kMillisecond), 0);
  ASSERT_EQ(with_opcode(cured, 49).size(), 1u);
  EXPECT_EQ(with_opcode(cured, 49).front().message.at("status_effect").at("status_effect_id"),
            kBurningImmunity);

  // While the immunity lasts, new burning build-up is ignored.
  harness.advance_to(kStart + 500 * kMillisecond);
  harness.use(kBurnSelfSkill, 0, 3, kStart + 600 * kMillisecond);
  EXPECT_TRUE(with_opcode(harness.advance_to(kStart + 600 * kMillisecond), 52).empty());
  EXPECT_EQ(harness.combat.abnormal_gauge(7, burning, kStart + 600 * kMillisecond), 0);

  // After it expires, build-up returns; a full gauge applies the burning ailment.
  harness.advance_to(kStart + 6000 * kMillisecond);
  harness.use(kBurnSelfSkill, 0, 4, kStart + 6000 * kMillisecond);
  harness.advance_to(kStart + 6000 * kMillisecond);
  harness.advance_to(kStart + 6200 * kMillisecond);
  harness.use(kBurnSelfSkill, 0, 5, kStart + 6200 * kMillisecond);
  const auto filled = harness.advance_to(kStart + 6200 * kMillisecond);
  ASSERT_EQ(opcodes(filled), (std::vector<std::uint16_t>{52, 49}));
  EXPECT_FALSE(filled.at(0).message.at("gauges").at(0).contains("value"));
  EXPECT_EQ(filled.at(1).message.at("status_effect").at("status_effect_id"), kBurningAilment);
  verify_encoding(filled);
}

TEST(CombatItemEffects, BalmAreaDebuffsMonsterArmorEverySecondUntilItsLifetimeEnds) {
  ItemUseHarness harness(effect_fixture(), tutorial_with_monster());
  harness.inventory.stacks[kBalmItem] = 1;
  ASSERT_EQ(harness.combat.monster_stat(kMonster, 52), 30);

  harness.use(kBalmSkill, kBalmItem, 1, kStart);
  const auto spread = harness.advance_to(kStart + 10 * kMillisecond);
  const auto spawned = with_opcode(spread, 10);
  ASSERT_EQ(spawned.size(), 1u);
  const auto &unit = spawned.front().message;
  EXPECT_EQ(std::stoull(unit.at("unit_id").get<std::string>()) & 0xF, 3u)
      << "skill-object type nibble";
  EXPECT_EQ(unit.at("nub").at("skill_object").at("type"), "AreaOfEffect");
  EXPECT_EQ(unit.at("nub").at("skill_object").at("skillobject_cms_id"), kArea);
  EXPECT_EQ(unit.at("nub").at("skill_object").at("duration"), 20000);
  verify_encoding(spawned);

  // The first interval hit lands on spawn: the monster loses 100 physical armor.
  const auto debuffs = with_opcode(spread, 49);
  ASSERT_EQ(debuffs.size(), 1u);
  EXPECT_EQ(debuffs.front().message.at("unit_id"), std::to_string(kMonster));
  EXPECT_EQ(debuffs.front().message.at("status_effect").at("status_effect_id"), kArmorBreak);
  EXPECT_EQ(harness.combat.monster_stat(kMonster, 52), -70);
  const auto area_hit = with_opcode(spread, 19);
  ASSERT_EQ(area_hit.size(), 1u);
  EXPECT_EQ(area_hit.front().message.at("caster_unit_id"), unit.at("unit_id"));
  EXPECT_EQ(area_hit.front().message.at("base_caster_unit_id"), "7");
  EXPECT_EQ(area_hit.front().message.at("skill_effect_origin_type"),
            "EX5SkillEffectOriginType_AreaOfEffect");

  // Every later second refreshes the debuff instead of stacking it.
  const auto second = harness.advance_to(kStart + 1010 * kMillisecond);
  EXPECT_EQ(with_opcode(second, 50).size(), 1u);
  EXPECT_EQ(harness.combat.monster_stat(kMonster, 52), -70);

  // Lifetime end: despawn at 20 s, unit removal 1496 ms later; the debuff ends 20 s after the
  // last hit and restores the armor.
  const auto ended = harness.advance_to(kStart + 20010 * kMillisecond);
  ASSERT_EQ(with_opcode(ended, 45).size(), 1u);
  EXPECT_EQ(with_opcode(ended, 45).front().message.at("reason_type"),
            "EX5SkillObjectDespawnReasonType_LifeTime");
  EXPECT_TRUE(with_opcode(ended, 11).empty());
  EXPECT_EQ(with_opcode(harness.advance_to(kStart + 21506 * kMillisecond), 11).size(), 1u);
  harness.advance_to(kStart + 40010 * kMillisecond);
  EXPECT_EQ(harness.combat.monster_stat(kMonster, 52), 30);
}

TEST(CombatItemEffects, PlayerHitsBuildTheMonsterAilmentGauge) {
  ItemUseHarness harness(effect_fixture(), tutorial_with_monster());

  Json strike{{"start_skill",
               {{"skill_id", kBurningStrikeSkill},
                {"instance_id", 1},
                {"pos", {{"millimeter_x", 800}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
                {"rotation", {{"yaw", 0}}},
                {"tick", "1"}}}};
  const WorldPosition near_monster{8000, 800, 0, 0};
  harness.combat.receive(20010, strike, near_monster, kStart);
  const auto packets = harness.combat.advance(kStart, near_monster, harness.tutorial);
  const auto gauges = with_opcode(packets, 52);
  ASSERT_EQ(gauges.size(), 1u);
  EXPECT_EQ(gauges.front().message.at("unit_id"), std::to_string(kMonster));
  // Candidate gain: 1.7 gauge points per catalog attack point.
  EXPECT_EQ(gauges.front().message.at("gauges").at(0).at("value"), 5100);
  EXPECT_EQ(harness.combat.abnormal_gauge(kMonster, combat_rules::AbnormalType::burning, kStart),
            5100);
}

TEST(CombatItemEffects, AttackFoodAppliesItsPercentageMultiplierThroughTheEffectList) {
  auto stats = player_stats();
  stats["53"] = 1000;
  stats["48"] = 500;
  ItemUseHarness harness(effect_fixture(), empty_tutorial(), stats);

  const auto *effects = harness.combat.item_effect_list(kAttackFoodItem);
  ASSERT_NE(effects, nullptr);
  EXPECT_EQ(harness.combat.item_effect_list(4242), nullptr);
  const auto packets = harness.combat.apply_item_effects(*effects, kPosition, kStart);
  ASSERT_EQ(opcodes(packets), (std::vector<std::uint16_t>{47, 49}));
  // statMul 200 is +0.2%: 1000 -> 1002 and 500 -> 501.
  EXPECT_EQ(harness.stat(53), 1002);
  EXPECT_EQ(harness.stat(48), 501);
  verify_encoding(packets);

  const auto expired = harness.advance_to(kStart + 600000 * kMillisecond);
  ASSERT_EQ(opcodes(expired), (std::vector<std::uint16_t>{47, 51}));
  EXPECT_EQ(harness.stat(53), 1000);
  EXPECT_EQ(harness.stat(48), 500);
}

} // namespace chronicles
