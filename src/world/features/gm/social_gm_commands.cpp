#include "world/features/gm/social_gm_commands.hpp"

#include "cms.generated.pb.h"
#include "world/features/gm/gm_commands.hpp"
#include "persistence/account_repository.hpp"

#include <limits>
#include <string_view>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr int kMaximumPositionLevel = 100;
constexpr std::int64_t kMaximumPermitMinutes = 60LL * 24 * 365;
constexpr std::int64_t kMaximumContributionDelta = std::numeric_limits<std::int32_t>::max();

SocialGmParse failure(std::string reason) {
  SocialGmParse result;
  result.error = std::move(reason);
  return result;
}

SocialGmParse single(SocialGmCommand command) {
  SocialGmParse result;
  result.commands.push_back(std::move(command));
  return result;
}

// "BlueSalt", "EX5CompanyType_BlueSalt" or the enum number; None is not a company.
std::optional<int> parse_company(std::string_view token) {
  if (const auto number = parse_gm_integer(token)) {
    if (*number > 0 && x5::cms::EX5CompanyType_IsValid(static_cast<int>(*number))) {
      return static_cast<int>(*number);
    }
    return std::nullopt;
  }

  for (int type = 1; x5::cms::EX5CompanyType_IsValid(type); ++type) {
    const auto full = x5::cms::EX5CompanyType_Name(static_cast<x5::cms::EX5CompanyType>(type));
    const auto short_name = full.substr(full.find('_') + 1);
    if (ascii_lower(std::string(token)) == ascii_lower(full) ||
        ascii_lower(std::string(token)) == ascii_lower(short_name)) {
      return type;
    }
  }
  return std::nullopt;
}

bool in_range(std::int64_t value, std::int64_t minimum, std::int64_t maximum) {
  return value >= minimum && value <= maximum;
}

// Text grammar. nullopt means "not a social command".
std::optional<SocialGmParse> parse_text(const Json &message) {
  const auto text = message.find("text");
  if (text == message.end() || !text->is_string()) {
    return std::nullopt;
  }
  const auto tokens = split_gm_command_tokens(text->get_ref<const std::string &>());
  if (tokens.empty()) {
    return std::nullopt;
  }

  const auto name = ascii_lower(std::string(tokens.front()));
  const auto integer_argument = [&](std::size_t index) -> std::optional<std::int64_t> {
    if (index >= tokens.size()) {
      return std::nullopt;
    }
    return parse_gm_integer(tokens[index]);
  };

  SocialGmCommand command;
  if (name == "guild_exp") {
    const auto delta = integer_argument(1);
    if (tokens.size() != 2 || !delta || *delta == 0 ||
        !in_range(*delta, -kMaximumContributionDelta, kMaximumContributionDelta)) {
      return failure("guild_exp expects a nonzero <delta>");
    }
    command.kind = SocialGmCommandKind::guild_experience;
    command.amount = *delta;
    return single(command);
  }
  if (name == "guild_level") {
    const auto level = integer_argument(1);
    if (tokens.size() != 2 || !level || !in_range(*level, 1, kMaximumPositionLevel)) {
      return failure("guild_level expects <level>");
    }
    command.kind = SocialGmCommandKind::guild_level;
    command.amount = *level;
    return single(command);
  }
  if (name == "guild_disband") {
    const auto guild_id = integer_argument(1);
    if (tokens.size() > 2 || (tokens.size() == 2 && (!guild_id || *guild_id <= 0))) {
      return failure("guild_disband expects [guild_id]");
    }
    command.kind = SocialGmCommandKind::guild_disband;
    command.guild_id = guild_id.value_or(0);
    return single(command);
  }
  if (name == "guild_master") {
    const auto char_id = integer_argument(1);
    const auto guild_id = integer_argument(2);
    if (tokens.size() < 2 || tokens.size() > 3 || !char_id || *char_id <= 0 ||
        (tokens.size() == 3 && (!guild_id || *guild_id <= 0))) {
      return failure("guild_master expects <char_id> [guild_id]");
    }
    command.kind = SocialGmCommandKind::guild_master;
    command.char_id = *char_id;
    command.guild_id = guild_id.value_or(0);
    return single(command);
  }
  if (name == "guild_reset_cooldown") {
    const auto char_id = integer_argument(1);
    if (tokens.size() > 2 || (tokens.size() == 2 && (!char_id || *char_id <= 0))) {
      return failure("guild_reset_cooldown expects [char_id]");
    }
    command.kind = SocialGmCommandKind::guild_reset_cooldown;
    command.char_id = char_id.value_or(0);
    return single(command);
  }

  command.kind = SocialGmCommandKind::company;
  if (name == "company") {
    const auto company = tokens.size() >= 2 ? parse_company(tokens[1]) : std::nullopt;
    const auto delta = integer_argument(2);
    if (tokens.size() < 2 || tokens.size() > 3 || !company ||
        (tokens.size() == 3 &&
         (!delta || !in_range(*delta, -kMaximumContributionDelta, kMaximumContributionDelta)))) {
      return failure("company expects <type> [contribution_delta]");
    }
    command.company_type = *company;
    if (delta) {
      command.contribution_delta = *delta;
    }
    return single(command);
  }
  if (name == "company_contribution") {
    const auto delta = integer_argument(1);
    if (tokens.size() != 2 || !delta || *delta == 0 ||
        !in_range(*delta, -kMaximumContributionDelta, kMaximumContributionDelta)) {
      return failure("company_contribution expects a nonzero <delta>");
    }
    command.contribution_delta = *delta;
    return single(command);
  }
  if (name == "company_position" || name == "company_unlock") {
    const auto level = integer_argument(1);
    if (tokens.size() != 2 || !level || !in_range(*level, 1, kMaximumPositionLevel)) {
      return failure(name + " expects <level>");
    }
    if (name == "company_position") {
      command.position_level = static_cast<int>(*level);
    } else {
      command.unlock_level = static_cast<int>(*level);
    }
    return single(command);
  }
  if (name == "pioneer_permit") {
    const auto minutes = integer_argument(1);
    if (tokens.size() != 2 || !minutes || !in_range(*minutes, 0, kMaximumPermitMinutes)) {
      return failure("pioneer_permit expects <minutes>");
    }
    command.permit_minutes = *minutes;
    return single(command);
  }
  return std::nullopt;
}

std::optional<std::int64_t> json_integer(const Json &body, const char *field) {
  const auto value = body.find(field);
  if (value == body.end()) {
    return std::int64_t{};
  }
  if (value->is_number_integer()) {
    return value->get<std::int64_t>();
  }
  if (value->is_string()) {
    return parse_gm_integer(value->get_ref<const std::string &>());
  }
  return std::nullopt;
}

bool social_field(const std::string &name) {
  return name == "guild_exp" || name == "guild_level" || name == "guild_disband" ||
         name == "guild_master_authorization" || name == "guild_reset_cooldown" ||
         name == "company";
}

SocialGmParse parse_structured_entry(const std::string &name, const Json &body) {
  SocialGmCommand command;
  if (name == "guild_exp") {
    const auto delta = json_integer(body, "delta");
    if (!delta || *delta == 0) {
      return failure("GmCmd.guild_exp needs a nonzero delta");
    }
    command.kind = SocialGmCommandKind::guild_experience;
    command.amount = *delta;
  } else if (name == "guild_level") {
    const auto level = json_integer(body, "level");
    if (!level || !in_range(*level, 1, kMaximumPositionLevel)) {
      return failure("GmCmd.guild_level needs a level");
    }
    command.kind = SocialGmCommandKind::guild_level;
    command.amount = *level;
  } else if (name == "guild_disband") {
    const auto guild_id = json_integer(body, "guild_id");
    if (!guild_id || *guild_id < 0) {
      return failure("GmCmd.guild_disband has an invalid guild_id");
    }
    command.kind = SocialGmCommandKind::guild_disband;
    command.guild_id = *guild_id;
  } else if (name == "guild_master_authorization") {
    const auto guild_id = json_integer(body, "guild_id");
    const auto char_id = json_integer(body, "char_id");
    if (!guild_id || *guild_id < 0 || !char_id || *char_id <= 0) {
      return failure("GmCmd.guild_master_authorization needs a char_id");
    }
    command.kind = SocialGmCommandKind::guild_master;
    command.guild_id = *guild_id;
    command.char_id = *char_id;
  } else if (name == "guild_reset_cooldown") {
    command.kind = SocialGmCommandKind::guild_reset_cooldown;
  } else {
    command.kind = SocialGmCommandKind::company;
    const auto type = body.find("type");
    if (type != body.end()) {
      if (!type->is_string()) {
        return failure("GmCmd.company type must be an enum name");
      }
      const auto company = parse_company(type->get_ref<const std::string &>());
      if (!company) {
        return failure("GmCmd.company has an unknown type");
      }
      command.company_type = *company;
    }
    const auto contribution = json_integer(body, "add_contribution");
    const auto permit = json_integer(body, "pioneer_permit");
    const auto position = json_integer(body, "position_lv");
    if (!contribution || !permit || !position || !in_range(*position, 0, kMaximumPositionLevel) ||
        !in_range(*permit, 0, kMaximumPermitMinutes) ||
        !in_range(*contribution, -kMaximumContributionDelta, kMaximumContributionDelta)) {
      return failure("GmCmd.company has an out-of-range field");
    }
    if (*contribution != 0) {
      command.contribution_delta = *contribution;
    }
    if (body.contains("pioneer_permit")) {
      command.permit_minutes = *permit;
    }
    if (*position != 0) {
      command.position_level = static_cast<int>(*position);
    }
    if (body.value("unlock_max_position", false)) {
      command.unlock_level = kMaximumPositionLevel;
    }
  }
  return single(command);
}

std::optional<SocialGmParse> parse_structured(const Json &message) {
  const auto entries = message.find("cmds");
  if (entries == message.end() || !entries->is_array() || entries->empty()) {
    return std::nullopt;
  }

  std::size_t social_entries = 0;
  for (const auto &entry : *entries) {
    if (entry.is_object() && entry.size() == 1 && social_field(entry.begin().key())) {
      ++social_entries;
    }
  }
  if (social_entries == 0) {
    return std::nullopt;
  }
  if (social_entries != entries->size()) {
    return failure("GmCmdReq mixes guild or company commands with other commands");
  }

  SocialGmParse result;
  for (const auto &entry : *entries) {
    if (!entry.begin().value().is_object()) {
      return failure("GmCmd entry must set exactly one command");
    }
    auto parsed = parse_structured_entry(entry.begin().key(), entry.begin().value());
    if (!parsed.error.empty()) {
      return parsed;
    }
    result.commands.insert(result.commands.end(), parsed.commands.begin(),
                           parsed.commands.end());
  }
  return result;
}

} // namespace

std::optional<SocialGmParse> parse_social_gm_request(std::uint16_t opcode,
                                                     const nlohmann::json &message) {
  if (!message.is_object()) {
    return std::nullopt;
  }
  if (opcode == kGmCommandTextRequestOpcode) {
    return parse_text(message);
  }
  if (opcode == kGmCommandRequestOpcode) {
    return parse_structured(message);
  }
  return std::nullopt;
}

const char *social_gm_command_name(SocialGmCommandKind kind) {
  switch (kind) {
  case SocialGmCommandKind::guild_experience:
    return "guild_exp";
  case SocialGmCommandKind::guild_level:
    return "guild_level";
  case SocialGmCommandKind::guild_disband:
    return "guild_disband";
  case SocialGmCommandKind::guild_master:
    return "guild_master";
  case SocialGmCommandKind::guild_reset_cooldown:
    return "guild_reset_cooldown";
  case SocialGmCommandKind::company:
    return "company";
  }
  return "unknown";
}

} // namespace chronicles
