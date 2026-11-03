#pragma once

#include "world/core/frame_context.hpp"
#include "world/core/progress_access.hpp"
#include "world/core/session_event_bus.hpp"
#include "world/features/market/market.hpp"

#include <memory>
#include <string>

namespace chronicles::market {

class MarketSession;
class MarketSessionContext;

class MarketFeature final {
public:
  MarketFeature(ClientCatalog *catalog, std::shared_ptr<const ShopCatalog> items);
  MarketFeature(std::shared_ptr<const MarketCatalog> catalog,
                std::shared_ptr<const ShopCatalog> items);

  void set_repository(std::shared_ptr<MarketRepository> repository);

  std::unique_ptr<MarketSession>
  create_session(ProgressAccess &progress, MarketSessionContext &context, SessionEventBus &events);

private:
  friend class MarketSession;

  const std::shared_ptr<const MarketCatalog> catalog_;
  const std::shared_ptr<const ShopCatalog> items_;
  std::shared_ptr<MarketRepository> repository_;
};

class MarketSessionContext {
public:
  virtual ~MarketSessionContext() = default;

  virtual bool market_ready() const = 0;
  virtual bool market_has_tutorial() const = 0;
  virtual std::int64_t market_epoch_ticks() const = 0;
  virtual const nlohmann::json *market_player_stats() const = 0;
  virtual void market_append_encumbrance(std::vector<GameplayPacket> &packets) = 0;
};

// The registration scope belongs to the connection, including across map changes.
// Storage/listing transactions remain in receive_market and the narrow repository.
class MarketSession final {
public:
  MarketSession(MarketFeature &feature, ProgressAccess &progress, MarketSessionContext &context,
                SessionEventBus &events);
  MarketSession(MarketFeature &feature, ProgressAccess &progress, MarketSessionContext &context,
                SessionEventBus &events, std::string registration_scope);

  bool handle(FrameContext &frame);
  std::vector<GameplayPacket> receive(const WorldGameplayRequest &request);

private:
  MarketFeature &feature_;
  ProgressAccess &progress_;
  MarketSessionContext &context_;
  SessionEventBus &events_;
  const std::string registration_scope_;
};

} // namespace chronicles::market
