#pragma once

// Guild and company game master commands.
//
// GmCmdReq (20008) fields from the recovered gmcmd.proto: guild_exp {delta}, guild_level
// {level}, guild_disband {guild_id}, guild_master_authorization {guild_id, char_id},
// guild_reset_cooldown {} and company {type, add_contribution, pioneer_permit, position_lv,
// unlock_max_position}. GmCmdTextReq (20009) uses an emulator grammar named after them, because
// the official GMCommand text table is absent from the export:
//   guild_exp <delta>                       the GM's own guild
//   guild_level <level>                     the GM's own guild
//   guild_disband [guild_id]                defaults to the GM's own guild
//   guild_master <char_id> [guild_id]       transfers mastership
//   guild_reset_cooldown [char_id]          defaults to the GM
//   company <type> [contribution_delta]     joins (or switches to) a company; type is a name
//                                           such as BlueSalt or its EX5CompanyType number
//   company_contribution <delta>
//   company_position <level>                sets contribution to the level's threshold and
//                                           unlocks it
//   company_unlock <level>
//   pioneer_permit <minutes>                0 clears the permit
// Authorization is the caller's job, exactly as for the item and money commands.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

enum class SocialGmCommandKind {
  guild_experience,
  guild_level,
  guild_disband,
  guild_master,
  guild_reset_cooldown,
  company,
};

struct SocialGmCommand {
  SocialGmCommandKind kind{SocialGmCommandKind::guild_experience};
  // Zero selects the GM's own guild or character.
  std::int64_t guild_id{};
  std::int64_t char_id{};
  std::int64_t amount{};
  // Company command fields; nullopt leaves that aspect unchanged.
  std::optional<int> company_type;
  std::optional<std::int64_t> contribution_delta;
  std::optional<int> position_level;
  std::optional<int> unlock_level;
  std::optional<std::int64_t> permit_minutes;
};

struct SocialGmParse {
  std::vector<SocialGmCommand> commands;
  // Empty on success; otherwise a short reason that never repeats free-form request text.
  std::string error;
};

// nullopt when the request contains no guild or company command, so the item/money parser
// handles it. A request that mixes both families is refused as a whole.
std::optional<SocialGmParse> parse_social_gm_request(std::uint16_t opcode,
                                                     const nlohmann::json &message);

const char *social_gm_command_name(SocialGmCommandKind kind);

} // namespace chronicles
