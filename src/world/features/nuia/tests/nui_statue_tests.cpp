#include "world/features/nuia/nui_statue.hpp"
#include "world/features/nuia/tests/nuia_test_support.hpp"

#include "protocol/protocol.hpp"

#include <gtest/gtest.h>
namespace chronicles {
namespace {

using Json = nlohmann::json;

using nuia_test::statue_fixture;

using nuia_test::start_request;

constexpr std::int64_t kStartTick = 10'000'000;
constexpr std::int64_t kExecuteTick = kStartTick + 50'000'000;
const WorldPosition kPosition{9012, 0, 0, 0, 0};

TEST(NuiStatue, DeadReconnectDiscoveryDoesNotShowAnErrorOrCommit) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  NuiStatueSession session(data, 17, [](const auto &, const auto &) {
    ADD_FAILURE() << "Discovery while dead must not commit";
    return false;
  });
  WorldProgress progress;
  progress.dead = true;
  progress.revision = 7;
  const Json request{{"spot_object_id", "4102"}, {"pos", Json::object()}};
  const auto discovery = session.receive(20050, request, 1, kPosition, kStartTick, progress, false);
  EXPECT_TRUE(discovery.empty());
  const auto activation =
      session.receive(20049, request, 2, kPosition, kStartTick, progress, false);
  ASSERT_EQ(activation.size(), 1);
  EXPECT_TRUE(activation.front().message.contains("ec"));
  EXPECT_TRUE(progress.dead);
  EXPECT_EQ(progress.revision, 7);
  EXPECT_TRUE(progress.adventure_objects.empty());
}

TEST(NuiStatue, DiscoveryUsesTheClientsHorizontalRadius) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  WorldProgress progress;
  NuiStatueSession session(data, 17, [](const auto &, const auto &) { return true; });

  // The client reports from 100 m on the ground plane; the tracked position lags it by 3 m and
  // the statue stands 45 m higher.
  const WorldPosition tracked{9012, 103000, 0, -45000, 0};
  const Json request{{"spot_object_id", "4102"},
                     {"pos", {{"millimeter_x", 100000}, {"millimeter_z", -45000}}}};
  const auto discovery = session.receive(20050, request, 1, tracked, kStartTick, progress, false);
  ASSERT_EQ(discovery.size(), 1);
  EXPECT_EQ(discovery.front().opcode, 90);
  EXPECT_EQ(progress.adventure_objects.at(6010000), 1);

  const WorldPosition beyond{9012, 106000, 0, 0, 0};
  const Json beyond_request{{"spot_object_id", "4102"}, {"pos", {{"millimeter_x", 106000}}}};
  const auto refused =
      session.receive(20050, beyond_request, 2, beyond, kStartTick, progress, false);
  ASSERT_EQ(refused.size(), 1);
  EXPECT_EQ(refused.front().message.at("ec"), "TooFar");
}

TEST(NuiStatue, ActivationWaitsForAuthoredEventAndCommitsBeforeSuccess) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  WorldProgress progress;
  int commits = 0;
  NuiStatueSession session(data, 17, [&](const auto &before, const auto &after) {
    EXPECT_FALSE(before.adventure_objects.contains(6010000));
    EXPECT_EQ(after.adventure_objects.at(6010000), 2);
    EXPECT_EQ(after.revision, before.revision + 1);
    ++commits;
    return true;
  });

  const auto started =
      session.receive(20069, start_request(), 1, kPosition, kStartTick, progress, false);
  ASSERT_EQ(started.size(), 1);
  EXPECT_EQ(started.front().opcode, 166);
  EXPECT_EQ(commits, 0);

  const auto early = session.receive(20070, {{"interaction_anim_id", 10}}, 2, kPosition,
                                     kStartTick + 10'000, progress, false);
  ASSERT_EQ(early.size(), 1);
  EXPECT_EQ(early.front().opcode, 167);
  EXPECT_EQ(commits, 0);

  const auto executed = session.receive(20070, {{"interaction_anim_id", 10}}, 3, kPosition,
                                        kExecuteTick, progress, false);
  ASSERT_GE(executed.size(), 2);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(progress.adventure_objects.at(6010000), 2);
  EXPECT_EQ(executed.front().opcode, 91);
  EXPECT_EQ(executed.back().opcode, 167);
  EXPECT_TRUE(progress.stackables.empty());

  session.receive(20070, {{"interaction_anim_id", 10}}, 4, kPosition, kExecuteTick + 1, progress,
                  false);
  EXPECT_EQ(commits, 1);
}

TEST(NuiStatue, RepeatedStartDoesNotMoveDeadlineAndCancellationGrantsNothing) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  WorldProgress progress;
  int commits = 0;
  NuiStatueSession session(data, 17, [&](const auto &, const auto &) {
    ++commits;
    return true;
  });
  session.receive(20069, start_request(), 1, kPosition, kStartTick, progress, false);
  session.receive(20069, start_request(), 2, kPosition, kStartTick + 40'000'000, progress, false);
  session.receive(20070, {{"interaction_anim_id", 10}}, 3, kPosition, kExecuteTick, progress,
                  false);
  EXPECT_EQ(commits, 1);

  WorldProgress other;
  NuiStatueSession cancelled(data, 33, [&](const auto &, const auto &) {
    ADD_FAILURE() << "A cancelled action must never commit";
    return true;
  });
  cancelled.receive(20069, start_request(), 1, kPosition, kStartTick, other, false);
  cancelled.receive(20071, {{"end_interaction_anim", {{"interaction_anim_id", 10}}}}, 2, kPosition,
                    kStartTick + 1, other, false);
  cancelled.receive(20070, {{"interaction_anim_id", 10}}, 3, kPosition, kExecuteTick, other, false);
  EXPECT_TRUE(other.adventure_objects.empty());
}

TEST(NuiStatue, FailedCommitPublishesNoActivationAndCanRetry) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  WorldProgress progress;
  bool allow_commit = false;
  NuiStatueSession session(data, 17, [&](const auto &, const auto &) { return allow_commit; });
  session.receive(20069, start_request(), 1, kPosition, kStartTick, progress, false);
  const auto refused = session.receive(20070, {{"interaction_anim_id", 10}}, 2, kPosition,
                                       kExecuteTick, progress, false);
  for (const auto &packet : refused) {
    EXPECT_NE(packet.opcode, 91);
  }
  EXPECT_TRUE(progress.adventure_objects.empty());
  allow_commit = true;
  session.receive(20070, {{"interaction_anim_id", 10}}, 3, kPosition, kExecuteTick + 1, progress,
                  false);
  EXPECT_EQ(progress.adventure_objects.at(6010000), 2);
}

TEST(NuiStatue, DeadBusyWrongMapMovedAndSpoofedTargetsFailClosed) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  const auto never_commit = [](const auto &, const auto &) {
    ADD_FAILURE() << "Invalid actions must not commit";
    return true;
  };
  WorldProgress progress;
  NuiStatueSession session(data, 17, never_commit);
  progress.dead = true;
  EXPECT_EQ(session.receive(20069, start_request(), 1, kPosition, kStartTick, progress, false)
                .front()
                .opcode,
            165);
  progress.dead = false;
  EXPECT_EQ(session.receive(20069, start_request(), 1, kPosition, kStartTick, progress, true)
                .front()
                .opcode,
            165);
  auto spoofed = start_request();
  spoofed["target_unit_id"] = "999999";
  EXPECT_EQ(
      session.receive(20069, spoofed, 1, kPosition, kStartTick, progress, false).front().opcode,
      165);
  const WorldPosition another_map{8000, 0, 0, 0, 0};
  EXPECT_EQ(session.receive(20069, start_request(), 1, another_map, kStartTick, progress, false)
                .front()
                .opcode,
            165);
  session.receive(20069, start_request(), 1, kPosition, kStartTick, progress, false);
  const WorldPosition distant{9012, 20000, 0, 0, 0};
  session.receive(20070, {{"interaction_anim_id", 10}}, 2, distant, kExecuteTick, progress, false);
  EXPECT_TRUE(progress.adventure_objects.empty());
}

TEST(NuiStatue, RevivalSelectsSameMapTypeAndRequiresActivatedStatue) {
  const NuiStatueData data(statue_fixture());
  WorldProgress progress;
  EXPECT_FALSE(data.revival_destination(kPosition, progress, false));
  progress.adventure_objects[6010000] = 2;
  const auto statue = data.revival_destination(kPosition, progress, false);
  ASSERT_TRUE(statue);
  EXPECT_EQ(statue->x_mm, 500);
  const auto flame = data.revival_destination(kPosition, progress, true);
  ASSERT_TRUE(flame);
  EXPECT_EQ(flame->x_mm, 20500);
  EXPECT_FALSE(data.revival_destination({8000, 0, 0, 0, 0}, progress, true));
}

TEST(NuiStatue, OfferingSpendsOwnedTearsAndCrossesIncrementalThresholdsAtomically) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  WorldProgress progress;
  progress.adventure_objects[6010000] = 2;
  progress.stackables[2000] = 10;
  int commits = 0;
  NuiStatueSession session(data, 17, [&](const auto &before, const auto &after) {
    EXPECT_EQ(before.stackables.at(2000) - after.stackables.at(2000), 4);
    ++commits;
    return true;
  });
  const Json offering{{"item_cms_id", 2000}, {"count", 4}, {"target_unit_id", "4102"}};
  const auto packets = session.receive(20077, offering, 1, kPosition, kStartTick, progress, false);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(progress.stackables.at(2000), 6);
  EXPECT_EQ(progress.nui_blessing_level, 2);
  EXPECT_EQ(progress.dedicated_nui_tears, 1);
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets[0].opcode, 169);
  EXPECT_EQ(packets[1].opcode, 74);
  EXPECT_EQ(packets[1].message.at("dedicated_nui_tears"), 1);
}

TEST(NuiStatue, RepeatedOfferingSequenceCannotSpendTearsTwice) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  WorldProgress progress;
  progress.stackables[2000] = 6;
  int commits = 0;
  NuiStatueSession session(data, 17, [&](const auto &, const auto &) {
    ++commits;
    return true;
  });
  const Json request{{"item_cms_id", 2000}, {"count", 1}, {"target_unit_id", "4102"}};
  session.receive(20077, request, 7, kPosition, kStartTick, progress, false);
  session.receive(20077, request, 7, kPosition, kStartTick + 1, progress, false);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(progress.stackables.at(2000), 5);
}

TEST(NuiStatue, OfferingRejectsInvalidCountsTargetsAndInsufficientOwnedTears) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  WorldProgress progress;
  progress.stackables[2000] = 2;
  NuiStatueSession session(data, 17, [](const auto &, const auto &) {
    ADD_FAILURE() << "Invalid offering must not commit";
    return true;
  });
  for (int count : {0, -1, 3, 1000000000}) {
    const Json request{{"item_cms_id", 2000}, {"count", count}, {"target_unit_id", "4102"}};
    const auto packets = session.receive(20077, request, 1, kPosition, kStartTick, progress, false);
    ASSERT_EQ(packets.size(), 1);
    EXPECT_EQ(packets[0].opcode, 2);
  }
  EXPECT_EQ(progress.stackables.at(2000), 2);
  EXPECT_EQ(progress.nui_blessing_level, 0);
}

TEST(NuiStatue, RefillReservesOwnedItemsWithoutCreatingOrSpendingInventory) {
  auto data = std::make_shared<NuiStatueData>(statue_fixture());
  WorldProgress progress;
  progress.stackables[40101101] = 7;
  progress.utility_slots[0] = {40101101, 1};
  progress.utility_slots[1] = {40101101, 1};
  NuiStatueSession session(data, 17, [](const auto &, const auto &) { return true; });
  session.receive(20069, start_request(16), 1, kPosition, kStartTick, progress, false);
  const auto packets = session.receive(20070, {{"interaction_anim_id", 16}}, 2, kPosition,
                                       kStartTick + 34'080'000, progress, false);
  EXPECT_EQ(progress.stackables.at(40101101), 7);
  EXPECT_EQ(progress.utility_slots[0].count, 5);
  EXPECT_EQ(progress.utility_slots[1].count, 2);
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets.front().opcode, 104);
  EXPECT_EQ(packets.back().opcode, 167);
}

TEST(NuiStatue, AuthoredRevivalSuppliesTopUpToFloorAndRefillWithoutRepeatRewards) {
  const NuiStatueData data(statue_fixture());
  WorldProgress progress;
  progress.stackables[40101101] = 2;
  progress.utility_slots[0] = {40101101, 1};
  const auto packets = data.apply_revival_supplies(1, progress);
  EXPECT_EQ(progress.stackables.at(40101101), 5);
  EXPECT_EQ(progress.utility_slots[0].count, 5);
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets[0].opcode, 169);
  EXPECT_EQ(packets[1].opcode, 104);
  data.apply_revival_supplies(1, progress);
  EXPECT_EQ(progress.stackables.at(40101101), 5);
  progress.stackables[40101101] = 9;
  data.apply_revival_supplies(1, progress);
  EXPECT_EQ(progress.stackables.at(40101101), 9);
}

TEST(NuiStatue, EquippedBagUsesRecoveredGroupTierRatherThanAnAdditiveBonus) {
  auto fixture = statue_fixture();
  fixture["utility_groups"] = {{"1000", {5, 6, 7, 8, 9, 10}}};
  fixture["utility_items"] = {{"40101101", 1000}};
  fixture["bag_options"] = {{"12700201", {{"level", 2}, {"groups", {1000}}}},
                            {"12700202", {{"level", 99}, {"groups", {1000}}}}};
  const NuiStatueData data(fixture);
  Character character;
  CharacterItem bag;
  bag.cms_id = 12700201;
  bag.equip_slot = 24;
  character.items.push_back(bag);
  EXPECT_EQ(data.utility_capacity(40101101, &character), 6);
  character.items[0].cms_id = 12700202;
  EXPECT_EQ(data.utility_capacity(40101101, &character), 5);
  character.items[0].equip_slot.reset();
  EXPECT_EQ(data.utility_capacity(40101101, &character), 5);
}

TEST(NuiStatue, MalformedDefinitionsAndDuplicateIdentitiesAreRejected) {
  auto document = statue_fixture();
  document["build_id"] = "unknown";
  EXPECT_THROW(NuiStatueData{document}, std::runtime_error);
  document = statue_fixture();
  document["placements"].push_back(document["placements"][0]);
  EXPECT_THROW(NuiStatueData{document}, std::runtime_error);
  document = statue_fixture();
  document["placements"][0]["position"][0] = 9'000'000'000LL;
  EXPECT_THROW(NuiStatueData{document}, std::runtime_error);
}

} // namespace
} // namespace chronicles
