#pragma once

// Raid channel matchmaking (client RaidUI table) for the world transport.
//
// Evidence (build 40367e24...):
// - Opcodes from protocol/opcodes.json: PC2S 20091 RaidChannelMoveReq, 20110 Register,
//   20111 Cancel, 20112 Leave; PS2C 211 RaidChannelRegisterNoti, 212 Cancel, 213 Matched,
//   214 Leave, 215 Destroy; PS2C 2 CommonErrorRes for refusals (no response packet exists).
// - RaidUI lists the raid maps 10103, 20229 and 10301. Their Map rows are type Raid on the
//   ArcheWorld world with chMaxCount 12, timeLimit 540/900/1200 s and channelDestroyDelay 20 s;
//   the raid time windows (startRaidChannelTime/endRaidChannelTime) are empty in this build.
// - GameVariable RaidAutoEnterTime = 10000 ms. The client's matched widget
//   (AAC_UI_RaidMatching_ShowMatchedCountdown_Candidate) shows RAID_MATCHING_Done and a
//   countdown derived from that variable. Automatic client entry remains unverified; this
//   service permits the member's move request only after the countdown.
// - EErrorCode Raid_RegisterDeclined, Raid_RequesterNotFound, Raid_AlreadyApplied,
//   Raid_InvalidChannelId, Raid_NoChannelToCancel and Raid_TimeUnavailable name the refusals.
//
// Emulator policy (no official capture or server rule exists): one FIFO queue per raid map; a
// full channel forms at once, and a partial group forms once its oldest applicant has waited
// RaidMatchmakerConfig::match_wait. A matched channel closes when its time limit ends after the
// entry countdown. Only one active group per raid map is supported: the transport currently
// isolates by map, not by a separate raid instance ID. Later groups wait in FIFO order.
// Character IDs on the wire are database IDs, as in the party roster.

#include "persistence/character_repository.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles {
class ClientCatalog;
} // namespace chronicles

namespace chronicles::raid {

inline constexpr std::uint16_t kRaidChannelMoveRequest = 20091;
inline constexpr std::uint16_t kRaidChannelRegisterRequest = 20110;
inline constexpr std::uint16_t kRaidChannelCancelRequest = 20111;
inline constexpr std::uint16_t kRaidChannelLeaveRequest = 20112;

inline constexpr std::uint16_t kCommonErrorResponse = 2;
inline constexpr std::uint16_t kRaidChannelRegisterNotification = 211;
inline constexpr std::uint16_t kRaidChannelCancelNotification = 212;
inline constexpr std::uint16_t kRaidChannelMatchedNotification = 213;
inline constexpr std::uint16_t kRaidChannelLeaveNotification = 214;
inline constexpr std::uint16_t kRaidChannelDestroyNotification = 215;

using Clock = std::chrono::steady_clock;

struct RaidMap {
  int map_id{};
  // Map.parentMapId: the public map whose coordinates the raid channel shares.
  int parent_map_id{};
  // Map.chMaxCount: members of one raid channel.
  int capacity{};
  // Map.timeLimit, in seconds.
  std::chrono::seconds time_limit{};
};

class RaidCatalog {
public:
  static std::vector<std::string> required_tables();

  static std::shared_ptr<const RaidCatalog> load(ClientCatalog &catalog);

  // Every RaidUI row must name a Map row of type Raid with a positive capacity.
  RaidCatalog(const std::vector<nlohmann::json> &raid_ui_rows,
              const std::vector<nlohmann::json> &map_rows, std::chrono::milliseconds auto_enter);

  const RaidMap *find(int map_id) const;

  // GameVariable RaidAutoEnterTime: the matched countdown before members enter.
  std::chrono::milliseconds auto_enter_delay() const;

  std::size_t size() const;

private:
  std::unordered_map<int, RaidMap> maps_;
  std::chrono::milliseconds auto_enter_;
};

struct RaidMatchmakerConfig {
  // Emulator policy: how long the oldest applicant waits before a partial channel forms.
  std::chrono::seconds match_wait{30};
  // Emulator policy: the smallest partial channel.
  std::size_t minimum_members{1};
};

// Display fields come from the authenticated character, never from a request payload.
struct RaidApplicant {
  std::int64_t char_id{};
  std::uint64_t connection_id{};
  std::string name;
  int level{};
  int char_class{};
};

// One decoded payload addressed to a world connection.
struct RaidPacket {
  std::uint64_t connection_id{};
  std::uint16_t opcode{};
  nlohmann::json message;
};

// A member a closed channel removed; its session returns it from the raid map if it is there.
struct RaidEviction {
  std::uint64_t connection_id{};
  int map_id{};
};

struct RaidUpdate {
  std::vector<RaidPacket> packets;
  std::vector<RaidEviction> evictions;

  void append(RaidUpdate other);
};

enum class RaidState { none, waiting, matched };

struct RaidMembership {
  RaidState state{RaidState::none};
  int map_id{};
  std::uint64_t channel_id{};
};

// Result of authorizing PC2S_RaidChannelMoveReq.
struct RaidMoveDecision {
  // Empty when the move may proceed.
  std::optional<std::string> error;
  const RaidMap *map{};
};

// Thread-safe. Every public method takes one internal lock and never calls back into callers.
class RaidMatchmaker {
public:
  RaidMatchmaker(std::shared_ptr<const RaidCatalog> catalog, RaidMatchmakerConfig config);

  // Register, cancel and leave. The session authorizes moves itself through authorize_move.
  static bool handles(std::uint16_t opcode);

  // `eligible` is false when the requester is in a private tutorial or housing instance.
  RaidUpdate receive(const RaidApplicant &requester, std::uint16_t opcode,
                     const nlohmann::json &message, std::uint32_t sequence, bool eligible,
                     Clock::time_point now);

  // A matched member may enter its own raid map after the countdown and before closure.
  RaidMoveDecision authorize_move(std::int64_t char_id, const nlohmann::json &message,
                                  std::uint64_t connection_id, Clock::time_point now) const;

  // GM raid_channel_enter: forms a one-member channel at once, replacing a queued application.
  RaidUpdate force_match(const RaidApplicant &requester, int map_id, Clock::time_point now);

  // Session closed: an application is dropped, a matched member leaves its channel. Ignores
  // stale connections so a newer session of the same character keeps its membership.
  RaidUpdate detach(std::int64_t char_id, std::uint64_t connection_id);

  // The session has left raid-map presence, or never entered it. Until every departing
  // session confirms this, the map stays reserved against another group's match.
  void complete_eviction(std::int64_t char_id, std::uint64_t connection_id, int map_id);

  // Forms due partial channels and closes expired ones. Call about once per second while
  // active().
  RaidUpdate tick(Clock::time_point now);

  bool active() const;

  RaidMembership membership(std::int64_t char_id) const;

  const RaidCatalog &catalog() const;

private:
  struct Application {
    RaidApplicant applicant;
    Clock::time_point registered_at;
  };

  struct Channel {
    std::uint64_t id{};
    int map_id{};
    std::vector<RaidApplicant> members;
    Clock::time_point opens_at;
    Clock::time_point closes_at;
  };

  struct Departure {
    std::uint64_t connection_id{};
    int map_id{};
  };

  RaidUpdate register_applicant(const RaidApplicant &requester, const nlohmann::json &message,
                                std::uint32_t sequence, bool eligible, Clock::time_point now);
  RaidUpdate cancel(const RaidApplicant &requester, const nlohmann::json &message,
                    std::uint32_t sequence);
  RaidUpdate leave(const RaidApplicant &requester, const nlohmann::json &message,
                   std::uint32_t sequence);
  void match_queue(int map_id, Clock::time_point now, RaidUpdate &update);
  void form_channel(const RaidMap &map, std::vector<RaidApplicant> members, Clock::time_point now,
                    RaidUpdate &update);
  void remove_member(Channel &channel, std::int64_t char_id, bool notify_leaver,
                     RaidUpdate &update);
  bool map_occupied(int map_id) const;
  bool queued(std::int64_t char_id) const;
  Channel *channel_of(std::int64_t char_id);
  const Channel *channel_of(std::int64_t char_id) const;

  mutable std::mutex mutex_;
  std::shared_ptr<const RaidCatalog> catalog_;
  RaidMatchmakerConfig config_;
  std::unordered_map<int, std::deque<Application>> queues_;
  std::unordered_map<std::uint64_t, Channel> channels_;
  // Character-keyed so a session cannot reapply while its previous eviction is pending.
  std::unordered_map<std::int64_t, Departure> departures_;
  std::uint64_t next_channel_id_{1};
};

// CommonErrorRes for a refused raid request.
nlohmann::json raid_error(const std::string &code, std::uint32_t sequence);

} // namespace chronicles::raid
