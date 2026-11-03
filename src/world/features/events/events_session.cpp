#include "world/features/events/events_session.hpp"

#include "common/clock_ticks.hpp"
#include "world/core/map_channels.hpp"
#include "world/features/events/preorder_bonus.hpp"
#include "world/features/gm/gm_diagnostics.hpp"

#include <spdlog/spdlog.h>

#include <limits>
#include <utility>

namespace chronicles::events {
namespace {

// Original native-world policies: 32 ranking lists/s, 5 m entry tolerance and 100 m party radius.
constexpr std::int64_t kRankingRequestWindowTicks = kEpochTicksPerSecond;
constexpr unsigned kMaximumRankingRequestsPerWindow = 32;
constexpr double kChannelEntryRangeMm = 5000;
constexpr double kRankingPartyRangeMm = 100000;

} // namespace

EventsSession::EventsSession(EventsFeature &feature, EventsSessionContext &context)
    : feature_(feature), context_(context) {
}

bool EventsSession::handle(FrameContext &frame, raid::RaidUpdate &raid_update) {
  const auto *request = frame.request();
  if (!request) {
    return false;
  }

  const auto routed =
      receive_event_request(*request, frame.party_position, frame.session_packets, raid_update);
  if (routed.moved) {
    frame.invalidate_positions();
  }
  return routed.handled;
}

EventsSession::EventRouting EventsSession::receive_event_request(
    const WorldGameplayRequest &request, const std::optional<WorldPosition> &position,
    std::vector<GameplayPacket> &packets, raid::RaidUpdate &raid_update) {
  EventRouting routing{true, false};
  switch (request.opcode) {
  case preorder_bonus::kStateRequest:
  case preorder_bonus::kClaimRequest:
    packets.push_back(preorder_bonus::respond_without_entitlement(
        request, context_.events_character().has_value()));
    break;
  case ranking::kRankingListRequest:
    packets.push_back(ranking_list(request));
    break;
  case raid::kRaidChannelRegisterRequest:
  case raid::kRaidChannelCancelRequest:
  case raid::kRaidChannelLeaveRequest:
    raid_update.append(receive_raid_request(request));
    break;
  case raid::kRaidChannelMoveRequest:
    routing.moved = receive_raid_move(request, position, packets);
    break;
  case channel_event::kTributeCountRequest:
    packets.push_back(tribute_count(request, position));
    break;
  case channel_event::kItemTributeRequest:
    receive_item_tribute(request, position, packets);
    break;
  case channel_event::kChannelTriggerEntryRequest:
    routing.moved = receive_channel_trigger_entry(request, position, packets);
    break;
  default:
    routing.handled = false;
    break;
  }
  return routing;
}

GameplayPacket EventsSession::ranking_list(const WorldGameplayRequest &request) {
  if (!context_.events_character() || !feature_.ranking_catalog_ || !feature_.ranking_repository_) {
    return {ranking::kRankingListResponse, {{"ec", "Ranking_InvalidCmsId"}}};
  }

  // A client can open every captured board at once, but the advertised refresh time is not
  // a storage rate limit. Bound each connection before opening any database transaction.
  const auto now = context_.events_monotonic_ticks();
  if (now - ranking_window_start_ticks_ >= kRankingRequestWindowTicks) {
    ranking_window_start_ticks_ = now;
    ranking_requests_in_window_ = 0;
  }
  if (ranking_requests_in_window_ >= kMaximumRankingRequestsPerWindow) {
    return channel_event::common_error("BadRequest", request.sequence);
  }
  ++ranking_requests_in_window_;

  return ranking::list_response(*feature_.ranking_catalog_, *feature_.ranking_repository_,
                                context_.events_character()->world_id,
                                context_.events_character()->character_id, request.message,
                                context_.events_epoch_ticks());
}

std::optional<raid::RaidApplicant> EventsSession::raid_applicant() const {
  if (!context_.events_party_identity()) {
    return std::nullopt;
  }
  return raid::RaidApplicant{
      context_.events_party_identity()->char_id, context_.events_connection_id(),
      context_.events_party_identity()->name, context_.events_party_identity()->level,
      context_.events_party_identity()->char_class};
}

raid::RaidUpdate EventsSession::receive_raid_request(const WorldGameplayRequest &request) {
  const auto applicant = raid_applicant();
  if (!feature_.raid_matchmaker_ || !applicant) {
    raid::RaidUpdate refused;
    refused.packets.push_back({context_.events_connection_id(), raid::kCommonErrorResponse,
                               raid::raid_error("Raid_RequesterNotFound", request.sequence)});
    return refused;
  }

  // Private tutorial and housing instances cannot hold a raid seat.
  const bool eligible = !context_.events_has_tutorial() && !context_.events_in_housing();
  return feature_.raid_matchmaker_->receive(*applicant, request.opcode, request.message,
                                            request.sequence, eligible, raid::Clock::now());
}

bool EventsSession::receive_raid_move(const WorldGameplayRequest &request,
                                      const std::optional<WorldPosition> &position,
                                      std::vector<GameplayPacket> &packets) {
  const auto refuse = [&](const std::string &code) {
    packets.push_back({raid::kCommonErrorResponse, raid::raid_error(code, request.sequence)});
    return false;
  };

  if (!feature_.raid_matchmaker_ || !context_.events_party_identity() ||
      !context_.events_character()) {
    return refuse("Raid_RequesterNotFound");
  }
  const auto decision = feature_.raid_matchmaker_->authorize_move(
      context_.events_party_identity()->char_id, request.message, context_.events_connection_id(),
      raid::Clock::now());
  if (decision.error) {
    return refuse(*decision.error);
  }
  if (!position || context_.events_has_tutorial() || context_.events_in_housing() ||
      context_.events_other_action_busy() || context_.events_has_gm_units() ||
      context_.events_player_dead()) {
    return refuse("Move_ChannelMoveFailed");
  }
  if (position->map_id == decision.map->map_id) {
    return false;
  }
  if (!channel_event::reported_position_near(request.message, *position, kChannelEntryRangeMm)) {
    return refuse("InvalidChannelEntryPosition");
  }

  if (position->map_id != decision.map->parent_map_id) {
    return refuse("InvalidChannelEntryPosition");
  }
  const auto origin = *position;
  auto destination = origin;
  destination.map_id = decision.map->map_id;
  {
    // Install the durable return point before the map helper or close() can save a position.
    std::lock_guard lock(context_.events_protocol_mutex());
    raid_origin_ = origin;
  }
  if (!context_.events_begin_map_move(destination, true)) {
    std::lock_guard lock(context_.events_protocol_mutex());
    raid_origin_.reset();
    return refuse("Move_ChannelMoveFailed");
  }

  context_.events_stage_map_destination(destination);
  spdlog::info("Raid channel entry connection={} from_map={} to_map={}",
               context_.events_connection_id(), origin.map_id, destination.map_id);
  return true;
}

void EventsSession::on_raid_eviction(int raid_map_id) {
  if (!context_.events_character() || context_.events_closed() || !feature_.raid_matchmaker_) {
    return;
  }

  const auto complete_eviction = [&] {
    feature_.raid_matchmaker_->complete_eviction(context_.events_character()->character_id,
                                                 context_.events_connection_id(), raid_map_id);
  };
  if (!raid_origin_) {
    // A matched member can leave without ever entering the raid map.
    complete_eviction();
    return;
  }

  std::optional<WorldPosition> position;
  {
    std::lock_guard lock(context_.events_protocol_mutex());
    position = context_.events_position_locked();
  }
  const bool inside = position && position->map_id == raid_map_id;
  const bool entering = context_.events_pending_map_destination() &&
                        context_.events_pending_map_destination()->map_id == raid_map_id;
  if (!inside && !entering) {
    {
      std::lock_guard lock(context_.events_protocol_mutex());
      raid_origin_.reset();
    }
    complete_eviction();
    return;
  }

  // A move still loading cannot be replaced; the arrival check retries after it completes.
  const auto origin = *raid_origin_;
  if (!context_.events_begin_map_move(origin, true)) {
    return;
  }
  context_.events_stage_map_destination(origin);
  {
    std::lock_guard lock(context_.events_protocol_mutex());
    raid_origin_.reset();
  }
  complete_eviction();
  spdlog::info("Raid channel exit connection={} map={}", context_.events_connection_id(),
               raid_map_id);
}

void EventsSession::on_map_load_complete() {
  if (!raid_origin_ || !context_.events_party_identity() || !feature_.raid_matchmaker_) {
    return;
  }

  std::optional<int> map_id;
  {
    std::lock_guard lock(context_.events_protocol_mutex());
    const auto position = context_.events_position_locked();
    if (position) {
      map_id = position->map_id;
    }
  }
  if (!map_id || !feature_.raid_matchmaker_->catalog().find(*map_id)) {
    return;
  }

  const auto membership =
      feature_.raid_matchmaker_->membership(context_.events_party_identity()->char_id);
  if (membership.state != raid::RaidState::matched || membership.map_id != *map_id) {
    on_raid_eviction(*map_id);
  }
}

WorldPosition EventsSession::persisted_position(const WorldPosition &position) const {
  if (raid_origin_ && feature_.raid_matchmaker_ &&
      feature_.raid_matchmaker_->catalog().find(position.map_id)) {
    return *raid_origin_;
  }
  return position;
}

GameplayPacket EventsSession::tribute_count(const WorldGameplayRequest &request,
                                            const std::optional<WorldPosition> &position) const {
  const auto *altar =
      feature_.channel_event_data_ && position
          ? feature_.channel_event_data_->nearest_altar(*position, channel_event::kTributeRangeMm)
          : nullptr;
  if (!altar) {
    return channel_event::common_error("Tribute_InvalidTarget", request.sequence);
  }
  return channel_event::tribute_count_packet(
      feature_.channel_events_.tribute_progress(altar->map_id, altar->unit_id));
}

void EventsSession::receive_item_tribute(const WorldGameplayRequest &request,
                                         const std::optional<WorldPosition> &position,
                                         std::vector<GameplayPacket> &packets) {
  if (!feature_.channel_event_data_ || !position || !context_.events_character()) {
    packets.push_back(channel_event::common_error("Tribute_InvalidTarget", request.sequence));
    return;
  }

  const auto before = context_.events_world_progress();
  const auto plan = channel_event::plan_tribute(*feature_.channel_event_data_, request.message,
                                                *position, before);
  if (plan.error) {
    packets.push_back(channel_event::common_error(*plan.error, request.sequence));
    return;
  }

  const auto map_id = plan.altar->map_id;
  const auto altar_id = plan.altar->unit_id;
  const auto after = channel_event::consume_tribute(before, plan);
  channel_event::TributeCommit committed;
  try {
    committed = feature_.channel_events_.commit_tribute(map_id, altar_id, plan.count, [&] {
      return context_.events_save_world_progress(before, after);
    });
  } catch (const std::exception &error) {
    spdlog::error("Tribute commit failed connection={}: {}", context_.events_connection_id(),
                  error.what());
  }
  if (committed.status != channel_event::TributeCommitStatus::committed) {
    const char *code = committed.status == channel_event::TributeCommitStatus::limit_reached
                           ? "Tribute_LimitReached"
                           : "BadRequest";
    packets.push_back(channel_event::common_error(code, request.sequence));
    return;
  }
  if (context_.events_has_tutorial()) {
    context_.events_adopt_committed_progress(after);
  }

  const auto progress = committed.progress;
  std::vector<GameplayPacket> shared{channel_event::tribute_count_packet(progress)};
  if (committed.completed_event) {
    shared.push_back(channel_event::status_packet(channel_event::EventStatus::success));
  }

  packets.push_back(
      channel_event::tribute_items_packet(plan, after, request.message, request.sequence));
  packets.insert(packets.end(), shared.begin(), shared.end());
  context_.events_deliver_observers(shared);
  spdlog::info("Tribute connection={} map={} altar={} count={} progress={}/{}",
               context_.events_connection_id(), map_id, altar_id, plan.count, progress.count,
               progress.target);
}

bool EventsSession::receive_channel_trigger_entry(const WorldGameplayRequest &request,
                                                  const std::optional<WorldPosition> &position,
                                                  std::vector<GameplayPacket> &packets) {
  const auto refuse = [&](const char *code) {
    packets.push_back(channel_event::common_error(code, request.sequence));
    return false;
  };

  const auto requested = request.message.find("map_id");
  const bool valid_map_id = requested != request.message.end() && requested->is_number_integer() &&
                            requested->get<std::int64_t>() > 0 &&
                            requested->get<std::int64_t>() <= std::numeric_limits<int>::max();
  if (!position || !context_.events_map_channels() || !valid_map_id ||
      !context_.events_map_channels()->supports_map(requested->get<int>())) {
    return refuse("InvalidChannel");
  }

  const auto destination = context_.events_map_channels()->resolve(*position, request.message);
  if (!destination || destination->map_id != requested->get<int>()) {
    return refuse("InvalidChannelEntryPosition");
  }
  if (!context_.events_zone_channel_move(request, position)) {
    return refuse("Move_ChannelMoveFailed");
  }
  return true;
}

std::vector<RankingMember>
EventsSession::ranking_participants(const WorldPosition &position) const {
  std::vector<RankingMember> members;
  if (!context_.events_party_identity()) {
    return members;
  }

  members.push_back(
      {context_.events_party_identity()->char_id, context_.events_party_identity()->name,
       context_.events_party_identity()->level, context_.events_party_identity()->char_class});
  const auto nearby = feature_.party_world_->nearby_party_members(
      context_.events_party_identity()->char_id, position,
      context_.events_party_identity()->channel_key, kRankingPartyRangeMm);
  for (const auto &member : nearby) {
    const auto presence = feature_.party_world_->online_presence(member.char_id);
    if (presence) {
      members.push_back({presence->char_id, presence->name, presence->level, presence->char_class});
    }
  }
  return members;
}

std::vector<GameplayPacket>
EventsSession::execute_gm_command(const event_gm::EventCommandParse &parse) {
  if (!parse.command) {
    context_.events_gm_reply("Usage: " + parse.error);
    return {};
  }

  const auto &command = *parse.command;
  std::optional<WorldPosition> position;
  {
    std::lock_guard lock(context_.events_protocol_mutex());
    position = context_.events_position_locked();
  }
  if (!position || !context_.events_character() || !context_.events_party_identity()) {
    context_.events_gm_reply("Enter the world first.");
    return {};
  }

  switch (command.kind) {
  case event_gm::EventCommandKind::ranking_start:
  case event_gm::EventCommandKind::ranking_finish:
  case event_gm::EventCommandKind::ranking_cancel:
    return execute_ranking_gm_command(command, *position);
  case event_gm::EventCommandKind::raid_channel_enter:
    return execute_raid_gm_command(command);
  case event_gm::EventCommandKind::complete_channel_event_objective:
  case event_gm::EventCommandKind::reset_channel_event_save_data:
    return execute_channel_event_gm_command(command, *position);
  }
  return {};
}

std::vector<GameplayPacket>
EventsSession::execute_ranking_gm_command(const event_gm::EventCommand &command,
                                          const WorldPosition &position) {
  if (!feature_.ranking_catalog_ || !feature_.ranking_repository_) {
    context_.events_gm_reply("Rankings are unavailable (no Ranking catalog or store).");
    return {};
  }

  const auto &catalog = *feature_.ranking_catalog_;
  if (command.kind == event_gm::EventCommandKind::ranking_cancel) {
    context_.events_gm_reply(ranking_runs_.cancel() ? "Ranking attempt cancelled."
                                                    : "No ranking attempt.");
    return {};
  }
  if (command.kind == event_gm::EventCommandKind::ranking_start) {
    auto packet = ranking_runs_.start(catalog, command.target_id, context_.events_monotonic_ticks(),
                                      context_.events_epoch_ticks());
    context_.events_gm_reply(
        packet.message.contains("ec")
            ? "Ranking " + std::to_string(command.target_id) + " is not in the catalog."
            : "Ranking " + std::to_string(command.target_id) + " attempt started.");
    return {std::move(packet)};
  }

  auto finished = ranking_runs_.finish(
      catalog, *feature_.ranking_repository_, context_.events_character()->world_id,
      command.target_id, ranking_participants(position), context_.events_monotonic_ticks(),
      context_.events_epoch_ticks());
  const auto &message = finished.packet.message;
  if (message.contains("ec")) {
    context_.events_gm_reply(
        "Ranking finish did not complete: " + message.at("ec").get<std::string>() +
        ". Check the board; a storage error can follow a committed record.");
  } else {
    context_.events_gm_reply("Ranking " + std::to_string(command.target_id) + " time " +
                             message.at("time_record_milli").get<std::string>() + " ms" +
                             (finished.recorded ? " recorded." : " (not a new best)."));
  }
  return {std::move(finished.packet)};
}

std::vector<GameplayPacket>
EventsSession::execute_raid_gm_command(const event_gm::EventCommand &command) {
  const auto applicant = raid_applicant();
  if (!feature_.raid_matchmaker_ || !applicant) {
    context_.events_gm_reply("Raid channels are unavailable (no RaidUI catalog).");
    return {};
  }

  auto update =
      feature_.raid_matchmaker_->force_match(*applicant, command.target_id, raid::Clock::now());
  feature_.schedule_raid_tick();

  // A one-member channel addresses only this client.
  std::vector<GameplayPacket> packets;
  for (auto &packet : update.packets) {
    packets.push_back({packet.opcode, std::move(packet.message)});
  }
  context_.events_gm_reply("raid_channel_enter " + std::to_string(command.target_id) +
                           (packets.empty() || packets.front().opcode == raid::kCommonErrorResponse
                                ? " refused."
                                : " matched; entry is permitted after the countdown."));
  return packets;
}

std::string EventsSession::channel_event_summary(const WorldPosition &position) const {
  const auto &board = feature_.channel_events_;
  const int map_id = position.map_id;
  const auto *binding =
      feature_.channel_event_data_ ? feature_.channel_event_data_->binding(map_id) : nullptr;
  std::string summary = "Map " + std::to_string(map_id) + ": channel event " +
                        (binding ? std::to_string(binding->channel_event_id) : "none") +
                        ", status " + channel_event::status_name(board.status(map_id));

  const auto *altar =
      feature_.channel_event_data_
          ? feature_.channel_event_data_->nearest_altar(position, channel_event::kTributeRangeMm)
          : nullptr;
  if (altar) {
    const auto progress = board.tribute_progress(map_id, altar->unit_id);
    summary +=
        ", tribute " + std::to_string(progress.count) + "/" + std::to_string(progress.target);
  }
  return summary;
}

std::vector<GameplayPacket>
EventsSession::execute_channel_event_gm_command(const event_gm::EventCommand &command,
                                                const WorldPosition &position) {
  auto &board = feature_.channel_events_;
  const int map_id = position.map_id;
  channel_event::EventStatus status = channel_event::EventStatus::success;
  if (command.kind == event_gm::EventCommandKind::reset_channel_event_save_data) {
    board.reset(map_id);
    status = channel_event::EventStatus::none;
  } else {
    board.set_status(map_id, status);
  }

  const std::vector<GameplayPacket> shared{channel_event::status_packet(status)};
  context_.events_deliver_observers(shared);
  context_.events_gm_reply(std::string(event_gm::command_name(command.kind)) + " applied on map " +
                           std::to_string(map_id) + ".");
  return shared;
}

std::vector<GameplayPacket>
EventsSession::channel_event_info(const std::optional<WorldPosition> &position) {
  if (!position) {
    context_.events_gm_reply("Enter the world first.");
    return {};
  }
  context_.events_gm_reply(channel_event_summary(*position));

  // The channel event board (src/world/features/events/channel_event.hpp) supplies the map's
  // event and completion. Objectives and saved progress are server-only data the emulator does
  // not model.
  auto message = gm::channel_event_info_message(position->map_id);
  const auto *binding = feature_.channel_event_data_
                            ? feature_.channel_event_data_->binding(position->map_id)
                            : nullptr;
  if (binding) {
    message["event_cms_id"] = binding->channel_event_id;
  }
  message["completed"] =
      feature_.channel_events_.status(position->map_id) == channel_event::EventStatus::success;
  return {{gm::kDebugChannelEventInfoNotification, std::move(message)}};
}

bool EventsSession::authorize_server_map_move(const WorldPosition &destination) const {
  if (feature_.is_raid_map(destination.map_id)) {
    const auto character = context_.events_character();
    const auto decision = feature_.raid_matchmaker_->authorize_move(
        character ? character->character_id : 0, {{"map_id", destination.map_id}},
        context_.events_connection_id(), raid::Clock::now());
    // Retain both the seat check and the return-point check before any map-side effects.
    if (decision.error || !raid_origin_) {
      spdlog::warn("Map move into raid refused connection={} map={}: no authorized raid entry",
                   context_.events_connection_id(), destination.map_id);
      return false;
    }
  }
  return true;
}

void EventsSession::on_map_departure(const WorldPosition &destination) {
  if (raid_origin_ && feature_.raid_matchmaker_ &&
      !feature_.raid_matchmaker_->catalog().find(destination.map_id)) {
    // This runs after old-map presence removal and position persistence, before the I/O post.
    feature_.deliver_raid_update(feature_.raid_matchmaker_->detach(
        context_.events_character()->character_id, context_.events_connection_id()));
    std::lock_guard lock(context_.events_protocol_mutex());
    raid_origin_.reset();
  }
}

void EventsSession::on_leave_world() {
  // The composition posts this after queued frames on the same existing worker strand.
  // Retain the caller's original has-service/character guard on the I/O close boundary.
  try {
    feature_.deliver_raid_update(feature_.raid_matchmaker_->detach(
        context_.events_character()->character_id, context_.events_connection_id()));
  } catch (const std::exception &error) {
    spdlog::error("Raid channel detach failed connection={}: {}", context_.events_connection_id(),
                  error.what());
  }
}

std::optional<WorldPosition>
EventsSession::housing_return_origin(const std::optional<WorldPosition> &fallback) const {
  return raid_origin_ ? raid_origin_ : fallback;
}

} // namespace chronicles::events
