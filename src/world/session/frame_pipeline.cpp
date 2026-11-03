#include "world/session/frame_pipeline.hpp"

#include "world/core/frame_router.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/character/achievements.hpp"
#include "world/features/character/progression.hpp"
#include "world/features/company/company_world.hpp"
#include "world/features/events/channel_event.hpp"
#include "world/features/events/preorder_bonus.hpp"
#include "world/features/events/ranking.hpp"
#include "world/features/expression/player_expression.hpp"
#include "world/features/gm/gm_commands.hpp"
#include "world/features/housing/housing_world.hpp"
#include "world/features/inventory/utility_bag.hpp"
#include "world/features/item_use/item_use.hpp"
#include "world/features/loot/loot.hpp"
#include "world/features/market/market_protocol.hpp"
#include "world/features/npc/npc_reactions.hpp"
#include "world/features/processing/processing.hpp"
#include "world/features/trade/trade_session.hpp"
#include "world/features/travel/map_travel.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace chronicles {
namespace {

constexpr auto kSlowFrameStepThreshold = std::chrono::milliseconds(100);

// Operators can enable this bounded diagnostic through the existing log-level control.
// Normal logging does not read the clock; diagnostics never include request/player data.
class SlowFrameStepTimer {
public:
  SlowFrameStepTimer(std::uint16_t opcode, std::string_view step) : opcode_(opcode), step_(step) {
    if (spdlog::should_log(spdlog::level::debug)) {
      started_ = Clock::now();
    }
  }

  ~SlowFrameStepTimer() {
    if (!started_) {
      return;
    }

    const auto elapsed = Clock::now() - *started_;
    if (elapsed > kSlowFrameStepThreshold) {
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
      spdlog::debug("Slow world frame opcode={} step={} elapsed_ms={}", opcode_, step_,
                    elapsed_ms.count());
    }
  }

private:
  using Clock = std::chrono::steady_clock;

  std::uint16_t opcode_;
  std::string_view step_;
  std::optional<Clock::time_point> started_;
};

template <typename Operation, typename... Arguments>
decltype(auto) profile_frame_step(std::uint16_t opcode, std::string_view step, Operation operation,
                                  Arguments &&...arguments) {
  const SlowFrameStepTimer timer(opcode, step);
  return std::invoke(operation, std::forward<Arguments>(arguments)...);
}

// The candidate sets below preserve the world-PC2S routing of the original frame chain;
// payload ownership stays in each operation. The wire IDs are defined once in
// world/core/world_request_opcodes.hpp, which the owning features also use, so a route
// can never drift from the request its owner decodes.
using world_request::kAcceptQuestRequest;
using world_request::kActivateAdventureObjectRequest;
using world_request::kChangeSkillStageRequest;
using world_request::kClaimQuestRewardRequest;
using world_request::kClientProjectileHitRequest;
using world_request::kDespawnClientProjectileRequest;
using world_request::kDiscardRequest;
using world_request::kDiscoverAdventureObjectRequest;
using world_request::kEquipRequest;
using world_request::kEscapeRequest;
using world_request::kExecuteSpawnProjectileRequest;
using world_request::kInteractionStartRequest;
using world_request::kInteractionUnitRequest;
using world_request::kMapLoadCompleteRequest;
using world_request::kPolishRequest;
using world_request::kProgressQuestRequest;
using world_request::kReviveRequest;
using world_request::kSkillEffectReportRequest;
using world_request::kStartSkillRequest;
using world_request::kUpdateProjectileRequest;
using world_request::kWorldChatRequest;
using world_request::kZoneChannelMoveRequest;

constexpr std::array kLootOpcodes{kLootItemRequestOpcode};
constexpr std::array kRevivalOpcodes{kReviveRequest};
constexpr std::array kPartyOpcodes{
    party::kPartyKickRequest,         party::kPartyLeaveRequest,
    party::kPartyChangeLeaderRequest, party::kPartyInvitePopupRequest,
    party::kPartyAcceptInviteRequest, party::kPartyRejectInviteRequest};
constexpr std::array kGuildOpcodes{guild_world::kInvitePopupRequest,
                                   guild_world::kAcceptInviteRequest,
                                   guild_world::kRejectInviteRequest};
constexpr std::array kHousingOpcodes{
    housing::kEditModeRequest,      housing::kContractExtensionRequest,
    housing::kPackageUnpackRequest, housing::kPackageRecoverRequest,
    housing::kUpdateHousingRequest, housing::kItemDiscardRequest,
    housing::kGoHomeRequest};
constexpr std::array kHousingToggleOpcodes{kInteractionStartRequest,
                                           world_request::kInteractionExecuteRequest,
                                           world_request::kInteractionEndRequest};
constexpr std::array kHousingDepartureOpcodes{kZoneChannelMoveRequest, kEscapeRequest};
constexpr std::array kZoneMoveOpcodes{kZoneChannelMoveRequest};
constexpr std::array kMapTravelOpcodes{travel::kLevelTriggerRequest, travel::kCutSceneEndRequest,
                                       channel_event::kChannelTriggerEntryRequest,
                                       world_request::kInteractionUnitRequest};
constexpr std::array kMarketOpcodes{
    kMarketRegisterRequest, kMarketRevokeRequest,  kMarketPurchaseRequest, kMarketQueryRequest,
    kMarketClaimRequest,    kMarketDetailsRequest, kMarketHistoryRequest};
constexpr std::array kEventOpcodes{
    preorder_bonus::kStateRequest,      preorder_bonus::kClaimRequest,
    ranking::kRankingListRequest,       channel_event::kTributeCountRequest,
    channel_event::kItemTributeRequest, channel_event::kChannelTriggerEntryRequest,
    raid::kRaidChannelMoveRequest,      raid::kRaidChannelRegisterRequest,
    raid::kRaidChannelCancelRequest,    raid::kRaidChannelLeaveRequest};
constexpr std::array kChatOpcodes{kWorldChatRequest};
constexpr std::array kGmOpcodes{kGmCommandRequestOpcode, kGmCommandTextRequestOpcode};
constexpr std::array kCompanionTradeOpcodes{kGmCommandRequestOpcode,
                                            kInteractionStartRequest,
                                            kProcessingExecuteInteractionRequest,
                                            kProcessingEndInteractionRequest,
                                            kGmCommandTextRequestOpcode,
                                            companion::kSpawnMateNpcRequest,
                                            companion::kDespawnMateNpcRequest,
                                            companion::kInviteMateNpcRequest,
                                            companion::kSpawnSummonNpcRequest,
                                            companion::kRideUnitRequest,
                                            companion::kGetOffUnitRequest,
                                            trade::kEscapeRequest,
                                            trade::kDialogueStartRequest,
                                            trade::kDialogueEndRequest,
                                            trade::kDialogueFunctionRequest,
                                            companion::kInteractUnitRequest,
                                            companion::kItemUseRequest,
                                            trade::kTradeCancelRequest,
                                            trade::kTradeMarketPriceRequest,
                                            trade::kTradeGoodLoadingRequest,
                                            trade::kTradeGoodSellRequest,
                                            trade::kTradingPostWagonParkingRequest,
                                            trade::kTradingPostWagonRemoveRequest};
constexpr std::array kStatueBusyOpcodes{kStartSkillRequest};
constexpr std::array kItemUseOpcodes{kItemUseRequestOpcode};
constexpr std::array kCompanyOpcodes{company_world::kDialogueFunctionRequest};
constexpr std::array kEquipOpcodes{kEquipRequest, world_request::kWeaponSlotActivationRequest};
constexpr std::array kDiscardOpcodes{kDiscardRequest};
constexpr std::array kPolishOpcodes{kPolishRequest};
constexpr std::array kProgressionOpcodes{kChangePerkRequest, kResetPerkRequest,
                                         kUpdateStatBonusRequest,
                                         character::kUpdateAchievementRequest};
constexpr std::array kExpressionOpcodes{kEmotionExecRequestOpcode, kSelfieModeRequestOpcode};
constexpr std::array kNpcThreatOpcodes{npc::kThreatenedNpcRequest};
constexpr std::array kMerchantOpcodes{kInteractionUnitRequest};
constexpr std::array kProcessingOpcodes{
    kProcessingDialogueStartRequest, kProcessingDialogueEndRequest,
    kProcessingStartInteractionRequest, kProcessingExecuteInteractionRequest,
    kProcessingEndInteractionRequest};
constexpr std::array kGatheringOpcodes{kGatheringStartInteractionRequest,
                                       kGatheringExecuteInteractionRequest,
                                       kGatheringEndInteractionRequest,
                                       kGatheringInteractUnitRequest,
                                       kMapLoadCompleteRequest};
constexpr std::array kTearOpcodes{kActivateAdventureObjectRequest, kDiscoverAdventureObjectRequest};
constexpr std::array kAdventureOpcodes{
    kInteractionStartRequest, kInteractionUnitRequest, kDiscoverAdventureObjectRequest,
    kProcessingExecuteInteractionRequest, kProcessingEndInteractionRequest};
constexpr std::array kUtilityBagOpcodes{kUtilityBagSetRequestOpcode};

// The wire-range test checks the quest set against WorldQuestBook::handles. CombatSession::handles
// needs a live session and request, so the combat set is declared here. The tutorial tail is
// deliberately broader than either and remains explicit below.
constexpr std::array kWorldQuestOpcodes{kAcceptQuestRequest,
                                        world_request::kAbandonQuestRequest,
                                        kClaimQuestRewardRequest,
                                        kProgressQuestRequest,
                                        world_request::kQuestJobBoardRequest,
                                        kProcessingDialogueStartRequest,
                                        kProcessingDialogueEndRequest};
constexpr std::array kPrivateCombatOpcodes{world_request::kStartRootMotionRequest,
                                           world_request::kAdaptedRotationRequest,
                                           world_request::kStartGlidingRequest,
                                           kStartSkillRequest,
                                           kSkillEffectReportRequest,
                                           kExecuteSpawnProjectileRequest,
                                           kChangeSkillStageRequest,
                                           kClientProjectileHitRequest,
                                           kDespawnClientProjectileRequest,
                                           kUpdateProjectileRequest};

using ObserverOperation = void (SessionFrameOperations::*)(SessionFrameContext &);
using HandlerOperation = bool (SessionFrameOperations::*)(SessionFrameContext &);

class ObserverAdapter final : public FrameObserver {
public:
  ObserverAdapter(std::string_view name, SessionFrameOperations &session,
                  ObserverOperation operation)
      : name_(name), session_(session), operation_(operation) {
  }

  std::string_view name() const noexcept override {
    return name_;
  }

  void observe(FrameContext &context) override {
    // Only SessionFramePipeline::run invokes this private composition, with this type.
    profile_frame_step(context.incoming_opcode(), name_, operation_, session_,
                       static_cast<SessionFrameContext &>(context));
  }

private:
  std::string_view name_;
  SessionFrameOperations &session_;
  ObserverOperation operation_;
};

class HandlerAdapter final : public FrameRequestHandler {
public:
  HandlerAdapter(std::string_view name, SessionFrameOperations &session,
                 std::span<const std::uint16_t> opcodes, HandlerOperation operation,
                 bool all_opcodes = false)
      : name_(name), session_(session), opcodes_(opcodes), operation_(operation),
        all_opcodes_(all_opcodes) {
  }

  std::string_view name() const noexcept override {
    return name_;
  }

  std::span<const std::uint16_t> opcodes() const noexcept override {
    return opcodes_;
  }

  bool handles_all_opcodes() const noexcept override {
    return all_opcodes_;
  }

  bool handle(FrameContext &context) override {
    return profile_frame_step(context.incoming_opcode(), name_, operation_, session_,
                              static_cast<SessionFrameContext &>(context));
  }

private:
  std::string_view name_;
  SessionFrameOperations &session_;
  std::span<const std::uint16_t> opcodes_;
  HandlerOperation operation_;
  bool all_opcodes_;
};

struct Composition {
  void add_observer(std::string_view name, SessionFrameOperations &session,
                    ObserverOperation operation) {
    auto observer = std::make_unique<ObserverAdapter>(name, session, operation);
    order.push_back(FrameStep::observer(*observer));
    observers.push_back(std::move(observer));
  }

  void add_handler(std::string_view name, SessionFrameOperations &session,
                   std::span<const std::uint16_t> opcodes, HandlerOperation operation,
                   bool all_opcodes = false) {
    auto handler = std::make_unique<HandlerAdapter>(name, session, opcodes, operation, all_opcodes);
    order.push_back(FrameStep::handler(*handler));
    handlers.push_back(std::move(handler));
  }

  std::vector<std::unique_ptr<ObserverAdapter>> observers;
  std::vector<std::unique_ptr<HandlerAdapter>> handlers;
  std::vector<FrameStep> order;
};

Composition compose(SessionFrameOperations &session) {
  using Operations = SessionFrameOperations;
  Composition result;

  // This is the original process_frame order. Observers stay interleaved with consuming
  // handlers; in particular, sight and finalization still run after request consumption.
  result.add_observer("movement-mode", session, &Operations::observe_movement_mode);
  result.add_observer("map-load-reset", session, &Operations::observe_map_load_reset);
  result.add_observer("frame-metrics", session, &Operations::observe_frame_metrics);
  result.add_observer("patrol-position", session, &Operations::observe_patrol_position);
  result.add_observer("map-load-vitals", session, &Operations::observe_map_load_vitals);
  result.add_observer("shared-map-mobs", session, &Operations::observe_shared_map_mobs);
  result.add_handler("shared-loot", session, kLootOpcodes, &Operations::handle_shared_loot);
  result.add_observer("movement-vitals", session, &Operations::observe_movement_vitals);
  result.add_handler("shared-revival", session, kRevivalOpcodes,
                     &Operations::handle_shared_revival);
  result.add_handler("party", session, kPartyOpcodes, &Operations::handle_party);
  result.add_handler("guild", session, kGuildOpcodes, &Operations::handle_guild);
  result.add_handler("housing-request", session, kHousingOpcodes,
                     &Operations::handle_housing_request);
  result.add_handler("housing-toggle", session, kHousingToggleOpcodes,
                     &Operations::handle_housing_toggle);
  result.add_handler("housing-departure", session, kHousingDepartureOpcodes,
                     &Operations::handle_housing_departure);
  result.add_handler("zone-channel-move", session, kZoneMoveOpcodes,
                     &Operations::handle_zone_channel_move);
  // Before "events": ChannelTriggerEntryReq travels through the map travel profile when it is
  // loaded; the events handler keeps its ungated boundary route otherwise.
  result.add_handler("map-travel", session, kMapTravelOpcodes, &Operations::handle_map_travel);
  result.add_observer("server-map-completion", session, &Operations::observe_server_map_completion);
  result.add_observer("raid-arrival", session, &Operations::observe_raid_arrival);
  result.add_handler("market", session, kMarketOpcodes, &Operations::handle_market);
  result.add_handler("events", session, kEventOpcodes, &Operations::handle_events);
  result.add_handler("world-chat", session, kChatOpcodes, &Operations::handle_world_chat);
  result.add_handler("companion-trade", session, kCompanionTradeOpcodes,
                     &Operations::handle_companion_trade);
  result.add_observer("companion-own-packets", session, &Operations::observe_companion_own_packets);
  // The guarded ownership check lazily creates the statue session even for unrelated
  // requests. Keep construction and its failure timing at this original composition point.
  result.add_handler("statue", session, {}, &Operations::handle_statue, true);
  result.add_handler("statue-busy-skill", session, kStatueBusyOpcodes,
                     &Operations::handle_statue_busy_skill);
  result.add_handler("item-use", session, kItemUseOpcodes, &Operations::handle_item_use);
  result.add_handler("company", session, kCompanyOpcodes, &Operations::handle_company);
  result.add_handler("social-gm", session, kGmOpcodes, &Operations::handle_social_gm);
  result.add_handler("generic-gm", session, kGmOpcodes, &Operations::handle_generic_gm);
  result.add_handler("equip", session, kEquipOpcodes, &Operations::handle_equip);
  result.add_handler("discard", session, kDiscardOpcodes, &Operations::handle_discard);
  result.add_handler("polish", session, kPolishOpcodes, &Operations::handle_polish);
  result.add_handler("progression", session, kProgressionOpcodes, &Operations::handle_progression);
  // escape_handles() also lazily creates its session before deciding opcode ownership.
  result.add_handler("escape", session, {}, &Operations::handle_escape, true);
  result.add_handler("expression", session, kExpressionOpcodes, &Operations::handle_expression);
  result.add_handler("npc-threat", session, kNpcThreatOpcodes, &Operations::handle_npc_threat);
  result.add_handler("merchant", session, kMerchantOpcodes, &Operations::handle_merchant);
  result.add_handler("processing", session, kProcessingOpcodes, &Operations::handle_processing);
  result.add_handler("gathering", session, kGatheringOpcodes, &Operations::handle_gathering);
  result.add_observer("adventure-lifecycle", session, &Operations::observe_adventure_lifecycle);
  result.add_handler("adventure", session, kAdventureOpcodes, &Operations::handle_adventure);
  result.add_handler("tears", session, kTearOpcodes, &Operations::handle_tears);
  result.add_observer("tear-snapshot", session, &Operations::observe_tear_snapshot);
  result.add_observer("quest-snapshot", session, &Operations::observe_quest_snapshot);
  result.add_observer("utility-bag-snapshot", session, &Operations::observe_utility_bag_snapshot);
  result.add_handler("utility-bag", session, kUtilityBagOpcodes, &Operations::handle_utility_bag);
  result.add_handler("world-quests", session, kWorldQuestOpcodes, &Operations::handle_world_quests);
  result.add_observer("shared-population", session, &Operations::observe_shared_population);
  result.add_observer("sector-sight", session, &Operations::observe_sector_sight);
  result.add_observer("npc-sight", session, &Operations::observe_npc_sight);
  result.add_handler("private-combat", session, kPrivateCombatOpcodes,
                     &Operations::handle_private_combat);
  result.add_observer("unhandled-interaction", session, &Operations::observe_unhandled_interaction);
  result.add_handler("tutorial-tail", session, {}, &Operations::handle_tutorial_tail, true);
  result.add_observer("start-playing-patrol", session, &Operations::observe_start_playing_patrol);
  result.add_observer("encumbrance", session, &Operations::observe_encumbrance);
  result.add_observer("session-encoding", session, &Operations::observe_session_encoding);
  result.add_observer("riding-vitals-reset", session, &Operations::observe_riding_vitals_reset);
  result.add_observer("final-deadline", session, &Operations::observe_final_deadline);
  return result;
}

} // namespace

SessionEventBus compose_session_events(SessionEventObservers observers) {
  SessionEventBus::Subscriptions subscriptions;
  // Resolve current feature/cache state when invoked; map changes rebuild combat.
  // Subscription order is fixed here before processing, never by feature self-registration.
  subscriptions.protection.push_back(std::move(observers.protection));
  subscriptions.visibility.push_back(std::move(observers.visibility));
  subscriptions.player_state.push_back(std::move(observers.combat_snapshot));
  subscriptions.company.push_back(std::move(observers.company_membership));
  subscriptions.progress.push_back(std::move(observers.progress_committed));
  if (observers.activity_experience) {
    subscriptions.activity_experience.push_back(std::move(observers.activity_experience));
  }
  if (observers.inventory_stats) {
    subscriptions.inventory_stats.push_back(std::move(observers.inventory_stats));
  }
  if (observers.inventory_snapshot) {
    subscriptions.inventory_snapshot.push_back(std::move(observers.inventory_snapshot));
  }
  if (observers.equipment_revision) {
    subscriptions.equipment_revision.push_back(std::move(observers.equipment_revision));
  }
  if (observers.world_loot_progress) {
    subscriptions.world_loot_progress.push_back(std::move(observers.world_loot_progress));
  }
  if (observers.progression_snapshot) {
    subscriptions.progression_snapshot.push_back(std::move(observers.progression_snapshot));
  }
  if (observers.player_level) {
    subscriptions.player_level.push_back(std::move(observers.player_level));
  }
  if (observers.player_life) {
    subscriptions.player_life.push_back(std::move(observers.player_life));
  }
  return SessionEventBus(std::move(subscriptions));
}

struct SessionFramePipeline::Impl {
  explicit Impl(SessionFrameOperations &session)
      : operations(session), composition(compose(session)), router(std::move(composition.order)) {
  }

  SessionFrameOperations &operations;
  Composition composition;
  FrameRouter router;
};

SessionFramePipeline::SessionFramePipeline(SessionFrameOperations &operations)
    : implementation_(std::make_unique<Impl>(operations)) {
}

SessionFramePipeline::~SessionFramePipeline() = default;

std::vector<std::string> SessionFramePipeline::step_names() const {
  return implementation_->router.step_names();
}

FramePipelineOutcome SessionFramePipeline::run(std::uint16_t incoming_opcode,
                                               WorldPacketMetadata &metadata) {
  auto &operations = implementation_->operations;
  SessionFrameState state(metadata);
  std::optional<SessionFrameContext> frame;
  bool rejected{};

  try {
    auto prepared = profile_frame_step(incoming_opcode, "prepare-frame",
                                       &SessionFrameOperations::prepare_frame, operations, state);
    frame.emplace(incoming_opcode, std::move(prepared), state);
    implementation_->router.dispatch(*frame);
  } catch (const std::exception &error) {
    const auto step = frame ? frame->active_step() : std::string_view("handshake");
    operations.report_frame_rejection(incoming_opcode, step, error);
    rejected = true;
  }

  // These operations used to follow the main rejection catch. Their narrower catches stay
  // inside the individual operation; an uncaught postlude failure must not become rejection.
  if (!rejected) {
    using Operations = SessionFrameOperations;
    profile_frame_step(incoming_opcode, "post-movement-presence",
                       &Operations::post_movement_presence, operations, *frame);
    profile_frame_step(incoming_opcode, "post-companion-entry", &Operations::post_companion_entry,
                       operations, *frame);
    profile_frame_step(incoming_opcode, "post-companion-delivery",
                       &Operations::post_companion_delivery, operations, *frame);
    profile_frame_step(incoming_opcode, "post-gathering-changes",
                       &Operations::post_gathering_changes, operations, *frame);
    profile_frame_step(incoming_opcode, "post-player-services", &Operations::post_player_services,
                       operations, *frame);
    profile_frame_step(incoming_opcode, "post-party-update", &Operations::post_party_update,
                       operations, *frame);
    profile_frame_step(incoming_opcode, "post-raid-update", &Operations::post_raid_update,
                       operations, *frame);
    profile_frame_step(incoming_opcode, "post-emotion-snapshot", &Operations::post_emotion_snapshot,
                       operations, *frame);
    profile_frame_step(incoming_opcode, "post-guild-update", &Operations::post_guild_update,
                       operations, *frame);
    profile_frame_step(incoming_opcode, "post-housing-snapshot", &Operations::post_housing_snapshot,
                       operations, *frame);
    profile_frame_step(incoming_opcode, "post-progress-checkpoint",
                       &Operations::post_progress_checkpoint, operations, *frame);
  }

  operations.log_frame_result(incoming_opcode, state, rejected);
  return {std::move(state.response), std::move(state.pending_notifications),
          frame ? frame->action_deadline : std::nullopt, rejected};
}

} // namespace chronicles
