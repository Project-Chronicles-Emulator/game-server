# Player PvP flag

The server owns a per-world-connection opt-in flag. `!pvp on`, `!pvp off` and
`!pvp status` use the authenticated character's active world connection. The command does
not transfer the character to another realm or channel. `!help` lists the command syntax.
The server also accepts forwarded `/pvp` and `/help` aliases. The original demo client consumes
unknown slash commands locally; its normal chat UI does not forward those slash aliases.
Use the bang forms with the stock client. See the static routing record under
`dumps/ida_research/player-chat-commands-20261026/`.

`PlayerPvpRegistry` in `src/world/features/combat/player_pvp.hpp` stores the flag, wire unit ID, map and instance.
Only a registered world connection can change its own flag. A changed flag produces
`PS2C_ChangeFactionNoti` (82), with `unit_id` and `type` set to
`EX5Faction_PvP_MODE` (on) or `EX5Faction_Player` (off). The world adapter sends the
notification to the owner and its current observers. New observer UnitAdd snapshots are
decorated from the same registry, so leaving and returning to sight retains the current flag.

The flag starts off on every connection, resets off when its map or instance changes, and is
removed on disconnect. Ordinary movement retains it. A dead character or a character performing
an action cannot change it. Repeating the existing setting emits no redundant faction packet.
These are emulator policies; the official game's transition cooldown rules are not implemented.
The extracted GameVariable table contains `PKMode_Cooldown_IntoPK=60000` and
`PKMode_Cooldown_IntoNormal=180000`; their exact client/server application was not recovered.

The native `PlayerNub.pk_channel_activated` field remains false. It describes the official
PK channel; an individual faction opt-in does not prove the character joined that channel.
The player faction in the snapshot is `PlayerNub.faction_type` (field 13).

The registry's `can_attack` requires two distinct, registered, opted-in players on the same
positive map and instance. Private tutorial instances use distinct instance keys. Housing keeps
the real map and derives a separate instance namespace from its channel's synthetic presence map.
A future shared combat engine must additionally validate visibility, distance, life and skill
rules at the actual hit time; the flag is not permission to accept client-reported damage.

## Current combat limitation

The existing `CombatSession::start` and `CombatSession::targets` accept local tutorial monsters
as hostile targets. They do not schedule attacks against another player's combat state. This
change implements the requested command and authoritative flag replication, but does not add
shared player-versus-player damage, kills or rewards. `can_attack` is an explicit policy seam
for that future integration, not a claim that the combat engine currently uses it.

## Evidence and validation

- Build: `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`; dumped input SHA-256:
  `e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`.
- `protocol/opcodes.json` establishes opcode 82 through the wrapper opcode/name getters.
  `protocol/descriptors.normalized.pb` establishes `ChangeFactionNoti.unit_id=1`,
  `ChangeFactionNoti.type=2`, and the faction enum values Player=0 / PvP_MODE=16.
- The extracted `BasicFaction` table says PvP_MODE treats itself as Enemy; Player treats
  PvP_MODE as Friendly. The server intentionally requires both players to opt in.
- IDA constructor `AAC_PS2C_ChangeFactionNoti_DecodeWrapper` at `0x1461989b0` builds the
  matching packet wrapper and parses its payload. The gameplay faction-application callback
  was not recovered, so original-client visual/targeting acceptance remains unverified.
- `dumps/ida_research/player-pvp-20261026/function-index.v1.json` and its pseudocode cache
  record the four bounded analyzed functions and their uncertainty. IDA `idb_save` confirmed
  `stored-sections`, in-place, at `dumps/ArcheAgeChronicles.memdump.exe.i64`.
- `PlayerPvp.*` tests cover registration ownership, death/action refusal, flag packets,
  same-map/instance double opt-in, map reset, teardown/reconnect, late observer decoration,
  malformed IDs, preservation of unrelated snapshots and native-schema encoding. They are
  synthetic checks. `NativeWorld.PlayerPvpFlagReachesOwnerObserversAndResetsAfterReconnect`
  exercises a real TLS listener, native faction packets to the owner and observer, the async
  command result, entry callback deduplication, reconnect reset and wrong-world rejection.
  No original-client flag trial has been observed in this change.

The final 2026-10-26 Release run passed all seven `PlayerPvp` cases and the native TLS
owner/observer/reconnect case. The isolated server-message smoke also passed `!pvp status`
through the real world-session callback. TLS used the documented process-local
`OPENSSL_ia32cap=0` diagnostic. See `VALIDATION.md` for the binary receipt and unrelated
GM test failure in the wider suite.
