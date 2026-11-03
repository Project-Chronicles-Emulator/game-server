#pragma once

#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chronicles {

struct MemoryDatabase;

// Marketplace ownership and escrow outlive connections. Closed rows retain claim receipts.
struct MarketListing {
  std::int64_t id{};
  int world_id{};
  std::int64_t seller_id{};
  std::int64_t buyer_id{};
  CharacterItem item;
  bool stackable{};
  int quantity{1};
  int main_type{};
  int sub_type{};
  std::int64_t price{};
  std::int64_t deadline{};
  std::int64_t updated{};
  std::int64_t completed{};
  // EX5MarketListingState: on sale, sold, revoked, expired.
  int state{};
  bool seller_claimed{};
  bool buyer_claimed{};
  // Random connection scope plus packet sequence; contains no authentication material.
  std::string registration_key;

  bool operator==(const MarketListing &) const = default;
};

struct MarketQuery {
  int type{};
  std::uint32_t start{};
  std::uint32_t count{50};
  std::optional<int> main_type;
  std::optional<int> sub_type;
  int sort{};
  bool history{};
};

struct MarketPage {
  std::uint32_t total{};
  std::vector<MarketListing> listings;
};

struct MarketCommit {
  bool committed{};
  bool replayed{};
  MarketListing listing;
  std::vector<CharacterItem> granted_items;
};

inline constexpr std::size_t kMaximumMarketSellerListings = 100;

// Shared validation and selection for memory/PostgreSQL marketplace transactions.
void validate_market_transition(std::int64_t character_id, const WorldProgress &before,
                                const WorldProgress &after, const ItemTrade &trade,
                                const MarketListing &expected, const MarketListing &next);

bool market_query_matches(const MarketListing &listing, std::int64_t character_id,
                          const MarketQuery &query, std::int64_t now);

MarketPage market_page(std::vector<MarketListing> listings, const MarketQuery &query);

// Marketplace listings, queries and the composite escrow/receipt commit.
// Implementations: MemoryMarketRepository and PostgresMarketRepository.
class MarketRepository {
public:
  virtual ~MarketRepository() = default;

  virtual std::optional<MarketListing> market_listing(int world_id, std::int64_t listing_id) = 0;

  virtual std::optional<MarketListing> market_registration(int world_id, std::int64_t seller_id,
                                                           const std::string &key) = 0;

  virtual MarketPage market_query(int world_id, std::int64_t character_id, const MarketQuery &query,
                                  std::int64_t now) = 0;

  // Compare the listing and gameplay revision, then commit escrow/receipts/items/money
  // together. An id-zero expected listing registers a new listing. No partial effects.
  virtual MarketCommit market_commit(const Character &actor, const WorldProgress &before,
                                     const WorldProgress &after, const ItemTrade &trade,
                                     const MarketListing &expected, const MarketListing &next) = 0;
};

// Memory implementation of MarketRepository. The repository holds the shared MemoryDatabase
// by shared_ptr and owns the listing table and id sequence; every access locks
// database_->mutex like the former in-memory member. The constructor registers a
// CharacterDeletionHook whose blocks_deletion vetoes removing a character that still has
// unclaimed escrow, and the destructor neutralizes it so the callback never outlives the
// table.
class MemoryMarketRepository final : public MarketRepository {
public:
  explicit MemoryMarketRepository(std::shared_ptr<MemoryDatabase> database);
  ~MemoryMarketRepository() override;

  // The registered hook callback points at this repository's table, so copying is forbidden.
  MemoryMarketRepository(const MemoryMarketRepository &) = delete;
  MemoryMarketRepository &operator=(const MemoryMarketRepository &) = delete;

  std::optional<MarketListing> market_listing(int world_id, std::int64_t listing_id) override;

  std::optional<MarketListing> market_registration(int world_id, std::int64_t seller_id,
                                                   const std::string &key) override;

  MarketPage market_query(int world_id, std::int64_t character_id, const MarketQuery &query,
                          std::int64_t now) override;

  MarketCommit market_commit(const Character &actor, const WorldProgress &before,
                             const WorldProgress &after, const ItemTrade &trade,
                             const MarketListing &expected, const MarketListing &next) override;

private:
  std::shared_ptr<MemoryDatabase> database_;
  // Position of this repository's entry in database_->deletion_hooks. The destructor
  // neutralizes it so the callback never outlives the listings table.
  std::size_t deletion_hook_index_{};
  std::int64_t next_market_id_{1};
  std::map<std::int64_t, MarketListing> market_listings_;
};

// PostgreSQL implementation of MarketRepository. The repository keeps its own copy of the
// connection string and opens one pqxx::connection per call; the character row lock
// serializes each escrow/receipt commit.
class PostgresMarketRepository final : public MarketRepository {
public:
  explicit PostgresMarketRepository(std::string connection) : connection_(std::move(connection)) {}

  std::optional<MarketListing> market_listing(int world_id, std::int64_t listing_id) override;

  std::optional<MarketListing> market_registration(int world_id, std::int64_t seller_id,
                                                   const std::string &key) override;

  MarketPage market_query(int world_id, std::int64_t character_id, const MarketQuery &query,
                          std::int64_t now) override;

  MarketCommit market_commit(const Character &actor, const WorldProgress &before,
                             const WorldProgress &after, const ItemTrade &trade,
                             const MarketListing &expected, const MarketListing &next) override;

private:
  std::string connection_;
};

} // namespace chronicles
