#include "world/features/events/event_gm_commands.hpp"

#include "world/features/gm/gm_commands.hpp"
#include "world/features/gm/gm_tools.hpp"

#include <charconv>
#include <limits>
#include <string_view>

namespace chronicles::event_gm {
namespace {

using Json = nlohmann::json;

struct TextCommand {
  const char *name;
  EventCommandKind kind;
  // Usage shown when the arguments do not match; empty for commands without arguments.
  const char *usage;
};

constexpr TextCommand kTextCommands[] = {
    {"ranking_start", EventCommandKind::ranking_start, "ranking_start <ranking_id>"},
    {"ranking_finish", EventCommandKind::ranking_finish, "ranking_finish <ranking_id>"},
    {"ranking_cancel", EventCommandKind::ranking_cancel, ""},
    {"raid_channel_enter", EventCommandKind::raid_channel_enter, "raid_channel_enter <map_id>"},
    {"complete_channel_event_objective", EventCommandKind::complete_channel_event_objective, ""},
    {"reset_channel_event_save_data", EventCommandKind::reset_channel_event_save_data, ""},
};

std::string ascii_lowercase(std::string_view text) {
  std::string lowered(text);
  for (char &character : lowered) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return lowered;
}

std::optional<int> positive_argument(std::string_view text) {
  int value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value <= 0) {
    return std::nullopt;
  }
  return value;
}

EventCommandParse matched(EventCommand command) {
  return {true, command, {}};
}

EventCommandParse matched_error(std::string usage) {
  return {true, std::nullopt, std::move(usage)};
}

EventCommandParse parse_text(const Json &message) {
  const auto text = message.find("text");
  if (text == message.end() || !text->is_string()) {
    return {};
  }

  const auto line = text->get<std::string>();
  const auto words = gm::split_command_words(line);
  if (words.empty()) {
    return {};
  }

  const auto name = ascii_lowercase(words.front());
  for (const auto &candidate : kTextCommands) {
    if (name != candidate.name) {
      continue;
    }

    const bool takes_argument = candidate.usage[0] != '\0';
    if (!takes_argument) {
      if (words.size() != 1) {
        return matched_error(std::string(candidate.name) + " takes no arguments");
      }
      return matched({candidate.kind, 0});
    }

    const auto argument = words.size() == 2 ? positive_argument(words[1]) : std::nullopt;
    if (!argument) {
      return matched_error(candidate.usage);
    }
    return matched({candidate.kind, *argument});
  }
  return {};
}

EventCommandParse parse_structured(const Json &message) {
  const auto commands = message.find("cmds");
  if (commands == message.end() || !commands->is_array() || commands->size() != 1) {
    return {};
  }

  const auto &entry = commands->front();
  if (!entry.is_object() || entry.size() != 1 || !entry.begin().value().is_object()) {
    return {};
  }

  const auto &field = entry.begin().key();
  const auto &body = entry.begin().value();
  if (field == "raid_channel_enter") {
    const auto map_id = body.find("map_id");
    if (map_id == body.end() || !map_id->is_number_integer() || map_id->get<std::int64_t>() <= 0 ||
        map_id->get<std::int64_t>() > std::numeric_limits<int>::max()) {
      return matched_error("GmCmd.raid_channel_enter needs a positive map_id");
    }
    return matched({EventCommandKind::raid_channel_enter, map_id->get<int>()});
  }
  if (field == "complete_channel_event_objective") {
    return matched({EventCommandKind::complete_channel_event_objective, 0});
  }
  if (field == "reset_channel_event_save_data") {
    return matched({EventCommandKind::reset_channel_event_save_data, 0});
  }
  return {};
}

} // namespace

EventCommandParse parse_event_command(std::uint16_t opcode, const Json &message) {
  if (opcode == kGmCommandTextRequestOpcode) {
    return parse_text(message);
  }
  if (opcode == kGmCommandRequestOpcode) {
    return parse_structured(message);
  }
  return {};
}

std::vector<std::string> help_lines() {
  return {
      "ranking_start <ranking_id> / ranking_finish <ranking_id> / ranking_cancel",
      "raid_channel_enter <map_id> (10103, 20229 or 10301)",
      "channel_event_info / complete_channel_event_objective / reset_channel_event_save_data",
  };
}

const char *command_name(EventCommandKind kind) {
  for (const auto &candidate : kTextCommands) {
    if (candidate.kind == kind) {
      return candidate.name;
    }
  }
  return "unknown";
}

} // namespace chronicles::event_gm
