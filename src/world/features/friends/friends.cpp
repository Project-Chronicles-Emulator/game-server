#include "world/features/friends/friends.hpp"

#include "data/client_catalog.hpp"
#include "common/clock_ticks.hpp"
#include "world/features/guild/guild.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace chronicles {
namespace {

constexpr std::string_view kFriendConfiguration = "Friend configuration";

// Plausibility bound for GameVariable MaxFriendNum; the shipped value is 60.
constexpr std::int64_t kMaximumFriendsBound = 1000;

// Stored character names are 3-16 ASCII characters; longer search text cannot match one.
constexpr std::size_t kMaximumSearchNameBytes = 64;

constexpr unsigned char kAsciiDelete = 0x7f;
constexpr unsigned char kUtf8C1LeadByte = 0xc2;
constexpr unsigned char kUtf8C1FirstContinuation = 0x80;
constexpr unsigned char kUtf8C1LastContinuation = 0x9f;

bool contains_control_character(std::string_view text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x20 || byte == kAsciiDelete) {
      return true;
    }

    // U+0080..U+009F (C1 controls) are encoded as C2 80..C2 9F.
    if (byte == kUtf8C1LeadByte && index + 1 < text.size()) {
      const auto next = static_cast<unsigned char>(text[index + 1]);
      if (next >= kUtf8C1FirstContinuation && next <= kUtf8C1LastContinuation) {
        return true;
      }
    }
  }
  return false;
}

bool created_before(const FriendView &first, const FriendView &second) {
  if (first.created_epoch_ticks != second.created_epoch_ticks) {
    return first.created_epoch_ticks < second.created_epoch_ticks;
  }
  return first.account_id < second.account_id;
}

// Runs one store write. A failure is logged and reported as false, so the caller keeps its
// in-memory state unchanged and answers SystemError instead of closing the client's socket.
template <typename Write>
bool store_write_succeeded(std::string_view operation, int world_id, Write &&write) {
  try {
    write();
    return true;
  } catch (const std::exception &error) {
    spdlog::error("Friend {} failed for realm {}: {}", operation, world_id, error.what());
    return false;
  }
}

} // namespace

bool valid_friend_alias(const std::string &alias, std::size_t max_code_points) {
  const auto code_points = utf8_code_points(alias);
  if (!code_points || *code_points > max_code_points) {
    return false;
  }
  return !contains_control_character(alias);
}

FriendRules load_friend_rules(ClientCatalog &catalog) {
  FriendRules rules;
  if (!catalog.has_table("GameVariable")) {
    spdlog::warn("{}: GameVariable table is missing; using defaults", kFriendConfiguration);
    return rules;
  }

  const auto max_friends =
      game_variable_integer(catalog, "MaxFriendNum", 1, kMaximumFriendsBound, kFriendConfiguration);
  if (max_friends) {
    rules.max_friends = static_cast<std::size_t>(*max_friends);
  }
  return rules;
}

CharacterFriendDirectory::CharacterFriendDirectory(std::shared_ptr<CharacterRepository> characters,
                                                   std::shared_ptr<GuildService> guilds)
    : characters_(std::move(characters)), guilds_(std::move(guilds)) {
  if (!characters_) {
    throw std::invalid_argument("Friend directory requires a character repository");
  }
}

std::optional<CharacterSummary> CharacterFriendDirectory::character(std::int64_t character_id) {
  return characters_->character_summary(character_id);
}

std::optional<CharacterSummary>
CharacterFriendDirectory::character_by_name(int world_id, const std::string &name) {
  return characters_->character_summary_by_name(world_id, name);
}

std::vector<CharacterSummary>
CharacterFriendDirectory::account_characters(std::int64_t account_id, int world_id) {
  return characters_->account_character_summaries(account_id, world_id);
}

std::string CharacterFriendDirectory::guild_name(std::int64_t character_id) {
  if (!guilds_) {
    return {};
  }
  return guilds_->status(character_id).guild_name;
}

FriendService::FriendService(std::shared_ptr<FriendStore> store,
                             std::shared_ptr<FriendDirectory> directory, FriendRules rules,
                             int world_id, std::function<std::int64_t()> epoch_ticks)
    : store_(std::move(store)), directory_(std::move(directory)), rules_(rules),
      world_id_(world_id), epoch_ticks_(std::move(epoch_ticks)) {
  if (!store_ || !directory_) {
    throw std::invalid_argument("Friend service requires a store and a character directory");
  }
  if (world_id_ <= 0) {
    throw std::invalid_argument("Friend service requires a positive realm ID");
  }
  if (rules_.max_friends == 0) {
    throw std::invalid_argument("Friend service requires a positive friend limit");
  }
  if (!epoch_ticks_) {
    epoch_ticks_ = unix_epoch_ticks_now;
  }

  load_snapshot(store_->load(world_id_));
}

void FriendService::subscribe(FriendEventSubscriber subscriber) {
  if (!subscriber) {
    throw std::invalid_argument("Friend event subscriber cannot be empty");
  }

  std::lock_guard lock(mutex_);
  subscribers_.push_back(std::move(subscriber));
}

int FriendService::world_id() const {
  return world_id_;
}

const FriendRules &FriendService::rules() const {
  return rules_;
}

std::vector<FriendView> FriendService::friends(const FriendActor &actor) {
  std::lock_guard lock(mutex_);
  std::vector<FriendView> views;
  if (actor.world_id != world_id_) {
    return views;
  }

  const auto owned = links_by_owner_.find(actor.account_id);
  if (owned == links_by_owner_.end()) {
    return views;
  }

  for (const auto &[friend_account_id, link] : owned->second) {
    views.push_back(view_of(friend_account_id, link.alias, link.created_epoch_ticks));
  }
  std::ranges::sort(views, created_before);
  return views;
}

std::vector<FriendView> FriendService::recommended(const FriendActor &actor) {
  std::lock_guard lock(mutex_);
  std::vector<FriendView> views;
  if (actor.world_id != world_id_) {
    return views;
  }

  const auto owners = owners_by_friend_.find(actor.account_id);
  if (owners == owners_by_friend_.end()) {
    return views;
  }

  for (const auto owner_account_id : owners->second) {
    const auto *link = find_link(owner_account_id, actor.account_id);
    if (!link || link->recommend_dismissed || find_link(actor.account_id, owner_account_id)) {
      continue;
    }

    // A recommendation has no nickname yet. The character name identifies it while offline.
    const auto owner_character = representative_character(owner_account_id);
    const std::string label = owner_character ? owner_character->name : std::string();
    views.push_back(view_of(owner_account_id, label, link->created_epoch_ticks));
  }
  std::ranges::sort(views, created_before);
  return views;
}

FriendSearchResult FriendService::search(const FriendActor &actor, const FriendSearchQuery &query) {
  std::lock_guard lock(mutex_);
  FriendSearchResult result;
  if (actor.world_id != world_id_) {
    result.error = x5::ec::InvalidChar;
    return result;
  }

  const auto target = find_search_target(query);
  if (!target || target->world_id != world_id_) {
    result.error = x5::ec::InvalidChar;
    return result;
  }

  result.found.account_id = target->account_id;
  const auto *existing = find_link(actor.account_id, target->account_id);
  if (existing) {
    result.found.alias = existing->alias;
    result.found.created_epoch_ticks = existing->created_epoch_ticks;
  }

  // The client displays the matched character even when the account is offline.
  result.found.character = character_view_of(*target);
  if (!online_character(target->account_id)) {
    const auto presence = presences_.find(target->account_id);
    const auto last_login =
        presence == presences_.end() ? std::int64_t{0} : presence->second.last_login_epoch_ticks;
    result.found.last_login_epoch_ticks = last_login;
    result.offline_since_epoch_ticks = last_login;
  }
  return result;
}

FriendAddResult FriendService::add(const FriendActor &actor, std::int64_t target_account_id) {
  FriendAddResult result;
  std::vector<FriendEvent> events;
  {
    std::lock_guard lock(mutex_);
    if (actor.world_id != world_id_ || actor.account_id <= 0 || target_account_id <= 0) {
      result.error = x5::ec::InvalidChar;
      return result;
    }
    if (target_account_id == actor.account_id) {
      result.error = x5::ec::BadRequest;
      return result;
    }
    if (find_link(actor.account_id, target_account_id)) {
      result.error = x5::ec::DuplicateRequest;
      return result;
    }
    if (friend_count(actor.account_id) >= rules_.max_friends) {
      result.error = x5::ec::InvalidState;
      return result;
    }

    const auto target_character = representative_character(target_account_id);
    if (!target_character) {
      result.error = x5::ec::InvalidChar;
      return result;
    }

    FriendLink link;
    link.world_id = world_id_;
    link.owner_account_id = actor.account_id;
    link.friend_account_id = target_account_id;
    link.alias = target_character->name;
    link.created_epoch_ticks = epoch_ticks_();
    const auto stored =
        store_write_succeeded("add", world_id_, [this, &link] { store_->insert_link(link); });
    if (!stored) {
      result.error = x5::ec::SystemError;
      return result;
    }

    links_by_owner_[actor.account_id][target_account_id] = link;
    owners_by_friend_[target_account_id].insert(actor.account_id);
    result.added = view_of(target_account_id, link.alias, link.created_epoch_ticks);

    // Only a new recommendation is announced: a friend who already listed the actor has no
    // recommended entry to look at.
    const auto target_in_world = online_character(target_account_id);
    if (target_in_world && !find_link(target_account_id, actor.account_id)) {
      events.push_back(FriendAddedAlarm{world_id_, *target_in_world});
    }
  }

  publish(events);
  return result;
}

FriendRemoveResult FriendService::remove(const FriendActor &actor, std::int64_t target_account_id,
                                         bool recommended) {
  std::lock_guard lock(mutex_);
  FriendRemoveResult result;
  if (actor.world_id != world_id_) {
    return result;
  }

  if (!recommended) {
    if (!find_link(actor.account_id, target_account_id)) {
      return result;
    }

    const auto removed = store_write_succeeded("remove", world_id_, [&] {
      store_->remove_link(world_id_, actor.account_id, target_account_id);
    });
    if (!removed) {
      result.error = x5::ec::SystemError;
      return result;
    }

    auto owned = links_by_owner_.find(actor.account_id);
    owned->second.erase(target_account_id);
    if (owned->second.empty()) {
      links_by_owner_.erase(owned);
    }

    auto owners = owners_by_friend_.find(target_account_id);
    owners->second.erase(actor.account_id);
    if (owners->second.empty()) {
      owners_by_friend_.erase(owners);
    }
    result.changed = true;
    return result;
  }

  // Dismissing a recommendation keeps the other account's friends list intact.
  auto *recommendation = find_link(target_account_id, actor.account_id);
  if (!recommendation || recommendation->recommend_dismissed ||
      find_link(actor.account_id, target_account_id)) {
    return result;
  }

  auto dismissed = *recommendation;
  dismissed.recommend_dismissed = true;
  const auto stored = store_write_succeeded("recommendation dismissal", world_id_,
                                            [this, &dismissed] { store_->update_link(dismissed); });
  if (!stored) {
    result.error = x5::ec::SystemError;
    return result;
  }

  *recommendation = dismissed;
  result.changed = true;
  return result;
}

FriendAliasResult FriendService::set_alias(const FriendActor &actor, std::int64_t target_account_id,
                                           const std::string &alias) {
  std::lock_guard lock(mutex_);
  FriendAliasResult result;
  auto *link =
      actor.world_id == world_id_ ? find_link(actor.account_id, target_account_id) : nullptr;
  if (!link || !valid_friend_alias(alias, rules_.max_alias_code_points)) {
    result.error = x5::ec::BadRequest;
    return result;
  }

  auto updated = *link;
  if (alias.empty()) {
    const auto friend_character = representative_character(target_account_id);
    if (friend_character) {
      updated.alias = friend_character->name;
    }
  } else {
    updated.alias = alias;
  }

  const auto stored = store_write_succeeded("alias change", world_id_,
                                            [this, &updated] { store_->update_link(updated); });
  if (!stored) {
    result.error = x5::ec::SystemError;
    return result;
  }

  *link = updated;
  result.updated = view_of(target_account_id, updated.alias, updated.created_epoch_ticks);
  return result;
}

void FriendService::character_entered_world(int world_id, std::int64_t character_id) {
  std::vector<FriendEvent> events;
  {
    std::lock_guard lock(mutex_);
    if (world_id != world_id_) {
      return;
    }

    const auto character = directory_->character(character_id);
    if (!character || character->world_id != world_id_) {
      return;
    }

    auto &session = online_[character->account_id];
    if (session.connections > 0 && session.character_id == character_id) {
      ++session.connections;
    } else {
      session.character_id = character_id;
      session.connections = 1;
    }

    auto presence = presences_[character->account_id];
    presence.world_id = world_id_;
    presence.account_id = character->account_id;
    presence.character_id = character_id;
    save_presence(presence);
    events = presence_events(character->account_id);
  }

  publish(events);
}

void FriendService::character_left_world(int world_id, std::int64_t character_id) {
  std::vector<FriendEvent> events;
  {
    std::lock_guard lock(mutex_);
    if (world_id != world_id_) {
      return;
    }

    // A connection that was replaced by another character of the account no longer owns the
    // account's presence.
    const auto session = std::ranges::find_if(online_, [character_id](const auto &entry) {
      return entry.second.character_id == character_id;
    });
    if (session == online_.end()) {
      return;
    }

    --session->second.connections;
    if (session->second.connections > 0) {
      return;
    }

    const auto account_id = session->first;
    online_.erase(session);

    auto presence = presences_[account_id];
    presence.world_id = world_id_;
    presence.account_id = account_id;
    presence.character_id = character_id;
    presence.last_login_epoch_ticks = epoch_ticks_();
    save_presence(presence);
    events = presence_events(account_id);
  }

  publish(events);
}

bool FriendService::online(std::int64_t account_id) const {
  std::lock_guard lock(mutex_);
  return online_character(account_id).has_value();
}

void FriendService::load_snapshot(FriendSnapshot snapshot) {
  for (auto &link : snapshot.links) {
    if (link.world_id != world_id_ || link.owner_account_id == link.friend_account_id) {
      spdlog::warn("Ignoring an invalid stored friendship for realm {}", world_id_);
      continue;
    }

    const auto owner_account_id = link.owner_account_id;
    const auto friend_account_id = link.friend_account_id;
    links_by_owner_[owner_account_id][friend_account_id] = std::move(link);
    owners_by_friend_[friend_account_id].insert(owner_account_id);
  }

  for (auto &presence : snapshot.presences) {
    if (presence.world_id == world_id_) {
      presences_[presence.account_id] = presence;
    }
  }
}

const FriendLink *FriendService::find_link(std::int64_t owner_account_id,
                                           std::int64_t friend_account_id) const {
  const auto owned = links_by_owner_.find(owner_account_id);
  if (owned == links_by_owner_.end()) {
    return nullptr;
  }

  const auto link = owned->second.find(friend_account_id);
  return link == owned->second.end() ? nullptr : &link->second;
}

FriendLink *FriendService::find_link(std::int64_t owner_account_id,
                                     std::int64_t friend_account_id) {
  const auto *link = std::as_const(*this).find_link(owner_account_id, friend_account_id);
  return const_cast<FriendLink *>(link);
}

std::size_t FriendService::friend_count(std::int64_t owner_account_id) const {
  const auto owned = links_by_owner_.find(owner_account_id);
  return owned == links_by_owner_.end() ? 0 : owned->second.size();
}

std::optional<CharacterSummary> FriendService::representative_character(std::int64_t account_id) {
  const auto belongs_here = [this, account_id](const std::optional<CharacterSummary> &character) {
    return character && character->account_id == account_id && character->world_id == world_id_;
  };

  const auto in_world = online_character(account_id);
  if (in_world) {
    auto character = directory_->character(*in_world);
    if (belongs_here(character)) {
      return character;
    }
  }

  const auto presence = presences_.find(account_id);
  if (presence != presences_.end() && presence->second.character_id > 0) {
    auto character = directory_->character(presence->second.character_id);
    if (belongs_here(character)) {
      return character;
    }
  }

  auto characters = directory_->account_characters(account_id, world_id_);
  if (characters.empty()) {
    return std::nullopt;
  }
  return std::move(characters.front());
}

FriendCharacterView FriendService::character_view_of(const CharacterSummary &character) {
  FriendCharacterView view;
  view.world_id = character.world_id;
  view.char_id = character.id;
  view.name = character.name;
  view.char_class = character.class_id;
  view.level = character.level;
  view.guild_name = directory_->guild_name(character.id);
  return view;
}

std::optional<FriendCharacterView> FriendService::character_view(std::int64_t character_id) {
  const auto character = directory_->character(character_id);
  if (!character || character->world_id != world_id_) {
    return std::nullopt;
  }
  return character_view_of(*character);
}

FriendView FriendService::view_of(std::int64_t account_id, const std::string &alias,
                                  std::int64_t created_epoch_ticks) {
  FriendView view;
  view.account_id = account_id;
  view.alias = alias;
  view.created_epoch_ticks = created_epoch_ticks;

  const auto in_world = online_character(account_id);
  if (in_world) {
    view.character = character_view(*in_world);
  }

  // A deleted in-world character reads as offline rather than as an empty online entry.
  if (!view.character) {
    const auto presence = presences_.find(account_id);
    if (presence != presences_.end()) {
      view.last_login_epoch_ticks = presence->second.last_login_epoch_ticks;
    }
  }
  return view;
}

std::optional<std::int64_t> FriendService::online_character(std::int64_t account_id) const {
  const auto session = online_.find(account_id);
  if (session == online_.end()) {
    return std::nullopt;
  }
  return session->second.character_id;
}

std::vector<FriendEvent> FriendService::presence_events(std::int64_t account_id) {
  std::vector<FriendEvent> events;
  const auto owners = owners_by_friend_.find(account_id);
  if (owners == owners_by_friend_.end()) {
    return events;
  }

  // The character lookup is shared; only the owner's nickname and friendship date differ.
  const auto shared_view = view_of(account_id, {}, 0);
  for (const auto owner_account_id : owners->second) {
    const auto recipient = online_character(owner_account_id);
    const auto *link = find_link(owner_account_id, account_id);
    if (!recipient || !link) {
      continue;
    }

    auto view = shared_view;
    view.alias = link->alias;
    view.created_epoch_ticks = link->created_epoch_ticks;
    events.push_back(FriendUpdated{world_id_, *recipient, std::move(view)});
  }
  return events;
}

void FriendService::save_presence(const FriendPresenceRecord &presence) {
  // Live presence must follow the world connection even when the database is unavailable; a
  // failed write only loses the persisted last-login value.
  presences_[presence.account_id] = presence;
  try {
    store_->save_presence(presence);
  } catch (const std::exception &error) {
    spdlog::warn("Friend presence save failed for realm {}: {}", world_id_, error.what());
  }
}

std::optional<CharacterSummary> FriendService::find_search_target(const FriendSearchQuery &query) {
  switch (query.kind) {
  case FriendSearchKind::character_id:
    if (query.id <= 0) {
      return std::nullopt;
    }
    return directory_->character(query.id);
  case FriendSearchKind::account_id:
    if (query.id <= 0) {
      return std::nullopt;
    }
    return representative_character(query.id);
  case FriendSearchKind::character_name:
    if (query.text.empty() || query.text.size() > kMaximumSearchNameBytes) {
      return std::nullopt;
    }
    return directory_->character_by_name(world_id_, query.text);
  case FriendSearchKind::steam_friend_code:
    // Steam friend codes are not linked to emulator accounts.
    return std::nullopt;
  }
  return std::nullopt;
}

void FriendService::publish(const std::vector<FriendEvent> &events) const {
  if (events.empty()) {
    return;
  }

  // Subscribers are registered during startup, before any operation can publish.
  for (const auto &subscriber : subscribers_) {
    subscriber(events);
  }
}

} // namespace chronicles
