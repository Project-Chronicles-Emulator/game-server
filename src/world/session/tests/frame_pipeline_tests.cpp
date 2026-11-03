#include "world/session/frame_pipeline.hpp"

#include "world/core/world_request_opcodes.hpp"
#include "world/features/character/achievements.hpp"
#include "world/features/character/progression.hpp"
#include "world/features/companion/companion_session.hpp"
#include "world/features/company/company_world.hpp"
#include "world/features/events/channel_event.hpp"
#include "world/features/events/preorder_bonus.hpp"
#include "world/features/events/raid_channel.hpp"
#include "world/features/events/ranking.hpp"
#include "world/features/expression/player_expression.hpp"
#include "world/features/gathering/gathering.hpp"
#include "world/features/gm/gm_commands.hpp"
#include "world/features/guild/guild_world.hpp"
#include "world/features/housing/housing_world.hpp"
#include "world/features/inventory/utility_bag.hpp"
#include "world/features/item_use/item_use.hpp"
#include "world/features/loot/loot.hpp"
#include "world/features/market/market.hpp"
#include "world/features/market/market_protocol.hpp"
#include "world/features/npc/npc_reactions.hpp"
#include "world/features/nuia/nuia_tears.hpp"
#include "world/features/party/party_world.hpp"
#include "world/features/processing/processing.hpp"
#include "world/features/quests/world_quests.hpp"
#include "world/features/shop/npc_shop.hpp"
#include "world/features/trade/trade_session.hpp"
#include "world/features/travel/map_travel.hpp"
#include "world/session/native_world.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chronicles {
namespace {

constexpr std::uint16_t kMapLoadCompleteOpcode = 20085;
constexpr std::uint16_t kInteractionStartOpcode = 20069;
constexpr std::uint16_t kEscapeOpcode = 20048;
constexpr std::uint16_t kChannelTriggerOpcode = 20082;
constexpr std::uint16_t kTimeSyncOpcode = 20001;
constexpr std::uint16_t kEquipOpcode = 20074;
constexpr std::uint16_t kUnknownOpcode = 65000;

class ScopedFrameDiagnosticLog {
public:
  explicit ScopedFrameDiagnosticLog(spdlog::level::level_enum level)
      : previous_(spdlog::default_logger()) {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output_);
    auto logger = std::make_shared<spdlog::logger>("frame-step-test", std::move(sink));
    logger->set_pattern("%v");
    logger->set_level(level);
    spdlog::set_default_logger(std::move(logger));
  }

  ~ScopedFrameDiagnosticLog() {
    spdlog::set_default_logger(std::move(previous_));
  }

  std::string text() const {
    return output_.str();
  }

private:
  std::ostringstream output_;
  std::shared_ptr<spdlog::logger> previous_;
};

Frame tagged_frame(std::uint16_t opcode) {
  Frame result;
  result.bytes = {4, 0, static_cast<std::uint8_t>(opcode & 0xff),
                  static_cast<std::uint8_t>(opcode >> 8)};
  return result;
}

class RecordingOperations final : public SessionFrameOperations {
public:
  using Action = std::function<void(SessionFrameContext &)>;
  using Handler = std::function<bool(SessionFrameContext &)>;

  PreparedFrameInput prepare_frame(SessionFrameState &state) override {
    record("prepare");
    if (prepare_action) {
      return prepare_action(state);
    }

    PreparedFrameInput result;
    if (has_request) {
      result.request = WorldGameplayRequest{request_opcode, 17, request_message};
    }
    result.gameplay_position = WorldPosition{10117, 10, 20, 30, 0};
    result.party_position = result.gameplay_position;
    return result;
  }

  void observe_movement_mode(SessionFrameContext &frame) override {
    observe("movement_mode", frame);
  }

  void observe_map_load_reset(SessionFrameContext &frame) override {
    observe("map_load_reset", frame);
  }

  void observe_frame_metrics(SessionFrameContext &frame) override {
    observe("frame_metrics", frame);
  }

  void observe_patrol_position(SessionFrameContext &frame) override {
    observe("patrol_position", frame);
  }

  void observe_map_load_vitals(SessionFrameContext &frame) override {
    observe("map_load_vitals", frame);
  }

  void observe_shared_map_mobs(SessionFrameContext &frame) override {
    observe("shared_map_mobs", frame);
  }

  void observe_movement_vitals(SessionFrameContext &frame) override {
    observe("movement_vitals", frame);
  }

  void observe_server_map_completion(SessionFrameContext &frame) override {
    observe("server_map_completion", frame);
  }

  void observe_raid_arrival(SessionFrameContext &frame) override {
    observe("raid_arrival", frame);
  }

  void observe_companion_own_packets(SessionFrameContext &frame) override {
    observe("companion_own_packets", frame);
  }

  void observe_tear_snapshot(SessionFrameContext &frame) override {
    observe("tear_snapshot", frame);
  }

  void observe_adventure_lifecycle(SessionFrameContext &frame) override {
    observe("adventure_lifecycle", frame);
  }

  void observe_quest_snapshot(SessionFrameContext &frame) override {
    observe("quest_snapshot", frame);
  }

  void observe_utility_bag_snapshot(SessionFrameContext &frame) override {
    observe("utility_bag_snapshot", frame);
  }

  void observe_shared_population(SessionFrameContext &frame) override {
    observe("shared_population", frame);
  }

  void observe_sector_sight(SessionFrameContext &frame) override {
    observe("sector_sight", frame);
  }

  void observe_npc_sight(SessionFrameContext &frame) override {
    observe("npc_sight", frame);
  }

  void observe_unhandled_interaction(SessionFrameContext &frame) override {
    observe("unhandled_interaction", frame);
  }

  void observe_start_playing_patrol(SessionFrameContext &frame) override {
    observe("start_playing_patrol", frame);
  }

  void observe_encumbrance(SessionFrameContext &frame) override {
    observe("encumbrance", frame);
  }

  void observe_session_encoding(SessionFrameContext &frame) override {
    observe("session_encoding", frame);
  }

  void observe_riding_vitals_reset(SessionFrameContext &frame) override {
    observe("riding_vitals_reset", frame);
  }

  void observe_final_deadline(SessionFrameContext &frame) override {
    observe("final_deadline", frame);
  }

  bool handle_shared_loot(SessionFrameContext &frame) override {
    return handle("shared_loot", frame);
  }

  bool handle_shared_revival(SessionFrameContext &frame) override {
    return handle("shared_revival", frame);
  }

  bool handle_party(SessionFrameContext &frame) override {
    return handle("party", frame);
  }

  bool handle_guild(SessionFrameContext &frame) override {
    return handle("guild", frame);
  }

  bool handle_housing_request(SessionFrameContext &frame) override {
    return handle("housing_request", frame);
  }

  bool handle_housing_toggle(SessionFrameContext &frame) override {
    return handle("housing_toggle", frame);
  }

  bool handle_housing_departure(SessionFrameContext &frame) override {
    return handle("housing_departure", frame);
  }

  bool handle_zone_channel_move(SessionFrameContext &frame) override {
    return handle("zone_channel_move", frame);
  }

  bool handle_map_travel(SessionFrameContext &frame) override {
    return handle("map_travel", frame);
  }

  bool handle_market(SessionFrameContext &frame) override {
    return handle("market", frame);
  }

  bool handle_events(SessionFrameContext &frame) override {
    return handle("events", frame);
  }

  bool handle_world_chat(SessionFrameContext &frame) override {
    return handle("world_chat", frame);
  }

  bool handle_companion_trade(SessionFrameContext &frame) override {
    return handle("companion_trade", frame);
  }

  bool handle_statue(SessionFrameContext &frame) override {
    return handle("statue", frame);
  }

  bool handle_statue_busy_skill(SessionFrameContext &frame) override {
    return handle("statue_busy_skill", frame);
  }

  bool handle_item_use(SessionFrameContext &frame) override {
    return handle("item_use", frame);
  }

  bool handle_company(SessionFrameContext &frame) override {
    return handle("company", frame);
  }

  bool handle_social_gm(SessionFrameContext &frame) override {
    return handle("social_gm", frame);
  }

  bool handle_generic_gm(SessionFrameContext &frame) override {
    return handle("generic_gm", frame);
  }

  bool handle_equip(SessionFrameContext &frame) override {
    return handle("equip", frame);
  }

  bool handle_discard(SessionFrameContext &frame) override {
    return handle("discard", frame);
  }

  bool handle_polish(SessionFrameContext &frame) override {
    return handle("polish", frame);
  }

  bool handle_progression(SessionFrameContext &frame) override {
    return handle("progression", frame);
  }

  bool handle_escape(SessionFrameContext &frame) override {
    return handle("escape", frame);
  }

  bool handle_expression(SessionFrameContext &frame) override {
    return handle("expression", frame);
  }

  bool handle_npc_threat(SessionFrameContext &frame) override {
    return handle("npc_threat", frame);
  }

  bool handle_merchant(SessionFrameContext &frame) override {
    return handle("merchant", frame);
  }

  bool handle_processing(SessionFrameContext &frame) override {
    return handle("processing", frame);
  }

  bool handle_gathering(SessionFrameContext &frame) override {
    return handle("gathering", frame);
  }

  bool handle_tears(SessionFrameContext &frame) override {
    return handle("tears", frame);
  }

  bool handle_adventure(SessionFrameContext &frame) override {
    return handle("adventure", frame);
  }

  bool handle_utility_bag(SessionFrameContext &frame) override {
    return handle("utility_bag", frame);
  }

  bool handle_world_quests(SessionFrameContext &frame) override {
    return handle("world_quests", frame);
  }

  bool handle_private_combat(SessionFrameContext &frame) override {
    return handle("private_combat", frame);
  }

  bool handle_tutorial_tail(SessionFrameContext &frame) override {
    return handle("tutorial_tail", frame);
  }

  void post_movement_presence(SessionFrameContext &frame) override {
    observe("post_movement_presence", frame);
  }

  void post_companion_entry(SessionFrameContext &frame) override {
    observe("post_companion_entry", frame);
  }

  void post_companion_delivery(SessionFrameContext &frame) override {
    observe("post_companion_delivery", frame);
  }

  void post_gathering_changes(SessionFrameContext &frame) override {
    observe("post_gathering_changes", frame);
  }

  void post_player_services(SessionFrameContext &frame) override {
    observe("post_player_services", frame);
  }

  void post_party_update(SessionFrameContext &frame) override {
    observe("post_party_update", frame);
  }

  void post_raid_update(SessionFrameContext &frame) override {
    observe("post_raid_update", frame);
  }

  void post_emotion_snapshot(SessionFrameContext &frame) override {
    observe("post_emotion_snapshot", frame);
  }

  void post_guild_update(SessionFrameContext &frame) override {
    observe("post_guild_update", frame);
  }

  void post_housing_snapshot(SessionFrameContext &frame) override {
    observe("post_housing_snapshot", frame);
  }

  void post_progress_checkpoint(SessionFrameContext &frame) override {
    observe("post_progress_checkpoint", frame);
  }

  void report_frame_rejection(std::uint16_t incoming_opcode, std::string_view step,
                              const std::exception &error) override {
    record("rejected");
    rejected_opcode = incoming_opcode;
    rejected_step = step;
    rejected_error = error.what();
    ++rejection_count;
  }

  void log_frame_result(std::uint16_t incoming_opcode, const SessionFrameState &state,
                        bool rejected) override {
    record("logged");
    logged_opcode = incoming_opcode;
    logged_rejected = rejected;
    logged_notifications = state.pending_notifications.size();
  }

  void encode_session_packets() {
    actions["session_encoding"] = [](SessionFrameContext &frame) {
      for (const auto &packet : frame.session_packets) {
        frame.state.pending_notifications.push_back(tagged_frame(packet.opcode));
      }
    };
  }

  bool called(std::string_view name) const {
    return std::ranges::find(trace, name) != trace.end();
  }

  std::size_t position(std::string_view name) const {
    const auto found = std::ranges::find(trace, name);
    if (found == trace.end()) {
      throw std::runtime_error("Operation missing from trace: " + std::string(name));
    }
    return static_cast<std::size_t>(found - trace.begin());
  }

  std::uint16_t request_opcode{kUnknownOpcode};
  nlohmann::json request_message = nlohmann::json::object();
  bool has_request{true};
  bool record_trace{true};
  std::function<PreparedFrameInput(SessionFrameState &)> prepare_action;
  std::unordered_map<std::string_view, Action> actions;
  std::unordered_map<std::string_view, Handler> handlers;
  std::unordered_set<std::string_view> visited_handlers;
  std::vector<std::string> trace;
  unsigned rejection_count{};
  std::uint16_t rejected_opcode{};
  std::string rejected_step;
  std::string rejected_error;
  std::uint16_t logged_opcode{};
  bool logged_rejected{};
  std::size_t logged_notifications{};

private:
  void record(std::string_view name) {
    if (record_trace) {
      trace.emplace_back(name);
    }
  }

  void observe(std::string_view name, SessionFrameContext &frame) {
    record(name);
    const auto action = actions.find(name);
    if (action != actions.end()) {
      action->second(frame);
    }
  }

  bool handle(std::string_view name, SessionFrameContext &frame) {
    record(name);
    visited_handlers.insert(name);
    const auto action = handlers.find(name);
    return action != handlers.end() && action->second(frame);
  }
};

struct OwnedRoute {
  std::string_view handler;
  std::vector<std::uint16_t> opcodes;
};

// The owners below keep their ownership predicates inside stateful sessions (catalogs, open
// actions, pending requests), so a session-free test cannot call them. Each route is therefore
// built from the opcode constants the owning feature publishes, never from the pipeline's own
// arrays. A handler owns an opcode only when the owner can accept it in some session state.
std::vector<OwnedRoute> routes_published_by_owners() {
  std::vector<std::uint16_t> companion_and_trade{kGmCommandRequestOpcode,
                                                 world_request::kInteractionStartRequest,
                                                 kProcessingExecuteInteractionRequest,
                                                 kProcessingEndInteractionRequest,
                                                 kGmCommandTextRequestOpcode,
                                                 companion::kSpawnMateNpcRequest,
                                                 companion::kDespawnMateNpcRequest,
                                                 companion::kInviteMateNpcRequest,
                                                 companion::kSpawnSummonNpcRequest,
                                                 companion::kRideUnitRequest,
                                                 companion::kGetOffUnitRequest,
                                                 companion::kInteractUnitRequest,
                                                 companion::kItemUseRequest,
                                                 trade::kEscapeRequest,
                                                 trade::kDialogueStartRequest,
                                                 trade::kDialogueEndRequest,
                                                 trade::kDialogueFunctionRequest,
                                                 trade::kTradeCancelRequest,
                                                 trade::kTradeMarketPriceRequest,
                                                 trade::kTradeGoodLoadingRequest,
                                                 trade::kTradeGoodSellRequest,
                                                 trade::kTradingPostWagonParkingRequest,
                                                 trade::kTradingPostWagonRemoveRequest};

  return {
      {"shared_loot", {kLootItemRequestOpcode}},
      {"shared_revival", {world_request::kReviveRequest}},
      {"housing_toggle",
       {world_request::kInteractionStartRequest, world_request::kInteractionExecuteRequest,
        world_request::kInteractionEndRequest}},
      {"housing_departure",
       {world_request::kZoneChannelMoveRequest, world_request::kEscapeRequest}},
      {"zone_channel_move", {world_request::kZoneChannelMoveRequest}},
      {"map_travel",
       {travel::kLevelTriggerRequest, travel::kCutSceneEndRequest,
        channel_event::kChannelTriggerEntryRequest, world_request::kInteractionUnitRequest}},
      {"world_chat", {world_request::kWorldChatRequest}},
      {"companion_trade", std::move(companion_and_trade)},
      {"statue_busy_skill", {world_request::kStartSkillRequest}},
      {"item_use", {kItemUseRequestOpcode}},
      {"company", {company_world::kDialogueFunctionRequest}},
      {"equip", {world_request::kEquipRequest, world_request::kWeaponSlotActivationRequest}},
      {"discard", {world_request::kDiscardRequest}},
      {"polish", {world_request::kPolishRequest}},
      {"progression",
       {kChangePerkRequest, kResetPerkRequest, kUpdateStatBonusRequest,
        character::kUpdateAchievementRequest}},
      {"npc_threat", {npc::kThreatenedNpcRequest}},
      {"merchant", {kInteractUnitRequestOpcode}},
      {"processing",
       {kProcessingDialogueStartRequest, kProcessingDialogueEndRequest,
        kProcessingStartInteractionRequest, kProcessingExecuteInteractionRequest,
        kProcessingEndInteractionRequest}},
      // The map-load entry is claimed only to append the depleted-node snapshot, then declines.
      {"gathering",
       {kGatheringStartInteractionRequest, kGatheringExecuteInteractionRequest,
        kGatheringEndInteractionRequest, kGatheringInteractUnitRequest,
        world_request::kMapLoadCompleteRequest}},
      {"utility_bag", {kUtilityBagSetRequestOpcode}},
      {"adventure",
       {world_request::kInteractionStartRequest, world_request::kInteractionUnitRequest,
        world_request::kDiscoverAdventureObjectRequest, kProcessingExecuteInteractionRequest,
        kProcessingEndInteractionRequest}},
      {"private_combat",
       {world_request::kStartRootMotionRequest, world_request::kAdaptedRotationRequest,
        world_request::kStartGlidingRequest, world_request::kStartSkillRequest,
        world_request::kSkillEffectReportRequest, world_request::kExecuteSpawnProjectileRequest,
        world_request::kChangeSkillStageRequest, world_request::kClientProjectileHitRequest,
        world_request::kDespawnClientProjectileRequest, world_request::kUpdateProjectileRequest}}};
}

// Handlers whose guard is "any opcode": the lazy statue and escape sessions and the tutorial
// tail. They must see every request, including ones no other handler owns.
bool sees_every_opcode(std::string_view handler) {
  return handler == "statue" || handler == "escape" || handler == "tutorial_tail";
}

// Every operation the pipeline composes, in the order the original frame chain ran them.
// This list is the single literal copy of that order: one test compares it with the composed
// step names, and the dispatch tests derive their expected traces from it.
struct ComposedStep {
  std::string_view operation;
  bool observer;
};

constexpr std::array kComposedSteps{
    ComposedStep{"movement_mode", true},
    ComposedStep{"map_load_reset", true},
    ComposedStep{"frame_metrics", true},
    ComposedStep{"patrol_position", true},
    ComposedStep{"map_load_vitals", true},
    ComposedStep{"shared_map_mobs", true},
    ComposedStep{"shared_loot", false},
    ComposedStep{"movement_vitals", true},
    ComposedStep{"shared_revival", false},
    ComposedStep{"party", false},
    ComposedStep{"guild", false},
    ComposedStep{"housing_request", false},
    ComposedStep{"housing_toggle", false},
    ComposedStep{"housing_departure", false},
    ComposedStep{"zone_channel_move", false},
    ComposedStep{"map_travel", false},
    ComposedStep{"server_map_completion", true},
    ComposedStep{"raid_arrival", true},
    ComposedStep{"market", false},
    ComposedStep{"events", false},
    ComposedStep{"world_chat", false},
    ComposedStep{"companion_trade", false},
    ComposedStep{"companion_own_packets", true},
    ComposedStep{"statue", false},
    ComposedStep{"statue_busy_skill", false},
    ComposedStep{"item_use", false},
    ComposedStep{"company", false},
    ComposedStep{"social_gm", false},
    ComposedStep{"generic_gm", false},
    ComposedStep{"equip", false},
    ComposedStep{"discard", false},
    ComposedStep{"polish", false},
    ComposedStep{"progression", false},
    ComposedStep{"escape", false},
    ComposedStep{"expression", false},
    ComposedStep{"npc_threat", false},
    ComposedStep{"merchant", false},
    ComposedStep{"processing", false},
    ComposedStep{"gathering", false},
    ComposedStep{"adventure_lifecycle", true},
    ComposedStep{"adventure", false},
    ComposedStep{"tears", false},
    ComposedStep{"tear_snapshot", true},
    ComposedStep{"quest_snapshot", true},
    ComposedStep{"utility_bag_snapshot", true},
    ComposedStep{"utility_bag", false},
    ComposedStep{"world_quests", false},
    ComposedStep{"shared_population", true},
    ComposedStep{"sector_sight", true},
    ComposedStep{"npc_sight", true},
    ComposedStep{"private_combat", false},
    ComposedStep{"unhandled_interaction", true},
    ComposedStep{"tutorial_tail", false},
    ComposedStep{"start_playing_patrol", true},
    ComposedStep{"encumbrance", true},
    ComposedStep{"session_encoding", true},
    ComposedStep{"riding_vitals_reset", true},
    ComposedStep{"final_deadline", true},
};

constexpr std::array kPostludeOperations{
    "post_movement_presence", "post_companion_entry",     "post_companion_delivery",
    "post_gathering_changes", "post_player_services",     "post_party_update",
    "post_raid_update",       "post_emotion_snapshot",    "post_guild_update",
    "post_housing_snapshot",  "post_progress_checkpoint",
};

// Names of the handlers (never the all-opcode ones) that own the opcode, taken from the public
// predicates where they exist and otherwise from the constants each owner publishes.
std::unordered_set<std::string_view> handlers_owning(std::uint16_t opcode,
                                                     const std::vector<OwnedRoute> &owner_routes) {
  std::unordered_set<std::string_view> owners;
  const auto claim_if = [&owners](bool owned, std::string_view handler) {
    if (owned) {
      owners.insert(handler);
    }
  };

  claim_if(party::PartyWorld::handles(opcode), "party");
  claim_if(GuildWorld::handles(opcode), "guild");
  claim_if(housing::HousingHub::handles(opcode), "housing_request");
  claim_if(is_market_request(opcode), "market");
  claim_if(PlayerExpression::handles(opcode), "expression");
  claim_if(NuiaTears::handles(opcode), "tears");
  claim_if(WorldQuestBook::handles(opcode), "world_quests");
  claim_if(is_gm_command_opcode(opcode), "social_gm");
  claim_if(is_gm_command_opcode(opcode), "generic_gm");
  claim_if(raid::RaidMatchmaker::handles(opcode) || opcode == raid::kRaidChannelMoveRequest ||
               opcode == preorder_bonus::kStateRequest || opcode == preorder_bonus::kClaimRequest ||
               opcode == ranking::kRankingListRequest ||
               opcode == channel_event::kTributeCountRequest ||
               opcode == channel_event::kItemTributeRequest ||
               opcode == channel_event::kChannelTriggerEntryRequest,
           "events");

  for (const auto &route : owner_routes) {
    claim_if(std::ranges::find(route.opcodes, opcode) != route.opcodes.end(), route.handler);
  }

  return owners;
}

TEST(FramePipeline, DeclaredRoutesMatchPublicOwnershipPredicatesAcrossTheWireRange) {
  RecordingOperations operations;
  operations.record_trace = false;
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;
  const auto owner_routes = routes_published_by_owners();

  // These predicates come from the existing feature implementations, independently of the
  // pipeline's registration arrays. Every handler declines so no earlier route hides one.
  for (std::uint32_t value = 0; value <= std::numeric_limits<std::uint16_t>::max(); ++value) {
    const auto opcode = static_cast<std::uint16_t>(value);
    operations.request_opcode = opcode;
    operations.visited_handlers.clear();
    const auto result = pipeline.run(opcode, metadata);
    ASSERT_FALSE(result.rejected) << value;

    const auto owners = handlers_owning(opcode, owner_routes);
    for (const auto &step : kComposedSteps) {
      if (step.observer) {
        continue;
      }

      const bool expected = sees_every_opcode(step.operation) || owners.contains(step.operation);
      ASSERT_EQ(operations.visited_handlers.contains(step.operation), expected)
          << step.operation << " " << value;
    }
  }
}
TEST(FramePipeline, SharedRequestIdsKeepTheirWireValuesAndEveryOwnerAliasAgrees) {
  EXPECT_EQ(world_request::kStartSkillRequest, 20010);
  EXPECT_EQ(world_request::kSkillEffectReportRequest, 20011);
  EXPECT_EQ(world_request::kExecuteSpawnProjectileRequest, 20012);
  EXPECT_EQ(world_request::kChangeSkillStageRequest, 20013);
  EXPECT_EQ(world_request::kAcceptQuestRequest, 20016);
  EXPECT_EQ(world_request::kClaimQuestRewardRequest, 20018);
  EXPECT_EQ(world_request::kProgressQuestRequest, 20019);
  EXPECT_EQ(world_request::kClientProjectileHitRequest, 20021);
  EXPECT_EQ(world_request::kDespawnClientProjectileRequest, 20022);
  EXPECT_EQ(world_request::kUpdateProjectileRequest, 20023);
  EXPECT_EQ(world_request::kWorldChatRequest, 20038);
  EXPECT_EQ(world_request::kReviveRequest, 20039);
  EXPECT_EQ(world_request::kEscapeRequest, 20048);
  EXPECT_EQ(world_request::kActivateAdventureObjectRequest, 20049);
  EXPECT_EQ(world_request::kDiscoverAdventureObjectRequest, 20050);
  EXPECT_EQ(world_request::kInteractionUnitRequest, 20068);
  EXPECT_EQ(world_request::kInteractionStartRequest, 20069);
  EXPECT_EQ(world_request::kDiscardRequest, 20072);
  EXPECT_EQ(world_request::kEquipRequest, 20074);
  EXPECT_EQ(world_request::kPolishRequest, 20075);
  EXPECT_EQ(world_request::kMapLoadCompleteRequest, 20085);
  EXPECT_EQ(world_request::kZoneChannelMoveRequest, 20090);

  // Owner constants that other modules read must stay the same wire IDs.
  EXPECT_EQ(companion::kInteractUnitRequest, world_request::kInteractionUnitRequest);
  EXPECT_EQ(kInteractUnitRequestOpcode, world_request::kInteractionUnitRequest);
  EXPECT_EQ(trade::kEscapeRequest, world_request::kEscapeRequest);
  EXPECT_EQ(kGatheringStartInteractionRequest, world_request::kInteractionStartRequest);
  EXPECT_EQ(kProcessingStartInteractionRequest, world_request::kInteractionStartRequest);
  EXPECT_EQ(world_request::kInteractionEndRequest, 20071);
  EXPECT_EQ(world_request::kInteractionExecuteRequest, 20070);
  EXPECT_EQ(kGatheringExecuteInteractionRequest, world_request::kInteractionExecuteRequest);
  EXPECT_EQ(kGatheringEndInteractionRequest, world_request::kInteractionEndRequest);
  EXPECT_EQ(kProcessingEndInteractionRequest, world_request::kInteractionEndRequest);
}

TEST(FramePipeline, DeclinedHousingToggleReachesStatueBeforeProcessingAndGathering) {
  RecordingOperations operations;
  operations.request_opcode = kInteractionStartOpcode;
  operations.handlers["housing_toggle"] = [](SessionFrameContext &) { return false; };
  operations.handlers["statue"] = [](SessionFrameContext &frame) {
    frame.session_packets.push_back({163, {{"owner", "statue"}}});
    return true;
  };
  operations.actions["sector_sight"] = [](SessionFrameContext &frame) {
    EXPECT_EQ(frame.request(), nullptr);
    frame.session_packets.push_back({206, {{"sector", true}}});
  };
  operations.encode_session_packets();
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kInteractionStartOpcode, metadata);

  EXPECT_FALSE(result.rejected);
  EXPECT_LT(operations.position("housing_toggle"), operations.position("statue"));
  EXPECT_FALSE(operations.called("processing"));
  EXPECT_FALSE(operations.called("gathering"));
  EXPECT_TRUE(operations.called("sector_sight"));
  ASSERT_EQ(result.pending_notifications.size(), 2);
  EXPECT_EQ(result.pending_notifications[0].opcode(), 163);
  EXPECT_EQ(result.pending_notifications[1].opcode(), 206);
}

TEST(FramePipeline, AcceptedHousingDeparturePreventsTradeAndGenericEscape) {
  RecordingOperations operations;
  operations.request_opcode = kEscapeOpcode;
  operations.handlers["housing_departure"] = [](SessionFrameContext &) { return true; };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kEscapeOpcode, metadata);

  EXPECT_FALSE(result.rejected);
  EXPECT_TRUE(operations.called("housing_departure"));
  EXPECT_FALSE(operations.called("companion_trade"));
  EXPECT_FALSE(operations.called("escape"));
  EXPECT_TRUE(operations.called("companion_own_packets"));
}

TEST(FramePipeline, TradingEscapeCanClaimADeclinedHousingDepartureBeforeGenericEscape) {
  RecordingOperations operations;
  operations.request_opcode = kEscapeOpcode;
  operations.request_message = {{"is_trading", true}};
  operations.handlers["companion_trade"] = [](SessionFrameContext &frame) {
    return frame.request()->message.value("is_trading", false);
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kEscapeOpcode, metadata);

  EXPECT_FALSE(result.rejected);
  EXPECT_LT(operations.position("housing_departure"), operations.position("companion_trade"));
  EXPECT_FALSE(operations.called("escape"));
}

TEST(FramePipeline, GatheringMapLoadSnapshotDoesNotHidePopulationOrSightObservers) {
  RecordingOperations operations;
  operations.request_opcode = kMapLoadCompleteOpcode;
  operations.handlers["gathering"] = [](SessionFrameContext &frame) {
    frame.session_packets.push_back({206, {{"depleted", true}}});
    return false;
  };
  operations.actions["shared_population"] = [](SessionFrameContext &frame) {
    ASSERT_NE(frame.request(), nullptr);
    frame.session_packets.push_back({10, {{"npc", true}}});
  };
  operations.actions["npc_sight"] = [](SessionFrameContext &frame) {
    EXPECT_NE(frame.request(), nullptr);
  };
  operations.encode_session_packets();
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kMapLoadCompleteOpcode, metadata);

  EXPECT_FALSE(result.rejected);
  EXPECT_LT(operations.position("gathering"), operations.position("shared_population"));
  EXPECT_LT(operations.position("sector_sight"), operations.position("npc_sight"));
  ASSERT_EQ(result.pending_notifications.size(), 2);
  EXPECT_EQ(result.pending_notifications[0].opcode(), 206);
  EXPECT_EQ(result.pending_notifications[1].opcode(), 10);
}

TEST(FramePipeline, UnknownTutorialTrafficCanProduceTimedPacketsWithoutConsumption) {
  RecordingOperations operations;
  operations.handlers["tutorial_tail"] = [](SessionFrameContext &frame) {
    frame.session_packets.push_back({27, {{"timed", true}}});
    frame.action_deadline = 123;
    return false;
  };
  operations.actions["encumbrance"] = [](SessionFrameContext &frame) {
    EXPECT_NE(frame.request(), nullptr);
  };
  operations.actions["final_deadline"] = [](SessionFrameContext &frame) {
    frame.action_deadline = 456;
  };
  operations.encode_session_packets();
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kUnknownOpcode, metadata);

  EXPECT_FALSE(result.rejected);
  EXPECT_EQ(result.action_deadline, 456);
  ASSERT_EQ(result.pending_notifications.size(), 1);
  EXPECT_EQ(result.pending_notifications.front().opcode(), 27);
  EXPECT_LT(operations.position("tutorial_tail"), operations.position("encumbrance"));
}

TEST(FramePipeline, UnrelatedRequestsRetainLazySessionChecksInOriginalOrder) {
  RecordingOperations operations;
  operations.request_opcode = kTimeSyncOpcode;
  unsigned statue_checks{};
  unsigned escape_checks{};
  operations.handlers["statue"] = [&statue_checks](SessionFrameContext &frame) {
    const auto *request = frame.request();
    EXPECT_NE(request, nullptr);
    if (request) {
      EXPECT_EQ(request->opcode, kTimeSyncOpcode);
    }
    ++statue_checks;
    return false;
  };
  operations.handlers["escape"] = [&escape_checks](SessionFrameContext &frame) {
    const auto *request = frame.request();
    EXPECT_NE(request, nullptr);
    if (request) {
      EXPECT_EQ(request->opcode, kTimeSyncOpcode);
    }
    ++escape_checks;
    return false;
  };
  operations.actions["tear_snapshot"] = [](SessionFrameContext &frame) {
    EXPECT_NE(frame.request(), nullptr);
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kTimeSyncOpcode, metadata);

  EXPECT_FALSE(result.rejected);
  EXPECT_EQ(statue_checks, 1);
  EXPECT_EQ(escape_checks, 1);
  EXPECT_LT(operations.position("companion_own_packets"), operations.position("statue"));
  EXPECT_LT(operations.position("statue"), operations.position("escape"));
  EXPECT_LT(operations.position("escape"), operations.position("tear_snapshot"));
  EXPECT_LT(operations.position("tear_snapshot"), operations.position("tutorial_tail"));
}

TEST(FramePipeline, LazySessionFailureOnUnrelatedRequestKeepsRejectionAttribution) {
  for (const std::string_view step : {"statue", "escape"}) {
    SCOPED_TRACE(step);
    RecordingOperations operations;
    operations.request_opcode = kTimeSyncOpcode;
    operations.handlers[step] = [](SessionFrameContext &) -> bool {
      throw std::invalid_argument("synthetic lazy session construction failure");
    };
    SessionFramePipeline pipeline(operations);
    WorldPacketMetadata metadata;

    const auto result = pipeline.run(kTimeSyncOpcode, metadata);

    EXPECT_TRUE(result.rejected);
    EXPECT_EQ(operations.rejection_count, 1);
    EXPECT_EQ(operations.rejected_opcode, kTimeSyncOpcode);
    EXPECT_EQ(operations.rejected_step, step);
    EXPECT_EQ(operations.rejected_error, "synthetic lazy session construction failure");
    EXPECT_FALSE(operations.called("post_movement_presence"));
    EXPECT_FALSE(operations.called("post_progress_checkpoint"));
    EXPECT_TRUE(operations.logged_rejected);
  }
}

TEST(FramePipeline, MovementInvalidationDoesNotRequireConsumption) {
  RecordingOperations operations;
  operations.request_opcode = kChannelTriggerOpcode;
  operations.handlers["events"] = [](SessionFrameContext &frame) {
    frame.invalidate_positions();
    return false;
  };
  operations.handlers["tutorial_tail"] = [](SessionFrameContext &frame) {
    EXPECT_FALSE(frame.gameplay_position);
    EXPECT_FALSE(frame.party_position);
    EXPECT_NE(frame.request(), nullptr);
    return false;
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  EXPECT_FALSE(pipeline.run(kChannelTriggerOpcode, metadata).rejected);
}

TEST(FramePipeline, EmptyGameplayRequestStillRunsFrameObserversAndDeliveryPostlude) {
  RecordingOperations operations;
  operations.has_request = false;
  operations.actions["sector_sight"] = [](SessionFrameContext &frame) {
    EXPECT_EQ(frame.request(), nullptr);
    frame.state.pending_notifications.push_back(tagged_frame(206));
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kTimeSyncOpcode, metadata);

  EXPECT_FALSE(result.rejected);
  EXPECT_TRUE(operations.visited_handlers.empty());
  EXPECT_TRUE(operations.called("post_progress_checkpoint"));
  ASSERT_EQ(result.pending_notifications.size(), 1);
  EXPECT_EQ(result.pending_notifications.front().opcode(), 206);
}

TEST(FramePipeline, DebugTimingIdentifiesSlowPrepareHandlerObserverAndPostludeWithoutPayload) {
  ScopedFrameDiagnosticLog log(spdlog::level::debug);
  RecordingOperations operations;
  operations.request_opcode = kEquipOpcode;
  operations.prepare_action = [](SessionFrameState &) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    PreparedFrameInput prepared;
    prepared.request = WorldGameplayRequest{
        kEquipOpcode, 17, {{"private-request-value", "must-not-appear-in-diagnostics"}}};
    return prepared;
  };
  operations.actions["movement_mode"] = [](SessionFrameContext &) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  };
  operations.handlers["equip"] = [](SessionFrameContext &frame) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_EQ(frame.request()->message.at("private-request-value"),
              "must-not-appear-in-diagnostics");
    frame.state.pending_notifications.push_back(tagged_frame(206));
    return true;
  };
  operations.actions["post_progress_checkpoint"] = [](SessionFrameContext &) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kEquipOpcode, metadata);

  EXPECT_FALSE(result.rejected);
  ASSERT_EQ(result.pending_notifications.size(), 1);
  EXPECT_EQ(result.pending_notifications.front().opcode(), 206);
  const auto diagnostic = log.text();
  for (const auto step : {"prepare-frame", "movement-mode", "equip", "post-progress-checkpoint"}) {
    EXPECT_NE(
        diagnostic.find(std::string("Slow world frame opcode=20074 step=") + step + " elapsed_ms="),
        std::string::npos);
  }
  EXPECT_EQ(diagnostic.find("private-request-value"), std::string::npos);
  EXPECT_EQ(diagnostic.find("must-not-appear-in-diagnostics"), std::string::npos);
}

TEST(FramePipeline, SlowStepTimingPreservesRejectionAndIsDisabledAtInfoLevel) {
  RecordingOperations operations;
  operations.request_opcode = kEquipOpcode;
  operations.handlers["equip"] = [](SessionFrameContext &) -> bool {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    throw std::runtime_error("private-failure-context");
  };
  SessionFramePipeline pipeline(operations);

  for (const auto level : {spdlog::level::info, spdlog::level::debug}) {
    ScopedFrameDiagnosticLog log(level);
    WorldPacketMetadata metadata;

    const auto result = pipeline.run(kEquipOpcode, metadata);

    EXPECT_TRUE(result.rejected);
    EXPECT_EQ(operations.rejected_step, "equip");
    EXPECT_EQ(operations.rejected_error, "private-failure-context");
    EXPECT_FALSE(operations.called("post_progress_checkpoint"));
    if (level == spdlog::level::info) {
      EXPECT_TRUE(log.text().empty());
    } else {
      EXPECT_NE(log.text().find("opcode=20074 step=equip elapsed_ms="), std::string::npos);
      EXPECT_EQ(log.text().find("private-failure-context"), std::string::npos);
    }
  }
}

TEST(FramePipeline, MainFailureIsAttributedAndSkipsAllDeliveryPostlude) {
  RecordingOperations operations;
  operations.request_opcode = kEquipOpcode;
  operations.handlers["equip"] = [](SessionFrameContext &) -> bool {
    throw std::runtime_error("synthetic commit failure");
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kEquipOpcode, metadata);

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(operations.rejection_count, 1);
  EXPECT_EQ(operations.rejected_opcode, kEquipOpcode);
  EXPECT_EQ(operations.rejected_step, "equip");
  EXPECT_EQ(operations.rejected_error, "synthetic commit failure");
  EXPECT_FALSE(operations.called("post_movement_presence"));
  EXPECT_FALSE(operations.called("post_progress_checkpoint"));
  EXPECT_TRUE(operations.logged_rejected);
  EXPECT_TRUE(operations.called("logged"));
}

TEST(FramePipeline, BootstrapFailureIsRejectedBeforeContextConstruction) {
  RecordingOperations operations;
  operations.prepare_action = [](SessionFrameState &) -> PreparedFrameInput {
    throw std::runtime_error("synthetic decode failure");
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kUnknownOpcode, metadata);

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(operations.rejected_step, "handshake");
  EXPECT_FALSE(operations.called("movement_mode"));
  EXPECT_FALSE(operations.called("post_movement_presence"));
  EXPECT_TRUE(operations.called("logged"));
}

TEST(FramePipeline, UncaughtPostludeFailureIsNotBroadenedIntoFrameRejection) {
  RecordingOperations operations;
  operations.actions["post_party_update"] = [](SessionFrameContext &) {
    throw std::runtime_error("synthetic uncaught delivery failure");
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  EXPECT_THROW(pipeline.run(kUnknownOpcode, metadata), std::runtime_error);
  EXPECT_TRUE(operations.called("post_companion_delivery"));
  EXPECT_FALSE(operations.called("post_raid_update"));
  EXPECT_EQ(operations.rejection_count, 0);
  EXPECT_FALSE(operations.called("logged"));
}

TEST(FramePipeline, LocalPostludeRecoveryCanContinueThroughLaterSnapshots) {
  RecordingOperations operations;
  operations.actions["post_party_update"] = [](SessionFrameContext &frame) {
    try {
      throw std::runtime_error("synthetic recovered presence failure");
    } catch (const std::runtime_error &) {
      frame.state.pending_notifications.push_back(tagged_frame(184));
    }
  };
  operations.actions["post_emotion_snapshot"] = [](SessionFrameContext &frame) {
    frame.state.pending_notifications.push_back(tagged_frame(132));
  };
  operations.actions["post_guild_update"] = [](SessionFrameContext &frame) {
    frame.state.pending_notifications.push_back(tagged_frame(137));
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kUnknownOpcode, metadata);

  EXPECT_FALSE(result.rejected);
  EXPECT_EQ(operations.rejection_count, 0);
  EXPECT_LT(operations.position("post_raid_update"), operations.position("post_emotion_snapshot"));
  EXPECT_LT(operations.position("post_emotion_snapshot"), operations.position("post_guild_update"));
  EXPECT_TRUE(operations.called("post_progress_checkpoint"));
  ASSERT_EQ(result.pending_notifications.size(), 3);
  EXPECT_EQ(result.pending_notifications[0].opcode(), 184);
  EXPECT_EQ(result.pending_notifications[1].opcode(), 132);
  EXPECT_EQ(result.pending_notifications[2].opcode(), 137);
}

TEST(FramePipeline, HandshakeAndOwnPacketsStaySeparateFromOrderedDeliveryEffects) {
  RecordingOperations operations;
  operations.request_opcode = kEquipOpcode;
  operations.prepare_action = [](SessionFrameState &state) {
    state.response = tagged_frame(183);
    state.pending_notifications.push_back(tagged_frame(125));
    return PreparedFrameInput{WorldGameplayRequest{kEquipOpcode, 17, {}}, {}, {}};
  };
  operations.handlers["equip"] = [](SessionFrameContext &frame) {
    frame.session_packets.push_back({169, {{"own", true}}});
    return true;
  };
  operations.actions["post_companion_delivery"] = [](SessionFrameContext &frame) {
    frame.state.pending_notifications.push_back(tagged_frame(10));
  };
  operations.actions["post_gathering_changes"] = [](SessionFrameContext &frame) {
    frame.state.pending_notifications.push_back(tagged_frame(206));
  };
  operations.encode_session_packets();
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(kEquipOpcode, metadata);

  ASSERT_TRUE(result.response);
  EXPECT_EQ(result.response->opcode(), 183);
  ASSERT_EQ(result.pending_notifications.size(), 4);
  EXPECT_EQ(result.pending_notifications[0].opcode(), 125);
  EXPECT_EQ(result.pending_notifications[1].opcode(), 169);
  EXPECT_EQ(result.pending_notifications[2].opcode(), 10);
  EXPECT_EQ(result.pending_notifications[3].opcode(), 206);
  EXPECT_EQ(operations.logged_notifications, 4);
  EXPECT_LT(operations.position("session_encoding"),
            operations.position("post_companion_delivery"));
  EXPECT_LT(operations.position("post_companion_delivery"),
            operations.position("post_gathering_changes"));
}

TEST(FramePipeline, PersistentCompositionDoesNotRetainPerFramePacketsOrDeadlines) {
  RecordingOperations operations;
  operations.actions["sector_sight"] = [](SessionFrameContext &frame) {
    frame.state.pending_notifications.push_back(tagged_frame(206));
    frame.action_deadline = 123;
  };
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;
  const auto first = pipeline.run(kUnknownOpcode, metadata);
  operations.actions.clear();

  const auto second = pipeline.run(kUnknownOpcode, metadata);

  ASSERT_EQ(first.pending_notifications.size(), 1);
  EXPECT_EQ(first.action_deadline, 123);
  EXPECT_TRUE(second.pending_notifications.empty());
  EXPECT_FALSE(second.action_deadline);
}

// Every step the pipeline visits, in order, when all handlers decline. Observers and handlers
// are interleaved on purpose (sight runs after request consumption, snapshots between
// handlers), so these literal lists fail on any reordering, insertion or removal of a step.
using StepTrace = std::vector<std::string>;

StepTrace visited_steps(std::uint16_t opcode, bool has_request = true) {
  RecordingOperations operations;
  operations.request_opcode = opcode;
  operations.has_request = has_request;
  SessionFramePipeline pipeline(operations);
  WorldPacketMetadata metadata;

  const auto result = pipeline.run(opcode, metadata);

  EXPECT_FALSE(result.rejected);
  return operations.trace;
}

TEST(FramePipeline, UnknownOpcodeVisitsTheExactFullStepSequence) {
  const StepTrace expected{
      "prepare",
      "movement_mode",
      "map_load_reset",
      "frame_metrics",
      "patrol_position",
      "map_load_vitals",
      "shared_map_mobs",
      "movement_vitals",
      "server_map_completion",
      "raid_arrival",
      "companion_own_packets",
      "statue",
      "escape",
      "adventure_lifecycle",
      "tear_snapshot",
      "quest_snapshot",
      "utility_bag_snapshot",
      "shared_population",
      "sector_sight",
      "npc_sight",
      "unhandled_interaction",
      "tutorial_tail",
      "start_playing_patrol",
      "encumbrance",
      "session_encoding",
      "riding_vitals_reset",
      "final_deadline",
      "post_movement_presence",
      "post_companion_entry",
      "post_companion_delivery",
      "post_gathering_changes",
      "post_player_services",
      "post_party_update",
      "post_raid_update",
      "post_emotion_snapshot",
      "post_guild_update",
      "post_housing_snapshot",
      "post_progress_checkpoint",
      "logged",
  };

  EXPECT_EQ(visited_steps(kUnknownOpcode), expected);

  // TimeSync belongs to no pipeline handler, so it takes exactly the unknown-opcode path.
  EXPECT_EQ(visited_steps(kTimeSyncOpcode), expected);
}

TEST(FramePipeline, MapLoadCompleteAddsGatheringBetweenEscapeAndTheTearSnapshot) {
  const StepTrace expected{
      "prepare",
      "movement_mode",
      "map_load_reset",
      "frame_metrics",
      "patrol_position",
      "map_load_vitals",
      "shared_map_mobs",
      "movement_vitals",
      "server_map_completion",
      "raid_arrival",
      "companion_own_packets",
      "statue",
      "escape",
      "gathering",
      "adventure_lifecycle",
      "tear_snapshot",
      "quest_snapshot",
      "utility_bag_snapshot",
      "shared_population",
      "sector_sight",
      "npc_sight",
      "unhandled_interaction",
      "tutorial_tail",
      "start_playing_patrol",
      "encumbrance",
      "session_encoding",
      "riding_vitals_reset",
      "final_deadline",
      "post_movement_presence",
      "post_companion_entry",
      "post_companion_delivery",
      "post_gathering_changes",
      "post_player_services",
      "post_party_update",
      "post_raid_update",
      "post_emotion_snapshot",
      "post_guild_update",
      "post_housing_snapshot",
      "post_progress_checkpoint",
      "logged",
  };

  EXPECT_EQ(visited_steps(kMapLoadCompleteOpcode), expected);
}

TEST(FramePipeline, InteractionStartOffersTheHousingToggleBeforeStatueProcessingAndGathering) {
  const StepTrace expected{
      "prepare",
      "movement_mode",
      "map_load_reset",
      "frame_metrics",
      "patrol_position",
      "map_load_vitals",
      "shared_map_mobs",
      "movement_vitals",
      "housing_toggle",
      "server_map_completion",
      "raid_arrival",
      "companion_trade",
      "companion_own_packets",
      "statue",
      "escape",
      "processing",
      "gathering",
      "adventure_lifecycle",
      "adventure",
      "tear_snapshot",
      "quest_snapshot",
      "utility_bag_snapshot",
      "shared_population",
      "sector_sight",
      "npc_sight",
      "unhandled_interaction",
      "tutorial_tail",
      "start_playing_patrol",
      "encumbrance",
      "session_encoding",
      "riding_vitals_reset",
      "final_deadline",
      "post_movement_presence",
      "post_companion_entry",
      "post_companion_delivery",
      "post_gathering_changes",
      "post_player_services",
      "post_party_update",
      "post_raid_update",
      "post_emotion_snapshot",
      "post_guild_update",
      "post_housing_snapshot",
      "post_progress_checkpoint",
      "logged",
  };

  EXPECT_EQ(visited_steps(kInteractionStartOpcode), expected);
}

TEST(FramePipeline, EscapeOffersHousingDepartureThenTradeBeforeTheGenericEscape) {
  const StepTrace expected{
      "prepare",
      "movement_mode",
      "map_load_reset",
      "frame_metrics",
      "patrol_position",
      "map_load_vitals",
      "shared_map_mobs",
      "movement_vitals",
      "housing_departure",
      "server_map_completion",
      "raid_arrival",
      "companion_trade",
      "companion_own_packets",
      "statue",
      "escape",
      "adventure_lifecycle",
      "tear_snapshot",
      "quest_snapshot",
      "utility_bag_snapshot",
      "shared_population",
      "sector_sight",
      "npc_sight",
      "unhandled_interaction",
      "tutorial_tail",
      "start_playing_patrol",
      "encumbrance",
      "session_encoding",
      "riding_vitals_reset",
      "final_deadline",
      "post_movement_presence",
      "post_companion_entry",
      "post_companion_delivery",
      "post_gathering_changes",
      "post_player_services",
      "post_party_update",
      "post_raid_update",
      "post_emotion_snapshot",
      "post_guild_update",
      "post_housing_snapshot",
      "post_progress_checkpoint",
      "logged",
  };

  EXPECT_EQ(visited_steps(kEscapeOpcode), expected);
}

TEST(FramePipeline, FrameWithoutAGameplayRequestVisitsEveryObserverAndNoHandler) {
  const StepTrace expected{
      "prepare",
      "movement_mode",
      "map_load_reset",
      "frame_metrics",
      "patrol_position",
      "map_load_vitals",
      "shared_map_mobs",
      "movement_vitals",
      "server_map_completion",
      "raid_arrival",
      "companion_own_packets",
      "adventure_lifecycle",
      "tear_snapshot",
      "quest_snapshot",
      "utility_bag_snapshot",
      "shared_population",
      "sector_sight",
      "npc_sight",
      "unhandled_interaction",
      "start_playing_patrol",
      "encumbrance",
      "session_encoding",
      "riding_vitals_reset",
      "final_deadline",
      "post_movement_presence",
      "post_companion_entry",
      "post_companion_delivery",
      "post_gathering_changes",
      "post_player_services",
      "post_party_update",
      "post_raid_update",
      "post_emotion_snapshot",
      "post_guild_update",
      "post_housing_snapshot",
      "post_progress_checkpoint",
      "logged",
  };

  EXPECT_EQ(visited_steps(kUnknownOpcode, false), expected);
}

TEST(FramePipeline, ComposedStepNamesFollowTheOriginalChainOrderStepForStep) {
  RecordingOperations operations;
  SessionFramePipeline pipeline(operations);

  std::vector<std::string> expected;
  for (const auto &step : kComposedSteps) {
    std::string label(step.operation);
    std::ranges::replace(label, '_', '-');
    expected.push_back(label);
  }

  // Every adjacent pair of steps is pinned here, together with the diagnostic label that
  // rejection logs print for it.
  EXPECT_EQ(pipeline.step_names(), expected);
}

// One representative opcode per handler, so each route is dispatched at least once.
std::vector<std::uint16_t> representative_opcodes() {
  return {
      kUnknownOpcode,
      kLootItemRequestOpcode,
      world_request::kReviveRequest,
      party::kPartyKickRequest,
      guild_world::kInvitePopupRequest,
      housing::kEditModeRequest,
      world_request::kInteractionStartRequest,
      world_request::kZoneChannelMoveRequest,
      travel::kLevelTriggerRequest,
      world_request::kEscapeRequest,
      kMarketRegisterRequest,
      ranking::kRankingListRequest,
      world_request::kWorldChatRequest,
      companion::kSpawnMateNpcRequest,
      world_request::kStartSkillRequest,
      kItemUseRequestOpcode,
      company_world::kDialogueFunctionRequest,
      kGmCommandRequestOpcode,
      world_request::kEquipRequest,
      world_request::kDiscardRequest,
      world_request::kPolishRequest,
      kChangePerkRequest,
      kEmotionExecRequestOpcode,
      npc::kThreatenedNpcRequest,
      kInteractUnitRequestOpcode,
      kProcessingDialogueStartRequest,
      kGatheringExecuteInteractionRequest,
      world_request::kMapLoadCompleteRequest,
      world_request::kActivateAdventureObjectRequest,
      kUtilityBagSetRequestOpcode,
      world_request::kAcceptQuestRequest,
      world_request::kSkillEffectReportRequest,
  };
}

StepTrace expected_dispatch_trace(const std::unordered_set<std::string_view> &owners) {
  StepTrace expected{"prepare"};
  for (const auto &step : kComposedSteps) {
    if (step.observer || sees_every_opcode(step.operation) || owners.contains(step.operation)) {
      expected.emplace_back(step.operation);
    }
  }
  for (const auto *operation : kPostludeOperations) {
    expected.emplace_back(operation);
  }
  expected.emplace_back("logged");
  return expected;
}

TEST(FramePipeline, EveryHandlerGroupIsDispatchedInTheComposedOrderBetweenItsObservers) {
  const auto owner_routes = routes_published_by_owners();
  std::unordered_set<std::string_view> exercised;

  for (const auto opcode : representative_opcodes()) {
    SCOPED_TRACE(opcode);
    const auto owners = handlers_owning(opcode, owner_routes);
    exercised.insert(owners.begin(), owners.end());

    EXPECT_EQ(visited_steps(opcode), expected_dispatch_trace(owners));
  }

  // A newly composed handler must get a representative opcode before this test passes again.
  for (const auto &step : kComposedSteps) {
    if (!step.observer && !sees_every_opcode(step.operation)) {
      EXPECT_TRUE(exercised.contains(step.operation)) << step.operation;
    }
  }
}

} // namespace
} // namespace chronicles
