#pragma once

// Game master commands for leaderboards, raid channels and channel events.
//
// The official GmCmd oneof has raid_channel_enter {map_id}, complete_channel_event_objective and
// reset_channel_event_save_data (descriptor fields 119, 99 and 100); no ranking GmCmd exists.
// GmCmd.channel_event_info (98) belongs to the debug panel parser
// (src/world/features/gm/gm_diagnostics.hpp), which reports this module's state. Text names reuse
// those fields; the ranking_* names and every argument grammar are emulator choices, as for the
// other GM modules (gm_tools.hpp).

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chronicles::event_gm {

enum class EventCommandKind {
  ranking_start,
  ranking_finish,
  ranking_cancel,
  raid_channel_enter,
  complete_channel_event_objective,
  reset_channel_event_save_data,
};

struct EventCommand {
  EventCommandKind kind{EventCommandKind::ranking_cancel};
  // Ranking.id for ranking_start/ranking_finish; the raid map for raid_channel_enter.
  int target_id{};
};

struct EventCommandParse {
  // False when another GM parser may own the request.
  bool matched{};
  std::optional<EventCommand> command;
  // Set when matched but invalid: a usage line that never repeats request text.
  std::string error;
};

// Text (PC2S_GmCmdTextReq, ASCII case-insensitive):
//   ranking_start <ranking_id>     starts a server-timed boss attempt
//   ranking_finish <ranking_id>    ends it and records the kill time
//   ranking_cancel                 drops the attempt
//   raid_channel_enter <map_id>    forms a one-member raid channel at once
//   complete_channel_event_objective   sets the current map's event status to Success
//   reset_channel_event_save_data  clears the current map's status and tribute ledgers
// Structured (PC2S_GmCmdReq): exactly one of GmCmd.raid_channel_enter {map_id},
// complete_channel_event_objective {} or reset_channel_event_save_data {}.
EventCommandParse parse_event_command(std::uint16_t opcode, const nlohmann::json &message);

// Help lines for the commands above.
std::vector<std::string> help_lines();

const char *command_name(EventCommandKind kind);

} // namespace chronicles::event_gm
