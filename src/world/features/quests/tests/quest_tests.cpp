#include "protocol/protocol.hpp"
#include "world/features/tutorial/tutorial.hpp"

#include <algorithm>
#include <cstdlib>
#include <gtest/gtest.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr std::uint64_t kFirstMonster = (2ULL << 60) | (1ULL << 12) | 2;
constexpr std::uint64_t kSecondMonster = (2ULL << 60) | (2ULL << 12) | 2;

Json quest_fixture() {
  const Json trigger{{"guid", {"123", "456"}}, {"center", {0, 0, 0}}, {"radius", 1000}};
  Json document{{"format_version", 1},
                {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
                {"map_id", 8000},
                {"objects", Json::array()},
                {"doodads", Json::array()},
                {"sectors", Json::array()},
                {"civilians", Json::array()},
                {"npc_spawns", Json::array()},
                {"utility_items", Json::array()},
                {"quests", Json::array()}};
  for (const auto unit_id : {kFirstMonster, kSecondMonster}) {
    document["npc_spawns"].push_back({{"unit_id", std::to_string(unit_id)},
                                      {"cms_id", 5001529},
                                      {"position", {0, 0, 0}},
                                      {"yaw", 0},
                                      {"spawner_guid", {"123", "456"}},
                                      {"anim_stance_key", 0},
                                      {"behavior", "Monster"},
                                      {"faction", "EX5Faction_Bandit"}});
  }
  document["quests"].push_back({{"cms_id", 100},
                                {"auto_complete", false},
                                {"sequential", true},
                                {"accept_triggers", {trigger}},
                                {"supplies", {{{"item_cms_id", 120059}, {"quantity", 1}}}},
                                {"experience", 0},
                                {"rewards", Json::array()},
                                {"next_quest", 200},
                                {"progresses",
                                 {{{"sequence", 1},
                                   {"group", 1},
                                   {"kind", "TriggerVolumeEnter"},
                                   {"parameters", Json::object()},
                                   {"trigger", trigger}}}}});
  document["quests"].push_back({{"cms_id", 200},
                                {"auto_complete", false},
                                {"sequential", true},
                                {"accept_triggers", Json::array()},
                                {"experience", 500},
                                {"rewards", {{{"item_cms_id", 100109}, {"quantity", 1}}}},
                                {"progresses",
                                 {{{"sequence", 1},
                                   {"group", 1},
                                   {"kind", "HuntMonster"},
                                   {"parameters", {{"npcId", 5001529}, {"count", 2}}}}}}});
  return document;
}

Json trigger_request(int quest_id) {
  return {{"quest_cms_id", quest_id},
          {"quest_progress_sequence_id", 1},
          {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
          {"trigger_volume_enter", {{"trigger_volume_x", "123"}, {"trigger_volume_y", "456"}}}};
}

class Quests : public testing::Test {
protected:
  std::shared_ptr<const TutorialData> data = std::make_shared<TutorialData>(quest_fixture());
  WorldProgress durable;
  WorldPosition position{8000, 0, 0, 0, 0};
  bool storage_available = true;
  unsigned commits{};

  TutorialSession reconnect() {
    return TutorialSession(data, 7, durable, [this](const auto &before, const auto &after) {
      if (!storage_available || before.revision != durable.revision) {
        return false;
      }
      durable = after;
      ++commits;
      return true;
    });
  }

  void verify(const std::vector<GameplayPacket> &packets) {
    Protocol protocol(CHRONICLES_PROTOCOL_DIR);
    for (const auto &packet : packets) {
      const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
      EXPECT_EQ(decoded.at("status"), "decoded");
    }
  }
};

TEST_F(Quests, KillReceiptsAndCountersSurviveReconnectAndRewardsAreExactlyOnce) {
  durable.quests[200].objectives[1] = 0;
  auto client = reconnect();
  EXPECT_EQ(client.receive(20019, trigger_request(200), 0, position, 100000000).at(0).opcode, 2);
  verify(client.record_monster_defeat(kFirstMonster, position));
  EXPECT_EQ(durable.quests.at(200).objectives.at(1), 1);
  EXPECT_EQ(durable.quests.at(200).credited_kills.size(), 1);
  EXPECT_TRUE(client.record_monster_defeat(kFirstMonster, position).empty());
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(client.receive(20018, {{"quest_cms_id", 200}}, 1, position, 100000000).at(0).opcode, 2);

  auto resumed = reconnect();
  EXPECT_TRUE(resumed.record_monster_defeat(kFirstMonster, position).empty());
  verify(resumed.record_monster_defeat(kSecondMonster, position));
  EXPECT_EQ(durable.quests.at(200).objectives.at(1), 2);
  verify(resumed.receive(20018, {{"quest_cms_id", 200}}, 2, position, 100000000));
  EXPECT_TRUE(durable.quests.at(200).complete);
  EXPECT_EQ(durable.stackables.at(100109), 1);
  EXPECT_EQ(durable.experience, 500);
  auto rewarded = reconnect();
  verify(rewarded.receive(20018, {{"quest_cms_id", 200}}, 3, position, 100000000));
  EXPECT_EQ(durable.stackables.at(100109), 1);
  EXPECT_EQ(durable.experience, 500);
  EXPECT_EQ(durable.quests.at(200).success_count, 1);
  verify(rewarded.snapshot());
}

TEST_F(Quests, KillStorageFailureCannotPublishOrConsumeTheReceipt) {
  durable.quests[200].objectives[1] = 0;
  auto client = reconnect();
  storage_available = false;
  EXPECT_THROW(client.record_monster_defeat(kFirstMonster, position), std::runtime_error);
  EXPECT_TRUE(client.progress().quests.at(200).credited_kills.empty());
  EXPECT_EQ(client.progress().quests.at(200).objectives.at(1), 0);
  storage_available = true;
  verify(client.record_monster_defeat(kFirstMonster, position));
  EXPECT_EQ(durable.quests.at(200).objectives.at(1), 1);
}

TEST_F(Quests, KillEventsRequireAnOwnedNearbyMonsterAndCannotCreditFutureQuests) {
  auto client = reconnect();
  EXPECT_TRUE(client.record_monster_defeat(kFirstMonster, position).empty());
  EXPECT_TRUE(durable.quests.empty());
  EXPECT_EQ(commits, 0);
  EXPECT_THROW(client.record_monster_defeat(999, position), std::invalid_argument);
  auto wrong_map = position;
  wrong_map.map_id = 9011;
  EXPECT_THROW(client.record_monster_defeat(kFirstMonster, wrong_map), std::invalid_argument);
  auto distant = position;
  distant.x_mm = 100001;
  EXPECT_THROW(client.record_monster_defeat(kFirstMonster, distant), std::invalid_argument);
}

TEST_F(Quests, AcceptanceSuppliesAndCompletedQuestRetriesDoNotRestartTheChain) {
  auto client = reconnect();
  verify(client.receive(20016, trigger_request(100), 0, position, 100000000));
  verify(client.receive(20016, trigger_request(100), 1, position, 100000000));
  EXPECT_EQ(durable.stackables.at(120059), 1);
  verify(client.receive(20019, trigger_request(100), 2, position, 100000000));
  verify(client.receive(20018, {{"quest_cms_id", 100}}, 3, position, 100000000));
  EXPECT_TRUE(durable.quests.contains(200));
  auto resumed = reconnect();
  const auto retry = resumed.receive(20016, trigger_request(100), 4, position, 100000000);
  ASSERT_EQ(retry.size(), 1);
  EXPECT_EQ(retry.front().opcode, 27);
  EXPECT_EQ(durable.stackables.at(120059), 1);
  const auto progress_retry = resumed.receive(20019, trigger_request(100), 5, position, 100000000);
  EXPECT_EQ(progress_retry.front().opcode, 27);
  EXPECT_TRUE(durable.quests.at(100).complete);
  EXPECT_EQ(durable.quests.at(100).success_count, 1);
}

TEST_F(Quests, OneKillDoesNotCompleteSeveralSequentialGroups) {
  auto document = quest_fixture();
  auto &objectives = document["quests"][1]["progresses"];
  objectives[0]["parameters"]["count"] = 1;
  auto second = objectives[0];
  second["sequence"] = 2;
  second["group"] = 2;
  objectives.push_back(second);
  data = std::make_shared<TutorialData>(std::move(document));
  durable.quests[200].objectives = {{1, 0}, {2, 0}};
  auto client = reconnect();
  verify(client.record_monster_defeat(kFirstMonster, position));
  EXPECT_EQ(durable.quests.at(200).objectives.at(1), 1);
  EXPECT_EQ(durable.quests.at(200).objectives.at(2), 0);
  verify(client.record_monster_defeat(kSecondMonster, position));
  EXPECT_EQ(durable.quests.at(200).objectives.at(2), 1);
}

TEST_F(Quests, AutoCompletionCannotCreditItsNewlyAcceptedSuccessorWithTheSameKill) {
  auto document = quest_fixture();
  auto &first = document["quests"][1];
  first["auto_complete"] = true;
  first["next_quest"] = 300;
  first["progresses"][0]["parameters"]["count"] = 1;
  auto successor = first;
  successor["cms_id"] = 300;
  successor.erase("next_quest");
  document["quests"].push_back(std::move(successor));
  data = std::make_shared<TutorialData>(std::move(document));
  durable.quests[200].objectives[1] = 0;
  auto client = reconnect();
  verify(client.record_monster_defeat(kFirstMonster, position));
  EXPECT_TRUE(durable.quests.at(200).complete);
  ASSERT_TRUE(durable.quests.contains(300));
  EXPECT_FALSE(durable.quests.at(300).complete);
  EXPECT_EQ(durable.quests.at(300).objectives.at(1), 0);
}

TEST_F(Quests, TriggerLeaveRequiresTheBoundaryAndAcceptsTheOutsideEdge) {
  auto document = quest_fixture();
  auto &objective = document["quests"][0]["progresses"][0];
  objective["kind"] = "TriggerVolumeLeave";
  objective["trigger"]["radius"] = 20000;
  data = std::make_shared<TutorialData>(std::move(document));
  durable.quests[100].objectives[1] = 0;
  auto client = reconnect();
  auto request = trigger_request(100);
  request["trigger_volume_leave"] = request.at("trigger_volume_enter");
  request.erase("trigger_volume_enter");
  EXPECT_EQ(client.receive(20019, request, 0, position, 100000000).at(0).opcode, 2);
  position.x_mm = 21000;
  request["pos"]["millimeter_x"] = position.x_mm;
  verify(client.receive(20019, request, 1, position, 100000000));
  EXPECT_EQ(durable.quests.at(100).objectives.at(1), 1);
}

TEST(QuestDefinitions, RejectsCyclesAndExcessiveObjectiveCounts) {
  auto document = quest_fixture();
  document["quests"][1]["next_quest"] = 100;
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
  document = quest_fixture();
  document["quests"][1]["progresses"][0]["parameters"]["count"] = 4097;
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);
}

// A level trigger gated on quest 100's completion plays an invisible cutscene whose end
// teleports the player and binds an NPC's AI phase (the shape of tutorial cutscene 1523).
// Monster 5001529 has a boss script that plays a cutscene ending in a map move (1521).
Json level_trigger_fixture() {
  auto document = quest_fixture();
  document["level_triggers"] = Json::array(
      {{{"guid", {"777", "888"}},
        {"center", {5000, 0, 0}},
        {"radius", 1000},
        {"volume_id", "fixture-door"},
        {"trigger_type", "Enter"},
        {"rules",
         {{{"supported", true},
           {"tasks", {{{"kind", "PlayCutScene"}, {"cut_scene_id", 1523}}}},
           {"conditions",
            {{{"group", 1},
              {"operator", "AND"},
              {"negated", false},
              {"quest_id", 100},
              {"requirement", "QuestComplete"}}}}}}}}});
  document["cut_scenes"] = Json::array(
      {{{"cut_scene_id", 1523},
        {"invisible", true},
        {"supported", true},
        {"npc_bindings",
         {{{"tag", "NPC_1"}, {"unit_id", std::to_string(kFirstMonster)}, {"ai_phase", 2302400}}}},
        {"end_teleport", {{"position", {9000, 100, 0}}, {"yaw", 125.0}}}},
       {{"cut_scene_id", 1521},
        {"invisible", true},
        {"supported", true},
        {"npc_bindings", Json::array()},
        {"end_map_move",
         {{"map_id", 10117},
          {"end_point_id", 9999993},
          {"position", {-1552556, 3078387, -628450}},
          {"yaw", 80.0}}}}});
  document["boss_defeats"] = Json::array({{{"npc_cms_id", 5001529},
                                           {"cut_scene_id", 1521},
                                           {"doodad_activations", Json::array()}}});
  return document;
}

Json level_trigger_request(int x_mm) {
  return {{"trigger_id_x", "777"},
          {"trigger_id_y", "888"},
          {"pos", {{"millimeter_x", x_mm}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
          {"body_yaw", 0.0},
          {"tick", "1"}};
}

std::vector<std::uint16_t> opcodes(const std::vector<GameplayPacket> &packets) {
  std::vector<std::uint16_t> result;
  for (const auto &packet : packets) {
    result.push_back(packet.opcode);
  }
  return result;
}

class LevelTriggers : public Quests {
protected:
  void SetUp() override {
    data = std::make_shared<TutorialData>(level_trigger_fixture());
  }

  std::vector<GameplayPacket> send(TutorialSession &session, std::uint16_t opcode,
                                   const Json &request) {
    ticks += 10000000;
    auto packets = session.receive(opcode, request, sequence++, position, ticks);
    verify(packets);
    return packets;
  }

  std::int64_t ticks = 100000000;
  std::uint32_t sequence{};
};

TEST_F(LevelTriggers, DoorCutsceneWaitsForItsQuestThenTeleportsAndBindsTheBoss) {
  auto session = reconnect();
  position = {8000, 5000, 0, 0, 0};

  // Quest 100 is not complete: entering the volume is routine and produces nothing.
  EXPECT_TRUE(send(session, 20084, level_trigger_request(5000)).empty());

  QuestProgress finished;
  finished.complete = true;
  finished.success_count = 1;
  durable.quests.emplace(100, finished);
  session = reconnect();

  // A report from outside the volume is rejected; one inside plays the cutscene.
  position = {8000, 15000, 0, 0, 0};
  const auto far = send(session, 20084, level_trigger_request(15000));
  ASSERT_EQ(opcodes(far), std::vector<std::uint16_t>{2});
  EXPECT_EQ(far.front().message.at("ec"), "TooFar");

  position = {8000, 5000, 0, 0, 0};
  const auto started = send(session, 20084, level_trigger_request(5000));
  ASSERT_EQ(opcodes(started), (std::vector<std::uint16_t>{67, 70}));
  EXPECT_EQ(started.at(0).message.at("visible_channel"), "6");
  EXPECT_EQ(started.at(1).message.at("cut_scene_id"), 1523);

  // While the cutscene plays, re-entering does not restart it.
  EXPECT_TRUE(send(session, 20084, level_trigger_request(5000)).empty());

  // A mismatched end has no effect; the matching end teleports and binds the NPC phase.
  EXPECT_TRUE(send(session, 20055, {{"cut_scene_id", 1521}}).empty());
  const auto ended = send(session, 20055, {{"cut_scene_id", 1523}});
  ASSERT_EQ(opcodes(ended), (std::vector<std::uint16_t>{181, 67, 114}));
  EXPECT_EQ(ended.at(0).message.at("pos").at("millimeter_x"), 9000);
  EXPECT_EQ(ended.at(0).message.at("body_yaw"), 125.0);
  EXPECT_EQ(ended.at(1).message.at("visible_channel"), "3");
  EXPECT_EQ(ended.at(2).message.at("unit_id"), std::to_string(kFirstMonster));
  EXPECT_EQ(ended.at(2).message.at("new_ai_phase_transition_id"), 2302400);
  EXPECT_TRUE(send(session, 20055, {{"cut_scene_id", 1523}}).empty());
  EXPECT_FALSE(session.take_map_move().has_value());

  // The client's teleport acknowledgements complete the relocation exactly once.
  EXPECT_FALSE(session.take_completed_teleport().has_value());
  EXPECT_TRUE(send(session, 20088, {{"tick", "1"}}).empty());
  EXPECT_TRUE(send(session, 20089, Json::object()).empty());
  const auto teleport = session.take_completed_teleport();
  ASSERT_TRUE(teleport.has_value());
  EXPECT_EQ(teleport->map_id, 8000);
  EXPECT_EQ(teleport->x_mm, 9000);
  EXPECT_EQ(teleport->y_mm, 100);
  EXPECT_FALSE(session.take_completed_teleport().has_value());
  const auto repeated = send(session, 20089, Json::object());
  ASSERT_EQ(opcodes(repeated), std::vector<std::uint16_t>{2});
}

TEST_F(LevelTriggers, BossScriptRunsWithTheFinishingKillAndItsCutsceneMovesMaps) {
  QuestProgress hunt;
  hunt.objectives.emplace(1, 0);
  durable.quests.emplace(200, hunt);
  auto session = reconnect();

  // The objective needs two kills; the first one credits it without the boss script.
  const auto first = session.record_monster_defeat(kFirstMonster, position, 200000000);
  verify(first);
  EXPECT_EQ(opcodes(first), std::vector<std::uint16_t>{27});

  const auto finishing = session.record_monster_defeat(kSecondMonster, position, 300000000);
  verify(finishing);
  ASSERT_EQ(opcodes(finishing), (std::vector<std::uint16_t>{27, 25, 67, 70}));
  EXPECT_EQ(finishing.at(1).message.at("boss_cms_id"), "5001529");
  EXPECT_EQ(finishing.at(3).message.at("cut_scene_id"), 1521);

  // The end request starts the map move; the visible channel is restored by the new map.
  const auto ended = send(session, 20055, {{"cut_scene_id", 1521}});
  EXPECT_TRUE(ended.empty());
  const auto move = session.take_map_move();
  ASSERT_TRUE(move.has_value());
  EXPECT_EQ(move->map_id, 10117);
  EXPECT_EQ(move->spawn.map_id, 10117);
  EXPECT_EQ(move->spawn.x_mm, -1552556);
  EXPECT_EQ(move->spawn.yaw, 80.0f);
  EXPECT_FALSE(session.take_map_move().has_value());
}

TEST(LevelTriggerDefinitions, RejectUnknownBindingsAndUnplayableCutscenes) {
  auto document = level_trigger_fixture();
  document["cut_scenes"][0]["npc_bindings"][0]["unit_id"] = "2305843009213693954";
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);

  document = level_trigger_fixture();
  document["cut_scenes"][0]["supported"] = false;
  EXPECT_THROW(TutorialData invalid(document), std::runtime_error);

  // An unsupported rule may name an unrecovered cutscene; it is simply never executed.
  document = level_trigger_fixture();
  document["level_triggers"][0]["rules"][0]["supported"] = false;
  document["level_triggers"][0]["rules"][0]["tasks"][0]["cut_scene_id"] = 9999;
  EXPECT_NO_THROW(TutorialData valid(document));
}

class GeneratedQuests : public Quests {
protected:
  std::unique_ptr<TutorialSession> client;
  std::int64_t ticks = 100000000;
  std::uint32_t sequence{};

  void resume() {
    client = std::make_unique<TutorialSession>(reconnect());
  }

  void send(std::uint16_t opcode, Json request) {
    ticks += 10000000;
    const auto packets = client->receive(opcode, request, sequence++, position, ticks);
    ASSERT_FALSE(packets.empty()) << "Missing response to opcode " << opcode;
    ASSERT_NE(packets.front().opcode, 2) << packets.front().message.dump();
    verify(packets);
  }

  Json point(const Json &coordinates) {
    position = {data->map_id(), coordinates.at(0).get<int>(), coordinates.at(1).get<int>(),
                coordinates.at(2).get<int>(), 0};
    return {{"millimeter_x", position.x_mm},
            {"millimeter_y", position.y_mm},
            {"millimeter_z", position.z_mm}};
  }

  void enter(int quest_id, int objective_sequence = 0) {
    const auto &quest = *data->quest(quest_id);
    const Json *trigger = nullptr;
    if (objective_sequence == 0) {
      trigger = &quest.at("accept_triggers").at(0);
    } else {
      for (const auto &objective : quest.at("progresses")) {
        if (objective.at("sequence") == objective_sequence) {
          trigger = &objective.at("trigger");
          break;
        }
      }
    }
    ASSERT_NE(trigger, nullptr);
    const auto field = trigger->value("request_field", std::string("trigger_volume_enter"));
    Json request{{"quest_cms_id", quest_id},
                 {"pos", point(trigger->at("center"))},
                 {field,
                  {{"trigger_volume_x", trigger->at("guid").at(0)},
                   {"trigger_volume_y", trigger->at("guid").at(1)}}}};
    if (objective_sequence != 0) {
      request["quest_progress_sequence_id"] = objective_sequence;
    }
    send(objective_sequence == 0 ? 20016 : 20019, std::move(request));
  }

  void defeat(int npc_id) {
    const auto npc = std::ranges::find_if(
        data->npcs, [npc_id](const auto &entry) { return entry.second.at("cms_id") == npc_id; });
    ASSERT_NE(npc, data->npcs.end());
    point(npc->second.at("position"));
    ticks += 10000000;
    verify(client->record_monster_defeat(npc->first, position, ticks));
  }

  void claim(int quest_id) {
    send(20018, {{"quest_cms_id", quest_id}});
    ASSERT_TRUE(durable.quests.at(quest_id).complete);
    resume();
  }

  void interact(int cms_id) {
    const auto found = std::ranges::find_if(
        data->doodads, [cms_id](const auto &entry) { return entry.second.at("cms_id") == cms_id; });
    ASSERT_NE(found, data->doodads.end());
    const auto &definition = found->second;
    send(20069,
         {{"target_unit_id", std::to_string(found->first)},
          {"interaction_anim_id", definition.at("animation_id")},
          {"param", {{"interaction", {{"interaction_id", definition.at("interaction_id")}}}}},
          {"pos", point(definition.at("position"))}});
    ticks += definition.value("execute_time_ms", 200) * 10000LL;
    send(20070, {{"interaction_anim_id", definition.at("animation_id")}});
    send(20071,
         {{"end_interaction_anim", {{"interaction_anim_id", definition.at("animation_id")}}}});
  }
};

TEST_F(GeneratedQuests, InitialTutorialCompletesEveryQuestAndRestoresItsRewardsAndObjects) {
  const auto *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS to the verified generated profile";
  }
  data = TutorialData::load(path);
  resume();
  enter(10000040);
  enter(90101006);
  for (int objective = 1; objective <= 3; ++objective) {
    enter(10000040, objective);
  }
  claim(10000040);
  enter(10000106);
  defeat(5001015);
  claim(10000106);
  ASSERT_EQ(durable.stackables.at(100109), 1);
  interact(49013);
  EXPECT_EQ(durable.stackables.at(100109), 0);
  enter(10000042, 2);
  enter(10000042, 4);
  claim(10000042);
  for (const int npc_id : {5001529, 5001530, 5001528}) {
    defeat(npc_id);
  }
  claim(10000043);

  const auto npc = std::ranges::find_if(
      data->civilians, [](const auto &entry) { return entry.second.at("cms_id") == 1001671; });
  ASSERT_NE(npc, data->civilians.end());
  const auto npc_position = point(npc->second.at("position"));
  send(20065, {{"target_unit_id", std::to_string(npc->first)}, {"pos", npc_position}});
  send(20019, {{"quest_cms_id", 10000044},
               {"quest_progress_sequence_id", 1},
               {"pos", npc_position},
               {"dialogue_selection",
                {{"unit_id", std::to_string(npc->first)},
                 {"dialogue_cms_id", 101010652},
                 {"sequence_order", 1},
                 {"select_order", 1}}}});
  send(20066, Json::object());
  enter(10000044, 2);
  interact(49020);
  enter(10000044, 4);
  enter(10000044, 5);
  claim(10000044);
  defeat(5001527);
  claim(10000045);

  ASSERT_EQ(durable.quests.size(), 7);
  for (const auto &[quest_id, quest] : durable.quests) {
    EXPECT_TRUE(quest.complete) << quest_id;
    EXPECT_EQ(quest.success_count, 1) << quest_id;
  }
  EXPECT_EQ(durable.stackables.at(120100), 1);
  EXPECT_EQ(durable.experience, 700);
  const auto saved = durable;
  send(20018, {{"quest_cms_id", 10000045}});
  EXPECT_EQ(durable, saved);
  verify(client->snapshot());
  verify(client->sectors());
}

// The official QuestUpdateNoti for a 20019 progress carries ONLY the objective that changed (capture records 320,
// 1339, 1361: one `progresses` entry, state InProgress), not the whole objective list. Ours resent every objective
// (including the untouched ones at value 0). The client store upserts by objective key, so this is expected to be
// harmless; the test pins the official frame shape and that the stored state still holds all three objectives.
TEST_F(GeneratedQuests, ProgressUpdateCarriesOnlyTheChangedObjectiveLikeTheOfficialFrame) {
  const auto *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS to the verified generated profile";
  }
  data = TutorialData::load(path);
  resume();
  enter(10000040);
  const auto &quest = *data->quest(10000040);
  const auto objective = std::ranges::find_if(quest.at("progresses"), [](const Json &entry) {
    return entry.at("sequence") == 1;
  });
  ASSERT_NE(objective, quest.at("progresses").end());
  const auto &trigger = objective->at("trigger");
  const Json request{{"quest_cms_id", 10000040},
                     {"quest_progress_sequence_id", 1},
                     {"pos", point(trigger.at("center"))},
                     {"trigger_volume_enter",
                      {{"trigger_volume_x", trigger.at("guid").at(0)},
                       {"trigger_volume_y", trigger.at("guid").at(1)}}}};
  const auto packets = client->receive(20019, request, sequence++, position, ticks += 10000000);
  ASSERT_FALSE(packets.empty());
  ASSERT_EQ(packets.front().opcode, 27) << packets.front().message.dump();
  const auto &update = packets.front().message.at("quest");
  EXPECT_EQ(update.at("cms_id"), 10000040);
  EXPECT_EQ(update.at("state"), "EX5QuestStateType_InProgress");
  ASSERT_EQ(update.at("progresses").size(), 1u) << update.dump();
  EXPECT_EQ(update.at("progresses").at(0).at("sequence_id"), 1);
  EXPECT_EQ(update.at("progresses").at(0).at("value"), 1);
  EXPECT_EQ(durable.quests.at(10000040).objectives.size(), 3u);
  EXPECT_EQ(durable.quests.at(10000040).objectives.at(2), 0);
}

// "Shadows of Memory" (10000040) has three TriggerVolumeEnter objectives that all read "Head to the
// Recalling Grounds" (count 1 each). After ONE objective the quest is legitimately still in
// progress with objectives 2 and 3 pending, so a client tracker showing "Head to the Recalling
// Grounds (0/1)" is the correct state, not a reset. Observed on the disposable fixture: persisted
// objectives {1:1, 2:0, 3:0}, complete=false, a repeated accept request (20016) and one progress
// request (20019). This pins that state through the real request path: official order, repeated
// accept/progress/claim, snapshot and reload from the store.
TEST_F(GeneratedQuests, ShadowsOfMemoryStaysPendingUntilAllThreeObjectivesAndRetriesNeverResetIt) {
  const auto *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS to the verified generated profile";
  }
  data = TutorialData::load(path);
  resume();
  enter(10000040);
  enter(10000040, 1);

  const auto &pending = durable.quests.at(10000040);
  EXPECT_FALSE(pending.complete);
  EXPECT_EQ(pending.objectives.at(1), 1);
  EXPECT_EQ(pending.objectives.at(2), 0);
  EXPECT_EQ(pending.objectives.at(3), 0);
  const auto after_first_objective = durable;

  // The client re-sent the accept request: nothing is reset, completed or re-committed.
  enter(10000040);
  EXPECT_EQ(durable, after_first_objective);

  // Claiming the reward before every objective is done is refused and changes nothing.
  const auto early_claim = client->receive(20018, {{"quest_cms_id", 10000040}}, sequence++,
                                           position, ticks += 10000000);
  ASSERT_FALSE(early_claim.empty());
  EXPECT_EQ(early_claim.front().opcode, 2);
  EXPECT_EQ(durable, after_first_objective);

  // Reload from the store: the snapshot lists the quest in progress with objectives 2 and 3 at 0.
  resume();
  EXPECT_EQ(durable, after_first_objective);
  const auto snapshot = client->snapshot();
  const auto quests = std::ranges::find_if(
      snapshot, [](const auto &packet) { return packet.opcode == 136; });
  ASSERT_NE(quests, snapshot.end());
  const auto entry = std::ranges::find_if(quests->message.at("quests"), [](const Json &quest) {
    return quest.at("cms_id") == 10000040;
  });
  ASSERT_NE(entry, quests->message.at("quests").end());
  EXPECT_EQ(entry->at("state"), "EX5QuestStateType_InProgress");
  ASSERT_EQ(entry->at("progresses").size(), 3);
  EXPECT_EQ(entry->at("progresses").at(1).at("value"), 0);
  EXPECT_EQ(entry->at("progresses").at(2).at("value"), 0);

  // Finishing the remaining objectives and claiming completes it exactly once and starts the
  // correct next quest; later repeats of accept, progress and claim change nothing.
  enter(10000040, 2);
  enter(10000040, 3);
  claim(10000040);
  EXPECT_TRUE(durable.quests.at(10000040).complete);
  EXPECT_EQ(durable.quests.at(10000040).success_count, 1);
  ASSERT_TRUE(durable.quests.contains(10000042));
  EXPECT_FALSE(durable.quests.at(10000042).complete);
  EXPECT_EQ(durable.experience, 0);
  const auto completed = durable;
  enter(10000040);
  enter(10000040, 1);
  send(20018, {{"quest_cms_id", 10000040}});
  EXPECT_EQ(durable, completed);
  resume();
  EXPECT_EQ(durable, completed);
}

TEST_F(GeneratedQuests, SecondTutorialAcceptsItsNpcQuestAndDoesNotDuplicateSupplies) {
  const auto *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS to the verified generated profile";
  }
  const auto root = TutorialData::load(path);
  data = root->followup(10117);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data->map_id(), 10117);
  const auto npc = std::ranges::find_if(
      data->npcs, [](const auto &entry) { return entry.second.at("cms_id") == 1001539; });
  ASSERT_NE(npc, data->npcs.end());
  const Json request{{"quest_cms_id", 10000046},
                     {"pos", point(npc->second.at("position"))},
                     {"unit", {{"unit_id", std::to_string(npc->first)}}}};
  resume();
  send(20016, request);
  resume();
  send(20016, request);
  EXPECT_EQ(durable.stackables.at(120059), 1);
  enter(10000046, 1);
  enter(90100011);
  EXPECT_TRUE(durable.quests.at(90100011).complete);
  EXPECT_EQ(durable.quests.at(10000046).objectives.at(1), 1);
  verify(client->snapshot());
}

// The reported live failure: after quest 10000044's final trigger, the client also sends
// LevelTriggerReq for the same volume. The official server answered with cutscene 1523 and,
// on its end, teleported the player through the sealed door into the Herald Knight arena.
// The boss kill then plays cutscene 1521, whose end point moves the player to map 10117.
TEST_F(GeneratedQuests, HeraldKnightDoorCutsceneReachesTheArenaAndTheBossOpensTheNextInstance) {
  const auto *path = std::getenv("CHRONICLES_TUTORIAL_DEFINITIONS");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_DEFINITIONS to the verified generated profile";
  }
  data = TutorialData::load(path);
  QuestProgress before_door;
  for (int objective = 1; objective <= 5; ++objective) {
    before_door.objectives.emplace(objective, objective < 5 ? 1 : 0);
  }
  durable.quests.emplace(10000044, before_door);
  resume();
  enter(10000044, 5);

  const auto &door_quest = *data->quest(10000044);
  const auto &door_trigger = door_quest.at("progresses").at(4).at("trigger");
  const Json trigger_request{{"trigger_id_x", door_trigger.at("guid").at(0)},
                             {"trigger_id_y", door_trigger.at("guid").at(1)},
                             {"pos", point(door_trigger.at("center"))},
                             {"body_yaw", 125.0},
                             {"tick", "1"}};
  ticks += 10000000;
  const auto started = client->receive(20084, trigger_request, sequence++, position, ticks);
  verify(started);
  ASSERT_EQ(opcodes(started), (std::vector<std::uint16_t>{67, 70}));
  EXPECT_EQ(started.at(1).message.at("cut_scene_id"), 1523);

  // The client claims 10000044 while the cutscene plays; 10000045 is accepted.
  send(20018, {{"quest_cms_id", 10000044}});
  ASSERT_TRUE(durable.quests.contains(10000045));

  ticks += 10000000;
  const auto ended = client->receive(20055, {{"cut_scene_id", 1523}}, sequence++, position, ticks);
  verify(ended);
  ASSERT_EQ(opcodes(ended), (std::vector<std::uint16_t>{181, 67, 114}));
  EXPECT_EQ(ended.at(0).message.at("pos").at("millimeter_x"), -1393646);
  EXPECT_EQ(ended.at(0).message.at("pos").at("millimeter_y"), 706393);
  const auto boss = std::ranges::find_if(
      data->npcs, [](const auto &entry) { return entry.second.at("cms_id") == 5001527; });
  ASSERT_NE(boss, data->npcs.end());
  EXPECT_EQ(ended.at(2).message.at("unit_id"), std::to_string(boss->first));
  EXPECT_EQ(ended.at(2).message.at("new_ai_phase_transition_id"), 2302400);

  client->receive(20088, {{"tick", "1"}}, sequence++, position, ticks += 10000000);
  client->receive(20089, Json::object(), sequence++, position, ticks += 10000000);
  const auto arena = client->take_completed_teleport();
  ASSERT_TRUE(arena.has_value());
  EXPECT_EQ(arena->x_mm, -1393646);

  // The arena doodad is absent from sector refreshes until the boss script activates it.
  const auto contains_doodad_two = [](const std::vector<GameplayPacket> &packets) {
    return std::ranges::any_of(packets, [](const auto &packet) {
      return packet.opcode == 193 && packet.message.at("sector_id") == 81923 &&
             packet.message.contains("doodads") &&
             std::ranges::any_of(packet.message.at("doodads"),
                                 [](const Json &doodad) { return doodad.at("id") == 2; });
    });
  };
  EXPECT_FALSE(contains_doodad_two(client->sectors()));

  point(boss->second.at("position"));
  ticks += 10000000;
  const auto defeated = client->record_monster_defeat(boss->first, position, ticks);
  verify(defeated);
  ASSERT_EQ(opcodes(defeated), (std::vector<std::uint16_t>{27, 25, 194, 67, 70}));
  EXPECT_EQ(defeated.at(4).message.at("cut_scene_id"), 1521);
  EXPECT_TRUE(contains_doodad_two(client->sectors()));

  send(20018, {{"quest_cms_id", 10000045}});
  ticks += 10000000;
  EXPECT_TRUE(client->receive(20055, {{"cut_scene_id", 1521}}, sequence++, position, ticks)
                  .empty());
  const auto move = client->take_map_move();
  ASSERT_TRUE(move.has_value());
  EXPECT_EQ(move->map_id, 10117);
  const auto followup = data->followup(move->map_id);
  ASSERT_NE(followup, nullptr);
  client->enter_map(followup);
  EXPECT_EQ(client->data().map_id(), 10117);
  verify(client->sectors());

  // The completed quest and the activated arena doodad are durable.
  EXPECT_TRUE(durable.quests.at(10000045).complete);
  EXPECT_EQ(durable.doodad_phases.at(351856605798406ULL), 1);
}

} // namespace
} // namespace chronicles
