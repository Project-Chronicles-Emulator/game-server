#include "world/features/escape/player_escape.hpp"

#include "data/client_catalog.hpp"
#include "common/clock_ticks.hpp"
#include "world/core/world_request_opcodes.hpp"

#include <limits>
#include <stdexcept>
#include <string_view>

#include <spdlog/spdlog.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;

using world_request::kEscapeRequest;
constexpr std::uint16_t kTeleportStartRequest = 20088;
constexpr std::uint16_t kTeleportCompletionRequest = 20089;

constexpr std::uint16_t kUnitChangeAttributeNotification = 33;
constexpr std::uint16_t kAddStatusEffectNotification = 49;
constexpr std::uint16_t kRemoveStatusEffectNotification = 51;
constexpr std::uint16_t kTeleportRequestNotification = 181;

// x5.cms.EX5UnitAttributeType_Invincible, keyed by its number in UnitChangeAttributeNoti.
constexpr char kInvincibleAttributeKey[] = "2";

// Observed for statuses without a catalog duration (50030002 adds): expire_tick INT64_MAX.
constexpr char kPermanentExpireTick[] = "9223372036854775807";

constexpr std::string_view kEscapeConfiguration = "Escape";
constexpr std::int64_t kCentimetersToMillimeters = 10;
constexpr std::int64_t kInternalTicksPerSecond = 10'000'000;
// Plausibility bounds for the catalog values: one kilometer and one day.
constexpr std::int64_t kMaximumCandidateDistanceCm = 100'000;
constexpr std::int64_t kMaximumCooldownSeconds = 86'400;

// Candidate: official status instance ids are small per-unit counters. Combat statuses count
// from 1 and encumbrance statuses from 0x4000'0000, so escape statuses use another disjoint
// range to stay unique on the same player unit.
constexpr std::uint32_t kFirstEscapeInstanceId = 0x5000'0000U;

bool is_trading_escape(const Json &message) {
  const auto trading = message.find("is_trading");
  return trading != message.end() && trading->is_boolean() && trading->get<bool>();
}

// A protobuf WorldPos decoded to JSON omits zero coordinates, so an absent axis is zero.
std::optional<int> coordinate(const Json &position, const char *axis) {
  const auto value = position.find(axis);
  if (value == position.end()) {
    return 0;
  }
  if (!value->is_number_integer()) {
    return std::nullopt;
  }

  const auto millimeters = value->get<std::int64_t>();
  if (millimeters < std::numeric_limits<int>::min() ||
      millimeters > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(millimeters);
}

std::optional<WorldPosition> candidate_position(const Json &message, const WorldPosition &current) {
  const auto candidate = message.find("candidate_pos");
  if (candidate == message.end() || !candidate->is_object()) {
    return std::nullopt;
  }

  const auto x_mm = coordinate(*candidate, "millimeter_x");
  const auto y_mm = coordinate(*candidate, "millimeter_y");
  const auto z_mm = coordinate(*candidate, "millimeter_z");
  if (!x_mm || !y_mm || !z_mm) {
    return std::nullopt;
  }

  // The request carries no yaw; the player keeps facing the current direction.
  return WorldPosition{current.map_id, *x_mm, *y_mm, *z_mm, current.yaw};
}

bool within_distance(const WorldPosition &first, const WorldPosition &second,
                     std::int64_t radius_mm) {
  const auto delta_x = static_cast<std::int64_t>(first.x_mm) - second.x_mm;
  const auto delta_y = static_cast<std::int64_t>(first.y_mm) - second.y_mm;
  const auto delta_z = static_cast<std::int64_t>(first.z_mm) - second.z_mm;
  const auto squared = delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;
  return squared <= radius_mm * radius_mm;
}

Json wire_position(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

GameplayPacket add_status(std::uint64_t unit_id, int status_id, std::uint32_t instance_id,
                          std::int64_t start_tick, std::string expire_tick) {
  const auto unit = std::to_string(unit_id);
  return {kAddStatusEffectNotification,
          {{"unit_id", unit},
           {"status_effect",
            {{"status_effect_id", status_id},
             {"instance_id", instance_id},
             {"level", 1},
             {"stack_count", 1},
             {"caster_unit_id", unit},
             {"start_tick", wire_monotonic_tick(start_tick)},
             {"expire_tick", std::move(expire_tick)}}}}};
}

GameplayPacket remove_status(std::uint64_t unit_id, int status_id, std::uint32_t instance_id,
                             std::int64_t tick) {
  return {kRemoveStatusEffectNotification,
          {{"unit_id", std::to_string(unit_id)},
           {"tick", wire_monotonic_tick(tick)},
           {"status_effects", {{{"instance_id", instance_id}, {"status_effect_id", status_id}}}}}};
}

} // namespace

EscapeRules EscapeRules::load(ClientCatalog &catalog) {
  EscapeRules rules;
  if (!catalog.has_table("GameVariable")) {
    spdlog::warn("{}: catalog has no GameVariable table; using defaults", kEscapeConfiguration);
    return rules;
  }

  const auto distance_cm = game_variable_integer(catalog, "EscapeCandidateMaxDistance", 1,
                                                 kMaximumCandidateDistanceCm,
                                                 kEscapeConfiguration);
  if (distance_cm) {
    rules.candidate_max_distance_mm = *distance_cm * kCentimetersToMillimeters;
  }

  const auto cooldown_seconds = game_variable_integer(catalog, "EscapeCooltimeSec", 0,
                                                      kMaximumCooldownSeconds,
                                                      kEscapeConfiguration);
  if (cooldown_seconds) {
    rules.cooldown_ticks = *cooldown_seconds * kInternalTicksPerSecond;
  }

  const auto cooldown_status = game_variable_integer(
      catalog, "StatusEffectIdForEscapeCooltime", 1, std::numeric_limits<int>::max(),
      kEscapeConfiguration);
  if (cooldown_status) {
    rules.cooldown_status_id = static_cast<int>(*cooldown_status);
  }

  const auto invincible_status = game_variable_integer(
      catalog, "StatusEffectIdForInvincible", 1, std::numeric_limits<int>::max(),
      kEscapeConfiguration);
  if (invincible_status) {
    rules.invincible_status_id = static_cast<int>(*invincible_status);
  }
  return rules;
}

const char *escape_refusal_name(EscapeRefusal refusal) {
  switch (refusal) {
  case EscapeRefusal::none:
    return "none";
  case EscapeRefusal::not_in_world:
    return "not_in_world";
  case EscapeRefusal::dead:
    return "dead";
  case EscapeRefusal::busy:
    return "busy";
  case EscapeRefusal::riding:
    return "riding";
  case EscapeRefusal::cooling_down:
    return "cooling_down";
  case EscapeRefusal::teleport_pending:
    return "teleport_pending";
  case EscapeRefusal::missing_candidate:
    return "missing_candidate";
  case EscapeRefusal::candidate_too_far:
    return "candidate_too_far";
  }
  return "unknown";
}

PlayerEscape::PlayerEscape(EscapeRules rules, std::uint64_t player_unit_id)
    : rules_(rules), player_unit_id_(player_unit_id), next_instance_id_(kFirstEscapeInstanceId) {
  if (rules_.candidate_max_distance_mm <= 0 || rules_.cooldown_ticks < 0) {
    throw std::invalid_argument("Escape rules need a positive range and a nonnegative cooldown");
  }
}

bool PlayerEscape::handles(std::uint16_t opcode, const nlohmann::json &message) const {
  switch (opcode) {
  case kEscapeRequest:
    return !is_trading_escape(message);
  case kTeleportStartRequest:
    // Another owner (a tutorial cutscene) may have its own teleport in flight; the echoed tick
    // tells them apart.
    return teleport_ && teleport_->phase == TeleportPhase::requested &&
           echoes_pending_tick(message);
  case kTeleportCompletionRequest:
    return teleport_ && teleport_->phase == TeleportPhase::started;
  default:
    return false;
  }
}

EscapeOutcome PlayerEscape::receive(std::uint16_t opcode, const nlohmann::json &message,
                                    const EscapeContext &context, std::int64_t now) {
  switch (opcode) {
  case kEscapeRequest:
    return request(message, context, now);
  case kTeleportStartRequest:
    return start_teleport(message);
  case kTeleportCompletionRequest:
    return complete_teleport(now);
  default:
    throw std::invalid_argument("Escape does not handle this opcode");
  }
}

std::vector<GameplayPacket> PlayerEscape::expire(std::int64_t now) {
  if (!cooldown_ || now < cooldown_->expire_tick) {
    return {};
  }

  std::vector<GameplayPacket> packets;
  packets.push_back(
      remove_status(player_unit_id_, rules_.cooldown_status_id, cooldown_->instance_id, now));
  cooldown_.reset();
  return packets;
}

std::optional<std::int64_t> PlayerEscape::next_expiry_tick() const {
  if (!cooldown_) {
    return std::nullopt;
  }
  return cooldown_->expire_tick;
}

bool PlayerEscape::teleport_pending() const {
  return teleport_.has_value();
}

EscapeOutcome PlayerEscape::request(const nlohmann::json &message, const EscapeContext &context,
                                    std::int64_t now) {
  EscapeOutcome outcome;
  // A cooldown whose removal timer has not fired yet must not block a request at its expiry.
  outcome.packets = expire(now);

  std::optional<WorldPosition> destination;
  outcome.refusal = refusal_for(message, context, destination);
  if (outcome.refusal != EscapeRefusal::none) {
    return outcome;
  }

  teleport_ = PendingTeleport{*destination, now, TeleportPhase::requested};
  outcome.packets.push_back({kTeleportRequestNotification,
                             {{"tick", wire_monotonic_tick(now)},
                              {"unit_id", std::to_string(player_unit_id_)},
                              {"pos", wire_position(*destination)},
                              {"body_yaw", destination->yaw}}});

  // Observed: the cooldown status follows the TeleportReq and starts at the request.
  const CooldownStatus cooldown{allocate_instance_id(), now + rules_.cooldown_ticks};
  cooldown_ = cooldown;
  outcome.packets.push_back(add_status(player_unit_id_, rules_.cooldown_status_id,
                                       cooldown.instance_id, now,
                                       wire_monotonic_tick(cooldown.expire_tick)));
  return outcome;
}

EscapeOutcome PlayerEscape::request_server_teleport(const WorldPosition &destination,
                                                    std::int64_t now) {
  EscapeOutcome outcome;
  if (teleport_) {
    outcome.refusal = EscapeRefusal::teleport_pending;
    return outcome;
  }

  teleport_ = PendingTeleport{destination, now, TeleportPhase::requested};
  outcome.packets.push_back({kTeleportRequestNotification,
                             {{"tick", wire_monotonic_tick(now)},
                              {"unit_id", std::to_string(player_unit_id_)},
                              {"pos", wire_position(destination)},
                              {"body_yaw", destination.yaw}}});
  return outcome;
}

EscapeRefusal PlayerEscape::refusal_for(const nlohmann::json &message,
                                        const EscapeContext &context,
                                        std::optional<WorldPosition> &destination) const {
  if (!context.position) {
    return EscapeRefusal::not_in_world;
  }
  if (context.dead) {
    return EscapeRefusal::dead;
  }
  if (context.busy) {
    return EscapeRefusal::busy;
  }
  // Candidate: moving the rider alone would leave the ridden unit behind.
  if (context.riding) {
    return EscapeRefusal::riding;
  }
  if (cooldown_) {
    return EscapeRefusal::cooling_down;
  }
  if (teleport_) {
    return EscapeRefusal::teleport_pending;
  }

  destination = candidate_position(message, *context.position);
  if (!destination) {
    return EscapeRefusal::missing_candidate;
  }
  if (!within_distance(*destination, *context.position, rules_.candidate_max_distance_mm)) {
    destination.reset();
    return EscapeRefusal::candidate_too_far;
  }
  return EscapeRefusal::none;
}

EscapeOutcome PlayerEscape::start_teleport(const nlohmann::json &message) {
  EscapeOutcome outcome;
  if (!handles(kTeleportStartRequest, message)) {
    return outcome;
  }

  teleport_->phase = TeleportPhase::started;
  outcome.packets.push_back(invincible_attribute(1));
  return outcome;
}

EscapeOutcome PlayerEscape::complete_teleport(std::int64_t now) {
  EscapeOutcome outcome;
  if (!handles(kTeleportCompletionRequest, Json::object())) {
    return outcome;
  }

  // Observed: the invincibility status starts at the TeleportReq tick and has no expiry; it is
  // removed after the sight refresh (about 36 ms later officially, immediately here).
  const auto instance_id = allocate_instance_id();
  outcome.packets.push_back(add_status(player_unit_id_, rules_.invincible_status_id, instance_id,
                                       teleport_->tick, kPermanentExpireTick));
  outcome.packets.push_back(invincible_attribute(0));
  outcome.packets.push_back(
      remove_status(player_unit_id_, rules_.invincible_status_id, instance_id, now));

  outcome.relocated_to = teleport_->destination;
  teleport_.reset();
  return outcome;
}

// TeleportStartReq echoes the wire tick of the TeleportReq it answers.
bool PlayerEscape::echoes_pending_tick(const nlohmann::json &message) const {
  const auto echoed = message.find("tick");
  return teleport_ && echoed != message.end() && echoed->is_string() &&
         echoed->get<std::string>() == wire_monotonic_tick(teleport_->tick);
}

std::uint32_t PlayerEscape::allocate_instance_id() {
  const auto instance_id = next_instance_id_;
  if (next_instance_id_ == std::numeric_limits<std::uint32_t>::max()) {
    next_instance_id_ = kFirstEscapeInstanceId;
  } else {
    ++next_instance_id_;
  }
  return instance_id;
}

GameplayPacket PlayerEscape::invincible_attribute(int value) const {
  return {kUnitChangeAttributeNotification,
          {{"unit_id", std::to_string(player_unit_id_)},
           {"attributes", {{kInvincibleAttributeKey, value}}}}};
}

} // namespace chronicles
