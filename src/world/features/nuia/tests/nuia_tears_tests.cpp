#include "protocol/protocol.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/nuia/nuia_tears.hpp"
#include "world/features/nuia/tests/nuia_test_support.hpp"

#include <gtest/gtest.h>
#include <limits>
namespace chronicles {
namespace {

using Json = nlohmann::json;

using nuia_test::tear_fixture;

using nuia_test::tear_request;

const WorldPosition kTearPosition{9012, 1000, 2000, 3000, 0};

TEST(NuiaTears, CollectionGrantsCatalogQuantitiesAndEncodesCapturedPackets) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const int quantity : {1, 5, 10}) {
    const NuiaTears tears(tear_fixture(quantity));
    const auto result = tears.receive(20049, tear_request(), 1, kTearPosition, {});
    ASSERT_TRUE(result.next);
    EXPECT_EQ(result.next->stackables.at(2000), quantity);
    EXPECT_EQ(result.next->adventure_objects.at(5010058), 2);
    EXPECT_EQ(result.next->revision, 1);
    ASSERT_EQ(result.packets.size(), 2);
    EXPECT_EQ(result.packets[0].opcode, 91);
    EXPECT_EQ(result.packets[1].opcode, 169);
    EXPECT_TRUE(result.packets[1].message.contains("adventure_object_activate"));
    for (const auto &packet : result.packets) {
      EXPECT_EQ(protocol.decode(protocol.encode(packet.opcode, packet.message)).at("status"),
                "decoded");
    }
  }
}

TEST(NuiaTears, DiscoveryDoesNotGrantAndCannotUndoCollection) {
  const NuiaTears tears(tear_fixture());
  const auto discovery = tears.receive(20050, tear_request(), 1, kTearPosition, {});
  ASSERT_TRUE(discovery.next);
  EXPECT_TRUE(discovery.next->stackables.empty());
  EXPECT_EQ(discovery.next->adventure_objects.at(5010058), 1);
  const auto collected = tears.receive(20049, tear_request(), 2, kTearPosition, *discovery.next);
  ASSERT_TRUE(collected.next);
  const auto repeat = tears.receive(20050, tear_request(), 3, kTearPosition, *collected.next);
  EXPECT_FALSE(repeat.next);
  EXPECT_EQ(repeat.packets.front().message.at("adventure_objects")[0].at("state"),
            "EX5AdventureObjectState_Activated");
}

TEST(NuiaTears, DeadReconnectDiscoveryDoesNotShowAnErrorOrChangeProgress) {
  const NuiaTears tears(tear_fixture());
  WorldProgress progress;
  progress.dead = true;
  progress.revision = 7;
  for (const auto &spot : {"123006", "9999"}) {
    auto request = tear_request();
    request["spot_object_id"] = spot;
    const auto result = tears.receive(20050, request, 1, kTearPosition, progress);
    EXPECT_TRUE(result.packets.empty());
    EXPECT_FALSE(result.next);
  }

  const auto activation = tears.receive(20049, tear_request(), 2, kTearPosition, progress);
  ASSERT_EQ(activation.packets.size(), 1);
  EXPECT_TRUE(activation.packets.front().message.contains("ec"));
  EXPECT_FALSE(activation.next);
  EXPECT_TRUE(progress.dead);
  EXPECT_EQ(progress.revision, 7);
  EXPECT_TRUE(progress.adventure_objects.empty());
  EXPECT_TRUE(progress.stackables.empty());
}

TEST(NuiaTears, ReconnectAndAnotherMapCannotAwardTheSameCmsTwice) {
  const NuiaTears tears(tear_fixture());
  const auto collected = tears.receive(20049, tear_request(), 1, kTearPosition, {});
  ASSERT_TRUE(collected.next);
  auto other_map = kTearPosition;
  other_map.map_id = 9011;
  const auto repeated = tears.receive(20049, tear_request(), 2, other_map, *collected.next);
  EXPECT_FALSE(repeated.next);
  ASSERT_EQ(repeated.packets.size(), 1);
  EXPECT_EQ(repeated.packets[0].opcode, 91);
  const auto snapshot = NuiaTears::snapshot(*collected.next);
  EXPECT_EQ(snapshot.opcode, 127);
  EXPECT_EQ(snapshot.message.at("adventure_objects")[0].at("cms_id"), 5010058);
  // An independent character still owns an uncollected copy.
  EXPECT_TRUE(tears.receive(20049, tear_request(), 1, kTearPosition, {}).next);
}

// Live regression (2026-11-01): tears on cliffs were reported at 100 m on the ground plane but
// up to 112 m in 3D, and each refusal showed "Target is too far." to a player who only walked.
TEST(NuiaTears, DiscoveryUsesTheClientsHorizontalRadius) {
  const NuiaTears tears(tear_fixture());
  auto tracked = kTearPosition;
  tracked.x_mm += 103000;
  tracked.z_mm -= 51000;
  auto request = tear_request();
  request["pos"]["millimeter_x"] = tracked.x_mm - 3000;
  request["pos"]["millimeter_z"] = tracked.z_mm;

  const auto below_cliff = tears.receive(20050, request, 1, tracked, {});
  ASSERT_TRUE(below_cliff.next);
  EXPECT_EQ(below_cliff.next->adventure_objects.at(5010058), 1);
  EXPECT_TRUE(below_cliff.next->stackables.empty());

  tracked.x_mm = kTearPosition.x_mm + 106000;
  request["pos"]["millimeter_x"] = tracked.x_mm;
  const auto beyond = tears.receive(20050, request, 2, tracked, {});
  EXPECT_FALSE(beyond.next);
  ASSERT_EQ(beyond.packets.size(), 1);
  EXPECT_EQ(beyond.packets.front().message.at("ec"), "TooFar");
}

TEST(NuiaTears, UnknownWrongMapFarAndSpoofedRequestsDoNotGrant) {
  const NuiaTears tears(tear_fixture());
  auto unknown = tear_request();
  unknown["spot_object_id"] = "9999";
  auto wrong_map = kTearPosition;
  wrong_map.map_id = 8000;
  auto far = kTearPosition;
  far.x_mm += 100000;
  auto spoof = tear_request();
  spoof["pos"]["millimeter_x"] = 20000;
  for (const auto &result : {tears.receive(20049, unknown, 1, kTearPosition, {}),
                             tears.receive(20049, tear_request(), 1, wrong_map, {}),
                             tears.receive(20049, tear_request(), 1, far, {}),
                             tears.receive(20049, spoof, 1, kTearPosition, {})}) {
    EXPECT_FALSE(result.next);
    ASSERT_EQ(result.packets.size(), 1);
    EXPECT_EQ(result.packets[0].opcode, 91);
    EXPECT_TRUE(result.packets[0].message.contains("ec"));
  }
}

TEST(NuiaTears, InvalidInputsOverflowAndDeadCharactersFailClosed) {
  const NuiaTears tears(tear_fixture());
  for (const auto &id : {"", "123x", "-1", "18446744073709551616"}) {
    auto request = tear_request();
    request["spot_object_id"] = id;
    EXPECT_FALSE(tears.receive(20049, request, 1, kTearPosition, {}).next);
  }
  EXPECT_FALSE(tears.receive(20049, Json::object(), 1, kTearPosition, {}).next);
  WorldProgress current;
  current.stackables[2000] = std::numeric_limits<int>::max();
  EXPECT_FALSE(tears.receive(20049, tear_request(), 1, kTearPosition, current).next);
  current.stackables.clear();
  current.revision = std::numeric_limits<std::int64_t>::max();
  EXPECT_FALSE(tears.receive(20049, tear_request(), 1, kTearPosition, current).next);
  current.revision = 0;
  current.dead = true;
  EXPECT_FALSE(tears.receive(20049, tear_request(), 1, kTearPosition, current).next);
}

TEST(NuiaTears, CapturedEightMetreDistanceFitsAndDistantCollectionIsRefused) {
  const NuiaTears tears(tear_fixture());
  auto position = kTearPosition;
  position.x_mm += 8001;
  auto request = tear_request();
  request["pos"]["millimeter_x"] = position.x_mm;
  EXPECT_TRUE(tears.receive(20049, request, 1, position, {}).next);

  position.x_mm = kTearPosition.x_mm + 10001;
  request["pos"]["millimeter_x"] = position.x_mm;
  const auto rejected = tears.receive(20049, request, 2, position, {});
  EXPECT_FALSE(rejected.next);
  EXPECT_EQ(rejected.packets.front().message.at("ec"), "TooFar");
}

TEST(NuiaTears, InvalidBuildDuplicatePlacementsAndCoordinatesAreRejected) {
  auto fixture = tear_fixture();
  fixture["build_id"] = "another-build";
  EXPECT_THROW(NuiaTears{fixture}, std::runtime_error);
  fixture = tear_fixture();
  fixture["placements"].push_back(fixture["placements"][0]);
  EXPECT_THROW(NuiaTears{fixture}, std::runtime_error);
  fixture = tear_fixture();
  fixture["placements"][0]["position"][0] = 1.5;
  EXPECT_THROW(NuiaTears{fixture}, std::runtime_error);
  fixture["placements"][0]["position"][0] = std::numeric_limits<std::uint64_t>::max();
  EXPECT_THROW(NuiaTears{fixture}, std::runtime_error);
  fixture = tear_fixture();
  fixture["placements"][0]["map_id"] = std::numeric_limits<std::uint64_t>::max();
  EXPECT_THROW(NuiaTears{fixture}, std::runtime_error);
  fixture = tear_fixture();
  fixture["placements"][0]["quantity"] = 1.5;
  EXPECT_THROW(NuiaTears{fixture}, std::runtime_error);
}

TEST(NuiaTears, RevisionCommitKeepsRewardAndConsumedMarkerAtomic) {
  MemoryRepositories store;
  Account account;
  account.name = "tear_fixture";
  ASSERT_TRUE(store.accounts->add_account(account));
  const auto owner = store.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "TearFixture";
  character = store.characters->create(character);
  const auto before = store.character_state->load_world_progress(owner->id, character.id, 1);
  const NuiaTears tears(tear_fixture());
  const auto first = tears.receive(20049, tear_request(), 1, kTearPosition, before);
  const auto stale = tears.receive(20049, tear_request(), 1, kTearPosition, before);
  ASSERT_TRUE(first.next);
  ASSERT_TRUE(stale.next);
  EXPECT_TRUE(
      store.character_state->save_world_progress(owner->id, character.id, 1, before, *first.next));
  EXPECT_FALSE(
      store.character_state->save_world_progress(owner->id, character.id, 1, before, *stale.next));
  const auto restored = store.character_state->load_world_progress(owner->id, character.id, 1);
  EXPECT_EQ(restored.stackables.at(2000), 1);
  EXPECT_EQ(restored.adventure_objects.at(5010058), 2);
  EXPECT_FALSE(tears.receive(20049, tear_request(), 2, kTearPosition, restored).next);
}

} // namespace
} // namespace chronicles
