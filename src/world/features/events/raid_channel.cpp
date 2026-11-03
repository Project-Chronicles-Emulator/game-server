#include "world/features/events/raid_channel.hpp"

#include "common/catalog_fields.hpp"
#include "data/client_catalog.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace chronicles::raid {
namespace {

using Json = nlohmann::json;

constexpr char kRaidMapType[] = "Raid";

std::optional<int> requested_map(const Json &message) {
  const auto found = message.find("map_id");
  if (found == message.end() || !found->is_number_integer()) {
    return std::nullopt;
  }

  const auto value = found->get<std::int64_t>();
  if (value <= 0 || value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

Json member_json(const RaidApplicant &member) {
  return {{"char_id", std::to_string(member.char_id)},
          {"name", member.name},
          {"level", member.level},
          {"char_class", member.char_class}};
}

RaidPacket error_packet(const RaidApplicant &requester, const std::string &code,
                        std::uint32_t sequence) {
  return {requester.connection_id, kCommonErrorResponse, raid_error(code, sequence)};
}

RaidUpdate single_error(const RaidApplicant &requester, const std::string &code,
                        std::uint32_t sequence) {
  RaidUpdate update;
  update.packets.push_back(error_packet(requester, code, sequence));
  return update;
}

} // namespace

void RaidUpdate::append(RaidUpdate other) {
  packets.insert(packets.end(), std::make_move_iterator(other.packets.begin()),
                 std::make_move_iterator(other.packets.end()));
  evictions.insert(evictions.end(), other.evictions.begin(), other.evictions.end());
}

Json raid_error(const std::string &code, std::uint32_t sequence) {
  return {{"ec", code}, {"packet_seq", {{"seq", std::to_string(sequence)}}}};
}

std::vector<std::string> RaidCatalog::required_tables() {
  return {"RaidUI", "Map", "GameVariable"};
}

std::shared_ptr<const RaidCatalog> RaidCatalog::load(ClientCatalog &catalog) {
  // Evidenced default; the catalog row normally supplies the same value.
  std::chrono::milliseconds auto_enter{10000};
  constexpr std::int64_t kMaximumAutoEnterMilliseconds = 600000;
  if (const auto value = game_variable_integer(catalog, "RaidAutoEnterTime", 0,
                                               kMaximumAutoEnterMilliseconds, "Raid channels")) {
    auto_enter = std::chrono::milliseconds{*value};
  }
  return std::make_shared<const RaidCatalog>(catalog.rows("RaidUI"), catalog.rows("Map"),
                                             auto_enter);
}

RaidCatalog::RaidCatalog(const std::vector<Json> &raid_ui_rows, const std::vector<Json> &map_rows,
                         std::chrono::milliseconds auto_enter)
    : auto_enter_(auto_enter) {
  if (auto_enter_.count() < 0) {
    throw std::invalid_argument("Raid auto-enter delay must not be negative");
  }

  std::unordered_map<int, const Json *> maps_by_id;
  for (const auto &row : map_rows) {
    maps_by_id.emplace(catalog_fields::required_identifier(row, "id"), &row);
  }

  for (const auto &row : raid_ui_rows) {
    const int map_id = catalog_fields::required_identifier(row, "FK_raidMapId");
    const auto found = maps_by_id.find(map_id);
    if (found == maps_by_id.end()) {
      throw std::runtime_error("RaidUI names an unknown map " + std::to_string(map_id));
    }

    const Json &map_row = *found->second;
    if (catalog_fields::text(map_row, "type") != kRaidMapType) {
      throw std::runtime_error("RaidUI map is not a Raid map: " + std::to_string(map_id));
    }

    RaidMap map;
    map.map_id = map_id;
    map.parent_map_id = catalog_fields::optional_identifier(map_row, "parentMapId").value_or(0);
    map.capacity = catalog_fields::non_negative_int(map_row, "chMaxCount");
    map.time_limit = std::chrono::seconds{catalog_fields::non_negative_int(map_row, "timeLimit")};
    if (map.capacity <= 0 || map.time_limit.count() <= 0) {
      throw std::runtime_error("Raid map capacity and time limit must be positive: " +
                               std::to_string(map_id));
    }
    if (!maps_.emplace(map_id, map).second) {
      throw std::runtime_error("Duplicate RaidUI map " + std::to_string(map_id));
    }
  }
}

const RaidMap *RaidCatalog::find(int map_id) const {
  const auto found = maps_.find(map_id);
  return found == maps_.end() ? nullptr : &found->second;
}

std::chrono::milliseconds RaidCatalog::auto_enter_delay() const {
  return auto_enter_;
}

std::size_t RaidCatalog::size() const {
  return maps_.size();
}

RaidMatchmaker::RaidMatchmaker(std::shared_ptr<const RaidCatalog> catalog,
                               RaidMatchmakerConfig config)
    : catalog_(std::move(catalog)), config_(config) {
  if (!catalog_) {
    throw std::invalid_argument("Raid matchmaking needs a raid catalog");
  }
  if (config_.minimum_members == 0) {
    throw std::invalid_argument("A raid channel needs at least one member");
  }
}

bool RaidMatchmaker::handles(std::uint16_t opcode) {
  return opcode == kRaidChannelRegisterRequest || opcode == kRaidChannelCancelRequest ||
         opcode == kRaidChannelLeaveRequest;
}

RaidUpdate RaidMatchmaker::receive(const RaidApplicant &requester, std::uint16_t opcode,
                                   const Json &message, std::uint32_t sequence, bool eligible,
                                   Clock::time_point now) {
  std::lock_guard lock(mutex_);
  if (requester.char_id <= 0) {
    return single_error(requester, "Raid_RequesterNotFound", sequence);
  }

  switch (opcode) {
  case kRaidChannelRegisterRequest:
    return register_applicant(requester, message, sequence, eligible, now);
  case kRaidChannelCancelRequest:
    return cancel(requester, message, sequence);
  case kRaidChannelLeaveRequest:
    return leave(requester, message, sequence);
  default:
    return {};
  }
}

RaidUpdate RaidMatchmaker::register_applicant(const RaidApplicant &requester, const Json &message,
                                              std::uint32_t sequence, bool eligible,
                                              Clock::time_point now) {
  if (!eligible) {
    return single_error(requester, "Raid_RegisterDeclined", sequence);
  }

  const auto map_id = requested_map(message);
  const auto *map = map_id ? catalog_->find(*map_id) : nullptr;
  if (!map) {
    return single_error(requester, "Raid_InvalidChannelId", sequence);
  }
  if (queued(requester.char_id) || channel_of(requester.char_id) ||
      departures_.contains(requester.char_id)) {
    return single_error(requester, "Raid_AlreadyApplied", sequence);
  }

  queues_[map->map_id].push_back({requester, now});

  RaidUpdate update;
  update.packets.push_back(
      {requester.connection_id, kRaidChannelRegisterNotification, {{"map_id", map->map_id}}});
  match_queue(map->map_id, now, update);
  return update;
}

RaidUpdate RaidMatchmaker::cancel(const RaidApplicant &requester, const Json &message,
                                  std::uint32_t sequence) {
  const auto map_id = requested_map(message);
  const auto queue = map_id ? queues_.find(*map_id) : queues_.end();
  if (queue == queues_.end()) {
    return single_error(requester, "Raid_NoChannelToCancel", sequence);
  }

  const auto application = std::ranges::find_if(queue->second, [&](const Application &entry) {
    return entry.applicant.char_id == requester.char_id &&
           entry.applicant.connection_id == requester.connection_id;
  });
  if (application == queue->second.end()) {
    return single_error(requester, "Raid_NoChannelToCancel", sequence);
  }
  queue->second.erase(application);

  RaidUpdate update;
  update.packets.push_back(
      {requester.connection_id, kRaidChannelCancelNotification, {{"map_id", *map_id}}});
  return update;
}

RaidUpdate RaidMatchmaker::leave(const RaidApplicant &requester, const Json &message,
                                 std::uint32_t sequence) {
  const auto map_id = requested_map(message);
  auto *channel = channel_of(requester.char_id);
  if (!channel || !map_id || channel->map_id != *map_id) {
    return single_error(requester, "Raid_RequesterNotFound", sequence);
  }

  const auto member = std::ranges::find_if(channel->members, [&](const RaidApplicant &entry) {
    return entry.char_id == requester.char_id && entry.connection_id == requester.connection_id;
  });
  if (member == channel->members.end()) {
    return single_error(requester, "Raid_RequesterNotFound", sequence);
  }

  RaidUpdate update;
  const int left_map = channel->map_id;
  departures_.emplace(requester.char_id, Departure{requester.connection_id, left_map});
  remove_member(*channel, requester.char_id, true, update);
  update.evictions.push_back({requester.connection_id, left_map});
  return update;
}

RaidMoveDecision RaidMatchmaker::authorize_move(std::int64_t char_id, const Json &message,
                                                std::uint64_t connection_id,
                                                Clock::time_point now) const {
  std::lock_guard lock(mutex_);
  const auto *channel = channel_of(char_id);
  if (!channel) {
    return {std::string("Raid_RequesterNotFound"), nullptr};
  }

  const bool owns_seat = std::ranges::any_of(channel->members, [&](const RaidApplicant &member) {
    return member.char_id == char_id && member.connection_id == connection_id;
  });
  if (!owns_seat) {
    return {std::string("Raid_RequesterNotFound"), nullptr};
  }
  const auto map_id = requested_map(message);
  if (!map_id || *map_id != channel->map_id) {
    return {std::string("Raid_InvalidChannelId"), nullptr};
  }
  if (now < channel->opens_at || now >= channel->closes_at) {
    return {std::string("Raid_TimeUnavailable"), nullptr};
  }
  return {std::nullopt, catalog_->find(channel->map_id)};
}

RaidUpdate RaidMatchmaker::force_match(const RaidApplicant &requester, int map_id,
                                       Clock::time_point now) {
  std::lock_guard lock(mutex_);
  const auto *map = catalog_->find(map_id);
  if (!map) {
    return single_error(requester, "Raid_InvalidChannelId", 0);
  }
  if (channel_of(requester.char_id) || departures_.contains(requester.char_id)) {
    return single_error(requester, "Raid_AlreadyApplied", 0);
  }
  if (map_occupied(map_id)) {
    return single_error(requester, "ChannelIsFull", 0);
  }

  for (auto &[queued_map, queue] : queues_) {
    std::erase_if(queue, [&](const Application &entry) {
      return entry.applicant.char_id == requester.char_id;
    });
  }

  RaidUpdate update;
  form_channel(*map, {requester}, now, update);
  return update;
}

RaidUpdate RaidMatchmaker::detach(std::int64_t char_id, std::uint64_t connection_id) {
  std::lock_guard lock(mutex_);
  const auto departure = departures_.find(char_id);
  if (departure != departures_.end() && departure->second.connection_id == connection_id) {
    departures_.erase(departure);
  }

  for (auto &[map_id, queue] : queues_) {
    std::erase_if(queue, [&](const Application &entry) {
      return entry.applicant.char_id == char_id && entry.applicant.connection_id == connection_id;
    });
  }

  RaidUpdate update;
  auto *channel = channel_of(char_id);
  if (!channel) {
    return update;
  }

  const auto member = std::ranges::find_if(
      channel->members, [&](const RaidApplicant &entry) { return entry.char_id == char_id; });
  if (member != channel->members.end() && member->connection_id == connection_id) {
    remove_member(*channel, char_id, false, update);
  }
  return update;
}

void RaidMatchmaker::complete_eviction(std::int64_t char_id, std::uint64_t connection_id,
                                       int map_id) {
  std::lock_guard lock(mutex_);
  const auto departure = departures_.find(char_id);
  if (departure != departures_.end() && departure->second.connection_id == connection_id &&
      departure->second.map_id == map_id) {
    departures_.erase(departure);
  }
}

RaidUpdate RaidMatchmaker::tick(Clock::time_point now) {
  std::lock_guard lock(mutex_);
  RaidUpdate update;
  for (auto &[map_id, queue] : queues_) {
    match_queue(map_id, now, update);
  }

  for (auto channel = channels_.begin(); channel != channels_.end();) {
    if (now < channel->second.closes_at) {
      ++channel;
      continue;
    }

    for (const auto &member : channel->second.members) {
      departures_.emplace(member.char_id, Departure{member.connection_id, channel->second.map_id});
      update.packets.push_back({member.connection_id,
                                kRaidChannelDestroyNotification,
                                {{"map_id", channel->second.map_id}}});
      update.evictions.push_back({member.connection_id, channel->second.map_id});
    }
    channel = channels_.erase(channel);
  }

  std::erase_if(queues_, [](const auto &entry) { return entry.second.empty(); });
  return update;
}

bool RaidMatchmaker::active() const {
  std::lock_guard lock(mutex_);
  if (!channels_.empty() || !departures_.empty()) {
    return true;
  }
  return std::ranges::any_of(queues_, [](const auto &entry) { return !entry.second.empty(); });
}

RaidMembership RaidMatchmaker::membership(std::int64_t char_id) const {
  std::lock_guard lock(mutex_);
  if (const auto *channel = channel_of(char_id)) {
    return {RaidState::matched, channel->map_id, channel->id};
  }

  for (const auto &[map_id, queue] : queues_) {
    const bool waiting = std::ranges::any_of(
        queue, [&](const Application &entry) { return entry.applicant.char_id == char_id; });
    if (waiting) {
      return {RaidState::waiting, map_id};
    }
  }
  return {};
}

const RaidCatalog &RaidMatchmaker::catalog() const {
  return *catalog_;
}

void RaidMatchmaker::match_queue(int map_id, Clock::time_point now, RaidUpdate &update) {
  const auto *map = catalog_->find(map_id);
  auto queue = queues_.find(map_id);
  if (!map || queue == queues_.end()) {
    return;
  }

  // Reserve the map until the prior group has actually left shared presence, not merely
  // until its membership records have been removed. An eviction can wait on a map load.
  if (map_occupied(map_id)) {
    return;
  }

  auto &applications = queue->second;
  const auto capacity = static_cast<std::size_t>(map->capacity);
  const auto take = [&](std::size_t count) {
    std::vector<RaidApplicant> members;
    for (std::size_t index = 0; index < count; ++index) {
      members.push_back(applications.front().applicant);
      applications.pop_front();
    }
    return members;
  };

  if (applications.size() >= capacity) {
    form_channel(*map, take(capacity), now, update);
    return;
  }

  const bool waited =
      !applications.empty() && now - applications.front().registered_at >= config_.match_wait;
  if (waited && applications.size() >= config_.minimum_members) {
    form_channel(*map, take(applications.size()), now, update);
  }
}

void RaidMatchmaker::form_channel(const RaidMap &map, std::vector<RaidApplicant> members,
                                  Clock::time_point now, RaidUpdate &update) {
  Json roster = Json::array();
  for (const auto &member : members) {
    roster.push_back(member_json(member));
  }

  for (const auto &member : members) {
    update.packets.push_back({member.connection_id,
                              kRaidChannelMatchedNotification,
                              {{"map_id", map.map_id}, {"members", roster}}});
  }

  Channel channel;
  channel.id = next_channel_id_++;
  channel.map_id = map.map_id;
  channel.members = std::move(members);
  // The channel opens when the entry countdown ends and lasts the map's time limit.
  channel.opens_at = now + catalog_->auto_enter_delay();
  channel.closes_at = channel.opens_at + map.time_limit;
  channels_.emplace(channel.id, std::move(channel));
}

void RaidMatchmaker::remove_member(Channel &channel, std::int64_t char_id, bool notify_leaver,
                                   RaidUpdate &update) {
  const Json notification{{"map_id", channel.map_id}, {"char_id", std::to_string(char_id)}};
  for (const auto &member : channel.members) {
    if (member.char_id == char_id && !notify_leaver) {
      continue;
    }
    update.packets.push_back({member.connection_id, kRaidChannelLeaveNotification, notification});
  }

  std::erase_if(channel.members,
                [&](const RaidApplicant &member) { return member.char_id == char_id; });
  if (channel.members.empty()) {
    channels_.erase(channel.id);
  }
}

bool RaidMatchmaker::map_occupied(int map_id) const {
  const auto on_map = [&](const auto &entry) { return entry.second.map_id == map_id; };
  return std::ranges::any_of(channels_, on_map) || std::ranges::any_of(departures_, on_map);
}

bool RaidMatchmaker::queued(std::int64_t char_id) const {
  for (const auto &[map_id, queue] : queues_) {
    const bool found = std::ranges::any_of(
        queue, [&](const Application &entry) { return entry.applicant.char_id == char_id; });
    if (found) {
      return true;
    }
  }
  return false;
}

RaidMatchmaker::Channel *RaidMatchmaker::channel_of(std::int64_t char_id) {
  const auto &self = *this;
  return const_cast<Channel *>(self.channel_of(char_id));
}

const RaidMatchmaker::Channel *RaidMatchmaker::channel_of(std::int64_t char_id) const {
  for (const auto &[id, channel] : channels_) {
    const bool member = std::ranges::any_of(
        channel.members, [&](const RaidApplicant &entry) { return entry.char_id == char_id; });
    if (member) {
      return &channel;
    }
  }
  return nullptr;
}

} // namespace chronicles::raid
