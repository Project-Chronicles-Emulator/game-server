# Combat rework — 2026-10-26

This is a local implementation and verification record for the reported invisible ranged
skills, repeated NPC attacks and missing area damage. Original-client visual acceptance
against the revised executable remains pending. The feature is still partial.

## Recovered inputs

The same-build catalog and seven official captures supply minimum/maximum range, cooldown
groups, action layers/stages, cast and release windows, unique effect events, projectile speed,
lifetime, radius and flight type, and area lifetime, shape, dimensions and pulse interval.
Action stage duration and effect time remain distinct: neither is a universal cast-time value.
Conditional perk/boss rules are not inferred from a timing average.

Re-extraction adds 379 NPC/skill memberships to the previous input and retains 176 NPC stat
definitions. The generated candidate contains 1,008 actions, 1,245 effects and 79 interval
areas, including three boxes. These are data coverage counts, not completed gameplay features.
Only the existing tutorial instances have an authoritative CombatSession.

The importer now begins the player's action clock at its request and preserves a stage input
that arrives before the echoed start. Thunderbolt 2249 effect 200004 is attributed to stage 1.
Capture samples adjust matching authored events; they cannot delete uncaptured events or
insert an effect into a different stage. Distinct unique-event IDs remain distinct.

## Runtime changes

- Damage and server projectile spawns retain the authored unique effect ID. Client flight
  confirmations retain the original client instance, skill instance, unique event, position,
  destination, rotation and timestamp. A confirmed event cannot spawn additional flights
  through a repeated report. An early report cannot advance the server's effect deadline.
- Free-aim flights can select the nearest living enemy whose hit capsule crosses the aimed
  segment and is reachable within the catalog lifetime. Straight arrows cannot select an
  off-path target by reporting its ID. A linear shot can miss when its victim leaves the
  original impact position. Homing and straight flights use different targeting rules.
- Projectile splash selects enemies around impact. Self-directed hit/chain effects keep
  the original caster. A projectile spawn runs once even when there is no selected enemy.
- Areas use the main target when authored, retain cast orientation, support cylinders and
  oriented boxes, and allow another hit on the next pulse despite an overlap group. Lifetime
  despawn and later unit removal remain separate and occur once. Sphere effects use a sphere
  distance test rather than the old cylinder approximation.
- NPCs choose the least recently used eligible offensive action, respecting minimum/maximum
  distance and per-skill/shared-group cooldowns. A ranged attack does not inherit the melee
  stopping gate merely because its minimum distance is zero. Melee attacks retain their
  suitable-distance gate. Pure movement/guard effects are not selected as attacks.
- NPC movement stops before casting; the closest usable skill band guides approach. A ranged
  only NPC can retreat out of its minimum-distance dead zone, bounded by its leash. Cooling
  ranged-only NPCs retain their range band. Pacing uses action duration and cooldown rather
  than a universal three-second delay. Selection/retreat are explicit local policies, not a
  recovered official behavior tree.
- Delayed action processing uses each event's deadline and preserves default-stage timing
  instead of restarting the clock at the polling time. Catch-up stage transitions are bounded.

The shared world adapter also contained duplicate begin_map_move declarations/definitions
from concurrent integration. The duplicate was removed, retaining the implementation that
rejects exited sessions and re-arms map completion and StartPlaying.

## Static client verification

The existing IDA database matches input SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6` and native-log build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa`. The existing projectile confirm-or-create handler
and pending actor skill/effect matching predicate were rechecked, annotated without renaming
or byte patches, and saved successfully to
`../dumps/ArcheAgeChronicles.memdump.exe.i64` with stored-section verification. Cached pseudocode,
function extents/fingerprints and append-only annotation records are under
`../dumps/ida_research/combat-rework-20261026/`. Exact actor-field roles remain Candidate.

## Verification and limits

Release `chronicles-server` and `chronicles-tests` build successfully in the reused
`out/build-projectiles` directory. Final focused C++ verification runs 159 cases: 151 pass, eight
optional historical-profile cases skip, and none fail. The additional 16 movement-vitals
checks cover the concurrent environmental-damage integration. The active same-build generated
profile is enabled. This includes twelve NPC/distance scenarios checking actual cast ranges
and repeat cooldowns, client confirmation encoding, free aim, off-path arrows, early/duplicate
reports, unique event identity, impact splash, repeated grouped area pulses, rotated box
targeting, items, statuses, gauges and the existing movement/lunge regressions.

The unchanged TLS combat/potion test executable fails with normal OpenSSL CPU dispatch
while passing with the documented child-process `OPENSSL_ia32cap=0` diagnostic. CDB shows
a corrupt nonvolatile saved store pointer passed into `MemoryStore::load_world_progress`,
consistent with `docs/openssl-local-diagnostic.md`; the precise external routine is not
established. The failing executable and debugger records are preserved. This is a dependency
diagnostic, not a permanent OpenSSL repair or a normal-dispatch acceptance pass.

Python checks pass: 90 combat-data, 14 event-shape, 24 lunge-data and nine reference tests.
The combat-only profile update preserves every non-combat tutorial section. A malformed
single-event initializer in the existing item-effect fixtures was corrected to an explicit
JSON array; their four cases now pass. Scoped whitespace checks pass.

The broader CTest run does **not** pass: 687 registered cases report 12 failed/not-run
entries. Nine auxiliary test executables were not built by the targeted build, distribution
integration lacks its launcher artifacts, and two NativeWorld tests fail
(`BagUnequipWithCarriedWeightAboveDefaultSafeWeightHasNoRefusal` and
`OverworldEscapeTeleportsToTheCandidateAndSavesTheDestination`). Their causes are outside
the verified combat result and are not presented as proven baseline failures. The native
world transport integration itself passes. Private logs and binary/source fingerprints are
under `out/combat-rework-20261026/`.

The already running original client was observed in the tutorial with a staff and a basic
attack animation/Fire Incantation prompt. Its bounded baseline log watcher yielded no complete
frames, so no packet-level acceptance follows from that trial. That service has not been
switched to this candidate. This observation proves no revised combat behavior.
Raw packets, runtime IDs and player state remain private under ignored output directories.

Terrain obstruction, full swept moving-world collision, piercing/multiple flight contacts,
turn-limited homing, ballistic gravity, moving skill objects, all conditional effects, full
boss logic and shared-world/PvP combat remain open. The current flight test selects contact
from the launch snapshot and checks linear impact occupancy; it is not a complete physics
simulation. Original-client trajectories, area visuals and timing need acceptance against
the reviewed running executable before the reported visual bugs can be marked fixed.
