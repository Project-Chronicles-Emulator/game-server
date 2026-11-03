#include "world/features/gm/gm_world_commands.hpp"

#include "world/core/world_request_opcodes.hpp"
#include "world/features/gm/gm_tools.hpp"
#include "persistence/account_repository.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <system_error>

namespace chronicles::gm {
namespace {

using Json = nlohmann::json;

constexpr std::uint16_t kGmCommandRequestOpcode = world_request::kGmCommandRequest;
constexpr std::uint16_t kGmCommandTextRequestOpcode = world_request::kGmCommandTextRequest;
constexpr std::size_t kMaximumTextBytes = 256;
constexpr int kMaximumPercent = 100;

WorldCommandParse not_matched() {
  return WorldCommandParse{};
}

WorldCommandParse matched_error(std::string usage) {
  WorldCommandParse parse;
  parse.matched = true;
  parse.error = std::move(usage);
  return parse;
}

WorldCommandParse matched_command(WorldCommand command) {
  WorldCommandParse parse;
  parse.matched = true;
  parse.command = std::move(command);
  return parse;
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

std::optional<int> positive_id(std::string_view token) {
  const auto value = parse_number<int>(token);
  if (!value || *value <= 0) {
    return std::nullopt;
  }
  return value;
}

// "25%" -> 25 (1-100).
std::optional<int> percent_token(std::string_view token) {
  if (token.size() < 2 || token.back() != '%') {
    return std::nullopt;
  }
  const auto value = parse_number<int>(token.substr(0, token.size() - 1));
  if (!value || *value < 1 || *value > kMaximumPercent) {
    return std::nullopt;
  }
  return value;
}

std::optional<UnitTarget> unit_target_token(std::string_view token) {
  const auto name = ascii_lower(std::string(token));
  if (name == "me" || name == "self") {
    return UnitTarget{UnitTargetKind::self, 0};
  }
  if (name == "spawned") {
    return UnitTarget{UnitTargetKind::spawned, 0};
  }
  const auto unit_id = parse_number<std::uint64_t>(token);
  if (!unit_id || *unit_id == 0) {
    return std::nullopt;
  }
  return UnitTarget{UnitTargetKind::unit, *unit_id};
}

std::optional<combat_rules::AbnormalType> gauge_type_token(std::string_view token) {
  struct Name {
    const char *name;
    combat_rules::AbnormalType type;
  };
  static constexpr Name kNames[] = {
      {"burning", combat_rules::AbnormalType::burning},
      {"freezing", combat_rules::AbnormalType::freezing},
      {"electric_shock", combat_rules::AbnormalType::electric_shock},
      {"shock", combat_rules::AbnormalType::electric_shock},
      {"bleeding", combat_rules::AbnormalType::bleeding},
      {"poisoning", combat_rules::AbnormalType::poisoning},
      {"poison", combat_rules::AbnormalType::poisoning},
      {"stun", combat_rules::AbnormalType::stun},
      {"shrink", combat_rules::AbnormalType::shrink},
  };

  const auto name = ascii_lower(std::string(token));
  for (const auto &entry : kNames) {
    if (name == entry.name) {
      return entry.type;
    }
  }
  return std::nullopt;
}

std::optional<double> coordinate_token(std::string_view token) {
  const auto value = parse_number<double>(token);
  if (!value || !std::isfinite(*value) || std::abs(*value) > kMaximumCoordinateMeters) {
    return std::nullopt;
  }
  return value;
}

struct TextCommand {
  const char *name;
  WorldCommandKind kind;
};

// Text names and aliases. npc_ai_on/off and the accept variants share one kind.
constexpr TextCommand kTextCommands[] = {
    {"monster_kill", WorldCommandKind::monster_kill},
    {"kill", WorldCommandKind::monster_kill},
    {"damage_monster", WorldCommandKind::damage_monster},
    {"damage", WorldCommandKind::damage_monster},
    {"add_status_effect", WorldCommandKind::add_status_effect},
    {"buff", WorldCommandKind::add_status_effect},
    {"remove_status_effect", WorldCommandKind::remove_status_effect},
    {"add_abnormal_status_gauge", WorldCommandKind::add_abnormal_gauge},
    {"gauge", WorldCommandKind::add_abnormal_gauge},
    {"cancel_skill", WorldCommandKind::cancel_skill},
    {"cooldown", WorldCommandKind::reset_cooldowns},
    {"reset_cooldowns", WorldCommandKind::reset_cooldowns},
    {"npc_ai_on", WorldCommandKind::npc_ai},
    {"npc_ai_off", WorldCommandKind::npc_ai},
    {"set_gm_stat", WorldCommandKind::set_gm_stat},
    {"add_gm_stat", WorldCommandKind::add_gm_stat},
    {"execute_skill_effect", WorldCommandKind::execute_skill_effect},
    {"move_unit", WorldCommandKind::move_unit},
    {"accept_quest", WorldCommandKind::accept_quest},
    {"accept_quest_if_passed_cond", WorldCommandKind::accept_quest},
    {"mail_send", WorldCommandKind::mail_send},
    {"combat_formula_log", WorldCommandKind::combat_formula_log},
    {"stat_sync_warning", WorldCommandKind::stat_sync_warning},
};

std::optional<WorldCommandKind> text_kind(const std::string &name) {
  for (const auto &entry : kTextCommands) {
    if (name == entry.name) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

using Words = std::vector<std::string_view>;

WorldCommandParse parse_unit_only(WorldCommandKind kind, const Words &words, const char *usage,
                                  bool target_optional) {
  WorldCommand command;
  command.kind = kind;
  if (words.size() == 1 && target_optional) {
    return matched_command(std::move(command));
  }
  const auto target = words.size() == 2 ? unit_target_token(words[1]) : std::nullopt;
  if (!target) {
    return matched_error(usage);
  }
  command.target = *target;
  return matched_command(std::move(command));
}

WorldCommandParse parse_text_damage(const Words &words) {
  constexpr const char *kUsage = "damage_monster <unit_id|spawned> <amount|percent%>";
  const auto target = words.size() == 3 ? unit_target_token(words[1]) : std::nullopt;
  if (!target || target->kind == UnitTargetKind::self) {
    return matched_error(kUsage);
  }

  WorldCommand command;
  command.kind = WorldCommandKind::damage_monster;
  command.target = *target;
  if (const auto percent = percent_token(words[2])) {
    command.percent = *percent;
    return matched_command(std::move(command));
  }
  const auto amount = parse_number<std::int64_t>(words[2]);
  if (!amount || *amount <= 0 || *amount > kMaximumGmDamage) {
    return matched_error(kUsage);
  }
  command.amount = *amount;
  return matched_command(std::move(command));
}

WorldCommandParse parse_text_status(WorldCommandKind kind, const Words &words) {
  const auto usage = std::string(world_command_name(kind)) + " <unit_id|me> <status_effect_id>";
  const auto target = words.size() == 3 ? unit_target_token(words[1]) : std::nullopt;
  const auto status_id = words.size() == 3 ? positive_id(words[2]) : std::nullopt;
  if (!target || !status_id) {
    return matched_error(usage);
  }

  WorldCommand command;
  command.kind = kind;
  command.target = *target;
  command.cms_id = *status_id;
  return matched_command(std::move(command));
}

WorldCommandParse parse_text_gauge(const Words &words) {
  constexpr const char *kUsage =
      "add_abnormal_status_gauge <unit_id|me> <burning|freezing|electric_shock|bleeding|"
      "poisoning|stun|shrink> <amount>";
  if (words.size() != 4) {
    return matched_error(kUsage);
  }
  const auto target = unit_target_token(words[1]);
  const auto type = gauge_type_token(words[2]);
  const auto amount = parse_number<int>(words[3]);
  if (!target || !type || !amount || *amount == 0 || *amount < -kMaximumGmGaugeAmount ||
      *amount > kMaximumGmGaugeAmount) {
    return matched_error(kUsage);
  }

  WorldCommand command;
  command.kind = WorldCommandKind::add_abnormal_gauge;
  command.target = *target;
  command.gauges.emplace_back(*type, *amount);
  return matched_command(std::move(command));
}

WorldCommandParse parse_text_npc_ai(const std::string &name, const Words &words) {
  auto parse = parse_unit_only(WorldCommandKind::npc_ai, words,
                               "npc_ai_on|npc_ai_off <unit_id|spawned>", false);
  if (parse.command) {
    if (parse.command->target.kind == UnitTargetKind::self) {
      return matched_error("npc_ai_on|npc_ai_off <unit_id|spawned>");
    }
    parse.command->enabled = name == "npc_ai_on";
  }
  return parse;
}

WorldCommandParse parse_text_set_stat(const Words &words) {
  constexpr const char *kUsage = "set_gm_stat <stat> <value> | set_gm_stat clear";
  WorldCommand command;
  command.kind = WorldCommandKind::set_gm_stat;
  if (words.size() == 2 && ascii_lower(std::string(words[1])) == "clear") {
    command.clear = true;
    return matched_command(std::move(command));
  }

  const auto stat = words.size() == 3 ? parse_stat_type_token(words[1]) : std::nullopt;
  const auto value = words.size() == 3 ? parse_number<std::int64_t>(words[2]) : std::nullopt;
  if (!stat || !value || *value < -kMaximumGmStatValue || *value > kMaximumGmStatValue) {
    return matched_error(kUsage);
  }
  command.cms_id = *stat;
  command.amount = *value;
  return matched_command(std::move(command));
}

WorldCommandParse parse_text_add_stat(const Words &words) {
  constexpr const char *kUsage = "add_gm_stat <stat> <value|percent%>";
  const auto stat = words.size() == 3 ? parse_stat_type_token(words[1]) : std::nullopt;
  if (!stat) {
    return matched_error(kUsage);
  }

  WorldCommand command;
  command.kind = WorldCommandKind::add_gm_stat;
  command.cms_id = *stat;
  if (words[2].size() > 1 && words[2].back() == '%') {
    const auto percent = parse_number<int>(words[2].substr(0, words[2].size() - 1));
    if (!percent || *percent == 0 || *percent < -1000 || *percent > 1000) {
      return matched_error(kUsage);
    }
    command.percent = *percent;
    return matched_command(std::move(command));
  }
  const auto value = parse_number<std::int64_t>(words[2]);
  if (!value || *value == 0 || *value < -kMaximumGmStatValue || *value > kMaximumGmStatValue) {
    return matched_error(kUsage);
  }
  command.amount = *value;
  return matched_command(std::move(command));
}

WorldCommandParse parse_text_move_unit(const Words &words) {
  constexpr const char *kUsage = "move_unit <unit_id|me|spawned> <x> <y> <z> [rel] (meters)";
  if (words.size() != 5 && words.size() != 6) {
    return matched_error(kUsage);
  }
  const auto target = unit_target_token(words[1]);
  const auto x = coordinate_token(words[2]);
  const auto y = coordinate_token(words[3]);
  const auto z = coordinate_token(words[4]);
  const bool relative = words.size() == 6;
  if (!target || !x || !y || !z || (relative && ascii_lower(std::string(words[5])) != "rel")) {
    return matched_error(kUsage);
  }

  WorldCommand command;
  command.kind = WorldCommandKind::move_unit;
  command.target = *target;
  command.x_meters = *x;
  command.y_meters = *y;
  command.z_meters = *z;
  command.relative = relative;
  return matched_command(std::move(command));
}

WorldCommandParse parse_text_mail(const Words &words) {
  constexpr const char *kUsage = "mail_send <name|me> [item <item_id> [count] | gold <amount>]";
  if (words.size() < 2 || words[1].size() > kMaximumPlayerNameBytes) {
    return matched_error(kUsage);
  }

  WorldCommand command;
  command.kind = WorldCommandKind::mail_send;
  if (ascii_lower(std::string(words[1])) != "me") {
    command.recipient_name = std::string(words[1]);
  }
  if (words.size() == 2) {
    return matched_command(std::move(command));
  }

  const auto attachment = ascii_lower(std::string(words[2]));
  if (attachment == "item" && (words.size() == 4 || words.size() == 5)) {
    const auto item_id = positive_id(words[3]);
    const auto count = words.size() == 5 ? positive_id(words[4]) : std::optional<int>(1);
    if (!item_id || !count) {
      return matched_error(kUsage);
    }
    command.attachment = MailAttachmentChoice::item;
    command.cms_id = *item_id;
    command.amount = *count;
    return matched_command(std::move(command));
  }
  if (attachment == "gold" && words.size() == 4) {
    const auto amount = positive_id(words[3]);
    if (!amount) {
      return matched_error(kUsage);
    }
    command.attachment = MailAttachmentChoice::gold;
    command.amount = *amount;
    return matched_command(std::move(command));
  }
  return matched_error(kUsage);
}

WorldCommandParse parse_text_no_arguments(WorldCommandKind kind, const Words &words) {
  if (words.size() != 1) {
    return matched_error(std::string(world_command_name(kind)) + " takes no arguments");
  }
  WorldCommand command;
  command.kind = kind;
  return matched_command(std::move(command));
}

WorldCommandParse parse_text_single_id(WorldCommandKind kind, const std::string &name,
                                       const Words &words, const char *argument) {
  const auto id = words.size() == 2 ? positive_id(words[1]) : std::nullopt;
  if (!id) {
    return matched_error(name + " <" + argument + ">");
  }
  WorldCommand command;
  command.kind = kind;
  command.cms_id = *id;
  command.check_conditions = name == "accept_quest_if_passed_cond";
  return matched_command(std::move(command));
}

WorldCommandParse parse_text(const Json &message) {
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

  const auto name = ascii_lower(std::string(words.front()));
  const auto kind = text_kind(name);
  if (!kind) {
    return not_matched();
  }

  switch (*kind) {
  case WorldCommandKind::monster_kill: {
    auto parse = parse_unit_only(*kind, words, "monster_kill <unit_id|spawned>", false);
    if (parse.command && parse.command->target.kind == UnitTargetKind::self) {
      return matched_error("monster_kill <unit_id|spawned> (use suicide for yourself)");
    }
    return parse;
  }
  case WorldCommandKind::damage_monster:
    return parse_text_damage(words);
  case WorldCommandKind::add_status_effect:
  case WorldCommandKind::remove_status_effect:
    return parse_text_status(*kind, words);
  case WorldCommandKind::add_abnormal_gauge:
    return parse_text_gauge(words);
  case WorldCommandKind::cancel_skill:
    return parse_unit_only(*kind, words, "cancel_skill [unit_id|me]", true);
  case WorldCommandKind::npc_ai:
    return parse_text_npc_ai(name, words);
  case WorldCommandKind::set_gm_stat:
    return parse_text_set_stat(words);
  case WorldCommandKind::add_gm_stat:
    return parse_text_add_stat(words);
  case WorldCommandKind::execute_skill_effect:
    return parse_text_single_id(*kind, name, words, "skill_effect_id");
  case WorldCommandKind::move_unit:
    return parse_text_move_unit(words);
  case WorldCommandKind::accept_quest:
    return parse_text_single_id(*kind, name, words, "quest_id");
  case WorldCommandKind::mail_send:
    return parse_text_mail(words);
  case WorldCommandKind::reset_cooldowns:
  case WorldCommandKind::combat_formula_log:
  case WorldCommandKind::stat_sync_warning:
    return parse_text_no_arguments(*kind, words);
  }
  return not_matched();
}

// Protobuf JSON: uint64/int64 arrive as decimal strings, 32-bit integers as numbers; an omitted
// field is zero.
std::optional<std::int64_t> json_integer(const Json &body, const char *field) {
  const auto value = body.find(field);
  if (value == body.end()) {
    return std::int64_t{};
  }
  if (value->is_number_unsigned() &&
      value->get<std::uint64_t>() >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  if (value->is_number_integer()) {
    return value->get<std::int64_t>();
  }
  if (value->is_string()) {
    return parse_number<std::int64_t>(value->get_ref<const std::string &>());
  }
  return std::nullopt;
}

bool has_nondefault_field(const Json &body, const char *field) {
  const auto value = body.find(field);
  if (value == body.end()) {
    return false;
  }
  return *value != 0 && *value != false && *value != "0" && *value != Json::array();
}

std::optional<bool> json_boolean(const Json &body, const char *field) {
  const auto value = body.find(field);
  if (value == body.end()) {
    return false;
  }
  if (!value->is_boolean()) {
    return std::nullopt;
  }
  return value->get<bool>();
}

// A structured unit id of zero names the GM's own character.
std::optional<UnitTarget> json_unit_target(const Json &body) {
  const auto unit_id = json_integer(body, "unit_id");
  if (!unit_id || *unit_id < 0) {
    return std::nullopt;
  }
  if (*unit_id == 0) {
    return UnitTarget{UnitTargetKind::self, 0};
  }
  return UnitTarget{UnitTargetKind::unit, static_cast<std::uint64_t>(*unit_id)};
}

std::optional<int> json_stat_type(const Json &body) {
  const auto type = body.find("type");
  if (type == body.end()) {
    return std::nullopt;
  }
  if (type->is_number_integer()) {
    const auto value = json_integer(body, "type");
    if (!value || *value < 1 || *value > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    return parse_stat_type_token(std::to_string(*value));
  }
  if (!type->is_string()) {
    return std::nullopt;
  }
  constexpr std::string_view kEnumPrefix = "EX5StatType_";
  std::string_view name = type->get_ref<const std::string &>();
  if (name.starts_with(kEnumPrefix)) {
    name.remove_prefix(kEnumPrefix.size());
  }
  return parse_stat_type_token(name);
}

std::optional<std::vector<double>> json_position(const Json &body, const char *field) {
  const auto values = body.find(field);
  if (values == body.end() || !values->is_array() || values->size() != 3) {
    return std::nullopt;
  }
  std::vector<double> position;
  for (const auto &value : *values) {
    if (!value.is_number() || !std::isfinite(value.get<double>()) ||
        std::abs(value.get<double>()) > kMaximumCoordinateMeters) {
      return std::nullopt;
    }
    position.push_back(value.get<double>());
  }
  return position;
}

WorldCommandParse structured_with_target(WorldCommandKind kind, const Json &body,
                                         const char *usage) {
  const auto target = json_unit_target(body);
  if (!target) {
    return matched_error(usage);
  }
  WorldCommand command;
  command.kind = kind;
  command.target = *target;
  return matched_command(std::move(command));
}

WorldCommandParse parse_structured_damage(WorldCommandKind kind, const Json &body) {
  auto parse = structured_with_target(kind, body, "GmCmd needs a monster unit_id");
  if (!parse.command || parse.command->target.kind == UnitTargetKind::self) {
    return matched_error("GmCmd needs a monster unit_id");
  }
  if (kind == WorldCommandKind::monster_kill) {
    return parse;
  }

  const auto amount = json_integer(body, "amount");
  const auto percent = json_integer(body, "hp_per");
  if (!amount || !percent || *percent < 0 || *percent > kMaximumPercent ||
      (*amount != 0 && *percent != 0)) {
    return matched_error("GmCmd.damage_monster needs either amount or hp_per, not both");
  }
  if (*percent > 0) {
    parse.command->percent = static_cast<int>(*percent);
    return parse;
  }
  if (!amount || *amount <= 0 || *amount > kMaximumGmDamage) {
    return matched_error("GmCmd.damage_monster needs amount or hp_per");
  }
  parse.command->amount = *amount;
  return parse;
}

WorldCommandParse parse_structured_status(WorldCommandKind kind, const Json &body) {
  auto parse = structured_with_target(kind, body, "GmCmd status effect needs a unit_id");
  const auto status_id = json_integer(body, "status_effect_id");
  if (!parse.command || !status_id || *status_id <= 0 ||
      *status_id > std::numeric_limits<int>::max()) {
    return matched_error("GmCmd status effect needs a positive status_effect_id");
  }
  if (kind == WorldCommandKind::add_status_effect) {
    const auto stacks = json_integer(body, "stack_count");
    if (!stacks || *stacks < 0 || *stacks > 1) {
      return matched_error("GmCmd.add_status_effect supports only one stack");
    }
  }
  parse.command->cms_id = static_cast<int>(*status_id);
  return parse;
}

WorldCommandParse parse_structured_gauge(const Json &body) {
  struct Field {
    const char *name;
    combat_rules::AbnormalType type;
  };
  static constexpr Field kFields[] = {
      {"burning", combat_rules::AbnormalType::burning},
      {"freezing", combat_rules::AbnormalType::freezing},
      {"electric_shock", combat_rules::AbnormalType::electric_shock},
      {"bleeding", combat_rules::AbnormalType::bleeding},
      {"poisoning", combat_rules::AbnormalType::poisoning},
      {"stun", combat_rules::AbnormalType::stun},
      {"shrink", combat_rules::AbnormalType::shrink},
  };

  auto parse = structured_with_target(WorldCommandKind::add_abnormal_gauge, body,
                                      "GmCmd.add_abnormal_status_gauge needs a unit_id");
  if (!parse.command) {
    return parse;
  }
  for (const auto &field : kFields) {
    const auto amount = json_integer(body, field.name);
    if (!amount || *amount < -kMaximumGmGaugeAmount || *amount > kMaximumGmGaugeAmount) {
      return matched_error("GmCmd.add_abnormal_status_gauge amount is out of range");
    }
    if (*amount != 0) {
      parse.command->gauges.emplace_back(field.type, static_cast<int>(*amount));
    }
  }
  if (parse.command->gauges.empty()) {
    return matched_error("GmCmd.add_abnormal_status_gauge sets no gauge");
  }
  return parse;
}

WorldCommandParse parse_structured_stat(WorldCommandKind kind, const Json &body) {
  WorldCommand command;
  command.kind = kind;
  const auto clear = json_boolean(body, "clear");
  const auto enable = json_boolean(body, "enable");
  if (!clear || !enable) {
    return matched_error("GmCmd stat flags must be booleans");
  }
  if (kind == WorldCommandKind::set_gm_stat) {
    if (*clear) {
      command.clear = true;
      return matched_command(std::move(command));
    }
    if (!*enable) {
      return matched_error("GmCmd.set_gm_stat needs enable=true; use clear=true to restore stats");
    }
  }

  const auto stat = json_stat_type(body);
  const auto value = json_integer(body, "value");
  if (!stat || !value || *value < -kMaximumGmStatValue || *value > kMaximumGmStatValue) {
    return matched_error("GmCmd stat command needs a known type and value");
  }
  command.cms_id = *stat;
  command.amount = *value;

  const auto percent = body.find("percent");
  if (kind == WorldCommandKind::add_gm_stat && percent != body.end()) {
    if (!percent->is_number()) {
      return matched_error("GmCmd.add_gm_stat percent must be finite and between -10 and 10");
    }
    const double ratio = percent->get<double>();
    if (!std::isfinite(ratio) || ratio < -10.0 || ratio > 10.0) {
      return matched_error("GmCmd.add_gm_stat percent must be finite and between -10 and 10");
    }
    if (ratio != 0.0) {
      if (*value != 0) {
        return matched_error("GmCmd.add_gm_stat needs either value or percent, not both");
      }
      command.percent = ratio * kMaximumPercent;
    }
  }
  return matched_command(std::move(command));
}

WorldCommandParse parse_structured_move(const Json &body) {
  auto parse =
      structured_with_target(WorldCommandKind::move_unit, body, "GmCmd.move_unit needs a unit_id");
  // Candidate units: GmCmd float positions are taken as meters, like the text grammar.
  const auto position = json_position(body, "pos");
  if (!parse.command || !position) {
    return matched_error("GmCmd.move_unit needs pos [x, y, z]");
  }
  parse.command->x_meters = (*position)[0];
  parse.command->y_meters = (*position)[1];
  parse.command->z_meters = (*position)[2];
  for (const auto *field : {"speed_type", "speed_value", "trading_post_id"}) {
    if (has_nondefault_field(body, field)) {
      return matched_error("GmCmd.move_unit supports only placement coordinates and rel");
    }
  }
  const auto relative = json_boolean(body, "rel");
  if (!relative) {
    return matched_error("GmCmd.move_unit rel must be a boolean");
  }
  parse.command->relative = *relative;
  return parse;
}

WorldCommandParse parse_structured_mail(const Json &body) {
  WorldCommand command;
  command.kind = WorldCommandKind::mail_send;
  const auto character_id = json_integer(body, "char_id");
  const auto account_id = json_integer(body, "account_id");
  if (!character_id || *character_id < 0 || !account_id || *account_id < 0) {
    return matched_error("GmCmd.mail_send recipient IDs are malformed");
  }
  command.recipient_character_id = *character_id;
  command.recipient_account_id = *account_id;
  for (const auto *field : {"title", "text"}) {
    const auto value = body.find(field);
    if (value != body.end() && !value->is_string()) {
      return matched_error("GmCmd.mail_send title and text must be strings");
    }
  }

  const auto text_field = [&body](const char *name) {
    const auto value = body.find(name);
    return value != body.end() && value->is_string() ? value->get<std::string>() : std::string();
  };
  command.mail_title = text_field("title");
  command.mail_text = text_field("text");
  if (command.mail_title.size() > kMaximumGmMailTitleBytes ||
      command.mail_text.size() > kMaximumGmMailTextBytes) {
    return matched_error("GmCmd.mail_send title or text is too long");
  }

  const auto item_id = json_integer(body, "item_cms_id");
  const auto money = json_integer(body, "money_amount");
  if (!item_id || !money || *item_id < 0 || *money < 0 ||
      *item_id > std::numeric_limits<int>::max() || *money > std::numeric_limits<int>::max() ||
      (*item_id != 0 && *money != 0)) {
    return matched_error("GmCmd.mail_send needs at most one bounded attachment");
  }
  if (*item_id > 0) {
    command.attachment = MailAttachmentChoice::item;
    command.cms_id = static_cast<int>(*item_id);
    command.amount = 1;
  } else if (*money > 0) {
    // Only gold is stored by the mail service attachments used here.
    const auto type = body.find("money_type");
    const auto numeric_type = json_integer(body, "money_type");
    const bool gold = type == body.end() ||
                      (type->is_string() && type->get<std::string>() == "EX5MoneyType_Gold") ||
                      (numeric_type && *numeric_type == 0);
    if (!gold) {
      return matched_error("GmCmd.mail_send supports only gold money");
    }
    command.attachment = MailAttachmentChoice::gold;
    command.amount = *money;
  }
  return matched_command(std::move(command));
}

WorldCommandParse parse_structured(const Json &message) {
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
  if (field == "monster_kill") {
    return parse_structured_damage(WorldCommandKind::monster_kill, body);
  }
  if (field == "damage_monster") {
    return parse_structured_damage(WorldCommandKind::damage_monster, body);
  }
  if (field == "add_status_effect") {
    return parse_structured_status(WorldCommandKind::add_status_effect, body);
  }
  if (field == "remove_status_effect") {
    return parse_structured_status(WorldCommandKind::remove_status_effect, body);
  }
  if (field == "add_abnormal_status_gauge") {
    return parse_structured_gauge(body);
  }
  if (field == "cancel_skill") {
    return structured_with_target(WorldCommandKind::cancel_skill, body,
                                  "GmCmd.cancel_skill unit_id is malformed");
  }
  if (field == "cooldown" || field == "combat_formula_log" || field == "stat_sync_warning") {
    WorldCommand command;
    command.kind = WorldCommandKind::reset_cooldowns;
    if (field == "combat_formula_log") {
      command.kind = WorldCommandKind::combat_formula_log;
    } else if (field == "stat_sync_warning") {
      command.kind = WorldCommandKind::stat_sync_warning;
    }
    return matched_command(std::move(command));
  }
  if (field == "npc_ai_on" || field == "npc_ai_off") {
    auto parse = structured_with_target(WorldCommandKind::npc_ai, body,
                                        "GmCmd npc_ai needs a monster unit_id");
    if (parse.command) {
      parse.command->enabled = field == "npc_ai_on";
    }
    return parse;
  }
  if (field == "set_gm_stat") {
    return parse_structured_stat(WorldCommandKind::set_gm_stat, body);
  }
  if (field == "add_gm_stat") {
    return parse_structured_stat(WorldCommandKind::add_gm_stat, body);
  }
  if (field == "execute_skill_effect" || field == "accept_quest" ||
      field == "accept_quest_if_passed_cond") {
    if (field == "execute_skill_effect") {
      for (const auto *extra : {"target_id", "conditional_cms_id", "pitch"}) {
        if (has_nondefault_field(body, extra)) {
          return matched_error(
              "GmCmd.execute_skill_effect supports only unconditional self effects");
        }
      }
    }
    const auto id = json_integer(body, "cms_id");
    if (!id || *id <= 0 || *id > std::numeric_limits<int>::max()) {
      return matched_error("GmCmd." + field + " needs a positive cms_id");
    }
    WorldCommand command;
    command.kind = field == "execute_skill_effect" ? WorldCommandKind::execute_skill_effect
                                                   : WorldCommandKind::accept_quest;
    command.cms_id = static_cast<int>(*id);
    command.check_conditions = field == "accept_quest_if_passed_cond";
    return matched_command(std::move(command));
  }
  if (field == "move_unit") {
    return parse_structured_move(body);
  }
  if (field == "mail_send") {
    return parse_structured_mail(body);
  }
  return not_matched();
}

struct UnsupportedCommand {
  const char *name;
  const char *reason;
};

// GmCmd fields this server recognizes but cannot serve, with the missing piece. Text commands
// use the same names.
constexpr UnsupportedCommand kUnsupportedCommands[] = {
    {"change_class", "class changes need a persisted class, class skills and weapons; the "
                     "emulator fixes the class at character creation"},
    {"abandon_quest", "quest records are append-only in storage; dropping a quest needs a "
                      "storage change"},
    {"remove_quest", "quest records are append-only in storage; removing a quest needs a "
                     "storage change"},
    {"remove_quest_job_board", "the quest job board is not implemented"},
    {"interact_doodad", "doodad interactions run through the tutorial and housing services only"},
    {"change_doodad_phase", "doodad phases are owned by the tutorial and housing services"},
    {"activate_gathering", "gathering points respawn on their catalog timers; GM respawn is not "
                           "implemented"},
    {"set_world_level", "the world level system is not implemented (always 1)"},
    {"spawn_projectile", "projectiles exist only inside simulated skill casts"},
    {"spawn_skillobject", "skill objects exist only inside simulated skill casts"},
    {"npc_use_skill", "forcing an NPC skill is not implemented"},
    {"set_reaction", "hit reactions are client-simulated"},
    {"ignore_reaction", "hit reactions are client-simulated"},
    {"npc_root_motion_in_place", "NPC root motion debugging is not implemented"},
    {"npc_server_preprocessing_skilleffect", "NPC skill-effect preprocessing is not simulated"},
    {"set_faction", "unit factions come from the catalog; runtime faction changes are not "
                    "implemented"},
    {"set_class_action_form_gauge", "class action forms are not simulated"},
    {"set_adventure_object_state", "adventure objects advance only through their own "
                                   "interactions"},
    {"company", "companies are not modeled"},
    {"toggle_circular_flight", "flight is client-simulated"},
    {"damage_destructible_object", "destructible objects are not implemented"},
    {"spawn_destructible_object", "destructible objects are not implemented"},
    {"mate_npc_follow_owner", "mate follow behavior is not implemented"},
    {"draw_mesh", "the server has no navigation or collision mesh"},
    {"debug_projetile_spawnable_area", "the server has no projectile volume data"},
    {"set_npc_spawner", "NPC spawners are fixed placements"},
    {"check_static_npc", "static NPC sector checks are not implemented"},
    {"set_nui_blessing_level", "direct GM blessing-level assignment is not implemented"},
    {"lv_up_expertise", "expertise progression is not implemented"},
    {"reset_expertise_lv", "expertise progression is not implemented"},
    {"achievement_update", "achievements are not implemented"},
    {"contents_unlock", "content unlock flags are not implemented"},
    {"content_reset_counter", "content-reset counter administration is not implemented"},
    {"party_channel_invite", "party channels are not implemented"},
    {"add_bluesalt", "bluesalt (premium currency) is not implemented"},
    {"bluesalt_fake_purchase_add", "paid purchases are not implemented"},
    {"bluesalt_recovery_run", "paid purchases are not implemented"},
    {"bluesalt_charge_claim_last", "paid purchases are not implemented"},
    {"preorder_bonus_dev", "preorder rewards are not implemented"},
    {"channel_incident_start", "channel incidents are not implemented"},
    {"channel_incident_stop", "channel incidents are not implemented"},
    {"channel_move", "channel numbers are not modeled; use tp_map"},
    {"guild_exp", "guild experience has no source yet; guild GM commands are not implemented"},
    {"guild_level", "guild GM commands are not implemented"},
    {"guild_master_authorization", "guild GM commands are not implemented"},
    {"guild_reset_cooldown", "guild GM commands are not implemented"},
    {"guild_disband", "guild GM commands are not implemented"},
    {"togglegraze", "handled by the client console itself"},
    {"toggle_graze", "handled by the client console itself"},
    {"raycast", "handled by the client console itself"},
};

std::optional<std::string> reason_for(const std::string &name) {
  for (const auto &entry : kUnsupportedCommands) {
    if (name == entry.name) {
      return std::string(entry.name) + " is not available: " + entry.reason + ".";
    }
  }
  return std::nullopt;
}

} // namespace

WorldCommandParse parse_world_command(std::uint16_t opcode, const nlohmann::json &message) {
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

std::optional<std::string> unsupported_command_reason(std::uint16_t opcode,
                                                      const nlohmann::json &message) {
  if (!message.is_object()) {
    return std::nullopt;
  }

  if (opcode == kGmCommandTextRequestOpcode) {
    const auto text = message.find("text");
    if (text == message.end() || !text->is_string() ||
        text->get_ref<const std::string &>().size() > kMaximumTextBytes) {
      return std::nullopt;
    }
    const auto words = split_command_words(text->get_ref<const std::string &>());
    if (words.empty()) {
      return std::nullopt;
    }
    return reason_for(ascii_lower(std::string(words.front())));
  }

  if (opcode == kGmCommandRequestOpcode) {
    const auto commands = message.find("cmds");
    if (commands == message.end() || !commands->is_array()) {
      return std::nullopt;
    }
    for (const auto &entry : *commands) {
      if (entry.is_object() && entry.size() == 1) {
        if (auto reason = reason_for(entry.begin().key())) {
          return reason;
        }
      }
    }
  }
  return std::nullopt;
}

const char *world_command_name(WorldCommandKind kind) {
  switch (kind) {
  case WorldCommandKind::monster_kill:
    return "monster_kill";
  case WorldCommandKind::damage_monster:
    return "damage_monster";
  case WorldCommandKind::add_status_effect:
    return "add_status_effect";
  case WorldCommandKind::remove_status_effect:
    return "remove_status_effect";
  case WorldCommandKind::add_abnormal_gauge:
    return "add_abnormal_status_gauge";
  case WorldCommandKind::cancel_skill:
    return "cancel_skill";
  case WorldCommandKind::reset_cooldowns:
    return "cooldown";
  case WorldCommandKind::npc_ai:
    return "npc_ai";
  case WorldCommandKind::set_gm_stat:
    return "set_gm_stat";
  case WorldCommandKind::add_gm_stat:
    return "add_gm_stat";
  case WorldCommandKind::execute_skill_effect:
    return "execute_skill_effect";
  case WorldCommandKind::move_unit:
    return "move_unit";
  case WorldCommandKind::accept_quest:
    return "accept_quest";
  case WorldCommandKind::mail_send:
    return "mail_send";
  case WorldCommandKind::combat_formula_log:
    return "combat_formula_log";
  case WorldCommandKind::stat_sync_warning:
    return "stat_sync_warning";
  }
  return "unknown";
}

} // namespace chronicles::gm
