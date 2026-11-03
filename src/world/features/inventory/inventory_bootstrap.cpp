#include "world/features/inventory/inventory_bootstrap.hpp"

#include "common/currency.hpp"
#include "world/features/inventory/inventory_items.hpp"

#include <string>
#include <utility>

namespace chronicles {
namespace {

constexpr std::uint16_t kWorldCharMoneyNotification = 126;
constexpr std::uint16_t kWorldCharItemsNotification = 130;

} // namespace

nlohmann::json world_money_values_json(std::int64_t gold, std::int64_t hersander_token) {
  CurrencyBalances balances;
  balances.gold = gold;
  balances.hersander_token = hersander_token;
  nlohmann::json values = nlohmann::json::object();
  for (const auto &[money_type_key, amount] :
       build_money_values_map(balances, hersander_token != 0)) {
    values[money_type_key] = std::to_string(amount);
  }
  return values;
}

namespace inventory {

void InventoryBootstrap::configure(std::optional<WorldProgress> &non_tutorial_progress) {
  if (non_tutorial_progress) {
    gold_ = non_tutorial_progress->gold;
    hersander_token_ = non_tutorial_progress->hersander_token;
    stackables_ = std::move(non_tutorial_progress->stackables);
  } else {
    gold_.reset();
    hersander_token_ = 0;
    stackables_.clear();
  }
}

GameplayPacket InventoryBootstrap::items_snapshot(const Character &character) const {
  auto inventory = character_inventory(character);
  if (gold_) {
    auto stackables = nlohmann::json::array();
    // Persisted zero-quantity stacks remain tombstones. Their original-client display
    // behavior is unobserved; entry continues to include them in the same ordered map walk.
    for (const auto &[cms_id, quantity] : stackables_) {
      stackables.push_back({{"item_cms_id", cms_id}, {"quantity", quantity}});
    }
    inventory["stackables"] = std::move(stackables);
  }
  return GameplayPacket{kWorldCharItemsNotification, std::move(inventory)};
}

std::optional<GameplayPacket> InventoryBootstrap::money_snapshot() const {
  if (!gold_) {
    return std::nullopt;
  }
  return GameplayPacket{
      kWorldCharMoneyNotification,
      {{"update", false},
       {"money", {{"values", world_money_values_json(*gold_, hersander_token_)}}}}};
}

} // namespace inventory
} // namespace chronicles
