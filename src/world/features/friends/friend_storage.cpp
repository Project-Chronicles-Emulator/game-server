#include "world/features/friends/friends.hpp"

#include <pqxx/pqxx>

#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

void validate_link(const FriendLink &link) {
  if (link.world_id <= 0 || link.owner_account_id <= 0 || link.friend_account_id <= 0 ||
      link.owner_account_id == link.friend_account_id || link.created_epoch_ticks <= 0) {
    throw std::invalid_argument("Friendship requires a realm, two distinct accounts and a time");
  }
}

void validate_presence(const FriendPresenceRecord &presence) {
  if (presence.world_id <= 0 || presence.account_id <= 0 || presence.character_id <= 0 ||
      presence.last_login_epoch_ticks < 0) {
    throw std::invalid_argument("Friend presence requires a realm, an account and a character");
  }
}

} // namespace

FriendSnapshot MemoryFriendStore::load(int world_id) {
  std::lock_guard lock(mutex_);
  FriendSnapshot snapshot;
  for (const auto &[key, link] : links_) {
    if (link.world_id == world_id) {
      snapshot.links.push_back(link);
    }
  }

  for (const auto &[key, presence] : presences_) {
    if (presence.world_id == world_id) {
      snapshot.presences.push_back(presence);
    }
  }
  return snapshot;
}

void MemoryFriendStore::insert_link(const FriendLink &link) {
  validate_link(link);

  std::lock_guard lock(mutex_);
  const LinkKey key{link.world_id, link.owner_account_id, link.friend_account_id};
  const auto [position, inserted] = links_.try_emplace(key, link);
  if (!inserted) {
    throw std::runtime_error("Friendship already exists");
  }
}

void MemoryFriendStore::update_link(const FriendLink &link) {
  validate_link(link);

  std::lock_guard lock(mutex_);
  const auto found = links_.find({link.world_id, link.owner_account_id, link.friend_account_id});
  if (found == links_.end()) {
    throw std::runtime_error("Friendship to update does not exist");
  }
  found->second.alias = link.alias;
  found->second.recommend_dismissed = link.recommend_dismissed;
}

void MemoryFriendStore::remove_link(int world_id, std::int64_t owner_account_id,
                                    std::int64_t friend_account_id) {
  std::lock_guard lock(mutex_);
  links_.erase({world_id, owner_account_id, friend_account_id});
}

void MemoryFriendStore::save_presence(const FriendPresenceRecord &presence) {
  validate_presence(presence);

  std::lock_guard lock(mutex_);
  presences_[{presence.world_id, presence.account_id}] = presence;
}

PostgresFriendStore::PostgresFriendStore(std::string connection)
    : connection_(std::move(connection)) {
  if (connection_.empty()) {
    throw std::invalid_argument("PostgreSQL friend store requires a connection string");
  }
}

FriendSnapshot PostgresFriendStore::load(int world_id) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  FriendSnapshot snapshot;

  const auto links = transaction.exec(
      "SELECT owner_account_id,friend_account_id,alias,created_epoch_ticks,recommend_dismissed "
      "FROM emulator_friends WHERE world_id=$1",
      pqxx::params{world_id});
  for (const auto &row : links) {
    FriendLink link;
    link.world_id = world_id;
    link.owner_account_id = row["owner_account_id"].as<std::int64_t>();
    link.friend_account_id = row["friend_account_id"].as<std::int64_t>();
    link.alias = row["alias"].as<std::string>();
    link.created_epoch_ticks = row["created_epoch_ticks"].as<std::int64_t>();
    link.recommend_dismissed = row["recommend_dismissed"].as<bool>();
    snapshot.links.push_back(std::move(link));
  }

  const auto presences = transaction.exec("SELECT account_id,character_id,last_login_epoch_ticks "
                                          "FROM emulator_friend_presence WHERE world_id=$1",
                                          pqxx::params{world_id});
  for (const auto &row : presences) {
    FriendPresenceRecord presence;
    presence.world_id = world_id;
    presence.account_id = row["account_id"].as<std::int64_t>();
    presence.character_id = row["character_id"].as<std::int64_t>();
    presence.last_login_epoch_ticks = row["last_login_epoch_ticks"].as<std::int64_t>();
    snapshot.presences.push_back(presence);
  }
  return snapshot;
}

void PostgresFriendStore::insert_link(const FriendLink &link) {
  validate_link(link);

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  transaction.exec("INSERT INTO emulator_friends(world_id,owner_account_id,friend_account_id,alias,"
                   "created_epoch_ticks,recommend_dismissed) VALUES ($1,$2,$3,$4,$5,$6)",
                   pqxx::params{link.world_id, link.owner_account_id, link.friend_account_id,
                                link.alias, link.created_epoch_ticks, link.recommend_dismissed});
  transaction.commit();
}

void PostgresFriendStore::update_link(const FriendLink &link) {
  validate_link(link);

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  const auto rows =
      transaction.exec("UPDATE emulator_friends SET alias=$4,recommend_dismissed=$5 "
                       "WHERE world_id=$1 AND owner_account_id=$2 AND friend_account_id=$3 "
                       "RETURNING owner_account_id",
                       pqxx::params{link.world_id, link.owner_account_id, link.friend_account_id,
                                    link.alias, link.recommend_dismissed});
  if (rows.empty()) {
    throw std::runtime_error("Friendship to update does not exist");
  }
  transaction.commit();
}

void PostgresFriendStore::remove_link(int world_id, std::int64_t owner_account_id,
                                      std::int64_t friend_account_id) {
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  transaction.exec("DELETE FROM emulator_friends "
                   "WHERE world_id=$1 AND owner_account_id=$2 AND friend_account_id=$3",
                   pqxx::params{world_id, owner_account_id, friend_account_id});
  transaction.commit();
}

void PostgresFriendStore::save_presence(const FriendPresenceRecord &presence) {
  validate_presence(presence);

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  transaction.exec(
      "INSERT INTO emulator_friend_presence(world_id,account_id,character_id,"
      "last_login_epoch_ticks) VALUES ($1,$2,$3,$4) "
      "ON CONFLICT (world_id,account_id) DO UPDATE SET character_id=EXCLUDED.character_id,"
      "last_login_epoch_ticks=EXCLUDED.last_login_epoch_ticks",
      pqxx::params{presence.world_id, presence.account_id, presence.character_id,
                   presence.last_login_epoch_ticks});
  transaction.commit();
}

} // namespace chronicles
