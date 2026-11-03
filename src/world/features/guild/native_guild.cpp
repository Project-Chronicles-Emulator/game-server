#include "world/features/guild/native_guild.hpp"

#include "auth/auth.hpp"
#include "world/features/company/company.hpp"
#include "world/features/chat/native_chat.hpp"
#include "p_c2f.ws.guild.pb.h"
#include "p_f2c.ws.guild.pb.h"

#include <stdexcept>
#include <variant>

namespace chronicles {
namespace {

using native_ws::parse_payload;
namespace wire = x5::pb::guild;

void fill_guild_info(const GuildRecord &guild, wire::GuildInfo &info) {
  info.set_guild_id(guild.id);
  info.set_name(guild.name);
  info.set_desc(guild.description);
  info.set_join_type(static_cast<x5::cms::EX5GuildJoinType>(guild.join_type));
  // Guilds accept members of their founder's company (Candidate); 0 for older guilds.
  info.set_join_company_type(static_cast<x5::cms::EX5CompanyType>(guild.join_company_type));
  info.set_join_level(guild.join_level);
  info.set_join_company_position(guild.join_company_position);
  info.set_exp(guild.experience);
  info.set_lv(guild.level);
  info.set_notice(guild.notice);
  info.set_notice_char_id(guild.notice_char_id);
  info.set_notice_created_at(guild.notice_epoch_ticks);
  info.set_guild_created_at(guild.created_epoch_ticks);
}

void fill_member(const GuildMemberRecord &record, wire::GuildMember &member) {
  member.set_char_id(record.char_id);
  member.set_member_grade(static_cast<x5::cms::EX5GuildMemberGrade>(record.grade));
  member.set_name(record.name);
  member.set_level(record.level);
  member.set_contribution(record.contribution);
  if (x5::cms::EX5CharacterClass_IsValid(record.char_class)) {
    member.set_char_class(static_cast<x5::cms::EX5CharacterClass>(record.char_class));
  }
  member.set_last_connect_epochtime(record.last_connect_epoch_ticks);
}

void fill_members(const std::vector<GuildMemberRecord> &records,
                  google::protobuf::RepeatedPtrField<wire::GuildMember> &members) {
  for (const auto &record : records) {
    fill_member(record, *members.Add());
  }
}

// The housing area of a guild with a contracted area; nullopt otherwise.
std::optional<wire::GuildHousingArea> housing_area(const GuildHousingRequests *housing,
                                                   std::int64_t guild_id) {
  if (!housing) {
    return std::nullopt;
  }
  return housing->housing_area(guild_id);
}

void fill_search_detail(const GuildSummary &summary, wire::GuildSearchDetail &detail) {
  const auto &guild = summary.guild;
  detail.set_guild_id(guild.id);
  detail.set_name(guild.name);
  detail.set_master_name(summary.master_name);
  detail.set_desc(guild.description);
  detail.set_member_count(static_cast<std::int32_t>(summary.member_count));
  detail.set_exp(guild.experience);
  detail.set_lv(guild.level);
  detail.set_join_level(guild.join_level);
  detail.set_join_company_position(guild.join_company_position);
  detail.set_join_type(static_cast<x5::cms::EX5GuildJoinType>(guild.join_type));
}

std::string joinable_list(GuildService &guilds, const std::optional<GuildActor> &actor,
                          native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildJoinableListReq>(frame.payload);
  wire::GuildJoinableListRes response;
  if (!actor) {
    response.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(native_guild::kJoinableListResponse, response);
  }

  const auto result = guilds.joinable(*actor, request.start_idx());
  response.set_ec(result.error);
  for (const auto &summary : result.guilds) {
    fill_search_detail(summary, *response.add_guilds());
  }
  if (result.next_start_index > 0) {
    response.set_next_start_idx(result.next_start_index);
  }
  return native_ws::encode_frame(native_guild::kJoinableListResponse, response);
}

std::string search(GuildService &guilds, const std::optional<GuildActor> &actor,
                   native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildSearchReq>(frame.payload);
  wire::GuildSearchRes response;
  if (!actor) {
    response.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(native_guild::kSearchResponse, response);
  }

  const auto result = guilds.search(*actor, request.name());
  response.set_ec(result.error);
  if (result.guild) {
    fill_search_detail(*result.guild, *response.mutable_guild());
  }
  return native_ws::encode_frame(native_guild::kSearchResponse, response);
}

std::string guild_detail(GuildService &guilds, const GuildHousingRequests *housing,
                         const std::optional<GuildActor> &actor, native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildReq>(frame.payload);
  wire::GuildRes response;
  if (!actor) {
    response.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(native_guild::kGuildResponse, response);
  }

  const auto result = guilds.detail(*actor, request.guild_id());
  response.set_ec(result.error);
  if (result.error == x5::ec::Success) {
    fill_guild_info(result.guild, *response.mutable_guild());
    fill_members(result.members, *response.mutable_members());
    if (const auto area = housing_area(housing, result.guild.id)) {
      *response.mutable_housing() = *area;
    }
  }
  return native_ws::encode_frame(native_guild::kGuildResponse, response);
}

std::string create(GuildService &guilds, const std::optional<GuildActor> &actor,
                   native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildCreateReq>(frame.payload);
  wire::GuildCreateRes response;
  if (!actor) {
    response.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(native_guild::kCreateResponse, response);
  }

  GuildCreateRequest create_request;
  create_request.name = request.name();
  create_request.description = request.desc();
  create_request.join_type = static_cast<int>(request.join_type());
  create_request.join_level = request.join_level();
  create_request.join_company_position = request.join_company_position();

  const auto result = guilds.create(*actor, create_request);
  response.set_ec(result.error);
  if (result.error == x5::ec::Success) {
    fill_guild_info(result.guild, *response.mutable_guild());
    fill_member(result.owner, *response.mutable_owner());
  }
  return native_ws::encode_frame(native_guild::kCreateResponse, response);
}

std::string manage(GuildService &guilds, const std::optional<GuildActor> &actor,
                   native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildManageReq>(frame.payload);
  wire::GuildManageRes response;
  if (!actor) {
    response.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(native_guild::kManageResponse, response);
  }

  GuildManageRequest manage_request;
  manage_request.guild_id = request.guild_id();
  manage_request.description = request.desc();
  manage_request.notice = request.notice();
  manage_request.join_type = static_cast<int>(request.join_type());
  manage_request.join_level = request.join_char_level();
  manage_request.join_company_position = request.join_company_position();

  const auto result = guilds.manage(*actor, manage_request);
  response.set_ec(result.error);
  if (result.error == x5::ec::Success) {
    fill_guild_info(result.guild, *response.mutable_guild());
  }
  return native_ws::encode_frame(native_guild::kManageResponse, response);
}

std::string manage_member(GuildService &guilds, const std::optional<GuildActor> &actor,
                          native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildMemberManageReq>(frame.payload);
  wire::GuildMemberManageRes response;
  response.set_char_id(request.char_id());
  response.set_grade(request.grade());
  if (!actor) {
    response.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(native_guild::kMemberManageResponse, response);
  }

  const auto result = guilds.manage_member(*actor, request.guild_id(), request.char_id(),
                                           static_cast<int>(request.grade()));
  response.set_ec(result.error);
  return native_ws::encode_frame(native_guild::kMemberManageResponse, response);
}

std::string join(GuildService &guilds, const GuildHousingRequests *housing,
                 const std::optional<GuildActor> &actor, native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildJoinReq>(frame.payload);
  wire::GuildJoinRes response;
  if (!actor) {
    response.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(native_guild::kJoinResponse, response);
  }

  const auto result = guilds.join(*actor, request.guild_id());
  response.set_ec(result.error);
  if (result.error == x5::ec::Success) {
    fill_guild_info(result.guild, *response.mutable_guild());
    fill_member(result.member, *response.mutable_member());
    fill_members(result.members, *response.mutable_members());
    if (const auto area = housing_area(housing, result.guild.id)) {
      *response.mutable_housing() = *area;
    }
  }
  return native_ws::encode_frame(native_guild::kJoinResponse, response);
}

std::string leave(GuildService &guilds, const std::optional<GuildActor> &actor,
                  native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildLeaveReq>(frame.payload);
  wire::GuildLeaveRes response;
  if (!actor) {
    response.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(native_guild::kLeaveResponse, response);
  }

  const auto result = guilds.leave(*actor, request.guild_id());
  response.set_ec(result.error);
  if (result.error == x5::ec::Success) {
    response.set_last_guild_leave_epochtime(result.left_epoch_ticks);
  }
  return native_ws::encode_frame(native_guild::kLeaveResponse, response);
}

std::string disband(GuildService &guilds, const std::optional<GuildActor> &actor,
                    native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildDisbandReq>(frame.payload);
  wire::GuildDisbandRes response;
  if (!actor) {
    response.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(native_guild::kDisbandResponse, response);
  }

  const auto result = guilds.disband(*actor, request.guild_id());
  response.set_ec(result.error);
  if (result.error == x5::ec::Success) {
    response.set_last_guild_disband_epochtime(result.disbanded_epoch_ticks);
  }
  return native_ws::encode_frame(native_guild::kDisbandResponse, response);
}

} // namespace

namespace guild_housing {

bool is_request(std::uint16_t packet_id) {
  return packet_id >= kAreaContractRequest && packet_id <= kAreaContractExtensionRequest;
}

std::uint16_t response_id(std::uint16_t request_id) {
  switch (request_id) {
  case kAreaContractRequest:
    return kAreaContractResponse;
  case kSiteAllowMemberRequest:
    return kSiteAllowMemberResponse;
  case kSiteAreaBoardGetRequest:
    return kSiteAreaBoardGetResponse;
  case kSiteAssignmentRequest:
    return kSiteAssignmentResponse;
  case kAreaContractExtensionRequest:
    return kAreaContractExtensionResponse;
  default:
    throw std::invalid_argument("Not a guild housing request ID: " + std::to_string(request_id));
  }
}

} // namespace guild_housing

NativeGuildFront::NativeGuildFront(AuthService &auth, GuildService &guilds,
                                   NativeChatService &delivery,
                                   std::shared_ptr<const CompanyService> companies,
                                   std::shared_ptr<GuildHousingRequests> housing)
    : auth_(auth), guilds_(guilds), delivery_(delivery), companies_(std::move(companies)),
      housing_(std::move(housing)) {}

bool NativeGuildFront::handles(std::uint16_t packet_id) const {
  if (housing_ && guild_housing::is_request(packet_id)) {
    return true;
  }
  return packet_id >= native_guild::kJoinableListRequest &&
         packet_id <= native_guild::kDisbandRequest;
}

std::string NativeGuildFront::handle(const std::string &token, native_ws::FrameView frame) {
  if (housing_ && guild_housing::is_request(frame.packet_id)) {
    return handle_housing(token, frame);
  }

  const auto requester = actor(token);
  switch (frame.packet_id) {
  case native_guild::kJoinableListRequest:
    return joinable_list(guilds_, requester, frame);
  case native_guild::kSearchRequest:
    return search(guilds_, requester, frame);
  case native_guild::kGuildRequest:
    return guild_detail(guilds_, housing_.get(), requester, frame);
  case native_guild::kCreateRequest:
    return create(guilds_, requester, frame);
  case native_guild::kManageRequest:
    return manage(guilds_, requester, frame);
  case native_guild::kMemberManageRequest:
    return manage_member(guilds_, requester, frame);
  case native_guild::kJoinRequest:
    return join(guilds_, housing_.get(), requester, frame);
  case native_guild::kLeaveRequest:
    return leave(guilds_, requester, frame);
  case native_guild::kDisbandRequest:
    return disband(guilds_, requester, frame);
  default:
    throw std::invalid_argument("Unsupported native guild packet ID " +
                                std::to_string(frame.packet_id));
  }
}

void NativeGuildFront::deliver(const std::vector<GuildEvent> &events) {
  for (const auto &event : events) {
    std::visit([this](const auto &typed_event) { deliver_event(typed_event); }, event);
  }
}

std::optional<GuildActor> NativeGuildFront::actor(const std::string &token) {
  const auto character = auth_.selected_character(token);
  if (!character) {
    return std::nullopt;
  }

  GuildActor actor;
  actor.world_id = character->world_id;
  actor.char_id = character->id;
  actor.name = character->name;
  actor.level = character->level;
  actor.char_class = character->class_id;
  actor.account_id = character->account_id;
  if (companies_) {
    const auto company = companies_->status(character->id);
    actor.company_type = company.record.company_type;
    actor.company_position = company.position_level;
  }
  return actor;
}

std::string NativeGuildFront::handle_housing(const std::string &token,
                                             native_ws::FrameView frame) {
  const auto character = auth_.selected_character(token);
  if (!character) {
    // Every guild housing response begins with field 1 `ec`, so one message type encodes the
    // refusal for each response ID.
    wire::GuildHousingSiteAreaBoardGetRes refused;
    refused.set_ec(x5::ec::Guild_InvalidChar);
    return native_ws::encode_frame(guild_housing::response_id(frame.packet_id), refused);
  }

  GuildHousingRequester requester;
  requester.account_id = character->account_id;
  requester.char_id = character->id;
  requester.world_id = character->world_id;
  requester.name = character->name;
  return housing_->handle(requester, frame);
}

void NativeGuildFront::push_frame(std::int64_t char_id, const std::string &frame) {
  delivery_.push_to_character(guilds_.world_id(), char_id, frame);
}

void NativeGuildFront::push_all(const std::vector<std::int64_t> &char_ids,
                                const std::string &frame) {
  for (const auto char_id : char_ids) {
    push_frame(char_id, frame);
  }
}

void NativeGuildFront::deliver_event(const GuildMemberJoined &event) {
  wire::GuildJoinNoti notification;
  notification.set_guild_id(event.guild.id);
  fill_member(event.member, *notification.mutable_member());
  fill_members(event.members, *notification.mutable_members());
  push_all(event.recipients,
           native_ws::encode_frame(native_guild::kJoinNotification, notification));

  // A world invitation has no front request, so the joiner receives the join response
  // that a GuildJoinReq would have produced (Candidate).
  if (event.via_invitation) {
    wire::GuildJoinRes response;
    response.set_ec(x5::ec::Success);
    fill_guild_info(event.guild, *response.mutable_guild());
    fill_member(event.member, *response.mutable_member());
    fill_members(event.members, *response.mutable_members());
    push_frame(event.member.char_id,
               native_ws::encode_frame(native_guild::kJoinResponse, response));
  }
}

void NativeGuildFront::deliver_event(const GuildMemberLeft &event) {
  if (event.kicked) {
    wire::GuildKickNoti kick;
    kick.set_guild_id(event.guild_id);
    kick.set_char_id(event.char_id);
    kick.set_last_guild_leave_epochtime(event.left_epoch_ticks);
    push_frame(event.char_id, native_ws::encode_frame(native_guild::kKickNotification, kick));
  }

  wire::GuildLeaveNoti notification;
  notification.set_guild_id(event.guild_id);
  notification.set_char_id(event.char_id);
  notification.set_kick(event.kicked);
  push_all(event.recipients,
           native_ws::encode_frame(native_guild::kLeaveNotification, notification));
}

void NativeGuildFront::deliver_event(const GuildMemberGradeChanged &event) {
  wire::GuildMemberManageUpdateNoti notification;
  notification.set_guild_id(event.guild_id);
  notification.set_char_id(event.char_id);
  notification.set_grade(static_cast<x5::cms::EX5GuildMemberGrade>(event.grade));
  push_all(event.recipients,
           native_ws::encode_frame(native_guild::kMemberManageUpdateNotification, notification));
}

void NativeGuildFront::deliver_event(const GuildMemberUpdated &event) {
  wire::GuildMemberUpdateNoti notification;
  notification.set_guild_id(event.member.guild_id);
  fill_member(event.member, *notification.mutable_member());
  push_all(event.recipients,
           native_ws::encode_frame(native_guild::kMemberUpdateNotification, notification));
}

void NativeGuildFront::deliver_event(const GuildInfoChanged &event) {
  wire::GuildManageUpdateNoti notification;
  notification.set_guild_id(event.guild.id);
  fill_guild_info(event.guild, *notification.mutable_guild());
  push_all(event.recipients,
           native_ws::encode_frame(native_guild::kManageUpdateNotification, notification));
}

void NativeGuildFront::deliver_event(const GuildDisbanded &event) {
  wire::GuildDisbandNoti notification;
  notification.set_guild_id(event.guild_id);
  push_all(event.recipients,
           native_ws::encode_frame(native_guild::kDisbandNotification, notification));
}

void NativeGuildFront::deliver_event(const GuildInviteIssued &) {
  // Invitations travel on the world transport (PS2C_GuildInvitePopupNoti).
}

void NativeGuildFront::deliver_event(const GuildExperienceChanged &event) {
  wire::GuildExpUpdateNoti notification;
  notification.set_guild_id(event.guild_id);
  notification.set_guild_exp(event.experience);
  notification.set_guild_lv(event.level);
  push_all(event.recipients,
           native_ws::encode_frame(native_guild::kExpUpdateNotification, notification));
}

void NativeGuildFront::deliver_event(const GuildInviteRejected &) {
  // Rejections travel on the world transport (PS2C_GuildInviteRejectNoti).
}

} // namespace chronicles
