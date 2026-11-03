# Civilian routes and service stops

The map-wide NPC exporter previously rejected every placement with a patrol path.
As a result, workers, strolling villagers and guards were missing while stationary
town NPCs were present. `tools/build_stationary_npc_placements.py` now joins ordinary
`npcData.pathId` records to the same layer's hash-checked `level_path.json`.

Map 9012 exports 261 active server actors: the existing 145 stationary placements
and 116 authored civilian routes. The 609 raw client-static records remain excluded;
the duplicate-static-NPC repair is preserved. Four random-direction routes and one
route requiring an unresolved SlowWalk speed remain explicitly excluded. Eight
source layers are still absent from the export.

## Runtime contract

`NpcPatrolRoute` compiles a closed forward polyline into timed movement legs and
authored pauses. Walk and Run speeds come from `Npc -> UnitModel -> UnitGroupModel`
and are converted from centimeters per second to millimeters per second. The
NearestPoint startup chooses the nearest authored node. The client receives neutral
MoveSpeedRate (stat 84, value 1000), matching the model-speed trajectory and the
existing CharacterStats/NpcAi per-mille convention. Speed inheritance between node
options, stayRotation as world yaw and the startup choice are local Candidate
policies, rather than a recovered official scheduling algorithm. Route positions include authored Z;
terrain projection, dynamic collision and navmesh detours are not implemented.

The population shares one monotonic epoch for the lifetime of the listener.
Every connection samples that timeline, including its UnitAddNoti snapshot;
late joins and reconnects do not restart a worker at the beginning of its route.
Sampling is deterministic and immutable. No client report controls these actors.

After StartPlaying, the existing session gameplay timer sends MoveNoti 177 with
Walk/Run, prediction position, a bounded prediction interval and nanosecond wire
ticks. Stops omit prediction. Each actor has its own notification deadline, normally
500 ms and at route boundaries with a 16 ms minimum timer interval. Another actor's
short leg does not force every NPC to send another frame. Delayed timers sample the
present phase instead of replaying an unbounded queue of old activities.

Five inline authored pauses are preserved. Three inline activities resolve through
the exact model/stance/motion and GameAnimKeyMapping joins and use StartX5MotionNoti
69. Arrival sends Stop before the activity; leaving restores the walking stance.
An activity is emitted once per phase, and late arrivals use its phase-start tick.
Unresolved animation keys never select an unrelated model's animation.

The server updates the announced-unit registry from the same route pose before
interaction/shop validation, including requests arriving between movement frames.
Map changes discard the patrol session and its deadlines, and a return to the
populated map creates a fresh delivery session on the existing shared timeline.
The existing socket executor/worker-strand separation and bounded outbound queues
remain in use. This is a bounded map population, not a measured MMORPG load result.

## Evidence and remaining behavior

The static inputs belong to demo build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa`, executable dump SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`.
The exporter reads all referenced tables and paths through the existing verified
Source reader and records their hashes in the generated file.

Official decoded captures establish route-NPC announcements and StartX5Motion
activities. They do not establish this server's 500 ms cadence, official route
scheduling or local-client acceptance of the new movement. MoveNoti's usable
prediction/tick shape is supported by the existing client static recovery in
`docs/npc-locomotion-fix-20261026.md` and its cached IDA records; no additional
game function was analyzed or renamed for this increment.

There are 188 unresolved activity references/keys in the supported routes. In
particular, referenced motion-point GUIDs have no corresponding definition in the
available raw export. They remain recorded in `patrol.unresolved_activities`, and
the runtime does not fabricate tasks, destinations, durations, items or rewards.
Random routes, SlowWalk calibration, day/night spawn windows, scripted work-point
selection/contention, dialogue-driven movement interruption, dynamic obstacle
avoidance and public monster combat/loot remain incomplete. Static client-managed
actors remain outside the server's ordinary civilian population.

## Validation and rollout

Focused unit, real-export and native socket results are recorded under ignored
`out/npc-patrol-20261026/`. C++ tests cover route continuity, stop/activity order,
wire encoding, shared clocks, late joins, duplicate starts, current registry
positions, ownership, bounded delayed timers and notification cadence. Python
tests cover model speeds, route modes, inline activities and unresolved references.

The final isolated C++20 Release server and main test executable build successfully.
The selected CTest run passes all 50 cases with zero failures or skips: 41 module
checks and nine native TLS/MemoryStore checks. The new socket case observes movement
without further player input, verifies the same route clock for a later connection,
and checks repeated StartPlaying does not reannounce the NPC. Existing real-map
entry, reconnect, map ownership, tutorial isolation and housing round-trip cases
also pass. Python checks pass all 60 cases: 30 placement, eight patrol-data,
11 tutorial-spawn and 11 reference-generator tests.

The TLS trial uses the existing child-process `OPENSSL_ia32cap=0` diagnostic; this
does not establish a fix for the separately documented normal-dispatch OpenSSL
failure. These are synthetic socket checks, not an original-client observation.
The selected checks are not a full-suite pass or an independent acceptance review.

The integrated candidate is built from a separate source snapshot and output
directory using the existing pinned dependency cache. The snapshot excludes two
newer unrelated GM/emote tests requiring APIs outside its frozen runtime; the
workspace tests remain intact, and neither case is in the selected patrol checks.
The exact exclusions and source fingerprints accompany the ignored artifacts. The running server is left
untouched at the user's request; applying the feature requires the updated binary
and regenerated placement file at the next reviewed restart. No corrected
original-client trial, PostgreSQL migration or live deployment is claimed.

The local protocol reference is regenerated. Its strict check still rejects
pending reviews in shared integration files and unrelated feature sources; their
review fingerprints were preserved. The final Page synchronization is unavailable
because the Page service returned `realtime_token_expired`. The last confirmed
publication fingerprint is unchanged by this patrol task, and the Page's earlier
patrol paragraph still describes native socket validation as pending.

The subsequent local-client trial exposed stepping without walking/turning.
See `docs/npc-patrol-animation-20261026.md`: the speed-stat-only deployment failed
visually, and the final deployed correction also fixes NPC identity type nibbles.
All 49 selected checks pass; the final original-client visual trial was stopped
by the user before acceptance. Earlier synthetic checks are not visual proof.

The user subsequently confirmed the final typed-ID server's NPC movement now
works (2026-10-26). This is human-reported original-client acceptance of the
walking/turning repair, not a new claim about the unresolved specialized routines.
