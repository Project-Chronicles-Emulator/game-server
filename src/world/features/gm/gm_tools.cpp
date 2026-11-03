#include "world/features/gm/gm_tools.hpp"

#include "cms.generated.pb.h"
#include "world/core/world_request_opcodes.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <system_error>

namespace chronicles::gm {
namespace {

using Json = nlohmann::json;

constexpr double kMillimetersPerMeter = 1000.0;
constexpr double kBesideOffsetMeters = 1.5;
constexpr double kSpawnRadiusMeters = 3.0;

// GM spawn ids: 0xF0000000 | character (12 bits) << 16 | sequence (12 bits) << 4 | NPC nibble 2.
// The client types units by the low nibble of the id (2 = NPC).
constexpr std::uint64_t kSpawnedNpcIdBase = 0xF0000000ULL;
constexpr std::uint64_t kNpcUnitTypeNibble = 0x2ULL;
constexpr std::uint64_t kTwelveBitMask = 0xFFFULL;

constexpr std::uint16_t kGmCommandRequestOpcode = world_request::kGmCommandRequest;
constexpr std::uint16_t kGmCommandTextRequestOpcode = world_request::kGmCommandTextRequest;
constexpr std::size_t kMaximumTextBytes = 256;

SessionParse not_matched() {
  return SessionParse{};
}

SessionParse matched_error(std::string usage) {
  SessionParse parse;
  parse.matched = true;
  parse.error = std::move(usage);
  return parse;
}

SessionParse matched_command(SessionCommand command) {
  SessionParse parse;
  parse.matched = true;
  parse.command = std::move(command);
  return parse;
}

std::string lowercase(std::string_view text) {
  std::string lowered(text);
  for (char &character : lowered) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return lowered;
}

bool is_space(char character) {
  return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

template <typename Number> std::optional<Number> parse_number(std::string_view token) {
  Number value{};
  const auto *end = token.data() + token.size();
  const auto [parsed_end, error] = std::from_chars(token.data(), end, value);
  if (error != std::errc() || parsed_end != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> parse_coordinate(std::string_view token) {
  const auto value = parse_number<double>(token);
  if (!value || !std::isfinite(*value) || std::abs(*value) > kMaximumCoordinateMeters) {
    return std::nullopt;
  }
  return value;
}

// "on"/"off"/"1"/"0"/"true"/"false"; absent means flip.
std::optional<std::optional<bool>> parse_toggle(const std::vector<std::string_view> &words) {
  if (words.size() == 1) {
    return std::optional<bool>{};
  }
  if (words.size() != 2) {
    return std::nullopt;
  }

  const auto value = lowercase(words[1]);
  if (value == "on" || value == "1" || value == "true") {
    return std::optional<bool>{true};
  }
  if (value == "off" || value == "0" || value == "false") {
    return std::optional<bool>{false};
  }
  return std::nullopt;
}

std::optional<SessionCommandKind> text_kind(const std::string &name) {
  struct Alias {
    const char *name;
    SessionCommandKind kind;
  };
  static constexpr Alias kAliases[] = {
      {"help", SessionCommandKind::help},
      {"commands", SessionCommandKind::help},
      {"pos", SessionCommandKind::position},
      {"where", SessionCommandKind::position},
      {"loc", SessionCommandKind::position},
      {"tp", SessionCommandKind::teleport},
      {"teleport", SessionCommandKind::teleport},
      {"set_pos_and_yaw", SessionCommandKind::teleport},
      {"set_pos", SessionCommandKind::teleport},
      {"up", SessionCommandKind::teleport_relative},
      {"down", SessionCommandKind::teleport_relative},
      {"fwd", SessionCommandKind::teleport_relative},
      {"forward", SessionCommandKind::teleport_relative},
      {"tp_map", SessionCommandKind::teleport_map},
      {"map", SessionCommandKind::teleport_map},
      {"warp", SessionCommandKind::teleport_map},
      {"map_move", SessionCommandKind::teleport_map},
      {"goto", SessionCommandKind::go_to_player},
      {"tp_player", SessionCommandKind::go_to_player},
      {"tpto", SessionCommandKind::go_to_player},
      {"follow_user", SessionCommandKind::go_to_player},
      {"summon", SessionCommandKind::summon_player},
      {"bring", SessionCommandKind::summon_player},
      {"players", SessionCommandKind::list_players},
      {"who", SessionCommandKind::list_players},
      {"god", SessionCommandKind::god_mode},
      {"invincible", SessionCommandKind::god_mode},
      {"invulnerable", SessionCommandKind::god_mode},
      {"invisible", SessionCommandKind::invisible},
      {"invis", SessionCommandKind::invisible},
      {"hide", SessionCommandKind::invisible},
      {"stamina", SessionCommandKind::infinite_stamina},
      {"infinite_stamina", SessionCommandKind::infinite_stamina},
      {"fly", SessionCommandKind::free_movement},
      {"noclip", SessionCommandKind::free_movement},
      {"freemove", SessionCommandKind::free_movement},
      {"debug_ghost", SessionCommandKind::free_movement},
      {"ghost", SessionCommandKind::free_movement},
      {"heal", SessionCommandKind::heal},
      {"restore", SessionCommandKind::heal},
      {"revive", SessionCommandKind::revive},
      {"spawn_npc", SessionCommandKind::spawn_npc},
      {"spawn", SessionCommandKind::spawn_npc},
      {"spawn_monster", SessionCommandKind::spawn_npc},
      {"monster", SessionCommandKind::spawn_npc},
      {"despawn_npcs", SessionCommandKind::despawn_npcs},
      {"clear_spawns", SessionCommandKind::despawn_npcs},
      {"gm_status", SessionCommandKind::status},
      {"status", SessionCommandKind::status},
      {"complete_quests", SessionCommandKind::complete_quests},
      {"finish_quests", SessionCommandKind::complete_quests},
      {"completable_quest_all", SessionCommandKind::complete_quests},
      {"complete_quest", SessionCommandKind::complete_quest},
      {"finish_quest", SessionCommandKind::complete_quest},
      {"reward_quest", SessionCommandKind::complete_quest},
      {"reveal_map", SessionCommandKind::reveal_map},
      {"unlock_map", SessionCommandKind::reveal_map},
      {"suicide", SessionCommandKind::suicide},
      {"kill_me", SessionCommandKind::suicide},
      {"kick", SessionCommandKind::kick},
      {"test_kick", SessionCommandKind::kick},
      {"announce", SessionCommandKind::announce},
      {"notice", SessionCommandKind::announce},
  };

  for (const auto &alias : kAliases) {
    if (name == alias.name) {
      return alias.kind;
    }
  }
  return std::nullopt;
}

SessionParse parse_toggle_command(SessionCommandKind kind,
                                  const std::vector<std::string_view> &words) {
  const auto toggle = parse_toggle(words);
  if (!toggle) {
    return matched_error(std::string(session_command_name(kind)) + " [on|off]");
  }

  SessionCommand command;
  command.kind = kind;
  command.enabled = *toggle;
  return matched_command(command);
}

SessionParse parse_no_arguments(SessionCommandKind kind,
                                const std::vector<std::string_view> &words) {
  if (words.size() != 1) {
    return matched_error(std::string(session_command_name(kind)) + " takes no arguments");
  }

  SessionCommand command;
  command.kind = kind;
  return matched_command(command);
}

SessionParse parse_help(const std::vector<std::string_view> &words) {
  SessionCommand command;
  command.kind = SessionCommandKind::help;
  if (words.size() == 1) {
    return matched_command(command);
  }

  const auto page = words.size() == 2 ? parse_number<int>(words[1]) : std::nullopt;
  if (!page || *page < 1 || *page > kHelpPageCount) {
    return matched_error("help [1-" + std::to_string(kHelpPageCount) + "]");
  }
  command.help_page = *page;
  return matched_command(command);
}

SessionParse parse_teleport(const std::vector<std::string_view> &words) {
  if (words.size() != 4 && words.size() != 5) {
    return matched_error("tp <x> <y> <z> [yaw] (meters, degrees, current map)");
  }

  const auto x = parse_coordinate(words[1]);
  const auto y = parse_coordinate(words[2]);
  const auto z = parse_coordinate(words[3]);
  if (!x || !y || !z) {
    return matched_error("tp coordinates must be numbers in meters");
  }

  SessionCommand command;
  command.kind = SessionCommandKind::teleport;
  command.x_meters = *x;
  command.y_meters = *y;
  command.z_meters = *z;
  command.has_coordinates = true;
  if (words.size() == 5) {
    const auto yaw = parse_number<float>(words[4]);
    if (!yaw || !std::isfinite(*yaw)) {
      return matched_error("tp yaw must be a number of degrees");
    }
    command.yaw = *yaw;
  }
  return matched_command(command);
}

SessionParse parse_relative(const std::string &name, const std::vector<std::string_view> &words) {
  if (words.size() != 2) {
    return matched_error(name + " <meters>");
  }

  const auto distance = parse_number<double>(words[1]);
  if (!distance || !std::isfinite(*distance) || *distance <= 0.0 ||
      *distance > kMaximumRelativeTeleportMeters) {
    return matched_error(name + " distance must be 0-" +
                         std::to_string(static_cast<int>(kMaximumRelativeTeleportMeters)) +
                         " meters");
  }

  SessionCommand command;
  command.kind = SessionCommandKind::teleport_relative;
  command.z_meters = *distance;
  if (name == "up") {
    command.direction = RelativeDirection::up;
  } else if (name == "down") {
    command.direction = RelativeDirection::down;
  } else {
    command.direction = RelativeDirection::forward;
  }
  return matched_command(command);
}

SessionParse parse_teleport_map(const std::vector<std::string_view> &words) {
  if (words.size() != 2 && words.size() != 5) {
    return matched_error("tp_map <map_id> [<x> <y> <z>] (meters)");
  }

  const auto map_id = parse_number<int>(words[1]);
  if (!map_id || *map_id <= 0) {
    return matched_error("tp_map map_id must be a positive integer");
  }

  SessionCommand command;
  command.kind = SessionCommandKind::teleport_map;
  command.map_id = *map_id;
  if (words.size() == 5) {
    const auto x = parse_coordinate(words[2]);
    const auto y = parse_coordinate(words[3]);
    const auto z = parse_coordinate(words[4]);
    if (!x || !y || !z) {
      return matched_error("tp_map coordinates must be numbers in meters");
    }
    command.x_meters = *x;
    command.y_meters = *y;
    command.z_meters = *z;
    command.has_coordinates = true;
  }
  return matched_command(command);
}

SessionParse parse_player_target(SessionCommandKind kind,
                                 const std::vector<std::string_view> &words) {
  if (words.size() != 2 || words[1].size() > kMaximumPlayerNameBytes) {
    return matched_error(std::string(session_command_name(kind)) + " <character name>");
  }

  SessionCommand command;
  command.kind = kind;
  command.player_name = std::string(words[1]);
  return matched_command(command);
}

SessionParse parse_spawn(const std::vector<std::string_view> &words) {
  if (words.size() != 2 && words.size() != 3) {
    return matched_error("spawn_npc <npc_cms_id> [count 1-" +
                         std::to_string(kMaximumSpawnedNpcsPerCommand) + "]");
  }

  const auto npc_cms_id = parse_number<int>(words[1]);
  if (!npc_cms_id || *npc_cms_id <= 0) {
    return matched_error("spawn_npc npc_cms_id must be a positive integer");
  }

  int count = 1;
  if (words.size() == 3) {
    const auto parsed = parse_number<int>(words[2]);
    if (!parsed || *parsed < 1 || *parsed > kMaximumSpawnedNpcsPerCommand) {
      return matched_error("spawn_npc count must be 1-" +
                           std::to_string(kMaximumSpawnedNpcsPerCommand));
    }
    count = *parsed;
  }

  SessionCommand command;
  command.kind = SessionCommandKind::spawn_npc;
  command.npc_cms_id = *npc_cms_id;
  command.count = count;
  return matched_command(command);
}

SessionParse parse_complete_quests(const std::vector<std::string_view> &words) {
  SessionCommand command;
  command.kind = SessionCommandKind::complete_quests;
  if (words.size() == 1) {
    return matched_command(command);
  }
  if (words.size() == 2 && lowercase(words[1]) == "chain") {
    command.chain = true;
    return matched_command(command);
  }
  return matched_error("complete_quests [chain]");
}

SessionParse parse_complete_quest(const std::vector<std::string_view> &words) {
  const auto quest_id = words.size() == 2 ? parse_number<int>(words[1]) : std::nullopt;
  if (!quest_id || *quest_id <= 0) {
    return matched_error("complete_quest <quest_id>");
  }

  SessionCommand command;
  command.kind = SessionCommandKind::complete_quest;
  command.quest_id = *quest_id;
  return matched_command(command);
}

std::optional<int> valid_kick_delay(std::int64_t seconds) {
  if (seconds < 0 || seconds > kMaximumKickDelaySeconds) {
    return std::nullopt;
  }
  return static_cast<int>(seconds);
}

SessionParse parse_kick(const std::vector<std::string_view> &words) {
  const auto usage = "kick [seconds 0-" + std::to_string(kMaximumKickDelaySeconds) + "]";
  SessionCommand command;
  command.kind = SessionCommandKind::kick;
  if (words.size() == 1) {
    return matched_command(command);
  }

  const auto parsed = words.size() == 2 ? parse_number<std::int64_t>(words[1]) : std::nullopt;
  const auto delay = parsed ? valid_kick_delay(*parsed) : std::nullopt;
  if (!delay) {
    return matched_error(usage);
  }
  command.delay_seconds = *delay;
  return matched_command(command);
}

std::optional<std::string> valid_announcement(std::string_view text) {
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  if (text.empty() || text.size() > kMaximumAnnouncementBytes) {
    return std::nullopt;
  }
  return std::string(text);
}

// The announcement keeps the line's own spacing after the command word.
SessionParse parse_announce(std::string_view line, const std::vector<std::string_view> &words) {
  const auto usage = "announce <text> (1-" + std::to_string(kMaximumAnnouncementBytes) + " bytes)";
  if (words.size() < 2) {
    return matched_error(usage);
  }

  const auto text_start = static_cast<std::size_t>(words[1].data() - line.data());
  const auto announcement = valid_announcement(line.substr(text_start));
  if (!announcement) {
    return matched_error(usage);
  }

  SessionCommand command;
  command.kind = SessionCommandKind::announce;
  command.announcement = *announcement;
  return matched_command(command);
}

SessionParse parse_text(const Json &message) {
  const auto text = message.find("text");
  if (text == message.end() || !text->is_string()) {
    return not_matched();
  }

  const auto &line = text->get_ref<const std::string &>();
  if (line.size() > kMaximumTextBytes) {
    return not_matched();
  }
  const auto words = split_command_words(line);
  if (words.empty()) {
    return not_matched();
  }

  const auto name = lowercase(words.front());
  const auto kind = text_kind(name);
  if (!kind) {
    return not_matched();
  }

  switch (*kind) {
  case SessionCommandKind::help:
    return parse_help(words);
  case SessionCommandKind::teleport:
    return parse_teleport(words);
  case SessionCommandKind::teleport_relative:
    return parse_relative(name, words);
  case SessionCommandKind::teleport_map:
    return parse_teleport_map(words);
  case SessionCommandKind::go_to_player:
  case SessionCommandKind::summon_player:
    return parse_player_target(*kind, words);
  case SessionCommandKind::god_mode:
  case SessionCommandKind::invisible:
  case SessionCommandKind::infinite_stamina:
  case SessionCommandKind::free_movement:
    return parse_toggle_command(*kind, words);
  case SessionCommandKind::spawn_npc:
    return parse_spawn(words);
  case SessionCommandKind::complete_quests:
    return parse_complete_quests(words);
  case SessionCommandKind::complete_quest:
    return parse_complete_quest(words);
  case SessionCommandKind::kick:
    return parse_kick(words);
  case SessionCommandKind::announce:
    return parse_announce(line, words);
  case SessionCommandKind::position:
  case SessionCommandKind::list_players:
  case SessionCommandKind::heal:
  case SessionCommandKind::revive:
  case SessionCommandKind::despawn_npcs:
  case SessionCommandKind::status:
  case SessionCommandKind::reveal_map:
  case SessionCommandKind::suicide:
    return parse_no_arguments(*kind, words);
  }
  return not_matched();
}

// GmCmd.test_kick.after_sec: an omitted int32 is zero (close now).
std::optional<int> structured_kick_delay(const Json &body) {
  const auto after = body.find("after_sec");
  if (after == body.end()) {
    return 0;
  }
  if (!after->is_number_integer()) {
    return std::nullopt;
  }
  return valid_kick_delay(after->get<std::int64_t>());
}

// Positive schema int32 values are range-checked before narrowing, including direct JSON calls.
std::optional<int> positive_int32_field(const Json &body, const char *name) {
  const auto value = body.find(name);
  if (value == body.end() || !value->is_number_integer() || *value <= 0 ||
      *value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return value->get<int>();
}

// GmCmd.map_move {map_id, pos} and GmCmd.set_pos_and_yaw {unit_id, pos, yaw} for the GM's own
// character. Candidate units: the float positions are taken as meters, like the text grammar.
SessionParse parse_structured_position(const std::string &field, const Json &body) {
  std::optional<std::vector<double>> position;
  const auto values = body.find("pos");
  if (values != body.end() && values->is_array() && values->size() == 3) {
    std::vector<double> coordinates;
    for (const auto &value : *values) {
      if (!value.is_number() || !std::isfinite(value.get<double>()) ||
          std::abs(value.get<double>()) > kMaximumCoordinateMeters) {
        return matched_error("GmCmd." + field + " pos must hold three coordinates in meters");
      }
      coordinates.push_back(value.get<double>());
    }
    position = std::move(coordinates);
  }

  if (values != body.end() && (!values->is_array() || values->size() != 3)) {
    return matched_error("GmCmd." + field + " pos must hold three coordinates in meters");
  }
  SessionCommand command;
  if (position) {
    command.x_meters = (*position)[0];
    command.y_meters = (*position)[1];
    command.z_meters = (*position)[2];
    command.has_coordinates = true;
  }

  if (field == "map_move") {
    // Only explicit map/coordinate travel is modeled. Refuse selector-based requests rather
    // than silently moving to the current coordinates instead of the selected spawn point.
    for (const auto *selector : {"spawn_id", "npc", "doodad", "gathering", "gathering_index",
                                 "npc_spawner", "portal", "trade_post", "sector"}) {
      const auto value = body.find(selector);
      if (value != body.end() && *value != 0 && *value != Json::array()) {
        return matched_error("GmCmd.map_move supports only map_id and explicit pos coordinates");
      }
    }
    const auto map_id = positive_int32_field(body, "map_id");
    if (!map_id) {
      return matched_error("GmCmd.map_move needs a positive map_id");
    }
    command.kind = SessionCommandKind::teleport_map;
    command.map_id = *map_id;
    return matched_command(command);
  }

  const auto unit_id = body.find("unit_id");
  const bool other_unit =
      unit_id != body.end() && !((unit_id->is_string() && unit_id->get<std::string>() == "0") ||
                                 (unit_id->is_number_integer() && *unit_id == 0));
  if (other_unit || !position) {
    return matched_error("GmCmd.set_pos_and_yaw moves only your own character (use move_unit)");
  }
  command.kind = SessionCommandKind::teleport;
  const auto yaw = body.find("yaw");
  if (yaw != body.end()) {
    if (!yaw->is_number() || !std::isfinite(yaw->get<double>()) ||
        std::abs(yaw->get<double>()) > std::numeric_limits<float>::max()) {
      return matched_error("GmCmd.set_pos_and_yaw needs a finite yaw");
    }
    command.yaw = yaw->get<float>();
  }
  return matched_command(command);
}

// Structured GmCmd oneofs handled here. Toggle bodies carry a bool `value` (or `on` for
// fly_mode); an omitted bool is false in protobuf JSON.
SessionParse parse_structured(const Json &message) {
  const auto commands = message.find("cmds");
  if (commands == message.end() || !commands->is_array() || commands->size() != 1) {
    return not_matched();
  }

  const auto &entry = commands->front();
  if (!entry.is_object() || entry.size() != 1 || !entry.begin().value().is_object()) {
    return not_matched();
  }

  const auto &field = entry.begin().key();
  const auto &body = entry.begin().value();
  const auto bool_field = [&body](const char *name) {
    const auto value = body.find(name);
    return value != body.end() && value->is_boolean() && value->get<bool>();
  };

  SessionCommand command;
  if (field == "invincible") {
    command.kind = SessionCommandKind::god_mode;
    command.enabled = bool_field("value");
  } else if (field == "invisible") {
    command.kind = SessionCommandKind::invisible;
    command.enabled = bool_field("value");
  } else if (field == "fly_mode") {
    command.kind = SessionCommandKind::free_movement;
    command.enabled = bool_field("on");
  } else if (field == "revive") {
    command.kind = SessionCommandKind::revive;
  } else if (field == "completable_quest_all") {
    command.kind = SessionCommandKind::complete_quests;
  } else if (field == "completable_quest" || field == "complete_quest_progress" ||
             field == "reward_quest") {
    const auto cms_id = positive_int32_field(body, "cms_id");
    if (!cms_id) {
      return matched_error("GmCmd." + field + " needs a positive cms_id");
    }
    command.kind = SessionCommandKind::complete_quest;
    command.quest_id = *cms_id;
  } else if (field == "spawn_npc") {
    const auto cms_id = positive_int32_field(body, "cms_id");
    if (!cms_id) {
      return matched_error("GmCmd.spawn_npc needs a positive cms_id");
    }
    command.kind = SessionCommandKind::spawn_npc;
    command.npc_cms_id = *cms_id;
  } else if (field == "regen") {
    command.kind = SessionCommandKind::heal;
  } else if (field == "debug_ghost") {
    command.kind = SessionCommandKind::free_movement;
    command.enabled = bool_field("value");
  } else if (field == "map_move" || field == "set_pos_and_yaw") {
    return parse_structured_position(field, body);
  } else if (field == "follow_user") {
    const auto follower = body.find("follower_char_id");
    if (follower != body.end() &&
        !((follower->is_string() && follower->get<std::string>() == "0") ||
          (follower->is_number_integer() && *follower == 0))) {
      return matched_error("GmCmd.follow_user supports only the issuing character as follower");
    }
    const auto character_id = body.find("char_id");
    const auto parsed = character_id != body.end() && character_id->is_string()
                            ? parse_number<std::int64_t>(character_id->get<std::string>())
                            : std::nullopt;
    if (!parsed || *parsed <= 0) {
      return matched_error("GmCmd.follow_user needs a char_id");
    }
    command.kind = SessionCommandKind::go_to_player;
    command.character_id = *parsed;
  } else if (field == "despawn_npc_all") {
    command.kind = SessionCommandKind::despawn_npcs;
  } else if (field == "suicide") {
    command.kind = SessionCommandKind::suicide;
  } else if (field == "test_kick") {
    const auto seconds = structured_kick_delay(body);
    if (!seconds) {
      return matched_error("GmCmd.test_kick after_sec must be 0-" +
                           std::to_string(kMaximumKickDelaySeconds));
    }
    command.kind = SessionCommandKind::kick;
    command.delay_seconds = *seconds;
  } else if (field == "announce") {
    const auto message = body.find("message");
    const auto announcement = message != body.end() && message->is_string()
                                  ? valid_announcement(message->get_ref<const std::string &>())
                                  : std::nullopt;
    if (!announcement) {
      return matched_error("GmCmd.announce needs a message of 1-" +
                           std::to_string(kMaximumAnnouncementBytes) + " bytes");
    }
    command.kind = SessionCommandKind::announce;
    command.announcement = *announcement;
  } else {
    return not_matched();
  }
  return matched_command(command);
}

double degrees_to_radians(double degrees) {
  return degrees * std::numbers::pi / 180.0;
}

std::string meters_text(std::int32_t millimeters) {
  const double meters = static_cast<double>(millimeters) / kMillimetersPerMeter;
  char buffer[32]{};
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof(buffer), meters, std::chars_format::fixed, 2);
  if (error != std::errc()) {
    return "?";
  }
  return std::string(buffer, end);
}

} // namespace

SessionParse parse_session_command(std::uint16_t opcode, const nlohmann::json &message) {
  if (!message.is_object()) {
    return not_matched();
  }
  if (opcode == kGmCommandTextRequestOpcode) {
    return parse_text(message);
  }
  if (opcode == kGmCommandRequestOpcode) {
    return parse_structured(message);
  }
  return not_matched();
}

std::vector<std::string> help_lines(int page) {
  switch (page) {
  case 1:
    return {
        "=== GM commands (1/5): movement and protection ===",
        "Type in chat: /gm <command>   (or open the ~ console and type <command>)",
        "pos - show your map and coordinates in meters",
        "tp <x> <y> <z> [yaw] - teleport in this map (meters; facing in degrees)",
        "tp_map <map_id> [x y z] - load another map (keeps x y z if omitted)",
        "up|down|fwd <meters> - move through walls/terrain (free-movement teleport)",
        "goto <name> - teleport to an online character",
        "summon <name> - bring an online character to you",
        "players - list online characters",
        "god [on|off] - take no damage (also falls)",
        "invisible [on|off] - hidden from players and monsters",
        "stamina [on|off] - infinite stamina (sprint/swim)",
        "fly [on|off] - free movement: god, infinite stamina, no fall damage",
        "heal - restore HP and stamina   revive - revive yourself   suicide - die",
        "status - show your GM toggles   help 2 / help 3 / help 4 - more commands (help 5: combat, "
        "quests, mail)",
    };
  case 2:
    return {
        "=== GM commands (2/5): items, money and character ===",
        "item_add <item_id> [count] (itemadd, additem) - give yourself items",
        "add_money <gold> (money, gold) - add gold (negative removes)",
        "set_weapon_durability <value> - set the durability of your equipped weapons",
        "set_level <level> (level) - set your character level",
        "exp_up <exp> (exp) - add experience",
        "add_char_build_point <levelup|weapon> <n> - add build points",
        "set_char_build_point <levelup|weapon> <n> - set build points",
        "reset_stat_bonus - reset allocated stat points",
        "force_reset_perk - reset perks",
        "spawn_npc <npc_id> [count] (spawn, monster) - spawn NPCs/monsters near you",
        "despawn_npcs - remove the NPCs you spawned",
        "reveal_map (unlock_map) - reveal all catalog fog areas; reselect character to reload",
        "complete_quests - complete every quest in progress (rewards and EXP included)",
        "complete_quests chain - repeat until no quest is left (follows story chains)",
        "complete_quest <quest_id> - complete one quest (accepts it first if needed)",
        "emotion_add <emotion_id> (add_emotion) - grant a non-Basic emote",
    };
  case 3:
    return {
        "=== GM commands (3/5): vehicles, mounts, pets and housing ===",
        "spawn_vehicle <vehicle_id> (spawn_operable) - spawn a vehicle (shared maps)",
        "car - call a car (the catalog's wagon); every player may use it, not only GMs",
        "damage_vehicle <amount> - damage your vehicle",
        "register_summon_npc <id> / spawn_summon_npc <id> / despawn_summon_npc - mounts",
        "register_mate_npc <id> / spawn_mate_npc / despawn_mate_npc / invite_mate_npc - mates",
        "add_mate_intimate <id> <value> - mate intimacy",
        "trade_voucher_add <id> [valid_minutes] - one voucher, optional validity duration",
        "housing_contract <small|medium|large> [monthly|periodic] - rent a lot (item 123 permits)",
        "teleport_to_home - go to your house   housing_unlock <small|medium|large|guild>",
        "release_housing [lot] / housing_remained_expiry_minutes <min> / housing_account_reset",
    };
  case 4:
    return {
        "=== GM commands (4/5): diagnostics and server ===",
        "game_time [stop] <hour 0-24> [day_seconds] (time) - set your sky clock",
        "regen_stat - show your HP/stamina regen stats in the debug panel",
        "watch <unit_id|me|spawned> [position|path|ai|all] - stream debug overlays",
        "unwatch [unit_id] - stop watching (all units when omitted)",
        "channel_event_info - channel event debug panel for this map",
        "announce <text> (notice) - World chat notice to every player",
        "kick [seconds 0-60] (test_kick) - disconnect yourself",
    };
  case 5:
    return {
        "=== GM commands (5/5): combat, quests and delivery ===",
        "monster_kill <unit|spawned> / damage_monster <unit|spawned> <amount|percent%>",
        "Targets: your own character (me), or your private spawn_npc units (spawned).",
        "add_status_effect <unit|me> <id> / remove_status_effect <unit|me> <id>",
        "add_abnormal_status_gauge <unit|me> <burning|freezing|shock|bleeding|poison|stun|shrink> "
        "<n>",
        "npc_ai_on <unit|spawned> / npc_ai_off <unit|spawned> - resume/pause private NPCs",
        "cancel_skill [unit|me] / cooldown - cancel a skill or clear server cooldowns",
        "move_unit <unit|me|spawned> <x> <y> <z> [rel] - place units (meters)",
        "set_gm_stat <stat> <value> / add_gm_stat <stat> <value|percent%> (temporary)",
        "set_gm_stat clear / execute_skill_effect <id> (supported self effects only)",
        "accept_quest <id> / accept_quest_if_passed_cond <id> - shared maps only",
        "add_stat_bonus <con|dex|end|int|str|tal> <n> / set_stat_bonus <stat> <n>",
        "learn_all_perks - grant compatible class perks (exclusive choices preserved)",
        "itembag_clear - remove unequipped instances and unreserved stack units",
        "mail_send <online-name|me> [item <id> [count] | gold <amount>]",
    };
  default:
    return {"help [1-" + std::to_string(kHelpPageCount) + "]"};
  }
}

std::int32_t meters_to_millimeters(double meters) {
  const double clamped = std::clamp(meters, -kMaximumCoordinateMeters, kMaximumCoordinateMeters);
  return static_cast<std::int32_t>(std::llround(clamped * kMillimetersPerMeter));
}

WorldPosition absolute_destination(int map_id, double x_meters, double y_meters, double z_meters,
                                   float yaw) {
  return WorldPosition{map_id, meters_to_millimeters(x_meters), meters_to_millimeters(y_meters),
                       meters_to_millimeters(z_meters), yaw};
}

std::optional<WorldPosition> relative_destination(const WorldPosition &current,
                                                  RelativeDirection direction, double meters) {
  double x = static_cast<double>(current.x_mm) / kMillimetersPerMeter;
  double y = static_cast<double>(current.y_mm) / kMillimetersPerMeter;
  double z = static_cast<double>(current.z_mm) / kMillimetersPerMeter;
  switch (direction) {
  case RelativeDirection::up:
    z += meters;
    break;
  case RelativeDirection::down:
    z -= meters;
    break;
  case RelativeDirection::forward: {
    const double yaw = degrees_to_radians(current.yaw);
    x += std::cos(yaw) * meters;
    y += std::sin(yaw) * meters;
    break;
  }
  }

  const auto in_range = [](double value) {
    return std::abs(value) <= kMaximumCoordinateMeters;
  };
  if (!in_range(x) || !in_range(y) || !in_range(z)) {
    return std::nullopt;
  }
  return absolute_destination(current.map_id, x, y, z, current.yaw);
}

WorldPosition beside(const WorldPosition &target) {
  auto arrival = target;
  const auto offset = meters_to_millimeters(kBesideOffsetMeters);
  if (arrival.x_mm <= std::numeric_limits<std::int32_t>::max() - offset) {
    arrival.x_mm += offset;
  } else {
    arrival.x_mm -= offset;
  }
  return arrival;
}

std::string describe_position(const WorldPosition &position) {
  char yaw[16]{};
  const auto [yaw_end, error] =
      std::to_chars(yaw, yaw + sizeof(yaw), position.yaw, std::chars_format::fixed, 1);
  const std::string yaw_text = error == std::errc() ? std::string(yaw, yaw_end) : "?";
  return "map " + std::to_string(position.map_id) + " x=" + meters_text(position.x_mm) +
         " y=" + meters_text(position.y_mm) + " z=" + meters_text(position.z_mm) +
         " yaw=" + yaw_text + " (meters)";
}

std::string online_player_line(const std::string &name, bool game_master, int map_id) {
  return name + (game_master ? " [GM]" : "") + " - map " + std::to_string(map_id);
}

bool same_character_name(std::string_view first, std::string_view second) {
  return first.size() == second.size() && lowercase(first) == lowercase(second);
}

std::vector<std::string_view> split_command_words(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t position = 0;
  while (position < text.size()) {
    while (position < text.size() && is_space(text[position])) {
      ++position;
    }

    const auto start = position;
    while (position < text.size() && !is_space(text[position])) {
      ++position;
    }
    if (position > start) {
      words.push_back(text.substr(start, position - start));
    }
  }
  return words;
}

std::optional<int> parse_stat_type_token(std::string_view token) {
  constexpr std::string_view kEnumPrefix = "EX5StatType_";
  const auto *descriptor = x5::cms::EX5StatType_descriptor();

  if (const auto number = parse_number<int>(token)) {
    const auto *value = descriptor->FindValueByNumber(*number);
    if (value == nullptr || *number == x5::cms::EX5StatType_None) {
      return std::nullopt;
    }
    return *number;
  }

  const auto wanted = lowercase(token);
  for (int index = 0; index < descriptor->value_count(); ++index) {
    const auto *value = descriptor->value(index);
    std::string_view name = value->name();
    if (name.starts_with(kEnumPrefix)) {
      name.remove_prefix(kEnumPrefix.size());
    }
    if (lowercase(name) == wanted && value->number() != x5::cms::EX5StatType_None) {
      return value->number();
    }
  }
  return std::nullopt;
}

nlohmann::json spawned_npc_message(std::uint64_t unit_id, int npc_cms_id,
                                   const WorldPosition &position, const std::string &faction) {
  Json npc{{"npc_cms_id", npc_cms_id},
           {"base_unit_id", std::to_string(unit_id)},
           {"state", "EX5UnitStateType_Idle"},
           {"faction_type", faction}};
  return {{"unit_id", std::to_string(unit_id)},
          {"spawned_unit", true},
          {"nub",
           {{"pos",
             {{"millimeter_x", position.x_mm},
              {"millimeter_y", position.y_mm},
              {"millimeter_z", position.z_mm}}},
            {"yaw", position.yaw},
            {"npc", std::move(npc)}}}};
}

nlohmann::json removed_units_message(const std::vector<std::uint64_t> &unit_ids) {
  Json ids = Json::array();
  for (const auto unit_id : unit_ids) {
    ids.push_back(std::to_string(unit_id));
  }
  return {{"unit_ids", std::move(ids)}};
}

std::vector<WorldPosition> spawn_positions(const WorldPosition &center, int count) {
  std::vector<WorldPosition> positions;
  if (count <= 0) {
    return positions;
  }

  positions.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    const double angle = degrees_to_radians(center.yaw) +
                         2.0 * std::numbers::pi * index / static_cast<double>(count);
    auto position = center;
    position.x_mm += meters_to_millimeters(std::cos(angle) * kSpawnRadiusMeters);
    position.y_mm += meters_to_millimeters(std::sin(angle) * kSpawnRadiusMeters);
    // Face the GM.
    position.yaw = static_cast<float>(std::fmod(center.yaw + 180.0 + 360.0 * index / count, 360.0));
    positions.push_back(position);
  }
  return positions;
}

std::uint64_t spawned_npc_unit_id(std::int64_t character_id, std::uint32_t sequence) {
  const auto character_bits = static_cast<std::uint64_t>(character_id) & kTwelveBitMask;
  const auto sequence_bits = static_cast<std::uint64_t>(sequence) & kTwelveBitMask;
  return kSpawnedNpcIdBase | (character_bits << 16) | (sequence_bits << 4) | kNpcUnitTypeNibble;
}

const char *session_command_name(SessionCommandKind kind) {
  switch (kind) {
  case SessionCommandKind::help:
    return "help";
  case SessionCommandKind::position:
    return "pos";
  case SessionCommandKind::teleport:
    return "tp";
  case SessionCommandKind::teleport_relative:
    return "up/down/fwd";
  case SessionCommandKind::teleport_map:
    return "tp_map";
  case SessionCommandKind::go_to_player:
    return "goto";
  case SessionCommandKind::summon_player:
    return "summon";
  case SessionCommandKind::list_players:
    return "players";
  case SessionCommandKind::god_mode:
    return "god";
  case SessionCommandKind::invisible:
    return "invisible";
  case SessionCommandKind::infinite_stamina:
    return "stamina";
  case SessionCommandKind::free_movement:
    return "fly";
  case SessionCommandKind::heal:
    return "heal";
  case SessionCommandKind::revive:
    return "revive";
  case SessionCommandKind::spawn_npc:
    return "spawn_npc";
  case SessionCommandKind::despawn_npcs:
    return "despawn_npcs";
  case SessionCommandKind::status:
    return "status";
  case SessionCommandKind::complete_quests:
    return "complete_quests";
  case SessionCommandKind::complete_quest:
    return "complete_quest";
  case SessionCommandKind::reveal_map:
    return "reveal_map";
  case SessionCommandKind::suicide:
    return "suicide";
  case SessionCommandKind::kick:
    return "kick";
  case SessionCommandKind::announce:
    return "announce";
  }
  return "unknown";
}

} // namespace chronicles::gm
