#include "world/features/trade/trade_run.hpp"

#include "persistence/item_wire.hpp"
#include "world/core/progress_packets.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>

namespace chronicles::trade {
namespace {

using Json = nlohmann::json;
using x5::ec::EErrorCode;

constexpr std::size_t kMaximumLoadLines = 64;
constexpr int kMaximumLineCount = 64;
constexpr std::size_t kMaximumGrantedInstances = 32;
constexpr std::int64_t kBasisPointsPerWhole = 10000;
constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr std::int64_t kTicksPerMinute = 60LL * 1000 * kTicksPerMillisecond;
constexpr int kGoodsGroupCount = 7;

// Demand tiers of TradePatternDesc ids (thousands digit) and the price change the emulator
// applies for each. The client data names the tiers (surge, rising, stable, falling, plunging)
// but carries no numbers (Candidate).
constexpr std::array<std::int64_t, 6> kEventBasisPointsByTier = {0, 3000, 1500, 0, -1500, -3000};
constexpr std::array<int, 4> kEventTiers = {1, 2, 4, 5};
// One goods group in four has a market event in a window (Candidate).
constexpr std::uint64_t kEventChanceDivisor = 4;

std::optional<std::int64_t> parse_decimal(const Json &value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  const auto &text = value.get_ref<const std::string &>();
  if (text.empty() || text.size() > 19) {
    return std::nullopt;
  }

  std::int64_t parsed{};
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return std::nullopt;
    }
    parsed = parsed * 10 + (digit - '0');
  }
  return parsed;
}

std::optional<int> wire_int(const Json &object, const char *field) {
  const auto found = object.find(field);
  if (found == object.end() || !found->is_number_integer()) {
    return std::nullopt;
  }
  const auto value = found->get<std::int64_t>();
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

std::uint64_t mix(std::uint64_t value) {
  // splitmix64 finalizer: a stable, well-distributed hash of the market inputs.
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

std::int64_t market_window_ticks(const TradeCatalog &catalog) {
  return catalog.settings().price_update_interval_minutes * kTicksPerMinute;
}

const char *quality_name(GoodQuality quality) {
  switch (quality) {
  case GoodQuality::better:
    return "EX5TradeGoodQualityType_Better";
  case GoodQuality::good:
    return "EX5TradeGoodQualityType_Good";
  case GoodQuality::normal:
    return "EX5TradeGoodQualityType_Normal";
  case GoodQuality::bad:
    return "EX5TradeGoodQualityType_Bad";
  case GoodQuality::none:
    break;
  }
  return "EX5TradeGoodQualityType_None";
}

// The last tier whose start has passed (AAC_Trade_ComputeGoodQualityByElapsedTime).
const QualityTier *quality_tier(const TradeGood &good, std::int64_t produced,
                                std::int64_t now) {
  const QualityTier *current = nullptr;
  const auto elapsed_ms = std::max<std::int64_t>(0, now - produced) / kTicksPerMillisecond;
  for (const auto &tier : good.quality_tiers) {
    if (tier.begins_after_ms <= elapsed_ms) {
      current = &tier;
    }
  }
  return current;
}

std::int64_t scale(std::int64_t value, std::int64_t basis_points) {
  if (value <= 0 || basis_points <= 0) {
    return 0;
  }
  if (value > std::numeric_limits<std::int64_t>::max() / basis_points) {
    return std::numeric_limits<std::int64_t>::max() / kBasisPointsPerWhole;
  }
  return value * basis_points / kBasisPointsPerWhole;
}

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

// Applies a signed money delta to `next`, recording the wire change. False on insufficient
// funds or an invalid stored balance.
// CUSTOM: a credit above the wallet limit fills the wallet to the limit instead of failing the
// delivery. Graduated characters hold the full wallet, so failing made every trade delivery
// impossible for them (official rule not captured).
bool apply_money(WorldProgress &next, MoneyType currency, std::int64_t delta,
                 std::vector<MoneyChange> &changes) {
  auto &balance = currency == MoneyType::Gold ? next.gold : next.hersander_token;
  if (balance < 0 || balance > kMoneyLimit) {
    return false;
  }
  if (delta > 0) {
    delta = std::min(delta, kMoneyLimit - balance);
  }
  if (delta == 0) {
    return true;
  }
  if (delta < 0 && balance < -delta) {
    return false;
  }

  balance += delta;
  for (auto &change : changes) {
    if (change.type == currency) {
      change.change += delta;
      change.result = balance;
      return true;
    }
  }
  MoneyChange change;
  change.type = currency;
  change.change = delta;
  change.result = balance;
  change.omit_type_on_wire = currency == MoneyType::Gold;
  changes.push_back(change);
  return true;
}

void reset_run(TradeRunRecord &run) {
  const auto last_spawn = run.last_wagon_spawn_epoch_ticks;
  run = TradeRunRecord{};
  run.last_wagon_spawn_epoch_ticks = last_spawn;
}

// Grants reward items into `plan.next` (stacks and housing counts) or as new instances.
EErrorCode grant_reward_item(const TradeContext &context, TradePlan &plan, int item_cms_id,
                             std::int64_t amount, std::vector<int> &free_slots) {
  const auto *definition = context.items ? context.items->item(item_cms_id) : nullptr;
  if (!definition || amount <= 0 || amount > std::numeric_limits<int>::max()) {
    return x5::ec::ItemCmsIdInvalid;
  }

  const auto count = static_cast<int>(amount);
  switch (definition->storage) {
  case ShopItemStorage::stackable: {
    auto &stack = plan.next.stackables[item_cms_id];
    if (plan.next.stackables.size() > kMaximumStackableKinds ||
        stack > std::numeric_limits<int>::max() - count) {
      return x5::ec::ItemSlotLimit;
    }
    stack += count;
    plan.changed_stackables.push_back(item_cms_id);
    return x5::ec::Success;
  }
  case ShopItemStorage::housing: {
    auto &housing = plan.next.housing_items[item_cms_id];
    if (plan.next.housing_items.size() > kMaximumHousingItemKinds ||
        housing.quantity > std::numeric_limits<int>::max() - count) {
      return x5::ec::ItemSlotLimit;
    }
    housing.quantity += count;
    plan.changed_housing_items.push_back(item_cms_id);
    return x5::ec::Success;
  }
  case ShopItemStorage::instance: {
    for (int index = 0; index < count; ++index) {
      if (free_slots.empty() || plan.granted_items.size() >= kMaximumGrantedInstances) {
        return x5::ec::ItemSlotLimit;
      }
      CharacterItem item;
      item.cms_id = item_cms_id;
      item.inventory_slot = free_slots.front();
      item.durability = definition->max_durability;
      item.grade = definition->grade;
      item.created_epoch_ticks = context.now_epoch_ticks;
      free_slots.erase(free_slots.begin());
      plan.granted_items.push_back(item);
    }
    return x5::ec::Success;
  }
  case ShopItemStorage::unsupported:
    break;
  }
  return x5::ec::ItemCmsIdInvalid;
}

Json reward_json(const std::optional<int> &item_cms_id, const std::optional<MoneyType> &currency,
                 std::int64_t value) {
  Json reward{{"value", std::to_string(value)}};
  if (item_cms_id) {
    reward["item_cms_id"] = *item_cms_id;
  } else {
    reward["money_type"] =
        currency.value_or(MoneyType::Gold) == MoneyType::Gold ? kGoldMoneyType : kTokenMoneyType;
  }
  return reward;
}

} // namespace

EErrorCode parse_load_request(const nlohmann::json &message, LoadRequest &request) {
  if (!message.is_object()) {
    return x5::ec::BadRequest;
  }

  const auto goods = message.find("add_goods");
  if (goods != message.end()) {
    if (!goods->is_array() || goods->size() > kMaximumLoadLines) {
      return x5::ec::Trade_InvalidGoodNums;
    }
    std::set<std::int64_t> seen_vouchers;
    for (const auto &entry : *goods) {
      if (!entry.is_object()) {
        return x5::ec::BadRequest;
      }
      LoadLine line;
      if (entry.contains("voucher_id")) {
        line.voucher_id = parse_decimal(entry.at("voucher_id"));
        if (!line.voucher_id || *line.voucher_id <= 0 ||
            !seen_vouchers.insert(*line.voucher_id).second) {
          return x5::ec::TradeVoucher_Invalid;
        }
        line.count = 1;
      } else if (entry.contains("good_count") && entry.at("good_count").is_object()) {
        const auto &count = entry.at("good_count");
        const auto good = wire_int(count, "trade_good_cms_id");
        const auto amount = wire_int(count, "count");
        if (!good || *good <= 0 || !amount || *amount <= 0 || *amount > kMaximumLineCount) {
          return x5::ec::Trade_InvalidGoodNums;
        }
        line.good_cms_id = *good;
        line.count = *amount;
      } else {
        return x5::ec::Trade_InvalidGood;
      }
      request.lines.push_back(line);
    }
  }

  request.wagon_cms_id = wire_int(message, "wagon_cms_id").value_or(0);
  const auto start = message.find("start");
  request.start = start != message.end() && start->is_boolean() && start->get<bool>();
  return x5::ec::Success;
}

EErrorCode parse_sell_request(const nlohmann::json &message, SellRequest &request) {
  const auto goods = message.is_object() ? message.find("goods") : message.end();
  if (goods == message.end() || !goods->is_array() || goods->empty() ||
      goods->size() > kMaximumLoadLines) {
    return x5::ec::Trade_InvalidGoodNums;
  }

  std::size_t units = 0;
  for (const auto &entry : *goods) {
    if (!entry.is_object()) {
      return x5::ec::BadRequest;
    }
    SellLine line;
    const auto good = wire_int(entry, "good_cms_id");
    const auto origin = wire_int(entry, "trading_post_cms_id");
    const auto times = entry.find("production_epochtimes");
    if (!good || *good <= 0 || !origin || *origin <= 0 || times == entry.end() ||
        !times->is_array() || times->empty()) {
      return x5::ec::Trade_InvalidGood;
    }
    for (const auto &time : *times) {
      const auto ticks = parse_decimal(time);
      if (!ticks) {
        return x5::ec::Trade_InvalidGood;
      }
      line.production_epoch_ticks.push_back(*ticks);
    }
    units += line.production_epoch_ticks.size();
    line.good_cms_id = *good;
    line.origin_post_id = *origin;
    request.lines.push_back(std::move(line));
  }
  return units <= kMaximumTradeRunGoods ? x5::ec::Success : x5::ec::Trade_InvalidGoodNums;
}

TradePlan plan_load(const TradeContext &context, int post_id, const LoadRequest &request) {
  TradePlan plan;
  plan.next = context.before;
  auto &run = plan.next.trade_run;
  const auto fail = [&plan](EErrorCode code) {
    plan.error = code;
    return plan;
  };

  if (context.before.dead) {
    return fail(x5::ec::InvalidState);
  }
  const auto *post = context.catalog.post(post_id);
  if (!post || !post->placed) {
    return fail(x5::ec::Trade_PostUnstartable);
  }
  if (run.phase == TradeRunPhase::running || run.phase == TradeRunPhase::parked ||
      run.phase == TradeRunPhase::stowed) {
    return fail(x5::ec::Trade_InProgress);
  }
  if (run.phase == TradeRunPhase::loading && run.start_post_id != post_id) {
    return fail(x5::ec::Trade_InvalidPost);
  }
  const auto *wagon = context.vehicles.vehicle(request.wagon_cms_id);
  if (!wagon || wagon->kind != companion::VehicleKind::trade_wagon) {
    return fail(x5::ec::Trade_InvalidWagon);
  }

  std::int64_t purchase_gold = 0;
  for (const auto &line : request.lines) {
    if (line.voucher_id) {
      const auto voucher = plan.next.trade_vouchers.find(*line.voucher_id);
      if (voucher == plan.next.trade_vouchers.end() || voucher->second.redeemed) {
        return fail(x5::ec::TradeVoucher_Invalid);
      }
      if (voucher->second.expiration_epoch_ticks <= context.now_epoch_ticks) {
        return fail(x5::ec::TradeVoucher_Expired);
      }
      if (!context.catalog.good(voucher->second.good_cms_id)) {
        return fail(x5::ec::TradeGood_NotLoadable);
      }
      voucher->second.redeemed = true;
      plan.consumed_voucher_ids.push_back(*line.voucher_id);
      plan.loaded_goods.push_back(
          {voucher->second.good_cms_id, post_id, context.now_epoch_ticks});
      continue;
    }

    const auto *good = context.catalog.good(line.good_cms_id);
    if (!good) {
      return fail(x5::ec::Trade_InvalidGood);
    }
    if (!context.catalog.post_sells(post_id, line.good_cms_id)) {
      return fail(x5::ec::TradeGood_NotPurchasable);
    }
    // Candidate price: the production fee of a voucher for the good, else its basic value.
    const auto *cost = context.catalog.production_cost(line.good_cms_id);
    const auto unit_price = cost ? cost->fee : good->basic_value;
    purchase_gold += unit_price * line.count;
    for (int unit = 0; unit < line.count; ++unit) {
      plan.loaded_goods.push_back({line.good_cms_id, post_id, context.now_epoch_ticks});
    }
  }

  const auto total_goods = run.goods.size() + plan.loaded_goods.size();
  if (total_goods > static_cast<std::size_t>(wagon->max_load_capacity) ||
      total_goods > kMaximumTradeRunGoods) {
    return fail(x5::ec::Trade_WagonLoadOverLimit);
  }
  if (total_goods == 0 || (plan.loaded_goods.empty() && !request.start)) {
    return fail(x5::ec::Trade_InvalidGoodNums);
  }

  if (!apply_money(plan.next, MoneyType::Gold, -purchase_gold, plan.money_changes)) {
    return fail(x5::ec::MoneyNotEnough);
  }
  if (request.start &&
      !apply_money(plan.next, wagon->price_currency, -wagon->price, plan.money_changes)) {
    return fail(x5::ec::MoneyNotEnough);
  }

  run.goods.insert(run.goods.end(), plan.loaded_goods.begin(), plan.loaded_goods.end());
  run.start_post_id = post_id;
  run.vehicle_cms_id = wagon->id;
  run.phase = request.start ? TradeRunPhase::running : TradeRunPhase::loading;
  if (request.start) {
    run.started_epoch_ticks = context.now_epoch_ticks;
    run.wagon_health = wagon->max_health;
    run.last_wagon_spawn_epoch_ticks = context.now_epoch_ticks;
    plan.departure_post_id = post_id;
  }
  return plan;
}

TradePlan plan_sell(const TradeContext &context, int post_id, const SellRequest &request,
                    std::uint32_t random_seed) {
  TradePlan plan;
  plan.next = context.before;
  auto &run = plan.next.trade_run;
  const auto fail = [&plan](EErrorCode code) {
    plan.error = code;
    return plan;
  };

  if (context.before.dead) {
    return fail(x5::ec::InvalidState);
  }
  const auto *post = context.catalog.post(post_id);
  if (!post || !post->placed) {
    return fail(x5::ec::Trade_InvalidPost);
  }
  switch (run.phase) {
  case TradeRunPhase::idle:
    return fail(x5::ec::Trade_NotInProgress);
  case TradeRunPhase::loading:
    // Goods still at their origin can only be sold back there (refund only).
    if (run.start_post_id != post_id) {
      return fail(x5::ec::Trade_PostTooFar);
    }
    break;
  case TradeRunPhase::running:
    return fail(x5::ec::Trade_WagonTooFar);
  case TradeRunPhase::parked:
  case TradeRunPhase::stowed:
    if (run.parked_post_id != post_id) {
      return fail(x5::ec::Trade_PostTooFar);
    }
    break;
  }

  auto remaining = run.goods;
  auto free_slots = free_inventory_slots(context.character);
  std::mt19937 random(random_seed);
  const auto &settings = context.catalog.settings();

  for (const auto &line : request.lines) {
    const auto *good = context.catalog.good(line.good_cms_id);
    if (!good) {
      return fail(x5::ec::Trade_InvalidGood);
    }

    SaleResult result;
    result.good_cms_id = good->id;
    const auto quote = market_quote(context.catalog, post_id, good->group,
                                    context.now_epoch_ticks);
    const bool same_post = line.origin_post_id == post_id;
    const PurchaseReward *row = nullptr;
    if (!same_post) {
      row = context.catalog.purchase_reward(post_id, good->id, line.origin_post_id);
    }

    // Ratios shown in the result popup: the market quote for a money route, 100 % for an item
    // route, and the refund rate when no route applies (origin post or off-route good).
    if (row && row->currency) {
      result.basic_price_ratio_basis_points = quote.basic_basis_points;
      result.price_ratio_basis_points = quote.total_basis_points;
      result.event_pattern_id = quote.event_pattern_id;
      result.event_price_ratio_basis_points = quote.event_basis_points;
    } else if (row) {
      result.basic_price_ratio_basis_points = kBasisPointsPerWhole;
      result.price_ratio_basis_points = kBasisPointsPerWhole;
    } else {
      result.basic_price_ratio_basis_points = settings.refund_basis_points;
      result.price_ratio_basis_points = settings.refund_basis_points;
    }

    std::int64_t reward_value = 0;
    bool first_unit = true;
    for (const auto produced : line.production_epoch_ticks) {
      const auto unit = std::ranges::find_if(remaining, [&](const TradeGoodUnit &candidate) {
        return candidate.good_cms_id == line.good_cms_id &&
               candidate.origin_post_id == line.origin_post_id &&
               candidate.production_epoch_ticks == produced;
      });
      if (unit == remaining.end()) {
        return fail(x5::ec::Trade_InvalidGood);
      }
      remaining.erase(unit);

      const auto *tier = quality_tier(*good, produced, context.now_epoch_ticks);
      if (first_unit) {
        result.quality = tier ? tier->quality : GoodQuality::none;
        first_unit = false;
      }
      const auto quality_factor = tier ? tier->factor_basis_points : kBasisPointsPerWhole;

      if (row && row->item_cms_id) {
        // Primary route with an item reward: the row's amount per unit.
        reward_value += row->item_amount;
      } else if (row && row->currency) {
        const auto base = scale(good->basic_value, row->reward_basis_points);
        reward_value += scale(scale(base, quote.total_basis_points), quality_factor);
      } else {
        // Origin post or a non-primary route: gold only at the refund rate.
        reward_value += scale(scale(good->basic_value, settings.refund_basis_points),
                              quality_factor);
      }
    }

    if (row && row->item_cms_id) {
      result.default_reward.item_cms_id = row->item_cms_id;
      const auto error = grant_reward_item(context, plan, *row->item_cms_id, reward_value,
                                           free_slots);
      if (error != x5::ec::Success) {
        return fail(error);
      }
    } else {
      const auto currency = row && row->currency ? *row->currency : MoneyType::Gold;
      result.default_reward.currency = currency;
      if (!apply_money(plan.next, currency, reward_value, plan.money_changes)) {
        return fail(x5::ec::ItemError);
      }
    }
    result.default_reward.value = reward_value;

    // TradeBonusReward rows roll independently per sale line (Candidate).
    if (row && row->bonus_reward_id) {
      if (const auto *bonuses = context.catalog.bonus_reward(*row->bonus_reward_id)) {
        for (const auto &bonus : *bonuses) {
          std::uniform_int_distribution<std::int64_t> roll(1, kBasisPointsPerWhole);
          if (roll(random) > bonus.probability_basis_points) {
            continue;
          }
          std::uniform_int_distribution<std::int64_t> amount(bonus.minimum_amount,
                                                             bonus.maximum_amount);
          const auto value = amount(random);
          if (bonus.item_cms_id) {
            const auto error =
                grant_reward_item(context, plan, *bonus.item_cms_id, value, free_slots);
            if (error != x5::ec::Success) {
              return fail(error);
            }
          } else if (!apply_money(plan.next, *bonus.currency, value, plan.money_changes)) {
            return fail(x5::ec::ItemError);
          }
          result.bonus_rewards.push_back({bonus.item_cms_id, bonus.currency, value});
        }
      }
    }
    plan.sale_results.push_back(std::move(result));
  }

  run.goods = std::move(remaining);
  if (run.goods.empty()) {
    reset_run(run);
    plan.run_ended = true;
  }
  return plan;
}

TradePlan plan_cancel(const TradeContext &context) {
  TradePlan plan;
  plan.next = context.before;
  if (plan.next.trade_run.phase == TradeRunPhase::idle) {
    plan.error = x5::ec::Trade_NotInProgress;
    return plan;
  }

  reset_run(plan.next.trade_run);
  plan.run_ended = true;
  return plan;
}

TradePlan plan_park(const TradeContext &context, int post_id, const WorldPosition &wagon) {
  TradePlan plan;
  plan.next = context.before;
  auto &run = plan.next.trade_run;
  if (run.phase != TradeRunPhase::running && run.phase != TradeRunPhase::parked) {
    plan.error = x5::ec::Trade_NotInProgress;
    return plan;
  }

  run.phase = TradeRunPhase::parked;
  run.parked_post_id = post_id;
  run.wagon_position = wagon;
  return plan;
}

TradePlan plan_stow(const TradeContext &context) {
  TradePlan plan;
  plan.next = context.before;
  auto &run = plan.next.trade_run;
  if (run.phase != TradeRunPhase::parked) {
    plan.error = x5::ec::Trade_NotInProgress;
    return plan;
  }

  run.phase = TradeRunPhase::stowed;
  run.wagon_position.reset();
  return plan;
}

TradePlan plan_wagon_state(const TradeContext &context, const std::optional<WorldPosition> &wagon,
                           int health, bool spawned_now) {
  TradePlan plan;
  plan.next = context.before;
  auto &run = plan.next.trade_run;
  if (run.phase != TradeRunPhase::running && run.phase != TradeRunPhase::parked) {
    plan.error = x5::ec::Trade_NotInProgress;
    return plan;
  }

  run.wagon_position = wagon;
  run.wagon_health = std::max(health, 0);
  if (spawned_now) {
    run.last_wagon_spawn_epoch_ticks = context.now_epoch_ticks;
  }
  return plan;
}

bool wagon_inside_post(const TradingPost &post, const WorldPosition &wagon) {
  if (!post.placed) {
    return false;
  }
  const auto delta_x = static_cast<std::int64_t>(wagon.x_mm) - post.sphere_center.x_mm;
  const auto delta_y = static_cast<std::int64_t>(wagon.y_mm) - post.sphere_center.y_mm;
  const auto delta_z = static_cast<std::int64_t>(wagon.z_mm) - post.sphere_center.z_mm;
  return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z <=
         post.sphere_radius_mm * post.sphere_radius_mm;
}

bool player_near_post(const TradingPost &post, const WorldPosition &position) {
  if (!post.placed || !post.map_ids.contains(position.map_id)) {
    return false;
  }
  const auto delta_x = static_cast<std::int64_t>(position.x_mm) - post.departure.x_mm;
  const auto delta_y = static_cast<std::int64_t>(position.y_mm) - post.departure.y_mm;
  const auto delta_z = static_cast<std::int64_t>(position.z_mm) - post.departure.z_mm;
  return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z <=
         kTradingPostReachMm * kTradingPostReachMm;
}

std::optional<int> post_at(const TradeCatalog &catalog, const WorldPosition &position) {
  for (const auto &[post_id, post] : catalog.posts()) {
    if (player_near_post(post, position)) {
      return post_id;
    }
  }
  return std::nullopt;
}

MarketQuote market_quote(const TradeCatalog &catalog, int post_id, int group,
                         std::int64_t now_epoch_ticks) {
  const auto &settings = catalog.settings();
  const auto window = std::max<std::int64_t>(0, now_epoch_ticks) / market_window_ticks(catalog);
  const auto hash = mix((static_cast<std::uint64_t>(post_id) << 40) ^
                        (static_cast<std::uint64_t>(group) << 32) ^
                        static_cast<std::uint64_t>(window));

  MarketQuote quote;
  const auto band_floor = settings.basic_market_min_percent * 100;
  const auto band_steps = (settings.basic_market_max_percent - settings.basic_market_min_percent) *
                          100 / settings.basic_market_step_basis_points;
  quote.basic_basis_points =
      band_floor + settings.basic_market_step_basis_points *
                       static_cast<std::int64_t>(hash % static_cast<std::uint64_t>(band_steps + 1));

  if ((hash >> 20) % kEventChanceDivisor == 0) {
    const auto tier = kEventTiers[(hash >> 24) % kEventTiers.size()];
    const auto candidates = catalog.patterns(tier, group);
    if (!candidates.empty()) {
      quote.event_pattern_id = candidates[(hash >> 32) % candidates.size()];
      quote.event_basis_points = kEventBasisPointsByTier[static_cast<std::size_t>(tier)];
    }
  }

  quote.total_basis_points =
      std::clamp(quote.basic_basis_points + quote.event_basis_points,
                 settings.clamp_min_percent * 100, settings.clamp_max_percent * 100);
  return quote;
}

std::int64_t next_market_update(const TradeCatalog &catalog, std::int64_t now_epoch_ticks) {
  const auto window = market_window_ticks(catalog);
  return (std::max<std::int64_t>(0, now_epoch_ticks) / window + 1) * window;
}

nlohmann::json market_price_message(const TradeCatalog &catalog, std::int64_t now_epoch_ticks) {
  auto posts = Json::array();
  for (const auto &[post_id, post] : catalog.posts()) {
    if (!post.placed) {
      continue;
    }

    auto patterns = Json::object();
    for (int group = 0; group < kGoodsGroupCount; ++group) {
      const auto quote = market_quote(catalog, post_id, group, now_epoch_ticks);
      if (quote.event_pattern_id) {
        patterns[std::to_string(group)] = *quote.event_pattern_id;
      }
    }
    posts.push_back({{"cms_id", post_id}, {"group_patterns", std::move(patterns)}});
  }
  return {{"next_update_tick", std::to_string(next_market_update(catalog, now_epoch_ticks))},
          {"trading_posts", std::move(posts)}};
}

nlohmann::json trade_goods_json(const std::vector<TradeGoodUnit> &goods) {
  // One TradeGoods entry per (good, origin) pair in first-seen order.
  std::vector<std::pair<int, int>> order;
  std::map<std::pair<int, int>, Json> times;
  for (const auto &unit : goods) {
    const auto key = std::pair{unit.good_cms_id, unit.origin_post_id};
    if (!times.contains(key)) {
      order.push_back(key);
      times[key] = Json::array();
    }
    times[key].push_back(std::to_string(unit.production_epoch_ticks));
  }

  auto entries = Json::array();
  for (const auto &key : order) {
    entries.push_back({{"good_cms_id", key.first},
                       {"trading_post_cms_id", key.second},
                       {"production_epochtimes", times.at(key)}});
  }
  return entries;
}

GameplayPacket loading_packet(const TradePlan &plan) {
  auto vouchers = Json::array();
  for (const auto voucher_id : plan.consumed_voucher_ids) {
    // An id-only voucher entry removes the voucher from the client's list.
    vouchers.push_back({{"voucher_id", std::to_string(voucher_id)}});
  }

  Json update{{"trade_loading",
               {{"trade_goods", trade_goods_json(plan.loaded_goods)}, {"vouchers", vouchers}}}};
  auto money = Json::array();
  for (const auto &change : plan.money_changes) {
    money.push_back(money_change_json(change));
  }
  if (!money.empty()) {
    update["money_changes"] = std::move(money);
  }
  return {kItemsUpdateNotification, std::move(update)};
}

GameplayPacket selling_packet(const TradePlan &plan,
                              const std::vector<CharacterItem> &committed_grants) {
  auto results = Json::array();
  for (const auto &result : plan.sale_results) {
    auto bonuses = Json::array();
    for (const auto &bonus : result.bonus_rewards) {
      bonuses.push_back(reward_json(bonus.item_cms_id, bonus.currency, bonus.value));
    }
    Json entry{{"trade_good_cms_id", result.good_cms_id},
               {"quality", quality_name(result.quality)},
               {"price_ratio_basis_points", result.price_ratio_basis_points},
               {"basic_price_ratio_basis_points", result.basic_price_ratio_basis_points},
               {"default_reward",
                reward_json(result.default_reward.item_cms_id, result.default_reward.currency,
                            result.default_reward.value)},
               {"bonus_rewards", std::move(bonuses)}};
    if (result.event_pattern_id) {
      entry["event_desc_id"] = *result.event_pattern_id;
      entry["event_price_ratio_basis_points"] = result.event_price_ratio_basis_points;
    }
    results.push_back(std::move(entry));
  }

  Json update{{"trade_sell",
               {{"results", std::move(results)},
                {"trade_goods", trade_goods_json(plan.next.trade_run.goods)}}}};
  auto money = Json::array();
  for (const auto &change : plan.money_changes) {
    money.push_back(money_change_json(change));
  }
  if (!money.empty()) {
    update["money_changes"] = std::move(money);
  }

  auto items = Json::array();
  for (const auto &item : committed_grants) {
    items.push_back(item_instance_json(item));
  }
  if (!items.empty()) {
    update["items"] = std::move(items);
  }

  auto stackables = Json::array();
  for (const auto item_cms_id : std::set<int>(plan.changed_stackables.begin(),
                                              plan.changed_stackables.end())) {
    stackables.push_back(
        {{"item_cms_id", item_cms_id}, {"quantity", plan.next.stackables.at(item_cms_id)}});
  }
  if (!stackables.empty()) {
    update["stackables"] = std::move(stackables);
  }

  auto housing = Json::array();
  for (const auto item_cms_id : std::set<int>(plan.changed_housing_items.begin(),
                                              plan.changed_housing_items.end())) {
    const auto &record = plan.next.housing_items.at(item_cms_id);
    housing.push_back(housing_item_count_json(item_cms_id, record));
  }
  if (!housing.empty()) {
    update["housing_items"] = std::move(housing);
  }
  return {kItemsUpdateNotification, std::move(update)};
}

GameplayPacket trade_start_packet(const WorldProgress &progress) {
  return {kTradeStartNotification, {{"trade_goods", trade_goods_json(progress.trade_run.goods)}}};
}

GameplayPacket wagon_position_packet(const WorldPosition &wagon) {
  return {kTradeWagonPosNotification,
          {{"pos",
            {{"millimeter_x", wagon.x_mm},
             {"millimeter_y", wagon.y_mm},
             {"millimeter_z", wagon.z_mm}}}}};
}

GameplayPacket error_packet(std::uint32_t request_sequence, EErrorCode code) {
  return {kCommonErrorResponse,
          {{"ec", x5::ec::EErrorCode_Name(code)},
           {"packet_seq", {{"seq", std::to_string(request_sequence)}}}}};
}

} // namespace chronicles::trade
