#pragma once

#include "protocol/gameplay_packet.hpp"
#include "persistence/character_state_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles {

class ClientCatalog;

// Player escape ("unstuck", system menu Escape) and the server teleport handshake it starts.
//
// Evidence (docs/player-escape.md):
// - Official capture generic-part-2: PC2S_EscapeReq{candidate_pos} -> PS2C_TeleportReq ->
//   AddStatusEffectNoti 50030003 (expire = start + 300 s) -> PC2S_TeleportStartReq{tick} ->
//   UnitChangeAttributeNoti{Invincible: 1} -> PC2S_TeleportCompletionReq -> AddStatusEffectNoti
//   50030002 (no expiry) -> sight refresh -> UnitChangeAttributeNoti{Invincible: 0} ->
//   RemoveStatusEffectNoti 50030002; RemoveStatusEffectNoti 50030003 about 300 s later.
// - Static client: the escape menu projects the local player onto the client navigation mesh
//   and sends that point as candidate_pos; it refuses locally while the escape-cooldown status
//   is active. The TeleportReq handler answers with TeleportStartReq, moves the pawn and sends
//   TeleportCompletionReq.
// - Catalog GameVariables: EscapeCandidateMaxDistance 500, EscapeCooltimeSec 300,
//   StatusEffectIdForEscapeCooltime 50030003, StatusEffectIdForInvincible 50030002.
//
// Candidate: the official destination was about 2.2 m from the client candidate (its server
// navigation choice is unresolved). The emulator has no server navigation data, so it teleports
// to the client candidate after checking it lies within EscapeCandidateMaxDistance.
struct EscapeRules {
  // GameVariable EscapeCandidateMaxDistance is in client world units (centimeters).
  std::int64_t candidate_max_distance_mm{5'000};
  // GameVariable EscapeCooltimeSec, in internal 100 ns monotonic ticks.
  std::int64_t cooldown_ticks{300LL * 10'000'000LL};
  int cooldown_status_id{50030003};
  int invincible_status_id{50030002};

  // Reads the four GameVariables; a missing or implausible row keeps the default above (the
  // value exported by the verified catalog) and logs a warning.
  static EscapeRules load(ClientCatalog &catalog);
};

// Why an escape request was refused. Refusals send no frames: no official refusal reply is
// captured, and the client re-enables its menu once its own two-second guard has elapsed.
enum class EscapeRefusal {
  none,
  not_in_world,
  dead,
  busy,
  riding,
  cooling_down,
  teleport_pending,
  missing_candidate,
  candidate_too_far,
};

const char *escape_refusal_name(EscapeRefusal refusal);

// Session conditions sampled when an escape request arrives.
struct EscapeContext {
  // The server's current position of the player; absent before map entry or after exit.
  std::optional<WorldPosition> position;
  bool dead{};
  // Another action (skill, interaction, gathering) is in progress.
  bool busy{};
  bool riding{};
};

struct EscapeOutcome {
  std::vector<GameplayPacket> packets;
  EscapeRefusal refusal{EscapeRefusal::none};
  // Set when the teleport handshake completed; the session adopts it as the authoritative
  // player position.
  std::optional<WorldPosition> relocated_to;
};

// Per-session escape state. Confined to the session worker strand.
class PlayerEscape {
public:
  PlayerEscape(EscapeRules rules, std::uint64_t player_unit_id);

  // True for a non-trading EscapeReq, for the TeleportStartReq echoing this escape's
  // TeleportReq tick, and for the TeleportCompletionReq that follows it. Trading escapes belong
  // to the trade session; other teleports (tutorial cutscenes) keep their own handshake.
  bool handles(std::uint16_t opcode, const nlohmann::json &message) const;

  // `now` is the internal 100 ns monotonic tick.
  EscapeOutcome receive(std::uint16_t opcode, const nlohmann::json &message,
                        const EscapeContext &context, std::int64_t now);

  // Server-initiated teleport inside the current map (game master commands). Sends the same
  // TeleportReq as an escape, without the escape cooldown or distance limit; the client's
  // TeleportStartReq/TeleportCompletionReq then finish it through receive(). Refused with
  // teleport_pending while another teleport of this session is in flight.
  EscapeOutcome request_server_teleport(const WorldPosition &destination, std::int64_t now);

  // RemoveStatusEffectNoti for an escape cooldown whose expiry has passed; empty otherwise.
  std::vector<GameplayPacket> expire(std::int64_t now);

  // Internal tick at which expire() will next produce a removal; absent without a cooldown.
  std::optional<std::int64_t> next_expiry_tick() const;

  bool teleport_pending() const;

private:
  enum class TeleportPhase { requested, started };

  struct PendingTeleport {
    WorldPosition destination;
    // Internal tick of the TeleportReq; its wire form is echoed by TeleportStartReq.
    std::int64_t tick{};
    TeleportPhase phase{TeleportPhase::requested};
  };

  struct CooldownStatus {
    std::uint32_t instance_id{};
    std::int64_t expire_tick{};
  };

  EscapeOutcome request(const nlohmann::json &message, const EscapeContext &context,
                        std::int64_t now);

  EscapeOutcome start_teleport(const nlohmann::json &message);

  EscapeOutcome complete_teleport(std::int64_t now);

  EscapeRefusal refusal_for(const nlohmann::json &message, const EscapeContext &context,
                            std::optional<WorldPosition> &destination) const;

  bool echoes_pending_tick(const nlohmann::json &message) const;

  std::uint32_t allocate_instance_id();

  GameplayPacket invincible_attribute(int value) const;

  EscapeRules rules_;
  std::uint64_t player_unit_id_{};
  std::optional<PendingTeleport> teleport_;
  std::optional<CooldownStatus> cooldown_;
  std::uint32_t next_instance_id_{};
};

} // namespace chronicles
