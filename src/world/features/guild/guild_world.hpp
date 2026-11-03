#pragma once

// World-transport (PC2S/PS2C) integration of GuildService.
//
// GuildWorld keeps the directory of characters that entered the world (character ->
// connection and authoritative identity), decodes the PC2S guild invitation requests and
// builds the PS2C payloads. It never touches sockets or the protocol codec: callers encode
// and deliver the returned packets, addressed by character ID. Thread-safe; it never holds
// its lock while calling GuildService, whose subscribers deliver through this directory.
//
// Evidence:
// - Opcodes from protocol/opcodes.json: PC2S 20113 GuildInvitePopupReq, 20114
//   GuildAcceptInviteReq, 20115 GuildRejectInviteReq; PS2C 146 WorldCharGuildNoti, 216
//   GuildInvitePopupNoti, 217 GuildInviteRejectNoti, 218 ChangeGuildUnitNoti, 219
//   GuildInvitePopupRes, 220 GuildRejectInviteRes; PS2C 2 CommonErrorRes.
// - Official captures contain WorldCharGuildNoti only as an empty entry snapshot; no
//   invitation or ChangeGuildUnitNoti traffic was captured. Field use follows the schema:
//   GuildInvitePopupNoti.guild_char_id is read as the inviter's character ID (Candidate).
// - PlayerNub.guild_id (28) and guild_name (24) decorate observed player units, matching
//   PlayerNub.party_id for parties.

#include "world/features/guild/guild.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles {

namespace guild_world {

inline constexpr std::uint16_t kInvitePopupRequest = 20113;
inline constexpr std::uint16_t kAcceptInviteRequest = 20114;
inline constexpr std::uint16_t kRejectInviteRequest = 20115;

inline constexpr std::uint16_t kCommonErrorResponse = 2;
inline constexpr std::uint16_t kWorldCharGuildNotification = 146;
inline constexpr std::uint16_t kInvitePopupNotification = 216;
inline constexpr std::uint16_t kInviteRejectNotification = 217;
inline constexpr std::uint16_t kChangeGuildUnitNotification = 218;
inline constexpr std::uint16_t kInvitePopupResponse = 219;
inline constexpr std::uint16_t kRejectInviteResponse = 220;

// One decoded payload addressed to a character's world connection.
struct OutboundPacket {
  std::int64_t char_id{};
  std::uint16_t opcode{};
  nlohmann::json message;
};

// A player unit whose guild label changed. guild_id zero means it left its guild.
struct UnitGuildChange {
  std::int64_t char_id{};
  std::int64_t guild_id{};
  std::string guild_name;
};

struct GuildWorldUpdate {
  std::vector<OutboundPacket> packets;
  std::vector<UnitGuildChange> unit_changes;

  void append(GuildWorldUpdate other);

  bool empty() const;
};

} // namespace guild_world

class GuildWorld {
public:
  explicit GuildWorld(std::shared_ptr<GuildService> guilds);

  static bool handles(std::uint16_t opcode);

  // Registers a character once its client entered the world, refreshes its guild member
  // display fields and returns its WorldCharGuildNoti plus its own unit's guild label.
  guild_world::GuildWorldUpdate attach(const GuildActor &actor, std::uint64_t connection_id);

  // Ignores a stale connection, so a newer session for the same character stays attached.
  void detach(std::int64_t char_id, std::uint64_t connection_id);

  // Handles one PC2S guild request from an attached requester. Responses address the
  // requester; cross-connection packets arrive through translate() of published events.
  guild_world::GuildWorldUpdate receive(std::int64_t requester, std::uint16_t opcode,
                                        const nlohmann::json &message, std::uint32_t sequence);

  // Converts published guild events into world packets and unit label changes.
  guild_world::GuildWorldUpdate translate(const std::vector<GuildEvent> &events) const;

  std::optional<std::uint64_t> connection_of(std::int64_t char_id) const;

  // Sets nub.player.guild_id and guild_name on a PS2C_UnitAddNoti payload whose unit is a
  // guild member (player unit IDs equal character IDs).
  void decorate_unit_add(nlohmann::json &unit_add_message) const;

  static nlohmann::json change_guild_unit_message(const guild_world::UnitGuildChange &change);

private:
  struct OnlineCharacter {
    GuildActor actor;
    std::uint64_t connection_id{};
  };

  std::optional<GuildActor> online_actor(std::int64_t char_id) const;

  guild_world::GuildWorldUpdate invite(const GuildActor &requester,
                                       const nlohmann::json &message);

  guild_world::GuildWorldUpdate accept(const GuildActor &requester, const nlohmann::json &message,
                                       std::uint32_t sequence);

  guild_world::GuildWorldUpdate reject(const GuildActor &requester,
                                       const nlohmann::json &message);

  std::shared_ptr<GuildService> guilds_;
  mutable std::mutex mutex_;
  std::unordered_map<std::int64_t, OnlineCharacter> online_;
};

} // namespace chronicles
