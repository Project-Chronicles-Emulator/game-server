#include "world/features/housing/housing_feature.hpp"

#include "world/core/world_request_opcodes.hpp"
#include <spdlog/spdlog.h>

#include <chrono>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace chronicles::housing {
namespace {

using world_request::kEscapeRequest;
using world_request::kInteractionEndRequest;
using world_request::kInteractionExecuteRequest;
using world_request::kInteractionStartRequest;
using world_request::kZoneChannelMoveRequest;

} // namespace

HousingSession::HousingSession(HousingFeature &feature, ::chronicles::ProgressAccess &progress,
                               HousingSessionContext &context)
    : feature_(feature), progress_(progress), context_(context) {
}

void HousingSession::stage_entry(const HousingDestination &destination) {
  pending_entry_ = destination;
}

bool HousingSession::has_pending_entry() const noexcept {
  return pending_entry_.has_value();
}

void HousingSession::cancel_pending_entry() {
  pending_entry_.reset();
}

void HousingSession::forget_origin() {
  origin_.reset();
}

bool HousingSession::in_housing() const noexcept {
  return location_.has_value();
}

int HousingSession::presence_map() const noexcept {
  return presence_map_.load();
}

void HousingSession::run_task(const std::function<void(const ProgressAccess &)> &task) {
  const auto access = progress_access();
  try {
    task(access);
  } catch (const std::exception &error) {
    spdlog::error("Housing session task failed connection={}: {}", context_.housing_connection_id(),
                  error.what());
  }
}

void HousingSession::on_map_departure() {
  location_.reset();
  presence_map_.store(0);
  if (feature_.hub_) {
    feature_.hub_->set_location(context_.housing_character_id(), context_.housing_connection_id(),
                                std::nullopt);
  }
}

void HousingSession::on_leave_world() {
  if (attached_ && feature_.hub_ && context_.housing_has_handoff()) {
    feature_.hub_->detach(context_.housing_character_id(), context_.housing_connection_id());
  }
}

// Where a character saved inside a housing area re-enters it: a pending visit, its own
// channel there, or the first channel. Empty outside housing maps.
std::optional<housing::HousingDestination>
HousingSession::entry_destination(const Character &character) const {
  if (!feature_.hub_ || !character.world_position ||
      !feature_.hub_->catalog().zone_for_map(character.world_position->map_id)) {
    return std::nullopt;
  }

  auto destination =
      feature_.hub_->service().take_destination(character.id, character.world_position->map_id);
  if (destination) {
    // A relog keeps the saved spot; a visit carries its own arrival point.
    if (destination->spawn.map_id != character.world_position->map_id) {
      destination->spawn = *character.world_position;
    }
  }
  return destination;
}

// The authenticated housing identity. Name and IDs come from the handoff and the stored
// character row captured at entry, never from a payload.
std::optional<housing::HousingActor>
HousingSession::authenticated_actor(const std::optional<WorldPosition> &position) const {
  if (!context_.housing_has_handoff() || !context_.housing_character_name()) {
    return std::nullopt;
  }

  housing::HousingActor actor;
  actor.account_id = context_.housing_account_id();
  actor.character_id = context_.housing_character_id();
  actor.world_id = context_.housing_world_id();
  actor.name = *context_.housing_character_name();
  actor.position = position;
  return actor;
}

// Housing commits read and adopt the tutorial's cached progress when one exists, so later
// tutorial writes still compare against stored state. Worker strand only.
housing::ProgressAccess HousingSession::progress_access() {
  housing::ProgressAccess access;
  access.character_id = context_.housing_has_handoff() ? context_.housing_character_id() : 0;
  access.load = [this] {
    return progress_.load();
  };
  access.adopt = [this](const WorldProgress &committed) {
    if (!context_.housing_has_tutorial()) {
      return;
    }
    try {
      context_.housing_publish_progress(committed);
    } catch (const std::logic_error &error) {
      spdlog::warn("Tutorial could not adopt housing progress connection={}: {}",
                   context_.housing_connection_id(), error.what());
    }
  };
  return access;
}

std::vector<GameplayPacket>
HousingSession::gameplay_packets(std::vector<housing::WorldPacket> packets) {
  std::vector<GameplayPacket> converted;
  converted.reserve(packets.size());
  for (auto &packet : packets) {
    converted.push_back({packet.opcode, std::move(packet.message)});
  }
  return converted;
}

std::vector<GameplayPacket> HousingSession::receive(const WorldGameplayRequest &request,
                                                    const std::optional<WorldPosition> &position) {
  const auto actor = authenticated_actor(position);
  if (!actor) {
    return {};
  }
  const auto unit_id = context_.housing_player_unit_id(actor->character_id);
  return gameplay_packets(feature_.hub_->receive(*actor, unit_id, request.opcode, request.message,
                                                 request.sequence, progress_access()));
}

// Synthetic presence map for one housing channel: visitors of the same channel see each
// other, other channels of the same map do not. Real map IDs are positive.
int HousingSession::channel_presence_map(const housing::HousingLocation &location) {
  return -(location.zone_key * 1000 + location.channel);
}

// Moves the character into a housing channel on this connection. Worker strand only.
void HousingSession::travel(const housing::HousingDestination &destination) {
  if (!context_.housing_has_handoff() || context_.housing_closed()) {
    return;
  }
  // Housing must remember the public return point, not a raid position whose seat this
  // travel will release. Capture it before map departure releases the raid seat and its return
  // origin.
  const auto origin = context_.housing_public_return_origin(destination.origin);
  if (!context_.housing_begin_map_move(destination.spawn)) {
    return;
  }
  context_.housing_clear_pending_exit();
  pending_entry_ = destination;
  pending_entry_->origin = origin;
  if (origin) {
    origin_ = origin;
  }
  spdlog::info("Housing travel connection={} map={} channel={}", context_.housing_connection_id(),
               destination.map_id, destination.channel);
}

bool HousingSession::walk_in(const WorldPosition &arrival) {
  if (!feature_.hub_ || !context_.housing_has_handoff() || context_.housing_closed()) {
    return false;
  }
  const auto destination =
      feature_.hub_->service().walk_in_destination(context_.housing_character_id(), arrival);
  if (!destination) {
    return false;
  }

  origin_.reset();
  pending_entry_.reset();
  travel(*destination);
  return pending_entry_.has_value();
}

// Leaves the housing area for the position the character came from. Worker strand only.
void HousingSession::leave(const std::optional<WorldPosition> &departure_point) {
  if (!context_.housing_has_handoff() || !location_) {
    return;
  }

  auto target = origin_;
  if (!target && feature_.hub_) {
    target = feature_.hub_->service().origin_of(context_.housing_character_id());
  }
  if (!target) {
    // No recorded origin (after a restart, a login inside the area or walking in): step out
    // where the client asked from, which it compares with the spawn point.
    target = context_.housing_default_return_position(
        departure_point ? departure_point : context_.housing_current_position());
  }
  if (const auto parent = context_.housing_raid_parent_map(target->map_id)) {
    // A recovered housing origin cannot return into a released raid seat.
    if (*parent <= 0) {
      spdlog::warn("Housing return refused connection={}: raid has no parent map",
                   context_.housing_connection_id());
      return;
    }
    target->map_id = *parent;
  }
  if (!context_.housing_begin_map_move(*target)) {
    return;
  }
  pending_entry_.reset();
  context_.housing_stage_pending_exit(*target);
  spdlog::info("Housing exit connection={} map={}", context_.housing_connection_id(),
               target->map_id);
}

// Registers the character with the housing directory once its client entered the world and
// returns its own site record. Worker strand only.
std::vector<GameplayPacket>
HousingSession::publish_presence(bool entered_world, const std::optional<WorldPosition> &position) {
  if (!feature_.hub_ || !entered_world || attached_ || context_.housing_closed()) {
    return {};
  }
  const auto actor = authenticated_actor(position);
  if (!actor) {
    return {};
  }

  auto &hub = *feature_.hub_;
  attached_ = true;
  hub.attach(*actor, context_.housing_connection_id(),
             context_.housing_player_unit_id(actor->character_id), context_.housing_session_port());
  if (location_) {
    hub.set_location(actor->character_id, context_.housing_connection_id(), location_);
  }
  if (context_.housing_closed()) {
    hub.detach(actor->character_id, context_.housing_connection_id());
    return {};
  }

  std::vector<GameplayPacket> packets;
  // A tutorial entry's snapshot carries no housing storage; other entries send it in the
  // bootstrap (WorldCharHousingItemsNoti).
  if (context_.housing_has_tutorial()) {
    if (auto items = housing::housing_items_packet(*context_.housing_tutorial_progress())) {
      packets.push_back({items->opcode, std::move(items->message)});
    }
  }

  auto site = hub.service().personal_site(actor->character_id);
  if (!site) {
    site = hub.service().guild_site_of(actor->character_id);
  }
  if (site) {
    const auto owner = housing::owner_site_packet(site);
    packets.push_back({owner.opcode, owner.message});
  }
  return packets;
}

bool HousingSession::handle_request(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &party_position = frame.party_position;
  auto &session_packets = frame.session_packets;

  if (gameplay_request && feature_.hub_) {
    const auto opcode = gameplay_request->opcode;
    if (housing::HousingHub::handles(opcode)) {
      session_packets = receive(*gameplay_request, party_position);
      return true;
    }
  }

  return false;
}

bool HousingSession::handle_toggle(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &party_position = frame.party_position;
  auto &session_packets = frame.session_packets;

  if (!gameplay_request || !feature_.hub_ || !location_) {
    return false;
  }
  const auto opcode = gameplay_request->opcode;
  if (opcode != kInteractionStartRequest && opcode != kInteractionExecuteRequest &&
      opcode != kInteractionEndRequest) {
    return false;
  }

  const auto actor = authenticated_actor(party_position);
  if (!actor) {
    return false;
  }
  auto &hub = *feature_.hub_;

  // Garden plots and interactive decorations first: their targets are profiled doodads, which
  // no pose or door request names.
  auto used = hub.receive_doodad_use(*actor, opcode, gameplay_request->message,
                                     gameplay_request->sequence, progress_access(),
                                     context_.housing_vitals(),
                                     context_.housing_monotonic_ticks());
  if (used) {
    session_packets = gameplay_packets(std::move(*used));
    return true;
  }
  if (opcode == kInteractionExecuteRequest) {
    return false;
  }

  if (opcode == kInteractionStartRequest) {
    auto toggled =
        hub.receive_part_toggle(*actor, gameplay_request->message, gameplay_request->sequence);
    if (toggled) {
      session_packets = gameplay_packets(std::move(*toggled));
      return true;
    }
  }

  auto posed =
      hub.receive_furniture_pose(*actor, opcode, gameplay_request->message,
                                 gameplay_request->sequence);
  if (posed) {
    session_packets = gameplay_packets(std::move(*posed));
    return true;
  }
  return false;
}

void HousingSession::observe_movement(const WorldPosition &position) {
  if (feature_.hub_ && attached_ && location_) {
    feature_.hub_->observe_movement(context_.housing_character_id(), position);
  }
}

std::optional<HousingPlacedDoodad> HousingSession::placed_doodad(std::uint64_t unit_id,
                                                                 int map_id) const {
  if (!feature_.hub_ || !location_) {
    return std::nullopt;
  }
  return feature_.hub_->placed_doodad(context_.housing_character_id(), unit_id, map_id);
}

std::int64_t HousingSessionContext::housing_monotonic_ticks() const {
  using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
  return std::chrono::duration_cast<Ticks>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

HousingVitals HousingSessionContext::housing_vitals() {
  return {};
}

bool HousingSession::handle_departure(FrameContext &frame) {
  const auto *gameplay_request = frame.request();

  if (gameplay_request && feature_.hub_) {
    const auto opcode = gameplay_request->opcode;
    if ((opcode == kZoneChannelMoveRequest || opcode == kEscapeRequest) && location_) {
      std::optional<WorldPosition> departure_point;
      if (opcode == kZoneChannelMoveRequest) {
        departure_point = reported_departure_point(gameplay_request->message);
      }
      leave(departure_point);
      return true;
    }
  }

  return false;
}

// The point a ZoneChannelMoveReq was sent from, when it lies within five metres of the
// tracked position (the same bound as other zone crossings); nullopt otherwise.
std::optional<WorldPosition>
HousingSession::reported_departure_point(const nlohmann::json &request) const {
  constexpr double kMaximumReportDistanceMm = 5000;

  const auto current = context_.housing_current_position();
  const auto found = request.find("pos");
  if (!current || found == request.end() || !found->is_object()) {
    return std::nullopt;
  }

  WorldPosition reported = *current;
  for (const auto &[field, target] :
       {std::pair{"millimeter_x", &reported.x_mm}, std::pair{"millimeter_y", &reported.y_mm},
        std::pair{"millimeter_z", &reported.z_mm}}) {
    const auto value = found->find(field);
    if (value == found->end()) {
      *target = 0;
      continue;
    }
    if (!value->is_number_integer()) {
      return std::nullopt;
    }
    const auto millimetres = value->get<std::int64_t>();
    if (millimetres < std::numeric_limits<int>::min() ||
        millimetres > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    *target = static_cast<int>(millimetres);
  }

  const double dx = static_cast<double>(reported.x_mm) - current->x_mm;
  const double dy = static_cast<double>(reported.y_mm) - current->y_mm;
  const double dz = static_cast<double>(reported.z_mm) - current->z_mm;
  if (dx * dx + dy * dy + dz * dz > kMaximumReportDistanceMm * kMaximumReportDistanceMm) {
    return std::nullopt;
  }
  return reported;
}

std::optional<std::vector<GameplayPacket>>
HousingSession::dispatch_gm(const WorldGameplayRequest &request) {
  if (feature_.hub_) {
    const auto position = context_.housing_current_position();
    if (const auto actor = authenticated_actor(position)) {
      auto reply = feature_.hub_->receive_gm(*actor, request.opcode, request.message,
                                             request.sequence, progress_access());
      if (reply) {
        for (const auto &line : reply->messages) {
          context_.housing_gm_reply(line);
        }
        return gameplay_packets(std::move(reply->packets));
      }
    }
  }
  return std::nullopt;
}

std::optional<std::vector<GameplayPacket>> HousingSession::complete_entry() {
  if (!pending_entry_) {
    return std::nullopt;
  }
  std::vector<GameplayPacket> packets;

  const auto destination = *pending_entry_;
  pending_entry_.reset();
  const housing::HousingLocation location{destination.zone_key, destination.channel};
  location_ = location;
  presence_map_.store(channel_presence_map(location));
  if (destination.origin) {
    origin_ = destination.origin;
  }
  if (feature_.hub_) {
    auto &hub = *feature_.hub_;
    hub.set_location(context_.housing_character_id(), context_.housing_connection_id(), location);
    auto snapshot = gameplay_packets(
        housing::channel_snapshot(hub.service(), location.zone_key, location.channel));
    packets.insert(packets.end(), std::make_move_iterator(snapshot.begin()),
                   std::make_move_iterator(snapshot.end()));
  }
  return packets;
}

} // namespace chronicles::housing
