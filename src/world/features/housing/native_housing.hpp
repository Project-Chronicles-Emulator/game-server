#pragma once

// Front WebSocket (WSC2F/WSF2C) adapter for housing.
//
// Requests are answered with their recovered response packet; refusals keep the response type
// and carry the Housing_* or Guild_* error code. Operations that charge the character run on
// its world session strand through HousingHub::run_for, so a cached tutorial progress revision
// stays equal to storage. Guild-area changes are pushed to every guild member's front socket.
//
// Evidence: packet IDs from protocol/native-ws-opcodes.json and the client housing manager's
// response handlers (AAC_Housing_On*Res, dumps/ida_research/housing-20261026). The release
// request (WSC2F_ReleaseHousingReq) has no sender or numeric ID in this client build
// (dumps/ida_research/housing-ordinary-20261029) and is therefore not served.

#include "world/features/guild/guild.hpp"
#include "world/features/housing/housing_world.hpp"
#include "net/native_ws_frames.hpp"
#include "p_character.pb.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace chronicles {

class AuthService;
class CharacterRepository;
class CompanyService;
class NativeChatService;

namespace native_housing {

inline constexpr std::uint16_t kSiteContractRequest = 20011;
inline constexpr std::uint16_t kAreaBoardRequest = 20012;
inline constexpr std::uint16_t kChannelListRequest = 20013;
inline constexpr std::uint16_t kChannelSearchRequest = 20014;
inline constexpr std::uint16_t kChannelVisitRequest = 20015;
inline constexpr std::uint16_t kPackageListRequest = 20017;
inline constexpr std::uint16_t kUnlockGetRequest = 20018;
inline constexpr std::uint16_t kGuildAreaContractRequest = 20034;
inline constexpr std::uint16_t kGuildAllowMemberRequest = 20035;
inline constexpr std::uint16_t kGuildAreaBoardRequest = 20036;
inline constexpr std::uint16_t kGuildAssignmentRequest = 20037;
inline constexpr std::uint16_t kGuildAreaExtensionRequest = 20038;

inline constexpr std::uint16_t kSiteContractResponse = 17;
inline constexpr std::uint16_t kAreaBoardResponse = 18;
inline constexpr std::uint16_t kChannelListResponse = 19;
inline constexpr std::uint16_t kChannelSearchResponse = 20;
inline constexpr std::uint16_t kChannelVisitResponse = 21;
inline constexpr std::uint16_t kPackageListResponse = 23;
inline constexpr std::uint16_t kUnlockGetResponse = 24;
inline constexpr std::uint16_t kGuildAreaContractResponse = 48;
inline constexpr std::uint16_t kGuildAreaContractNotification = 49;
inline constexpr std::uint16_t kGuildAllowMemberResponse = 50;
inline constexpr std::uint16_t kGuildAreaBoardResponse = 51;
inline constexpr std::uint16_t kGuildAssignmentResponse = 52;
inline constexpr std::uint16_t kGuildAreaExtensionResponse = 53;
inline constexpr std::uint16_t kGuildAreaExtensionNotification = 54;
inline constexpr std::uint16_t kGuildAreaUpdateNotification = 55;

} // namespace native_housing

// Housing's read-only view of GuildService. Member account IDs come from the character
// repository, because guild rows key members by character only.
class GuildServiceDirectory final : public housing::GuildDirectory {
public:
  GuildServiceDirectory(std::shared_ptr<GuildService> guilds,
                        std::shared_ptr<CharacterRepository> characters);

  std::optional<housing::GuildMembership> membership(
      housing::CharacterId character_id) const override;

  std::vector<housing::GuildMember> members(housing::GuildId guild_id) const override;

  // Answered from GuildService's in-memory registry, loaded from storage at startup.
  std::optional<bool> guild_exists(housing::GuildId guild_id) const override;

private:
  std::shared_ptr<GuildService> guilds_;
  std::shared_ptr<CharacterRepository> characters_;
};

class NativeHousingFront {
public:
  // `companies`, when present, supplies the requester's company position so personal
  // contracts enforce the catalog's company-position levels.
  NativeHousingFront(AuthService &auth, std::shared_ptr<housing::HousingHub> hub,
                     NativeChatService &delivery,
                     std::shared_ptr<const CompanyService> companies = {});

  static bool handles(std::uint16_t packet_id);

  // Returns the encoded response frame. The actor is the session's selected character; a
  // session without one is refused with InvalidChar. Malformed payloads throw
  // std::invalid_argument, which closes the socket like any other malformed packet.
  std::string handle(const std::string &token, native_ws::FrameView frame);

  // Restores owned-site and saved housing-channel metadata in CharSelectRes. The recovered
  // WorldCharacter.entry schema carries housing context separately from world-map packets.
  void populate_character_entry(const std::string &token, x5::pb::WorldCharacter &character);

  // HousingHub guild-area listener: pushes WSF2C guild housing notifications to members.
  void deliver_guild_area(const housing::GuildAreaNotice &notice);

private:
  std::optional<housing::HousingActor> actor(const std::string &token);

  // The housing zone a request without a zone key addresses: the character's current housing
  // area, otherwise the first rentable zone of this client build.
  int request_zone(const housing::HousingActor &actor) const;

  // The channel a resident-board request reads. A zero (omitted) channel resolves to the
  // character's live channel in `zone_key`, otherwise the primary personal channel; explicit
  // channels pass through unchanged for HousingService to validate.
  int board_channel(const housing::HousingActor &actor, int zone_key,
                    int requested_channel) const;

  std::string handle_personal(const housing::HousingActor &actor, native_ws::FrameView frame);

  std::string handle_guild(const housing::HousingActor &actor, native_ws::FrameView frame);

  AuthService &auth_;
  std::shared_ptr<housing::HousingHub> hub_;
  NativeChatService &delivery_;
  std::shared_ptr<const CompanyService> companies_;
};

} // namespace chronicles
