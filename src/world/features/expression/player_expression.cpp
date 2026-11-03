#include "world/features/expression/player_expression.hpp"

#include "common/catalog_fields.hpp"
#include "data/client_catalog.hpp"
#include "common/clock_ticks.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kEmotionConfiguration = "Emotion";
constexpr std::int64_t kInternalTicksPerSecond = 10'000'000;
// Plausibility bound for the catalog cooldown: one minute.
constexpr double kMaximumCooldownSeconds = 60.0;

// The client measures EmotionCooltimeSec from its own previous send. Arrival intervals can be
// shorter by network jitter, so the server accepts this much less (emulator policy, 200 ms).
constexpr std::int64_t kCooldownArrivalAllowanceTicks = 2'000'000;

// A client yaw outside one full turn in either direction is not a facing.
constexpr double kMaximumYawDegrees = 360.0;

EmotionCategory parse_category(const std::string &text) {
  if (text.empty() || text == "None") {
    return EmotionCategory::none;
  }
  if (text == "Basic") {
    return EmotionCategory::basic;
  }
  if (text == "Reward") {
    return EmotionCategory::reward;
  }
  if (text == "BM") {
    return EmotionCategory::paid;
  }
  throw std::runtime_error("Invalid Emotion category: " + text);
}

// A protobuf int32 decoded to JSON; an absent field is zero.
int integer_field(const Json &message, const char *name) {
  const auto value = message.find(name);
  if (value == message.end() || !value->is_number_integer()) {
    return 0;
  }

  const auto number = value->get<std::int64_t>();
  if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
    return 0;
  }
  return static_cast<int>(number);
}

bool bool_field(const Json &message, const char *name) {
  const auto value = message.find(name);
  return value != message.end() && value->is_boolean() && value->get<bool>();
}

// EmotionExecReq.yaw in degrees. An absent field is the proto3 default 0; a value that is not a
// finite facing keeps the server's last known yaw.
float requested_yaw(const Json &message, float fallback) {
  const auto value = message.find("yaw");
  if (value == message.end()) {
    return 0.0F;
  }
  if (!value->is_number()) {
    return fallback;
  }

  const auto yaw = value->get<double>();
  if (!std::isfinite(yaw) || std::abs(yaw) > kMaximumYawDegrees) {
    return fallback;
  }
  return static_cast<float>(yaw);
}

Json wire_position(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

GameplayPacket emotion_response(x5::ec::EErrorCode code) {
  auto message = Json::object();
  // Success is the proto3 default and is omitted, like every other success response.
  if (code != x5::ec::Success) {
    message["ec"] = x5::ec::EErrorCode_Name(code);
  }
  return {kEmotionExecResponseOpcode, std::move(message)};
}

Json emotion_list(const std::set<int> &emotion_ids) {
  auto emotions = Json::array();
  for (const auto emotion_id : emotion_ids) {
    emotions.push_back({{"cms_id", emotion_id}});
  }
  return emotions;
}

GameplayPacket selfie_notification(std::uint64_t unit_id, bool enter) {
  return {kSelfieModeNotificationOpcode, {{"unit_id", std::to_string(unit_id)}, {"enter", enter}}};
}

std::optional<std::uint64_t> unit_id_of(const Json &message) {
  const auto value = message.find("unit_id");
  if (value == message.end()) {
    return std::nullopt;
  }
  if (value->is_number_unsigned()) {
    return value->get<std::uint64_t>();
  }
  if (!value->is_string()) {
    return std::nullopt;
  }

  const auto &text = value->get_ref<const std::string &>();
  std::uint64_t unit_id{};
  const auto *end = text.data() + text.size();
  const auto [last, error] = std::from_chars(text.data(), end, unit_id);
  if (error != std::errc{} || last != end) {
    return std::nullopt;
  }
  return unit_id;
}

// Same bound as the other GM text parsers (kMaximumGmCommandTextBytes in gm_commands.hpp).
constexpr std::size_t kMaximumGmTextBytes = 256;

std::vector<std::string_view> split_words(std::string_view text) {
  std::vector<std::string_view> words;
  if (text.size() > kMaximumGmTextBytes) {
    return words;
  }

  std::size_t start = 0;
  while (start < text.size()) {
    const auto first = text.find_first_not_of(" \t", start);
    if (first == std::string_view::npos) {
      break;
    }
    const auto last = std::min(text.find_first_of(" \t", first), text.size());
    words.push_back(text.substr(first, last - first));
    start = last;
  }
  return words;
}

std::string ascii_lowercase(std::string_view text) {
  std::string lowered(text);
  for (auto &character : lowered) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return lowered;
}

bool is_emotion_grant_alias(const std::string &name) {
  return name == "emotion_add" || name == "add_emotion" || name == "emote_add";
}

std::optional<int> positive_integer(std::string_view text) {
  int value{};
  const auto *end = text.data() + text.size();
  const auto [last, error] = std::from_chars(text.data(), end, value);
  if (error != std::errc{} || last != end || value <= 0) {
    return std::nullopt;
  }
  return value;
}

} // namespace

EmotionCatalog EmotionCatalog::load(ClientCatalog &catalog) {
  if (!catalog.has_table("Emotion")) {
    spdlog::warn("{}: catalog has no Emotion table; every emote is refused", kEmotionConfiguration);
    return {};
  }

  auto cooldown_ticks = kDefaultCooldownTicks;
  const auto row = catalog.has_table("GameVariable")
                       ? catalog.find_row("GameVariable", "name", "EmotionCooltimeSec")
                       : std::nullopt;
  const auto seconds = row ? catalog_fields::optional_number(*row, "value") : std::nullopt;
  if (seconds && std::isfinite(*seconds) && *seconds >= 0.0 &&
      *seconds <= kMaximumCooldownSeconds) {
    cooldown_ticks = std::llround(*seconds * static_cast<double>(kInternalTicksPerSecond));
  } else {
    spdlog::warn("{}: GameVariable EmotionCooltimeSec is missing or implausible; using {} s",
                 kEmotionConfiguration, kDefaultCooldownTicks / kInternalTicksPerSecond);
  }

  auto loaded = from_rows(catalog.rows("Emotion"), cooldown_ticks);
  spdlog::info("{}: {} emotions, cooldown {} ms", kEmotionConfiguration, loaded.size(),
               loaded.cooldown_ticks() / (kInternalTicksPerSecond / 1000));
  return loaded;
}

EmotionCatalog EmotionCatalog::from_rows(const std::vector<nlohmann::json> &rows,
                                         std::int64_t cooldown_ticks) {
  if (cooldown_ticks < 0) {
    throw std::invalid_argument("Emotion cooldown cannot be negative");
  }

  EmotionCatalog catalog;
  catalog.cooldown_ticks_ = cooldown_ticks;
  for (const auto &row : rows) {
    EmotionDefinition definition;
    definition.id = catalog_fields::required_identifier(row, "id");
    definition.category = parse_category(catalog_fields::text(row, "category"));
    definition.looping = !catalog_fields::text(row, "targetAnimStanceKey").empty();
    if (!catalog.definitions_.emplace(definition.id, definition).second) {
      throw std::runtime_error("Duplicate Emotion id " + std::to_string(definition.id));
    }
  }
  return catalog;
}

const EmotionDefinition *EmotionCatalog::find(int emotion_id) const {
  const auto found = definitions_.find(emotion_id);
  if (found == definitions_.end()) {
    return nullptr;
  }
  return &found->second;
}

std::int64_t EmotionCatalog::cooldown_ticks() const {
  return cooldown_ticks_;
}

std::size_t EmotionCatalog::size() const {
  return definitions_.size();
}

bool owns_emotion(const EmotionCatalog &catalog, const std::set<int> &granted_emotions,
                  int emotion_id) {
  const auto *definition = catalog.find(emotion_id);
  if (!definition) {
    return false;
  }
  return definition->category == EmotionCategory::basic || granted_emotions.contains(emotion_id);
}

PlayerExpression::PlayerExpression(std::shared_ptr<const EmotionCatalog> catalog,
                                   std::uint64_t player_unit_id, std::set<int> granted_emotions)
    : catalog_(std::move(catalog)), player_unit_id_(player_unit_id),
      granted_emotions_(std::move(granted_emotions)) {
  if (!catalog_) {
    throw std::invalid_argument("PlayerExpression requires an emotion catalog");
  }
}

bool PlayerExpression::handles(std::uint16_t opcode) {
  return opcode == kEmotionExecRequestOpcode || opcode == kSelfieModeRequestOpcode;
}

ExpressionOutcome PlayerExpression::receive(std::uint16_t opcode, const nlohmann::json &message,
                                            const ExpressionContext &context, std::int64_t now) {
  switch (opcode) {
  case kEmotionExecRequestOpcode:
    return execute_emotion(message, context, now);
  case kSelfieModeRequestOpcode:
    return change_selfie(message, context);
  default:
    throw std::invalid_argument("PlayerExpression does not handle this opcode");
  }
}

GameplayPacket PlayerExpression::snapshot() const {
  return {kWorldCharEmotionsNotificationOpcode, {{"emotions", emotion_list(granted_emotions_)}}};
}

void PlayerExpression::adopt_grant(int emotion_id) {
  granted_emotions_.insert(emotion_id);
}

bool PlayerExpression::selfie_active() const {
  return selfie_active_;
}

void PlayerExpression::reset_selfie() {
  selfie_active_ = false;
}

ExpressionOutcome PlayerExpression::execute_emotion(const nlohmann::json &message,
                                                    const ExpressionContext &context,
                                                    std::int64_t now) {
  ExpressionOutcome outcome;
  const auto emotion_id = integer_field(message, "emotion_cms_id");
  const auto refusal = emotion_refusal(emotion_id, context, now);
  outcome.emotion_result = refusal;
  outcome.own.push_back(emotion_response(refusal));
  if (refusal != x5::ec::Success) {
    return outcome;
  }

  last_emotion_tick_ = now;

  // Viewers play the emote where the server knows the player is; the client only chooses
  // which way the character faces.
  const auto &position = *context.position;
  outcome.observers.push_back({kEmotionExecNotificationOpcode,
                               {{"unit_id", std::to_string(player_unit_id_)},
                                {"emotion_cms_id", emotion_id},
                                {"tick", wire_monotonic_tick(now)},
                                {"pos", wire_position(position)},
                                {"yaw", requested_yaw(message, position.yaw)}}});
  return outcome;
}

ExpressionOutcome PlayerExpression::change_selfie(const nlohmann::json &message,
                                                  const ExpressionContext &context) {
  ExpressionOutcome outcome;
  const auto enter = bool_field(message, "enter");

  // A repeated report changes nothing. Entering needs a living player on a loaded map (emulator
  // policy: the client already refuses its selfie camera in those states).
  if (enter == selfie_active_) {
    return outcome;
  }
  if (enter && (!context.position || context.dead)) {
    return outcome;
  }

  selfie_active_ = enter;
  outcome.observers.push_back(selfie_notification(player_unit_id_, enter));
  return outcome;
}

x5::ec::EErrorCode PlayerExpression::emotion_refusal(int emotion_id,
                                                     const ExpressionContext &context,
                                                     std::int64_t now) const {
  if (!context.position || !catalog_->find(emotion_id)) {
    return x5::ec::Emotion_InvalidData;
  }
  if (!owns_emotion(*catalog_, granted_emotions_, emotion_id)) {
    return x5::ec::Emotion_NotOwned;
  }
  if (context.dead) {
    return x5::ec::Emotion_Dead;
  }
  if (context.using_skill) {
    return x5::ec::Emotion_UsingSkill;
  }
  if (context.interacting) {
    return x5::ec::Emotion_Interaction;
  }
  if (context.off_ground) {
    return x5::ec::Emotion_NonGroundIdle;
  }

  if (last_emotion_tick_) {
    const auto required_interval =
        std::max<std::int64_t>(0, catalog_->cooldown_ticks() - kCooldownArrivalAllowanceTicks);
    if (now - *last_emotion_tick_ < required_interval) {
      return x5::ec::Emotion_Cooltime;
    }
  }
  return x5::ec::Success;
}

EmotionGrant plan_emotion_grant(const EmotionCatalog &catalog, const WorldProgress &before,
                                int emotion_id) {
  EmotionGrant grant;
  const auto *definition = catalog.find(emotion_id);
  if (!definition) {
    grant.error = "unknown emotion id";
    return grant;
  }
  if (definition->category == EmotionCategory::basic) {
    grant.error = "Basic emotions are always owned";
    return grant;
  }
  if (before.owned_emotions.contains(emotion_id)) {
    grant.error = "the emotion is already owned";
    return grant;
  }
  if (before.owned_emotions.size() >= kMaximumOwnedEmotions) {
    grant.error = "the owned emotion limit is reached";
    return grant;
  }

  grant.after = before;
  grant.after.revision = before.revision + 1;
  grant.after.owned_emotions.insert(emotion_id);
  grant.notification = {kEmotionsUpdateNotificationOpcode,
                        {{"emotions", emotion_list(std::set<int>{emotion_id})}}};
  return grant;
}

EmotionGrantCommand parse_emotion_grant_command(const nlohmann::json &gm_text_message) {
  EmotionGrantCommand command;
  const auto text = gm_text_message.find("text");
  if (text == gm_text_message.end() || !text->is_string()) {
    return command;
  }

  const auto words = split_words(text->get_ref<const std::string &>());
  if (words.empty() || !is_emotion_grant_alias(ascii_lowercase(words.front()))) {
    return command;
  }

  command.matched = true;
  const auto emotion_id = words.size() == 2 ? positive_integer(words[1]) : std::nullopt;
  if (!emotion_id) {
    command.error = "emotion_add <emotion_id>";
    return command;
  }
  command.emotion_id = emotion_id;
  return command;
}

void SelfieRegistry::set(std::uint64_t unit_id, bool active) {
  std::lock_guard lock(mutex_);
  if (active) {
    active_units_.insert(unit_id);
  } else {
    active_units_.erase(unit_id);
  }
}

std::optional<GameplayPacket>
SelfieRegistry::late_viewer_notification(const nlohmann::json &unit_add_message) const {
  const auto unit_id = unit_id_of(unit_add_message);
  if (!unit_id) {
    return std::nullopt;
  }

  std::lock_guard lock(mutex_);
  if (!active_units_.contains(*unit_id)) {
    return std::nullopt;
  }
  return selfie_notification(*unit_id, true);
}

} // namespace chronicles
