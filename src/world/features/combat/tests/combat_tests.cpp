#include "common/clock_ticks.hpp"
#include "data/starter_gear.hpp"
#include "distribution/distribution.hpp"
#include "protocol/protocol.hpp"
#include "world/core/retained_owner_supervisor.hpp"
#include "world/features/character/character_stats.hpp"
#include "world/features/combat/combat.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/session/combat_work_schedule.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles {

// Disposable fixtures may explicitly discard a fenced synthetic outcome after assertions.
struct CombatSessionTestAccess {
  static void discard_retained_milestone(CombatSession &session) {
    session.retained_world_milestone_.reset();
  }
};

namespace {

struct SyntheticMilestoneOwner final : CombatMilestoneOwner {
  explicit SyntheticMilestoneOwner(CombatSession &session) : session_(&session) {
  }

  const CombatSession *owned_combat_session() const noexcept override {
    return session_;
  }

  CombatSession *session_;
};

using Json = nlohmann::json;
constexpr std::int64_t kStart = 100000000;
constexpr std::int64_t kMillisecond = 10000;
constexpr std::uint64_t kMonster = 2305843009213698050;
constexpr std::uint64_t kInactiveMonster = 2305843009213702146;

// Test-only fault injection: the production damage path logs after updating HP and before
// constructing its notification. A throwing sink exposes allocation-like failures there.
class DamageFaultSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
  explicit DamageFaultSink(int failure_on_hit = 1) : remaining_hits_(failure_on_hit) {
  }

protected:
  void sink_it_(const spdlog::details::log_msg &message) override {
    const std::string_view text(message.payload.data(), message.payload.size());
    if (text.find("Combat damage npc_to_player") != std::string_view::npos &&
        --remaining_hits_ == 0) {
      throw std::runtime_error("Injected damage notification failure");
    }
  }

  void flush_() override {
  }

private:
  int remaining_hits_;
};

class ScopedDamageFaultLogger final {
public:
  explicit ScopedDamageFaultLogger(int failure_on_hit = 1) : previous_(spdlog::default_logger()) {
    auto logger = std::make_shared<spdlog::logger>("combat-damage-fault",
                                                   std::make_shared<DamageFaultSink>(failure_on_hit));
    logger->set_level(spdlog::level::info);
    logger->set_error_handler([](const std::string &) {
      throw std::runtime_error("Injected damage notification failure");
    });
    spdlog::set_default_logger(std::move(logger));
  }

  ~ScopedDamageFaultLogger() {
    spdlog::set_default_logger(std::move(previous_));
  }

private:
  std::shared_ptr<spdlog::logger> previous_;
};

Json combat_fixture() {
  const Json stage{{"index", 0},
                   {"duration_ms", 1000},
                   {"next_stage", -1},
                   {"events", {{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 300}}}},
                   {"notifies", Json::array()}};
  const Json skill{{"id", 200},           {"classes", {1, 2, 3, 5}},
                   {"npc", false},        {"weapon", "None"},
                   {"type", "Normal"},    {"costs", {{"SP", 20}}},
                   {"cooldown_ms", 1500}, {"cooldown_group", 0},
                   {"range_mm", 5000},    {"require_target", false},
                   {"angle", 360},        {"layers", {{{"index", 0}, {"stages", {stage}}}}}};
  const Json effect{
      {"kind", "damage"},
      {"parameters", {{"physicalCoefficient", 1}, {"magicCoefficient", 0}}},
      {"targeting_kind", "shapeCylinderSector"},
      {"targeting",
       {{"outerRadius", 300}, {"halfHeight", 200}, {"angle", 120}, {"maxTargetCount", 1}}},
      {"chain", 0}};
  const Json npc_stats{{"18", 50}, {"19", 1}, {"69", 50}, {"72", 1}, {"22", 10}, {"17", 10}};
  return {{"format_version", 1},
          {"skills", {skill}},
          {"effects", {{"1", effect}}},
          {"projectiles", Json::object()},
          {"npc_stats", {{"5001015", npc_stats}, {"5001528", npc_stats}}},
          {"npc_skills", Json::object()},
          {"weapon_items", Json::object()},
          {"recoveries", Json::object()}};
}

std::shared_ptr<TutorialData> tutorial_fixture(bool second_monster_active = false) {
  const auto npc = [](std::uint64_t id, int cms, bool active) {
    return Json{{"unit_id", std::to_string(id)}, {"cms_id", cms},
                {"behavior", "Monster"},         {"faction", "EX5Faction_Bandit"},
                {"position", {1000, 0, 0}},      {"yaw", 0},
                {"spawner_guid", {"1", "2"}},    {"anim_stance_key", 0},
                {"initially_active", active}};
  };
  return std::make_shared<TutorialData>(
      Json{{"format_version", 1},
           {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
           {"map_id", 8000},
           {"objects", Json::array()},
           {"doodads", Json::array()},
           {"utility_items", Json::array()},
           {"civilians", Json::array()},
           {"sectors", {1}},
           {"npc_spawns", {npc(kMonster, 5001015, true), npc(kInactiveMonster, 5001528, second_monster_active)}},
           {"quests",
            {{{"cms_id", 10000106},
              {"accept_triggers", Json::array()},
              {"auto_complete", false},
              {"sequential", true},
              {"experience", 0},
              {"rewards", Json::array()},
              {"progresses",
               {{{"sequence", 1},
                 {"group", 1},
                 {"kind", "HuntMonster"},
                 {"parameters", {{"npcId", 5001015}, {"count", 1}}}}}}}}}});
}

Character character(int class_id = 1) {
  Character result;
  result.id = 7;
  result.class_id = class_id;
  return result;
}

Json player_stats() {
  return {{"18", 100}, {"19", 50}, {"69", 100}, {"72", 50},
          {"22", 60},  {"17", 10}, {"15", 1},   {"25", 10}};
}

Json start_request(std::uint32_t instance = 1) {
  return {{"start_skill",
           {{"skill_id", 200},
            {"instance_id", instance},
            {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
            {"rotation", {{"yaw", 0}}},
            {"tick", "999999999999"}}}};
}

TEST(Combat, NpcStatsCapAccepts173Entries) {
  auto definitions = combat_fixture();
  Json stats = definitions.at("npc_stats");
  ASSERT_EQ(stats.size(), 2u);
  const Json &sample = definitions.at("npc_stats").at("5001015");
  for (int i = 0; i < 171; ++i) {
    ASSERT_FALSE(stats.contains(std::to_string(9000000 + i)));
    stats[std::to_string(9000000 + i)] = sample;
  }
  definitions["npc_stats"] = stats;
  ASSERT_EQ(definitions.at("npc_stats").size(), 173u);
  EXPECT_NO_THROW(std::make_shared<CombatData>(definitions));
}

TEST(Combat, NpcStatsCapAccepts256Entries) {
  auto definitions = combat_fixture();
  Json stats = definitions.at("npc_stats");
  ASSERT_EQ(stats.size(), 2u);
  const Json &sample = definitions.at("npc_stats").at("5001015");
  for (int i = 0; i < 254; ++i) {
    ASSERT_FALSE(stats.contains(std::to_string(9000000 + i)));
    stats[std::to_string(9000000 + i)] = sample;
  }
  definitions["npc_stats"] = stats;
  ASSERT_EQ(definitions.at("npc_stats").size(), 256u);
  EXPECT_NO_THROW(std::make_shared<CombatData>(definitions));
}

TEST(Combat, NpcStatsCapRejects8193Entries) {
  auto definitions = combat_fixture();
  Json stats = definitions.at("npc_stats");
  ASSERT_EQ(stats.size(), 2u);
  const Json &sample = definitions.at("npc_stats").at("5001015");
  for (int i = 0; i < 8191; ++i) {
    ASSERT_FALSE(stats.contains(std::to_string(9000000 + i)));
    stats[std::to_string(9000000 + i)] = sample;
  }
  definitions["npc_stats"] = stats;
  ASSERT_EQ(definitions.at("npc_stats").size(), 8193u);
  EXPECT_THROW(std::make_shared<CombatData>(definitions), std::runtime_error);
}

void verify_encoding(const std::vector<GameplayPacket> &packets) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : packets) {
    EXPECT_NO_THROW(protocol.encode(packet.opcode, packet.message))
        << packet.opcode << ": " << packet.message;
  }
}

TEST(Combat, DeadlinesOwnDamageAndKillCreditWithoutClientEffectReports) {
  const auto tutorial_data = tutorial_fixture();
  WorldProgress progress;
  progress.quests[10000106].objectives[1] = 0;
  int commits = 0;
  TutorialSession tutorial(tutorial_data, 7, progress, [&](const auto &, const auto &) {
    ++commits;
    return true;
  });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture()), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  const auto started = combat.receive(20010, start_request(), position, kStart);
  ASSERT_EQ(started.size(), 2);
  EXPECT_EQ(combat.player_stats().at("72"), 30);
  EXPECT_EQ(combat.next_action_tick(), kStart + 300 * kMillisecond);
  verify_encoding(started);
  EXPECT_TRUE(combat.advance(kStart + 299 * kMillisecond, position, tutorial).empty());
  EXPECT_EQ(combat.monster_hp(kMonster), 50);
  const auto damage = combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 0);
  EXPECT_EQ(tutorial.progress().quests.at(10000106).objectives.at(1), 1);
  EXPECT_EQ(commits, 1);
  verify_encoding(damage);
  combat.advance(kStart + 500 * kMillisecond, position, tutorial);
  EXPECT_EQ(commits, 1);
}

TEST(Combat, SpentHealthAndStaminaSurviveEquipmentStatRefresh) {
  auto definitions = combat_fixture();
  definitions["skills"][0]["costs"]["HP"] = 35;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_fixture(),
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  const auto started = combat.receive(20010, start_request(), position, kStart);
  ASSERT_EQ(started.at(0).opcode, 53);
  ASSERT_EQ(combat.player_stats().at("69"), 65);
  ASSERT_EQ(combat.player_stats().at("72"), 30);

  // Equipping cannot convert fresh derived maxima into free HP or SP.
  auto equipped_stats = player_stats();
  equipped_stats["18"] = 130;
  equipped_stats["19"] = 70;
  equipped_stats["69"] = 130;
  equipped_stats["72"] = 70;
  combat.update_player_stats(equipped_stats);
  EXPECT_EQ(combat.player_stats().at("69"), 65);
  EXPECT_EQ(combat.player_stats().at("72"), 30);
  EXPECT_EQ(combat.player_stats().at("18"), 130);
  EXPECT_EQ(combat.player_stats().at("19"), 70);

  equipped_stats["18"] = 50;
  equipped_stats["19"] = 20;
  combat.update_player_stats(std::move(equipped_stats));
  EXPECT_EQ(combat.player_stats().at("69"), 50);
  EXPECT_EQ(combat.player_stats().at("72"), 20);
}

TEST(Combat, ReportsCannotAccelerateDamageAndForgedTargetsAreRejected) {
  auto definitions = combat_fixture();
  definitions["effects"]["1"]["parameters"]["physicalCoefficient"] = 0.1;
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  auto request = start_request();
  request["target_ids"] = {std::to_string(kInactiveMonster)};
  const auto rejected = combat.receive(20010, request, position, kStart);
  ASSERT_EQ(rejected.size(), 1);
  EXPECT_EQ(rejected.at(0).opcode, 13);
  EXPECT_EQ(combat.player_stats().at("72"), 50);
  verify_encoding(rejected);
  combat.receive(20010, start_request(), position, kStart);
  const Json report{{"skill_instance_id", 1},
                    {"pos", start_request()["start_skill"]["pos"]},
                    {"skill_effect", {{"effect_id", 1}, {"targets", {{{"unit_id", "7"}}}}}}};
  combat.receive(20011, report, position, kStart + kMillisecond);
  combat.advance(kStart + kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 50);
  combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 44);
  combat.receive(20011, report, position, kStart + 400 * kMillisecond);
  combat.advance(kStart + 500 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 44);
}

TEST(Combat, QuestCreditedDeadMonstersAreNeitherSpawnedNorTargetableOnReconnect) {
  const auto tutorial_data = tutorial_fixture();
  WorldProgress progress;
  progress.quests[10000106].credited_kills.insert(kMonster);
  CombatSession combat(std::make_shared<CombatData>(combat_fixture()), tutorial_data, character(),
                       player_stats(), progress);
  EXPECT_EQ(combat.monster_hp(kMonster), 0);
  auto packets = TutorialSession(tutorial_data, 7, progress, [](const auto &, const auto &) {
                   return true;
                 }).sectors();
  int suppressed = 0;
  for (auto &packet : packets) {
    if (!combat.decorate_spawn(packet)) {
      ++suppressed;
    }
  }
  EXPECT_EQ(suppressed, 1);
  auto request = start_request();
  request["target_ids"] = {std::to_string(kMonster)};
  const auto rejected = combat.receive(20010, request, {8000, 0, 0, 0}, kStart);
  ASSERT_EQ(rejected.size(), 1);
  EXPECT_EQ(rejected.at(0).opcode, 13);
  EXPECT_EQ(combat.player_stats().at("72"), 50);
}

TEST(Combat, CooldownNotificationsIdentifyTheCatalogGroup) {
  auto definitions = combat_fixture();
  definitions["skills"][0]["cooldown_group"] = 17;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_fixture(), character(),
                       player_stats());
  const auto started = combat.receive(20010, start_request(), {8000, 0, 0, 0}, kStart);
  ASSERT_EQ(started.at(0).opcode, 53);
  EXPECT_EQ(started.at(0).message.at("cooldowns").at(0).at("group_id"), 17);
  verify_encoding(started);
}

TEST(Combat, StaminaCooldownReplayAndMapRestrictionsApplyToEveryClass) {
  for (const int class_id : {1, 2, 3, 5}) {
    const auto tutorial_data = tutorial_fixture();
    TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
    CombatSession combat(std::make_shared<CombatData>(combat_fixture()), tutorial_data,
                         character(class_id), player_stats());
    WorldPosition position{9011, 0, 0, 0};
    EXPECT_EQ(combat.receive(20010, start_request(), position, kStart).at(0).opcode, 13);
    position.map_id = 8000;
    combat.receive(20010, start_request(), position, kStart);
    EXPECT_EQ(combat.receive(20010, start_request(), position, kStart + kMillisecond).at(0).opcode,
              13);
    combat.advance(kStart + 1000 * kMillisecond, position, tutorial);
    const auto cooldown =
        combat.receive(20010, start_request(2), position, kStart + 1001 * kMillisecond);
    EXPECT_EQ(cooldown.at(0).message.at("ec"), "Skill_Cooldown");
    combat.advance(kStart + 2000 * kMillisecond, position, tutorial);
    EXPECT_EQ(combat.player_stats().at("72"), 50);
    verify_encoding(cooldown);
  }
}

TEST(Combat, ServerProjectileTravelTimeIgnoresForgedHitRequests) {
  auto definitions = combat_fixture();
  definitions["effects"]["2"] = definitions["effects"]["1"];
  definitions["effects"]["1"]["kind"] = "spawnProjectile";
  definitions["effects"]["1"]["parameters"] = {{"projectileId", 3}};
  definitions["projectiles"]["3"] = {
      {"duration_ms", 2000}, {"parameters", {{"speed", 100}}}, {"hit_effects", {2}}};
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);
  verify_encoding(combat.advance(kStart + 300 * kMillisecond, position, tutorial));
  combat.receive(20021, {{"instance_id", "1"}, {"hit", {{"unit_id", std::to_string(kMonster)}}}},
                 position, kStart + 301 * kMillisecond);
  combat.advance(kStart + 1299 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 50);
  verify_encoding(combat.advance(kStart + 1300 * kMillisecond, position, tutorial));
  EXPECT_EQ(combat.monster_hp(kMonster), 0);
}

Json player_motion_fixture() {
  auto document = combat_fixture();
  auto &stage = document["skills"][0]["layers"][0]["stages"][0];
  stage["player_motion"] = {{"start_ms", 0},           {"end_ms", 900},
                            {"max_distance_mm", 2000}, {"path_length_mm", 1000},
                            {"ground_move", true},     {"using_scale", true}};
  stage["adapted_rotations"] = {{{"startTime", 0}, {"endTime", 900}}};
  return document;
}

TEST(Combat, PlayerRootMotionPublishesBoundedObserverStateOnceWithoutMovingTheOwner) {
  CombatSession combat(std::make_shared<CombatData>(player_motion_fixture()), tutorial_fixture(),
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);
  Json request{{"root_motion",
                {{"skill_instance_id", 1},
                 {"start_pos", {{"millimeter_x", 0}}},
                 {"dest_pos", {{"millimeter_x", 1500}}},
                 {"scale", 900000},
                 {"ground_move", false},
                 {"end_tick", wire_monotonic_tick(kStart + 900 * kMillisecond)}}}};
  auto result = combat.receive_player_motion(20033, request, 5, position, kStart);
  ASSERT_TRUE(result.own.empty());
  ASSERT_EQ(result.observers.size(), 1u);
  EXPECT_EQ(result.observers[0].opcode, 64);
  const auto &motion = result.observers[0].message.at("root_motion");
  EXPECT_EQ(motion.at("scale"), 1.5);
  EXPECT_EQ(motion.at("ground_move"), true);
  verify_encoding(result.observers);
  EXPECT_TRUE(combat.receive_player_motion(20033, request, 6, position, kStart).observers.empty());
}

TEST(Combat, PlayerMotionRejectsForeignActionsTeleportDistancesAndFutureDeadlines) {
  CombatSession combat(std::make_shared<CombatData>(player_motion_fixture()), tutorial_fixture(),
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);
  const Json valid{{"skill_instance_id", 1},
                   {"start_pos", Json::object()},
                   {"dest_pos", {{"millimeter_x", 1000}}},
                   {"end_tick", wire_monotonic_tick(kStart + 900 * kMillisecond)}};
  for (const auto &change : std::vector<Json>{{{"skill_instance_id", 2}},
                                              {{"stage_index", 1}},
                                              {{"dest_pos", {{"millimeter_x", 1000000}}}},
                                              {{"start_pos", {{"millimeter_x", 1000000}}}},
                                              {{"end_tick", "9223372036854775807"}}}) {
    auto motion = valid;
    motion.update(change);
    const auto result =
        combat.receive_player_motion(20033, {{"root_motion", motion}}, 5, position, kStart);
    EXPECT_TRUE(result.observers.empty());
    ASSERT_EQ(result.own.size(), 1u);
    EXPECT_EQ(result.own[0].opcode, 2);
    verify_encoding(result.own);
  }
}

// Sends one root-motion report for a fresh skill whose stage carries `player_motion`, from the
// origin to `destination`. True when the server relays it to observers.
bool player_motion_accepted(const Json &player_motion, const Json &destination) {
  auto definitions = player_motion_fixture();
  definitions["skills"][0]["layers"][0]["stages"][0]["player_motion"] = player_motion;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_fixture(),
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);

  const Json motion{{"skill_instance_id", 1},
                    {"start_pos", Json::object()},
                    {"dest_pos", destination},
                    {"end_tick", wire_monotonic_tick(kStart + 900 * kMillisecond)}};
  const auto result =
      combat.receive_player_motion(20033, {{"root_motion", motion}}, 5, position, kStart);
  if (!result.observers.empty()) {
    return true;
  }

  EXPECT_EQ(result.own.size(), 1u);
  return false;
}

Json authored_player_motion(double max_distance_mm, double path_length_mm, bool ground_move) {
  return {{"start_ms", 0},
          {"end_ms", 900},
          {"max_distance_mm", max_distance_mm},
          {"path_length_mm", path_length_mm},
          {"ground_move", ground_move},
          {"using_scale", true}};
}

// Official reports exceed the authored reach by up to 196 mm regardless of length (skill 2249:
// 500 mm authored, 675 mm reported). The old 5% + 50 mm bound refused these short lunges.
TEST(Combat, PlayerMotionAcceptsTheObservedOvershootOfShortLungesButNotATeleport) {
  const auto lunge = authored_player_motion(500, 500, true);

  EXPECT_TRUE(player_motion_accepted(lunge, {{"millimeter_x", 675}}));
  EXPECT_TRUE(player_motion_accepted(lunge, {{"millimeter_x", 820}}));
  EXPECT_FALSE(player_motion_accepted(lunge, {{"millimeter_x", 830}}));
  EXPECT_FALSE(player_motion_accepted(lunge, {{"millimeter_x", 5000}}));
}

// Adapted motions play their whole authored path when maxMoveDistance is shorter (skill
// 1100518: 1 m adaptation limit, 2,559 mm path, reports up to 2,727 mm).
TEST(Combat, PlayerMotionBoundsAdaptedMotionByItsAuthoredPathWhenTheLimitIsShorter) {
  const auto adapted = authored_player_motion(1000, 2559, true);

  EXPECT_TRUE(player_motion_accepted(adapted, {{"millimeter_y", 2727}}));
  EXPECT_FALSE(player_motion_accepted(adapted, {{"millimeter_y", 3100}}));
}

// Ground-following motions take their height from steps, slopes and ledges (official reports
// change height by up to 1.5 m), so height does not consume the horizontal reach.
TEST(Combat, GroundPlayerMotionAcceptsTerrainHeightChangesWithinThePositionTolerance) {
  const auto ground = authored_player_motion(500, 500, true);

  EXPECT_TRUE(player_motion_accepted(ground, {{"millimeter_x", 500}, {"millimeter_z", -1500}}));
  EXPECT_TRUE(player_motion_accepted(ground, {{"millimeter_x", 400}, {"millimeter_z", 900}}));
  EXPECT_FALSE(player_motion_accepted(ground, {{"millimeter_x", 400}, {"millimeter_z", -6000}}));

  // An airborne motion authors its height, so the full distance stays bounded.
  const auto airborne = authored_player_motion(500, 500, false);
  EXPECT_FALSE(player_motion_accepted(airborne, {{"millimeter_x", 400}, {"millimeter_z", 900}}));
  EXPECT_TRUE(player_motion_accepted(airborne, {{"millimeter_x", 400}, {"millimeter_z", 300}}));
}

TEST(Combat, AdaptedRotationRequiresAnActiveAuthoredWindowAndPublishesOnlyToObservers) {
  CombatSession combat(std::make_shared<CombatData>(player_motion_fixture()), tutorial_fixture(),
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  Json request{{"skill_instance_id", 1},
               {"end_yaw", 90},
               {"activate_tick", wire_monotonic_tick(kStart)},
               {"end_tick", wire_monotonic_tick(kStart + 900 * kMillisecond)}};
  const auto absent = combat.receive_player_motion(20034, request, 1, position, kStart);
  ASSERT_EQ(absent.own.size(), 1u);
  EXPECT_EQ(absent.own[0].opcode, 65);
  verify_encoding(absent.own);
  combat.receive(20010, start_request(), position, kStart);
  auto result = combat.receive_player_motion(20034, request, 2, position, kStart);
  EXPECT_TRUE(result.own.empty());
  ASSERT_EQ(result.observers.size(), 1u);
  EXPECT_EQ(result.observers[0].opcode, 66);
  EXPECT_EQ(result.observers[0].message.at("end_yaw"), 90);
  verify_encoding(result.observers);
  EXPECT_TRUE(combat.receive_player_motion(20034, request, 3, position, kStart).observers.empty());
}

TEST(Combat, PlayerMotionProfileRejectsInvalidBoundsBeforeServingRequests) {
  for (const auto &change : std::vector<Json>{{{"start_ms", -1}},
                                              {{"end_ms", 1001}},
                                              {{"end_ms", -1}},
                                              {{"max_distance_mm", -1}},
                                              {{"path_length_mm", 1000001}},
                                              {{"ground_move", "true"}}}) {
    auto definitions = player_motion_fixture();
    definitions["skills"][0]["layers"][0]["stages"][0]["player_motion"].update(change);
    EXPECT_THROW(CombatData data(definitions), std::exception);
  }
}

TEST(Combat, PersistedDemiseRestoresFullVitalsOnlyAfterRevival) {
  WorldProgress progress;
  progress.dead = true;
  CombatSession combat(std::make_shared<CombatData>(combat_fixture()), tutorial_fixture(),
                       character(), player_stats(), progress);
  EXPECT_TRUE(combat.player_dead());
  EXPECT_EQ(combat.player_stats().at("69"), 0);
  EXPECT_EQ(combat.player_stats().at("72"), 0);
  const WorldPosition position{8000, 0, 0, 0};
  const auto blocked = combat.receive(20010, start_request(), position, kStart);
  ASSERT_EQ(blocked.size(), 1u);
  EXPECT_EQ(blocked.front().opcode, 13);

  const auto revived = combat.revive(kStart);
  ASSERT_EQ(revived.size(), 4u);
  EXPECT_EQ(revived[0].opcode, 49);
  EXPECT_EQ(revived[0].message.at("status_effect").at("status_effect_id"), 50030002);
  EXPECT_EQ(revived[0].message.at("status_effect").at("expire_tick"), "9223372036854775807");
  EXPECT_EQ(revived[1].opcode, 51);
  EXPECT_EQ(revived[1].message.at("status_effects").at(0).at("status_effect_id"), 50030002);
  EXPECT_EQ(revived[1].message.at("status_effects").at(0).at("instance_id"),
            revived[0].message.at("status_effect").at("instance_id"));
  EXPECT_EQ(revived[2].opcode, 47);
  EXPECT_EQ(revived[2].message.at("default_source"), "SCS_Revive");
  const auto &changes = revived[2].message.at("stat_changes");
  ASSERT_EQ(changes.size(), 7u);
  const std::vector<int> captured_order{68, 69, 70, 71, 72, 73, 80};
  for (std::size_t index = 0; index < captured_order.size(); ++index) {
    EXPECT_EQ(changes[index].at("stat_type"), captured_order[index]);
  }
  EXPECT_EQ(changes[1].at("result"), 100);
  EXPECT_EQ(changes[4].at("result"), 50);
  EXPECT_EQ(revived[3].opcode, 32);
  EXPECT_EQ(revived[3].message.at("state"), "EX5UnitStateType_Idle");
  EXPECT_EQ(combat.player_stats().at("69"), 100);
  EXPECT_EQ(combat.player_stats().at("72"), 50);
  EXPECT_FALSE(combat.player_dead());
  EXPECT_THROW(combat.revive(kStart + kMillisecond), std::logic_error);
  verify_encoding(revived);
}

// A repeated revive request, or one from a living player, is a no-op.
TEST(Combat, ReviveRequestIsIdempotentAndIgnoredWhileAlive) {
  WorldProgress progress;
  progress.dead = true;
  CombatSession dead(std::make_shared<CombatData>(combat_fixture()), tutorial_fixture(),
                     character(), player_stats(), progress);
  EXPECT_EQ(dead.revive_if_dead(kStart).size(), 4u);
  const auto stats_after_revival = dead.player_stats();
  EXPECT_TRUE(dead.revive_if_dead(kStart + kMillisecond).empty());
  EXPECT_EQ(dead.player_stats(), stats_after_revival);

  CombatSession alive(std::make_shared<CombatData>(combat_fixture()), tutorial_fixture(),
                      character(), player_stats());
  const auto stats_before = alive.player_stats();
  EXPECT_TRUE(alive.revive_if_dead(kStart).empty());
  EXPECT_EQ(alive.player_stats(), stats_before);
}

Json npc_ai_block() {
  return {{"search_circle_range_mm", 2000},    {"search_cone_range_mm", 8000},
          {"search_half_cone_angle", 90.0},    {"hearing_range_mm", 0},
          {"leash_range_mm", 50000},           {"reset_delay_ms", 3000},
          {"combat_distance_min_mm", 1000},    {"combat_distance_max_mm", 2000},
          {"run_speed_mm_per_second", 3000.0}, {"move_speed_rate", 1000}};
}

std::shared_ptr<TutorialData> tutorial_fixture_with_ai(const Json &ai_block) {
  Json document = tutorial_fixture()->document;
  for (auto &npc : document.at("npc_spawns")) {
    npc["ai"] = ai_block;
  }
  return std::make_shared<TutorialData>(document);
}

// Adds one executable NPC attack so monsters may proactively pursue.
Json combat_fixture_with_npc_attack() {
  auto definitions = combat_fixture();
  Json skill = definitions.at("skills").at(0);
  skill["id"] = 201;
  skill["npc"] = true;
  definitions["skills"].push_back(skill);
  definitions["npc_skills"] = {{"5001015", {201}}, {"5001528", {201}}};
  return definitions;
}

const GameplayPacket *find_packet(const std::vector<GameplayPacket> &packets,
                                  std::uint16_t opcode) {
  const auto found = std::ranges::find_if(
      packets, [opcode](const GameplayPacket &packet) { return packet.opcode == opcode; });
  return found == packets.end() ? nullptr : &*found;
}

constexpr int kBasicSkill = 200;
constexpr std::uint16_t kStartSkillRequest = 20010;
constexpr std::uint16_t kChangeSkillStageRequest = 20013;
constexpr std::uint16_t kStartSkillFailure = 13;
constexpr std::uint16_t kStartSkillNotification = 14;
constexpr std::uint16_t kEndSkillNotification = 15;
constexpr std::uint16_t kChangeSkillStageResponse = 16;

// Mirrors the extracted shape of landing skill 2213 and the other landing
// skills: stage 0 waits up to 99 seconds for ground contact, which authorizes
// stage 1 through an action_stage_condition notify instead of input_condition.
constexpr int kLandingSkill = 300;
constexpr int kLandingWaitMs = 99000;
constexpr int kLandingStageMs = 1800;
constexpr int kLandingEffectMs = 200;

// A three-stage action: stage 0 defaults to stage 1 and accepts an input
// transition to stage 2 during its first 400 milliseconds.
constexpr int kMultiStageSkill = 400;
constexpr int kFirstStageMs = 1000;
constexpr int kInputWindowEndMs = 400;
constexpr int kDefaultStageMs = 500;
constexpr int kInputStageMs = 300;

Json action_stage(int index, int duration_ms, int next_stage, Json events, Json notifies) {
  return {{"index", index},
          {"duration_ms", duration_ms},
          {"next_stage", next_stage},
          {"events", std::move(events)},
          {"notifies", std::move(notifies)}};
}

Json player_skill(const Json &definitions, int skill_id, Json stages) {
  auto skill = definitions["skills"][0];
  skill["id"] = skill_id;
  skill["cooldown_ms"] = 0;
  skill["layers"] = Json::array({{{"index", 0}, {"stages", std::move(stages)}}});
  return skill;
}

Json with_landing_skill(Json definitions) {
  const Json ground_contact{{"eventType", "EX5ActionStageConditionType_Ground_Landing"},
                            {"transitActionStageIndex", 1}};
  const Json landing_condition{
      {"notifyType", "action_stage_condition"},
      {"startTime", 0},
      {"endTime", kLandingWaitMs},
      {"actionStageCondition", {{"datas", Json::array({ground_contact})}}}};
  const Json landing_event{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", kLandingEffectMs}};

  auto skill = player_skill(
      definitions, kLandingSkill,
      Json::array(
          {action_stage(0, kLandingWaitMs, -1, Json::array(), Json::array({landing_condition})),
           action_stage(1, kLandingStageMs, -1, Json::array({landing_event}), Json::array())}));
  skill["type"] = "LandingSkill";
  definitions["skills"].push_back(std::move(skill));
  return definitions;
}

Json with_multi_stage_skill(Json definitions) {
  const Json input_condition{{"notifyType", "input_condition"},
                             {"startTime", 0},
                             {"endTime", kInputWindowEndMs},
                             {"inputCondition", {{"nextStageIndex", 2}, {"inputEvent", 3}}}};
  definitions["skills"].push_back(player_skill(
      definitions, kMultiStageSkill,
      Json::array({action_stage(0, kFirstStageMs, 1, Json::array(), Json::array({input_condition})),
                   action_stage(1, kDefaultStageMs, -1, Json::array(), Json::array()),
                   action_stage(2, kInputStageMs, -1, Json::array(), Json::array())})));
  return definitions;
}

Json skill_start_request(int skill_id, std::uint32_t instance) {
  auto request = start_request(instance);
  request["start_skill"]["skill_id"] = skill_id;
  return request;
}

// Mirrors captured PC2S_ChangeSkillStageReq fields. A missing reason is the
// proto3 default, which reports the stage's default transition.
Json stage_request(std::uint32_t instance, int stage_index,
                   std::optional<std::string> reason = std::nullopt) {
  Json request{{"skill_instance_id", instance},
               {"stage_index", stage_index},
               {"pos", start_request()["start_skill"]["pos"]},
               {"rotation", {{"yaw", 0}}}};
  if (reason) {
    request["reason"] = *reason;
  }
  return request;
}

bool has_opcode(const std::vector<GameplayPacket> &packets, std::uint16_t opcode) {
  return find_packet(packets, opcode) != nullptr;
}

bool accepted_start(const std::vector<GameplayPacket> &packets) {
  return has_opcode(packets, kStartSkillNotification) && !has_opcode(packets, kStartSkillFailure);
}

TEST(Combat, GroundLandingEntersTheLandingStageAndCompletionFreesTheCaster) {
  auto definitions = with_landing_skill(combat_fixture());
  definitions["effects"]["1"]["parameters"]["physicalCoefficient"] = 0.1;
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  const auto started =
      combat.receive(kStartSkillRequest, skill_start_request(kLandingSkill, 1), position, kStart);
  ASSERT_TRUE(accepted_start(started));

  // While the landing skill waits for ground contact, its caster is busy.
  const auto refused = combat.receive(kStartSkillRequest, skill_start_request(kBasicSkill, 2),
                                      position, kStart + 100 * kMillisecond);
  ASSERT_EQ(refused.size(), 1);
  EXPECT_EQ(refused.at(0).message.at("ec"), "Skill_UsingSkill");

  // Captured official ground-landing acceptances are silent to their owner.
  const auto landing_tick = kStart + 330 * kMillisecond;
  const auto landed = combat.receive(kChangeSkillStageRequest,
                                     stage_request(1, 1, "ground_landing"), position, landing_tick);
  EXPECT_TRUE(landed.empty());
  EXPECT_TRUE(combat.busy());
  EXPECT_EQ(combat.next_action_tick(), landing_tick + kLandingEffectMs * kMillisecond);

  combat.advance(landing_tick + kLandingEffectMs * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 44);

  const auto completion_tick = landing_tick + kLandingStageMs * kMillisecond;
  const auto completed = combat.advance(completion_tick, position, tutorial);
  const auto *ended = find_packet(completed, kEndSkillNotification);
  ASSERT_NE(ended, nullptr);
  EXPECT_EQ(ended->message.at("end_skill").at("instance_id"), 1);
  EXPECT_EQ(ended->message.at("end_skill").at("reason"), "EX5EndSkillReason_Default");
  EXPECT_FALSE(combat.busy());
  verify_encoding(completed);

  const auto next_cast = combat.receive(kStartSkillRequest, skill_start_request(kBasicSkill, 3),
                                        position, completion_tick + kMillisecond);
  EXPECT_TRUE(accepted_start(next_cast));
  verify_encoding(next_cast);
}

TEST(Combat, RefusedStageChangeEndsTheActionInsteadOfHoldingItUntilItsDeadline) {
  struct RefusalCase {
    std::string reason;
    int stage_index;
    std::int64_t reported_x_mm;
    std::string error_code;
    std::string end_reason;
  };

  // Stage 2 exists but is not the ground-landing target.
  auto definitions = with_landing_skill(combat_fixture());
  auto &landing_stages = definitions["skills"].back()["layers"][0]["stages"];
  landing_stages.push_back(action_stage(2, 100, -1, Json::array(), Json::array()));

  const std::vector<RefusalCase> cases{
      {"input_condition", 1, 0, "Skill_InvalidStage", "EX5EndSkillReason_Default"},
      {"ground_landing", 2, 0, "Skill_GroundLandingInvalidStageIndex",
       "EX5EndSkillReason_LandingSkillFail"},
      {"ground_landing", 5, 0, "Skill_InvalidStage", "EX5EndSkillReason_LandingSkillFail"},
      {"ground_landing", 1, 20000, "TooFar", "EX5EndSkillReason_LandingSkillFail"},
      {"chase", 1, 0, "Skill_InvalidStage", "EX5EndSkillReason_Default"}};

  for (const auto &refusal : cases) {
    SCOPED_TRACE(refusal.reason + " -> " + std::to_string(refusal.stage_index));
    const auto tutorial_data = tutorial_fixture();
    CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                         player_stats());
    const WorldPosition position{8000, 0, 0, 0};
    ASSERT_TRUE(accepted_start(combat.receive(
        kStartSkillRequest, skill_start_request(kLandingSkill, 1), position, kStart)));

    auto request = stage_request(1, refusal.stage_index, refusal.reason);
    request["pos"]["millimeter_x"] = refusal.reported_x_mm;
    const auto refused =
        combat.receive(kChangeSkillStageRequest, request, position, kStart + 330 * kMillisecond);

    ASSERT_EQ(refused.size(), 2);
    EXPECT_EQ(refused.at(0).opcode, kChangeSkillStageResponse);
    EXPECT_EQ(refused.at(0).message.at("ec"), refusal.error_code);
    EXPECT_EQ(refused.at(0).message.at("skill_instance_id"), 1);
    EXPECT_EQ(refused.at(1).opcode, kEndSkillNotification);
    EXPECT_EQ(refused.at(1).message.at("end_skill").at("reason"), refusal.end_reason);
    EXPECT_FALSE(combat.busy());
    verify_encoding(refused);

    const auto next_cast = combat.receive(kStartSkillRequest, skill_start_request(kBasicSkill, 2),
                                          position, kStart + 331 * kMillisecond);
    EXPECT_TRUE(accepted_start(next_cast));
  }
}

TEST(Combat, StaleStageChangeReportDoesNotEndANewerAction) {
  CombatSession combat(std::make_shared<CombatData>(with_landing_skill(combat_fixture())),
                       tutorial_fixture(), character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  ASSERT_TRUE(accepted_start(
      combat.receive(kStartSkillRequest, skill_start_request(kLandingSkill, 4), position, kStart)));

  const auto stale = combat.receive(kChangeSkillStageRequest, stage_request(3, 1, "ground_landing"),
                                    position, kStart + 10 * kMillisecond);
  ASSERT_EQ(stale.size(), 1);
  EXPECT_EQ(stale.at(0).opcode, kChangeSkillStageResponse);
  EXPECT_EQ(stale.at(0).message.at("ec"), "Skill_NotUsingSkill");
  EXPECT_TRUE(combat.busy());
  verify_encoding(stale);

  const auto landed =
      combat.receive(kChangeSkillStageRequest, stage_request(4, 1, "ground_landing"), position,
                     kStart + 20 * kMillisecond);
  EXPECT_TRUE(landed.empty());
}

TEST(Combat, DefaultStageReportsFollowTheServerDeadline) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(with_multi_stage_skill(combat_fixture())),
                       tutorial_data, character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  ASSERT_TRUE(accepted_start(combat.receive(
      kStartSkillRequest, skill_start_request(kMultiStageSkill, 1), position, kStart)));

  // An early report of the correct default stage leaves the deadline in charge.
  const auto early = combat.receive(kChangeSkillStageRequest, stage_request(1, 1), position,
                                    kStart + 100 * kMillisecond);
  EXPECT_TRUE(early.empty());
  EXPECT_EQ(combat.next_action_tick(), kStart + kFirstStageMs * kMillisecond);

  const auto deadline_tick = kStart + kFirstStageMs * kMillisecond;
  combat.advance(deadline_tick, position, tutorial);
  EXPECT_TRUE(combat.busy());

  // The client's report of the transition the server already applied.
  const auto late = combat.receive(kChangeSkillStageRequest, stage_request(1, 1), position,
                                   deadline_tick + 10 * kMillisecond);
  EXPECT_TRUE(late.empty());

  // The redundant report did not restart stage 1, which still ends on time.
  const auto stage_end_tick = deadline_tick + kDefaultStageMs * kMillisecond;
  const auto before_end = combat.advance(stage_end_tick - kMillisecond, position, tutorial);
  EXPECT_FALSE(has_opcode(before_end, kEndSkillNotification));
  EXPECT_TRUE(combat.busy());

  const auto completed = combat.advance(stage_end_tick, position, tutorial);
  EXPECT_TRUE(has_opcode(completed, kEndSkillNotification));
  EXPECT_FALSE(combat.busy());
}

TEST(Combat, DefaultStageReportNamingAnotherStageIsRefused) {
  CombatSession combat(std::make_shared<CombatData>(with_multi_stage_skill(combat_fixture())),
                       tutorial_fixture(), character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  ASSERT_TRUE(accepted_start(combat.receive(
      kStartSkillRequest, skill_start_request(kMultiStageSkill, 1), position, kStart)));

  const auto refused = combat.receive(kChangeSkillStageRequest, stage_request(1, 2), position,
                                      kStart + kFirstStageMs * kMillisecond - kMillisecond);
  ASSERT_EQ(refused.size(), 2);
  EXPECT_EQ(refused.at(0).message.at("ec"), "Skill_InvalidStage");
  EXPECT_EQ(refused.at(1).opcode, kEndSkillNotification);
  EXPECT_FALSE(combat.busy());
}

TEST(Combat, InputConditionStageChangeHonorsItsExtractedWindow) {
  const auto definitions = std::make_shared<CombatData>(with_multi_stage_skill(combat_fixture()));
  const WorldPosition position{8000, 0, 0, 0};

  CombatSession accepted(definitions, tutorial_fixture(), character(), player_stats());
  ASSERT_TRUE(accepted_start(accepted.receive(
      kStartSkillRequest, skill_start_request(kMultiStageSkill, 1), position, kStart)));
  const auto transition_tick = kStart + 200 * kMillisecond;
  const auto transitioned = accepted.receive(
      kChangeSkillStageRequest, stage_request(1, 2, "input_condition"), position, transition_tick);
  EXPECT_TRUE(transitioned.empty());
  EXPECT_EQ(accepted.next_action_tick(), transition_tick + kInputStageMs * kMillisecond);

  // The window ends at 400 ms; 800 ms is outside the transport tolerance too.
  CombatSession late(definitions, tutorial_fixture(), character(), player_stats());
  ASSERT_TRUE(accepted_start(late.receive(
      kStartSkillRequest, skill_start_request(kMultiStageSkill, 1), position, kStart)));
  const auto refused =
      late.receive(kChangeSkillStageRequest, stage_request(1, 2, "input_condition"), position,
                   kStart + 800 * kMillisecond);
  ASSERT_EQ(refused.size(), 2);
  EXPECT_EQ(refused.at(0).message.at("ec"), "Skill_InputConditionInvalidTick");
  EXPECT_EQ(refused.at(1).opcode, kEndSkillNotification);
  EXPECT_FALSE(late.busy());
  verify_encoding(refused);
}

TEST(Combat, StaticProfileLandingSkill2213AcceptsGroundLandingAndFreesTheCaster) {
  const char *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS for the private exported profile";
  }

  // Skill 2213 is the ElementalStaff landing variant captured as a follow-up of
  // skill 2249. Class 5 starts with that weapon.
  constexpr int kLandingSkill2213 = 2213;
  constexpr int kElementalStaffClass = 5;
  constexpr int kCapturedLandingDelayMs = 330;

  const auto tutorial_data = TutorialData::load(path);
  const auto data = std::make_shared<CombatData>(tutorial_data->document.at("combat"));
  ASSERT_TRUE(data->skills.contains(kLandingSkill2213));
  const auto &landing_stage = data->skills.at(kLandingSkill2213).at("layers").at(0).at("stages");
  const auto landing_duration_ms = landing_stage.at(1).at("duration_ms").get<std::int64_t>();

  ClientCatalog catalog(std::filesystem::path(path).parent_path().parent_path() / "catalog");
  const auto stats = CharacterStats::load(catalog);
  const auto player = character(kElementalStaffClass);
  CombatSession combat(data, tutorial_data, player, stats->stats(player));
  TutorialSession tutorial(tutorial_data, player.id, {},
                           [](const auto &, const auto &) { return true; });
  const WorldPosition position{tutorial_data->map_id(), 0, 0, 0};

  const auto started = combat.receive(kStartSkillRequest,
                                      skill_start_request(kLandingSkill2213, 1), position, kStart);
  ASSERT_TRUE(accepted_start(started))
      << (started.empty() ? std::string("no packets") : started.front().message.dump());

  const auto landing_tick = kStart + kCapturedLandingDelayMs * kMillisecond;
  const auto landed = combat.receive(kChangeSkillStageRequest,
                                     stage_request(1, 1, "ground_landing"), position, landing_tick);
  EXPECT_TRUE(landed.empty());
  EXPECT_TRUE(combat.busy());

  const auto completed =
      combat.advance(landing_tick + landing_duration_ms * kMillisecond, position, tutorial);
  EXPECT_TRUE(has_opcode(completed, kEndSkillNotification));
  EXPECT_FALSE(combat.busy());
  verify_encoding(completed);
}


Json world_mob_map(int map_id = 9012, bool one_time = false) {
  return {{"map_id", map_id},
          {"spawns", Json::array({{{"unit_id", "576460752303423506"},
                                   {"cms_id", 5001015},
                                   {"position", {1000, 0, 0}},
                                   {"yaw", 180.0},
                                   {"faction", "EX5Faction_Bandit"},
                                   {"spawner_guid", {"1", "2"}},
                                   {"anim_stance_key", 0},
                                   {"respawn_ms", 2000},
                                   {"one_time_use", one_time}}})}};
}

TEST(Combat, StaticProfileLightningStormRainsSeveralBoltsOnATargetAhead) {
  const char *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS for the private exported profile";
  }

  // Lightning Storm 2235 spawns 25 bolt areas (160501) across a fan 3 m to 14 m ahead at
  // 1200 ms. Each bolt strikes once when its 1200 ms lifetime ends (effect 160212, a 2 m sphere
  // with one target). A Bound Prowler (1 m hit capsule) 8 m ahead is inside five bolts' reach.
  constexpr int kLightningStorm = 2235;
  constexpr int kElementalStaffClass = 5;
  constexpr int kStormBoltDamage = 160212;
  constexpr int kBoltsLandMs = 2400;
  constexpr std::size_t kBoltsReachingTarget = 5;

  const auto tutorial_data = TutorialData::load(path);
  auto profile = tutorial_data->document.at("combat");
  // Enough health to survive every bolt, so each one is counted.
  profile["npc_stats"]["5001015"]["18"] = 100000000;
  profile["npc_stats"]["5001015"]["69"] = 100000000;
  const auto data = std::make_shared<CombatData>(profile);
  ASSERT_TRUE(data->skills.contains(kLightningStorm));
  ClientCatalog catalog(std::filesystem::path(path).parent_path().parent_path() / "catalog");
  const auto stats = CharacterStats::load(catalog);
  const auto player = character(kElementalStaffClass);
  auto map = world_mob_map();
  map["spawns"][0]["position"] = {8000, 0, 0};
  const auto world = CombatSession::create_world_mobs(map, data);
  CombatSession combat(data, nullptr, player, stats->stats(player));
  combat.attach_world_mobs(world);
  const WorldPosition here{9012, 0, 0, 0};

  const auto entry = combat.advance_private_encounter(kStart, here);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto monster_text = find_packet(entry, 10)->message.at("unit_id").get<std::string>();

  const auto started = combat.receive(kStartSkillRequest,
                                      skill_start_request(kLightningStorm, 1), here, kStart);
  ASSERT_TRUE(accepted_start(started))
      << (started.empty() ? std::string("no packets") : started.front().message.dump());

  const auto summoned =
      combat.advance_private_encounter(kStart + (kBoltsLandMs - 1) * kMillisecond, here);
  std::set<std::string> bolt_units;
  for (const auto &packet : summoned) {
    if (packet.opcode == 10 && packet.message.at("nub").contains("skill_object")) {
      bolt_units.insert(packet.message.at("unit_id").get<std::string>());
    }
    EXPECT_FALSE(packet.opcode == 19 && packet.message.at("skill_effect_cms_id") == kStormBoltDamage)
        << "A bolt struck before its lifetime ended";
  }
  EXPECT_EQ(bolt_units.size(), 25u);

  const auto storm = combat.advance_private_encounter(kStart + kBoltsLandMs * kMillisecond, here);
  std::set<std::string> striking_bolts;
  for (const auto &packet : storm) {
    if (packet.opcode != 19 || packet.message.at("skill_effect_cms_id") != kStormBoltDamage) {
      continue;
    }
    EXPECT_EQ(packet.message.at("target_unit_id"), monster_text);
    EXPECT_EQ(packet.message.at("base_caster_unit_id"), "7");
    EXPECT_GT(packet.message.at("final_damage_amount").get<int>(), 0);
    const auto bolt = packet.message.at("caster_unit_id").get<std::string>();
    EXPECT_TRUE(bolt_units.contains(bolt));
    striking_bolts.insert(bolt);
  }
  EXPECT_EQ(striking_bolts.size(), kBoltsReachingTarget);
  verify_encoding(storm);
}

TEST(Combat, StaticProfileLightningBurstLandsItsSpawnDamageOnTheTarget) {
  const char *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS for the private exported profile";
  }

  // Lightning burst 2274 spawns area 160402 on its target. Its interval effect 143514 is
  // harmless; the damage is the area's spawn effect 160505 (official mage capture).
  constexpr int kLightningBurst = 2274;
  constexpr int kElementalStaffClass = 5;
  constexpr int kLightningBurstSpawnDamage = 160505;
  constexpr int kLightningBurstEventMs = 540;

  const auto tutorial_data = TutorialData::load(path);
  const auto data = std::make_shared<CombatData>(tutorial_data->document.at("combat"));
  ASSERT_TRUE(data->skills.contains(kLightningBurst));
  ClientCatalog catalog(std::filesystem::path(path).parent_path().parent_path() / "catalog");
  const auto stats = CharacterStats::load(catalog);
  const auto player = character(kElementalStaffClass);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession combat(data, nullptr, player, stats->stats(player));
  combat.attach_world_mobs(world);
  const WorldPosition here{9012, 0, 0, 0};

  const auto entry = combat.advance_private_encounter(kStart, here);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto monster_text = find_packet(entry, 10)->message.at("unit_id").get<std::string>();
  const auto monster_id = std::stoull(monster_text);
  const auto health_before = combat.live_monster_health(monster_id);
  ASSERT_TRUE(health_before.has_value());

  auto request = skill_start_request(kLightningBurst, 1);
  request["target_ids"] = {monster_text};
  const auto started = combat.receive(kStartSkillRequest, request, here, kStart);
  ASSERT_TRUE(accepted_start(started))
      << (started.empty() ? std::string("no packets") : started.front().message.dump());

  const auto burst =
      combat.advance_private_encounter(kStart + kLightningBurstEventMs * kMillisecond, here);
  const auto *area = find_packet(burst, 10);
  ASSERT_NE(area, nullptr);
  // The world spawn stands at x = 1000 mm; the area is placed on it, not on the caster.
  EXPECT_EQ(area->message.at("nub").at("pos").value("millimeter_x", 0), 1000);
  const auto spawn_hit = std::ranges::find_if(burst, [](const GameplayPacket &packet) {
    return packet.opcode == 19 &&
           packet.message.at("skill_effect_cms_id") == kLightningBurstSpawnDamage;
  });
  ASSERT_NE(spawn_hit, burst.end());
  EXPECT_EQ(spawn_hit->message.at("target_unit_id"), monster_text);
  EXPECT_EQ(spawn_hit->message.at("caster_unit_id"), area->message.at("unit_id"));
  EXPECT_GT(spawn_hit->message.at("final_damage_amount").get<int>(), 0);
  verify_encoding(burst);
}

TEST(WorldMobs, ClientsShareHealthAndOnlyOneKillGetsCredit) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession second(data, nullptr, other, player_stats());
  first.attach_world_mobs(world);
  second.attach_world_mobs(world);
  const WorldPosition here{9012, 0, 0, 0};
  const auto first_entry = first.advance_private_encounter(kStart, here);
  const auto second_entry = second.advance_private_encounter(kStart, here);
  ASSERT_NE(find_packet(first_entry, 10), nullptr);
  ASSERT_NE(find_packet(second_entry, 10), nullptr);
  const auto id_text = find_packet(first_entry, 10)->message.at("unit_id").get<std::string>();
  EXPECT_EQ(find_packet(second_entry, 10)->message.at("unit_id"), id_text);
  const auto id = std::stoull(id_text);
  const auto started = first.receive(20010, start_request(), here, kStart);
  const auto hit = first.advance_private_encounter(kStart + 300 * kMillisecond, here);
  EXPECT_EQ(second.live_monster_health(id), std::nullopt);
  EXPECT_EQ(first.take_world_defeats().size(), 1u);
  EXPECT_TRUE(second.take_world_defeats().empty());
  EXPECT_TRUE(second.gm_damage_monster(id, 100, kStart + 400 * kMillisecond).empty());
  const auto seen = second.advance_private_encounter(kStart + 400 * kMillisecond, here);
  EXPECT_NE(find_packet(seen, 23), nullptr);
  verify_encoding(first_entry);
  verify_encoding(second_entry);
  verify_encoding(started);
  verify_encoding(hit);
  verify_encoding(seen);
}

TEST(WorldMobs, ActionCopyFailureDoesNotConsumeTheUnexecutedEvent) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_stats"]["5001015"]["18"] = 1000;
  definitions["npc_stats"]["5001015"]["69"] = 1000;
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession player(data, nullptr, character(), player_stats());
  player.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};
  const auto entry = player.advance_private_encounter(kStart, position);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  ASSERT_TRUE(accepted_start(player.receive(20010, start_request(), position, kStart)));
  player.set_action_copy_hook_for_test([] {
    throw std::bad_alloc();
  });

  EXPECT_THROW(player.advance_private_encounter(kStart + 300 * kMillisecond, position),
               std::bad_alloc);
  EXPECT_EQ(player.live_monster_health(id), 1000);
  player.set_action_copy_hook_for_test({});
  const auto retried = player.advance_private_encounter(kStart + 300 * kMillisecond, position);
  EXPECT_NE(find_packet(retried, 19), nullptr);
  EXPECT_EQ(player.live_monster_health(id), 940);
  const auto repeated = player.advance_private_encounter(kStart + 300 * kMillisecond, position);
  EXPECT_EQ(find_packet(repeated, 19), nullptr);
  EXPECT_EQ(player.live_monster_health(id), 940);
}

TEST(CombatData, MalformedEventEffectIdsAreRejectedBeforeSessionsStart) {
  const auto above_int_max =
      static_cast<std::int64_t>((std::numeric_limits<int>::max)()) + 1;
  const std::vector<Json> invalid_ids{nullptr, "1", 1.5, -1, 0, above_int_max};
  for (const auto &invalid_id : invalid_ids) {
    SCOPED_TRACE(invalid_id.dump());
    auto definitions = combat_fixture();
    definitions["skills"][0]["layers"][0]["stages"][0]["events"][0]["effect_id"] = invalid_id;
    EXPECT_THROW(CombatData data(definitions), std::exception);
  }
  auto missing = combat_fixture();
  missing["skills"][0]["layers"][0]["stages"][0]["events"][0].erase("effect_id");
  EXPECT_THROW(CombatData data(missing), std::exception);
}

TEST(WorldMobs, FailedLootObserverRetriesOneUnpublishedDefeat) {
  for (const bool scripted : {false, true}) {
    SCOPED_TRACE(scripted ? "scripted" : "non-scripted");
    const auto data = std::make_shared<CombatData>(combat_fixture_with_npc_attack());
    CombatSession combat(data, nullptr, character(), player_stats());
    combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
    auto ledger = std::make_shared<quests::QuestIntentLedger>(WorldProgress{});
    combat.set_world_defeat_capture([&](const MonsterDefeat &defeat) {
      MonsterRewardCapture capture;
      if (scripted) {
        capture.boss_script = BossScriptIntent{defeat.reward_event_id, 1, defeat.position.map_id,
                                                defeat.npc_cms_id, "private-test-incarnation"};
      }
      MonsterRewardReceipt receipt;
      receipt.account_id = 1;
      receipt.character_id = 1;
      receipt.world_id = 1;
      receipt.receipt_id = defeat.reward_event_id;
      receipt.credited_unit_id = defeat.reward_credit_id;
      receipt.npc_cms_id = defeat.npc_cms_id;
      receipt.position = defeat.position;
      capture.recipients.push_back(std::move(receipt));
      capture.quest_captures.push_back(ledger->acquire_capture());
      return capture;
    });
    int observer_calls{};
    std::string observed_event;
    combat.set_defeat_observer([&](const MonsterDefeat &defeat) {
      ++observer_calls;
      if (observed_event.empty()) {
        observed_event = defeat.reward_event_id;
        throw std::runtime_error("Synthetic temporary loot failure");
      }
      EXPECT_EQ(defeat.reward_event_id, observed_event);
      return MonsterDefeatReaction{};
    });

    const WorldPosition here{9012, 0, 0, 0};
    const auto entry = combat.advance_private_encounter(kStart, here);
    ASSERT_NE(find_packet(entry, 10), nullptr);
    const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
    combat.gm_damage_monster(id, 100000, kStart);
    ASSERT_EQ(combat.pending_world_defeats().size(), 1U);
    const auto event_id = combat.pending_world_defeats().front().defeat.reward_event_id;
    const auto script = combat.pending_world_defeats().front().capture.boss_script;
    ASSERT_EQ(script.has_value(), scripted);
    if (script) {
      EXPECT_EQ(script->event_id, event_id);
      EXPECT_EQ(script->copy_scope, "private-test-incarnation");
    }
    EXPECT_THROW(combat.finalize_world_defeat(event_id, kStart), std::runtime_error);
    EXPECT_EQ(combat.pending_world_defeats().front().capture.boss_script, script);
    EXPECT_TRUE(combat.take_world_defeats().empty());
    EXPECT_FALSE(combat.pending_world_defeats().front().finalized);

    const auto result = combat.finalize_world_defeat(event_id, kStart + kMillisecond);
    EXPECT_NE(find_packet(result, 23), nullptr);
    EXPECT_EQ(observer_calls, 2);
    const auto defeats = combat.take_world_defeats();
    ASSERT_EQ(defeats.size(), scripted ? 0U : 1U);
    if (!defeats.empty()) {
      EXPECT_EQ(defeats.front().reward_event_id, event_id);
    }
    EXPECT_TRUE(combat.finalize_world_defeat(event_id, kStart + 2 * kMillisecond).empty());
    EXPECT_EQ(observer_calls, 2);
    EXPECT_TRUE(combat.acknowledge_world_defeat(event_id));
    EXPECT_TRUE(combat.take_world_defeats().empty());
  }
}

TEST(WorldMobs, FailedLootPublicationRollsBackPublicDeathUntilRetry) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_npc_attack());
  auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession first(data, nullptr, character(), player_stats());
  auto second_character = character();
  second_character.id = 23;
  CombatSession second(data, nullptr, second_character, player_stats());
  first.attach_world_mobs(world);
  second.attach_world_mobs(world);
  auto ledger = std::make_shared<quests::QuestIntentLedger>(WorldProgress{});
  first.set_world_defeat_capture([&](const MonsterDefeat &defeat) {
    MonsterRewardCapture capture;
    MonsterRewardReceipt receipt;
    receipt.account_id = 1;
    receipt.character_id = 1;
    receipt.world_id = 1;
    receipt.receipt_id = defeat.reward_event_id;
    receipt.credited_unit_id = defeat.reward_credit_id;
    receipt.npc_cms_id = defeat.npc_cms_id;
    receipt.position = defeat.position;
    capture.recipients.push_back(std::move(receipt));
    capture.quest_captures.push_back(ledger->acquire_capture());
    return capture;
  });
  int observer_calls{};
  int publication_calls{};
  first.set_defeat_observer([&](const MonsterDefeat &) {
    ++observer_calls;
    MonsterDefeatReaction reaction;
    reaction.on_published = [&](std::int64_t) {
      if (++publication_calls == 1) {
        throw std::runtime_error("Synthetic publication failure");
      }
    };
    return reaction;
  });

  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = first.advance_private_encounter(kStart, here);
  second.advance_private_encounter(kStart, here);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  first.gm_damage_monster(id, 100000, kStart);
  const auto event_id = first.pending_world_defeats().front().defeat.reward_event_id;
  EXPECT_THROW(first.finalize_world_defeat(event_id, kStart), std::runtime_error);
  EXPECT_TRUE(first.take_world_defeats().empty());
  EXPECT_EQ(find_packet(second.advance_private_encounter(kStart + 100 * kMillisecond, here), 23),
            nullptr);

  EXPECT_NE(find_packet(first.finalize_world_defeat(event_id, kStart + 200 * kMillisecond), 23),
            nullptr);
  const auto seen = second.advance_private_encounter(kStart + 300 * kMillisecond, here);
  EXPECT_EQ(std::ranges::count_if(seen, [](const auto &packet) { return packet.opcode == 23; }), 1);
  EXPECT_EQ(first.take_world_defeats().size(), 1U);
  EXPECT_EQ(observer_calls, 1);
  EXPECT_EQ(publication_calls, 2);
  EXPECT_TRUE(first.acknowledge_world_defeat(event_id));
}

TEST(WorldMobs, FinalizedDeathWaitsForAcknowledgementWithoutRepeatingItsEffects) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_npc_attack());
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  auto ledger = std::make_shared<quests::QuestIntentLedger>(WorldProgress{});
  combat.set_world_defeat_capture([&](const MonsterDefeat &defeat) {
    MonsterRewardCapture capture;
    MonsterRewardReceipt receipt;
    receipt.account_id = 1;
    receipt.character_id = 1;
    receipt.world_id = 1;
    receipt.receipt_id = defeat.reward_event_id;
    receipt.credited_unit_id = defeat.reward_credit_id;
    receipt.npc_cms_id = defeat.npc_cms_id;
    receipt.position = defeat.position;
    capture.recipients.push_back(std::move(receipt));
    capture.quest_captures.push_back(ledger->acquire_capture());
    return capture;
  });

  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, here);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  combat.gm_damage_monster(id, 100000, kStart);
  ASSERT_EQ(combat.pending_world_defeats().size(), 1U);
  const auto event_id = combat.pending_world_defeats().front().defeat.reward_event_id;
  const auto original_timeline = combat.world_combat_timeline();
  const auto other_world = CombatSession::create_world_mobs(world_mob_map(), data);
  EXPECT_THROW(combat.attach_world_mobs(other_world), std::runtime_error);
  EXPECT_EQ(combat.world_combat_timeline(), original_timeline);
  ASSERT_EQ(combat.pending_world_defeats().size(), 1U);
  EXPECT_EQ(combat.pending_world_defeats().front().defeat.reward_event_id, event_id);
  EXPECT_FALSE(combat.acknowledge_world_defeat(event_id));

  const auto finalized = combat.finalize_world_defeat(event_id, kStart);
  EXPECT_NE(find_packet(finalized, 23), nullptr);
  ASSERT_EQ(combat.pending_world_defeats().size(), 1U);
  EXPECT_TRUE(combat.pending_world_defeats().front().finalized);
  EXPECT_TRUE(combat.finalize_world_defeat(event_id, kStart).empty());
  std::vector<GameplayPacket> staged;
  combat.set_world_defeat_stage_hook_for_test([] {
    throw std::runtime_error("Injected finalized packet staging failure");
  });
  EXPECT_THROW(combat.stage_finalized_world_defeat(event_id, staged), std::runtime_error);
  EXPECT_TRUE(staged.empty());
  EXPECT_FALSE(combat.pending_world_defeats().front().owner_packets_staged);
  const auto retained = combat.finalized_world_defeat_batch(event_id);
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained, combat.finalized_world_defeat_batch(event_id));
  combat.set_world_defeat_stage_hook_for_test({});
  ASSERT_TRUE(combat.stage_finalized_world_defeat(event_id, staged));
  EXPECT_EQ(staged.size(), finalized.size());
  EXPECT_NE(find_packet(staged, 23), nullptr);
  ASSERT_TRUE(combat.stage_finalized_world_defeat(event_id, staged));
  EXPECT_EQ(staged.size(), finalized.size());
  EXPECT_EQ(combat.take_world_defeats().size(), 1U);
  const auto wrong = std::make_shared<const std::vector<GameplayPacket>>(*retained);
  EXPECT_FALSE(combat.acknowledge_delivered_world_defeat(event_id, wrong));
  EXPECT_FALSE(combat.acknowledge_delivered_world_defeat("wrong-event", retained));
  EXPECT_TRUE(combat.acknowledge_delivered_world_defeat(event_id, retained));
  EXPECT_TRUE(combat.pending_world_defeats().empty());
  EXPECT_EQ(retained->size(), finalized.size());
  EXPECT_NE(find_packet(*retained, 23), nullptr);
  EXPECT_FALSE(combat.acknowledge_world_defeat(event_id));
}

TEST(WorldMobs, FullDefeatBacklogCancelsNewLethalHitsAndRecoversAfterSettlement) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_npc_attack());
  auto map = world_mob_map();
  const auto prototype = map.at("spawns").at(0);
  for (std::size_t index = 1; index <= kMaximumPendingWorldDefeats; ++index) {
    auto spawn = prototype;
    spawn["unit_id"] = std::to_string(576460752303423506ULL + index * 16);
    map["spawns"].push_back(std::move(spawn));
  }
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(map, data));
  auto ledger = std::make_shared<quests::QuestIntentLedger>(WorldProgress{});
  std::size_t captures = 0;
  combat.set_world_defeat_capture([&](const MonsterDefeat &defeat) {
    ++captures;
    MonsterRewardCapture capture;
    MonsterRewardReceipt receipt;
    receipt.account_id = 1;
    receipt.character_id = 1;
    receipt.world_id = 1;
    receipt.receipt_id = defeat.reward_event_id;
    receipt.credited_unit_id = defeat.reward_credit_id;
    receipt.npc_cms_id = defeat.npc_cms_id;
    receipt.position = defeat.position;
    capture.recipients.push_back(std::move(receipt));
    capture.quest_captures.push_back(ledger->acquire_capture());
    return capture;
  });

  const WorldPosition position{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, position);
  std::vector<std::uint64_t> units;
  for (const auto &packet : entry) {
    if (packet.opcode == 10) {
      units.push_back(std::stoull(packet.message.at("unit_id").get<std::string>()));
    }
  }
  ASSERT_EQ(units.size(), kMaximumPendingWorldDefeats + 1);
  for (std::size_t index = 0; index < kMaximumPendingWorldDefeats; ++index) {
    combat.gm_damage_monster(units[index], 100000, kStart);
  }
  ASSERT_EQ(combat.pending_world_defeats().size(), kMaximumPendingWorldDefeats);
  ASSERT_EQ(captures, kMaximumPendingWorldDefeats);

  EXPECT_THROW(combat.gm_damage_monster(units.back(), 100000, kStart), std::runtime_error);
  EXPECT_EQ(combat.live_monster_health(units.back()), 50);
  EXPECT_EQ(captures, kMaximumPendingWorldDefeats);
  ASSERT_TRUE(accepted_start(combat.receive(20010, start_request(), position, kStart)));
  const auto rejected = combat.advance_private_encounter(kStart + 300 * kMillisecond, position);
  EXPECT_NE(find_packet(rejected, 20), nullptr);
  EXPECT_EQ(combat.live_monster_health(units.back()), 50);
  EXPECT_EQ(captures, kMaximumPendingWorldDefeats);
  EXPECT_EQ(combat.pending_world_defeats().size(), kMaximumPendingWorldDefeats);

  const auto first_event = combat.pending_world_defeats().front().defeat.reward_event_id;
  EXPECT_NE(find_packet(combat.finalize_world_defeat(first_event, kStart + 400 * kMillisecond), 23),
            nullptr);
  ASSERT_TRUE(combat.acknowledge_world_defeat(first_event));
  combat.gm_damage_monster(units.back(), 100000, kStart + 500 * kMillisecond);
  EXPECT_EQ(captures, kMaximumPendingWorldDefeats + 1);
  EXPECT_EQ(combat.pending_world_defeats().size(), kMaximumPendingWorldDefeats);
}

TEST(WorldMobs, FailedLethalCaptureCancelsOnlyThatHitAndReleasesItsOverlapGroup) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["effects"]["1"]["overlap_group"] = "lethal-contact";
  definitions["skills"][0]["layers"][0]["stages"][0]["events"] =
      Json::array({{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 300}},
                   {{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 600}}});
  const auto data = std::make_shared<CombatData>(definitions);
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  auto ledger = std::make_shared<quests::QuestIntentLedger>(WorldProgress{});
  int captures = 0;
  combat.set_world_defeat_capture([&](const MonsterDefeat &defeat) {
    if (++captures == 1) {
      throw std::runtime_error("Injected eligibility capture failure");
    }
    MonsterRewardCapture capture;
    MonsterRewardReceipt receipt;
    receipt.account_id = 1;
    receipt.character_id = 1;
    receipt.world_id = 1;
    receipt.receipt_id = defeat.reward_event_id;
    receipt.credited_unit_id = defeat.reward_credit_id;
    receipt.npc_cms_id = defeat.npc_cms_id;
    receipt.position = defeat.position;
    capture.recipients.push_back(std::move(receipt));
    capture.quest_captures.push_back(ledger->acquire_capture());
    return capture;
  });

  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, here);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  ASSERT_TRUE(accepted_start(combat.receive(20010, start_request(), here, kStart)));
  const auto cancelled = combat.advance_private_encounter(kStart + 300 * kMillisecond, here);
  const auto *immune = find_packet(cancelled, 20);
  ASSERT_NE(immune, nullptr);
  EXPECT_TRUE(immune->message.value("damage", false));
  EXPECT_EQ(find_packet(cancelled, 19), nullptr);
  EXPECT_EQ(combat.live_monster_health(id), 50);
  EXPECT_TRUE(combat.pending_world_defeats().empty());

  combat.advance_private_encounter(kStart + 600 * kMillisecond, here);
  EXPECT_EQ(captures, 2);
  ASSERT_EQ(combat.pending_world_defeats().size(), 1U);
  verify_encoding(cancelled);
}

TEST(WorldMobs, FailedBossSupervisorActivationCancelsHitAndKeepsTheActionRunning) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["skills"][0]["layers"][0]["stages"][0]["events"] =
      Json::array({{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 300}},
                   {{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 600}}});
  const auto data = std::make_shared<CombatData>(definitions);
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));

  RetainedOwnerSupervisor supervisor(
      {},
      [](RetainedOwnerKey, RetainedOwnerSupervisor::Completion) {
        return RetainedOwnerSubmit::definitely_not_posted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  int capture_attempts = 0;
  combat.set_world_defeat_capture([&](const MonsterDefeat &) -> MonsterRewardCapture {
    ++capture_attempts;
    if (!supervisor.activate_owner({1, 1})) {
      throw std::runtime_error("Injected boss supervisor activation failure");
    }
    throw std::logic_error("Unexpected activation");
  });

  const WorldPosition position{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, position);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto monster =
      std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  ASSERT_TRUE(accepted_start(combat.receive(20010, start_request(), position, kStart)));

  const auto first = combat.advance_private_encounter(kStart + 300 * kMillisecond, position);
  EXPECT_EQ(capture_attempts, 1);
  EXPECT_EQ(combat.live_monster_health(monster), 50);
  EXPECT_EQ(find_packet(first, 19), nullptr);
  EXPECT_NE(find_packet(first, 20), nullptr);
  EXPECT_TRUE(combat.pending_world_defeats().empty());

  const auto second = combat.advance_private_encounter(kStart + 600 * kMillisecond, position);
  EXPECT_EQ(capture_attempts, 2);
  EXPECT_EQ(combat.live_monster_health(monster), 50);
  EXPECT_EQ(find_packet(second, 19), nullptr);
  EXPECT_NE(find_packet(second, 20), nullptr);
  EXPECT_TRUE(combat.pending_world_defeats().empty());
  EXPECT_EQ(supervisor.health().retained, 0U);
  EXPECT_TRUE(supervisor.stop());
}

TEST(WorldMobs, FailedBossSupervisorActivationRejectsGmLethalHit) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_npc_attack());
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  RetainedOwnerSupervisor supervisor(
      {},
      [](RetainedOwnerKey, RetainedOwnerSupervisor::Completion) {
        return RetainedOwnerSubmit::definitely_not_posted;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  combat.set_world_defeat_capture([&](const MonsterDefeat &) -> MonsterRewardCapture {
    if (!supervisor.activate_owner({1, 1})) {
      throw std::runtime_error("Injected boss supervisor activation failure");
    }
    throw std::logic_error("Unexpected activation");
  });

  const WorldPosition position{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, position);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto monster =
      std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  EXPECT_THROW(combat.gm_damage_monster(monster, 100000, kStart), std::runtime_error);
  EXPECT_EQ(combat.live_monster_health(monster), 50);
  EXPECT_TRUE(combat.pending_world_defeats().empty());
  EXPECT_TRUE(combat.take_world_defeats().empty());
  EXPECT_EQ(supervisor.health().retained, 0U);
  EXPECT_TRUE(supervisor.stop());
}

TEST(WorldMobs, FailedLethalCaptureRestoresTheMonstersShield) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_stats"]["5001015"]["18"] = 30;
  definitions["npc_stats"]["5001015"]["69"] = 30;
  definitions["npc_stats"]["5001015"]["71"] = 20;
  definitions["effects"]["2"] = definitions["effects"]["1"];
  definitions["effects"]["2"]["parameters"]["physicalCoefficient"] = 0.1;
  definitions["skills"][0]["layers"][0]["stages"][0]["events"] =
      Json::array({{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 300}},
                   {{"effect_id", 2}, {"unique_id", 0}, {"time_ms", 600}}});
  const auto data = std::make_shared<CombatData>(definitions);
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  combat.set_world_defeat_capture([](const MonsterDefeat &) -> MonsterRewardCapture {
    throw std::runtime_error("Injected shielded capture failure");
  });

  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, here);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  ASSERT_TRUE(accepted_start(combat.receive(20010, start_request(), here, kStart)));
  const auto cancelled = combat.advance_private_encounter(kStart + 300 * kMillisecond, here);
  ASSERT_NE(find_packet(cancelled, 20), nullptr);
  EXPECT_EQ(combat.live_monster_health(id), 30);

  const auto followup = combat.advance_private_encounter(kStart + 600 * kMillisecond, here);
  const auto *damage = find_packet(followup, 19);
  ASSERT_NE(damage, nullptr);
  EXPECT_EQ(damage->message.value("final_damage_amount", -1), 0);
  EXPECT_EQ(damage->message.value("damage_type", std::string{}), "EX5DamageType_Shield");
  EXPECT_EQ(combat.live_monster_health(id), 30);
  verify_encoding(cancelled);
  verify_encoding(followup);
}

TEST(WorldMobs, AFirstHitCountsAsCombatBeforeAStunnedNpcCanAcquireItsTarget) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_stats"]["5001015"]["18"] = 1000;
  definitions["npc_stats"]["5001015"]["69"] = 1000;
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  definitions["status_effects"] = {{"18", {{"duration_ms", 2000}}}};
  definitions["abnormal_gauges"] = {
      {"reduce_tick_ms", 100},
      {"types", {{"Stun", {{"maximum", 20000}, {"reduce_per_tick", 40},
                            {"status_effect_id", 18}}}}}};
  definitions["effects"]["1"]["status_effect_id"] = 18;
  auto map = world_mob_map();
  map["spawns"][0]["faction"] = "EX5Faction_Herbivore";
  const auto data = std::make_shared<CombatData>(definitions);
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(map, data));
  const WorldPosition here{9012, 0, 0, 0};
  combat.advance_private_encounter(kStart, here);
  EXPECT_FALSE(combat.player_in_combat());
  combat.receive(20010, start_request(), here, kStart);
  combat.advance_private_encounter(kStart + 300 * kMillisecond, here);
  EXPECT_TRUE(combat.player_in_combat());
}

TEST(WorldMobs, EveryPlayerWhoDamagesAnEngagedMonsterIsInCombat) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_stats"]["5001015"]["18"] = 1000;
  definitions["npc_stats"]["5001015"]["69"] = 1000;
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto attacker_character = character();
  attacker_character.id = 23;
  auto observer_character = character();
  observer_character.id = 24;
  CombatSession target(data, nullptr, character(), player_stats());
  CombatSession attacker(data, nullptr, attacker_character, player_stats());
  CombatSession observer(data, nullptr, observer_character, player_stats());
  target.attach_world_mobs(world);
  attacker.attach_world_mobs(world);
  observer.attach_world_mobs(world);
  const WorldPosition nearby{9012, 0, 0, 0};

  target.advance_private_encounter(kStart, nearby);
  attacker.advance_private_encounter(kStart, nearby);
  observer.advance_private_encounter(kStart, nearby);
  EXPECT_TRUE(target.player_in_combat());
  EXPECT_FALSE(attacker.player_in_combat());
  EXPECT_FALSE(observer.player_in_combat());

  attacker.receive(20010, start_request(), nearby, kStart + 100 * kMillisecond);
  attacker.advance_private_encounter(kStart + 400 * kMillisecond, nearby);
  EXPECT_TRUE(attacker.player_in_combat());
  EXPECT_FALSE(observer.player_in_combat());

  const WorldPosition distant{9012, 1000000, 0, 0};
  attacker.advance_private_encounter(kStart + 2000 * kMillisecond, distant);
  EXPECT_FALSE(attacker.player_in_combat());
  attacker.advance_private_encounter(kStart + 4000 * kMillisecond, nearby);
  EXPECT_FALSE(attacker.player_in_combat())
      << "Returning to another player's fight does not restore old participation";
}

TEST(WorldMobs, DebuffingAnotherPlayersEngagedMonsterCountsAsCombat) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  definitions["status_effects"] = {{"18", {{"duration_ms", 2000}}}};
  definitions["effects"]["1"]["kind"] = "addStatusEffect";
  definitions["effects"]["1"]["parameters"] = {{"statusEffectId", 18}, {"rate", 100}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto attacker_character = character();
  attacker_character.id = 23;
  CombatSession target(data, nullptr, character(), player_stats());
  CombatSession attacker(data, nullptr, attacker_character, player_stats());
  target.attach_world_mobs(world);
  attacker.attach_world_mobs(world);
  const WorldPosition nearby{9012, 0, 0, 0};
  target.advance_private_encounter(kStart, nearby);
  attacker.advance_private_encounter(kStart, nearby);
  EXPECT_FALSE(attacker.player_in_combat());

  attacker.receive(20010, start_request(), nearby, kStart + 100 * kMillisecond);
  const auto applied = attacker.advance_private_encounter(kStart + 400 * kMillisecond, nearby);
  ASSERT_NE(find_packet(applied, 49), nullptr);
  EXPECT_TRUE(attacker.player_in_combat());
}

TEST(WorldMobs, AHitOnANpcWithoutAiCountsAsCombat) {
  auto definitions = combat_fixture();
  definitions["npc_stats"]["5001015"]["18"] = 1000;
  definitions["npc_stats"]["5001015"]["69"] = 1000;
  const auto data = std::make_shared<CombatData>(definitions);
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  const WorldPosition here{9012, 0, 0, 0};
  combat.advance_private_encounter(kStart, here);
  EXPECT_FALSE(combat.player_in_combat());
  combat.receive(20010, start_request(), here, kStart);
  combat.advance_private_encounter(kStart + 300 * kMillisecond, here);
  EXPECT_TRUE(combat.player_in_combat());
}

TEST(WorldMobs, GmCooldownResetWaitsForAnotherSessionsSharedMutation) {
  const auto data = std::make_shared<CombatData>(combat_fixture());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession second(data, nullptr, other, player_stats());
  first.attach_world_mobs(world);
  second.attach_world_mobs(world);
  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = first.advance_private_encounter(kStart, here);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());

  std::promise<void> mutation_entered;
  auto entered = mutation_entered.get_future();
  std::promise<void> release_mutation;
  const auto release = release_mutation.get_future().share();
  first.set_defeat_observer([&](const MonsterDefeat &) {
    mutation_entered.set_value();
    // Bound the worker even if an assertion or thread creation fails in this test.
    release.wait_for(std::chrono::seconds(2));
    return MonsterDefeatReaction{};
  });
  auto owner =
      std::async(std::launch::async, [&] { return first.gm_damage_monster(id, 100, kStart); });
  if (entered.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    release_mutation.set_value();
    owner.get();
    FAIL() << "The shared mutation did not reach its defeat observer";
  }

  std::promise<void> reset_started;
  auto started = reset_started.get_future();
  auto reset = std::async(std::launch::async, [&] {
    reset_started.set_value();
    return second.gm_reset_cooldowns();
  });
  started.wait();
  const auto while_locked = reset.wait_for(std::chrono::milliseconds(50));
  release_mutation.set_value();
  owner.get();
  reset.get();

  EXPECT_EQ(while_locked, std::future_status::timeout);
  EXPECT_FALSE(second.live_monster_health(id));
}

TEST(WorldMobs, RespawnHasFreshIdentityAndRejectsOldTargets) {
  const auto data = std::make_shared<CombatData>(combat_fixture());
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, here);
  const auto old_id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  combat.gm_damage_monster(old_id, 100, kStart);
  const auto respawn = combat.advance_private_encounter(kStart + 2000 * kMillisecond, here);
  ASSERT_NE(find_packet(respawn, 10), nullptr);
  const auto new_id =
      std::stoull(find_packet(respawn, 10)->message.at("unit_id").get<std::string>());
  EXPECT_NE(new_id, old_id);
  EXPECT_EQ(new_id & 15, 2u);
  EXPECT_LE(old_id, std::numeric_limits<std::uint32_t>::max());
  EXPECT_LE(new_id, std::numeric_limits<std::uint32_t>::max());
  EXPECT_EQ(combat.live_monster_health(new_id), 50);
  EXPECT_FALSE(combat.live_monster_health(old_id));
  EXPECT_TRUE(combat.gm_damage_monster(old_id, 100, kStart + 2100 * kMillisecond).empty());
  verify_encoding(respawn);
}

TEST(WorldMobs, WireIdsResolveAfterClientLowDwordNormalizationAndCreditIsSeparate) {
  const auto data = std::make_shared<CombatData>(combat_fixture());
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, here);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  EXPECT_EQ(id, static_cast<std::uint32_t>(id));
  EXPECT_EQ(id & 15, 2u);
  EXPECT_TRUE(is_server_npc_unit_id(id));
  combat.gm_damage_monster(id, 100, kStart);
  const auto first = combat.take_world_defeats();
  ASSERT_EQ(first.size(), 1u);
  EXPECT_NE(first.front().reward_credit_id, id);
  EXPECT_GT(first.front().reward_credit_id, std::numeric_limits<std::uint32_t>::max());
  EXPECT_EQ(first.front().reward_event_id.size(), 32U);

  const auto respawn = combat.advance_private_encounter(kStart + 2000 * kMillisecond, here);
  const auto next = std::stoull(find_packet(respawn, 10)->message.at("unit_id").get<std::string>());
  combat.gm_damage_monster(next, 100, kStart + 2000 * kMillisecond);
  const auto second = combat.take_world_defeats();
  ASSERT_EQ(second.size(), 1u);
  EXPECT_NE(first.front().reward_credit_id, second.front().reward_credit_id);
  EXPECT_EQ(second.front().reward_event_id.size(), 32U);
  EXPECT_NE(first.front().reward_event_id, second.front().reward_event_id);
}

TEST(WorldMobs, ExplicitSkillTargetCannotBypassFriendlyFaction) {
  auto map = world_mob_map();
  map["spawns"][0]["faction"] = "EX5Faction_Player";
  const auto data = std::make_shared<CombatData>(combat_fixture());
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(map, data));
  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, here);
  const auto id = find_packet(entry, 10)->message.at("unit_id");
  auto request = start_request();
  request["target_ids"] = Json::array({id});
  const auto refused = combat.receive(20010, request, here, kStart);
  ASSERT_NE(find_packet(refused, 13), nullptr);
  EXPECT_EQ(find_packet(refused, 13)->message.at("ec"), "Skill_InvalidTarget");
  EXPECT_FALSE(combat.busy());
  verify_encoding(refused);
}

TEST(WorldMobs, RoamingWalksWithoutPlayerInputAndRespawnStartsAtHome) {
  auto map = world_mob_map();
  map["spawns"][0]["roaming"] = {
      {"mode", "circle_forward"},
      {"nodes", Json::array({
          {{"position_mm", {{"x", 1000}, {"y", 0}, {"z", 0}}},
           {"speed_mm_per_second", 1000}, {"move_detail", "MoveDetail_Walk"}, {"wait_ms", 0}},
          {{"position_mm", {{"x", 4000}, {"y", 0}, {"z", 0}}},
           {"speed_mm_per_second", 1000}, {"move_detail", "MoveDetail_Walk"}, {"wait_ms", 500}}
      })}};
  const auto data = std::make_shared<CombatData>(combat_fixture());
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(map, data));
  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, here);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  const auto walking = combat.advance_private_encounter(kStart + 1000 * kMillisecond, here);
  const auto *move = find_packet(walking, 177);
  ASSERT_NE(move, nullptr);
  EXPECT_EQ(move->message.at("detail"), "MoveDetail_Walk");
  EXPECT_EQ(move->message.at("pos").at("millimeter_x"), 2000);
  combat.gm_damage_monster(id, 100, kStart + 1000 * kMillisecond);
  const auto respawn = combat.advance_private_encounter(kStart + 3000 * kMillisecond, here);
  ASSERT_NE(find_packet(respawn, 10), nullptr);
  EXPECT_EQ(find_packet(respawn, 10)->message.at("nub").at("pos").at("millimeter_x"), 1000);
  verify_encoding(walking);
  verify_encoding(respawn);
}

TEST(WorldMobs, OneTimeSpawnsStayDeadAndSightIsMapScoped) {
  const auto data = std::make_shared<CombatData>(combat_fixture());
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(9012, true), data));
  EXPECT_TRUE(combat.advance_private_encounter(kStart, {9012, 500000, 0, 0}).empty());
  EXPECT_TRUE(combat.advance_private_encounter(kStart, {9031, 0, 0, 0}).empty());
  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, here);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  combat.gm_damage_monster(id, 100, kStart);
  const auto expired = combat.advance_private_encounter(kStart + 90001 * kMillisecond, here);
  EXPECT_EQ(find_packet(expired, 10), nullptr);
  EXPECT_NE(find_packet(expired, 11), nullptr);
  EXPECT_EQ(find_packet(combat.advance_private_encounter(kStart + 91000 * kMillisecond, here), 11),
            nullptr);
}

TEST(WorldMobs, OnlyEngagedPlayersWorkerResolvesMonsterAttacks) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession second(data, nullptr, other, player_stats());
  first.attach_world_mobs(world);
  second.attach_world_mobs(world);
  const WorldPosition here{9012, 0, 0, 0};
  for (int step = 0; step <= 25; ++step) {
    const auto tick = kStart + step * 100 * kMillisecond;
    first.advance_private_encounter(tick, here);
    second.advance_private_encounter(tick, here);
  }
  EXPECT_LT(first.player_stats().at("69"), 100);
  EXPECT_EQ(second.player_stats().at("69"), 100);
  first.detach_world_mobs(kStart + 2600 * kMillisecond);
  second.advance_private_encounter(kStart + 3000 * kMillisecond, here);
  EXPECT_TRUE(second.next_action_tick());
}

TEST(WorldMobs, DepartingInstanceTargetDoesNotHealTheSharedMonster) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_stats"]["5001015"]["18"] = 1000;
  definitions["npc_stats"]["5001015"]["69"] = 1000;
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  auto distant_character = character();
  distant_character.id = 24;
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession second(data, nullptr, other, player_stats());
  CombatSession distant(data, nullptr, distant_character, player_stats());
  first.attach_world_mobs(world, true);
  second.attach_world_mobs(world, true);
  distant.attach_world_mobs(world, true);
  const WorldPosition here{9012, 0, 0, 0};
  const WorldPosition far_away{9012, 100000, 0, 0};
  const auto entry = first.advance_private_encounter(kStart, here);
  second.advance_private_encounter(kStart, here);
  distant.advance_private_encounter(kStart, far_away);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());

  for (int step = 1; step <= 10; ++step) {
    first.advance_private_encounter(kStart + step * 100 * kMillisecond, here);
  }
  first.gm_damage_monster(id, 100, kStart + 1100 * kMillisecond);
  ASSERT_EQ(second.live_monster_health(id), 900);

  first.detach_world_mobs(kStart + 1100 * kMillisecond);
  EXPECT_EQ(second.live_monster_health(id), 900);
  const auto distant_packets = distant.advance_private_encounter(kStart + 1200 * kMillisecond,
                                                                  far_away);
  EXPECT_TRUE(std::ranges::none_of(distant_packets, [](const GameplayPacket &packet) {
    return packet.opcode == 98 && packet.message.contains("new_target_unit_id");
  }));
  const auto nearby_packets = second.advance_private_encounter(kStart + 1300 * kMillisecond, here);
  EXPECT_TRUE(std::ranges::any_of(nearby_packets, [](const GameplayPacket &packet) {
    return packet.opcode == 98 && packet.message.contains("new_target_unit_id");
  }));
  EXPECT_EQ(second.live_monster_health(id), 900);
}

TEST(WorldMobs, FailedJournalBatchRestoresEveryEntryAndSequenceBeforeRetry) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_npc_attack());
  const std::vector<GameplayPacket> batch{{98, {{"unit_id", "0"}, {"marker", 1}}},
                                          {98, {{"unit_id", "0"}, {"marker", 2}}}};
  for (const std::size_t initial_entries : {std::size_t{0}, std::size_t{4096}}) {
    for (const int failure_entry : {1, 2}) {
      SCOPED_TRACE(initial_entries);
      SCOPED_TRACE(failure_entry);
      const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
      CombatSession owner(data, nullptr, character(), player_stats());
      owner.attach_world_mobs(world);
      owner.seed_world_detach_journal_for_test(initial_entries);
      const auto before = owner.world_detach_state_for_test();
      int inserted = 0;
      owner.set_world_journal_hook_for_test([&] {
        if (++inserted == failure_entry) {
          throw std::runtime_error("Injected world journal batch failure");
        }
      });

      EXPECT_THROW(owner.publish_world_journal_for_test(batch), std::runtime_error);
      EXPECT_EQ(inserted, failure_entry);
      const auto failed = owner.world_detach_state_for_test();
      EXPECT_EQ(failed.at("journal"), before.at("journal"));
      EXPECT_EQ(failed.at("sequence"), before.at("sequence"));

      owner.set_world_journal_hook_for_test({});
      owner.publish_world_journal_for_test(batch);
      const auto committed = owner.world_detach_state_for_test();
      EXPECT_EQ(committed.at("sequence").get<std::uint64_t>(),
                before.at("sequence").get<std::uint64_t>() + batch.size());
      ASSERT_EQ(committed.at("journal").size(), std::min<std::size_t>(4096, initial_entries + 2));
      EXPECT_EQ(committed.at("journal").at(committed.at("journal").size() - 2)
                    .at("message").at("marker"), 1);
      EXPECT_EQ(committed.at("journal").back().at("message").at("marker"), 2);
    }
  }
}

TEST(WorldMobs, FailedDetachRestoresMonsterFlightsAndJournalBeforeRetry) {
  for (const bool instance_copy : {false, true}) {
    for (const std::string_view checkpoint : {"owner_cleared", "journal_inserted"}) {
      SCOPED_TRACE(instance_copy ? "instance copy" : "shared world");
      SCOPED_TRACE(checkpoint);
      auto definitions = combat_fixture_with_npc_attack();
      definitions["npc_stats"]["5001015"]["18"] = 1000;
      definitions["npc_stats"]["5001015"]["69"] = 1000;
      definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
      const auto data = std::make_shared<CombatData>(definitions);
      const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
      auto other = character();
      other.id = 23;
      CombatSession owner(data, nullptr, character(), player_stats());
      CombatSession observer(data, nullptr, other, player_stats());
      const auto owner_lifetime = std::make_shared<SyntheticMilestoneOwner>(owner);
      const auto observer_lifetime = std::make_shared<SyntheticMilestoneOwner>(observer);
      owner.attach_world_mobs(world, instance_copy);
      observer.attach_world_mobs(world, instance_copy);
      const auto binding = owner.bind_world_milestone_owner(1, owner_lifetime);
      ASSERT_TRUE(binding);
      const WorldPosition here{9012, 0, 0, 0};
      const auto entry = owner.advance_private_encounter(kStart, here);
      observer.advance_private_encounter(kStart, here);
      ASSERT_NE(find_packet(entry, 10), nullptr);
      const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
      for (int step = 1; step <= 10; ++step) {
        owner.advance_private_encounter(kStart + step * 100 * kMillisecond, here);
      }
      owner.gm_damage_monster(id, 100, kStart + 1100 * kMillisecond);
      const auto before = owner.world_detach_state_for_test();
      ASSERT_NE(owner.monster_state_fingerprint_for_test().find("own=7"), std::string::npos);
      bool reached = false;
      owner.set_world_detach_hook_for_test([&](std::string_view phase) {
        if (phase == checkpoint) {
          reached = true;
          throw std::runtime_error("Injected detach failure");
        }
      });
      EXPECT_THROW(owner.detach_world_mobs(kStart + 1100 * kMillisecond), std::runtime_error);
      ASSERT_TRUE(reached);
      EXPECT_EQ(owner.world_detach_state_for_test(), before);
      EXPECT_EQ(observer.live_monster_health(id), 900);
      EXPECT_FALSE(observer.bind_world_milestone_owner(1, observer_lifetime));

      owner.set_world_detach_hook_for_test({});
      owner.detach_world_mobs(kStart + 1100 * kMillisecond);
      EXPECT_FALSE(owner.has_world_mobs());
      const auto rebound = observer.bind_world_milestone_owner(1, observer_lifetime);
      ASSERT_TRUE(rebound);
      EXPECT_NE(rebound->generation, binding->generation);
      EXPECT_EQ(observer.live_monster_health(id), instance_copy ? 900 : 1000);
      const auto after = observer.world_detach_state_for_test();
      owner.detach_world_mobs(kStart + 1100 * kMillisecond);
      EXPECT_EQ(observer.world_detach_state_for_test(), after);
    }
  }
}

TEST(WorldMobs, FailedInstanceInterruptionRestoresSharedEpochAndAction) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession owner(data, nullptr, character(), player_stats());
  owner.attach_world_mobs(world, true);
  const WorldPosition here{9012, 0, 0, 0};
  for (int step = 0; step <= 10; ++step) {
    owner.advance_private_encounter(kStart + step * 100 * kMillisecond, here);
  }
  const auto before = owner.world_detach_state_for_test();
  bool reached = false;
  owner.set_world_detach_hook_for_test([&](std::string_view phase) {
    if (phase == "interrupted") {
      reached = true;
      throw std::runtime_error("Injected interruption failure");
    }
  });
  EXPECT_THROW(owner.detach_world_mobs(kStart + 1100 * kMillisecond), std::runtime_error);
  ASSERT_TRUE(reached);
  EXPECT_EQ(owner.world_detach_state_for_test(), before);
  owner.set_world_detach_hook_for_test({});
  EXPECT_NO_THROW(owner.detach_world_mobs(kStart + 1100 * kMillisecond));
}

TEST(WorldMobs, FailedDetachAtFullJournalRestoresPrunedHistoryAndSequence) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession owner(data, nullptr, character(), player_stats());
  CombatSession observer(data, nullptr, other, player_stats());
  owner.attach_world_mobs(world);
  observer.attach_world_mobs(world);
  const WorldPosition here{9012, 0, 0, 0};
  for (int step = 0; step <= 10; ++step) {
    owner.advance_private_encounter(kStart + step * 100 * kMillisecond, here);
  }
  owner.seed_world_detach_journal_for_test(4096);
  const auto before = owner.world_detach_state_for_test();
  ASSERT_EQ(before.at("journal").size(), 4096U);
  int inserted = 0;
  owner.set_world_detach_hook_for_test([&](std::string_view phase) {
    if (phase == "journal_inserted" && ++inserted == 2) {
      throw std::runtime_error("Injected partial journal failure");
    }
  });
  EXPECT_THROW(owner.detach_world_mobs(kStart + 1100 * kMillisecond), std::runtime_error);
  ASSERT_EQ(inserted, 2);
  EXPECT_EQ(owner.world_detach_state_for_test(), before);

  owner.set_world_detach_hook_for_test({});
  owner.detach_world_mobs(kStart + 1100 * kMillisecond);
  const auto after = observer.world_detach_state_for_test();
  EXPECT_EQ(after.at("journal").size(), 4096U);
  EXPECT_GT(after.at("sequence").get<std::uint64_t>(),
            before.at("sequence").get<std::uint64_t>());
}

TEST(WorldMobs, RetriedDetachDoesNotRepublishAnEarlierCommittedMonster) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto data = std::make_shared<CombatData>(definitions);
  auto map = world_mob_map();
  auto second_spawn = map["spawns"][0];
  second_spawn["unit_id"] = "576460752303423522";
  second_spawn["spawner_guid"] = {"3", "4"};
  second_spawn["position"] = {1500, 0, 0};
  map["spawns"].push_back(std::move(second_spawn));
  const auto world = CombatSession::create_world_mobs(map, data);
  auto other = character();
  other.id = 23;
  CombatSession owner(data, nullptr, character(), player_stats());
  CombatSession observer(data, nullptr, other, player_stats());
  owner.attach_world_mobs(world, true);
  observer.attach_world_mobs(world, true);
  const WorldPosition here{9012, 0, 0, 0};
  owner.advance_private_encounter(kStart, here);
  observer.advance_private_encounter(kStart, here);
  for (int step = 1; step <= 10; ++step) {
    owner.advance_private_encounter(kStart + step * 100 * kMillisecond, here);
  }
  int cleared = 0;
  owner.set_world_detach_hook_for_test([&](std::string_view phase) {
    if (phase == "owner_cleared" && ++cleared == 2) {
      throw std::runtime_error("Injected second monster failure");
    }
  });
  EXPECT_THROW(owner.detach_world_mobs(kStart + 1100 * kMillisecond), std::runtime_error);
  ASSERT_EQ(cleared, 2);
  const auto partial = observer.world_detach_state_for_test();
  owner.set_world_detach_hook_for_test({});
  owner.detach_world_mobs(kStart + 1100 * kMillisecond);
  const auto completed = observer.world_detach_state_for_test();
  const auto &partial_journal = partial.at("journal");
  const auto &completed_journal = completed.at("journal");
  ASSERT_LE(partial_journal.size(), completed_journal.size());
  for (std::size_t index = 0; index < partial_journal.size(); ++index) {
    EXPECT_EQ(partial_journal[index], completed_journal[index]);
  }
  owner.detach_world_mobs(kStart + 1100 * kMillisecond);
  EXPECT_EQ(observer.world_detach_state_for_test(), completed);
}

TEST(WorldMobs, SameInstanceHandoffKeepsTheMonstersHealth) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_stats"]["5001015"]["18"] = 1000;
  definitions["npc_stats"]["5001015"]["69"] = 1000;
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession player(data, nullptr, character(), player_stats());
  player.attach_world_mobs(world, true);
  const WorldPosition here{9012, 0, 0, 0};
  const auto entry = player.advance_private_encounter(kStart, here);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());

  for (int step = 1; step <= 10; ++step) {
    player.advance_private_encounter(kStart + step * 100 * kMillisecond, here);
  }
  player.gm_damage_monster(id, 100, kStart + 1100 * kMillisecond);
  player.detach_world_mobs(kStart + 1100 * kMillisecond);
  player.attach_world_mobs(world, true);
  player.advance_private_encounter(kStart + 1200 * kMillisecond, here);
  EXPECT_EQ(player.live_monster_health(id), 900);
}

TEST(WorldMobs, LegacyMonsterWaitsForANearbyInstanceTargetAfterDeparture) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_stats"]["5001015"]["18"] = 1000;
  definitions["npc_stats"]["5001015"]["69"] = 1000;
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto near_character = character();
  near_character.id = 23;
  auto far_character = character();
  far_character.id = 24;
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession nearby(data, nullptr, near_character, player_stats());
  CombatSession distant(data, nullptr, far_character, player_stats());
  first.attach_world_mobs(world, true);
  nearby.attach_world_mobs(world, true);
  distant.attach_world_mobs(world, true);
  const WorldPosition here{9012, 0, 0, 0};
  const WorldPosition far_away{9012, 100000, 0, 0};
  const auto entry = first.advance_private_encounter(kStart, here);
  nearby.advance_private_encounter(kStart, here);
  distant.advance_private_encounter(kStart, far_away);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());

  first.receive(20010, start_request(), here, kStart);
  first.advance_private_encounter(kStart + 300 * kMillisecond, here);
  const auto health = nearby.live_monster_health(id);
  ASSERT_TRUE(health);
  ASSERT_LT(*health, 1000);
  first.detach_world_mobs(kStart + 400 * kMillisecond);

  const auto far_packets = distant.advance_private_encounter(kStart + 500 * kMillisecond,
                                                              far_away);
  EXPECT_TRUE(std::ranges::none_of(far_packets, [](const GameplayPacket &packet) {
    return packet.opcode == 98 && packet.message.contains("new_target_unit_id");
  }));
  const auto near_packets = nearby.advance_private_encounter(kStart + 600 * kMillisecond, here);
  EXPECT_TRUE(std::ranges::any_of(near_packets, [](const GameplayPacket &packet) {
    return packet.opcode == 98 && packet.message.contains("new_target_unit_id");
  }));
  EXPECT_EQ(nearby.live_monster_health(id), health);
}

TEST(WorldMobs, PrivateGmMonstersDoNotBecomePublicOrGrantWorldRewards) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession second(data, nullptr, other, player_stats());
  first.attach_world_mobs(world);
  second.attach_world_mobs(world);
  const WorldPosition here{9012, 0, 0, 0};
  first.advance_private_encounter(kStart, here);
  second.advance_private_encounter(kStart, here);
  constexpr std::uint64_t private_id = 0x3000000000000012ULL;
  int loot_rolls = 0;
  first.set_defeat_observer([&](const MonsterDefeat &) {
    ++loot_rolls;
    return MonsterDefeatReaction{};
  });
  ASSERT_TRUE(first.add_gm_monster(private_id, 5001015, {9012, 2000, 0, 0}));
  EXPECT_FALSE(second.live_monster_health(private_id));
  const auto seen = second.advance_private_encounter(kStart + 100 * kMillisecond, here);
  EXPECT_EQ(find_packet(seen, 10), nullptr);
  EXPECT_TRUE(second.gm_damage_monster(private_id, 100, kStart).empty());
  first.gm_damage_monster(private_id, 100, kStart);
  EXPECT_TRUE(first.take_world_defeats().empty());
  EXPECT_EQ(loot_rolls, 0);
}

TEST(WorldMobs, SeparateChannelsOfTheSameMapHaveIndependentLives) {
  const auto data = std::make_shared<CombatData>(combat_fixture());
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession second(data, nullptr, character(), player_stats());
  first.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  second.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  const WorldPosition here{9012, 0, 0, 0};
  const auto first_entry = first.advance_private_encounter(kStart, here);
  const auto second_entry = second.advance_private_encounter(kStart, here);
  const auto first_id =
      std::stoull(find_packet(first_entry, 10)->message.at("unit_id").get<std::string>());
  const auto second_id =
      std::stoull(find_packet(second_entry, 10)->message.at("unit_id").get<std::string>());
  EXPECT_NE(first_id, second_id);
  first.gm_damage_monster(first_id, 100, kStart);
  EXPECT_EQ(second.live_monster_health(second_id), 50);
  EXPECT_FALSE(second.live_monster_health(first_id));
}

TEST(WorldMobs, GeneratedMapPopulationsLoadAndEncode) {
  const auto profile_path = std::getenv("CHRONICLES_WORLD_MOB_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Set CHRONICLES_WORLD_MOB_PROFILE to the verified generated profile";
  }
  std::ifstream input(profile_path);
  Json profile;
  input >> profile;
  const auto data = std::make_shared<CombatData>(profile.at("combat"));
  std::size_t populated_maps = 0;
  std::set<int> checked_attack_types;
  for (const auto &map : profile.at("maps")) {
    const auto population = CombatSession::create_world_mobs(map, data);
    if (map.at("spawns").empty()) {
      continue;
    }
    ++populated_maps;
    CombatSession combat(data, nullptr, character(), player_stats());
    combat.attach_world_mobs(population);
    const auto &point = map.at("spawns").front().at("position");
    const WorldPosition here{map.at("map_id"), point.at(0), point.at(1), point.at(2)};
    const auto packets = combat.advance_private_encounter(kStart, here);
    EXPECT_NE(find_packet(packets, 10), nullptr) << here.map_id;
    verify_encoding(packets);
    for (const auto &spawn : map.at("spawns")) {
      const int cms = spawn.at("cms_id");
      if (spawn.at("faction") == "EX5Faction_Player" || !checked_attack_types.insert(cms).second) {
        continue;
      }
      bool executable_attack = false;
      for (const auto &skill_id : data->npc_skills.at(std::to_string(cms))) {
        const auto skill = data->skills.find(skill_id.get<int>());
        if (skill == data->skills.end()) {
          continue;
        }
        const double distance =
            (skill->second.value("min_range_mm", 0) + skill->second.at("range_mm").get<int>()) /
            2.0;
        if (select_npc_attack_skill(*data, cms, {}, distance, kStart)) {
          executable_attack = true;
          break;
        }
      }
      EXPECT_TRUE(executable_attack) << "NPC " << cms;
    }
  }
  EXPECT_GT(populated_maps, 1u);
}

// Every generated monster type with locomotion and an attack is provoked by a player standing
// 4 m in front of it. Before the 2026-10-27 combat fix, melee monsters stopped at their catalog
// maxDistance and swung shapes that could not reach (no damage although the attack played);
// a type that keeps casting for 10 s without one hit on a standing player is that defect.
TEST(WorldMobs, GeneratedMonstersLandTheirAttacksOnAStandingPlayer) {
  const auto profile_path = std::getenv("CHRONICLES_WORLD_MOB_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Set CHRONICLES_WORLD_MOB_PROFILE to the verified generated profile";
  }
  std::ifstream input(profile_path);
  Json profile;
  input >> profile;
  const auto data = std::make_shared<CombatData>(profile.at("combat"));

  constexpr int kOpenWorldMap = 9012;
  constexpr std::uint64_t kTestMonster = 0x3000000000000012ULL;
  constexpr int kSimulatedMs = 10000;
  const auto player_id_text = std::to_string(character().id);
  const WorldPosition player{kOpenWorldMap, 4000, 0, 0, 0.0F};

  std::size_t simulated = 0;
  std::vector<int> casting_without_hits;
  std::vector<int> never_casting;
  for (const auto &[cms_id, attacks] : data->npc_attacks) {
    if (attacks.empty() || !data->npc_locomotion.contains(cms_id)) {
      continue;
    }
    // Enough health that no monster kills the player during the run.
    auto stats = player_stats();
    stats["18"] = 100000000;
    CombatSession combat(data, nullptr, character(), stats);
    if (!combat.add_gm_monster(kTestMonster, cms_id, {kOpenWorldMap, 0, 0, 0, 0.0F})) {
      continue;
    }
    combat.advance_private_encounter(kStart, player);
    combat.gm_damage_monster(kTestMonster, 1, kStart);

    int casts = 0;
    int hits = 0;
    std::string cast_summary;
    for (int elapsed_ms = 100; elapsed_ms <= kSimulatedMs; elapsed_ms += 100) {
      const auto ticks = kStart + elapsed_ms * kMillisecond;
      for (const auto &packet : combat.advance_private_encounter(ticks, player)) {
        if (packet.opcode == 14 && packet.message.at("unit_id") == std::to_string(kTestMonster)) {
          ++casts;
          const auto &start = packet.message.at("start_skill");
          cast_summary += " " + std::to_string(start.at("skill_id").get<int>()) + "@" +
                          std::to_string(start.at("pos").value("millimeter_x", 0));
        }
        if (packet.opcode == 19 && packet.message.value("target_unit_id", "") == player_id_text) {
          ++hits;
        }
      }
    }
    ++simulated;
    if (casts == 0) {
      never_casting.push_back(cms_id);
    }
    if (casts > 0 && hits == 0) {
      casting_without_hits.push_back(cms_id);
      std::cout << "  " << cms_id << ":" << cast_summary << "\n";
    }
  }

  std::cout << "simulated " << simulated << " monster types; casting without a hit: "
            << casting_without_hits.size() << "; never casting: " << never_casting.size()
            << "\n";
  for (const auto cms_id : never_casting) {
    std::cout << "  never casts: " << cms_id << "\n";
  }
  EXPECT_GT(simulated, 100u);
  // Status-only or conditional repertoires may legitimately never damage; ordinary attacks must.
  EXPECT_LE(casting_without_hits.size() * 20, simulated)
      << "more than 5% of monster types swing without ever landing on a standing player";
  // The reach gate must not leave monsters without any usable attack against a nearby player.
  EXPECT_LE(never_casting.size() * 20, simulated)
      << "more than 5% of monster types never attack a player 4 m in front of them";
}

// The shared world-mob tick runs at least every 100 ms and around every combat request, on the
// worker strand that also answers the player's packets, Ping included. Live logs from
// 2026-10-27 showed every packet waiting 40-175 ms there while each tick re-walked the NPC
// skill/effect JSON of every mob on the map. This bounds one tick on the busiest generated map.
TEST(WorldMobs, GeneratedBusiestMapTickStaysWithinPacketBudget) {
  const auto profile_path = std::getenv("CHRONICLES_WORLD_MOB_PROFILE");
  if (!profile_path) {
    GTEST_SKIP() << "Set CHRONICLES_WORLD_MOB_PROFILE to the verified generated profile";
  }
  std::ifstream input(profile_path);
  Json profile;
  input >> profile;
  const auto data = std::make_shared<CombatData>(profile.at("combat"));

  const Json *busiest = nullptr;
  for (const auto &map : profile.at("maps")) {
    if (!busiest || map.at("spawns").size() > busiest->at("spawns").size()) {
      busiest = &map;
    }
  }
  ASSERT_NE(busiest, nullptr);
  ASSERT_FALSE(busiest->at("spawns").empty());

  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(*busiest, data));
  const auto &point = busiest->at("spawns").front().at("position");
  const WorldPosition here{busiest->at("map_id"), point.at(0), point.at(1), point.at(2)};
  // The first tick announces the nearby population; it is not part of the steady state.
  combat.advance_private_encounter(kStart, here);

  constexpr int kTicks = 50;
  constexpr std::int64_t kTickIntervalTicks = 100 * kMillisecond;
  constexpr double kTickBudgetMs = 10.0;
  const auto started = std::chrono::steady_clock::now();
  for (int tick = 1; tick <= kTicks; ++tick) {
    combat.advance_private_encounter(kStart + tick * kTickIntervalTicks, here);
  }
  const std::chrono::duration<double, std::milli> elapsed =
      std::chrono::steady_clock::now() - started;

  const double average_ms = elapsed.count() / kTicks;
  std::cout << "World-mob tick on map " << busiest->at("map_id") << " with "
            << busiest->at("spawns").size() << " spawns: " << average_ms << " ms average\n";
  EXPECT_LT(average_ms, kTickBudgetMs);
}

TEST(CombatData, NpcAttackOptionsKeepCatalogOrderRangesAndSkipNonAttacks) {
  auto definitions = combat_fixture_with_npc_attack();
  Json dodge = definitions.at("skills").at(1);
  dodge["id"] = 202;
  dodge["type"] = "Dodge";
  Json ranged = definitions.at("skills").at(1);
  ranged["id"] = 203;
  ranged["min_range_mm"] = 3000;
  ranged["range_mm"] = 12000;
  ranged["layers"][0]["stages"][0]["events"] = {
      {{"effect_id", 4}, {"unique_id", 0}, {"time_ms", 300}}};
  definitions["effects"]["2"] = definitions.at("effects").at("1");
  definitions["effects"]["4"] = {
      {"kind", "spawnProjectile"}, {"parameters", {{"projectileId", 3}}}, {"chain", 0}};
  definitions["projectiles"]["3"] = {
      {"duration_ms", 2000}, {"parameters", {{"speed", 100}}}, {"hit_effects", {2}}};
  definitions["skills"].push_back(dodge);
  definitions["skills"].push_back(ranged);
  // 999 is not a combat skill; listed order is the selection tie-break order.
  definitions["npc_skills"]["5001015"] = {203, 202, 999, 201};
  const CombatData data(definitions);

  const auto &attacks = data.npc_attack_options(5001015);
  ASSERT_EQ(attacks.size(), 2u);
  EXPECT_EQ(attacks[0].skill_id, 203);
  EXPECT_EQ(attacks[0].min_range_mm, 3000);
  EXPECT_EQ(attacks[0].range_mm, 12000);
  EXPECT_TRUE(attacks[0].ranged);
  EXPECT_EQ(attacks[1].skill_id, 201);
  EXPECT_EQ(attacks[1].min_range_mm, 0);
  EXPECT_EQ(attacks[1].range_mm, 5000);
  EXPECT_FALSE(attacks[1].ranged);
  EXPECT_TRUE(data.npc_attack_options(424242).empty());

  // Band selection reads the table: only the ranged attack covers 10 m, only melee covers 1 m.
  EXPECT_EQ(select_npc_attack_skill(data, 5001015, {}, 10000, kStart), 203);
  EXPECT_EQ(select_npc_attack_skill(data, 5001015, {}, 1000, kStart), 201);
}

TEST(Combat, PrivateGmEncounterOnAnOpenWorldMapRunsBeforeItCanAttack) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  constexpr std::uint64_t unit_id = 0x3000000000000012ULL;
  const WorldPosition spawn{9012, 6000, 0, 0, 180.0F};
  const WorldPosition player{9012, 0, 0, 0};
  ASSERT_TRUE(combat.add_gm_monster(unit_id, 5001015, spawn));
  EXPECT_TRUE(combat.private_encounter_active());
  EXPECT_FALSE(combat.add_gm_monster(unit_id + 16, 5001015, {8000, 6000, 0, 0}));

  auto packets = combat.advance_private_encounter(kStart, player);
  ASSERT_NE(find_packet(packets, 177), nullptr);
  EXPECT_EQ(find_packet(packets, 177)->message.at("detail"), "MoveDetail_Run");
  EXPECT_EQ(find_packet(packets, 14), nullptr);
  EXPECT_EQ(find_packet(packets, 64), nullptr);
  EXPECT_EQ(find_packet(packets, 106), nullptr);
  EXPECT_TRUE(combat.next_action_tick());
  for (int step = 1; step <= 25; ++step) {
    auto due = combat.advance_private_encounter(kStart + step * 100 * kMillisecond, player);
    packets.insert(packets.end(), due.begin(), due.end());
  }
  const auto *attack = find_packet(packets, 14);
  ASSERT_NE(attack, nullptr);
  EXPECT_EQ(attack->message.at("unit_id"), std::to_string(unit_id));
  EXPECT_GE(attack->message.at("start_skill").at("pos").at("millimeter_x"), 1999);
  EXPECT_LE(attack->message.at("start_skill").at("pos").at("millimeter_x"), 2001);
  EXPECT_LT(combat.player_stats().at("69"), 100);
  verify_encoding(packets);
}

TEST(Combat, GmDamageRunsDefeatObserverExactlyOnce) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  constexpr std::uint64_t unit_id = 0xF0010012;
  ASSERT_TRUE(combat.add_gm_monster(unit_id, 5001015, {9012, 6000, 0, 0}));
  int defeats = 0;
  combat.set_defeat_observer([&](const MonsterDefeat &defeat) {
    EXPECT_EQ(defeat.unit_id, unit_id);
    ++defeats;
    return MonsterDefeatReaction{};
  });
  const auto health = combat.live_monster_health(unit_id);
  ASSERT_TRUE(health);
  const auto packets = combat.gm_damage_monster(unit_id, *health, kStart);
  EXPECT_EQ(defeats, 1);
  EXPECT_NE(find_packet(packets, 19), nullptr);
  EXPECT_NE(find_packet(packets, 23), nullptr);
  EXPECT_FALSE(combat.live_monster_health(unit_id));
  EXPECT_TRUE(combat.gm_damage_monster(unit_id, *health, kStart).empty());
  EXPECT_EQ(defeats, 1);
  verify_encoding(packets);
}

TEST(Combat, GmPlacementCancelsTheOldRoute) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  constexpr std::uint64_t unit_id = 0xF0010012;
  ASSERT_TRUE(combat.add_gm_monster(unit_id, 5001015, {9012, 6000, 0, 0}));
  combat.advance_private_encounter(kStart, {9012, 0, 0, 0});
  const WorldPosition destination{9012, 20000, 10000, 0, 90.0F};
  const auto packets = combat.gm_place_monster(unit_id, destination, kStart);
  const auto view = combat.monster_debug_view(unit_id, kStart + 100 * kMillisecond);
  ASSERT_TRUE(view);
  EXPECT_EQ(view->position, destination);
  EXPECT_FALSE(view->route_end);
  verify_encoding(packets);
}

TEST(Combat, GmStatsCannotBypassDeathOrRevival) {
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()), nullptr,
                       character(), player_stats());
  EXPECT_TRUE(combat.gm_set_player_stat(69, 0, kStart).empty());
  EXPECT_FALSE(combat.player_dead());
  combat.gm_kill_player({9012, 0, 0, 0}, kStart);
  EXPECT_TRUE(combat.gm_set_player_stat(69, 100, kStart).empty());
  EXPECT_TRUE(combat.player_dead());
}

TEST(Combat, GmMonsterRequiresCompleteLocomotionInsteadOfBecomingAStationaryAttacker) {
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()), nullptr,
                       character(), player_stats());
  EXPECT_FALSE(combat.add_gm_monster(0x3000000000000012ULL, 5001015, {9012, 6000, 0, 0}));
  EXPECT_FALSE(combat.private_encounter_active());
}

TEST(Combat, GmSuicideKillsThePlayerEvenUnderGmProtection) {
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()), nullptr,
                       character(), player_stats());
  const WorldPosition position{9012, 0, 0, 0};
  combat.set_gm_protection(true, false, false);
  EXPECT_TRUE(combat.apply_fall_damage(50, position, kStart).empty());

  const auto death = combat.gm_kill_player(position, kStart);
  EXPECT_TRUE(combat.player_dead());
  EXPECT_EQ(combat.player_stats().at("69"), 0);
  EXPECT_NE(find_packet(death, 23), nullptr);
  EXPECT_NE(find_packet(death, 54), nullptr);
  EXPECT_TRUE(combat.gm_kill_player(position, kStart + kMillisecond).empty());
  verify_encoding(death);
}

TEST(Combat, MonsterDebugViewReportsTheRouteWithoutChangingTheMonster) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  constexpr std::uint64_t unit_id = 0x3000000000000012ULL;
  const WorldPosition spawn{9012, 6000, 0, 0, 180.0F};
  ASSERT_TRUE(combat.add_gm_monster(unit_id, 5001015, spawn));
  EXPECT_FALSE(combat.monster_debug_view(unit_id + 16, kStart));

  const auto idle = combat.monster_debug_view(unit_id, kStart);
  ASSERT_TRUE(idle);
  EXPECT_TRUE(idle->alive);
  EXPECT_EQ(idle->position.x_mm, 6000);
  EXPECT_EQ(idle->ai_state, NpcAiState::Idle);
  EXPECT_FALSE(idle->route_end);

  // The pursuit starts on the first AI step; the view follows the route at a later tick.
  combat.advance_private_encounter(kStart, {9012, 0, 0, 0});
  const auto pursuing = combat.monster_debug_view(unit_id, kStart + 200 * kMillisecond);
  ASSERT_TRUE(pursuing);
  EXPECT_EQ(pursuing->ai_state, NpcAiState::Engaged);
  ASSERT_TRUE(pursuing->route_end);
  EXPECT_LT(pursuing->position.x_mm, 6000);
  EXPECT_GE(pursuing->position.x_mm, pursuing->route_end->x_mm);
  const auto repeated = combat.monster_debug_view(unit_id, kStart + 200 * kMillisecond);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->position, pursuing->position);
}

TEST(Combat, APrivateGmKillDoesNotBecomeAnAuthoredTutorialQuestKill) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  int commits = 0;
  TutorialSession tutorial(tutorial_data, 7, {}, [&](const auto &, const auto &) {
    ++commits;
    return true;
  });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  constexpr std::uint64_t unit_id = 0x3000000000000012ULL;
  ASSERT_TRUE(combat.add_gm_monster(unit_id, 5001015, {8000, 1000, 0, 0}));
  auto request = start_request();
  request["target_ids"] = Json::array({std::to_string(unit_id)});
  ASSERT_NE(find_packet(combat.receive(20010, request, {8000, 0, 0, 0}, kStart), 14), nullptr);
  const auto killed = combat.advance(kStart + 300 * kMillisecond, {8000, 0, 0, 0}, tutorial);
  EXPECT_EQ(combat.monster_hp(unit_id), 0);
  EXPECT_NE(find_packet(killed, 23), nullptr);
  EXPECT_EQ(commits, 0);
  verify_encoding(killed);
}

TEST(Combat, NpcLocomotionLoaderRejectsHalfPresentSpeedAndInvalidRanges) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  definitions["npc_locomotion"]["5001015"].erase("move_speed_rate");
  EXPECT_THROW(CombatData{definitions}, std::runtime_error);
  definitions["npc_locomotion"]["5001015"] = npc_ai_block();
  definitions["npc_locomotion"]["5001015"]["combat_distance_min_mm"] = 99999;
  EXPECT_THROW(CombatData{definitions}, std::runtime_error);
}

TEST(Combat, MonsterWithAiDefinitionAcquiresPlayerAndAnnouncesRunningInWireTicks) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  const auto wire_player_id = player_wire_unit_id(19);
  auto wire_character = character();
  wire_character.id = static_cast<std::int64_t>(wire_player_id);
  TutorialSession tutorial(tutorial_data, wire_player_id, {},
                           [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()),
                       tutorial_data, wire_character, player_stats());
  const WorldPosition position{8000, 6000, 0, 0};

  const auto acquired = combat.advance(kStart, position, tutorial);
  ASSERT_NE(find_packet(acquired, 98), nullptr);
  EXPECT_EQ(find_packet(acquired, 98)->message.at("new_target_unit_id"),
            std::to_string(wire_player_id));
  const auto *state = find_packet(acquired, 32);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->message.at("state"), "EX5UnitStateType_Combat");
  EXPECT_EQ(state->message.at("tick"), std::to_string(kStart * 100));
  ASSERT_NE(find_packet(acquired, 114), nullptr);
  EXPECT_EQ(find_packet(acquired, 114)->message.at("new_ai_phase_transition_id"), 1230);

  // Pursuit is ordinary locomotion. No skill has started, so 64/106 would be ignored.
  EXPECT_EQ(find_packet(acquired, 64), nullptr);
  EXPECT_EQ(find_packet(acquired, 106), nullptr);
  const auto *move = find_packet(acquired, 177);
  ASSERT_NE(move, nullptr);
  EXPECT_EQ(move->message.at("detail"), "MoveDetail_Run");
  EXPECT_EQ(move->message.at("tick"), std::to_string(kStart * 100));
  EXPECT_EQ(move->message.at("pos").at("millimeter_x"), 1000);
  EXPECT_EQ(wire_player_id & 0xF, 1u);
  EXPECT_EQ(wire_player_id & (1ULL << 61), 0u);
  EXPECT_EQ(combat.next_action_tick(), kStart + 100 * kMillisecond);
  verify_encoding(acquired);

  const auto arrived = combat.advance(kStart + 1000 * kMillisecond, position, tutorial);
  EXPECT_EQ(find_packet(arrived, 106), nullptr);
  const auto *finished = find_packet(arrived, 177);
  ASSERT_NE(finished, nullptr);
  EXPECT_EQ(finished->message.at("detail"), "MoveDetail_Stop");
  EXPECT_EQ(finished->message.at("pos").at("millimeter_x"), 4000);
  const auto *attack = find_packet(arrived, 14);
  ASSERT_NE(attack, nullptr);
  EXPECT_EQ(attack->message.at("start_skill").at("pos").at("millimeter_x"), 4000);
  verify_encoding(arrived);
}

TEST(Combat, DamagedMonsterWithAiDefinitionAnnouncesItsAggroExactlyOnce) {
  auto definitions = combat_fixture();
  definitions["effects"]["1"]["parameters"]["physicalCoefficient"] = 0.1;
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  ASSERT_EQ(combat.receive(20010, start_request(), position, kStart).size(), 2u);

  const auto hit = combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  EXPECT_GT(combat.monster_hp(kMonster), 0);
  EXPECT_LT(combat.monster_hp(kMonster), 50);
  EXPECT_NE(find_packet(hit, 98), nullptr);
  const auto combat_states = std::ranges::count_if(hit, [](const GameplayPacket &packet) {
    return packet.opcode == 32 && packet.message.at("state") == "EX5UnitStateType_Combat";
  });
  EXPECT_EQ(combat_states, 1);
  verify_encoding(hit);
}

TEST(Combat, MonsterWithoutAiDefinitionKeepsLegacyBehaviourAndIgnoresProximity) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()),
                       tutorial_data, character(), player_stats());
  EXPECT_TRUE(combat.advance(kStart, {8000, 6000, 0, 0}, tutorial).empty());
}

TEST(Combat, MonsterWithBothSpeedFieldsAbsentKeepsLegacyBehaviour) {
  auto block = npc_ai_block();
  block.erase("run_speed_mm_per_second");
  block.erase("move_speed_rate");
  const auto tutorial_data = tutorial_fixture_with_ai(block);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()),
                       tutorial_data, character(), player_stats());
  EXPECT_TRUE(combat.advance(kStart, {8000, 6000, 0, 0}, tutorial).empty());
}

TEST(Combat, MonsterWithHalfPresentSpeedPairRejectsTheProfile) {
  for (const char *removed : {"run_speed_mm_per_second", "move_speed_rate"}) {
    auto block = npc_ai_block();
    block.erase(removed);
    EXPECT_THROW(CombatSession(std::make_shared<CombatData>(combat_fixture()),
                               tutorial_fixture_with_ai(block), character(), player_stats()),
                 std::runtime_error)
        << removed;
  }
}

// Every outgoing tick field must be wire nanoseconds: a multiple of 100 and not below the first
// server tick converted to nanoseconds. A raw internal 100 ns tick would fall below the bound.
void expect_wire_ticks(const Json &value, std::int64_t first_internal_tick) {
  if (value.is_array()) {
    for (const auto &child : value) {
      expect_wire_ticks(child, first_internal_tick);
    }
  } else if (value.is_object()) {
    for (const auto &item : value.items()) {
      const auto &key = item.key();
      const bool is_tick =
          key.size() >= 4 && key.compare(key.size() - 4, 4, "tick") == 0 && item.value().is_string();
      if (is_tick) {
        const auto wire = std::stoll(item.value().get<std::string>());
        EXPECT_EQ(wire % 100, 0) << key;
        EXPECT_GE(wire, first_internal_tick * 100) << key;
      } else {
        expect_wire_ticks(item.value(), first_internal_tick);
      }
    }
  }
}

Json combat_fixture_with_npc_projectile() {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["effects"]["2"] = definitions["effects"]["1"];
  definitions["effects"]["1"]["kind"] = "spawnProjectile";
  definitions["effects"]["1"]["parameters"] = {{"projectileId", 3}};
  definitions["projectiles"]["3"] = {
      {"duration_ms", 2000}, {"parameters", {{"speed", 100}}}, {"hit_effects", {2}}};
  return definitions;
}

Json combat_fixture_with_stunnable_npc_projectile() {
  constexpr int kStunStatus = 18;
  auto definitions = combat_fixture_with_npc_projectile();
  definitions["npc_stats"]["5001015"]["18"] = 1000;
  definitions["npc_stats"]["5001015"]["69"] = 1000;
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  definitions["status_effects"] = {{"18", {{"duration_ms", 2000}}}};
  definitions["abnormal_gauges"] = {
      {"reduce_tick_ms", 100},
      {"types", {{"Stun", {{"maximum", 20000}, {"reduce_per_tick", 40},
                            {"status_effect_id", kStunStatus}}}}}};
  definitions["effects"]["4"] = definitions["effects"]["2"];
  definitions["effects"]["4"]["status_effect_id"] = kStunStatus;
  definitions["skills"][0]["layers"][0]["stages"][0]["events"][0]["effect_id"] = 4;
  return definitions;
}

Json combat_fixture_with_stun_projectile() {
  auto definitions = combat_fixture_with_stunnable_npc_projectile();
  definitions["effects"]["5"] = definitions["effects"]["4"];
  definitions["effects"]["5"]["kind"] = "spawnProjectile";
  definitions["effects"]["5"]["parameters"] = {{"projectileId", 4}};
  definitions["projectiles"]["4"] = {
      {"duration_ms", 2000}, {"parameters", {{"speed", 1000}}}, {"hit_effects", {4}}};
  definitions["skills"][0]["layers"][0]["stages"][0]["events"][0]["effect_id"] = 5;
  return definitions;
}

TEST(WorldMobs, CombatTimelineIsSharedOnlyWithinTheSamePopulation) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto first_world = CombatSession::create_world_mobs(world_mob_map(), data);
  const auto second_world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession second(data, nullptr, other, player_stats());

  EXPECT_EQ(first.world_combat_timeline(), nullptr);
  first.attach_world_mobs(first_world);
  second.attach_world_mobs(first_world);
  const auto first_timeline = first.world_combat_timeline();
  ASSERT_NE(first_timeline, nullptr);
  EXPECT_EQ(first_timeline, second.world_combat_timeline());
  second.attach_world_mobs(second_world);
  EXPECT_NE(first_timeline, second.world_combat_timeline());
  first.detach_world_mobs(kStart);
  EXPECT_EQ(first.world_combat_timeline(), nullptr);
  EXPECT_NE(first_timeline, nullptr);
}

TEST(WorldMobs, MilestoneAdvanceRejectsAStaleClaimAndStopsAtItsScheduledTick) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession player(data, nullptr, character(), player_stats());
  const auto lifetime = std::make_shared<SyntheticMilestoneOwner>(player);
  player.attach_world_mobs(world);
  const auto owner = player.bind_world_milestone_owner(1, lifetime);
  ASSERT_TRUE(owner);
  const WorldPosition position{9012, 0, 0, 0};
  player.advance_private_encounter(kStart, position);

  const auto due = player.next_action_tick();
  ASSERT_TRUE(due);
  const auto timeline = player.world_combat_timeline();
  ASSERT_TRUE(timeline);
  const auto entry = timeline->enqueue(*owner, *due, combat::CombatTimeline::Kind::milestone);
  ASSERT_TRUE(entry.token);
  ASSERT_EQ(timeline->try_claim(*entry.token, *due).status,
            combat::CombatTimeline::ClaimStatus::ready);
  auto stale = *entry.token;
  ++stale.tick;
  EXPECT_FALSE(player.advance_next_world_milestone(stale, *due + 10000, position));
  EXPECT_EQ(player.next_action_tick(), due);
  EXPECT_FALSE(player.advance_next_world_milestone(*entry.token, *due - 1, position));

  const auto advanced = player.advance_next_world_milestone(*entry.token, *due + 10000, position);
  ASSERT_TRUE(advanced);
  EXPECT_EQ(advanced->tick, *due);
  EXPECT_EQ(advanced->next_tick, player.next_action_tick());
  EXPECT_FALSE(player.advance_next_world_milestone(*entry.token, *due + 10000, position));
  const auto retained = player.retained_world_milestone(*entry.token);
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->state, CombatSession::WorldMilestoneOutcomeState::ready);
  EXPECT_FALSE(player.acknowledge_retained_world_milestone(*entry.token, retained));
  EXPECT_EQ(timeline->complete(*entry.token).status,
            combat::CombatTimeline::EnqueueStatus::invalid);
  ASSERT_TRUE(timeline->mark_milestone_staged(*entry.token));
  EXPECT_EQ(timeline->complete(*entry.token).status,
            combat::CombatTimeline::EnqueueStatus::accepted);
  EXPECT_TRUE(player.acknowledge_retained_world_milestone(*entry.token, retained));
  EXPECT_FALSE(player.retained_world_milestone(*entry.token));
}

TEST(WorldMobs, MilestonePacketsRemainOwnedWhenSuccessorCalculationFails) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession player(data, nullptr, character(), player_stats());
  const auto lifetime = std::make_shared<SyntheticMilestoneOwner>(player);
  player.attach_world_mobs(world);
  const auto owner = player.bind_world_milestone_owner(1, lifetime);
  ASSERT_TRUE(owner);
  const WorldPosition position{9012, 0, 0, 0};
  player.advance_private_encounter(kStart, position);
  const auto due = player.next_action_tick();
  ASSERT_TRUE(due);
  const auto timeline = player.world_combat_timeline();
  const auto queued = timeline->enqueue(*owner, *due, combat::CombatTimeline::Kind::milestone);
  ASSERT_TRUE(queued.token);
  ASSERT_EQ(timeline->try_claim(*queued.token, *due).status,
            combat::CombatTimeline::ClaimStatus::ready);
  player.set_milestone_retention_hook_for_test([] {
    throw std::bad_alloc();
  });

  EXPECT_THROW(player.advance_next_world_milestone(*queued.token, *due, position), std::bad_alloc);
  const auto retained = player.retained_world_milestone(*queued.token);
  ASSERT_TRUE(retained);
  EXPECT_EQ(retained->state, CombatSession::WorldMilestoneOutcomeState::packets_retained);
  EXPECT_EQ(retained->result.tick, *due);
  const auto after_execution = player.monster_state_fingerprint_for_test();
  EXPECT_FALSE(player.advance_next_world_milestone(*queued.token, *due, position));
  player.set_milestone_retention_hook_for_test({});
  ASSERT_TRUE(player.finish_retained_world_milestone(*queued.token));
  EXPECT_EQ(player.monster_state_fingerprint_for_test(), after_execution);
  EXPECT_EQ(retained->state, CombatSession::WorldMilestoneOutcomeState::ready);
  EXPECT_FALSE(player.finish_retained_world_milestone(*queued.token));
  ASSERT_TRUE(timeline->mark_milestone_staged(*queued.token));
  ASSERT_EQ(timeline->complete(*queued.token).status, combat::CombatTimeline::EnqueueStatus::accepted);
  EXPECT_TRUE(player.acknowledge_retained_world_milestone(*queued.token, retained));
}

TEST(WorldMobs, ClaimedMilestoneRejectsAnotherSessionsOwnerAndBlocksPopulationReplacement) {
  using Timeline = combat::CombatTimeline;
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  const auto replacement = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other_character = character();
  other_character.id = 23;
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession second(data, nullptr, other_character, player_stats());
  const auto first_lifetime = std::make_shared<SyntheticMilestoneOwner>(first);
  const auto second_lifetime = std::make_shared<SyntheticMilestoneOwner>(second);
  first.attach_world_mobs(world);
  second.attach_world_mobs(world);
  EXPECT_FALSE(first.bind_world_milestone_owner(1, second_lifetime));
  const auto first_owner = first.bind_world_milestone_owner(1, first_lifetime);
  ASSERT_TRUE(first_owner);
  EXPECT_FALSE(second.bind_world_milestone_owner(1, second_lifetime));
  ASSERT_TRUE(second.bind_world_milestone_owner(2, second_lifetime));
  const WorldPosition position{9012, 0, 0, 0};
  first.advance_private_encounter(kStart, position);
  second.advance_private_encounter(kStart, position);
  const auto due = first.next_action_tick();
  ASSERT_TRUE(due);
  const auto timeline = first.world_combat_timeline();
  const auto queued = timeline->enqueue(*first_owner, *due, Timeline::Kind::milestone);
  ASSERT_TRUE(queued.token);
  EXPECT_THROW(first.detach_world_mobs(), std::logic_error);
  EXPECT_THROW(first.attach_world_mobs(world), std::logic_error);
  EXPECT_THROW(first.attach_world_mobs(replacement), std::logic_error);
  EXPECT_EQ(first.world_combat_timeline(), timeline);
  ASSERT_EQ(timeline->try_claim(*queued.token, *due).status, Timeline::ClaimStatus::ready);
  const auto second_before = second.monster_state_fingerprint_for_test();
  EXPECT_FALSE(second.advance_next_world_milestone(*queued.token, *due, position));
  EXPECT_EQ(second.monster_state_fingerprint_for_test(), second_before);
  ASSERT_TRUE(first.advance_next_world_milestone(*queued.token, *due, position));
  const auto retained = first.retained_world_milestone(*queued.token);
  ASSERT_TRUE(retained);
  EXPECT_THROW(first.detach_world_mobs(), std::logic_error);
  ASSERT_TRUE(timeline->mark_milestone_staged(*queued.token));
  ASSERT_EQ(timeline->complete(*queued.token).status, Timeline::EnqueueStatus::accepted);
  ASSERT_TRUE(first.acknowledge_retained_world_milestone(*queued.token, retained));
  EXPECT_NO_THROW(first.detach_world_mobs());
  EXPECT_NO_THROW(first.attach_world_mobs(replacement));
}

TEST(WorldMobs, RetainedMilestoneKeepsItsEnclosingOwnerAliveUntilAcknowledged) {
  using Timeline = combat::CombatTimeline;
  struct Owner final : CombatMilestoneOwner {
    CombatSession player;
    explicit Owner(std::shared_ptr<const CombatData> data)
        : player(std::move(data), nullptr, character(), player_stats()) {
    }

    const CombatSession *owned_combat_session() const noexcept override {
      return &player;
    }
  };
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto owner = std::make_shared<Owner>(data);
  std::weak_ptr<Owner> weak_owner = owner;
  owner->player.attach_world_mobs(world);
  const auto binding = owner->player.bind_world_milestone_owner(1, owner);
  ASSERT_TRUE(binding);
  const WorldPosition position{9012, 0, 0, 0};
  owner->player.advance_private_encounter(kStart, position);
  const auto due = owner->player.next_action_tick();
  ASSERT_TRUE(due);
  const auto timeline = owner->player.world_combat_timeline();
  const auto queued = timeline->enqueue(*binding, *due, Timeline::Kind::milestone);
  ASSERT_TRUE(queued.token);
  ASSERT_EQ(timeline->try_claim(*queued.token, *due).status, Timeline::ClaimStatus::ready);
  ASSERT_TRUE(owner->player.advance_next_world_milestone(*queued.token, *due, position));
  auto retained = owner->player.retained_world_milestone(*queued.token);
  ASSERT_TRUE(retained);
  owner.reset();
  ASSERT_FALSE(weak_owner.expired());
  auto live = weak_owner.lock();
  ASSERT_TRUE(live);
  EXPECT_THROW(live->player.detach_world_mobs(), std::logic_error);
  ASSERT_TRUE(timeline->mark_milestone_staged(*queued.token));
  ASSERT_EQ(timeline->complete(*queued.token).status, Timeline::EnqueueStatus::accepted);
  ASSERT_TRUE(live->player.acknowledge_retained_world_milestone(*queued.token, retained));
  EXPECT_NO_THROW(live->player.detach_world_mobs());
  retained.reset();
  live.reset();
  EXPECT_TRUE(weak_owner.expired());
}

TEST(WorldMobs, QueuedOrClaimedMilestoneKeepsSupervisedOwnerUntilDetach) {
  using Timeline = combat::CombatTimeline;
  struct Owner final : CombatMilestoneOwner {
    CombatSession player;
    explicit Owner(std::shared_ptr<const CombatData> data)
        : player(std::move(data), nullptr, character(), player_stats()) {
    }

    const CombatSession *owned_combat_session() const noexcept override {
      return &player;
    }
  };
  for (const bool claim_before_detach : {false, true}) {
    SCOPED_TRACE(claim_before_detach);
    const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
    const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
    auto owner = std::make_shared<Owner>(data);
    std::weak_ptr<Owner> weak_owner = owner;
    std::map<int, std::shared_ptr<Owner>> supervisor;
    supervisor.emplace(1, owner);
    owner->player.attach_world_mobs(world);
    const auto binding = owner->player.bind_world_milestone_owner(1, owner);
    ASSERT_TRUE(binding);
    const auto timeline = owner->player.world_combat_timeline();
    const auto queued = timeline->enqueue(*binding, kStart, Timeline::Kind::milestone);
    ASSERT_TRUE(queued.token);
    if (claim_before_detach) {
      ASSERT_EQ(timeline->try_claim(*queued.token, kStart).status, Timeline::ClaimStatus::ready);
    }

    owner.reset();
    auto retained = supervisor.at(1);
    ASSERT_FALSE(weak_owner.expired());
    EXPECT_THROW(retained->player.detach_world_mobs(), std::logic_error);
    EXPECT_EQ(timeline->enqueue(*binding, kStart + 1, Timeline::Kind::input).status,
              Timeline::EnqueueStatus::invalid);
    if (!claim_before_detach) {
      ASSERT_EQ(timeline->try_claim(*queued.token, kStart).status, Timeline::ClaimStatus::ready);
    }
    ASSERT_EQ(timeline->replace_unstarted_milestone(*queued.token).status,
              Timeline::EnqueueStatus::accepted);
    EXPECT_NO_THROW(retained->player.detach_world_mobs());
    EXPECT_EQ(timeline->enqueue(*binding, kStart + 1, Timeline::Kind::input).status,
              Timeline::EnqueueStatus::invalid);
    supervisor.clear();
    retained.reset();
    EXPECT_TRUE(weak_owner.expired());
  }
}

TEST(WorldMobs, FailedMilestoneExecutionRemainsUncertainAndCannotReplay) {
  using Timeline = combat::CombatTimeline;
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession player(data, nullptr, character(), player_stats());
  const auto lifetime = std::make_shared<SyntheticMilestoneOwner>(player);
  player.attach_world_mobs(world);
  const auto owner = player.bind_world_milestone_owner(1, lifetime);
  ASSERT_TRUE(owner);
  const WorldPosition position{9012, 0, 0, 0};
  player.advance_private_encounter(kStart, position);
  const auto timeline = player.world_combat_timeline();
  const auto first_tick = player.next_action_tick();
  ASSERT_TRUE(first_tick);
  auto queued = timeline->enqueue(*owner, *first_tick, Timeline::Kind::milestone);
  ASSERT_TRUE(queued.token);
  player.set_world_journal_hook_for_test([] {
    throw std::runtime_error("Injected milestone journal failure");
  });
  struct ClearJournalHook {
    CombatSession &session;
    ~ClearJournalHook() {
      session.set_world_journal_hook_for_test({});
    }
  } clear_hook{player};

  bool failed = false;
  for (int iteration = 0; iteration < 50 && queued.token; ++iteration) {
    const auto token = *queued.token;
    ASSERT_EQ(timeline->try_claim(token, token.tick).status, Timeline::ClaimStatus::ready);
    try {
      const auto step = player.advance_next_world_milestone(token, token.tick, position);
      ASSERT_TRUE(step);
      const auto retained = player.retained_world_milestone(token);
      ASSERT_TRUE(retained);
      ASSERT_TRUE(timeline->mark_milestone_staged(token));
      const auto successor = step->next_tick
          ? std::optional<Timeline::Successor>{
                Timeline::Successor{*step->next_tick, Timeline::Kind::milestone}}
          : std::nullopt;
      queued = timeline->complete(token, successor);
      ASSERT_EQ(queued.status, Timeline::EnqueueStatus::accepted);
      ASSERT_TRUE(player.acknowledge_retained_world_milestone(token, retained));
    } catch (const std::runtime_error &) {
      const auto retained = player.retained_world_milestone(token);
      ASSERT_TRUE(retained);
      EXPECT_EQ(retained->state, CombatSession::WorldMilestoneOutcomeState::uncertain);
      EXPECT_TRUE(timeline->is_claimed(token));
      EXPECT_FALSE(player.advance_next_world_milestone(token, token.tick, position));
      EXPECT_FALSE(player.finish_retained_world_milestone(token));
      EXPECT_FALSE(player.acknowledge_retained_world_milestone(token, retained));
      EXPECT_EQ(timeline->complete(token).status, Timeline::EnqueueStatus::invalid);
      failed = true;
      // Disposal only: this fixture has no persistence or external recipients. Never turn
      // an uncertain production result into a staged result without real reconciliation.
      ASSERT_TRUE(timeline->mark_milestone_staged(token));
      ASSERT_EQ(timeline->complete(token).status, Timeline::EnqueueStatus::accepted);
      CombatSessionTestAccess::discard_retained_milestone(player);
      break;
    }
  }
  EXPECT_TRUE(failed);
}

TEST(WorldMobs, MonsterStatusInstancesAreUniqueAcrossPlayersAndRefreshes) {
  auto definitions = combat_fixture_with_stunnable_npc_projectile();
  definitions["status_effects"]["19"] = {{"duration_ms", 2000}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession first(data, nullptr, character(), player_stats());
  CombatSession second(data, nullptr, other, player_stats());
  first.attach_world_mobs(world);
  second.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};
  const auto entry = first.advance_private_encounter(kStart, position);
  const auto *spawn = find_packet(entry, 10);
  ASSERT_NE(spawn, nullptr);
  const auto unit_id = std::stoull(spawn->message.at("unit_id").get<std::string>());
  second.advance_private_encounter(kStart, position);

  const auto first_add = first.gm_add_status_effect(unit_id, 18, kStart + 100 * kMillisecond);
  const auto second_add = second.gm_add_status_effect(unit_id, 19, kStart + 200 * kMillisecond);
  const auto *first_status = find_packet(first_add, 49);
  const auto *second_status = find_packet(second_add, 49);
  ASSERT_NE(first_status, nullptr);
  ASSERT_NE(second_status, nullptr);
  const auto first_instance = first_status->message.at("status_effect").at("instance_id");
  const auto second_instance = second_status->message.at("status_effect").at("instance_id");
  EXPECT_NE(first_instance, second_instance);

  const auto refreshed = second.gm_add_status_effect(unit_id, 18, kStart + 300 * kMillisecond);
  const auto *update = find_packet(refreshed, 50);
  ASSERT_NE(update, nullptr);
  EXPECT_EQ(update->message.at("status_effects").at(0).at("instance_id"), first_instance);

  const auto removed = first.gm_remove_status_effect(unit_id, 19, kStart + 400 * kMillisecond);
  const auto *removal = find_packet(removed, 51);
  ASSERT_NE(removal, nullptr);
  EXPECT_EQ(removal->message.at("status_effects").at(0).at("instance_id"), second_instance);

  const auto expired = second.advance_private_encounter(kStart + 2400 * kMillisecond, position);
  const auto *expiry = find_packet(expired, 51);
  ASSERT_NE(expiry, nullptr);
  EXPECT_EQ(expiry->message.at("status_effects").at(0).at("instance_id"), first_instance);
}

TEST(WorldMobs, MonsterStatusInstanceExhaustionNeverWrapsOrReusesAnId) {
  auto definitions = combat_fixture_with_stunnable_npc_projectile();
  definitions["status_effects"]["19"] = {{"duration_ms", 2000}};
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession player(data, nullptr, character(), player_stats());
  player.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};
  const auto entry = player.advance_private_encounter(kStart, position);
  const auto *spawn = find_packet(entry, 10);
  ASSERT_NE(spawn, nullptr);
  const auto unit_id = std::stoull(spawn->message.at("unit_id").get<std::string>());
  constexpr auto last_instance = (std::numeric_limits<std::uint32_t>::max)();
  ASSERT_TRUE(player.set_monster_status_sequence_for_test(unit_id, last_instance));

  const auto added = player.gm_add_status_effect(unit_id, 18, kStart + 100 * kMillisecond);
  const auto *status = find_packet(added, 49);
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(status->message.at("status_effect").at("instance_id"), last_instance);
  EXPECT_THROW(player.gm_add_status_effect(unit_id, 19, kStart + 200 * kMillisecond),
               std::overflow_error);
  EXPECT_FALSE(player.set_monster_status_sequence_for_test(unit_id, 1));
}

TEST(WorldMobs, EarlierStunFlightPrecedesAnNpcFlightInTheSameLatePoll) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession player(data, nullptr, character(), player_stats());
  player.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};

  ASSERT_NE(find_packet(player.advance_private_encounter(kStart, position), 10), nullptr);
  player.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  ASSERT_NE(find_packet(player.advance_private_encounter(kStart + 1300 * kMillisecond, position),
                        40), nullptr);
  ASSERT_TRUE(accepted_start(player.receive(20010, start_request(), position,
                                             kStart + 1350 * kMillisecond)));
  ASSERT_NE(find_packet(player.advance_private_encounter(kStart + 1650 * kMillisecond, position),
                        40), nullptr);

  const auto late = player.advance_private_encounter(kStart + 2400 * kMillisecond, position);
  EXPECT_NE(find_packet(late, 49), nullptr);
  EXPECT_TRUE(std::ranges::none_of(late, [](const GameplayPacket &packet) {
    return packet.opcode == 19 && packet.message.value("target_unit_id", std::string{}) == "7";
  }));
  EXPECT_EQ(player.player_stats().at("69"), 100);
}

TEST(WorldMobs, EqualDeadlineFlightsKeepTheirExistingLaunchOrder) {
  auto definitions = combat_fixture_with_stun_projectile();
  // The player flight covers 1000 mm in 650 ms, matching the NPC's 2300 ms impact.
  definitions["projectiles"]["4"]["parameters"]["speed"] = 153.85;
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession player(data, nullptr, character(), player_stats());
  player.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};

  ASSERT_NE(find_packet(player.advance_private_encounter(kStart, position), 10), nullptr);
  player.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  ASSERT_NE(find_packet(player.advance_private_encounter(kStart + 1300 * kMillisecond, position),
                        40), nullptr);
  ASSERT_TRUE(accepted_start(player.receive(20010, start_request(), position,
                                             kStart + 1350 * kMillisecond)));
  ASSERT_NE(find_packet(player.advance_private_encounter(kStart + 1650 * kMillisecond, position),
                        40), nullptr);

  const auto late = player.advance_private_encounter(kStart + 2400 * kMillisecond, position);
  EXPECT_NE(find_packet(late, 49), nullptr);
  EXPECT_TRUE(std::ranges::any_of(late, [](const GameplayPacket &packet) {
    return packet.opcode == 19 && packet.message.value("target_unit_id", std::string{}) == "7";
  }));
  EXPECT_LT(player.player_stats().at("69").get<int>(), 100);
}

TEST(WorldMobs, NativeScheduleOrdersUnlaunchedStunBeforeNpcImpactOnLateReversePoll) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession target(data, nullptr, character(), player_stats());
  CombatSession attacker(data, nullptr, other, player_stats());
  target.attach_world_mobs(world, false, kStart);
  attacker.attach_world_mobs(world, false, kStart);
  const WorldPosition position{9012, 0, 0, 0};
  const auto lifetime = std::make_shared<int>(1);
  CombatWorkSchedule target_schedule(target.world_combat_timeline(), 1, lifetime);
  CombatWorkSchedule attacker_schedule(attacker.world_combat_timeline(), 2, lifetime);
  target_schedule.initialize(target.next_action_tick());
  attacker_schedule.initialize(attacker.next_action_tick());
  std::vector<GameplayPacket> target_packets;
  std::vector<GameplayPacket> attacker_packets;

  const auto poll = [&](CombatWorkSchedule &schedule, CombatSession &session,
                        std::vector<GameplayPacket> &packets, std::int64_t now) {
    if (schedule.claim(now).status != combat::CombatTimeline::ClaimStatus::ready) {
      return false;
    }
    const auto work = *schedule.active();
    auto output = session.advance_private_encounter(work.token.tick, position);
    packets.insert(packets.end(), output.begin(), output.end());
    schedule.complete(session.next_action_tick());
    return true;
  };
  const auto drain = [&](std::int64_t now) {
    for (int attempt = 0; attempt < 256; ++attempt) {
      // Always try the NPC's target first, including after both impact deadlines.
      const bool target_ran = poll(target_schedule, target, target_packets, now);
      const bool attacker_ran = poll(attacker_schedule, attacker, attacker_packets, now);
      if (!target_ran && !attacker_ran) {
        return;
      }
    }
    FAIL() << "Population scheduler did not reach its future deadline";
  };

  drain(kStart + 1300 * kMillisecond);
  ASSERT_NE(find_packet(target_packets, 40), nullptr);
  const auto input_tick = kStart + 1350 * kMillisecond;
  attacker_schedule.submit_input(input_tick);
  ASSERT_EQ(attacker_schedule.claim(input_tick).status, combat::CombatTimeline::ClaimStatus::ready);
  ASSERT_TRUE(accepted_start(attacker.receive(20010, start_request(), position, input_tick)));
  attacker_schedule.complete(attacker.next_action_tick());

  target_packets.clear();
  attacker_packets.clear();
  // No 1650 ms poll launches the stun in advance. Its action event and subsequent
  // 1750 ms impact must both precede the NPC impact at 2300 ms.
  drain(kStart + 2400 * kMillisecond);
  ASSERT_NE(find_packet(attacker_packets, 40), nullptr);
  ASSERT_NE(find_packet(attacker_packets, 49), nullptr);
  EXPECT_FALSE(std::ranges::any_of(target_packets, [&](const GameplayPacket &packet) {
    return packet.opcode == 19 &&
           packet.message.value("target_unit_id", std::string{}) == std::to_string(character().id);
  }));
  EXPECT_EQ(target.player_stats().at("69"), 100);

  const auto close_tick = kStart + 2401 * kMillisecond;
  target_schedule.submit_close(close_tick);
  attacker_schedule.submit_close(close_tick);
  ASSERT_EQ(target_schedule.claim(close_tick).status, combat::CombatTimeline::ClaimStatus::ready);
  target.detach_world_mobs(close_tick);
  target_schedule.complete(std::nullopt);
  target_schedule.retire();
  ASSERT_EQ(attacker_schedule.claim(close_tick).status, combat::CombatTimeline::ClaimStatus::ready);
  attacker.detach_world_mobs(close_tick);
  attacker_schedule.complete(std::nullopt);
  attacker_schedule.retire();
}

TEST(WorldMobs, ProjectileWorkInspectionKeepsDistinctLivesAndDeadlines) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession target(data, nullptr, character(), player_stats());
  CombatSession attacker(data, nullptr, other, player_stats());
  target.attach_world_mobs(world);
  attacker.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};

  ASSERT_NE(find_packet(target.advance_private_encounter(kStart, position), 10), nullptr);
  attacker.advance_private_encounter(kStart, position);
  EXPECT_FALSE(target.earliest_projectile_work());
  target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  ASSERT_NE(find_packet(target.advance_private_encounter(kStart + 1300 * kMillisecond, position),
                        40), nullptr);
  const auto npc_flight = target.earliest_projectile_work();
  ASSERT_TRUE(npc_flight);
  EXPECT_EQ(npc_flight->deadline, kStart + 2300 * kMillisecond);

  ASSERT_TRUE(accepted_start(attacker.receive(20010, start_request(), position,
                                               kStart + 1350 * kMillisecond)));
  ASSERT_NE(find_packet(attacker.advance_private_encounter(kStart + 1650 * kMillisecond,
                                                            position), 40), nullptr);
  const auto stun_flight = attacker.earliest_projectile_work();
  ASSERT_TRUE(stun_flight);
  EXPECT_EQ(stun_flight->deadline, kStart + 1750 * kMillisecond);
  EXPECT_EQ(target.earliest_projectile_work(), npc_flight);
  EXPECT_EQ(attacker.earliest_projectile_work(), stun_flight);
  EXPECT_EQ(target.player_stats().at("69"), 100);
}

TEST(WorldMobs, SimultaneousProjectileWorkInspectionPreservesLaunchOrder) {
  auto definitions = combat_fixture_with_stun_projectile();
  definitions["projectiles"]["4"]["parameters"]["speed"] = 153.85;
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession player(data, nullptr, character(), player_stats());
  player.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};

  ASSERT_NE(find_packet(player.advance_private_encounter(kStart, position), 10), nullptr);
  player.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  ASSERT_NE(find_packet(player.advance_private_encounter(kStart + 1300 * kMillisecond, position),
                        40), nullptr);
  const auto first = player.earliest_projectile_work();
  ASSERT_TRUE(first);
  ASSERT_TRUE(accepted_start(player.receive(20010, start_request(), position,
                                             kStart + 1350 * kMillisecond)));
  ASSERT_NE(find_packet(player.advance_private_encounter(kStart + 1650 * kMillisecond, position),
                        40), nullptr);
  EXPECT_EQ(player.earliest_projectile_work(), first);
  EXPECT_EQ(first->deadline, kStart + 2300 * kMillisecond);

  player.advance_private_encounter(kStart + 2400 * kMillisecond, position);
  ASSERT_TRUE(accepted_start(player.receive(20010, start_request(2), position,
                                             kStart + 3000 * kMillisecond)));
  ASSERT_NE(find_packet(player.advance_private_encounter(kStart + 3300 * kMillisecond, position),
                        40), nullptr);
  const auto later = player.earliest_projectile_work();
  ASSERT_TRUE(later);
  EXPECT_GT(later->life_id, first->life_id);
}

TEST(WorldMobs, ProjectileFailureAfterDamageCanRetryWithoutPartialMutation) {
  auto definitions = combat_fixture_with_stun_projectile();
  definitions["status_effects"]["18"]["duration_ms"] = 1100;
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession target(data, nullptr, character(), player_stats());
  target.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};

  ASSERT_NE(find_packet(target.advance_private_encounter(kStart, position), 10), nullptr);
  target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  ASSERT_NE(find_packet(target.advance_private_encounter(kStart + 1300 * kMillisecond, position),
                        40), nullptr);
  ASSERT_NE(find_packet(target.gm_add_status_effect(7, 18, kStart + 1350 * kMillisecond), 49),
            nullptr);

  {
    ScopedDamageFaultLogger inject_failure;
    const auto deferred =
        target.advance_private_encounter(kStart + 2400 * kMillisecond, position);
    EXPECT_EQ(find_packet(deferred, 19), nullptr);
  }

  // A failed result construction must leave both HP and the pending flight unchanged.
  EXPECT_EQ(target.player_stats().at("69"), 100);
  const auto pending = target.earliest_projectile_work();
  ASSERT_TRUE(pending);
  EXPECT_EQ(pending->deadline, kStart + 2300 * kMillisecond);
  ASSERT_TRUE(target.next_action_tick());
  EXPECT_LE(*target.next_action_tick(), kStart + 2450 * kMillisecond);
  const auto unrelated = target.advance_private_encounter(kStart + 2450 * kMillisecond, position);
  EXPECT_NE(find_packet(unrelated, 51), nullptr);
  EXPECT_EQ(find_packet(unrelated, 19), nullptr);
  EXPECT_EQ(target.earliest_projectile_work(), pending);
  const auto retry = target.advance_private_encounter(kStart + 2500 * kMillisecond, position);
  EXPECT_NE(find_packet(retry, 19), nullptr);
  EXPECT_EQ(target.player_stats().at("69"), 90);
}

TEST(WorldMobs, ProjectilePreflightFailureRetainsFlightAndAllowsStatusExpiry) {
  auto definitions = combat_fixture_with_stun_projectile();
  definitions["status_effects"]["18"]["duration_ms"] = 1100;
  const auto data = std::make_shared<CombatData>(definitions);
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  CombatSession target(data, nullptr, character(), player_stats());
  target.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};

  target.advance_private_encounter(kStart, position);
  target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  ASSERT_NE(find_packet(target.advance_private_encounter(kStart + 1300 * kMillisecond, position),
                        40), nullptr);
  ASSERT_NE(find_packet(target.gm_add_status_effect(7, 18, kStart + 1350 * kMillisecond), 49),
            nullptr);
  const auto pending = target.earliest_projectile_work();
  ASSERT_TRUE(pending);
  target.set_projectile_preflight_hook_for_test([] {
    throw std::runtime_error("Injected projectile classification failure");
  });
  const auto deferred = target.advance_private_encounter(kStart + 2400 * kMillisecond, position);
  EXPECT_EQ(find_packet(deferred, 19), nullptr);
  EXPECT_EQ(target.earliest_projectile_work(), pending);
  target.set_projectile_preflight_hook_for_test({});

  const auto unrelated = target.advance_private_encounter(kStart + 2450 * kMillisecond, position);
  EXPECT_NE(find_packet(unrelated, 51), nullptr);
  EXPECT_EQ(find_packet(unrelated, 19), nullptr);
  EXPECT_EQ(target.earliest_projectile_work(), pending);
  const auto retry = target.advance_private_encounter(kStart + 2500 * kMillisecond, position);
  EXPECT_NE(find_packet(retry, 19), nullptr);
  EXPECT_EQ(target.player_stats().at("69"), 90);
}

TEST(WorldMobs, FailedSecondSimpleProjectileRetainsOnlyTheUnappliedFlight) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  auto map = world_mob_map();
  auto second = map["spawns"][0];
  second["unit_id"] = "576460752303423522";
  second["spawner_guid"] = {"3", "4"};
  map["spawns"].push_back(std::move(second));
  const auto world = CombatSession::create_world_mobs(map, data);
  CombatSession target(data, nullptr, character(), player_stats());
  target.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};

  target.advance_private_encounter(kStart, position);
  target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  const auto launches =
      target.advance_private_encounter(kStart + 1300 * kMillisecond, position);
  ASSERT_EQ(std::ranges::count_if(launches, [](const GameplayPacket &packet) {
    return packet.opcode == 40;
  }), 2);
  std::vector<std::string> launch_order;
  for (const auto &packet : launches) {
    if (packet.opcode == 40) {
      launch_order.push_back(packet.message.at("caster_unit_id").get<std::string>());
    }
  }
  ASSERT_NE(launch_order[0], launch_order[1]);

  {
    ScopedDamageFaultLogger inject_failure(2);
    const auto partial =
        target.advance_private_encounter(kStart + 2400 * kMillisecond, position);
    EXPECT_EQ(std::ranges::count_if(partial, [](const GameplayPacket &packet) {
      return packet.opcode == 19;
    }), 1);
    ASSERT_NE(find_packet(partial, 19), nullptr);
    EXPECT_EQ(find_packet(partial, 19)->message.at("caster_unit_id"), launch_order[0]);
  }
  EXPECT_EQ(target.player_stats().at("69"), 90);
  const auto retry = target.advance_private_encounter(kStart + 2500 * kMillisecond, position);
  EXPECT_EQ(std::ranges::count_if(retry, [](const GameplayPacket &packet) {
    return packet.opcode == 19;
  }), 1);
  ASSERT_NE(find_packet(retry, 19), nullptr);
  EXPECT_EQ(find_packet(retry, 19)->message.at("caster_unit_id"), launch_order[1]);
  EXPECT_EQ(target.player_stats().at("69"), 80);
}

TEST(WorldMobs, SimpleNpcProjectileBatchHandlesTheBoundedMaximum) {
  constexpr int kFlightCount = 256;
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  auto map = world_mob_map();
  const auto prototype = map["spawns"][0];
  for (int index = 1; index < kFlightCount; ++index) {
    auto spawn = prototype;
    spawn["unit_id"] = std::to_string(576460752303423506ULL + 16ULL * index);
    spawn["spawner_guid"] = {std::to_string(index + 10), std::to_string(index + 1000)};
    map["spawns"].push_back(std::move(spawn));
  }
  const auto world = CombatSession::create_world_mobs(map, data);
  auto stats = player_stats();
  stats["18"] = 1000000;
  stats["69"] = 1000000;
  CombatSession target(data, nullptr, character(), stats);
  target.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};
  target.advance_private_encounter(kStart, position);
  target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  const auto launches =
      target.advance_private_encounter(kStart + 1300 * kMillisecond, position);
  ASSERT_EQ(std::ranges::count_if(launches, [](const GameplayPacket &packet) {
    return packet.opcode == 40;
  }), kFlightCount);

  const auto started = std::chrono::steady_clock::now();
  const auto impacts = target.advance_private_encounter(kStart + 2400 * kMillisecond, position);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  RecordProperty("bounded_batch_elapsed_ms", std::to_string(elapsed.count()));
  EXPECT_EQ(std::ranges::count_if(impacts, [](const GameplayPacket &packet) {
    return packet.opcode == 19;
  }), kFlightCount);
  EXPECT_EQ(target.player_stats().at("69"), 1000000 - 10 * kFlightCount);
  EXPECT_FALSE(target.earliest_projectile_work());
}

TEST(WorldMobs, ComplexLethalAndShieldedProjectileHitsDoNotUseSimpleRollback) {
  for (const int scenario : {0, 1, 2}) {
    SCOPED_TRACE(scenario);
    auto definitions = combat_fixture_with_stun_projectile();
    auto stats = player_stats();
    if (scenario == 0) {
      definitions["effects"]["2"]["status_effect_id"] = 18;
    } else if (scenario == 1) {
      stats["18"] = 10;
      stats["69"] = 10;
    } else {
      stats["71"] = 5;
    }
    const auto data = std::make_shared<CombatData>(definitions);
    const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
    CombatSession target(data, nullptr, character(), stats);
    target.attach_world_mobs(world);
    const WorldPosition position{9012, 0, 0, 0};
    target.advance_private_encounter(kStart, position);
    target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
    ASSERT_NE(find_packet(target.advance_private_encounter(kStart + 1300 * kMillisecond,
                                                            position), 40), nullptr);

    ScopedDamageFaultLogger inject_failure;
    EXPECT_THROW(target.advance_private_encounter(kStart + 2400 * kMillisecond, position),
                 std::runtime_error);
  }
}

TEST(WorldMobs, TimelineMilestonesOrderTwoPlayersFlightsInASyntheticDriver) {
  using Timeline = combat::CombatTimeline;
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stun_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession target(data, nullptr, character(), player_stats());
  CombatSession attacker(data, nullptr, other, player_stats());
  const auto target_lifetime = std::make_shared<SyntheticMilestoneOwner>(target);
  const auto attacker_lifetime = std::make_shared<SyntheticMilestoneOwner>(attacker);
  target.attach_world_mobs(world);
  attacker.attach_world_mobs(world);
  const auto target_owner = target.bind_world_milestone_owner(1, target_lifetime);
  const auto attacker_owner = attacker.bind_world_milestone_owner(2, attacker_lifetime);
  ASSERT_TRUE(target_owner);
  ASSERT_TRUE(attacker_owner);
  const WorldPosition position{9012, 0, 0, 0};

  ASSERT_NE(find_packet(target.advance_private_encounter(kStart, position), 10), nullptr);
  attacker.advance_private_encounter(kStart, position);
  target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  ASSERT_NE(find_packet(target.advance_private_encounter(kStart + 1300 * kMillisecond, position),
                        40), nullptr);
  ASSERT_TRUE(accepted_start(attacker.receive(20010, start_request(), position,
                                               kStart + 1350 * kMillisecond)));
  target.advance_private_encounter(kStart + 1650 * kMillisecond, position);
  ASSERT_NE(find_packet(attacker.advance_private_encounter(kStart + 1650 * kMillisecond, position),
                        40), nullptr);

  const auto timeline = target.world_combat_timeline();
  ASSERT_EQ(timeline, attacker.world_combat_timeline());
  std::array<CombatSession *, 2> sessions{&target, &attacker};
  const std::array<Timeline::Owner, 2> owners{*target_owner, *attacker_owner};
  std::array<std::optional<Timeline::Token>, 2> tokens;
  for (std::size_t index = 0; index < sessions.size(); ++index) {
    const auto next = sessions[index]->next_action_tick();
    ASSERT_TRUE(next);
    tokens[index] = timeline->enqueue(owners[index], *next, Timeline::Kind::milestone).token;
    ASSERT_TRUE(tokens[index]);
  }

  bool saw_stun = false;
  const auto cutoff = kStart + 2400 * kMillisecond;
  for (int iteration = 0; iteration < 100; ++iteration) {
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
      if (tokens[index] && (!selected ||
          std::tie(tokens[index]->tick, tokens[index]->sequence) <
              std::tie(tokens[*selected]->tick, tokens[*selected]->sequence))) {
        selected = index;
      }
    }
    if (!selected || tokens[*selected]->tick > cutoff) {
      break;
    }
    const auto token = *tokens[*selected];
    ASSERT_EQ(timeline->try_claim(token, cutoff).status, Timeline::ClaimStatus::ready);
    const auto step = sessions[*selected]->advance_next_world_milestone(token, cutoff, position);
    ASSERT_TRUE(step) << "A shared event invalidated the previously scheduled owner milestone";
    const auto retained = sessions[*selected]->retained_world_milestone(token);
    ASSERT_TRUE(retained);
    saw_stun = saw_stun || find_packet(step->packets, 49) != nullptr;
    ASSERT_TRUE(timeline->mark_milestone_staged(token));
    const auto next = step->next_tick
        ? std::optional<Timeline::Successor>{
              Timeline::Successor{*step->next_tick, Timeline::Kind::milestone}}
        : std::nullopt;
    const auto completed = timeline->complete(token, next);
    ASSERT_EQ(completed.status, Timeline::EnqueueStatus::accepted);
    ASSERT_TRUE(sessions[*selected]->acknowledge_retained_world_milestone(token, retained));
    tokens[*selected] = completed.token;
    ASSERT_LT(iteration, 99) << "The synthetic scheduler did not make bounded progress";
  }
  EXPECT_TRUE(saw_stun);
  EXPECT_EQ(target.player_stats().at("69"), 100);
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (!tokens[index]) {
      continue;
    }
    const auto close = timeline->enqueue(owners[index], cutoff, Timeline::Kind::close);
    ASSERT_TRUE(close.token);
    ASSERT_EQ(timeline->try_claim(*close.token, cutoff).status, Timeline::ClaimStatus::ready);
    ASSERT_TRUE(timeline->cancel_milestone_after_close(*close.token, *tokens[index]));
    ASSERT_EQ(timeline->complete(*close.token).status, Timeline::EnqueueStatus::accepted);
  }
}

TEST(WorldMobs, AnotherPlayersStunCancelsTheTargetsInFlightMonsterProjectile) {
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stunnable_npc_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession target(data, nullptr, character(), player_stats());
  CombatSession attacker(data, nullptr, other, player_stats());
  target.attach_world_mobs(world);
  attacker.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};

  const auto entry = target.advance_private_encounter(kStart, position);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  attacker.advance_private_encounter(kStart, position);
  target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
  const auto launched = target.advance_private_encounter(kStart + 1300 * kMillisecond, position);
  const auto *spawn = find_packet(launched, 40);
  ASSERT_NE(spawn, nullptr);
  const auto projectile_id = spawn->message.at("spawn_projectile").at("instance_id");

  const auto started = attacker.receive(20010, start_request(), position,
                                        kStart + 1350 * kMillisecond);
  ASSERT_TRUE(accepted_start(started));
  const auto stunned = attacker.advance_private_encounter(kStart + 1650 * kMillisecond, position);
  ASSERT_NE(find_packet(stunned, 49), nullptr);

  const auto cancelled = target.advance_private_encounter(kStart + 1700 * kMillisecond, position);
  const auto *despawn = find_packet(cancelled, 44);
  ASSERT_NE(despawn, nullptr);
  EXPECT_EQ(despawn->message.at("despawn_projectile").at("instance_id"), projectile_id);
  EXPECT_EQ(despawn->message.at("despawn_projectile").at("reason_type"),
            "EX5SkillObjectDespawnReasonType_SkillStageCancel");
  EXPECT_TRUE(std::ranges::none_of(cancelled, [](const GameplayPacket &packet) {
    return packet.opcode == 19 && packet.message.value("target_unit_id", std::string{}) == "7";
  }));
  const auto after_deadline =
      target.advance_private_encounter(kStart + 2400 * kMillisecond, position);
  EXPECT_EQ(find_packet(after_deadline, 19), nullptr);
  EXPECT_EQ(target.player_stats().at("69"), 100);
  verify_encoding(cancelled);
}

TEST(WorldMobs, LateStunDoesNotCancelAProjectileWhoseHitDeadlineAlreadyPassed) {
  constexpr int kStunStatus = 18;
  for (const bool target_applies_stun : {false, true}) {
    SCOPED_TRACE(target_applies_stun ? "target interrupts" : "other player interrupts");
    const auto data = std::make_shared<CombatData>(combat_fixture_with_stunnable_npc_projectile());
    const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
    auto other = character();
    other.id = 23;
    CombatSession target(data, nullptr, character(), player_stats());
    CombatSession attacker(data, nullptr, other, player_stats());
    target.attach_world_mobs(world);
    attacker.attach_world_mobs(world);
    const WorldPosition position{9012, 0, 0, 0};

    const auto entry = target.advance_private_encounter(kStart, position);
    const auto *spawned_monster = find_packet(entry, 10);
    ASSERT_NE(spawned_monster, nullptr);
    const auto unit_id = std::stoull(spawned_monster->message.at("unit_id").get<std::string>());
    attacker.advance_private_encounter(kStart, position);
    target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
    const auto launched = target.advance_private_encounter(kStart + 1300 * kMillisecond, position);
    ASSERT_NE(find_packet(launched, 40), nullptr);

    // A later interruption must not erase a hit that was already due when a session polls late.
    auto &interrupter = target_applies_stun ? target : attacker;
    const auto stunned = interrupter.gm_add_status_effect(unit_id, kStunStatus,
                                                          kStart + 2400 * kMillisecond);
    ASSERT_NE(find_packet(stunned, 49), nullptr);
    const auto resolved = target.advance_private_encounter(kStart + 2500 * kMillisecond, position);
    EXPECT_NE(find_packet(resolved, 19), nullptr);
    EXPECT_LT(target.player_stats().at("69").get<int>(), 100);
  }
}

TEST(WorldMobs, RemovingOrReplacingTheCasterPreservesOnlyAlreadyDueHits) {
  for (const bool removal_before_hit : {false, true}) {
    for (const bool reuse_unit_id : {false, true}) {
      SCOPED_TRACE(removal_before_hit ? "removed before hit" : "removed after hit");
      SCOPED_TRACE(reuse_unit_id ? "unit ID reused" : "unit ID absent");
      auto definitions = combat_fixture_with_stunnable_npc_projectile();
      definitions["npc_locomotion"]["5001528"] = npc_ai_block();
      definitions["npc_stats"]["5001528"]["22"] = 900;
      const auto data = std::make_shared<CombatData>(definitions);
      const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
      auto other = character();
      other.id = 23;
      CombatSession target(data, nullptr, character(), player_stats());
      CombatSession remover(data, nullptr, other, player_stats());
      target.attach_world_mobs(world);
      remover.attach_world_mobs(world);
      const WorldPosition position{9012, 0, 0, 0};

      const auto entry = target.advance_private_encounter(kStart, position);
      const auto *spawned_monster = find_packet(entry, 10);
      ASSERT_NE(spawned_monster, nullptr);
      const auto unit_id = std::stoull(spawned_monster->message.at("unit_id").get<std::string>());
      remover.advance_private_encounter(kStart, position);
      target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
      const auto launched = target.advance_private_encounter(kStart + 1300 * kMillisecond, position);
      ASSERT_NE(find_packet(launched, 40), nullptr);

      const auto removal_tick = kStart + (removal_before_hit ? 1700 : 2400) * kMillisecond;
      remover.remove_gm_monster(unit_id, removal_tick);
      if (reuse_unit_id) {
        ASSERT_TRUE(remover.add_gm_monster(unit_id, 5001528, {9012, 1000, 0, 0, 180.0F}));
      }
      const auto resolved = target.advance_private_encounter(kStart + 2500 * kMillisecond,
                                                              position);
      if (removal_before_hit) {
        EXPECT_EQ(find_packet(resolved, 19), nullptr);
        EXPECT_EQ(target.player_stats().at("69"), 100);
        const auto *despawn = find_packet(resolved, 44);
        ASSERT_NE(despawn, nullptr);
        EXPECT_EQ(despawn->message.at("despawn_projectile").at("reason_type"),
                  "EX5SkillObjectDespawnReasonType_SkillStageCancel");
      } else {
        EXPECT_NE(find_packet(resolved, 19), nullptr);
        EXPECT_EQ(target.player_stats().at("69"), 90)
            << "A replacement unit must not supply the old projectile's attack stats";
      }
    }
  }
}

TEST(WorldMobs, RespawnDoesNotReuseAnEarlierLifesProjectileInterruption) {
  constexpr int kStunStatus = 18;
  const auto data = std::make_shared<CombatData>(combat_fixture_with_stunnable_npc_projectile());
  const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
  auto other = character();
  other.id = 23;
  CombatSession target(data, nullptr, character(), player_stats());
  CombatSession attacker(data, nullptr, other, player_stats());
  target.attach_world_mobs(world);
  attacker.attach_world_mobs(world);
  const WorldPosition position{9012, 0, 0, 0};

  const auto entry = target.advance_private_encounter(kStart, position);
  const auto *spawned_monster = find_packet(entry, 10);
  ASSERT_NE(spawned_monster, nullptr);
  const auto unit_id = std::stoull(spawned_monster->message.at("unit_id").get<std::string>());
  attacker.advance_private_encounter(kStart, position);
  const auto stunned = attacker.gm_add_status_effect(unit_id, kStunStatus,
                                                     kStart + 100 * kMillisecond);
  ASSERT_NE(find_packet(stunned, 49), nullptr);
  attacker.gm_damage_monster(unit_id, 1000000, kStart + 200 * kMillisecond);

  const auto respawned = target.advance_private_encounter(kStart + 2200 * kMillisecond, position);
  ASSERT_NE(find_packet(respawned, 10), nullptr);
  target.advance_private_encounter(kStart + 3200 * kMillisecond, position);
  const auto launched = target.advance_private_encounter(kStart + 3500 * kMillisecond, position);
  ASSERT_NE(find_packet(launched, 40), nullptr);
  const auto resolved = target.advance_private_encounter(kStart + 4500 * kMillisecond, position);
  EXPECT_NE(find_packet(resolved, 19), nullptr);
  EXPECT_EQ(target.player_stats().at("69"), 90);
}

TEST(Combat, RemovingAPrivateGmCasterCancelsItsFlightAndReleasesItsActor) {
  auto definitions = combat_fixture_with_npc_projectile();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  constexpr std::uint64_t unit_id = 0x3000000000000012ULL;
  const WorldPosition player{9012, 0, 0, 0};
  ASSERT_TRUE(combat.add_gm_monster(unit_id, 5001015, {9012, 1000, 0, 0, 180.0F}));
  combat.advance_private_encounter(kStart, player);
  ASSERT_NE(find_packet(combat.advance_private_encounter(kStart + 1000 * kMillisecond, player),
                        14), nullptr);
  const auto launched = combat.advance_private_encounter(kStart + 1300 * kMillisecond, player);
  ASSERT_NE(find_packet(launched, 40), nullptr);

  const auto cleanup = combat.remove_gm_monster(unit_id, kStart + 1400 * kMillisecond);
  EXPECT_NE(find_packet(cleanup, 44), nullptr);
  EXPECT_NE(find_packet(cleanup, 15), nullptr);
  EXPECT_TRUE(combat.remove_gm_monster(unit_id, kStart + 1500 * kMillisecond).empty());
  const auto later = combat.advance_private_encounter(kStart + 2 * 1000 * kMillisecond, player);
  EXPECT_EQ(find_packet(later, 19), nullptr);
  EXPECT_EQ(find_packet(later, 177), nullptr);
  EXPECT_TRUE(combat.add_gm_monster(unit_id, 5001015, {9012, 1000, 0, 0, 180.0F}))
      << "despawn releases retained actor state instead of accumulating tombstones";
  verify_encoding(cleanup);
}

TEST(Combat, AiMonsterNeverStartsAnAttackWhileItsPursuitIsStillRunning) {
  auto block = npc_ai_block();
  block["run_speed_mm_per_second"] = 1500.0;
  const auto tutorial_data = tutorial_fixture_with_ai(block);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()),
                       tutorial_data, character(), player_stats());
  const WorldPosition position{8000, 6000, 0, 0};

  // 3000 mm of travel at 1500 mm/s: two seconds.
  const auto acquired = combat.advance(kStart, position, tutorial);
  EXPECT_EQ(find_packet(acquired, 64), nullptr);
  ASSERT_NE(find_packet(acquired, 177), nullptr);
  // Past the one-second attack timer and inside the former 5000 mm gate, but still en route.
  EXPECT_EQ(find_packet(combat.advance(kStart + 1000 * kMillisecond, position, tutorial), 14),
            nullptr);
  EXPECT_EQ(find_packet(combat.advance(kStart + 1500 * kMillisecond, position, tutorial), 14),
            nullptr);

  // The attack starts on arrival, after the trajectory is finished, and only once.
  const auto arrived = combat.advance(kStart + 2000 * kMillisecond, position, tutorial);
  const auto *finished = find_packet(arrived, 177);
  const auto *attack = find_packet(arrived, 14);
  ASSERT_NE(finished, nullptr);
  ASSERT_NE(attack, nullptr);
  EXPECT_LT(finished, attack);
  EXPECT_EQ(find_packet(combat.advance(kStart + 2100 * kMillisecond, position, tutorial), 14),
            nullptr);
}

TEST(Combat, AiPursuitAndAttackGateShareTheShorterOfCombatDistanceAndSkillRange) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["skills"][1]["range_mm"] = 1500;
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 6000, 0, 0};

  // ai.combat_distance_max_mm is 2000 but the skill reaches 1500: stop 1500 mm from the player.
  const auto acquired = combat.advance(kStart, position, tutorial);
  EXPECT_EQ(find_packet(acquired, 64), nullptr);
  ASSERT_NE(find_packet(acquired, 177), nullptr);
  // 3500 mm at 3000 mm/s is 11666667 internal ticks; the gate opens exactly there.
  const auto arrival = kStart + 11666667;
  const auto arrived = combat.advance(arrival, position, tutorial);
  ASSERT_NE(find_packet(arrived, 177), nullptr);
  EXPECT_EQ(find_packet(arrived, 177)->message.at("pos").at("millimeter_x"), 4500);
  EXPECT_NE(find_packet(arrived, 14), nullptr);
}

TEST(Combat, AiPacketsCarryWireNanosecondTicksWhileSchedulingStaysInternal) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_projectile()),
                       tutorial_data, character(), player_stats());
  const WorldPosition near_player{8000, 0, 0, 0};
  const WorldPosition far_player{8000, 100000, 0, 0};

  const auto check = [&](const std::vector<GameplayPacket> &packets) {
    verify_encoding(packets);
    for (const auto &packet : packets) {
      expect_wire_ticks(packet.message, kStart);
    }
  };

  const auto acquired = combat.advance(kStart, near_player, tutorial);
  ASSERT_NE(find_packet(acquired, 98), nullptr);
  check(acquired);
  // Internal deadline: the AI's 100 ms movement interval in 100 ns ticks, not nanoseconds.
  EXPECT_EQ(combat.next_action_tick(), kStart + 100 * kMillisecond);

  const auto started = combat.advance(kStart + 1000 * kMillisecond, near_player, tutorial);
  ASSERT_NE(find_packet(started, 14), nullptr);
  check(started);

  const auto spawned = combat.advance(kStart + 1300 * kMillisecond, near_player, tutorial);
  ASSERT_NE(find_packet(spawned, 40), nullptr);
  check(spawned);

  // The player leaves the leash radius while the projectile is in flight: the action ends,
  // the flight is despawned (44) and the monster returns (32), all with nanosecond ticks.
  const auto released = combat.advance(kStart + 1500 * kMillisecond, far_player, tutorial);
  ASSERT_NE(find_packet(released, 44), nullptr);
  EXPECT_NE(find_packet(released, 15), nullptr);
  EXPECT_NE(find_packet(released, 98), nullptr);
  EXPECT_NE(find_packet(released, 114), nullptr);
  const auto *returning = find_packet(released, 32);
  ASSERT_NE(returning, nullptr);
  EXPECT_EQ(returning->message.at("state"), "EX5UnitStateType_Return");
  check(released);
}

// Projectile 3 flown by the player's client, as every captured player projectile is
// (isServerDriven false). At 100 cm/s the monster 1000 mm away is reached in one second.
Json client_projectile_fixture() {
  auto definitions = combat_fixture();
  definitions["effects"]["2"] = definitions["effects"]["1"];
  definitions["effects"]["1"]["kind"] = "spawnProjectile";
  definitions["effects"]["1"]["parameters"] = {{"projectileId", 3}, {"maxSpawnCount", 1}};
  definitions["projectiles"]["3"] = {{"duration_ms", 2000},
                                     {"parameters", {{"speed", 100}, {"isServerDriven", false}}},
                                     {"hit_effects", {2}}};
  return definitions;
}

Json millimetres(int x, int y, int z) {
  return {{"millimeter_x", x}, {"millimeter_y", y}, {"millimeter_z", z}};
}

// Mirrors the captured PC2S_ExecuteSpawnProjectileSkillEffectReq of the tutorial fireball.
Json client_projectile_report(const std::string &instance,
                              std::optional<std::uint64_t> target = kMonster,
                              const Json &fire_position = millimetres(0, 0, 0)) {
  Json flight{{"instance_id", instance},
              {"dest_pos", millimetres(1000, 0, 0)},
              {"direction", {{"pitch", -3.8}, {"yaw", 0}, {"roll", 0}}}};
  if (target) {
    flight["target"] = {{"unit_id", std::to_string(*target)}};
  }
  return {{"tick", "292135653968000"},
          {"skill_instance_id", 1},
          {"pos", millimetres(0, 0, 0)},
          {"rotation", {{"yaw", 0}}},
          {"skill_effect",
           {{"effect_id", 1},
            {"fire_pos", fire_position},
            {"projectile_datas", Json::array({flight})}}}};
}

Json client_despawn_report(const std::string &instance, const std::string &reason) {
  return {{"despawn_projectile",
           {{"instance_id", instance},
            {"despawn_tick", "292136323635500"},
            {"pos", millimetres(990, 0, 0)},
            {"reason_type", reason}}}};
}

Json targeted_start_request() {
  auto request = start_request();
  request["target_ids"] = {std::to_string(kMonster)};
  return request;
}

TEST(WorldMobs, ProjectileCaptureFailureCancelsTheHitWithoutLosingTheMonster) {
  auto definitions = client_projectile_fixture();
  definitions["effects"]["2"]["overlap_group"] = "projectile-contact";
  const auto data = std::make_shared<CombatData>(definitions);
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(world_mob_map(), data));
  auto ledger = std::make_shared<quests::QuestIntentLedger>(WorldProgress{});
  int captures = 0;
  combat.set_world_defeat_capture([&](const MonsterDefeat &defeat) {
    if (++captures == 1) {
      throw std::runtime_error("Injected projectile capture failure");
    }
    MonsterRewardCapture capture;
    MonsterRewardReceipt receipt;
    receipt.account_id = 1;
    receipt.character_id = 1;
    receipt.world_id = 1;
    receipt.receipt_id = defeat.reward_event_id;
    receipt.credited_unit_id = defeat.reward_credit_id;
    receipt.npc_cms_id = defeat.npc_cms_id;
    receipt.position = defeat.position;
    capture.recipients.push_back(std::move(receipt));
    capture.quest_captures.push_back(ledger->acquire_capture());
    return capture;
  });

  const WorldPosition position{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, position);
  ASSERT_NE(find_packet(entry, 10), nullptr);
  const auto id = std::stoull(find_packet(entry, 10)->message.at("unit_id").get<std::string>());
  auto first_start = start_request(1);
  first_start["target_ids"] = {std::to_string(id)};
  ASSERT_TRUE(accepted_start(combat.receive(20010, first_start, position, kStart)));
  combat.advance_private_encounter(kStart + 300 * kMillisecond, position);
  combat.receive(20012, client_projectile_report("1", id), position,
                 kStart + 300 * kMillisecond);
  const auto cancelled = combat.advance_private_encounter(kStart + 1300 * kMillisecond, position);
  ASSERT_NE(find_packet(cancelled, 20), nullptr);
  EXPECT_TRUE(find_packet(cancelled, 20)->message.value("damage", false));
  EXPECT_EQ(find_packet(cancelled, 19), nullptr);
  EXPECT_NE(find_packet(cancelled, 42), nullptr);
  EXPECT_EQ(combat.live_monster_health(id), 50);
  EXPECT_TRUE(combat.pending_world_defeats().empty());

  auto second_start = start_request(2);
  second_start["target_ids"] = {std::to_string(id)};
  ASSERT_TRUE(accepted_start(combat.receive(20010, second_start, position,
                                             kStart + 2000 * kMillisecond)));
  combat.advance_private_encounter(kStart + 2300 * kMillisecond, position);
  auto second_report = client_projectile_report("2", id);
  second_report["skill_instance_id"] = 2;
  combat.receive(20012, second_report, position, kStart + 2300 * kMillisecond);
  combat.advance_private_encounter(kStart + 3300 * kMillisecond, position);
  EXPECT_EQ(captures, 2);
  ASSERT_EQ(combat.pending_world_defeats().size(), 1U);
  verify_encoding(cancelled);
}

TEST(Combat, ClientDrivenProjectileIsConfirmedUnderTheClientInstanceAndHitsAtTheServerDeadline) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(client_projectile_fixture()), tutorial_data,
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  ASSERT_EQ(combat.receive(20010, targeted_start_request(), position, kStart).size(), 2u);

  // The stage event no longer announces a second, server-numbered copy of the client's flight.
  EXPECT_EQ(find_packet(combat.advance(kStart + 300 * kMillisecond, position, tutorial), 40),
            nullptr);

  const auto confirmed =
      combat.receive(20012, client_projectile_report("1"), position, kStart + 300 * kMillisecond);
  ASSERT_EQ(confirmed.size(), 1u);
  ASSERT_EQ(confirmed.at(0).opcode, 40);
  EXPECT_EQ(confirmed.at(0).message.at("caster_unit_id"), "7");
  const auto &spawn = confirmed.at(0).message.at("spawn_projectile");
  EXPECT_EQ(spawn.at("instance_id"), "1");
  EXPECT_EQ(spawn.at("skill_instance_id"), 1);
  EXPECT_EQ(spawn.at("projectile_cms_id"), 3);
  EXPECT_EQ(spawn.at("spawn_tick"), "292135653968000");
  EXPECT_EQ(spawn.at("duration"), "2000");
  EXPECT_EQ(spawn.at("target_unit_id"), std::to_string(kMonster));
  EXPECT_EQ(spawn.at("start_pos"), millimetres(0, 0, 0));
  EXPECT_EQ(spawn.at("dest_pos"), millimetres(1000, 0, 0));
  EXPECT_DOUBLE_EQ(spawn.at("start_rotation").at("pitch").get<double>(), -3.8);
  EXPECT_FALSE(spawn.contains("effect_unique_id"));
  verify_encoding(confirmed);

  // A forged early hit report is neither answered nor able to land the hit.
  const Json forged_hit{{"instance_id", "1"},
                        {"hit", {{"hit_unit_id", std::to_string(kMonster)}}}};
  EXPECT_TRUE(combat.receive(20021, forged_hit, position, kStart + 301 * kMillisecond).empty());
  combat.advance(kStart + 1299 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 50);

  const auto hit = combat.advance(kStart + 1300 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 0);
  const auto *damage = find_packet(hit, 19);
  const auto *hit_notification = find_packet(hit, 42);
  ASSERT_NE(damage, nullptr);
  ASSERT_NE(hit_notification, nullptr);
  EXPECT_LT(damage, hit_notification);
  EXPECT_EQ(damage->message.at("instance_id"), "1");
  EXPECT_EQ(damage->message.at("skill_effect_origin_type"), "EX5SkillEffectOriginType_Projectile");
  EXPECT_EQ(hit_notification->message.at("instance_id"), "1");
  EXPECT_EQ(hit_notification->message.at("target_datas").at(0).at("target").at("unit_id"),
            std::to_string(kMonster));
  // The client reports the despawn of its own flight; the server does not announce one.
  EXPECT_EQ(find_packet(hit, 44), nullptr);
  verify_encoding(hit);

  const auto despawn = client_despawn_report("1", "EX5SkillObjectDespawnReasonType_HitTarget");
  const auto echoed = combat.receive(20022, despawn, position, kStart + 1310 * kMillisecond);
  ASSERT_EQ(echoed.size(), 1u);
  EXPECT_EQ(echoed.at(0).opcode, 44);
  EXPECT_EQ(echoed.at(0).message.at("despawn_projectile"), despawn.at("despawn_projectile"));
  verify_encoding(echoed);
  EXPECT_TRUE(combat.receive(20022, despawn, position, kStart + 1320 * kMillisecond).empty());
}

TEST(Combat, OwnedProjectileUpdatesReachObserversWithoutAcceptingAClientDestination) {
  CombatSession combat(std::make_shared<CombatData>(client_projectile_fixture()),
                       tutorial_fixture(), character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, targeted_start_request(), position, kStart);
  combat.receive(20012, client_projectile_report("1"), position, kStart + 300 * kMillisecond);
  Json update{{"instance_id", "1"},
              {"cur_tick", wire_monotonic_tick(kStart + 500 * kMillisecond)},
              {"prev_tick", wire_monotonic_tick(kStart + 400 * kMillisecond)},
              {"cur_pos", millimetres(200, 0, 0)},
              {"prev_pos", millimetres(100, 0, 0)},
              {"dest_pos", millimetres(1000000, 0, 0)},
              {"lost_target", true},
              {"cur_rotation", {{"yaw", 0}}},
              {"prev_rotation", {{"yaw", 0}}}};
  const auto packets = combat.receive_projectile_report(20023, {{"update_projectile", update}},
                                                        position, kStart + 500 * kMillisecond);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].opcode, 46);
  EXPECT_EQ(packets[0].message.at("update_projectile").at("dest_pos"), millimetres(1000, 0, 0));
  EXPECT_EQ(packets[0].message.at("update_projectile").at("lost_target"), false);
  verify_encoding(packets);
  EXPECT_TRUE(combat
                  .receive_projectile_report(20023, {{"update_projectile", update}}, position,
                                             kStart + 500 * kMillisecond)
                  .empty());
  EXPECT_EQ(combat.monster_hp(kMonster), 50);
}

TEST(Combat, ProjectileReportsCannotForgeAnOwnerPositionOrFutureClock) {
  CombatSession combat(std::make_shared<CombatData>(client_projectile_fixture()),
                       tutorial_fixture(), character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, targeted_start_request(), position, kStart);
  combat.receive(20012, client_projectile_report("1"), position, kStart + 300 * kMillisecond);
  const Json valid{{"instance_id", "1"},
                   {"cur_tick", wire_monotonic_tick(kStart + 500 * kMillisecond)},
                   {"prev_tick", wire_monotonic_tick(kStart + 400 * kMillisecond)},
                   {"cur_pos", millimetres(200, 0, 0)},
                   {"prev_pos", millimetres(100, 0, 0)},
                   {"cur_rotation", Json::object()},
                   {"prev_rotation", Json::object()}};
  for (const auto &change : std::vector<Json>{{{"instance_id", "999"}},
                                              {{"cur_tick", "9223372036854775807"}},
                                              {{"cur_pos", millimetres(1000000, 0, 0)}},
                                              {{"prev_tick", "-1"}}}) {
    auto update = valid;
    update.update(change);
    EXPECT_TRUE(combat
                    .receive_projectile_report(20023, {{"update_projectile", update}}, position,
                                               kStart + 500 * kMillisecond)
                    .empty());
  }
  EXPECT_EQ(combat.monster_hp(kMonster), 50);
}

TEST(Combat, ConfirmedClientHitStillWaitsForTheServerAndCannotDealDamageTwice) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(client_projectile_fixture()), tutorial_data,
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, targeted_start_request(), position, kStart);
  combat.receive(20012, client_projectile_report("1"), position, kStart + 300 * kMillisecond);
  const Json hit{
      {"instance_id", "1"},
      {"elapsed_time_milli", 900},
      {"hit", {{"hit_unit_id", std::to_string(kMonster)}, {"hit_pos", millimetres(1000, 0, 0)}}}};
  EXPECT_TRUE(
      combat.receive_projectile_report(20021, hit, position, kStart + 1200 * kMillisecond).empty());
  EXPECT_EQ(combat.monster_hp(kMonster), 50);
  const auto deadline = combat.advance(kStart + 1300 * kMillisecond, position, tutorial);
  EXPECT_EQ(std::ranges::count_if(deadline, [](const auto &packet) { return packet.opcode == 19; }),
            1);
  EXPECT_EQ(combat.monster_hp(kMonster), 0);
  EXPECT_TRUE(
      combat.receive_projectile_report(20021, hit, position, kStart + 1400 * kMillisecond).empty());
}

TEST(Combat, RefusedOrRepeatedClientProjectileReportsGetOneFailNotificationPerFlight) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(client_projectile_fixture()), tutorial_data,
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  const auto expect_refusal = [&](const Json &report, const std::string &instance) {
    const auto refused = combat.receive(20012, report, position, kStart + 300 * kMillisecond);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_EQ(refused.at(0).opcode, 41);
    EXPECT_EQ(refused.at(0).message.at("instance_id"), instance);
    verify_encoding(refused);
  };

  // No action of this instance is running.
  expect_refusal(client_projectile_report("5"), "5");

  ASSERT_EQ(combat.receive(20010, targeted_start_request(), position, kStart).size(), 2u);
  // Launched ten metres away from the caster.
  expect_refusal(client_projectile_report("6", kMonster, millimetres(10000, 0, 0)), "6");
  ASSERT_EQ(combat.receive(20012, client_projectile_report("7"), position,
                           kStart + 300 * kMillisecond)
                .at(0)
                .opcode,
            40);
  // The same client instance cannot be launched twice.
  expect_refusal(client_projectile_report("7"), "7");

  // More flights than the catalog maxSpawnCount allows.
  auto crowded = client_projectile_report("8");
  auto second_flight = crowded["skill_effect"]["projectile_datas"][0];
  second_flight["instance_id"] = "9";
  crowded["skill_effect"]["projectile_datas"].push_back(second_flight);
  const auto refused = combat.receive(20012, crowded, position, kStart + 300 * kMillisecond);
  ASSERT_EQ(refused.size(), 2u);
  EXPECT_EQ(refused.at(0).message.at("instance_id"), "8");
  EXPECT_EQ(refused.at(1).message.at("instance_id"), "9");
}

TEST(Combat, AClientMissWithdrawsItsFlightAndFreeAimCanHitAnEnemyOnTheFlightPath) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(client_projectile_fixture()), tutorial_data,
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  ASSERT_EQ(combat.receive(20010, targeted_start_request(), position, kStart).size(), 2u);
  ASSERT_EQ(combat.receive(20012, client_projectile_report("1"), position,
                           kStart + 300 * kMillisecond)
                .size(),
            1u);

  const auto missed = combat.receive(
      20022, client_despawn_report("1", "EX5SkillObjectDespawnReasonType_HitStatic"), position,
      kStart + 500 * kMillisecond);
  ASSERT_EQ(missed.size(), 1u);
  EXPECT_EQ(missed.at(0).message.at("despawn_projectile").at("reason_type"),
            "EX5SkillObjectDespawnReasonType_HitStatic");
  EXPECT_EQ(find_packet(combat.advance(kStart + 1300 * kMillisecond, position, tutorial), 19),
            nullptr);
  EXPECT_EQ(combat.monster_hp(kMonster), 50);

  // A flight without a live target is still confirmed so that the client shows it.
  CombatSession untargeted(std::make_shared<CombatData>(client_projectile_fixture()),
                           tutorial_data, character(), player_stats());
  ASSERT_EQ(untargeted.receive(20010, start_request(), position, kStart).size(), 2u);
  const auto confirmed = untargeted.receive(20012, client_projectile_report("1", std::nullopt),
                                            position, kStart + 300 * kMillisecond);
  ASSERT_EQ(confirmed.size(), 1u);
  EXPECT_EQ(confirmed.at(0).message.at("spawn_projectile").at("target_unit_id"),
            std::to_string(kMonster));
  untargeted.advance(kStart + 2500 * kMillisecond, position, tutorial);
  EXPECT_EQ(untargeted.monster_hp(kMonster), 0);
}

TEST(Combat, EarlyProjectileReportsCannotAccelerateTheAuthoredEventOrSpawnTwice) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(client_projectile_fixture()), tutorial_data,
                       character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, targeted_start_request(), position, kStart);
  const auto first = combat.receive(20012, client_projectile_report("1"), position, kStart);
  ASSERT_NE(find_packet(first, 40), nullptr);
  const auto duplicate = combat.receive(20012, client_projectile_report("2"), position, kStart);
  ASSERT_NE(find_packet(duplicate, 41), nullptr);
  EXPECT_EQ(find_packet(duplicate, 40), nullptr);
  combat.advance(kStart + 1299 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 50);
  combat.advance(kStart + 1300 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 0);
}

TEST(Combat, ALinearArrowCannotHitAReportedTargetAwayFromItsVisiblePath) {
  auto definitions = client_projectile_fixture();
  definitions["projectiles"]["3"]["parameters"]["projectileType"] = "EX5ProjectileType_Linearity";
  definitions["projectiles"]["3"]["parameters"]["radius"] = 5;
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, targeted_start_request(), position, kStart);
  auto report = client_projectile_report("1");
  report["skill_effect"]["projectile_datas"][0]["dest_pos"] = millimetres(0, 1000, 0);
  report["skill_effect"]["projectile_datas"][0]["direction"]["yaw"] = 90;
  const auto confirmed = combat.receive(20012, report, position, kStart + 300 * kMillisecond);
  ASSERT_NE(find_packet(confirmed, 40), nullptr);
  EXPECT_FALSE(confirmed[0].message.at("spawn_projectile").contains("target_unit_id"));
  combat.advance(kStart + 1500 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 50);
  verify_encoding(confirmed);
}

TEST(Combat, ProjectileSplashSelectsEnemiesAroundImpactAndPreservesTheUniqueEventId) {
  auto definitions = client_projectile_fixture();
  definitions["skills"][0]["layers"][0]["stages"][0]["events"][0]["unique_id"] = 9;
  definitions["effects"]["2"]["targeting"] = {
      {"outerRadius", 20}, {"halfHeight", 100}, {"angle", 360}, {"maxTargetCount", 2}};
  const auto tutorial_data = tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, targeted_start_request(), position, kStart);
  auto report = client_projectile_report("1");
  report["skill_effect"]["unique_effect_id"] = 9;
  const auto confirmed = combat.receive(20012, report, position, kStart + 300 * kMillisecond);
  ASSERT_EQ(confirmed[0].message.at("spawn_projectile").at("effect_unique_id"), 9);
  const auto hit = combat.advance(kStart + 1300 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 0);
  EXPECT_EQ(combat.monster_hp(kInactiveMonster), 0);
  for (const auto &packet : hit) {
    if (packet.opcode == 19) {
      EXPECT_EQ(packet.message.at("skill_effect_unique_id"), 9);
    }
  }
  verify_encoding(hit);
}

Json interval_area_fixture() {
  auto definitions = combat_fixture();
  definitions["effects"]["2"] = definitions["effects"]["1"];
  definitions["effects"]["2"]["parameters"] = {{"fixedDamage", 10}};
  definitions["effects"]["2"]["overlap_group"] = "area-contact";
  definitions["effects"]["1"]["kind"] = "spawnAreaOfEffect";
  definitions["effects"]["1"]["parameters"] = {{"areaOfEffectId", 5}};
  definitions["effects"]["1"]["targeting_purpose"] = {
      {"targetingPosType", "EX5TargetingPosType_MainTarget"}};
  definitions["areas"]["5"] = {{"duration_ms", 600},      {"interval_ms", 200},
                               {"interval_effect_id", 2}, {"radius_mm", 200},
                               {"half_height_mm", 1000},  {"max_targets", 2}};
  return definitions;
}

TEST(Combat, RemovingAPrivateGmCasterDespawnsItsAreaAndCancelsFurtherPulses) {
  auto definitions = interval_area_fixture();
  auto npc_skill = definitions["skills"][0];
  npc_skill["id"] = 201;
  npc_skill["npc"] = true;
  definitions["skills"].push_back(npc_skill);
  definitions["npc_skills"] = {{"5001015", {201}}};
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  constexpr std::uint64_t unit_id = 0x3000000000000012ULL;
  const WorldPosition player{9012, 0, 0, 0};
  ASSERT_TRUE(combat.add_gm_monster(unit_id, 5001015, {9012, 1000, 0, 0, 180.0F}));
  combat.advance_private_encounter(kStart, player);
  ASSERT_NE(find_packet(combat.advance_private_encounter(kStart + 1000 * kMillisecond, player),
                        14), nullptr);
  const auto spawned = combat.advance_private_encounter(kStart + 1300 * kMillisecond, player);
  const auto *area = find_packet(spawned, 10);
  ASSERT_NE(area, nullptr);
  const auto area_id = area->message.at("unit_id");

  const auto cleanup = combat.remove_gm_monster(unit_id, kStart + 1400 * kMillisecond);
  ASSERT_NE(find_packet(cleanup, 45), nullptr);
  EXPECT_EQ(find_packet(cleanup, 45)->message.at("unit_id"), area_id);
  ASSERT_NE(find_packet(cleanup, 11), nullptr);
  EXPECT_EQ(find_packet(cleanup, 11)->message.at("unit_ids"), Json::array({area_id}));
  EXPECT_EQ(find_packet(combat.advance_private_encounter(kStart + 1700 * kMillisecond, player), 19),
            nullptr);
  verify_encoding(cleanup);
}

TEST(Combat, MainTargetAreaHitsEveryPulseDespiteOverlapGroupsAndExpiresOnce) {
  const auto definitions = interval_area_fixture();
  const auto tutorial_data = tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, targeted_start_request(), position, kStart);
  const auto spawned = combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  ASSERT_NE(find_packet(spawned, 10), nullptr);
  EXPECT_EQ(find_packet(spawned, 10)->message.at("nub").at("pos"), millimetres(1000, 0, 0));
  EXPECT_EQ(combat.monster_hp(kMonster), 40);
  combat.advance(kStart + 500 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 30);
  combat.advance(kStart + 700 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kInactiveMonster), 20);
  const auto expired = combat.advance(kStart + 900 * kMillisecond, position, tutorial);
  EXPECT_NE(find_packet(expired, 45), nullptr);
  EXPECT_EQ(combat.monster_hp(kMonster), 20);
  const auto removed = combat.advance(kStart + 2396 * kMillisecond, position, tutorial);
  EXPECT_NE(find_packet(removed, 11), nullptr);
  EXPECT_EQ(find_packet(combat.advance(kStart + 3000 * kMillisecond, position, tutorial), 11),
            nullptr);
  verify_encoding(spawned);
  verify_encoding(expired);
  verify_encoding(removed);
}

TEST(Combat, AreaSpawnHitLandsAtTheCreatedTickFromAThirtyTwoBitAreaUnit) {
  // Elemental-burst shape (areas 160401/160402): the interval pulse is harmless and the damage
  // is the area's aoeSpawnSkillEffectId, applied once when the area appears on the target.
  auto definitions = interval_area_fixture();
  definitions["effects"]["3"] = definitions["effects"]["2"];
  definitions["effects"]["3"]["parameters"] = {{"fixedDamage", 25}};
  definitions["effects"]["3"].erase("overlap_group");
  definitions["areas"]["5"]["spawn_effect_id"] = 3;
  definitions["areas"]["5"]["max_targets"] = 0;
  const auto tutorial_data = tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  combat.receive(20010, targeted_start_request(), position, kStart);
  const auto spawned = combat.advance(kStart + 300 * kMillisecond, position, tutorial);

  const auto added = std::ranges::find_if(
      spawned, [](const GameplayPacket &packet) { return packet.opcode == 10; });
  ASSERT_NE(added, spawned.end());
  EXPECT_EQ(added->message.at("nub").at("pos"), millimetres(1000, 0, 0));
  const auto area_text = added->message.at("unit_id").get<std::string>();
  const auto area_id = std::stoull(area_text);
  EXPECT_LE(area_id, 0xFFFFFFFFULL);
  EXPECT_EQ(area_id & 0xF0000000ULL, 0xD0000000ULL);
  EXPECT_EQ(area_id & 0xFULL, 3U);

  // Spawn hit first (created tick, area unit as caster), then the first interval pulse.
  const auto damage_by_effect = [&](int effect_id) {
    return std::ranges::find_if(spawned, [effect_id](const GameplayPacket &packet) {
      return packet.opcode == 19 && packet.message.at("skill_effect_cms_id") == effect_id;
    });
  };
  const auto spawn_hit = damage_by_effect(3);
  const auto pulse = damage_by_effect(2);
  ASSERT_NE(spawn_hit, spawned.end());
  ASSERT_NE(pulse, spawned.end());
  EXPECT_LT(added, spawn_hit);
  EXPECT_LT(spawn_hit, pulse);
  EXPECT_EQ(spawn_hit->message.at("caster_unit_id"), area_text);
  EXPECT_EQ(spawn_hit->message.at("tick"),
            added->message.at("nub").at("skill_object").at("created_tick"));
  EXPECT_EQ(spawn_hit->message.at("skill_effect_origin_type"),
            "EX5SkillEffectOriginType_AreaOfEffect");
  EXPECT_EQ(combat.monster_hp(kMonster), 15);
  EXPECT_EQ(combat.monster_hp(kInactiveMonster), 50);

  // The spawn hit is applied once; the later pulse adds only the interval damage.
  combat.advance(kStart + 500 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 5);

  // A later area gets a different id from the same 32-bit block while the first one is still
  // awaiting its removal. The intermediate poll ends the first action. The second cast names a
  // target, so its area appears at once instead of waiting for a client placement report.
  combat.advance(kStart + 1200 * kMillisecond, position, tutorial);
  auto second_request = start_request(2);
  second_request["target_ids"] = {std::to_string(kInactiveMonster)};
  combat.receive(20010, second_request, position, kStart + 1600 * kMillisecond);
  const auto second = combat.advance(kStart + 1900 * kMillisecond, position, tutorial);
  const auto *second_area = find_packet(second, 10);
  ASSERT_NE(second_area, nullptr);
  const auto second_id = std::stoull(second_area->message.at("unit_id").get<std::string>());
  EXPECT_NE(second_id, area_id);
  EXPECT_EQ(second_id & 0xFFFFFFFFF000000FULL, 0xD0000003ULL);
  verify_encoding(spawned);
  verify_encoding(second);
}

TEST(Combat, BoxAreaUsesTheCastYawAndExcludesEnemiesOutsideItsWidth) {
  auto definitions = interval_area_fixture();
  definitions["areas"]["5"].update(
      {{"shape", "box"}, {"radius_mm", 0}, {"extent_x_mm", 2000}, {"extent_y_mm", 100}});
  definitions["effects"]["1"]["targeting_purpose"]["targetingPosType"] =
      "EX5TargetingPosType_Caster";
  const auto tutorial_data = tutorial_fixture(true);
  tutorial_data->npcs.at(kMonster)["position"] = {0, 1500, 0};
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  auto request = start_request();
  request["start_skill"]["rotation"]["yaw"] = 90;
  combat.receive(20010, request, position, kStart);
  const auto spawned = combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 40);
  EXPECT_EQ(combat.monster_hp(kInactiveMonster), 50);
  ASSERT_NE(find_packet(spawned, 10), nullptr);
  EXPECT_EQ(find_packet(spawned, 10)->message.at("nub").at("yaw"), 90);
  verify_encoding(spawned);
}

TEST(Combat, AreaLifetimeEndHitStrikesWithItsOwnShapeBeforeTheDespawn) {
  // Lightning storm shape (area 160501, effect 160212): the interval pulse is harmless and the
  // bolt is the lifetime-end hit, a 2 m sphere around the area that reaches beyond the area's
  // own collision radius.
  auto definitions = interval_area_fixture();
  definitions["effects"]["1"]["targeting_purpose"] = {
      {"targetingPosType", "EX5TargetingPosType_Caster"}, {"offsetX", 150.0}};
  definitions["effects"]["3"] = definitions["effects"]["2"];
  definitions["effects"]["3"]["parameters"] = {{"fixedDamage", 25}};
  definitions["effects"]["3"].erase("overlap_group");
  definitions["effects"]["3"]["targeting_kind"] = "shapeSphere";
  definitions["effects"]["3"]["targeting"] = {{"radius", 200}, {"maxTargetCount", 1}};
  definitions["areas"]["5"]["radius_mm"] = 1;
  definitions["areas"]["5"]["despawn_effect_id"] = 3;
  const auto tutorial_data = tutorial_fixture(true);
  tutorial_data->npcs.at(kMonster)["position"] = {500, 1500, 0};
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  auto request = start_request();
  request["start_skill"]["rotation"]["yaw"] = 90;

  combat.receive(20010, request, position, kStart);
  const auto spawned = combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  const auto *area = find_packet(spawned, 10);
  ASSERT_NE(area, nullptr);
  EXPECT_EQ(area->message.at("nub").at("pos"), millimetres(0, 1500, 0));
  EXPECT_EQ(combat.monster_hp(kMonster), 50);

  // The area lives 600 ms; its bolt lands at the end tick, ahead of the LifeTime despawn.
  const auto expired = combat.advance(kStart + 900 * kMillisecond, position, tutorial);
  const auto damage = std::ranges::find_if(
      expired, [](const GameplayPacket &packet) { return packet.opcode == 19; });
  const auto despawn = std::ranges::find_if(
      expired, [](const GameplayPacket &packet) { return packet.opcode == 45; });
  ASSERT_NE(damage, expired.end());
  ASSERT_NE(despawn, expired.end());
  EXPECT_LT(damage, despawn);
  EXPECT_EQ(damage->message.at("skill_effect_cms_id"), 3);
  EXPECT_EQ(damage->message.at("caster_unit_id"), area->message.at("unit_id"));
  EXPECT_EQ(damage->message.at("base_caster_unit_id"), "7");
  EXPECT_EQ(damage->message.at("skill_effect_origin_type"),
            "EX5SkillEffectOriginType_AreaOfEffect");
  EXPECT_EQ(damage->message.at("tick"), wire_monotonic_tick(kStart + 900 * kMillisecond));
  EXPECT_EQ(combat.monster_hp(kMonster), 25);
  EXPECT_EQ(combat.monster_hp(kInactiveMonster), 50);
  verify_encoding(spawned);
  verify_encoding(expired);
}

TEST(Combat, CasterAreaIsPlacedAtItsAuthoredForwardOffsetAlongTheCastYaw) {
  // Lightning line 2266 shape: Caster-anchored areas with targetingPurpose.offsetX in catalog
  // centimetres. Official captures place each area that far ahead of the caster along the cast
  // yaw; before this rule every strike landed under the caster.
  auto definitions = interval_area_fixture();
  definitions["effects"]["1"]["targeting_purpose"] = {
      {"targetingPosType", "EX5TargetingPosType_Caster"},
      {"offsetX", 150.0},
      {"stickToGround", true}};
  const auto tutorial_data = tutorial_fixture(true);
  tutorial_data->npcs.at(kMonster)["position"] = {0, 1500, 0};
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  auto request = start_request();
  request["start_skill"]["rotation"]["yaw"] = 90;

  combat.receive(20010, request, position, kStart);
  const auto spawned = combat.advance(kStart + 300 * kMillisecond, position, tutorial);

  const auto *area = find_packet(spawned, 10);
  ASSERT_NE(area, nullptr);
  EXPECT_EQ(area->message.at("nub").at("pos"), millimetres(0, 1500, 0));
  EXPECT_EQ(area->message.at("nub").at("yaw"), 90);
  EXPECT_EQ(combat.monster_hp(kMonster), 40);
  EXPECT_EQ(combat.monster_hp(kInactiveMonster), 50);
  verify_encoding(spawned);
}

TEST(Combat, ServerDrivenNpcProjectileLaunchesFromItsSpawnOffsetAndReportsItsHit) {
  auto definitions = combat_fixture_with_npc_projectile();
  definitions["effects"]["1"]["parameters"]["spawnOffsetX"] = 50;
  definitions["effects"]["1"]["parameters"]["spawnOffsetZ"] = 96;
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  combat.advance(kStart, position, tutorial);
  ASSERT_NE(find_packet(combat.advance(kStart + 1000 * kMillisecond, position, tutorial), 14),
            nullptr);
  const auto launched = combat.advance(kStart + 1300 * kMillisecond, position, tutorial);
  const auto *spawn = find_packet(launched, 40);
  ASSERT_NE(spawn, nullptr);
  const auto &flight = spawn->message.at("spawn_projectile");
  // The monster at x = 1000 faces the player at the origin: 500 mm ahead is x = 500, 960 mm up.
  EXPECT_EQ(flight.at("start_pos"), millimetres(500, 0, 960));
  EXPECT_EQ(flight.at("dest_pos"), millimetres(0, 0, 0));
  const auto instance = flight.at("instance_id").get<std::string>();
  EXPECT_GE(std::stoull(instance), 0x40000000u);

  // sqrt(500^2 + 960^2) = 1082.4 mm at 1000 mm/s: the hit lands 1083 ms after the launch.
  EXPECT_EQ(find_packet(combat.advance(kStart + 2382 * kMillisecond, position, tutorial), 19),
            nullptr);
  const auto hit = combat.advance(kStart + 2383 * kMillisecond, position, tutorial);
  const auto *damage = find_packet(hit, 19);
  const auto *hit_notification = find_packet(hit, 42);
  const auto *despawn = find_packet(hit, 44);
  ASSERT_NE(damage, nullptr);
  ASSERT_NE(hit_notification, nullptr);
  ASSERT_NE(despawn, nullptr);
  EXPECT_LT(damage, hit_notification);
  EXPECT_LT(hit_notification, despawn);
  EXPECT_EQ(damage->message.at("instance_id"), instance);
  EXPECT_EQ(damage->message.at("skill_effect_origin_type"), "EX5SkillEffectOriginType_Projectile");
  EXPECT_EQ(hit_notification->message.at("target_datas").at(0).at("target").at("unit_id"), "7");
  const auto &despawned = despawn->message.at("despawn_projectile");
  EXPECT_EQ(despawned.at("instance_id"), instance);
  EXPECT_EQ(despawned.at("reason_type"), "EX5SkillObjectDespawnReasonType_HitTarget");
  EXPECT_EQ(despawned.at("hit_unit_id"), "7");
  verify_encoding(hit);
}

TEST(Combat, StaticProfileAndAllClassSpawnPacketsEncodeAgainstRecoveredProtocol) {
  const char *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS for the private exported profile";
  }
  const auto tutorial_data = TutorialData::load(path);
  ASSERT_TRUE(tutorial_data->document.contains("combat"));
  const auto data = std::make_shared<CombatData>(tutorial_data->document.at("combat"));
  EXPECT_GT(data->skills.size(), 500u);
  ClientCatalog catalog(std::filesystem::path(path).parent_path().parent_path() / "catalog");
  const auto stats = CharacterStats::load(catalog);
  std::map<int, int> counts;
  for (const auto &[id, skill] : data->skills) {
    for (const auto &class_id : skill.at("classes")) {
      ++counts[class_id.get<int>()];
    }
  }
  for (const int class_id : {1, 2, 3, 5}) {
    const auto player = character(class_id);
    CombatSession combat(data, tutorial_data, player, stats->stats(player));
    EXPECT_GT(counts[class_id], 50);
    auto packets = TutorialSession(tutorial_data, player.id, {}, [](const auto &, const auto &) {
                     return true;
                   }).sectors();
    for (auto &packet : packets) {
      combat.decorate_spawn(packet);
    }
    verify_encoding(packets);
  }
}

TEST(Combat, CatalogSkill1418RefusesInsufficientStamina) {
  auto definitions = combat_fixture();
  auto &skill = definitions["skills"][0];
  skill["id"] = 1418;
  skill["weapon"] = "LongBow";
  skill["costs"] = Json{{"SP", 10}};
  skill["cooldown_ms"] = 0;
  definitions["weapon_items"][std::to_string(kWildStarterGear.front().cms_id)] = "LongBow";
  const auto data = std::make_shared<CombatData>(definitions);
  const auto tutorial_data = tutorial_fixture();
  const WorldPosition position{8000, 0, 0, 0};
  auto request = start_request();
  request["start_skill"]["skill_id"] = 1418;

  auto insufficient_stats = player_stats();
  insufficient_stats["19"] = 9;
  CombatSession insufficient(data, tutorial_data, character(kWildCharacterClass),
                             insufficient_stats);
  ASSERT_EQ(insufficient.player_stats().at("72"), 9);
  const auto refused = insufficient.receive(20010, request, position, kStart);
  ASSERT_EQ(refused.size(), 1u);
  ASSERT_EQ(refused.front().opcode, 13);
  // Expected red: capture says NotEnoughCost; current CombatSession emits NotEnoughSP.
  EXPECT_EQ(refused.front().message.at("ec"), "Skill_Condition_NotEnoughCost");
  EXPECT_EQ(insufficient.player_stats().at("72"), 9);

  auto exact_stats = player_stats();
  exact_stats["19"] = 10;
  CombatSession exact(data, tutorial_data, character(kWildCharacterClass), exact_stats);
  ASSERT_EQ(exact.player_stats().at("72"), 10);
  const auto accepted = exact.receive(20010, request, position, kStart);
  ASSERT_FALSE(accepted.empty());
  ASSERT_EQ(accepted.front().opcode, 53);
  ASSERT_EQ(accepted.front().message.at("stat_changes").size(), 1u);
  EXPECT_EQ(accepted.front().message.at("stat_changes").at(0).at("delta"), -10);
  EXPECT_EQ(exact.player_stats().at("72"), 0);
}

TEST(Combat, CatalogSkill1100515CooldownPreservesStaminaAndDeadlineOnRefusal) {
  auto definitions = combat_fixture();
  auto &skill = definitions["skills"][0];
  skill["id"] = 1100515;
  skill["weapon"] = "HandAxe";
  skill["type"] = "Command";
  skill["costs"] = Json{{"SP", 20}};
  skill["cooldown_ms"] = 1000;
  // The short test-local action ends before the catalog cooldown expires.
  skill["layers"][0]["stages"][0]["duration_ms"] = 400;
  definitions["weapon_items"][std::to_string(kFightStarterGear.front().cms_id)] = "HandAxe";
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) {
    return true;
  });
  auto stats = player_stats();
  stats["25"] = 0;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data,
                       character(1), stats);
  ASSERT_EQ(combat.player_stats().at("72"), 50);
  const WorldPosition position{8000, 0, 0, 0};
  auto request = start_request();
  request["start_skill"]["skill_id"] = 1100515;

  const auto first = combat.receive(20010, request, position, kStart);
  ASSERT_FALSE(first.empty());
  ASSERT_EQ(first.front().opcode, 53);
  const auto original_deadline = wire_monotonic_tick(kStart + 1000 * kMillisecond);
  EXPECT_EQ(first.front().message.at("cooldowns").at(0).at("expire_tick"),
            original_deadline);
  EXPECT_EQ(combat.player_stats().at("72"), 30);

  combat.advance(kStart + 500 * kMillisecond, position, tutorial);
  ASSERT_FALSE(combat.busy());
  request["start_skill"]["instance_id"] = 2;
  const auto refused = combat.receive(20010, request, position,
                                     kStart + 500 * kMillisecond);
  ASSERT_EQ(refused.size(), 1u);
  ASSERT_EQ(refused.front().opcode, 13);
  EXPECT_EQ(refused.front().message.at("ec"), "Skill_Cooldown");
  EXPECT_EQ(combat.player_stats().at("72"), 30);

  // A failed attempt must not extend the original deadline.
  combat.advance(kStart + 1001 * kMillisecond, position, tutorial);
  request["start_skill"]["instance_id"] = 3;
  const auto accepted = combat.receive(20010, request, position,
                                      kStart + 1001 * kMillisecond);
  ASSERT_FALSE(accepted.empty());
  ASSERT_EQ(accepted.front().opcode, 53);
  ASSERT_EQ(accepted.front().message.at("stat_changes").size(), 1u);
  EXPECT_EQ(accepted.front().message.at("stat_changes").at(0).at("delta"), -20);
  EXPECT_EQ(combat.player_stats().at("72"), 10);
}

// ---- Self-cast addStatusEffect (catalog duration, opcode 49 add, opcode 51 removal at expiry).
// ----

namespace {

constexpr int kBuffStatusId = 2001005;
constexpr int kZeroDurationStatusId = 2001006;
constexpr int kUnlistedStatusId = 2009999;

// Skill 200 gains a second event at 100 ms: a self-targeted addStatusEffect effect.
Json status_fixture(int status_effect_id, Json status_effects) {
  auto definitions = combat_fixture();
  definitions["effects"]["2"] = {
      {"kind", "addStatusEffect"},
      {"parameters",
       {{"statusEffectId", static_cast<double>(status_effect_id)},
        {"stackCount", 1},
        {"rate", 100.0},
        {"level", 1}}},
      {"targeting_kind", "refTarget"},
      {"targeting", {{"refType", "EX5ReferenceTargetType_Self"}}},
      {"chain", 0}};
  definitions["skills"][0]["layers"][0]["stages"][0]["events"].push_back(
      {{"effect_id", 2}, {"unique_id", 1}, {"time_ms", 100}});
  definitions["status_effects"] = std::move(status_effects);
  return definitions;
}

std::vector<GameplayPacket> packets_with_opcode(const std::vector<GameplayPacket> &packets,
                                                std::uint16_t opcode) {
  std::vector<GameplayPacket> result;
  for (const auto &packet : packets) {
    if (packet.opcode == opcode) {
      result.push_back(packet);
    }
  }
  return result;
}

// Starts skill 200 and advances past its 100 ms event; returns every packet produced.
std::vector<GameplayPacket> cast_and_run_event(CombatSession &combat, TutorialSession &tutorial,
                                               std::uint32_t instance, std::int64_t at) {
  const WorldPosition position{8000, 0, 0, 0};
  auto packets = combat.receive(20010, start_request(instance), position, at);
  const auto later = combat.advance(at + 150 * kMillisecond, position, tutorial);
  packets.insert(packets.end(), later.begin(), later.end());
  return packets;
}

} // namespace

TEST(Combat, GmSelfEffectAndExplicitRemovalUseNormalStatusLifecycle) {
  const auto definitions = status_fixture(
      kBuffStatusId, {{std::to_string(kBuffStatusId), {{"duration_ms", 1000}}}});
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  const WorldPosition position{9012, 0, 0, 0};
  const auto added = combat.gm_execute_self_effect(2, position, kStart);
  EXPECT_NE(find_packet(added, 49), nullptr);
  EXPECT_TRUE(combat.gm_execute_self_effect(1, position, kStart).empty());
  const auto removed = combat.gm_remove_status_effect(7, kBuffStatusId, kStart);
  EXPECT_NE(find_packet(removed, 51), nullptr);
  EXPECT_TRUE(combat.gm_remove_status_effect(7, kBuffStatusId, kStart).empty());
  verify_encoding(added);
  verify_encoding(removed);
}

TEST(Combat, GmPauseStopsPursuitAndResumeRearmsIt) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  constexpr std::uint64_t unit_id = 0xF0010012;
  ASSERT_TRUE(combat.add_gm_monster(unit_id, 5001015, {9012, 6000, 0, 0}));
  const WorldPosition player{9012, 0, 0, 0};
  combat.advance_private_encounter(kStart, player);
  const auto paused = combat.gm_toggle_monster_ai(unit_id, false, kStart + 100 * kMillisecond);
  const auto stopped = combat.monster_debug_view(unit_id, kStart + 100 * kMillisecond);
  ASSERT_TRUE(stopped);
  EXPECT_FALSE(stopped->route_end);
  combat.advance_private_encounter(kStart + 1000 * kMillisecond, player);
  EXPECT_EQ(combat.monster_debug_view(unit_id, kStart + 1000 * kMillisecond)->position,
            stopped->position);
  combat.gm_toggle_monster_ai(unit_id, true, kStart + 1000 * kMillisecond);
  combat.advance_private_encounter(kStart + 1000 * kMillisecond, player);
  EXPECT_TRUE(combat.next_action_tick());
  verify_encoding(paused);
}

TEST(Combat, StunnedMonsterStopsPursuingAndResumesAfterTheStatusExpires) {
  constexpr int kStunStatus = 18;
  constexpr std::uint64_t kUnit = 0xF0010013;
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  definitions["status_effects"] = {{"18", {{"duration_ms", 2000}}}};
  definitions["abnormal_gauges"] = {
      {"reduce_tick_ms", 100},
      {"types", {{"Stun", {{"maximum", 20000}, {"reduce_per_tick", 40},
                            {"status_effect_id", kStunStatus}}}}}};
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  const WorldPosition player{9012, 0, 0, 0};
  ASSERT_TRUE(combat.add_gm_monster(kUnit, 5001015, {9012, 6000, 0, 0, 180.0F}));
  EXPECT_FALSE(combat.player_in_combat());
  combat.advance_private_encounter(kStart, player);
  EXPECT_TRUE(combat.player_in_combat());

  const auto stunned = combat.gm_add_status_effect(kUnit, kStunStatus, kStart + 100 * kMillisecond);
  ASSERT_NE(find_packet(stunned, 49), nullptr);
  const auto stopped = combat.monster_debug_view(kUnit, kStart + 100 * kMillisecond);
  ASSERT_TRUE(stopped);
  EXPECT_FALSE(stopped->route_end);
  const auto during = combat.advance_private_encounter(kStart + 1500 * kMillisecond, player);
  EXPECT_EQ(combat.monster_debug_view(kUnit, kStart + 1500 * kMillisecond)->position,
            stopped->position);
  EXPECT_EQ(find_packet(during, 14), nullptr);

  combat.advance_private_encounter(kStart + 2100 * kMillisecond, player);
  combat.advance_private_encounter(kStart + 2400 * kMillisecond, player);
  const auto resumed = combat.monster_debug_view(kUnit, kStart + 2400 * kMillisecond);
  ASSERT_TRUE(resumed);
  EXPECT_TRUE(resumed->route_end || resumed->position != stopped->position);
  combat.gm_damage_monster(kUnit, 1000000, kStart + 2500 * kMillisecond);
  EXPECT_FALSE(combat.player_in_combat());
}

TEST(Combat, RemovingStunEarlyLetsMonsterResumePursuit) {
  constexpr int kStunStatus = 18;
  constexpr std::uint64_t kUnit = 0xF0010013;
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  definitions["status_effects"] = {{"18", {{"duration_ms", 2000}}}};
  definitions["abnormal_gauges"] = {
      {"reduce_tick_ms", 100},
      {"types", {{"Stun", {{"maximum", 20000}, {"reduce_per_tick", 40},
                            {"status_effect_id", kStunStatus}}}}}};
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  const WorldPosition player{9012, 0, 0, 0};
  ASSERT_TRUE(combat.add_gm_monster(kUnit, 5001015, {9012, 6000, 0, 0, 180.0F}));
  combat.advance_private_encounter(kStart, player);
  combat.gm_add_status_effect(kUnit, kStunStatus, kStart + 100 * kMillisecond);
  const auto stopped = combat.monster_debug_view(kUnit, kStart + 100 * kMillisecond);
  ASSERT_TRUE(stopped);
  combat.gm_remove_status_effect(kUnit, kStunStatus, kStart + 500 * kMillisecond);
  combat.advance_private_encounter(kStart + 500 * kMillisecond, player);
  const auto resumed = combat.monster_debug_view(kUnit, kStart + 500 * kMillisecond);
  ASSERT_TRUE(resumed);
  EXPECT_TRUE(resumed->route_end || resumed->position != stopped->position);
}

TEST(Combat, SelfCastStatusEffectIsAddedWithCatalogDurationAndRemovedOnceAtExpiry) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(
      std::make_shared<CombatData>(status_fixture(
          kBuffStatusId, {{std::to_string(kBuffStatusId), {{"duration_ms", 1000}}}})),
      tutorial_data, character(), player_stats());

  const auto packets = cast_and_run_event(combat, tutorial, 1, kStart);
  const auto added = packets_with_opcode(packets, 49);
  ASSERT_EQ(added.size(), 1u);
  const auto &message = added.front().message;
  EXPECT_EQ(message.at("unit_id"), "7");
  const auto &status = message.at("status_effect");
  EXPECT_EQ(status.at("status_effect_id"), kBuffStatusId);
  EXPECT_EQ(status.at("level"), 1);
  EXPECT_EQ(status.at("stack_count"), 1);
  EXPECT_EQ(status.at("caster_unit_id"), "7");
  const auto start_ns = std::stoll(status.at("start_tick").get<std::string>());
  const auto expire_ns = std::stoll(status.at("expire_tick").get<std::string>());
  EXPECT_EQ(expire_ns - start_ns, 1000LL * 1000000LL) << "catalog duration in nanoseconds";
  verify_encoding(added);

  // Not expired yet: no removal. The scheduler asks for the expiry tick.
  ASSERT_TRUE(combat.next_action_tick().has_value());
  const WorldPosition position{8000, 0, 0, 0};
  const auto expire_internal = expire_ns / 100;
  EXPECT_TRUE(packets_with_opcode(
                  combat.advance(expire_internal - kMillisecond, position, tutorial), 51)
                  .empty());

  const auto removed = packets_with_opcode(
      combat.advance(expire_internal + kMillisecond, position, tutorial), 51);
  ASSERT_EQ(removed.size(), 1u);
  EXPECT_EQ(removed.front().message.at("unit_id"), "7");
  EXPECT_EQ(removed.front().message.at("status_effects").at(0).at("status_effect_id"),
            kBuffStatusId);
  EXPECT_EQ(removed.front().message.at("status_effects").at(0).at("instance_id"),
            status.at("instance_id"));
  verify_encoding(removed);
  EXPECT_TRUE(packets_with_opcode(
                  combat.advance(expire_internal + 10 * kMillisecond, position, tutorial), 51)
                  .empty());
}

// The cast above reports the skill start, its executed effect and the status the player gained,
// so CounterUseSkill, CounterUseSkillEffect and AddStatusEffect objectives can advance.
TEST(Combat, PlayerSkillUseEffectsAndStatusesAreReportedForQuests) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(
      std::make_shared<CombatData>(status_fixture(
          kBuffStatusId, {{std::to_string(kBuffStatusId), {{"duration_ms", 1000}}}})),
      tutorial_data, character(), player_stats());

  cast_and_run_event(combat, tutorial, 1, kStart);
  const auto events = combat.take_quest_events();
  const auto has = [&events](QuestEventKind kind) {
    return std::ranges::any_of(events, [kind](const QuestEvent &event) {
      return event.kind == kind;
    });
  };
  EXPECT_TRUE(has(QuestEventKind::SkillUsed));
  EXPECT_TRUE(has(QuestEventKind::SkillEffectExecuted));
  EXPECT_TRUE(std::ranges::any_of(events, [](const QuestEvent &event) {
    return event.kind == QuestEventKind::StatusEffectApplied && event.target_id == kBuffStatusId;
  }));
  EXPECT_TRUE(combat.take_quest_events().empty());
}

TEST(Combat, ActiveStatusEffectIsNotAppliedTwiceAndIsAppliedAgainAfterExpiry) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  auto definitions = status_fixture(
      kBuffStatusId, {{std::to_string(kBuffStatusId), {{"duration_ms", 5000}}}});
  definitions["skills"][0]["cooldown_ms"] = 0;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  ASSERT_EQ(packets_with_opcode(cast_and_run_event(combat, tutorial, 1, kStart), 49).size(), 1u);
  // Second cast while the status is active: the stamina is spent, no second add is emitted.
  combat.advance(kStart + 1200 * kMillisecond, position, tutorial);
  EXPECT_TRUE(packets_with_opcode(
                  cast_and_run_event(combat, tutorial, 2, kStart + 1300 * kMillisecond), 49)
                  .empty());
  // After the 5 s duration the same status may be applied again, as a new instance.
  combat.advance(kStart + 7000 * kMillisecond, position, tutorial);
  const auto again =
      packets_with_opcode(cast_and_run_event(combat, tutorial, 3, kStart + 7100 * kMillisecond), 49);
  ASSERT_EQ(again.size(), 1u);
  EXPECT_EQ(again.front().message.at("status_effect").at("instance_id"), 2);
}

TEST(Combat, ZeroDurationUnlistedAndMissingStatusDefinitionsApplyNothing) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  {
    // An empty catalog duration is exported as 0 and is not a proven permanent status.
    CombatSession combat(
        std::make_shared<CombatData>(status_fixture(
            kZeroDurationStatusId,
            {{std::to_string(kZeroDurationStatusId), {{"duration_ms", 0}}}})),
        tutorial_data, character(), player_stats());
    EXPECT_TRUE(packets_with_opcode(cast_and_run_event(combat, tutorial, 1, kStart), 49).empty());
  }
  {
    CombatSession combat(
        std::make_shared<CombatData>(status_fixture(
            kUnlistedStatusId, {{std::to_string(kBuffStatusId), {{"duration_ms", 1000}}}})),
        tutorial_data, character(), player_stats());
    EXPECT_TRUE(packets_with_opcode(cast_and_run_event(combat, tutorial, 1, kStart), 49).empty());
  }
  {
    // Profiles without the optional key keep working and apply nothing.
    auto definitions = status_fixture(kBuffStatusId, Json::object());
    definitions.erase("status_effects");
    CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                         player_stats());
    EXPECT_TRUE(packets_with_opcode(cast_and_run_event(combat, tutorial, 1, kStart), 49).empty());
  }
}

TEST(Combat, StatusEffectDefinitionsAreValidated) {
  for (const auto &row :
       {Json{{"duration_ms", -1}}, Json{{"duration_ms", 1.5}}, Json{{"duration_ms", "1000"}},
        Json{{"duration_ms", 86400001}}, Json::object(), Json(5)}) {
    auto definitions = combat_fixture();
    definitions["status_effects"] = {{"2001005", row}};
    EXPECT_THROW(CombatData{definitions}, std::runtime_error) << row.dump();
  }
  auto definitions = combat_fixture();
  definitions["status_effects"] = Json::array();
  EXPECT_THROW(CombatData{definitions}, std::runtime_error);
}

TEST(Combat, PlayerDeathRemovesActiveSelfCastStatusOnceAndReviveEmitsNoRemoval) {
  auto definitions = status_fixture(
      kBuffStatusId, {{std::to_string(kBuffStatusId), {{"duration_ms", 20000}}}});
  // Effect 3: a lethal monster hit that reaches the player from any distance.
  definitions["effects"]["3"] = {{"kind", "damage"},
                                 {"parameters", {{"fixedDamage", 100000}}},
                                 {"targeting_kind", "shapeSphere"},
                                 {"targeting", {{"outerRadius", 100000}, {"halfHeight", 100000}}},
                                 {"chain", 0}};
  Json monster_skill = definitions.at("skills").at(0);
  monster_skill["id"] = 201;
  monster_skill["npc"] = true;
  monster_skill["layers"][0]["stages"][0]["events"] = {
      {{"effect_id", 3}, {"unique_id", 0}, {"time_ms", 300}}};
  definitions["skills"].push_back(monster_skill);
  definitions["npc_skills"] = {{"5001015", {201}}, {"5001528", {201}}};
  // Keep the player's own skill active for far longer than the fight.
  constexpr int kLongPlayerSkillMs = 20000;
  definitions["skills"][0]["layers"][0]["stages"][0]["duration_ms"] = kLongPlayerSkillMs;

  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 6000, 0, 0};

  auto request = start_request(1);
  request["start_skill"]["pos"] = {{"millimeter_x", 6000}, {"millimeter_y", 0}, {"millimeter_z", 0}};
  std::vector<GameplayPacket> all = combat.receive(20010, request, position, kStart);
  ASSERT_EQ(all.front().opcode, 53) << all.front().message.dump();
  std::int64_t now = kStart;
  while (!combat.player_dead() && now < kStart + 5000 * kMillisecond) {
    now += 50 * kMillisecond;
    const auto step = combat.advance(now, position, tutorial);
    all.insert(all.end(), step.begin(), step.end());
  }
  ASSERT_TRUE(combat.player_dead());
  ASSERT_EQ(packets_with_opcode(all, 49).size(), 1u);
  const auto removed = packets_with_opcode(all, 51);
  ASSERT_EQ(removed.size(), 1u);
  EXPECT_EQ(removed.front().message.at("unit_id"), "7");
  EXPECT_EQ(removed.front().message.at("status_effects").at(0).at("status_effect_id"),
            kBuffStatusId);
  const auto index_of = [&](std::uint16_t opcode) {
    return std::ranges::find_if(all, [opcode](const GameplayPacket &packet) {
             return packet.opcode == opcode;
           }) - all.begin();
  };
  ASSERT_LT(index_of(23), static_cast<std::ptrdiff_t>(all.size()));
  EXPECT_LT(index_of(23), index_of(51)) << "the removal follows the death notification";
  verify_encoding(removed);

  // Official order at a local player's death (3 of 3 captures, same tick): Demise state, lethal
  // DamageNoti, DeathNoti, then ChangeStatNoti (SCS_StatusEffect, stat 71) and a StatSyncNoti with
  // the health regen checkpoint (the durability ItemsUpdateNoti sits between Demise and Damage in
  // the native world and is covered there).
  std::vector<int> death_sequence;
  for (const auto &packet : all) {
    const bool about_the_player =
        packet.message.value("target_unit_id", std::string()) == "7" ||
        packet.message.value("unit_id", std::string()) == "7";
    if (about_the_player && (packet.opcode == 32 || packet.opcode == 19 || packet.opcode == 23 ||
                             packet.opcode == 47 || packet.opcode == 54)) {
      death_sequence.push_back(packet.opcode);
    }
  }
  ASSERT_GE(death_sequence.size(), 5u);
  const auto lethal_start = std::ranges::find(death_sequence, 32);
  ASSERT_TRUE(lethal_start != death_sequence.end());
  EXPECT_EQ((std::vector<int>(lethal_start, lethal_start + 5)),
            (std::vector<int>{32, 19, 23, 54, 47}));
  // Official: no EndSkill for the dying player within 200 ms of the death (0 of 4).
  const auto death_notice = std::ranges::find_if(all, [](const GameplayPacket &packet) {
    return packet.opcode == 23 &&
           packet.message.value("target_unit_id", std::string()) == "7";
  });
  ASSERT_TRUE(death_notice != all.end());
  // The player's action must still be running when the lethal hit lands (it lasts far longer than
  // the fight), otherwise the absence of a post-death EndSkill would prove nothing: the old code
  // announced one here.
  EXPECT_TRUE(std::none_of(all.begin(), death_notice, [](const GameplayPacket &packet) {
    return packet.opcode == 15 && packet.message.value("unit_id", std::string()) == "7";
  })) << "the player's skill ended before the lethal hit";
  EXPECT_TRUE(std::none_of(death_notice, all.end(), [](const GameplayPacket &packet) {
    return packet.opcode == 15 && packet.message.value("unit_id", std::string()) == "7";
  }));
  const auto change_stat = packets_with_opcode(all, 47);
  ASSERT_FALSE(change_stat.empty());
  const auto death_stat = std::ranges::find_if(change_stat, [](const GameplayPacket &packet) {
    return packet.message.value("default_source", std::string()) == "SCS_StatusEffect";
  });
  ASSERT_TRUE(death_stat != change_stat.end());
  EXPECT_EQ(death_stat->message.at("stat_changes").at(0).at("stat_type"), 71);
  const auto sync = packets_with_opcode(all, 54);
  ASSERT_FALSE(sync.empty());
  EXPECT_EQ(sync.back().message.at("regens").at(0).at("checkpoint").at("source"), "SCS_Regen");
  verify_encoding(change_stat);
  verify_encoding(sync);

  // Revive adds and removes only the spawn-reset status 50030002; the self-cast status that death
  // already removed is neither re-added nor removed again.
  constexpr int kSpawnResetStatusId = 50030002;
  const auto revived = combat.revive(now + 1000 * kMillisecond);
  const auto revived_status_ids = [&](std::uint16_t opcode) {
    std::vector<int> status_ids;
    for (const auto &packet : packets_with_opcode(revived, opcode)) {
      const auto &status = opcode == 49 ? packet.message.at("status_effect")
                                        : packet.message.at("status_effects").at(0);
      status_ids.push_back(status.at("status_effect_id").get<int>());
    }
    return status_ids;
  };
  EXPECT_EQ(revived_status_ids(49), std::vector<int>{kSpawnResetStatusId});
  EXPECT_EQ(revived_status_ids(51), std::vector<int>{kSpawnResetStatusId});
  // The original 20 s expiry passes without a second removal.
  EXPECT_TRUE(
      packets_with_opcode(combat.advance(kStart + 30000 * kMillisecond, position, tutorial), 51)
          .empty());
}

// Needs the regenerated tutorial profile (CHRONICLES_TUTORIAL_DEFINITIONS) whose npc_skills include
// the captured casts of NPC 5001084 (190100200 and 190100300,
// protocol/tutorial-combat-observations.json). A skip is not proof. Every distance below comes from
// that profile: the player stands half of the AI's engagement range (the shorter of NpcTemplate
// combat distance and the attack skill range) from the monster.
TEST(Combat, ProfileMonster5001084AcquiresAfterAPlayerHitAndHurtsThePlayerWithACapturedSkill) {
  const char *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS for the regenerated profile";
  }
  constexpr int kMonsterCmsId = 5001084;
  constexpr int kPlayerBasicAttack = 1100000;
  const auto tutorial_data = TutorialData::load(path);
  const auto &combat_document = tutorial_data->document.at("combat");
  const auto data = std::make_shared<CombatData>(combat_document);

  const auto &attack_ids = combat_document.at("npc_skills").at(std::to_string(kMonsterCmsId));
  const auto has_attack = [&](int skill_id) {
    return std::ranges::any_of(attack_ids,
                               [skill_id](const Json &entry) { return entry.get<int>() == skill_id; });
  };
  ASSERT_TRUE(has_attack(190100200));
  ASSERT_TRUE(has_attack(190100300));

  const auto spawn = std::ranges::find_if(tutorial_data->document.at("npc_spawns"),
                                          [](const Json &npc) {
                                            return npc.at("cms_id") == kMonsterCmsId;
                                          });
  ASSERT_TRUE(spawn != tutorial_data->document.at("npc_spawns").end());
  const std::string monster_text = spawn->at("unit_id");
  const std::uint64_t monster_id = std::stoull(monster_text);
  const auto &monster_position = spawn->at("position");

  std::int64_t attack_range_mm = 0;
  for (const auto &skill_id : attack_ids) {
    const auto &skill = data->skills.at(skill_id.get<int>());
    if (skill.at("npc").get<bool>() &&
        !skill.at("layers").at(0).at("stages").at(0).at("events").empty()) {
      attack_range_mm = skill.at("range_mm").get<std::int64_t>();
      break;
    }
  }
  ASSERT_GT(attack_range_mm, 0);
  const std::int64_t engagement_mm = std::min(
      spawn->at("ai").at("combat_distance_max_mm").get<std::int64_t>(), attack_range_mm);
  const WorldPosition player_position{
      8000, static_cast<int>(monster_position.at(0).get<int>() + engagement_mm / 2),
      monster_position.at(1).get<int>(), monster_position.at(2).get<int>(), 180.0F};

  auto stats = player_stats();
  stats["18"] = stats["69"] = 100000;
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(data, tutorial_data, character(kWildCharacterClass), stats);
  const int initial_health = combat.player_stats().at("69");
  ASSERT_GT(combat.monster_hp(monster_id), 0);
  const int initial_monster_health = combat.monster_hp(monster_id);

  Json request{{"start_skill",
                {{"skill_id", kPlayerBasicAttack},
                 {"instance_id", 1},
                 {"pos",
                  {{"millimeter_x", player_position.x_mm},
                   {"millimeter_y", player_position.y_mm},
                   {"millimeter_z", player_position.z_mm}}},
                 {"rotation", {{"yaw", 180.0}}},
                 {"tick", "999999999999"}}},
               {"target_ids", {monster_text}}};
  const auto started = combat.receive(20010, request, player_position, kStart);
  ASSERT_FALSE(started.empty());
  ASSERT_NE(started.front().opcode, 13) << started.front().message.dump();

  std::vector<GameplayPacket> packets;
  for (std::int64_t elapsed_ms = 50; elapsed_ms <= 10000; elapsed_ms += 50) {
    auto step = combat.advance(kStart + elapsed_ms * kMillisecond, player_position, tutorial);
    packets.insert(packets.end(), step.begin(), step.end());
  }
  EXPECT_LT(combat.monster_hp(monster_id), initial_monster_health);

  const auto from_monster = [&](const GameplayPacket &packet, std::uint16_t opcode) {
    return packet.opcode == opcode && packet.message.value("unit_id", std::string{}) == monster_text;
  };
  const auto acquired = std::ranges::find_if(packets, [&](const GameplayPacket &packet) {
    return from_monster(packet, 98) && packet.message.contains("new_target_unit_id");
  });
  const auto combat_state = std::ranges::find_if(packets, [&](const GameplayPacket &packet) {
    return from_monster(packet, 32) && packet.message.at("state") == "EX5UnitStateType_Combat";
  });
  const auto phase = std::ranges::find_if(packets, [&](const GameplayPacket &packet) {
    return from_monster(packet, 114) && packet.message.contains("new_ai_phase_transition_id");
  });
  const auto attack = std::ranges::find_if(packets, [&](const GameplayPacket &packet) {
    return from_monster(packet, 14);
  });
  ASSERT_TRUE(acquired != packets.end());
  ASSERT_TRUE(combat_state != packets.end());
  ASSERT_TRUE(phase != packets.end());
  ASSERT_TRUE(attack != packets.end()) << "the monster never started a skill";
  EXPECT_LT(acquired, attack);
  const int cast = attack->message.at("start_skill").at("skill_id").get<int>();
  EXPECT_TRUE(cast == 190100200 || cast == 190100300) << cast;

  EXPECT_LT(combat.player_stats().at("69").get<int>(), initial_health);
  verify_encoding(packets);
}

TEST(Combat, DefeatedMonsterUnitIsRemovedExactlyOnceAtTheCandidateCorpseDelay) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture()), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  const auto removals = [](const std::vector<GameplayPacket> &packets) {
    return std::ranges::count_if(packets,
                                 [](const GameplayPacket &packet) { return packet.opcode == 11; });
  };

  ASSERT_EQ(combat.receive(20010, start_request(), position, kStart).size(), 2u);
  const auto death_tick = kStart + 300 * kMillisecond;
  const auto killed = combat.advance(death_tick, position, tutorial);
  ASSERT_EQ(combat.monster_hp(kMonster), 0);
  EXPECT_EQ(removals(killed), 0);

  // The scheduler must wake for the removal even when nothing else is pending.
  const auto deadline = death_tick + kCorpseRemovalDelayMs * kMillisecond;
  const auto next = combat.next_action_tick();
  ASSERT_TRUE(next.has_value());
  EXPECT_LE(*next, deadline);

  EXPECT_EQ(removals(combat.advance(deadline - kMillisecond, position, tutorial)), 0);
  const auto removed = combat.advance(deadline, position, tutorial);
  ASSERT_EQ(removals(removed), 1);
  const auto frame = std::ranges::find_if(
      removed, [](const GameplayPacket &packet) { return packet.opcode == 11; });
  ASSERT_EQ(frame->message.at("unit_ids").size(), 1u);
  EXPECT_EQ(frame->message.at("unit_ids").at(0), std::to_string(kMonster));
  verify_encoding(removed);

  EXPECT_EQ(removals(combat.advance(deadline + 1000 * kMillisecond, position, tutorial)), 0);
  EXPECT_EQ(removals(combat.advance(deadline + 60000 * kMillisecond, position, tutorial)), 0);
}

// Three stage events reference one damage effect. Official captures (5001083 / 194200300: three
// events, one DamageNoti per cast) show that an effect with a catalog overlapGroupId lands once per
// target per cast; an effect without one lands at every event. The mitigation term is not modelled.
int damage_notifications_for_three_event_cast(bool with_overlap_group, int &monster_health) {
  auto definitions = combat_fixture();
  definitions["effects"]["1"]["parameters"]["physicalCoefficient"] = 0.1;
  if (with_overlap_group) {
    definitions["effects"]["1"]["overlap_group"] = "194200300";
  }
  auto &events = definitions["skills"][0]["layers"][0]["stages"][0]["events"];
  events = Json::array({{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 100}},
                        {{"effect_id", 1}, {"unique_id", 1}, {"time_ms", 200}},
                        {{"effect_id", 1}, {"unique_id", 2}, {"time_ms", 300}}});
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);
  int damage_notifications = 0;
  for (const auto offset_ms : {100, 200, 300, 400}) {
    for (const auto &packet : combat.advance(kStart + offset_ms * kMillisecond, position, tutorial)) {
      damage_notifications += packet.opcode == 19 ? 1 : 0;
    }
  }
  monster_health = combat.monster_hp(kMonster);
  return damage_notifications;
}

TEST(Combat, DamageEffectWithAnOverlapGroupLandsOncePerTargetPerCast) {
  int grouped_health = 0;
  int ungrouped_health = 0;
  EXPECT_EQ(damage_notifications_for_three_event_cast(true, grouped_health), 1);
  EXPECT_EQ(damage_notifications_for_three_event_cast(false, ungrouped_health), 3);
  EXPECT_EQ(grouped_health, 44);
  EXPECT_EQ(ungrouped_health, 32);
}

// Two monsters inside one effect shape: the overlap group is scoped per target, so each target
// takes one hit per cast while an ungrouped effect still lands every event on each target.
std::map<std::string, int> damage_notifications_by_target(bool with_overlap_group) {
  auto definitions = combat_fixture();
  definitions["effects"]["1"]["parameters"]["physicalCoefficient"] = 0.1;
  definitions["effects"]["1"]["targeting"]["maxTargetCount"] = 2;
  if (with_overlap_group) {
    definitions["effects"]["1"]["overlap_group"] = "194200300";
  }
  auto &events = definitions["skills"][0]["layers"][0]["stages"][0]["events"];
  events = Json::array({{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 100}},
                        {{"effect_id", 1}, {"unique_id", 1}, {"time_ms", 200}},
                        {{"effect_id", 1}, {"unique_id", 2}, {"time_ms", 300}}});
  const auto tutorial_data = tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);
  std::map<std::string, int> by_target;
  for (const auto offset_ms : {100, 200, 300, 400}) {
    for (const auto &packet : combat.advance(kStart + offset_ms * kMillisecond, position, tutorial)) {
      if (packet.opcode == 19) {
        ++by_target[packet.message.at("target_unit_id").get<std::string>()];
      }
    }
  }
  return by_target;
}

TEST(Combat, OverlapGroupIsScopedPerTargetWhileUngroupedEffectsHitEveryEvent) {
  const auto grouped = damage_notifications_by_target(true);
  ASSERT_EQ(grouped.size(), 2u);
  EXPECT_EQ(grouped.at(std::to_string(kMonster)), 1);
  EXPECT_EQ(grouped.at(std::to_string(kInactiveMonster)), 1);

  const auto ungrouped = damage_notifications_by_target(false);
  ASSERT_EQ(ungrouped.size(), 2u);
  EXPECT_EQ(ungrouped.at(std::to_string(kMonster)), 3);
  EXPECT_EQ(ungrouped.at(std::to_string(kInactiveMonster)), 3);
}

// Real-profile fight: a monster of the augmented tutorial profile (overlap_group on its damage
// effects) is hit once by the player, acquires, and keeps casting at the player for duration_ms.
// Needs CHRONICLES_TUTORIAL_DEFINITIONS to name the profile with overlap groups; a skip is not
// proof.
struct MonsterFight {
  std::shared_ptr<const TutorialData> tutorial_data;
  std::string monster_text;
  std::vector<GameplayPacket> packets;
};

MonsterFight fight_profile_monster(const char *path, int monster_cms_id, std::int64_t duration_ms) {
  constexpr int kPlayerBasicAttack = 1100000;
  MonsterFight fight;
  fight.tutorial_data = TutorialData::load(path);
  const auto &combat_document = fight.tutorial_data->document.at("combat");
  const auto data = std::make_shared<CombatData>(combat_document);

  const auto &attack_ids = combat_document.at("npc_skills").at(std::to_string(monster_cms_id));
  const auto spawn = std::ranges::find_if(fight.tutorial_data->document.at("npc_spawns"),
                                          [&](const Json &npc) {
                                            return npc.at("cms_id") == monster_cms_id;
                                          });
  EXPECT_TRUE(spawn != fight.tutorial_data->document.at("npc_spawns").end());
  if (spawn == fight.tutorial_data->document.at("npc_spawns").end()) {
    return fight;
  }
  fight.monster_text = spawn->at("unit_id");
  const auto &monster_position = spawn->at("position");

  std::int64_t attack_range_mm = 0;
  for (const auto &skill_id : attack_ids) {
    const auto &skill = data->skills.at(skill_id.get<int>());
    if (skill.at("npc").get<bool>() &&
        !skill.at("layers").at(0).at("stages").at(0).at("events").empty()) {
      attack_range_mm = skill.at("range_mm").get<std::int64_t>();
      break;
    }
  }
  EXPECT_GT(attack_range_mm, 0);
  const std::int64_t engagement_mm = std::min(
      spawn->at("ai").at("combat_distance_max_mm").get<std::int64_t>(), attack_range_mm);
  const WorldPosition player_position{
      8000, static_cast<int>(monster_position.at(0).get<int>() + engagement_mm / 2),
      monster_position.at(1).get<int>(), monster_position.at(2).get<int>(), 180.0F};

  auto stats = player_stats();
  stats["18"] = stats["69"] = 1000000;
  TutorialSession tutorial(fight.tutorial_data, 7, {},
                           [](const auto &, const auto &) { return true; });
  CombatSession combat(data, fight.tutorial_data, character(kWildCharacterClass), stats);
  Json request{{"start_skill",
                {{"skill_id", kPlayerBasicAttack},
                 {"instance_id", 1},
                 {"pos",
                  {{"millimeter_x", player_position.x_mm},
                   {"millimeter_y", player_position.y_mm},
                   {"millimeter_z", player_position.z_mm}}},
                 {"rotation", {{"yaw", 180.0}}},
                 {"tick", "999999999999"}}},
               {"target_ids", {fight.monster_text}}};
  const auto started = combat.receive(20010, request, player_position, kStart);
  EXPECT_FALSE(started.empty());

  for (std::int64_t elapsed_ms = 50; elapsed_ms <= duration_ms; elapsed_ms += 50) {
    auto step = combat.advance(kStart + elapsed_ms * kMillisecond, player_position, tutorial);
    fight.packets.insert(fight.packets.end(), step.begin(), step.end());
  }
  return fight;
}

// DamageNoti count (opcode 19) that each cast instance of the monster landed on the player.
std::map<std::uint32_t, int> monster_hits_per_cast(const MonsterFight &fight) {
  std::map<std::uint32_t, int> hits_per_cast;
  for (const auto &packet : fight.packets) {
    if (packet.opcode == 19 &&
        packet.message.at("caster_unit_id").get<std::string>() == fight.monster_text) {
      ++hits_per_cast[packet.message.at("skill_instance_id").get<std::uint32_t>()];
    }
  }
  return hits_per_cast;
}

void expect_every_cast_of_profile_monster_lands_one_hit(int monster_cms_id, std::size_t min_casts) {
  const char *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS to the overlap-group profile";
  }
  const auto fight = fight_profile_monster(path, monster_cms_id, 30000);
  const auto &effects = fight.tutorial_data->document.at("combat").at("effects");
  for (const auto &skill_id :
       fight.tutorial_data->document.at("combat").at("npc_skills").at(std::to_string(monster_cms_id))) {
    if (!effects.contains(std::to_string(skill_id.get<int>()))) {
      continue;
    }
    EXPECT_FALSE(effects.at(std::to_string(skill_id.get<int>())).value("overlap_group", "").empty())
        << "effect " << skill_id << " has no overlap_group: not the augmented profile";
  }

  const auto hits_per_cast = monster_hits_per_cast(fight);
  EXPECT_GE(hits_per_cast.size(), min_casts);
  for (const auto &[instance, hits] : hits_per_cast) {
    EXPECT_EQ(hits, 1) << "cast instance " << instance << " landed " << hits << " hits";
  }
  verify_encoding(fight.packets);
}

TEST(Combat, ProfileMonster5001083LandsOneGroupedHitPerCast) {
  expect_every_cast_of_profile_monster_lands_one_hit(5001083, 1);
}

// Two or more separate casts are independent: each one lands its own single hit.
TEST(Combat, ProfileMonster5001084LandsOneGroupedHitPerCastAcrossIndependentCasts) {
  expect_every_cast_of_profile_monster_lands_one_hit(5001084, 2);
}

// Cast A hits with a grouped effect and launches a projectile whose hit effect shares the group
// (a different effect id). Cast B starts and lands its own grouped hit before A's projectile
// arrives. Suppression is keyed by (caster, cast): A's late projectile must not re-hit, B must land
// exactly once per target, and A's state must survive B's events.
TEST(Combat, OverlapSuppressionOfAnOlderCastSurvivesALaterCastAndItsLateProjectile) {
  auto definitions = combat_fixture();
  auto &damage = definitions["effects"]["1"];
  damage["parameters"]["physicalCoefficient"] = 0.1;
  damage["overlap_group"] = "G";
  definitions["effects"]["2"] = damage;
  definitions["effects"]["4"] = damage;
  definitions["effects"]["4"]["kind"] = "spawnProjectile";
  definitions["effects"]["4"]["parameters"] = {{"projectileId", 3}};
  definitions["effects"]["4"].erase("overlap_group");
  definitions["projectiles"]["3"] = {
      {"duration_ms", 2000}, {"parameters", {{"speed", 100}}}, {"hit_effects", {2}}};
  auto &skill = definitions["skills"][0];
  skill["cooldown_ms"] = 0;
  skill["layers"][0]["stages"][0]["events"] =
      Json::array({{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 100}},
                   {{"effect_id", 4}, {"unique_id", 1}, {"time_ms", 200}}});
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  int damage_notifications = 0;
  const auto step = [&](std::int64_t offset_ms) {
    for (const auto &packet : combat.advance(kStart + offset_ms * kMillisecond, position, tutorial)) {
      damage_notifications += packet.opcode == 19 ? 1 : 0;
    }
  };

  ASSERT_EQ(combat.receive(20010, start_request(1), position, kStart).size(), 2u);
  step(100);   // A hits once (group G).
  step(200);   // A launches its projectile (flight 1000 ms).
  step(1000);  // A's action is over; its projectile is still in flight.
  ASSERT_EQ(combat.receive(20010, start_request(2), position, kStart + 1050 * kMillisecond).size(),
            2u);
  step(1150);  // B hits once (its own group state, not A's).
  EXPECT_EQ(damage_notifications, 2);
  step(1200);  // A's late projectile arrives: the group was already consumed by cast A.
  EXPECT_EQ(damage_notifications, 2);
  step(1250);  // B launches its projectile.
  step(2300);  // B's projectile arrives: suppressed too.
  step(3000);
  EXPECT_EQ(damage_notifications, 2);
  EXPECT_EQ(combat.monster_hp(kMonster), 38);
}

// Only an event that actually hits consumes the group: an earlier scheduled event whose shape does
// not reach the target leaves it available for a later event that does.
TEST(Combat, OverlapGroupIsConsumedOnlyByAnEventThatReachesTheTarget) {
  auto definitions = combat_fixture();
  auto &near_miss = definitions["effects"]["1"];
  near_miss["parameters"]["physicalCoefficient"] = 0.1;
  near_miss["overlap_group"] = "G";
  near_miss["targeting"]["outerRadius"] = 50;
  definitions["effects"]["2"] = near_miss;
  definitions["effects"]["2"]["targeting"]["outerRadius"] = 300;
  definitions["skills"][0]["layers"][0]["stages"][0]["events"] =
      Json::array({{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 100}},
                   {{"effect_id", 2}, {"unique_id", 1}, {"time_ms", 200}},
                   {{"effect_id", 2}, {"unique_id", 2}, {"time_ms", 300}}});
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);
  int damage_notifications = 0;
  for (const auto offset_ms : {100, 200, 300, 400}) {
    for (const auto &packet : combat.advance(kStart + offset_ms * kMillisecond, position, tutorial)) {
      damage_notifications += packet.opcode == 19 ? 1 : 0;
    }
  }
  EXPECT_EQ(damage_notifications, 1);
  EXPECT_EQ(combat.monster_hp(kMonster), 44);
}

// ---- Catalog-driven NPC skill selection (5001010-like skills on the fixture monster). ----

// Which same-band skill wins is unknown and never pinned here: every fixture case has exactly
// one eligible skill (only band, cooldown and type are asserted). 301 melee [0, 3000] mm cooldown
// 0; 302 ranged [5000, 20000] mm cooldown 7000; 303 Dodge. The monster lists them with the Dodge
// and the ranged skill first, so neither "first listed" nor "first sorted" can pass.
Json npc_selection_fixture(const Json &npc_skill_ids) {
  auto definitions = combat_fixture_with_npc_attack();
  const Json template_skill = definitions.at("skills").at(1);
  const auto add_skill = [&](int id, const char *type, int min_range_mm, int range_mm,
                             int cooldown_ms) {
    Json skill = template_skill;
    skill["id"] = id;
    skill["type"] = type;
    skill["min_range_mm"] = min_range_mm;
    skill["range_mm"] = range_mm;
    skill["cooldown_ms"] = cooldown_ms;
    definitions["skills"].push_back(skill);
  };
  add_skill(301, "Normal", 0, 3000, 0);
  add_skill(302, "Normal", 5000, 20000, 7000);
  add_skill(303, "Dodge", 0, 5000, 0);
  // 302 is the ranged attack: a 25 m ranged-distance shape covers its whole 5..20 m band, so the
  // melee reach gate never shortens it.
  Json ranged_effect = definitions.at("effects").at("1");
  ranged_effect["targeting"]["outerRadius"] = 2500;
  ranged_effect["parameters"]["distanceType"] = "EX5SkillEffectDistanceType_Ranged";
  definitions["effects"]["302"] = ranged_effect;
  for (auto &skill : definitions["skills"]) {
    if (skill.at("id") == 302) {
      skill["layers"][0]["stages"][0]["events"][0]["effect_id"] = 302;
    }
  }
  definitions["npc_skills"] = {{"5001015", npc_skill_ids}, {"5001528", npc_skill_ids}};
  return definitions;
}

struct NpcCast {
  int skill_id{};
  std::int64_t tick{};
};

// Advances the session in 100 ms steps and returns every monster StartSkill (opcode 14).
std::vector<NpcCast> collect_npc_casts(const Json &definitions, const WorldPosition &position,
                                       std::int64_t duration_ms) {
  auto block = npc_ai_block();
  block["search_cone_range_mm"] = 30000;
  const auto tutorial_data = tutorial_fixture_with_ai(block);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  std::vector<NpcCast> casts;
  for (std::int64_t elapsed = 0; elapsed <= duration_ms; elapsed += 100) {
    const auto tick = kStart + elapsed * kMillisecond;
    for (const auto &packet : combat.advance(tick, position, tutorial)) {
      if (packet.opcode == 14 && packet.message.at("unit_id") != "7") {
        casts.push_back({packet.message.at("start_skill").at("skill_id"), tick});
      }
    }
  }
  return casts;
}

TEST(Combat, NpcSelectsTheMeleeSkillInsideItsCatalogBandNotTheFirstListedSkill) {
  const auto casts = collect_npc_casts(npc_selection_fixture({303, 302, 301}),
                                       WorldPosition{8000, 3000, 0, 0}, 3000);
  ASSERT_FALSE(casts.empty());
  for (const auto &cast : casts) {
    EXPECT_EQ(cast.skill_id, 301);
  }
}

// Selection by band and cooldown at unit level, without any NPC movement: the ranged skill is
// eligible at 10 m, the melee skill is not, and a skill in cooldown is skipped until it ends.
TEST(Combat, NpcSelectionUsesCatalogBandsAndPerSkillCooldowns) {
  const CombatData data(npc_selection_fixture({303, 302, 301}));
  const std::map<int, std::int64_t> no_cooldowns;
  EXPECT_EQ(select_npc_attack_skill(data, 5001015, no_cooldowns, 2000, 0), 301);
  EXPECT_EQ(select_npc_attack_skill(data, 5001015, no_cooldowns, 10000, 0), 302);
  EXPECT_FALSE(select_npc_attack_skill(data, 5001015, no_cooldowns, 4000, 0).has_value());
  EXPECT_FALSE(select_npc_attack_skill(data, 5001015, no_cooldowns, 25000, 0).has_value());
  const std::map<int, std::int64_t> ranged_cooling{{302, 7000 * kMillisecond}};
  EXPECT_FALSE(select_npc_attack_skill(data, 5001015, ranged_cooling, 10000, 6000 * kMillisecond).has_value());
  EXPECT_EQ(select_npc_attack_skill(data, 5001015, ranged_cooling, 10000, 7000 * kMillisecond),
            302);
  // The melee skill has no cooldown of its own: a cooling ranged skill does not block it.
  EXPECT_EQ(select_npc_attack_skill(data, 5001015, ranged_cooling, 2000, 0), 301);
}

TEST(Combat, RangedOnlyNpcCreatesDistanceBeforeCastingWithinItsCatalogBand) {
  const auto casts = collect_npc_casts(npc_selection_fixture({302}),
                                       WorldPosition{8000, 3000, 0, 0}, 5000);
  ASSERT_FALSE(casts.empty());
  for (const auto &cast : casts) {
    EXPECT_EQ(cast.skill_id, 302);
  }
}

TEST(Combat, EligibleNpcAttacksRotateAndNormalMovementEffectsAreNotAttacks) {
  auto definitions = npc_selection_fixture({301, 304, 305});
  auto alternative = definitions["skills"][2];
  alternative["id"] = 304;
  definitions["skills"].push_back(alternative);
  auto movement = alternative;
  movement["id"] = 305;
  movement["layers"][0]["stages"][0]["events"][0]["effect_id"] = 99;
  definitions["effects"]["99"] = definitions["effects"]["1"];
  definitions["effects"]["99"]["kind"] = "move";
  definitions["skills"].push_back(movement);
  const CombatData data(definitions);
  EXPECT_EQ(select_npc_attack_skill(data, 5001015, {}, 1000, kStart), 301);
  EXPECT_EQ(select_npc_attack_skill(data, 5001015, {}, 1000, kStart, {{301, kStart}}), 304);
  EXPECT_EQ(select_npc_attack_skill(data, 5001015, {}, 1000, kStart + 1,
                                    {{301, kStart}, {304, kStart + 1}}),
            301);
  const auto casts = collect_npc_casts(definitions, WorldPosition{8000, 0, 0, 0}, 3500);
  ASSERT_GE(casts.size(), 2u);
  EXPECT_EQ(casts[0].skill_id, 301);
  EXPECT_EQ(casts[1].skill_id, 304);
  EXPECT_LT(casts[1].tick - casts[0].tick, 3000 * kMillisecond);
}

TEST(Combat, NpcNeverCastsADodgeSkillAsAnAttack) {
  const auto casts = collect_npc_casts(npc_selection_fixture({303}),
                                       WorldPosition{8000, 3000, 0, 0}, 5000);
  EXPECT_TRUE(casts.empty());
}

// ---- Real-profile selection (5001010) and profile 87c4051c vs 5dc7258b comparison. ----
// Run explicitly: CHRONICLES_TUTORIAL_TEST_PROFILE = definitions.json (87c4051c) and
// CHRONICLES_COMBAT_PROFILE_V4 = the regenerated combat profile (5dc7258b).

std::optional<Json> load_json_file(const char *path) {
  std::ifstream stream(path);
  if (!stream) {
    return std::nullopt;
  }
  return Json::parse(stream);
}

struct SpawnProbe {
  std::shared_ptr<TutorialData> tutorial;
  std::string unit_id;
  WorldPosition monster;
};

// The real definitions with every spawn but one left as they are and the probe's perception widened
// so the player is always noticed; nothing else of the profile is changed.
std::optional<SpawnProbe> make_spawn_probe(const Json &definitions, int cms_id) {
  Json document = definitions;
  std::optional<SpawnProbe> probe;
  for (auto &npc : document.at("npc_spawns")) {
    const bool target = !probe && npc.at("cms_id") == cms_id;
    if (!target) {
      continue;
    }
    npc["initially_active"] = true;
    npc["ai"]["search_circle_range_mm"] = 30000;
    npc["ai"]["search_cone_range_mm"] = 30000;
    npc["ai"]["search_half_cone_angle"] = 180.0;
    const auto &position = npc.at("position");
    probe = SpawnProbe{nullptr,
                       npc.at("unit_id").get<std::string>(),
                       WorldPosition{document.at("map_id").get<int>(), position.at(0).get<int>(),
                                     position.at(1).get<int>(), position.at(2).get<int>(), 0.0F}};
  }
  if (probe) {
    probe->tutorial = std::make_shared<TutorialData>(document);
  }
  return probe;
}

struct Position3 {
  int x{};
  int y{};
  int z{};
  bool operator==(const Position3 &) const = default;
};

// One announced NPC attack (opcode 14): its instance, skill, wire tick and caster pose.
struct AttackRecord {
  std::uint64_t instance{};
  int skill_id{};
  std::int64_t tick{};
  Position3 position;
};

// One opcode 64 root motion with the skill instance it carries.
struct MotionRecord {
  std::uint64_t instance{};
  Position3 start;
  Position3 destination;
};

struct ProbeResult {
  std::vector<NpcCast> casts;
  std::vector<std::pair<std::int64_t, std::int64_t>> destinations;
  std::vector<AttackRecord> attacks;
  std::vector<MotionRecord> motions;
  Position3 player_position;
  std::vector<int> projectiles;
  std::vector<int> hit_effects;
  std::vector<std::string> hit_targets;
  std::vector<int> hit_damages;
};

ProbeResult probe_npc(const Json &definitions, const Json &combat_profile, int cms_id,
                      int distance_mm, std::int64_t duration_ms) {
  const auto probe = make_spawn_probe(definitions, cms_id);
  EXPECT_TRUE(probe.has_value()) << cms_id;
  ProbeResult result;
  if (!probe) {
    return result;
  }
  auto stats = player_stats();
  stats["18"] = 1000000000;
  stats["69"] = 1000000000;
  TutorialSession tutorial(probe->tutorial, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_profile), probe->tutorial, character(),
                       stats);
  auto player = probe->monster;
  player.x_mm += distance_mm;
  result.player_position = {player.x_mm, player.y_mm, player.z_mm};
  for (std::int64_t elapsed = 0; elapsed <= duration_ms; elapsed += 100) {
    const auto tick = kStart + elapsed * kMillisecond;
    for (const auto &packet : combat.advance(tick, player, tutorial)) {
      if (packet.message.value("caster_unit_id", std::string()) == probe->unit_id) {
        if (packet.opcode == 40) {
          result.projectiles.push_back(packet.message.at("spawn_projectile").at("projectile_cms_id"));
        } else if (packet.opcode == 19) {
          result.hit_effects.push_back(packet.message.at("skill_effect_cms_id"));
          result.hit_targets.push_back(packet.message.at("target_unit_id"));
          result.hit_damages.push_back(packet.message.at("final_damage_amount"));
        }
      }
      if (packet.message.value("unit_id", std::string()) != probe->unit_id) {
        continue;
      }
      if (packet.opcode == 14) {
        const auto &start = packet.message.at("start_skill");
        result.casts.push_back({start.at("skill_id"), tick});
        const auto &pos = start.at("pos");
        result.attacks.push_back({start.at("instance_id").get<std::uint64_t>(),
                                  start.at("skill_id").get<int>(),
                                  tick,
                                  {pos.at("millimeter_x"), pos.at("millimeter_y"),
                                   pos.at("millimeter_z")}});
      } else if (packet.opcode == 64) {
        const auto &motion = packet.message.at("root_motion");
        const auto &destination = motion.at("dest_pos");
        const auto &origin = motion.at("start_pos");
        result.destinations.emplace_back(destination.at("millimeter_x"),
                                         destination.at("millimeter_y"));
        result.motions.push_back({motion.at("skill_instance_id").get<std::uint64_t>(),
                                  {origin.at("millimeter_x"), origin.at("millimeter_y"),
                                   origin.at("millimeter_z")},
                                  {destination.at("millimeter_x"), destination.at("millimeter_y"),
                                   destination.at("millimeter_z")}});
      }
    }
  }
  return result;
}

TEST(Combat, CurrentExtractedNpcRepertoiresRespectRangesAndCooldowns) {
  const char *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS for the same-build generated profile";
  }
  const auto definitions = load_json_file(path);
  ASSERT_TRUE(definitions);
  const auto &profile = definitions->at("combat");
  const CombatData data(profile);
  for (const int cms_id : {5001010, 5001016, 5001527, 5001530}) {
    for (const int distance_mm : {2500, 10000, 15000}) {
      SCOPED_TRACE(::testing::Message() << "npc=" << cms_id << " distance_mm=" << distance_mm);
      const auto result = probe_npc(*definitions, profile, cms_id, distance_mm, 12000);
      ASSERT_FALSE(result.attacks.empty());
      std::map<int, std::int64_t> previous_cast;
      for (const auto &attack : result.attacks) {
        const auto &skill = data.skills.at(attack.skill_id);
        const double dx = static_cast<double>(result.player_position.x) - attack.position.x;
        const double dy = static_cast<double>(result.player_position.y) - attack.position.y;
        const double dz = static_cast<double>(result.player_position.z) - attack.position.z;
        const double range = std::sqrt(dx * dx + dy * dy + dz * dz);
        EXPECT_GE(range + kNpcPursuitSettleToleranceMm, skill.value("min_range_mm", 0));
        EXPECT_LE(range, skill.at("range_mm").get<double>() + kNpcPursuitSettleToleranceMm);
        const auto previous = previous_cast.find(attack.skill_id);
        if (previous != previous_cast.end()) {
          EXPECT_GE(attack.tick - previous->second,
                    skill.at("cooldown_ms").get<std::int64_t>() * kMillisecond);
        }
        previous_cast[attack.skill_id] = attack.tick;
      }
    }
  }
}

TEST(Combat, RealProfileNpc5001010UsesItsCatalogBandsAndCooldown) {
  const auto *definitions_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *profile_path = std::getenv("CHRONICLES_COMBAT_PROFILE_V4");
  if (!definitions_path || !profile_path) {
    GTEST_SKIP() << "Run explicitly against the 87c4051c definitions and the v4 combat profile";
  }
  const auto definitions = load_json_file(definitions_path);
  const auto profile = load_json_file(profile_path);
  ASSERT_TRUE(definitions && profile);

  // Catalog: 190100400 melee [0, 3000] mm, 190100100 ranged [5000, 20000] mm cooldown 7000 ms.
  // End to end the NPC walks to its melee reach and casts the melee skill.
  const auto near_casts = probe_npc(*definitions, *profile, 5001010, 2500, 6000);
  ASSERT_FALSE(near_casts.casts.empty());
  for (const auto &cast : near_casts.casts) {
    EXPECT_EQ(cast.skill_id, 190100400);
  }

  // Selection by band and cooldown, without any movement: the ranged skill is eligible at
  // 10 m, the melee skill is not, and the 3000-5000 mm gap has no eligible skill.
  const CombatData data(*profile);
  const std::map<int, std::int64_t> no_cooldowns;
  EXPECT_EQ(select_npc_attack_skill(data, 5001010, no_cooldowns, 2500, 0), 190100400);
  EXPECT_EQ(select_npc_attack_skill(data, 5001010, no_cooldowns, 10000, 0), 190100100);
  EXPECT_FALSE(select_npc_attack_skill(data, 5001010, no_cooldowns, 4000, 0).has_value());
  const std::map<int, std::int64_t> ranged_cooling{{190100100, 7000 * kMillisecond}};
  EXPECT_FALSE(select_npc_attack_skill(data, 5001010, ranged_cooling, 10000, 6999 * kMillisecond).has_value());
  EXPECT_EQ(select_npc_attack_skill(data, 5001010, ranged_cooling, 10000, 7000 * kMillisecond),
            190100100);
}

// End to end: the probe NPC 5001010 stands 10 m from the player. Its ranged skill 190100100
// (catalog band [5000, 20000] mm) must be cast by the real CombatSession, spawn projectile
// 190100101. Only ONE ranged cast happens in this arrangement (the NPC then walks to melee
// reach), so the 7000 ms cooldown is proven at unit level (NpcSelection... and the 5001010
// selection test), not end to end; a second cast here would be only conditionally checked.
TEST(Combat, RealProfileNpc5001010CastsItsRangedSkillAtTenMetresEndToEnd) {
  const auto *definitions_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *profile_path = std::getenv("CHRONICLES_COMBAT_PROFILE_V4");
  if (!definitions_path || !profile_path) {
    GTEST_SKIP() << "Run explicitly against the 87c4051c definitions and the v4 combat profile";
  }
  const auto definitions = load_json_file(definitions_path);
  const auto profile = load_json_file(profile_path);
  ASSERT_TRUE(definitions && profile);

  const auto result = probe_npc(*definitions, *profile, 5001010, 10000, 20000);
  const auto ranged_casts = std::ranges::count_if(
      result.casts, [](const NpcCast &cast) { return cast.skill_id == 190100100; });
  ASSERT_GE(ranged_casts, 1);
  ASSERT_FALSE(result.casts.empty());
  EXPECT_EQ(result.casts.front().skill_id, 190100100);
  EXPECT_NE(std::ranges::find(result.projectiles, 190100100), result.projectiles.end());
  const auto hit = std::ranges::find(result.hit_effects, 190100101);
  ASSERT_NE(hit, result.hit_effects.end());
  const auto hit_index = hit - result.hit_effects.begin();
  EXPECT_EQ(result.hit_targets[hit_index], std::to_string(character().id));
  EXPECT_GT(result.hit_damages[hit_index], 0);
  const auto first_ranged = std::ranges::find_if(
      result.casts, [](const NpcCast &cast) { return cast.skill_id == 190100100; });
  const auto second_ranged = std::find_if(
      first_ranged + 1, result.casts.end(),
      [](const NpcCast &cast) { return cast.skill_id == 190100100; });
  if (second_ranged != result.casts.end()) {
    EXPECT_GE((second_ranged->tick - first_ranged->tick) / kMillisecond, 7000);
  }
}

TEST(Combat, RealProfileNpc5001530NeverCastsItsDodgeSkillAsAnAttack) {
  const auto *definitions_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *profile_path = std::getenv("CHRONICLES_COMBAT_PROFILE_V4");
  if (!definitions_path || !profile_path) {
    GTEST_SKIP() << "Run explicitly against the 87c4051c definitions and the v4 combat profile";
  }
  const auto definitions = load_json_file(definitions_path);
  const auto profile = load_json_file(profile_path);
  ASSERT_TRUE(definitions && profile);
  for (const int distance_mm : {1000, 3700, 8000, 12000}) {
    const auto result = probe_npc(*definitions, *profile, 5001530, distance_mm, 12000);
    for (const auto &cast : result.casts) {
      EXPECT_NE(cast.skill_id, 220110700) << distance_mm;
    }
  }
}

// All 11 baseline NPCs under the deployed combat section and the regenerated one, at 2, 5 and
// 10 m over 12 s: casts, cast ticks and root-motion destinations must be IDENTICAL, except for
// the named, justified exceptions below.
//   5001016 at 2000 mm: its only skill 190700100 has a catalog minimum distance of 3000 mm
//   (Skill.minDistance, same x10 conversion as the maximum). The deployed profile had no
//   minimum, so it cast at 2 m; the regenerated one casts nothing there. Official evidence
//   neither contradicts nor proves this (the two captured casts, 3.5 m and 11.2 m, are both
//   above the minimum; the capture distances are noisy player positions), and what an NPC
//   does inside its minimum distance is unknown, so no fallback is guessed.
//   5001010, 5001528 and 5001530 at 10000 mm: each lists a ranged skill whose catalog band
//   starts beyond the pursuit stop distance (190100100 [5000, 20000], 234730400 [4000, 8000],
//   5000020 [5000, 20000]). The deployed profile could only cast after the NPC had walked to
//   its melee reach; the regenerated one casts the ranged skill as soon as the player is inside
//   its band (for 234730400 while closing in). Official casts from those distances exist:
//   190100100 at 7.5 m (full-tutorial record 494), 234730400 at 8.6 m (2296), 5000020 at
//   10.6 m (2135); the capture distances are noisy player positions. No movement rule changes.
struct NamedException {
  int first_cast_skill;  // 0 = the regenerated profile casts nothing
};

std::optional<NamedException> named_baseline_exception(int cms_id, int distance_mm) {
  if (cms_id == 5001016 && distance_mm == 2000) {
    return NamedException{0};
  }
  if (distance_mm >= 10000 && cms_id == 5001010) {
    return NamedException{190100100};
  }
  // 5001527 (boss list of 13): 230241400 [5000, 12000] (official casts at 7.2-12.1 m, six
  // captured; 230241500 [3000, 8000] at 4.8-8.8 m) is now cast at 10 m before the NPC has walked
  // to melee reach, where the deployed profile first cast the melee 230240100 (official 1.2-3.6
  // m). Skills with min 0 and a long maximum (for example 230240700 [0, 20000], official
  // 6.7-9.0 m) are STILL held back by the unchanged engagement gate: a remaining gap.
  if (distance_mm >= 10000 && cms_id == 5001527) {
    return NamedException{230241400};
  }
  if (distance_mm >= 10000 && cms_id == 5001528) {
    return NamedException{234730400};
  }
  if (distance_mm >= 10000 && cms_id == 5001530) {
    return NamedException{5000020};
  }
  return std::nullopt;
}

TEST(Combat, RegeneratedProfileKeepsEveryBaselineNpcBehaviourExceptNamedExceptions) {
  // Candidate: definitions 0d677942 (CHRONICLES_TUTORIAL_TEST_PROFILE) whose combat section is
  // the regenerated profile 5dc7258b (CHRONICLES_COMBAT_PROFILE_V4). Baseline: the immutable
  // deployed definitions 87c4051c, loaded separately (CHRONICLES_BASELINE_DEFINITIONS). Every
  // file is hash-checked before it is trusted, so "before" can never silently equal "after".
  const auto *definitions_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *profile_path = std::getenv("CHRONICLES_COMBAT_PROFILE_V4");
  const auto *baseline_path = std::getenv("CHRONICLES_BASELINE_DEFINITIONS");
  if (!definitions_path || !profile_path || !baseline_path) {
    GTEST_SKIP() << "Run explicitly with the candidate definitions, the candidate combat profile "
                    "and the baseline 87c4051c definitions";
  }
  ASSERT_EQ(distribution::sha256_file(baseline_path),
            "87c4051c331353f590892392ab0bb651f0685f6eb34cabfec94f3b1c55368459");
  ASSERT_EQ(distribution::sha256_file(definitions_path),
            "0d677942b7d1cf5d3836b0b5720ffce122f0f5de59277ce9c7205cc6cdfadf4b");
  ASSERT_EQ(distribution::sha256_file(profile_path),
            "5dc7258b4dc821ac1ab1ef2fff8f0f6b589e9a33ffaaaf0ad9ab003549dfb679");
  const auto definitions = load_json_file(definitions_path);
  const auto baseline_definitions = load_json_file(baseline_path);
  const auto regenerated = load_json_file(profile_path);
  ASSERT_TRUE(definitions && baseline_definitions && regenerated);
  EXPECT_EQ(definitions->at("combat"), *regenerated);
  const auto &deployed = baseline_definitions->at("combat");
  EXPECT_NE(deployed, *regenerated);
  EXPECT_NO_THROW(CombatData{deployed});
  EXPECT_NO_THROW(CombatData{*regenerated});

  ASSERT_EQ(regenerated->at("npc_skills").size(), 11u);
  for (const auto &item : regenerated->at("npc_skills").items()) {
    const int cms_id = std::stoi(item.key());
    // 4000/7000 mm lie inside the ranged bands of 5001528 [4000, 8000] and 5001527
    // [3000, 8000] / [5000, 12000].
    for (const int distance_mm : {2000, 4000, 5000, 7000, 10000, 15000}) {
      SCOPED_TRACE("npc " + item.key() + " at " + std::to_string(distance_mm) + " mm");
      const auto before = probe_npc(*definitions, deployed, cms_id, distance_mm, 12000);
      const auto after = probe_npc(*definitions, *regenerated, cms_id, distance_mm, 12000);
      const auto exception = named_baseline_exception(cms_id, distance_mm);
      if (exception) {
        EXPECT_FALSE(before.casts.empty());
        if (exception->first_cast_skill == 0) {
          EXPECT_TRUE(after.casts.empty());
        } else {
          ASSERT_FALSE(after.casts.empty());
          EXPECT_EQ(after.casts.front().skill_id, exception->first_cast_skill);
          EXPECT_NE(before.casts.front().skill_id, exception->first_cast_skill);
        }
        continue;
      }
      ASSERT_EQ(before.casts.size(), after.casts.size());
      for (std::size_t index = 0; index < before.casts.size(); ++index) {
        EXPECT_EQ(before.casts[index].skill_id, after.casts[index].skill_id);
        EXPECT_EQ(before.casts[index].tick, after.casts[index].tick);
      }
      EXPECT_EQ(before.destinations, after.destinations);
    }
  }
}

// The catalog minimum distance is a hard eligibility bound (see the comparison test above for
// the evidence limits): below it the only skill is not eligible, above it it is cast.
TEST(Combat, RealProfileNpc5001016HonoursItsCatalogMinimumDistance) {
  const auto *definitions_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *profile_path = std::getenv("CHRONICLES_COMBAT_PROFILE_V4");
  if (!definitions_path || !profile_path) {
    GTEST_SKIP() << "Run explicitly against the 87c4051c definitions and the v4 combat profile";
  }
  const auto definitions = load_json_file(definitions_path);
  const auto profile = load_json_file(profile_path);
  ASSERT_TRUE(definitions && profile);
  EXPECT_TRUE(probe_npc(*definitions, *profile, 5001016, 2000, 8000).casts.empty());
  const auto above = probe_npc(*definitions, *profile, 5001016, 4000, 8000);
  ASSERT_FALSE(above.casts.empty());
  for (const auto &cast : above.casts) {
    EXPECT_EQ(cast.skill_id, 190700100);
  }
}

// ---- NPC PS2C_MoveNoti (177) during server-side pursuit (stage 1 design tests). ----
// The client moves a non-local unit only from 177 frames (builder-2 addenda 22-24, provisional
// contract): unit_id, mode MoveMode_Normal, detail MoveDetail_Run while moving and
// MoveDetail_Stop at the end, pos in mm, body_yaw and tick. These tests fail on f4e8e0ca, which
// sends no 177 at all for NPCs.

std::vector<const GameplayPacket *> move_notifications(const std::vector<GameplayPacket> &packets,
                                                       const std::string &unit_id) {
  std::vector<const GameplayPacket *> found;
  for (const auto &packet : packets) {
    if (packet.opcode == 177 && packet.message.value("unit_id", std::string()) == unit_id) {
      found.push_back(&packet);
    }
  }
  return found;
}

TEST(Combat, NpcPursuitAnnouncesItsRouteStartWithAMoveNotification) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()),
                       tutorial_data, character(), player_stats());
  const WorldPosition player{8000, 6000, 0, 0};

  const auto acquired = combat.advance(kStart, player, tutorial);
  const auto *route = find_packet(acquired, 177);
  ASSERT_NE(route, nullptr);
  const auto unit_id = route->message.at("unit_id").get<std::string>();
  const auto moves = move_notifications(acquired, unit_id);
  ASSERT_EQ(moves.size(), 1u);
  const auto &move = moves.front()->message;
  EXPECT_EQ(move.at("mode"), "MoveMode_Normal");
  EXPECT_EQ(move.at("detail"), "MoveDetail_Run");
  EXPECT_EQ(move.at("pos").at("millimeter_x"), 1000);
  EXPECT_EQ(move.at("tick"), std::to_string(kStart * 100));
  EXPECT_GE(move.at("pred_milli").get<int>(), 16);
  // The goal is the authoritative route position pred_milli ahead: pred_milli is the time to move
  // the 600 mm step at 3000 mm/s (200 ms), so 600 mm along x from the spawn, never the player.
  ASSERT_TRUE(move.contains("pred_pos"));
  EXPECT_EQ(move.at("pred_milli").get<int>(), 200);
  EXPECT_NEAR(move.at("pred_pos").at("millimeter_x").get<double>(), 1600.0, 2.0);
  EXPECT_TRUE(move.contains("body_yaw"));
  verify_encoding(acquired);
}

// Diagnostic only: with CHRONICLES_DUMP_MOVE_FRAMES set, prints every 177 frame of a movement test
// (tick in ns, position, goal, pred_milli and the speed |dpos| / dtick from the previous frame) so
// the per-pair speeds are visible in the test log. It asserts nothing.
void dump_move_frames(const char *label, const std::vector<const GameplayPacket *> &moves) {
  if (!std::getenv("CHRONICLES_DUMP_MOVE_FRAMES")) {
    return;
  }
  const nlohmann::json *previous = nullptr;
  for (std::size_t index = 0; index < moves.size(); ++index) {
    const auto &message = moves[index]->message;
    const auto tick = std::stoll(message.at("tick").get<std::string>());
    const auto &pos = message.at("pos");
    double speed = 0.0;
    if (previous) {
      const auto previous_tick = std::stoll(previous->at("tick").get<std::string>());
      const auto &before = previous->at("pos");
      const double dx = pos.at("millimeter_x").get<double>() - before.at("millimeter_x").get<double>();
      const double dy = pos.at("millimeter_y").get<double>() - before.at("millimeter_y").get<double>();
      const double dz = pos.at("millimeter_z").get<double>() - before.at("millimeter_z").get<double>();
      const double seconds = static_cast<double>(tick - previous_tick) / 1e9;
      speed = seconds > 0 ? std::sqrt(dx * dx + dy * dy + dz * dz) / seconds : 0.0;
    }
    std::cout << "MOVEFRAME " << label << " #" << index << " " << message.at("detail").get<std::string>()
              << " tick_ns=" << tick << " x=" << pos.at("millimeter_x") << " y=" << pos.at("millimeter_y")
              << " z=" << pos.at("millimeter_z") << " pred_milli=" << message.value("pred_milli", -1)
              << " goal_x=" << (message.contains("pred_pos") ? message.at("pred_pos").at("millimeter_x").dump() : "-")
              << " speed_from_previous_mm_s=" << speed << "\n";
    previous = &message;
  }
}

// One continuous Run interval: two consecutive Run frames of the same route stream with no Stop
// between them. The client derives its speed from |dpos| / dtick, so between them the emitted
// positions must follow the route speed (3000 mm/s in the fixture, the unchanged AI speed), the
// ticks must strictly increase in nanoseconds, the next frame must arrive inside the client's 1 s
// blend window and a step the client would snap (<= 500 mm) is only allowed when the 900 ms cap
// forced the frame.
void expect_continuous_walk_pair(const nlohmann::json &previous, const nlohmann::json &current) {
  const auto tick = std::stoll(current.at("tick").get<std::string>());
  const auto previous_tick = std::stoll(previous.at("tick").get<std::string>());
  ASSERT_GT(tick, previous_tick) << "ticks strictly increase";
  const double seconds = static_cast<double>(tick - previous_tick) / 1e9;
  const auto coordinate = [](const nlohmann::json &message, const char *axis) {
    return message.at("pos").at(axis).get<double>();
  };
  const double dx = coordinate(current, "millimeter_x") - coordinate(previous, "millimeter_x");
  const double dy = coordinate(current, "millimeter_y") - coordinate(previous, "millimeter_y");
  const double dz = coordinate(current, "millimeter_z") - coordinate(previous, "millimeter_z");
  const double step_mm = std::sqrt(dx * dx + dy * dy + dz * dz);
  EXPECT_LT(seconds, 1.0) << "the next frame arrives inside the client's 1 s blend window";
  EXPECT_TRUE(step_mm > 500.0 || seconds * 1000.0 >= 900.0)
      << "a step the client would snap (<= 500 mm) is only allowed when the 900 ms cap hit";
  EXPECT_NEAR(step_mm / seconds, 3000.0, 90.0) << "no position jumps";
}

void expect_running_shape(const nlohmann::json &message) {
  EXPECT_EQ(message.at("mode"), "MoveMode_Normal");
  EXPECT_GE(message.value("pred_milli", 0), 16);
  EXPECT_LE(message.value("pred_milli", 0), 900);
  ASSERT_TRUE(message.contains("pred_pos"));
  EXPECT_TRUE(message.at("pred_pos").contains("millimeter_z")) << "the goal carries z";
  EXPECT_FALSE(message.contains("velocity")) << "the client ignores velocity; pred_pos is the goal";
}

// The target walks away faster (4 m/s) than the monster runs (3 m/s): the monster never reaches
// it, so there is no Stop and every Run frame belongs to one continuous stream even though the
// route is re-planned each poll.
TEST(Combat, NpcPursuitKeepsTheRouteSpeedWhileItsTargetKeepsMovingAwayFasterThanIt) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()),
                       tutorial_data, character(), player_stats());
  WorldPosition player{8000, 6000, 0, 0};

  auto packets = combat.advance(kStart, player, tutorial);
  const auto unit_id = find_packet(packets, 177)->message.at("unit_id").get<std::string>();
  for (int step = 1; step <= 40; ++step) {
    player.x_mm += 400;
    const auto step_packets = combat.advance(kStart + step * 100 * kMillisecond, player, tutorial);
    packets.insert(packets.end(), step_packets.begin(), step_packets.end());
  }

  const auto moves = move_notifications(packets, unit_id);
  dump_move_frames("moving-away", moves);
  ASSERT_GE(moves.size(), 8u);
  const nlohmann::json *previous = nullptr;
  for (const auto *move : moves) {
    const auto &message = move->message;
    ASSERT_EQ(message.at("detail"), "MoveDetail_Run") << "the monster never reaches its target";
    expect_running_shape(message);
    if (previous) {
      expect_continuous_walk_pair(*previous, message);
    }
    previous = &message;
  }
  EXPECT_GT(previous->at("pos").at("millimeter_x").get<std::int64_t>(), 4000)
      << "the monster keeps advancing after its target";
}

// A stationary target: the route ends at the stopping distance and the stream ends with exactly
// one Stop at the route end (x 4000 = 6000 - the 2000 mm combat distance).
TEST(Combat, NpcPursuitRunsToItsStoppingDistanceAndThenSendsOneStopAtTheRouteEnd) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()),
                       tutorial_data, character(), player_stats());
  const WorldPosition player{8000, 6000, 0, 0};

  auto packets = combat.advance(kStart, player, tutorial);
  const auto unit_id = find_packet(packets, 177)->message.at("unit_id").get<std::string>();
  for (int step = 1; step <= 20; ++step) {
    const auto step_packets = combat.advance(kStart + step * 100 * kMillisecond, player, tutorial);
    packets.insert(packets.end(), step_packets.begin(), step_packets.end());
  }

  const auto moves = move_notifications(packets, unit_id);
  dump_move_frames("arrival", moves);
  ASSERT_GE(moves.size(), 5u);
  const nlohmann::json *previous = nullptr;
  for (std::size_t index = 0; index + 1 < moves.size(); ++index) {
    const auto &message = moves[index]->message;
    ASSERT_EQ(message.at("detail"), "MoveDetail_Run") << "frame " << index;
    expect_running_shape(message);
    if (previous) {
      expect_continuous_walk_pair(*previous, message);
    }
    previous = &message;
  }
  const auto &stop = moves.back()->message;
  EXPECT_EQ(stop.at("detail"), "MoveDetail_Stop");
  EXPECT_FALSE(stop.contains("pred_pos"));
  EXPECT_NEAR(stop.at("pos").at("millimeter_x").get<double>(), 4000.0, 2.0);
  // The last Run to the Stop covers at most the route speed over the elapsed time (the route may
  // end between two cadence frames) and the Stop tick is later.
  ASSERT_NE(previous, nullptr);
  const auto stop_tick = std::stoll(stop.at("tick").get<std::string>());
  const auto walk_tick = std::stoll(previous->at("tick").get<std::string>());
  ASSERT_GT(stop_tick, walk_tick);
  const double seconds = static_cast<double>(stop_tick - walk_tick) / 1e9;
  const double dx = stop.at("pos").at("millimeter_x").get<double>() -
                    previous->at("pos").at("millimeter_x").get<double>();
  EXPECT_LE(std::abs(dx) / seconds, 3000.0 + 90.0);
}

TEST(Combat, NpcStopsItsMoveNotificationsWhenItsAttackStartsAndNeverResendsRunAfterwards) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()),
                       tutorial_data, character(), player_stats());
  const WorldPosition player{8000, 6000, 0, 0};

  auto packets = combat.advance(kStart, player, tutorial);
  const auto unit_id = find_packet(packets, 177)->message.at("unit_id").get<std::string>();
  for (int step = 1; step <= 60; ++step) {
    const auto step_packets = combat.advance(kStart + step * 100 * kMillisecond, player, tutorial);
    packets.insert(packets.end(), step_packets.begin(), step_packets.end());
  }

  const auto moves = move_notifications(packets, unit_id);
  ASSERT_GE(moves.size(), 2u);
  EXPECT_EQ(moves.front()->message.at("detail"), "MoveDetail_Run");
  // A Stop is sent once the route ends and before the monster's own attack starts.
  std::ptrdiff_t stop_index = -1;
  std::ptrdiff_t attack_index = -1;
  for (std::size_t index = 0; index < packets.size(); ++index) {
    const auto &packet = packets[index];
    if (packet.opcode == 177 && packet.message.value("unit_id", std::string()) == unit_id &&
        packet.message.at("detail") == "MoveDetail_Stop" && stop_index < 0) {
      stop_index = static_cast<std::ptrdiff_t>(index);
    }
    if (packet.opcode == 14 && packet.message.value("unit_id", std::string()) == unit_id &&
        attack_index < 0) {
      attack_index = static_cast<std::ptrdiff_t>(index);
    }
  }
  ASSERT_GE(stop_index, 0);
  ASSERT_GE(attack_index, 0);
  EXPECT_LT(stop_index, attack_index);
  for (std::size_t index = static_cast<std::size_t>(stop_index) + 1; index < packets.size();
       ++index) {
    const auto &packet = packets[index];
    if (packet.opcode == 177 && packet.message.value("unit_id", std::string()) == unit_id) {
      EXPECT_EQ(packet.message.at("detail"), "MoveDetail_Stop") << "no Run after the Stop";
    }
  }
}

TEST(Combat, ANewSessionAnnouncesTheRouteStartAgainAfterAReconnect) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  const WorldPosition player{8000, 6000, 0, 0};
  const auto data = std::make_shared<CombatData>(combat_fixture_with_npc_attack());
  for (int connection = 0; connection < 2; ++connection) {
    TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
    CombatSession combat(data, tutorial_data, character(), player_stats());
    const auto acquired = combat.advance(kStart, player, tutorial);
    const auto unit_id = find_packet(acquired, 177)->message.at("unit_id").get<std::string>();
    const auto moves = move_notifications(acquired, unit_id);
    ASSERT_EQ(moves.size(), 1u) << "connection " << connection;
    EXPECT_TRUE(moves.front()->message.contains("pred_pos")) << "running shape, not a snap";
  }
}

TEST(Combat, NpcSendsRunThenStopAtHomeWhenThePlayerLeavesTheLeashMidRoute) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture_with_npc_attack()),
                       tutorial_data, character(), player_stats());
  const WorldPosition near_player{8000, 6000, 0, 0};
  const WorldPosition far_player{8000, 200000, 0, 0};

  auto packets = combat.advance(kStart, near_player, tutorial);
  const auto unit_id = find_packet(packets, 177)->message.at("unit_id").get<std::string>();
  // The route to the player is still running when the player leaves the 50000 mm leash.
  for (int step = 1; step <= 400; ++step) {
    const auto &player = step < 3 ? near_player : far_player;
    const auto step_packets = combat.advance(kStart + step * 100 * kMillisecond, player, tutorial);
    packets.insert(packets.end(), step_packets.begin(), step_packets.end());
  }

  const auto moves = move_notifications(packets, unit_id);
  ASSERT_GE(moves.size(), 3u);
  // The last frame is a Stop at the spawn position (the monster's home): the route back home
  // ended there and nothing was sent after it.
  const auto &last = moves.back()->message;
  EXPECT_EQ(last.at("detail"), "MoveDetail_Stop");
  EXPECT_EQ(last.at("pos").at("millimeter_x"), 1000);
}

// The player dies from the monster's first attack. Contract: the monster's Stop precedes the
// attack, the frames after the death follow the existing return-home transition, every Run after
// a Stop is a fresh start at the Stop position, ticks strictly increase and the stream ends with
// a Stop at the monster's home (spawn x 1000).
TEST(Combat, NpcFramesKeepTheContractWhenThePlayerDiesAndItReturnsHome) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["effects"]["1"]["parameters"]["physicalCoefficient"] = 100000;
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition player{8000, 6000, 0, 0};

  auto packets = combat.advance(kStart, player, tutorial);
  const auto unit_id = find_packet(packets, 177)->message.at("unit_id").get<std::string>();
  for (int step = 1; step <= 150; ++step) {
    const auto step_packets = combat.advance(kStart + step * 100 * kMillisecond, player, tutorial);
    packets.insert(packets.end(), step_packets.begin(), step_packets.end());
  }

  ASSERT_TRUE(combat.player_dead()) << "the monster's attack actually killed the player";
  const auto moves = move_notifications(packets, unit_id);
  ASSERT_GE(moves.size(), 3u);
  const nlohmann::json *previous = nullptr;
  for (const auto *move : moves) {
    const auto &message = move->message;
    if (previous) {
      EXPECT_GT(std::stoll(message.at("tick").get<std::string>()),
                std::stoll(previous->at("tick").get<std::string>()));
      if (previous->at("detail") == "MoveDetail_Stop" && message.at("detail") == "MoveDetail_Run") {
        EXPECT_EQ(message.at("pos").at("millimeter_x"), previous->at("pos").at("millimeter_x"))
            << "a fresh start begins where the monster stopped";
      }
    }
    previous = &message;
  }
  const auto &last = moves.back()->message;
  EXPECT_EQ(last.at("detail"), "MoveDetail_Stop");
  EXPECT_EQ(last.at("pos").at("millimeter_x"), 1000);
}

// ---- Per-event hit shapes (profile event.shape): NPC casters only. ----
// Every pose below is a FIXTURE chosen for the geometry; none is a decoded official lunge pose.
// Shapes are attached to the events of NPC skill 201. The AI monster (unit 7, cms 5001015) stands
// at x 1000, y 0, z 0 and, with the player inside its 2000 mm engagement distance, casts without
// walking, facing the player (the NPC yaw is the bearing monster -> player). Each landed hit takes
// 10 hp from the player; the generic effect is a 3000 mm / 120 degree sector, so a miss below is
// caused by the event's own shape.

Json cylinder_event_shape(double outer_mm, double inner_mm, double angle_degrees,
                          double axis_degrees, double offset_x_mm, double offset_y_mm = 0) {
  return {{"kind", "cylinder_sector"},
          {"verified", true},
          {"outer_radius_mm", outer_mm},
          {"inner_radius_mm", inner_mm},
          {"half_height_mm", 1000},
          {"angle_degrees", angle_degrees},
          {"axis_degrees", axis_degrees},
          {"offset_mm", {{"x", offset_x_mm}, {"y", offset_y_mm}, {"z", 0}}}};
}

constexpr int kNpcAttackHitDamage = 10;

Json &first_player_event(Json &definitions) {
  return definitions["skills"][0]["layers"][0]["stages"][0]["events"][0];
}

Json &npc_events(Json &definitions) {
  return definitions["skills"][1]["layers"][0]["stages"][0]["events"];
}

Json npc_fixture_with_event_shape(const Json &shape) {
  auto definitions = combat_fixture_with_npc_attack();
  npc_events(definitions)[0]["shape"] = shape;
  return definitions;
}

// Lets the AI monster cast skill 201 at a player standing at the given FIXTURE position and
// returns how many of its events landed (one cast; the next attack is 3 s later).
int npc_hits_on_player(const Json &definitions, int player_x_mm, int player_y_mm,
                       int player_z_mm = 0) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition player{8000, player_x_mm, player_y_mm, player_z_mm, 0};
  bool cast_started = false;
  for (int step = 0; step <= 20; ++step) {
    const auto packets = combat.advance(kStart + step * 100 * kMillisecond, player, tutorial);
    cast_started = cast_started || find_packet(packets, 14) != nullptr;
  }
  EXPECT_TRUE(cast_started) << "the monster actually cast skill 201";
  return (100 - combat.player_stats().at("69").get<int>()) / kNpcAttackHitDamage;
}

TEST(Combat, NpcSecondEventUsesItsOwnShapeAndHitsWhereTheFirstEventMissed) {
  // Shapes shaped like the official 190100400 pair: uid 0 is a narrow arc rotated +40 degrees,
  // uid 1 a wider arc rotated +18.38 degrees; both start 400 mm in front of the caster.
  auto definitions = combat_fixture_with_npc_attack();
  auto &events = npc_events(definitions);
  events[0]["shape"] = cylinder_event_shape(1500, 10, 40, 40, 400);
  auto second = events[0];
  second["unique_id"] = 1;
  second["time_ms"] = 320;
  second["shape"] = cylinder_event_shape(1500, 10, 76.76, 18.38, 400);
  events.push_back(second);

  // FIXTURE pose: player 1000 mm ahead of the monster, so 600 mm straight ahead of the origin.
  EXPECT_EQ(npc_hits_on_player(definitions, 2000, 0), 1) << "uid 0 misses, uid 1 hits";

  // Control: with uid 1's shape on both events both land.
  npc_events(definitions)[0]["shape"] = cylinder_event_shape(1500, 10, 76.76, 18.38, 400);
  EXPECT_EQ(npc_hits_on_player(definitions, 2000, 0), 2);
}

TEST(Combat, NpcEventShapeOffsetIsRotatedByTheNpcYaw) {
  // The player's bearing from the monster is the NPC yaw. FIXTURE poses: player 400 mm ahead of the
  // monster, once facing +y and once facing -y; a 100 mm radius only reaches it when the 400 mm
  // forward offset is rotated with the yaw. A left offset (y) misses the same pose.
  const auto forward_offset =
      npc_fixture_with_event_shape(cylinder_event_shape(100, 0, 360, 0, 400));
  EXPECT_EQ(npc_hits_on_player(forward_offset, 1000, 400), 1) << "yaw +90";
  EXPECT_EQ(npc_hits_on_player(forward_offset, 1000, -400), 1) << "yaw -90";
  const auto left_offset =
      npc_fixture_with_event_shape(cylinder_event_shape(100, 0, 360, 0, 0, 400));
  EXPECT_EQ(npc_hits_on_player(left_offset, 1000, 400), 0) << "yaw +90: left is -x";
}

TEST(Combat, NpcEventShapeAxisIsTheNpcYawPlusTheShapeAxis) {
  // The target always lies on the NPC yaw, so it is inside the sector when |axis_degrees| <=
  // angle / 2. FIXTURE poses: player ahead on +x and on +y.
  const auto narrow_axis_5 = npc_fixture_with_event_shape(cylinder_event_shape(3000, 0, 20, 5, 0));
  EXPECT_EQ(npc_hits_on_player(narrow_axis_5, 2000, 0), 1);
  EXPECT_EQ(npc_hits_on_player(narrow_axis_5, 1000, 1000), 1)
      << "yaw +90 plus axis 5 still contains the target";
  const auto narrow_axis_15 =
      npc_fixture_with_event_shape(cylinder_event_shape(3000, 0, 20, 15, 0));
  EXPECT_EQ(npc_hits_on_player(narrow_axis_15, 2000, 0), 0);
  const auto narrow_axis_90 =
      npc_fixture_with_event_shape(cylinder_event_shape(3000, 0, 20, 90, 0));
  EXPECT_EQ(npc_hits_on_player(narrow_axis_90, 2000, 0), 0);
}

TEST(Combat, NpcEventShapeRespectsItsInnerAndOuterRadius) {
  // FIXTURE pose: player 1000 mm ahead of the monster, full circle, no offset.
  const auto inside_inner = npc_fixture_with_event_shape(cylinder_event_shape(3000, 1200, 360, 0, 0));
  EXPECT_EQ(npc_hits_on_player(inside_inner, 2000, 0), 0) << "inside the inner radius";
  const auto in_ring = npc_fixture_with_event_shape(cylinder_event_shape(3000, 500, 360, 0, 0));
  EXPECT_EQ(npc_hits_on_player(in_ring, 2000, 0), 1);

  // The cast gate closes in until the cast's widest event reaches the player, so the narrow
  // 900 mm event is paired with a wide one that keeps the monster at the fixture's 1000 mm.
  auto beyond_outer = npc_fixture_with_event_shape(cylinder_event_shape(900, 0, 360, 0, 0));
  auto wide_event = npc_events(beyond_outer)[0];
  wide_event["unique_id"] = 1;
  wide_event["time_ms"] = 320;
  wide_event["shape"] = cylinder_event_shape(3000, 0, 360, 0, 0);
  npc_events(beyond_outer).push_back(wide_event);
  EXPECT_EQ(npc_hits_on_player(beyond_outer, 2000, 0), 1)
      << "the 900 mm event stays beyond the outer radius; only the wide event lands";
}

TEST(Combat, NpcMeleeClosesToItsShapeReachBeforeSwinging) {
  // The 1 m attack shape is shorter than both the 2 m combat distance and the 5 m skill range.
  // Stopping at 2 m played a swing that could never land; the monster now closes to 1 m first.
  auto definitions = combat_fixture_with_npc_attack();
  definitions["effects"]["1"]["targeting"]["outerRadius"] = 100;
  // FIXTURE pose: player 5 m ahead of the monster.
  EXPECT_EQ(npc_hits_on_player(definitions, 6000, 0), 1);
}

TEST(Combat, NpcMeleeCylinderNeverHitsAPlayerWhoLeftItsRadius) {
  // shapeCylinder used to fall through to the skill-range fallback, so a 1.5 m melee cylinder of
  // a 50 m skill still hit a player who had run 20 m away before the damage event.
  auto definitions = combat_fixture_with_npc_attack();
  definitions["effects"]["1"]["targeting_kind"] = "shapeCylinder";
  definitions["effects"]["1"]["targeting"] = {
      {"radius", 150}, {"halfHeight", 200}, {"maxTargetCount", 1}};
  definitions["skills"][1]["range_mm"] = 50000;
  EXPECT_EQ(npc_hits_on_player(definitions, 2000, 0), 1) << "a player beside the monster is hit";

  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition beside{8000, 2000, 0, 0, 0};
  const WorldPosition far_away{8000, 22000, 0, 0, 0};

  std::int64_t elapsed_ms = 0;
  bool cast_started = false;
  while (elapsed_ms <= 2000 && !cast_started) {
    const auto packets = combat.advance(kStart + elapsed_ms * kMillisecond, beside, tutorial);
    cast_started = has_opcode(packets, 14);
    elapsed_ms += 100;
  }
  ASSERT_TRUE(cast_started);

  // The player runs off before the cast's 300 ms damage event.
  for (int step = 0; step < 10; ++step) {
    combat.advance(kStart + elapsed_ms * kMillisecond, far_away, tutorial);
    elapsed_ms += 100;
  }
  EXPECT_EQ(combat.player_stats().at("69").get<int>(), 100);
}

TEST(Combat, NpcHitShapesTouchThePlayerCapsuleNotOnlyItsCentre) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["effects"]["1"]["targeting_kind"] = "shapeBox";
  definitions["effects"]["1"]["targeting"] = {
      {"extentX", 100}, {"extentY", 50}, {"extentZ", 30}, {"maxTargetCount", 1}};

  // FIXTURE pose: player 1.2 m ahead of and 0.4 m above the monster. Its centre point stays
  // outside the 1 m by 0.3 m box even after the monster closes in.
  EXPECT_EQ(npc_hits_on_player(definitions, 2200, 0, 400), 0)
      << "a profile without a collision table tests the centre point only";

  // A 300 mm radius, 960 mm half-height player capsule overlaps the box from the same pose.
  definitions["player_collision"] = {{"10",
                                      {{"game_hit_capsule_radius_mm", 300.0},
                                       {"game_hit_capsule_half_height_mm", 960.0}}}};
  EXPECT_EQ(npc_hits_on_player(definitions, 2200, 0, 400), 1);
}

TEST(Combat, NpcEventsWithoutAVerifiedCylinderShapeKeepTheGenericEffectShape) {
  // FIXTURE pose: player 1000 mm ahead, inside the generic sector. A box, a sphere and an
  // unverified cylinder would all miss if they were applied.
  const Json unsupported_shapes[] = {
      {{"kind", "box"}, {"verified", false}, {"authored", {{"boxExtent", {{"x", 1}}}}}},
      {{"kind", "sphere"}, {"verified", false}, {"authored", {{"radius", 1}}}},
      {{"kind", "cylinder_sector"}, {"verified", false}, {"outer_radius_mm", 1}}};
  EXPECT_EQ(npc_hits_on_player(combat_fixture_with_npc_attack(), 2000, 0), 1) << "no metadata";
  for (const auto &shape : unsupported_shapes) {
    EXPECT_EQ(npc_hits_on_player(npc_fixture_with_event_shape(shape), 2000, 0), 1)
        << shape.at("kind").get<std::string>();
  }
}

// Casts player skill 200 with the given request yaw from the FIXTURE pose (0, 0): the stationary
// monster is 1000 mm ahead of yaw 0. Returns the monster's hp after the 300 ms event.
int monster_hp_after_player_cast(const Json &definitions, double yaw_degrees) {
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition caster{8000, 0, 0, 0, 0};
  auto request = start_request();
  request["start_skill"]["rotation"]["yaw"] = yaw_degrees;
  EXPECT_EQ(combat.receive(20010, request, caster, kStart).size(), 2u);
  combat.advance(kStart + 300 * kMillisecond, caster, tutorial);
  return combat.monster_hp(kMonster);
}

TEST(Combat, PlayerSkillsKeepTheGenericEffectPathEvenWhenTheEventCarriesAShape) {
  EXPECT_EQ(monster_hp_after_player_cast(combat_fixture(), 0), 0) << "no metadata";
  // A verified cylinder sector that would miss the monster (outer radius 1 mm) is ignored for a
  // player caster: the generic 3000 mm / 120 degree effect still hits.
  auto tiny_shape = combat_fixture();
  first_player_event(tiny_shape)["shape"] = cylinder_event_shape(1, 0, 360, 0, 0);
  EXPECT_EQ(monster_hp_after_player_cast(tiny_shape, 0), 0);
  // A shape that would reach the monster does not extend the player's reach: facing away (yaw 180)
  // the monster is outside the generic 120 degree sector.
  auto wide_shape = combat_fixture();
  first_player_event(wide_shape)["shape"] = cylinder_event_shape(3000, 0, 360, 0, 0);
  EXPECT_EQ(monster_hp_after_player_cast(wide_shape, 180), 50)
      << "generic sector misses; the event shape is not applied to a player";
}

TEST(Combat, AMalformedVerifiedEventShapeIsRejectedWhenTheProfileLoads) {
  const auto negative = npc_fixture_with_event_shape(cylinder_event_shape(-1, 0, 360, 0, 0));
  EXPECT_THROW(std::make_shared<CombatData>(negative), std::runtime_error);
  const auto inverted = npc_fixture_with_event_shape(cylinder_event_shape(500, 900, 360, 0, 0));
  EXPECT_THROW(std::make_shared<CombatData>(inverted), std::runtime_error);
  auto missing_offset_shape = cylinder_event_shape(500, 0, 360, 0, 0);
  missing_offset_shape.erase("offset_mm");
  const auto missing_offset = npc_fixture_with_event_shape(missing_offset_shape);
  EXPECT_THROW(std::make_shared<CombatData>(missing_offset), std::runtime_error);
}

// Run explicitly: CHRONICLES_COMBAT_PROFILE_SHAPES = the regenerated combat profile whose events
// carry shape objects (285 verified cylinder sectors, boxes and spheres marked unverified).
TEST(Combat, RealProfileWithEventShapesLoadsAndCountsEveryVerifiedCylinderSector) {
  const auto *profile_path = std::getenv("CHRONICLES_COMBAT_PROFILE_SHAPES");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the regenerated shape profile (e557dc9b)";
  }
  const auto profile = load_json_file(profile_path);
  ASSERT_TRUE(profile);

  int verified_cylinders = 0;
  int boxes = 0;
  int spheres = 0;
  int unverified = 0;
  for (const auto &skill : profile->at("skills")) {
    for (const auto &layer : skill.at("layers")) {
      for (const auto &stage : layer.at("stages")) {
        for (const auto &event : stage.at("events")) {
          if (!event.contains("shape")) {
            continue;
          }
          const auto &shape = event.at("shape");
          const auto kind = shape.at("kind").get<std::string>();
          if (!shape.at("verified").get<bool>()) {
            ++unverified;
          }
          if (kind == "cylinder_sector" && shape.at("verified").get<bool>()) {
            ++verified_cylinders;
          } else if (kind == "box") {
            ++boxes;
          } else if (kind == "sphere") {
            ++spheres;
          }
        }
      }
    }
  }
  EXPECT_EQ(verified_cylinders, 285);
  EXPECT_EQ(boxes, 412);
  EXPECT_EQ(spheres, 117);
  EXPECT_EQ(unverified, boxes + spheres);

  // The loader validates every verified cylinder sector and ignores the rest without throwing.
  EXPECT_NO_THROW(std::make_shared<CombatData>(*profile));
}

// ---- Attack root motion and unit collision data: parsed and validated at load, stored only. ----

Json valid_root_motion_block() {
  return {{"type", "EX5RootMotionType_AdaptedChaseTarget"},
          {"window_start_ms", 100},
          {"window_end_ms", 400},
          {"max_move_distance_mm", 2100.0},
          {"offset_distance_mm", 500.0},
          {"offset_basis_pos_type", "EX5AdaptedMotionOffsetBasisPos_TargetPos"},
          {"offset_basis_dir_type", "EX5AdaptedMotionOffsetBasisDir_CasterToTargetDir"},
          {"offset_direction_degrees", 180},
          {"look_at_dir_type", "EX5AdaptedMotionLookAtDir_ToTarget"},
          {"rotation_angular_speed_degrees", 90},
          {"using_scale", true},
          {"ground_move", true},
          {"ignore_collision", false},
          {"end_ground_move", true},
          {"position_data",
           {{"time_base", "montage_ms"},
            {"uniform", false},
            {"sample_count", 2},
            {"samples",
             {{{"elapsed_ms", 100}, {"x_mm", 0.0}, {"y_mm", 0.0}, {"z_mm", 0.0}},
              {{"elapsed_ms", 400}, {"x_mm", 700.0}, {"y_mm", 0.0}, {"z_mm", 0.0}}}}}}};
}

Json fixture_with_root_motion(const Json &block) {
  auto definitions = combat_fixture();
  definitions["skills"][0]["layers"][0]["stages"][0]["root_motion"] = block;
  return definitions;
}

TEST(Combat, ARootMotionBlockIsParsedAndStoredWithoutMovingAnything) {
  const CombatData data(fixture_with_root_motion(valid_root_motion_block()));
  ASSERT_EQ(data.root_motion.size(), 1u);
  const auto &window = data.root_motion.at({200, 0, 0});
  EXPECT_EQ(window.window_start_ms, 100);
  EXPECT_EQ(window.window_end_ms, 400);
  EXPECT_EQ(window.max_move_distance_mm, 2100.0);
  EXPECT_EQ(window.offset_distance_mm, 500.0);
  EXPECT_EQ(window.offset_direction_degrees, 180.0);
  EXPECT_TRUE(window.using_scale);
  EXPECT_TRUE(window.ground_move);
  EXPECT_FALSE(window.ignore_collision);
  EXPECT_TRUE(window.end_ground_move);
  EXPECT_EQ(window.time_base, "montage_ms");
  ASSERT_EQ(window.samples.size(), 2u);
  EXPECT_EQ(window.samples.back().elapsed_ms, 400);
  EXPECT_EQ(window.samples.back().x_mm, 700.0);
  EXPECT_TRUE(CombatData(combat_fixture()).root_motion.empty()) << "profiles without the block";
}

TEST(Combat, AMalformedRootMotionBlockIsRejectedWhenTheProfileLoads) {
  const auto rejects = [](const std::function<void(Json &)> &damage) {
    auto block = valid_root_motion_block();
    damage(block);
    const auto definitions = fixture_with_root_motion(block);
    EXPECT_THROW(std::make_shared<CombatData>(definitions), std::runtime_error);
  };
  rejects([](Json &block) { block["window_end_ms"] = 50; });
  rejects([](Json &block) { block["window_start_ms"] = -1; });
  rejects([](Json &block) { block["max_move_distance_mm"] = -1.0; });
  rejects([](Json &block) { block["using_scale"] = "yes"; });
  rejects([](Json &block) { block.erase("offset_basis_pos_type"); });
  rejects([](Json &block) { block.erase("position_data"); });
  rejects([](Json &block) { block["position_data"]["time_base"] = "window_ms"; });
  rejects([](Json &block) { block["position_data"]["samples"] = Json::array(); });
  rejects([](Json &block) { block["position_data"]["sample_count"] = 3; });
  rejects([](Json &block) { block["position_data"]["samples"][1]["elapsed_ms"] = 100; });
  rejects([](Json &block) { block["position_data"]["samples"][0].erase("z_mm"); });
}

TEST(Combat, AMalformedUnitCollisionTableIsRejectedWhenTheProfileLoads) {
  auto definitions = combat_fixture();
  definitions["npc_collision"] = {{"5001010",
                                   {{"game_hit_capsule_radius_mm", 350.0},
                                    {"game_hit_capsule_half_height_mm", 930.0}}}};
  definitions["player_collision"] = {{"10",
                                      {{"game_hit_capsule_radius_mm", 300.0},
                                       {"game_hit_capsule_half_height_mm", 960.0}}}};
  const CombatData data(definitions);
  EXPECT_EQ(data.npc_collision.at(5001010).radius_mm, 350.0);
  EXPECT_EQ(data.player_collision.at(10).half_height_mm, 960.0);

  auto negative = definitions;
  negative["npc_collision"]["5001010"]["game_hit_capsule_radius_mm"] = -1.0;
  EXPECT_THROW(std::make_shared<CombatData>(negative), std::runtime_error);
  auto bad_key = definitions;
  bad_key["player_collision"] = {{"ten",
                                  {{"game_hit_capsule_radius_mm", 300.0},
                                   {"game_hit_capsule_half_height_mm", 960.0}}}};
  EXPECT_THROW(std::make_shared<CombatData>(bad_key), std::runtime_error);
  auto missing = definitions;
  missing["npc_collision"]["5001010"].erase("game_hit_capsule_half_height_mm");
  EXPECT_THROW(std::make_shared<CombatData>(missing), std::runtime_error);
}

// Run explicitly: CHRONICLES_COMBAT_PROFILE_LUNGE = the regenerated combat profile with
// root_motion blocks and the unit collision tables (profile-lunge-2.json, 6f1aae5d).
TEST(Combat, RealProfileWithRootMotionLoadsAllBlocksAndCollisionRadii) {
  const auto *profile_path = std::getenv("CHRONICLES_COMBAT_PROFILE_LUNGE");
  if (!profile_path) {
    GTEST_SKIP() << "Run explicitly against the lunge profile (6f1aae5d)";
  }
  const auto profile = load_json_file(profile_path);
  ASSERT_TRUE(profile);
  const CombatData data(*profile);
  EXPECT_EQ(data.root_motion.size(), 20u);
  EXPECT_EQ(data.npc_collision.size(), 11u);
  EXPECT_EQ(data.player_collision.size(), 6u);
  // Both data-backed spot checks come from the profile itself, not from a constant in the server.
  EXPECT_EQ(data.npc_collision.at(5001010).radius_mm, 350.0);
  EXPECT_EQ(data.player_collision.at(10).radius_mm, 300.0);
  const auto lunge = std::ranges::find_if(data.root_motion, [](const auto &entry) {
    return std::get<0>(entry.first) == 190100200;
  });
  ASSERT_NE(lunge, data.root_motion.end());
  EXPECT_EQ(lunge->second.window_start_ms, 1006);
  EXPECT_EQ(lunge->second.window_end_ms, 1313);
  EXPECT_EQ(lunge->second.samples.front().elapsed_ms, 1006) << "absolute montage time";
  EXPECT_EQ(lunge->second.samples.size(), 5u);
}

// ---- NPC attack lunge (AdaptedChaseTarget, window starting with the action). ----
// The AI monster (unit 7, cms 5001015) stands at x 1000, y 0, z 0 and casts NPC skill 201 at a
// FIXTURE player standing at x 3000 (2000 mm away, inside its engagement distance). The fixture
// block is the 190100400 kind: window 0..1900 ms, forward-only samples reaching max_move at the
// window end, offset 500 mm; hit radii 350 (NPC) + 300 (player) give a standoff of 1150 mm, so
// the lunge destination is x 1850 (displacement 850 mm, scale 0.17). The cylinder event shape
// (full circle, offset 0) is the only hit test, evaluated 1500 ms after the cast.

Json lunge_block(int window_start_ms, int window_end_ms, double max_move_mm) {
  const int middle_ms = (window_start_ms + window_end_ms) / 2;
  auto block = valid_root_motion_block();
  block["window_start_ms"] = window_start_ms;
  block["window_end_ms"] = window_end_ms;
  block["max_move_distance_mm"] = max_move_mm;
  block["offset_distance_mm"] = 500.0;
  block["position_data"]["sample_count"] = 3;
  block["position_data"]["samples"] = {
      {{"elapsed_ms", window_start_ms}, {"x_mm", 0.0}, {"y_mm", 0.0}, {"z_mm", 0.0}},
      {{"elapsed_ms", middle_ms}, {"x_mm", max_move_mm / 2}, {"y_mm", 0.0}, {"z_mm", 0.0}},
      {{"elapsed_ms", window_end_ms}, {"x_mm", max_move_mm}, {"y_mm", 0.0}, {"z_mm", 0.0}}};
  return block;
}

Json lunge_fixture(double hit_radius_mm, const Json &block, bool with_npc_collision = true) {
  auto definitions = combat_fixture_with_npc_attack();
  auto &stage = definitions["skills"][1]["layers"][0]["stages"][0];
  stage["duration_ms"] = 2500;
  stage["events"][0]["time_ms"] = 1500;
  stage["events"][0]["shape"] = cylinder_event_shape(hit_radius_mm, 0, 360, 0, 0);
  if (!block.is_null()) {
    stage["root_motion"] = block;
  }
  if (with_npc_collision) {
    definitions["npc_collision"] = {{"5001015",
                                     {{"game_hit_capsule_radius_mm", 350.0},
                                      {"game_hit_capsule_half_height_mm", 930.0}}}};
  }
  definitions["player_collision"] = {{"10",
                                      {{"game_hit_capsule_radius_mm", 300.0},
                                       {"game_hit_capsule_half_height_mm", 960.0}}},
                                     {"11",
                                      {{"game_hit_capsule_radius_mm", 300.0},
                                       {"game_hit_capsule_half_height_mm", 960.0}}}};
  return definitions;
}

struct LungeRun {
  std::vector<GameplayPacket> packets;
  int hits{};
};

LungeRun run_lunge(const Json &definitions, int poll_ms, int total_ms,
                   const std::function<int(int)> &player_x_at) {
  const auto tutorial_data = tutorial_fixture_with_ai(npc_ai_block());
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  LungeRun run;
  for (int elapsed = 0; elapsed <= total_ms; elapsed += poll_ms) {
    const WorldPosition player{8000, player_x_at(elapsed), 0, 0, 0};
    const auto packets = combat.advance(kStart + elapsed * kMillisecond, player, tutorial);
    run.packets.insert(run.packets.end(), packets.begin(), packets.end());
  }
  run.hits = (100 - combat.player_stats().at("69").get<int>()) / kNpcAttackHitDamage;
  return run;
}

std::vector<const GameplayPacket *> packets_with_opcode(const LungeRun &run, int opcode) {
  std::vector<const GameplayPacket *> found;
  for (const auto &packet : run.packets) {
    if (packet.opcode == opcode) {
      found.push_back(&packet);
    }
  }
  return found;
}

int player_at_three_metres(int) {
  return 3000;
}

TEST(Combat, StandingTwoMetreAttackOnlyHitsOnceTheLungeClosesTheDistance) {
  // 1500 mm hit radius plus the 300 mm player capsule: out of reach from the standing pose
  // (2000 mm away), in reach from the lunge pose 1500 ms into the motion (about 1329 mm away).
  // Without a lunge the monster must first walk to its 1800 mm hit reach; with one, the lunge
  // travel counts toward the reach and the cast starts from the standing pose.
  const auto without_motion = lunge_fixture(1500, Json());
  const auto walked = run_lunge(without_motion, 100, 2600, player_at_three_metres);
  const auto walked_starts = packets_with_opcode(walked, 14);
  ASSERT_EQ(walked_starts.size(), 1u);
  EXPECT_EQ(walked_starts[0]->message.at("start_skill").at("pos").at("millimeter_x"), 1200)
      << "no root_motion data: the monster closes to its hit reach before swinging";
  EXPECT_EQ(walked.hits, 1);

  const auto with_motion = lunge_fixture(1500, lunge_block(0, 1900, 5000));
  const auto run = run_lunge(with_motion, 100, 2600, player_at_three_metres);
  const auto lunge_starts = packets_with_opcode(run, 14);
  ASSERT_EQ(lunge_starts.size(), 1u);
  EXPECT_EQ(lunge_starts[0]->message.at("start_skill").at("pos").at("millimeter_x"), 1000)
      << "the lunge closes the distance, so the monster swings from where it stands";
  EXPECT_EQ(run.hits, 1);
  verify_encoding(run.packets);
}

TEST(Combat, TheLungeAnnouncesItsMotionAfterTheAttackStartAndEndsItWithTheFinalPose) {
  const auto run = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 5000)), 100, 3400,
                             player_at_three_metres);
  const auto starts = packets_with_opcode(run, 14);
  const auto motions = packets_with_opcode(run, 64);
  const auto ends = packets_with_opcode(run, 106);
  ASSERT_EQ(starts.size(), 1u);
  ASSERT_EQ(motions.size(), 1u);
  ASSERT_EQ(ends.size(), 1u);
  const auto index_of = [&run](const GameplayPacket *packet) { return packet - run.packets.data(); };
  EXPECT_LT(index_of(starts[0]), index_of(motions[0])) << "the 64 follows the 14";
  EXPECT_LT(index_of(motions[0]), index_of(ends[0]));

  const auto &start = starts[0]->message.at("start_skill");
  const auto &motion = motions[0]->message.at("root_motion");
  EXPECT_EQ(motions[0]->message.at("unit_id"), starts[0]->message.at("unit_id"));
  EXPECT_EQ(motion.at("skill_instance_id"), start.at("instance_id"));
  EXPECT_EQ(motion.at("start_pos").at("millimeter_x"), 1000);
  EXPECT_EQ(motion.at("dest_pos").at("millimeter_x"), 1850) << "3000 - (500 + 350 + 300)";
  EXPECT_EQ(motion.at("rotate_duration"), 1900);
  EXPECT_NEAR(motion.at("scale").get<double>(), 850.0 / 5000.0, 1e-6);
  EXPECT_EQ(motion.at("target_unit_id"), "7");
  EXPECT_TRUE(motion.at("ground_move").get<bool>());
  const auto cast_tick = std::stoll(start.at("tick").get<std::string>());
  EXPECT_EQ(std::stoll(motion.at("end_tick").get<std::string>()), cast_tick + 1900 * 1000000LL);

  const auto &end = ends[0]->message;
  EXPECT_EQ(end.at("unit_id"), starts[0]->message.at("unit_id"));
  EXPECT_EQ(end.at("skill_instance_id"), start.at("instance_id"));
  EXPECT_EQ(end.at("dest_pos").at("millimeter_x"), 1850);
  EXPECT_EQ(end.at("end_tick"), motion.at("end_tick"));
}

TEST(Combat, TheLungeHitIsEvaluatedFromThePoseAtTheEventTimeUnderCoarsePolls) {
  // Event pose (1500 ms): about 1329 mm from the player; end pose: 1150 mm. A 900 mm radius
  // touches the 300 mm player capsule only from the end pose, so it must miss even when the poll
  // resolving the event arrives after the motion has ended.
  const auto narrow = lunge_fixture(900, lunge_block(0, 1900, 5000));
  // The monster acquires at 0 and casts 1 s later (first poll at or after it: 1000 ms at 100 ms
  // polls, 1400 ms at 700 ms polls); the event resolves 1500 ms after the cast.
  EXPECT_EQ(run_lunge(narrow, 100, 3400, player_at_three_metres).hits, 0);
  EXPECT_EQ(run_lunge(narrow, 700, 3500, player_at_three_metres).hits, 0)
      << "coarse polls must not use the later pose";
  const auto wide = lunge_fixture(1100, lunge_block(0, 1900, 5000));
  EXPECT_EQ(run_lunge(wide, 100, 3400, player_at_three_metres).hits, 1);
  EXPECT_EQ(run_lunge(wide, 700, 3500, player_at_three_metres).hits, 1);
}

TEST(Combat, TheMonsterKeepsTheLungePoseAndTheMotionIsNotRepeatedByTheNextCast) {
  // The second cast (3 s after the first) starts where the lunge ended and its displacement is
  // exactly zero (the player is already at the standoff distance), so it sends no second 64.
  const auto run = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 5000)), 100, 4200,
                             player_at_three_metres);
  const auto starts = packets_with_opcode(run, 14);
  ASSERT_EQ(starts.size(), 2u);
  EXPECT_EQ(starts[1]->message.at("start_skill").at("pos").at("millimeter_x"), 1850);
  EXPECT_EQ(packets_with_opcode(run, 64).size(), 1u);
  EXPECT_EQ(packets_with_opcode(run, 106).size(), 1u);
}

TEST(Combat, TheLungeIsBoundedFromItsStartAndNeverHomesOnAMovingTarget) {
  // The target is sampled once at the cast; running away afterwards does not extend the path.
  const auto escaping = [](int elapsed_ms) { return elapsed_ms < 1100 ? 3000 : 9000; };
  const auto run = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 5000)), 100, 3400, escaping);
  const auto ends = packets_with_opcode(run, 106);
  ASSERT_EQ(packets_with_opcode(run, 64).size(), 1u);
  ASSERT_EQ(ends.size(), 1u);
  EXPECT_EQ(ends[0]->message.at("dest_pos").at("millimeter_x"), 1850);

  // A short max_move clamps the destination from the caster start (3D) and the scale becomes 1.
  const auto clamped = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 500)), 100, 3400,
                                 player_at_three_metres);
  const auto motions = packets_with_opcode(clamped, 64);
  ASSERT_EQ(motions.size(), 1u);
  const auto &motion = motions[0]->message.at("root_motion");
  EXPECT_EQ(motion.at("dest_pos").at("millimeter_x"), 1500);
  EXPECT_NEAR(motion.at("scale").get<double>(), 1.0, 1e-6);
}

TEST(Combat, AnInterruptedLungeEndsAtTheRealPoseWithMatchingIdsAndNoStaleMotion) {
  // The player leaves the 50000 mm leash 1 s into the motion: the action is cancelled, the 106
  // retargets the client's path to the pose reached (between start and destination), exactly one
  // 106 is sent and it precedes the end_skill (15).
  // The monster acquires at 0 and casts 1 s later; the motion runs 1000..2900 ms.
  const auto leaving = [](int elapsed_ms) { return elapsed_ms < 2000 ? 3000 : 200000; };
  const auto run = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 5000)), 100, 3400, leaving);
  const auto skill_ends = packets_with_opcode(run, 15);
  const auto attack_starts = packets_with_opcode(run, 14);
  ASSERT_EQ(attack_starts.size(), 1u);
  ASSERT_GE(skill_ends.size(), 1u);
  // The AI's own pursuit/return routes also use 64/106; only the attack's instance is the lunge.
  const auto attack_instance = attack_starts[0]->message.at("start_skill").at("instance_id");
  std::vector<const GameplayPacket *> lunge_starts;
  std::vector<const GameplayPacket *> ends;
  for (const auto &packet : run.packets) {
    if (packet.opcode == 64 &&
        packet.message.at("root_motion").at("skill_instance_id") == attack_instance) {
      lunge_starts.push_back(&packet);
    }
    if (packet.opcode == 106 && packet.message.at("skill_instance_id") == attack_instance) {
      ends.push_back(&packet);
    }
  }
  ASSERT_EQ(lunge_starts.size(), 1u);
  ASSERT_EQ(ends.size(), 1u);
  const auto reached = ends[0]->message.at("dest_pos").at("millimeter_x").get<int>();
  EXPECT_GT(reached, 1000);
  EXPECT_LT(reached, 1850);
  EXPECT_EQ(ends[0]->message.at("skill_instance_id"),
            skill_ends[0]->message.at("end_skill").at("instance_id"));
  EXPECT_LT(ends[0] - run.packets.data(), skill_ends[0] - run.packets.data());
}

TEST(Combat, ADelayedLungeWindowStartsAtItsOwnTimeAndUnmeasuredWindowsStayUnmoved) {
  // Delayed window (400..1900 ms): no 64 before the window opens, then one 64 and a hit judged
  // from the moving pose. The cast starts at 1000 ms, so the window opens at 1400 ms.
  const auto delayed_block = lunge_block(400, 1900, 5000);
  const auto before_window = run_lunge(lunge_fixture(1500, delayed_block), 100, 1300,
                                       player_at_three_metres);
  EXPECT_EQ(packets_with_opcode(before_window, 14).size(), 1u);
  EXPECT_TRUE(packets_with_opcode(before_window, 64).empty()) << "still inside the wind-up";
  const auto delayed = run_lunge(lunge_fixture(1500, delayed_block), 100, 2600,
                                 player_at_three_metres);
  EXPECT_EQ(packets_with_opcode(delayed, 64).size(), 1u);
  EXPECT_EQ(delayed.hits, 1);
  // No NPC collision row: the standoff cannot be computed from data, so nothing moves.
  const auto no_radius = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 5000), false), 100,
                                   2600, player_at_three_metres);
  EXPECT_TRUE(packets_with_opcode(no_radius, 64).empty());
  // Exactly the standoff distance: displacement is zero, no 64 and the standing hit lands.
  const auto at_standoff = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 5000)), 100, 2600,
                                     [](int) { return 2150; });
  EXPECT_TRUE(packets_with_opcode(at_standoff, 64).empty());
  EXPECT_EQ(at_standoff.hits, 1);
}

TEST(Combat, PlayerSkillsNeverLungeEvenWhenTheirStageCarriesARootMotionBlock) {
  auto definitions = combat_fixture();
  definitions["skills"][0]["layers"][0]["stages"][0]["root_motion"] = lunge_block(0, 900, 5000);
  definitions["npc_collision"] = {{"5001015",
                                   {{"game_hit_capsule_radius_mm", 350.0},
                                    {"game_hit_capsule_half_height_mm", 930.0}}}};
  definitions["player_collision"] = {{"10",
                                      {{"game_hit_capsule_radius_mm", 300.0},
                                       {"game_hit_capsule_half_height_mm", 960.0}}}};
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition caster{8000, 0, 0, 0, 0};
  auto packets = combat.receive(20010, start_request(), caster, kStart);
  const auto later = combat.advance(kStart + 300 * kMillisecond, caster, tutorial);
  packets.insert(packets.end(), later.begin(), later.end());
  EXPECT_EQ(combat.monster_hp(kMonster), 0) << "the generic player attack is unchanged";
  EXPECT_EQ(find_packet(packets, 64), nullptr);
  EXPECT_EQ(find_packet(packets, 106), nullptr);
}

// The 64/106 frames of the attack itself: the AI's own pursuit/return routes also use these
// opcodes (pre-existing baseline behavior, unverified against the official client and not part
// of the lunge change), so lunge assertions always filter by the attack's 14 instance.
std::vector<const GameplayPacket *> attack_motion_packets(const LungeRun &run, int opcode) {
  std::vector<const GameplayPacket *> found;
  const auto starts = packets_with_opcode(run, 14);
  if (starts.empty()) {
    return found;
  }
  const auto instance = starts[0]->message.at("start_skill").at("instance_id");
  for (const auto &packet : run.packets) {
    if (packet.opcode != opcode) {
      continue;
    }
    const auto &carrier = opcode == 64 ? packet.message.at("root_motion") : packet.message;
    if (carrier.at("skill_instance_id") == instance) {
      found.push_back(&packet);
    }
  }
  return found;
}

TEST(Combat, ALungeWindowWithAnyUnimplementedSettingIsLeftUnmoved) {
  // Supported 190100400-like semantics: offset direction 180, look at the target, ground move,
  // collision honoured, ground-adjusted end. Every other value is a meaning the server does not
  // implement, so the attack itself stays unmoved (no 64, no 106). Such a window does not count
  // toward the hit reach either: the monster walks in with ordinary pursuit and lands one
  // standing hit instead of swinging from out of reach.
  struct Mutation {
    const char *name;
    std::function<void(Json &)> apply;
  };
  const std::vector<Mutation> mutations{
      {"offset direction", [](Json &block) { block["offset_direction_degrees"] = 90; }},
      {"look at", [](Json &block) { block["look_at_dir_type"] = "EX5AdaptedMotionLookAtDir_None"; }},
      {"ground move", [](Json &block) { block["ground_move"] = false; }},
      {"ignore collision", [](Json &block) { block["ignore_collision"] = true; }},
      {"end ground move", [](Json &block) { block["end_ground_move"] = false; }},
      {"basis dir", [](Json &block) { block["offset_basis_dir_type"] = "EX5AdaptedMotionOffsetBasisDir_Other"; }},
  };
  const auto control = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 5000)), 100, 3400,
                                 player_at_three_metres);
  EXPECT_EQ(attack_motion_packets(control, 64).size(), 1u) << "unmutated block moves";
  for (const auto &mutation : mutations) {
    auto block = lunge_block(0, 1900, 5000);
    mutation.apply(block);
    const auto run = run_lunge(lunge_fixture(1500, block), 100, 3400, player_at_three_metres);
    EXPECT_TRUE(attack_motion_packets(run, 64).empty()) << mutation.name;
    EXPECT_TRUE(attack_motion_packets(run, 106).empty()) << mutation.name;
    EXPECT_EQ(packets_with_opcode(run, 19).size(), 1u) << mutation.name;
  }
  // The window may not outlast the stage (stage duration 2500 ms in the fixture). Damage
  // notifications are counted because health regeneration starts within this longer run.
  const auto too_long = run_lunge(lunge_fixture(1500, lunge_block(0, 2600, 5000)), 100, 3600,
                                  player_at_three_metres);
  EXPECT_TRUE(attack_motion_packets(too_long, 64).empty()) << "window longer than the stage";
  EXPECT_EQ(packets_with_opcode(too_long, 19).size(), 1u);
}

TEST(Combat, AnInterruptedLungeUnderCoarsePollsEndsAtTheExactCurrentPoseAndTick) {
  // 700 ms polls: the monster casts at the 1400 ms poll and the player leaves the leash before
  // the 2100 ms poll, which cancels the action 700 ms into the motion. The 106 must carry the
  // pose of THAT poll (1000 + 850 * 700 / 1900 = 1313 mm), not the previous poll's pose (1000).
  const auto leaving = [](int elapsed_ms) { return elapsed_ms < 2000 ? 3000 : 200000; };
  const auto run = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 5000)), 700, 3500, leaving);
  const auto starts = packets_with_opcode(run, 14);
  const auto motions = attack_motion_packets(run, 64);
  const auto ends = attack_motion_packets(run, 106);
  ASSERT_EQ(starts.size(), 1u);
  ASSERT_EQ(motions.size(), 1u);
  ASSERT_EQ(ends.size(), 1u) << "exactly one 106 for the attack, none later";
  const auto cast_tick = std::stoll(starts[0]->message.at("start_skill").at("tick").get<std::string>());
  EXPECT_EQ(std::stoll(ends[0]->message.at("end_tick").get<std::string>()),
            cast_tick + 700 * 1000000LL);
  EXPECT_NEAR(ends[0]->message.at("dest_pos").at("millimeter_x").get<double>(), 1313.0, 1.0);
  EXPECT_LT(motions[0] - run.packets.data(), ends[0] - run.packets.data());
}

TEST(Combat, ATargetInsideTheStandoffGetsTheDecodedBackwardDestinationCandidate) {
  // FIXTURE: the player stands 600 mm from the monster, inside the 1150 mm standoff. The decoded
  // destination formula (target - direction * standoff) has no special case for this, so the
  // destination lies BEHIND the caster (x 450) and the path runs backward. Pinned as Candidate:
  // no invented skip threshold; only exactly zero displacement is skipped.
  const auto run = run_lunge(lunge_fixture(1500, lunge_block(0, 1900, 5000)), 100, 3400,
                             [](int) { return 1600; });
  const auto motions = attack_motion_packets(run, 64);
  ASSERT_EQ(motions.size(), 1u);
  const auto &motion = motions[0]->message.at("root_motion");
  EXPECT_EQ(motion.at("start_pos").at("millimeter_x"), 1000);
  EXPECT_EQ(motion.at("dest_pos").at("millimeter_x"), 450);
  EXPECT_NEAR(motion.at("scale").get<double>(), 550.0 / 5000.0, 1e-6);
}

// ---- Reviewed-data invariant: NEW full definitions 24048d4f versus the deployed 0d677942. ----
// Run explicitly (skipped, not passed, when unset): CHRONICLES_TUTORIAL_TEST_PROFILE = the OLD
// 0d677942 definitions (as for the baseline test), CHRONICLES_REVIEWED_DEFINITIONS = the reviewed
// 24048d4f definitions and CHRONICLES_REVIEWED_COMBAT = the reviewed combat profile 6f1aae5d.
// The baseline test above stays pinned to the OLD triple and is not touched.

struct StrippedAdditions {
  int cylinder_shapes{};
  int box_shapes{};
  int sphere_shapes{};
  int root_motion_blocks{};
};

// Removes exactly the additive skill data of the reviewed profile (event shapes and stage
// root_motion blocks) and counts it, so what remains can be compared with the old skills.
StrippedAdditions strip_reviewed_skill_additions(Json &skills) {
  StrippedAdditions removed;
  for (auto &skill : skills) {
    for (auto &layer : skill.at("layers")) {
      for (auto &stage : layer.at("stages")) {
        if (stage.contains("root_motion")) {
          stage.erase("root_motion");
          ++removed.root_motion_blocks;
        }
        for (auto &event : stage.at("events")) {
          if (!event.contains("shape")) {
            continue;
          }
          const auto kind = event.at("shape").at("kind").get<std::string>();
          if (kind == "cylinder_sector") {
            ++removed.cylinder_shapes;
          } else if (kind == "box") {
            ++removed.box_shapes;
          } else if (kind == "sphere") {
            ++removed.sphere_shapes;
          }
          event.erase("shape");
        }
      }
    }
  }
  return removed;
}

TEST(Combat, ReviewedDefinitionsOnlyAddTheReviewedCombatDataAndKeepEveryOldBehaviour) {
  const auto *old_path = std::getenv("CHRONICLES_TUTORIAL_TEST_PROFILE");
  const auto *reviewed_definitions_path = std::getenv("CHRONICLES_REVIEWED_DEFINITIONS");
  const auto *reviewed_combat_path = std::getenv("CHRONICLES_REVIEWED_COMBAT");
  if (!old_path || !reviewed_definitions_path || !reviewed_combat_path) {
    GTEST_SKIP() << "Run explicitly with CHRONICLES_TUTORIAL_TEST_PROFILE (old 0d677942), "
                    "CHRONICLES_REVIEWED_DEFINITIONS (24048d4f) and CHRONICLES_REVIEWED_COMBAT "
                    "(6f1aae5d)";
  }
  ASSERT_EQ(distribution::sha256_file(old_path),
            "0d677942b7d1cf5d3836b0b5720ffce122f0f5de59277ce9c7205cc6cdfadf4b");
  ASSERT_EQ(distribution::sha256_file(reviewed_definitions_path),
            "24048d4f3861a39957d10b9c648d1323bdb8d0ef58b0bc7c3cc2387a292eb641");
  ASSERT_EQ(distribution::sha256_file(reviewed_combat_path),
            "6f1aae5d7f2be4ca3b6d033ec0ec14384df1ddabd1656d280b59c8164d574052");
  const auto old_definitions = load_json_file(old_path);
  const auto reviewed_definitions = load_json_file(reviewed_definitions_path);
  const auto reviewed_combat = load_json_file(reviewed_combat_path);
  ASSERT_TRUE(old_definitions && reviewed_definitions && reviewed_combat);

  // (1) Top level: the same keys; only combat and sources differ.
  ASSERT_EQ(old_definitions->size(), reviewed_definitions->size());
  for (const auto &item : old_definitions->items()) {
    ASSERT_TRUE(reviewed_definitions->contains(item.key())) << item.key();
    if (item.key() != "combat" && item.key() != "sources") {
      EXPECT_EQ(item.value(), reviewed_definitions->at(item.key())) << item.key();
    }
  }
  EXPECT_EQ(reviewed_definitions->at("combat"), *reviewed_combat);

  // (2) sources: every old entry unchanged, exactly 47 added.
  const auto &old_sources = old_definitions->at("sources");
  const auto &reviewed_sources = reviewed_definitions->at("sources");
  EXPECT_EQ(reviewed_sources.size(), old_sources.size() + 47);
  for (const auto &item : old_sources.items()) {
    ASSERT_TRUE(reviewed_sources.contains(item.key())) << item.key();
    EXPECT_EQ(item.value(), reviewed_sources.at(item.key())) << item.key();
  }

  // (3) combat: the only new keys are the two collision tables; every old key is unchanged
  // except skills, which equal the old skills once the reviewed additions are removed.
  const auto &old_combat = old_definitions->at("combat");
  for (const auto &item : reviewed_combat->items()) {
    if (!old_combat.contains(item.key())) {
      EXPECT_TRUE(item.key() == "npc_collision" || item.key() == "player_collision") << item.key();
    }
  }
  for (const auto &item : old_combat.items()) {
    ASSERT_TRUE(reviewed_combat->contains(item.key())) << item.key();
    if (item.key() != "skills") {
      EXPECT_EQ(item.value(), reviewed_combat->at(item.key())) << item.key();
    }
  }
  EXPECT_EQ(reviewed_combat->at("npc_collision").size(), 11u);
  EXPECT_EQ(reviewed_combat->at("player_collision").size(), 6u);
  EXPECT_EQ(reviewed_combat->at("npc_skills").size(), 11u);
  EXPECT_EQ(reviewed_combat->at("projectiles").size(), 675u);
  auto old_skills = old_combat.at("skills");
  const auto old_additions = strip_reviewed_skill_additions(old_skills);
  EXPECT_EQ(old_additions.cylinder_shapes + old_additions.box_shapes + old_additions.sphere_shapes +
                old_additions.root_motion_blocks,
            0)
      << "the old combat profile carries none of the reviewed additions";
  auto reviewed_skills = reviewed_combat->at("skills");
  const auto added = strip_reviewed_skill_additions(reviewed_skills);
  EXPECT_EQ(added.cylinder_shapes, 285);
  EXPECT_EQ(added.box_shapes, 412);
  EXPECT_EQ(added.sphere_shapes, 117);
  EXPECT_EQ(added.root_motion_blocks, 20);
  EXPECT_EQ(reviewed_skills, old_combat.at("skills"));

  // (4) Behaviour. Every NPC set casts the same skills at the same ticks with the reviewed combat
  // profile as with the old one, and every 64 motion is identical EXCEPT the skill motions of the
  // lunge skill 190100400 attack instances: for each such instance of the reviewed run exactly
  // the expected new-only 64 exists (destination recomputed from the reviewed data: target backed
  // off by offset + both hit radii, clamped to the max move distance, exactly zero displacement
  // sends none) and the old run has none. All other 64s (pursuit/return, other NPCs and skills)
  // must be equal; no NPC is excluded and no 64 is dropped.
  constexpr int kLungeSkill = 190100400;
  ASSERT_NO_THROW(CombatData{old_combat});
  ASSERT_NO_THROW(CombatData{*reviewed_combat});
  const Json *lunge_block = nullptr;
  for (const auto &skill : reviewed_combat->at("skills")) {
    if (skill.at("id") != kLungeSkill) {
      continue;
    }
    for (const auto &layer : skill.at("layers")) {
      for (const auto &stage : layer.at("stages")) {
        if (stage.contains("root_motion")) {
          lunge_block = &stage.at("root_motion");
        }
      }
    }
  }
  ASSERT_NE(lunge_block, nullptr);
  const double lunge_offset_mm = lunge_block->at("offset_distance_mm").get<double>();
  const double lunge_max_move_mm = lunge_block->at("max_move_distance_mm").get<double>();
  double player_radius_mm = -1;
  for (const auto &row : reviewed_combat->at("player_collision")) {
    const double radius = row.at("game_hit_capsule_radius_mm").get<double>();
    ASSERT_TRUE(player_radius_mm < 0 || player_radius_mm == radius) << "all player models agree";
    player_radius_mm = radius;
  }
  ASSERT_GT(player_radius_mm, 0);

  const auto is_lunge_instance = [](const ProbeResult &result, std::uint64_t instance) {
    return std::ranges::any_of(result.attacks, [&](const AttackRecord &attack) {
      return attack.instance == instance && attack.skill_id == kLungeSkill;
    });
  };
  // The destination the lunge must announce, or none when the displacement is exactly zero.
  const auto expected_destination = [&](const AttackRecord &attack, const Position3 &player,
                                        double npc_radius_mm) -> std::optional<Position3> {
    const double to_x = static_cast<double>(player.x) - attack.position.x;
    const double to_y = static_cast<double>(player.y) - attack.position.y;
    const double to_z = static_cast<double>(player.z) - attack.position.z;
    const double distance = std::sqrt(to_x * to_x + to_y * to_y + to_z * to_z);
    if (distance == 0.0) {
      return std::nullopt;
    }
    const double standoff = lunge_offset_mm + npc_radius_mm + player_radius_mm;
    double move_x = to_x - to_x / distance * standoff;
    double move_y = to_y - to_y / distance * standoff;
    double move_z = to_z - to_z / distance * standoff;
    const double displacement = std::sqrt(move_x * move_x + move_y * move_y + move_z * move_z);
    if (displacement > lunge_max_move_mm) {
      const double clamp = lunge_max_move_mm / displacement;
      move_x *= clamp;
      move_y *= clamp;
      move_z *= clamp;
    }
    const Position3 destination{static_cast<int>(std::llround(attack.position.x + move_x)),
                                static_cast<int>(std::llround(attack.position.y + move_y)),
                                static_cast<int>(std::llround(attack.position.z + move_z))};
    if (destination == attack.position) {
      return std::nullopt;
    }
    return destination;
  };

  for (const auto &item : reviewed_combat->at("npc_skills").items()) {
    const int cms_id = std::stoi(item.key());
    const double npc_radius_mm =
        reviewed_combat->at("npc_collision").at(item.key()).at("game_hit_capsule_radius_mm");
    for (const int distance_mm : {2000, 4000, 5000, 7000, 10000, 15000}) {
      SCOPED_TRACE("npc " + item.key() + " at " + std::to_string(distance_mm) + " mm");
      const auto before = probe_npc(*old_definitions, old_combat, cms_id, distance_mm, 12000);
      const auto after = probe_npc(*old_definitions, *reviewed_combat, cms_id, distance_mm, 12000);
      ASSERT_EQ(before.casts.size(), after.casts.size());
      for (std::size_t index = 0; index < before.casts.size(); ++index) {
        EXPECT_EQ(before.casts[index].skill_id, after.casts[index].skill_id);
        EXPECT_EQ(before.casts[index].tick, after.casts[index].tick);
      }

      std::vector<std::pair<std::int64_t, std::int64_t>> before_other;
      std::vector<std::pair<std::int64_t, std::int64_t>> after_other;
      for (const auto &motion : before.motions) {
        EXPECT_FALSE(is_lunge_instance(before, motion.instance)) << "the old data never lunges";
        before_other.emplace_back(motion.destination.x, motion.destination.y);
      }
      for (const auto &motion : after.motions) {
        if (!is_lunge_instance(after, motion.instance)) {
          after_other.emplace_back(motion.destination.x, motion.destination.y);
        }
      }
      EXPECT_EQ(before_other, after_other) << "every non-lunge 64 is unchanged";

      for (const auto &attack : after.attacks) {
        if (attack.skill_id != kLungeSkill) {
          continue;
        }
        const auto expected = expected_destination(attack, after.player_position, npc_radius_mm);
        const auto lunges = std::ranges::count_if(after.motions, [&](const MotionRecord &motion) {
          return motion.instance == attack.instance;
        });
        if (!expected) {
          EXPECT_EQ(lunges, 0) << "zero displacement sends no 64";
          continue;
        }
        ASSERT_EQ(lunges, 1) << "exactly one lunge 64 for the attack instance";
        const auto motion = std::ranges::find_if(after.motions, [&](const MotionRecord &candidate) {
          return candidate.instance == attack.instance;
        });
        EXPECT_EQ(motion->start, attack.position);
        EXPECT_NEAR(motion->destination.x, expected->x, 1);
        EXPECT_NEAR(motion->destination.y, expected->y, 1);
        EXPECT_NEAR(motion->destination.z, expected->z, 1);
      }
    }
  }
}

} // namespace

std::shared_ptr<TutorialData> local_tutorial_fixture(bool detect_players = false) {
  const auto npc = [detect_players](std::uint64_t id, int cms, bool active) {
    const Json ai{{"search_circle_range_mm", detect_players ? 2000 : 0},
                  {"search_cone_range_mm", detect_players ? 8000 : 0},
                  {"search_half_cone_angle", 90.0},
                  {"hearing_range_mm", 0},
                  {"leash_range_mm", 50000},
                  {"reset_delay_ms", 3000},
                  {"combat_distance_min_mm", 1000},
                  {"combat_distance_max_mm", 2000},
                  {"run_speed_mm_per_second", 3000.0},
                  {"move_speed_rate", 1000}};
    return Json{{"unit_id", std::to_string(id)}, {"cms_id", cms},
                {"behavior", "Monster"},         {"faction", "EX5Faction_Bandit"},
                {"position", {1000, 0, 0}},      {"yaw", 0},
                {"spawner_guid", {"1", "2"}},    {"anim_stance_key", 0},
                {"initially_active", active},    {"ai", ai}};
  };
  return std::make_shared<TutorialData>(
      Json{{"format_version", 1},
           {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
           {"map_id", 8000},
           {"objects", Json::array()},
           {"doodads", Json::array()},
           {"utility_items", Json::array()},
           {"civilians", Json::array()},
           {"sectors", {1}},
           {"npc_spawns", {npc(kMonster, 5001015, true), npc(kInactiveMonster, 5001528, false)}},
           {"quests",
            {{{"cms_id", 10000106},
              {"accept_triggers", Json::array()},
              {"auto_complete", false},
              {"sequential", true},
              {"experience", 0},
              {"rewards", Json::array()},
              {"progresses",
               {{{"sequence", 1},
                 {"group", 1},
                 {"kind", "HuntMonster"},
                 {"parameters", {{"npcId", 5001015}, {"count", 1}}}}}}}}}});
}

Json with_npc_basic_attack(Json definitions) {
  auto npc_skill = definitions["skills"][0];
  npc_skill["id"] = 201;
  npc_skill["npc"] = true;
  npc_skill["classes"] = Json::array();
  npc_skill["costs"] = Json::object();
  definitions["skills"].push_back(npc_skill);
  definitions["npc_skills"]["5001015"] = {201};
  return definitions;
}
TEST(Combat, DefeatObserverRunsOnceAndItsPacketsPrecedeDemiseAndDeath) {
  const auto tutorial_data = local_tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture()), tutorial_data, character(),
                       player_stats());
  std::vector<MonsterDefeat> defeats;
  combat.set_defeat_observer([&](const MonsterDefeat &defeat) {
    defeats.push_back(defeat);
    MonsterDefeatReaction reaction;
    reaction.packets.push_back(
        {173, {{"unit_id", std::to_string(defeat.unit_id)}, {"has_chance_to_loot", true}}});
    return reaction;
  });
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);

  const auto killed = combat.advance(kStart + 300 * kMillisecond, position, tutorial);

  ASSERT_EQ(defeats.size(), 1U);
  EXPECT_EQ(defeats[0].unit_id, kMonster);
  EXPECT_EQ(defeats[0].npc_cms_id, 5001015);
  EXPECT_EQ(defeats[0].killer_character_id, 7);
  EXPECT_EQ(defeats[0].ticks, kStart + 300 * kMillisecond);
  EXPECT_EQ(defeats[0].position.x_mm, 1000);

  // Captured order: the loot announcement, then the Demise state, then the death.
  const auto index_of = [&](std::uint16_t opcode) {
    return std::distance(killed.begin(),
                         std::ranges::find_if(killed, [opcode](const GameplayPacket &packet) {
                           return packet.opcode == opcode;
                         }));
  };
  ASSERT_TRUE(has_opcode(killed, 173));
  EXPECT_LT(index_of(173), index_of(32));
  EXPECT_LT(index_of(32), index_of(23));
  verify_encoding(killed);

  // An observer that does not manage the corpse leaves combat's default removal in place.
  const auto removal_tick = kStart + 300 * kMillisecond + kCorpseRemovalDelayMs * kMillisecond;
  EXPECT_TRUE(has_opcode(combat.advance(removal_tick, position, tutorial), 11));
  EXPECT_EQ(defeats.size(), 1U);
}

TEST(Combat, CorpseRemovalManagedByTheObserverIsNotAnnouncedByCombat) {
  const auto tutorial_data = local_tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(combat_fixture()), tutorial_data, character(),
                       player_stats());
  combat.set_defeat_observer([](const MonsterDefeat &) {
    MonsterDefeatReaction reaction;
    reaction.manages_corpse_removal = true;
    return reaction;
  });
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);
  combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  ASSERT_EQ(combat.monster_hp(kMonster), 0);

  // The observer announces this removal itself, so combat sends none and sets no deadline for it.
  const auto removal_tick = kStart + 300 * kMillisecond + kCorpseRemovalDelayMs * kMillisecond;
  EXPECT_FALSE(has_opcode(combat.advance(removal_tick, position, tutorial), 11));
  EXPECT_FALSE(has_opcode(combat.advance(removal_tick + 600000 * kMillisecond, position, tutorial),
                          11));
}

TEST(Combat, ProximityAggroPursuesAndAttacksWithoutPlayerSkill) {
  const auto tutorial_data = local_tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(with_npc_basic_attack(combat_fixture())),
                       tutorial_data, character(), player_stats());
  WorldPosition position{8000, 10000, 0, 0};
  EXPECT_TRUE(combat.advance(kStart, position, tutorial).empty());

  position.x_mm = 7000;
  const auto acquired = combat.advance(kStart + 250 * kMillisecond, position, tutorial);
  EXPECT_TRUE(has_opcode(acquired, 98));
  EXPECT_TRUE(has_opcode(acquired, 32));
  EXPECT_TRUE(has_opcode(acquired, 114));
  EXPECT_FALSE(has_opcode(acquired, 64));
  EXPECT_TRUE(has_opcode(acquired, 177));
  EXPECT_FALSE(has_opcode(acquired, 14));
  verify_encoding(acquired);

  const auto attack = combat.advance(kStart + 2 * 1000 * kMillisecond, position, tutorial);
  EXPECT_FALSE(has_opcode(attack, 106));
  EXPECT_TRUE(has_opcode(attack, 177));
  EXPECT_TRUE(has_opcode(attack, 14));
  verify_encoding(attack);

  const auto damage = combat.advance(kStart + 2300 * kMillisecond, position, tutorial);
  ASSERT_TRUE(has_opcode(damage, 19));
  EXPECT_LT(combat.player_stats().at("69"), 100);
  verify_encoding(damage);
}

TEST(Combat, RedirectContinuesTheRunningStreamWithoutInventingASkillMotion) {
  const auto tutorial_data = local_tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(with_npc_basic_attack(combat_fixture())),
                       tutorial_data, character(), player_stats());
  WorldPosition position{8000, 7000, 0, 0};
  const auto acquired = combat.advance(kStart, position, tutorial);
  const auto *initial = find_packet(acquired, 177);
  ASSERT_NE(initial, nullptr);
  EXPECT_EQ(initial->message.at("pos").at("millimeter_x"), 1000);

  position.x_mm = 12000;
  const auto redirect = combat.advance(kStart + 100 * kMillisecond, position, tutorial);
  EXPECT_FALSE(has_opcode(redirect, 64));
  EXPECT_FALSE(has_opcode(redirect, 106));
  EXPECT_FALSE(has_opcode(redirect, 177)) << "retain the running cadence through a replan";
  const auto continued = combat.advance(kStart + 200 * kMillisecond, position, tutorial);
  const auto *move = find_packet(continued, 177);
  ASSERT_NE(move, nullptr);
  EXPECT_EQ(move->message.at("detail"), "MoveDetail_Run");
  EXPECT_EQ(move->message.at("pos").at("millimeter_x"), 1600);
  EXPECT_EQ(move->message.at("tick"), std::to_string((kStart + 200 * kMillisecond) * 100));
  verify_encoding(continued);
}

TEST(Combat, ADeadEngagedMonsterReleasesAggroWithoutReceivingAnIdleState) {
  const auto tutorial_data = local_tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(with_npc_basic_attack(combat_fixture())),
                       tutorial_data, character(), player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  ASSERT_TRUE(has_opcode(combat.advance(kStart, position, tutorial), 98));
  combat.receive(20010, start_request(), position, kStart);
  const auto killed = combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 0);

  // Captured order: Demise state, death, then aggro and AI-phase clears.
  const auto monster_id = std::to_string(kMonster);
  const auto position_of = [&](std::uint16_t opcode) {
    return std::ranges::find_if(killed, [&](const GameplayPacket &packet) {
      return packet.opcode == opcode &&
             packet.message.value("unit_id", std::string()) == monster_id;
    });
  };
  const auto death = std::ranges::find_if(
      killed, [](const GameplayPacket &packet) { return packet.opcode == 23; });
  const auto aggro_clear = position_of(98);
  const auto phase_clear = position_of(114);
  ASSERT_TRUE(death != killed.end());
  ASSERT_TRUE(aggro_clear != killed.end());
  ASSERT_TRUE(phase_clear != killed.end());
  EXPECT_LT(std::distance(killed.begin(), death), std::distance(killed.begin(), aggro_clear));
  EXPECT_LT(std::distance(killed.begin(), aggro_clear), std::distance(killed.begin(), phase_clear));
  EXPECT_FALSE(aggro_clear->message.contains("new_target_unit_id"));
  EXPECT_FALSE(phase_clear->message.contains("new_ai_phase_transition_id"));

  bool demise = false;
  for (const auto &packet : killed) {
    if (packet.opcode == 32 && packet.message.at("unit_id") == monster_id) {
      EXPECT_NE(packet.message.at("state"), "EX5UnitStateType_Idle");
      EXPECT_NE(packet.message.at("state"), "EX5UnitStateType_Return");
      demise = demise || packet.message.at("state") == "EX5UnitStateType_Demise";
    }
  }
  EXPECT_TRUE(demise);
  verify_encoding(killed);
}

TEST(Combat, MonsterWithoutAnExecutableAttackRetaliatesButDoesNotProximityAggro) {
  const auto tutorial_data = local_tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  auto definitions = combat_fixture();
  definitions["effects"]["1"]["parameters"]["physicalCoefficient"] = 0.1;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  EXPECT_FALSE(has_opcode(combat.advance(kStart, position, tutorial), 98));
  EXPECT_FALSE(has_opcode(combat.advance(kStart + 250 * kMillisecond, position, tutorial), 98));

  combat.receive(20010, start_request(), position, kStart + 250 * kMillisecond);
  const auto damaged = combat.advance(kStart + 550 * kMillisecond, position, tutorial);
  EXPECT_LT(combat.monster_hp(kMonster), 50);
  EXPECT_TRUE(has_opcode(damaged, 98));
  verify_encoding(damaged);
}

TEST(Combat, EngagedMonsterWithoutAnAttackDoesNotScheduleExpiredAttackDeadlines) {
  const auto tutorial_data = local_tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  auto definitions = combat_fixture();
  definitions["effects"]["1"]["parameters"]["physicalCoefficient"] = 0.1;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};
  combat.receive(20010, start_request(), position, kStart);
  auto engaged = combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  const auto followup = combat.advance(kStart + 500 * kMillisecond, position, tutorial);
  engaged.insert(engaged.end(), followup.begin(), followup.end());
  ASSERT_TRUE(has_opcode(engaged, 98));

  const auto later = kStart + 5000 * kMillisecond;
  combat.advance(later, position, tutorial);
  const auto next = combat.next_action_tick();
  ASSERT_TRUE(next.has_value());
  EXPECT_GT(*next, later);
}

TEST(Combat, NpcEffectWithoutATargetShapeFallsBackToTheSkillRange) {
  const auto tutorial_data = local_tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  auto definitions = with_npc_basic_attack(combat_fixture());
  definitions["effects"]["2"] = {{"kind", "damage"}, {"parameters", Json::object()}, {"chain", 0}};
  definitions["skills"][1]["layers"][0]["stages"][0]["events"][0]["effect_id"] = 2;
  definitions["skills"][1]["range_mm"] = 1500;
  std::optional<CombatSession> combat;
  ASSERT_NO_THROW(combat.emplace(std::make_shared<CombatData>(definitions), tutorial_data,
                                 character(), player_stats()));

  const WorldPosition position{8000, 7000, 0, 0};
  const auto acquired = combat->advance(kStart, position, tutorial);
  EXPECT_FALSE(has_opcode(acquired, 64));
  ASSERT_NE(find_packet(acquired, 177), nullptr);
  const auto arrived = combat->advance(kStart + 1500 * kMillisecond, position, tutorial);
  const auto *stop = find_packet(arrived, 177);
  ASSERT_NE(stop, nullptr);
  EXPECT_EQ(stop->message.at("detail"), "MoveDetail_Stop");
  EXPECT_EQ(stop->message.at("pos").at("millimeter_x"), 5500);
  EXPECT_TRUE(has_opcode(arrived, 14));
}

TEST(Combat, PursuitAndAttackRangesAgreeForVerySmallAttackRadii) {
  const auto tutorial_data = local_tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  auto definitions = with_npc_basic_attack(combat_fixture());
  definitions["effects"]["1"]["targeting"]["outerRadius"] = 0.1;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 7000, 0, 0};
  bool attacked = false;
  for (std::int64_t elapsed_ms = 0; elapsed_ms <= 8000 && !attacked; elapsed_ms += 100) {
    const auto packets = combat.advance(kStart + elapsed_ms * kMillisecond, position, tutorial);
    attacked = has_opcode(packets, 14);
  }
  EXPECT_TRUE(attacked);
}

TEST(Combat, LeashClearsTargetAndRestoresMonsterHealthAfterReturnDelay) {
  const auto tutorial_data = local_tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  auto definitions = with_npc_basic_attack(combat_fixture());
  definitions["effects"]["1"]["parameters"]["physicalCoefficient"] = 0.1;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  WorldPosition position{8000, 0, 0, 0};
  const auto acquired = combat.advance(kStart, position, tutorial);
  EXPECT_TRUE(has_opcode(acquired, 98));
  combat.receive(20010, start_request(), position, kStart);

  const auto damaged = combat.advance(kStart + 300 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 44);
  verify_encoding(damaged);

  const auto monster_state = [](const std::vector<GameplayPacket> &packets) {
    std::optional<std::string> state;
    for (const auto &packet : packets) {
      if (packet.opcode == 32 && packet.message.at("unit_id") == std::to_string(kMonster)) {
        state = packet.message.at("state").get<std::string>();
      }
    }
    return state;
  };

  position.x_mm = 60000;
  const auto cleared = combat.advance(kStart + 301 * kMillisecond, position, tutorial);
  EXPECT_TRUE(has_opcode(cleared, 98));
  EXPECT_EQ(monster_state(cleared), "EX5UnitStateType_Return");
  EXPECT_FALSE(has_opcode(cleared, 14));
  verify_encoding(cleared);

  const auto reset = combat.advance(kStart + 3301 * kMillisecond, position, tutorial);
  EXPECT_EQ(combat.monster_hp(kMonster), 50);
  EXPECT_TRUE(has_opcode(reset, 47));
  EXPECT_EQ(monster_state(reset), "EX5UnitStateType_Idle");
  EXPECT_FALSE(has_opcode(reset, 19));
  verify_encoding(reset);
}

TEST(Combat, LeashCancelsPendingNpcActionBeforeItsDamageEvent) {
  const auto tutorial_data = local_tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(with_npc_basic_attack(combat_fixture())),
                       tutorial_data, character(), player_stats());
  WorldPosition position{8000, 0, 0, 0};
  combat.advance(kStart, position, tutorial);
  const auto started = combat.advance(kStart + 1000 * kMillisecond, position, tutorial);
  ASSERT_TRUE(has_opcode(started, 14));

  position.x_mm = 60000;
  const auto cancelled = combat.advance(kStart + 1001 * kMillisecond, position, tutorial);
  EXPECT_TRUE(has_opcode(cancelled, 98));
  EXPECT_TRUE(has_opcode(cancelled, 15));
  verify_encoding(cancelled);

  const auto former_effect_deadline =
      combat.advance(kStart + 1300 * kMillisecond, position, tutorial);
  EXPECT_FALSE(has_opcode(former_effect_deadline, 19));
  EXPECT_EQ(combat.player_stats().at("69"), 100);
}

// ---- Idle-mob early-out: differential test against the original path (oracle) ----------------

namespace {

Json idle_world_map(bool with_town_npc = false) {
  Json spawns = Json::array();
  std::vector<std::array<int, 2>> positions{{1000, 0}, {30000, 0}, {-4000, 3000},
                                            {9000, 9000}, {1500, -200}};
  if (with_town_npc) {
    positions.push_back({2000, 3000});  // the sixth spawn is the non-hostile town NPC
  }
  // The world-spawn loader (combat_world.cpp, create_world_mobs: `(*id & 15) != 2` throws) needs
  // every unit id to have low nibble 2. The base below ends in ...506 (nibble 2) and each spawn
  // adds a multiple of 16, so the ids stay distinct and valid.
  constexpr std::uint64_t kFirstSpawnUnitId = 576460752303423506ull;
  int index = 0;
  for (const auto &position : positions) {
    ++index;
    const auto unit_id = kFirstSpawnUnitId + (static_cast<std::uint64_t>(index - 1) << 4);
    spawns.push_back({{"unit_id", std::to_string(unit_id)},
                      {"cms_id", 5001015},
                      {"position", {position[0], position[1], 0}},
                      {"yaw", 180.0},
                      {"faction", index == 6 ? "EX5Faction_TownNpc" : "EX5Faction_Bandit"},
                      {"spawner_guid", {std::to_string(index), "2"}},
                      {"anim_stance_key", 0},
                      {"respawn_ms", 2000},
                      {"one_time_use", false}});
  }
  return {{"map_id", 9012}, {"spawns", spawns}};
}

std::uint64_t world_metric_counter(const char *name) {
  return Json::parse(world_metrics::snapshot_json()).at("counters").at(name).get<std::uint64_t>();
}

// Runtime unit ids are random (combat_world.cpp next_world_unit_id uses RAND_bytes), and the world
// keeps its monsters in a std::map ordered by that id. Two worlds built from the same spawns
// therefore hold the same mobs under different ids AND iterate them in different orders, so
// packets and state lines may only be compared by stable identity and as multisets, never by raw
// id or position in the stream. Stable identity = the spawner id the opcode-10 add packet carries
// (unique per spawn in this fixture). Ticks, deadlines, positions and every other value are
// compared verbatim (both worlds share kStart), so a timing or state difference still fails.
class IdLabels {
public:
  // Learns "m<spawner_id_x>" for every NPC announced in these packets (respawned lives of one
  // spawn share the label).
  void learn(const std::vector<GameplayPacket> &packets) {
    for (const auto &packet : packets) {
      if (packet.opcode != 10 || !packet.message.contains("unit_id")) {
        continue;
      }
      const auto &npc = packet.message.at("nub").at("npc");
      if (npc.contains("spawner_id_x")) {
        labels_[packet.message.at("unit_id").get<std::string>()] =
            "m" + npc.at("spawner_id_x").get<std::string>();
      }
    }
  }

  std::string name(const std::string &id) {
    if (id.empty() || id == "0") {
      return id;
    }
    const auto known = labels_.find(id);
    if (known != labels_.end()) {
      return known->second;
    }
    // Not a world mob (a player): ids are deterministic, named by order of first appearance.
    return "p" + std::to_string(fallback_.emplace(id, fallback_.size()).first->second);
  }

  std::string id_for(const std::string &label) const {
    for (const auto &[id, known] : labels_) {
      if (known == label) {
        return id;
      }
    }
    return {};
  }

private:
  std::map<std::string, std::string> labels_;
  std::map<std::string, std::size_t> fallback_;
};

void rename_unit_ids(Json &value, IdLabels &labels) {
  if (value.is_array()) {
    for (auto &item : value) {
      rename_unit_ids(item, labels);
    }
    return;
  }
  if (!value.is_object()) {
    return;
  }
  for (auto &[key, member] : value.items()) {
    if (key.find("unit_id") == std::string::npos) {
      rename_unit_ids(member, labels);
    } else if (member.is_string()) {
      member = labels.name(member.get<std::string>());
    } else if (member.is_array()) {
      for (auto &item : member) {
        if (item.is_string()) {
          item = labels.name(item.get<std::string>());
        }
      }
    }
  }
}

// One line per packet, sorted: the multiset of packets, independent of emission order.
std::string dump_packets(const std::vector<GameplayPacket> &packets, IdLabels &labels) {
  std::vector<std::string> lines;
  for (const auto &packet : packets) {
    auto message = packet.message;
    rename_unit_ids(message, labels);
    lines.push_back(std::to_string(packet.opcode) + " " + message.dump());
  }
  std::sort(lines.begin(), lines.end());
  std::string text;
  for (const auto &line : lines) {
    text += line + "\n";
  }
  return text;
}

// Fingerprint lines start with "<id>:" and carry "own=<id>"; only those two are renamed, and the
// lines are sorted because the world's own order follows the random ids.
std::string renamed_fingerprint(const std::string &fingerprint, IdLabels &labels) {
  std::vector<std::string> lines;
  std::size_t line_start = 0;
  while (line_start < fingerprint.size()) {
    auto line_end = fingerprint.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = fingerprint.size();
    }
    auto line = fingerprint.substr(line_start, line_end - line_start);
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
      line = labels.name(line.substr(0, colon)) + line.substr(colon);
    }
    const auto own = line.find(" own=");
    if (own != std::string::npos) {
      const auto value_start = own + 5;
      const auto value_end = line.find(' ', value_start);
      line = line.substr(0, value_start) +
             labels.name(line.substr(value_start, value_end - value_start)) +
             line.substr(value_end);
    }
    lines.push_back(line);
    line_start = line_end + 1;
  }
  std::sort(lines.begin(), lines.end());
  std::string text;
  for (const auto &line : lines) {
    text += line + "\n";
  }
  return text;
}

// Restores the process-wide switch even when an ASSERT leaves the test early.
class IdleEarlyOutSwitch {
public:
  IdleEarlyOutSwitch() = default;
  IdleEarlyOutSwitch(const IdleEarlyOutSwitch &) = delete;
  IdleEarlyOutSwitch &operator=(const IdleEarlyOutSwitch &) = delete;
  ~IdleEarlyOutSwitch() {
    CombatSession::set_idle_early_out_enabled_for_test(true);
  }
  void set(bool enabled) {
    CombatSession::set_idle_early_out_enabled_for_test(enabled);
  }
};

struct IdleWorld {
  IdleWorld(const Json &definitions, bool with_town_npc) {
    data = std::make_shared<CombatData>(definitions);
    auto other_character = character();
    other_character.id = 23;
    first = std::make_unique<CombatSession>(data, nullptr, character(), player_stats());
    second = std::make_unique<CombatSession>(data, nullptr, other_character, player_stats());
    const auto world = CombatSession::create_world_mobs(idle_world_map(with_town_npc), data);
    first->attach_world_mobs(world);
    second->attach_world_mobs(world);
  }

  std::shared_ptr<CombatData> data;
  std::unique_ptr<CombatSession> first;
  std::unique_ptr<CombatSession> second;
  IdLabels labels;
  std::string killed_id;       // runtime id of the mob of the first spawn, found by identity
  std::string idle_killed_id;  // same for the fourth spawn, which is idle and holds a deadline
};

// One scripted step: both sessions tick; at fixed times a mob is killed (respawn follows) and the
// first player is on another map, far away, near the mobs, or moving between them.
std::string idle_step(IdleWorld &world, int step_ms) {
  WorldPosition first_position{9012, 0, 0, 0};
  if (step_ms >= 800 && step_ms < 1600) {
    first_position.x_mm = static_cast<int>(20000.0 * (step_ms - 800) / 800.0);
  } else if (step_ms >= 1600 && step_ms < 2200) {
    first_position.map_id = 9013;
  } else if (step_ms >= 2200) {
    first_position.x_mm = 1000;
    first_position.y_mm = 500;
  }
  const WorldPosition second_position{9012, 30000, 100, 0};
  const auto ticks = kStart + step_ms * kMillisecond;

  const auto first_packets = world.first->advance_private_encounter(ticks, first_position);
  world.labels.learn(first_packets);
  if (world.killed_id.empty()) {
    world.killed_id = world.labels.id_for("m1");
  }
  if (world.idle_killed_id.empty()) {
    world.idle_killed_id = world.labels.id_for("m4");
  }
  std::vector<GameplayPacket> kill_packets;
  if (step_ms == 1000 && !world.killed_id.empty()) {
    kill_packets = world.first->gm_damage_monster(std::stoull(world.killed_id), 100000, ticks);
    world.labels.learn(kill_packets);
  }
  if (step_ms == 1200 && !world.idle_killed_id.empty()) {
    // The fourth spawn never engages, so at this tick it is Idle with a pending think deadline: a
    // dead mob that still holds a deadline, which the early-out must not skip (reset branch).
    const auto killed =
        world.first->gm_damage_monster(std::stoull(world.idle_killed_id), 100000, ticks);
    kill_packets.insert(kill_packets.end(), killed.begin(), killed.end());
    world.labels.learn(kill_packets);
  }
  const auto second_packets = world.second->advance_private_encounter(ticks, second_position);
  world.labels.learn(second_packets);

  std::string text = "step " + std::to_string(step_ms) + "\nfirst\n";
  text += dump_packets(first_packets, world.labels);
  text += "kill\n" + dump_packets(kill_packets, world.labels);
  text += "second\n" + dump_packets(second_packets, world.labels);
  text += "state first\n" +
          renamed_fingerprint(world.first->monster_state_fingerprint_for_test(), world.labels);
  text += "state second\n" +
          renamed_fingerprint(world.second->monster_state_fingerprint_for_test(), world.labels);
  return text;
}

struct IdleScenario {
  int step_ms = 40;  // 50 and 10 divide the 250 ms idle think interval: ticks land ON deadlines
  bool with_town_npc = false;
  // Lower bound for skipped / (skipped + evaluated) over the guarded run, see the test bodies.
  double minimum_skip_fraction = 0.0;
};

void expect_idle_early_out_matches_the_original_path(const Json &definitions,
                                                     const IdleScenario &scenario) {
  world_metrics::enable();
  IdleEarlyOutSwitch early_out;
  IdleWorld with_guard(definitions, scenario.with_town_npc);
  IdleWorld oracle(definitions, scenario.with_town_npc);
  std::uint64_t skipped_with_guard = 0;
  std::uint64_t evaluated_with_guard = 0;
  std::uint64_t skipped_oracle = 0;
  for (int step_ms = 0; step_ms <= 4000; step_ms += scenario.step_ms) {
    early_out.set(true);
    auto skipped_before = world_metric_counter("mobs_skipped_idle_not_due");
    auto evaluated_before = world_metric_counter("mobs_evaluated");
    const auto guarded = idle_step(with_guard, step_ms);
    skipped_with_guard += world_metric_counter("mobs_skipped_idle_not_due") - skipped_before;
    evaluated_with_guard += world_metric_counter("mobs_evaluated") - evaluated_before;

    early_out.set(false);
    skipped_before = world_metric_counter("mobs_skipped_idle_not_due");
    const auto original = idle_step(oracle, step_ms);
    skipped_oracle += world_metric_counter("mobs_skipped_idle_not_due") - skipped_before;

    ASSERT_EQ(guarded, original) << "diverged at " << step_ms << " ms";
  }
  early_out.set(true);
  ::testing::Test::RecordProperty("skipped_with_guard", static_cast<int>(skipped_with_guard));
  ::testing::Test::RecordProperty("evaluated_with_guard", static_cast<int>(evaluated_with_guard));
  EXPECT_GT(skipped_with_guard, 0u) << "the early-out never fired: the comparison would be vacuous";
  EXPECT_EQ(skipped_oracle, 0u);
  const auto considered = skipped_with_guard + evaluated_with_guard;
  ASSERT_GT(considered, 0u);
  EXPECT_GE(static_cast<double>(skipped_with_guard) / static_cast<double>(considered),
            scenario.minimum_skip_fraction)
      << "skipped " << skipped_with_guard << " of " << considered;
  EXPECT_FALSE(with_guard.killed_id.empty());
  EXPECT_FALSE(oracle.killed_id.empty());
  EXPECT_FALSE(with_guard.idle_killed_id.empty());
  EXPECT_FALSE(oracle.idle_killed_id.empty());
}

// Skip-fraction thresholds (a guess to be tightened from the recorded properties, chosen as lower
// bounds from the script): mobs with no usable attack never perceive, so they stay Idle and
// think once per 250 ms; two sessions tick every step, so at 40 ms about 12 of 13 calls per mob
// are not due, minus the two killed mobs that are dead for 2 s each (a dead mob always takes
// the full path). 0.5 is far below that. With attacks, up to four of five mobs may be engaged
// (owned mobs are skipped by the other session, not counted) and only the idle ones are
// skipped; the idle fourth spawn alone gives roughly 30 percent before its own 2 s death, about
// 15 percent after, so 0.10 only proves the early-out fires on a real share of mob ticks.
Json idle_attack_definitions() {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  return definitions;
}

Json idle_no_attack_definitions() {
  auto definitions = combat_fixture();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  return definitions;
}

} // namespace

TEST(WorldMobsIdleEarlyOut, MatchesTheOriginalPathForMobsWithAnAttack) {
  expect_idle_early_out_matches_the_original_path(idle_attack_definitions(), {40, false, 0.10});
}

TEST(WorldMobsIdleEarlyOut, MatchesTheOriginalPathForMobsWithoutAnyUsableAttack) {
  expect_idle_early_out_matches_the_original_path(idle_no_attack_definitions(), {40, false, 0.50});
}

// Steps of 50 ms and 10 ms divide the 250 ms idle think interval, so the session ticks exactly at
// each mob's next_action_tick(): with `<=` instead of `<` in NpcAi::idle_and_not_due the due think
// is skipped at its own deadline and runs one step later, which moves the next deadline (`due=` in
// the state fingerprint) and fails these tests. The 40 ms script never lands on a deadline.
TEST(WorldMobsIdleEarlyOut, StepsExactlyOnIdleDeadlinesForMobsWithAnAttack) {
  expect_idle_early_out_matches_the_original_path(idle_attack_definitions(), {50, false, 0.10});
  expect_idle_early_out_matches_the_original_path(idle_attack_definitions(), {10, false, 0.10});
}

TEST(WorldMobsIdleEarlyOut, StepsExactlyOnIdleDeadlinesForMobsWithoutAnyUsableAttack) {
  expect_idle_early_out_matches_the_original_path(idle_no_attack_definitions(), {50, false, 0.50});
}

// A sixth spawn is a non-hostile town NPC. NpcAi::advance resets it on every call, so it never
// holds a deadline and the early-out cannot reach it; the case pins that it is never skipped
// and that its state matches. It does not kill a "drop the hostile check" mutant by itself, since
// hostility cannot change after spawn in this fixture.
TEST(WorldMobsIdleEarlyOut, NonHostileTownNpcIsNeverSkippedAndMatches) {
  expect_idle_early_out_matches_the_original_path(idle_attack_definitions(), {50, true, 0.10});
}

TEST(NpcAiIdleAndNotDue, IsTrueOnlyForAnIdleMobWhoseDeadlineHasNotArrivedOnTheHomeMap) {
  NpcAi ai(*NpcAiDefinition::from_json(npc_ai_block()), WorldPosition{9012, 0, 0, 0});
  EXPECT_FALSE(ai.idle_and_not_due(kStart, 9012));  // no deadline yet

  NpcAiInput input;
  input.player_position = WorldPosition{9012, 100000, 0, 0};
  input.ticks = kStart;
  input.player_alive = true;
  input.monster_active = true;
  input.monster_alive = true;
  input.monster_hostile = true;
  ai.advance(input);
  ASSERT_TRUE(ai.next_action_tick().has_value());
  const auto due = *ai.next_action_tick();

  EXPECT_TRUE(ai.idle_and_not_due(due - 1, 9012));
  EXPECT_FALSE(ai.idle_and_not_due(due, 9012));  // `<`, not `<=`
  EXPECT_FALSE(ai.idle_and_not_due(due + 1, 9012));
  EXPECT_FALSE(ai.idle_and_not_due(due - 1, 9013));  // other map: the reset branch must run
}

// Player skill ids of the skill-special fixtures below.
constexpr int kDodgeSkill = 210;
constexpr int kShieldSkill = 220;
constexpr int kSlotSwapSkill = 230;
constexpr int kCooldownResetSkill = 240;
constexpr std::uint64_t kSpecialGmMonster = 0x3000000000000012ULL;
constexpr std::uint64_t kPlayerUnit = 7;

// Adds a player skill with one stage of `duration_ms` and the given events and notifies.
void add_player_skill(Json &definitions, int skill_id, const char *type, int duration_ms,
                      Json events, Json notifies) {
  Json skill = definitions.at("skills").at(0);
  skill["id"] = skill_id;
  skill["type"] = type;
  skill["cooldown_ms"] = 0;
  auto &stage = skill["layers"][0]["stages"][0];
  stage["duration_ms"] = duration_ms;
  stage["events"] = std::move(events);
  stage["notifies"] = std::move(notifies);
  definitions["skills"].push_back(std::move(skill));
}

// A self-targeted effect row of the given kind, as the catalog writes refTarget Self effects.
Json self_effect(const char *kind, Json parameters) {
  return {{"kind", kind},
          {"parameters", std::move(parameters)},
          {"targeting_kind", "refTarget"},
          {"targeting",
           {{"targetType1", "EX5CollisionTargetType_Ally"},
            {"maxTargetCount", 1},
            {"refType", "EX5ReferenceTargetType_Self"}}},
          {"chain", 0}};
}

// GM monster 5001015 with one melee attack (skill 201, hit at 300 ms) and a player dodge whose
// immune window covers its first 1.5 s with Immune row 1, named as `immune_names`.
Json skill_special_fixture(const Json &immune_names) {
  auto definitions = combat_fixture_with_npc_attack();
  definitions["npc_locomotion"] = {{"5001015", npc_ai_block()}};
  const Json dodge_window{{"notifyType", "immune"},
                          {"startTime", 0},
                          {"endTime", 1500},
                          {"immune", {{"immuneCmsId", 1}}}};
  add_player_skill(definitions, kDodgeSkill, "Dodge", 2000, Json::array(),
                   Json::array({dodge_window}));
  definitions["immunities"] = {{"1", immune_names}};
  return definitions;
}

Json player_skill_request(int skill_id, std::uint32_t instance) {
  auto request = start_request(instance);
  request["start_skill"]["skill_id"] = skill_id;
  return request;
}

TEST(WorldMobs, CandidateProjectileRetryPreservesImmunityAtOriginalImpactTick) {
  auto definitions = combat_fixture_with_stun_projectile();
  const Json immunity_window{{"notifyType", "immune"},
                             {"startTime", 0},
                             {"endTime", 125},
                             {"immune", {{"immuneCmsId", 1}}}};
  add_player_skill(definitions, kDodgeSkill, "Dodge", 200, Json::array(),
                   Json::array({immunity_window}));
  definitions["immunities"] = {{"1", Json::array({"Dodge"})}};

  for (const bool inject_failure : {false, true}) {
    SCOPED_TRACE(inject_failure ? "retry after preflight failure" : "no failure control");
    const auto data = std::make_shared<CombatData>(definitions);
    const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
    CombatSession target(data, nullptr, character(), player_stats());
    target.attach_world_mobs(world);
    const WorldPosition position{9012, 0, 0, 0};

    target.advance_private_encounter(kStart, position);
    target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
    ASSERT_NE(find_packet(target.advance_private_encounter(kStart + 1300 * kMillisecond, position),
                          40), nullptr);
    const auto flight = target.earliest_projectile_work();
    ASSERT_TRUE(flight);
    ASSERT_EQ(flight->deadline, kStart + 2300 * kMillisecond);
    const auto dodge = target.receive(kStartSkillRequest, player_skill_request(kDodgeSkill, 1),
                                      position, kStart + 2250 * kMillisecond);
    ASSERT_NE(find_packet(dodge, kStartSkillNotification), nullptr);

    if (inject_failure) {
      target.set_projectile_preflight_hook_for_test([] {
        throw std::runtime_error("Injected projectile preflight allocation failure");
      });
    }
    const auto due = target.advance_private_encounter(kStart + 2400 * kMillisecond, position);
    target.set_projectile_preflight_hook_for_test({});
    EXPECT_EQ(find_packet(due, 19), nullptr);
    if (!inject_failure) {
      ASSERT_NE(find_packet(due, 20), nullptr);
      EXPECT_EQ(target.player_stats().at("69"), 100);
      continue;
    }

    EXPECT_EQ(target.earliest_projectile_work(), flight);
    const auto expired = target.advance_private_encounter(kStart + 2450 * kMillisecond, position);
    EXPECT_EQ(find_packet(expired, 19), nullptr);
    const auto retried = target.advance_private_encounter(kStart + 2500 * kMillisecond, position);
    // Candidate compatibility assertion: an impact due during immunity must retain that outcome.
    EXPECT_NE(find_packet(retried, 20), nullptr);
    EXPECT_EQ(find_packet(retried, 19), nullptr);
    EXPECT_EQ(target.player_stats().at("69"), 100);
  }
}

TEST(WorldMobs, RetiredDodgeImmunityBelongsOnlyToFlightsInsideItsLifetime) {
  auto definitions = combat_fixture_with_stun_projectile();
  const Json immunity_window{{"notifyType", "immune"},
                             {"startTime", 0},
                             {"endTime", 200},
                             {"immune", {{"immuneCmsId", 1}}}};
  add_player_skill(definitions, kDodgeSkill, "Dodge", 200, Json::array(),
                   Json::array({immunity_window}));
  definitions["immunities"] = {{"1", Json::array({"Dodge"})}};

  struct Scenario {
    const char *name;
    std::int64_t dodge_start;
    std::optional<std::int64_t> cancellation;
    bool immune;
  };
  const std::array scenarios{
      Scenario{"natural expiry before late poll", 2250 * kMillisecond, std::nullopt, true},
      Scenario{"cancel before impact", 2250 * kMillisecond, 2275 * kMillisecond, false},
      Scenario{"cancel after impact", 2250 * kMillisecond, 2325 * kMillisecond, true},
      Scenario{"cancel at impact", 2250 * kMillisecond, 2300 * kMillisecond, false},
      Scenario{"new dodge after impact", 2300 * kMillisecond + 1, std::nullopt, false},
      Scenario{"stage ends at impact", 2100 * kMillisecond, std::nullopt, false}};

  for (const auto &scenario : scenarios) {
    SCOPED_TRACE(scenario.name);
    const auto data = std::make_shared<CombatData>(definitions);
    const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
    CombatSession target(data, nullptr, character(), player_stats());
    target.attach_world_mobs(world);
    const WorldPosition position{9012, 0, 0, 0};
    target.advance_private_encounter(kStart, position);
    target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
    ASSERT_NE(
        find_packet(target.advance_private_encounter(kStart + 1300 * kMillisecond, position), 40),
        nullptr);
    const auto dodge = target.receive(kStartSkillRequest, player_skill_request(kDodgeSkill, 1),
                                      position, kStart + scenario.dodge_start);
    ASSERT_NE(find_packet(dodge, kStartSkillNotification), nullptr);
    if (scenario.cancellation) {
      const auto cancelled =
          target.gm_cancel_skill(character().id, position, kStart + *scenario.cancellation);
      ASSERT_NE(find_packet(cancelled, kEndSkillNotification), nullptr);
    }

    const auto impact = target.advance_private_encounter(kStart + 2500 * kMillisecond, position);
    EXPECT_EQ(find_packet(impact, 20) != nullptr, scenario.immune);
    EXPECT_EQ(find_packet(impact, 19) != nullptr, !scenario.immune);
    EXPECT_EQ(target.player_stats().at("69"), scenario.immune ? 100 : 90);
    verify_encoding(impact);
  }
}

TEST(WorldMobs, ProjectileImmunitySurvivesAutomaticAndRequestedStageRetirement) {
  auto definitions = combat_fixture_with_stun_projectile();
  const Json immunity_window{{"notifyType", "immune"},
                             {"startTime", 0},
                             {"endTime", 200},
                             {"immune", {{"immuneCmsId", 1}}}};
  const Json input_window{{"notifyType", "input_condition"},
                          {"startTime", 0},
                          {"endTime", 200},
                          {"inputCondition", {{"nextStageIndex", 1}, {"inputEvent", 3}}}};
  definitions["skills"].push_back(player_skill(
      definitions, kMultiStageSkill,
      Json::array(
          {action_stage(0, 200, 1, Json::array(), Json::array({immunity_window, input_window})),
           action_stage(1, 1000, -1, Json::array(), Json::array())})));
  definitions["immunities"] = {{"1", Json::array({"Dodge"})}};

  for (const int transition_ms : {0, 2275, 2325}) {
    SCOPED_TRACE(transition_ms);
    const auto data = std::make_shared<CombatData>(definitions);
    const auto world = CombatSession::create_world_mobs(world_mob_map(), data);
    CombatSession target(data, nullptr, character(), player_stats());
    target.attach_world_mobs(world);
    const WorldPosition position{9012, 0, 0, 0};
    target.advance_private_encounter(kStart, position);
    target.advance_private_encounter(kStart + 1000 * kMillisecond, position);
    ASSERT_NE(
        find_packet(target.advance_private_encounter(kStart + 1300 * kMillisecond, position), 40),
        nullptr);
    ASSERT_TRUE(
        accepted_start(target.receive(kStartSkillRequest, skill_start_request(kMultiStageSkill, 1),
                                      position, kStart + 2250 * kMillisecond)));
    if (transition_ms != 0) {
      const auto changed =
          target.receive(kChangeSkillStageRequest, stage_request(1, 1, "input_condition"), position,
                         kStart + transition_ms * kMillisecond);
      ASSERT_TRUE(changed.empty());
    }

    const auto impact = target.advance_private_encounter(kStart + 2500 * kMillisecond, position);
    const bool immune = transition_ms != 2275;
    EXPECT_EQ(find_packet(impact, 20) != nullptr, immune);
    EXPECT_EQ(find_packet(impact, 19) != nullptr, !immune);
    EXPECT_EQ(target.player_stats().at("69"), immune ? 100 : 90);
    verify_encoding(impact);
  }
}

// Spawns the GM monster 1.5 m in front of the player and advances until 900 ms after its first
// attack started; `on_npc_start` runs at the tick the monster's StartSkillNoti is produced.
std::vector<GameplayPacket>
run_first_npc_attack(CombatSession &combat,
                     const std::function<void(std::int64_t, std::vector<GameplayPacket> &)>
                         &on_npc_start = {}) {
  const WorldPosition player{9012, 0, 0, 0};
  EXPECT_TRUE(combat.add_gm_monster(kSpecialGmMonster, 5001015, {9012, 1500, 0, 0, 180.0F}));

  std::vector<GameplayPacket> packets;
  std::optional<std::int64_t> npc_started;
  for (int step = 0; step <= 60; ++step) {
    const auto ticks = kStart + step * 50 * kMillisecond;
    if (npc_started && ticks > *npc_started + 900 * kMillisecond) {
      break;
    }
    auto due = combat.advance_private_encounter(ticks, player);
    const bool started_now = !npc_started && find_packet(due, kStartSkillNotification) != nullptr;
    packets.insert(packets.end(), due.begin(), due.end());
    if (started_now) {
      npc_started = ticks;
      if (on_npc_start) {
        on_npc_start(ticks, packets);
      }
    }
  }
  EXPECT_TRUE(npc_started.has_value());
  return packets;
}

std::ptrdiff_t first_index(const std::vector<GameplayPacket> &packets, std::uint16_t opcode,
                           std::uint64_t target_unit) {
  const auto found = std::ranges::find_if(packets, [&](const GameplayPacket &packet) {
    return packet.opcode == opcode &&
           packet.message.value("target_unit_id", std::string{}) == std::to_string(target_unit);
  });
  return found == packets.end() ? -1 : std::distance(packets.begin(), found);
}

// The ChangeStatNoti that reports stat 71 (the shield), not a regeneration or cost change.
const GameplayPacket *find_shield_stat_change(const std::vector<GameplayPacket> &packets) {
  const auto found = std::ranges::find_if(packets, [](const GameplayPacket &packet) {
    return packet.opcode == 47 && packet.message.at("stat_changes").size() == 1 &&
           packet.message.at("stat_changes").at(0).at("stat_type") == 71;
  });
  return found == packets.end() ? nullptr : &*found;
}

TEST(Combat, PlayerDodgeWindowCancelsAMonsterHitWithDodgeImmunity) {
  CombatSession combat(std::make_shared<CombatData>(skill_special_fixture(Json::array({"Dodge"}))),
                       nullptr, character(), player_stats());
  const WorldPosition player{9012, 0, 0, 0};

  const auto packets = run_first_npc_attack(
      combat, [&](std::int64_t ticks, std::vector<GameplayPacket> &out) {
        const auto started =
            combat.receive(kStartSkillRequest, player_skill_request(kDodgeSkill, 1), player, ticks);
        ASSERT_NE(find_packet(started, kStartSkillNotification), nullptr);
        out.insert(out.end(), started.begin(), started.end());
      });

  const auto immune_index = first_index(packets, 20, kPlayerUnit);
  ASSERT_GE(immune_index, 0);
  const auto &immune = packets[static_cast<std::size_t>(immune_index)].message;
  EXPECT_EQ(immune.at("dodge"), true);
  EXPECT_EQ(immune.at("caster_unit_id"), std::to_string(kSpecialGmMonster));
  EXPECT_EQ(immune.at("skill_cms_id"), 201);
  EXPECT_EQ(first_index(packets, 19, kPlayerUnit), -1);
  EXPECT_EQ(combat.player_stats().at("69"), 100);
  verify_encoding(packets);
}

TEST(Combat, MonsterHitLandsWithoutTheDodgeAndOutsideTheWindow) {
  // Negative control: the same encounter without a dodge hits the player.
  CombatSession combat(std::make_shared<CombatData>(skill_special_fixture(Json::array({"Dodge"}))),
                       nullptr, character(), player_stats());

  const auto packets = run_first_npc_attack(combat);

  EXPECT_EQ(first_index(packets, 20, kPlayerUnit), -1);
  EXPECT_GE(first_index(packets, 19, kPlayerUnit), 0);
  EXPECT_LT(combat.player_stats().at("69"), 100);
}

TEST(Combat, ReactionImmunityKeepsTheDamageAndAnnouncesTheFirstReactionLevelFirst) {
  auto definitions = skill_special_fixture(Json::array({"Sway", "Knockback"}));
  definitions["effects"]["1"]["parameters"]["reactionType"] = "EX5ReactionType_Sway2";
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  const WorldPosition player{9012, 0, 0, 0};

  const auto packets = run_first_npc_attack(
      combat, [&](std::int64_t ticks, std::vector<GameplayPacket> &out) {
        const auto started =
            combat.receive(kStartSkillRequest, player_skill_request(kDodgeSkill, 1), player, ticks);
        out.insert(out.end(), started.begin(), started.end());
      });

  const auto immune_index = first_index(packets, 20, kPlayerUnit);
  const auto damage_index = first_index(packets, 19, kPlayerUnit);
  ASSERT_GE(immune_index, 0);
  ASSERT_GE(damage_index, 0);
  EXPECT_LT(immune_index, damage_index);
  const auto &immune = packets[static_cast<std::size_t>(immune_index)].message;
  EXPECT_EQ(immune.at("reaction"), "EX5ReactionType_Sway1");
  EXPECT_FALSE(immune.contains("dodge"));
  EXPECT_LT(combat.player_stats().at("69"), 100);
  verify_encoding(packets);
}

TEST(Combat, ShieldAbsorbsAMonsterHitBeforeHealthAndExpires) {
  auto definitions = skill_special_fixture(Json::array({"Dodge"}));
  definitions["immunities"]["4"] = Json::array({"Sway", "Knockback", "KnockDown", "BeGrasped"});
  definitions["shields"] = {{"3006",
                             {{"duration_ms", 5000},
                              {"amount", 0},
                              {"hp_per_remain_milli", 0},
                              {"hp_per_total_milli", 100},
                              {"immune_id", "4"}}}};
  definitions["status_effects"] = {
      {"100014", {{"duration_ms", 5000}, {"stack_rule", "ResetDuration"}}}};
  auto shield_effect = self_effect("addShield", {{"shieldId", 3006}});
  shield_effect["status_effect_id"] = 100014;
  definitions["effects"]["5"] = shield_effect;
  definitions["effects"]["1"]["parameters"]["reactionType"] = "EX5ReactionType_Knockback3";
  add_player_skill(definitions, kShieldSkill, "Normal", 400,
                   {{{"effect_id", 5}, {"unique_id", 0}, {"time_ms", 0}}}, Json::array());
  CombatSession combat(std::make_shared<CombatData>(definitions), nullptr, character(),
                       player_stats());
  const WorldPosition player{9012, 0, 0, 0};
  ASSERT_TRUE(combat.add_gm_monster(kSpecialGmMonster + 16, 5001015, {9012, 60000, 0, 0}));

  // 10% of the 100 maximum health, reported as stat 71 before the shield's status.
  combat.receive(kStartSkillRequest, player_skill_request(kShieldSkill, 1), player, kStart);
  const auto granted = combat.advance_private_encounter(kStart, player);
  const auto *stat_change = find_shield_stat_change(granted);
  ASSERT_NE(stat_change, nullptr);
  EXPECT_EQ(stat_change->message.at("stat_changes").at(0).at("result"), 10);
  EXPECT_EQ(stat_change->message.at("default_source"), "SCS_StatusEffect");
  const auto status = std::ranges::find_if(
      granted, [](const GameplayPacket &packet) { return packet.opcode == 49; });
  ASSERT_NE(status, granted.end());
  EXPECT_LT(stat_change, &*status);
  EXPECT_EQ(combat.player_stats().at("71"), 10);

  // The monster's hit spends the shield first; the shield also suppresses the knockback.
  const auto hit = run_first_npc_attack(combat);
  const auto damage_index = first_index(hit, 19, kPlayerUnit);
  ASSERT_GE(damage_index, 0);
  const auto &damage = hit[static_cast<std::size_t>(damage_index)].message;
  const int base = damage.at("base_damage_amount");
  const int final_damage = damage.at("final_damage_amount");
  const int absorbed = std::min(base, 10);
  EXPECT_EQ(final_damage, base - absorbed);
  ASSERT_EQ(damage.at("stat_changes").size(), 2u);
  EXPECT_EQ(damage.at("stat_changes").at(1).at("stat_type"), 71);
  EXPECT_EQ(damage.at("stat_changes").at(1).at("delta"), -absorbed);
  EXPECT_EQ(combat.player_stats().at("69"), 100 - final_damage);
  EXPECT_EQ(combat.player_stats().at("71"), 10 - absorbed);
  if (final_damage == 0) {
    EXPECT_EQ(damage.at("damage_type"), "EX5DamageType_Shield");
  }
  const auto immune_index = first_index(hit, 20, kPlayerUnit);
  ASSERT_GE(immune_index, 0);
  EXPECT_EQ(hit[static_cast<std::size_t>(immune_index)].message.at("reaction"),
            "EX5ReactionType_Knockback1");
  verify_encoding(granted);
  verify_encoding(hit);
}

TEST(Combat, ShieldExpiryClearsStat71AndStopsItsImmunity) {
  auto definitions = combat_fixture();
  definitions["immunities"] = {{"4", Json::array({"Sway"})}};
  definitions["shields"] = {{"3006",
                             {{"duration_ms", 5000},
                              {"amount", 25},
                              {"hp_per_remain_milli", 0},
                              {"hp_per_total_milli", 0},
                              {"immune_id", "4"}}}};
  definitions["effects"]["5"] = self_effect("addShield", {{"shieldId", 3006}});
  add_player_skill(definitions, kShieldSkill, "Normal", 400,
                   {{{"effect_id", 5}, {"unique_id", 0}, {"time_ms", 0}}}, Json::array());
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  combat.receive(kStartSkillRequest, player_skill_request(kShieldSkill, 1), position, kStart);
  combat.advance(kStart, position, tutorial);
  ASSERT_EQ(combat.player_stats().at("71"), 25);
  const auto before_expiry = combat.advance(kStart + 4999 * kMillisecond, position, tutorial);
  EXPECT_EQ(find_shield_stat_change(before_expiry), nullptr);
  EXPECT_EQ(combat.player_stats().at("71"), 25);
  ASSERT_TRUE(combat.next_action_tick());
  EXPECT_LE(*combat.next_action_tick(), kStart + 5000 * kMillisecond);

  const auto expired = combat.advance(kStart + 5000 * kMillisecond, position, tutorial);
  const auto *cleared = find_shield_stat_change(expired);
  ASSERT_NE(cleared, nullptr);
  EXPECT_FALSE(cleared->message.at("stat_changes").at(0).contains("result"));
  EXPECT_EQ(combat.player_stats().at("71"), 0);
  verify_encoding(expired);
}

TEST(Combat, DispelShieldRemovesTheShieldAtOnce) {
  auto definitions = combat_fixture();
  definitions["shields"] = {{"1",
                             {{"duration_ms", 10000},
                              {"amount", 30},
                              {"hp_per_remain_milli", 0},
                              {"hp_per_total_milli", 0},
                              {"immune_id", ""}}}};
  definitions["effects"]["5"] = self_effect("addShield", {{"shieldId", 1}});
  definitions["effects"]["6"] = self_effect("dispelShield", Json::object());
  add_player_skill(definitions, kShieldSkill, "Normal", 400,
                   {{{"effect_id", 5}, {"unique_id", 0}, {"time_ms", 0}},
                    {{"effect_id", 6}, {"unique_id", 1}, {"time_ms", 200}}},
                   Json::array());
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  combat.receive(kStartSkillRequest, player_skill_request(kShieldSkill, 1), position, kStart);
  combat.advance(kStart, position, tutorial);
  ASSERT_EQ(combat.player_stats().at("71"), 30);
  const auto dispelled = combat.advance(kStart + 200 * kMillisecond, position, tutorial);
  ASSERT_NE(find_shield_stat_change(dispelled), nullptr);
  EXPECT_EQ(combat.player_stats().at("71"), 0);
  verify_encoding(dispelled);
}

TEST(Combat, ChangeWeaponSkillSlotAnnouncesTheSwapUntilItsCatalogDuration) {
  auto definitions = combat_fixture();
  auto swap = self_effect("changeWeaponSkillSlot", Json::object());
  swap["duration_ms"] = 20000;
  definitions["effects"]["160300"] = swap;
  add_player_skill(definitions, kSlotSwapSkill, "Normal", 1000,
                   {{{"effect_id", 160300}, {"unique_id", 0}, {"time_ms", 250}}}, Json::array());
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  combat.receive(kStartSkillRequest, player_skill_request(kSlotSwapSkill, 1), position, kStart);
  const auto swapped = combat.advance(kStart + 250 * kMillisecond, position, tutorial);

  const auto *notification = find_packet(swapped, 96);
  ASSERT_NE(notification, nullptr);
  EXPECT_EQ(notification->message.at("skill_effect_cms_id"), 160300);
  EXPECT_EQ(notification->message.at("expire_tick"),
            wire_monotonic_tick(kStart + (250 + 20000) * kMillisecond));
  verify_encoding(swapped);
}

TEST(Combat, SkillCooldownResetReadiesTheNamedSkillAndAnnouncesTheEffect) {
  auto definitions = combat_fixture();
  definitions["effects"]["110328"] =
      self_effect("skillCooldownReset", Json::object());
  definitions["effects"]["110328"]["cooldown_reset_skill_ids"] = Json::array({200});
  add_player_skill(definitions, kCooldownResetSkill, "Normal", 400,
                   {{{"effect_id", 110328}, {"unique_id", 0}, {"time_ms", 100}}}, Json::array());
  const auto tutorial_data = tutorial_fixture();
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  // Enough stamina for three casts inside the cooldown.
  auto stats = player_stats();
  stats["19"] = 200;
  stats["72"] = 200;
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       stats);
  const WorldPosition position{8000, 0, 0, 0};

  // Skill 200 has a 1.5 s cooldown; a second cast 1 s later is refused until the reset.
  combat.receive(kStartSkillRequest, start_request(1), position, kStart);
  combat.advance(kStart + 1000 * kMillisecond, position, tutorial);
  const auto refused =
      combat.receive(kStartSkillRequest, start_request(2), position, kStart + 1000 * kMillisecond);
  ASSERT_NE(find_packet(refused, kStartSkillFailure), nullptr);
  EXPECT_EQ(find_packet(refused, kStartSkillFailure)->message.at("ec"), "Skill_Cooldown");

  combat.receive(kStartSkillRequest, player_skill_request(kCooldownResetSkill, 3), position,
                 kStart + 1000 * kMillisecond);
  const auto reset = combat.advance(kStart + 1100 * kMillisecond, position, tutorial);
  const auto *notification = find_packet(reset, 22);
  ASSERT_NE(notification, nullptr);
  EXPECT_EQ(notification->message.at("skill_effect_cms_id"), 110328);
  combat.advance(kStart + 1400 * kMillisecond, position, tutorial);

  const auto ready =
      combat.receive(kStartSkillRequest, start_request(4), position, kStart + 1400 * kMillisecond);
  EXPECT_NE(find_packet(ready, kStartSkillNotification), nullptr);
  verify_encoding(reset);
}

TEST(Combat, ImmunityAndShieldTablesRejectDanglingReferences) {
  auto definitions = combat_fixture();
  definitions["shields"] = {{"1",
                             {{"duration_ms", 1000},
                              {"amount", 1},
                              {"hp_per_remain_milli", 0},
                              {"hp_per_total_milli", 0},
                              {"immune_id", "9"}}}};
  EXPECT_THROW(CombatData{definitions}, std::runtime_error);

  definitions["immunities"] = {{"9", Json::array({"Sway"})}};
  EXPECT_NO_THROW(CombatData{definitions});

  definitions["immunities"] = {{"9", Json::array({"Sway", 3})}};
  EXPECT_THROW(CombatData{definitions}, std::runtime_error);
}


// PC2S_ExecuteSkillEffectReq for the interval-area fixture's event (effect 1, unique id 0).
Json area_effect_report(std::optional<std::uint64_t> target, std::optional<Json> aim) {
  Json effect{{"effect_id", 1}, {"unique_effect_id", 0}};
  if (target) {
    effect["targets"] = Json::array({{{"unit_id", std::to_string(*target)}}});
  }
  if (aim) {
    effect["aim_position"] = *aim;
  }
  return {{"skill_instance_id", 1},
          {"pos", millimetres(0, 0, 0)},
          {"rotation", {{"yaw", 0}}},
          {"tick", "999999999999"},
          {"skill_effect", effect}};
}

const GameplayPacket *find_area_announcement(const std::vector<GameplayPacket> &packets) {
  const auto found = std::ranges::find_if(packets, [](const GameplayPacket &packet) {
    return packet.opcode == 10 && packet.message.at("nub").contains("skill_object");
  });
  return found == packets.end() ? nullptr : &*found;
}

TEST(Combat, UntargetedPlayerAreaWaitsForTheReportedTargetInsteadOfLandingOnTheCaster) {
  auto definitions = interval_area_fixture();
  const auto tutorial_data = tutorial_fixture(true);
  tutorial_data->npcs.at(kInactiveMonster)["position"] = {0, 2000, 0};
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  // No target in the cast: the MainTarget area waits for the client's report.
  combat.receive(20010, start_request(), position, kStart);
  EXPECT_EQ(find_area_announcement(combat.advance(kStart + 300 * kMillisecond, position, tutorial)),
            nullptr);
  ASSERT_TRUE(combat.next_action_tick());
  EXPECT_EQ(*combat.next_action_tick(), kStart + 600 * kMillisecond);

  // The report names the unit the client aimed at; the area appears there at once.
  const auto reported = combat.receive(
      20011, area_effect_report(kInactiveMonster, std::nullopt), position,
      kStart + 350 * kMillisecond);
  EXPECT_TRUE(reported.empty());
  const auto spawned = combat.advance(kStart + 350 * kMillisecond, position, tutorial);
  const auto *area = find_area_announcement(spawned);
  ASSERT_NE(area, nullptr);
  EXPECT_EQ(area->message.at("nub").at("pos"), millimetres(0, 2000, 0));
  EXPECT_EQ(area->message.at("nub").at("skill_object").at("created_tick"),
            wire_monotonic_tick(kStart + 350 * kMillisecond));
  verify_encoding(spawned);
}

TEST(Combat, UntargetedPlayerAreaWithoutAReportLandsAheadAtTheSkillRange) {
  const auto definitions = interval_area_fixture();
  const auto tutorial_data = tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  combat.receive(20010, start_request(), position, kStart);
  EXPECT_EQ(find_area_announcement(combat.advance(kStart + 599 * kMillisecond, position, tutorial)),
            nullptr);
  const auto spawned = combat.advance(kStart + 600 * kMillisecond, position, tutorial);
  const auto *area = find_area_announcement(spawned);
  ASSERT_NE(area, nullptr);
  // Skill 200 reaches 5 m; the area is placed there along the cast yaw, never on the caster.
  EXPECT_EQ(area->message.at("nub").at("pos"), millimetres(5000, 0, 0));
  verify_encoding(spawned);
}

TEST(Combat, AimedAreaUsesTheReportedAimPointWithinReachAndOtherwiseItsTarget) {
  auto definitions = interval_area_fixture();
  definitions["effects"]["1"]["targeting_purpose"]["targetingPosType"] =
      "EX5TargetingPosType_Aim";
  const auto tutorial_data = tutorial_fixture(true);
  const WorldPosition position{8000, 0, 0, 0};

  for (const bool within_reach : {true, false}) {
    TutorialSession tutorial(tutorial_data, 7, {},
                             [](const auto &, const auto &) { return true; });
    CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                         player_stats());
    // Even a targeted cast waits for an aimed area's report.
    combat.receive(20010, targeted_start_request(), position, kStart);
    EXPECT_EQ(
        find_area_announcement(combat.advance(kStart + 300 * kMillisecond, position, tutorial)),
        nullptr);

    const auto aim = within_reach ? millimetres(2500, 500, 0) : millimetres(40000, 0, 0);
    combat.receive(20011, area_effect_report(std::nullopt, std::optional<Json>(aim)), position,
                   kStart + 320 * kMillisecond);
    const auto spawned = combat.advance(kStart + 320 * kMillisecond, position, tutorial);
    const auto *area = find_area_announcement(spawned);
    ASSERT_NE(area, nullptr);
    // An aim point beyond the skill range plus 5 m is refused; the cast target is used.
    EXPECT_EQ(area->message.at("nub").at("pos"),
              within_reach ? millimetres(2500, 500, 0) : millimetres(1000, 0, 0));
  }
}

// A player skill (210) firing a server-driven, piercing linear projectile whose hit is a
// conditional choice, like the Elemental Staff stitching burn (projectile 160560).
Json piercing_conditional_projectile_fixture() {
  auto definitions = combat_fixture();
  definitions["effects"]["20"] = {{"kind", "damage"},
                                  {"parameters", {{"fixedDamage", 5}}},
                                  {"targeting_kind", "refTarget"},
                                  {"targeting", {{"maxTargetCount", 1}}},
                                  {"chain", 0}};
  definitions["effects"]["21"] = definitions["effects"]["20"];
  definitions["effects"]["21"]["parameters"] = {{"fixedDamage", 15}};
  definitions["effects"]["30"] = {{"kind", "spawnProjectile"},
                                  {"parameters", {{"projectileId", 300}, {"maxSpawnCount", 1}}},
                                  {"targeting_kind", "refTarget"},
                                  {"targeting", {{"maxTargetCount", 1}}},
                                  {"chain", 0}};
  definitions["projectiles"]["300"] = {
      {"duration_ms", 450},
      {"parameters",
       {{"speed", 4500.0},
        {"radius", 30.0},
        {"isServerDriven", true},
        {"isPenetration", true},
        {"projectileType", "EX5ProjectileType_Linearity"}}},
      {"max_target_count", 10},
      {"hit_effects", Json::array()},
      {"conditional_hit",
       {{"default_effect_id", 20},
        {"branches",
         Json::array({{{"effect_id", 21},
                       {"conditions",
                        Json::array({{{"kind", "maxHPRatio"},
                                      {"unitType", "EX5SkillEffectConditionUnitType_Target"},
                                      {"comparisonType", "EX5ComparisonType_LessThanOrEqual"},
                                      {"hpRatio", 50.0}}})}}})}}}};
  add_player_skill(definitions, 210, "Normal", 1000,
                   {{{"effect_id", 30}, {"unique_id", 0}, {"time_ms", 100}}}, Json::array());
  return definitions;
}

TEST(Combat, PiercingFlightStrikesEveryEnemyOnItsPathWithItsConditionalHit) {
  auto definitions = piercing_conditional_projectile_fixture();
  // The nearer monster is already at or below half health, so it takes the stronger branch.
  definitions["npc_stats"]["5001015"]["69"] = 25;
  const auto tutorial_data = tutorial_fixture(true);
  tutorial_data->npcs.at(kMonster)["position"] = {1500, 0, 0};
  tutorial_data->npcs.at(kInactiveMonster)["position"] = {3000, 0, 0};
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  auto request = player_skill_request(210, 1);
  request["target_ids"] = {std::to_string(kInactiveMonster)};
  ASSERT_TRUE(accepted_start(combat.receive(20010, request, position, kStart)));
  std::vector<GameplayPacket> packets;
  for (int elapsed = 100; elapsed <= 1500; elapsed += 50) {
    auto due = combat.advance(kStart + elapsed * kMillisecond, position, tutorial);
    packets.insert(packets.end(), due.begin(), due.end());
  }

  // 5001015 (25 of 50 health) takes the conditional branch (15); the healthy far target the
  // default (5). Both are listed in the one hit notification.
  EXPECT_EQ(combat.monster_hp(kMonster), 10);
  EXPECT_EQ(combat.monster_hp(kInactiveMonster), 45);
  const auto *hit = find_packet(packets, 42);
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(hit->message.at("target_datas").size(), 2u);
  verify_encoding(packets);
}

TEST(WorldMobs, PiercingCaptureFailureDoesNotRepeatEarlierTargetDamage) {
  auto definitions = piercing_conditional_projectile_fixture();
  definitions["npc_stats"]["5001528"]["18"] = 5;
  definitions["npc_stats"]["5001528"]["69"] = 5;
  auto map = world_mob_map();
  map["spawns"][0]["position"] = {1500, 0, 0};
  auto second_spawn = map["spawns"][0];
  second_spawn["unit_id"] = "576460752303423522";
  second_spawn["cms_id"] = 5001528;
  second_spawn["position"] = {3000, 0, 0};
  map["spawns"].push_back(second_spawn);
  const auto data = std::make_shared<CombatData>(definitions);
  CombatSession combat(data, nullptr, character(), player_stats());
  combat.attach_world_mobs(CombatSession::create_world_mobs(map, data));
  combat.set_world_defeat_capture([](const MonsterDefeat &) -> MonsterRewardCapture {
    throw std::runtime_error("Injected piercing capture failure");
  });

  const WorldPosition position{9012, 0, 0, 0};
  const auto entry = combat.advance_private_encounter(kStart, position);
  std::uint64_t near_id{};
  std::uint64_t far_id{};
  for (const auto &packet : entry) {
    if (packet.opcode != 10 || !packet.message.at("nub").contains("npc")) {
      continue;
    }
    const int cms_id = packet.message.at("nub").at("npc").at("npc_cms_id");
    if (cms_id == 5001015) {
      near_id = std::stoull(packet.message.at("unit_id").get<std::string>());
    } else if (cms_id == 5001528) {
      far_id = std::stoull(packet.message.at("unit_id").get<std::string>());
    }
  }
  ASSERT_NE(near_id, 0U);
  ASSERT_NE(far_id, 0U);
  auto request = player_skill_request(210, 1);
  request["target_ids"] = {std::to_string(far_id)};
  ASSERT_TRUE(accepted_start(combat.receive(20010, request, position, kStart)));
  std::vector<GameplayPacket> packets;
  for (int elapsed = 100; elapsed <= 1500; elapsed += 50) {
    auto due = combat.advance_private_encounter(kStart + elapsed * kMillisecond, position);
    packets.insert(packets.end(), due.begin(), due.end());
  }

  const auto near_text = std::to_string(near_id);
  const auto far_text = std::to_string(far_id);
  const auto near_hits = std::ranges::count_if(packets, [&](const GameplayPacket &packet) {
    return packet.opcode == 19 &&
           packet.message.value("target_unit_id", std::string{}) == near_text;
  });
  EXPECT_EQ(near_hits, 1);
  EXPECT_EQ(combat.live_monster_health(near_id), 45);
  EXPECT_EQ(combat.live_monster_health(far_id), 5);
  const auto cancelled = std::ranges::find_if(packets, [&](const GameplayPacket &packet) {
    return packet.opcode == 20 &&
           packet.message.value("target_unit_id", std::string{}) == far_text;
  });
  ASSERT_NE(cancelled, packets.end());
  EXPECT_TRUE(cancelled->message.value("damage", false));
  const auto *hit = find_packet(packets, 42);
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(hit->message.at("target_datas").size(), 2U);
  EXPECT_TRUE(combat.pending_world_defeats().empty());
  verify_encoding(packets);
}

// Dragon's Roar shape (effect 11130, projectile 11130, area 11131): a player skill (250) fires a
// server-driven ballistic flight at the client's aimed marker; its lifetime-end effect leaves a
// damaging area where it lands.
Json aimed_flight_fixture() {
  auto definitions = combat_fixture();
  definitions["effects"]["40"] = {
      {"kind", "spawnProjectile"},
      {"parameters", {{"projectileId", 400}, {"maxSpawnCount", 6}}},
      {"targeting_kind", "refTarget"},
      {"targeting", {{"maxTargetCount", 1}, {"refType", "EX5ReferenceTargetType_LockOn"}}},
      {"targeting_purpose", {{"targetingPosType", "EX5TargetingPosType_Aim"}}},
      {"chain", 0}};
  definitions["projectiles"]["400"] = {
      {"duration_ms", 1500},
      {"parameters",
       {{"speed", 1500.0},
        {"radius", 8.0},
        {"isServerDriven", true},
        {"projectileType", "EX5ProjectileType_Ballistic"}}},
      {"max_target_count", 1},
      {"hit_effects", Json::array()},
      {"lifetime_despawn_effects", Json::array({41})}};
  definitions["effects"]["41"] = {
      {"kind", "spawnAreaOfEffect"},
      {"parameters", {{"areaOfEffectId", 6}}},
      {"targeting_kind", "refTarget"},
      {"targeting", {{"maxTargetCount", 4}}},
      {"targeting_purpose", {{"targetingPosType", "EX5TargetingPosType_MainTarget"}}},
      {"chain", 0}};
  definitions["effects"]["42"] = {{"kind", "damage"},
                                  {"parameters", {{"fixedDamage", 7}}},
                                  {"targeting_kind", "refTarget"},
                                  {"targeting", {{"maxTargetCount", 4}}},
                                  {"chain", 0}};
  definitions["areas"]["6"] = {{"duration_ms", 600},      {"interval_ms", 300},
                               {"interval_effect_id", 42}, {"radius_mm", 1500},
                               {"half_height_mm", 1000},  {"max_targets", 4}};
  add_player_skill(definitions, 250, "Normal", 1000,
                   {{{"effect_id", 40}, {"unique_id", 0}, {"time_ms", 100}}}, Json::array());
  definitions["skills"].back()["range_mm"] = 30000;
  return definitions;
}

Json aimed_flight_report(const Json &aim) {
  return {{"skill_instance_id", 1},
          {"pos", millimetres(0, 0, 0)},
          {"rotation", {{"yaw", 0}}},
          {"tick", "999999999999"},
          {"skill_effect", {{"effect_id", 40}, {"unique_effect_id", 0}, {"aim_position", aim}}}};
}

TEST(Combat, AimedFlightLandsOnTheReportedMarkerAndLeavesItsAreaThere) {
  const auto definitions = aimed_flight_fixture();
  const auto tutorial_data = tutorial_fixture(true);
  // The locked-on unit stands elsewhere; the aimed marker is on the other monster.
  tutorial_data->npcs.at(kMonster)["position"] = {3000, 1000, 0};
  tutorial_data->npcs.at(kInactiveMonster)["position"] = {1000, -3000, 0};
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  auto request = player_skill_request(250, 1);
  request["target_ids"] = {std::to_string(kInactiveMonster)};
  ASSERT_TRUE(accepted_start(combat.receive(20010, request, position, kStart)));
  // The aimed flight waits for the client's marker.
  EXPECT_EQ(find_packet(combat.advance(kStart + 100 * kMillisecond, position, tutorial), 40),
            nullptr);

  combat.receive(20011, aimed_flight_report(millimetres(3000, 1000, 0)), position,
                 kStart + 150 * kMillisecond);
  const auto launched = combat.advance(kStart + 150 * kMillisecond, position, tutorial);
  const auto *spawn = find_packet(launched, 40);
  ASSERT_NE(spawn, nullptr);
  EXPECT_EQ(spawn->message.at("spawn_projectile").at("dest_pos"), millimetres(3000, 1000, 0));
  EXPECT_EQ(spawn->message.at("spawn_projectile").at("target_unit_id"), "0");

  std::vector<GameplayPacket> landed;
  for (int elapsed = 200; elapsed <= 600; elapsed += 50) {
    auto due = combat.advance(kStart + elapsed * kMillisecond, position, tutorial);
    landed.insert(landed.end(), due.begin(), due.end());
  }
  const auto *area = find_area_announcement(landed);
  ASSERT_NE(area, nullptr);
  EXPECT_EQ(area->message.at("nub").at("pos"), millimetres(3000, 1000, 0));
  EXPECT_EQ(combat.monster_hp(kMonster), 43);
  EXPECT_EQ(combat.monster_hp(kInactiveMonster), 50);
  verify_encoding(launched);
  verify_encoding(landed);
}

TEST(Combat, AimedMarkerBeyondTheFlightReachIsClampedWhereItsLifetimeEnds) {
  const auto definitions = aimed_flight_fixture();
  const auto tutorial_data = tutorial_fixture(true);
  TutorialSession tutorial(tutorial_data, 7, {}, [](const auto &, const auto &) { return true; });
  CombatSession combat(std::make_shared<CombatData>(definitions), tutorial_data, character(),
                       player_stats());
  const WorldPosition position{8000, 0, 0, 0};

  ASSERT_TRUE(
      accepted_start(combat.receive(20010, player_skill_request(250, 1), position, kStart)));
  // 15 m/s for 1.5 s reaches 22.5 m; the 25 m marker is within the 30 m skill range.
  combat.receive(20011, aimed_flight_report(millimetres(25000, 0, 0)), position,
                 kStart + 120 * kMillisecond);
  const auto launched = combat.advance(kStart + 120 * kMillisecond, position, tutorial);
  const auto *spawn = find_packet(launched, 40);
  ASSERT_NE(spawn, nullptr);
  EXPECT_EQ(spawn->message.at("spawn_projectile").at("dest_pos"), millimetres(22500, 0, 0));
}

} // namespace chronicles
