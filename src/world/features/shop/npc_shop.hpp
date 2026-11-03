#pragma once

#include "common/currency.hpp"
#include "ec.generated.pb.h"
#include "data/shop_catalog.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"
#include "data/trade_catalog.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/core/world_request_opcodes.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

// NPC merchants use PC2S_InteractUnitReq with InteractionParam.shop, .repair or .voucher_group.
// Official captures show an empty shop param (open), shop buy_list / sell_list, and repair
// item_ids. The server answers a trade with ItemsUpdateNoti (reason `shop`) followed by
// InteractUnitRes, and an open with InteractUnitRes followed by ShopItemLimitNoti.
// voucher_group has no capture; its handling is Candidate.
inline constexpr std::uint16_t kInteractUnitRequestOpcode = world_request::kInteractionUnitRequest;
inline constexpr std::uint16_t kCommonErrorResponseOpcode = 2;
inline constexpr std::uint16_t kInteractUnitResponseOpcode = 163;
inline constexpr std::uint16_t kItemsUpdateNotificationOpcode = 169;
inline constexpr std::uint16_t kShopItemLimitNotificationOpcode = 171;

// Emulator request bound. Every captured buy or sell request carried a single line and the
// largest repair request carried nine items.
inline constexpr std::size_t kMaximumShopRequestLines = 32;

// Emulator bound on new item rows from one purchase; storage accepts at most 64 per trade.
inline constexpr int kMaximumPurchasedInstances = 32;

// Emulator bound on trade vouchers issued by one request (Candidate; no capture).
inline constexpr int kMaximumVouchersPerRequest = 10;

// Same three-dimensional NPC interaction radius the tutorial session applies.
inline constexpr double kMerchantInteractionRangeMm = 7000.0;

// A merchant NPC unit the server announced to this session.
struct MerchantTarget {
  std::uint64_t unit_id{};
  int npc_cms_id{};
  WorldPosition position;
};

enum class ShopAction { open, buy, sell, repair, voucher_purchase };

struct ShopPurchaseLine {
  int item_cms_id{};
  int count{};
};

// Exactly one of an owned instance, a stackable (or housing) CMS ID with a count, or a trade
// voucher.
struct ShopSaleLine {
  std::optional<std::int64_t> item_id;
  std::optional<std::int64_t> trade_voucher_id;
  int item_cms_id{};
  int count{};
};

struct ShopRequest {
  std::uint64_t target_unit_id{};
  int interaction_id{};
  ShopAction action{ShopAction::open};
  std::vector<ShopPurchaseLine> purchases;
  std::vector<ShopSaleLine> sales;
  std::vector<std::int64_t> repair_item_ids;
  int voucher_good_cms_id{};
  int voucher_count{};
  // Echoed in replies; official replies carried an empty PacketSeq.
  nlohmann::json packet_seq = nlohmann::json::object();
};

struct ShopRequestParse {
  x5::ec::EErrorCode error{x5::ec::Success};
  ShopRequest request;
};

// True for an InteractUnitReq whose param selects the shop, repair or voucher_group variant.
bool is_merchant_request(const nlohmann::json &message);

ShopRequestParse parse_shop_request(const nlohmann::json &message);

struct MerchantAccess {
  x5::ec::EErrorCode error{x5::ec::Success};
  MerchantService service;
};

// Validates the target merchant, that its interaction matches the requested action, the
// interaction's conditions, and the character's range.
MerchantAccess authorize_merchant_access(const ShopCatalog &catalog, const MerchantTarget *target,
                                         const ShopRequest &request,
                                         const std::optional<WorldPosition> &player_position,
                                         const ConditionContext &conditions);

// Carried weight and the hard ceiling the emulator enforces on purchases.
struct WeightBudget {
  std::int64_t carried_weight{};
  std::int64_t ceiling_weight{};
};

struct MerchantTradeInput {
  const ShopCatalog &catalog;
  const Character &character;
  const WorldProgress &before;
  std::int64_t now_epoch_ticks{};
  ConditionContext conditions;
  std::optional<WeightBudget> weight_budget;
  // Trade-run rules. When present, a voucher costs its MakeTradeGood production fee and
  // materials, lasts TradeGood.validPeriod minutes and respects TradeVoucherOwnMaxLimit.
  const trade::TradeCatalog *trade_catalog{};
};

// One validated trade ready for an atomic storage commit.
struct ShopTrade {
  MerchantService service;
  WorldProgress next;
  std::vector<CharacterItem> granted_items;
  std::vector<CharacterItem> removed_items;
  // Items whose durability a repair restores, with their post-repair state.
  std::vector<CharacterItem> repaired_items;
  std::vector<ItemDurabilityUpdate> durability_updates;
  std::vector<MoneyChange> money_changes;
  std::vector<int> changed_stackables;
  std::vector<int> changed_housing_items;
  std::vector<std::int64_t> issued_voucher_ids;
  std::vector<std::int64_t> redeemed_voucher_ids;

  ItemTrade storage_changes() const;
};

struct ShopTradePlan {
  x5::ec::EErrorCode error{x5::ec::Success};
  ShopTrade trade;
};

// Plans a buy, sell, repair or voucher purchase against the character's owned items and
// progress. No state changes.
ShopTradePlan plan_merchant_trade(const MerchantTradeInput &input, const MerchantService &service,
                                  const ShopRequest &request);

// InteractUnitRes then ShopItemLimitNoti with the current window's purchased counts.
std::vector<GameplayPacket> shop_open_packets(const ShopCatalog &catalog,
                                              const ShopRequest &request, int shop_id,
                                              const WorldProgress &progress,
                                              std::int64_t now_epoch_ticks);

// ItemsUpdateNoti with the committed changes, then InteractUnitRes.
std::vector<GameplayPacket>
merchant_trade_packets(const ShopRequest &request, const ShopTrade &trade,
                       const std::vector<CharacterItem> &committed_grants);

// PS2C_CommonErrorRes carrying the rejection code and the request's frame sequence, the channel
// the server uses for every other rejected world request (the capture shows TooFar this way).
// Candidate: no official shop rejection was captured, so the specific codes are inferred.
GameplayPacket shop_error_packet(std::uint32_t request_sequence, x5::ec::EErrorCode code);

} // namespace chronicles
