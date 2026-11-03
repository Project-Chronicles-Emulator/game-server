#include "world/features/guild/guild_world.hpp"

#include "auth/auth.hpp"
#include "p_c2s.guild.pb.h"
#include "p_s2c.guild.pb.h"
#include "p_system.pb.h"
#include "world/core/unit_registry.hpp"
#include "world/features/npc/npc_population.hpp"

#include <iterator>
#include <stdexcept>
#include <utility>
#include <variant>

namespace chronicles {
namespace {

using guild_world::GuildWorldUpdate;
using guild_world::OutboundPacket;
using guild_world::UnitGuildChange;

nlohmann::json common_error(x5::ec::EErrorCode code, std::uint32_t sequence) {
  x5::pb::CommonErrorRes response;
  response.set_ec(code);
  response.set_category(x5::pb::EErrorCategory_Guild);
  response.mutable_packet_seq()->set_seq(sequence);
  return message_json(response);
}

// WorldCharGuildNoti (p_s2c.character.proto) is not a generated class; proto3 JSON omits
// zero values, so a character without guild history receives an empty snapshot as in the
// official captures.
nlohmann::json world_char_guild_message(const GuildStatus &status) {
  auto message = nlohmann::json::object();
  if (status.guild_id != 0) {
    message["guild_id"] = std::to_string(status.guild_id);
  }
  if (status.last_leave_epoch_ticks != 0) {
    message["last_guild_leave_epochtime"] = std::to_string(status.last_leave_epoch_ticks);
  }
  if (status.last_disband_epoch_ticks != 0) {
    message["last_guild_disband_epochtime"] = std::to_string(status.last_disband_epoch_ticks);
  }
  return message;
}

void translate_event(const GuildMemberJoined &event, GuildWorldUpdate &update) {
  update.unit_changes.push_back({event.member.char_id, event.guild.id, event.guild.name});
}

void translate_event(const GuildMemberLeft &event, GuildWorldUpdate &update) {
  update.unit_changes.push_back({event.char_id, 0, {}});
}

void translate_event(const GuildDisbanded &event, GuildWorldUpdate &update) {
  for (const auto char_id : event.former_members) {
    update.unit_changes.push_back({char_id, 0, {}});
  }
}

void translate_event(const GuildInviteIssued &event, GuildWorldUpdate &update) {
  x5::pb::guild::GuildInvitePopupNoti notification;
  notification.set_guild_id(event.guild_id);
  notification.set_guild_char_id(event.inviter_char_id);
  notification.set_guild_name(event.guild_name);
  update.packets.push_back({event.target_char_id, guild_world::kInvitePopupNotification,
                            message_json(notification)});
}

void translate_event(const GuildInviteRejected &event, GuildWorldUpdate &update) {
  x5::pb::guild::GuildInviteRejectNoti notification;
  notification.set_target_char_name(event.target_name);
  notification.set_reason(static_cast<x5::pb::guild::EGuildInviteRejectReason>(event.reason));
  update.packets.push_back({event.inviter_char_id, guild_world::kInviteRejectNotification,
                            message_json(notification)});
}

// Roster, grade and guild-information changes travel only on the front WebSocket.
void translate_event(const GuildMemberGradeChanged &, GuildWorldUpdate &) {}

void translate_event(const GuildMemberUpdated &, GuildWorldUpdate &) {}

void translate_event(const GuildInfoChanged &, GuildWorldUpdate &) {}

// Guild experience changes travel only on the front WebSocket (GuildExpUpdateNoti).
void translate_event(const GuildExperienceChanged &, GuildWorldUpdate &) {}

} // namespace

void guild_world::GuildWorldUpdate::append(GuildWorldUpdate other) {
  packets.insert(packets.end(), std::make_move_iterator(other.packets.begin()),
                 std::make_move_iterator(other.packets.end()));
  unit_changes.insert(unit_changes.end(), std::make_move_iterator(other.unit_changes.begin()),
                      std::make_move_iterator(other.unit_changes.end()));
}

bool guild_world::GuildWorldUpdate::empty() const {
  return packets.empty() && unit_changes.empty();
}

GuildWorld::GuildWorld(std::shared_ptr<GuildService> guilds) : guilds_(std::move(guilds)) {
  if (!guilds_) {
    throw std::invalid_argument("Guild world integration requires a guild service");
  }
}

bool GuildWorld::handles(std::uint16_t opcode) {
  return opcode == guild_world::kInvitePopupRequest ||
         opcode == guild_world::kAcceptInviteRequest ||
         opcode == guild_world::kRejectInviteRequest;
}

GuildWorldUpdate GuildWorld::attach(const GuildActor &actor, std::uint64_t connection_id) {
  {
    std::lock_guard lock(mutex_);
    online_[actor.char_id] = OnlineCharacter{actor, connection_id};
  }

  guilds_->refresh_member(actor);
  const auto status = guilds_->status(actor.char_id);

  GuildWorldUpdate update;
  update.packets.push_back({actor.char_id, guild_world::kWorldCharGuildNotification,
                            world_char_guild_message(status)});
  if (status.guild_id != 0) {
    // Observers already receive the label in UnitAddNoti. Only the entering client
    // needs its own label here.
    const UnitGuildChange own_label{actor.char_id, status.guild_id, status.guild_name};
    update.packets.push_back({actor.char_id, guild_world::kChangeGuildUnitNotification,
                              change_guild_unit_message(own_label)});
  }
  return update;
}

void GuildWorld::detach(std::int64_t char_id, std::uint64_t connection_id) {
  std::lock_guard lock(mutex_);
  const auto found = online_.find(char_id);
  if (found != online_.end() && found->second.connection_id == connection_id) {
    online_.erase(found);
  }
}

GuildWorldUpdate GuildWorld::receive(std::int64_t requester, std::uint16_t opcode,
                                     const nlohmann::json &message, std::uint32_t sequence) {
  const auto actor = online_actor(requester);
  if (!actor) {
    // A request before StartPlaying has no authoritative directory entry yet.
    GuildWorldUpdate update;
    update.packets.push_back({requester, guild_world::kCommonErrorResponse,
                              common_error(x5::ec::Guild_InvalidChar, sequence)});
    return update;
  }

  switch (opcode) {
  case guild_world::kInvitePopupRequest:
    return invite(*actor, message);
  case guild_world::kAcceptInviteRequest:
    return accept(*actor, message, sequence);
  case guild_world::kRejectInviteRequest:
    return reject(*actor, message);
  default:
    throw std::invalid_argument("Unsupported world guild opcode " + std::to_string(opcode));
  }
}

GuildWorldUpdate GuildWorld::translate(const std::vector<GuildEvent> &events) const {
  GuildWorldUpdate update;
  for (const auto &event : events) {
    std::visit([&update](const auto &typed_event) { translate_event(typed_event, update); },
               event);
  }
  return update;
}

std::optional<std::uint64_t> GuildWorld::connection_of(std::int64_t char_id) const {
  std::lock_guard lock(mutex_);
  const auto found = online_.find(char_id);
  if (found == online_.end()) {
    return std::nullopt;
  }
  return found->second.connection_id;
}

void GuildWorld::decorate_unit_add(nlohmann::json &unit_add_message) const {
  if (!unit_add_message.is_object()) {
    return;
  }

  // UnitAddNoti.unit_id is a uint64, which protobuf JSON encodes as a decimal string.
  const auto unit = unit_add_message.find("unit_id");
  if (unit == unit_add_message.end() || !unit->is_string()) {
    return;
  }
  const auto unit_id = parse_unit_id_text(unit->get_ref<const std::string &>());
  if (!unit_id) {
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

  const auto character_id = character_id_from_wire_unit_id(*unit_id);
  if (!character_id || !is_supported_character_id(*character_id)) {
    return;
  }

  const auto status = guilds_->status(*character_id);
  if (status.guild_id != 0) {
    (*player)["guild_id"] = std::to_string(status.guild_id);
    (*player)["guild_name"] = status.guild_name;
  }
}

nlohmann::json GuildWorld::change_guild_unit_message(const UnitGuildChange &change) {
  // Wire IDs carry the player type nibble; persistence keeps raw character IDs.
  // An empty guild omits its fields (proto3 zero values).
  x5::pb::guild::ChangeGuildUnitNoti notification;
  notification.set_unit_id(player_wire_unit_id(change.char_id));
  if (change.guild_id != 0) {
    notification.set_guild_id(change.guild_id);
    notification.set_guild_name(change.guild_name);
  }
  return message_json(notification);
}

std::optional<GuildActor> GuildWorld::online_actor(std::int64_t char_id) const {
  std::lock_guard lock(mutex_);
  const auto found = online_.find(char_id);
  if (found == online_.end()) {
    return std::nullopt;
  }
  return found->second.actor;
}

GuildWorldUpdate GuildWorld::invite(const GuildActor &requester, const nlohmann::json &message) {
  x5::pb::GuildInvitePopupReq request;
  parse_json(message, request);

  x5::pb::guild::GuildInvitePopupRes response;
  const auto target = online_actor(request.target_char_id());
  if (!target) {
    response.set_ec(x5::ec::Guild_NotExistsInvitee);
  } else {
    const auto result = guilds_->invite(requester, request.guild_id(), *target);
    response.set_ec(result.error);
    response.set_target_char_name(result.target_name);
  }

  GuildWorldUpdate update;
  update.packets.push_back(
      {requester.char_id, guild_world::kInvitePopupResponse, message_json(response)});
  return update;
}

GuildWorldUpdate GuildWorld::accept(const GuildActor &requester, const nlohmann::json &message,
                                    std::uint32_t sequence) {
  x5::pb::GuildAcceptInviteReq request;
  parse_json(message, request);

  // Success has no world response: the join reaches the client as WSF2C_GuildJoinRes and
  // ChangeGuildUnitNoti through the published events.
  GuildWorldUpdate update;
  const auto error =
      guilds_->accept_invite(requester, request.guild_id(), request.guild_char_id());
  if (error != x5::ec::Success) {
    update.packets.push_back(
        {requester.char_id, guild_world::kCommonErrorResponse, common_error(error, sequence)});
  }
  return update;
}

GuildWorldUpdate GuildWorld::reject(const GuildActor &requester, const nlohmann::json &message) {
  x5::pb::GuildRejectInviteReq request;
  parse_json(message, request);

  x5::pb::guild::GuildRejectInviteRes response;
  response.set_ec(
      guilds_->reject_invite(requester, request.guild_id(), request.guild_char_id()));

  GuildWorldUpdate update;
  update.packets.push_back(
      {requester.char_id, guild_world::kRejectInviteResponse, message_json(response)});
  return update;
}

} // namespace chronicles
