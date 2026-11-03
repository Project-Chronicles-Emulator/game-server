#include "world/features/events/channel_event.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace chronicles::channel_event {
namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t kMaximumDefinitionBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaximumBindings = 10000;
constexpr std::size_t kMaximumAltars = 10000;

int positive_integer(const Json &value, const char *field) {
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string("Channel event definition field is not an integer: ") +
                             field);
  }
  const auto number = value.get<std::int64_t>();
  if (number <= 0 || number > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Channel event definition field is out of range: ") +
                             field);
  }
  return static_cast<int>(number);
}

int coordinate(const Json &value) {
  if (!value.is_number_integer()) {
    throw std::runtime_error("Tribute altar coordinate is not an integer");
  }
  const auto number = value.get<std::int64_t>();
  if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
    throw std::runtime_error("Tribute altar coordinate is out of range");
  }
  return static_cast<int>(number);
}

std::optional<std::uint64_t> decimal_unit_id(std::string_view text) {
  std::uint64_t result{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size() || result == 0) {
    return std::nullopt;
  }
  return result;
}

// ChannelEventItemTributeReq.target_unit_id is uint64, which protobuf JSON writes as text.
std::optional<std::uint64_t> requested_unit_id(const Json &request) {
  const auto found = request.find("target_unit_id");
  if (found == request.end()) {
    return std::nullopt;
  }
  if (found->is_string()) {
    return decimal_unit_id(found->get<std::string>());
  }
  if (found->is_number_unsigned()) {
    const auto value = found->get<std::uint64_t>();
    return value == 0 ? std::nullopt : std::optional<std::uint64_t>(value);
  }
  return std::nullopt;
}

std::optional<int> requested_positive(const Json &request, const char *field) {
  const auto found = request.find(field);
  if (found == request.end() || !found->is_number_integer()) {
    return std::nullopt;
  }
  const auto value = found->get<std::int64_t>();
  if (value <= 0 || value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

double distance_mm(const WorldPosition &position, const std::array<int, 3> &point) {
  const double dx = static_cast<double>(position.x_mm) - point[0];
  const double dy = static_cast<double>(position.y_mm) - point[1];
  const double dz = static_cast<double>(position.z_mm) - point[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Json packet_seq(const Json &request, std::uint32_t sequence) {
  const auto found = request.find("packet_seq");
  if (found != request.end() && found->is_object()) {
    return *found;
  }
  return {{"seq", std::to_string(sequence)}};
}

TributePlan refused(const char *code) {
  TributePlan plan;
  plan.error = code;
  return plan;
}

} // namespace

const char *status_name(EventStatus status) {
  switch (status) {
  case EventStatus::none:
    return "EX5ChannelEventStatusType_None";
  case EventStatus::fail:
    return "EX5ChannelEventStatusType_Fail";
  case EventStatus::success:
    return "EX5ChannelEventStatusType_Success";
  case EventStatus::fail_timeout:
    return "EX5ChannelEventStatusType_FailTimeout";
  case EventStatus::raid_success:
    return "EX5ChannelEventStatusType_RaidSuccess";
  case EventStatus::channel_close:
    return "EX5ChannelEventStatusType_ChannelClose";
  }
  throw std::invalid_argument("Unknown channel event status");
}

std::shared_ptr<const ChannelEventData> ChannelEventData::load(const std::filesystem::path &path) {
  if (std::filesystem::file_size(path) > kMaximumDefinitionBytes) {
    throw std::runtime_error("Channel event definitions exceed the size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open channel event definitions");
  }

  Json document;
  input >> document;
  return std::make_shared<const ChannelEventData>(document);
}

ChannelEventData::ChannelEventData(const Json &document) {
  if (document.at("format_version") != 1 || !document.at("channel_events").is_array() ||
      !document.at("tribute_altars").is_array() ||
      document.at("channel_events").size() > kMaximumBindings ||
      document.at("tribute_altars").size() > kMaximumAltars) {
    throw std::runtime_error("Unsupported channel event definitions");
  }

  for (const auto &row : document.at("channel_events")) {
    ChannelEventBinding binding;
    binding.map_id = positive_integer(row.at("map_id"), "map_id");
    binding.channel_event_id = positive_integer(row.at("channel_event_id"), "channel_event_id");
    if (!row.at("reward_id").is_null()) {
      binding.reward_id = positive_integer(row.at("reward_id"), "reward_id");
    }
    if (!bindings_.emplace(binding.map_id, binding).second) {
      throw std::runtime_error("Duplicate channel event map binding");
    }
  }

  for (const auto &row : document.at("tribute_altars")) {
    TributeAltar altar;
    altar.map_id = positive_integer(row.at("map_id"), "map_id");
    const auto unit_id = decimal_unit_id(row.at("unit_id").get<std::string>());
    if (!unit_id) {
      throw std::runtime_error("Invalid tribute altar unit ID");
    }
    altar.unit_id = *unit_id;
    altar.doodad_cms_id = positive_integer(row.at("doodad_cms_id"), "doodad_cms_id");
    altar.item_cms_id = positive_integer(row.at("item_cms_id"), "item_cms_id");

    const auto &position = row.at("position");
    if (!position.is_array() || position.size() != 3) {
      throw std::runtime_error("Tribute altar position must have three coordinates");
    }
    for (std::size_t axis = 0; axis < altar.position_mm.size(); ++axis) {
      altar.position_mm[axis] = coordinate(position.at(axis));
    }

    if (!altars_.emplace(std::make_pair(altar.map_id, altar.unit_id), altar).second) {
      throw std::runtime_error("Duplicate tribute altar placement");
    }
  }
}

const ChannelEventBinding *ChannelEventData::binding(int map_id) const {
  const auto found = bindings_.find(map_id);
  return found == bindings_.end() ? nullptr : &found->second;
}

const TributeAltar *ChannelEventData::altar(int map_id, std::uint64_t unit_id) const {
  const auto found = altars_.find({map_id, unit_id});
  return found == altars_.end() ? nullptr : &found->second;
}

const TributeAltar *ChannelEventData::nearest_altar(const WorldPosition &position,
                                                    double range_mm) const {
  const TributeAltar *nearest{};
  double nearest_distance = range_mm;
  for (const auto &[key, altar] : altars_) {
    if (altar.map_id != position.map_id) {
      continue;
    }
    const double distance = distance_mm(position, altar.position_mm);
    if (distance <= nearest_distance) {
      nearest = &altar;
      nearest_distance = distance;
    }
  }
  return nearest;
}

std::size_t ChannelEventData::altar_count() const {
  return altars_.size();
}

std::size_t ChannelEventData::binding_count() const {
  return bindings_.size();
}

ChannelEventBoard::ChannelEventBoard(int tribute_target) : tribute_target_(tribute_target) {
  if (tribute_target_ <= 0) {
    throw std::invalid_argument("The tribute target must be positive");
  }
}

TributeProgress ChannelEventBoard::tribute_progress(int map_id, std::uint64_t altar_unit_id) const {
  std::lock_guard lock(mutex_);
  const auto found = offerings_.find({map_id, altar_unit_id});
  return {found == offerings_.end() ? 0 : found->second, tribute_target_};
}

TributeCommit ChannelEventBoard::commit_tribute(int map_id, std::uint64_t altar_unit_id, int count,
                                                const std::function<bool()> &commit_items) {
  if (count <= 0 || map_id <= 0 || altar_unit_id == 0 || !commit_items) {
    throw std::invalid_argument("Invalid tribute commit arguments");
  }

  std::lock_guard lock(mutex_);
  auto &offered = offerings_[{map_id, altar_unit_id}];
  auto &event_status = statuses_[map_id];
  const TributeProgress before{offered, tribute_target_};
  if (count > tribute_target_ - offered) {
    return {TributeCommitStatus::limit_reached, before, false};
  }
  if (!commit_items()) {
    return {TributeCommitStatus::storage_conflict, before, false};
  }

  offered += count;
  const bool completed = offered == tribute_target_ && event_status != EventStatus::success;
  if (completed) {
    event_status = EventStatus::success;
  }
  return {TributeCommitStatus::committed, {offered, tribute_target_}, completed};
}

EventStatus ChannelEventBoard::status(int map_id) const {
  std::lock_guard lock(mutex_);
  const auto found = statuses_.find(map_id);
  return found == statuses_.end() ? EventStatus::none : found->second;
}

bool ChannelEventBoard::set_status(int map_id, EventStatus status) {
  std::lock_guard lock(mutex_);
  auto &current = statuses_[map_id];
  if (current == status) {
    return false;
  }
  current = status;
  return true;
}

void ChannelEventBoard::reset(int map_id) {
  std::lock_guard lock(mutex_);
  statuses_.erase(map_id);
  std::erase_if(offerings_, [&](const auto &entry) { return entry.first.first == map_id; });
}

TributePlan plan_tribute(const ChannelEventData &data, const Json &request,
                         const WorldPosition &position, const WorldProgress &progress) {
  if (progress.dead) {
    return refused("BadRequest");
  }
  const auto unit_id = requested_unit_id(request);
  const auto *altar = unit_id ? data.altar(position.map_id, *unit_id) : nullptr;
  if (!altar) {
    return refused("Tribute_InvalidTarget");
  }
  if (distance_mm(position, altar->position_mm) > kTributeRangeMm) {
    return refused("Tribute_TargetTooFar");
  }

  const auto item_cms_id = requested_positive(request, "item_cms_id");
  const auto count = requested_positive(request, "count");
  if (!item_cms_id || *item_cms_id != altar->item_cms_id) {
    return refused("Tribute_InvalidItem");
  }
  if (!count || *count > kMaximumTributePerRequest) {
    return refused("BadRequest");
  }

  const auto owned = progress.stackables.find(altar->item_cms_id);
  if (owned == progress.stackables.end() || owned->second < *count) {
    return refused("Tribute_InvalidItem");
  }
  if (progress.revision == std::numeric_limits<std::int64_t>::max()) {
    return refused("BadRequest");
  }

  TributePlan plan;
  plan.altar = altar;
  plan.count = *count;
  return plan;
}

WorldProgress consume_tribute(const WorldProgress &before, const TributePlan &plan) {
  if (plan.error || !plan.altar || plan.count <= 0) {
    throw std::invalid_argument("Only an accepted tribute plan can be consumed");
  }

  auto after = before;
  auto stack = after.stackables.find(plan.altar->item_cms_id);
  if (stack == after.stackables.end() || stack->second < plan.count) {
    throw std::invalid_argument("The tribute plan exceeds the owned stack");
  }
  // World-progress storage keeps stack rows at zero; removing one violates its monotonic
  // record-set contract and would reject an otherwise valid last-item contribution.
  stack->second -= plan.count;
  ++after.revision;
  return after;
}

GameplayPacket tribute_count_packet(const TributeProgress &progress) {
  return {kTributeCountNotification,
          {{"count", progress.count}, {"target_count", progress.target}}};
}

GameplayPacket status_packet(EventStatus status) {
  return {kChangeChannelEventStatusNotification, {{"status_type", status_name(status)}}};
}

GameplayPacket tribute_items_packet(const TributePlan &plan, const WorldProgress &after,
                                    const Json &request, std::uint32_t sequence) {
  const auto stack = after.stackables.find(plan.altar->item_cms_id);
  const int remaining = stack == after.stackables.end() ? 0 : stack->second;
  return {kItemsUpdateNotification,
          {{"stackables",
            Json::array({{{"item_cms_id", plan.altar->item_cms_id}, {"quantity", remaining}}})},
           {"tribute", Json::object()},
           {"packet_seq", packet_seq(request, sequence)}}};
}

GameplayPacket common_error(std::string_view code, std::uint32_t sequence) {
  return {kCommonErrorResponse,
          {{"ec", std::string(code)}, {"packet_seq", {{"seq", std::to_string(sequence)}}}}};
}

bool reported_position_near(const Json &request, const WorldPosition &server_position,
                            double range_mm) {
  const auto position = request.find("pos");
  if (position == request.end() || !position->is_object()) {
    return false;
  }

  std::array<int, 3> reported{};
  constexpr const char *kAxes[] = {"millimeter_x", "millimeter_y", "millimeter_z"};
  for (std::size_t axis = 0; axis < reported.size(); ++axis) {
    // Protobuf JSON omits zero coordinates.
    const auto value = position->value(kAxes[axis], Json(0));
    if (!value.is_number_integer()) {
      return false;
    }
    const auto number = value.get<std::int64_t>();
    if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
      return false;
    }
    reported[axis] = static_cast<int>(number);
  }
  return distance_mm(server_position, reported) <= range_mm;
}

} // namespace chronicles::channel_event
