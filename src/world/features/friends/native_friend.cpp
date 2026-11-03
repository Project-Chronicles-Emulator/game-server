#include "world/features/friends/native_friend.hpp"

#include "auth/auth.hpp"
#include "cms.generated.pb.h"
#include "world/features/chat/native_chat.hpp"
#include "p_c2f.ws.pb.h"
#include "p_f2c.ws.pb.h"
#include "p_friend.pb.h"
#include "p_system.pb.h"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <variant>

namespace chronicles {
namespace {

using native_ws::parse_payload;

void fill_friend_info(const FriendView &view, x5::pb::FriendInfo &info) {
  info.set_account_id(view.account_id);
  info.set_alias(view.alias);
  info.set_created_epochtime(view.created_epoch_ticks);

  // FriendInfo.status is a oneof: the character marks the friend online, otherwise the client
  // shows the time since last_login_epochtime.
  if (!view.character) {
    info.set_last_login_epochtime(view.last_login_epoch_ticks);
    return;
  }

  const auto &source = *view.character;
  auto &character = *info.mutable_char_();
  character.set_world_id(source.world_id);
  character.set_char_id(source.char_id);
  // Player-kill counts are not tracked by the emulator.
  character.set_pk_kill_count(0);
  character.set_char_name(source.name);
  if (x5::cms::EX5CharacterClass_IsValid(source.char_class)) {
    character.set_char_class(static_cast<x5::cms::EX5CharacterClass>(source.char_class));
  }
  character.set_level(source.level);
  if (!source.guild_name.empty()) {
    character.set_guild_name(source.guild_name);
  }
}

std::string common_error(x5::ec::EErrorCode code) {
  x5::pb::CommonErrorRes response;
  response.set_ec(code);
  return native_ws::encode_frame(native_friend::kCommonErrorResponse, response);
}

std::optional<FriendSearchQuery> search_query(const x5::pb::ws::front::FriendSearchReq &request) {
  using Request = x5::pb::ws::front::FriendSearchReq;

  FriendSearchQuery query;
  switch (request.key_case()) {
  case Request::kCharId:
    query.kind = FriendSearchKind::character_id;
    query.id = request.char_id();
    return query;
  case Request::kAccountId:
    query.kind = FriendSearchKind::account_id;
    query.id = request.account_id();
    return query;
  case Request::kCharName:
    query.kind = FriendSearchKind::character_name;
    query.text = request.char_name();
    return query;
  case Request::kSteamFriendCode:
    query.kind = FriendSearchKind::steam_friend_code;
    query.text = request.steam_friend_code();
    return query;
  case Request::KEY_NOT_SET:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

NativeFriendFront::NativeFriendFront(AuthService &auth, FriendService &friends,
                                     NativeChatService &delivery)
    : auth_(auth), friends_(friends), delivery_(delivery) {}

bool NativeFriendFront::handles(std::uint16_t packet_id) {
  switch (packet_id) {
  case native_friend::kSearchRequest:
  case native_friend::kAddRequest:
  case native_friend::kRemoveRequest:
  case native_friend::kModifyRequest:
  case native_friend::kListRequest:
    return true;
  default:
    return false;
  }
}

std::string NativeFriendFront::handle(const std::string &token, native_ws::FrameView frame) {
  if (frame.packet_id == native_friend::kListRequest) {
    return list(token, frame);
  }

  const auto requester = actor(token);
  switch (frame.packet_id) {
  case native_friend::kSearchRequest:
    return search(requester, frame);
  case native_friend::kAddRequest:
    return add(requester, frame);
  case native_friend::kRemoveRequest:
    return remove(requester, frame);
  case native_friend::kModifyRequest:
    return modify(requester, frame);
  default:
    throw std::invalid_argument("Packet is not a friend request");
  }
}

void NativeFriendFront::deliver(const std::vector<FriendEvent> &events) {
  for (const auto &event : events) {
    if (const auto *updated = std::get_if<FriendUpdated>(&event)) {
      x5::pb::frontwebsocket::FriendUpdateNoti notification;
      fill_friend_info(updated->view, *notification.mutable_friend_());
      // An offline recipient misses the update; its next FriendListReq returns the state.
      delivery_.push_to_character(
          updated->world_id, updated->recipient_char_id,
          native_ws::encode_frame(native_friend::kUpdateNotification, notification));
    } else if (const auto *alarm = std::get_if<FriendAddedAlarm>(&event)) {
      x5::pb::SystemMessageNoti notification;
      notification.set_key(x5::cms::ESystemMessage_SM_FRIEND_ADD_ALARM);
      delivery_.push_to_character(
          alarm->world_id, alarm->recipient_char_id,
          native_ws::encode_frame(native_friend::kSystemMessageNotification, notification));
    }
  }
}

std::optional<FriendActor> NativeFriendFront::actor(const std::string &token) {
  const auto character = auth_.selected_character(token);
  if (!character) {
    return std::nullopt;
  }

  FriendActor requester;
  requester.world_id = character->world_id;
  requester.account_id = character->account_id;
  requester.char_id = character->id;
  return requester;
}

std::string NativeFriendFront::list(const std::string &token, native_ws::FrameView frame) {
  const auto request = parse_payload<x5::pb::ws::front::FriendListReq>(frame.payload);

  // The lobby requests the list before a character is selected. That reply keeps the original
  // empty-list contract (authenticated realm session, recommend preserved).
  const auto requester = actor(token);
  if (!requester) {
    return native_ws::encode_frame(native_friend::kListResponse, auth_.friends(token, request));
  }

  x5::pb::frontwebsocket::FriendListRes response;
  if (request.has_recommend()) {
    response.set_recommend(request.recommend());
  }

  const bool recommended = request.has_recommend() && request.recommend();
  const auto views = recommended ? friends_.recommended(*requester) : friends_.friends(*requester);
  for (const auto &view : views) {
    fill_friend_info(view, *response.add_friends());
  }
  return native_ws::encode_frame(native_friend::kListResponse, response);
}

std::string NativeFriendFront::search(const std::optional<FriendActor> &requester,
                                      native_ws::FrameView frame) {
  const auto request = parse_payload<x5::pb::ws::front::FriendSearchReq>(frame.payload);
  x5::pb::frontwebsocket::FriendSearchRes response;
  // The client matches the reply to its pending search by this sequence.
  response.set_req_seq(request.req_seq());

  const auto query = search_query(request);
  if (!query) {
    response.set_ec(x5::ec::BadRequest);
    return native_ws::encode_frame(native_friend::kSearchResponse, response);
  }
  if (!requester) {
    response.set_ec(x5::ec::InvalidChar);
    return native_ws::encode_frame(native_friend::kSearchResponse, response);
  }

  const auto result = friends_.search(*requester, *query);
  response.set_ec(result.error);
  if (result.error == x5::ec::Success) {
    fill_friend_info(result.found, *response.mutable_friend_());
    if (result.offline_since_epoch_ticks) {
      response.set_last_login_epochtime(*result.offline_since_epoch_ticks);
    }
  }
  return native_ws::encode_frame(native_friend::kSearchResponse, response);
}

std::string NativeFriendFront::add(const std::optional<FriendActor> &requester,
                                   native_ws::FrameView frame) {
  const auto request = parse_payload<x5::pb::ws::front::FriendAddReq>(frame.payload);
  x5::pb::frontwebsocket::FriendAddRes response;
  if (!requester) {
    response.set_ec(x5::ec::InvalidChar);
    return native_ws::encode_frame(native_friend::kAddResponse, response);
  }

  const auto result = friends_.add(*requester, request.account_id());
  response.set_ec(result.error);
  if (result.error == x5::ec::Success) {
    fill_friend_info(result.added, *response.mutable_friend_());
  } else {
    spdlog::info("Friend add refused ec={}", static_cast<int>(result.error));
  }
  return native_ws::encode_frame(native_friend::kAddResponse, response);
}

std::string NativeFriendFront::remove(const std::optional<FriendActor> &requester,
                                      native_ws::FrameView frame) {
  const auto request = parse_payload<x5::pb::ws::front::FriendRemoveReq>(frame.payload);
  if (!requester) {
    return {};
  }

  const bool recommended = request.has_recommend() && request.recommend();
  const auto result = friends_.remove(*requester, request.account_id(), recommended);
  if (result.error != x5::ec::Success) {
    // The entry still exists; a FriendRemoveNoti would make the client drop it locally.
    return common_error(result.error);
  }

  // Removal is idempotent: the notification also clears a stale local entry. The client shows
  // its confirmation only when that entry existed.
  x5::pb::frontwebsocket::FriendRemoveNoti notification;
  notification.set_account_id(request.account_id());
  if (recommended) {
    notification.set_recommend(true);
  }
  return native_ws::encode_frame(native_friend::kRemoveNotification, notification);
}

std::string NativeFriendFront::modify(const std::optional<FriendActor> &requester,
                                      native_ws::FrameView frame) {
  const auto request = parse_payload<x5::pb::ws::front::FriendModifyReq>(frame.payload);
  if (!requester) {
    return common_error(x5::ec::BadRequest);
  }

  const auto result = friends_.set_alias(*requester, request.account_id(), request.alias());
  if (result.error != x5::ec::Success) {
    return common_error(result.error);
  }

  x5::pb::frontwebsocket::FriendUpdateNoti notification;
  fill_friend_info(result.updated, *notification.mutable_friend_());
  return native_ws::encode_frame(native_friend::kUpdateNotification, notification);
}

} // namespace chronicles
