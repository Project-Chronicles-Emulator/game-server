#include "world/features/gathering/gathering.hpp"
#include "world/features/gathering/construction_budget.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string>

namespace chronicles {
namespace {

TEST(GatheringConstructionBudget, ConcurrentLeasesFailBeforeOvercommitAndRecover) {
  using gathering::ConstructionBudget;
  std::vector<std::shared_ptr<ConstructionBudget::Lease>> leases;
  for (int index = 0; index < 4; ++index) {
    auto lease = ConstructionBudget::acquire();
    lease->charge(ConstructionBudget::kMaximumBatchBytes);
    leases.push_back(std::move(lease));
  }
  auto rejected = ConstructionBudget::acquire();
  EXPECT_THROW(rejected->charge(1), std::runtime_error);
  leases.pop_back();
  EXPECT_NO_THROW(rejected->charge(ConstructionBudget::kMaximumBatchBytes));
  EXPECT_THROW(rejected->charge(1), std::runtime_error);
}

TEST(GatheringConstructionBudget, SimultaneousWorkersCannotExceedGlobalReservation) {
  using gathering::ConstructionBudget;
  std::latch start(1);
  std::latch finished(8);
  std::latch release(1);
  std::atomic<int> accepted{};
  std::vector<std::future<void>> workers;
  for (int index = 0; index < 8; ++index) {
    workers.push_back(std::async(std::launch::async, [&] {
      start.wait();
      auto lease = ConstructionBudget::acquire();
      try {
        lease->charge(ConstructionBudget::kMaximumBatchBytes);
        ++accepted;
      } catch (const std::runtime_error &) {
      }
      finished.count_down();
      release.wait();
    }));
  }
  start.count_down();
  finished.wait();
  const auto accepted_count = accepted.load();
  release.count_down();
  for (auto &worker : workers) {
    worker.get();
  }
  EXPECT_EQ(accepted_count, 4);
  auto after_release = ConstructionBudget::acquire();
  EXPECT_NO_THROW(after_release->charge(ConstructionBudget::kMaximumBatchBytes));
}

using Json = nlohmann::json;

constexpr std::int64_t kStartTicks = 100000000;
constexpr std::int64_t kTicksPerMs = 10000;
constexpr std::int64_t kWireNsPerTick = 100;
constexpr std::uint64_t kPlayerUnit = 4242;
constexpr int kSharedMap = 9012;
constexpr std::uint32_t kSector = 4784280;
constexpr std::uint32_t kHerbIndex = 23;
constexpr std::uint32_t kTreeIndex = 24;
constexpr std::uint32_t kUnsupportedIndex = 25;
constexpr std::uint32_t kOutsideMapIndex = 26;
constexpr int kHerbAnimation = 1006;
constexpr int kSickleAnimation = 1007;
constexpr int kHatchetAnimation = 1009;
constexpr int kHerbExecuteMs = 1646;
constexpr int kHerbDurationMs = 2606;
constexpr int kSickleExecuteMs = 4989;
constexpr int kHatchetExecuteMs = 6508;
constexpr int kHerbItem = 70102001;
constexpr int kLogItem = 70107001;
constexpr int kSeedItem = 50002;
constexpr int kHatchetItem = 11200101;
constexpr int kToolSlot2 = 3;
constexpr int kSpRegenDelayMs = 300;

// Doodads: a Roseberry Bush (74) that despawns, and a Wild Bush (58000) with timed phases.
constexpr std::uint32_t kDoodadSector = 4735150;
constexpr std::uint32_t kBushLocalId = 9;
constexpr std::uint32_t kWildBushLocalId = 10;
constexpr int kRoseberryInteraction = 20001;
constexpr int kWildBushInteraction = 20003;
constexpr int kRoseberryRespawnMs = 60000;
constexpr int kWildBushRegrowMs = 1000;

Json loot(int item_cms_id, int minimum, int maximum) {
  return {{"pack_rate_percent", 100.0},
          {"groups", Json::array({{{"rate_percent", 100.0},
                                   {"items", Json::array({{{"item_cms_id", item_cms_id},
                                                           {"min_quantity", minimum},
                                                           {"max_quantity", maximum},
                                                           {"weight", 1}}})}}})}};
}

Json animation(int id, const char *tool, int execute_ms, int duration_ms) {
  return {{"interaction_anim_id", id},
          {"sp_cost", 50},
          {"tool_equipment_type", tool},
          {"execute_time_ms", execute_ms},
          {"duration_ms", duration_ms}};
}

Json gathering_profile() {
  Json profile{
      {"format_version", 2},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"maps", Json::array({{{"map_id", kSharedMap},
                             {"area_min_mm", {-100000, -100000, -100000}},
                             {"area_max_mm", {100000, 100000, 100000}}}})},
      {"tool_durability_loss", 100},
      {"sp_regen_delay_ms", kSpRegenDelayMs},
      {"animations",
       Json::array({animation(kHerbAnimation, "None", kHerbExecuteMs, kHerbDurationMs),
                    animation(kSickleAnimation, "None", kSickleExecuteMs, 5633),
                    animation(kHatchetAnimation, "Hatchet", kHatchetExecuteMs, 7008)})},
      {"gatherings", Json::array()},
      {"unsupported_gatherings", Json::array({{{"gathering_id", 999}, {"reason", "x"}}})},
      {"tool_items", {{"Hatchet", {kHatchetItem}}}},
      {"points", Json::array()},
      {"doodads", Json::array()},
      {"doodad_points", Json::array()}};

  const auto add_definition = [&](int gathering_id, int animation_id, int item_cms_id) {
    auto definition = loot(item_cms_id, 5, 7);
    definition["gathering_id"] = gathering_id;
    definition["interaction_anim_id"] = animation_id;
    definition["deactivate_delay_ms"] = 1000;
    definition["activate_delay_min_ms"] = 30000;
    definition["activate_delay_max_ms"] = 180000;
    profile["gatherings"].push_back(std::move(definition));
  };
  add_definition(80, kHerbAnimation, kHerbItem);
  add_definition(200, kHatchetAnimation, kLogItem);

  profile["points"].push_back({kSector, kHerbIndex, 80, 0, 0, 0});
  profile["points"].push_back({kSector, kTreeIndex, 200, 1000, 0, 0});
  profile["points"].push_back({kSector, kUnsupportedIndex, 999, 0, 1000, 0});
  profile["points"].push_back({kSector, kOutsideMapIndex, 80, 500000, 0, 0});

  auto roseberry_reward = loot(kHerbItem, 5, 7);
  roseberry_reward["reason"] = "looting";
  profile["doodads"].push_back(
      {{"doodad_id", 74},
       {"initial_phase", 1},
       {"respawn_delay_min_ms", kRoseberryRespawnMs},
       {"respawn_delay_max_ms", kRoseberryRespawnMs},
       {"phases", Json::array({{{"phase_id", 1},
                                {"trigger", "interaction"},
                                {"interaction_id", kRoseberryInteraction},
                                {"interaction_anim_id", kHerbAnimation},
                                {"reward", roseberry_reward},
                                {"next", Json::array({Json::array({0, 1})})}}})}});

  auto seed_reward = loot(kSeedItem, 1, 3);
  seed_reward["reason"] = "looting";
  Json wild_bush_harvest{{"trigger", "interaction"},
                         {"interaction_id", kWildBushInteraction},
                         {"interaction_anim_id", kSickleAnimation},
                         {"reward", seed_reward},
                         {"next", Json::array({Json::array({2, 1})})}};
  auto first_harvest = wild_bush_harvest;
  first_harvest["phase_id"] = 1;
  auto regrown_harvest = wild_bush_harvest;
  regrown_harvest["phase_id"] = 4;
  profile["doodads"].push_back(
      {{"doodad_id", 58000},
       {"initial_phase", 1},
       {"phases", Json::array({first_harvest,
                               {{"phase_id", 2},
                                {"trigger", "timer"},
                                {"duration_ms", kWildBushRegrowMs},
                                {"next", Json::array({Json::array({4, 1})})}},
                               regrown_harvest})}});

  profile["doodad_points"].push_back({kDoodadSector, kBushLocalId, 74, 0, 500, 0});
  profile["doodad_points"].push_back({kDoodadSector, kWildBushLocalId, 58000, 500, 500, 0});
  return profile;
}

std::string target_for(std::uint32_t sector_id) {
  return std::to_string(static_cast<std::uint64_t>(sector_id) << 32);
}

std::string wire_tick(std::int64_t ticks) {
  return std::to_string(ticks * kWireNsPerTick);
}

Json start_request(std::uint32_t index, int animation_id = kHerbAnimation,
                   std::uint32_t sector_id = kSector) {
  return {{"interaction_anim_id", animation_id},
          {"target_unit_id", target_for(sector_id)},
          {"param", {{"gathering", {{"indices", Json::array({index})}}}}},
          {"pos", {{"millimeter_x", 10}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
          {"yaw", -92.5},
          {"tick", wire_tick(kStartTicks)}};
}

Json doodad_request(std::uint32_t local_id, int interaction_id = kRoseberryInteraction,
                    int animation_id = kHerbAnimation) {
  return {{"interaction_anim_id", animation_id},
          {"target_unit_id", std::to_string(doodad_unit_id(kDoodadSector, local_id))},
          {"param", {{"interaction", {{"interaction_id", interaction_id}}}}},
          {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
          {"yaw", 25.25},
          {"tick", wire_tick(kStartTicks)}};
}

Json execute_request(int animation_id = kHerbAnimation) {
  return {{"interaction_anim_id", animation_id}, {"tick", "1"}};
}

Json end_request(int animation_id = kHerbAnimation) {
  return {{"end_interaction_anim", {{"interaction_anim_id", animation_id}}}, {"tick", "1"}};
}

void expect_wire(const std::vector<GameplayPacket> &packets) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : packets) {
    const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
    EXPECT_EQ(decoded.at("status"), "decoded") << "opcode " << packet.opcode;
  }
}

const GameplayPacket *find_packet(const GatheringOutcome &outcome, std::uint16_t opcode) {
  for (const auto &packet : outcome.packets) {
    if (packet.opcode == opcode) {
      return &packet;
    }
  }
  return nullptr;
}

std::string error_code(const GatheringOutcome &outcome) {
  if (outcome.packets.empty() || !outcome.packets.back().message.contains("ec")) {
    return "";
  }
  return outcome.packets.back().message.at("ec").get<std::string>();
}

std::vector<std::uint16_t> opcodes(const GatheringOutcome &outcome) {
  std::vector<std::uint16_t> result;
  for (const auto &packet : outcome.packets) {
    result.push_back(packet.opcode);
  }
  return result;
}

GatheringNodeKey placement_node(std::uint32_t index, int map_id = kSharedMap) {
  return {map_id, GatheringNodeKind::Placement, kSector, index};
}

GatheringNodeKey doodad_node(std::uint32_t local_id) {
  return {kSharedMap, GatheringNodeKind::Doodad, kDoodadSector, local_id};
}

TEST(GatheringReservations, PlacementWaitsForOwnerAndArmsRespawnOnlyAfterCommit) {
  GatheringNodes nodes;
  const auto node = placement_node(kHerbIndex);
  EXPECT_THROW(nodes.reserve_placement(node, kStartTicks, 0), std::invalid_argument);
  const auto first = nodes.reserve_placement(node, kStartTicks, 100 * kTicksPerMs);
  ASSERT_TRUE(first);
  const auto delayed = kStartTicks + 1000 * kTicksPerMs;
  EXPECT_FALSE(nodes.is_active(node, delayed));
  EXPECT_FALSE(nodes.reserve_placement(node, delayed, 100 * kTicksPerMs));
  EXPECT_TRUE(nodes.collect_respawns(delayed).empty());
  EXPECT_FALSE(nodes.finish_reservation(node, *first + 1, true, delayed));
  ASSERT_TRUE(nodes.finish_reservation(node, *first, true, delayed));
  EXPECT_FALSE(nodes.is_active(node, delayed + 99 * kTicksPerMs));
  EXPECT_TRUE(nodes.is_active(node, delayed + 100 * kTicksPerMs));
  const auto successor = nodes.reserve_placement(node, delayed + 100 * kTicksPerMs,
                                                  100 * kTicksPerMs);
  ASSERT_TRUE(successor);
  EXPECT_FALSE(nodes.finish_reservation(node, *first, false, delayed + 101 * kTicksPerMs));
  EXPECT_FALSE(nodes.is_active(node, delayed + 101 * kTicksPerMs));
  ASSERT_TRUE(nodes.finish_reservation(node, *successor, false, delayed + 101 * kTicksPerMs));
  EXPECT_TRUE(nodes.is_active(node, delayed + 101 * kTicksPerMs));
}

TEST(GatheringReservations, DoodadKeepsPriorPhaseAndRejectsCompetingOwners) {
  auto data = std::make_shared<const GatheringData>(gathering_profile());
  GatheringNodes nodes(data);
  const auto node = doodad_node(kWildBushLocalId);
  const auto *definition = data->doodad(58000);
  ASSERT_NE(definition, nullptr);
  const auto first = nodes.reserve_doodad(node, *definition, 1, 2, 100 * kTicksPerMs);
  ASSERT_TRUE(first);
  EXPECT_EQ(nodes.doodad_phase(node, *definition), 1);
  EXPECT_TRUE(nodes.doodads_on_map(kSharedMap).empty());
  EXPECT_FALSE(nodes.reserve_doodad(node, *definition, 1, 2, 100 * kTicksPerMs));
  EXPECT_THROW(nodes.set_scripted_doodad_phase(node, 58000, 4, kStartTicks),
               std::runtime_error);
  ASSERT_TRUE(nodes.finish_reservation(node, *first, false, kStartTicks));
  EXPECT_EQ(nodes.doodad_phase(node, *definition), 1);
  const auto successor = nodes.reserve_doodad(node, *definition, 1, 2, 100 * kTicksPerMs);
  ASSERT_TRUE(successor);
  EXPECT_FALSE(nodes.finish_reservation(node, *first, true, kStartTicks));
  ASSERT_TRUE(nodes.finish_reservation(node, *successor, true, kStartTicks));
  EXPECT_EQ(nodes.doodad_phase(node, *definition), 2);
  EXPECT_TRUE(nodes.collect_respawns(kStartTicks + 99 * kTicksPerMs).empty());
  EXPECT_FALSE(nodes.collect_respawns(kStartTicks + 100 * kTicksPerMs).empty());
}

// Documents the current restart limitation until shared-node restoration is connected.
TEST(GatheringSharedSources, DuePagesRotatePastFailuresAndRevisitNewEarlierSources) {
  GatheringNodes nodes;
  StoredSharedGatheringSource due;
  due.generation = 1;
  due.active = false;
  due.next_transition_epoch_ticks = 1100;
  for (std::uint32_t index = 1; index <= 130; ++index) {
    const GatheringNodeKey node{kSharedMap, GatheringNodeKind::Placement, kSector, index};
    ASSERT_TRUE(nodes.install_shared_source(node, due, 1000, kStartTicks));
  }

  EXPECT_TRUE(nodes.due_shared_sources(kStartTicks + 99, 128).empty());
  const auto first = nodes.due_shared_sources(kStartTicks + 100, 128);
  ASSERT_EQ(first.size(), 128U);
  const GatheringNodeKey cursor{kSharedMap, GatheringNodeKind::Placement, kSector,
                                first.back().key.local_index};
  const GatheringNodeKey newly_due{kSharedMap, GatheringNodeKind::Placement, kSector, 0};
  ASSERT_TRUE(nodes.install_shared_source(newly_due, due, 1000, kStartTicks));

  const auto later = nodes.due_shared_sources(kStartTicks + 100, 128, cursor);
  ASSERT_EQ(later.size(), 2U);
  EXPECT_EQ(later.front().key.local_index, 129U);
  const GatheringNodeKey last{kSharedMap, GatheringNodeKind::Placement, kSector,
                              later.back().key.local_index};
  EXPECT_TRUE(nodes.due_shared_sources(kStartTicks + 100, 128, last).empty());
  const auto wrapped = nodes.due_shared_sources(kStartTicks + 100, 128);
  ASSERT_EQ(wrapped.size(), 128U);
  EXPECT_EQ(wrapped.front().key.local_index, 0U);
}

TEST(GatheringSharedSources, DISABLED_ProfileScaleDeadlineScanDiagnostic) {
  constexpr std::uint32_t kProfileSources = 120000;
  constexpr int kRepetitions = 20;
  GatheringNodes nodes;
  StoredSharedGatheringSource active;
  active.generation = 1;
  active.active = true;
  StoredSharedGatheringSource due = active;
  due.generation = 2;
  due.active = false;
  due.next_transition_epoch_ticks = 1100;

  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t index = 0; index < kProfileSources; ++index) {
    const GatheringNodeKey node{kSharedMap, GatheringNodeKind::Placement, kSector, index};
    ASSERT_TRUE(nodes.install_shared_source(node, index % 1000 == 0 ? due : active,
                                            1000, kStartTicks));
  }
  const auto populated = std::chrono::steady_clock::now();
  for (int repetition = 0; repetition < kRepetitions; ++repetition) {
    ASSERT_TRUE(nodes.next_shared_respawn_tick());
    EXPECT_EQ(nodes.due_shared_sources(kStartTicks + 100, 128).size(), 120U);
  }
  const auto sparse_done = std::chrono::steady_clock::now();
  for (std::uint32_t index = 0; index < kProfileSources; ++index) {
    const GatheringNodeKey node{kSharedMap, GatheringNodeKind::Placement, kSector, index};
    ASSERT_TRUE(nodes.install_shared_source(node, due, 1000, kStartTicks));
  }
  const auto backlog_start = std::chrono::steady_clock::now();
  for (int repetition = 0; repetition < kRepetitions; ++repetition) {
    ASSERT_TRUE(nodes.next_shared_respawn_tick());
    EXPECT_EQ(nodes.due_shared_sources(kStartTicks + 100, 128).size(), 128U);
  }
  const auto backlog_done = std::chrono::steady_clock::now();
  const auto milliseconds = [](auto duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
  };
  std::cout << "profile sources=" << kProfileSources
            << " populate_ms=" << milliseconds(populated - start)
            << " sparse_20_scans_ms=" << milliseconds(sparse_done - populated)
            << " backlog_20_scans_ms=" << milliseconds(backlog_done - backlog_start) << '\n';
}

TEST(GatheringSharedSources, DeadlineIndexesFollowReservationRestoreAndSnapshots) {
  GatheringNodes nodes;
  const auto node = placement_node(kHerbIndex);
  StoredSharedGatheringSource inactive;
  inactive.generation = 1;
  inactive.active = false;
  inactive.next_transition_epoch_ticks = 1100;
  ASSERT_TRUE(nodes.install_shared_source(node, inactive, 1000, kStartTicks));
  ASSERT_TRUE(nodes.next_shared_respawn_tick());
  EXPECT_EQ(*nodes.next_shared_respawn_tick(), kStartTicks + 100);

  StoredSharedGatheringSource active;
  active.generation = 2;
  active.active = true;
  ASSERT_TRUE(nodes.install_shared_source(node, active, 1000, kStartTicks));
  EXPECT_FALSE(nodes.next_shared_respawn_tick());
  const auto token = nodes.reserve_placement(node, kStartTicks, 200);
  ASSERT_TRUE(token);
  EXPECT_FALSE(nodes.next_shared_respawn_tick());
  ASSERT_TRUE(nodes.finish_reservation(node, *token, false, kStartTicks));
  EXPECT_FALSE(nodes.next_shared_respawn_tick());

  const auto local = placement_node(kTreeIndex);
  ASSERT_TRUE(nodes.try_deplete(local, kStartTicks, kStartTicks + 50));
  ASSERT_TRUE(nodes.next_respawn_tick(true));
  EXPECT_EQ(*nodes.next_respawn_tick(true), kStartTicks + 50);
  const auto local_owner = nodes.reserve_placement(local, kStartTicks + 51, 100);
  ASSERT_TRUE(local_owner);
  EXPECT_FALSE(nodes.next_respawn_tick(true));
  ASSERT_TRUE(nodes.finish_reservation(local, *local_owner, false, kStartTicks + 51));
  ASSERT_TRUE(nodes.next_respawn_tick(true));
  EXPECT_EQ(*nodes.next_respawn_tick(true), kStartTicks + 50);
  nodes.restore(local);
  EXPECT_FALSE(nodes.next_respawn_tick(true));
  ASSERT_TRUE(nodes.try_deplete(local, kStartTicks, kStartTicks + 50));
  ASSERT_EQ(nodes.collect_respawns(kStartTicks + 50).size(), 1U);
  EXPECT_FALSE(nodes.next_respawn_tick(true));
}

TEST(GatheringPublication, StaleNodeRevisionsAreDroppedWithoutDroppingOtherNodes) {
  auto nodes = std::make_shared<GatheringNodes>();
  const auto first = placement_node(kHerbIndex);
  const auto second = placement_node(kTreeIndex);
  std::uint64_t first_revision{};
  std::uint64_t second_revision{};
  ASSERT_TRUE(nodes->try_deplete(first, kStartTicks, kStartTicks + 100, &first_revision));
  ASSERT_TRUE(nodes->try_deplete(second, kStartTicks, kStartTicks + 100, &second_revision));
  const std::vector<GatheringStateChange> changes{
      {first, false, std::nullopt, kStartTicks, first_revision},
      {second, false, std::nullopt, kStartTicks, second_revision}};
  auto packets = gathering_state_packets(changes, nodes, 7);
  ASSERT_EQ(packets.size(), 1U);
  ASSERT_TRUE(packets.front().local_plan);
  EXPECT_EQ(packets.front().local_plan->expected_map(), kSharedMap);
  EXPECT_EQ(packets.front().local_plan->expected_channel(), 7U);
  EXPECT_GE(packets.front().local_plan->retained_bytes(),
            changes.size() * sizeof(GatheringStateChange));
  auto unchanged = packets.front().message;
  EXPECT_EQ(packets.front().local_plan->prepare(unchanged), LocalPacketDecision::keep);

  nodes->restore(first);
  std::uint64_t newer_revision{};
  ASSERT_TRUE(nodes->try_deplete(first, kStartTicks, kStartTicks + 100, &newer_revision));
  EXPECT_NE(first_revision, newer_revision);
  auto filtered = packets.front().message;
  EXPECT_EQ(packets.front().local_plan->prepare(filtered), LocalPacketDecision::replace);
  EXPECT_EQ(filtered.at("deactivated_indices"), Json::array({kTreeIndex}));
  auto merged = packets.front().message;
  merged["deactivated_indices"].push_back(999);
  merged["unrelated"] = "preserved";
  EXPECT_EQ(packets.front().local_plan->prepare(merged), LocalPacketDecision::replace);
  EXPECT_EQ(merged.at("deactivated_indices"), Json::array({kTreeIndex, 999}));
  EXPECT_EQ(merged.at("unrelated"), "preserved");
  nodes->restore(second);
  EXPECT_EQ(packets.front().local_plan->prepare(filtered), LocalPacketDecision::drop);
}

TEST(GatheringPublication, OrderedDeliveryBlocksMutationUntilTheFrameIsQueued) {
  auto nodes = std::make_shared<GatheringNodes>();
  const auto node = placement_node(kHerbIndex);
  std::uint64_t revision{};
  ASSERT_TRUE(nodes->try_deplete(node, kStartTicks, kStartTicks + 100, &revision));
  const auto packets = gathering_state_packets(
      {{node, false, std::nullopt, kStartTicks, revision}}, nodes, 0);
  ASSERT_EQ(packets.size(), 1U);
  ASSERT_TRUE(packets.front().local_plan);
  auto guard = packets.front().local_plan->begin_ordered_delivery();
  ASSERT_TRUE(guard);
  std::promise<void> started;
  auto started_future = started.get_future();
  auto mutation = std::async(std::launch::async, [&] {
    started.set_value();
    nodes->restore(node);
  });
  started_future.wait();
  EXPECT_EQ(mutation.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
  auto message = packets.front().message;
  EXPECT_EQ(packets.front().local_plan->prepare(message), LocalPacketDecision::keep);
  guard.reset();
  EXPECT_EQ(mutation.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  mutation.get();
  EXPECT_EQ(packets.front().local_plan->prepare(message), LocalPacketDecision::drop);
}

TEST(GatheringPublication, SectorReconstructionPreservesUnrelatedFieldsAndUsesCurrentState) {
  auto nodes = std::make_shared<GatheringNodes>();
  const auto first = placement_node(kHerbIndex);
  ASSERT_TRUE(nodes->try_deplete(first, kStartTicks, kStartTicks + 100));
  const auto plan = gathering_sector_plan(nodes, kSharedMap, 3, kSector, {});
  Json message{{"sector_id", kSector}, {"unrelated", "preserved"},
               {"doodads", Json::array({{{"id", 999}, {"extra", 1}}})}};
  const auto second = placement_node(kTreeIndex);
  ASSERT_TRUE(nodes->try_deplete(second, kStartTicks, kStartTicks + 100));
  EXPECT_EQ(plan->prepare(message), LocalPacketDecision::replace);
  EXPECT_EQ(message.at("unrelated"), "preserved");
  EXPECT_EQ(message.at("gathering_deactivated_indices"),
            Json::array({kHerbIndex, kTreeIndex}));
  EXPECT_EQ(message.at("doodads").at(0).at("id"), 999);
  nodes.reset();
  EXPECT_EQ(plan->prepare(message), LocalPacketDecision::drop);
}

TEST(GatheringPublication, OversizedDeltaPlanFailsClosed) {
  auto nodes = std::make_shared<GatheringNodes>();
  std::vector<GatheringStateChange> changes;
  for (std::uint32_t index = 0; index <= 4096; ++index) {
    changes.push_back({{kSharedMap, GatheringNodeKind::Placement, kSector, index},
                       false, std::nullopt, kStartTicks, 1});
  }
  EXPECT_THROW(gathering_state_packets(changes, nodes, 0), std::runtime_error);
}

TEST(GatheringPublication, SectorReconstructionFailsClosedAtItsBound) {
  auto nodes = std::make_shared<GatheringNodes>();
  constexpr std::uint32_t kLimit = 4096;
  for (std::uint32_t index = 0; index < kLimit; ++index) {
    const GatheringNodeKey node{kSharedMap, GatheringNodeKind::Placement, kSector, index};
    ASSERT_TRUE(nodes->try_deplete(node, kStartTicks, kStartTicks + 100));
  }
  const auto within = nodes->sector_state(kSharedMap, kSector);
  EXPECT_EQ(within.first.size(), kLimit);
  const GatheringNodeKey overflow{kSharedMap, GatheringNodeKind::Placement, kSector, kLimit};
  ASSERT_TRUE(nodes->try_deplete(overflow, kStartTicks, kStartTicks + 100));
  EXPECT_THROW(nodes->sector_state(kSharedMap, kSector), std::runtime_error);

  auto empty_nodes = std::make_shared<GatheringNodes>();
  const auto plan = gathering_sector_plan(empty_nodes, kSharedMap, 0, kSector, {});
  Json oversized{{"sector_id", kSector}, {"doodads", Json::array()}};
  for (std::uint32_t index = 0; index <= kLimit; ++index) {
    oversized["doodads"].push_back({{"id", index}});
  }
  EXPECT_THROW(plan->prepare(oversized), std::runtime_error);
}

TEST(GatheringPublication, SectorReconstructionBoundsCombinedIdentityAndOutput) {
  constexpr std::uint32_t kLimit = 4096;
  auto nodes = std::make_shared<GatheringNodes>();
  const GatheringNodeKey current{kSharedMap, GatheringNodeKind::Doodad, kSector, kLimit};
  nodes->set_scripted_doodad_phase(current, 58000, 2, kStartTicks);

  std::vector<GatheringStateChange> original;
  for (std::uint32_t index = 0; index < kLimit; ++index) {
    original.push_back({{kSharedMap, GatheringNodeKind::Doodad, kSector, index},
                        true, 1, kStartTicks, 0});
  }
  const auto identity_plan = gathering_sector_plan(nodes, kSharedMap, 0, kSector, original);
  Json empty{{"sector_id", kSector}};
  EXPECT_THROW(identity_plan->prepare(empty), std::runtime_error);

  const auto output_plan = gathering_sector_plan(nodes, kSharedMap, 0, kSector, {});
  Json at_limit{{"sector_id", kSector}, {"doodads", Json::array()}};
  for (std::uint32_t index = 0; index < kLimit - 1; ++index) {
    at_limit["doodads"].push_back({{"id", index}});
  }
  EXPECT_EQ(output_plan->prepare(at_limit), LocalPacketDecision::replace);
  EXPECT_EQ(at_limit.at("doodads").size(), kLimit);

  Json overflow{{"sector_id", kSector}, {"doodads", Json::array()}};
  for (std::uint32_t index = 0; index < kLimit; ++index) {
    overflow["doodads"].push_back({{"id", index}});
  }
  EXPECT_THROW(output_plan->prepare(overflow), std::runtime_error);
}

TEST(GatheringSharedSources, RejectsUnknownOrWrongMapPlacements) {
  auto data = std::make_shared<const GatheringData>(gathering_profile());
  GatheringNodes nodes(data);
  StoredSharedGatheringSource source;
  source.generation = 1;
  source.active = true;
  EXPECT_THROW(nodes.install_shared_source(placement_node(99999), source, 1000, kStartTicks),
               std::runtime_error);
  const GatheringNodeKey outside{kSharedMap, GatheringNodeKind::Placement, kSector,
                                 kOutsideMapIndex};
  EXPECT_THROW(nodes.install_shared_source(outside, source, 1000, kStartTicks),
               std::runtime_error);
  EXPECT_TRUE(nodes.install_shared_source(placement_node(kHerbIndex), source, 1000, kStartTicks));
}

TEST(GatheringSharedSources, RestoresOnlyTheEvidencedCliffsCannonBarrierOutcome) {
  auto data = std::make_shared<const GatheringData>(gathering_profile());
  GatheringNodes nodes(data);
  const GatheringNodeKey barrier{30104, GatheringNodeKind::Doodad, 171 | (289 << 14), 2};
  const auto token = nodes.reserve_cliffs_cannon_barrier(barrier);
  ASSERT_TRUE(token);
  EXPECT_FALSE(nodes.reserve_cliffs_cannon_barrier(barrier));
  StoredSharedGatheringSource opened;
  opened.generation = 1;
  opened.active = false;
  opened.phase = kDespawnedDoodadPhase;
  EXPECT_FALSE(nodes.install_shared_source(barrier, opened, 1000, kStartTicks, *token + 1));
  ASSERT_TRUE(nodes.install_shared_source(barrier, opened, 1000, kStartTicks, *token));
  ASSERT_TRUE(nodes.shared_source(barrier));
  EXPECT_EQ(nodes.shared_source(barrier)->phase, kDespawnedDoodadPhase);

  auto unsupported = barrier;
  ++unsupported.index;
  EXPECT_THROW(nodes.reserve_cliffs_cannon_barrier(unsupported), std::invalid_argument);
  EXPECT_THROW(nodes.install_shared_source(unsupported, opened, 1000, kStartTicks),
               std::runtime_error);
  opened.active = true;
  opened.phase = 1;
  EXPECT_THROW(nodes.install_shared_source(barrier, opened, 1000, kStartTicks),
               std::runtime_error);
}

TEST(GatheringSharedSources, UnknownBarrierFenceDoesNotInventAVisiblePhase) {
  auto data = std::make_shared<const GatheringData>(gathering_profile());
  GatheringNodes nodes(data);
  const GatheringNodeKey barrier{30104, GatheringNodeKind::Doodad, 171 | (289 << 14), 2};
  StoredSharedGatheringSource unknown;
  unknown.generation = 1;
  unknown.active = false;
  ASSERT_TRUE(nodes.install_shared_source(barrier, unknown, 1000, kStartTicks));
  ASSERT_TRUE(nodes.shared_source(barrier));
  EXPECT_FALSE(nodes.shared_source(barrier)->phase);
  EXPECT_TRUE(nodes.sector_state(barrier.map_id, barrier.sector_id).second.empty());
  EXPECT_TRUE(nodes.doodads_on_map(barrier.map_id).empty());

  const auto cancelled = nodes.reserve_cliffs_cannon_barrier(barrier);
  ASSERT_TRUE(cancelled);
  ASSERT_TRUE(nodes.finish_reservation(barrier, *cancelled, false, kStartTicks));
  EXPECT_FALSE(nodes.shared_source(barrier)->phase);
  EXPECT_TRUE(nodes.sector_state(barrier.map_id, barrier.sector_id).second.empty());

  const auto accepted = nodes.reserve_cliffs_cannon_barrier(barrier);
  ASSERT_TRUE(accepted);
  StoredSharedGatheringSource opened;
  opened.generation = 2;
  opened.active = false;
  opened.phase = kDespawnedDoodadPhase;
  ASSERT_TRUE(nodes.install_shared_source(barrier, opened, 1000, kStartTicks, *accepted));
  ASSERT_EQ(nodes.sector_state(barrier.map_id, barrier.sector_id).second.size(), 1U);
  EXPECT_EQ(nodes.shared_source(barrier)->phase, kDespawnedDoodadPhase);
}

TEST(GatheringSharedSources, UnknownFenceRejectsOtherKeysAndVisibleLocalBarrier) {
  GatheringNodes nodes;
  const GatheringNodeKey barrier{30104, GatheringNodeKind::Doodad, 171 | (289 << 14), 2};
  auto unsupported = barrier;
  ++unsupported.index;
  StoredSharedGatheringSource unknown;
  unknown.generation = 1;
  unknown.active = false;
  EXPECT_THROW(nodes.install_shared_source(unsupported, unknown, 1000, kStartTicks),
               std::runtime_error);
  nodes.set_scripted_doodad_phase(barrier, 10151, 2, kStartTicks);
  EXPECT_FALSE(nodes.install_shared_source(barrier, unknown, 1000, kStartTicks));
  EXPECT_FALSE(nodes.shared_source(barrier));
  ASSERT_EQ(nodes.doodads_on_map(barrier.map_id).size(), 1U);
  EXPECT_EQ(nodes.doodads_on_map(barrier.map_id).front().phase, 2);
}

TEST(GatheringSharedSources, LinkedSnapshotsInstallTogetherOnlyForBothOwners) {
  auto data = std::make_shared<const GatheringData>(gathering_profile());
  GatheringNodes nodes(data);
  const auto primary = doodad_node(kWildBushLocalId);
  const auto *definition = data->doodad(58000);
  ASSERT_NE(definition, nullptr);
  const GatheringNodeKey barrier{30104, GatheringNodeKind::Doodad, 171 | (289 << 14), 2};
  const auto primary_token = nodes.reserve_doodad(primary, *definition, 1, 2, 100);
  const auto barrier_token = nodes.reserve_cliffs_cannon_barrier(barrier);
  ASSERT_TRUE(primary_token);
  ASSERT_TRUE(barrier_token);

  StoredSharedGatheringSource primary_source;
  primary_source.generation = 1;
  primary_source.active = true;
  primary_source.phase = 2;
  primary_source.next_transition_epoch_ticks = 1100;
  primary_source.timed_successor_phase = 1;
  StoredSharedGatheringSource barrier_source;
  barrier_source.generation = 1;
  barrier_source.active = false;
  barrier_source.phase = kDespawnedDoodadPhase;
  EXPECT_FALSE(nodes.install_shared_source_pair(primary, primary_source, *primary_token,
                                                barrier, barrier_source, *barrier_token + 1,
                                                1000, kStartTicks));
  EXPECT_FALSE(nodes.shared_source(primary));
  EXPECT_FALSE(nodes.shared_source(barrier));
  EXPECT_FALSE(nodes.reserve_cliffs_cannon_barrier(barrier));
  EXPECT_EQ(nodes.doodad_phase(primary, *definition), 1);

  ASSERT_TRUE(nodes.install_shared_source_pair(primary, primary_source, *primary_token,
                                               barrier, barrier_source, *barrier_token,
                                               1000, kStartTicks));
  EXPECT_EQ(nodes.doodad_phase(primary, *definition), 2);
  ASSERT_TRUE(nodes.shared_source(barrier));
  EXPECT_EQ(nodes.shared_source(barrier)->phase, kDespawnedDoodadPhase);
}

TEST(GatheringReservations, CurrentSharedDoodadCooldownIsLostWhenNodesAreReconstructed) {
  DoodadDefinition definition;
  definition.id = 30200226;
  definition.initial_phase = 1;
  const auto node = doodad_node(42);
  GatheringNodes before_restart;
  ASSERT_TRUE(before_restart.try_advance_doodad(
      node, definition, 1, 2, kStartTicks, kStartTicks + 300000 * kTicksPerMs));
  EXPECT_EQ(before_restart.doodad_phase(node, definition), 2);
  GatheringNodes after_restart;
  EXPECT_EQ(after_restart.doodad_phase(node, definition), 1);
}

// Records how the session drives the player's stamina, as CombatSession would.
struct StaminaProbe {
  int stamina{567};
  bool dead{};
  int begun{};
  int spent{};
  int cancelled{};
  std::optional<GatheringStaminaFlow> last_flow;
};

class GatheringTest : public testing::Test {
protected:
  std::shared_ptr<const GatheringData> data =
      std::make_shared<const GatheringData>(gathering_profile());
  std::shared_ptr<GatheringNodes> nodes = std::make_shared<GatheringNodes>(data, 3);
  WorldProgress durable;
  Character character;
  bool allow_commit = true;
  int commits{};
  ItemTrade last_trade;
  WorldPosition position{kSharedMap, 0, 0, 0, 0};
  StaminaProbe stamina;
  bool attach_vitals = false;
  bool effect_enabled = true;
  int applied_effect_id{};

  GatheringSession session(std::uint64_t unit_id = kPlayerUnit, std::uint32_t seed = 7) {
    GatheringPersistence persistence;
    persistence.load_character = [this]() -> std::optional<Character> { return character; };
    persistence.load_progress = [this] { return durable; };
    persistence.commit = [this](const WorldProgress &before, const WorldProgress &after,
                                const ItemTrade &trade) {
      // Storage accepts only a transition that advances the revision exactly once.
      if (!allow_commit || before.revision != durable.revision ||
          after.revision != before.revision + 1) {
        return false;
      }
      for (const auto &update : trade.durability_updates) {
        for (auto &item : character.items) {
          if (item.id == update.item_id) {
            EXPECT_EQ(item.durability, update.expected_durability);
            item.durability = update.durability;
          }
        }
      }
      durable = after;
      last_trade = trade;
      ++commits;
      return true;
    };
    return GatheringSession(data, nodes, unit_id, std::move(persistence), seed, vitals());
  }

  GatheringVitals vitals() {
    GatheringVitals result;
    if (!attach_vitals) {
      return result;
    }
    result.dead = [this] { return stamina.dead; };
    result.begin_stamina = [this](const GatheringStaminaFlow &flow)
        -> std::optional<std::vector<GameplayPacket>> {
      if (stamina.stamina < flow.cost) {
        return std::nullopt;
      }
      ++stamina.begun;
      stamina.last_flow = flow;
      return std::vector<GameplayPacket>{
          {54, {{"unit_id", std::to_string(kPlayerUnit)}, {"tick", "1"}}}};
    };
    result.spend_stamina = [this](std::int64_t) {
      ++stamina.spent;
      stamina.stamina -= stamina.last_flow->cost;
    };
    result.cancel_stamina = [this](std::int64_t) -> std::optional<GameplayPacket> {
      ++stamina.cancelled;
      return GameplayPacket{54, {{"unit_id", std::to_string(kPlayerUnit)}, {"tick", "2"}}};
    };
    result.tool_worn = [](const CharacterItem &tool) {
      return std::vector<GameplayPacket>{
          {47, {{"unit_id", std::to_string(kPlayerUnit)}, {"default_source", "SCS_Equip"}}},
          {168,
           {{"unit_id", std::to_string(kPlayerUnit)},
            {"equipments", Json::array({{{"item_cms_id", tool.cms_id}, {"slot", 3}}})}}}};
    };
    result.effect_available = [this](int) { return effect_enabled; };
    result.apply_effect = [this](int effect_id, const WorldPosition &, std::int64_t) {
      applied_effect_id = effect_id;
      return std::vector<GameplayPacket>{{80, {{"unit_id", std::to_string(kPlayerUnit)}}}};
    };
    return result;
  }

  GatheringOutcome gather(GatheringSession &gatherer, std::uint32_t index,
                          std::int64_t start_ticks = kStartTicks) {
    gatherer.receive(kGatheringStartInteractionRequest, start_request(index), 1, position,
                     start_ticks);
    return gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(), 2, position,
                            start_ticks + kHerbExecuteMs * kTicksPerMs);
  }

  GatheringOutcome harvest_doodad(GatheringSession &gatherer, const Json &request,
                                  int execute_ms, std::int64_t start_ticks = kStartTicks) {
    const auto started =
        gatherer.receive(kGatheringStartInteractionRequest, request, 1, position, start_ticks);
    EXPECT_EQ(started.packets.back().opcode, 166) << started.packets.back().message.dump();
    return gatherer.receive(kGatheringExecuteInteractionRequest,
                            execute_request(request.at("interaction_anim_id").get<int>()), 2,
                            position, start_ticks + execute_ms * kTicksPerMs);
  }

  void equip_hatchet(int durability = 2000) {
    CharacterItem hatchet;
    hatchet.id = 1;
    hatchet.cms_id = kHatchetItem;
    hatchet.equip_slot = kToolSlot2;
    hatchet.durability = durability;
    character.items.push_back(hatchet);
  }
};

TEST_F(GatheringTest, StartsExecutesAndGrantsCatalogLootAtServerDeadline) {
  auto gatherer = session();
  durable.stackables[kHerbItem] = 2;
  const auto revision = durable.revision;

  const auto started = gatherer.receive(kGatheringStartInteractionRequest,
                                        start_request(kHerbIndex), 1, position, kStartTicks);
  ASSERT_EQ(started.packets.size(), 1U);
  const auto &notification = started.packets.front();
  EXPECT_EQ(notification.opcode, 166);
  EXPECT_EQ(notification.message.at("unit_id"), std::to_string(kPlayerUnit));
  EXPECT_EQ(notification.message.at("param").at("gathering").at("indices"), Json::array({23}));
  // Official notifications echo the request tick (client nanoseconds), position and yaw.
  EXPECT_EQ(notification.message.at("tick"), wire_tick(kStartTicks));
  EXPECT_EQ(notification.message.at("pos").at("millimeter_x"), 10);
  EXPECT_EQ(notification.message.at("packet_seq"), Json::object());
  EXPECT_FALSE(notification.message.contains("target_unit_id"));
  ASSERT_EQ(started.observers.size(), 1U);
  EXPECT_EQ(started.observers.front().opcode, 166);
  EXPECT_TRUE(gatherer.busy());

  const auto too_early =
      gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(), 2, position,
                       kStartTicks + (kHerbExecuteMs - 200) * kTicksPerMs);
  EXPECT_EQ(error_code(too_early), "InteractionAnim_TimeNotMatch");
  EXPECT_EQ(commits, 0);

  const auto executed = gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(), 3,
                                         position, kStartTicks + kHerbExecuteMs * kTicksPerMs);
  EXPECT_EQ(opcodes(executed),
            (std::vector<std::uint16_t>{169, kGatheringStateChangedNotification, 167}));

  const auto &stack = executed.packets[0].message.at("stackables").at(0);
  EXPECT_EQ(stack.at("item_cms_id"), kHerbItem);
  const int total = stack.at("quantity");
  EXPECT_GE(total, 2 + 5);
  EXPECT_LE(total, 2 + 7);
  EXPECT_TRUE(executed.packets[0].message.contains("system"));
  EXPECT_FALSE(executed.packets[0].message.contains("items"));
  EXPECT_EQ(durable.stackables.at(kHerbItem), total);
  EXPECT_EQ(durable.revision, revision + 1);
  EXPECT_EQ(commits, 1);

  EXPECT_EQ(executed.packets[1].message.at("sector_id"), kSector);
  EXPECT_EQ(executed.packets[1].message.at("deactivated_indices"), Json::array({23}));
  EXPECT_EQ(executed.packets[2].message.at("unit_id"), std::to_string(kPlayerUnit));
  ASSERT_EQ(executed.broadcasts.size(), 1U);
  EXPECT_FALSE(executed.broadcasts.front().active);
  EXPECT_EQ(executed.broadcasts.front().node, placement_node(kHerbIndex));
  ASSERT_EQ(executed.observers.size(), 1U);
  EXPECT_EQ(executed.observers.front().opcode, 167);

  const auto ended = gatherer.receive(kGatheringEndInteractionRequest, end_request(), 4, position,
                                      kStartTicks + kHerbDurationMs * kTicksPerMs);
  ASSERT_EQ(ended.packets.size(), 1U);
  EXPECT_EQ(ended.packets.front().opcode, 167);
  EXPECT_EQ(ended.observers.size(), 1U);
  EXPECT_FALSE(gatherer.busy());

  std::vector<GameplayPacket> all = started.packets;
  all.insert(all.end(), too_early.packets.begin(), too_early.packets.end());
  all.insert(all.end(), executed.packets.begin(), executed.packets.end());
  all.insert(all.end(), ended.packets.begin(), ended.packets.end());
  expect_wire(all);
}

TEST_F(GatheringTest, ImplausibleOrMissingRequestTicksUseTheServerClock) {
  auto gatherer = session();
  auto request = start_request(kHerbIndex);
  // Ten seconds away from the server clock: not a synchronised client tick.
  request["tick"] = wire_tick(kStartTicks + 10000 * kTicksPerMs);
  const auto skewed =
      gatherer.receive(kGatheringStartInteractionRequest, request, 1, position, kStartTicks);
  EXPECT_EQ(skewed.packets.back().message.at("tick"), wire_tick(kStartTicks));

  gatherer.receive(kGatheringEndInteractionRequest, end_request(), 2, position, kStartTicks);
  request.erase("tick");
  const auto missing =
      gatherer.receive(kGatheringStartInteractionRequest, request, 3, position, kStartTicks);
  EXPECT_EQ(missing.packets.back().message.at("tick"), wire_tick(kStartTicks));
}

TEST_F(GatheringTest, StaminaFlowPrecedesTheStartAndIsSpentOnceAtExecution) {
  attach_vitals = true;
  auto gatherer = session();

  const auto started = gatherer.receive(kGatheringStartInteractionRequest,
                                        start_request(kHerbIndex), 1, position, kStartTicks);
  EXPECT_EQ(opcodes(started), (std::vector<std::uint16_t>{54, 166}));
  ASSERT_TRUE(stamina.last_flow.has_value());
  EXPECT_EQ(stamina.last_flow->cost, 50);
  EXPECT_EQ(stamina.last_flow->execute_ms, kHerbExecuteMs);
  // Captured regen ranges end SPStatRegenDelayMilli after the animation.
  EXPECT_EQ(stamina.last_flow->regen_resume_ms, kHerbDurationMs + kSpRegenDelayMs);
  EXPECT_EQ(std::to_string(stamina.last_flow->wire_start_ns), wire_tick(kStartTicks));
  ASSERT_EQ(started.observers.size(), 1U);
  EXPECT_EQ(started.observers.front().opcode, 166);

  gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(), 2, position,
                   kStartTicks + kHerbExecuteMs * kTicksPerMs);
  gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(), 3, position,
                   kStartTicks + (kHerbExecuteMs + 10) * kTicksPerMs);
  EXPECT_EQ(stamina.spent, 1);
  EXPECT_EQ(stamina.stamina, 517);
  EXPECT_EQ(stamina.cancelled, 0);
}

TEST_F(GatheringTest, CancellingBeforeExecutionStopsTheStaminaFlow) {
  attach_vitals = true;
  auto gatherer = session();
  gatherer.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex), 1, position,
                   kStartTicks);
  const auto ended = gatherer.receive(kGatheringEndInteractionRequest, end_request(), 2, position,
                                      kStartTicks + 500 * kTicksPerMs);
  EXPECT_EQ(opcodes(ended), (std::vector<std::uint16_t>{54, 167}));
  EXPECT_EQ(stamina.cancelled, 1);
  EXPECT_EQ(stamina.spent, 0);
  EXPECT_EQ(commits, 0);
}

TEST_F(GatheringTest, DamageInterruptsGatheringWithoutGrantingOrDepletingTheNode) {
  attach_vitals = true;
  auto gatherer = session();
  gatherer.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex), 1, position,
                   kStartTicks);
  const auto interrupted = gatherer.interrupt(kStartTicks + 500 * kTicksPerMs);
  EXPECT_EQ(opcodes(interrupted), (std::vector<std::uint16_t>{54, 167}));
  EXPECT_EQ(interrupted.observers.size(), 1U);
  EXPECT_FALSE(gatherer.busy());
  EXPECT_EQ(stamina.cancelled, 1);
  EXPECT_EQ(stamina.spent, 0);
  EXPECT_EQ(commits, 0);
  EXPECT_TRUE(nodes->is_active(placement_node(kHerbIndex),
                               kStartTicks + kHerbExecuteMs * kTicksPerMs));
  EXPECT_TRUE(gatherer.interrupt(kStartTicks + 600 * kTicksPerMs).packets.empty());
}

TEST_F(GatheringTest, AirBubbleExecutesItsStaminaRecoveryAndReturnsAfterItsTimer) {
  auto profile = gathering_profile();
  auto bubble_animation = animation(11150, "None", 2951, 3333);
  bubble_animation["sp_cost"] = 0;
  profile["animations"].push_back(bubble_animation);
  profile["doodads"].push_back({
      {"doodad_id", 11152}, {"initial_phase", 1}, {"quest", true},
      {"phases", Json::array({
                     {{"phase_id", 1}, {"trigger", "interaction"},
                      {"interaction_id", 11150}, {"interaction_anim_id", 11150},
                      {"skill_effect_id", 11152}, {"next", Json::array({{2, 1}})}},
                     {{"phase_id", 2}, {"trigger", "timer"}, {"duration_ms", 4000},
                      {"next", Json::array({{1, 1}})}},
                 })}});
  profile["doodad_points"].push_back({kDoodadSector, 11, 11152, 0, 0, 0});
  data = std::make_shared<const GatheringData>(profile);
  nodes = std::make_shared<GatheringNodes>(data, 3);
  attach_vitals = true;
  auto gatherer = session();
  const auto request = doodad_request(11, 11150, 11150);
  ASSERT_TRUE(gatherer.handles(kGatheringStartInteractionRequest, request));
  EXPECT_EQ(gatherer.receive(kGatheringStartInteractionRequest, request, 1, position, kStartTicks)
                .packets.back().opcode, 166);
  const auto executed = gatherer.receive(kGatheringExecuteInteractionRequest,
                                         execute_request(11150), 2, position,
                                         kStartTicks + 2951 * kTicksPerMs);
  EXPECT_EQ(applied_effect_id, 11152);
  EXPECT_NE(find_packet(executed, 80), nullptr);
  EXPECT_EQ(nodes->doodad_phase(doodad_node(11), *data->doodad(11152)), 2);
  nodes->collect_respawns(kStartTicks + (2951 + 4000) * kTicksPerMs);
  EXPECT_EQ(nodes->doodad_phase(doodad_node(11), *data->doodad(11152)), 1);
  gatherer.receive(kGatheringEndInteractionRequest, end_request(11150), 3, position,
                   kStartTicks + 7000 * kTicksPerMs);
  const auto second_start = gatherer.receive(kGatheringStartInteractionRequest, request, 4, position,
                                             kStartTicks + 8000 * kTicksPerMs);
  EXPECT_EQ(second_start.packets.back().opcode, 166);
  effect_enabled = false;
  const int previous_commits = commits;
  const auto refused = gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(11150),
                                         5, position, kStartTicks + 11000 * kTicksPerMs);
  EXPECT_EQ(error_code(refused), "InteractionAnim_InvalidCondition");
  EXPECT_EQ(commits, previous_commits);
  EXPECT_EQ(nodes->doodad_phase(doodad_node(11), *data->doodad(11152)), 1);
}

TEST_F(GatheringTest, InsufficientStaminaOrDeathRefusesTheStart) {
  attach_vitals = true;
  auto gatherer = session();
  stamina.stamina = 49;
  const auto tired = gatherer.receive(kGatheringStartInteractionRequest,
                                      start_request(kHerbIndex), 1, position, kStartTicks);
  EXPECT_EQ(error_code(tired), "InteractionAnim_InvalidCondition");
  EXPECT_EQ(tired.packets.back().opcode, 165);
  EXPECT_FALSE(gatherer.busy());

  stamina.stamina = 500;
  stamina.dead = true;
  const auto dead = gatherer.receive(kGatheringStartInteractionRequest,
                                     start_request(kHerbIndex), 2, position, kStartTicks);
  EXPECT_EQ(error_code(dead), "InteractionAnim_Dead");
  EXPECT_EQ(stamina.begun, 0);
}

TEST_F(GatheringTest, DeathBeforeExecutionCancelsWithoutGrantingOrDepleting) {
  attach_vitals = true;
  auto gatherer = session();
  gatherer.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex), 1, position,
                   kStartTicks);

  stamina.dead = true;
  const auto execution_ticks = kStartTicks + kHerbExecuteMs * kTicksPerMs;
  const auto outcome = gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(),
                                        2, position, execution_ticks);

  EXPECT_EQ(error_code(outcome), "InteractionAnim_Dead");
  EXPECT_FALSE(gatherer.busy());
  EXPECT_EQ(stamina.cancelled, 1);
  EXPECT_EQ(stamina.spent, 0);
  EXPECT_EQ(commits, 0);
  EXPECT_TRUE(nodes->is_active(placement_node(kHerbIndex), execution_ticks));
}

TEST_F(GatheringTest, AnotherServiceActionRefusesTheStart) {
  auto gatherer = session();
  const auto refused = gatherer.receive(kGatheringStartInteractionRequest,
                                        start_request(kHerbIndex), 1, position, kStartTicks, true);
  EXPECT_EQ(error_code(refused), "InteractionAnim_UsingSkill");
  EXPECT_FALSE(gatherer.busy());
}

TEST_F(GatheringTest, RetriedExecuteDoesNotGrantTwice) {
  auto gatherer = session();
  gather(gatherer, kHerbIndex);
  const auto granted = durable.stackables.at(kHerbItem);

  const auto retried =
      gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(), 3, position,
                       kStartTicks + (kHerbExecuteMs + 50) * kTicksPerMs);
  ASSERT_EQ(retried.packets.size(), 1U);
  EXPECT_EQ(retried.packets.front().opcode, 167);
  EXPECT_TRUE(retried.broadcasts.empty());
  EXPECT_EQ(durable.stackables.at(kHerbItem), granted);
  EXPECT_EQ(commits, 1);
}

TEST_F(GatheringTest, RejectsOutOfRangeAndMismatchedRequestPositions) {
  auto gatherer = session();
  position.x_mm = 8000;
  auto request = start_request(kHerbIndex);
  request["pos"]["millimeter_x"] = 8000;

  const auto far =
      gatherer.receive(kGatheringStartInteractionRequest, request, 1, position, kStartTicks);
  ASSERT_EQ(far.packets.size(), 1U);
  EXPECT_EQ(far.packets.front().opcode, 165);
  EXPECT_EQ(error_code(far), "TooFar");

  position.x_mm = 0;
  request["pos"]["millimeter_x"] = 9000;
  const auto mismatched =
      gatherer.receive(kGatheringStartInteractionRequest, request, 2, position, kStartTicks);
  EXPECT_EQ(error_code(mismatched), "TooFar");
  EXPECT_FALSE(gatherer.busy());
  expect_wire(far.packets);
}

TEST_F(GatheringTest, MovingAwayBeforeExecutionPreventsTheReward) {
  auto gatherer = session();
  gatherer.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex), 1, position,
                   kStartTicks);

  position.x_mm = 9000;
  const auto executed = gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(), 2,
                                         position, kStartTicks + kHerbExecuteMs * kTicksPerMs);
  EXPECT_EQ(error_code(executed), "TooFar");
  EXPECT_EQ(commits, 0);
  EXPECT_FALSE(gatherer.busy());
  EXPECT_TRUE(nodes->is_active(placement_node(kHerbIndex), kStartTicks));
}

TEST_F(GatheringTest, DepletedNodeRejectsOtherPlayers) {
  auto first = session(kPlayerUnit, 1);
  auto second = session(kPlayerUnit + 1, 2);
  const auto executed = gather(first, kHerbIndex);
  ASSERT_EQ(executed.broadcasts.size(), 1U);

  const auto later = kStartTicks + 5000 * kTicksPerMs;
  const auto rejected = second.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex),
                                       1, position, later);
  EXPECT_EQ(error_code(rejected), "InteractionAnim_TargetStateNotMatch");
  EXPECT_EQ(commits, 1);
}

TEST_F(GatheringTest, OnlyOneOfTwoConcurrentGatherersReceivesTheNode) {
  auto first = session(kPlayerUnit, 1);
  auto second = session(kPlayerUnit + 1, 2);
  first.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex), 1, position,
                kStartTicks);
  second.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex), 1, position,
                 kStartTicks);

  const auto execute_ticks = kStartTicks + kHerbExecuteMs * kTicksPerMs;
  const auto winner = first.receive(kGatheringExecuteInteractionRequest, execute_request(), 2,
                                    position, execute_ticks);
  const auto loser = second.receive(kGatheringExecuteInteractionRequest, execute_request(), 2,
                                    position, execute_ticks);
  EXPECT_NE(find_packet(winner, 169), nullptr);
  EXPECT_EQ(error_code(loser), "InteractionAnim_TargetStateNotMatch");
  EXPECT_TRUE(loser.broadcasts.empty());
  EXPECT_EQ(commits, 1);
  EXPECT_FALSE(second.busy());
}

TEST_F(GatheringTest, NodeRespawnsAfterDeactivateAndActivationDelay) {
  auto gatherer = session();
  const auto execute_ticks = kStartTicks + kHerbExecuteMs * kTicksPerMs;
  gather(gatherer, kHerbIndex);
  gatherer.receive(kGatheringEndInteractionRequest, end_request(), 3, position,
                   kStartTicks + kHerbDurationMs * kTicksPerMs);

  const auto node = placement_node(kHerbIndex);
  const auto earliest = execute_ticks + (1000 + 30000) * kTicksPerMs;
  const auto latest = execute_ticks + (1000 + 180000) * kTicksPerMs;
  ASSERT_TRUE(nodes->next_respawn_tick().has_value());
  EXPECT_GE(*nodes->next_respawn_tick(), earliest);
  EXPECT_LE(*nodes->next_respawn_tick(), latest);
  EXPECT_TRUE(nodes->collect_respawns(earliest - 1).empty());
  EXPECT_FALSE(nodes->is_active(node, earliest - 1));
  EXPECT_EQ(nodes->depleted_on_map(kSharedMap).size(), 1U);
  EXPECT_TRUE(nodes->depleted_on_map(9011).empty());
  EXPECT_TRUE(nodes->doodads_on_map(kSharedMap).empty());

  const auto respawned = nodes->collect_respawns(latest);
  ASSERT_EQ(respawned.size(), 1U);
  EXPECT_TRUE(respawned.front().active);
  EXPECT_FALSE(nodes->next_respawn_tick().has_value());

  const auto packets = gathering_state_packets(respawned);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets.front().message.at("activated_indices"), Json::array({23}));
  expect_wire(packets);

  const auto again = gather(gatherer, kHerbIndex, latest + kTicksPerMs);
  EXPECT_NE(find_packet(again, 169), nullptr);
  EXPECT_EQ(commits, 2);
}

TEST_F(GatheringTest, SeparateMapChannelsDepleteIndependently) {
  data = std::make_shared<const GatheringData>([] {
    auto profile = gathering_profile();
    profile["maps"].push_back({{"map_id", 9011},
                               {"area_min_mm", {-100000, -100000, -100000}},
                               {"area_max_mm", {100000, 100000, 100000}}});
    return profile;
  }());
  auto first = session(kPlayerUnit, 1);
  auto second = session(kPlayerUnit + 1, 2);
  gather(first, kHerbIndex);

  position.map_id = 9011;
  const auto other_channel = gather(second, kHerbIndex);
  EXPECT_NE(find_packet(other_channel, 169), nullptr);
  EXPECT_EQ(commits, 2);
}

TEST_F(GatheringTest, RejectsUnknownMalformedAndUnsupportedTargets) {
  auto gatherer = session();
  const auto expect_start_error = [&](const Json &request, const std::string &code) {
    const auto outcome =
        gatherer.receive(kGatheringStartInteractionRequest, request, 1, position, kStartTicks);
    EXPECT_EQ(error_code(outcome), code) << request.dump();
    EXPECT_FALSE(gatherer.busy());
  };

  expect_start_error(start_request(999), "InteractionAnim_NotFoundTarget");
  expect_start_error(start_request(kHerbIndex, kHerbAnimation, kSector + 1),
                     "InteractionAnim_NotFoundTarget");
  expect_start_error(start_request(kOutsideMapIndex), "InteractionAnim_NotFoundTarget");
  expect_start_error(start_request(kUnsupportedIndex), "Gathering_InvalidCmsId");
  expect_start_error(start_request(kHerbIndex, kHatchetAnimation), "InteractionAnim_Mismatch");

  auto low_bits = start_request(kHerbIndex);
  low_bits["target_unit_id"] = std::to_string((static_cast<std::uint64_t>(kSector) << 32) | 6);
  expect_start_error(low_bits, "InteractionAnim_NotFoundTarget");

  auto two_indices = start_request(kHerbIndex);
  two_indices["param"]["gathering"]["indices"] = Json::array({23, 24});
  expect_start_error(two_indices, "InteractionAnim_InvalidAnimParamType");

  auto malformed = start_request(kHerbIndex);
  malformed["target_unit_id"] = "not-a-number";
  expect_start_error(malformed, "BadRequest");

  position.map_id = 8000;
  expect_start_error(start_request(kHerbIndex), "InteractionAnim_NotFoundTarget");
}

TEST_F(GatheringTest, ToolGatherWearsTheToolInTheSameCommit) {
  attach_vitals = true;
  auto gatherer = session();
  const auto missing =
      gatherer.receive(kGatheringStartInteractionRequest,
                       start_request(kTreeIndex, kHatchetAnimation), 1, position, kStartTicks);
  EXPECT_EQ(error_code(missing), "InteractionAnim_InvalidCondition");

  equip_hatchet();
  character.items.front().equip_slot.reset();
  const auto unequipped =
      gatherer.receive(kGatheringStartInteractionRequest,
                       start_request(kTreeIndex, kHatchetAnimation), 2, position, kStartTicks);
  EXPECT_EQ(error_code(unequipped), "InteractionAnim_InvalidCondition");

  character.items.front().equip_slot = kToolSlot2;
  auto request = start_request(kTreeIndex, kHatchetAnimation);
  request["pos"]["millimeter_x"] = 1000;
  position.x_mm = 1000;
  const auto started =
      gatherer.receive(kGatheringStartInteractionRequest, request, 3, position, kStartTicks);
  EXPECT_EQ(started.packets.back().opcode, 166);

  const auto executed =
      gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(kHatchetAnimation), 4,
                       position, kStartTicks + kHatchetExecuteMs * kTicksPerMs);
  // Captured tool gathers: 169 (tool row + stack), 47 (SCS_Equip), 168, 196, 167.
  EXPECT_EQ(opcodes(executed),
            (std::vector<std::uint16_t>{169, 47, 168, kGatheringStateChangedNotification, 167}));
  const auto &items = executed.packets.front().message.at("items");
  ASSERT_EQ(items.size(), 1U);
  EXPECT_EQ(items.at(0).at("item_cms_id"), kHatchetItem);
  EXPECT_EQ(items.at(0).at("durability"), 1900);
  EXPECT_EQ(character.items.front().durability, 1900);
  ASSERT_EQ(last_trade.durability_updates.size(), 1U);
  EXPECT_EQ(last_trade.durability_updates.front().expected_durability, 2000);
  EXPECT_GE(durable.stackables.at(kLogItem), 5);
  expect_wire({executed.packets.front()});
}

TEST_F(GatheringTest, ToolBrokenDuringTheAnimationGrantsNothing) {
  auto gatherer = session();
  equip_hatchet();
  auto request = start_request(kTreeIndex, kHatchetAnimation);
  request["pos"]["millimeter_x"] = 1000;
  position.x_mm = 1000;
  gatherer.receive(kGatheringStartInteractionRequest, request, 1, position, kStartTicks);

  character.items.front().durability = 0;
  const auto executed =
      gatherer.receive(kGatheringExecuteInteractionRequest, execute_request(kHatchetAnimation), 2,
                       position, kStartTicks + kHatchetExecuteMs * kTicksPerMs);
  EXPECT_EQ(error_code(executed), "InteractionAnim_InvalidCondition");
  EXPECT_EQ(commits, 0);
  EXPECT_FALSE(gatherer.busy());
  EXPECT_TRUE(nodes->is_active(placement_node(kTreeIndex), kStartTicks));
}

TEST_F(GatheringTest, FailedCommitReleasesTheNodeAndGrantsNothing) {
  auto gatherer = session();
  allow_commit = false;
  const auto failed = gather(gatherer, kHerbIndex);
  EXPECT_EQ(error_code(failed), "InteractionAnim_GatheringFail");
  EXPECT_TRUE(failed.broadcasts.empty());
  EXPECT_FALSE(durable.stackables.contains(kHerbItem));
  EXPECT_TRUE(nodes->is_active(placement_node(kHerbIndex), kStartTicks));
  EXPECT_FALSE(nodes->next_respawn_tick().has_value());

  allow_commit = true;
  const auto retried = gather(gatherer, kHerbIndex, kStartTicks + 3000 * kTicksPerMs);
  EXPECT_NE(find_packet(retried, 169), nullptr);
  EXPECT_EQ(commits, 1);
}

TEST_F(GatheringTest, EndingBeforeExecutionCancelsWithoutConsumingTheNode) {
  auto gatherer = session();
  gatherer.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex), 1, position,
                   kStartTicks);
  const auto ended = gatherer.receive(kGatheringEndInteractionRequest, end_request(), 2, position,
                                      kStartTicks + 500 * kTicksPerMs);
  ASSERT_EQ(ended.packets.size(), 1U);
  EXPECT_EQ(ended.packets.front().opcode, 167);
  EXPECT_FALSE(gatherer.handles(kGatheringExecuteInteractionRequest, execute_request()));
  EXPECT_EQ(commits, 0);
  EXPECT_TRUE(nodes->is_active(placement_node(kHerbIndex), kStartTicks));
}

TEST_F(GatheringTest, SecondStartWhileGatheringIsRefusedUntilTimeout) {
  auto gatherer = session();
  gatherer.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex), 1, position,
                   kStartTicks);
  const auto refused =
      gatherer.receive(kGatheringStartInteractionRequest, start_request(kHerbIndex), 2, position,
                       kStartTicks + 100 * kTicksPerMs);
  EXPECT_EQ(error_code(refused), "InteractionAnim_UsingSkill");

  const auto later = kStartTicks + 31000 * kTicksPerMs;
  auto request = start_request(kHerbIndex);
  request["tick"] = wire_tick(later);
  const auto replaced =
      gatherer.receive(kGatheringStartInteractionRequest, request, 3, position, later);
  EXPECT_EQ(replaced.packets.back().opcode, 166);
}

TEST_F(GatheringTest, RoseberryDoodadDespawnsAndRespawnsInItsInitialPhase) {
  auto first = session(kPlayerUnit, 1);
  auto second = session(kPlayerUnit + 1, 2);
  const auto request = doodad_request(kBushLocalId);
  ASSERT_TRUE(first.handles(kGatheringStartInteractionRequest, request));

  const auto executed = harvest_doodad(first, request, kHerbExecuteMs);
  // Captured: 169 (looting reason naming the doodad), 194 (phase 0), 167.
  EXPECT_EQ(opcodes(executed),
            (std::vector<std::uint16_t>{169, kDoodadPhaseChangedNotification, 167}));
  const auto unit_id = std::to_string(doodad_unit_id(kDoodadSector, kBushLocalId));
  const auto &update = executed.packets[0].message;
  EXPECT_EQ(update.at("looting").at("target_unit_id"), unit_id);
  EXPECT_FALSE(update.contains("system"));
  EXPECT_GE(durable.stackables.at(kHerbItem), 5);
  const auto &phase = executed.packets[1].message;
  EXPECT_EQ(phase.at("unit_id"), unit_id);
  EXPECT_EQ(phase.at("phase_id"), 0);
  EXPECT_EQ(phase.at("phase_begin_tick"),
            wire_tick(kStartTicks + kHerbExecuteMs * kTicksPerMs));
  ASSERT_EQ(executed.broadcasts.size(), 1U);
  EXPECT_EQ(executed.broadcasts.front().node, doodad_node(kBushLocalId));

  const auto rejected = second.receive(kGatheringStartInteractionRequest, request, 1, position,
                                       kStartTicks + 5000 * kTicksPerMs);
  EXPECT_EQ(error_code(rejected), "InteractionAnim_DoodadNotProgressible");

  const auto despawned = nodes->doodads_on_map(kSharedMap);
  ASSERT_EQ(despawned.size(), 1U);
  const auto nubs = sector_doodad_nubs(despawned);
  ASSERT_EQ(nubs.size(), 1U);
  EXPECT_EQ(nubs.at(0).at("id"), kBushLocalId);
  EXPECT_FALSE(nubs.at(0).contains("phase"));

  const auto respawn = kStartTicks + (kHerbExecuteMs + kRoseberryRespawnMs) * kTicksPerMs;
  EXPECT_TRUE(nodes->collect_respawns(respawn - 1).empty());
  const auto respawned = nodes->collect_respawns(respawn);
  ASSERT_EQ(respawned.size(), 1U);
  EXPECT_FALSE(respawned.front().phase.has_value());
  const auto packets = gathering_state_packets(respawned);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets.front().opcode, kDoodadPhaseChangedNotification);
  EXPECT_FALSE(packets.front().message.contains("phase_id"));
  EXPECT_TRUE(nodes->doodads_on_map(kSharedMap).empty());

  std::vector<GameplayPacket> all = executed.packets;
  all.insert(all.end(), packets.begin(), packets.end());
  expect_wire(all);

  auto later = doodad_request(kBushLocalId);
  later["tick"] = wire_tick(respawn + kTicksPerMs);
  const auto regrown = harvest_doodad(second, later, kHerbExecuteMs, respawn + kTicksPerMs);
  EXPECT_NE(find_packet(regrown, 169), nullptr);
}

TEST_F(GatheringTest, TimedDoodadPhasesAdvanceAndAllowTheNextHarvest) {
  auto gatherer = session();
  const auto request = doodad_request(kWildBushLocalId, kWildBushInteraction, kSickleAnimation);
  auto adjusted = request;
  adjusted["pos"]["millimeter_x"] = 500;
  position.x_mm = 500;

  const auto first = harvest_doodad(gatherer, adjusted, kSickleExecuteMs);
  const auto *phase = find_packet(first, kDoodadPhaseChangedNotification);
  ASSERT_NE(phase, nullptr);
  EXPECT_EQ(phase->message.at("phase_id"), 2);
  gatherer.receive(kGatheringEndInteractionRequest, end_request(kSickleAnimation), 3, position,
                   kStartTicks + 5633 * kTicksPerMs);

  const auto harvested = kStartTicks + kSickleExecuteMs * kTicksPerMs;
  const auto early = gatherer.receive(kGatheringStartInteractionRequest, adjusted, 4, position,
                                      harvested + 100 * kTicksPerMs);
  EXPECT_EQ(error_code(early), "InteractionAnim_DoodadNotProgressible");

  const auto regrown = nodes->collect_respawns(harvested + kWildBushRegrowMs * kTicksPerMs);
  ASSERT_EQ(regrown.size(), 1U);
  ASSERT_TRUE(regrown.front().phase.has_value());
  EXPECT_EQ(*regrown.front().phase, 4);
  EXPECT_EQ(gathering_state_packets(regrown).front().message.at("phase_id"), 4);

  const auto second_start = harvested + (kWildBushRegrowMs + 10) * kTicksPerMs;
  adjusted["tick"] = wire_tick(second_start);
  const auto second = harvest_doodad(gatherer, adjusted, kSickleExecuteMs, second_start);
  EXPECT_NE(find_packet(second, 169), nullptr);
  EXPECT_EQ(find_packet(second, kDoodadPhaseChangedNotification)->message.at("phase_id"), 2);
  EXPECT_EQ(commits, 2);
}

TEST_F(GatheringTest, DoodadRequestsNamingAnotherTargetOrInteractionAreRefused) {
  auto gatherer = session();
  const auto expect_start_error = [&](const Json &request, const std::string &code) {
    const auto outcome =
        gatherer.receive(kGatheringStartInteractionRequest, request, 1, position, kStartTicks);
    EXPECT_EQ(error_code(outcome), code) << request.dump();
    EXPECT_FALSE(gatherer.busy());
  };

  expect_start_error(doodad_request(77), "InteractionAnim_NotFoundTarget");
  expect_start_error(doodad_request(kBushLocalId, kWildBushInteraction, kSickleAnimation),
                     "InteractionAnim_NotMatchInteractionGroup");
  expect_start_error(doodad_request(kBushLocalId, kRoseberryInteraction, kSickleAnimation),
                     "InteractionAnim_Mismatch");

  auto npc_target = doodad_request(kBushLocalId);
  npc_target["target_unit_id"] = std::to_string(doodad_unit_id(kDoodadSector, kBushLocalId) - 6);
  expect_start_error(npc_target, "InteractionAnim_InvalidTargetUnitType");

  position.map_id = 8000;
  expect_start_error(doodad_request(kBushLocalId), "InteractionAnim_NotFoundTarget");
}

TEST_F(GatheringTest, OnlyGatheringRequestsAreClaimed) {
  auto gatherer = session();
  Json statue = start_request(kHerbIndex);
  statue["param"] = {{"interaction", {{"interaction_id", 10}}}};
  EXPECT_FALSE(gatherer.handles(kGatheringStartInteractionRequest, statue));
  EXPECT_TRUE(gatherer.handles(kGatheringStartInteractionRequest, doodad_request(kBushLocalId)));
  EXPECT_TRUE(gatherer.handles(kGatheringStartInteractionRequest, start_request(kHerbIndex)));
  EXPECT_FALSE(gatherer.handles(kGatheringExecuteInteractionRequest, execute_request()));
  EXPECT_FALSE(gatherer.handles(kGatheringEndInteractionRequest, end_request()));
  EXPECT_FALSE(gatherer.handles(20010, Json::object()));
}

TEST_F(GatheringTest, RequestBurstsAreBounded) {
  auto gatherer = session();
  GatheringOutcome last;
  for (int request = 0; request < 40; ++request) {
    last = gatherer.receive(kGatheringStartInteractionRequest, start_request(999), 1, position,
                            kStartTicks);
  }
  EXPECT_EQ(error_code(last), "BadRequest");
}

TEST(GatheringDataTest, RejectsProfilesFromOtherBuildsOrWithDanglingReferences) {
  auto other_build = gathering_profile();
  other_build["build_id"] = "0000";
  EXPECT_THROW(GatheringData{other_build}, std::runtime_error);

  auto old_format = gathering_profile();
  old_format["format_version"] = 1;
  EXPECT_THROW(GatheringData{old_format}, std::runtime_error);

  auto dangling = gathering_profile();
  dangling["points"].push_back({kSector, 77, 998, 0, 0, 0});
  EXPECT_THROW(GatheringData{dangling}, std::runtime_error);

  auto duplicate = gathering_profile();
  duplicate["points"].push_back({kSector, kHerbIndex, 80, 0, 0, 0});
  EXPECT_THROW(GatheringData{duplicate}, std::runtime_error);

  auto bad_delay = gathering_profile();
  bad_delay["gatherings"][0]["activate_delay_min_ms"] = 200000;
  EXPECT_THROW(GatheringData{bad_delay}, std::runtime_error);

  auto unknown_doodad = gathering_profile();
  unknown_doodad["doodad_points"].push_back({kDoodadSector, 40, 12345, 0, 0, 0});
  EXPECT_THROW(GatheringData{unknown_doodad}, std::runtime_error);

  auto malformed_deactivation = gathering_profile();
  malformed_deactivation["doodad_points"][0].push_back(0);
  malformed_deactivation["doodad_points"][0].push_back(1);
  EXPECT_THROW(GatheringData{malformed_deactivation}, std::runtime_error);

  auto dangling_phase = gathering_profile();
  dangling_phase["doodads"][1]["phases"][1]["next"] = Json::array({Json::array({9, 1})});
  EXPECT_THROW(GatheringData{dangling_phase}, std::runtime_error);

  auto no_respawn = gathering_profile();
  no_respawn["doodads"][0].erase("respawn_delay_min_ms");
  EXPECT_ANY_THROW(GatheringData{no_respawn});

  const GatheringData valid{gathering_profile()};
  EXPECT_EQ(valid.point_count(), 4U);
  EXPECT_EQ(valid.definition_count(), 2U);
  EXPECT_EQ(valid.doodad_point_count(), 2U);
  EXPECT_TRUE(valid.has_map(kSharedMap));
  EXPECT_FALSE(valid.has_map(8000));
  EXPECT_TRUE(valid.is_tool(kHatchetItem, "Hatchet"));
  EXPECT_FALSE(valid.is_tool(kHatchetItem, "Pick"));
  EXPECT_TRUE(valid.is_doodad_interaction(kRoseberryInteraction));
  EXPECT_FALSE(valid.is_doodad_interaction(10));
  EXPECT_EQ(valid.tool_durability_loss(), 100);
  EXPECT_EQ(valid.sp_regen_delay_ms(), kSpRegenDelayMs);
}

TEST(GatheringDataTest, GeneratedProfileResolvesTheCapturedNodes) {
  const auto *profile = std::getenv("CHRONICLES_GATHERING_DEFINITIONS");
  if (!profile) {
    GTEST_SKIP() << "Set CHRONICLES_GATHERING_DEFINITIONS to the generated gathering profile";
  }

  const auto data = GatheringData::load(profile);
  // Sector (152, 292) index 23 was the first captured 1006 gathering target.
  const auto *point = data->point(kSector, kHerbIndex);
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(point->gathering_id, 80);
  const std::array<int, 3> point_mm{point->x_mm, point->y_mm, point->z_mm};
  EXPECT_TRUE(data->map_contains(9011, point_mm));
  EXPECT_TRUE(data->map_contains(kSharedMap, point_mm));

  const auto *definition = data->definition(point->gathering_id);
  ASSERT_NE(definition, nullptr);
  EXPECT_EQ(definition->animation_id, kHerbAnimation);
  ASSERT_EQ(definition->loot.groups.size(), 1U);
  EXPECT_EQ(definition->loot.groups.front().items.front().item_cms_id, kHerbItem);
  EXPECT_EQ(definition->loot.groups.front().items.front().min_quantity, 5);
  EXPECT_EQ(definition->loot.groups.front().items.front().max_quantity, 7);

  EXPECT_EQ(data->animation(kHerbAnimation)->execute_time_ms, kHerbExecuteMs);
  EXPECT_EQ(data->animation(kHatchetAnimation)->execute_time_ms, kHatchetExecuteMs);
  // Young Tree 102 has no authored reward; its capture-confirmed reward is the Log pack.
  ASSERT_NE(data->definition(102), nullptr);
  EXPECT_FALSE(data->is_unsupported(102));

  // Official Roseberry Bush target: sector 4735150, spot local ID 9 (unit ...691270).
  const auto *bush = data->doodad_point(kDoodadSector, kBushLocalId);
  ASSERT_NE(bush, nullptr);
  EXPECT_EQ(bush->doodad_id, 74);
  EXPECT_EQ(doodad_unit_id(kDoodadSector, kBushLocalId), 20337314391691270ULL);
  const auto *roseberry = data->doodad(74);
  ASSERT_NE(roseberry, nullptr);
  ASSERT_NE(roseberry->phase(1), nullptr);
  EXPECT_EQ(roseberry->phase(1)->interaction_id, kRoseberryInteraction);
  EXPECT_EQ(roseberry->phase(1)->reason, DoodadRewardReason::Looting);
  EXPECT_EQ(roseberry->respawn_delay_min_ms, kRoseberryRespawnMs);
  EXPECT_EQ(data->tool_durability_loss(), 100);
  EXPECT_EQ(data->sp_regen_delay_ms(), kSpRegenDelayMs);
}

// --- Corpse butchering (handling loot) -----------------------------------------------------

constexpr int kKnifeAnimation = 1000;
constexpr int kKnifeExecuteMs = 2930;
constexpr int kKnifeItem = 11200301;
constexpr int kAlphaWolf = 5001029;
constexpr int kWolfMolar = 70108008;
constexpr std::uint64_t kCorpseUnit = 1077794;
// Corpses lie wherever monsters die, including outside the gathering map areas.
constexpr int kUngatheredMap = 9031;

Json butchering_profile() {
  auto profile = gathering_profile();
  profile["animations"].push_back(animation(kKnifeAnimation, "Knife", kKnifeExecuteMs, 3833));
  profile["tool_items"]["Knife"] = Json::array({kKnifeItem});
  auto reward = loot(kWolfMolar, 1, 1);
  reward["npc_cms_id"] = kAlphaWolf;
  profile["handling"] = {{"interaction_anim_id", kKnifeAnimation},
                         {"rewards", Json::array({reward})},
                         {"unsupported", Json::array()}};
  return profile;
}

Json butcher_request(std::uint64_t unit_id = kCorpseUnit) {
  return {{"interaction_anim_id", kKnifeAnimation},
          {"target_unit_id", std::to_string(unit_id)},
          {"param", {{"handling_loot", Json::object()}}},
          {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
          {"yaw", -47.8},
          {"tick", wire_tick(kStartTicks)}};
}

class ButcheringTest : public GatheringTest {
protected:
  void SetUp() override {
    data = std::make_shared<const GatheringData>(butchering_profile());
    nodes = std::make_shared<GatheringNodes>(data, 3);
    position = {kUngatheredMap, 0, 0, 0, 0};
  }

  GatheringSession butcher_session() {
    auto gatherer = session();
    GatheringCorpses corpses;
    corpses.find = [this](std::uint64_t unit_id,
                          std::int64_t) -> std::optional<ButcheringTarget> {
      if (unit_id != kCorpseUnit || butchered) {
        return std::nullopt;
      }
      return ButcheringTarget{kAlphaWolf, kUngatheredMap, {0, 2000, 0}};
    };
    corpses.butchered = [this](std::uint64_t unit_id, std::int64_t) {
      butchered = true;
      return std::vector<GameplayPacket>{{173, {{"unit_id", std::to_string(unit_id)}}}};
    };
    gatherer.set_corpses(std::move(corpses));
    return gatherer;
  }

  void equip_knife() {
    CharacterItem knife;
    knife.id = 2;
    knife.cms_id = kKnifeItem;
    knife.equip_slot = kToolSlot2;
    knife.durability = 1500;
    character.items.push_back(knife);
  }

  bool butchered{};
};

// Captured official exchange (wavesword-mob-combat-20261026): Start 166 echoing handling_loot,
// execute 2.93 s later, then ItemsUpdateNoti (worn knife 1500 -> 1400, Wolf Molar, system
// reason), the corpse's cleared UnitLootItemInfoNoti and InteractionAnimRes.
TEST_F(ButcheringTest, KnifeButchersTheKillersCorpseOnceWithTheCapturedPacketOrder) {
  auto gatherer = butcher_session();
  equip_knife();
  ASSERT_TRUE(gatherer.handles(kGatheringStartInteractionRequest, butcher_request()));
  ASSERT_TRUE(gatherer.corpse_request(kGatheringStartInteractionRequest, butcher_request()));

  const auto started = gatherer.receive(kGatheringStartInteractionRequest, butcher_request(), 1,
                                        position, kStartTicks);
  ASSERT_EQ(opcodes(started), (std::vector<std::uint16_t>{166}));
  EXPECT_EQ(started.packets.front().message.at("param"),
            (Json{{"handling_loot", Json::object()}}));
  EXPECT_TRUE(gatherer.corpse_request(kGatheringExecuteInteractionRequest,
                                      execute_request(kKnifeAnimation)));

  const auto early = gatherer.receive(kGatheringExecuteInteractionRequest,
                                      execute_request(kKnifeAnimation), 2, position,
                                      kStartTicks + 1000 * kTicksPerMs);
  EXPECT_EQ(error_code(early), "InteractionAnim_TimeNotMatch");

  const auto executed = gatherer.receive(kGatheringExecuteInteractionRequest,
                                         execute_request(kKnifeAnimation), 3, position,
                                         kStartTicks + kKnifeExecuteMs * kTicksPerMs);
  ASSERT_EQ(opcodes(executed), (std::vector<std::uint16_t>{169, 173, 167}));
  const auto &update = executed.packets.front().message;
  EXPECT_EQ(update.at("stackables"),
            Json::array({{{"item_cms_id", kWolfMolar}, {"quantity", 1}}}));
  EXPECT_EQ(update.at("items").at(0).at("durability"), 1400);
  EXPECT_TRUE(update.contains("system"));
  EXPECT_TRUE(executed.broadcasts.empty());
  expect_wire(started.packets);
  expect_wire(executed.packets);

  EXPECT_TRUE(butchered);
  EXPECT_EQ(durable.stackables.at(kWolfMolar), 1);
  ASSERT_EQ(last_trade.durability_updates.size(), 1U);
  EXPECT_EQ(last_trade.durability_updates.front().durability, 1400);
  EXPECT_TRUE(gatherer.take_quest_events().empty());

  // The corpse is butchered: a second attempt has nothing to handle.
  gatherer.receive(kGatheringEndInteractionRequest, end_request(kKnifeAnimation), 4, position,
                   kStartTicks + 4000 * kTicksPerMs);
  const auto again = gatherer.receive(kGatheringStartInteractionRequest, butcher_request(), 5,
                                      position, kStartTicks + 5000 * kTicksPerMs);
  EXPECT_EQ(error_code(again), "Npc_HasNoHandlingLoot");
  EXPECT_EQ(commits, 1);
}

TEST_F(ButcheringTest, ButcheringNeedsAKnifeAKnownCorpseAndTheKnifeAnimation) {
  auto gatherer = butcher_session();
  const auto no_knife = gatherer.receive(kGatheringStartInteractionRequest, butcher_request(), 1,
                                         position, kStartTicks);
  EXPECT_EQ(error_code(no_knife), "InteractionAnim_InvalidCondition");

  equip_knife();
  const auto unknown = gatherer.receive(kGatheringStartInteractionRequest, butcher_request(99),
                                        2, position, kStartTicks);
  EXPECT_EQ(error_code(unknown), "Npc_HasNoHandlingLoot");

  auto wrong_animation = butcher_request();
  wrong_animation["interaction_anim_id"] = kHerbAnimation;
  const auto mismatched = gatherer.receive(kGatheringStartInteractionRequest, wrong_animation, 3,
                                           position, kStartTicks);
  EXPECT_EQ(error_code(mismatched), "InteractionAnim_Mismatch");

  position.y_mm = 20000;
  auto far_request = butcher_request();
  far_request["pos"]["millimeter_y"] = 20000;
  const auto far = gatherer.receive(kGatheringStartInteractionRequest, far_request, 4, position,
                                    kStartTicks);
  EXPECT_EQ(error_code(far), "TooFar");
  EXPECT_EQ(commits, 0);
  EXPECT_FALSE(butchered);
}

TEST_F(ButcheringTest, DamageCancelsCorpseGatheringAndLeavesItAvailableForAFullRetry) {
  attach_vitals = true;
  equip_knife();
  auto gatherer = butcher_session();

  const auto started = gatherer.receive(kGatheringStartInteractionRequest, butcher_request(), 1,
                                        position, kStartTicks);
  ASSERT_EQ(opcodes(started), (std::vector<std::uint16_t>{54, 166}));
  const auto interrupted = gatherer.interrupt(kStartTicks + 500 * kTicksPerMs);
  EXPECT_EQ(opcodes(interrupted), (std::vector<std::uint16_t>{54, 167}));
  EXPECT_EQ(interrupted.observers.size(), 1U);
  EXPECT_FALSE(gatherer.busy());
  EXPECT_EQ(stamina.cancelled, 1);
  EXPECT_EQ(stamina.spent, 0);
  EXPECT_EQ(commits, 0);
  EXPECT_EQ(character.items.front().durability, 1500);
  EXPECT_FALSE(butchered);

  const auto stale = gatherer.receive(kGatheringExecuteInteractionRequest,
                                      execute_request(kKnifeAnimation), 2, position,
                                      kStartTicks + kKnifeExecuteMs * kTicksPerMs);
  EXPECT_EQ(error_code(stale), "InteractionAnim_Mismatch");
  EXPECT_EQ(commits, 0);

  const auto retry_ticks = kStartTicks + 4000 * kTicksPerMs;
  const auto retried = gatherer.receive(kGatheringStartInteractionRequest, butcher_request(), 3,
                                        position, retry_ticks);
  ASSERT_EQ(opcodes(retried), (std::vector<std::uint16_t>{54, 166}));
  const auto early = gatherer.receive(kGatheringExecuteInteractionRequest,
                                      execute_request(kKnifeAnimation), 4, position,
                                      retry_ticks + 1000 * kTicksPerMs);
  EXPECT_EQ(error_code(early), "InteractionAnim_TimeNotMatch");
  EXPECT_EQ(commits, 0);

  const auto completed = gatherer.receive(kGatheringExecuteInteractionRequest,
                                          execute_request(kKnifeAnimation), 5, position,
                                          retry_ticks + kKnifeExecuteMs * kTicksPerMs);
  EXPECT_EQ(opcodes(completed), (std::vector<std::uint16_t>{169, 47, 168, 173, 167}));
  EXPECT_EQ(stamina.spent, 1);
  EXPECT_EQ(commits, 1);
  EXPECT_TRUE(butchered);
}

TEST_F(ButcheringTest, WithoutCorpseRightsTheRequestIsNotClaimed) {
  auto gatherer = session();
  EXPECT_FALSE(gatherer.handles(kGatheringStartInteractionRequest, butcher_request()));
  EXPECT_FALSE(gatherer.corpse_request(kGatheringStartInteractionRequest, butcher_request()));
  EXPECT_EQ(data->handling_animation_id(), kKnifeAnimation);
  ASSERT_NE(data->handling_reward(kAlphaWolf), nullptr);
  EXPECT_EQ(data->handling_reward(kAlphaWolf + 1), nullptr);
}

TEST_F(GatheringTest, DoodadHarvestsAreReportedForQuestObjectives) {
  auto gatherer = session();
  harvest_doodad(gatherer, doodad_request(kBushLocalId), kHerbExecuteMs);
  EXPECT_EQ(gatherer.take_quest_events(),
            (QuestEvents{{QuestEventKind::DoodadInteracted, 74, 1, 1}}));
  EXPECT_TRUE(gatherer.take_quest_events().empty());
}

} // namespace
} // namespace chronicles
