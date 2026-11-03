#include "world/features/market/market_repository.hpp"

#include "common/currency.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/postgres/character_rows.hpp"
#include "world/features/market/market_states.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <set>
#include <tuple>

namespace chronicles {
namespace {

constexpr const char *kColumns =
    "id,world_id,seller_id,buyer_id,item,stackable,quantity,main_type,sub_type,price,"
    "deadline,updated,completed,state,seller_claimed,buyer_claimed,registration_key";

void require(bool condition) {
  if (!condition) {
    throw std::invalid_argument("Invalid marketplace transition");
  }
}

nlohmann::json item_json(const CharacterItem &item) {
  return {{"id", item.id},
          {"cms", item.cms_id},
          {"slot", item.inventory_slot},
          {"quantity", item.quantity},
          {"durability", item.durability},
          {"grade", item.grade},
          {"polish", item.polish_step},
          {"created", item.created_epoch_ticks}};
}

MarketListing read_listing(const pqxx::row &row) {
  MarketListing listing;
  listing.id = row["id"].as<std::int64_t>();
  listing.world_id = row["world_id"].as<int>();
  listing.seller_id = row["seller_id"].as<std::int64_t>();
  listing.buyer_id = row["buyer_id"].as<std::int64_t>();
  const auto item = nlohmann::json::parse(row["item"].as<std::string>());
  listing.item.id = item.at("id").get<std::int64_t>();
  listing.item.cms_id = item.at("cms").get<int>();
  listing.item.inventory_slot = item.at("slot").get<int>();
  listing.item.quantity = item.at("quantity").get<int>();
  listing.item.durability = item.at("durability").get<int>();
  listing.item.grade = item.at("grade").get<int>();
  listing.item.polish_step = item.at("polish").get<int>();
  listing.item.created_epoch_ticks = item.at("created").get<std::int64_t>();
  listing.stackable = row["stackable"].as<bool>();
  listing.quantity = row["quantity"].as<int>();
  listing.main_type = row["main_type"].as<int>();
  listing.sub_type = row["sub_type"].as<int>();
  listing.price = row["price"].as<std::int64_t>();
  listing.deadline = row["deadline"].as<std::int64_t>();
  listing.updated = row["updated"].as<std::int64_t>();
  listing.completed = row["completed"].as<std::int64_t>();
  listing.state = row["state"].as<int>();
  listing.seller_claimed = row["seller_claimed"].as<bool>();
  listing.buyer_claimed = row["buyer_claimed"].as<bool>();
  listing.registration_key = row["registration_key"].as<std::string>();
  return listing;
}

bool same_grant(const CharacterItem &grant, const CharacterItem &escrow) {
  auto normalized = grant;
  normalized.id = escrow.id;
  normalized.inventory_slot = escrow.inventory_slot;
  return normalized == escrow && grant.id == 0 && !grant.equip_slot;
}

bool same_registration(const MarketListing &left, const MarketListing &right) {
  return left.world_id == right.world_id && left.seller_id == right.seller_id &&
         left.item == right.item && left.stackable == right.stackable &&
         left.quantity == right.quantity && left.price == right.price &&
         left.deadline - left.updated == right.deadline - right.updated;
}

} // namespace

void validate_market_transition(std::int64_t character_id, const WorldProgress &before,
                                const WorldProgress &after, const ItemTrade &trade,
                                const MarketListing &expected, const MarketListing &next) {
  detail::validate_world_progress_transition(before, after);
  require(!before.dead && next.world_id > 0 && next.seller_id > 0 && next.item.cms_id > 0 &&
          next.quantity > 0 && next.price > 0 && next.price <= kMoneyLimit && next.updated > 0 &&
          next.deadline > 0 && !next.item.equip_slot && next.registration_key.size() >= 16 &&
          next.registration_key.size() <= 100 && next.item.polish_step == 0 &&
          next.main_type >= 0 && next.main_type <= 2 && next.sub_type >= 0 && next.sub_type <= 6 &&
          trade.durability_updates.empty());
  auto progress = before;
  ++progress.revision;
  bool remove_item{};
  bool grant_item{};
  if (expected.id == 0) {
    require(next.id == 0 && next.seller_id == character_id && next.buyer_id == 0 &&
            next.state == kMarketOnSale && !next.seller_claimed && !next.buyer_claimed &&
            next.completed == 0 && next.deadline > next.updated);
    remove_item = true;
  } else {
    auto immutable = next;
    immutable.buyer_id = expected.buyer_id;
    immutable.state = expected.state;
    immutable.updated = expected.updated;
    immutable.completed = expected.completed;
    immutable.seller_claimed = expected.seller_claimed;
    immutable.buyer_claimed = expected.buyer_claimed;
    require(immutable == expected && next.updated >= expected.updated);
    if (expected.state == kMarketOnSale && next.state == kMarketSold) {
      require(next.updated < expected.deadline && character_id != next.seller_id &&
              next.buyer_id == character_id && next.completed == next.updated &&
              !next.seller_claimed && !next.buyer_claimed && progress.gold >= next.price);
      progress.gold -= next.price;
    } else if (expected.state == kMarketOnSale && next.state == kMarketRevoked) {
      require(character_id == next.seller_id && next.updated < next.deadline &&
              next.buyer_id == 0 && !next.seller_claimed && !next.buyer_claimed &&
              next.completed == 0);
    } else {
      const auto effective_state =
          expected.state == kMarketOnSale && next.updated >= expected.deadline ? kMarketExpired
                                                                               : expected.state;
      require(next.state == effective_state && next.buyer_id == expected.buyer_id &&
              next.completed == expected.completed && effective_state != kMarketOnSale);
      if (character_id == next.seller_id) {
        require(!expected.seller_claimed && next.seller_claimed &&
                next.buyer_claimed == expected.buyer_claimed);
        if (effective_state == kMarketSold) {
          require(progress.gold <= kMoneyLimit - next.price);
          progress.gold += next.price;
        } else {
          grant_item = true;
        }
      } else {
        require(effective_state == kMarketSold && character_id == expected.buyer_id &&
                !expected.buyer_claimed && next.buyer_claimed &&
                next.seller_claimed == expected.seller_claimed);
        grant_item = true;
      }
    }
  }
  if (next.stackable && (remove_item || grant_item)) {
    auto &quantity = progress.stackables[next.item.cms_id];
    if (remove_item) {
      require(quantity >= next.quantity);
      quantity -= next.quantity;
    } else {
      require(quantity <= std::numeric_limits<int>::max() - next.quantity);
      quantity += next.quantity;
    }
  }
  require(progress == after);
  const bool removes_instance = remove_item && !next.stackable;
  const bool grants_instance = grant_item && !next.stackable;
  require(trade.removed_item_ids.size() == (removes_instance ? 1 : 0) &&
          trade.granted_items.size() == (grants_instance ? 1 : 0));
  if (removes_instance) {
    require(next.quantity == 1 && next.item.quantity == 1 && next.item.id > 0 &&
            trade.removed_item_ids.front() == next.item.id);
  }
  if (grants_instance) {
    validate_character_item(trade.granted_items.front());
    require(same_grant(trade.granted_items.front(), next.item));
  }
}

bool market_query_matches(const MarketListing &listing, std::int64_t character_id,
                          const MarketQuery &query, std::int64_t now) {
  if (query.history) {
    return listing.state == kMarketSold &&
           (listing.seller_id == character_id || listing.buyer_id == character_id);
  }
  if (query.main_type && listing.main_type != *query.main_type) {
    return false;
  }
  if (query.sub_type && listing.sub_type != *query.sub_type) {
    return false;
  }
  if (query.type == 0) {
    return listing.state == kMarketOnSale && now < listing.deadline &&
           listing.seller_id != character_id;
  }
  if (query.type == 1) {
    // Listings that still hold seller assets. Settled listings remain in trade history, so a
    // retrieved listing does not reappear when the client re-queries after MarketRevokeRes.
    return listing.seller_id == character_id && !listing.seller_claimed;
  }
  return (listing.seller_id == character_id && !listing.seller_claimed &&
          (listing.state != kMarketOnSale || now >= listing.deadline)) ||
         (listing.buyer_id == character_id && listing.state == kMarketSold &&
          !listing.buyer_claimed);
}

MarketPage market_page(std::vector<MarketListing> listings, const MarketQuery &query) {
  require(query.type >= 0 && query.type <= 2 && query.sort >= 0 && query.sort <= 2 &&
          query.count > 0 && query.count <= 100);
  require(listings.size() <= std::numeric_limits<std::uint32_t>::max());
  std::ranges::sort(listings, [&](const auto &left, const auto &right) {
    if (query.history) {
      return std::tie(left.completed, left.id) > std::tie(right.completed, right.id);
    }
    if (query.sort != 0 && left.price != right.price) {
      return query.sort == 1 ? left.price < right.price : left.price > right.price;
    }
    return left.id > right.id;
  });
  MarketPage page;
  page.total = static_cast<std::uint32_t>(listings.size());
  const auto start = std::min<std::size_t>(query.start, listings.size());
  const auto end = std::min<std::size_t>(start + query.count, listings.size());
  page.listings.assign(listings.begin() + start, listings.begin() + end);
  return page;
}

MemoryMarketRepository::MemoryMarketRepository(std::shared_ptr<MemoryDatabase> database)
    : database_(std::move(database)) {
  // Reproduce the former in-memory deletion veto: a character with unclaimed escrow (as
  // seller or buyer) cannot be removed. The callback runs with database_->mutex held.
  MemoryDatabase::CharacterDeletionHook hook;
  hook.blocks_deletion = [this](std::int64_t character_id) {
    for (const auto &[id, listing] : market_listings_) {
      if ((listing.seller_id == character_id && !listing.seller_claimed) ||
          (listing.buyer_id == character_id && !listing.buyer_claimed)) {
        return true;
      }
    }
    return false;
  };
  std::lock_guard lock(database_->mutex);
  deletion_hook_index_ = database_->deletion_hooks.size();
  database_->deletion_hooks.push_back(std::move(hook));
}

MemoryMarketRepository::~MemoryMarketRepository() {
  // The database can outlive this repository through the shared accessor; neutralize the
  // hook so its callback never touches the listings table after it is gone.
  std::lock_guard lock(database_->mutex);
  if (deletion_hook_index_ < database_->deletion_hooks.size()) {
    database_->deletion_hooks[deletion_hook_index_].blocks_deletion = nullptr;
  }
}

std::optional<MarketListing> MemoryMarketRepository::market_listing(int world_id,
                                                                    std::int64_t listing_id) {
  std::lock_guard lock(database_->mutex);
  const auto found = market_listings_.find(listing_id);
  if (found == market_listings_.end() || found->second.world_id != world_id) {
    return std::nullopt;
  }
  return found->second;
}

std::optional<MarketListing>
MemoryMarketRepository::market_registration(int world_id, std::int64_t seller_id,
                                            const std::string &key) {
  std::lock_guard lock(database_->mutex);
  for (const auto &[id, listing] : market_listings_) {
    if (listing.world_id == world_id && listing.seller_id == seller_id &&
        listing.registration_key == key) {
      return listing;
    }
  }
  return std::nullopt;
}

MarketPage MemoryMarketRepository::market_query(int world_id, std::int64_t character_id,
                                                const MarketQuery &query, std::int64_t now) {
  std::lock_guard lock(database_->mutex);
  std::vector<MarketListing> listings;
  for (const auto &[id, listing] : market_listings_) {
    if (listing.world_id == world_id && market_query_matches(listing, character_id, query, now)) {
      listings.push_back(listing);
    }
  }
  return market_page(std::move(listings), query);
}

MarketCommit MemoryMarketRepository::market_commit(const Character &actor,
                                                   const WorldProgress &before,
                                                   const WorldProgress &after,
                                                   const ItemTrade &trade,
                                                   const MarketListing &expected,
                                                   const MarketListing &next) {
  validate_market_transition(actor.id, before, after, trade, expected, next);
  std::lock_guard lock(database_->mutex);
  const auto owner = std::ranges::find_if(database_->characters, [&](const auto &character) {
    return character.id == actor.id && character.account_id == actor.account_id &&
           character.world_id == actor.world_id && character.world_id == next.world_id;
  });
  if (owner == database_->characters.end()) {
    return {};
  }
  if (expected.id == 0) {
    std::size_t pending{};
    for (const auto &[id, listing] : market_listings_) {
      if (listing.seller_id == actor.id) {
        if (listing.registration_key == next.registration_key) {
          if (!same_registration(listing, next)) {
            return {};
          }
          return {true, true, listing, {}};
        }
        pending += !listing.seller_claimed;
      }
    }
    if (pending >= kMaximumMarketSellerListings) {
      return {};
    }
  } else {
    const auto existing = market_listings_.find(expected.id);
    if (existing == market_listings_.end() || existing->second != expected) {
      return {};
    }
  }
  auto &stored = database_->world_progress[actor.id];
  auto current = stored;
  current.experience = owner->experience;
  current.level = owner->level;
  current.build = owner->build;
  if (current != before) {
    return {};
  }
  auto items = owner->items;
  for (const auto id : trade.removed_item_ids) {
    const auto item =
        std::ranges::find_if(items, [id](const auto &value) { return value.id == id; });
    if (item == items.end() || *item != next.item || item->equip_slot) {
      return {};
    }
    items.erase(item);
  }
  auto granted = trade.granted_items;
  auto next_item_id = database_->next_item_id;
  for (auto &item : granted) {
    if (std::ranges::any_of(items, [&](const auto &value) {
          return value.inventory_slot == item.inventory_slot;
        })) {
      return {};
    }
    item.id = next_item_id++;
    items.push_back(item);
  }
  auto committed = next;
  if (expected.id == 0) {
    committed.id = next_market_id_;
    market_listings_.emplace(committed.id, committed);
    ++next_market_id_;
  } else {
    market_listings_.at(committed.id) = committed;
  }
  detail::commit_progress_and_items(*database_, *owner, after, std::move(items), next_item_id);
  return {true, false, committed, granted};
}

std::optional<MarketListing> PostgresMarketRepository::market_listing(int world_id,
                                                                      std::int64_t listing_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(std::string("SELECT ") + kColumns +
                                     " FROM emulator_market WHERE world_id=$1 AND id=$2",
                                     pqxx::params{world_id, listing_id});
  if (rows.empty()) {
    return std::nullopt;
  }
  return read_listing(rows.front());
}

std::optional<MarketListing>
PostgresMarketRepository::market_registration(int world_id, std::int64_t seller_id,
                                              const std::string &key) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(
      std::string("SELECT ") + kColumns +
          " FROM emulator_market WHERE world_id=$1 AND seller_id=$2 AND registration_key=$3",
      pqxx::params{world_id, seller_id, key});
  if (rows.empty()) {
    return std::nullopt;
  }
  return read_listing(rows.front());
}

MarketPage PostgresMarketRepository::market_query(int world_id, std::int64_t character_id,
                                                  const MarketQuery &query, std::int64_t now) {
  require(query.type >= 0 && query.type <= 2 && query.sort >= 0 && query.sort <= 2 &&
          query.count > 0 && query.count <= 100);
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  transaction.exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
  const std::string filter =
      " FROM emulator_market WHERE world_id=$1 AND "
      "(($9 AND state=1 AND (seller_id=$2 OR buyer_id=$2)) OR (NOT $9 AND "
      "($5=-1 OR main_type=$5) AND ($6=-1 OR sub_type=$6) AND "
      "(($3=0 AND state=0 AND deadline>$4 AND seller_id<>$2) OR "
      "($3=1 AND seller_id=$2 AND NOT seller_claimed) OR ($3=2 AND "
      "((seller_id=$2 AND NOT seller_claimed AND (state<>0 OR deadline<=$4)) OR "
      "(buyer_id=$2 AND state=1 AND NOT buyer_claimed))))))";
  const auto parameters = pqxx::params{world_id,
                                       character_id,
                                       query.type,
                                       now,
                                       query.main_type.value_or(-1),
                                       query.sub_type.value_or(-1),
                                       query.count,
                                       query.start,
                                       query.history};
  // Count and page share a snapshot, including an out-of-range offset.
  const auto count = transaction.exec(
      "SELECT count(*)" + filter + " AND $7::bigint>0 AND $8::bigint>=0", parameters);
  MarketPage page;
  page.total = count.front()[0].as<std::uint32_t>();
  std::string order = query.history ? "completed DESC,id DESC" : "id DESC";
  if (!query.history && query.sort != 0) {
    order = query.sort == 1 ? "price ASC,id DESC" : "price DESC,id DESC";
  }
  const auto rows = transaction.exec(std::string("SELECT ") + kColumns + filter + " ORDER BY " +
                                         order + " LIMIT $7 OFFSET $8",
                                     parameters);
  for (const auto &row : rows) {
    page.listings.push_back(read_listing(row));
  }
  return page;
}

MarketCommit PostgresMarketRepository::market_commit(const Character &actor,
                                                     const WorldProgress &before,
                                                     const WorldProgress &after,
                                                     const ItemTrade &trade,
                                                     const MarketListing &expected,
                                                     const MarketListing &next) {
  validate_market_transition(actor.id, before, after, trade, expected, next);
  try {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    // All gameplay writers lock the character first. No other participant's character is
    // needed: purchase escrows Gold; seller and buyer settle on their own worker later.
    if (actor.world_id != next.world_id ||
        transaction
            .exec("SELECT id FROM emulator_characters WHERE id=$1 AND account_id=$2 "
                  "AND world_id=$3 FOR UPDATE",
                  pqxx::params{actor.id, actor.account_id, actor.world_id})
            .empty()) {
      return {};
    }
    if (expected.id == 0) {
      const auto existing =
          transaction.exec(std::string("SELECT ") + kColumns +
                               " FROM emulator_market WHERE seller_id=$1 AND registration_key=$2",
                           pqxx::params{actor.id, next.registration_key});
      if (!existing.empty()) {
        const auto listing = read_listing(existing.front());
        if (!same_registration(listing, next)) {
          return {};
        }
        return {true, true, listing, {}};
      }
      const auto count = transaction.exec(
          "SELECT count(*) FROM emulator_market WHERE seller_id=$1 AND NOT seller_claimed",
          pqxx::params{actor.id});
      if (count.front()[0].as<std::size_t>() >= kMaximumMarketSellerListings) {
        return {};
      }
    } else {
      const auto rows =
          transaction.exec(std::string("SELECT ") + kColumns +
                               " FROM emulator_market WHERE id=$1 AND world_id=$2 FOR UPDATE",
                           pqxx::params{expected.id, actor.world_id});
      if (rows.empty() || read_listing(rows.front()) != expected) {
        return {};
      }
    }
    if (!detail::write_world_progress_transition(transaction, actor.account_id, actor.id,
                                                 actor.world_id, before, after)) {
      return {};
    }
    for (const auto id : trade.removed_item_ids) {
      const auto removed = transaction.exec(
          "DELETE FROM emulator_character_items WHERE id=$1 AND character_id=$2 AND "
          "equip_slot IS NULL AND item_cms_id=$3 AND quantity=$4 AND durability=$5 AND "
          "grade=$6 AND polish_step=$7 AND created_epoch_ticks=$8 AND inventory_slot=$9 "
          "RETURNING id",
          pqxx::params{id, actor.id, next.item.cms_id, next.item.quantity, next.item.durability,
                       next.item.grade, next.item.polish_step, next.item.created_epoch_ticks,
                       next.item.inventory_slot});
      if (removed.empty()) {
        return {};
      }
    }
    auto granted = trade.granted_items;
    for (auto &item : granted) {
      const auto rows = transaction.exec(
          "INSERT INTO emulator_character_items(character_id,item_cms_id,inventory_slot,"
          "equip_slot,quantity,durability,grade,polish_step,created_epoch_ticks) "
          "VALUES($1,$2,$3,NULL,$4,$5,$6,$7,$8) RETURNING id",
          pqxx::params{actor.id, item.cms_id, item.inventory_slot, item.quantity, item.durability,
                       item.grade, item.polish_step, item.created_epoch_ticks});
      item.id = rows.front()[0].as<std::int64_t>();
    }
    auto committed = next;
    if (expected.id == 0) {
      const auto rows = transaction.exec(
          "INSERT INTO emulator_market(world_id,seller_id,buyer_id,item,stackable,quantity,"
          "main_type,sub_type,price,deadline,updated,completed,state,seller_claimed,buyer_claimed,"
          "registration_key) VALUES($1,$2,0,$3::jsonb,$4,$5,$6,$7,$8,$9,$10,0,0,false,false,$11) "
          "RETURNING id",
          pqxx::params{next.world_id, next.seller_id, item_json(next.item).dump(), next.stackable,
                       next.quantity, next.main_type, next.sub_type, next.price, next.deadline,
                       next.updated, next.registration_key});
      committed.id = rows.front()[0].as<std::int64_t>();
    } else {
      transaction.exec("UPDATE emulator_market SET buyer_id=$2,updated=$3,completed=$4,state=$5,"
                       "seller_claimed=$6,buyer_claimed=$7 WHERE id=$1",
                       pqxx::params{next.id, next.buyer_id, next.updated, next.completed,
                                    next.state, next.seller_claimed, next.buyer_claimed});
    }
    transaction.commit();
    return {true, false, committed, granted};
  } catch (const pqxx::unique_violation &) {
    return {};
  }
}

} // namespace chronicles
