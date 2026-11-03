#include "world/core/progress_packets.hpp"

#include "common/currency.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <string>

namespace chronicles {

// ItemsUpdateNoti (opcode 169), the same wire constant every feature module defines locally.
constexpr std::uint16_t kItemsUpdateNotification = 169;

nlohmann::json money_change_json(const MoneyChange &change) {
  nlohmann::json entry{{"change", std::to_string(change.change)},
                       {"result", std::to_string(change.result)}};
  // Gold is the protobuf default and is omitted, as in the captured money updates.
  if (!change.omit_type_on_wire) {
    entry["money_type"] = change.type == MoneyType::Gold ? kGoldMoneyType : kTokenMoneyType;
  }
  return entry;
}

nlohmann::json housing_item_count_json(int item_cms_id, const HousingItemRecord &record) {
  const int stored_copies = std::max(0, record.quantity - record.installed_count);
  return {{"item_cms_id", item_cms_id},
          {"quantity", stored_copies},
          {"housing_installed_count", record.installed_count}};
}

std::optional<GameplayPacket> progress_change_packet(const WorldProgress &before,
                                                     const WorldProgress &after,
                                                     const std::string &reason) {
  auto stackables = nlohmann::json::array();
  std::set<int> item_ids;
  for (const auto &[cms_id, quantity] : before.stackables) {
    item_ids.insert(cms_id);
  }
  for (const auto &[cms_id, quantity] : after.stackables) {
    item_ids.insert(cms_id);
  }
  for (const auto cms_id : item_ids) {
    const auto old_quantity = before.stackables.contains(cms_id) ? before.stackables.at(cms_id) : 0;
    const auto new_quantity = after.stackables.contains(cms_id) ? after.stackables.at(cms_id) : 0;
    if (old_quantity != new_quantity) {
      stackables.push_back({{"item_cms_id", cms_id}, {"quantity", new_quantity}});
    }
  }

  auto money_changes = nlohmann::json::array();
  if (after.gold != before.gold) {
    MoneyChange change;
    change.type = MoneyType::Gold;
    change.change = after.gold - before.gold;
    change.result = after.gold;
    change.omit_type_on_wire = true;
    money_changes.push_back(money_change_json(change));
  }
  if (after.hersander_token != before.hersander_token) {
    MoneyChange change;
    change.type = MoneyType::HersanderToken;
    change.change = after.hersander_token - before.hersander_token;
    change.result = after.hersander_token;
    money_changes.push_back(money_change_json(change));
  }

  if (stackables.empty() && money_changes.empty()) {
    return std::nullopt;
  }

  nlohmann::json message{{reason, nlohmann::json::object()}};
  if (!stackables.empty()) {
    message["stackables"] = std::move(stackables);
  }
  if (!money_changes.empty()) {
    message["money_changes"] = std::move(money_changes);
  }
  return GameplayPacket{kItemsUpdateNotification, std::move(message)};
}

} // namespace chronicles
