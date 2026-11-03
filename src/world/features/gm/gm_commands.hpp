#pragma once

#include "ec.generated.pb.h"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/character/progression_edit.hpp"
#include "world/features/shop/item_bundle.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles {

class Progression;

// Game master commands sent by the original client (static research,
// dumps/ida_research/gm-20261026/FINDINGS.md):
//   PC2S_GmCmdTextReq (20009) carries one console line. The client sends it from the Tilde
//   console, which it enables only after WorldCharBaseNoti.gm=true.
//   PC2S_GmCmdReq (20008) carries repeated structured GmCmd oneofs; for example a GM's
//   revive button sends GmCmd.revive instead of ReviveReq.
// The official GMCommand.csv text grammar is absent from the exported client data, so the
// text commands below are an emulator grammar named after the recovered GmCmd fields.
// Authorization is the caller's job: only a world session whose handoff carries the
// account's game-master role may reach these functions.
inline constexpr std::uint16_t kGmCommandRequestOpcode = world_request::kGmCommandRequest;
inline constexpr std::uint16_t kGmCommandTextRequestOpcode = world_request::kGmCommandTextRequest;

// Emulator request bounds, not recovered official limits.
inline constexpr std::size_t kMaximumGmCommandsPerRequest = 8;
inline constexpr std::size_t kMaximumGmCommandTextBytes = 256;
inline constexpr int kMaximumGmGrantedInstances = 32;

enum class GmCommandKind {
  item_add,
  add_money,
  // Sets the durability of the equipped weapon slots (GmCmd.set_weapon_durability).
  set_weapon_durability,
  // Removes every unequipped item instance and the stack units the utility bag does not
  // reference (GmCmd.itembag_clear with equip=false).
  itembag_clear,
  // Character progression is executed by the Character owner through the session binding,
  // separately from plan_gm_command's item-trade plans.
  exp_up,
  set_level,
  add_char_build_point,
  set_char_build_point,
  reset_stat_bonus,
  force_reset_perk,
  // GmCmd.add_stat_bonus / set_stat_bonus {type, value}: allocated attribute bonus without
  // spending LevelUpPoints. GmCmd.learn_all_perks: every class perk at its maximum level.
  add_stat_bonus,
  set_stat_bonus,
  learn_all_perks,
};

struct GmCommand {
  GmCommandKind kind{GmCommandKind::item_add};
  int item_cms_id{};
  int item_count{};
  // Gold delta; negative values remove gold but never below zero.
  std::int64_t money_amount{};
  // set_weapon_durability target; each weapon is capped at its catalog maximum.
  int durability{};
  // exp_up EXP delta, set_level level, or the build point amount of *_char_build_point.
  std::int64_t progression_value{};
  // x5.cms.EX5CharBuildPointType of *_char_build_point (1 LevelUpPoint, 2 WeaponPoint).
  int build_point_type{};
  // x5.cms.EX5StatType of add_stat_bonus / set_stat_bonus (Con..Tal).
  int stat_type{};
};

// True for the progression commands that the world session applies through Progression.
bool is_progression_gm_command(GmCommandKind kind);

// Authorization, request parsing and response policy stay with GM. This copies only the
// selected progression operation and the three domain fields its transaction consumes.
std::optional<character::ProgressionEdit> character_progression_edit(const GmCommand &command);

struct GmCommandParse {
  std::vector<GmCommand> commands;
  // Empty on success. Otherwise a short reason naming the command or GmCmd field; it never
  // repeats free-form request text, so it is safe to log.
  std::string error;
};

bool is_gm_command_opcode(std::uint16_t opcode);

// Parses a decoded GmCmdTextReq or GmCmdReq message (protobuf JSON with original field names).
//
// Text grammar, ASCII case-insensitive, whitespace-separated:
//   item_add <item_cms_id> [count]     aliases: itemadd, additem
//   add_money <gold_delta>             aliases: addmoney, money, gold
//   set_weapon_durability <value>      aliases: weapon_durability, durability
//   itembag_clear                      alias: clear_bag
//   exp_up <exp_delta>                 aliases: expup, exp
//   set_level <level>                  aliases: setlevel, level
//   add_char_build_point <type> <n>    type: 1|levelup, 2|weapon
//   set_char_build_point <type> <n>
//   reset_stat_bonus
//   force_reset_perk
//   add_stat_bonus <stat> <n>          stat: con|dex|end|int|str|tal or 1-6
//   set_stat_bonus <stat> <n>
//   learn_all_perks
// GmCmdReq accepts GmCmd.item_add {cms_id, count}, GmCmd.add_money {amount, type=Gold},
// GmCmd.set_weapon_durability {durability}, GmCmd.itembag_clear {equip=false}, GmCmd.exp_up
// {delta}, GmCmd.set_level {level},
// GmCmd.add_char_build_point and GmCmd.set_char_build_point {type, value},
// GmCmd.reset_stat_bonus {}, GmCmd.force_reset_perk {}, GmCmd.add_stat_bonus and
// GmCmd.set_stat_bonus {type, value} and GmCmd.learn_all_perks {}.
// A request with any unsupported command is rejected as a whole.
GmCommandParse parse_gm_request(std::uint16_t opcode, const nlohmann::json &message);

struct GmCommandPlan {
  x5::ec::EErrorCode error{x5::ec::Success};
  // Resulting state ready for one atomic commit; shop_id is unused.
  ShopTrade trade;
};

// Plans one command against the character's owned items and progress. No state changes.
// Item grants follow the same storage rules as NPC purchases. Gold stays within
// [0, Money.limitUserMax]. Durability changes are reported as repaired items and committed
// with their expected previous value.
GmCommandPlan plan_gm_command(const ShopCatalog &catalog, const GmCommand &command,
                              const Character &character, const WorldProgress &before);

// Preserve the existing GM API names for the shared, real item-grant plan values.
using GmBundleItem = ItemBundleEntry;
using GmBundlePlan = ItemBundlePlan;

// Plans several item grants and an optional gold top-up as one atomic trade. `gold_target` raises
// the balance to that amount (capped at Money.limitUserMax) and never lowers it.
// `maximum_instances` bounds the equipment instances this one plan may add.
GmBundlePlan plan_gm_grant_bundle(const ShopCatalog &catalog,
                                  const std::vector<GmBundleItem> &items,
                                  std::optional<std::int64_t> gold_target,
                                  const Character &character, const WorldProgress &before,
                                  int maximum_instances);

// ItemsUpdateNoti (169) with the `command` reason for one committed plan. Candidate: the
// schema has Reason_Command, but no official GM grant was captured.
GameplayPacket gm_command_packet(const ShopTrade &trade,
                                 const std::vector<CharacterItem> &committed_grants);

// Operator-facing command name for logs, e.g. "item_add".
const char *gm_command_name(GmCommandKind kind);

// Shared text grammar helpers for other GM command families (social_gm_commands.cpp).
// Splits a console line on ASCII whitespace.
std::vector<std::string_view> split_gm_command_tokens(std::string_view text);

// A complete base-10 token such as "-12"; nullopt for "12abc", "1e3" or out-of-range values.
std::optional<std::int64_t> parse_gm_integer(std::string_view token);

} // namespace chronicles
