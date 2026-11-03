# Authored zone boundaries and movement restrictions

The 2026-10-26 local-client investigation found a decoded `PC2S_ZoneChannelMoveReq`
(20090) with no response outside housing. The client's `X5MapMoveManager` retained
its pending transition and directional boundary restriction. Its movement component's
maximum walking speed fell from 400 to approximately 137 cm/s while the neutral speed
stat remained unchanged. The pending destination matched the authored eastern checkpoint
volume for map 10110, which has no entrance or exit condition. No achievement or quest
completion was required to explain this incident.

Static findings, cached pseudocode, fingerprints and save verification are under
`dumps/ida_research/world-blocking-20261026/` in the parent workspace. The matching
`ArcheAgeChronicles.memdump.exe.i64` was saved successfully with stored-section
verification. Live inspection was read-only; no client executable bytes were patched.

## Runtime contract

Generate the immutable boundary export before starting the server:

```powershell
python tools/build_level_trigger_volumes.py --source data/generated `
    --output-dir data/generated/triggers
python tools/build_zone_channels.py --source data/generated `
    --output-dir data/generated/triggers
```

When the catalog is configured, startup loads
`triggers/ArcheWorld-level-triggers.json` beside the catalog. Missing definitions disable
this route. Malformed or oversized definitions fail startup. The export is generated
through the hash-checked raw/catalog source; the expected current SHA-256 is
`82efee8b9759118e106aabf9c1684986355dd3797b318c24f5893f05a099acda`.

Outside housing and tutorial instances, a zone request first resolves active authored
`channelSession` boxes with spawn-position ID zero. The source and target must be eligible
ArcheWorld catalog maps with no entrance or exit condition. Both the authoritative
position and reported point must lie in the oriented box, allowing a local one-meter
overlap tolerance, and the reported point must be within five meters of the server point.
Different overlapping target maps are ambiguous and refused, including gated or unknown
targets. There is no invented priority based on box size. Since 2026-11-02 the server uses the
reported point and its own yaw (see "Seamless nested zones" below); before, it kept its own
coordinates and changed only the selected map identity.

The optional sibling spatial export maps occupied 5100-cm authored grid cells to unique,
ungated public Field maps. It currently covers 11 fields and 15,976 non-overlapping cells.
When no eligible trigger matches, that grid resolves regional crossings without trigger
boxes. It retains the five-metre reported-position bound and authoritative coordinates.
Malformed, overlapping or oversized grids fail loading. Static spatial-zone callers and
catalog masks support this Candidate policy; no official request capture proves its full
selection rules. See [gameplay-repairs-20261029.md](gameplay-repairs-20261029.md).

Accepted travel uses the existing same-connection lifecycle: MapMoveStartNoti 175,
MapLoadCompleteReq 20085, MapMoveCompleteNoti 176, StartPlayingReq 20086 and StartPlayingRes
183. Old presence and announced NPCs are removed; only the configured destination's
population is admitted. Reconnect accepts eligible catalog channels without pretending
that an unpopulated map has the field map's NPCs. Unit lookups follow the actual handshake
map independently of population availability.

Natural channel travel retains the existing player combat object, including HP, stamina,
statuses and cooldowns, and publishes its current stats in the arrival snapshot. Dead or
busy characters and sessions with GM-spawned map actors do not travel immediately. Since
2026-11-02 the request is kept (the latest one per session) and gameplay ticks retry it every
250 ms; it starts once the action has ended, death has been resolved or the GM actors are gone,
and is dropped after two minutes or when another map move starts. Housing keeps its existing
recorded-origin route. No quest, achievement, item, reward or character progression is granted
by a crossing.

## Regional movement and vehicles (local candidate, 2026-11-01)

A natural region request retains the server-side glide and movement-vitals state through the
map-load handshake. Fall and swim tracking move to the destination map without inventing a
landing; time spent waiting for map loading is not charged as underwater movement.

During the transition the ridden mount or wagon is hidden from origin observers and kept in
the companion world. After StartPlaying the destination receives the same unit ID, its
passenger snapshot and seating notification. An active trade run reuses that wagon and sends
its existing goods rather than spawning another. A driven trade wagon is checkpointed to the
destination before the transition; a definite checkpoint failure falls back to the current
map. Reconnect retains the wagon's recorded map instead of relabeling old coordinates.

For a driven trade wagon, the destination character position and revisioned wagon progress
are committed together. A rejected checkpoint stays on the current map. An uncertain database
outcome closes the connection without a later location checkpoint, so reconnect reads the
committed result. A process-local character admission lease serializes close settlement with
new entry; a busy connection leaves its ticket available for retry. Entry advances the durable
progress revision before loading, fencing older delayed checkpoint transactions. A failed
close settlement is retained in memory for reconciliation before a subsequent entry. If the
process stops during a database outage, that in-memory snapshot is lost; no crash-durable spool
is provided. The ownership and pending-recovery policy assumes a single world-server process,
and externally written character positions do not carry a version. Multi-passenger behavior
across regions remains unverified. The original client has not been observed maintaining flight
or seating through this candidate's map load, so uninterrupted visual gameplay is not yet proven.

Focused owner, companion and trade tests cover movement state, underwater load delay, actor
visibility, wagon identity, goods, checkpoint rejection and a simulated interrupted checkpoint.
Native TLS tests cross an exported region while gliding, riding a horse and driving a loaded
trade wagon. They verify server state and packets, not original-client rendering.

## Seamless nested zones (2026-11-02)

A live player saw loading screens at some region borders, also while gliding. The logs showed
`ZoneChannelMoveReq` answered with a same-map handoff ("no eligible boundary"). Static analysis
(`dumps/ida_research/seamless-region-20261102/`, saved IDB) shows why that loads:

- `PS2C_MapMoveStartNoti` to a map with the same root world is transition mode 2
  (`AAC_MapLoading_ClassifyTargetMap`). Mode 2 shows the loading UI only when
  `AAC_MapMove_Candidate_IsSeamlessArrivalForPendingTransition` fails (or a forced-loading flag
  is set). Different root worlds are mode 1, a full level load.
- That check needs the notified map to equal the client's pending target. For a spatial zone
  request (pending kind 4) nothing else is checked. For a parent return (kind 0) the spawn point
  must lie within 50 cm of the pawn (a squared distance of 2500 cm²); with a trigger volume the
  spawn must be inside the volume and the pawn inside its box.
- The client resolves its position to the deepest authored zone. Villages, points of interest
  and the housing area are zones nested in their field (for example village 9013 in field 9012).
  Entering one, it asks for the nested map; leaving, it asks for the current map's parent.

The server exported only top-level ungated fields, so every nested zone resolved to "no eligible
boundary" and the same-map answer failed the target check. Now:

- `tools/build_zone_channels.py` (format 2) exports every zone with exactly one ArcheWorld field,
  point of interest or housing area, with its parent, nesting depth and whether it has entrance or
  exit conditions. 33 grids; equal-depth grids never overlap. Instance zones are not exported and
  keep the travel planner's entry rules.
- `MapChannels` gives the deepest cell precedence. A character on a nested map whose position
  resolves to any other zone travels to that map's parent first, as the client asks.
- The destination uses the request's reported point (still bounded to five metres of the tracked
  position) with the tracked yaw, so a parent return passes the 50 cm spawn check.
- Crossings into or out of a map with conditions use `travel::Catalog::travel_allowed` with the
  quest-condition evaluator; without the travel profile they stay refused.
- Walking into the housing area's cells enters its channel where the character stands (the
  character's own channel there, else the first), without a recorded return point; walking out
  steps onto map 9012 at the reported point. A rider is refused (same-map handoff).

Still a loading screen: crossings between different root worlds, instance zones reached by
walking, and refused or ambiguous requests (the same-map recovery). A gliding character arrives at
the point it asked from, so a fast glide may be pulled back by the distance it covered while the
answer was in flight (about one round trip); this is unobserved in the original client.

## Limits and verification

Gated instances, dungeons, portals and level-trigger travel are answered by the map travel
planner since 2026-11-01; see [instances-and-dungeons.md](instances-and-dungeons.md). This page
covers only the seamless `ZoneChannelMoveReq` route.

This is a bounded local policy informed by static recovery and the reproduced pending
handoff. The seven canonical official captures contain no opcode 20090. Their other map
changes establish the same-connection packet lifecycle, not complete zone-selection rules.
Full regional `channel` semantics, spheres, nonzero spawn positions, conditional/gated maps,
interaction-driven travel and general multi-channel allocation remain unresolved. A refused
idle request now receives the normal lifecycle back to its current map, so the client can
clear its pending transition. Busy/dead/private-GM sessions have no reconstructed
failure/reset reply; their request is deferred instead (see above), so the client keeps its
pending restriction only until the blocking state ends, at most two minutes. Collision/navigation
and server movement authority remain separate unfinished systems. New channels have only the
services/data actually configured for them; this change does not populate their NPCs.

On 2026-10-28 a live client blocked at a map 9021 crossing held exactly that retained pending
state: the map-move manager's pending marker stayed set because the deployed server route did
not resolve that regional crossing. Clearing the pending request bytes in the running client released
movement immediately, which the operator confirmed. The in-process local unlock now clears those
bytes after they stay set for 20 seconds, so refused crossings recover on the client; accepted
handoffs keep their normal lifecycle. That recovery does not add a server travel route or
simulate a failure reply.

Focused checks include rotated geometry, distance/coordinate validation, ambiguity with
gated targets, authored return boundaries and missing exports. The native TLS/MemoryStore
regression crosses the real exported checkpoint after spending stamina, verifies the
arrival retains that stamina, finishes StartPlaying, saves the map and reconnects without
foreign NPCs. Build, test, deployment and original-client results are recorded separately
in the movement feature's verification entry.

Additional TLS checks exercise a spatial-only crossing to map 9021 and an unresolved
same-map recovery, including StartPlaying and reconnect. These checks do not establish
original-client UI recovery. Static inactive-zone flags remain a separate launcher patch;
this packet repair does not claim that every authored region is playable.
