# Shared-world gathering

This page describes the emulator's gathering service: herbs, trees, ore veins,
shellfish and harvestable doodads (Roseberry Bush, Dark Vein, Wild Bush) that a
player harvests with an interaction animation. The definitions and timings apply
to client build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa` (executable dump SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`).

**Status: partial.** Shared-world sessions on the demo ArcheWorld maps (9011,
9012, 10107, 10109, 10117, 10118, 10119 and 30104) route every gathering request
to `src/world/features/gathering/gathering.*`. Private tutorial sessions never route to gathering, because
the tutorial session owns its cached progress revision. Synthetic unit and
real-socket tests pass; the corrected flow still needs an original-client trial
(see [Live trial history](#live-trial-history)).

## Two gathering paths

The client uses two different request shapes, both on
`PC2S_StartInteractionAnimReq` (20069):

| Path | Request | Target | Examples |
| --- | --- | --- | --- |
| Placement | `param.gathering.indices = [index]` | `sector_id << 32` | herbs, trees, ore, clams from `ArcheWorld/gathering.json` |
| Doodad | `param.interaction.interaction_id` (20001, 20002, 20003) | spot unit `sector << 32 \| local_id << 12 \| 6` | Roseberry Bush 74, Dark Vein 90, Wild Bush 58000 |

A Roseberry Bush exists in both forms: gathering placement 80 and doodad 74. The
doodad form was completely unhandled before 2026-10-27, so pressing F at one did
nothing at all.

## Evidence

All statements below come from the hash-checked client export and the sanitized
official captures described in `docs/capture-corpus.md` (T/P1/P2 and the
2026-10-26 additional-activities capture A1). No player, character, item-instance
or session values are copied into data or documents.

| Finding | Evidence |
| --- | --- |
| A placement start carries one index and `target_unit_id = sector_id << 32`; `sector_id = x \| (y << 14)`. | Every captured placement start (animations 1006, 1007, 1009, 1010, 1031) resolved to an exported placement. |
| `gathering.json` stores centimetres relative to the sector: world = stored + (sector index − 1) × 5,100 cm. | Requests were 0.4–1.9 m from the node resolved this way. |
| The reward pack is `ExpertiseReward.id == Gathering.id`; `Gathering.lootpackId` is the placeholder pack 1. | Gathering 80 (1006) → pack 3060101 → 5–7 × item 70102001. |
| Thirteen gathering IDs have no `ExpertiseReward` row. The `1306xxxx` family mirrors the authored rewards one-to-one. | 13060101 is gathering 80's pack, 13061101/13061102 the tree packs. Captures confirm Young Tree 102 → Log 1–2, Clam 300 → Clam 5, Sea Mushroom Coral 301 → Sea Mushroom, Suspicious Rock 302 → Crab. |
| `StartInteractionAnimNoti` echoes the request's `param`, `pos`, `yaw` and `tick` (client nanoseconds), names the requester's **wire unit ID**, carries an empty `packet_seq` and no `target_unit_id`. | Every official own start (placements and doodads). |
| An SP flow `StatSyncNoti` precedes `StartInteractionAnimNoti`: stat 72, source `SCS_InteractionFlow`, rate −cost ÷ execute seconds, `end_tick` = start + execute time, regen range ending at start + animation duration + 300 ms (`SPStatRegenDelayMilli`). | 50 / 1.646 s = 30.377, 50 / 6.508 s = 7.683, 50 / 0.641 s = 78.003 SP/s; regen offsets 2,906 ms (1006) and 7,308 ms (1009). |
| Execution occurs at the montage's `interaction_execute` notify after the loop section repeats `loopCount − 1` extra times. | Execute requests followed 1,646 / 4,989 / 6,508 / 641 / 4,951 ms after the start. |
| Placement success: `ItemsUpdateNoti` (new stack totals, `system`), `ChangeGatheringStateNoti` (deactivated index), `InteractionAnimRes`; the end request receives another `InteractionAnimRes`. | All successful own placement gathers. |
| A tool gather lowers the tool by exactly 100 (`ToolDurabiliyDecreaseValue`). The tool row rides in the same `ItemsUpdateNoti`; `ChangeStatNoti` (`SCS_Equip`, vitals 68/69/70/71/72/73/80) and `EquipmentsUpdateNoti` follow before the state change. | Hatchet 1900 → 1800 → 1700, sickle 1260 → 1160 ... in P1 and A1. |
| Doodad success: `ItemsUpdateNoti` (`looting` naming the doodad for `LootingPack` tasks, `system` for `ExpertiseReward` tasks), `ChangeDoodadPhaseNoti` (`phase_id` 0), `InteractionAnimRes`. No tool row changed. | P1/P2 Roseberry Bush 74 and Dark Vein 90 gathers; all 12 targets resolve to authored spot doodads within 0.4–1.4 m. |
| A despawned doodad respawns after `Doodad.respawnDelayMin..Max`; the respawn `ChangeDoodadPhaseNoti` omits `phase_id`. | Doodad 74 (60 s authored) respawned 60.1–60.7 s later. |
| Initial state arrives through `SectorUpdateNoti`: `gathering_deactivated_indices`, and `doodads` entries `{id, phase_begin_tick}` (despawned) or `{id, phase, phase_begin_tick}`. | 986 P2 sector updates; default-phase doodads are never listed. |
| Placement respawn after deactivate delay (1 s) + uniform 30–180 s. | 116 captured deactivate→activate pairs took 31.8–180.5 s. |
| Other players receive the gatherer's `StartInteractionAnimNoti` and both `InteractionAnimRes`. | Other units' 166/167 interleave with own gathers in P1. |
| No character EXP and no expertise update follow a gather. | No `ChangeCharacterBaseNoti` or `CharExpertiseSkillsUpdateNoti` within 200 ms of any of the 35 gathering executes in P1, P2 and A1; P2's entry expertise snapshot is still Lv1 after many P1 gathers. |

## Building the profile

```powershell
cd 'D:\PROJETOS\Archeage Emulator\Emulator'
python tools/build_gathering_data.py
python -m unittest discover -s tools -p 'test_build_gathering_data.py'
```

The builder reads `data/generated` (see `docs/client-data.md`) and verifies
catalog and raw hashes through the existing export manifests. It writes the
ignored `data/generated/gathering/definitions.json` (**format version 2**). The
current export yields 15,322 placements on the demo ArcheWorld maps, 26
supported gathering definitions (13 authored, 4 capture-confirmed and 9
catalog-name Candidate joins, none unsupported), three gathering doodads (74, 90
and 58000) with 289 placements, and six gathering animations. The server loads
the profile at startup when `--client-data-dir` and the world bootstrap diagnostic
are enabled and the profile exists next to the catalog. A format-1 profile is
rejected at startup; rebuild it before deploying this server.

`INFERRED_REWARDS` in the builder lists every unauthored join and its evidence:
`capture` for 102, 300, 301 and 302; `catalog_name` for 209/210 (Mature Tree as
200–208), 50/51 (Wildfiber → Wild Fiber), 71–74 (flowers → Wild Petal, as Lily 70)
and 15 (Rock → Marble, as doodad Rock 91). Each definition records its
`reward_source`.

Doodads are indexed from every ArcheWorld `spot_object.json` layer inside the
hosted map areas. A gathering doodad needs an `Interaction` whose animation has a
harvesting expertise (Gathering, Mining, Logging) and a `LootingPack` or
`ExpertiseReward` task. Its whole `DoodadPhase`/`DoodadPhaseFlow` graph must be
supported: interaction triggers with one interaction, timer triggers with no
task, weighted next-phase ratios, and phase 0 (despawn) only with an authored
respawn delay. Anything else is listed in `unsupported_doodads` instead of being
guessed.

The builder also fails closed on finite or initially inactive nodes, invalid
delays, loot filters, currency or graded loot, equipment reward items, execute
events inside a loop section, and timing that differs between character rigs.

## Runtime flow

`src/world/features/gathering/gathering.*` owns the rules; `src/world/session/native_world.cpp` routes requests,
supplies the player's vitals and delivers notifications.

1. **Start.** The session refuses a start while another gathering, skill, statue
   or crafting action is open (`InteractionAnim_UsingSkill`) or while the player
   is dead. It resolves the target (placement index and sector, or doodad spot
   and its current phase), checks the current map's area, the requested
   animation, the request position (within 5 m of the server position) and the
   node distance (7 m). Placements must be active and tool placements need an
   undamaged matching tool in a tool slot; doodads must be in a phase whose
   interaction is the requested one. The SP cost is checked against the live
   combat vitals; the reply is the SP flow `StatSyncNoti` followed by
   `StartInteractionAnimNoti`, and nearby players receive the notification.
2. **Execute.** Accepted after the server-side execute time minus a 100 ms
   polling allowance, while still in range and before the 30 s expiry. The
   session rolls the loot, wears the tool, claims the node in the shared
   registry, then commits the stack increase and tool durability in one
   `commit_world_item_trade` transaction under the next progress revision. A
   failed commit releases the node, stops the SP flow and grants nothing. After
   the commit the announced SP is deducted once, silently. A retried execute
   returns the result again without a second reward. Since 2026-10-30 the
   shared item-trade validator accepts tool wear (any non-negative durability on a
   distinct row), so equipped-tool gathering commits through the real repository.
   The same flow butchers the killer's corpse with a knife (`param.handling_loot`,
   animation 1000) on any map; see [monster-loot.md](monster-loot.md).
3. **End.** Ending before execution cancels the action, sends a correcting
   `StatSyncNoti` (Candidate shape: same flow instance, no rate) and claims
   nothing. Every result is also delivered to nearby players.
4. **Node state.** One shared registry, keyed by map ID (channels 9011 and 9012
   are independent), holds depleted placements and doodad phases. A shared
   timer respawns placements, advances timed doodad phases (for example the
   Wild Bush 2 → 3/4 cycle) and respawns despawned doodads, sending
   `ChangeGatheringStateNoti` or `ChangeDoodadPhaseNoti` to every session on the
   map.
5. **Snapshots.** With sector streaming (the shared-world sector grid), each
   `SectorUpdateNoti` lists the sector's depleted placements and changed
   doodads, as captured. Without the grid, every map load receives incremental
   `ChangeGatheringStateNoti`/`ChangeDoodadPhaseNoti` notifications instead
   (local choice).

| Rejection | Error code |
| --- | --- |
| Unknown placement or doodad, wrong sector, non-zero placement target low bits, node outside the current map | `InteractionAnim_NotFoundTarget` |
| Doodad target without the doodad unit type | `InteractionAnim_InvalidTargetUnitType` |
| Doodad despawned or in a timed phase | `InteractionAnim_DoodadNotProgressible` |
| Doodad phase expects another interaction | `InteractionAnim_NotMatchInteractionGroup` |
| Placement without a reward | `Gathering_InvalidCmsId` |
| Requested animation differs from the node's animation | `InteractionAnim_Mismatch` |
| More or fewer than one index, or a missing interaction ID | `InteractionAnim_InvalidAnimParamType` |
| Too far, or request position disagrees with the server position | `TooFar` |
| Placement already depleted, or lost to another player at execute | `InteractionAnim_TargetStateNotMatch` |
| Required tool missing or broken; not enough SP (Candidate code) | `InteractionAnim_InvalidCondition` |
| Dead player | `InteractionAnim_Dead` |
| Another action is open | `InteractionAnim_UsingSkill` |
| Execute before the deadline or after expiry | `InteractionAnim_TimeNotMatch` |
| Commit failure or stack overflow | `InteractionAnim_GatheringFail` |
| Malformed request or more than 32 requests per second | `BadRequest` |

Start rejections use `PS2C_StartInteractionAnimRes`, matching the captured
rejection shape. Execute and end rejections use `PS2C_CommonError`, the existing
tutorial-interaction precedent; no official gathering execute rejection was
captured. A start that no shared-world service consumes is logged as
`Native world interaction start unhandled` with only static metadata
(animation, parameter kind, CMS interaction ID, map, gathering readiness).

## Persistence

Gathered materials are stackables in `WorldProgress`, and the worn tool is an
item row; both commit atomically under the character revision in the memory and
PostgreSQL stores. No schema migration was needed. Node and doodad state is
deliberately transient: a server restart makes every node available again.

## Limits and remaining work

- Expertise progression is not implemented: this build levels expertise with
  `ItemLvUpExpertiseReq` and `ExpertiseReqItemCount`, and grants level benefits
  (`SPDecrease`, `RewardIncrease`, `GatheringCountIncrease*`). No capture shows
  a level-up, so gathering never changes expertise and no benefit applies.
- Doodad tool requirements are not enforced and doodads never wear a tool,
  matching the four captured doodad mining gathers; whether the official server
  requires the tool is unverified.
- The not-enough-SP error code and the cancellation `StatSyncNoti` are
  Candidates; neither was captured. `InteractionCancelMinimumTime` (500 ms) is
  not enforced.
- Weight and capacity are not enforced; no rejection rule was captured.
- Nine reward joins are catalog-name Candidates (see above).
- Seventeen gathering doodads with unsupported phase graphs exist in the
  catalog, but none is placed on a hosted map.
- Env_AzhraMap and Interaction1 placements and release-package maps are out of
  scope. The 1 % rare loot groups follow the catalog rate as a percentage; no
  rare drop was captured.

## Live trial history

2026-10-27 (hosted server, original client): pressing F at Roseberry Bushes
produced three outcomes, read from the server log without payloads. Two
interaction starts were silently ignored (the doodad path had no handler). Two
others were answered with `StartInteractionAnimNoti`, but the client never sent
`ExecuteInteractionReq`: the notification named the raw character ID instead of
the wire unit ID and stamped a 100 ns tick instead of the client's nanosecond
clock. A third defect would have refused every reward: the commit did not
advance the progress revision that storage requires. All three are fixed and
covered by tests; the corrected server has not yet been tried with the original
client.

## Verification

```powershell
python tools/build_gathering_data.py
python -m unittest discover -s tools -p 'test_build_gathering_data.py'
cmake --build --preset release --target chronicles-tests
ctest --preset release -R 'Gathering'
```

The C++ unit tests cover the request echo and wire unit, server-clock fallback
for implausible ticks, the SP flow order and single deduction, cancellation,
insufficient SP and death, other open actions, the storage revision rule, tool
wear in the same commit and broken tools, retry safety, range and position
checks, depletion by another player, concurrent claims, placement respawn timing,
independent channels, unknown/malformed/unsupported targets, Roseberry Bush
despawn and respawn without `phase_id`, timed Wild Bush phases, doodad
interaction mismatches, sector doodad entries, request bursts and profile
validation. `NativeWorld.GatheringPlacementsAndDoodadsRewardTheWireUnitAndShareStateOverTls`
runs a placement and a doodad gather over real sockets with `MemoryStore`,
including observer delivery and the map-load snapshot. Set
`CHRONICLES_GATHERING_DEFINITIONS` to the generated profile to run the
generated-profile check.
