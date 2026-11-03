#pragma once

// Front WebSocket (WSC2F/WSF2C) adapter for GuildService.
//
// Requests are answered with their recovered response packet; refusals keep the response
// type and carry the Guild_* error code. Committed changes are pushed to the other members'
// front sockets through NativeChatService's per-character queues.
//
// Evidence: packet IDs from protocol/native-ws-opcodes.json (wrapper vtable ID getters) and
// the client send helpers AAC_Guild_WSC2F_*_Send (dumps/ida_research/guild-20261026).
// No official guild traffic was captured, so the notification choice per change is the
// emulator's Candidate mapping of the recovered message names.

#include "world/features/guild/guild.hpp"
#include "net/native_ws_frames.hpp"
#include "p_guild.pb.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

class AuthService;
class CompanyService;
class NativeChatService;

namespace native_guild {

inline constexpr std::uint16_t kJoinableListRequest = 20025;
inline constexpr std::uint16_t kSearchRequest = 20026;
inline constexpr std::uint16_t kGuildRequest = 20027;
inline constexpr std::uint16_t kCreateRequest = 20028;
inline constexpr std::uint16_t kManageRequest = 20029;
inline constexpr std::uint16_t kMemberManageRequest = 20030;
inline constexpr std::uint16_t kJoinRequest = 20031;
inline constexpr std::uint16_t kLeaveRequest = 20032;
inline constexpr std::uint16_t kDisbandRequest = 20033;

inline constexpr std::uint16_t kJoinableListResponse = 31;
inline constexpr std::uint16_t kSearchResponse = 32;
inline constexpr std::uint16_t kGuildResponse = 33;
inline constexpr std::uint16_t kCreateResponse = 34;
inline constexpr std::uint16_t kJoinResponse = 35;
inline constexpr std::uint16_t kJoinNotification = 36;
inline constexpr std::uint16_t kLeaveResponse = 37;
inline constexpr std::uint16_t kKickNotification = 38;
inline constexpr std::uint16_t kLeaveNotification = 39;
inline constexpr std::uint16_t kManageResponse = 40;
inline constexpr std::uint16_t kManageUpdateNotification = 41;
inline constexpr std::uint16_t kMemberManageResponse = 42;
inline constexpr std::uint16_t kMemberManageUpdateNotification = 43;
inline constexpr std::uint16_t kMemberUpdateNotification = 44;
inline constexpr std::uint16_t kExpUpdateNotification = 45;
inline constexpr std::uint16_t kDisbandResponse = 46;
inline constexpr std::uint16_t kDisbandNotification = 47;

} // namespace native_guild

namespace guild_housing {

inline constexpr std::uint16_t kAreaContractRequest = 20034;
inline constexpr std::uint16_t kSiteAllowMemberRequest = 20035;
inline constexpr std::uint16_t kSiteAreaBoardGetRequest = 20036;
inline constexpr std::uint16_t kSiteAssignmentRequest = 20037;
inline constexpr std::uint16_t kAreaContractExtensionRequest = 20038;

inline constexpr std::uint16_t kAreaContractResponse = 48;
inline constexpr std::uint16_t kAreaContractNotification = 49;
inline constexpr std::uint16_t kSiteAllowMemberResponse = 50;
inline constexpr std::uint16_t kSiteAreaBoardGetResponse = 51;
inline constexpr std::uint16_t kSiteAssignmentResponse = 52;
inline constexpr std::uint16_t kAreaContractExtensionResponse = 53;
inline constexpr std::uint16_t kAreaContractExtensionNotification = 54;
inline constexpr std::uint16_t kAreaUpdateNotification = 55;

bool is_request(std::uint16_t packet_id);

// Response packet ID answering a guild housing request ID (20034-20038).
std::uint16_t response_id(std::uint16_t request_id);

} // namespace guild_housing

// Sends one encoded front frame to a character's front sockets.
using FrontDelivery = std::function<void(std::int64_t char_id, const std::string &frame)>;

// The session's selected character, from the stored character row.
struct GuildHousingRequester {
  std::int64_t account_id{};
  std::int64_t char_id{};
  int world_id{};
  std::string name;
};

// Guild housing requests 20034-20038. Implemented by GuildHousingCoordinator in the housing
// library, which this front adapter does not link against directly.
class GuildHousingRequests {
public:
  virtual ~GuildHousingRequests() = default;

  // Returns the encoded response, or an empty string when the response is delivered
  // asynchronously through the front delivery. Malformed payloads throw std::invalid_argument.
  virtual std::string handle(const GuildHousingRequester &requester,
                             native_ws::FrameView frame) = 0;

  // GuildRes/GuildJoinRes.housing for a guild with a contracted area.
  virtual std::optional<x5::pb::guild::GuildHousingArea>
  housing_area(std::int64_t guild_id) const = 0;

  virtual void set_front_delivery(FrontDelivery delivery) = 0;
};

class NativeGuildFront {
public:
  // `companies` supplies the actor's company and position; null means no character has a
  // company. `housing` enables the guild housing requests 20034-20038 and GuildRes.housing.
  NativeGuildFront(AuthService &auth, GuildService &guilds, NativeChatService &delivery,
                   std::shared_ptr<const CompanyService> companies = {},
                   std::shared_ptr<GuildHousingRequests> housing = {});

  bool handles(std::uint16_t packet_id) const;

  // Returns the encoded response frame, or an empty string for a guild housing request that
  // answers asynchronously. The actor is the session's selected character; a session without
  // one is refused with Guild_InvalidChar. Malformed payloads throw std::invalid_argument,
  // which closes the socket like any other malformed packet.
  std::string handle(const std::string &token, native_ws::FrameView frame);

  // GuildService subscriber: pushes WSF2C notifications to the recipients' front sockets.
  void deliver(const std::vector<GuildEvent> &events);

  // Queues an encoded frame for every front socket of the character on this realm. An
  // offline character misses it; its next request returns the current state.
  void push_frame(std::int64_t char_id, const std::string &frame);

private:
  std::optional<GuildActor> actor(const std::string &token);

  void push_all(const std::vector<std::int64_t> &char_ids, const std::string &frame);

  void deliver_event(const GuildMemberJoined &event);
  void deliver_event(const GuildMemberLeft &event);
  void deliver_event(const GuildMemberGradeChanged &event);
  void deliver_event(const GuildMemberUpdated &event);
  void deliver_event(const GuildInfoChanged &event);
  void deliver_event(const GuildDisbanded &event);
  void deliver_event(const GuildInviteIssued &event);
  void deliver_event(const GuildInviteRejected &event);
  void deliver_event(const GuildExperienceChanged &event);

  std::string handle_housing(const std::string &token, native_ws::FrameView frame);

  AuthService &auth_;
  GuildService &guilds_;
  NativeChatService &delivery_;
  std::shared_ptr<const CompanyService> companies_;
  std::shared_ptr<GuildHousingRequests> housing_;
};

} // namespace chronicles
