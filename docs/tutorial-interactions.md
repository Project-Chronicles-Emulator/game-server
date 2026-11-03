# Tutorial interactions

Door/lever links, animation timing, and the second tutorial's notes and quest
object are described in [object-interactions.md](object-interactions.md).

The first runtime increment implements the floor pickups and quest interaction
path on map 8000. Definitions are generated from the extracted client tables
and map placements, rather than from account-specific official packet payloads.

`tools/build_tutorial_data.py` writes
`data/generated/tutorial/definitions.json`. It joins adventure objects to
expertise rewards, loot conditions, and loot items; quests to objectives and
trigger volumes; and NPC placements to spawner selectors and NPC templates.
The generated profile has ten floor pickup definitions, two animated quest
doodads, seven quest definitions, and twelve NPC spawners. Eight pickup
identifiers and all twelve captured NPC types matched the official trace.
The uncollected floor objects are supported from static data and have no live
pickup observation in this trace.

The captured kill objectives resolve to these catalog entries:

| NPC CMS ID | Catalog name | Quest and objective |
| ---: | --- | --- |
| 5001015 | Bound Prowler | 10000106 / 1 |
| 5001529 | Recalling Grounds Warden | 10000043 / 1 |
| 5001530 | Recalling Grounds Scout | 10000043 / 2 |
| 5001528 | Recalling Grounds Herald | 10000043 / 3 |
| 5001527 | Herald Knight | 10000045 / 1 |

The remaining captured enemies are Skeleton Scout (5001010), Skeleton Warder
(5001016), Skeleton Spearman (5001082), Rotting Skeleton Spearman (5001083),
Rotting Skeleton Infantryman (5001084), and Rotting Skeleton Heavy Infantryman
(5001085). The mapping verifier checks their captured spawner GUIDs against the
extracted placements, not just their NPC CMS IDs.

The placement increment also verifies positions, patrol corridors, factions,
stances, and activation flags for both tutorial areas. See
[tutorial-spawns.md](tutorial-spawns.md) for the complete fifteen-placement index,
runtime spawn behavior, and scripted-activation limits.

## Current behavior

- Discovering and activating a nearby floor object updates its character-owned
  state. Potions, materials, and gold use the catalog's fixed reward quantities.
  The treasure chest (20080008) is opened with its animation instead and grants
  its necklace as an item instance; see [object-interactions.md](object-interactions.md).
- Repeating an activation returns the object's current state without granting
  its reward again. Reconnecting restores inventory, gold, and consumed objects.
- Utility slots validate item type and available quantity, including combined
  allocations across slots. Slot assignment does not create inventory.
- The first potion (40101101) uses its catalog skill and extracted action
  timing: skill 1000 starts the drink, and the server resolves effect 100000141
  at the animation's 435 ms consumption event. A client effect report is optional
  and cannot advance that deadline. Inventory and utility counts commit
  together before notification. Duplicate effects cannot consume
  twice; unknown items, mismatched instances, non-self targets, early effects,
  expired action reports, and overlapping drinks are rejected. One socket timer
  schedules the next action deadline; persistence and requests run on the same
  per-session worker strand. Action completion uses the character's latest
  position. This increment requires the full 2,881 ms action before another drink; movement cancellation
  and health recovery need the later authoritative health/combat simulation.
  This path is now only the fallback for a profile without item skills: with the
  current generated profile the combat session owns every item skill, including the
  potion's health recovery ([item-use.md](item-use.md)).
- Trigger-based quest acceptance and progress validate the expected GUID,
  position, objective order, and completion groups. Completing a quest can
  accept its next sequential quest. Kill objectives cannot be claimed through
  `ProgressQuestReq` or by claiming a reward early.
- The key-consuming object (CMS 49013) and lever (49020) support start, execute,
  and end animation messages. Item consumption, phase changes, and objective
  progress commit together. Their quest and item preconditions remain required.
- The Shard of Recollection NPC has a local unit ID and supports dialogue and
  its catalog-defined quest selection. Official runtime NPC IDs are not reused.
- With the tutorial profile loaded, map 8000 is private to each character.
  The earlier generic bootstrap presence trial remains available without a
  profile; it is not the tutorial instance model.

The initial trigger chain can advance from quest 10000040 into 10000042.
Subsequent increments add quest kill receipts, NPC placement, and initial
server-timed combat; see [quests.md](quests.md), [tutorial-spawns.md](tutorial-spawns.md),
and [tutorial-combat.md](tutorial-combat.md). Their synthetic checks do not
establish original-client combat or the complete tutorial route. Potion health
recovery is implemented in synthetic tests only ([item-use.md](item-use.md)); complete
combat formulas, and travel to the second instance and shared map still need their own
implementation or live verification.

## Persistence and execution

Migration version 5 adds indexed per-character tables for stack quantities,
adventure objects, quests, doodad phases, and utility slots. Quest objectives
are a bounded JSONB object keyed by sequence. Gold and a gameplay revision
belong to the character row. One transaction compares the revision and writes
only changed rows, so concurrent stale sessions cannot both award an object.
Success packets are queued after the commit succeeds. A revision conflict
closes the stale world connection so reconnect can load a consistent snapshot.

Static definitions are parsed and indexed once at server startup. A world
session loads durable state once, processes its requests serially, and keeps
bounded state in memory. Database actions run on the existing worker pool,
outside the socket event loop and shared codec lock. Invalid actions have a
per-session rate limit, and the existing outbound byte/frame limits still
apply. There is no per-frame catalog scan or file read.

Interaction range uses the session's current world position. Quest trigger
requests must also agree with that position. The underlying movement handler
still lacks authoritative collision and speed simulation; these checks do
not establish a complete movement authority model or production throughput.
The current PostgreSQL adapter opens connections for storage operations;
a measured load test and a bounded connection pool remain necessary before
scaling this service to a public MMORPG workload.

## Running and checking

The regular client export builds this profile when the raw tutorial map is
present. `start_persistent.ps1` also rebuilds it before starting the server.
When `--world-bootstrap-diagnostic true` and `--client-data-dir` are supplied,
the server loads the sibling `tutorial/definitions.json` profile.

```powershell
python tools/build_tutorial_data.py
python tools/test_build_tutorial_data.py
python tools/verify_tutorial_mapping.py `
  out/captures/official-full-session-20261024-01/decoded-complete.jsonl `
  --output out/captures/official-full-session-20261024-01/tutorial-mapping.json
```

The private mapping report records CMS IDs and catalog names, with no player
names, session credentials, or copied official item instance IDs. Runtime
tests cover range checks, duplicate pickup after reconnect, utility allocation,
quest ordering, atomic key use, persistence failure, ownership, and concurrent
revision conflicts. The PostgreSQL test runs only against the isolated cluster
created by `tools/test_postgres.py`. Passing those checks establishes synthetic
behavior. In the first original-client trial, the user confirmed the first
pickup and a five-potion display. Later pickups raised the PostgreSQL potion
quantity to seven. The server also acknowledged quest acceptance. Potion use
remained at five in the screenshot because skill requests were ignored by that
build. The consumption increment passed twelve focused behavior/TLS tests,
twelve NativeWorld CTests, and four extraction tests; its original-client trial
failed: the server acknowledged drinks, but the local client sent no effect
report, so the first consumption handler never committed a count change.
The corrected handler resolves the event on the server timer and has a TLS
regression that sends no effect report. The broader quest chain remains
unverified in the original client.

## NPC pursuit MoveNotification (177) contract

`CombatSession::announce_npc_move` (`src/world/features/combat/combat.cpp`) is the only emitter of `PS2C_MoveNoti`
(177) for a pursuing tutorial monster. It reads the server's authoritative route
(`NpcAi::route_position_at`, `src/world/features/npc/npc_ai.cpp`) and adds no motion, speed, leash or perception.

| Field | Walk | Stop |
| --- | --- | --- |
| `unit_id` | the monster's exact unit id | same |
| `tick` | existing wire nanosecond clock, strictly increasing per unit | same |
| `mode` | `MoveMode_Normal` (enum 0, omitted when decoded) | same |
| `detail` | `MoveDetail_Walk` | `MoveDetail_Stop` (enum 0, omitted when decoded) |
| `pos` | current authoritative position (zero coordinates omitted) | final position |
| `pred_pos` | route position `pred_milli` ahead (x/y/z) | absent: a Stop carries no goal |
| `pred_milli` | 600 mm / route speed, clamped to 16..900 ms | absent |
| `velocity` | never set (the client ignores it; `pred_pos` is the goal) | never set |

Cadence (**Candidate, not official**): the route start is announced at once when no Walk stream
runs; while walking, a Walk is sent when the 3-D displacement since the previous Walk is at
least 600 mm or 900 ms have elapsed; a re-plan while walking follows the same cadence. The values
come from the decoded client consumer (builder-2 addenda 25-28): mode 0 snaps within 50 cm of the
client object, the blend window is max(interval, 1 s), speed is |dpos| / dtick, the first update
only places the pawn, and a moving update needs `pred_milli` >= 16 with a goal. No official NPC
MoveNoti capture exists.

Halt contract:

- **Tested (synthetic Combat tests):** arrival (one Stop at the route end); the player leaves
  the leash (Stop at home); the monster's own attack (Stop before it, no Walk after); the player
  dies at the monster's first attack (the Stop precedes the attack, the return home starts with
  a fresh Walk at the Stop position and ends with a Stop at home); a reconnect announces the
  route start again.
- **Tested (native socket):** the real client socket receives the Walk/Stop shape in order.
- **UNTESTED:** the monster is killed mid-route. By reading the code a single Stop follows from
  `complete_motion`, but no test proves it (a fixture for it is still unfinished). A player
  death while the monster is still mid-route is also untested, as are despawn/removal and other
  halt reasons.
- `pred_milli` models the route speed (N2); a re-route waits for the next cadence frame, so the
  client may follow the previous goal for up to 900 ms (N3).

Evidence level: synthetic Combat tests, a native-socket test and a private PostgreSQL run (7/7 on
port 55467, cleanup proven; `NewsStoreIntegration.PostgresPersistsAcrossInstances` was not in
that set). **Original-client visible following of NPC MoveNoti has NOT been observed.**

The corrected server-timed potion path passed the combined 60-case gameplay
run (58 passed, two opt-in checks skipped) using the explicit process-level
portable OpenSSL configuration. The database and private capture checks passed
separately. See [openssl-local-diagnostic.md](openssl-local-diagnostic.md) for
the reproduced normal-dispatch failure and the bounded local workaround.
