#include "world/features/market/market.hpp"
#include "world/features/market/market_protocol.hpp"
#include "world/features/market/market_repository.hpp"

#include "auth/auth.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "testing/memory_repositories.hpp"
#include "data/client_catalog.hpp"
#include "common/clock_ticks.hpp"
#include "ec.generated.pb.h"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <thread>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr int kMaterial = 701;
constexpr int kEquipment = 702;
constexpr std::int64_t kNow = 10'000'000'000;

// The storage interfaces a market flow touches: accounts and characters for ownership and
// deletion, character state for progress and items, market for listings and escrow. Helpers
// take this bundle so tests and the server wire the repositories the same way.
struct MarketStorage {
  AccountRepository &accounts;
  CharacterRepository &characters;
  CharacterStateRepository &state;
  MarketRepository &market;
};

ShopCatalog shop_catalog() {
  ShopCatalogTables tables;
  tables.items = {
      {{"id", "701"}, {"category", "CraftMaterial"}, {"grade", "Normal"}},
      {{"id", "702"}, {"category", "Equipment"}, {"grade", "Rare"}, {"maxDurability", "10000"}}};
  tables.game_variables = {{{"name", "ItemMaxStack"}, {"value", "999"}},
                           {{"name", "ResetBaseDateTime"}, {"value", "2025-01-01 00:00:00"}}};
  tables.money = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  return ShopCatalog(tables);
}

MarketCatalog market_catalog() {
  return MarketCatalog({{{"id", "701"}}, {{"id", "702"}}},
                       {{{"FK_ItemId", "701"}, {"FK_CategoryGroupId", "301"}},
                        {{"FK_ItemId", "702"}, {"FK_CategoryGroupId", "101"}}},
                       {{{"Id", "301"}, {"MainType", "Material"}, {"SubType", "RareCraftMaterial"}},
                        {{"Id", "101"}, {"MainType", "Equipment"}, {"SubType", "Weapon"}}});
}

Character owner(MarketStorage &storage, const std::string &suffix) {
  Account account;
  account.name = "market" + suffix;
  account.salt = "test-salt";
  account.hash = "test-hash";
  if (!storage.accounts.add_account(account)) {
    throw std::runtime_error("Cannot create market test account");
  }
  Character character;
  character.account_id = storage.accounts.account(account.name)->id;
  character.name = "Mk" + suffix;
  return storage.characters.create(character);
}

WorldProgress progress(MarketStorage &storage, const Character &actor) {
  return storage.state.load_world_progress(actor.account_id, actor.id, actor.world_id);
}

void fund(MarketStorage &storage, const Character &actor, std::int64_t gold, int material = 0) {
  const auto before = progress(storage, actor);
  auto after = before;
  ++after.revision;
  after.gold = gold;
  after.stackables[kMaterial] = material;
  ASSERT_TRUE(storage.state.save_world_progress(actor.account_id, actor.id, actor.world_id,
                                                before, after));
}

MarketResult call(MarketStorage &storage, const Character &actor, std::uint16_t opcode,
                  const Json &request = Json::object(), std::uint32_t sequence = 1,
                  std::int64_t now = kNow) {
  const auto loaded = storage.characters.character_by_id(actor.id);
  if (!loaded) {
    throw std::runtime_error("Missing market actor");
  }
  return receive_market(storage.market, *loaded, progress(storage, actor), market_catalog(),
                        shop_catalog(), opcode, request, sequence, "test-market-scope-00000000",
                        now);
}

int code(const MarketResult &result) {
  return result.packets.back().message.at("ec").get<int>();
}

std::int64_t register_stack(MarketStorage &storage, const Character &seller, int count = 3,
                            std::int64_t price = 25, std::uint32_t sequence = 1) {
  auto result = call(storage, seller, 20116,
                     {{"identity", {{"stackable", {{"item_cms_id", kMaterial}, {"count", count}}}}},
                      {"price", std::to_string(price)},
                      {"period", 2}},
                     sequence);
  if (code(result) != 0) {
    throw std::runtime_error("Registration failed: " + std::to_string(code(result)));
  }
  return std::stoll(
      result.packets.back().message.at("listing_registered").at("listing_id").get<std::string>());
}

Json listing_request(std::int64_t id) {
  return {{"listing_id", std::to_string(id)}};
}

void exercise_settlement(MarketStorage &storage, const std::string &suffix) {
  const auto seller = owner(storage, "s" + suffix);
  const auto buyer = owner(storage, "b" + suffix);
  const auto stranger = owner(storage, "x" + suffix);
  fund(storage, seller, 10, 9);
  fund(storage, buyer, 100);
  fund(storage, stranger, 100);
  const auto id = register_stack(storage, seller);
  const auto listed = call(storage, buyer, 20119,
                           {{"count", 100}, {"main_type", 2}, {"sub_type", 6}, {"sort_type", 2}});
  EXPECT_EQ(code(listed), 0);
  EXPECT_GE(listed.packets.back().message.at("total").get<int>(), 1);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 6);
  EXPECT_FALSE(storage.characters.delete_character(seller.id));
  const auto request = listing_request(id);
  EXPECT_EQ(code(call(storage, seller, 20118, request)),
            x5::ec::Market_NotAvailablePurchaseSelfRegisteredItem);
  EXPECT_EQ(code(call(storage, stranger, 20117, request)), x5::ec::Market_SellerMismatch);
  EXPECT_EQ(code(call(storage, stranger, 20120, request)), x5::ec::Market_StillOnSale);
  EXPECT_EQ(code(call(storage, buyer, 20118, request)), 0);
  EXPECT_EQ(progress(storage, buyer).gold, 75);
  EXPECT_EQ(progress(storage, buyer).stackables.at(kMaterial), 0);
  EXPECT_EQ(progress(storage, seller).gold, 10);
  EXPECT_EQ(code(call(storage, buyer, 20118, request)),
            x5::ec::Market_NotAvailableAlreadySoldItem);
  EXPECT_EQ(code(call(storage, stranger, 20120, request)), x5::ec::Market_BuyerMismatch);
  EXPECT_EQ(code(call(storage, stranger, 20121, request)), x5::ec::Market_ListingNotFound);
  EXPECT_FALSE(storage.characters.delete_character(buyer.id));
  EXPECT_EQ(code(call(storage, buyer, 20120, request)), 0);
  EXPECT_EQ(code(call(storage, buyer, 20120, request)), x5::ec::Market_AlreadyClaimed);
  EXPECT_EQ(progress(storage, buyer).stackables.at(kMaterial), 3);
  EXPECT_EQ(code(call(storage, seller, 20120, request)), 0);
  EXPECT_EQ(code(call(storage, seller, 20120, request)), x5::ec::Market_AlreadyClaimed);
  EXPECT_EQ(progress(storage, seller).gold, 35);
  EXPECT_EQ(call(storage, seller, 20122).packets.back().message.at("entries").size(), 1);
  EXPECT_EQ(call(storage, buyer, 20122).packets.back().message.at("entries").at(0).at("side"), 0);
  EXPECT_TRUE(call(storage, stranger, 20122).packets.back().message.at("entries").empty());
  EXPECT_TRUE(storage.characters.delete_character(seller.id));
  EXPECT_TRUE(storage.characters.delete_character(buyer.id));
  const auto replacement = owner(storage, "r" + suffix);
  EXPECT_NE(replacement.id, seller.id);
  EXPECT_NE(replacement.id, buyer.id);
}

TEST(Market, AtomicSettlementOwnershipReceiptsHistoryAndDeletion) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market};
  exercise_settlement(storage, "memory");
}

TEST(Market, MemoryEscrowBlocksCharacterDeletion) {
  // Bare repositories over one shared database, wired like create_repositories builds them:
  // the market repository's deletion hook vetoes removing a character with unclaimed escrow.
  const auto database = std::make_shared<MemoryDatabase>();
  MemoryAccountRepository accounts{database};
  MemoryCharacterRepository characters{database};
  MemoryCharacterStateRepository state{database};
  MemoryMarketRepository market{database};
  MarketStorage storage{accounts, characters, state, market};

  const auto seller = owner(storage, "escrow");
  fund(storage, seller, 0, 3);
  ASSERT_TRUE(register_stack(storage, seller) > 0);
  EXPECT_FALSE(characters.delete_character(seller.id));
}

TEST(Market, RevokeAndExclusiveExpiryReturnEscrowOnce) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market};
  const auto seller = owner(storage, "expiry");
  const auto buyer = owner(storage, "expirybuyer");
  fund(storage, seller, 10, 12);
  fund(storage, buyer, 100);
  const auto id = register_stack(storage, seller);
  EXPECT_EQ(code(call(storage, seller, 20117, listing_request(id))), 0);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 9);
  EXPECT_EQ(code(call(storage, seller, 20120, listing_request(id))), 0);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 12);
  const auto expired = register_stack(storage, seller, 3, 25, 2);
  const auto deadline = storage.market.market_listing(1, expired)->deadline;
  EXPECT_EQ(code(call(storage, buyer, 20118, listing_request(expired), 3, deadline)),
            x5::ec::Market_NotAvailableAlreadySoldItem);
  EXPECT_EQ(code(call(storage, seller, 20120, listing_request(expired), 5, deadline)), 0);
  EXPECT_EQ(storage.market.market_listing(1, expired)->state, 3);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 12);
  EXPECT_EQ(code(call(storage, seller, 20117, listing_request(expired), 6, deadline)),
            x5::ec::Market_AlreadyClaimed);
  EXPECT_EQ(progress(storage, buyer).gold, 100);
}

int registered_by_me_total(MarketStorage &storage, const Character &seller, std::int64_t now) {
  const auto result =
      call(storage, seller, 20119, {{"count", 100}, {"listing_type", 1}}, 1, now);
  return result.packets.back().message.at("total").get<int>();
}

TEST(Market, RevokeRetrievesAnExpiredListingOnceAndHidesItFromMyListings) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state, market};
  const auto seller = owner(storage, "retrieve");
  fund(storage, seller, 10, 12);

  const auto expired = register_stack(storage, seller, 3, 25, 1);
  const auto deadline = storage.market.market_listing(1, expired)->deadline;
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 9);
  EXPECT_EQ(registered_by_me_total(storage, seller, deadline), 1);

  // "Retrieve" on the client's listing-details screen sends MarketRevokeReq.
  const auto retrieved = call(storage, seller, 20117, listing_request(expired), 2, deadline);
  ASSERT_EQ(code(retrieved), 0);
  ASSERT_EQ(retrieved.packets.size(), 2);
  EXPECT_EQ(retrieved.packets.front().opcode, kMarketItemsUpdate);
  EXPECT_EQ(retrieved.packets.front().message.at("stackables").at(0).at("quantity"), 12);
  EXPECT_EQ(retrieved.packets.back().opcode, 222);
  EXPECT_EQ(retrieved.packets.back().message.at("listing_id"), std::to_string(expired));
  EXPECT_TRUE(retrieved.committed_progress);

  const auto settled = storage.market.market_listing(1, expired);
  EXPECT_EQ(settled->state, kMarketExpired);
  EXPECT_TRUE(settled->seller_claimed);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 12);
  EXPECT_EQ(progress(storage, seller).gold, 10);
  EXPECT_EQ(registered_by_me_total(storage, seller, deadline), 0);

  EXPECT_EQ(code(call(storage, seller, 20117, listing_request(expired), 3, deadline)),
            x5::ec::Market_AlreadyClaimed);
  EXPECT_EQ(code(call(storage, seller, 20120, listing_request(expired), 4, deadline)),
            x5::ec::Market_AlreadyClaimed);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 12);
  EXPECT_TRUE(storage.characters.delete_character(seller.id));
}

TEST(Market, RevokeRetrievesACancelledListingButNeverASoldOne) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state, market};
  const auto seller = owner(storage, "cancelretrieve");
  const auto buyer = owner(storage, "cancelbuyer");
  fund(storage, seller, 10, 12);
  fund(storage, buyer, 100);

  const auto cancelled = register_stack(storage, seller, 3, 25, 1);
  EXPECT_EQ(code(call(storage, seller, 20117, listing_request(cancelled), 2)), 0);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 9);
  EXPECT_EQ(code(call(storage, seller, 20117, listing_request(cancelled), 3)), 0);
  EXPECT_EQ(storage.market.market_listing(1, cancelled)->state, kMarketRevoked);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 12);

  const auto sold = register_stack(storage, seller, 3, 25, 4);
  EXPECT_EQ(code(call(storage, buyer, 20118, listing_request(sold))), 0);
  EXPECT_EQ(code(call(storage, seller, 20117, listing_request(sold), 5)),
            x5::ec::Market_NotSellRegisterCancel);
  EXPECT_FALSE(storage.market.market_listing(1, sold)->seller_claimed);
  EXPECT_EQ(registered_by_me_total(storage, seller, kNow), 1);
  EXPECT_EQ(code(call(storage, seller, 20120, listing_request(sold), 6)), 0);
  EXPECT_EQ(progress(storage, seller).gold, 35);
  EXPECT_EQ(registered_by_me_total(storage, seller, kNow), 0);
}

TEST(Market, RevokeRetrievesExpiredEquipmentWithItsAttributes) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state, market};
  const auto seller = owner(storage, "retrieveequipment");
  fund(storage, seller, 0);

  auto before = progress(storage, seller);
  auto after = before;
  ++after.revision;
  ItemTrade grant;
  CharacterItem equipment;
  equipment.cms_id = kEquipment;
  equipment.inventory_slot = 7;
  equipment.grade = 4;
  equipment.durability = 6700;
  equipment.created_epoch_ticks = 654321;
  grant.granted_items.push_back(equipment);
  const auto items = storage.state.commit_item_trade(seller.account_id, seller.id, 1, before,
                                                     after, grant);
  ASSERT_TRUE(items);
  const auto item = items->front();

  const Json request{{"identity", {{"item_id", std::to_string(item.id)}}}, {"price", "300"}};
  const auto registered = call(storage, seller, 20116, request);
  ASSERT_EQ(code(registered), 0);
  const auto id = std::stoll(registered.packets.back()
                                 .message.at("listing_registered")
                                 .at("listing_id")
                                 .get<std::string>());
  const auto deadline = storage.market.market_listing(1, id)->deadline;

  const auto retrieved = call(storage, seller, 20117, listing_request(id), 2, deadline);
  ASSERT_EQ(code(retrieved), 0);
  ASSERT_EQ(retrieved.packets.size(), 2);
  const auto &granted = retrieved.packets.front().message.at("items");
  ASSERT_EQ(granted.size(), 1);
  EXPECT_EQ(granted.at(0).at("item_cms_id"), kEquipment);

  const auto restored = storage.characters.character_by_id(seller.id);
  const auto found = std::ranges::find_if(
      restored->items, [](const auto &owned) { return owned.cms_id == kEquipment; });
  ASSERT_NE(found, restored->items.end());
  EXPECT_EQ(found->durability, item.durability);
  EXPECT_EQ(found->grade, item.grade);
  EXPECT_EQ(found->created_epoch_ticks, item.created_epoch_ticks);
  EXPECT_FALSE(found->equip_slot);
}

TEST(Market, QueryFiltersSortingPagingAndWorldIsolation) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market};
  const auto seller = owner(storage, "queryseller");
  const auto buyer = owner(storage, "querybuyer");
  fund(storage, seller, 0, 20);
  register_stack(storage, seller, 2, 30, 1);
  const auto cheap = register_stack(storage, seller, 2, 10, 2);
  register_stack(storage, seller, 2, 20, 3);
  const auto queried = call(storage, buyer, 20119,
                            {{"count", 1}, {"start_index", 0}, {"sort_type", 1}, {"main_type", 2}});
  const auto &page = queried.packets.back().message;
  EXPECT_EQ(page.at("total"), 3);
  EXPECT_EQ(page.at("listings").size(), 1);
  EXPECT_EQ(page.at("listings").at(0).at("listing_id"), std::to_string(cheap));
  EXPECT_EQ(call(storage, buyer, 20119, {{"count", 10}, {"main_type", 0}})
                .packets.back()
                .message.at("total"),
            0);
  EXPECT_TRUE(call(storage, buyer, 20119, {{"count", 1}, {"start_index", 99}})
                  .packets.back()
                  .message.at("listings")
                  .empty());
  EXPECT_FALSE(storage.market.market_listing(2, cheap));
  EXPECT_EQ(storage.market.market_query(2, buyer.id, MarketQuery{}, kNow).total, 0);
  const auto named_enums = call(storage, seller, 20119,
                                {{"count", 10},
                                 {"listing_type", "MarketListingType_RegisteredByMe"},
                                 {"main_type", "EX5ItemMarketMainType_Material"},
                                 {"sub_type", "EX5ItemMarketSubType_RareCraftMaterial"},
                                 {"sort_type", "EX5MarketListingSortType_PriceDescending"}});
  EXPECT_EQ(code(named_enums), 0);
  EXPECT_EQ(named_enums.packets.back().message.at("total"), 3);
  EXPECT_NE(code(call(storage, buyer, 20119, {{"count", 101}})), 0);
  EXPECT_NE(code(call(storage, buyer, 20119, {{"count", 1}, {"start_index", "4294967296"}})), 0);
}

TEST(Market, RejectionsCapacityAndStaleRevisionLeaveEscrowUntouched) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market};
  const auto seller = owner(storage, "rejects");
  const auto buyer = owner(storage, "rejectbuyer");
  fund(storage, seller, 0, 10);
  fund(storage, buyer, 0);
  for (const auto price : {"0", "-1", "9000000000000001"}) {
    EXPECT_EQ(code(call(storage, seller, 20116,
                        {{"identity", {{"stackable", {{"item_cms_id", kMaterial}, {"count", 1}}}}},
                         {"price", price}})),
              x5::ec::Market_InvalidPrice);
  }
  const auto id = register_stack(storage, seller);
  EXPECT_EQ(code(call(storage, buyer, 20118, listing_request(id))), x5::ec::Market_NotEnoughGold);
  EXPECT_EQ(storage.market.market_listing(1, id)->state, 0);
  fund(storage, buyer, 100, 999);
  EXPECT_EQ(code(call(storage, buyer, 20118, listing_request(id))), 0);
  EXPECT_EQ(code(call(storage, buyer, 20120, listing_request(id))),
            x5::ec::Market_NotEnoughInventory);
  EXPECT_FALSE(storage.market.market_listing(1, id)->buyer_claimed);
  EXPECT_EQ(progress(storage, buyer).stackables.at(kMaterial), 999);
  auto expected = *storage.market.market_listing(1, id);
  auto next = expected;
  next.seller_claimed = true;
  auto before = progress(storage, seller);
  auto after = before;
  ++after.revision;
  after.gold += next.price;
  fund(storage, seller, 1, 7);
  EXPECT_FALSE(storage.market.market_commit(seller, before, after, {}, expected, next).committed);
  EXPECT_FALSE(storage.market.market_listing(1, id)->seller_claimed);
  EXPECT_EQ(progress(storage, seller).gold, 1);
}

TEST(Market, ConcurrentBuyersSpendOnlyOnce) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market};
  const auto seller = owner(storage, "race");
  const auto first = owner(storage, "racea");
  const auto second = owner(storage, "raceb");
  fund(storage, seller, 0, 3);
  fund(storage, first, 100);
  fund(storage, second, 100);
  const auto id = register_stack(storage, seller);
  std::atomic<int> successes{};
  auto buy = [&](const Character &buyer) {
    if (code(call(storage, buyer, 20118, listing_request(id))) == 0) {
      ++successes;
    }
  };
  std::thread left(buy, first);
  std::thread right(buy, second);
  left.join();
  right.join();
  EXPECT_EQ(successes, 1);
  EXPECT_EQ(progress(storage, first).gold + progress(storage, second).gold, 175);
}

TEST(Market, EquipmentEscrowPreservesAttributesAndRegistrationRetry) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market};
  const auto seller = owner(storage, "equipment");
  fund(storage, seller, 0);
  auto before = progress(storage, seller);
  auto after = before;
  ++after.revision;
  ItemTrade grant;
  CharacterItem equipment;
  equipment.cms_id = kEquipment;
  equipment.inventory_slot = 100;
  equipment.grade = 3;
  equipment.durability = 4321;
  equipment.created_epoch_ticks = 123456;
  grant.granted_items.push_back(equipment);
  const auto items = storage.state.commit_item_trade(seller.account_id, seller.id, 1, before,
                                                   after, grant);
  ASSERT_TRUE(items);
  const auto item = items->front();
  const Json request{{"identity", {{"item_id", std::to_string(item.id)}}}, {"price", "25"}};
  const auto result = call(storage, seller, 20116, request);
  ASSERT_EQ(code(result), 0);
  const auto id = std::stoll(
      result.packets.back().message.at("listing_registered").at("listing_id").get<std::string>());
  EXPECT_EQ(storage.market.market_listing(1, id)->item, item);
  EXPECT_EQ(code(call(storage, seller, 20117, listing_request(id))), 0);
  EXPECT_EQ(code(call(storage, seller, 20120, listing_request(id))), 0);
  const auto restored = storage.characters.character_by_id(seller.id);
  const auto found = std::ranges::find_if(
      restored->items, [](const auto &owned) { return owned.cms_id == kEquipment; });
  ASSERT_NE(found, restored->items.end());
  EXPECT_EQ(found->durability, item.durability);
  EXPECT_EQ(found->grade, item.grade);
  EXPECT_EQ(found->created_epoch_ticks, item.created_epoch_ticks);
  EXPECT_NE(found->id, item.id);
  // The same storage request key cannot escrow a second stack.
  fund(storage, seller, 0, 10);
  const auto stack = register_stack(storage, seller, 3, 25, 10);
  const auto retry = register_stack(storage, seller, 3, 25, 10);
  EXPECT_EQ(stack, retry);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 7);
}

TEST(Market, ExportedCatalogAndEveryResponseRoundTrips) {
  ClientCatalog catalog(std::filesystem::path(CHRONICLES_PROJECT_ROOT) / "data/generated/catalog");
  const auto market = MarketCatalog::load(&catalog);
  ASSERT_TRUE(market);
  EXPECT_NE(market->item(77777), nullptr);
  EXPECT_EQ(market->item(1), nullptr);
  MemoryRepositories store;
  MemoryMarketRepository market_repository{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market_repository};
  const auto seller = owner(storage, "wire");
  const auto buyer = owner(storage, "wirebuyer");
  fund(storage, seller, 0, 10);
  fund(storage, buyer, 100);
  Protocol protocol(std::filesystem::path(CHRONICLES_PROJECT_ROOT) / "protocol");
  const auto id = register_stack(storage, seller);
  for (std::uint16_t opcode = 20116; opcode <= 20122; ++opcode) {
    Json request = listing_request(id);
    if (opcode == 20116) {
      request = {{"identity", {{"stackable", {{"item_cms_id", kMaterial}, {"count", 1}}}}},
                 {"price", "25"}};
    } else if (opcode == 20119) {
      request = {{"count", 10}};
    } else if (opcode == 20122) {
      request = Json::object();
    }
    // Exercise the actual protobuf JSON representation, including named enum values.
    const auto decoded = protocol.decode(protocol.encode(opcode, request, opcode));
    const auto result =
        call(storage, opcode == 20117 ? seller : buyer, opcode, decoded.at("message"), opcode);
    for (const auto &packet : result.packets) {
      EXPECT_NO_THROW(protocol.decode(protocol.encode(packet.opcode, packet.message)));
    }
    const auto error = market_error(opcode, request, x5::ec::Market_TransactionFailed);
    EXPECT_NO_THROW(protocol.decode(protocol.encode(error.opcode, error.message)));
  }
}

TEST(Market, RegistrationAtCapacityAndFullStackRetry) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market};
  const auto seller = owner(storage, "limit");
  fund(storage, seller, 0, 101);
  for (std::uint32_t sequence = 1; sequence <= 100; ++sequence) {
    register_stack(storage, seller, 1, 1, sequence);
  }
  const auto refused = call(
      storage, seller, 20116,
      {{"identity", {{"stackable", {{"item_cms_id", kMaterial}, {"count", 1}}}}}, {"price", "1"}},
      101);
  EXPECT_EQ(code(refused), x5::ec::Market_NotEnoughSellListStorage);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 1);
  const auto other = owner(storage, "fullretry");
  fund(storage, other, 0, 3);
  const auto id = register_stack(storage, other, 3);
  EXPECT_EQ(register_stack(storage, other, 3), id);
  EXPECT_EQ(progress(storage, other).stackables.at(kMaterial), 0);
}

TEST(Market, ForgedDeltasAndOwnershipCannotCommit) {
  MemoryRepositories store;
  MemoryMarketRepository market{store.database};
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market};
  const auto seller = owner(storage, "forged");
  const auto stranger = owner(storage, "forger");
  fund(storage, seller, 0, 3);
  fund(storage, stranger, 100);
  const auto id = register_stack(storage, seller);
  const auto expected = *storage.market.market_listing(1, id);
  auto next = expected;
  next.state = 1;
  next.buyer_id = stranger.id;
  next.completed = kNow;
  auto before = progress(storage, stranger);
  auto after = before;
  ++after.revision;
  // Omitting the price debit or adding an unrelated reward is rejected by storage itself.
  EXPECT_THROW(storage.market.market_commit(stranger, before, after, {}, expected, next),
               std::invalid_argument);
  after.gold -= next.price;
  after.hersander_token = 1;
  EXPECT_THROW(storage.market.market_commit(stranger, before, after, {}, expected, next),
               std::invalid_argument);
  after.hersander_token = 0;
  auto forged = stranger;
  forged.account_id = seller.account_id;
  EXPECT_FALSE(storage.market.market_commit(forged, before, after, {}, expected, next).committed);
  EXPECT_EQ(progress(storage, stranger).gold, 100);
  EXPECT_EQ(storage.market.market_listing(1, id)->state, 0);
}

TEST(Market, PostgresSettlementPersistsAcrossStoreRecreation) {
  const auto *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (connection == nullptr) {
    GTEST_SKIP() << "Requires disposable PostgreSQL database";
  }
  auto store = make_postgres_character_repositories(connection);
  std::ifstream stream(std::filesystem::path(CHRONICLES_PROJECT_ROOT) /
                           "migrations/001_accounts_characters.sql",
                       std::ios::binary);
  const std::string migration{std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>()};
  ASSERT_FALSE(migration.empty());
  migrate_postgres(connection, migration);
  PostgresMarketRepository market(connection);
  MarketStorage storage{*store.accounts, *store.characters, *store.character_state,
                        market};
  const auto suffix = random_hex(5);
  exercise_settlement(storage, suffix);
  const auto seller = owner(storage, "p" + suffix);
  const auto buyer = owner(storage, "q" + suffix);
  fund(storage, seller, 10, 6);
  fund(storage, buyer, 100);
  const auto id = register_stack(storage, seller);
  EXPECT_EQ(code(call(storage, buyer, 20118, listing_request(id))), 0);
  auto reopened = make_postgres_character_repositories(connection);
  PostgresMarketRepository reopened_market(connection);
  MarketStorage reopened_storage{*reopened.accounts, *reopened.characters,
                                 *reopened.character_state, reopened_market};
  EXPECT_EQ(reopened_market.market_listing(1, id)->buyer_id, buyer.id);
  EXPECT_EQ(code(call(reopened_storage, buyer, 20120, listing_request(id))), 0);
  EXPECT_EQ(code(call(reopened_storage, seller, 20120, listing_request(id))), 0);
  auto final_store = make_postgres_character_repositories(connection);
  PostgresMarketRepository final_market(connection);
  MarketStorage final_storage{*final_store.accounts, *final_store.characters,
                              *final_store.character_state, final_market};
  EXPECT_EQ(code(call(final_storage, buyer, 20120, listing_request(id))),
            x5::ec::Market_AlreadyClaimed);
  EXPECT_EQ(progress(final_storage, buyer).gold, 75);
  EXPECT_EQ(progress(final_storage, buyer).stackables.at(kMaterial), 3);

  // Independent database connections race for one row. Only the winner may debit Gold.
  const auto other = owner(storage, "v" + suffix);
  fund(storage, other, 100);
  const auto race_id = register_stack(storage, seller, 3, 25, 2);
  std::atomic<int> winners{};
  auto purchase = [&](const Character &participant) {
    if (code(call(storage, participant, 20118, listing_request(race_id))) == 0) {
      ++winners;
    }
  };
  std::thread left(purchase, buyer);
  std::thread right(purchase, other);
  left.join();
  right.join();
  EXPECT_EQ(winners, 1);
  EXPECT_EQ(progress(storage, buyer).gold + progress(storage, other).gold, 150);

  const auto winner_id = storage.market.market_listing(1, race_id)->buyer_id;
  const auto winner = winner_id == buyer.id ? buyer : other;
  const auto material_before = progress(storage, winner).stackables.at(kMaterial);
  std::atomic<int> claims{};
  auto claim = [&] {
    if (code(call(storage, winner, 20120, listing_request(race_id))) == 0) {
      ++claims;
    }
  };
  std::thread first_claim(claim);
  std::thread second_claim(claim);
  first_claim.join();
  second_claim.join();
  EXPECT_EQ(claims, 1);
  EXPECT_EQ(progress(storage, winner).stackables.at(kMaterial), material_before + 3);

  const auto expected = *storage.market.market_listing(1, race_id);
  auto next = expected;
  next.seller_claimed = true;
  const auto before = progress(storage, seller);
  auto after = before;
  ++after.revision;
  after.gold += next.price;
  fund(storage, seller, before.gold, 0);
  EXPECT_FALSE(storage.market.market_commit(seller, before, after, {}, expected, next).committed);
  EXPECT_FALSE(storage.market.market_listing(1, race_id)->seller_claimed);
  EXPECT_EQ(progress(storage, seller).gold, before.gold);

  // "Retrieve" (MarketRevokeReq on an expired listing) settles through PostgreSQL, and
  // settled listings leave the seller's registered list.
  fund(storage, seller, before.gold, 3);
  const auto expired_id = register_stack(storage, seller, 3, 25, 3);
  const auto deadline = storage.market.market_listing(1, expired_id)->deadline;
  EXPECT_EQ(code(call(storage, seller, 20117, listing_request(expired_id), 4, deadline)), 0);
  EXPECT_TRUE(storage.market.market_listing(1, expired_id)->seller_claimed);
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 3);
  const auto registered =
      call(storage, seller, 20119, {{"count", 100}, {"listing_type", 1}}, 5, deadline);
  for (const auto &listing : registered.packets.back().message.at("listings")) {
    EXPECT_NE(listing.at("listing_id"), std::to_string(expired_id));
    EXPECT_NE(listing.at("listing_id"), std::to_string(id));
  }
}

TEST(Market, IsolatedPostgresExpiredRetrieveAndClaimRacePreserveEquipmentOnFullInventory) {
  const auto *connection = std::getenv("CHRONICLES_MARKET_ISOLATED_TEST_DATABASE_URL");
  if (connection == nullptr) {
    GTEST_SKIP() << "Requires the isolated marketplace PostgreSQL test cluster";
  }

  pqxx::connection probe(connection);
  pqxx::work probe_transaction(probe);
  const auto identity = probe_transaction.exec(
      "SELECT current_database(), host(inet_server_addr())");
  ASSERT_EQ(identity.front()[0].as<std::string>(), "codex_market_isolated");
  ASSERT_EQ(identity.front()[1].as<std::string>(), "127.0.0.1");
  probe_transaction.commit();

  std::ifstream stream(std::filesystem::path(CHRONICLES_PROJECT_ROOT) /
                           "migrations/001_accounts_characters.sql",
                       std::ios::binary);
  const std::string migration{std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>()};
  ASSERT_FALSE(migration.empty());
  migrate_postgres(connection, migration);

  auto repositories = make_postgres_character_repositories(connection);
  PostgresMarketRepository market(connection);
  MarketStorage storage{*repositories.accounts, *repositories.characters,
                        *repositories.character_state, market};
  const auto suffix = random_hex(5);
  const auto seller = owner(storage, "race" + suffix);
  fund(storage, seller, 10, 12);
  const auto listing_id = register_stack(storage, seller, 3, 25, 1);
  const auto deadline = market.market_listing(1, listing_id)->deadline;

  // Separate repositories and connections model two independent in-flight requests.
  std::barrier start(3);
  std::array<int, 2> outcomes{-1, -1};
  std::array<std::exception_ptr, 2> failures;
  const auto racing_character = storage.characters.character_by_id(seller.id);
  ASSERT_TRUE(racing_character);
  const auto racing_progress = progress(storage, seller);
  PostgresMarketRepository left_market(connection);
  PostgresMarketRepository right_market(connection);
  const auto settle = [&](std::size_t index, std::uint16_t opcode) {
    start.arrive_and_wait();
    try {
      auto &repository = index == 0 ? left_market : right_market;
      const auto result = receive_market(repository, *racing_character, racing_progress,
                                         market_catalog(), shop_catalog(), opcode,
                                         listing_request(listing_id),
                                         static_cast<std::uint32_t>(index + 2),
                                         "test-market-scope-00000000", deadline);
      outcomes[index] = code(result);
    } catch (...) {
      failures[index] = std::current_exception();
    }
  };
  std::thread retrieve(settle, 0, 20117);
  std::thread claim(settle, 1, 20120);
  start.arrive_and_wait();
  retrieve.join();
  claim.join();
  ASSERT_FALSE(failures[0]);
  ASSERT_FALSE(failures[1]);
  EXPECT_EQ(std::ranges::count(outcomes, 0), 1);
  for (const auto outcome : outcomes) {
    if (outcome != 0) {
      EXPECT_TRUE(outcome == x5::ec::Market_AlreadyClaimed ||
                  outcome == x5::ec::Market_TransactionFailed);
    }
  }
  EXPECT_EQ(progress(storage, seller).stackables.at(kMaterial), 12);
  ASSERT_TRUE(market.market_listing(1, listing_id)->seller_claimed);
  EXPECT_EQ(code(call(storage, seller, 20117, listing_request(listing_id), 4, deadline)),
            x5::ec::Market_AlreadyClaimed);
  EXPECT_EQ(code(call(storage, seller, 20120, listing_request(listing_id), 5, deadline)),
            x5::ec::Market_AlreadyClaimed);

  const auto equipment_seller = owner(storage, "gear" + suffix);
  auto before_grant = progress(storage, equipment_seller);
  auto after_grant = before_grant;
  ++after_grant.revision;
  ItemTrade grant;
  CharacterItem equipment;
  equipment.cms_id = kEquipment;
  equipment.inventory_slot = 7;
  equipment.grade = 4;
  equipment.durability = 6700;
  equipment.created_epoch_ticks = 654321;
  grant.granted_items.push_back(equipment);
  const auto granted = storage.state.commit_item_trade(equipment_seller.account_id,
                                                       equipment_seller.id, 1, before_grant,
                                                       after_grant, grant);
  ASSERT_TRUE(granted);
  ASSERT_EQ(granted->size(), 1u);
  const auto original = granted->front();
  const auto registered = call(storage, equipment_seller, 20116,
                               {{"identity", {{"item_id", std::to_string(original.id)}}},
                                {"price", "300"}}, 1);
  ASSERT_EQ(code(registered), 0);
  const auto equipment_listing_id = std::stoll(registered.packets.back()
                                                   .message.at("listing_registered")
                                                   .at("listing_id")
                                                   .get<std::string>());
  const auto equipment_deadline = market.market_listing(1, equipment_listing_id)->deadline;

  // Seed all equipment slots in this specifically named loopback-only disposable database.
  pqxx::work fill(probe);
  fill.exec("INSERT INTO emulator_character_items(character_id,item_cms_id,inventory_slot) "
            "SELECT $1,$2,slot FROM generate_series(0,$3) AS slot "
            "WHERE NOT EXISTS (SELECT 1 FROM emulator_character_items existing "
            "WHERE existing.character_id=$1 AND existing.inventory_slot=slot)",
            pqxx::params{equipment_seller.id, kEquipment, kMaximumInventorySlot});
  fill.commit();
  const auto before_refusal = progress(storage, equipment_seller);
  const auto rejected = call(storage, equipment_seller, 20117,
                             listing_request(equipment_listing_id), 2, equipment_deadline);
  ASSERT_EQ(code(rejected), x5::ec::Market_NotEnoughInventory);
  EXPECT_EQ(progress(storage, equipment_seller), before_refusal);
  EXPECT_FALSE(market.market_listing(1, equipment_listing_id)->seller_claimed);
  const auto full_character = storage.characters.character_by_id(equipment_seller.id);
  ASSERT_TRUE(full_character);
  ASSERT_EQ(full_character->items.size(), static_cast<std::size_t>(kMaximumInventorySlot + 1));

  const auto filler = std::ranges::find_if(full_character->items, [](const CharacterItem &item) {
    return item.cms_id == kEquipment && item.created_epoch_ticks == 0 && !item.equip_slot;
  });
  ASSERT_NE(filler, full_character->items.end());
  ItemTrade remove_one;
  remove_one.removed_item_ids.push_back(filler->id);
  auto after_discard = before_refusal;
  ++after_discard.revision;
  ASSERT_TRUE(storage.state.commit_item_trade(equipment_seller.account_id, equipment_seller.id, 1,
                                              before_refusal, after_discard, remove_one));
  const auto retrieved = call(storage, equipment_seller, 20117,
                              listing_request(equipment_listing_id), 3, equipment_deadline);
  ASSERT_EQ(code(retrieved), 0);
  EXPECT_TRUE(market.market_listing(1, equipment_listing_id)->seller_claimed);
  const auto restored = storage.characters.character_by_id(equipment_seller.id);
  ASSERT_TRUE(restored);
  ASSERT_EQ(restored->items.size(), static_cast<std::size_t>(kMaximumInventorySlot + 1));
  const auto recovered = std::ranges::find_if(restored->items, [&](const CharacterItem &item) {
    return item.created_epoch_ticks == original.created_epoch_ticks &&
           item.durability == original.durability && item.grade == original.grade;
  });
  ASSERT_NE(recovered, restored->items.end());
  EXPECT_EQ(recovered->cms_id, original.cms_id);
  EXPECT_FALSE(recovered->equip_slot);
  EXPECT_NE(recovered->id, original.id);
  EXPECT_EQ(code(call(storage, equipment_seller, 20120, listing_request(equipment_listing_id),
                      4, equipment_deadline)), x5::ec::Market_AlreadyClaimed);
}

} // namespace
} // namespace chronicles
