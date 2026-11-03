#pragma once

#include "world/core/frame_context.hpp"
#include "world/core/progress_access.hpp"
#include "world/core/session_event_bus.hpp"
#include "world/features/shop/npc_shop.hpp"

#include <memory>

namespace chronicles::shop {

class ShopSession;
class ShopSessionContext;

class ShopFeature final {
public:
  explicit ShopFeature(ClientCatalog *catalog);
  explicit ShopFeature(std::shared_ptr<const ShopCatalog> catalog);

  const std::shared_ptr<const ShopCatalog> &catalog() const noexcept;
  std::unique_ptr<ShopSession> create_session(ProgressAccess &progress, ShopSessionContext &context,
                                              SessionEventBus &events);

private:
  friend class ShopSession;
  const std::shared_ptr<const ShopCatalog> catalog_;
};

struct MerchantWeightMeasurement {
  std::int64_t carried_weight{};
  std::int64_t safe_weight{};
};

class ShopSessionContext {
public:
  virtual ~ShopSessionContext() = default;

  virtual std::optional<std::int64_t> shop_character_id() const = 0;
  virtual bool shop_has_tutorial() const = 0;
  virtual std::int64_t shop_monotonic_ticks() const = 0;
  virtual std::int64_t shop_epoch_ticks() const = 0;
  virtual std::optional<MerchantTarget> shop_target(std::uint64_t unit_id) const = 0;
  virtual std::optional<WorldPosition> shop_position() const = 0;
  virtual const trade::TradeCatalog *shop_trade_catalog() const = 0;
  virtual bool shop_has_weight_measurement() const = 0;
  virtual MerchantWeightMeasurement shop_measure_weight(const Character &character,
                                                        const WorldProgress &progress) = 0;
  virtual const nlohmann::json *shop_player_stats() const = 0;
  // Company standing and pioneer permit for catalog conditions on merchant access and goods.
  virtual ConditionContext shop_condition_context() const = 0;
};

// NPC visibility and inventory measurements are explicit queries to their owners. Shop
// owns merchant policy, its request budget and the plan/commit/refresh/packet ordering.
class ShopSession final {
public:
  ShopSession(ShopFeature &feature, ProgressAccess &progress, ShopSessionContext &context,
              SessionEventBus &events);

  bool handle(FrameContext &frame);
  std::vector<GameplayPacket> receive(const WorldGameplayRequest &request);

  // The existing GM inventory service uses the same committed-row refresh. Authorization,
  // planning and persistence remain with its caller; this hook never commits a trade.
  void on_item_trade_committed(const Character &before_trade, const ShopTrade &trade,
                               const std::vector<CharacterItem> &committed_grants);

private:
  bool allow_request();
  std::optional<WeightBudget> purchase_weight_budget(const Character &character,
                                                     const WorldProgress &progress);
  ShopFeature &feature_;
  ProgressAccess &progress_;
  ShopSessionContext &context_;
  SessionEventBus &events_;
  std::int64_t window_start_ticks_{};
  unsigned requests_in_window_{};
};

} // namespace chronicles::shop
