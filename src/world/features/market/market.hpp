#pragma once

#include "protocol/gameplay_packet.hpp"
#include "data/shop_catalog.hpp"
#include "world/features/market/market_repository.hpp"

#include <map>
#include <memory>
#include <string>

namespace chronicles {

class ClientCatalog;

struct MarketItemRule {
  int main_type{};
  int sub_type{};
  bool bound{};
};

// Build-specific allowlist/category indexes. These establish eligibility, not official
// auction fees, price bands, level requirements or UI message ordering.
class MarketCatalog {
public:
  static std::shared_ptr<const MarketCatalog> load(ClientCatalog *catalog);

  MarketCatalog(const std::vector<nlohmann::json> &items,
                const std::vector<nlohmann::json> &allowlist,
                const std::vector<nlohmann::json> &categories);

  const MarketItemRule *item(int cms_id) const;

private:
  std::map<int, MarketItemRule> items_;
};

struct MarketResult {
  std::vector<GameplayPacket> packets;
  std::optional<WorldProgress> committed_progress;
};

bool is_market_request(std::uint16_t opcode);

GameplayPacket market_error(std::uint16_t opcode, const nlohmann::json &request, int error);

// Called on the authenticated character's worker. Commits compare both the character
// revision and the listing snapshot, so competing sessions cannot spend/grant twice.
MarketResult receive_market(MarketRepository &market, const Character &actor,
                            const WorldProgress &before, const MarketCatalog &catalog,
                            const ShopCatalog &items,
                            std::uint16_t opcode, const nlohmann::json &request,
                            std::uint32_t sequence, const std::string &registration_scope,
                            std::int64_t now);

} // namespace chronicles
