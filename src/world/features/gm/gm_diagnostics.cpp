#include "world/features/gm/gm_diagnostics.hpp"

#include "common/clock_ticks.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/gm/gm_tools.hpp"
#include "persistence/account_repository.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>
#include <system_error>

namespace chronicles::gm {
namespace {

using Json = nlohmann::json;

constexpr std::uint16_t kGmCommandRequestOpcode = world_request::kGmCommandRequest;
constexpr std::uint16_t kGmCommandTextRequestOpcode = world_request::kGmCommandTextRequest;
constexpr std::size_t kMaximumTextBytes = 256;
constexpr double kHoursPerDay = 24.0;

// EX5StatType values the regen panel reports (cms enum numbers).
constexpr int kHpRegenStat = 15;
constexpr int kMaxHealthStat = 18;
constexpr int kMaxStaminaStat = 19;
constexpr int kSpRegenStat = 25;
constexpr int kSatietyRegenStat = 26;
constexpr int kHealthStat = 69;
constexpr int kStaminaStat = 72;
constexpr int kRegenPanelStats[] = {
    kHpRegenStat,      kMaxHealthStat, kMaxStaminaStat, kSpRegenStat,
    kSatietyRegenStat, kHealthStat,    kStaminaStat,
};

// ARGB colors of the path overlay: the current point and the route end.
constexpr std::uint32_t kPathCurrentColor = 0xFF00FF00;
constexpr std::uint32_t kPathDestinationColor = 0xFFFFFF00;

// x5.pb.debug.BtNode.Result.
constexpr const char *kBehaviorNodeRunning = "Running";

DiagnosticParse not_matched() {
  return DiagnosticParse{};
}

DiagnosticParse matched_error(std::string usage) {
  DiagnosticParse parse;
  parse.matched = true;
  parse.error = std::move(usage);
  return parse;
}

DiagnosticParse matched_command(DiagnosticCommand command) {
  DiagnosticParse parse;
  parse.matched = true;
  parse.command = std::move(command);
  return parse;
}

DiagnosticParse matched_kind(DiagnosticKind kind) {
  DiagnosticCommand command;
  command.kind = kind;
  return matched_command(std::move(command));
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

std::optional<double> valid_hour(double hour) {
  if (!std::isfinite(hour) || hour < 0.0 || hour > kHoursPerDay) {
    return std::nullopt;
  }
  return hour;
}

bool valid_day_seconds(std::int64_t seconds) {
  return seconds >= kMinimumGameDaySeconds && seconds <= kMaximumGameDaySeconds;
}

std::optional<DiagnosticKind> text_kind(const std::string &name) {
  if (name == "game_time" || name == "time") {
    return DiagnosticKind::game_time;
  }
  if (name == "regen_stat" || name == "regen_stats" || name == "debug_player_regen_stat") {
    return DiagnosticKind::regen_stat;
  }
  if (name == "watch" || name == "debug_watch") {
    return DiagnosticKind::watch;
  }
  if (name == "unwatch") {
    return DiagnosticKind::unwatch;
  }
  if (name == "channel_event_info" || name == "channel_event") {
    return DiagnosticKind::channel_event_info;
  }
  if (name == "debug_npc_field") {
    return DiagnosticKind::npc_field;
  }
  return std::nullopt;
}

DiagnosticParse parse_text_game_time(const std::vector<std::string_view> &words) {
  constexpr const char *kUsage = "game_time [stop] <hour 0-24> [day_seconds 60-604800]";
  std::size_t index = 1;
  DiagnosticCommand command;
  command.kind = DiagnosticKind::game_time;
  if (index < words.size() && ascii_lower(std::string(words[index])) == "stop") {
    command.game_time.stopped = true;
    ++index;
  }

  const std::size_t remaining = words.size() - index;
  if (remaining != 1 && remaining != 2) {
    return matched_error(kUsage);
  }

  const auto parsed_hour = parse_number<double>(words[index]);
  const auto hour = parsed_hour ? valid_hour(*parsed_hour) : std::nullopt;
  if (!hour) {
    return matched_error(kUsage);
  }
  command.game_time.hour = *hour;

  if (remaining == 2) {
    const auto day_seconds = parse_number<int>(words[index + 1]);
    if (!day_seconds || !valid_day_seconds(*day_seconds)) {
      return matched_error(kUsage);
    }
    command.game_time.day_seconds = *day_seconds;
  }
  return matched_command(std::move(command));
}

std::optional<std::vector<WatchType>> text_watch_types(std::string_view token) {
  const auto name = ascii_lower(std::string(token));
  if (name == "position" || name == "pos") {
    return std::vector<WatchType>{WatchType::position};
  }
  if (name == "path") {
    return std::vector<WatchType>{WatchType::path};
  }
  if (name == "ai") {
    return std::vector<WatchType>{WatchType::ai};
  }
  if (name == "all") {
    return std::vector<WatchType>{WatchType::position, WatchType::path, WatchType::ai};
  }
  return std::nullopt;
}

DiagnosticParse parse_text_watch(const std::vector<std::string_view> &words) {
  constexpr const char *kUsage = "watch <unit_id|me|spawned> [position|path|ai|all]";
  if (words.size() != 2 && words.size() != 3) {
    return matched_error(kUsage);
  }

  DiagnosticCommand command;
  command.kind = DiagnosticKind::watch;
  const auto target = ascii_lower(std::string(words[1]));
  if (target == "me") {
    command.target = WatchTarget::self;
  } else if (target == "spawned") {
    command.target = WatchTarget::spawned;
  } else {
    const auto unit_id = parse_number<std::uint64_t>(words[1]);
    if (!unit_id || *unit_id == 0) {
      return matched_error(kUsage);
    }
    command.unit_id = *unit_id;
  }

  command.watch_types = {WatchType::position};
  if (words.size() == 3) {
    const auto types = text_watch_types(words[2]);
    if (!types) {
      return matched_error(kUsage);
    }
    command.watch_types = *types;
  }
  return matched_command(std::move(command));
}

DiagnosticParse parse_text_unwatch(const std::vector<std::string_view> &words) {
  DiagnosticCommand command;
  command.kind = DiagnosticKind::unwatch;
  if (words.size() == 1) {
    return matched_command(std::move(command));
  }

  const auto unit_id = words.size() == 2 ? parse_number<std::uint64_t>(words[1]) : std::nullopt;
  if (!unit_id || *unit_id == 0) {
    return matched_error("unwatch [unit_id]");
  }
  command.unit_id = *unit_id;
  return matched_command(std::move(command));
}

DiagnosticParse parse_text(const Json &message) {
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

  const auto kind = text_kind(ascii_lower(std::string(words.front())));
  if (!kind) {
    return not_matched();
  }

  switch (*kind) {
  case DiagnosticKind::game_time:
    return parse_text_game_time(words);
  case DiagnosticKind::watch:
    return parse_text_watch(words);
  case DiagnosticKind::unwatch:
    return parse_text_unwatch(words);
  case DiagnosticKind::regen_stat:
  case DiagnosticKind::channel_event_info:
  case DiagnosticKind::npc_field:
    if (words.size() != 1) {
      return matched_error(std::string(diagnostic_name(*kind)) + " takes no arguments");
    }
    return matched_kind(*kind);
  }
  return not_matched();
}

// Protobuf JSON prints uint64 as a decimal string and int32 as a number; omitted means zero.
std::optional<std::uint64_t> json_unsigned(const Json &body, const char *field) {
  const auto value = body.find(field);
  if (value == body.end()) {
    return std::uint64_t{};
  }
  if (value->is_number_unsigned()) {
    return value->get<std::uint64_t>();
  }
  if (value->is_number_integer() && value->get<std::int64_t>() >= 0) {
    return static_cast<std::uint64_t>(value->get<std::int64_t>());
  }
  if (value->is_string()) {
    return parse_number<std::uint64_t>(value->get_ref<const std::string &>());
  }
  return std::nullopt;
}

// ESubscribeType arrives by enum name (protobuf JSON) or number; omitted means AbnormalStatus.
std::optional<WatchType> json_watch_type(const Json &body) {
  struct Name {
    const char *name;
    WatchType type;
  };
  static constexpr Name kNames[] = {
      {"AbnormalStatus", WatchType::abnormal_status},
      {"AI", WatchType::ai},
      {"Path", WatchType::path},
      {"Position", WatchType::position},
      {"SkillCollision", WatchType::skill_collision},
  };

  const auto value = body.find("type");
  if (value == body.end()) {
    return WatchType::abnormal_status;
  }
  if (value->is_number_integer()) {
    const auto number = value->get<std::int64_t>();
    if (number < 0 || number > static_cast<std::int64_t>(WatchType::skill_collision)) {
      return std::nullopt;
    }
    return static_cast<WatchType>(number);
  }
  if (value->is_string()) {
    for (const auto &entry : kNames) {
      if (value->get_ref<const std::string &>() == entry.name) {
        return entry.type;
      }
    }
  }
  return std::nullopt;
}

DiagnosticParse parse_structured_watch(const Json &body) {
  const auto unit_id = json_unsigned(body, "unit_id");
  const auto type = json_watch_type(body);
  if (!unit_id || *unit_id == 0 || !type) {
    return matched_error("GmCmd.debug_watch needs a unit_id and a known type");
  }

  const auto add = body.find("add");
  const bool adding = add != body.end() && add->is_boolean() && add->get<bool>();
  DiagnosticCommand command;
  command.kind = adding ? DiagnosticKind::watch : DiagnosticKind::unwatch;
  command.unit_id = *unit_id;
  command.watch_types = {*type};
  return matched_command(std::move(command));
}

DiagnosticParse parse_structured_game_time(const Json &body) {
  constexpr const char *kUsage = "GmCmd.game_time needs hours 0-24 and one_day_secs 60-604800";
  DiagnosticCommand command;
  command.kind = DiagnosticKind::game_time;

  const auto day_seconds = body.find("one_day_secs");
  if (day_seconds != body.end()) {
    if (!day_seconds->is_number_integer()) {
      return matched_error(kUsage);
    }
    const auto seconds = day_seconds->get<std::int64_t>();
    // Zero (omitted) keeps the emulator default day length.
    if (seconds != 0 && !valid_day_seconds(seconds)) {
      return matched_error(kUsage);
    }
    if (seconds != 0) {
      command.game_time.day_seconds = static_cast<int>(seconds);
    }
  }

  const auto hours = body.find("hours");
  if (hours != body.end()) {
    const auto hour = hours->is_number() ? valid_hour(hours->get<double>()) : std::nullopt;
    if (!hour) {
      return matched_error(kUsage);
    }
    command.game_time.hour = *hour;
  }
  return matched_command(std::move(command));
}

DiagnosticParse parse_structured(const Json &message) {
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
  if (field == "debug_watch") {
    return parse_structured_watch(body);
  }
  if (field == "game_time") {
    return parse_structured_game_time(body);
  }
  if (field == "debug_player_regen_stat") {
    return matched_kind(DiagnosticKind::regen_stat);
  }
  if (field == "channel_event_info") {
    return matched_kind(DiagnosticKind::channel_event_info);
  }
  if (field == "debug_npc_field") {
    return matched_kind(DiagnosticKind::npc_field);
  }
  return not_matched();
}

Json world_position_json(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

} // namespace

bool is_supported_watch_type(WatchType type) {
  return type == WatchType::position || type == WatchType::path || type == WatchType::ai;
}

const char *watch_type_name(WatchType type) {
  switch (type) {
  case WatchType::abnormal_status:
    return "abnormal_status";
  case WatchType::ai:
    return "ai";
  case WatchType::path:
    return "path";
  case WatchType::position:
    return "position";
  case WatchType::skill_collision:
    return "skill_collision";
  }
  return "unknown";
}

DiagnosticParse parse_diagnostic_command(std::uint16_t opcode, const nlohmann::json &message) {
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

nlohmann::json world_game_time_message(const GameTimeSetting &setting,
                                       std::int64_t now_unix_seconds) {
  const double daily_ratio = std::clamp(setting.hour / kHoursPerDay, 0.0, 1.0);
  return {{"started_timestamp", std::to_string(now_unix_seconds)},
          {"one_day_secs", std::to_string(setting.day_seconds)},
          {"daily_ratio", daily_ratio},
          {"stopped", setting.stopped}};
}

nlohmann::json player_regen_stat_message(const nlohmann::json &player_stats,
                                         std::int64_t monotonic_ticks) {
  Json infos = Json::array();
  for (const int stat_type : kRegenPanelStats) {
    const auto value = player_stats.find(std::to_string(stat_type));
    if (value == player_stats.end() || !value->is_number_integer()) {
      continue;
    }
    infos.push_back({{"stat_type", stat_type}, {"stat_value", value->get<int>()}});
  }
  return {{"tick", wire_monotonic_tick(monotonic_ticks)}, {"debug_stat_infos", std::move(infos)}};
}

nlohmann::json channel_event_info_message(int map_id) {
  return {{"map_cms_id", map_id},
          {"save_completion_status", false},
          {"cur_objective_index", Json::array()},
          {"event_cms_id", 0},
          {"completed", false},
          {"db_saved", false}};
}

nlohmann::json debug_position_message(std::uint64_t unit_id, const WorldPosition &position) {
  return {{"unit_id", std::to_string(unit_id)},
          {"pos", world_position_json(position)},
          {"yaw", position.yaw}};
}

nlohmann::json debug_path_message(std::uint64_t unit_id, const WorldPosition &position,
                                  const std::optional<WorldPosition> &route_end) {
  Json points = Json::array();
  points.push_back({{"pos", world_position_json(position)}, {"color", kPathCurrentColor}});
  if (route_end) {
    points.push_back({{"pos", world_position_json(*route_end)}, {"color", kPathDestinationColor}});
  }
  return {{"unit_id", std::to_string(unit_id)}, {"paths", std::move(points)}};
}

nlohmann::json debug_ai_message(std::uint64_t unit_id, NpcAiState state) {
  const int phase = static_cast<int>(state);
  Json node{{"no", phase}, {"result", kBehaviorNodeRunning}, {"name", ai_state_name(state)}};
  return {{"unit_id", std::to_string(unit_id)},
          {"phase", phase},
          {"nodes", Json::array({std::move(node)})}};
}

const char *ai_state_name(NpcAiState state) {
  switch (state) {
  case NpcAiState::Idle:
    return "Idle";
  case NpcAiState::Engaged:
    return "Engaged";
  case NpcAiState::Returning:
    return "Returning";
  }
  return "Unknown";
}

WatchAddResult WatchList::add(std::uint64_t unit_id, WatchType type) {
  if (!is_supported_watch_type(type)) {
    return WatchAddResult::unsupported_type;
  }

  const Watch watch{unit_id, type};
  if (std::ranges::find(watches_, watch) != watches_.end()) {
    return WatchAddResult::already_watched;
  }
  if (watches_.size() >= kMaximumWatchesPerSession) {
    return WatchAddResult::limit_reached;
  }
  watches_.push_back(watch);
  return WatchAddResult::added;
}

bool WatchList::remove(std::uint64_t unit_id, WatchType type) {
  const auto removed = std::erase(watches_, Watch{unit_id, type});
  if (watches_.empty()) {
    next_tick_.reset();
  }
  return removed > 0;
}

std::size_t WatchList::remove_unit(std::uint64_t unit_id) {
  const auto removed =
      std::erase_if(watches_, [unit_id](const Watch &watch) { return watch.unit_id == unit_id; });
  if (watches_.empty()) {
    next_tick_.reset();
  }
  return removed;
}

void WatchList::clear() {
  watches_.clear();
  next_tick_.reset();
}

bool WatchList::empty() const {
  return watches_.empty();
}

const std::vector<Watch> &WatchList::entries() const {
  return watches_;
}

std::optional<std::int64_t> WatchList::next_tick() const {
  if (watches_.empty()) {
    return std::nullopt;
  }
  return next_tick_;
}

bool WatchList::due(std::int64_t ticks) const {
  if (watches_.empty()) {
    return false;
  }
  return !next_tick_ || ticks >= *next_tick_;
}

void WatchList::mark_emitted(std::int64_t ticks) {
  next_tick_ = ticks + kWatchIntervalTicks;
}

const char *diagnostic_name(DiagnosticKind kind) {
  switch (kind) {
  case DiagnosticKind::game_time:
    return "game_time";
  case DiagnosticKind::regen_stat:
    return "regen_stat";
  case DiagnosticKind::watch:
    return "watch";
  case DiagnosticKind::unwatch:
    return "unwatch";
  case DiagnosticKind::channel_event_info:
    return "channel_event_info";
  case DiagnosticKind::npc_field:
    return "debug_npc_field";
  }
  return "unknown";
}

} // namespace chronicles::gm
