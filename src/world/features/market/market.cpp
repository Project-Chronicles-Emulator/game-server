#include "world/features/market/market.hpp"

#include "common/catalog_fields.hpp"
#include "data/client_catalog.hpp"
#include "common/clock_ticks.hpp"
#include "cms.generated.pb.h"
#include "ec.generated.pb.h"
#include "persistence/item_wire.hpp"
#include "world/features/market/market_protocol.hpp"
#include "p_c2s.market.pb.h"
#include "p_market.pb.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;

class Refused : public std::exception {
public:
  explicit Refused(int value) : code(value) {}
  int code;
};

void ensure(bool condition, int code) {
  if (!condition) {
    throw Refused(code);
  }
}

std::int64_t integer(const Json &object, const char *name, std::int64_t fallback = 0) {
  const auto found = object.find(name);
  if (found == object.end()) {
    return fallback;
  }
  if (found->is_string()) {
    const auto text = found->get<std::string>();
    std::int64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    ensure(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
           x5::ec::Market_InvalidItemMarketRequest);
    return value;
  }
  ensure(found->is_number_integer() &&
             (!found->is_number_unsigned() ||
              found->get<std::uint64_t>() <=
                  static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())),
         x5::ec::Market_InvalidItemMarketRequest);
  return found->get<std::int64_t>();
}

int bounded(const Json &object, const char *name, int minimum, int maximum, int fallback = 0) {
  const auto value = integer(object, name, fallback);
  ensure(value >= minimum && value <= maximum, x5::ec::Market_InvalidItemMarketRequest);
  return static_cast<int>(value);
}

int enumeration(const Json &object, const char *name,
                const google::protobuf::EnumDescriptor *descriptor, int maximum) {
  const auto found = object.find(name);
  if (found != object.end() && found->is_string()) {
    const auto text = found->get<std::string>();
    const auto *value = descriptor->FindValueByName(text);
    if (value != nullptr) {
      ensure(value->number() >= 0 && value->number() <= maximum,
             x5::ec::Market_InvalidItemMarketRequest);
      return value->number();
    }
  }
  return bounded(object, name, 0, maximum);
}

Json listing_json(const MarketListing &listing, std::int64_t now) {
  return {{"listing_id", std::to_string(listing.id)},
          {"item_cms_id", listing.item.cms_id},
          {"price", std::to_string(listing.price)},
          {"deadline_time", std::to_string(listing.deadline)},
          {"state", listing.state == kMarketOnSale && now >= listing.deadline ? kMarketExpired
                                                                              : listing.state},
          {"update_time", std::to_string(listing.updated)},
          {"quantity", listing.quantity}};
}

GameplayPacket update_packet(const WorldProgress &before, const WorldProgress &after,
                             const ItemTrade &trade, const MarketCommit &commit,
                             std::uint32_t sequence) {
  Json update{{"system", Json::object()}, {"packet_seq", {{"seq", std::to_string(sequence)}}}};
  for (const auto id : trade.removed_item_ids) {
    // Item with identity only is the existing inventory deletion representation.
    update["items"].push_back({{"item_id", std::to_string(id)}});
  }
  for (const auto &item : commit.granted_items) {
    update["items"].push_back(item_instance_json(item));
  }
  for (const auto &[cms_id, quantity] : after.stackables) {
    const auto previous = before.stackables.find(cms_id);
    if (previous == before.stackables.end() || previous->second != quantity) {
      update["stackables"].push_back({{"item_cms_id", cms_id}, {"quantity", quantity}});
    }
  }
  if (before.gold != after.gold) {
    update["money_changes"].push_back({{"money_type", 0},
                                       {"change", std::to_string(after.gold - before.gold)},
                                       {"result", std::to_string(after.gold)}});
  }
  return {kMarketItemsUpdate, std::move(update)};
}

void plan_item_claim(const MarketListing &listing, const Character &actor, WorldProgress &after,
                     ItemTrade &trade, const ShopCatalog &catalog) {
  if (listing.stackable) {
    ensure(after.stackables.contains(listing.item.cms_id) ||
               after.stackables.size() < kMaximumStackableKinds,
           x5::ec::Market_NotEnoughInventory);
    auto &quantity = after.stackables[listing.item.cms_id];
    ensure(quantity <= catalog.maximum_stack() - listing.quantity,
           x5::ec::Market_NotEnoughInventory);
    quantity += listing.quantity;
    return;
  }
  std::set<int> occupied;
  for (const auto &item : actor.items) {
    occupied.insert(item.inventory_slot);
  }
  int slot{};
  while (slot <= kMaximumInventorySlot && occupied.contains(slot)) {
    ++slot;
  }
  ensure(slot <= kMaximumInventorySlot, x5::ec::Market_NotEnoughInventory);
  auto item = listing.item;
  item.id = 0;
  item.inventory_slot = slot;
  item.equip_slot.reset();
  trade.granted_items.push_back(item);
}

// Settles the seller's side of a closed listing: the sale proceeds for a sold listing,
// otherwise the escrowed item. The durable seller receipt grants either one at most once.
void plan_seller_settlement(MarketListing &listing, const Character &actor, WorldProgress &after,
                            ItemTrade &trade, const ShopCatalog &catalog) {
  ensure(!listing.seller_claimed, x5::ec::Market_AlreadyClaimed);
  listing.seller_claimed = true;

  if (listing.state == kMarketSold) {
    const auto gold_limit = std::min(catalog.gold_limit(), kMoneyLimit);
    ensure(after.gold <= gold_limit - listing.price, x5::ec::Market_TransactionFailed);
    after.gold += listing.price;
    return;
  }

  plan_item_claim(listing, actor, after, trade, catalog);
}

} // namespace

std::shared_ptr<const MarketCatalog> MarketCatalog::load(ClientCatalog *catalog) {
  if (catalog == nullptr || !catalog->has_table("ItemMarketAllowList") ||
      !catalog->has_table("ItemMarketCategory") || !catalog->has_table("Item")) {
    return nullptr;
  }
  return std::make_shared<MarketCatalog>(catalog->rows("Item"),
                                         catalog->rows("ItemMarketAllowList"),
                                         catalog->rows("ItemMarketCategory"));
}

MarketCatalog::MarketCatalog(const std::vector<Json> &items, const std::vector<Json> &allowlist,
                             const std::vector<Json> &categories) {
  std::map<int, MarketItemRule> category_rules;
  for (const auto &row : categories) {
    x5::cms::EX5ItemMarketMainType main_type;
    x5::cms::EX5ItemMarketSubType sub_type;
    if (!x5::cms::EX5ItemMarketMainType_Parse(
            "EX5ItemMarketMainType_" + catalog_fields::text(row, "MainType"), &main_type) ||
        !x5::cms::EX5ItemMarketSubType_Parse(
            "EX5ItemMarketSubType_" + catalog_fields::text(row, "SubType"), &sub_type)) {
      throw std::runtime_error("Unknown marketplace catalog category");
    }
    const auto id = catalog_fields::required_identifier(row, "Id");
    if (!category_rules.emplace(id, MarketItemRule{main_type, sub_type, false}).second) {
      throw std::runtime_error("Duplicate marketplace catalog category");
    }
  }
  std::map<int, bool> bound;
  for (const auto &row : items) {
    bound.emplace(catalog_fields::required_identifier(row, "id"),
                  !catalog_fields::text(row, "bindType").empty());
  }
  for (const auto &row : allowlist) {
    const auto id = catalog_fields::required_identifier(row, "FK_ItemId");
    const auto category = catalog_fields::required_identifier(row, "FK_CategoryGroupId");
    if (!category_rules.contains(category) || !bound.contains(id)) {
      throw std::runtime_error("Marketplace allowlist references a missing item/category");
    }
    auto rule = category_rules.at(category);
    rule.bound = bound.at(id);
    if (!items_.emplace(id, rule).second) {
      throw std::runtime_error("Duplicate marketplace allowlist item");
    }
  }
}

const MarketItemRule *MarketCatalog::item(int cms_id) const {
  const auto found = items_.find(cms_id);
  return found == items_.end() ? nullptr : &found->second;
}

bool is_market_request(std::uint16_t opcode) {
  return opcode >= kMarketRegisterRequest && opcode <= kMarketHistoryRequest;
}

GameplayPacket market_error(std::uint16_t opcode, const Json &request, int error) {
  Json reply{{"ec", error}};
  if (opcode != kMarketRegisterRequest && opcode != kMarketQueryRequest &&
      opcode != kMarketHistoryRequest && request.contains("listing_id")) {
    reply["listing_id"] = request.at("listing_id");
  }
  if (opcode == kMarketQueryRequest) {
    for (const auto *field : {"listing_type", "start_index"}) {
      if (request.contains(field)) {
        reply[field] = request.at(field);
      }
    }
  }
  return {static_cast<std::uint16_t>(kMarketRegisterResponse + opcode - kMarketRegisterRequest),
          std::move(reply)};
}

MarketResult receive_market(MarketRepository &market, const Character &actor,
                            const WorldProgress &before, const MarketCatalog &catalog,
                            const ShopCatalog &items, std::uint16_t opcode, const Json &request,
                            std::uint32_t sequence, const std::string &registration_scope,
                            std::int64_t now) {
  if (!is_market_request(opcode)) {
    throw std::invalid_argument("Not a marketplace request");
  }
  try {
    ensure(actor.id > 0 && actor.account_id > 0 && actor.world_id > 0 && now > 0 &&
               request.is_object(),
           x5::ec::Market_InvalidItemMarketSession);
    if (opcode == kMarketRegisterRequest) {
      const auto key = registration_scope + ":" + std::to_string(sequence);
      const auto receipt = market.market_registration(actor.world_id, actor.id, key);
      if (receipt) {
        ensure(request.contains("identity") && request.at("identity").is_object(),
               x5::ec::Market_InvalidItemMarketRequest);
        const auto &identity = request.at("identity");
        ensure(identity.size() == 1 && integer(request, "price") == receipt->price,
               x5::ec::Market_InvalidItemMarketRequest);
        if (receipt->stackable) {
          ensure(identity.contains("stackable") && identity.at("stackable").is_object(),
                 x5::ec::Market_InvalidItemMarketRequest);
          ensure(integer(identity.at("stackable"), "item_cms_id") == receipt->item.cms_id &&
                     integer(identity.at("stackable"), "count") == receipt->quantity,
                 x5::ec::Market_InvalidItemMarketRequest);
        } else {
          ensure(integer(identity, "item_id") == receipt->item.id,
                 x5::ec::Market_InvalidItemMarketRequest);
        }
        return {{{kMarketRegisterResponse,
                  {{"ec", 0}, {"listing_registered", listing_json(*receipt, now)}}}},
                {}};
      }
    }
    if (opcode == kMarketQueryRequest || opcode == kMarketHistoryRequest) {
      MarketQuery query;
      query.history = opcode == kMarketHistoryRequest;
      if (!query.history) {
        query.type =
            enumeration(request, "listing_type", x5::pb::MarketListingType_descriptor(), 2);
        const auto start = integer(request, "start_index");
        ensure(start >= 0 && start <= std::numeric_limits<std::uint32_t>::max(),
               x5::ec::Market_InvalidItemMarketRequest);
        query.start = static_cast<std::uint32_t>(start);
        query.count = bounded(request, "count", 1, 100);
        query.sort =
            enumeration(request, "sort_type", x5::cms::EX5MarketListingSortType_descriptor(), 2);
        if (request.contains("main_type")) {
          query.main_type =
              enumeration(request, "main_type", x5::cms::EX5ItemMarketMainType_descriptor(), 2);
        }
        if (request.contains("sub_type")) {
          query.sub_type =
              enumeration(request, "sub_type", x5::cms::EX5ItemMarketSubType_descriptor(), 6);
        }
      } else {
        query.count = 100;
      }
      const auto page = market.market_query(actor.world_id, actor.id, query, now);
      Json response{{"ec", 0}};
      if (query.history) {
        response["entries"] = Json::array();
        for (const auto &listing : page.listings) {
          response["entries"].push_back({{"item_cms_id", listing.item.cms_id},
                                         {"quantity", listing.quantity},
                                         {"price", std::to_string(listing.price)},
                                         {"completed_time", std::to_string(listing.completed)},
                                         {"side", listing.buyer_id == actor.id ? 0 : 1}});
        }
      } else {
        response["listing_type"] = query.type;
        response["start_index"] = query.start;
        response["total"] = page.total;
        response["listings"] = Json::array();
        for (const auto &listing : page.listings) {
          response["listings"].push_back(listing_json(listing, now));
        }
      }
      return {{{static_cast<std::uint16_t>(query.history ? kMarketHistoryResponse
                                                         : kMarketQueryResponse),
                std::move(response)}},
              {}};
    }

    MarketListing expected;
    MarketListing next;
    ItemTrade trade;
    auto after = before;
    ensure(before.revision < std::numeric_limits<std::int64_t>::max(),
           x5::ec::Market_TransactionFailed);
    ++after.revision;
    if (opcode == kMarketRegisterRequest) {
      ensure(!before.dead, x5::ec::Market_InvalidItemMarketSession);
      next.world_id = actor.world_id;
      next.seller_id = actor.id;
      next.price = integer(request, "price");
      ensure(next.price > 0 && next.price <= items.gold_limit() && next.price <= kMoneyLimit,
             x5::ec::Market_InvalidPrice);
      const auto period =
          enumeration(request, "period", x5::pb::MarketListingPeriod_descriptor(), 2);
      // Emulator policy: RapidTest is sixty seconds. Official durations/fees are unobserved.
      const std::int64_t seconds = period == 2 ? 60 : (period == 1 ? 3 : 1) * 86400;
      ensure(now <= std::numeric_limits<std::int64_t>::max() - seconds * kEpochTicksPerSecond,
             x5::ec::Market_InvalidItemMarketRequest);
      next.deadline = now + seconds * kEpochTicksPerSecond;
      next.updated = now;
      next.registration_key = registration_scope + ":" + std::to_string(sequence);
      ensure(request.contains("identity") && request.at("identity").is_object(),
             x5::ec::Market_ItemNotFound);
      const auto &identity = request.at("identity");
      ensure(identity.size() == 1, x5::ec::Market_InvalidItemMarketRequest);
      if (identity.contains("item_id")) {
        const auto id = integer(identity, "item_id");
        const auto item =
            std::ranges::find_if(actor.items, [id](const auto &owned) { return owned.id == id; });
        ensure(item != actor.items.end() && !item->equip_slot, x5::ec::Market_ItemNotFound);
        ensure(item->polish_step == 0, x5::ec::Market_PolishedItem);
        ensure(item->quantity == 1, x5::ec::Market_InvalidQuantity);
        next.item = *item;
        trade.removed_item_ids.push_back(id);
      } else {
        ensure(identity.contains("stackable") && identity.at("stackable").is_object(),
               x5::ec::Market_NotTradeableItem);
        const auto &stack = identity.at("stackable");
        next.item.cms_id = bounded(stack, "item_cms_id", 1, std::numeric_limits<int>::max());
        next.quantity = bounded(stack, "count", 1, items.maximum_stack());
        const auto owned = after.stackables.find(next.item.cms_id);
        ensure(owned != after.stackables.end() && owned->second >= next.quantity,
               x5::ec::Market_InvalidQuantity);
        owned->second -= next.quantity;
        next.stackable = true;
      }
      const auto *rule = catalog.item(next.item.cms_id);
      const auto *definition = items.item(next.item.cms_id);
      ensure(rule != nullptr && definition != nullptr, x5::ec::Market_NotTradeableItem);
      ensure(!rule->bound, x5::ec::Market_BoundItem);
      ensure(definition->storage ==
                 (next.stackable ? ShopItemStorage::stackable : ShopItemStorage::instance),
             x5::ec::Market_NotTradeableItem);
      next.main_type = rule->main_type;
      next.sub_type = rule->sub_type;
      MarketQuery own;
      own.type = 1;
      own.count = 100;
      const auto page = market.market_query(actor.world_id, actor.id, own, now);
      const auto pending = std::ranges::count_if(
          page.listings, [](const auto &listing) { return !listing.seller_claimed; });
      ensure(pending < kMaximumMarketSellerListings, x5::ec::Market_NotEnoughSellListStorage);
    } else {
      const auto id = integer(request, "listing_id");
      const auto listing = market.market_listing(actor.world_id, id);
      ensure(listing.has_value(), x5::ec::Market_ListingNotFound);
      expected = *listing;
      next = expected;
      const auto effective_state =
          next.state == kMarketOnSale && now >= next.deadline ? kMarketExpired : next.state;
      if (opcode == kMarketDetailsRequest) {
        ensure(effective_state == kMarketOnSale || actor.id == next.seller_id ||
                   actor.id == next.buyer_id,
               x5::ec::Market_ListingNotFound);
        return {{{kMarketDetailsResponse,
                  {{"ec", 0},
                   {"listing_id", std::to_string(id)},
                   {"item", item_instance_json(next.item)},
                   {"quantity", next.quantity}}}},
                {}};
      }
      ensure(!before.dead && now >= expected.updated, x5::ec::Market_InvalidItemMarketSession);
      next.updated = now;
      if (opcode == kMarketRevokeRequest) {
        ensure(actor.id == next.seller_id, x5::ec::Market_SellerMismatch);
        if (effective_state == kMarketOnSale) {
          next.state = kMarketRevoked;
        } else {
          // The client's listing-details action sends this same request as "Retrieve" for a
          // closed listing (AAC_MarketUI_ListingDetails_OnPrimaryAction, 0x146329960). An
          // unsold expired or cancelled listing returns its escrow like a seller claim; a sold
          // listing keeps its proceeds for MarketClaimReq.
          ensure(effective_state != kMarketSold, x5::ec::Market_NotSellRegisterCancel);
          next.state = effective_state;
          plan_seller_settlement(next, actor, after, trade, items);
        }
      } else if (opcode == kMarketPurchaseRequest) {
        ensure(actor.id != next.seller_id, x5::ec::Market_NotAvailablePurchaseSelfRegisteredItem);
        ensure(effective_state == kMarketOnSale, x5::ec::Market_NotAvailableAlreadySoldItem);
        ensure(after.gold >= next.price, x5::ec::Market_NotEnoughGold);
        after.gold -= next.price;
        next.state = kMarketSold;
        next.buyer_id = actor.id;
        next.completed = now;
      } else {
        ensure(effective_state != kMarketOnSale, x5::ec::Market_StillOnSale);
        next.state = effective_state;
        if (actor.id == next.seller_id) {
          plan_seller_settlement(next, actor, after, trade, items);
        } else {
          ensure(next.state == kMarketSold && actor.id == next.buyer_id,
                 x5::ec::Market_BuyerMismatch);
          ensure(!next.buyer_claimed, x5::ec::Market_AlreadyClaimed);
          next.buyer_claimed = true;
          plan_item_claim(next, actor, after, trade, items);
        }
      }
    }
    const auto committed = market.market_commit(actor, before, after, trade, expected, next);
    ensure(committed.committed, x5::ec::Market_TransactionFailed);
    Json response{{"ec", 0}};
    if (opcode == kMarketRegisterRequest) {
      response["listing_registered"] = listing_json(committed.listing, now);
    } else {
      response["listing_id"] = std::to_string(committed.listing.id);
    }
    MarketResult result;
    if (!committed.replayed) {
      result.committed_progress = after;
      if (before.gold != after.gold || before.stackables != after.stackables ||
          !trade.removed_item_ids.empty() || !committed.granted_items.empty()) {
        result.packets.push_back(update_packet(before, after, trade, committed, sequence));
      }
    }
    result.packets.push_back(
        {static_cast<std::uint16_t>(kMarketRegisterResponse + opcode - kMarketRegisterRequest),
         std::move(response)});
    return result;
  } catch (const Refused &refused) {
    return {{market_error(opcode, request, refused.code)}, {}};
  }
}

} // namespace chronicles
