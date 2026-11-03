#pragma once

#include "world/features/combat/combat_rules.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chronicles::gm {

// Game master commands that act on combat units, quests and mail. They complement the
// diagnostics (gm_diagnostics.hpp), session (gm_tools.hpp), inventory/progression
// (gm_commands.hpp), companion, housing and event GM modules.
//
// Text names reuse the recovered GmCmd field names (the official GMCommand.csv grammar is absent
// from the exported client data, so every argument grammar is an emulator choice). In this
// client build the structured forms arrive only from tools; a GM types the text form after /gm.

enum class WorldCommandKind {
  monster_kill,
  damage_monster,
  add_status_effect,
  remove_status_effect,
  add_abnormal_gauge,
  cancel_skill,
  reset_cooldowns,
  npc_ai,
  set_gm_stat,
  add_gm_stat,
  execute_skill_effect,
  move_unit,
  accept_quest,
  mail_send,
  combat_formula_log,
  stat_sync_warning,
};

// Which unit a command acts on.
enum class UnitTargetKind {
  unit,
  self,
  // Every NPC this GM spawned with spawn_npc.
  spawned,
};

struct UnitTarget {
  UnitTargetKind kind{UnitTargetKind::self};
  std::uint64_t unit_id{};
};

enum class MailAttachmentChoice {
  none,
  item,
  gold,
};

struct WorldCommand {
  WorldCommandKind kind{WorldCommandKind::reset_cooldowns};
  UnitTarget target;
  // Status effect, skill effect, quest, stat type or item CMS id, depending on the command.
  int cms_id{};
  // Damage, stat value, gauge amount, item count or gold.
  std::int64_t amount{};
  // damage_monster: percent of current HP instead of `amount`. add_gm_stat: percent change.
  // A floating percentage preserves the fractional structured stat ratio without rounding it.
  std::optional<double> percent;
  // npc_ai on/off.
  bool enabled{};
  // set_gm_stat clear: recompute stats from equipment and build.
  bool clear{};
  // accept_quest_if_passed_cond: require the catalog conditions.
  bool check_conditions{};
  // add_abnormal_status_gauge: one or more ailment gauges and their amounts.
  std::vector<std::pair<combat_rules::AbnormalType, int>> gauges;
  // move_unit destination in meters; relative adds it to the current position.
  double x_meters{};
  double y_meters{};
  double z_meters{};
  bool relative{};
  // mail_send recipient: empty means the GM's own character.
  std::string recipient_name;
  std::int64_t recipient_character_id{};
  // Optional ownership assertion checked against the resolved recipient by the world adapter.
  std::int64_t recipient_account_id{};
  MailAttachmentChoice attachment{MailAttachmentChoice::none};
  std::string mail_title;
  std::string mail_text;
};

struct WorldCommandParse {
  // False when another GM parser owns the request.
  bool matched{};
  std::optional<WorldCommand> command;
  // Set when matched but invalid: a short usage text that never repeats request text.
  std::string error;
};

// Emulator bounds, not recovered official limits.
inline constexpr std::int64_t kMaximumGmDamage = 100'000'000;
inline constexpr int kMaximumGmGaugeAmount = 100'000;
inline constexpr std::int64_t kMaximumGmStatValue = 100'000'000;
inline constexpr std::size_t kMaximumGmMailTitleBytes = 64;
inline constexpr std::size_t kMaximumGmMailTextBytes = 200;

// Text grammar (ASCII case-insensitive; <unit> is a unit id, `me` or `spawned`):
//   monster_kill <unit>                          alias: kill
//   damage_monster <unit> <amount|percent%>      alias: damage
//   add_status_effect <unit> <status_effect_id>  alias: buff
//   remove_status_effect <unit> <status_effect_id>
//   add_abnormal_status_gauge <unit> <type> <amount>   alias: gauge
//       type: burning|freezing|electric_shock|bleeding|poisoning|stun|shrink
//   cancel_skill [unit]
//   cooldown                                      alias: reset_cooldowns
//   npc_ai_on <unit> / npc_ai_off <unit>
//   set_gm_stat <stat> <value> | set_gm_stat clear
//   add_gm_stat <stat> <value|percent%>
//   execute_skill_effect <skill_effect_id>
//   move_unit <unit> <x> <y> <z> [rel]            meters
//   accept_quest <quest_id> / accept_quest_if_passed_cond <quest_id>
//   mail_send <name|me> [item <item_id> [count] | gold <amount>]
//   combat_formula_log / stat_sync_warning
WorldCommandParse parse_world_command(std::uint16_t opcode, const nlohmann::json &message);

// For GmCmd fields and text names that this server recognizes but cannot serve: a one-line
// reason naming what is missing. Nothing for commands that are served or unknown.
std::optional<std::string> unsupported_command_reason(std::uint16_t opcode,
                                                      const nlohmann::json &message);

const char *world_command_name(WorldCommandKind kind);

} // namespace chronicles::gm
