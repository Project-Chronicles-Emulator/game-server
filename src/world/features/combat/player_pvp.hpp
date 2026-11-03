#pragma once

#include "protocol/gameplay_packet.hpp"
#include "persistence/character_state_types.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace chronicles {

inline constexpr std::uint16_t kPlayerPvpFactionNotificationOpcode = 82;

enum class PlayerPvpStatus {
  changed,
  unchanged,
  unknown_player,
  player_dead,
  player_in_combat,
};

struct PlayerPvpResult {
  PlayerPvpStatus status{PlayerPvpStatus::unknown_player};
  bool enabled{};
  std::optional<GameplayPacket> notification;
};

// A flag belongs to an authenticated world connection. The session supplies position,
// instance, death and combat state; none of them may come from a chat payload.
// Both players must opt in before a shared combat engine may damage another player.
class PlayerPvpRegistry {
public:
  // Reconnecting and entering a different map/instance reset the flag to off.
  // Duplicate connection or unit registrations are refused without changing state.
  bool enter(std::uint64_t connection_id, std::uint64_t unit_id, const WorldPosition &position,
             std::uint64_t instance_id = 0);

  // A map/instance change returns an off notification only if the old flag was on.
  PlayerPvpResult update_position(std::uint64_t connection_id, const WorldPosition &position,
                                  std::uint64_t instance_id = 0);

  void leave(std::uint64_t connection_id);
  bool enabled(std::uint64_t connection_id) const;

  PlayerPvpResult set_enabled(std::uint64_t connection_id, bool enabled, bool player_alive = true,
                              bool in_combat = false);

  // Eligibility is necessary, not sufficient: the combat engine must also validate
  // its own visibility, distance, target life and skill/effect rules at hit time.
  bool can_attack(std::uint64_t attacker_connection_id, std::uint64_t target_connection_id) const;

  // Decorates one PlayerNub or a PS2C_UnitAddNoti {unit_id, nub} body.
  // Unknown units are left untouched so NPC and non-player snapshots are preserved.
  void decorate_player_nub(std::uint64_t connection_id, nlohmann::json &player) const;
  void decorate_unit_add(nlohmann::json &message) const;

private:
  struct Participant {
    std::uint64_t unit_id{};
    WorldPosition position;
    std::uint64_t instance_id{};
    bool enabled{};
  };

  static GameplayPacket faction_notification(const Participant &participant);
  static void decorate_player(nlohmann::json &player, bool enabled);

  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, Participant> participants_;
  std::unordered_map<std::uint64_t, std::uint64_t> unit_connections_;
};

} // namespace chronicles
