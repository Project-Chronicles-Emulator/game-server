#pragma once

// Front WebSocket (WSC2F/WSF2C) adapter for FriendService.
//
// Every friend request is answered on the requesting socket with the message the client's
// friend manager expects (dumps/ida_research/friends-20261027):
//   FriendSearchReq 20020 -> FriendSearchRes 26 (req_seq echoed)
//   FriendAddReq    20021 -> FriendAddRes 27
//   FriendRemoveReq 20022 -> FriendRemoveNoti 29 (the client has no other removal reply)
//   FriendModifyReq 20023 -> FriendUpdateNoti 30 (no modify response type exists)
//   FriendListReq   20024 -> FriendListRes 28 (recommend presence and value preserved)
// Published presence changes become FriendUpdateNoti and new recommendations become
// SystemMessageNoti 2 with SM_FRIEND_ADD_ALARM, pushed through NativeChatService's
// per-character front queues. The client ignores friend replies outside the world, so a session
// without a selected character receives empty or refused replies instead of a closed socket.

#include "world/features/friends/friends.hpp"
#include "net/native_ws_frames.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

class AuthService;
class NativeChatService;

namespace native_friend {

inline constexpr std::uint16_t kSearchRequest = 20020;
inline constexpr std::uint16_t kAddRequest = 20021;
inline constexpr std::uint16_t kRemoveRequest = 20022;
inline constexpr std::uint16_t kModifyRequest = 20023;
inline constexpr std::uint16_t kListRequest = native_ws::kFriendListRequest;

inline constexpr std::uint16_t kCommonErrorResponse = 1;
inline constexpr std::uint16_t kSystemMessageNotification = 2;
inline constexpr std::uint16_t kSearchResponse = 26;
inline constexpr std::uint16_t kAddResponse = 27;
inline constexpr std::uint16_t kListResponse = native_ws::kFriendListResponse;
inline constexpr std::uint16_t kRemoveNotification = 29;
inline constexpr std::uint16_t kUpdateNotification = 30;

} // namespace native_friend

class NativeFriendFront {
public:
  NativeFriendFront(AuthService &auth, FriendService &friends, NativeChatService &delivery);

  static bool handles(std::uint16_t packet_id);

  // Returns the encoded reply frame, or an empty string when nothing is sent. Malformed payloads
  // throw std::invalid_argument, which closes the socket like any other malformed packet.
  std::string handle(const std::string &token, native_ws::FrameView frame);

  // FriendService subscriber: pushes WSF2C notifications to the recipients' front sockets.
  void deliver(const std::vector<FriendEvent> &events);

private:
  // The session's selected character on this realm. Throws Unauthorized for an invalid realm
  // session; nullopt when no character is selected.
  std::optional<FriendActor> actor(const std::string &token);

  std::string list(const std::string &token, native_ws::FrameView frame);

  std::string search(const std::optional<FriendActor> &requester, native_ws::FrameView frame);

  std::string add(const std::optional<FriendActor> &requester, native_ws::FrameView frame);

  std::string remove(const std::optional<FriendActor> &requester, native_ws::FrameView frame);

  std::string modify(const std::optional<FriendActor> &requester, native_ws::FrameView frame);

  AuthService &auth_;
  FriendService &friends_;
  NativeChatService &delivery_;
};

} // namespace chronicles
