#include "common/world_metrics.hpp"
#include "world/features/nuia/nuia_feature.hpp"
#include "world/features/nuia/tests/nuia_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>

namespace chronicles::nuia {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kStartTick = 10'000'000;
const WorldPosition kStatuePosition{9012, 0, 0, 0, 0};
const WorldPosition kTearPosition{9012, 1000, 2000, 3000, 0};

class NuiaContext final : public ProgressAccess, public NuiaSessionContext {
public:
  NuiaContext() {
    actor.id = 17;
    stored.revision = 7;
    stored.adventure_objects[6010000] = 2;
    stored.stackables[2000] = 10;
  }

  WorldProgress load() override {
    calls.push_back("load");
    ++loads;
    if (fail_load) {
      throw std::runtime_error("load failed");
    }
    return stored;
  }

  std::optional<Character> character() override {
    calls.push_back("character");
    ++character_reads;
    if (character_reads == missing_character_read) {
      return std::nullopt;
    }
    auto current = actor;
    current.nui_blessing_level = stored.nui_blessing_level;
    current.dedicated_nui_tears = stored.dedicated_nui_tears;
    return current;
  }

  std::optional<std::vector<CharacterItem>> commit(const WorldProgress &, const WorldProgress &,
                                                   const ItemTrade &) override {
    ADD_FAILURE() << "Nuia must keep its validated progress/revival commit boundaries";
    return std::nullopt;
  }

  bool nuia_has_handoff() const override {
    return authenticated;
  }

  bool nuia_has_tutorial() const override {
    return tutorial;
  }

  std::uint64_t nuia_player_unit_id() const override {
    ++unit_queries;
    return 17;
  }

  std::int64_t nuia_monotonic_ticks() const override {
    return now;
  }

  bool nuia_has_character_stats() const override {
    return has_stats;
  }

  Json nuia_character_stats(const Character &character) const override {
    calls.push_back("stats:" + std::to_string(character.nui_blessing_level));
    if (fail_stats) {
      throw std::runtime_error("unsupported character model");
    }
    return {{"level", character.nui_blessing_level}, {"69", 100}};
  }

  bool nuia_has_combat() const override {
    return combat;
  }

  bool nuia_player_dead() const override {
    return dead;
  }

  bool nuia_other_action_busy() const override {
    blockers.push_back("other");
    return other_busy;
  }

  bool nuia_processing_busy() const override {
    blockers.push_back("processing");
    return processing_busy;
  }

  bool nuia_riding() const override {
    blockers.push_back("riding");
    return riding;
  }

  bool nuia_teleport_pending() override {
    blockers.push_back("escape");
    return teleport_pending;
  }

  bool nuia_server_teleport_pending() override {
    calls.push_back("server-pending");
    return server_pending;
  }

  bool nuia_save_progress(const WorldProgress &before, const WorldProgress &after) override {
    calls.push_back("save");
    ++save_attempts;
    EXPECT_EQ(before, stored);
    EXPECT_EQ(after.revision, before.revision + 1);
    if (!save_allowed) {
      return false;
    }
    stored = after;
    return true;
  }

  void nuia_publish_progress(const WorldProgress &progress) override {
    calls.push_back("progress");
    EXPECT_EQ(progress, stored);
  }

  const Json &nuia_adopt_blessing_stats(const Json &stats) override {
    calls.push_back("combat-stats");
    adopted_model = stats;
    combat_stats = stats;
    combat_stats["69"] = 37;
    return combat_stats;
  }

  void nuia_publish_inventory_snapshot_locked(const Character &character,
                                              const Json *prepared_stats) override {
    std::lock_guard lock(protocol_mutex);
    calls.push_back("snapshot");
    EXPECT_EQ(character.nui_blessing_level, stored.nui_blessing_level);
    ASSERT_NE(prepared_stats, nullptr);
    snapshot_stats = *prepared_stats;
    if (combat) {
      EXPECT_EQ(prepared_stats, &combat_stats);
    }
    auto attempt = std::async(std::launch::async, [this] {
      if (!protocol_mutex.try_lock()) {
        return false;
      }
      protocol_mutex.unlock();
      return true;
    });
    EXPECT_FALSE(attempt.get()) << "Snapshot must remain inside the codec lock";
  }

  GameplayPacket nuia_growth_notification(std::int64_t character_id, const Json &before,
                                          const Json &after, const Json &current) const override {
    calls.push_back("growth");
    EXPECT_EQ(character_id, 17);
    EXPECT_EQ(before.at("level"), 0);
    EXPECT_EQ(after.at("level"), stored.nui_blessing_level);
    EXPECT_EQ(after.at("69"), 100);
    EXPECT_EQ(current.at("69"), combat ? 37 : 100);
    return {19, {{"current", current}}};
  }

  std::vector<GameplayPacket> nuia_legacy_revive() override {
    calls.push_back("legacy");
    return {{24, {{"legacy", true}}}};
  }

  std::optional<WorldPosition> nuia_revival_position() const override {
    calls.push_back("position");
    return position;
  }

  std::optional<std::vector<GameplayPacket>>
  nuia_request_server_teleport(const WorldPosition &destination, std::int64_t tick) override {
    calls.push_back("teleport");
    EXPECT_EQ(tick, now);
    requested_destination = destination;
    if (!teleport_allowed) {
      return std::nullopt;
    }
    return std::vector<GameplayPacket>{{181, {{"tick", "1"}}}};
  }

  std::vector<GameplayPacket> nuia_persist_revival(const WorldPosition &destination,
                                                   int task_id) override {
    calls.push_back("persist");
    ++persist_attempts;
    EXPECT_TRUE(requested_destination.has_value());
    if (requested_destination) {
      EXPECT_EQ(destination, *requested_destination);
    }
    EXPECT_EQ(task_id, 0);
    if (fail_revival) {
      throw std::runtime_error("revision conflict");
    }
    return {{169, {{"supply", true}}}};
  }

  std::vector<GameplayPacket> nuia_revive_combat(std::int64_t tick) override {
    calls.push_back("revive");
    EXPECT_EQ(tick, now);
    return {{49, {}}, {47, {}}, {51, {}}, {48, {}}};
  }

  void nuia_publish_combat_snapshot() override {
    calls.push_back("sync");
  }

  void nuia_publish_player_life() override {
    calls.push_back("life");
  }

  WorldProgress stored;
  Character actor;
  mutable std::vector<std::string> calls;
  mutable std::vector<std::string> blockers;
  world_metrics::TimedMutex protocol_mutex;
  std::optional<WorldPosition> position{kStatuePosition};
  std::optional<WorldPosition> requested_destination;
  Json adopted_model;
  Json combat_stats;
  Json snapshot_stats;
  std::int64_t now{kStartTick};
  mutable int unit_queries{};
  int loads{};
  int character_reads{};
  int missing_character_read{-1};
  int save_attempts{};
  int persist_attempts{};
  bool authenticated{true};
  bool tutorial{};
  bool has_stats{};
  bool fail_stats{};
  bool fail_load{};
  bool combat{true};
  bool dead{true};
  bool other_busy{};
  bool processing_busy{};
  bool riding{};
  bool teleport_pending{};
  bool server_pending{};
  bool teleport_allowed{true};
  bool save_allowed{true};
  bool fail_revival{};
};

NuiaFeature statue_feature() {
  return {std::make_shared<NuiStatueData>(nuia_test::statue_fixture()), std::nullopt};
}

FrameContext request_frame(std::uint16_t opcode, Json message,
                           std::optional<WorldPosition> position = kStatuePosition,
                           std::uint32_t sequence = 1) {
  FrameContext frame(opcode, WorldGameplayRequest{opcode, sequence, std::move(message)});
  frame.gameplay_position = position;
  return frame;
}

FrameContext offering() {
  return request_frame(20077, {{"item_cms_id", 2000}, {"count", 4}, {"target_unit_id", "4102"}});
}

std::vector<std::uint16_t> opcodes(const std::vector<GameplayPacket> &packets) {
  std::vector<std::uint16_t> result;
  for (const auto &packet : packets) {
    result.push_back(packet.opcode);
  }
  return result;
}

TEST(NuiaSession, PassiveQueriesStayColdAndAnUnrelatedSurvivingRequestConstructsTheLazyOwner) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  EXPECT_FALSE(session->has_statue_session());
  EXPECT_FALSE(session->busy());
  EXPECT_FALSE(session->next_action_tick());
  EXPECT_TRUE(session->advance(kStartTick, kStatuePosition).empty());
  session->cancel_action();
  EXPECT_EQ(context.unit_queries, 0);

  auto unrelated = request_frame(20003, Json::object());
  EXPECT_FALSE(session->handle_statue(unrelated));
  EXPECT_TRUE(session->has_statue_session());
  EXPECT_FALSE(session->busy());
  EXPECT_EQ(context.unit_queries, 1);
  EXPECT_EQ(context.loads, 0);
  EXPECT_EQ(context.character_reads, 0);
}

TEST(NuiaSession, MissingPositionOrOwnershipDoesNotConstructTheStatueOwner) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  auto missing_position = request_frame(20003, {}, std::nullopt);
  EXPECT_FALSE(session->handle_statue(missing_position));
  context.authenticated = false;
  auto missing_owner = request_frame(20003, {});
  EXPECT_FALSE(session->handle_statue(missing_owner));
  EXPECT_FALSE(session->has_statue_session());
  EXPECT_EQ(context.unit_queries, 0);
}

TEST(NuiaSession, BusyGuardsShortCircuitBeforeConstructiveEscapeChecks) {
  for (int blocking_owner = 0; blocking_owner < 4; ++blocking_owner) {
    SCOPED_TRACE(blocking_owner);
    auto feature = statue_feature();
    NuiaContext context;
    context.other_busy = blocking_owner == 0;
    context.processing_busy = blocking_owner == 1;
    context.riding = blocking_owner == 2;
    context.teleport_pending = blocking_owner == 3;
    auto session = feature.create_session(context, context);
    auto start = request_frame(20069, nuia_test::start_request());
    EXPECT_TRUE(session->handle_statue(start));
    const std::vector<std::string> order{"other", "processing", "riding", "escape"};
    EXPECT_EQ(context.blockers,
              std::vector<std::string>(order.begin(), order.begin() + blocking_owner + 1));
    EXPECT_FALSE(session->busy());
    EXPECT_EQ(context.save_attempts, 0);
  }
}

TEST(NuiaSession, ActionDeadlineBusySkillAndMapCancellationRetainTheirSeparateRoles) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  auto start = request_frame(20069, nuia_test::start_request());
  ASSERT_TRUE(session->handle_statue(start));
  ASSERT_TRUE(session->busy());
  ASSERT_TRUE(session->next_action_tick());
  EXPECT_EQ(*session->next_action_tick(), kStartTick + 300'000'000);

  auto skill = request_frame(20010, {{"start_skill", {{"skill_id", 123}, {"instance_id", 456}}}});
  ASSERT_TRUE(session->handle_statue_busy_skill(skill));
  ASSERT_EQ(skill.session_packets.size(), 1);
  EXPECT_EQ(skill.session_packets[0].opcode, 13);
  EXPECT_EQ(skill.session_packets[0].message.at("ec"), "Skill_UsingSkill");
  EXPECT_EQ(skill.session_packets[0].message.at("instance_id"), 456);

  session->cancel_action();
  EXPECT_FALSE(session->busy());
  EXPECT_FALSE(session->next_action_tick());
  EXPECT_TRUE(session->has_statue_session());
  auto after_cancel = request_frame(20010, Json::object());
  EXPECT_FALSE(session->handle_statue_busy_skill(after_cancel));
}

TEST(NuiaSession, ExpiryProducesTheExistingActionFailureWithoutLoadingProgress) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  auto start = request_frame(20069, nuia_test::start_request());
  ASSERT_TRUE(session->handle_statue(start));
  const auto loads = context.loads;
  const auto expired = session->advance(kStartTick + 300'000'000, kStatuePosition);
  ASSERT_EQ(expired.size(), 1);
  EXPECT_EQ(expired[0].message.at("ec"), "InteractionAnim_Mismatch");
  EXPECT_FALSE(session->busy());
  EXPECT_EQ(context.loads, loads);
}

TEST(NuiaSession, UnsupportedBlessingModelCannotSpendOrPublishSuccess) {
  auto feature = statue_feature();
  NuiaContext context;
  context.has_stats = true;
  context.fail_stats = true;
  const auto before = context.stored;
  auto session = feature.create_session(context, context);
  auto request = offering();
  EXPECT_THROW(session->handle_statue(request), std::runtime_error);
  EXPECT_EQ(context.stored, before);
  EXPECT_EQ(context.save_attempts, 0);
  EXPECT_TRUE(request.session_packets.empty());
  EXPECT_EQ(context.calls.back(), "stats:2");
}

TEST(NuiaSession, MissingCharacterDuringBlessingValidationRefusesTheCommit) {
  auto feature = statue_feature();
  NuiaContext context;
  context.has_stats = true;
  context.missing_character_read = 2;
  const auto before = context.stored;
  auto session = feature.create_session(context, context);
  auto request = offering();
  ASSERT_TRUE(session->handle_statue(request));
  EXPECT_EQ(context.stored, before);
  EXPECT_EQ(context.save_attempts, 0);
  EXPECT_EQ(opcodes(request.session_packets), std::vector<std::uint16_t>({2}));
}

TEST(NuiaSession, BlessingValidatesThenCommitsCopiesCombatStatsAndSnapshotsUnderTheCodecLock) {
  auto feature = statue_feature();
  NuiaContext context;
  context.has_stats = true;
  context.tutorial = true;
  auto session = feature.create_session(context, context);
  auto request = offering();
  ASSERT_TRUE(session->handle_statue(request));
  EXPECT_EQ(context.stored.stackables.at(2000), 6);
  EXPECT_EQ(context.stored.nui_blessing_level, 2);
  EXPECT_EQ(opcodes(request.session_packets), std::vector<std::uint16_t>({169, 74, 19}));
  EXPECT_EQ(request.session_packets.back().message.at("default_source"), "SCS_Growth");
  EXPECT_EQ(context.adopted_model.at("69"), 100);
  EXPECT_EQ(context.snapshot_stats.at("69"), 37);
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"load", "character", "character", "stats:2", "save",
                                      "progress", "character", "stats:0", "stats:2", "combat-stats",
                                      "snapshot", "growth"}));
}

TEST(NuiaSession, BlessingWithoutCombatKeepsThePreparedModelAliveForSnapshotAndGrowth) {
  auto feature = statue_feature();
  NuiaContext context;
  context.has_stats = true;
  context.combat = false;
  auto session = feature.create_session(context, context);
  auto request = offering();
  ASSERT_TRUE(session->handle_statue(request));
  EXPECT_EQ(context.snapshot_stats.at("69"), 100);
  EXPECT_TRUE(context.adopted_model.is_null());
  EXPECT_EQ(context.calls.back(), "growth");
}

TEST(NuiaSession, UnownedDiscoveriesDoNotConsumeStorageOrTheTearRequestBudget) {
  NuiaFeature feature(nullptr, NuiaTears(nuia_test::tear_fixture()));
  NuiaContext context;
  const auto before = context.stored;
  auto session = feature.create_session(context, context);
  for (std::uint32_t index = 0; index < 40; ++index) {
    auto request = nuia_test::tear_request();
    request["spot_object_id"] = std::to_string(900000 + index);
    auto discovery = request_frame(20050, request, kTearPosition, index + 1);
    EXPECT_FALSE(session->handle_tears(discovery));
    EXPECT_TRUE(discovery.session_packets.empty());
  }

  auto other_map = kTearPosition;
  other_map.map_id = 8000;
  auto wrong_map = request_frame(20050, nuia_test::tear_request(), other_map);
  EXPECT_FALSE(session->handle_tears(wrong_map));
  EXPECT_TRUE(wrong_map.session_packets.empty());
  EXPECT_EQ(context.loads, 0);
  EXPECT_EQ(context.save_attempts, 0);
  EXPECT_EQ(context.stored, before);

  auto owned = request_frame(20050, nuia_test::tear_request(), kTearPosition, 42);
  ASSERT_TRUE(session->handle_tears(owned));
  EXPECT_EQ(opcodes(owned.session_packets), std::vector<std::uint16_t>({90}));
  EXPECT_EQ(context.loads, 1);
  EXPECT_EQ(context.save_attempts, 1);
  EXPECT_EQ(context.stored.adventure_objects.at(5010058), 1);
  EXPECT_EQ(context.stored.stackables, before.stackables);
}

TEST(NuiaSession, MalformedDiscoveryStillFailsWithoutChangingProgress) {
  NuiaFeature feature(nullptr, NuiaTears(nuia_test::tear_fixture()));
  NuiaContext context;
  const auto before = context.stored;
  auto session = feature.create_session(context, context);
  for (const auto &request :
       {Json::object(), Json{{"spot_object_id", "invalid"}}, Json{{"spot_object_id", 123006}}}) {
    auto discovery = request_frame(20050, request, kTearPosition, 17);
    ASSERT_TRUE(session->handle_tears(discovery));
    ASSERT_EQ(discovery.session_packets.size(), 1);
    EXPECT_EQ(discovery.session_packets[0].opcode, 2);
    EXPECT_EQ(discovery.session_packets[0].message.at("ec"), "BadRequest");
    EXPECT_EQ(discovery.session_packets[0].message.at("packet_seq").at("seq"), "17");
  }
  EXPECT_EQ(context.save_attempts, 0);
  EXPECT_EQ(context.stored, before);
}

TEST(NuiaSession, TearBudgetBoundsStorageWorkAndResetsAfterOneSecond) {
  NuiaFeature feature(nullptr, NuiaTears(nuia_test::tear_fixture()));
  NuiaContext context;
  auto session = feature.create_session(context, context);
  for (std::uint32_t index = 0; index < 33; ++index) {
    auto invalid_spot =
        request_frame(20049, {{"spot_object_id", "999"}}, kTearPosition, index + 10);
    ASSERT_TRUE(session->handle_tears(invalid_spot));
    ASSERT_EQ(invalid_spot.session_packets.size(), 1);
    if (index == 32) {
      EXPECT_EQ(invalid_spot.session_packets[0].message.at("ec"), "BadRequest");
      EXPECT_EQ(invalid_spot.session_packets[0].message.at("packet_seq").at("seq"), "42");
    } else {
      EXPECT_EQ(invalid_spot.session_packets[0].message.at("ec"), "Interaction_TargetNotFound");
    }
  }
  EXPECT_EQ(context.loads, 32);
  context.now += 10'000'000;
  auto next_window = request_frame(20049, {{"spot_object_id", "999"}}, kTearPosition);
  EXPECT_TRUE(session->handle_tears(next_window));
  EXPECT_EQ(context.loads, 33);
}

TEST(NuiaSession, TearConflictThrowsBeforeSuccessOrTutorialAdoption) {
  NuiaFeature feature(nullptr, NuiaTears(nuia_test::tear_fixture()));
  NuiaContext context;
  context.tutorial = true;
  context.save_allowed = false;
  const auto before = context.stored;
  auto session = feature.create_session(context, context);
  auto request = request_frame(20049, nuia_test::tear_request(), kTearPosition);
  EXPECT_THROW(session->handle_tears(request), std::runtime_error);
  EXPECT_EQ(context.stored, before);
  EXPECT_EQ(context.save_attempts, 1);
  EXPECT_EQ(context.calls.back(), "save");
  EXPECT_TRUE(request.session_packets.empty());
}

TEST(NuiaSession, TutorialTearDiscoveryFallsThroughButActivationCommitsBeforePackets) {
  NuiaFeature feature(nullptr, NuiaTears(nuia_test::tear_fixture(5)));
  NuiaContext context;
  context.tutorial = true;
  auto session = feature.create_session(context, context);
  auto discovery = request_frame(20050, nuia_test::tear_request(), kTearPosition);
  EXPECT_FALSE(session->handle_tears(discovery));
  EXPECT_EQ(context.loads, 0);
  auto activation = request_frame(20049, nuia_test::tear_request(), kTearPosition);
  ASSERT_TRUE(session->handle_tears(activation));
  EXPECT_EQ(opcodes(activation.session_packets), std::vector<std::uint16_t>({91, 169}));
  EXPECT_EQ(context.stored.stackables.at(2000), 15);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"load", "save", "progress"}));
}

TEST(NuiaSession, StatueOnlySnapshotRetriesAfterFailureAndSendsOnceAfterEarlierPackets) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  auto sync = request_frame(20001, {});
  context.fail_load = true;
  EXPECT_THROW(session->observe_tear_snapshot(sync), std::runtime_error);
  context.fail_load = false;
  sync.session_packets.push_back({123, {}});
  session->observe_tear_snapshot(sync);
  EXPECT_EQ(opcodes(sync.session_packets), std::vector<std::uint16_t>({123, 127}));
  session->observe_tear_snapshot(sync);
  EXPECT_EQ(sync.session_packets.size(), 2);
  EXPECT_EQ(context.loads, 2);
}

TEST(NuiaSession, SnapshotRemainsWithTutorialAndUnauthenticatedOwners) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  auto sync = request_frame(20001, {});
  context.tutorial = true;
  session->observe_tear_snapshot(sync);
  context.tutorial = false;
  context.authenticated = false;
  session->observe_tear_snapshot(sync);
  EXPECT_EQ(context.loads, 0);
  EXPECT_TRUE(sync.session_packets.empty());
}

TEST(NuiaSession, MissingStatueDataUsesTheExistingLegacyRevivalPath) {
  NuiaFeature feature;
  NuiaContext context;
  auto session = feature.create_session(context, context);
  const auto packets = session->revive_at_shrine({20039, 1, Json::object()});
  ASSERT_EQ(packets.size(), 1);
  EXPECT_TRUE(packets[0].message.at("legacy"));
  EXPECT_EQ(context.calls, std::vector<std::string>({"legacy"}));
}

TEST(NuiaSession, ShrineRefusalDoesNotInstallPendingStateAndAcceptanceDefersTheCommit) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  context.teleport_allowed = false;
  EXPECT_EQ(opcodes(session->revive_at_shrine({20039, 1, Json::object()})),
            std::vector<std::uint16_t>({24}));
  context.teleport_allowed = true;
  EXPECT_EQ(opcodes(session->revive_at_shrine({20039, 2, Json::object()})),
            std::vector<std::uint16_t>({181}));
  EXPECT_EQ(context.persist_attempts, 0);
  const auto calls = context.calls.size();
  EXPECT_TRUE(session->revive_at_shrine({20039, 3, Json::object()}).empty());
  EXPECT_EQ(context.calls.size(), calls);
}

TEST(NuiaSession,
     TeleportCompletionFiltersProtectionAndOrdersRevivalSuppliesBeforeLifePublication) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  ASSERT_EQ(opcodes(session->revive_at_shrine({20039, 1, Json::object()})),
            std::vector<std::uint16_t>({181}));
  context.calls.clear();
  std::vector<GameplayPacket> packets{{181, {}}, {182, {}}};
  ASSERT_TRUE(session->finish_escape_revival(packets));
  EXPECT_EQ(opcodes(packets), std::vector<std::uint16_t>({181, 47, 48, 182, 169}));
  EXPECT_EQ(context.calls, (std::vector<std::string>{"persist", "revive", "sync", "life"}));
  ASSERT_TRUE(session->finish_escape_revival(packets));
  EXPECT_EQ(context.persist_attempts, 1);
}

TEST(NuiaSession, FailedDurableRevivalConsumesPendingStateWithoutRevivalOrObserverSuccess) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  session->revive_at_shrine({20039, 1, Json::object()});
  context.calls.clear();
  context.fail_revival = true;
  std::vector<GameplayPacket> packets{{181, {}}};
  EXPECT_FALSE(session->finish_escape_revival(packets));
  EXPECT_EQ(opcodes(packets), std::vector<std::uint16_t>({181}));
  EXPECT_EQ(context.calls, std::vector<std::string>({"persist"}));
  EXPECT_TRUE(session->finish_escape_revival(packets));
  EXPECT_EQ(context.persist_attempts, 1);
}

TEST(NuiaSession, CompletionWithoutExistingTeleportPacketsInsertsRevivalAtTheBeginning) {
  auto feature = statue_feature();
  NuiaContext context;
  auto session = feature.create_session(context, context);
  session->revive_at_shrine({20039, 1, Json::object()});
  std::vector<GameplayPacket> packets;
  EXPECT_TRUE(session->finish_escape_revival(packets));
  EXPECT_EQ(opcodes(packets), std::vector<std::uint16_t>({47, 48, 169}));
}

} // namespace
} // namespace chronicles::nuia
