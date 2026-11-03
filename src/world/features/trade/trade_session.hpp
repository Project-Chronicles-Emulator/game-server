#pragma once

// One world connection's trade run: the trade-manager dialogue, loading, departure, wagon
// recovery, parking, stowing, selling, cancelling and market prices.
//
// TradeSession plans every change with src/world/features/trade/trade_run.cpp against committed
// progress, commits it atomically, and asks the shared CompanionWorld to spawn, move or remove the
// wagon. Confined to the session's worker strand.
//
// Requests (opcodes from protocol/opcodes.json):
//   20065 DialogueStartReq      -> DialogueStartRes(161) for TradeManager NPCs only
//   20066 DialogueEndReq        -> DialogueEndNoti(162) while a trade dialogue is open
//   20067 DialogueFunctionReq   -> TradeManagement selection: DialogueEndNoti (opens the UI)
//   20105 TradeMarketPriceReq   -> TradeMarketPriceRes(209)
//   20106 TradeGoodLoadingReq   -> ItemsUpdateNoti(trade_loading); on start the wagon
//                                  UnitAddNoti + MountUnitNoti, then TradeStartNoti(205)
//   20108 TradingPostWagonParkingReq -> TradingPostWagonParkingRes(210)
//   20109 TradingPostWagonRemoveReq  -> wagon dismount + UnitRemoveNoti (no typed reply)
//   20107 TradeGoodSellReq      -> ItemsUpdateNoti(trade_sell); TradeEndNoti when sold out
//   20104 TradeCancelReq        -> wagon removal (ClearTrade) and TradeEndNoti(206)
//   20048 EscapeReq{is_trading} -> wagon respawned beside the player, SpawnTradeWagonRes(119)
// Failures without a typed reply use PS2C_CommonErrorRes.

#include "world/core/quest_events.hpp"
#include "world/features/companion/companion_session.hpp"
#include "world/features/trade/trade_run.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace chronicles::trade {

enum class RegionCheckpointResult {
  no_driven_trade_wagon,
  committed,
  conflict,
  unknown,
};

// Identifies the run whose detached wagon must be settled. A later run must never
// receive an earlier wagon's transform, even when it uses the same model.
struct TradeRunIdentity {
  int start_post_id{};
  int vehicle_cms_id{};
  std::int64_t started_epoch_ticks{};

  bool operator==(const TradeRunIdentity &) const = default;
};

struct PendingWagonSettlement {
  companion::VehicleSnapshot wagon;
  std::optional<TradeRunIdentity> run_identity;
  std::optional<WorldPosition> player_position;
};

enum class WagonSettlementResult {
  applied,
  already_applied,
  superseded,
  retry,
};

// Fence any previous uncertain transaction before retrying. Readback then
// recognizes a snapshot that committed despite a lost acknowledgement.
WagonSettlementResult reconcile_wagon_settlement(ProgressAccess &progress,
                                                 const PendingWagonSettlement &pending);

inline constexpr std::uint16_t kDialogueStartRequest = 20065;
inline constexpr std::uint16_t kDialogueEndRequest = 20066;
inline constexpr std::uint16_t kDialogueStartResponse = 161;

// Reach for starting a dialogue with a trade-manager NPC, the NPC interaction radius.
inline constexpr std::int64_t kTradeManagerReachMm = 7000;

struct TradeServices {
  std::shared_ptr<companion::CompanionWorld> world;
  std::shared_ptr<const companion::CompanionCatalog> vehicles;
  std::shared_ptr<const TradeCatalog> catalog;
  std::shared_ptr<const ShopCatalog> items;
  // Npc ids whose Npc.category is TradeManager.
  std::shared_ptr<const std::set<int>> trade_managers;
};

class TradeSession {
public:
  TradeSession(TradeServices services, std::unique_ptr<ProgressAccess> progress,
               std::uint64_t connection_id, std::int64_t character_id);

  // True for trade requests. EscapeReq is owned only while trading; dialogue requests only for
  // trade-manager dialogues.
  bool handles(std::uint16_t opcode, const nlohmann::json &message) const;

  companion::SessionResult receive(std::uint16_t opcode, const nlohmann::json &message,
                                   std::uint32_t sequence, const WorldPosition &position,
                                   std::int64_t now_epoch_ticks);

  bool is_trade_manager(int npc_cms_id) const;

  // DialogueStartReq aimed at an announced TradeManager NPC (resolved by the caller).
  companion::SessionResult start_dialogue(std::uint64_t target_unit_id,
                                          const WorldPosition &npc_position,
                                          const WorldPosition &player_position);

  // Restores an unfinished run after the client entered the map: the wagon reappears at its last
  // transform and TradeWagonPosNoti marks it. TradeStartNoti is sent at once only when the owner
  // already drives the wagon (a region re-entry); otherwise it waits for wagon_boarded().
  companion::SessionResult enter_world(const WorldPosition &position,
                                       std::int64_t now_epoch_ticks);

  // The owner boarded its trade wagon: a run restored on foot now sends its TradeStartNoti.
  companion::SessionResult wagon_boarded();

  // A driven trade wagon and its character move in one revision-guarded transaction.
  // An unknown outcome requires reconnecting to read the committed state.
  RegionCheckpointResult checkpoint_region(const WorldPosition &destination,
                                            std::int64_t now_epoch_ticks);

  // The owner got off its wagon: persist the wagon transform and tell the client where it is.
  companion::SessionResult wagon_dismounted(std::int64_t now_epoch_ticks);

  // Persists the transform and health of wagons that left with the session.
  bool wagons_left(const std::vector<companion::VehicleSnapshot> &vehicles,
                   std::int64_t now_epoch_ticks);

  // Capture once immediately after detach, before delivery or persistence can fail.
  // This uses only session state and remains available during a database outage.
  std::optional<PendingWagonSettlement> capture_wagon_settlement(
      const std::vector<companion::VehicleSnapshot> &vehicles,
      std::optional<WorldPosition> player_position) const;

  // Wagon damage and voucher grants from authorized GM commands.
  companion::SessionResult gm_command(const std::vector<companion::CompanionGmCommand> &commands,
                                      std::int64_t now_epoch_ticks);

  // Trade-good sales committed since the last call, for TradePayment quest objectives.
  QuestEvents take_quest_events();

private:
  companion::SessionResult dialogue_function(const nlohmann::json &message,
                                             std::uint32_t sequence);
  companion::SessionResult end_dialogue();
  companion::SessionResult load(const nlohmann::json &message, std::uint32_t sequence,
                                const WorldPosition &position, std::int64_t now);
  companion::SessionResult sell(const nlohmann::json &message, std::uint32_t sequence,
                                const WorldPosition &position, std::int64_t now);
  companion::SessionResult park(const nlohmann::json &message, std::uint32_t sequence,
                                const WorldPosition &position, std::int64_t now);
  companion::SessionResult stow(std::uint32_t sequence, std::int64_t now);
  companion::SessionResult cancel(std::uint32_t sequence, std::int64_t now);
  companion::SessionResult escape(std::uint32_t sequence, const WorldPosition &position,
                                  std::int64_t now);
  companion::SessionResult damage_wagon(std::uint64_t unit_id, int amount, std::int64_t now);
  companion::SessionResult add_voucher(int good_cms_id, int valid_minutes, std::int64_t now);

  TradeContext context(const Character &character, const WorldProgress &before,
                       std::int64_t now) const;
  // Commits a plan; false when storage refused it.
  std::optional<std::vector<CharacterItem>> commit(const WorldProgress &before,
                                                   const TradePlan &plan);
  // The current trading post when the player still stands at it.
  std::optional<int> current_post(const WorldPosition &position) const;

  TradeServices services_;
  std::unique_ptr<ProgressAccess> progress_;
  std::uint64_t connection_id_{};
  std::int64_t character_id_{};
  // Trade-manager dialogue in progress (the NPC's position).
  std::optional<WorldPosition> dialogue_npc_position_;
  // Trading post chosen through the TradeManagement dialogue or the last parking reply.
  std::optional<int> current_post_id_;
  // Committed trade-good sales not yet handed to the quest service.
  QuestEvents quest_events_;
  std::optional<TradeRunIdentity> active_run_identity_;
  // Unit identity is assigned only by this session's successful trade-wagon spawns.
  std::optional<std::uint64_t> trade_wagon_unit_id_;
  // The client's TradeStartNoti handler cuts the screen to opaque black, and its 0.5 s trade
  // tick restores the view only while the local character rides a vehicle
  // (AAC_Trade_FX5TradeManager_OnTradeStartNoti / _Tick_ParkingStateMachine_Candidate). A run
  // restored while the owner is on foot therefore holds TradeStartNoti until the owner drives.
  bool trade_start_pending_{};
};

} // namespace chronicles::trade
