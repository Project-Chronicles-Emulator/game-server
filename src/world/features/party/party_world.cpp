#include "world/features/party/party_world.hpp"

#include "data/client_catalog.hpp"
#include "ec.generated.pb.h"
#include "world/features/npc/npc_population.hpp"

#include <spdlog/spdlog.h>

#include <charconv>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace chronicles::party {
namespace {

// x5.pb.CommonErrorRes.category for every party refusal.
constexpr char kPartyErrorCategory[] = "EErrorCategory_Party";

constexpr char kGameVariableTable[] = "GameVariable";

// Log prefix for catalog values that fall back to their defaults.
constexpr char kPartyConfiguration[] = "Party configuration";

// Plausibility bounds for catalog values. A value outside them indicates a
// corrupt or misread catalog row, so the evidenced default is kept instead.
constexpr std::int64_t kMinimumPartyMembers = 2;
constexpr std::int64_t kMaximumPartyMembers = 64;
constexpr std::int64_t kMinimumIntervalMillis = 1000;
constexpr std::int64_t kMaximumIntervalMillis = 600000;

PartyResult refusal(PartyError error) {
  PartyResult result;
  result.error = error;
  return result;
}

std::string id_string(std::int64_t id) {
  return std::to_string(id);
}

// sfixed64/uint64 fields decode as JSON strings; integers are accepted too.
std::optional<std::int64_t> parse_int64(const nlohmann::json &value) {
  if (value.is_number_integer()) {
    return value.get<std::int64_t>();
  }
  if (!value.is_string()) {
    return std::nullopt;
  }

  const auto &text = value.get_ref<const std::string &>();
  std::int64_t parsed = 0;
  const auto *end = text.data() + text.size();
  const auto [last, error] = std::from_chars(text.data(), end, parsed);
  if (error != std::errc{} || last != end) {
    return std::nullopt;
  }
  return parsed;
}

// Proto3 omits zero-valued fields, so an absent ID is zero. A present field
// that is not an integer is malformed.
std::optional<std::int64_t> read_id(const nlohmann::json &message, const char *field) {
  if (!message.is_object()) {
    return std::nullopt;
  }

  const auto found = message.find(field);
  if (found == message.end()) {
    return 0;
  }
  return parse_int64(*found);
}

const char *connection_state_name(ConnectionState state) {
  switch (state) {
  case ConnectionState::kConnected:
    return "EX5PartyMemberConnStateType_Connected";
  case ConnectionState::kRetain:
    return "EX5PartyMemberConnStateType_Retain";
  case ConnectionState::kNone:
    break;
  }
  return "EX5PartyMemberConnStateType_None";
}

// x5.pb.party.PartyMember. zone_key stays unset: no zone model exists.
nlohmann::json member_json(const MemberDescriptor &descriptor) {
  nlohmann::json member{{"char_id", id_string(descriptor.char_id)},
                        {"level", descriptor.level},
                        {"char_class", descriptor.char_class},
                        {"connection_state", connection_state_name(descriptor.connection_state)}};
  if (!descriptor.name.empty()) {
    member["name"] = descriptor.name;
  }
  if (descriptor.map_cms_id != 0) {
    member["map_cms_id"] = descriptor.map_cms_id;
  }
  return member;
}

nlohmann::json members_json(const std::vector<MemberDescriptor> &members) {
  auto list = nlohmann::json::array();
  for (const auto &member : members) {
    list.push_back(member_json(member));
  }
  return list;
}

nlohmann::json ids_json(const std::vector<CharacterId> &ids) {
  auto list = nlohmann::json::array();
  for (const auto id : ids) {
    list.push_back(id_string(id));
  }
  return list;
}

std::uint16_t event_opcode(EventKind kind) {
  switch (kind) {
  case EventKind::kJoinNoti:
    return kPartyJoinNotification;
  case EventKind::kLeaveNoti:
    return kPartyLeaveNotification;
  case EventKind::kMemberUpdateNoti:
    return kPartyMemberUpdateNotification;
  case EventKind::kInvitePopupNoti:
    return kPartyInvitePopupNotification;
  case EventKind::kRejectInviteNoti:
    return kPartyRejectInviteNotification;
  case EventKind::kRemoveTemporaryNoti:
    return kPartyRemoveTemporaryNotification;
  }
  throw std::logic_error("Unknown party event kind");
}

nlohmann::json event_message(const PartyEvent &event) {
  const auto party_id = id_string(event.party_id);
  switch (event.kind) {
  case EventKind::kJoinNoti:
    return {{"party_id", party_id},
            {"leader_char_id", id_string(event.leader_char_id)},
            {"members", members_json(event.members)}};
  case EventKind::kLeaveNoti:
    return {{"party_id", party_id}, {"kicked", event.kicked}};
  case EventKind::kMemberUpdateNoti:
    // "disband_panding" is the recovered schema spelling of field 6.
    return {{"party_id", party_id},
            {"members", members_json(event.members)},
            {"leave_char_ids", ids_json(event.leave_char_ids)},
            {"kicked_char_ids", ids_json(event.kicked_char_ids)},
            {"leader_char_id", id_string(event.leader_char_id)},
            {"disband_panding", event.disband_pending}};
  case EventKind::kInvitePopupNoti:
    return {{"party_id", party_id},
            {"leader", member_json(event.leader)},
            {"target", member_json(event.target)}};
  case EventKind::kRejectInviteNoti:
    return {{"party_id", party_id}, {"target", member_json(event.target)}};
  case EventKind::kRemoveTemporaryNoti:
    return {{"party_id", party_id}};
  }
  throw std::logic_error("Unknown party event kind");
}

bool same_channel(const MemberPresence &first, const MemberPresence &second) {
  if (!first.position || !second.position) {
    return false;
  }
  return first.channel_key == second.channel_key &&
         first.position->map_id == second.position->map_id;
}

// x5.pb.party.PartyMemberWorldStateNoti describing `subject` to one member.
nlohmann::json world_state_message(PartyId party_id, const MemberPresence &subject,
                                   bool shares_channel) {
  nlohmann::json member{{"char_id", id_string(subject.char_id)},
                        {"hp", subject.hp},
                        {"max_hp", subject.max_hp},
                        {"is_dead", subject.max_hp > 0 && subject.hp <= 0}};
  if (subject.position) {
    member["pos"] = {{"millimeter_x", subject.position->x_mm},
                     {"millimeter_y", subject.position->y_mm},
                     {"millimeter_z", subject.position->z_mm}};
  }

  return {{"party_id", id_string(party_id)},
          {"member", std::move(member)},
          {"same_channel", shares_channel}};
}

} // namespace

void PartyUpdate::append(PartyUpdate other) {
  packets.insert(packets.end(), std::make_move_iterator(other.packets.begin()),
                 std::make_move_iterator(other.packets.end()));
  unit_changes.insert(unit_changes.end(), std::make_move_iterator(other.unit_changes.begin()),
                      std::make_move_iterator(other.unit_changes.end()));
}

PartyWorldConfig load_party_world_config(ClientCatalog &catalog) {
  PartyWorldConfig config;
  if (!catalog.has_table(kGameVariableTable)) {
    spdlog::warn("Party configuration: catalog has no GameVariable table; using defaults");
    return config;
  }

  const auto max_members = game_variable_integer(catalog, "PartyMaxMemberNum", kMinimumPartyMembers,
                                                 kMaximumPartyMembers, kPartyConfiguration);
  if (max_members) {
    config.service.max_members = static_cast<std::size_t>(*max_members);
  }

  const auto invite_timeout =
      game_variable_integer(catalog, "PartyInviteTimeoutMillis", kMinimumIntervalMillis,
                            kMaximumIntervalMillis, kPartyConfiguration);
  if (invite_timeout) {
    config.service.invite_ttl = std::chrono::milliseconds(*invite_timeout);
  }

  const auto sync_interval =
      game_variable_integer(catalog, "PartyMemberWorldStateSyncMillis", kMinimumIntervalMillis,
                            kMaximumIntervalMillis, kPartyConfiguration);
  if (sync_interval) {
    config.world_state_interval = std::chrono::milliseconds(*sync_interval);
  }

  const auto unlock_quest = game_variable_integer(catalog, "PartyUnlockQuestId", 0,
                                                  std::numeric_limits<int>::max(),
                                                  kPartyConfiguration);
  if (unlock_quest) {
    config.unlock_quest_id = static_cast<int>(*unlock_quest);
  }

  return config;
}

bool party_unlocked(const WorldProgress &progress, int quest_id) {
  if (quest_id == 0) {
    return true;
  }

  const auto quest = progress.quests.find(quest_id);
  return quest != progress.quests.end() && quest->second.complete;
}

PartyWorld::PartyWorld(PartyWorldConfig config)
    : config_(std::move(config)),
      // The resolver runs only inside PartyService calls, which this class
      // makes while holding mutex_, so it reads known_ without locking.
      service_(config_.service,
               [this](CharacterId member) {
                 return describe(member);
               }) {}

bool PartyWorld::handles(std::uint16_t opcode) {
  return opcode >= kPartyKickRequest && opcode <= kPartyRejectInviteRequest;
}

int PartyWorld::unlock_quest_id() const {
  return config_.unlock_quest_id;
}

PartyUpdate PartyWorld::attach(const MemberPresence &presence, TimePoint now) {
  (void)now;
  std::lock_guard lock(mutex_);
  PartyUpdate update;

  auto &entry = known_[presence.char_id];
  entry.presence = presence;
  entry.online = true;
  // A fresh session has not seen any ChangePartyUnitNoti yet.
  announced_party_.erase(presence.char_id);

  if (service_.party_of(presence.char_id)) {
    translate(service_.restore(presence.char_id).events, update);
    world_state_sync_due_ = true;
  }

  collect_unit_changes(update);
  return update;
}

void PartyWorld::refresh(const MemberPresence &presence) {
  std::lock_guard lock(mutex_);
  const auto entry = known_.find(presence.char_id);
  if (entry == known_.end() || !entry->second.online ||
      entry->second.presence.connection_id != presence.connection_id) {
    return;
  }
  entry->second.presence = presence;
}

PartyUpdate PartyWorld::detach(CharacterId char_id, std::uint64_t connection_id, TimePoint now) {
  return detach_staged(char_id, connection_id, now, {});
}

PartyUpdate PartyWorld::detach_staged(
    CharacterId char_id, std::uint64_t connection_id, TimePoint now,
    const std::function<void(const PartyUpdate &)> &before_commit) {
  std::lock_guard lock(mutex_);
  PartyUpdate update;

  const auto entry = known_.find(char_id);
  if (entry == known_.end() || !entry->second.online ||
      entry->second.presence.connection_id != connection_id) {
    return update;
  }

  std::vector<std::pair<CharacterId, PartyId>> announcement_changes;
  std::vector<CharacterId> prepared_entries;

  const auto prepare_update = [&](const PartyResult &result) {
    entry->second.online = false;
    try {
      translate(result.events, update);
      for (const auto &[member_id, member] : known_) {
        if (!member.online) {
          continue;
        }
        const PartyId current = service_.established_party_of(member_id).value_or(0);
        const auto announced = announced_party_.find(member_id);
        const PartyId previous = announced == announced_party_.end() ? 0 : announced->second;
        if (current == previous) {
          continue;
        }
        update.unit_changes.push_back({member.presence.connection_id, member_id, current});
        announcement_changes.emplace_back(member_id, current);
      }

      prepared_entries.reserve(announcement_changes.size());
      if (announcement_changes.size() > announced_party_.max_size() - announced_party_.size()) {
        throw std::length_error("Party announcement staging exceeds map capacity");
      }
      if (!announcement_changes.empty()) {
        announced_party_.reserve(announced_party_.size() + announcement_changes.size());
      }
      for (const auto &[member_id, current] : announcement_changes) {
        if (current == 0) {
          continue;
        }
        const bool inserted = announced_party_.try_emplace(member_id, 0).second;
        if (inserted) {
          prepared_entries.push_back(member_id);
        }
      }
      if (before_commit) {
        before_commit(update);
      }
    } catch (...) {
      entry->second.online = true;
      for (const auto member_id : prepared_entries) {
        announced_party_.erase(member_id);
      }
      throw;
    }
    entry->second.online = true;
  };

  if (service_.party_of(char_id)) {
    service_.disconnect_staged(char_id, now, prepare_update);
  } else {
    prepare_update(PartyResult{});
  }

  entry->second.online = false;
  announced_party_.erase(char_id);
  for (const auto &[member_id, current] : announcement_changes) {
    if (current == 0) {
      announced_party_.erase(member_id);
    } else {
      announced_party_.find(member_id)->second = current;
    }
  }

  // Offline characters are remembered only while they hold a retained seat.
  if (!service_.party_of(char_id)) {
    known_.erase(char_id);
  }

  return update;
}

PartyUpdate PartyWorld::receive(CharacterId requester, std::uint64_t connection_id,
                                std::uint16_t opcode, const nlohmann::json &message,
                                std::uint32_t sequence, TimePoint now) {
  std::lock_guard lock(mutex_);
  PartyUpdate update;

  // Flush lapsed invites first. PartyService::invite expires lazily but drops
  // those notifications when it then refuses, which would leave stale popups.
  translate(service_.expire_invites(now), update);

  PartyResult result;
  const KnownMember *member = online_member(requester);
  if (member == nullptr || member->presence.connection_id != connection_id) {
    // The session has not finished entering the world, so it has no roster
    // identity yet.
    result = refusal(PartyError::kNotJoined);
  } else {
    result = dispatch(requester, opcode, message, now);
  }

  if (result.ok()) {
    translate(result.events, update);
    world_state_sync_due_ = true;
  } else {
    update.packets.push_back(
        {connection_id, kCommonErrorResponse, error_message(result.error, sequence)});
  }

  collect_unit_changes(update);
  return update;
}

PartyUpdate PartyWorld::tick(TimePoint now) {
  std::lock_guard lock(mutex_);
  PartyUpdate update;

  translate(service_.expire_invites(now), update);
  translate(service_.expire_retained(now), update);
  collect_unit_changes(update);
  prune_offline_members();

  if (world_state_sync_due_ || now >= next_world_state_sync_) {
    append_world_states(update);
    world_state_sync_due_ = false;
    next_world_state_sync_ = now + config_.world_state_interval;
  }

  return update;
}

bool PartyWorld::active() const {
  std::lock_guard lock(mutex_);
  return service_.party_count() != 0;
}

PartyWorld::Membership PartyWorld::membership(CharacterId member) const {
  std::lock_guard lock(mutex_);
  if (!service_.established_party_of(member)) {
    return Membership::none;
  }
  return service_.is_leader(member) ? Membership::leader : Membership::member;
}

std::optional<PartyId> PartyWorld::established_party(CharacterId member) const {
  std::lock_guard lock(mutex_);
  return service_.established_party_of(member);
}

std::vector<std::uint64_t> PartyWorld::party_connections(CharacterId member) const {
  std::lock_guard lock(mutex_);
  std::vector<std::uint64_t> connections;
  if (!service_.established_party_of(member)) {
    return connections;
  }

  for (const auto &descriptor : service_.members_of(member)) {
    if (descriptor.connection_state != ConnectionState::kConnected) {
      continue;
    }

    const KnownMember *known = online_member(descriptor.char_id);
    if (known != nullptr) {
      connections.push_back(known->presence.connection_id);
    }
  }
  return connections;
}

std::vector<PartyWorld::NearbyMember>
PartyWorld::nearby_party_members(CharacterId member, const WorldPosition &position,
                                 std::uint64_t channel_key, double range_mm) const {
  std::lock_guard lock(mutex_);
  return nearby_party_members_locked(member, position, channel_key, range_mm);
}

void PartyWorld::capture_nearby_party_members(
    CharacterId member, const WorldPosition &position, std::uint64_t channel_key, double range_mm,
    const std::function<void(const std::optional<MemberPresence> &,
                             const std::vector<NearbyMember> &)> &capture) const {
  std::lock_guard lock(mutex_);
  const auto *known = online_member(member);
  const auto presence = known ? std::optional<MemberPresence>(known->presence) : std::nullopt;
  capture(presence, nearby_party_members_locked(member, position, channel_key, range_mm));
}

std::vector<PartyWorld::NearbyMember> PartyWorld::nearby_party_members_locked(
    CharacterId member, const WorldPosition &position, std::uint64_t channel_key,
    double range_mm) const {
  std::vector<NearbyMember> nearby;
  if (!service_.established_party_of(member)) {
    return nearby;
  }

  const double range_squared = range_mm * range_mm;
  for (const auto &descriptor : service_.members_of(member)) {
    if (descriptor.char_id == member || descriptor.connection_state != ConnectionState::kConnected) {
      continue;
    }

    const KnownMember *known = online_member(descriptor.char_id);
    if (known == nullptr || !known->presence.position ||
        known->presence.channel_key != channel_key ||
        known->presence.position->map_id != position.map_id) {
      continue;
    }

    const auto &other = *known->presence.position;
    const double delta_x = static_cast<double>(other.x_mm) - position.x_mm;
    const double delta_y = static_cast<double>(other.y_mm) - position.y_mm;
    const double delta_z = static_cast<double>(other.z_mm) - position.z_mm;
    if (delta_x * delta_x + delta_y * delta_y + delta_z * delta_z <= range_squared) {
      nearby.push_back({descriptor.char_id, known->presence.connection_id, other,
                        known->presence.account_id, known->presence.world_id,
                        known->presence.hp > 0, known->presence.quest_ledger});
    }
  }
  return nearby;
}

std::optional<std::uint64_t> PartyWorld::connection_of(CharacterId member) const {
  std::lock_guard lock(mutex_);
  const KnownMember *known = online_member(member);
  if (known == nullptr) {
    return std::nullopt;
  }
  return known->presence.connection_id;
}

std::optional<MemberPresence> PartyWorld::online_presence(CharacterId member) const {
  std::lock_guard lock(mutex_);
  const KnownMember *known = online_member(member);
  if (known == nullptr) {
    return std::nullopt;
  }
  return known->presence;
}

void PartyWorld::decorate_unit_add(nlohmann::json &unit_add_message) const {
  if (!unit_add_message.is_object()) {
    return;
  }

  const auto unit = unit_add_message.find("unit_id");
  if (unit == unit_add_message.end()) {
    return;
  }

  const auto wire_id = parse_int64(*unit);
  if (!wire_id || *wire_id <= 0) {
    return;
  }

  const auto char_id =
      character_id_from_wire_unit_id(static_cast<std::uint64_t>(*wire_id));
  if (!char_id || !is_supported_character_id(*char_id)) {
    return;
  }

  const auto nub = unit_add_message.find("nub");
  if (nub == unit_add_message.end() || !nub->is_object()) {
    return;
  }

  const auto player = nub->find("player");
  if (player == nub->end() || !player->is_object()) {
    return;
  }

  std::lock_guard lock(mutex_);
  const auto party_id = service_.established_party_of(*char_id);
  if (party_id) {
    (*player)["party_id"] = id_string(*party_id);
  }
}

nlohmann::json PartyWorld::change_party_unit_message(const UnitPartyChange &change) {
  // Wire IDs carry the player type nibble. Omit party_id (proto3 zero) on leave.
  nlohmann::json message{{"unit_id", std::to_string(player_wire_unit_id(change.char_id))}};
  if (change.party_id != 0) {
    message["party_id"] = id_string(change.party_id);
  }
  return message;
}

nlohmann::json PartyWorld::error_message(PartyError error, std::uint32_t sequence) {
  const auto code = static_cast<int>(error);
  if (!x5::ec::EErrorCode_IsValid(code)) {
    throw std::logic_error("Party error has no recovered EErrorCode");
  }

  return {{"ec", x5::ec::EErrorCode_Name(static_cast<x5::ec::EErrorCode>(code))},
          {"category", kPartyErrorCategory},
          {"packet_seq", {{"seq", std::to_string(sequence)}}}};
}

PartyResult PartyWorld::dispatch(CharacterId requester, std::uint16_t opcode,
                                 const nlohmann::json &message, TimePoint now) {
  switch (opcode) {
  case kPartyInvitePopupRequest:
    return invite(requester, message, now);
  case kPartyAcceptInviteRequest:
    return accept(requester, message, now);
  case kPartyRejectInviteRequest: {
    const auto leader = read_id(message, "leader_char_id");
    const auto target = read_id(message, "target_char_id");
    if (!leader || !target) {
      return refusal(PartyError::kInviteRequestNotFound);
    }
    return service_.reject(requester, *leader, *target, now);
  }
  case kPartyKickRequest: {
    const auto target = read_id(message, "target_char_id");
    if (!target) {
      return refusal(PartyError::kNotExistsMember);
    }
    return service_.kick(requester, *target, now);
  }
  case kPartyLeaveRequest:
    return service_.leave(requester, now);
  case kPartyChangeLeaderRequest: {
    const auto target = read_id(message, "target_char_id");
    if (!target) {
      return refusal(PartyError::kNotExistsMember);
    }
    return service_.change_leader(requester, *target, now);
  }
  default:
    break;
  }
  throw std::invalid_argument("Opcode is not a party membership request");
}

PartyResult PartyWorld::invite(CharacterId requester, const nlohmann::json &message,
                               TimePoint now) {
  const auto target = read_id(message, "target_char_id");
  if (!target || *target == 0) {
    return refusal(PartyError::kNotExistsInvitee);
  }
  if (!unlocked(requester)) {
    return refusal(PartyError::kUnlockPartyContentRequired);
  }

  // Only an in-world character can see and answer the invite popup.
  if (online_member(*target) == nullptr) {
    return refusal(PartyError::kNotExistsInvitee);
  }
  if (!unlocked(*target)) {
    return refusal(PartyError::kUnlockPartyContentRequired);
  }

  return service_.invite(requester, *target, now);
}

PartyResult PartyWorld::accept(CharacterId requester, const nlohmann::json &message,
                               TimePoint now) {
  const auto leader = read_id(message, "leader_char_id");
  const auto target = read_id(message, "target_char_id");
  const auto party_id = read_id(message, "party_id");
  if (!leader || !target || !party_id) {
    return refusal(PartyError::kInviteRequestNotFound);
  }
  if (!unlocked(requester)) {
    return refusal(PartyError::kUnlockPartyContentRequired);
  }

  return service_.accept(requester, *leader, *target, *party_id, now);
}

bool PartyWorld::unlocked(CharacterId member) const {
  if (config_.unlock_quest_id == 0) {
    return true;
  }

  const KnownMember *known = online_member(member);
  return known != nullptr && known->presence.party_unlocked;
}

const PartyWorld::KnownMember *PartyWorld::online_member(CharacterId member) const {
  const auto found = known_.find(member);
  if (found == known_.end() || !found->second.online) {
    return nullptr;
  }
  return &found->second;
}

std::optional<MemberDescriptor> PartyWorld::describe(CharacterId member) const {
  const auto found = known_.find(member);
  if (found == known_.end()) {
    return std::nullopt;
  }

  const auto &presence = found->second.presence;
  MemberDescriptor descriptor;
  descriptor.char_id = member;
  descriptor.name = presence.name;
  descriptor.level = presence.level;
  descriptor.char_class = presence.char_class;
  if (presence.position) {
    descriptor.map_cms_id = presence.position->map_id;
  }
  return descriptor;
}

void PartyWorld::translate(const std::vector<PartyEvent> &events, PartyUpdate &update) const {
  for (const auto &event : events) {
    const auto opcode = event_opcode(event.kind);
    const auto message = event_message(event);
    for (const auto recipient : event.recipients) {
      // PartyService addresses only Connected seats; this also drops any
      // recipient whose session closed since the event was produced.
      const KnownMember *member = online_member(recipient);
      if (member != nullptr) {
        update.packets.push_back({member->presence.connection_id, opcode, message});
      }
    }
  }
}

void PartyWorld::collect_unit_changes(PartyUpdate &update) {
  for (const auto &[char_id, member] : known_) {
    if (!member.online) {
      continue;
    }

    const PartyId current = service_.established_party_of(char_id).value_or(0);
    const auto announced = announced_party_.find(char_id);
    const PartyId previous = announced == announced_party_.end() ? 0 : announced->second;
    if (current == previous) {
      continue;
    }

    update.unit_changes.push_back({member.presence.connection_id, char_id, current});
    if (current == 0) {
      announced_party_.erase(char_id);
    } else {
      announced_party_[char_id] = current;
    }
  }
}

void PartyWorld::append_world_states(PartyUpdate &update) const {
  for (const auto &[recipient_id, recipient] : known_) {
    if (!recipient.online) {
      continue;
    }

    const auto party_id = service_.established_party_of(recipient_id);
    if (!party_id) {
      continue;
    }

    for (const auto &descriptor : service_.members_of(recipient_id)) {
      if (descriptor.char_id == recipient_id ||
          descriptor.connection_state != ConnectionState::kConnected) {
        continue;
      }

      const KnownMember *subject = online_member(descriptor.char_id);
      if (subject == nullptr) {
        continue;
      }

      const bool shares_channel = same_channel(recipient.presence, subject->presence);
      update.packets.push_back(
          {recipient.presence.connection_id, kPartyMemberWorldStateNotification,
           world_state_message(*party_id, subject->presence, shares_channel)});
    }
  }
}

void PartyWorld::prune_offline_members() {
  std::erase_if(known_, [this](const auto &entry) {
    return !entry.second.online && !service_.party_of(entry.first);
  });
}

} // namespace chronicles::party
