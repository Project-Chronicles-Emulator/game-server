# NPC speed provenance

This note records where NPC movement speeds come from and why the captures could not confirm
their unit. It condenses an issue #63 study from 2026-10-24 (archived worktree
`combat-data-716871c`), corrected on 2026-10-27 against the current generators. The capture
measurements below were not rerun for this correction.

## Current rule (Candidate)

- `tools/tutorial_spawns.py` `npc_ai_definition` resolves `Npc.FK_modelId -> UnitModel.id ->
  UnitModel.fk_UnitGroupModelId -> UnitGroupModel` and exports
  `run_speed_mm_per_second = UnitGroupModel.runSpeed x 10`. `move_speed_rate` is the neutral
  constant 1000 (`NEUTRAL_MOVE_SPEED_RATE`). Both are Candidate: no catalog column or capture
  proves the centimeter-per-second reading or the rate.
- `tools/npc_patrol_data.py` `model_speeds` uses the same join and factor for `walkSpeed` and
  `runSpeed` of civilian patrols ([civilian-patrols.md](civilian-patrols.md)).
- `src/world/features/npc/npc_ai.cpp` requires both fields together and rejects a zero speed; there is no runtime
  default ([npc-locomotion-fix-20261026.md](npc-locomotion-fix-20261026.md)).

`UnitGroupModel` (191 rows) is the only catalog table with movement speeds: `walkSpeed`,
`runSpeed`, `sprintSpeed`, `swimSpeed`, `swimSprintSpeed`, `crouchWalkspeed` and `flySpeed`.
No catalog table, raw export or generator contains a field named `run_speed_mm_per_second` or
`move_speed_rate`; those names exist only as protobuf field names in `protocol/descriptors*.pb`.

The original study joined `Npc.FK_modelId` directly to `UnitGroupModel.id`. That gives the same
row for low model ids (for example NPC 5001524 -> model 59 -> walk 260, run 400), but it found
no row for 5001247, 5001104 and 5001510. Through `UnitModel` those resolve to groups 2100,
2100 and 2300, each with `runSpeed` 400.

## Capture measurements (Observed, inconclusive)

Sources: `full-tutorial-capture-20261024-152100`, `generic-part-1-through-level-6-20261024-153513`
and `generic-part-2-20261024-162411` (hashes in [capture-corpus.md](capture-corpus.md)). Only CMS
ids, aggregate speeds and counts are recorded here.

**Displacement between consecutive casts.** Median planar displacement between two skill-cast
positions of the same NPC, divided by the tick difference, compared with the raw `runSpeed`:

| NPC CMS | Median (mm/s) | runSpeed | walkSpeed | Median / runSpeed | Samples |
| --- | ---: | ---: | ---: | ---: | ---: |
| 5001524 | 451 | 400 | 260 | 1.13 | 168 |
| 5001531 | 596 | 450 | 300 | 1.32 | 58 |
| 5001026 | 603 | 250 | 180 | 2.41 | 56 |
| 5001028 | 543 | 450 | 150 | 1.21 | 52 |
| 5001024 | 97 | 150 | 75 | 0.64 | 47 |
| 5001247 | 1136 | 400 | 150 | 2.84 | 55 |
| 5001104 | 2645 | 400 | 150 | 6.61 | 55 |
| 5001510 | 670 | 400 | 150 | 1.68 | 39 |

The last three ratios were recomputed from the recorded medians with the corrected join.
Restricting samples to gaps of at least one second and to NPCs spawned with `MoveDetail_Run`
(no `MoveDetail_Walk` spawn was seen) still gave ratios from 1.04 to 5.88. These intervals mix
standing, repositioning and reactions, so they are a lower bound of travel speed and neither
confirm nor refute the factor 10.

**Root motion and reactions.** `PS2C_StartX5RootMotionNoti` carries start and destination
positions and an end tick but no start tick. Damage and reaction packets carry both ticks but
describe knockback or sway, not locomotion. No captured packet gives start and end time and
position for ordinary NPC walking.

**TimeSync pairing.** `TimeSyncRes` (opcode 1) occurs in two of the three captures. Mapping
frame arrival to server ticks and using arrival as the motion start gave 23 root motions of at
least 500 ms with implausible speeds from 35 to 11,571 mm/s (median 3,257) on the tutorial
capture. Arrival time does not mark the motion start, so this method is unusable.

## What would settle it

- consecutive official NPC `MoveNoti` positions and ticks for an uninterrupted run (the
  original study does not list `MoveNoti` among the packets it searched), or a capture that
  records one authored patrol leg with its start and end ticks, or
- the client movement routine that converts `UnitGroupModel` speeds, traced in the matching IDB.

Until then, keep the factor and the neutral rate marked Candidate and do not claim NPC
locomotion fidelity from synthetic tests.
