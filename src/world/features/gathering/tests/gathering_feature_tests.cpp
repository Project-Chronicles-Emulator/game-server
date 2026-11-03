#include "common/clock_ticks.hpp"
#include "common/world_metrics.hpp"
#include "world/features/gathering/gathering_feature.hpp"
#include "world/features/gathering/gathering_recovery.hpp"

#include "testing/log_capture.hpp"
#include "world/core/tests/economy_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <mutex>
#include <set>

namespace chronicles::gathering {
namespace {

using namespace economy_testing;

constexpr int kMap = 8000;
constexpr int kCliffsMap = 30104;
constexpr std::uint32_t kSector = 123;
constexpr std::uint32_t kIndex = 4;
constexpr int kAnimation = 1006;
constexpr int kDefinition = 80;
constexpr int kProbeToolFollowUp = 900;
constexpr int kToolSlot = 3;

std::int64_t current_ticks() {
  using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
  return std::chrono::duration_cast<Ticks>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::shared_ptr<const GatheringData> focused_data(bool requires_tool = false,
                                                   int respawn_delay_ms = 10000,
                                                   int extra_placements = 0,
                                                   bool deactivated_doodad = false) {
  const Json reward{{"pack_rate_percent", 100.0},
                    {"groups", Json::array({{{"rate_percent", 100.0},
                                             {"items", Json::array({{{"item_cms_id", kMaterial},
                                                                     {"min_quantity", 1},
                                                                     {"max_quantity", 1},
                                                                     {"weight", 1}}})}}})}};
  auto doodad_reward = reward;
  doodad_reward["reason"] = "looting";
  auto definition = reward;
  definition.update({{"gathering_id", kDefinition},
                     {"interaction_anim_id", kAnimation},
                     {"deactivate_delay_ms", std::min(1000, respawn_delay_ms)},
                     {"activate_delay_min_ms", respawn_delay_ms},
                     {"activate_delay_max_ms", respawn_delay_ms}});
  Json profile{
      {"format_version", 2},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"maps", Json::array({{{"map_id", kMap},
                             {"area_min_mm", {-1000, -1000, -1000}},
                             {"area_max_mm", {1000, 1000, 1000}}},
                            {{"map_id", kCliffsMap},
                             {"area_min_mm", {-1000, -1000, -1000}},
                             {"area_max_mm", {1000, 1000, 1000}}}})},
      {"tool_durability_loss", 100},
      {"sp_regen_delay_ms", 300},
      {"animations", Json::array({{{"interaction_anim_id", kAnimation},
                                   {"sp_cost", 10},
                                   {"tool_equipment_type", requires_tool ? "Hatchet" : "None"},
                                   {"execute_time_ms", 10},
                                   {"duration_ms", 20}}})},
      {"gatherings", Json::array({definition})},
      {"unsupported_gatherings", Json::array()},
      {"tool_items", {{"Hatchet", {kEquipment}}}},
      {"points", Json::array({{kSector, kIndex, kDefinition, 0, 0, 0}})},
      {"doodads", Json::array({{{"doodad_id", 58000},
                                  {"initial_phase", 1},
                                  {"phases", Json::array({
                                      {{"phase_id", 1}, {"trigger", "interaction"},
                                       {"interaction_id", 20003},
                                       {"interaction_anim_id", kAnimation}, {"reward", doodad_reward},
                                       {"next", Json::array({Json::array({2, 1})})}},
                                      {{"phase_id", 2}, {"trigger", "timer"},
                                       {"duration_ms", respawn_delay_ms},
                                       {"next", Json::array({Json::array({1, 1})})}}})}}})},
      {"doodad_points", Json::array({{kSector, 9, 58000, 0, 0, 0}})}};
  if (deactivated_doodad) {
    profile["doodad_points"][0] = {kSector, 9, 58000, 0, 0, 0, 0, true};
    profile["doodad_points"].push_back({kSector, 10, 58000, 0, 0, 0});
  }
  for (int index = 0; index <= extra_placements; ++index) {
    if (extra_placements > 0) {
      profile["points"].push_back({kSector, 1000 + index, kDefinition, 0, 0, 0});
    }
  }
  profile["doodads"].push_back(
      {{"doodad_id", 777}, {"initial_phase", 1},
       {"phases", Json::array({{{"phase_id", 1}, {"trigger", "interaction"},
                                 {"interaction_id", 10000146}, {"instant", true},
                                 {"next", Json::array({Json::array({2, 1})})}},
                                {{"phase_id", 2}, {"trigger", "interaction"},
                                 {"interaction_id", 10000146}, {"instant", true},
                                 {"next", Json::array({Json::array({1, 1})})}}})}});
  profile["doodad_points"].push_back({170 | (289 << 14), 8, 777, 0, 0, 0});
  return std::make_shared<const GatheringData>(profile);
}

class ProbeContext final : public GatheringSessionContext {
public:
  explicit ProbeContext(EconomyFixture &fixture) : fixture_(fixture) {
  }

  bool gathering_register_recovery(std::shared_ptr<GatheringRecoveryOperation> operation) override {
    if (recovery_) {
      return false;
    }
    recovery_ = std::move(operation);
    return true;
  }

  void gathering_recovery_updated(std::shared_ptr<GatheringRecoveryOperation> operation) override {
    if (operation != recovery_) {
      throw std::runtime_error("Unexpected gathering recovery owner");
    }
    if (operation->status() != GatheringRecoveryStatus::pending) {
      recovery_.reset();
    }
  }

  void gathering_force_reconnect() override {
    reconnect_requested = true;
  }

  GatheringActorIdentity gathering_actor_identity() const override {
    return {fixture_.actor.account_id, fixture_.actor.id, fixture_.actor.world_id, 0, "shared"};
  }

  GatheringPreparedQuest gathering_prepare_quest(
      const QuestEvents &events, const WorldPosition &, const WorldProgress &,
      const WorldProgress &after, const ItemTrade &trade) override {
    GatheringPreparedQuest result;
    result.after = after;
    result.trade = trade;
    if (!events.empty()) {
      fixture_.trace.push_back("quest-plan");
      result.finalize_intent = [this](bool committed) {
        fixture_.trace.push_back(committed ? "quest-commit" : "quest-reject");
      };
    }
    return result;
  }

  std::vector<GameplayPacket> gathering_publish_committed(
      const WorldProgress &, const WorldProgress &, const std::vector<CharacterItem> &) override {
    if (fail_committed_publication) {
      throw std::runtime_error("Synthetic committed response failure");
    }
    return {};
  }

  bool gathering_has_tutorial() const override {
    return tutorial;
  }

  bool gathering_sector_streaming() const override {
    return streamed;
  }

  bool gathering_other_action_busy() const override {
    return other_busy;
  }

  std::int64_t gathering_monotonic_ticks() const override {
    return ticks;
  }

  bool gathering_player_dead() const override {
    return dead;
  }

  std::vector<GameplayPacket> gathering_advance_combat(const WorldPosition &position,
                                                       std::int64_t now) override {
    if (advance_combat) {
      return advance_combat(position, now);
    }
    return {};
  }

  std::optional<std::vector<GameplayPacket>>
  gathering_begin_stamina(const GatheringStaminaFlow &) override {
    begin_generation = generation;
    if (insufficient_stamina) {
      return std::nullopt;
    }
    return std::vector<GameplayPacket>{};
  }

  bool gathering_spend_stamina(std::int64_t) override {
    fixture_.trace.push_back("spend");
    spent_generation = generation;
    return combat;
  }

  std::optional<GameplayPacket> gathering_cancel_stamina(std::int64_t) override {
    fixture_.trace.push_back("cancel");
    return std::nullopt;
  }

  std::vector<GameplayPacket> gathering_tool_wear_updates(const CharacterItem &tool) override {
    fixture_.trace.push_back("tool-worn");
    worn_durability = tool.durability;
    return {{kProbeToolFollowUp, Json::object()}};
  }

  void gathering_deliver_observers(const std::vector<GameplayPacket> &packets) override {
    observers.insert(observers.end(), packets.begin(), packets.end());
  }

  bool tutorial{};
  bool streamed{};
  bool other_busy{};
  bool dead{};
  bool combat{true};
  bool insufficient_stamina{};
  bool reconnect_requested{};
  bool fail_committed_publication{};
  int generation{1};
  int begin_generation{};
  int spent_generation{};
  int worn_durability{};
  std::int64_t ticks{current_ticks()};
  std::vector<GameplayPacket> observers;
  std::function<std::vector<GameplayPacket>(const WorldPosition &, std::int64_t)> advance_combat;
  world_metrics::TimedMutex codec_probe;

private:
  EconomyFixture &fixture_;
  std::shared_ptr<GatheringRecoveryOperation> recovery_;
};

class GatheringRecordingProgress final : public ProgressAccess {
public:
  explicit GatheringRecordingProgress(EconomyFixture &fixture)
      : backend_(*fixture.repositories.characters, *fixture.repositories.character_state,
                 fixture.actor.account_id, fixture.actor.id, fixture.actor.world_id),
        trace_(fixture.trace) {
  }

  WorldProgress load() override {
    trace_.push_back("load");
    return backend_.load();
  }

  std::optional<Character> character() override {
    trace_.push_back("character");
    return backend_.character();
  }

  std::optional<std::vector<CharacterItem>> commit(
      const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) override {
    trace_.push_back("commit");
    return backend_.commit(before, after, trade);
  }

  LootClaimResult commit_gathering_quest_claim(
      const std::string &receipt, const std::string &fingerprint, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source) override {
    trace_.push_back("commit");
    return backend_.commit_gathering_quest_claim(receipt, fingerprint, before, after, trade,
                                                effects, source);
  }

  GatheringFinalizeResult finalize_gathering_quest_claim(
      const std::string &receipt, const std::string &fingerprint, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source,
      std::int64_t confirmation_ticks) override {
    return backend_.finalize_gathering_quest_claim(receipt, fingerprint, before, after, trade,
                                                   effects, source, confirmation_ticks);
  }

private:
  RepositoryProgressAccess backend_;
  std::vector<std::string> &trace_;
};

FrameContext request_frame(std::uint16_t opcode, Json message = Json::object()) {
  FrameContext frame(opcode, WorldGameplayRequest{opcode, 9, std::move(message)});
  frame.gameplay_position = WorldPosition{kMap, 0, 0, 0, 0.0F};
  return frame;
}

class HarvestProbe {
public:
  explicit HarvestProbe(bool requires_tool = false, int respawn_delay_ms = 10000)
      : context(fixture) {
    if (requires_tool) {
      const auto tool = fixture.repositories.character_state->grant_item(
          fixture.actor.account_id, fixture.actor.id, fixture.actor.world_id,
          CharacterItem{0, kEquipment, 5, kToolSlot, 1, 2000, 0});
      if (!tool) {
        throw std::runtime_error("Cannot grant the focused gathering tool");
      }
    }
    SessionEventBus::Subscriptions subscriptions;
    subscriptions.player_state.push_back([this](const PlayerCombatSnapshotChanged &) {
      std::lock_guard lock(context.codec_probe);
      fixture.trace.push_back("combat-cache");
    });
    subscriptions.progress.push_back([this](const PlayerProgressCommitted &) {
      fixture.trace.push_back("unexpected-tutorial-adopt");
    });
    events = std::make_unique<SessionEventBus>(std::move(subscriptions));
    fixture.repositories.character_state->acquire_monster_reward_owner_epoch();
    feature = std::make_shared<GatheringFeature>(io, focused_data(requires_tool, respawn_delay_ms));
    const auto repository = fixture.repositories.character_state;
    const auto world_id = fixture.actor.world_id;
    GatheringSharedSourceStore source_store;
    source_store.list = [repository, world_id](const auto &after, std::size_t limit) {
      return repository->shared_gathering_sources(world_id, after, limit);
    };
    source_store.transition = [repository, world_id](const auto &transition,
                                                     std::int64_t ticks, bool require_due) {
      return repository->update_shared_gathering_source(world_id, transition, ticks, require_due);
    };
    source_store.fence = [repository, world_id](const auto &transition) {
      return repository->fence_shared_gathering_source(world_id, transition);
    };
    source_store.post_worker = [](std::function<void()> task) {
      task();
    };
    feature->set_shared_source_store(std::move(source_store));
    feature->hydrate_shared_sources();
    auto progress = std::make_shared<GatheringRecordingProgress>(fixture);
    session = feature->create_session(17, progress, context, *events, 7);
  }

  bool start() {
    auto frame = request_frame(
        kGatheringStartInteractionRequest,
        {{"interaction_anim_id", kAnimation},
         {"target_unit_id", std::to_string(static_cast<std::uint64_t>(kSector) << 32)},
         {"param", {{"gathering", {{"indices", Json::array({kIndex})}}}}},
         {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
         {"yaw", 0.0},
         {"tick", std::to_string(context.ticks * 100)}});
    return feature->handle(session.get(), frame, context, broadcasts);
  }

  std::vector<GameplayPacket> execute() {
    context.ticks += 100'000;
    auto frame = request_frame(kGatheringExecuteInteractionRequest,
                               {{"interaction_anim_id", kAnimation}, {"tick", "1"}});
    EXPECT_TRUE(feature->handle(session.get(), frame, context, broadcasts));
    return frame.session_packets;
  }

  EconomyFixture fixture;
  boost::asio::io_context io;
  ProbeContext context;
  std::unique_ptr<SessionEventBus> events;
  std::shared_ptr<GatheringFeature> feature;
  std::unique_ptr<GatheringSession> session;
  std::vector<GatheringStateChange> broadcasts;
};

// A focused port that records the trade, exercising the GatheringSession wear callback contract
// independently of the repository; the repository case below commits real wear.
class SuccessfulWearProgress final : public ProgressAccess {
public:
  explicit SuccessfulWearProgress(EconomyFixture &fixture) : fixture_(fixture), durable_(fixture) {
    character_ = fixture.progress->character();
    progress_ = fixture.stored_progress();
  }

  WorldProgress load() override {
    return progress_;
  }

  std::optional<Character> character() override {
    return character_;
  }

  std::optional<std::vector<CharacterItem>>
  commit(const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) override {
    fixture_.trace.push_back("commit");
    if (before != progress_ || after.revision != before.revision + 1) {
      return std::nullopt;
    }
    for (const auto &update : trade.durability_updates) {
      auto item = std::ranges::find(character_->items, update.item_id, &CharacterItem::id);
      if (item == character_->items.end() || item->durability != update.expected_durability) {
        return std::nullopt;
      }
      item->durability = update.durability;
    }
    progress_ = after;
    last_trade = trade;
    return std::vector<CharacterItem>{};
  }

  LootClaimResult commit_gathering_quest_claim(
      const std::string &receipt, const std::string &fingerprint, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source) override {
    last_trade = trade;
    return durable_.commit_gathering_quest_claim(receipt, fingerprint, before, after, trade,
                                                effects, source);
  }

  GatheringFinalizeResult finalize_gathering_quest_claim(
      const std::string &receipt, const std::string &fingerprint, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source,
      std::int64_t confirmation_ticks) override {
    return durable_.finalize_gathering_quest_claim(receipt, fingerprint, before, after, trade,
                                                   effects, source, confirmation_ticks);
  }

  ItemTrade last_trade;

private:
  EconomyFixture &fixture_;
  GatheringRecordingProgress durable_;
  std::optional<Character> character_;
  WorldProgress progress_;
};

TEST(GatheringFeature, FailedTimerPagesDoNotStarveLaterOrNewSources) {
  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, focused_data(false, 30, 129));
  std::map<SharedGatheringSourceKey, StoredSharedGatheringSource> durable;
  StoredSharedGatheringSource due;
  due.generation = 1;
  due.active = false;
  due.next_transition_epoch_ticks = unix_epoch_ticks_now() + 200 * 10000;
  for (std::uint32_t index = 1001; index <= 1129; ++index) {
    durable.emplace(SharedGatheringSourceKey{kMap, 0, kSector, index}, due);
  }
  GatheringSharedSourceStore store;
  store.list = [&](const std::optional<SharedGatheringSourceKey> &after, std::size_t limit) {
    std::vector<SharedGatheringSourceRecord> page;
    auto entry = after ? durable.upper_bound(*after) : durable.begin();
    for (; entry != durable.end() && page.size() < limit; ++entry) {
      page.push_back({entry->first, entry->second});
    }
    return page;
  };
  store.transition = [&](const SharedGatheringSourceTransition &transition, std::int64_t,
                         bool require_due) {
    EXPECT_TRUE(require_due);
    if (transition.local_index != 1000 && transition.local_index != 1129) {
      throw std::runtime_error("Synthetic stuck source");
    }
    const SharedGatheringSourceKey key{transition.map_id, transition.kind, transition.sector_id,
                                        transition.local_index};
    auto &source = durable.at(key);
    EXPECT_EQ(source.generation, transition.expected_generation);
    ++source.generation;
    source.active = true;
    source.next_transition_epoch_ticks.reset();
    return SharedGatheringSourceUpdateResult{true, source};
  };
  store.fence = [](const SharedGatheringSourceTransition &) -> SharedGatheringSourceUpdateResult {
    throw std::runtime_error("Unexpected source fence");
  };
  store.post_worker = [](std::function<void()> task) {
    task();
  };
  feature->set_shared_source_store(std::move(store));
  feature->set_respawn_delivery([](const auto &, std::uint64_t) {});
  feature->hydrate_shared_sources();
  io.run_for(std::chrono::milliseconds(400));

  const SharedGatheringSourceKey newcomer{kMap, 0, kSector, 1000};
  due.next_transition_epoch_ticks = unix_epoch_ticks_now();
  durable.emplace(newcomer, due);
  const GatheringNodeKey new_node{kMap, GatheringNodeKind::Placement, kSector, 1000};
  ASSERT_TRUE(feature->nodes_for(nullptr)->install_shared_source(
      new_node, due, unix_epoch_ticks_now(), current_ticks()));
  feature->schedule_respawn();
  io.restart();
  io.run_for(std::chrono::milliseconds(1800));
  EXPECT_TRUE(durable.at(newcomer).active);
  EXPECT_TRUE(durable.at({kMap, 0, kSector, 1129}).active);
  EXPECT_FALSE(durable.at({kMap, 0, kSector, 1001}).active);
}

TEST(GatheringFeature, BossLeaseReservesAndInstallsTheWholeEvent) {
  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, focused_data());
  const BossScriptIntent intent{std::string(32, 'b'), 1, kCliffsMap, 5001510, "shared"};
  const auto lease = feature->reserve_boss_script_sources(intent);
  ASSERT_TRUE(lease);
  EXPECT_FALSE(feature->reserve_boss_script_sources(intent));
  ASSERT_TRUE(lease->mark_write_started());
  EXPECT_FALSE(lease->abort_before_write(current_ticks()));

  StoredSharedGatheringSource barricade;
  barricade.generation = 1;
  barricade.active = true;
  barricade.phase = 2;
  StoredSharedGatheringSource object;
  object.generation = 1;
  object.active = true;
  object.phase = 1;
  const std::uint32_t sector = 170 | (289 << 14);
  BossScriptSettlement settlement{true, {{{kCliffsMap, 1, sector, 5}, barricade},
                                         {{kCliffsMap, 1, sector, 8}, object}}, intent.event_id};
  auto wrong_event = settlement;
  wrong_event.event_id = std::string(32, 'c');
  EXPECT_FALSE(lease->install(wrong_event, 1000, current_ticks()));
  auto pending = settlement;
  pending.sources[1].state.pending_receipt_id = std::string(32, 'a');
  EXPECT_FALSE(lease->install(pending, 1000, current_ticks()));
  const auto changes = lease->install(settlement, 1000, current_ticks());
  ASSERT_TRUE(changes);
  ASSERT_EQ(changes->size(), 2U);
  EXPECT_EQ(changes->front().phase, 2);
  EXPECT_EQ(changes->back().phase, 1);
  const auto retry_publication = lease->install(settlement, 1000, current_ticks());
  ASSERT_TRUE(retry_publication);
  EXPECT_EQ(retry_publication->size(), 2U);
  EXPECT_FALSE(feature->reserve_boss_script_sources(intent));
  const auto object_node = changes->back().node;
  const auto definitions = focused_data();
  const auto *object_definition = definitions->doodad(777);
  ASSERT_NE(object_definition, nullptr);
  EXPECT_FALSE(feature->nodes_for(nullptr)->reserve_doodad(object_node, *object_definition,
                                                          1, 2, std::nullopt));
  ASSERT_TRUE(lease->acknowledge_publication());
  const auto next_interaction = feature->nodes_for(nullptr)->reserve_doodad(
      object_node, *object_definition, 1, 2, std::nullopt);
  ASSERT_TRUE(next_interaction);
  ASSERT_TRUE(feature->nodes_for(nullptr)->finish_reservation(object_node, *next_interaction,
                                                               false, current_ticks()));

  const auto replay = feature->reserve_boss_script_sources(intent);
  ASSERT_TRUE(replay);
  ASSERT_TRUE(replay->mark_write_started());
  const auto repeated = replay->install(settlement, 1000, current_ticks());
  ASSERT_TRUE(repeated);
  EXPECT_EQ(repeated->size(), 2U);
  EXPECT_TRUE(replay->acknowledge_publication());
}

TEST(GatheringFeature, CliffsActivatedInitialPhasesAppearInLateAndRestartedSnapshots) {
  const auto *profile = std::getenv("CHRONICLES_GATHERING_DEFINITIONS");
  if (!profile) {
    GTEST_SKIP() << "Set CHRONICLES_GATHERING_DEFINITIONS to the generated gathering profile";
  }

  const auto data = GatheringData::load(profile);
  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, data);
  constexpr std::uint32_t kEliteSector = 170 | (289 << 14);
  constexpr std::uint32_t kRiftSector = 170 | (290 << 14);
  const BossScriptIntent elite{std::string(32, 'e'), 1, kCliffsMap, 5001510, "shared"};
  const BossScriptIntent captain{std::string(32, 'f'), 1, kCliffsMap, 5001104, "shared"};
  StoredSharedGatheringSource activated;
  activated.generation = 1;
  activated.active = true;
  activated.phase = 1;
  auto barricade = activated;
  barricade.phase = 2;
  const BossScriptSettlement elite_result{
      true, {{{kCliffsMap, 1, kEliteSector, 5}, barricade},
             {{kCliffsMap, 1, kEliteSector, 8}, activated}}, elite.event_id};
  const BossScriptSettlement captain_result{
      true, {{{kCliffsMap, 1, kRiftSector, 2}, activated}}, captain.event_id};

  for (const auto &[intent, settlement] :
       std::array<std::pair<BossScriptIntent, BossScriptSettlement>, 2>{
           std::pair{elite, elite_result}, std::pair{captain, captain_result}}) {
    const auto lease = feature->reserve_boss_script_sources(intent);
    ASSERT_TRUE(lease);
    ASSERT_TRUE(lease->mark_write_started());
    ASSERT_TRUE(lease->install(settlement, 1000, current_ticks()));
    ASSERT_TRUE(lease->acknowledge_publication());
  }

  const auto expect_activated = [](const std::vector<GameplayPacket> &packets,
                                   std::uint32_t local_id) {
    ASSERT_EQ(packets.size(), 1U);
    ASSERT_TRUE(packets.front().message.contains("doodads"));
    const auto &doodads = packets.front().message.at("doodads");
    EXPECT_TRUE(std::ranges::any_of(doodads, [&](const Json &doodad) {
      return doodad.at("id") == local_id && doodad.value("phase", 0) == 1;
    }));
  };

  const std::array elite_sector{kEliteSector};
  const std::array rift_sector{kRiftSector};
  expect_activated(feature->sector_snapshots(kCliffsMap, elite_sector), 8);
  expect_activated(feature->sector_snapshots(kCliffsMap, rift_sector), 2);
  const auto current_doodads = feature->nodes_for(nullptr)->doodads_on_map(kCliffsMap);
  EXPECT_TRUE(std::ranges::any_of(current_doodads, [](const GatheringStateChange &change) {
    return change.node.sector_id == kEliteSector && change.node.index == 8 && change.phase == 1;
  }));
  EXPECT_TRUE(std::ranges::any_of(current_doodads, [](const GatheringStateChange &change) {
    return change.node.sector_id == kRiftSector && change.node.index == 2 && change.phase == 1;
  }));

  // A fresh feature models process restart and hydration from the committed source rows.
  const auto restarted = std::make_shared<GatheringFeature>(io, data);
  const auto nodes = restarted->nodes_for(nullptr);
  ASSERT_TRUE(nodes->install_shared_source({kCliffsMap, GatheringNodeKind::Doodad,
                                             kEliteSector, 5}, barricade, 1000, current_ticks()));
  ASSERT_TRUE(nodes->install_shared_source({kCliffsMap, GatheringNodeKind::Doodad,
                                             kEliteSector, 8}, activated, 1000, current_ticks()));
  ASSERT_TRUE(nodes->install_shared_source({kCliffsMap, GatheringNodeKind::Doodad,
                                             kRiftSector, 2}, activated, 1000, current_ticks()));
  expect_activated(restarted->sector_snapshots(kCliffsMap, elite_sector), 8);
  expect_activated(restarted->sector_snapshots(kCliffsMap, rift_sector), 2);
  const auto restored_doodads = nodes->doodads_on_map(kCliffsMap);
  EXPECT_TRUE(std::ranges::any_of(restored_doodads, [](const GatheringStateChange &change) {
    return change.node.sector_id == kEliteSector && change.node.index == 8 && change.phase == 1;
  }));
  EXPECT_TRUE(std::ranges::any_of(restored_doodads, [](const GatheringStateChange &change) {
    return change.node.sector_id == kRiftSector && change.node.index == 2 && change.phase == 1;
  }));
}

TEST(GatheringFeature, OrdinaryDurableDoodadsStillOmitTheirInitialPhase) {
  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, focused_data());
  StoredSharedGatheringSource source;
  source.generation = 1;
  source.active = true;
  source.phase = 1;
  const auto nodes = feature->nodes_for(nullptr);
  ASSERT_TRUE(nodes->install_shared_source({kMap, GatheringNodeKind::Doodad, kSector, 9},
                                           source, 1000, current_ticks()));
  const std::array sectors{kSector};
  const auto packets = feature->sector_snapshots(kMap, sectors);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_FALSE(packets.front().message.contains("doodads"));
  EXPECT_TRUE(nodes->doodads_on_map(kMap).empty());
}

TEST(GatheringFeature, AuthoredDeactivationIsPerPlacementInLateAndRestartedSnapshots) {
  const auto data = focused_data(false, 10000, 0, true);
  const auto *inactive = data->doodad_point(kMap, kSector, 9);
  const auto *ordinary = data->doodad_point(kMap, kSector, 10);
  ASSERT_NE(inactive, nullptr);
  ASSERT_NE(ordinary, nullptr);
  EXPECT_TRUE(inactive->initially_deactivated);
  EXPECT_FALSE(ordinary->initially_deactivated);

  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, data);
  const auto nodes = feature->nodes_for(nullptr);
  StoredSharedGatheringSource activated;
  activated.generation = 1;
  activated.active = true;
  activated.phase = 1;
  const GatheringNodeKey inactive_node{kMap, GatheringNodeKind::Doodad, kSector, 9};
  const GatheringNodeKey ordinary_node{kMap, GatheringNodeKind::Doodad, kSector, 10};
  ASSERT_TRUE(nodes->install_shared_source(inactive_node, activated, 1000, current_ticks()));
  ASSERT_TRUE(nodes->install_shared_source(ordinary_node, activated, 1000, current_ticks()));

  const std::array sectors{kSector};
  const auto packets = feature->sector_snapshots(kMap, sectors);
  ASSERT_EQ(packets.size(), 1U);
  const auto &doodads = packets.front().message.at("doodads");
  ASSERT_EQ(doodads.size(), 1U);
  EXPECT_EQ(doodads.front().at("id"), 9);
  EXPECT_EQ(doodads.front().at("phase"), 1);
  const auto map_changes = nodes->doodads_on_map(kMap);
  ASSERT_EQ(map_changes.size(), 1U);
  EXPECT_EQ(map_changes.front().node.index, 9U);

  const auto restarted = std::make_shared<GatheringFeature>(io, data);
  ASSERT_TRUE(restarted->nodes_for(nullptr)->install_shared_source(
      inactive_node, activated, 1000, current_ticks()));
  const auto restored = restarted->sector_snapshots(kMap, sectors);
  ASSERT_EQ(restored.size(), 1U);
  ASSERT_EQ(restored.front().message.at("doodads").size(), 1U);
  EXPECT_EQ(restored.front().message.at("doodads").front().at("id"), 9);
}

TEST(GatheringFeature, PrivateBossLeaseRetainsCopySourcesUntilPublication) {
  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, focused_data());
  InstanceCopies copies({kCliffsMap});
  const auto copy = copies.enter(kCliffsMap, 1, std::nullopt);
  const auto other = copies.enter(kCliffsMap, 2, std::nullopt);
  ASSERT_TRUE(copy);
  ASSERT_TRUE(other);
  const BossScriptIntent intent{std::string(32, 'b'), 1, kCliffsMap, 5001510,
                                "nonce:" + std::to_string(copy->serial())};
  EXPECT_FALSE(feature->reserve_boss_script_sources(intent));
  const auto lease = feature->reserve_boss_script_sources(intent, copy.get());
  ASSERT_TRUE(lease);
  ASSERT_TRUE(lease->mark_write_started());
  const auto changes = lease->install_private(current_ticks());
  ASSERT_TRUE(changes);
  ASSERT_EQ(changes->size(), 2U);
  EXPECT_EQ(changes->front().phase, 2);
  EXPECT_EQ(changes->back().phase, 1);
  EXPECT_FALSE(feature->reserve_boss_script_sources(intent, copy.get()));
  ASSERT_TRUE(lease->install_private(current_ticks()));
  ASSERT_TRUE(lease->acknowledge_publication());
  EXPECT_FALSE(feature->reserve_boss_script_sources(intent, other.get()));
  auto other_intent = intent;
  other_intent.copy_scope = "nonce:" + std::to_string(other->serial());
  const auto other_lease = feature->reserve_boss_script_sources(other_intent, other.get());
  ASSERT_TRUE(other_lease);
  EXPECT_TRUE(other_lease->abort_before_write(current_ticks()));
}

TEST(GatheringFeature, BossLeaseRefusesToResetUnpersistedProgress) {
  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, focused_data());
  const GatheringNodeKey object{kCliffsMap, GatheringNodeKind::Doodad, 170 | (289 << 14), 8};
  feature->nodes_for(nullptr)->set_scripted_doodad_phase(object, 777, 2, current_ticks());
  const BossScriptIntent intent{std::string(32, 'b'), 1, kCliffsMap, 5001510, "shared"};
  EXPECT_FALSE(feature->reserve_boss_script_sources(intent));
  feature->nodes_for(nullptr)->set_scripted_doodad_phase(object, 777, 1, current_ticks());
  const auto safe = feature->reserve_boss_script_sources(intent);
  ASSERT_TRUE(safe);
  EXPECT_TRUE(safe->abort_before_write(current_ticks()));
}

TEST(GatheringFeature, ConfirmedGatherSchedulesItsAuthoredRespawn) {
  HarvestProbe probe(false, 30);
  std::vector<GatheringStateChange> delivered;
  probe.feature->set_respawn_delivery(
      [&](const std::vector<GatheringStateChange> &changes, std::uint64_t channel) {
        EXPECT_EQ(channel, 0U);
        delivered.insert(delivered.end(), changes.begin(), changes.end());
      });
  ASSERT_TRUE(probe.start());
  const auto before_commit = unix_epoch_ticks_now();
  probe.execute();
  const GatheringNodeKey node{kMap, GatheringNodeKind::Placement, kSector, kIndex};
  const auto depleted = probe.feature->nodes_for(nullptr)->shared_source(node);
  ASSERT_TRUE(depleted);
  EXPECT_FALSE(depleted->active);
  ASSERT_TRUE(depleted->next_transition_epoch_ticks);
  EXPECT_GE(*depleted->next_transition_epoch_ticks, before_commit + 60 * 10000);

  probe.io.restart();
  probe.io.run_for(std::chrono::milliseconds(150));
  const auto restored = probe.feature->nodes_for(nullptr)->shared_source(node);
  ASSERT_TRUE(restored);
  EXPECT_TRUE(restored->active);
  EXPECT_FALSE(restored->next_transition_epoch_ticks);
  ASSERT_EQ(delivered.size(), 1U);
  EXPECT_EQ(delivered.front().node, node);
  EXPECT_TRUE(delivered.front().active);
}

TEST(GatheringFeature, ActorSourcePacketCarriesItsCommittedRevision) {
  HarvestProbe probe(false, 30);
  ASSERT_TRUE(probe.start());
  auto packets = probe.execute();
  const auto state_packet = std::ranges::find_if(packets, [](const GameplayPacket &packet) {
    return packet.opcode == kGatheringStateChangedNotification;
  });
  ASSERT_NE(state_packet, packets.end());
  ASSERT_TRUE(state_packet->local_plan);
  auto unchanged = state_packet->message;
  EXPECT_EQ(state_packet->local_plan->prepare(unchanged), LocalPacketDecision::keep);

  const GatheringNodeKey node{kMap, GatheringNodeKind::Placement, kSector, kIndex};
  auto current = probe.feature->nodes_for(nullptr)->shared_source(node);
  ASSERT_TRUE(current);
  ++current->generation;
  current->active = true;
  current->next_transition_epoch_ticks.reset();
  ASSERT_TRUE(probe.feature->nodes_for(nullptr)->install_shared_source(
      node, *current, unix_epoch_ticks_now(), current_ticks()));
  EXPECT_EQ(state_packet->local_plan->prepare(unchanged), LocalPacketDecision::drop);
}

TEST(GatheringFeature, CommittedResponseFailureStillSchedulesOneRespawn) {
  HarvestProbe probe(false, 30);
  std::vector<GatheringStateChange> delivered;
  probe.feature->set_respawn_delivery(
      [&](const std::vector<GatheringStateChange> &changes, std::uint64_t) {
        delivered.insert(delivered.end(), changes.begin(), changes.end());
      });
  probe.context.fail_committed_publication = true;
  ASSERT_TRUE(probe.start());
  probe.execute();
  EXPECT_TRUE(probe.context.reconnect_requested);
  EXPECT_EQ(probe.fixture.stored_progress().stackables.at(kMaterial), 21);
  EXPECT_TRUE(probe.broadcasts.empty());

  probe.io.restart();
  probe.io.run_for(std::chrono::milliseconds(150));
  ASSERT_EQ(delivered.size(), 2U);
  EXPECT_FALSE(delivered.front().active);
  EXPECT_TRUE(delivered.back().active);
  EXPECT_EQ(probe.fixture.stored_progress().stackables.at(kMaterial), 21);
}

TEST(GatheringFeature, ConfirmedDoodadAdvancesItsAuthoredTimedPhase) {
  HarvestProbe probe(false, 30);
  std::vector<GatheringStateChange> delivered;
  probe.feature->set_respawn_delivery(
      [&](const std::vector<GatheringStateChange> &changes, std::uint64_t) {
        delivered.insert(delivered.end(), changes.begin(), changes.end());
      });
  const auto unit = std::to_string(doodad_unit_id(kSector, 9));
  auto start = request_frame(
      kGatheringStartInteractionRequest,
      {{"interaction_anim_id", kAnimation}, {"target_unit_id", unit},
       {"param", {{"interaction", {{"interaction_id", 20003}}}}},
       {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}},
       {"yaw", 0.0}, {"tick", std::to_string(probe.context.ticks * 100)}});
  ASSERT_TRUE(probe.feature->handle(probe.session.get(), start, probe.context, probe.broadcasts));
  const auto before_commit = unix_epoch_ticks_now();
  probe.execute();
  const GatheringNodeKey node{kMap, GatheringNodeKind::Doodad, kSector, 9};
  const auto before = probe.feature->nodes_for(nullptr)->shared_source(node);
  ASSERT_TRUE(before);
  EXPECT_EQ(before->phase, 2);
  ASSERT_TRUE(before->next_transition_epoch_ticks);
  EXPECT_GE(*before->next_transition_epoch_ticks, before_commit + 30 * 10000);
  EXPECT_LT(probe.fixture.trace_index("quest-plan"), probe.fixture.trace_index("quest-commit"));

  probe.io.restart();
  probe.io.run_for(std::chrono::milliseconds(150));
  const auto after = probe.feature->nodes_for(nullptr)->shared_source(node);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->phase, 1);
  EXPECT_FALSE(after->next_transition_epoch_ticks);
  ASSERT_EQ(delivered.size(), 1U);
  EXPECT_EQ(delivered.front().phase, 1);
}

TEST(GatheringFeature, RequestGuardKeepsTutorialAndUnrelatedFramesUnconsumed) {
  HarvestProbe probe;
  probe.context.tutorial = true;
  EXPECT_FALSE(probe.start());
  auto ping = request_frame(20003);
  EXPECT_FALSE(probe.feature->handle(probe.session.get(), ping, probe.context, probe.broadcasts));
  EXPECT_TRUE(probe.fixture.trace.empty());
}

TEST(GatheringFeature, VitalsResolveCurrentCombatAndRefreshOnlyAfterDurableExecution) {
  HarvestProbe probe;
  probe.context.generation = 2;
  ASSERT_TRUE(probe.start());
  ASSERT_TRUE(probe.session->busy());
  probe.context.generation = 3;
  probe.execute();

  EXPECT_EQ(probe.context.begin_generation, 2);
  EXPECT_EQ(probe.context.spent_generation, 3);
  EXPECT_EQ(probe.fixture.stored_progress().stackables.at(kMaterial), 21);
  EXPECT_EQ(probe.fixture.progress->cached.stackables.at(kMaterial), 20);
  EXPECT_LT(probe.fixture.trace_index("commit"), probe.fixture.trace_index("spend"));
  EXPECT_LT(probe.fixture.trace_index("spend"), probe.fixture.trace_index("combat-cache"));
  EXPECT_EQ(std::ranges::count(probe.fixture.trace, "unexpected-tutorial-adopt"), 0);
}

TEST(GatheringFeature, DueAttackInterruptsBeforeTheGatheringRewardCanCommit) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  ASSERT_TRUE(probe.session->busy());
  const auto before = probe.fixture.stored_progress();
  probe.context.advance_combat = [&](const WorldPosition &, std::int64_t ticks) {
    probe.fixture.trace.push_back("attack");
    auto interrupted = probe.session->interrupt(ticks);
    probe.context.gathering_deliver_observers(interrupted.observers);
    std::vector<GameplayPacket> packets{{19, {{"target_unit_id", "17"},
                                              {"base_damage_amount", 5}}}};
    packets.insert(packets.end(), std::make_move_iterator(interrupted.packets.begin()),
                   std::make_move_iterator(interrupted.packets.end()));
    return packets;
  };

  const auto packets = probe.execute();
  ASSERT_FALSE(packets.empty());
  EXPECT_EQ(packets.front().opcode, 19);
  EXPECT_FALSE(probe.session->busy());
  EXPECT_EQ(probe.fixture.stored_progress(), before);
  EXPECT_TRUE(probe.feature->nodes_for(nullptr)->depleted_on_map(kMap).empty());
  EXPECT_LT(probe.fixture.trace_index("attack"), probe.fixture.trace_index("cancel"));
  EXPECT_FALSE(probe.context.observers.empty());
}

TEST(GatheringFeature, ToolWearCapabilityRunsAfterCommitAndBeforeSharedStatePackets) {
  HarvestProbe probe(true);
  auto progress = std::make_shared<SuccessfulWearProgress>(probe.fixture);
  probe.session = probe.feature->create_session(17, progress, probe.context, *probe.events, 7);
  ASSERT_TRUE(probe.start());
  const auto packets = probe.execute();

  ASSERT_EQ(progress->last_trade.durability_updates.size(), 1U);
  EXPECT_EQ(progress->last_trade.durability_updates.front().expected_durability, 2000);
  EXPECT_EQ(progress->last_trade.durability_updates.front().durability, 1900);
  EXPECT_EQ(probe.context.worn_durability, 1900);
  EXPECT_LT(probe.fixture.trace_index("commit"), probe.fixture.trace_index("tool-worn"));
  ASSERT_EQ(packets.size(), 4U);
  EXPECT_EQ(packets[0].opcode, 169);
  EXPECT_EQ(packets[1].opcode, kProbeToolFollowUp);
  EXPECT_EQ(packets[2].opcode, kGatheringStateChangedNotification);
  EXPECT_EQ(packets[3].opcode, 167);
}

TEST(GatheringFeature, RealRepositoryCommitsToolWearBeforePostCommitEffects) {
  HarvestProbe probe(true);
  const auto before = probe.fixture.stored_progress();
  ASSERT_TRUE(probe.start());
  const auto packets = probe.execute();

  // The repository accepts wear (captured official tool durability 1500 -> 1400) together with
  // the reward and the next progress revision.
  EXPECT_EQ(probe.fixture.stored_progress().revision, before.revision + 1);
  EXPECT_EQ(probe.fixture.stored_progress().stackables.at(kMaterial),
            before.stackables.contains(kMaterial) ? before.stackables.at(kMaterial) + 1 : 1);
  EXPECT_EQ(probe.context.worn_durability, 1900);
  EXPECT_LT(probe.fixture.trace_index("commit"), probe.fixture.trace_index("tool-worn"));
  EXPECT_EQ(probe.broadcasts.size(), 1U);
  ASSERT_FALSE(packets.empty());
  EXPECT_EQ(packets.front().opcode, 169);
  EXPECT_EQ(packets.back().opcode, 167);
}

TEST(GatheringFeature, MapLoadPreservesStateWithoutClaimingTheRequest) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  auto frame = request_frame(20085);
  frame.session_packets.push_back({10, Json::object()});

  EXPECT_FALSE(probe.feature->handle(probe.session.get(), frame, probe.context, probe.broadcasts));
  ASSERT_EQ(frame.session_packets.size(), 1U);
  EXPECT_EQ(frame.session_packets[0].opcode, 10);
  const auto current = probe.session->nodes()->depleted_on_map(kMap);
  const auto snapshot = gathering_state_packets(current, probe.session->nodes(), 0);
  ASSERT_EQ(snapshot.size(), 1U);
  EXPECT_EQ(snapshot.front().opcode, kGatheringStateChangedNotification);
  probe.context.streamed = true;
  auto streamed = request_frame(20085);
  EXPECT_FALSE(
      probe.feature->handle(probe.session.get(), streamed, probe.context, probe.broadcasts));
  EXPECT_TRUE(streamed.session_packets.empty());
}

TEST(GatheringFeature, SectorSnapshotUsesTheSameOwnedDepletedNodeState) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  const std::vector<std::uint32_t> sectors{kSector};
  const auto packets = probe.feature->sector_snapshots(kMap, sectors);

  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets.front().message.at("gathering_deactivated_indices"), Json::array({kIndex}));
  ASSERT_TRUE(packets.front().local_plan);
  EXPECT_EQ(packets.front().local_plan->source_sector(), kSector);
}

TEST(GatheringFeature, SectorSnapshotReadsOnlyTheRequestedSightWindow) {
  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, focused_data());
  const auto nodes = feature->nodes_for(nullptr);
  constexpr std::uint32_t kUnrelatedSector = kSector + 1;
  for (std::uint32_t index = 0; index < 5000; ++index) {
    const GatheringNodeKey node{kMap, GatheringNodeKind::Placement, kUnrelatedSector, index};
    ASSERT_TRUE(nodes->try_deplete(node, 100, 1000));
  }
  const GatheringNodeKey visible{kMap, GatheringNodeKind::Placement, kSector, kIndex};
  ASSERT_TRUE(nodes->try_deplete(visible, 100, 1000));

  const std::array<std::uint32_t, 1> sectors{kSector};
  const auto packets = feature->sector_snapshots(kMap, sectors);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets.front().message.at("gathering_deactivated_indices"), Json::array({kIndex}));

  const std::array<std::uint32_t, 1> oversized_sector{kUnrelatedSector};
  EXPECT_THROW(feature->sector_snapshots(kMap, oversized_sector), std::runtime_error);
  const std::vector<std::uint32_t> oversized_sight(4097, kSector);
  EXPECT_THROW(feature->sector_snapshots(kMap, oversized_sight), std::runtime_error);
}

TEST(GatheringFeature, CliffsMapLoadRestoresSavedProgressForSectorStreaming) {
  HarvestProbe probe;
  auto progress = std::make_shared<RecordingProgress>(probe.fixture.repositories,
                                                       probe.fixture.actor, probe.fixture.trace);
  QuestProgress elite;
  elite.objectives[6] = 1;
  progress->cached.quests.emplace(20001054, elite);
  QuestProgress captain;
  captain.objectives[1] = 1;
  progress->cached.quests.emplace(10000146, captain);
  probe.session = probe.feature->create_session(17, progress, probe.context, *probe.events, 7);
  probe.context.streamed = true;
  auto frame = request_frame(20085);
  frame.gameplay_position->map_id = kCliffsMap;

  EXPECT_FALSE(probe.feature->handle(probe.session.get(), frame, probe.context, probe.broadcasts));
  ASSERT_EQ(probe.broadcasts.size(), 4U);
  ASSERT_EQ(frame.session_packets.size(), 4U);
  const std::vector<std::uint32_t> sectors{170 | (290 << 14)};
  const auto snapshots = probe.feature->sector_snapshots(kCliffsMap, sectors);
  ASSERT_EQ(snapshots.size(), 1U);
  ASSERT_TRUE(snapshots.front().message.contains("doodads"));
  EXPECT_EQ(snapshots.front().message.at("doodads").size(), 1U);
  EXPECT_TRUE(probe.feature->on_monster_defeated(kCliffsMap, 5001104, kNow).empty());
}

TEST(GatheringFeature, QueuedRespawnDoesNotRetainTheWorldFeature) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  std::weak_ptr<GatheringFeature> weak = probe.feature;
  probe.feature->schedule_respawn();
  probe.feature.reset();

  EXPECT_TRUE(weak.expired());
  probe.io.run();
}

TEST(GatheringFeature, ArmedRespawnReleasesItsFeatureAndNeverCallsTheDeadWorldDelivery) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  int deliveries{};
  probe.feature->set_respawn_delivery([&](const std::vector<GatheringStateChange> &, std::uint64_t) {
    ++deliveries;
  });
  probe.feature->schedule_respawn();
  ASSERT_EQ(probe.io.poll_one(), 1U);
  std::weak_ptr<GatheringFeature> weak = probe.feature;
  probe.feature.reset();

  EXPECT_TRUE(weak.expired());
  probe.io.restart();
  probe.io.run();
  EXPECT_EQ(deliveries, 0);
}

std::vector<GatheringStateChange> one_respawned_node() {
  GatheringStateChange change;
  change.active = true;
  return {change};
}

TEST(GatheringFeature, RespawnWithoutDeliveryIsDroppedAndReportedOnce) {
  testing_support::LogCapture log;
  boost::asio::io_context io;
  GatheringFeature feature(io, focused_data());

  feature.deliver_respawn_changes(one_respawned_node());
  feature.deliver_respawn_changes(one_respawned_node());

  EXPECT_EQ(log.count("Gathering respawn update dropped"), 1U);
}

TEST(GatheringFeature, NoRespawnedNodesAreNeverReportedAsDropped) {
  testing_support::LogCapture log;
  boost::asio::io_context io;
  GatheringFeature feature(io, focused_data());

  feature.deliver_respawn_changes({});

  EXPECT_EQ(log.count("dropped"), 0U);
}

TEST(GatheringFeature, InstalledRespawnDeliveryReceivesTheChangesWithoutReports) {
  testing_support::LogCapture log;
  boost::asio::io_context io;
  GatheringFeature feature(io, focused_data());
  std::size_t delivered_changes{};
  std::uint64_t delivered_channel{99};
  feature.set_respawn_delivery(
      [&](const std::vector<GatheringStateChange> &changes, std::uint64_t channel) {
        delivered_changes += changes.size();
        delivered_channel = channel;
      });

  feature.deliver_respawn_changes(one_respawned_node());

  EXPECT_EQ(delivered_changes, 1U);
  EXPECT_EQ(delivered_channel, 0U);
  EXPECT_EQ(log.count("dropped"), 0U);
}

class WorldProbe final : public GatheringWorldContext {
public:
  struct Peer {
    std::uint64_t connection{};
    std::optional<int> map;
    std::uint64_t channel{};
    std::optional<std::set<std::uint32_t>> ready_sectors;
  };

  Frame gathering_encode(const GameplayPacket &packet) override {
    ++encode_count;
    if (fail_once) {
      fail_once = false;
      throw std::runtime_error("Synthetic gathering codec failure");
    }
    return encoded_probe_frame(packet.opcode);
  }

  void gathering_each_peer_locked(std::uint64_t excluded_connection,
                                  const std::function<void(GatheringPeer)> &visitor) override {
    std::lock_guard lock(mutex);
    for (const auto &peer : peers) {
      if (peer.connection == excluded_connection) {
        continue;
      }
      const auto connection = peer.connection;
      visitor({peer.map, peer.channel, [this, connection](Frame frame) {
                 std::lock_guard lock(mutex);
                 if (!frame.shared) {
                   all_shared = false;
                 } else if (!shared_payload) {
                   shared_payload = frame.shared;
                 } else if (shared_payload != frame.shared) {
                   all_shared = false;
                 }
                 posted.emplace_back(connection, frame.opcode());
               },
               [ready_sectors = peer.ready_sectors](std::uint32_t sector) {
                 return !ready_sectors || ready_sectors->contains(sector);
               }});
    }
  }

  mutable world_metrics::TimedMutex mutex;
  std::vector<Peer> peers;
  std::vector<std::pair<std::uint64_t, std::uint16_t>> posted;
  std::shared_ptr<const Bytes> shared_payload;
  std::size_t encode_count{};
  bool all_shared{true};
  bool fail_once{};
};

TEST(GatheringFeature, SharedChangesFilterMapAndSenderAndPostOnlyAfterUnlocking) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  WorldProbe world;
  world.peers = {{1, kMap}, {2, kMap}, {3, kMap + 1}, {4, std::nullopt}, {5, kMap, 7}};

  probe.feature->deliver_changes(probe.broadcasts, 0, 1, world);
  EXPECT_EQ(world.posted, (std::vector<std::pair<std::uint64_t, std::uint16_t>>{
                              {2, kGatheringStateChangedNotification}}));
}

TEST(GatheringFeature, FanoutSharesOneEncodedPayloadAcrossManyRecipients) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  WorldProbe world;
  for (std::uint64_t connection = 1; connection <= 1000; ++connection) {
    world.peers.push_back({connection, kMap, 0});
  }
  probe.feature->deliver_changes(probe.broadcasts, 0, 0, world);
  EXPECT_EQ(world.posted.size(), 1000U);
  EXPECT_EQ(world.encode_count, 1U);
  EXPECT_TRUE(world.all_shared);
}

TEST(GatheringFeature, FanoutOnlySendsChangesForReadySectors) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  ASSERT_FALSE(probe.broadcasts.empty());

  const auto first_sector = probe.broadcasts.front().node.sector_id;
  const auto second_sector = first_sector + 1;
  auto second_change = probe.broadcasts.front();
  second_change.node.sector_id = second_sector;
  auto changes = probe.broadcasts;
  changes.push_back(second_change);

  WorldProbe world;
  world.peers = {{1, kMap, 0, std::set<std::uint32_t>{first_sector}},
                 {2, kMap, 0, std::set<std::uint32_t>{second_sector}},
                 {3, kMap, 0, std::set<std::uint32_t>{}}};

  probe.feature->deliver_changes(changes, 0, 0, world);
  ASSERT_EQ(world.posted.size(), 2U);
  EXPECT_EQ(world.posted[0].first, 1U);
  EXPECT_EQ(world.posted[1].first, 2U);
  EXPECT_EQ(world.encode_count, 2U);
}

TEST(GatheringFeature, OversizedFanoutFailsBeforePostingAnyRecipient) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  WorldProbe world;
  for (std::uint64_t connection = 1; connection <= 65537; ++connection) {
    world.peers.push_back({connection, kMap, 0});
  }
  EXPECT_THROW(probe.feature->deliver_changes(probe.broadcasts, 0, 0, world), std::runtime_error);
  EXPECT_TRUE(world.posted.empty());
  EXPECT_EQ(world.encode_count, 1U);
}

TEST(GatheringFeature, CopyChangesReachOnlyPlayersInTheSameCopy) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  InstanceCopies copies({kMap});
  const auto first = copies.enter(kMap, 1, std::nullopt);
  const auto second = copies.enter(kMap, 2, std::nullopt);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  const auto first_nodes = probe.feature->nodes_for(first.get());
  const auto second_nodes = probe.feature->nodes_for(second.get());
  ASSERT_NE(first_nodes, second_nodes);
  auto changes = probe.broadcasts;
  ASSERT_EQ(changes.size(), 1U);
  std::uint64_t revision{};
  ASSERT_TRUE(first_nodes->try_deplete(changes.front().node, 100, 200, &revision));
  changes.front().revision = revision;
  WorldProbe world;
  world.peers = {{1, kMap, first->serial()}, {2, kMap, second->serial()}, {3, kMap}};

  probe.feature->deliver_changes(changes, first->serial(), 0, world);
  EXPECT_EQ(world.posted, (std::vector<std::pair<std::uint64_t, std::uint16_t>>{
                              {1, kGatheringStateChangedNotification}}));
}

TEST(GatheringFeature, EachCopyOwnsItsNodesAndTheSharedMapKeepsItsOwn) {
  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, focused_data());
  InstanceCopies copies({kMap});
  const auto first = copies.enter(kMap, 1, std::nullopt);
  const auto second = copies.enter(kMap, 2, std::nullopt);

  const auto first_nodes = feature->nodes_for(first.get());
  EXPECT_EQ(first_nodes, feature->nodes_for(first.get()));
  EXPECT_NE(first_nodes, feature->nodes_for(second.get()));
  EXPECT_NE(first_nodes, feature->nodes_for(nullptr));
}

TEST(GatheringFeature, VolumeDoodadPhasesAreRecordedOncePerCopyAndRestoresNeverOverride) {
  boost::asio::io_context io;
  const auto feature = std::make_shared<GatheringFeature>(io, focused_data());
  InstanceCopies copies({kMap});
  const auto copy = copies.enter(kMap, 1, std::nullopt);
  const auto door = doodad_unit_id(4321, 5);
  const std::vector<GatheringFeature::VolumeDoodadPhase> open{{door, 2}};

  const auto opened = feature->record_volume_doodad_phases(kMap, copy.get(), open, 100);
  ASSERT_EQ(opened.size(), 1U);
  EXPECT_EQ(opened.front().node.sector_id, 4321U);
  EXPECT_EQ(opened.front().node.index, 5U);
  EXPECT_EQ(opened.front().phase, 2);
  // Walking in again changes nothing; other copies and the shared map are untouched.
  EXPECT_TRUE(feature->record_volume_doodad_phases(kMap, copy.get(), open, 200).empty());
  EXPECT_TRUE(feature->nodes_for(nullptr)->doodads_on_map(kMap).empty());
  ASSERT_EQ(feature->nodes_for(copy.get())->doodads_on_map(kMap).size(), 1U);

  // A saved phase never overrides the copy's live state, but fills an untouched copy.
  const std::vector<GatheringFeature::VolumeDoodadPhase> saved{{door, 1}};
  EXPECT_TRUE(feature->record_volume_doodad_phases(kMap, copy.get(), saved, 300, true).empty());
  const auto other = copies.enter(kMap, 2, std::nullopt);
  EXPECT_EQ(feature->record_volume_doodad_phases(kMap, other.get(), saved, 300, true).size(), 1U);

  // A non-doodad unit id is ignored.
  const std::vector<GatheringFeature::VolumeDoodadPhase> invalid{{4097, 2}};
  EXPECT_TRUE(feature->record_volume_doodad_phases(kMap, copy.get(), invalid, 400).empty());

  // The sector snapshot of the copy carries the opened door.
  const std::array<std::uint32_t, 1> sectors{4321};
  const auto snapshot = feature->sector_snapshots(kMap, sectors, copy.get());
  ASSERT_EQ(snapshot.size(), 1U);
  ASSERT_TRUE(snapshot.front().message.contains("doodads"));
  EXPECT_EQ(snapshot.front().message.at("doodads").at(0).at("phase"), 2);
}

TEST(GatheringFeature, OneCodecFailureKeepsLaterRecipientsAndFutureDeliveriesAlive) {
  HarvestProbe probe;
  ASSERT_TRUE(probe.start());
  probe.execute();
  WorldProbe world;
  world.peers = {{1, kMap}, {2, kMap}};
  world.fail_once = true;

  probe.feature->deliver_changes(probe.broadcasts, 0, 0, world);
  ASSERT_EQ(world.posted.size(), 1U);
  EXPECT_EQ(world.posted.front().first, 2);
  probe.feature->deliver_changes(probe.broadcasts, 0, 0, world);
  EXPECT_EQ(world.posted.size(), 3U);
}

} // namespace
} // namespace chronicles::gathering
