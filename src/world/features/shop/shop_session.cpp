#include "world/features/shop/shop_session.hpp"

#include "data/client_catalog.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace chronicles::shop {
namespace {

constexpr std::int64_t kRequestWindowTicks = 10'000'000;
constexpr unsigned kMaximumRequestsPerWindow = 8;

std::shared_ptr<const ShopCatalog> load_shop_catalog(ClientCatalog *catalog) {
  if (!catalog) {
    return {};
  }
  for (const auto &table : ShopCatalog::required_tables()) {
    if (!catalog->has_table(table)) {
      spdlog::warn("NPC shops disabled: client catalog table {} is absent", table);
      return {};
    }
  }

  auto shops = ShopCatalog::load(*catalog);
  spdlog::info("NPC shop catalog loaded: {} shops", shops->shop_count());
  return shops;
}

} // namespace

ShopFeature::ShopFeature(ClientCatalog *catalog) : ShopFeature(load_shop_catalog(catalog)) {
}

ShopFeature::ShopFeature(std::shared_ptr<const ShopCatalog> catalog)
    : catalog_(std::move(catalog)) {
}

const std::shared_ptr<const ShopCatalog> &ShopFeature::catalog() const noexcept {
  return catalog_;
}

std::unique_ptr<ShopSession> ShopFeature::create_session(ProgressAccess &progress,
                                                         ShopSessionContext &context,
                                                         SessionEventBus &events) {
  return std::make_unique<ShopSession>(*this, progress, context, events);
}

ShopSession::ShopSession(ShopFeature &feature, ProgressAccess &progress,
                         ShopSessionContext &context, SessionEventBus &events)
    : feature_(feature), progress_(progress), context_(context), events_(events) {
}

bool ShopSession::handle(FrameContext &frame) {
  const auto *request = frame.request();
  if (!request || request->opcode != kInteractUnitRequestOpcode ||
      !is_merchant_request(request->message)) {
    return false;
  }
  frame.session_packets = receive(*request);
  return true;
}

std::vector<GameplayPacket> ShopSession::receive(const WorldGameplayRequest &request) {
  const auto reject = [&request](x5::ec::EErrorCode code) {
    return std::vector<GameplayPacket>{shop_error_packet(request.sequence, code)};
  };
  if (!context_.shop_character_id() || !feature_.catalog_) {
    return reject(x5::ec::Interaction_Invalid);
  }
  const auto parsed = parse_shop_request(request.message);
  if (parsed.error != x5::ec::Success) {
    return reject(parsed.error);
  }
  const auto &shop_request = parsed.request;
  const auto &catalog = *feature_.catalog_;
  const auto conditions = context_.shop_condition_context();
  const auto target = context_.shop_target(shop_request.target_unit_id);
  const auto access = authorize_merchant_access(catalog, target ? &*target : nullptr, shop_request,
                                                context_.shop_position(), conditions);
  if (access.error != x5::ec::Success) {
    return reject(access.error);
  }
  if (!allow_request()) {
    return reject(x5::ec::BadRequest);
  }

  const auto now = context_.shop_epoch_ticks();
  const auto before = progress_.load();
  if (shop_request.action == ShopAction::open) {
    return shop_open_packets(catalog, shop_request, access.service.target_id, before, now);
  }
  const auto character = progress_.character();
  if (!character) {
    return reject(x5::ec::BadRequest);
  }
  MerchantTradeInput input{catalog, *character, before, now, conditions, std::nullopt};
  input.trade_catalog = context_.shop_trade_catalog();
  if (shop_request.action == ShopAction::buy) {
    input.weight_budget = purchase_weight_budget(*character, before);
  }
  const auto plan = plan_merchant_trade(input, access.service, shop_request);
  if (plan.error != x5::ec::Success) {
    return reject(plan.error);
  }

  std::optional<std::vector<CharacterItem>> committed;
  try {
    committed = progress_.commit(before, plan.trade.next, plan.trade.storage_changes());
  } catch (const std::exception &error) {
    spdlog::warn("NPC shop commit rejected for character {}: {}", *context_.shop_character_id(),
                 error.what());
    return reject(x5::ec::ItemError);
  }
  if (!committed) {
    return reject(x5::ec::ItemError);
  }

  if (context_.shop_has_tutorial()) {
    events_.publish(PlayerProgressCommitted{plan.trade.next});
  }
  on_item_trade_committed(*character, plan.trade, *committed);
  return merchant_trade_packets(shop_request, plan.trade, *committed);
}

bool ShopSession::allow_request() {
  const auto now = context_.shop_monotonic_ticks();
  if (now - window_start_ticks_ >= kRequestWindowTicks) {
    window_start_ticks_ = now;
    requests_in_window_ = 0;
  }
  ++requests_in_window_;
  return requests_in_window_ <= kMaximumRequestsPerWindow;
}

std::optional<WeightBudget> ShopSession::purchase_weight_budget(const Character &character,
                                                                const WorldProgress &progress) {
  const auto percent = feature_.catalog_->weight_ceiling_percent();
  if (!context_.shop_has_weight_measurement() || !percent) {
    return std::nullopt;
  }
  try {
    const auto carried = context_.shop_measure_weight(character, progress);
    const auto scaled = carried.safe_weight <= (std::numeric_limits<std::int64_t>::max)() / *percent
                            ? std::optional<std::int64_t>(carried.safe_weight * *percent / 100)
                            : std::nullopt;
    if (!scaled) {
      return std::nullopt;
    }
    return WeightBudget{carried.carried_weight, *scaled};
  } catch (const std::exception &error) {
    spdlog::warn("NPC shop weight check skipped for character {}: {}",
                 *context_.shop_character_id(), error.what());
    return std::nullopt;
  }
}

void ShopSession::on_item_trade_committed(const Character &before_trade, const ShopTrade &trade,
                                          const std::vector<CharacterItem> &committed_grants) {
  if (trade.removed_items.empty() && committed_grants.empty() && trade.repaired_items.empty()) {
    return;
  }
  Character live = before_trade;
  std::erase_if(live.items, [&](const CharacterItem &item) {
    return std::ranges::any_of(trade.removed_items, [&](const CharacterItem &removed) {
      return removed.id == item.id;
    });
  });
  for (const auto &repaired : trade.repaired_items) {
    const auto owned = std::ranges::find_if(live.items, [&](const CharacterItem &item) {
      return item.id == repaired.id;
    });
    if (owned != live.items.end()) {
      owned->durability = repaired.durability;
    }
  }
  live.items.insert(live.items.end(), committed_grants.begin(), committed_grants.end());
  events_.publish(PlayerInventoryStatsChanged{live});
  events_.publish(PlayerInventorySnapshotChanged{live, context_.shop_player_stats()});
}

} // namespace chronicles::shop
