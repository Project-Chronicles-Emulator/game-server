#include "world/features/shop/npc_shop.hpp"

#include "world/core/inventory_slots.hpp"
#include "world/core/progress_packets.hpp"
#include "persistence/item_wire.hpp"
#include "world/features/shop/shop_math.hpp"
#include "world/core/unit_registry.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using x5::ec::EErrorCode;

constexpr std::int64_t kBasisPointsDivisor = 10'000;
constexpr std::int64_t kEpochTicksPerHour = 36'000'000'000;
constexpr std::int64_t kEpochTicksPerMinute = 600'000'000;
// Voucher ids pack the character id above this many bits of per-character sequence.
constexpr int kVoucherSequenceBits = 20;
constexpr const char *kTokenMoneyTypeName = "EX5MoneyType_HersanderToken";

bool purchase_window_active(const ShopPurchaseRecord &record, std::int64_t now_epoch_ticks) {
  return record.reset_epoch_ticks == 0 || now_epoch_ticks < record.reset_epoch_ticks;
}

// Unit ids follow the registry's strict decimal rules (non-zero, at most INT64_MAX).
std::optional<std::uint64_t> parse_decimal_unit_id(const Json &value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  return parse_unit_id_text(value.get_ref<const std::string &>());
}

std::optional<std::int64_t> parse_decimal_item_id(const Json &value) {
  if (!value.is_string()) {
    return std::nullopt;
  }

  const auto &text = value.get_ref<const std::string &>();
  std::int64_t item_id{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), item_id);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      item_id <= 0) {
    return std::nullopt;
  }
  return item_id;
}

// Proto3 JSON omits zero integers, so an absent field reads as zero.
std::optional<int> optional_wire_int(const Json &object, const char *field) {
  const auto found = object.find(field);
  if (found == object.end()) {
    return 0;
  }
  if (!found->is_number_integer()) {
    return std::nullopt;
  }

  const auto value = found->get<std::int64_t>();
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

ShopRequestParse parse_failure(EErrorCode code) {
  ShopRequestParse result;
  result.error = code;
  return result;
}

EErrorCode parse_purchase_lines(const Json &entries, std::vector<ShopPurchaseLine> &lines) {
  std::set<int> seen_items;
  for (const auto &entry : entries) {
    if (!entry.is_object()) {
      return x5::ec::BadRequest;
    }

    const auto item_cms_id = optional_wire_int(entry, "item_cms_id");
    const auto count = optional_wire_int(entry, "count");
    if (!item_cms_id || *item_cms_id <= 0) {
      return x5::ec::ItemCmsIdInvalid;
    }
    if (!count || *count <= 0) {
      return x5::ec::ItemCountError;
    }
    if (!seen_items.insert(*item_cms_id).second) {
      return x5::ec::BadRequest;
    }
    lines.push_back({*item_cms_id, *count});
  }
  return x5::ec::Success;
}

// Parses ItemIdentity.stackable into the sale line.
EErrorCode parse_stack_sale(const Json &stack, ShopSaleLine &line) {
  if (!stack.is_object()) {
    return x5::ec::BadRequest;
  }
  const auto item_cms_id = optional_wire_int(stack, "item_cms_id");
  const auto count = optional_wire_int(stack, "count");
  if (!item_cms_id || *item_cms_id <= 0) {
    return x5::ec::ItemCmsIdInvalid;
  }
  if (!count || *count <= 0) {
    return x5::ec::ItemCountError;
  }
  line.item_cms_id = *item_cms_id;
  line.count = *count;
  return x5::ec::Success;
}

EErrorCode parse_sale_lines(const Json &entries, std::vector<ShopSaleLine> &lines) {
  std::set<std::int64_t> seen_instances;
  std::set<std::int64_t> seen_vouchers;
  std::set<int> seen_stacks;
  for (const auto &entry : entries) {
    if (!entry.is_object()) {
      return x5::ec::BadRequest;
    }
    // ItemIdentity is a oneof: exactly one identity form per line.
    const int identity_forms = static_cast<int>(entry.contains("item_id")) +
                               static_cast<int>(entry.contains("stackable")) +
                               static_cast<int>(entry.contains("trade_voucher_id"));
    if (identity_forms != 1) {
      return x5::ec::BadRequest;
    }

    ShopSaleLine line;
    if (entry.contains("item_id")) {
      line.item_id = parse_decimal_item_id(entry.at("item_id"));
      if (!line.item_id) {
        return x5::ec::ItemIdInvalid;
      }
      if (!seen_instances.insert(*line.item_id).second) {
        return x5::ec::BadRequest;
      }
    } else if (entry.contains("trade_voucher_id")) {
      line.trade_voucher_id = parse_decimal_item_id(entry.at("trade_voucher_id"));
      if (!line.trade_voucher_id) {
        return x5::ec::ItemIdInvalid;
      }
      if (!seen_vouchers.insert(*line.trade_voucher_id).second) {
        return x5::ec::BadRequest;
      }
    } else {
      const auto stack_error = parse_stack_sale(entry.at("stackable"), line);
      if (stack_error != x5::ec::Success) {
        return stack_error;
      }
      if (!seen_stacks.insert(line.item_cms_id).second) {
        return x5::ec::BadRequest;
      }
    }
    lines.push_back(line);
  }
  return x5::ec::Success;
}

EErrorCode parse_repair_ids(const Json &repair, std::vector<std::int64_t> &item_ids) {
  const auto ids = repair.find("item_ids");
  if (!repair.is_object() || ids == repair.end() || !ids->is_array() || ids->empty() ||
      ids->size() > kMaximumShopRequestLines) {
    return x5::ec::BadRequest;
  }

  std::set<std::int64_t> seen;
  for (const auto &entry : *ids) {
    const auto item_id = parse_decimal_item_id(entry);
    if (!item_id) {
      return x5::ec::ItemIdInvalid;
    }
    if (!seen.insert(*item_id).second) {
      return x5::ec::BadRequest;
    }
    item_ids.push_back(*item_id);
  }
  return x5::ec::Success;
}

EErrorCode parse_shop_lists(const Json &shop, ShopRequest &request) {
  if (!shop.is_object()) {
    return x5::ec::BadRequest;
  }
  const Json empty_list = Json::array();
  const auto &buy_list = shop.contains("buy_list") ? shop.at("buy_list") : empty_list;
  const auto &sell_list = shop.contains("sell_list") ? shop.at("sell_list") : empty_list;
  if (!buy_list.is_array() || !sell_list.is_array() || buy_list.size() > kMaximumShopRequestLines ||
      sell_list.size() > kMaximumShopRequestLines) {
    return x5::ec::BadRequest;
  }
  if (!buy_list.empty() && !sell_list.empty()) {
    // Every captured request carried one list; a mixed trade has no observed reply.
    return x5::ec::Interaction_InvalidParamType;
  }

  if (!buy_list.empty()) {
    request.action = ShopAction::buy;
    return parse_purchase_lines(buy_list, request.purchases);
  }
  if (!sell_list.empty()) {
    request.action = ShopAction::sell;
    return parse_sale_lines(sell_list, request.sales);
  }
  request.action = ShopAction::open;
  return x5::ec::Success;
}

EErrorCode parse_voucher_group(const Json &voucher_group, ShopRequest &request) {
  if (!voucher_group.is_object()) {
    return x5::ec::BadRequest;
  }
  const auto good_cms_id = optional_wire_int(voucher_group, "good_cms_id");
  const auto count = optional_wire_int(voucher_group, "count");
  if (!good_cms_id || *good_cms_id <= 0) {
    return x5::ec::ItemCmsIdInvalid;
  }
  if (!count || *count <= 0 || *count > kMaximumVouchersPerRequest) {
    return x5::ec::ItemCountError;
  }
  request.action = ShopAction::voucher_purchase;
  request.voucher_good_cms_id = *good_cms_id;
  request.voucher_count = *count;
  return x5::ec::Success;
}

double distance_squared_mm(const WorldPosition &first, const WorldPosition &second) {
  const double delta_x = static_cast<double>(first.x_mm) - static_cast<double>(second.x_mm);
  const double delta_y = static_cast<double>(first.y_mm) - static_cast<double>(second.y_mm);
  const double delta_z = static_cast<double>(first.z_mm) - static_cast<double>(second.z_mm);
  return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;
}

bool service_matches_action(MerchantServiceKind kind, ShopAction action) {
  switch (action) {
  case ShopAction::open:
  case ShopAction::buy:
  case ShopAction::sell:
    return kind == MerchantServiceKind::shop;
  case ShopAction::repair:
    return kind == MerchantServiceKind::repair;
  case ShopAction::voucher_purchase:
    return kind == MerchantServiceKind::voucher_group;
  }
  return false;
}

// Maps a condition verdict to the wire error. Unevaluable conditions fail closed with their own
// code so they stay distinguishable from conditions the character does not meet.
EErrorCode condition_error(ConditionResult result) {
  switch (result) {
  case ConditionResult::met:
    return x5::ec::Success;
  case ConditionResult::unmet:
    return x5::ec::Interaction_ConditionsNotMatch;
  case ConditionResult::unsupported:
    return x5::ec::Interaction_InvalidData;
  }
  return x5::ec::Interaction_InvalidData;
}

// Applies per-currency debits or credits to a copy of the balances and records each change.
// Gold and Hersander Token have independent balances, so one request may touch both.
class MoneyLedger {
public:
  explicit MoneyLedger(const WorldProgress &progress) {
    balances_.gold = progress.gold;
    balances_.hersander_token = progress.hersander_token;
  }

  // Returns false when the running total would overflow int64.
  bool add(const Price &price) {
    const auto total = checked_add(totals_[price.currency], price.amount);
    if (!total) {
      return false;
    }
    totals_[price.currency] = *total;
    return true;
  }

  // Applies the accumulated totals as debits or credits and writes the new balances.
  EErrorCode settle(bool debit, WorldProgress &next, std::vector<MoneyChange> &changes) {
    for (const auto &[currency, amount] : totals_) {
      auto applied_amount = amount;
      if (!debit) {
        // CUSTOM: sale proceeds above Money.limitUserMax fill the wallet to the limit instead
        // of refusing the sale. Graduated characters hold the full wallet, so refusing blocked
        // every sale, including quest steps that ask for one (official rule not captured).
        const auto balance = currency == MoneyType::HersanderToken ? balances_.hersander_token
                                                                   : balances_.gold;
        applied_amount = std::min(amount, std::max<std::int64_t>(0, kMoneyLimit - balance));
      }
      if (applied_amount == 0) {
        continue;
      }
      const auto applied =
          apply_money_change(balances_, currency, debit ? -applied_amount : applied_amount);
      if (!applied.ok) {
        // Underflow is the currency module's proven MoneyNotEnough; an invalid stored balance
        // has no observed rejection code.
        return applied.error == MoneyErrorCode::MoneyNotEnough ? x5::ec::MoneyNotEnough
                                                               : x5::ec::ItemError;
      }
      changes.push_back(applied.change);
    }
    next.gold = balances_.gold;
    next.hersander_token = balances_.hersander_token;
    return x5::ec::Success;
  }

private:
  CurrencyBalances balances_;
  std::map<MoneyType, std::int64_t> totals_;
};

std::vector<int> free_inventory_slots(const Character &character) {
  std::set<int> occupied;
  for (const auto &item : character.items) {
    occupied.insert(item.inventory_slot);
  }

  std::vector<int> free_slots;
  for (int slot = 0; slot <= kMaximumInventorySlot; ++slot) {
    if (!occupied.contains(slot)) {
      free_slots.push_back(slot);
    }
  }
  return free_slots;
}

const CharacterItem *find_owned_item(const Character &character, std::int64_t item_id) {
  const auto found = std::ranges::find_if(
      character.items, [&](const CharacterItem &item) { return item.id == item_id; });
  return found == character.items.end() ? nullptr : &*found;
}

// Counts a purchase against Shop.limitAmount inside the current reset window.
EErrorCode record_limited_purchase(const MerchantTradeInput &input, int shop_id,
                                   const ShopGood &good, int count, WorldProgress &next) {
  if (!good.limit_amount) {
    return x5::ec::Success;
  }

  const ShopLimitKey key{shop_id, good.item_cms_id};
  const auto existing = next.shop_purchases.find(key);
  const bool window_active = existing != next.shop_purchases.end() &&
                             purchase_window_active(existing->second, input.now_epoch_ticks);
  const int already_purchased = window_active ? existing->second.purchased_count : 0;
  if (count > *good.limit_amount - already_purchased) {
    return x5::ec::PurchaseLimitExceeded;
  }

  ShopPurchaseRecord record;
  record.purchased_count = already_purchased + count;
  if (window_active) {
    record.reset_epoch_ticks = existing->second.reset_epoch_ticks;
  } else {
    record.reset_epoch_ticks =
        input.catalog.next_reset_epoch_ticks(good.reset_period, input.now_epoch_ticks);
  }
  next.shop_purchases[key] = record;
  return x5::ec::Success;
}

EErrorCode add_purchased_stack(ShopTrade &trade, int item_cms_id, int count) {
  auto &stackables = trade.next.stackables;
  if (!stackables.contains(item_cms_id) && stackables.size() >= kMaximumStackableKinds) {
    return x5::ec::ItemSlotLimit;
  }

  auto &owned = stackables[item_cms_id];
  if (owned > std::numeric_limits<int>::max() - count) {
    return x5::ec::ItemCountError;
  }
  owned += count;
  trade.changed_stackables.push_back(item_cms_id);
  return x5::ec::Success;
}

EErrorCode add_purchased_housing_item(ShopTrade &trade, int item_cms_id, int count) {
  auto &housing = trade.next.housing_items;
  if (!housing.contains(item_cms_id) && housing.size() >= kMaximumHousingItemKinds) {
    return x5::ec::ItemSlotLimit;
  }

  auto &owned = housing[item_cms_id];
  if (owned.quantity > std::numeric_limits<int>::max() - count) {
    return x5::ec::ItemCountError;
  }
  owned.quantity += count;
  trade.changed_housing_items.push_back(item_cms_id);
  return x5::ec::Success;
}

// Candidate server rule: the hard ceiling is safe weight x BagMaxWeightMultiplier / 100. The
// client's own use of the multiplier was not located, and 1004 (ItemWeightLimit, the code with
// a user-facing message) is chosen over 1024 (ItemNotEnoughFreeWeight) for a purchase.
EErrorCode check_weight_budget(const MerchantTradeInput &input, std::int64_t added_weight) {
  if (!input.weight_budget) {
    return x5::ec::Success;
  }
  const auto total = checked_add(input.weight_budget->carried_weight, added_weight);
  if (!total || *total > input.weight_budget->ceiling_weight) {
    return x5::ec::ItemWeightLimit;
  }
  return x5::ec::Success;
}

class PurchasePlanner {
public:
  PurchasePlanner(const MerchantTradeInput &input, ShopTrade &trade, int shop_id)
      : input_(input), trade_(trade), shop_id_(shop_id), ledger_(input.before),
        free_slots_(free_inventory_slots(input.character)) {}

  EErrorCode plan(const std::vector<ShopPurchaseLine> &lines) {
    for (const auto &line : lines) {
      const auto error = plan_line(line);
      if (error != x5::ec::Success) {
        return error;
      }
    }

    const auto weight_error = check_weight_budget(input_, added_weight_);
    if (weight_error != x5::ec::Success) {
      return weight_error;
    }
    return ledger_.settle(true, trade_.next, trade_.money_changes);
  }

private:
  EErrorCode plan_line(const ShopPurchaseLine &line) {
    const auto *good = input_.catalog.good(shop_id_, line.item_cms_id);
    const auto *item = input_.catalog.item(line.item_cms_id);
    if (!good || !item) {
      return x5::ec::ItemCmsIdInvalid;
    }
    if (good->condition_id) {
      const auto condition =
          input_.catalog.evaluate_good_condition(*good->condition_id, input_.conditions);
      if (condition != ConditionResult::met) {
        return condition_error(condition);
      }
    }
    if (line.count > input_.catalog.maximum_stack()) {
      return x5::ec::ItemCountError;
    }
    if (item->storage == ShopItemStorage::unsupported || !item->price) {
      // Categories without emulator storage (drawings, permits, recipes, quest items).
      return x5::ec::ItemError;
    }

    const auto cost =
        scaled_amount(item->price->amount, good->price_multiplier_per_mille, line.count);
    if (!cost || !ledger_.add({item->price->currency, *cost})) {
      return x5::ec::Interaction_InvalidData;
    }
    const auto weight = checked_multiply(item->unit_weight, line.count);
    const auto total_weight = weight ? checked_add(added_weight_, *weight) : std::nullopt;
    if (!total_weight) {
      return x5::ec::Interaction_InvalidData;
    }
    added_weight_ = *total_weight;

    const auto limit_error =
        record_limited_purchase(input_, shop_id_, *good, line.count, trade_.next);
    if (limit_error != x5::ec::Success) {
      return limit_error;
    }
    return store_purchase(*item, line.count);
  }

  EErrorCode store_purchase(const ShopItemDefinition &item, int count) {
    switch (item.storage) {
    case ShopItemStorage::stackable:
      return add_purchased_stack(trade_, item.cms_id, count);
    case ShopItemStorage::housing:
      return add_purchased_housing_item(trade_, item.cms_id, count);
    case ShopItemStorage::instance:
      return add_purchased_instances(item, count);
    case ShopItemStorage::unsupported:
      break;
    }
    return x5::ec::ItemError;
  }

  EErrorCode add_purchased_instances(const ShopItemDefinition &item, int count) {
    if (item.max_durability <= 0) {
      return x5::ec::Interaction_InvalidData;
    }
    const auto instance_count = trade_.granted_items.size() + static_cast<std::size_t>(count);
    if (instance_count > static_cast<std::size_t>(kMaximumPurchasedInstances)) {
      return x5::ec::ItemCountError;
    }

    for (int index = 0; index < count; ++index) {
      if (next_free_slot_ >= free_slots_.size()) {
        return x5::ec::ItemSlotLimit;
      }
      CharacterItem granted;
      granted.cms_id = item.cms_id;
      granted.inventory_slot = free_slots_[next_free_slot_];
      granted.durability = item.max_durability;
      granted.grade = item.grade;
      granted.created_epoch_ticks = input_.now_epoch_ticks;
      ++next_free_slot_;
      trade_.granted_items.push_back(granted);
    }
    return x5::ec::Success;
  }

  const MerchantTradeInput &input_;
  ShopTrade &trade_;
  int shop_id_{};
  MoneyLedger ledger_;
  std::vector<int> free_slots_;
  std::size_t next_free_slot_{};
  std::int64_t added_weight_{};
};

class SalePlanner {
public:
  SalePlanner(const MerchantTradeInput &input, ShopTrade &trade, int shop_id)
      : input_(input), trade_(trade), shop_id_(shop_id), ledger_(input.before) {}

  EErrorCode plan(const std::vector<ShopSaleLine> &lines) {
    for (const auto &line : lines) {
      const auto error = plan_line(line);
      if (error != x5::ec::Success) {
        return error;
      }
    }
    // A credit that would pass Money.limitUserMax is refused by the ledger.
    return ledger_.settle(false, trade_.next, trade_.money_changes);
  }

private:
  EErrorCode plan_line(const ShopSaleLine &line) {
    if (line.trade_voucher_id) {
      return plan_voucher(*line.trade_voucher_id);
    }
    if (line.item_id) {
      return plan_instance(*line.item_id);
    }
    return plan_counted_item(line.item_cms_id, line.count);
  }

  // Credit = Item.refundValue x count, scaled by the selling shop's refund multiplier when that
  // shop also sells the item.
  EErrorCode credit_item(const ShopItemDefinition &item, int count) {
    if (!item.refund) {
      return x5::ec::ItemNonSellable;
    }
    const auto *good = input_.catalog.good(shop_id_, item.cms_id);
    const auto multiplier = good ? good->refund_multiplier_per_mille : kPerMille;
    const auto credit = scaled_amount(item.refund->amount, multiplier, count);
    if (!credit || !ledger_.add({item.refund->currency, *credit})) {
      return x5::ec::Interaction_InvalidData;
    }
    return x5::ec::Success;
  }

  EErrorCode plan_instance(std::int64_t item_id) {
    const auto *owned = find_owned_item(input_.character, item_id);
    if (!owned) {
      return x5::ec::ItemIdInvalid;
    }
    if (owned->equip_slot) {
      return x5::ec::ItemEquipped;
    }
    const auto *item = input_.catalog.item(owned->cms_id);
    if (!item) {
      return x5::ec::ItemCmsIdInvalid;
    }
    const auto error = credit_item(*item, owned->quantity);
    if (error != x5::ec::Success) {
      return error;
    }
    trade_.removed_items.push_back(*owned);
    return x5::ec::Success;
  }

  // A stackable or housing-object line: ItemIdentity.stackable.
  EErrorCode plan_counted_item(int item_cms_id, int count) {
    const auto stack = trade_.next.stackables.find(item_cms_id);
    const auto housing = trade_.next.housing_items.find(item_cms_id);
    const bool owns_stack = stack != trade_.next.stackables.end();
    const bool owns_housing = housing != trade_.next.housing_items.end();
    if (owns_stack) {
      if (stack->second < count) {
        return x5::ec::ItemCountError;
      }
      // Candidate: quick-slot allocations must stay backed by owned stock, as
      // UtilityBagSetReq validation requires; the official sale behavior is unobserved.
      if (stack->second - count < reserved_utility_count(trade_.next, item_cms_id)) {
        return x5::ec::ItemLocked;
      }
    } else if (owns_housing) {
      // Candidate: only housing objects that are not installed can be sold.
      if (housing->second.quantity - housing->second.installed_count < count) {
        return x5::ec::ItemCountError;
      }
    } else {
      return x5::ec::ItemCountError;
    }

    const auto *item = input_.catalog.item(item_cms_id);
    if (!item) {
      return x5::ec::ItemCmsIdInvalid;
    }
    const auto error = credit_item(*item, count);
    if (error != x5::ec::Success) {
      return error;
    }

    if (owns_stack) {
      stack->second -= count;
      trade_.changed_stackables.push_back(item_cms_id);
    } else {
      housing->second.quantity -= count;
      trade_.changed_housing_items.push_back(item_cms_id);
    }
    return x5::ec::Success;
  }

  // Candidate: a voucher refunds TradeGood.basicValue x GameVariable.TradeRefundBasisPoints /
  // 10000. TradeRefundBasisPoints is the only refund rate the catalog defines for vouchers.
  EErrorCode plan_voucher(std::int64_t voucher_id) {
    const auto voucher = trade_.next.trade_vouchers.find(voucher_id);
    if (voucher == trade_.next.trade_vouchers.end() || voucher->second.redeemed) {
      return x5::ec::ItemIdInvalid;
    }
    if (voucher->second.expiration_epoch_ticks <= input_.now_epoch_ticks) {
      return x5::ec::ItemHasExpired;
    }
    const auto *good = input_.catalog.trade_good(voucher->second.good_cms_id);
    const auto basis_points = input_.catalog.trade_refund_basis_points();
    if (!good || !basis_points) {
      return x5::ec::ItemNonSellable;
    }
    const auto scaled = checked_multiply(good->basic_value, *basis_points);
    if (!scaled || !ledger_.add({MoneyType::Gold, *scaled / kBasisPointsDivisor})) {
      return x5::ec::Interaction_InvalidData;
    }

    voucher->second.redeemed = true;
    trade_.redeemed_voucher_ids.push_back(voucher_id);
    return x5::ec::Success;
  }

  const MerchantTradeInput &input_;
  ShopTrade &trade_;
  int shop_id_{};
  MoneyLedger ledger_;
};

// Repair cost per item = ceil(ItemRepairCost.costValue x missing durability / max durability).
// Observed: per-item rounding up reproduces the totals of both captured multi-item repairs
// (-207 for nine items) and the single-item -5. Tools (max durability 2000) use the same
// proportion; that case is unobserved.
EErrorCode plan_repairs(const MerchantTradeInput &input, const std::vector<std::int64_t> &item_ids,
                        ShopTrade &trade) {
  MoneyLedger ledger(input.before);
  for (const auto item_id : item_ids) {
    const auto *owned = find_owned_item(input.character, item_id);
    if (!owned) {
      return x5::ec::ItemIdInvalid;
    }
    const auto *item = input.catalog.item(owned->cms_id);
    if (!item || !item->npc_repairable) {
      return x5::ec::ItemNotNpcRepairable;
    }

    // The client lists every equipped item in a repair-all request, so undamaged items are
    // skipped instead of failing the request (Candidate; the captured lists were not
    // checked against item wear).
    const int missing = item->max_durability - owned->durability;
    if (missing <= 0) {
      continue;
    }

    const auto scaled = checked_multiply(item->full_repair_cost, missing);
    const auto cost = scaled ? checked_ceiling_divide(*scaled, item->max_durability) : std::nullopt;
    if (!cost || !ledger.add({MoneyType::Gold, *cost})) {
      return x5::ec::Interaction_InvalidData;
    }

    CharacterItem repaired = *owned;
    repaired.durability = item->max_durability;
    trade.durability_updates.push_back({owned->id, owned->durability, repaired.durability});
    trade.repaired_items.push_back(repaired);
  }

  if (trade.repaired_items.empty()) {
    return x5::ec::ItemNotRepairable;
  }
  return ledger.settle(true, trade.next, trade.money_changes);
}

// Trade-run voucher rules: the own limit counts live (unredeemed, unexpired) vouchers, and the
// production materials leave the character's stacks with the fee.
EErrorCode plan_voucher_production(const MerchantTradeInput &input,
                                   const TradeGoodDefinition &good, int count, ShopTrade &trade) {
  const auto &rules = *input.trade_catalog;
  const auto *production = rules.production_cost(good.id);
  if (!production || production->currency != MoneyType::Gold) {
    return x5::ec::TradeGood_NotCraftable;
  }

  const auto live = std::ranges::count_if(trade.next.trade_vouchers, [&](const auto &entry) {
    return !entry.second.redeemed && entry.second.expiration_epoch_ticks > input.now_epoch_ticks;
  });
  if (live + count > rules.settings().voucher_own_limit) {
    return x5::ec::TradeVoucher_OwnMaxLimit;
  }

  for (const auto &material : production->materials) {
    const auto needed = checked_multiply(material.count, count);
    const auto stack = trade.next.stackables.find(material.item_cms_id);
    if (!needed || stack == trade.next.stackables.end() || stack->second < *needed) {
      return x5::ec::Craft_MaterialNotEnough;
    }
    stack->second -= static_cast<int>(*needed);
    if (std::ranges::find(trade.changed_stackables, material.item_cms_id) ==
        trade.changed_stackables.end()) {
      trade.changed_stackables.push_back(material.item_cms_id);
    }
  }
  return x5::ec::Success;
}

// Candidate: no capture of a voucher purchase exists. Without trade-run rules the price is
// TradeGood.basicValue Gold per voucher and expiry is TradeGood.validPeriod hours after issue.
// With them (the server's normal configuration) a voucher costs its MakeTradeGood production fee
// and materials (the client shows "Production Fee" and "Crafting Material"), lasts validPeriod
// minutes (1440 for the demo goods) and at most TradeVoucherOwnMaxLimit live vouchers are held
// (the client checks the same limit, message 241). Ids pack the character id above a
// per-character sequence so they never collide.
EErrorCode plan_voucher_purchase(const MerchantTradeInput &input, int group_id,
                                 const ShopRequest &request, ShopTrade &trade) {
  const auto *offers = input.catalog.voucher_group(group_id);
  if (!offers) {
    return x5::ec::Interaction_InvalidData;
  }
  const auto offer = std::ranges::find_if(*offers, [&](const VoucherOffer &candidate) {
    return candidate.trade_good_id == request.voucher_good_cms_id;
  });
  const auto *good = input.catalog.trade_good(request.voucher_good_cms_id);
  if (offer == offers->end() || !good) {
    return x5::ec::ItemCmsIdInvalid;
  }

  if (offer->condition_id) {
    auto context = input.conditions;
    context.subject_company = good->company;
    const auto condition = input.catalog.evaluate_voucher_condition(*offer->condition_id, context);
    if (condition != ConditionResult::met) {
      return condition_error(condition);
    }
  }

  const auto existing = trade.next.trade_vouchers.size();
  if (existing + static_cast<std::size_t>(request.voucher_count) > kMaximumTradeVouchers) {
    return x5::ec::ItemSlotLimit;
  }

  std::optional<std::int64_t> period_ticks;
  std::optional<std::int64_t> cost;
  if (input.trade_catalog) {
    const auto error = plan_voucher_production(input, *good, request.voucher_count, trade);
    if (error != x5::ec::Success) {
      return error;
    }
    const auto *production = input.trade_catalog->production_cost(good->id);
    period_ticks = checked_multiply(good->valid_period, kEpochTicksPerMinute);
    cost = checked_multiply(production->fee, request.voucher_count);
  } else {
    period_ticks = checked_multiply(good->valid_period, kEpochTicksPerHour);
    cost = checked_multiply(good->basic_value, request.voucher_count);
  }
  const auto expiration =
      period_ticks ? checked_add(input.now_epoch_ticks, *period_ticks) : std::nullopt;
  MoneyLedger ledger(input.before);
  if (!expiration || !cost || !ledger.add({MoneyType::Gold, *cost})) {
    return x5::ec::Interaction_InvalidData;
  }

  for (int index = 0; index < request.voucher_count; ++index) {
    const std::int64_t sequence = static_cast<std::int64_t>(existing) + index + 1;
    const std::int64_t voucher_id = (input.character.id << kVoucherSequenceBits) + sequence;
    trade.next.trade_vouchers[voucher_id] = {good->id, input.now_epoch_ticks, *expiration, false};
    trade.issued_voucher_ids.push_back(voucher_id);
  }
  return ledger.settle(true, trade.next, trade.money_changes);
}

GameplayPacket interaction_response(const ShopRequest &request) {
  return {kInteractUnitResponseOpcode,
          {{"target_unit_id", std::to_string(request.target_unit_id)},
           {"interaction_id", request.interaction_id},
           {"packet_seq", request.packet_seq}}};
}

// Unlike the shared world/core money_change_json, a non-Gold shop change always names the
// Hersander Token, the only other shop currency.
Json shop_money_change_json(const MoneyChange &change) {
  Json entry{{"change", std::to_string(change.change)}, {"result", std::to_string(change.result)}};
  if (!change.omit_type_on_wire) {
    entry["money_type"] = kTokenMoneyTypeName;
  }
  return entry;
}

Json voucher_json(std::int64_t voucher_id, const TradeVoucherRecord &record) {
  return {{"voucher_id", std::to_string(voucher_id)},
          {"good_cms_id", record.good_cms_id},
          {"created_epochtime", std::to_string(record.created_epoch_ticks)},
          {"expiration_epochtime", std::to_string(record.expiration_epoch_ticks)}};
}

// The client removes a voucher from its list when it receives an entry with the voucher id and
// no good_cms_id (AAC_Trade_FX5TradeManager_ApplyVoucherDelta, high confidence), and applies such
// deltas for the discard, trade_loading and trade_voucher reasons only. A sold voucher is
// therefore removed with a discard-reason update after the shop update; whether the original
// server pairs them this way is unobserved (Candidate).
GameplayPacket voucher_removal_packet(const std::vector<std::int64_t> &voucher_ids) {
  Json vouchers = Json::array();
  for (const auto voucher_id : voucher_ids) {
    vouchers.push_back({{"voucher_id", std::to_string(voucher_id)}});
  }
  return {kItemsUpdateNotificationOpcode, {{"discard", {{"vouchers", std::move(vouchers)}}}}};
}

} // namespace

ItemTrade ShopTrade::storage_changes() const {
  ItemTrade changes;
  changes.granted_items = granted_items;
  for (const auto &item : removed_items) {
    changes.removed_item_ids.push_back(item.id);
  }
  changes.durability_updates = durability_updates;
  return changes;
}

bool is_merchant_request(const Json &message) {
  if (!message.is_object()) {
    return false;
  }
  const auto param = message.find("param");
  return param != message.end() && param->is_object() &&
         (param->contains("shop") || param->contains("repair") || param->contains("voucher_group"));
}

ShopRequestParse parse_shop_request(const Json &message) {
  if (!is_merchant_request(message)) {
    return parse_failure(x5::ec::BadRequest);
  }

  ShopRequestParse result;
  auto &request = result.request;
  const auto target = message.contains("target_unit_id")
                          ? parse_decimal_unit_id(message.at("target_unit_id"))
                          : std::nullopt;
  const auto &param = message.at("param");
  const auto interaction_id = optional_wire_int(param, "interaction_id");
  if (!target || !interaction_id || *interaction_id <= 0) {
    return parse_failure(x5::ec::BadRequest);
  }
  request.target_unit_id = *target;
  request.interaction_id = *interaction_id;

  if (message.contains("packet_seq") && message.at("packet_seq").is_object()) {
    request.packet_seq = message.at("packet_seq");
  }

  // InteractionParam.value is a oneof, so exactly one merchant variant can be present.
  EErrorCode error = x5::ec::Success;
  if (param.contains("shop")) {
    error = parse_shop_lists(param.at("shop"), request);
  } else if (param.contains("repair")) {
    request.action = ShopAction::repair;
    error = parse_repair_ids(param.at("repair"), request.repair_item_ids);
  } else {
    error = parse_voucher_group(param.at("voucher_group"), request);
  }
  if (error != x5::ec::Success) {
    return parse_failure(error);
  }
  return result;
}

MerchantAccess authorize_merchant_access(const ShopCatalog &catalog, const MerchantTarget *target,
                                         const ShopRequest &request,
                                         const std::optional<WorldPosition> &player_position,
                                         const ConditionContext &conditions) {
  MerchantAccess access;
  if (!player_position) {
    access.error = x5::ec::BadRequest;
    return access;
  }
  if (!target || target->unit_id != request.target_unit_id ||
      target->position.map_id != player_position->map_id) {
    access.error = x5::ec::Interaction_TargetNotFound;
    return access;
  }

  const auto service = catalog.service_for(target->npc_cms_id, request.interaction_id);
  if (!service) {
    access.error = x5::ec::Interaction_GroupIdNotMatch;
    return access;
  }
  if (!service_matches_action(service->kind, request.action)) {
    access.error = x5::ec::Interaction_InvalidParamType;
    return access;
  }
  if (service->condition_id) {
    const auto condition =
        catalog.evaluate_interaction_condition(*service->condition_id, conditions);
    if (condition != ConditionResult::met) {
      access.error = condition_error(condition);
      return access;
    }
  }

  const double range_squared = kMerchantInteractionRangeMm * kMerchantInteractionRangeMm;
  if (distance_squared_mm(*player_position, target->position) > range_squared) {
    access.error = x5::ec::TooFar;
    return access;
  }

  access.service = *service;
  return access;
}

ShopTradePlan plan_merchant_trade(const MerchantTradeInput &input, const MerchantService &service,
                                  const ShopRequest &request) {
  ShopTradePlan plan;
  if (request.action == ShopAction::open || input.now_epoch_ticks <= 0 ||
      input.before.revision == std::numeric_limits<std::int64_t>::max()) {
    plan.error = x5::ec::BadRequest;
    return plan;
  }
  if (input.before.dead) {
    plan.error = x5::ec::Interaction_Invalid;
    return plan;
  }

  plan.trade.service = service;
  plan.trade.next = input.before;
  ++plan.trade.next.revision;

  switch (request.action) {
  case ShopAction::buy:
    plan.error = PurchasePlanner(input, plan.trade, service.target_id).plan(request.purchases);
    break;
  case ShopAction::sell:
    plan.error = SalePlanner(input, plan.trade, service.target_id).plan(request.sales);
    break;
  case ShopAction::repair:
    plan.error = plan_repairs(input, request.repair_item_ids, plan.trade);
    break;
  case ShopAction::voucher_purchase:
    plan.error = plan_voucher_purchase(input, service.target_id, request, plan.trade);
    break;
  case ShopAction::open:
    plan.error = x5::ec::BadRequest;
    break;
  }
  return plan;
}

std::vector<GameplayPacket> shop_open_packets(const ShopCatalog &catalog,
                                              const ShopRequest &request, int shop_id,
                                              const WorldProgress &progress,
                                              std::int64_t now_epoch_ticks) {
  // ShopItemLimit.count is the purchased count: the observed open of a shop whose goods are
  // all limited, before any purchase, carried an empty list.
  Json limits = Json::array();
  if (const auto *goods = catalog.goods(shop_id)) {
    for (const auto &[item_cms_id, good] : *goods) {
      if (!good.limit_amount) {
        continue;
      }
      const auto record = progress.shop_purchases.find({shop_id, item_cms_id});
      if (record == progress.shop_purchases.end() || record->second.purchased_count == 0 ||
          !purchase_window_active(record->second, now_epoch_ticks)) {
        continue;
      }

      Json limit{{"item_cms_id", item_cms_id}, {"count", record->second.purchased_count}};
      if (record->second.reset_epoch_ticks != 0) {
        limit["reset_epochtime"] = std::to_string(record->second.reset_epoch_ticks);
      }
      limits.push_back(std::move(limit));
    }
  }

  return {interaction_response(request),
          {kShopItemLimitNotificationOpcode, {{"shop_id", shop_id}, {"limits", limits}}}};
}

std::vector<GameplayPacket>
merchant_trade_packets(const ShopRequest &request, const ShopTrade &trade,
                       const std::vector<CharacterItem> &committed_grants) {
  Json items = Json::array();
  for (const auto &item : committed_grants) {
    items.push_back(item_instance_json(item));
  }
  for (const auto &item : trade.repaired_items) {
    items.push_back(item_instance_json(item));
  }
  // A sold instance is reported by identity only, as in the captured equipment sale.
  for (const auto &item : trade.removed_items) {
    items.push_back({{"item_id", std::to_string(item.id)}, {"item_cms_id", item.cms_id}});
  }

  // ItemStackable.quantity is the resulting total, as in other ItemsUpdateNoti reasons.
  Json stackables = Json::array();
  for (const int item_cms_id : trade.changed_stackables) {
    stackables.push_back(
        {{"item_cms_id", item_cms_id}, {"quantity", trade.next.stackables.at(item_cms_id)}});
  }

  // Candidate: housing counts follow the stackable convention (resulting totals); no capture of
  // a housing_items update exists.
  Json housing_items = Json::array();
  for (const int item_cms_id : trade.changed_housing_items) {
    const auto &record = trade.next.housing_items.at(item_cms_id);
    housing_items.push_back(housing_item_count_json(item_cms_id, record));
  }

  Json update{{"packet_seq", request.packet_seq}};
  Json money_changes = Json::array();
  for (const auto &change : trade.money_changes) {
    money_changes.push_back(shop_money_change_json(change));
  }
  if (!money_changes.empty()) {
    update["money_changes"] = std::move(money_changes);
  }
  if (!items.empty()) {
    update["items"] = std::move(items);
  }
  if (!stackables.empty()) {
    update["stackables"] = std::move(stackables);
  }
  if (!housing_items.empty()) {
    update["housing_items"] = std::move(housing_items);
  }

  // The reason is a oneof. Shop, repair and voucher sales were captured (or are assumed) under
  // `shop`; a voucher purchase uses `trade_voucher`, which carries the issued vouchers
  // (Candidate; no capture).
  if (trade.issued_voucher_ids.empty()) {
    update["shop"] = Json::object();
  } else {
    Json vouchers = Json::array();
    for (const auto voucher_id : trade.issued_voucher_ids) {
      vouchers.push_back(voucher_json(voucher_id, trade.next.trade_vouchers.at(voucher_id)));
    }
    update["trade_voucher"] = {{"vouchers", std::move(vouchers)}};
  }

  std::vector<GameplayPacket> packets;
  packets.push_back({kItemsUpdateNotificationOpcode, std::move(update)});
  if (!trade.redeemed_voucher_ids.empty()) {
    packets.push_back(voucher_removal_packet(trade.redeemed_voucher_ids));
  }
  packets.push_back(interaction_response(request));
  return packets;
}

GameplayPacket shop_error_packet(std::uint32_t request_sequence, EErrorCode code) {
  return {kCommonErrorResponseOpcode,
          {{"ec", x5::ec::EErrorCode_Name(code)},
           {"packet_seq", {{"seq", std::to_string(request_sequence)}}}}};
}

} // namespace chronicles
