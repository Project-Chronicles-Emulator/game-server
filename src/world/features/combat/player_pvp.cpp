#include "world/features/combat/player_pvp.hpp"

#include <charconv>
#include <string>
#include <system_error>

namespace chronicles {

namespace {

constexpr const char *kNormalPlayerFaction = "EX5Faction_Player";
constexpr const char *kPvpPlayerFaction = "EX5Faction_PvP_MODE";

std::optional<std::uint64_t> parse_unit_id(const std::string &text) {
  std::uint64_t unit_id{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), unit_id);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || unit_id == 0) {
    return std::nullopt;
  }

  return unit_id;
}

} // namespace

bool PlayerPvpRegistry::enter(std::uint64_t connection_id, std::uint64_t unit_id,
                              const WorldPosition &position, std::uint64_t instance_id) {
  if (connection_id == 0 || unit_id == 0 || position.map_id <= 0) {
    return false;
  }

  std::lock_guard lock(mutex_);
  if (participants_.contains(connection_id) || unit_connections_.contains(unit_id)) {
    return false;
  }

  participants_.emplace(connection_id, Participant{unit_id, position, instance_id, false});
  unit_connections_.emplace(unit_id, connection_id);
  return true;
}

PlayerPvpResult PlayerPvpRegistry::update_position(std::uint64_t connection_id,
                                                   const WorldPosition &position,
                                                   std::uint64_t instance_id) {
  std::lock_guard lock(mutex_);
  const auto found = participants_.find(connection_id);
  if (found == participants_.end()) {
    return {};
  }

  auto &participant = found->second;
  const bool changed_instance =
      participant.position.map_id != position.map_id || participant.instance_id != instance_id;
  participant.position = position;
  participant.instance_id = instance_id;
  if (!changed_instance || !participant.enabled) {
    return {PlayerPvpStatus::unchanged, participant.enabled, std::nullopt};
  }

  participant.enabled = false;
  return {PlayerPvpStatus::changed, false, faction_notification(participant)};
}

void PlayerPvpRegistry::leave(std::uint64_t connection_id) {
  std::lock_guard lock(mutex_);
  const auto found = participants_.find(connection_id);
  if (found == participants_.end()) {
    return;
  }

  unit_connections_.erase(found->second.unit_id);
  participants_.erase(found);
}

bool PlayerPvpRegistry::enabled(std::uint64_t connection_id) const {
  std::lock_guard lock(mutex_);
  const auto found = participants_.find(connection_id);
  return found != participants_.end() && found->second.enabled;
}

PlayerPvpResult PlayerPvpRegistry::set_enabled(std::uint64_t connection_id, bool enabled,
                                               bool player_alive, bool in_combat) {
  std::lock_guard lock(mutex_);
  const auto found = participants_.find(connection_id);
  if (found == participants_.end()) {
    return {};
  }

  auto &participant = found->second;
  if (participant.enabled == enabled) {
    return {PlayerPvpStatus::unchanged, participant.enabled, std::nullopt};
  }
  if (!player_alive) {
    return {PlayerPvpStatus::player_dead, participant.enabled, std::nullopt};
  }
  if (in_combat) {
    return {PlayerPvpStatus::player_in_combat, participant.enabled, std::nullopt};
  }

  participant.enabled = enabled;
  return {PlayerPvpStatus::changed, enabled, faction_notification(participant)};
}

bool PlayerPvpRegistry::can_attack(std::uint64_t attacker_connection_id,
                                   std::uint64_t target_connection_id) const {
  if (attacker_connection_id == target_connection_id) {
    return false;
  }

  std::lock_guard lock(mutex_);
  const auto attacker = participants_.find(attacker_connection_id);
  const auto target = participants_.find(target_connection_id);
  if (attacker == participants_.end() || target == participants_.end()) {
    return false;
  }

  return attacker->second.enabled && target->second.enabled &&
         attacker->second.position.map_id > 0 &&
         attacker->second.position.map_id == target->second.position.map_id &&
         attacker->second.instance_id == target->second.instance_id;
}

void PlayerPvpRegistry::decorate_player_nub(std::uint64_t connection_id,
                                            nlohmann::json &player) const {
  std::lock_guard lock(mutex_);
  const auto found = participants_.find(connection_id);
  if (found != participants_.end() && player.is_object()) {
    decorate_player(player, found->second.enabled);
  }
}

void PlayerPvpRegistry::decorate_unit_add(nlohmann::json &message) const {
  if (!message.is_object()) {
    return;
  }

  const auto unit = message.find("unit_id");
  const auto nub = message.find("nub");
  if (unit == message.end() || !unit->is_string() || nub == message.end() || !nub->is_object()) {
    return;
  }

  const auto unit_id = parse_unit_id(unit->get_ref<const std::string &>());
  const auto player = nub->find("player");
  if (!unit_id || player == nub->end() || !player->is_object()) {
    return;
  }

  std::lock_guard lock(mutex_);
  const auto connection = unit_connections_.find(*unit_id);
  if (connection != unit_connections_.end()) {
    decorate_player(*player, participants_.at(connection->second).enabled);
  }
}

GameplayPacket PlayerPvpRegistry::faction_notification(const Participant &participant) {
  return {kPlayerPvpFactionNotificationOpcode,
          {{"unit_id", std::to_string(participant.unit_id)},
           {"type", participant.enabled ? kPvpPlayerFaction : kNormalPlayerFaction}}};
}

void PlayerPvpRegistry::decorate_player(nlohmann::json &player, bool enabled) {
  player["faction_type"] = enabled ? kPvpPlayerFaction : kNormalPlayerFaction;
  // This flag reports the official channel, not an individual faction toggle.
  // Keep it false: the emulator command does not transfer the player to a PK channel.
  player["pk_channel_activated"] = false;
}

} // namespace chronicles
