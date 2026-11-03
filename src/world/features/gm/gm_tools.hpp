#pragma once

#include "persistence/character_state_types.hpp"
#include "world/features/quests/graduation_quests.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::gm {

// Session-level game master commands: movement, protection, visibility and NPC spawning.
// They complement the inventory/progression commands in gm_commands.hpp and the companion and
// housing GM commands, which keep their own parsers.
//
// How the client sends them (static research, dumps/ida_research/gm-20261026/FINDINGS.md):
// - A chat line starting with "/gm " is intercepted by the client's slash-command handler and
//   the rest of the line is sent as PC2S_GmCmdTextReq{text}.
// - The Tilde console (enabled by WorldCharBaseNoti.gm) sends the whole line the same way.
// - The client only handles "togglegraze" and "raycast" locally; everything else reaches here.
// The text grammar is defined by this emulator: the official GMCommand.csv table is absent from
// the exported client data.

enum class SessionCommandKind {
  help,
  position,
  teleport,
  teleport_relative,
  teleport_map,
  go_to_player,
  summon_player,
  list_players,
  god_mode,
  invisible,
  infinite_stamina,
  free_movement,
  heal,
  revive,
  spawn_npc,
  despawn_npcs,
  status,
  complete_quests,
  complete_quest,
  reveal_map,
  suicide,
  kick,
  announce,
};

// Relative teleport direction for "up", "down" and "fwd".
enum class RelativeDirection { up, down, forward };

struct SessionCommand {
  SessionCommandKind kind{SessionCommandKind::help};
  // on/off for toggles. Empty means "flip the current state".
  std::optional<bool> enabled;
  // Teleport destination or relative distance, in meters (client positions are millimeters).
  double x_meters{};
  double y_meters{};
  double z_meters{};
  bool has_coordinates{};
  RelativeDirection direction{RelativeDirection::up};
  int map_id{};
  std::string player_name;
  int npc_cms_id{};
  int count{1};
  int help_page{1};
  // complete_quest target, or zero.
  int quest_id{};
  // complete_quests chain: repeat until no quest is in progress (accepted successors too).
  bool chain{};
  // tp / set_pos_and_yaw: facing in degrees; empty keeps the current yaw.
  std::optional<float> yaw;
  // go_to_player by character id (GmCmd.follow_user.char_id) when no name is given.
  std::int64_t character_id{};
  // kick: seconds before this GM's own connection closes (GmCmd.test_kick.after_sec).
  int delay_seconds{};
  // announce: the notice text, already validated for length.
  std::string announcement;
};

struct SessionParse {
  // False when the request is not a session command; another GM parser may own it.
  bool matched{};
  std::optional<SessionCommand> command;
  // Set when matched but invalid. A short usage text that never repeats request text.
  std::string error;
};

// Emulator bounds, not recovered official limits.
inline constexpr int kMaximumSpawnedNpcsPerCommand = 10;
inline constexpr std::size_t kMaximumSpawnedNpcsPerSession = 50;
inline constexpr double kMaximumRelativeTeleportMeters = 5000.0;
// Client positions are signed 32-bit millimeters; this keeps every converted value in range.
inline constexpr double kMaximumCoordinateMeters = 2'000'000.0;
inline constexpr std::size_t kMaximumPlayerNameBytes = 32;
// complete_quests chain stops after this many commit rounds (sequential chains are finite,
// but a catalog cycle or a large backlog must not keep one request running).
inline constexpr int kMaximumQuestCompletionRounds = quests::kMaximumQuestCompletionRounds;
inline constexpr int kMaximumKickDelaySeconds = 60;
inline constexpr std::size_t kMaximumAnnouncementBytes = 200;

// Parses PC2S_GmCmdTextReq (20009) or PC2S_GmCmdReq (20008). Structured requests are matched only
// when they hold exactly one of GmCmd.invincible, invisible, fly_mode, spawn_npc, revive,
// completable_quest_all, completable_quest, complete_quest_progress, reward_quest, regen,
// despawn_npc_all, suicide, test_kick or announce.
SessionParse parse_session_command(std::uint16_t opcode, const nlohmann::json &message);

// Lines of the requested help page (1-based). Lists every GM command this server accepts,
// including the ones implemented by other GM modules.
std::vector<std::string> help_lines(int page);

inline constexpr int kHelpPageCount = 5;

std::int32_t meters_to_millimeters(double meters);

// Absolute destination in the given map; keeps the current yaw.
WorldPosition absolute_destination(int map_id, double x_meters, double y_meters, double z_meters,
                                   float yaw);

// Destination of "up", "down" or "fwd". Forward uses the current yaw in degrees, with
// x = cos(yaw) and y = sin(yaw) (Unreal Engine convention; Candidate for this client).
// Returns nothing when the result would leave the supported coordinate range.
std::optional<WorldPosition> relative_destination(const WorldPosition &current,
                                                  RelativeDirection direction, double meters);

// Arrival point beside another unit so two characters do not overlap (1.5 m along +x).
WorldPosition beside(const WorldPosition &target);

// "map 9012 x=-795.15 y=712.19 z=-698.78 yaw=158.6 (meters)".
std::string describe_position(const WorldPosition &position);

// Formats one online-player label; the caller owns lookup, codec locking and peer retention.
std::string online_player_line(const std::string &name, bool game_master, int map_id);

// Case-insensitive ASCII name comparison for character lookup.
bool same_character_name(std::string_view first, std::string_view second);

// Whitespace-separated words of one GM text line, shared by the GM text parsers.
std::vector<std::string_view> split_command_words(std::string_view text);

// x5.cms.EX5StatType from a GM token: the number ("18") or the enum name without its prefix,
// ASCII case-insensitive ("maxhp", "Con"). Nothing for unknown or None.
std::optional<int> parse_stat_type_token(std::string_view token);

// PS2C_UnitAddNoti (10) body for a GM-spawned NPC, matching the populated-NPC shape.
nlohmann::json spawned_npc_message(std::uint64_t unit_id, int npc_cms_id,
                                   const WorldPosition &position, const std::string &faction);

// PS2C_UnitRemoveNoti (11) body for GM-spawned NPCs.
nlohmann::json removed_units_message(const std::vector<std::uint64_t> &unit_ids);

// Positions for `count` spawned NPCs on a circle around the GM, starting 3 m in front.
std::vector<WorldPosition> spawn_positions(const WorldPosition &center, int count);

// Unit ids reserved for GM spawns at the top of the server NPC range, disjoint from populated
// NPC ids (allocated upward from kFirstNpcUnitId). `sequence` counts spawns per session.
std::uint64_t spawned_npc_unit_id(std::int64_t character_id, std::uint32_t sequence);

const char *session_command_name(SessionCommandKind kind);

} // namespace chronicles::gm
