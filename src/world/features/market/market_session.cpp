#include "world/features/market/market_session.hpp"

#include "auth/auth.hpp"
#include "ec.generated.pb.h"

#include <utility>

namespace chronicles::market {

MarketFeature::MarketFeature(ClientCatalog *catalog, std::shared_ptr<const ShopCatalog> items)
    : MarketFeature(MarketCatalog::load(catalog), std::move(items)) {
}

MarketFeature::MarketFeature(std::shared_ptr<const MarketCatalog> catalog,
                             std::shared_ptr<const ShopCatalog> items)
    : catalog_(std::move(catalog)), items_(std::move(items)) {
}

void MarketFeature::set_repository(std::shared_ptr<MarketRepository> repository) {
  repository_ = std::move(repository);
}

std::unique_ptr<MarketSession> MarketFeature::create_session(ProgressAccess &progress,
                                                             MarketSessionContext &context,
                                                             SessionEventBus &events) {
  return std::make_unique<MarketSession>(*this, progress, context, events);
}

MarketSession::MarketSession(MarketFeature &feature, ProgressAccess &progress,
                             MarketSessionContext &context, SessionEventBus &events)
    : MarketSession(feature, progress, context, events, random_hex(16)) {
}

MarketSession::MarketSession(MarketFeature &feature, ProgressAccess &progress,
                             MarketSessionContext &context, SessionEventBus &events,
                             std::string registration_scope)
    : feature_(feature), progress_(progress), context_(context), events_(events),
      registration_scope_(std::move(registration_scope)) {
}

bool MarketSession::handle(FrameContext &frame) {
  const auto *request = frame.request();
  if (!request || !is_market_request(request->opcode)) {
    return false;
  }
  frame.session_packets = receive(*request);
  return true;
}

std::vector<GameplayPacket> MarketSession::receive(const WorldGameplayRequest &request) {
  if (!context_.market_ready() || !feature_.repository_ || !feature_.catalog_ || !feature_.items_) {
    return {market_error(request.opcode, request.message, x5::ec::Market_Disabled)};
  }
  const auto character = progress_.character();
  if (!character) {
    return {market_error(request.opcode, request.message, x5::ec::Market_InvalidItemMarketSession)};
  }
  const auto before = progress_.load();
  // Ambiguous storage and post-commit failures still reach the existing frame failure path.
  auto result = receive_market(*feature_.repository_, *character, before, *feature_.catalog_,
                               *feature_.items_, request.opcode, request.message, request.sequence,
                               registration_scope_, context_.market_epoch_ticks());
  if (result.committed_progress) {
    if (context_.market_has_tutorial()) {
      events_.publish(PlayerProgressCommitted{*result.committed_progress});
    }
    const auto updated = progress_.character();
    if (updated) {
      // Preserve the original unlocked cache refresh and existing combat snapshot.
      events_.publish(PlayerInventorySnapshotChanged{*updated, context_.market_player_stats()});
    }
    context_.market_append_encumbrance(result.packets);
  }
  return result.packets;
}

} // namespace chronicles::market
