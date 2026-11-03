#include "world/features/combat/tests/combat_owner_test_support.hpp"

#include <algorithm>

namespace chronicles::combat {
namespace {

using namespace testing_support;

TEST_F(PlayerOwner, DamageToThisPlayerInterruptsGatheringButOtherDamageDoesNot) {
  context.tutorial = true;
  const auto player_id = std::to_string(context.combat_wire_player_id(context.actor.id));
  context.tutorial_packets = std::vector<GameplayPacket>{
      {19, {{"target_unit_id", "999"}, {"base_damage_amount", 12}}}};
  player->advance_session_combat(kStart, kPosition);
  EXPECT_EQ(context.gathering_interruptions, 0);
  context.tutorial_packets = std::vector<GameplayPacket>{
      {19, {{"target_unit_id", player_id}, {"base_damage_amount", 12}}}};
  const auto packets = player->advance_session_combat(kStart, kPosition);
  EXPECT_EQ(context.gathering_interruptions, 1);
  EXPECT_EQ(packets.back().opcode, 167);
}

TEST_F(PlayerOwner, DamageInterruptionUsesVictimAndIncludesAbsorbedHits) {
  const auto player_id = std::to_string(context.combat_wire_player_id(context.actor.id));
  std::vector<GameplayPacket> harmless{
      {19, {{"target_unit_id", "999"}, {"base_damage_amount", 10}}},
      {19, {{"target_unit_id", player_id}, {"base_damage_amount", 0}}}};
  player->interrupt_gathering_on_damage(harmless, kStart);
  EXPECT_EQ(context.gathering_interruptions, 0);

  std::vector<GameplayPacket> absorbed{
      {19, {{"target_unit_id", player_id}, {"base_damage_amount", 12},
            {"final_damage_amount", 0}}},
      {19, {{"target_unit_id", player_id}, {"base_damage_amount", 3}}}};
  player->interrupt_gathering_on_damage(absorbed, kStart);
  EXPECT_EQ(context.gathering_interruptions, 1);
  EXPECT_EQ(absorbed.back().opcode, 167);
}

TEST_F(PlayerOwner, GlidingIsHandledOnAnEmptyMapAndInTheTutorial) {
  for (const bool tutorial : {false, true}) {
    context.tutorial = tutorial;
    player->reset_movement_vitals();
    move("MoveMode_Falling", kPosition.z_mm);
    FrameContext frame(20032, WorldGameplayRequest{20032,
                                                   2,
                                                   {{"pos",
                                                     {{"millimeter_x", kPosition.x_mm},
                                                      {"millimeter_y", kPosition.y_mm},
                                                      {"millimeter_z", kPosition.z_mm}}}}});
    frame.party_position = kPosition;
    ASSERT_TRUE(player->handle_private_combat(frame));
    EXPECT_TRUE(frame.session_packets.empty());

    context.riding = true;
    ASSERT_TRUE(player->handle_private_combat(frame));
    ASSERT_EQ(frame.session_packets.size(), 1);
    EXPECT_EQ(frame.session_packets.front().message.at("ec"), "Move_GlideWhileRiding");
    context.riding = false;
  }
}

TEST_F(PlayerOwner, RegionalHandoffKeepsGlidingWhileOrdinaryMapMovesResetIt) {
  move("MoveMode_Falling", kPosition.z_mm);
  FrameContext start(20032, WorldGameplayRequest{20032,
                                                2,
                                                {{"pos",
                                                  {{"millimeter_x", kPosition.x_mm},
                                                   {"millimeter_y", kPosition.y_mm},
                                                   {"millimeter_z", kPosition.z_mm}}}}});
  start.party_position = kPosition;
  ASSERT_TRUE(player->handle_private_combat(start));
  ASSERT_TRUE(start.session_packets.empty());

  auto destination = kPosition;
  destination.map_id += 1;
  player->on_map_departure(true, true);
  FrameContext loaded(20085, WorldGameplayRequest{20085, 3, Json::object()});
  loaded.party_position = destination;
  player->observe_movement_vitals(loaded, std::nullopt);
  start.party_position = destination;
  ASSERT_TRUE(player->handle_private_combat(start));
  ASSERT_EQ(start.session_packets.size(), 1U);
  EXPECT_EQ(start.session_packets.front().message.at("ec"), "Move_GlideStartDuplication");

  player->on_map_departure(true);
  start.session_packets.clear();
  ASSERT_TRUE(player->handle_private_combat(start));
  ASSERT_EQ(start.session_packets.size(), 1U);
  EXPECT_EQ(start.session_packets.front().message.at("ec"), "Move_GlideNotFalling");
}

TEST_F(PlayerOwner, EntryVitalsDoNotRecreateAnExistingLiveSimulation) {
  const auto *first = std::as_const(*player).current_simulation();
  player->ensure_player_vitals(context.actor, context.progress);
  EXPECT_EQ(std::as_const(*player).current_simulation(), first);
  EXPECT_EQ(context.stat_reads, 1);
}

TEST_F(PlayerOwner, BoardingStopsTheExistingSwimmingStaminaFlow) {
  const auto started = move("MoveMode_Swimming", kPosition.z_mm, "MoveDetail_Run");
  ASSERT_EQ(started.session_packets.size(), 1U);
  const auto instance = started.session_packets.front().message.at("flows").at(0).at("instance_id");
  context.riding = true;
  player->observe_riding_vitals_reset();
  ASSERT_EQ(context.owner_packets.size(), 1U);
  const auto &stopped = context.owner_packets.front();
  EXPECT_EQ(stopped.opcode, 54);
  EXPECT_EQ(stopped.message.at("flows").at(0).at("instance_id"), instance);
  EXPECT_FALSE(stopped.message.at("flows").at(0).contains("rate_per_sec"));
  EXPECT_NE(stopped.message.at("regens").at(0).at("applied_regen_range_end_tick"),
            "9223372036854775807");
  player->observe_riding_vitals_reset();
  EXPECT_EQ(context.owner_packets.size(), 1U);
}

TEST_F(PlayerOwner, DisabledTutorialCombatKeepsTheExistingModelWithoutInstallingCallbacks) {
  const auto *first = std::as_const(*player).current_simulation();
  EXPECT_FALSE(player->start_tutorial_combat(context.actor, 305, true));
  EXPECT_EQ(std::as_const(*player).current_simulation(), first);
  EXPECT_EQ(context.stat_reads, 1);
  EXPECT_TRUE(context.calls.empty());
}

TEST_F(PlayerOwner, LethalLandingCommitsWearBeforePresenceAndPreservesEarlierBatchPackets) {
  CharacterItem item;
  item.id = 1001;
  item.cms_id = 200;
  item.quantity = 1;
  item.grade = 1;
  item.equip_slot = 0;
  item.durability = 1000;
  context.actor.items.push_back(item);
  move("MoveMode_Falling", 33000);
  context.calls.clear();

  FrameContext landing(20010, WorldGameplayRequest{20010, 2, Json::object()});
  landing.party_position = kPosition;
  landing.session_packets.push_back({10, Json::object()});
  context.on_life = [&landing](const PlayerLifeCommitted &) {
    EXPECT_EQ(std::ranges::count(landing.session_packets, 169, &GameplayPacket::opcode), 0);
  };
  player->observe_movement_vitals(landing, Json{{"mode", "MoveMode_Normal"}});
  EXPECT_EQ(context.gathering_interruptions, 1);
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"death_commit", "snapshot", "presence", "snapshot"}));
  EXPECT_TRUE(context.progress.dead);
  ASSERT_EQ(context.death_changes.size(), 1U);
  EXPECT_EQ(context.death_changes[0].expected_durability, 1000);
  EXPECT_EQ(context.death_changes[0].durability, 970);
  ASSERT_FALSE(landing.session_packets.empty());
  EXPECT_EQ(landing.session_packets.front().opcode, 10);
  const auto items = std::ranges::find(landing.session_packets, 169, &GameplayPacket::opcode);
  const auto damage = std::ranges::find(landing.session_packets, 19, &GameplayPacket::opcode);
  ASSERT_NE(items, landing.session_packets.end());
  ASSERT_NE(damage, landing.session_packets.end());
  EXPECT_LT(items, damage);
}

TEST_F(PlayerOwner, DeathConflictPublishesNoSnapshotOrDurableLifeEffect) {
  move("MoveMode_Falling", 33000);
  context.calls.clear();
  context.commit_succeeds = false;
  EXPECT_THROW(move("MoveMode_Normal", 3000), std::runtime_error);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"death_commit"}));
  EXPECT_FALSE(context.progress.dead);
}

TEST_F(PlayerOwner, LostDeathAndFenceAcknowledgementsReconcileWithoutSecondWear) {
  CharacterItem item;
  item.id = 1001;
  item.cms_id = 200;
  item.equip_slot = 0;
  item.durability = 1000;
  context.actor.items.push_back(item);
  context.apply_death_wear = true;
  context.lose_death_ack = true;
  move("MoveMode_Falling", 33000);
  EXPECT_THROW(move("MoveMode_Normal", 3000), std::runtime_error);
  ASSERT_TRUE(context.progress.dead);
  ASSERT_EQ(context.actor.items.front().durability, 970);

  context.lose_fence_ack = true;
  EXPECT_EQ(player->reconcile_pending_death(), CombatPlayerSession::DeathRecovery::retry);
  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 1);
  EXPECT_EQ(context.actor.items.front().durability, 970);
  EXPECT_TRUE(player->has_pending_death());
  EXPECT_TRUE(context.death_registered);
  EXPECT_TRUE(player->acknowledge_reconciled_death());
  EXPECT_FALSE(player->has_pending_death());
  EXPECT_FALSE(context.death_registered);
}

TEST_F(PlayerOwner, FencedUncommittedDeathRetriesTheExactWearOnce) {
  CharacterItem item;
  item.id = 1001;
  item.cms_id = 200;
  item.equip_slot = 0;
  item.durability = 1000;
  context.actor.items.push_back(item);
  context.apply_death_wear = true;
  context.throw_before_death_commit = true;
  move("MoveMode_Falling", 33000);
  EXPECT_THROW(move("MoveMode_Normal", 3000), std::runtime_error);
  EXPECT_FALSE(context.progress.dead);
  EXPECT_EQ(context.actor.items.front().durability, 1000);

  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_TRUE(context.progress.dead);
  EXPECT_EQ(context.actor.items.front().durability, 970);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 2);
  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 2);
}

TEST_F(PlayerOwner, FencedDeathWithUnexpectedProgressStaysQuarantined) {
  context.throw_before_death_commit = true;
  move("MoveMode_Falling", 33000);
  EXPECT_THROW(move("MoveMode_Normal", 3000), std::runtime_error);
  ++context.progress.gold;

  EXPECT_EQ(player->reconcile_pending_death(), CombatPlayerSession::DeathRecovery::conflict);
  EXPECT_FALSE(context.progress.dead);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 1);
  EXPECT_TRUE(context.death_registered);
  EXPECT_FALSE(player->acknowledge_reconciled_death());
}

TEST_F(PlayerOwner, RefusedDeathRecoveryRegistrationDoesNotStartAWrite) {
  context.death_registration_allowed = false;
  move("MoveMode_Falling", 33000);
  EXPECT_THROW(move("MoveMode_Normal", 3000), std::runtime_error);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 0);
  EXPECT_FALSE(context.progress.dead);
  EXPECT_TRUE(player->has_pending_death());
  EXPECT_FALSE(context.death_registered);
  EXPECT_EQ(player->reconcile_pending_death(), CombatPlayerSession::DeathRecovery::retry);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 0);
}

TEST_F(PlayerOwner, RefusedDeathRegistrationCanRetryWithoutRepeatingWear) {
  CharacterItem item;
  item.id = 1001;
  item.cms_id = 200;
  item.equip_slot = 0;
  item.durability = 1000;
  context.actor.items.push_back(item);
  context.apply_death_wear = true;
  context.death_registration_allowed = false;
  move("MoveMode_Falling", 33000);
  EXPECT_THROW(move("MoveMode_Normal", 3000), std::runtime_error);
  ASSERT_TRUE(player->has_pending_death());
  EXPECT_FALSE(context.death_registered);
  EXPECT_EQ(context.death_recovery_updates, 1);

  EXPECT_EQ(player->reconcile_pending_death(), CombatPlayerSession::DeathRecovery::retry);
  EXPECT_FALSE(context.death_registered);
  EXPECT_FALSE(context.progress.dead);
  EXPECT_EQ(context.actor.items.front().durability, 1000);
  EXPECT_EQ(std::ranges::count(context.calls, "death_fence"), 0);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 0);

  context.death_registration_allowed = true;
  context.lose_death_ack = true;
  EXPECT_EQ(player->reconcile_pending_death(), CombatPlayerSession::DeathRecovery::retry);
  EXPECT_TRUE(context.death_registered);
  EXPECT_EQ(context.death_recovery_updates, 2);
  EXPECT_TRUE(context.progress.dead);
  EXPECT_EQ(context.actor.items.front().durability, 970);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 1);

  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_EQ(context.actor.items.front().durability, 970);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 1);
  EXPECT_TRUE(player->acknowledge_reconciled_death());
  EXPECT_FALSE(context.death_registered);
  EXPECT_FALSE(player->has_pending_death());
}

TEST_F(PlayerOwner, DeathPreparationFailureRetainsOwnerUntilFencedRetry) {
  CharacterItem item;
  item.id = 1001;
  item.cms_id = 200;
  item.equip_slot = 0;
  item.durability = 1000;
  context.actor.items.push_back(item);
  context.apply_death_wear = true;
  move("MoveMode_Falling", 33000);
  context.throw_next_progress_read = true;

  EXPECT_THROW(move("MoveMode_Normal", 3000), std::bad_alloc);
  EXPECT_TRUE(player->has_pending_death());
  EXPECT_TRUE(context.death_registered);
  EXPECT_FALSE(context.progress.dead);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 0);
  EXPECT_EQ(context.death_recovery_updates, 1);

  context.closed = true;
  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_TRUE(context.progress.dead);
  EXPECT_EQ(context.actor.items.front().durability, 970);
  EXPECT_TRUE(player->acknowledge_reconciled_death());
}

TEST_F(PlayerOwner, MissingDeathCatalogItemRetainsOwnerWithoutWriting) {
  CharacterItem item;
  item.id = 1001;
  item.cms_id = 999999;
  item.equip_slot = 0;
  item.durability = 1000;
  context.actor.items.push_back(item);
  move("MoveMode_Falling", 33000);

  EXPECT_THROW(move("MoveMode_Normal", 3000), std::runtime_error);
  EXPECT_TRUE(player->has_pending_death());
  EXPECT_TRUE(context.death_registered);
  EXPECT_EQ(player->reconcile_pending_death(), CombatPlayerSession::DeathRecovery::retry);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 0);
  EXPECT_FALSE(context.progress.dead);

  context.actor.items.front().cms_id = 200;
  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_TRUE(context.progress.dead);
}

TEST_F(PlayerOwner, MissingDeathCharacterRetainsOwnerWithoutWriting) {
  move("MoveMode_Falling", 33000);
  context.character_exists = false;

  EXPECT_THROW(move("MoveMode_Normal", 3000), std::runtime_error);
  EXPECT_TRUE(player->has_pending_death());
  EXPECT_TRUE(context.death_registered);
  EXPECT_EQ(player->reconcile_pending_death(), CombatPlayerSession::DeathRecovery::retry);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 0);

  context.character_exists = true;
  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_TRUE(context.progress.dead);
}

TEST_F(PlayerOwner, MissingDeathNotificationRetainsUncommittedDeathForReconnect) {
  player->current_simulation()->apply_fall_damage(1000, kPosition, context.ticks);
  std::vector<GameplayPacket> incomplete_packets;

  EXPECT_THROW(player->persist_player_death(incomplete_packets), std::runtime_error);
  EXPECT_TRUE(player->has_pending_death());
  EXPECT_TRUE(context.death_registered);
  EXPECT_FALSE(context.progress.dead);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 0);

  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_TRUE(context.progress.dead);
}

TEST_F(PlayerOwner, FailedDeathOwnerRetirementRetainsThePlanForReconciliation) {
  context.death_forget_allowed = false;
  move("MoveMode_Falling", 33000);
  EXPECT_THROW(move("MoveMode_Normal", 3000), std::runtime_error);
  ASSERT_TRUE(context.progress.dead);
  ASSERT_TRUE(context.death_registered);
  ASSERT_EQ(context.death_recovery_updates, 1);
  EXPECT_EQ(player->reconcile_pending_death(),
            CombatPlayerSession::DeathRecovery::committed_reconnect);
  EXPECT_FALSE(player->acknowledge_reconciled_death());
  EXPECT_TRUE(player->has_pending_death());
  context.death_forget_allowed = true;
  EXPECT_TRUE(player->acknowledge_reconciled_death());
  EXPECT_FALSE(context.death_registered);
  EXPECT_EQ(std::ranges::count(context.calls, "death_commit"), 1);
}

TEST_F(PlayerOwner, OrdinaryMovementSnapshotDoesNotRefreshDurablePresence) {
  move("MoveMode_Normal", 3000);
  EXPECT_EQ(std::ranges::count(context.calls, "snapshot"), 1);
  EXPECT_EQ(std::ranges::count(context.calls, "presence"), 0);
}

TEST_F(PlayerOwner, RefusedRevivalLeavesTheSimulationAndStoreDeadWithoutSuccessPackets) {
  kill_model();
  context.commit_succeeds = false;
  context.calls.clear();
  EXPECT_TRUE(player->revive_player().empty());
  EXPECT_EQ(context.calls, (std::vector<std::string>{"revival_commit"}));
  EXPECT_TRUE(player->current_simulation()->player_dead());
  EXPECT_TRUE(context.progress.dead);
}

TEST_F(PlayerOwner, RevivalCommitsBeforeSimulationEffectsAndDurablePresenceExactlyOnce) {
  kill_model();
  context.calls.clear();
  const auto packets = player->revive_player();
  EXPECT_EQ(context.calls, (std::vector<std::string>{"revival_commit", "snapshot", "presence"}));
  EXPECT_FALSE(context.progress.dead);
  EXPECT_FALSE(player->current_simulation()->player_dead());
  std::vector<std::uint16_t> opcodes;
  for (const auto &packet : packets) {
    opcodes.push_back(packet.opcode);
  }
  EXPECT_EQ(opcodes, (std::vector<std::uint16_t>{49, 51, 47, 32}));
  EXPECT_TRUE(player->revive_player().empty());
  EXPECT_EQ(std::ranges::count(context.calls, "presence"), 1);
}

TEST_F(PlayerOwner, ShrineSupplyPlanningPrecedesItsAtomicCommitAndTutorialAdoption) {
  kill_model();
  context.tutorial = true;
  const WorldPosition destination{9012, 8000, 9000, 10000, 1};
  const auto packets = player->persist_player_revival(destination, 21);
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"supplies", "shrine_commit", "tutorial_adopt"}));
  ASSERT_TRUE(context.shrine_candidate);
  EXPECT_EQ(context.shrine_candidate->stackables.at(101), 3);
  ASSERT_TRUE(context.adopted_progress);
  EXPECT_FALSE(context.adopted_progress->dead);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets[0].opcode, 169);
}

TEST_F(PlayerOwner, MapDepartureClearsObserverWhilePreservingOnlyTheRequestedSimulation) {
  auto retained = std::make_shared<int>(1);
  std::weak_ptr<int> observer = retained;
  player->current_simulation()->set_defeat_observer(
      [retained](const MonsterDefeat &) -> MonsterDefeatReaction {
        return {};
      });
  retained.reset();
  player->on_map_departure(true);
  EXPECT_TRUE(player->has_simulation());
  EXPECT_TRUE(observer.expired());
  player->on_map_departure(false);
  EXPECT_FALSE(player->has_simulation());
}

class DepartingCombatOwner : public PlayerOwner {
protected:
  DepartingCombatOwner() : PlayerOwner(profile()) {}

  static Json profile() {
    auto map = empty_population(kPosition.map_id);
    map["spawns"].push_back({{"unit_id", "576460752303423506"},
                              {"cms_id", 5001015},
                              {"position", {1000, 2000, 3000}},
                              {"yaw", 0.0},
                              {"faction", "EX5Faction_Bandit"},
                              {"spawner_guid", {"1", "2"}},
                              {"anim_stance_key", 0},
                              {"respawn_ms", 2000},
                              {"one_time_use", false}});
    auto result = world_profile(Json::array({map}));
    auto &combat = result["combat"];
    combat["npc_stats"]["5001015"] =
        {{"18", 50}, {"19", 1}, {"69", 50}, {"72", 1}, {"22", 10}, {"17", 10}};
    combat["npc_locomotion"]["5001015"] =
        {{"search_circle_range_mm", 2000}, {"search_cone_range_mm", 8000},
         {"search_half_cone_angle", 90.0}, {"hearing_range_mm", 0},
         {"leash_range_mm", 50000}, {"reset_delay_ms", 3000},
         {"combat_distance_min_mm", 1000}, {"combat_distance_max_mm", 2000},
         {"run_speed_mm_per_second", 3000.0}, {"move_speed_rate", 1000}};
    const Json stage{{"index", 0}, {"duration_ms", 1000}, {"next_stage", -1},
                     {"events", {{{"effect_id", 1}, {"unique_id", 0}, {"time_ms", 300}}}},
                     {"notifies", Json::array()}};
    combat["skills"].push_back(
        {{"id", 201}, {"classes", {1, 2, 3, 5}}, {"npc", true},
         {"weapon", "None"}, {"type", "Normal"}, {"costs", {{"SP", 20}}},
         {"cooldown_ms", 1500}, {"cooldown_group", 0}, {"range_mm", 5000},
         {"require_target", false}, {"angle", 360},
         {"layers", {{{"index", 0}, {"stages", {stage}}}}}});
    combat["effects"]["1"] =
        {{"kind", "damage"}, {"parameters", {{"physicalCoefficient", 1},
                                                   {"magicCoefficient", 0}}},
         {"targeting_kind", "shapeCylinderSector"},
         {"targeting", {{"outerRadius", 300}, {"halfHeight", 200},
                         {"angle", 120}, {"maxTargetCount", 1}}}, {"chain", 0}};
    combat["npc_skills"]["5001015"] = {201};
    return result;
  }
};

TEST_F(DepartingCombatOwner, FailedDetachPreservesGlideAndDefeatObserverUntilRetry) {
  player->ensure_world_mobs(kPosition);
  auto *simulation = player->current_simulation();
  const auto entry = player->advance_session_combat(kStart, kPosition);
  const auto spawn = std::ranges::find(entry, 10, &GameplayPacket::opcode);
  ASSERT_NE(spawn, entry.end());
  const auto unit_id = std::stoull(spawn->message.at("unit_id").get<std::string>());
  const auto player_id = context.combat_wire_player_id(context.actor.id);
  ASSERT_NE(simulation->monster_state_fingerprint_for_test().find(
                "own=" + std::to_string(player_id)), std::string::npos);

  move("MoveMode_Falling", kPosition.z_mm);
  FrameContext start(20032, WorldGameplayRequest{20032, 2,
                                                {{"pos", {{"millimeter_x", kPosition.x_mm},
                                                          {"millimeter_y", kPosition.y_mm},
                                                          {"millimeter_z", kPosition.z_mm}}}}});
  start.party_position = kPosition;
  ASSERT_TRUE(player->handle_private_combat(start));
  ASSERT_TRUE(start.session_packets.empty());
  ASSERT_TRUE(player->gliding());

  int observer_calls = 0;
  auto retained = std::make_shared<int>(1);
  std::weak_ptr<int> observer = retained;
  simulation->set_defeat_observer([retained, &observer_calls](const MonsterDefeat &) {
    ++observer_calls;
    return MonsterDefeatReaction{};
  });
  retained.reset();
  bool injected = false;
  simulation->set_world_detach_hook_for_test([&](std::string_view phase) {
    if (!injected && phase == "owner_cleared") {
      injected = true;
      throw std::runtime_error("Injected map departure detach failure");
    }
  });
  EXPECT_THROW(player->on_map_departure(true), std::runtime_error);
  ASSERT_TRUE(injected);
  EXPECT_TRUE(player->has_world_mobs());
  EXPECT_TRUE(player->gliding());
  EXPECT_FALSE(observer.expired());

  simulation->gm_damage_monster(unit_id, 1000, kStart + kMillisecond);
  EXPECT_EQ(observer_calls, 1);
  simulation->set_world_detach_hook_for_test({});
  player->on_map_departure(true);
  EXPECT_FALSE(player->has_world_mobs());
  EXPECT_FALSE(player->gliding());
  EXPECT_TRUE(observer.expired());
}

TEST_F(PlayerOwner, TutorialDepartureReleasesItsModelAndPendingMovementDeadline) {
  move("MoveMode_Normal", 3000, "MoveDetail_Sprint");
  ASSERT_TRUE(player->next_action_tick());
  player->end_tutorial_combat();
  EXPECT_FALSE(player->has_simulation());
  EXPECT_FALSE(player->next_action_tick());
}

TEST_F(PlayerOwner, WorldLootAttachesOnlyAfterThePopulationAndOnlyOnce) {
  player->ensure_world_mobs(kPosition);
  player->ensure_world_mobs(kPosition);
  EXPECT_TRUE(player->has_world_mobs());
  EXPECT_EQ(context.loot_attachments, 1);
  player->detach_world_mobs();
  EXPECT_FALSE(player->has_world_mobs());
}

class InstancePopulationEntry : public PlayerOwner {
protected:
  InstancePopulationEntry()
      : PlayerOwner(world_profile(Json::array({empty_population(9013, "Instance")}))) {}
};

TEST_F(InstancePopulationEntry, PopulationWaitsForStartPlayingAfterMapLoad) {
  const WorldPosition instance_position{9013, 1000, 2000, 3000, 0};
  FrameContext loaded(20085, WorldGameplayRequest{20085, 1, Json::object()});
  loaded.party_position = instance_position;
  player->observe_shared_map_mobs(loaded, true);
  EXPECT_FALSE(player->has_world_mobs());
  EXPECT_EQ(context.loot_attachments, 0);

  FrameContext started(20086, WorldGameplayRequest{20086, 2, Json::object()});
  started.party_position = instance_position;
  player->observe_shared_map_mobs(started, true);
  EXPECT_TRUE(player->has_world_mobs());
  EXPECT_EQ(context.loot_attachments, 1);
}

TEST_F(PlayerOwner, HousingAndTutorialKeepWorldPopulationAndLootUnattached) {
  context.housing = true;
  player->ensure_world_mobs(kPosition);
  context.housing = false;
  context.tutorial = true;
  player->ensure_world_mobs(kPosition);
  EXPECT_FALSE(player->has_world_mobs());
  EXPECT_EQ(context.loot_attachments, 0);
}

TEST_F(PlayerOwner, SurfaceMovementCancelsDrowningBeforeTheSharedTimerRuns) {
  move("MoveMode_Swimming", 3000, "MoveDetail_SW_Dive");
  context.ticks += 500 * kMillisecond;
  move("MoveMode_Swimming", 3000, "MoveDetail_SurfaceIdle");
  context.ticks += 1000 * kMillisecond;
  std::vector<GameplayPacket> packets;
  player->advance_gameplay_tick(context.ticks, kPosition, packets);
  EXPECT_EQ(player->current_simulation()->player_stats().at("69"), 1000);
  EXPECT_EQ(std::ranges::count(context.calls, "presence"), 0);
}

TEST_F(PlayerOwner, DrowningDamageInterruptsGatheringOnTheGameplayTick) {
  move("MoveMode_Swimming", 3000, "MoveDetail_SW_Dive");
  context.ticks += 1000 * kMillisecond;
  std::vector<GameplayPacket> packets;

  player->advance_gameplay_tick(context.ticks, kPosition, packets);

  EXPECT_EQ(context.gathering_interruptions, 1);
  EXPECT_NE(std::ranges::find(packets, 19, &GameplayPacket::opcode), packets.end());
  EXPECT_NE(std::ranges::find(packets, 167, &GameplayPacket::opcode), packets.end());
}

TEST_F(PlayerOwner, PvpStatusChangesDeliverOwnerBeforeObserversAndKeepEncodingFailureState) {
  const auto entry = player->update_pvp_presence(true, false, kPosition, 0, false);
  ASSERT_TRUE(entry.registration);
  ASSERT_TRUE(*entry.registration);
  std::string reply;
  player->player_pvp_command(true, [&reply](std::string message) {
    reply = std::move(message);
  });
  ASSERT_EQ(context.owner_packets.size(), 1U);
  ASSERT_EQ(context.observer_packets.size(), 1U);
  EXPECT_EQ(context.owner_packets[0].opcode, 82);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"owner", "observers"}));
  EXPECT_EQ(reply, "PvP flag enabled.");
  context.send_owner_throws = true;
  player->player_pvp_command(false, [&reply](std::string message) {
    reply = std::move(message);
  });
  EXPECT_FALSE(feature.player_pvp().enabled(context.combat_connection_id()));
  EXPECT_EQ(context.observer_packets.size(), 1U);
  EXPECT_EQ(reply, "The server could not update your PvP flag. Try again.");
}

TEST_F(PlayerOwner, PvpHousingAndTutorialNamespacesStillResetTheFlagOnTravel) {
  player->update_pvp_presence(true, false, kPosition, -7, false);
  player->player_pvp_command(true, [](std::string) {
  });
  const auto change = player->update_pvp_presence(false, true, kPosition, 0, true);
  ASSERT_TRUE(change.notification);
  EXPECT_FALSE(feature.player_pvp().enabled(context.combat_connection_id()));
}

// Captured official weapon switches: StartSkillReq of a SwapWeapon skill carrying the destination
// equip_slot (Weapon_2), or omitting it for Weapon_1, answered by WeaponSlotActivationNoti 172.
class WeaponSwapOwner : public PlayerOwner {
protected:
  static constexpr int kSwapToAxeSkill = 1102000;
  static constexpr int kSwordSkill = 1101;
  static constexpr int kSword = 200;
  static constexpr int kAxe = 300;

  WeaponSwapOwner() : PlayerOwner(world_profile_with(swap_combat_data())) {
    // Recreate the session so that it starts with both weapons equipped and Weapon_1 active.
    context.actor.items = {weapon(1, kSword, 0), weapon(2, kAxe, 1)};
    context.actor.build.active_weapon_slot = 0;
    player = feature.create_session(context);
    context.player = player.get();
    player->ensure_player_vitals(context.actor, context.progress);
    // Skills are accepted only on the map whose shared encounters the session owns.
    player->ensure_world_mobs(kPosition);
    // The inventory owner commits the slot and refreshes combat's weapon, as native_world does.
    context.on_weapon_selection = [this](const Json &slot) {
      context.actor.build.active_weapon_slot = slot == "EX5EquipSlotType_Weapon_2" ? 1 : 0;
      player->current_simulation()->update_player_weapon(context.actor);
      return std::vector<GameplayPacket>{
          {172, {{"ec", "Success"}, {"slot", context.actor.build.active_weapon_slot}}}};
    };
    context.calls.clear();
  }

  static Json swap_combat_data() {
    auto combat = empty_combat_data();
    combat["skills"] = Json::array({skill(kSwapToAxeSkill, "SwapWeapon", "HandAxe"),
                                    skill(kSwordSkill, "Normal", "Sword")});
    combat["weapon_items"] = {{std::to_string(kSword), "Sword"},
                              {std::to_string(kAxe), "HandAxe"}};
    return combat;
  }

  static Json world_profile_with(Json combat) {
    // The session owns encounters only on a map the profile lists.
    auto profile = world_profile(Json::array({empty_population(kPosition.map_id)}));
    profile["combat"] = std::move(combat);
    return profile;
  }

  static Json skill(int skill_id, const char *type, const char *weapon_type) {
    const Json stage{{"index", 0}, {"duration_ms", 500}, {"next_stage", -1},
                     {"events", Json::array()}, {"notifies", Json::array()}};
    return {{"id", skill_id},       {"classes", {1, 2, 3, 5}},  {"npc", false},
            {"weapon", weapon_type}, {"type", type},             {"costs", Json::object()},
            {"cooldown_ms", 0},     {"cooldown_group", 0},      {"range_mm", 0},
            {"require_target", false}, {"angle", 360},
            {"layers", {{{"index", 0}, {"stages", {stage}}}}}};
  }

  static CharacterItem weapon(std::int64_t id, int cms_id, int slot) {
    CharacterItem item;
    item.id = id;
    item.cms_id = cms_id;
    item.quantity = 1;
    item.grade = 1;
    item.equip_slot = slot;
    item.durability = 1000;
    return item;
  }

  WorldGameplayRequest start_skill(int skill_id, std::uint32_t instance,
                                   std::optional<std::string> equip_slot) const {
    Json message{{"start_skill",
                  {{"skill_id", skill_id},
                   {"instance_id", instance},
                   {"pos",
                    {{"millimeter_x", kPosition.x_mm},
                     {"millimeter_y", kPosition.y_mm},
                     {"millimeter_z", kPosition.z_mm}}},
                   {"rotation", {{"yaw", 0}}},
                   {"tick", "1"}}}};
    if (equip_slot) {
      message["equip_slot"] = *equip_slot;
    }
    return {20010, instance, std::move(message)};
  }
};

TEST_F(WeaponSwapOwner, SwapSkillSelectsItsWeaponSlotBeforeTheSkillStarts) {
  const auto packets =
      player->receive(start_skill(kSwapToAxeSkill, 1, "EX5EquipSlotType_Weapon_2"), kPosition,
                      context.ticks);
  ASSERT_EQ(context.weapon_selections.size(), 1U);
  EXPECT_EQ(context.weapon_selections[0], "EX5EquipSlotType_Weapon_2");
  ASSERT_FALSE(packets.empty());
  EXPECT_EQ(packets.front().opcode, 172);
  // The swap skill is validated against the newly selected axe and starts (opcode 14).
  const auto started = std::ranges::find(packets, 14, &GameplayPacket::opcode);
  ASSERT_NE(started, packets.end()) << packets.back().message.dump();
  EXPECT_EQ(started->message.at("weapon_slot_type"), 1);
}

TEST_F(WeaponSwapOwner, OmittedEquipSlotSelectsTheFirstWeapon) {
  player->receive(start_skill(kSwapToAxeSkill, 1, "EX5EquipSlotType_Weapon_2"), kPosition,
                  context.ticks);
  context.ticks += 600 * kMillisecond;
  player->receive(start_skill(kSwapToAxeSkill, 2, std::nullopt), kPosition, context.ticks);
  ASSERT_EQ(context.weapon_selections.size(), 2U);
  EXPECT_EQ(context.weapon_selections[1], 0);
}

TEST_F(WeaponSwapOwner, OrdinarySkillsNeverChangeTheSelectedWeapon) {
  const auto packets = player->receive(start_skill(kSwordSkill, 1, "EX5EquipSlotType_Weapon_2"),
                                       kPosition, context.ticks);
  EXPECT_TRUE(context.weapon_selections.empty());
  EXPECT_EQ(std::ranges::count(packets, 172, &GameplayPacket::opcode), 0);
  EXPECT_EQ(std::ranges::count(packets, 14, &GameplayPacket::opcode), 1);
}

} // namespace
} // namespace chronicles::combat
