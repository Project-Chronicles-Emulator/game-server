#pragma once

#include "world/features/npc/npc_ai.hpp"
#include "persistence/character_state_types.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace chronicles::gm {

// Game master diagnostics: the client's debug overlays and the world clock override.
//
// Static evidence (client build 40367e24, dumps/ida_research/gm-diagnostics-20261026):
// - The world packet table installs real handlers for opcodes 150-159; only DebugText (149) is
//   discarded. WorldGameTimeNoti (158) reaches the world time manager. The debug overlay
//   notifications (150-157, 159) reach the X5 debug subsystem, which draws positions, paths,
//   AI nodes, regen stats and a channel-event text panel.
// - The client's debug UI itself sends GmCmd.debug_watch {unit_id, type, add} (oneof 1),
//   GmCmd.debug_npc_field (oneof 3) and GmCmd.channel_event_info (oneof 98).
// Whether the shipping client instantiates the debug subsystem was not established, so the
// overlay packets are Candidate output; the time manager path is static_verified.

inline constexpr std::uint16_t kDebugAiNotification = 150;
inline constexpr std::uint16_t kDebugPathNotification = 151;
inline constexpr std::uint16_t kDebugPositionNotification = 152;
inline constexpr std::uint16_t kDebugPlayerRegenStatNotification = 155;
inline constexpr std::uint16_t kWorldGameTimeNotification = 158;
inline constexpr std::uint16_t kDebugChannelEventInfoNotification = 159;

// x5.pb.debug.ESubscribeType values (GmCmd.DebugWatch.type).
enum class WatchType {
  abnormal_status = 0,
  ai = 1,
  path = 2,
  position = 3,
  skill_collision = 4,
};

// The server can only report watch types whose state it simulates.
bool is_supported_watch_type(WatchType type);

const char *watch_type_name(WatchType type);

// Emulator bounds, not recovered official limits.
inline constexpr std::size_t kMaximumWatchesPerSession = 8;
inline constexpr std::int64_t kWatchIntervalTicks = 5'000'000;
inline constexpr int kMinimumGameDaySeconds = 60;
inline constexpr int kMaximumGameDaySeconds = 7 * 24 * 60 * 60;
// Day length used when the command does not name one: one game day per real hour. This is an
// emulator choice; the official default day length is unknown.
inline constexpr int kDefaultGameDaySeconds = 60 * 60;

// World clock override carried by WorldGameTimeNoti.
struct GameTimeSetting {
  int day_seconds{kDefaultGameDaySeconds};
  // Time of day in hours, 0 (midnight) to 24.
  double hour{};
  // True freezes the clock at `hour`; false lets it run from `hour`.
  bool stopped{};
};

enum class DiagnosticKind {
  game_time,
  regen_stat,
  watch,
  unwatch,
  channel_event_info,
  npc_field,
};

// Which units a watch command names.
enum class WatchTarget {
  unit,
  self,
  spawned,
};

struct DiagnosticCommand {
  DiagnosticKind kind{DiagnosticKind::game_time};
  GameTimeSetting game_time;
  WatchTarget target{WatchTarget::unit};
  // Watched unit for WatchTarget::unit; zero with `unwatch` means every watch.
  std::uint64_t unit_id{};
  std::vector<WatchType> watch_types;
};

struct DiagnosticParse {
  // False when another GM parser owns the request.
  bool matched{};
  std::optional<DiagnosticCommand> command;
  // Set when matched but invalid: a short usage text that never repeats request text.
  std::string error;
};

// Parses PC2S_GmCmdTextReq (20009) or a single-command PC2S_GmCmdReq (20008).
//
// Text grammar (emulator-defined; the official GMCommand.csv is absent from the exported data):
//   game_time <hour 0-24> [day_seconds]    alias: time
//   game_time stop <hour 0-24> [day_seconds]
//   regen_stat                             aliases: regen_stats, debug_player_regen_stat
//   watch <unit_id|me|spawned> [position|path|ai|all]   alias: debug_watch
//   unwatch [unit_id]                      no unit clears every watch
//   channel_event_info                     alias: channel_event
//   debug_npc_field
// Structured: GmCmd.game_time, debug_player_regen_stat, debug_watch, channel_event_info and
// debug_npc_field.
DiagnosticParse parse_diagnostic_command(std::uint16_t opcode, const nlohmann::json &message);

// PS2C_WorldGameTimeNoti body. Static client behavior: a running clock starts at
// started_timestamp (Unix seconds) shifted back by daily_ratio of a day; a stopped clock shows
// daily_ratio of a day. one_day_secs is the real length of one game day.
nlohmann::json world_game_time_message(const GameTimeSetting &setting,
                                       std::int64_t now_unix_seconds);

// PS2C_DebugPlayerRegenStatNoti body with the regen-related stats present in `player_stats`
// (protobuf JSON map keyed by decimal EX5StatType).
nlohmann::json player_regen_stat_message(const nlohmann::json &player_stats,
                                         std::int64_t monotonic_ticks);

// PS2C_DebugChannelEventInfoNoti body for the current map with no event, objective or save.
// The world session fills event_cms_id and completed from the channel event board
// (src/world/features/events/channel_event.hpp); objectives and saved progress are server-only
// data.
nlohmann::json channel_event_info_message(int map_id);

nlohmann::json debug_position_message(std::uint64_t unit_id, const WorldPosition &position);

// Current position, then the end of the running route when one exists.
nlohmann::json debug_path_message(std::uint64_t unit_id, const WorldPosition &position,
                                  const std::optional<WorldPosition> &route_end);

// One behavior-tree node named after the emulator AI state. The phase numbers are the
// emulator's NpcAiState order; the official behavior-tree numbering is unknown.
nlohmann::json debug_ai_message(std::uint64_t unit_id, NpcAiState state);

const char *ai_state_name(NpcAiState state);

struct Watch {
  std::uint64_t unit_id{};
  WatchType type{WatchType::position};

  bool operator==(const Watch &) const = default;
};

enum class WatchAddResult {
  added,
  already_watched,
  unsupported_type,
  limit_reached,
};

// Bounded per-session list of watched units and the emission deadline. Confined to the
// session worker.
class WatchList {
public:
  WatchAddResult add(std::uint64_t unit_id, WatchType type);

  // Removes one watch. Returns false when it was not present.
  bool remove(std::uint64_t unit_id, WatchType type);

  // Removes every watch of one unit and returns how many were removed.
  std::size_t remove_unit(std::uint64_t unit_id);

  void clear();

  bool empty() const;

  const std::vector<Watch> &entries() const;

  // Next emission tick while any watch exists and an emission was recorded.
  std::optional<std::int64_t> next_tick() const;

  // True while any watch exists and its emission interval has elapsed (or nothing was emitted).
  bool due(std::int64_t ticks) const;

  // Records an emission at `ticks`; the next one is due one interval later.
  void mark_emitted(std::int64_t ticks);

private:
  std::vector<Watch> watches_;
  std::optional<std::int64_t> next_tick_;
};

const char *diagnostic_name(DiagnosticKind kind);

} // namespace chronicles::gm
