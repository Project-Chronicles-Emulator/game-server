#include "world/features/npc/npc_interactions.hpp"

#include "data/client_catalog.hpp"
#include "world/core/unit_registry.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>
#include <string>

namespace chronicles {
namespace {

constexpr const char *kShopCase = "Shop";

[[noreturn]] void reject(const char *table, std::size_t row_index, const std::string &detail) {
  throw std::invalid_argument(std::string(table) + " row " + std::to_string(row_index) + ": " +
                              detail);
}

// Catalog rows store every value as a string. Identifiers must be non-negative decimal integers.
std::int64_t identifier(const nlohmann::json &row, const char *table, std::size_t row_index,
                        const char *field) {
  if (!row.is_object()) {
    reject(table, row_index, "row is not an object");
  }
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    reject(table, row_index, std::string("field '") + field + "' is missing or not a string");
  }
  const auto &text = found->get_ref<const std::string &>();
  std::int64_t value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value < 0) {
    reject(table, row_index,
           std::string("field '") + field + "' is not a non-negative integer: '" + text + "'");
  }
  return value;
}

// Interaction.playerTask holds a JSON object body such as "shopId":1006.
std::int64_t shop_id_from_task(const nlohmann::json &row, std::size_t row_index) {
  const auto found = row.find("playerTask");
  if (found == row.end() || !found->is_string()) {
    reject("Interaction", row_index, "Shop interaction has no playerTask string");
  }
  const auto task = nlohmann::json::parse("{" + found->get<std::string>() + "}", nullptr, false);
  if (task.is_discarded() || !task.is_object() || !task.contains("shopId") ||
      !task.at("shopId").is_number_integer() || task.at("shopId").get<std::int64_t>() < 0) {
    reject("Interaction", row_index,
           "Shop interaction playerTask has no valid shopId: '" + found->get<std::string>() + "'");
  }
  return task.at("shopId").get<std::int64_t>();
}

} // namespace

NpcInteractionIndex NpcInteractionIndex::build(const std::vector<nlohmann::json> &npc_rows,
                                               const std::vector<nlohmann::json> &interaction_rows,
                                               const std::vector<nlohmann::json> &shop_rows) {
  std::set<std::int64_t> shop_ids;
  for (std::size_t index = 0; index < shop_rows.size(); ++index) {
    shop_ids.insert(identifier(shop_rows[index], "Shop", index, "id"));
  }

  NpcInteractionIndex result;
  std::unordered_map<std::int64_t, std::vector<std::int64_t>> interactions_by_group;
  for (std::size_t index = 0; index < interaction_rows.size(); ++index) {
    const auto &row = interaction_rows[index];
    const auto interaction_id = identifier(row, "Interaction", index, "id");
    const auto group_id = identifier(row, "Interaction", index, "groupId");
    if (result.interaction_shop_.contains(interaction_id)) {
      reject("Interaction", index, "duplicate id " + std::to_string(interaction_id));
    }

    std::optional<std::int64_t> shop_id;
    if (row.value("playerTaskCase", std::string()) == kShopCase) {
      shop_id = shop_id_from_task(row, index);
      if (!shop_ids.contains(*shop_id)) {
        reject("Interaction", index,
               "shopId " + std::to_string(*shop_id) + " is not a Shop.id");
      }
    }
    result.interaction_shop_.emplace(interaction_id, shop_id);
    interactions_by_group[group_id].push_back(interaction_id);
  }

  for (std::size_t index = 0; index < npc_rows.size(); ++index) {
    const auto &row = npc_rows[index];
    const auto npc_id = identifier(row, "Npc", index, "id");
    const auto group_id = identifier(row, "Npc", index, "interactionGroupId");
    if (result.npc_interactions_.contains(npc_id)) {
      reject("Npc", index, "duplicate id " + std::to_string(npc_id));
    }

    std::vector<std::int64_t> offered;
    const auto group = interactions_by_group.find(group_id);
    if (group != interactions_by_group.end()) {
      offered = group->second;
      std::sort(offered.begin(), offered.end());
    }
    result.npc_interactions_.emplace(npc_id, std::move(offered));
  }
  return result;
}

NpcInteractionIndex NpcInteractionIndex::load(ClientCatalog &catalog) {
  return build(catalog.rows("Npc"), catalog.rows("Interaction"), catalog.rows("Shop"));
}

bool NpcInteractionIndex::npc_offers_interaction(std::int64_t npc_id,
                                                 std::int64_t interaction_id) const {
  const auto &offered = interactions_for_npc(npc_id);
  return std::binary_search(offered.begin(), offered.end(), interaction_id);
}

bool NpcInteractionIndex::npc_offers_shop(std::int64_t npc_id, std::int64_t shop_id) const {
  const auto &offered = interactions_for_npc(npc_id);
  return std::any_of(offered.begin(), offered.end(), [&](std::int64_t interaction_id) {
    const auto found = interaction_shop_.find(interaction_id);
    return found != interaction_shop_.end() && found->second == shop_id;
  });
}

std::optional<std::int64_t>
NpcInteractionIndex::shop_for_interaction(std::int64_t interaction_id) const {
  const auto found = interaction_shop_.find(interaction_id);
  if (found == interaction_shop_.end()) {
    return std::nullopt;
  }
  return found->second;
}

const std::vector<std::int64_t> &NpcInteractionIndex::interactions_for_npc(
    std::int64_t npc_id) const {
  static const std::vector<std::int64_t> kNone;
  const auto found = npc_interactions_.find(npc_id);
  return found == npc_interactions_.end() ? kNone : found->second;
}

std::size_t NpcInteractionIndex::npc_count() const { return npc_interactions_.size(); }

std::size_t NpcInteractionIndex::interaction_count() const { return interaction_shop_.size(); }

ShopOpenResult OpenedShopState::open(const NpcInteractionIndex &index,
                                     const UnitRegistry &registry, std::uint64_t unit_id,
                                     std::int64_t interaction_id, int map_id,
                                     std::string_view instance_key) {
  const auto *unit = registry.find_in_instance(unit_id, map_id, instance_key);
  if (unit == nullptr) {
    return ShopOpenResult::UnknownUnit;
  }
  if (!index.npc_offers_interaction(unit->npc_template_id, interaction_id)) {
    return ShopOpenResult::InteractionNotOffered;
  }
  const auto shop_id = index.shop_for_interaction(interaction_id);
  if (!shop_id) {
    return ShopOpenResult::NotAShopInteraction;
  }

  opened_ = OpenedShop{*shop_id, interaction_id, unit->unit_id, unit->map_id,
                       unit->instance_key};
  return ShopOpenResult::Opened;
}

bool OpenedShopState::permits(std::int64_t shop_id, std::uint64_t unit_id, int map_id,
                              std::string_view instance_key) const {
  return opened_ && opened_->shop_id == shop_id && opened_->unit_id == unit_id &&
         opened_->map_id == map_id && opened_->instance_key == instance_key;
}

void OpenedShopState::on_map_or_instance_change(int map_id, std::string_view instance_key) {
  if (opened_ && (opened_->map_id != map_id || opened_->instance_key != instance_key)) {
    opened_.reset();
  }
}

void OpenedShopState::on_unit_despawn(std::uint64_t unit_id) {
  if (opened_ && opened_->unit_id == unit_id) {
    opened_.reset();
  }
}

void OpenedShopState::clear() { opened_.reset(); }

const std::optional<OpenedShop> &OpenedShopState::current() const { return opened_; }

} // namespace chronicles
