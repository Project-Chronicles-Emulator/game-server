#pragma once

// Trade-run rules: loading goods onto a wagon, departure, parking, selling, cancelling and
// market prices. Every function plans against the character's committed progress and returns
// the next progress plus the packets to send after the caller commits it. Nothing here touches
// storage, sockets or the shared companion units; the session commits the plan and asks
// CompanionWorld to spawn or remove the wagon.
//
// Client contract (static analysis, out/vehicle-research/trade-findings.md):
// - TradeGoodLoadingReq{add_goods: voucher_id | good_count, wagon_cms_id, start}. start=false
//   loads; start=true loads the same selection and departs. Replies: ItemsUpdateNoti
//   trade_loading (goods appended, consumed vouchers as id-only entries), then on departure the
//   wagon UnitAddNoti, MountUnitNoti and TradeStartNoti{trade_goods}.
// - TradeGoodSellReq{goods: TradeGoods{good, origin post, production times}} is answered with
//   ItemsUpdateNoti trade_sell{results, remaining trade_goods}; an empty remainder resets the
//   client's trade (TradeEndNoti is sent as well).
// - TradeMarketPriceRes{next_update_tick, trading_posts{cms_id, group_patterns{group: pattern}}}.
// - TradingPostWagonParkingReq is sent inside 0.9 x the post's async sphere radius.
// Reward numbers, market ratios and purchase prices are Candidate design choices; the client
// only displays what the server sends.

#include "data/companion_catalog.hpp"
#include "ec.generated.pb.h"
#include "data/shop_catalog.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"
#include "data/trade_catalog.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/core/world_request_opcodes.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace chronicles::trade {

inline constexpr std::uint16_t kEscapeRequest = world_request::kEscapeRequest;
inline constexpr std::uint16_t kDialogueFunctionRequest = 20067;
inline constexpr std::uint16_t kTradeCancelRequest = 20104;
inline constexpr std::uint16_t kTradeMarketPriceRequest = 20105;
inline constexpr std::uint16_t kTradeGoodLoadingRequest = 20106;
inline constexpr std::uint16_t kTradeGoodSellRequest = 20107;
inline constexpr std::uint16_t kTradingPostWagonParkingRequest = 20108;
inline constexpr std::uint16_t kTradingPostWagonRemoveRequest = 20109;

inline constexpr std::uint16_t kCommonErrorResponse = 2;
inline constexpr std::uint16_t kSystemMessageNotification = 3;
inline constexpr std::uint16_t kSpawnTradeWagonResponse = 119;
inline constexpr std::uint16_t kDialogueEndNotification = 162;
inline constexpr std::uint16_t kItemsUpdateNotification = 169;
inline constexpr std::uint16_t kTradeStartNotification = 205;
inline constexpr std::uint16_t kTradeEndNotification = 206;
inline constexpr std::uint16_t kTradeWagonPosNotification = 207;
inline constexpr std::uint16_t kTradeMarketPriceResponse = 209;
inline constexpr std::uint16_t kTradingPostWagonParkingResponse = 210;

// A player must stand this close to a trading post's wagon departure point to trade there.
// The trade-manager NPCs stand 11-15 m from their post's departure point (Candidate bound).
inline constexpr std::int64_t kTradingPostReachMm = 30000;

struct LoadLine {
  std::optional<std::int64_t> voucher_id;
  int good_cms_id{};
  int count{};
};

struct LoadRequest {
  std::vector<LoadLine> lines;
  int wagon_cms_id{};
  bool start{};
};

struct SellLine {
  int good_cms_id{};
  int origin_post_id{};
  std::vector<std::int64_t> production_epoch_ticks;
};

struct SellRequest {
  std::vector<SellLine> lines;
};

x5::ec::EErrorCode parse_load_request(const nlohmann::json &message, LoadRequest &request);

x5::ec::EErrorCode parse_sell_request(const nlohmann::json &message, SellRequest &request);

struct TradeContext {
  const TradeCatalog &catalog;
  const companion::CompanionCatalog &vehicles;
  // Item storage kinds for reward items; without it item rewards are refused.
  const ShopCatalog *items{};
  const Character &character;
  const WorldProgress &before;
  std::int64_t now_epoch_ticks{};
};

// One reward line: an item amount or a money value.
struct RewardLine {
  std::optional<int> item_cms_id;
  std::optional<MoneyType> currency;
  std::int64_t value{};
};

// One sold result line as reported in ItemsUpdateNoti.trade_sell.results.
struct SaleResult {
  int good_cms_id{};
  GoodQuality quality{GoodQuality::none};
  std::int64_t price_ratio_basis_points{};
  std::int64_t basic_price_ratio_basis_points{};
  std::optional<int> event_pattern_id;
  std::int64_t event_price_ratio_basis_points{};
  RewardLine default_reward;
  std::vector<RewardLine> bonus_rewards;
};

struct TradePlan {
  x5::ec::EErrorCode error{x5::ec::Success};
  WorldProgress next;
  // Reward item instances (no storage ids until committed).
  std::vector<CharacterItem> granted_items;
  std::vector<MoneyChange> money_changes;
  std::vector<int> changed_stackables;
  std::vector<int> changed_housing_items;
  std::vector<std::int64_t> consumed_voucher_ids;
  std::vector<TradeGoodUnit> loaded_goods;
  std::vector<SaleResult> sale_results;
  // Departure: the session spawns the wagon at this post's departure point and seats the owner.
  std::optional<int> departure_post_id;
  // The run ended (sold out or cancelled); the session removes the wagon.
  bool run_ended{};
};

// Loads goods at `post_id` (and departs when request.start).
TradePlan plan_load(const TradeContext &context, int post_id, const LoadRequest &request);

// Sells goods at `post_id` from the character's run.
TradePlan plan_sell(const TradeContext &context, int post_id, const SellRequest &request,
                    std::uint32_t random_seed);

// Abandons the run: goods and fees are not refunded.
TradePlan plan_cancel(const TradeContext &context);

// Records the wagon parked at `post_id` (the session verified the sphere).
TradePlan plan_park(const TradeContext &context, int post_id, const WorldPosition &wagon);

// Stows the parked wagon; selling remains possible at the parked post.
TradePlan plan_stow(const TradeContext &context);

// Records the wagon's last transform and health (disconnect, escape, despawn).
TradePlan plan_wagon_state(const TradeContext &context, const std::optional<WorldPosition> &wagon,
                           int health, bool spawned_now);

// True when the wagon is inside the post's async sphere (the client uses 0.9 x radius).
bool wagon_inside_post(const TradingPost &post, const WorldPosition &wagon);

// True when `position` is within kTradingPostReachMm of the post's departure point.
bool player_near_post(const TradingPost &post, const WorldPosition &position);

// The placed post on the player's map that the player stands at, if any.
std::optional<int> post_at(const TradeCatalog &catalog, const WorldPosition &position);

// Market state for one post and goods group at `now`: deterministic per update window, so every
// session reports the same prices (Candidate model inside the GameVariable bands).
struct MarketQuote {
  std::int64_t basic_basis_points{};
  std::optional<int> event_pattern_id;
  std::int64_t event_basis_points{};
  std::int64_t total_basis_points{};
};

MarketQuote market_quote(const TradeCatalog &catalog, int post_id, int group,
                         std::int64_t now_epoch_ticks);

// The end of the current market window in 100 ns Unix-epoch ticks.
std::int64_t next_market_update(const TradeCatalog &catalog, std::int64_t now_epoch_ticks);

// PS2C_TradeMarketPriceRes for every placed trading post.
nlohmann::json market_price_message(const TradeCatalog &catalog, std::int64_t now_epoch_ticks);

// Wire forms.
nlohmann::json trade_goods_json(const std::vector<TradeGoodUnit> &goods);

// ItemsUpdateNoti for a committed load (reason trade_loading).
GameplayPacket loading_packet(const TradePlan &plan);

// ItemsUpdateNoti for a committed sale (reason trade_sell), with committed reward instances.
GameplayPacket selling_packet(const TradePlan &plan,
                              const std::vector<CharacterItem> &committed_grants);

GameplayPacket trade_start_packet(const WorldProgress &progress);

GameplayPacket wagon_position_packet(const WorldPosition &wagon);

GameplayPacket error_packet(std::uint32_t request_sequence, x5::ec::EErrorCode code);

} // namespace chronicles::trade
