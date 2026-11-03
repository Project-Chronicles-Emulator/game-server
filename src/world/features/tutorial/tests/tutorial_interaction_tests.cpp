#include "cms.generated.pb.h"
#include "protocol/protocol.hpp"
#include "world/features/tutorial/tutorial.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kStart = 100000000;
constexpr std::int64_t kTicksPerMs = 10000;
constexpr std::uint64_t kLever = 351856605794310;
constexpr std::uint64_t kGate = 351860900765702;
constexpr std::uint64_t kDoor = 351865195728902;
constexpr std::uint64_t kNote = 20477948800827398;
constexpr std::uint64_t kScene = 20477948800815110;

// Map 8000 treasure chest (AdventureObject 20080008), as generated from the client tables and
// observed in three official captures: interaction 300, animation 100, necklace 11500101.
constexpr std::uint64_t kChest = 281492156583942;
constexpr int kChestCmsId = 20080008;
constexpr int kChestInteraction = 300;
constexpr int kChestAnimation = 100;
constexpr int kChestExecuteMs = 2851;
constexpr int kNecklace = 11500101;
constexpr std::int64_t kFirstStoredItemId = 900;

Json interaction_profile(int map_id = 8000) {
  Json profile{{"format_version", 1},
               {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
               {"map_id", map_id},
               {"objects", Json::array()},
               {"quests", Json::array()},
               {"doodads", Json::array()},
               {"civilians", Json::array()},
               {"utility_items", Json::array()},
               {"sectors", {81923, 81924, 81925}}};
  const auto add = [&](std::uint64_t id, int cms_id, int quest_id, int animation_id) {
    profile["doodads"].push_back({{"unit_id", std::to_string(id)},
                                  {"cms_id", cms_id},
                                  {"sector_id", 81923},
                                  {"local_id", 1},
                                  {"position", {0, 0, 0}},
                                  {"yaw", -55},
                                  {"initial_phase", 1},
                                  {"completed_phase", 2},
                                  {"quest_id", quest_id},
                                  {"quest_sequence", 1},
                                  {"interaction_id", cms_id},
                                  {"animation_id", animation_id},
                                  {"execute_time_ms", 933},
                                  {"duration_ms", 4733},
                                  {"consumes", Json::array()}});
    profile["quests"].push_back(
        {{"cms_id", quest_id},
         {"auto_complete", false},
         {"sequential", true},
         {"accept_triggers", Json::array()},
         {"experience", 0},
         {"rewards", Json::array()},
         {"progresses", Json::array({{{"sequence", 1},
                                      {"group", 1},
                                      {"kind", "InteractDoodad"},
                                      {"parameters", {{"doodadId", cms_id}}}}})}});
  };
  if (map_id == 10117) {
    add(kScene, 10000048, 10000046, 0);
    profile["doodads"][0]["cut_scene_id"] = 1520;
    profile["doodads"][0]["reset_time_ms"] = 10000;
    profile["objects"].push_back({{"unit_id", std::to_string(kNote)},
                                  {"cms_id", 2010033},
                                  {"interaction_id", 305},
                                  {"animation_id", 305},
                                  {"execute_time_ms", 616},
                                  {"duration_ms", 1000},
                                  {"position", {0, 0, 0}},
                                  {"experience", 100},
                                  {"rewards", Json::array()}});
    return profile;
  }
  add(kLever, 49020, 10000044, 3);
  profile["doodads"][0]["execute_time_ms"] = 2666;
  profile["doodads"][0]["phase_updates"] =
      Json::array({{{"unit_id", std::to_string(kGate)}, {"phase", 2}}});
  add(kDoor, 49013, 10000042, 200);
  profile["doodads"][1]["consumes"] = Json::array({{{"item_cms_id", 100109}, {"quantity", 1}}});
  profile["doodads"].push_back({{"unit_id", std::to_string(kGate)},
                                {"cms_id", 49014},
                                {"sector_id", 81924},
                                {"local_id", 2},
                                {"position", {0, 0, 0}},
                                {"initial_phase", 1},
                                {"phases", {1, 2}},
                                {"consumes", Json::array()}});
  const Json necklace{{"item_cms_id", kNecklace},
                      {"quantity", 1},
                      {"instance", {{"grade", "Normal"}, {"max_durability", 10000}}}};
  profile["objects"].push_back({{"unit_id", std::to_string(kChest)},
                                {"cms_id", kChestCmsId},
                                {"interaction_id", kChestInteraction},
                                {"animation_id", kChestAnimation},
                                {"execute_time_ms", kChestExecuteMs},
                                {"duration_ms", 4000},
                                {"position", {0, 0, 0}},
                                {"rewards", Json::array({necklace})}});
  return profile;
}

Json start_request(std::uint64_t id, int animation_id, int interaction_id) {
  return {{"target_unit_id", std::to_string(id)},
          {"interaction_anim_id", animation_id},
          {"param", {{"interaction", {{"interaction_id", interaction_id}}}}},
          {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}}};
}

void check_wire(const std::vector<GameplayPacket> &packets) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : packets) {
    EXPECT_EQ(protocol.decode(protocol.encode(packet.opcode, packet.message)).at("status"),
              "decoded");
  }
}

class TutorialInteractionTest : public testing::Test {
protected:
  WorldProgress durable;
  bool allow_commit = true;
  int commits{};
  WorldPosition position{8000, 0, 0, 0, 0};

  TutorialSession session(int map_id = 8000) {
    return TutorialSession(std::make_shared<TutorialData>(interaction_profile(map_id)), 7, durable,
                           [this](const auto &, const auto &next) {
                             if (!allow_commit) {
                               return false;
                             }
                             durable = next;
                             ++commits;
                             return true;
                           });
  }

  // Stub item storage for instance rewards: commits progress with the rows and assigns
  // increasing storage ids, or reports a full inventory without writing anything.
  bool inventory_full = false;
  std::vector<CharacterItem> stored_items;

  TutorialSession::InstanceCommitResult
  commit_instances(const WorldProgress &next, const std::vector<CharacterItem> &granted) {
    TutorialSession::InstanceCommitResult result;
    if (inventory_full) {
      result.status = TutorialSession::InstanceCommitStatus::inventory_full;
      return result;
    }

    durable = next;
    ++commits;
    for (auto item : granted) {
      item.id = kFirstStoredItemId + static_cast<std::int64_t>(stored_items.size());
      stored_items.push_back(item);
      result.stored.push_back(item);
    }
    result.status = TutorialSession::InstanceCommitStatus::committed;
    return result;
  }

  TutorialSession chest_session() {
    auto client = session();
    client.set_instance_commit(
        [this](const WorldProgress &, const WorldProgress &next,
               const std::vector<CharacterItem> &granted) {
          return commit_instances(next, granted);
        });
    return client;
  }

  Json chest_start() const {
    auto request = start_request(kChest, kChestAnimation, kChestInteraction);
    request["yaw"] = -45.5;
    return request;
  }
};

TEST_F(TutorialInteractionTest, LeverAndLinkedGateCommitTogetherAndRestore) {
  durable.quests[10000044].objectives[1] = 0;
  auto client = session();
  check_wire(client.receive(20069, start_request(kLever, 3, 49020), 1, position, kStart));
  check_wire(client.receive(20070, {{"interaction_anim_id", 3}}, 2, position,
                            kStart + 2666 * kTicksPerMs));
  EXPECT_EQ(durable.doodad_phases.at(kLever), 2);
  EXPECT_EQ(durable.doodad_phases.at(kGate), 2);
  EXPECT_EQ(durable.quests.at(10000044).objectives.at(1), 1);
  EXPECT_EQ(commits, 1);
  auto reconnected = session();
  EXPECT_EQ(reconnected.sectors().at(1).message.at("doodads").at(0).at("phase"), 2);
}

TEST_F(TutorialInteractionTest, LegacyLeverSaveProjectsOpenGate) {
  durable.doodad_phases[kLever] = 2;
  auto client = session();
  EXPECT_EQ(client.sectors().at(1).message.at("doodads").at(0).at("phase"), 2);
  EXPECT_EQ(commits, 0);
}

TEST_F(TutorialInteractionTest, RepeatedStartKeepsDeadlineAndExecutionUsesAuthoredTiming) {
  durable.quests[10000044].objectives[1] = 0;
  auto client = session();
  const auto request = start_request(kLever, 3, 49020);
  client.receive(20069, request, 1, position, kStart);
  const auto retry = client.receive(20069, request, 2, position, kStart + 1000 * kTicksPerMs);
  EXPECT_EQ(retry.at(0).message.at("tick"), std::to_string(kStart * 100));
  EXPECT_EQ(
      client.receive(20070, {{"interaction_anim_id", 3}}, 3, position, kStart + 200 * kTicksPerMs)
          .at(0)
          .opcode,
      2);
  client.receive(20070, {{"interaction_anim_id", 3}}, 4, position, kStart + 2666 * kTicksPerMs);
  EXPECT_EQ(durable.doodad_phases.at(kGate), 2);
}

TEST_F(TutorialInteractionTest, CancelledOrExpiredDoorAnimationDoesNotConsumeKey) {
  durable.quests[10000042].objectives[1] = 0;
  durable.stackables[100109] = 1;
  auto client = session();
  const auto request = start_request(kDoor, 200, 49013);
  client.receive(20069, request, 1, position, kStart);
  EXPECT_TRUE(client.busy());
  client.receive(20071, {{"end_interaction_anim", {{"interaction_anim_id", 200}}}}, 2, position,
                 kStart);
  EXPECT_FALSE(client.busy());
  EXPECT_EQ(
      client.receive(20070, {{"interaction_anim_id", 200}}, 3, position, kStart + 933 * kTicksPerMs)
          .at(0)
          .opcode,
      2);
  client.receive(20069, request, 4, position, kStart);
  ASSERT_TRUE(client.next_action_tick());
  check_wire(client.advance(*client.next_action_tick(), position));
  EXPECT_FALSE(client.next_action_tick());
  EXPECT_EQ(durable.stackables.at(100109), 1);
  EXPECT_EQ(commits, 0);
}

TEST_F(TutorialInteractionTest, FailedCommitDoesNotMarkInteractionExecuted) {
  durable.quests[10000042].objectives[1] = 0;
  durable.stackables[100109] = 1;
  auto client = session();
  client.receive(20069, start_request(kDoor, 200, 49013), 1, position, kStart);
  allow_commit = false;
  EXPECT_THROW(client.receive(20070, {{"interaction_anim_id", 200}}, 2, position,
                              kStart + 933 * kTicksPerMs),
               std::runtime_error);
  EXPECT_EQ(client.progress().stackables.at(100109), 1);
  allow_commit = true;
  client.receive(20070, {{"interaction_anim_id", 200}}, 3, position, kStart + 1000 * kTicksPerMs);
  EXPECT_EQ(durable.stackables.at(100109), 0);
  EXPECT_EQ(commits, 1);
}

TEST_F(TutorialInteractionTest, AnimatedNoteAwardsExperienceOnceAndCannotBypassAnimation) {
  position.map_id = 10117;
  auto client = session(10117);
  EXPECT_EQ(client
                .receive(20068,
                         {{"target_unit_id", std::to_string(kNote)},
                          {"param", {{"interaction_id", 305}}}},
                         1, position, kStart)
                .at(0)
                .opcode,
            2);
  client.receive(20069, start_request(kNote, 305, 305), 2, position, kStart);
  check_wire(client.receive(20070, {{"interaction_anim_id", 305}}, 3, position,
                            kStart + 616 * kTicksPerMs));
  auto reconnected = session(10117);
  reconnected.receive(20069, start_request(kNote, 305, 305), 4, position, kStart);
  reconnected.receive(20070, {{"interaction_anim_id", 305}}, 5, position,
                      kStart + 616 * kTicksPerMs);
  EXPECT_EQ(durable.experience, 100);
  EXPECT_EQ(durable.adventure_objects.at(2010033), 2);
  EXPECT_EQ(commits, 1);
}

TEST_F(TutorialInteractionTest, DirectObjectPlaysSceneOnceAndResetsWithoutReopeningObjective) {
  position.map_id = 10117;
  durable.quests[10000046].objectives[1] = 0;
  auto client = session(10117);
  const Json request{{"target_unit_id", std::to_string(kScene)},
                     {"param", {{"interaction_id", 10000048}}}};
  const auto packets = client.receive(20068, request, 1, position, kStart);
  ASSERT_EQ(packets.size(), 4);
  EXPECT_FALSE(client.busy());
  EXPECT_TRUE(client.next_action_tick());
  check_wire(packets);
  EXPECT_EQ(packets.at(1).opcode, 70);
  EXPECT_EQ(client.receive(20068, request, 2, position, kStart).size(), 1);
  check_wire(client.advance(kStart + 10000 * kTicksPerMs, position));
  EXPECT_EQ(durable.doodad_phases.at(kScene), 1);
  EXPECT_EQ(client.receive(20068, request, 3, position, kStart + 11000 * kTicksPerMs).size(), 1);
  EXPECT_EQ(durable.quests.at(10000046).objectives.at(1), 1);
}

TEST_F(TutorialInteractionTest, MapAndRangeChecksRejectForeignObjectActions) {
  auto client = session(10117);
  const auto foreign_map =
      client.receive(20069, start_request(kNote, 305, 305), 1, position, kStart);
  ASSERT_EQ(foreign_map.size(), 1);
  EXPECT_EQ(foreign_map.at(0).opcode, 165);
  EXPECT_EQ(foreign_map.at(0).message.at("ec"), "BadRequest");
  check_wire(foreign_map);

  position.map_id = 10117;
  position.x_mm = 100000;
  const auto too_far = client.receive(20069, start_request(kNote, 305, 305), 2, position, kStart);
  ASSERT_EQ(too_far.size(), 1);
  EXPECT_EQ(too_far.at(0).opcode, 165);
  EXPECT_EQ(too_far.at(0).message.at("ec"), "TooFar");
  EXPECT_FALSE(client.busy());
}

// Live regression (2026-11-01): the client reports objects within 100 m on the ground plane,
// so a cliff-top object 50 m higher, seen from a tracked position that lags the client's own
// by 3 m, was refused as TooFar and showed "Target is too far." while the player only walked.
TEST_F(TutorialInteractionTest, DiscoveryUsesTheClientsHorizontalRadius) {
  auto client = session();
  const Json discovery{{"spot_object_id", std::to_string(kChest)}};

  position.x_mm = 103000;
  position.z_mm = 50000;
  const auto below_cliff = client.receive(20050, discovery, 1, position, kStart);
  ASSERT_EQ(below_cliff.size(), 1);
  EXPECT_EQ(below_cliff.at(0).opcode, 90);
  EXPECT_EQ(durable.adventure_objects.at(kChestCmsId), 1);
  check_wire(below_cliff);

  position.x_mm = 106000;
  position.z_mm = 0;
  const auto beyond = client.receive(20050, discovery, 2, position, kStart);
  ASSERT_EQ(beyond.size(), 1);
  EXPECT_EQ(beyond.at(0).opcode, 2);
  EXPECT_EQ(beyond.at(0).message.at("ec"), "TooFar");
}

TEST_F(TutorialInteractionTest, TreasureChestGrantsItsNecklaceInstanceOnceAtTheExecuteEvent) {
  auto client = chest_session();

  // Captured chest starts echo the request yaw and carry no target_unit_id.
  const auto started = client.receive(20069, chest_start(), 1, position, kStart);
  ASSERT_EQ(started.size(), 1);
  EXPECT_EQ(started.at(0).opcode, 166);
  EXPECT_FALSE(started.at(0).message.contains("target_unit_id"));
  EXPECT_DOUBLE_EQ(started.at(0).message.at("yaw").get<double>(), -45.5);
  check_wire(started);

  const Json execute{{"interaction_anim_id", kChestAnimation}};
  const auto early = client.receive(20070, execute, 2, position, kStart + 2000 * kTicksPerMs);
  ASSERT_EQ(early.size(), 1);
  EXPECT_EQ(early.at(0).opcode, 2);
  EXPECT_TRUE(stored_items.empty());

  // Official order: ActivateAdventureObjectRes, ItemsUpdateNoti, InteractionAnimRes.
  const auto executed =
      client.receive(20070, execute, 3, position, kStart + kChestExecuteMs * kTicksPerMs);
  ASSERT_EQ(executed.size(), 3);
  EXPECT_EQ(executed.at(0).opcode, 91);
  EXPECT_EQ(executed.at(0).message.at("adventure_object").at("cms_id"), kChestCmsId);
  EXPECT_EQ(executed.at(1).opcode, 169);
  EXPECT_TRUE(executed.at(1).message.contains("adventure_object_activate"));
  const auto &items = executed.at(1).message.at("items");
  ASSERT_EQ(items.size(), 1);
  EXPECT_EQ(items.at(0).at("item_cms_id"), kNecklace);
  EXPECT_EQ(items.at(0).at("item_id"), std::to_string(kFirstStoredItemId));
  EXPECT_EQ(items.at(0).at("durability"), 10000);
  EXPECT_EQ(executed.at(2).opcode, 167);
  check_wire(executed);

  ASSERT_EQ(stored_items.size(), 1);
  EXPECT_EQ(stored_items.at(0).cms_id, kNecklace);
  EXPECT_EQ(stored_items.at(0).grade, x5::cms::EX5ItemGrade_Normal);
  EXPECT_EQ(stored_items.at(0).durability, 10000);
  EXPECT_FALSE(stored_items.at(0).equip_slot.has_value());
  EXPECT_EQ(durable.adventure_objects.at(kChestCmsId), 2);
  EXPECT_FALSE(durable.stackables.contains(kNecklace));
  EXPECT_EQ(commits, 1);

  const auto ended = client.receive(
      20071, {{"end_interaction_anim", {{"interaction_anim_id", kChestAnimation}}}}, 4, position,
      kStart + 4000 * kTicksPerMs);
  ASSERT_EQ(ended.size(), 1);
  EXPECT_EQ(ended.at(0).opcode, 167);
  EXPECT_FALSE(client.busy());

  // After reconnect the chest stays opened: the animation plays without a second item.
  auto reconnected = chest_session();
  reconnected.receive(20069, chest_start(), 5, position, kStart);
  const auto repeated =
      reconnected.receive(20070, execute, 6, position, kStart + kChestExecuteMs * kTicksPerMs);
  ASSERT_EQ(repeated.size(), 1);
  EXPECT_EQ(repeated.at(0).opcode, 167);
  EXPECT_EQ(stored_items.size(), 1);
  EXPECT_EQ(commits, 1);
}

TEST_F(TutorialInteractionTest, TreasureChestStaysClosedWhenItsItemCannotBeStored) {
  auto client = chest_session();
  inventory_full = true;
  const Json execute{{"interaction_anim_id", kChestAnimation}};
  client.receive(20069, chest_start(), 1, position, kStart);

  const auto rejected =
      client.receive(20070, execute, 2, position, kStart + kChestExecuteMs * kTicksPerMs);
  ASSERT_EQ(rejected.size(), 1);
  EXPECT_EQ(rejected.at(0).opcode, 2);
  EXPECT_EQ(rejected.at(0).message.at("ec"), "ItemSlotLimit");
  check_wire(rejected);
  EXPECT_FALSE(client.progress().adventure_objects.contains(kChestCmsId));
  EXPECT_EQ(commits, 0);

  // The same animation can still execute once space is available.
  inventory_full = false;
  const auto retried =
      client.receive(20070, execute, 3, position, kStart + 3000 * kTicksPerMs);
  ASSERT_FALSE(retried.empty());
  EXPECT_EQ(retried.at(0).opcode, 91);
  EXPECT_EQ(stored_items.size(), 1);
  EXPECT_EQ(commits, 1);
}

TEST_F(TutorialInteractionTest, TreasureChestRequiresItemStorageAndItsAnimation) {
  auto client = session();
  EXPECT_EQ(client
                .receive(20068,
                         {{"target_unit_id", std::to_string(kChest)},
                          {"param", {{"interaction_id", kChestInteraction}}}},
                         1, position, kStart)
                .at(0)
                .opcode,
            2);

  client.receive(20069, chest_start(), 2, position, kStart);
  const auto rejected = client.receive(20070, {{"interaction_anim_id", kChestAnimation}}, 3,
                                       position, kStart + kChestExecuteMs * kTicksPerMs);
  ASSERT_EQ(rejected.size(), 1);
  EXPECT_EQ(rejected.at(0).message.at("ec"), "Interaction_ConditionsNotMatch");
  EXPECT_FALSE(client.progress().adventure_objects.contains(kChestCmsId));
  EXPECT_EQ(commits, 0);
}

TEST_F(TutorialInteractionTest, InstanceRewardsAreLimitedToAdventureObjects) {
  auto profile = interaction_profile();
  profile["quests"][0]["rewards"] = profile["objects"][0]["rewards"];
  EXPECT_THROW(TutorialData{profile}, std::runtime_error);

  auto unknown_grade = interaction_profile();
  unknown_grade["objects"][0]["rewards"][0]["instance"]["grade"] = "NotAnItemGrade";
  EXPECT_THROW(TutorialData{unknown_grade}, std::runtime_error);
}

} // namespace
} // namespace chronicles
