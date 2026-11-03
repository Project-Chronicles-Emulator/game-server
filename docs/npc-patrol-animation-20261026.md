# Civilian patrol animation: deployed speed-stat omission

The user observed NPCs stepping forward without a walking animation after the
civilian routes appeared in the local game. This is an observed failure of that
running artifact, despite the earlier synthetic route checks passing.

## Confirmed artifact mismatch

The running executable is byte-identical to the map-load-timer/zone-channel
baseline. Its frozen `npc_population.cpp` lacks the later movement-speed-stat
initialization in the current workspace. A read-only snapshot of the local native
client log identifies all 116 generated patrol actors and finds no stat 84 in any
of their NPC announcement nubs. The server log writes MoveNoti 177; the client's
native packet logger contains no 177 records, so it cannot supply a movement
cadence or client-velocity trace. Only sanitized counts/hashes are recorded here.

The current source seeds `EX5StatType_MoveSpeedRate` 84 at 1000 for patrol NPCs.
That is required by the client's normal movement-speed calculation. Merely having
this source in the checkout did not establish that the deployed binary included it.

## Client control flow

The matching build is `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`, dump SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`, base
`0x140000000`.

- `0x14601E080` normally computes the maximum movement speed from base movement
  attribute 0 times stat 84 times 0.001, with optional attribute overrides/modifiers.
  Missing/zero stat 84 can therefore zero this speed cap.
- `0x145CC16A0` clears derived movement speed and accepts a positive measured speed
  only when it does not exceed twice the movement component's current maximum.
- `0x145CC3380` needs accepted nonzero speed to write sustained movement-component
  velocity. Other correction/timeout paths can change position without that velocity.
- The Walk/Run detail flags and prediction fields are distinct from this cap and
  velocity. Sending Walk alone is insufficient evidence of walking animation.

These static branches and the missing live stat establish a concrete defect; they
do not prove this is the only remaining visual defect. No artificial animation,
movement skill, altered cadence or client-memory patch was added.

The initial eighteen rechecked functions have repeatable comments, bounded semantic renames,
fingerprints and an annotation journal under
`../dumps/ida_research/npc-patrol-animation-20261026/`. Existing meaningful names
were preserved. The matching IDB saved in place with stored-section verification
and a backup at `dumps/ArcheAgeChronicles.memdump.exe.i64.bak`.

## Correction and validation

An isolated CMake C++20 Release build reuses immutable copies of the running
artifact's library archives and compiles the current corrected population source
as an explicit object before archive extraction. All other runtime archive members
and frozen server source are retained. The executable link map identifies the
corrected population object. This avoids replacing newer deployed channel/map-load
behavior with the earlier patrol-only candidate.

The real-map native socket regression now requires neutral stat 84 on every patrol
announcement, including actors paused when the connection enters. Linked against
the running build's unchanged population, this test fails at the missing stats
field. Linked with the corrected population, it passes for all 116 patrol actors.
All eight selected native TLS/MemoryStore entry, reconnect, ownership, tutorial
isolation and housing transition checks pass without skips. TLS uses the existing
child-process `OPENSSL_ia32cap=0` diagnostic; normal OpenSSL dispatch is not repaired.

Artifacts, sanitized live evidence, archive/source/binary fingerprints and the
ready executable with matching dependency DLLs are under ignored
`out/npc-patrol-animation-20261026/`. No player state or database schema changes.
The user then explicitly authorized "restart now". The old sessions drained through
the existing admin API, the service shut down gracefully, and the corrected binary
started on the same HTTP/world ports with the same frozen migration and settings.
Its executable hash matches the tested candidate. The PostgreSQL listener retains
the same process identity; no database reset occurred. The previous executable is
retained for rollback. Reconnect and original-client observation must still verify
smooth travel and animation before describing the visual issue as fixed.

## Speed-stat-only trial failed; typed identity correction

After the first restart the current native log confirms all 116 patrol actors
receive stat 84=1000. The user still observes stepping without walking or turning.
The speed-stat omission was a real artifact defect, but fixing it alone did not
resolve the visual issue.

A second contract defect is confirmed in the allocator: sequential ids starting
at 0x80000000 assign every possible low nibble, although the client uses it as
the entity type. Only 17 of 261 announcements had NPC type 2. Registration
`0x145EBAA00` selects one of eight typed tables and rejects nibbles 8..15.
Character lookup `0x145EB8C40` accepts only types 1 and 2 and is used by the
movement-speed setter. Wrongly typed NPCs therefore fail character lookups or
registration; this is independent of supplying the speed rate in the nub.

Read-only kernel inspection confirmed the matching live movement-function bytes,
manager layout and mismatched typed tables. A type-0 patrol sample had no accepted
movement tick; other types retained default movement caps. Actor pointers can be
replaced by distance culling, so stale pawn pointers were discarded. These are
local diagnostic observations, not a universal rendering claim.

The allocator now uses `0x80000002 + 16 * placement_index`, preserving the fixed
placement sort order and the reserved range. Every NPC retains type 2, uniqueness
and stable identity across reconnects. Runtime NPC ids are not persisted, so no
player/database migration is required. The same unit id is used in announcements,
movement, activities and the interaction registry. The client-static exclusion
and 145 stationary plus 116 patrol placements are unchanged.

The isolated Release build passes all 41 population/patrol/registry/player-id
module tests and eight native socket cases, with no skips or failures. The real-map
socket regression checks every announced NPC's type nibble and every patrol's
speed rate. The typed executable is running on ports 8080/8081; PostgreSQL retains
its process identity and both prior executables are retained. The source header
and corrected population object are the only changed runtime inputs relative to
the deployed baseline archives. Thirty-nine inspected functions have saved
comments, fingerprints and append-only journal records in the research directory;
the IDB save again passed stored-section verification.

The user stopped Computer Use with physical Escape before the typed build's
reconnect/visual trial. No further UI actions were sent. Smooth walking and turning
on this final binary are still unverified; do not report the animation as fixed.

## Human-confirmed final client result

After the typed-ID restart, the user confirmed on 2026-10-26 that NPC movement is
now working. This confirms the reported walking/turning repair on the running
typed candidate `e9b03ff9be533c3a0f6946de33b9bb1a5609fb23d5dec86ec47c59c8e45fa36d`.
It is a human-observed original-client result, distinct from the 49 selected
synthetic checks. No further Computer Use input was needed. This acceptance does
not extend to unresolved work-point scripts, navigation or all civilian AI.
