#include "world/features/companion/companion_session.hpp"

#include "common/clock_ticks.hpp"

#include <cmath>
#include <iterator>

namespace chronicles::companion {
namespace {

using Json = nlohmann::json;
constexpr std::uint16_t kStartAnimationRequest = 20069;
constexpr std::uint16_t kExecuteInteractionRequest = 20070;
constexpr std::uint16_t kEndAnimationRequest = 20071;
constexpr std::uint16_t kStartAnimationResponse = 165;
constexpr std::uint16_t kStartAnimationNotification = 166;
constexpr std::uint16_t kAnimationResult = 167;
constexpr int kRegistrationAnimation = 1013;

// Recovered from every rig's InteractionAnimDetail 1013 montage using
// tutorial_interactions.animation_timing: execute at 3408 ms; finish at 4400 ms.
constexpr std::int64_t kExecuteTicks = 3408 * 10000LL;
constexpr std::int64_t kDurationTicks = 4400 * 10000LL;
constexpr double kMovementToleranceMm = 1000;

bool near(const WorldPosition &first, const WorldPosition &second) {
  const double dx = static_cast<double>(first.x_mm) - second.x_mm;
  const double dy = static_cast<double>(first.y_mm) - second.y_mm;
  const double dz = static_cast<double>(first.z_mm) - second.z_mm;
  return first.map_id == second.map_id &&
         dx * dx + dy * dy + dz * dz <= kMovementToleranceMm * kMovementToleranceMm;
}

Json position_json(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

const Json *item_parameter(const Json &message) {
  const auto param = message.find("param");
  if (param == message.end() || !param->is_object()) {
    return nullptr;
  }
  const auto item = param->find("use_item");
  return item != param->end() && item->is_object() ? &*item : nullptr;
}

} // namespace

bool CompanionSession::handles_registration_animation(std::uint16_t opcode,
                                                      const Json &message) const {
  if (opcode == kStartAnimationRequest) {
    const auto *item = item_parameter(message);
    return item && services_.catalog->summon_registered_by_item(item->value("item_cms_id", 0));
  }
  return registration_ && (opcode == kExecuteInteractionRequest || opcode == kEndAnimationRequest);
}

bool CompanionSession::registration_busy() const noexcept {
  return registration_.has_value();
}

std::optional<std::int64_t> CompanionSession::registration_deadline() const {
  if (!registration_) {
    return std::nullopt;
  }
  return registration_->started + (registration_->executed ? kDurationTicks : kExecuteTicks);
}

GameplayPacket CompanionSession::registration_started() const {
  const auto &pending = registration_.value();
  return {kStartAnimationNotification,
          {{"unit_id", std::to_string(pending.player_unit_id)},
           {"interaction_anim_id", kRegistrationAnimation},
           {"param", {{"use_item", {{"item_cms_id", pending.item_cms_id}}}}},
           {"pos", position_json(pending.position)},
           {"yaw", pending.position.yaw},
           {"tick", wire_monotonic_tick(pending.started)},
           {"weapon_state_type", "EX5WeaponStateType_Sheathed"},
           {"packet_seq", pending.packet_sequence}}};
}

GameplayPacket CompanionSession::registration_finished(const char *error) const {
  return {kAnimationResult,
          {{"unit_id", std::to_string(registration_->player_unit_id)}, {"ec", error}}};
}

SessionResult CompanionSession::receive_registration_animation(
    const WorldGameplayRequest &request, const std::optional<WorldPosition> &position,
    std::uint64_t player_unit_id, std::int64_t now, bool blocked) {
  SessionResult result;
  if (request.opcode != kStartAnimationRequest) {
    if (!registration_) {
      return result;
    }
    const auto &message = request.opcode == kEndAnimationRequest
                              ? request.message.value("end_interaction_anim", Json::object())
                              : request.message;
    if (message.value("interaction_anim_id", 0) != kRegistrationAnimation) {
      result.own.push_back(registration_finished("InteractionAnim_Mismatch"));
      registration_.reset();
      return result;
    }
    if (position) {
      result = advance_registration(now, *position, blocked);
    }
    if (request.opcode == kEndAnimationRequest && registration_) {
      result.own.push_back(registration_finished());
      registration_.reset();
    }
    return result;
  }

  const auto refuse = [&](const char *error) {
    return SessionResult{{{kStartAnimationResponse,
                           {{"ec", error},
                            {"weapon_state_type", "EX5WeaponStateType_Sheathed"},
                            {"packet_seq", request.message.value("packet_seq", Json::object())}}}},
                         {},
                         {}};
  };
  const auto *item = item_parameter(request.message);
  const int item_id = item ? item->value("item_cms_id", 0) : 0;
  const auto animation = services_.catalog->registration_animation(item_id);
  const auto summon = services_.catalog->summon_registered_by_item(item_id);
  if (!entered_ || !position) {
    return refuse("InvalidState");
  }
  if (!item || !summon || animation != kRegistrationAnimation ||
      request.message.value("interaction_anim_id", 0) != kRegistrationAnimation ||
      item->value("item_id", std::string("0")) != "0" ||
      request.message.value("target_unit_id", std::string("0")) != "0") {
    return refuse("InteractionAnim_Mismatch");
  }
  const auto point = request.message.value("pos", Json::object());
  const WorldPosition requested{position->map_id, point.value("millimeter_x", 0),
                                point.value("millimeter_y", 0), point.value("millimeter_z", 0),
                                request.message.value("yaw", position->yaw)};
  if (!request.message.contains("pos") || !std::isfinite(requested.yaw) ||
      !near(requested, *position)) {
    return refuse("TooFar");
  }
  if (blocked || riding()) {
    return refuse("InteractionAnim_UsingSkill");
  }
  if (registration_) {
    if (registration_->item_cms_id != item_id || !near(registration_->position, *position)) {
      return refuse("InteractionAnim_Mismatch");
    }
    // Retransmission does not restart the deadline or consume another registration item.
    result.own.push_back(registration_started());
    return result;
  }
  const auto progress = progress_->load();
  const auto stack = progress.stackables.find(item_id);
  if (progress.dead || progress.registered_summons.contains(*summon)) {
    return refuse("ItemUnusable");
  }
  if (stack == progress.stackables.end() || stack->second <= 0) {
    return refuse("ItemCountError");
  }

  registration_ = RegistrationAnimation{
      item_id,        request.sequence, request.message.value("packet_seq", Json::object()),
      player_unit_id, requested,        now,
      false};
  result.own.push_back(registration_started());
  return result;
}

SessionResult CompanionSession::advance_registration(std::int64_t now,
                                                     const WorldPosition &position, bool blocked) {
  SessionResult result;
  if (!registration_) {
    return result;
  }
  if (!entered_ || blocked || riding() || !near(registration_->position, position)) {
    result.own.push_back(registration_finished("InteractionAnim_Mismatch"));
    registration_.reset();
    return result;
  }
  if (!registration_->executed && now >= registration_->started + kExecuteTicks) {
    result = use_item({{"item_cms_id", registration_->item_cms_id},
                       {"packet_seq", registration_->packet_sequence}},
                      registration_->sequence);
    if (!result.own.empty() && result.own.front().opcode == kCommonErrorResponse) {
      result.own = {registration_finished(
          result.own.front().message.at("ec").get_ref<const std::string &>().c_str())};
      registration_.reset();
      return result;
    }
    registration_->executed = true;
  }
  if (now >= registration_->started + kDurationTicks) {
    result.own.push_back(registration_finished());
    registration_.reset();
  }
  return result;
}

} // namespace chronicles::companion
