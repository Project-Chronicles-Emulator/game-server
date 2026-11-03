#pragma once

#include "world/core/unit_registry.hpp"
#include "world/features/companion/companion_feature.hpp"
#include "world/features/trade/trade_bootstrap.hpp"
#include "world/features/trade/trade_session.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <set>

namespace chronicles::trade {

class TradeSessionContext {
public:
  virtual ~TradeSessionContext() = default;

  virtual const AnnouncedUnit *trade_interaction_npc(std::uint64_t unit_id) const = 0;
  virtual std::int64_t trade_epoch_ticks() const = 0;
};

// Managers retain their declaration-time load, while the placed catalog retains its later
// catalog-root startup position. The existing TradeSession remains the player state owner.
class TradeFeature final {
public:
  TradeFeature(ClientCatalog *catalog, std::shared_ptr<const ShopCatalog> items);
  TradeFeature(std::shared_ptr<const TradeCatalog> catalog,
               std::shared_ptr<const std::set<int>> managers,
               std::shared_ptr<const ShopCatalog> items);

  void load(ClientCatalog *catalog, const std::filesystem::path &root);
  const std::shared_ptr<const TradeCatalog> &catalog() const noexcept;
  std::unique_ptr<TradeSession> create_session(const companion::CompanionFeature &companions,
                                               std::unique_ptr<ProgressAccess> progress,
                                               std::uint64_t connection_id,
                                               std::int64_t character_id) const;

  bool handle_dialogue(TradeSession *session, const WorldGameplayRequest &request,
                       const std::optional<WorldPosition> &position, TradeSessionContext &context,
                       companion::SessionResult &result) const;
  bool handle_request(TradeSession *session, const WorldGameplayRequest &request,
                      const std::optional<WorldPosition> &position, TradeSessionContext &context,
                      companion::SessionResult &result) const;

private:
  std::shared_ptr<const TradeCatalog> catalog_;
  const std::shared_ptr<const std::set<int>> managers_;
  const std::shared_ptr<const ShopCatalog> items_;
};

} // namespace chronicles::trade
