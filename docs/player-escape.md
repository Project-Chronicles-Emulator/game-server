# Player escape

`src/world/features/escape/player_escape.*` answers the system-menu Escape ("unstuck") request and runs the server
teleport handshake it starts. The world session routes a non-trading `PC2S_EscapeReq`
(20048) to it in tutorial, followup and shared-map sessions. A trading escape
(`is_trading=true`) stays with the trade run (`docs/vehicles-and-trade.md`).

## Evidence

**Official capture** (generic-part-2, the only escape in the corpus; build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa`). Order and fields:

1. `PC2S_EscapeReq{candidate_pos}`; `is_trading` absent (false).
2. `PS2C_TeleportReq` (181) `{tick, unit_id, pos, body_yaw}` about 21 ms later. Reason is the
   default `TeleportReason_Normal`.
3. `PS2C_AddStatusEffectNoti` (49) for status 50030003, `expire_tick = start_tick + 300 s`.
4. `PC2S_TeleportStartReq{tick}` echoing the TeleportReq tick, then `PC2S_TeleportCompletionReq`.
5. `PS2C_UnitChangeAttributeNoti` (33) `{2: 1}` (`EX5UnitAttributeType_Invincible`).
6. `AddStatusEffectNoti` 50030002 with `start_tick` equal to the TeleportReq tick and
   `expire_tick` INT64_MAX.
7. A sight refresh: UnitRemoveNoti for every visible unit, then UnitAddNoti for those in sight.
8. `UnitChangeAttributeNoti {2: 0}`, then `RemoveStatusEffectNoti` (51) of 50030002 about
   36 ms after the teleport tick.
9. `RemoveStatusEffectNoti` of 50030003 about 300.1 s after it was added.

Other captured TeleportReq flows (cutscene and revival teleports) use the same start/completion
handshake. Those that stay on the same map also show the invincibility attribute pair and the
50030002 add/remove.

**Catalog GameVariables:** `EscapeCandidateMaxDistance=500`, `EscapeCooltimeSec=300`,
`StatusEffectIdForEscapeCooltime=50030003`, `StatusEffectIdForInvincible=50030002`.
Status 50030003 is `ui_type=Hidden`, `jsonCase=escapeCooltime`.

**Static client** (IDA, `dumps/ida_research/escape-20261026/`):

- `AAC_SystemMenu_Escape_Candidate_OnSelected` refuses with system message 452 while the
  cooldown query is positive; otherwise it sends the request and shows message 316.
- `AAC_Escape_LocalPlayer_TrySendEscapeReq` projects the local pawn onto the client
  navigation mesh and sends that point, converted from centimeters to millimeters, as
  `candidate_pos`. A failed projection sends no `candidate_pos`.
- `AAC_Escape_LocalPlayer_Candidate_GetCooldownRemaining` applies a two-second local guard
  after each request. It then reads timing from the player unit, interpreted as the
  escape-cooldown status (Candidate).
- `AAC_PS2C_TeleportReq_Candidate_BeginTeleportSuspendTriggers` sends TeleportStartReq with
  the received tick, moves the local pawn and sends TeleportCompletionReq. It does not clear
  other units itself.

## Emulator behavior

An accepted escape sends TeleportReq to the destination with the player's current yaw, then
the 50030003 cooldown status. The matching TeleportStartReq sends `{2: 1}`. The completion
sends 50030002 (no expiry), `{2: 0}` and the 50030002 removal, and makes the destination the
session's authoritative position. The position is saved with the session on disconnect.
A session timer sends the 50030003 removal at expiry. A request arriving exactly at expiry
first removes the old cooldown.

The rules load from the four GameVariables at listener startup. A missing or implausible row
keeps the exported value and logs a warning. Sessions without a catalog use those values.

Refusals send no frames, because no official refusal reply was captured. The client clears
its own two-second guard afterwards. Refused requests leave no cooldown. Reasons are logged:

- not in a loaded map, dead, busy with another action, or riding (Candidate);
- the cooldown is active, or an escape teleport is still pending;
- `candidate_pos` is missing or malformed;
- the candidate lies more than `EscapeCandidateMaxDistance` (500 cm = 5,000 mm, 3D) from the
  server's current position. The centimeter interpretation matches the client's
  centimeter-to-millimeter conversion of the candidate itself.

TeleportStartReq is claimed only when its tick echoes the pending escape TeleportReq, and
TeleportCompletionReq only after that start. Other teleports, such as the tutorial cutscene
teleport, keep their own handshake. Escape status instance ids use a range disjoint from
combat (from 1) and encumbrance (from 0x4000_0000) statuses.

## Limits

- **Destination (Candidate):** the official destination was about 2.2 m from the client
  candidate. Its server navigation choice is unresolved. Without server navigation data, the
  emulator trusts the client's navmesh candidate once it passes the distance check.
- No sight refresh is sent after completion. The move is at most 5 m, and the client keeps its
  units during a teleport. Observers learn the new position from the client's next MoveReq.
  `PS2C_TeleportNoti` (182, observed for other players' teleports) is not broadcast.
- The cooldown lives in the session only. A reconnect clears it, and the client receives no
  50030003 status on entry. Official cooldown persistence is unknown.
- The 50030002 removal is immediate instead of the official ~36 ms delay.
- The dead/busy/riding refusals are local policy; the official server's conditions are unknown.
- Verification is synthetic: `src/world/features/escape/tests/player_escape_tests.cpp` and
  `NativeWorld.OverworldEscapeTeleportsToTheCandidateAndSavesTheDestination` over the real
  TLS world transport. Original-client acceptance has not been observed.

## Player command

`!unstuck` is a separate server command that does not rely on the client finding a navmesh
candidate. It selects the nearest catalog-backed Nui idol or flame on the current map, or on
the parent map if an instance has no local shrine. Both same-map and parent-map destinations
use the normal map-move lifecycle, refreshing visibility at the selected location.
The command refuses while dead, riding, busy, trading, in housing, or during another teleport
or cutscene. It has a five-minute session-local cooldown; reconnecting clears that cooldown.
If no known safe destination is available it reports the failure and does not move the player.
The command and map-move path have synthetic tests; original-client acceptance is pending.
