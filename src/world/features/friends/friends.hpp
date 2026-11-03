#pragma once

// Friend lists, recommended friends and friend presence for one realm.
//
// FriendService is transport-free. The front WebSocket adapter (native_friend) translates the
// WSC2F friend requests into calls and the published FriendEvents into WSF2C packets. The
// service keeps the realm's friendships in memory and writes every change through a
// FriendStore first, so a failed write leaves both copies unchanged.
//
// Evidence (docs/friends.md and dumps/ida_research/friends-20261027):
// - Friendships are account-scoped: every request and FriendInfo carries account_id.
// - FriendInfo.status is a oneof: `char` while the friend is online, otherwise
//   `last_login_epochtime` (AAC_Friend_ConvertFriendInfoToRecord). The client shows "Online" or
//   the time since that value (AAC_Friend_UI_FormatPresenceText).
// - AAC_Friend_ReportRequestError maps InvalidChar (6) to "Unable to find target", InvalidState
//   (7) to "Friends list full" and DuplicateRequest (35) to "Friend request already sent".
// - GameVariable MaxFriendNum (60) bounds the friends list; the client checks it before sending.
// - SystemMessage 335 tells a player that "another player added you as a friend" and points to
//   the Recommended Friends list.
//
// Candidate interpretations, used because no official friend traffic was captured:
// - Adding is one-directional and needs no acceptance: A adding B lists B for A, and A appears
//   in B's recommended list until B adds A back or dismisses A.
// - Offline FriendInfo has no character name, so a new friendship's alias defaults to the
//   friend's character name; an empty alias restores it.
// - "Online" means the account has a character in the world service. Steam friend codes are not
//   modeled; such searches find nobody.

#include "ec.generated.pb.h"
#include "persistence/character_repository.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace chronicles {

class ClientCatalog;
class GuildService;

// One row of a friends list: `friend_account_id` is listed for `owner_account_id`.
struct FriendLink {
  int world_id{};
  std::int64_t owner_account_id{};
  std::int64_t friend_account_id{};
  // The owner's nickname for the friend.
  std::string alias;
  // Wall-clock values are 100 ns Unix-epoch ticks, the client's *_epochtime unit.
  std::int64_t created_epoch_ticks{};
  // The friend removed the owner from the friend's recommended list. The owner's own list is
  // unaffected.
  bool recommend_dismissed{};

  bool operator==(const FriendLink &) const = default;
};

// Last known world presence of an account on one realm.
struct FriendPresenceRecord {
  int world_id{};
  std::int64_t account_id{};
  // The character the account last entered the world with.
  std::int64_t character_id{};
  // When that character last left the world; zero until a departure is recorded.
  std::int64_t last_login_epoch_ticks{};

  bool operator==(const FriendPresenceRecord &) const = default;
};

struct FriendSnapshot {
  std::vector<FriendLink> links;
  std::vector<FriendPresenceRecord> presences;
};

// Persistence for one realm's friendships. Every method is one transaction and throws on a
// storage failure. Implementations: MemoryFriendStore and PostgresFriendStore.
class FriendStore {
public:
  virtual ~FriendStore() = default;

  virtual FriendSnapshot load(int world_id) = 0;

  // Throws when the row already exists.
  virtual void insert_link(const FriendLink &link) = 0;

  // Replaces the alias and recommend_dismissed of an existing row; throws when it is missing.
  virtual void update_link(const FriendLink &link) = 0;

  // Removing a missing row is not an error.
  virtual void remove_link(int world_id, std::int64_t owner_account_id,
                           std::int64_t friend_account_id) = 0;

  virtual void save_presence(const FriendPresenceRecord &presence) = 0;
};

class MemoryFriendStore final : public FriendStore {
public:
  FriendSnapshot load(int world_id) override;

  void insert_link(const FriendLink &link) override;

  void update_link(const FriendLink &link) override;

  void remove_link(int world_id, std::int64_t owner_account_id,
                   std::int64_t friend_account_id) override;

  void save_presence(const FriendPresenceRecord &presence) override;

private:
  using LinkKey = std::tuple<int, std::int64_t, std::int64_t>;
  using PresenceKey = std::pair<int, std::int64_t>;

  std::mutex mutex_;
  std::map<LinkKey, FriendLink> links_;
  std::map<PresenceKey, FriendPresenceRecord> presences_;
};

// Uses the emulator_friends and emulator_friend_presence tables from
// migrations/001_accounts_characters.sql (schema version 31).
class PostgresFriendStore final : public FriendStore {
public:
  explicit PostgresFriendStore(std::string connection);

  FriendSnapshot load(int world_id) override;

  void insert_link(const FriendLink &link) override;

  void update_link(const FriendLink &link) override;

  void remove_link(int world_id, std::int64_t owner_account_id,
                   std::int64_t friend_account_id) override;

  void save_presence(const FriendPresenceRecord &presence) override;

private:
  std::string connection_;
};

// Character lookups the friend service needs. Thread-safe implementations only.
class FriendDirectory {
public:
  virtual ~FriendDirectory() = default;

  virtual std::optional<CharacterSummary> character(std::int64_t character_id) = 0;

  virtual std::optional<CharacterSummary> character_by_name(int world_id,
                                                            const std::string &name) = 0;

  // Ordered by lobby slot.
  virtual std::vector<CharacterSummary> account_characters(std::int64_t account_id,
                                                           int world_id) = 0;

  // Empty when the character has no guild.
  virtual std::string guild_name(std::int64_t character_id) = 0;
};

// Reads characters from the account's character repository and guild names from the guild
// registry (optional).
class CharacterFriendDirectory final : public FriendDirectory {
public:
  CharacterFriendDirectory(std::shared_ptr<CharacterRepository> characters,
                           std::shared_ptr<GuildService> guilds);

  std::optional<CharacterSummary> character(std::int64_t character_id) override;

  std::optional<CharacterSummary> character_by_name(int world_id, const std::string &name) override;

  std::vector<CharacterSummary> account_characters(std::int64_t account_id, int world_id) override;

  std::string guild_name(std::int64_t character_id) override;

private:
  std::shared_ptr<CharacterRepository> characters_;
  std::shared_ptr<GuildService> guilds_;
};

struct FriendRules {
  // GameVariable MaxFriendNum.
  std::size_t max_friends{60};
  // Candidate: the alias text policy was not recovered; the lobby character-name length is
  // used as the nickname bound, counted in Unicode code points.
  std::size_t max_alias_code_points{16};
};

// Reads GameVariable MaxFriendNum. A missing or implausible row keeps the default and is logged.
FriendRules load_friend_rules(ClientCatalog &catalog);

// Authoritative identity of the requester, built by the caller from its authenticated session.
struct FriendActor {
  int world_id{};
  std::int64_t account_id{};
  std::int64_t char_id{};
};

struct FriendCharacterView {
  int world_id{};
  std::int64_t char_id{};
  std::string name;
  int char_class{};
  int level{};
  // Empty when the character has no guild; the optional wire field is then omitted.
  std::string guild_name;

  bool operator==(const FriendCharacterView &) const = default;
};

// One FriendInfo as one owner sees it.
struct FriendView {
  std::int64_t account_id{};
  std::string alias;
  std::int64_t created_epoch_ticks{};
  // Lists and notifications set this only while the friend is online. A search result always
  // carries the matched character and reports offline state separately.
  std::optional<FriendCharacterView> character;
  // Used when `character` is absent.
  std::int64_t last_login_epoch_ticks{};
};

enum class FriendSearchKind {
  character_id,
  account_id,
  character_name,
  steam_friend_code,
};

struct FriendSearchQuery {
  FriendSearchKind kind{FriendSearchKind::character_name};
  std::int64_t id{};
  std::string text;
};

struct FriendSearchResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  FriendView found;
  // FriendSearchRes.last_login_epochtime: set only when the matched account is offline.
  std::optional<std::int64_t> offline_since_epoch_ticks;
};

struct FriendAddResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  FriendView added;
};

struct FriendAliasResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  FriendView updated;
};

struct FriendRemoveResult {
  // SystemError when the store write failed; removing nothing is not an error.
  x5::ec::EErrorCode error{x5::ec::Success};
  bool changed{};
};

// Published events address the recipient account's in-world character.

// A listed friend's FriendInfo changed for this recipient (presence). Sent as FriendUpdateNoti.
struct FriendUpdated {
  int world_id{};
  std::int64_t recipient_char_id{};
  FriendView view;
};

// Another account added the recipient. Sent as SystemMessageNoti SM_FRIEND_ADD_ALARM.
struct FriendAddedAlarm {
  int world_id{};
  std::int64_t recipient_char_id{};
};

using FriendEvent = std::variant<FriendUpdated, FriendAddedAlarm>;

using FriendEventSubscriber = std::function<void(const std::vector<FriendEvent> &)>;

// Thread-safe. Operations serialize on one lock, including their store write and directory
// lookups. A failed store write leaves the in-memory state unchanged and reports SystemError.
// Events are published after the lock is released; subscribers must not block.
class FriendService {
public:
  FriendService(std::shared_ptr<FriendStore> store, std::shared_ptr<FriendDirectory> directory,
                FriendRules rules, int world_id, std::function<std::int64_t()> epoch_ticks = {});

  // Register during startup, before requests are served.
  void subscribe(FriendEventSubscriber subscriber);

  int world_id() const;

  const FriendRules &rules() const;

  // The actor's friends list, oldest friendship first.
  std::vector<FriendView> friends(const FriendActor &actor);

  // Accounts that listed the actor, which the actor neither listed nor dismissed.
  std::vector<FriendView> recommended(const FriendActor &actor);

  FriendSearchResult search(const FriendActor &actor, const FriendSearchQuery &query);

  FriendAddResult add(const FriendActor &actor, std::int64_t target_account_id);

  // Removes the target from the actor's friends list, or dismisses it from the actor's
  // recommended list. `changed` is false when there was nothing to remove.
  FriendRemoveResult remove(const FriendActor &actor, std::int64_t target_account_id,
                            bool recommended);

  // An empty alias restores the friend's character name.
  FriendAliasResult set_alias(const FriendActor &actor, std::int64_t target_account_id,
                              const std::string &alias);

  // Presence hooks from the world service. Calls are paired per world connection; a second
  // connection of the same character keeps it online until both have left.
  void character_entered_world(int world_id, std::int64_t character_id);

  void character_left_world(int world_id, std::int64_t character_id);

  bool online(std::int64_t account_id) const;

private:
  struct OnlineSession {
    std::int64_t character_id{};
    unsigned connections{};
  };

  using OwnerLinks = std::map<std::int64_t, FriendLink>;

  // Every helper below requires mutex_.
  void load_snapshot(FriendSnapshot snapshot);
  const FriendLink *find_link(std::int64_t owner_account_id, std::int64_t friend_account_id) const;
  FriendLink *find_link(std::int64_t owner_account_id, std::int64_t friend_account_id);
  std::size_t friend_count(std::int64_t owner_account_id) const;
  std::optional<CharacterSummary> representative_character(std::int64_t account_id);
  FriendCharacterView character_view_of(const CharacterSummary &character);
  std::optional<FriendCharacterView> character_view(std::int64_t character_id);
  FriendView view_of(std::int64_t account_id, const std::string &alias,
                     std::int64_t created_epoch_ticks);
  std::optional<std::int64_t> online_character(std::int64_t account_id) const;
  std::vector<FriendEvent> presence_events(std::int64_t account_id);
  void save_presence(const FriendPresenceRecord &presence);
  std::optional<CharacterSummary> find_search_target(const FriendSearchQuery &query);

  void publish(const std::vector<FriendEvent> &events) const;

  mutable std::mutex mutex_;
  std::shared_ptr<FriendStore> store_;
  std::shared_ptr<FriendDirectory> directory_;
  FriendRules rules_;
  int world_id_{};
  std::function<std::int64_t()> epoch_ticks_;
  // Owner account -> friend account -> link.
  std::map<std::int64_t, OwnerLinks> links_by_owner_;
  // Friend account -> owners that list it.
  std::map<std::int64_t, std::set<std::int64_t>> owners_by_friend_;
  std::unordered_map<std::int64_t, FriendPresenceRecord> presences_;
  std::unordered_map<std::int64_t, OnlineSession> online_;
  std::vector<FriendEventSubscriber> subscribers_;
};

// Accepts valid UTF-8 without control characters and at most `max_code_points` code points.
bool valid_friend_alias(const std::string &alias, std::size_t max_code_points);

} // namespace chronicles
