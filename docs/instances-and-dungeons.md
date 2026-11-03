# Instances, dungeons and portals

Instances (caves, checkpoints, points of interest) and dungeons are separate maps placed in the
same world coordinates as the field around them. The client decides locally when the character
walks into a travel volume, checks the map's conditions, asks the server to move it, and waits.
Before 2026-11-01 the shared world answered none of these requests, so instance entrances
played their effect and stayed shut; a reconnect inside a cave put the character on the field
map with no instance content and no objective (live report: the first cave after the tutorial).

## Client contract (static analysis, build 40367e24)

Findings are saved in the IDA database and recorded under
`dumps/ida_research/instances-20261101/`.

| Client path | Request | Evidence |
| --- | --- | --- |
| Entering a `channelSession`, `channel` or `portal` level-trigger volume | `PC2S_LevelTriggerReq` (20084): `trigger_id_x/y`, `pos`, `body_yaw`, `leave` | `AAC_ChannelSessionVolume_Candidate_OnEnterRequestTravel`, `AAC_ChannelVolume_Candidate_OnEnterRequestTravel` build the request; `AAC_MapMove_LevelTriggerReq_BeginPendingAndSend` starts a pending transition first |
| Travel chosen in a dialogue or UI | `PC2S_ChannelTriggerEntryReq` (20082): `map_id`, 0 for the parent map | `AAC_MapMove_Candidate_RequestMapTravel` |
| Both | Local checks of the current map's exit condition and the destination's entrance condition (`MapCondition`) | same functions |

A `channel` volume naming the current map sends the request without a pending transition. Any
other travel request leaves the client waiting until `MapMoveStartNoti` (175) arrives.

## Server behaviour

`tools/build_map_travel.py` writes `data/generated/travel/definitions.json`:

- every Map row: type, world, parent, area, level origin (dungeons), entrance and exit
  conditions;
- every travel volume of every world, including the 15 `MazeDungeons` levels, and every volume
  with `LevelTriggerVolume` rules (cutscene, map move, doodad phase, client dialogue);
- `Portal` rows, `MapMoveCutScene` rows and the flags of the cutscenes they name.

`world/features/travel` answers the requests:

1. The volume must be placed on the character's map and hold its tracked position (1.5 m
   margin); the request point must be within 5 m of the tracked position.
2. The current map's exit conditions and the destination's entrance conditions must hold.
   Quest, item, adventure object, level and play record rows use the quest condition
   evaluator; party state rows use the party service (`NoParty`, `LeaderOnly`,
   `InviteAccepted` = member); `MapEnter`/`MapLeave` compare the maps. CUSTOM: other kinds
   (game time, swimming, camera mode, ...) are trusted because the client checked them.
3. `channelSession`/`channel` volumes move to their map (0 = parent); portal volumes use the
   `Portal` row leaving the current map.
4. CUSTOM arrival: the character keeps its position when the destination's authored area holds
   it. Every field dungeon entrance lies inside its dungeon's area and instances share
   ArcheWorld coordinates; otherwise the dungeon's level origin is used. The client export has
   no spawn-point table for `spawnPositionId` or `Portal.spawnId`.
5. `MapMoveStartNoti.cut_scene_id` carries the matching `MapMoveCutScene` row.
6. A refused travel that the client is waiting for gets a same-map handoff, which releases the
   client's pending transition. Busy, dead or private-encounter characters wait instead: the
   accepted travel (or the owed same-map handoff) is kept and gameplay ticks retry it every
   250 ms until the blocking state ends, for at most two minutes (2026-11-02; before, it was
   never answered and the client stayed blocked at the volume).

Scripted volumes play their server cutscene (`CutScenePlayNoti`, with `VisibleChannelNoti`
for invisible cutscenes, restored at `CutSceneEndReq`).

Gated instances, dungeons and points of interest of the main world resume on reconnect
(`Map.retainOnDisconnect`), with their monsters, NPCs, doodads and shared-world quests.

## Instance and dungeon copies

`world/core/instance_copies` gives every `Instance` and `Dungeon` map (Map.type, from the
travel profile) separate copies. Map.chMaxCount and the client's channel-invite packets show
that the official server runs several channels of these maps; its allocation rules are not in
the export. CUSTOM policy:

- A character in an established party enters its party's copy; a character without a party
  enters a copy of its own. The copy is chosen when the map move starts (or at login, for a
  character that resumes inside the map), before the client loads the map.
- Everyone in one copy shares its monsters (the copy owns its `WorldMobs`), its gathering
  nodes and doodad state, and sees the other players in it. Presence uses a synthetic negative
  map per copy (like housing channels); PvP, party kill credit, loot sharing and companion
  visibility use the copy's keys. Players in other copies of the same map see none of this.
- A same-map handoff keeps the copy. Leaving the map releases it; a copy ends when the last
  character in it leaves or disconnects, so the next entry builds a fresh copy (the official
  reset timer of an emptied instance is not recovered).
- Joining or leaving a party inside a copy keeps the character in that copy until it leaves
  the map. Raid maps keep the event service's matchmaker channels; points of interest and
  fields stay shared.

## Dungeon boss and exit jumps

The dungeons' same-map `Portal` rows `GoBoss` (11) and `GoExit` (12) name spawn ids of a server
table the export lacks. The same dungeon's boss and exit doodads move the character with
authored `MapMove` interactions numbered `<prefix>01` (boss room) and `<prefix>02` (beside the
exit), for example 10110101 and 10110102 in map 10111. `tools/build_map_travel.py` gives a jump
row the destination of the matching interaction when exactly one prefix offers both and the
exit interaction lies nearer to the dungeon's exit portal volume (Candidate). All 24 rows of
the 12 dungeons resolve. A portal volume naming such a row moves the character within the map
(a same-map handoff to the arrival); rows without a recovered arrival stay refused. No volume of
this build names a jump row; the boss and exit doodads remain the client's way to use them.

## Cutscene end points

Every CutScene row is exported with its end point (`fk_EndPointMapId`, `endPointId`). When a
server cutscene played in the shared world (scripted volume, doodad or gate) ends with the
matching `CutSceneEndReq`, the character moves to the end point with a server map move:

- the captured position when one exists (Cliffs entrance 1565 and the tutorial captures);
- else the single doodad whose id equals the end point id placed on the end map (its world,
  layers and authored area), for example the seven Ruins of Gold seats of cutscenes
  100002-100008 (Candidate);
- else the travel arrival rule (CUSTOM): the character keeps its position when the end map's
  area holds it, else the end map's level origin.

No move happens when the end point would leave the character where it stands, when it names a
raid map (raid channels are admitted by the event service) or when no arrival exists; the
cutscene's visibility is then restored as before.

## Entrance doodad phases

Scripted volumes change doodad phases (`LevelTriggerTask ChangeDoodadPhase`, for example on
instances 10104 and 20321 and dungeons 10201, 10302 and 10303). The phase is recorded
in the doodad state of the map or copy, sent to the character (`DoodadPhaseChangedNoti`) and to
the other players there, and carried by later sector snapshots (`SectorUpdateNoti` doodads), so
players who arrive later see the door open. Walking into the volume again changes nothing.

The phase is also saved in the character's `WorldProgress.doodad_phases` (keyed by wire unit
id; `tools/build_map_travel.py` checks that no volume doodad shares a tutorial unit id). When
the character enters that map again (map move or login) its saved phases are applied to the
map or copy wherever no live state exists, so a fresh copy opens the doors the character
already opened. Saved phases never override a phase already set on the map.

## Cliffs of Madness entrance

The P2 official capture shows a separate entrance from ordinary travel volumes: after
**Missing Husband** is complete, the player uses the Pirate Hideout gate and the server plays
cutscene 1565. On its matching end request the server starts a map handoff to 30104. The
destination is the fixed spawn observed in that capture; it is not inferred from the gate's
position, which is outside the destination map's area. The generated travel profile checks the
gate's client placement, interaction, quest condition and cutscene against the verified build.

The local implementation checks the recorded prerequisite, the tracked interaction range and
map entrance conditions before playing the scene. Solo players and party leaders can use it;
the separate channel-invited party-member state has not been recovered and is refused. A
reconnect on map 30104 uses the existing instance resume path. The original client has not yet
been observed entering this gate on the updated emulator.

## Limits

- The copy policy (party or solo, a copy ends when its last character leaves) is CUSTOM; the
  official channel allocation, the channel-invite flow and the reset timer of an emptied
  instance are not recovered.
- Dungeon jump arrivals and doodad end points are Candidate derivations from authored data;
  other end points use the CUSTOM arrival rule. The field portals of map 9041 that jump within
  the map (104010, 504001-504003) have no recovered arrival and stay refused.
- Saved entrance doodad phases are per character; a door another party member opened is shared
  while the copy lives but is restored later only for characters who triggered it.
- No original-client trial of instance entry, copies, jumps or end points has been recorded yet.

## Verification

```powershell
python tools/build_map_travel.py
$env:PYTHONPATH = "tools"; python -m unittest tools.test_build_map_travel
```

C++: `InstanceCopies.*`, `MapTravel.*`, `MapTravelProfile.*` (generated tests need
`CHRONICLES_CLIENT_CATALOG`), `SharedCombatOwner.InstanceCopiesOwnTheirMonstersForEveryoneInside`,
`GatheringFeature.CopyChangesReachOnlyPlayersInTheSameCopy`,
`GatheringFeature.EachCopyOwnsItsNodesAndTheSharedMapKeepsItsOwn`,
`GatheringFeature.VolumeDoodadPhasesAreRecordedOncePerCopyAndRestoresNeverOverride`,
`CompanionWorldTest.ARiderEnteringAnotherCopyTakesItsMountAlong`, the TLS tests
`NativeWorld.AuthoredInstanceMonsterPopulationsArePrivatePerCharacter` and
`NativeWorld.DungeonMonsterPopulationsArePrivatePerSoloCharacter` (need
`CHRONICLES_CLIENT_CATALOG`), and
`NativeWorld.InstanceVolumeEntersTheCaveAndChannelEntryLeavesIt`,
`NativeWorld.SoloCharactersEnterSeparateCopiesOfTheCave` and
`NativeWorld.EntranceDoorPhaseIsSavedAndReturnsWithTheMap` (need
`CHRONICLES_TUTORIAL_TEST_PROFILE` and `CHRONICLES_NPC_PLACEMENTS_9012`). These enter the
smuggling cave 10119 and leave it with `ChannelTriggerEntryReq {map_id: 0}`, show that two
characters without a party do not see each other inside the cave while they do on the field,
and open, save and restore the entrance doodad of instance 10104 across a reconnect.

## Release review in progress

The instance-copy candidate is not ready for deployment. Independent review found that a
party boss kill credits only the character that landed the final hit. A cross-session stun can
also leave the target's already-launched projectile active in its owner session. These need fixes
and multi-player regression coverage before release. Copy allocation now fails before the map
transition and does not fall back to shared state; focused registry tests pass, while transport-level
failure coverage remains open. Local handoff changes preserve shared boss health when the target
leaves, and focused tests cover nearby and distant replacements and the legacy non-AI path; leash,
stun and native multi-player paths still need validation. A vehicle passenger moving another
player's vehicle is a latent invariant risk: the current boarding path only accepts the owner, so
that scenario is not established as reachable.

The current fresh-install schema accepts authored phase zero directly. Older servers
may reject these saved phases. Historical version-37/38 upgrade, archive and rollback
scripts are no longer distributed; the installer is not a downgrade or data-restoration
tool. Existing older databases require a separately reviewed migration procedure.

### Monster reward receipt capacity and offline compaction

Each defeat has an immutable 128-bit event ID and recipient snapshot; processed receipts are
retained so a retry cannot award a kill twice. Pending receipts are read in order in pages of 128.
The server refuses a new batch when any recipient already has 1,024 pending receipts, or when its
world retains 1,000,000 event envelopes. The gameplay path must retain and retry the same defeat
while either limit is reached; it must never report the refused batch as successfully saved.

A saved defeat is staged until encounter finalization makes it ready. Staged events are hidden
from reward consumers and block later rewards for the same character. A ready transition can be
retried with the same event ID after an uncertain acknowledgement. On restart, the startup owner
must first establish exclusive authority, advance the database owner epoch, recover every staged
event in bounded pages and only then admit players or start reward consumers. This recovery treats
an accepted lethal action as a defeat after the old process-local encounters have been reset, even
if the prior server stopped before its death animation was seen. The database epoch fences stale
outbox transactions; safe server takeover also requires preventing a lease-losing process from
publishing old in-memory world state. Do not deploy until that lifecycle is independently verified.

Use `tools/reward_source_monitor.py` with a dedicated read-only database role to
inspect retained events, capacity, pending work and backlog. Its aggregate queries
are built into the tool and require no extra installation SQL. See
[reward-source-monitor.md](reward-source-monitor.md) for role setup and thresholds.
The reported actual count must equal the recorded event count. A discrepancy is a storage fault,
not a reason to reset the counter without investigation.

No automatic tombstone expiry is safe while a live actor or another durable source can replay an
event ID. Only after stopping every server and other retry source, confirming there is no external
retry queue, and verifying a fresh restorable database backup may an operator compact processed
history. The old compaction SQL is not distributed with the fresh-install schema.
Any maintenance must use a separately reviewed procedure that preserves pending
work and deduplication guarantees. Reinstalling the schema does not compact history.
