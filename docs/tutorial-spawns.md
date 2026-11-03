# Tutorial NPC and monster placements

The static export maps all twelve spawners in map 8000 and all three spawners
in the next tutorial instance, map 10117. The official full-session capture
contains 14 and 3 NPC-add notifications respectively. Every notification
matches its map, spawner GUID, NPC CMS ID, faction, animation stance, and
authored spawn point or patrol corridor. Repeated notifications are not new
spawn definitions.

These findings apply to build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`, executable
dump SHA-256 `e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`.
The generator checks catalog and raw-file hashes through the existing export
manifests. `definitions.json.sources` records the files used and their hashes.

## Placement index

Coordinates are authored spawn positions in wire millimeters, in X, Y, Z order.
Yaw is in degrees; the table rounds display values only. BuiltData positions
are centimeters and are multiplied by ten, then truncated toward zero. The
two maps have identity world transforms in the recovered Map rows.

| Map | NPC CMS ID | Catalog name | Spawn X, Y, Z (mm) | Yaw | Movement definition | Initially active |
| --- | ---: | --- | --- | ---: | --- | --- |
| 8000 | 1001671 | Shard of Recollection | -1383698, 751225, -695744 | 35 | Fixed | Yes |
| 8000 | 5001010 | Skeleton Scout | -1005412, 761837, -696436 | -35 | Patrol | Yes |
| 8000 | 5001015 | Bound Prowler | -1221142, 727084, -697328 | 0 | Patrol | Yes |
| 8000 | 5001016 | Skeleton Warder | -1069474, 861211, -696083 | 180 | Patrol | Yes |
| 8000 | 5001082 | Skeleton Spearman | -1026849, 839774, -699937 | -70 | Patrol | Yes |
| 8000 | 5001083 | Rotting Skeleton Spearman | -1080771, 842892, -697068 | 0 | Patrol | Yes |
| 8000 | 5001084 | Rotting Skeleton Infantryman | -964036, 740856, -692500 | -170 | Fixed | Yes |
| 8000 | 5001085 | Rotting Skeleton Heavy Infantryman | -1016559, 798533, -698833 | -90 | Patrol | Yes |
| 8000 | 5001527 | Herald Knight | -1399100, 714200, -699735 | -55 | Fixed | Yes |
| 8000 | 5001528 | Recalling Grounds Herald | -1371859, 759079, -696080 | 35 | Fixed | Scripted |
| 8000 | 5001529 | Recalling Grounds Warden | -1342981, 779252, -696978 | -60 | Patrol | Scripted |
| 8000 | 5001530 | Recalling Grounds Scout | -1358865, 768230, -696790 | 30 | Patrol | Scripted |
| 10117 | 1001294 | Evalo | -1541835, 3080177, -628518 | 155.7915 | Fixed | Yes |
| 10117 | 1001539 | Vadim | -1551945, 3083855, -628527 | -90 | Fixed | Yes |
| 10117 | 1001540 | Guard Captain | -1558654, 3065520, -628514 | -90 | Fixed | Yes |

Map 8000 uses `ArcheWorld_Tutorial/DataLayers/Runtime/Z01_Tutorial`. Map 10117
uses the Map table's `Zone_01_POI_006` layer under `ArcheWorld`; the additional
`Zone_01_POI_006_M` layer has no NPC spawner file in this export. Do not use
`Z03_Channel_Tutorial001` or the public-area copies of these NPC types: their
spawner GUIDs and positions do not match this tutorial instance.

## Generated contract and runtime boundary

`tools/tutorial_spawns.py` joins Map, NpcSpawner, Npc, AnimStanceKeyMapping,
`npc_spawner.json`, and `level_path.json`. Only the recovered deterministic
single-NPC, single-point tutorial selector is supported. Ambiguous populations,
missing patrols, duplicate GUIDs, and invalid coordinates fail extraction.
Mannequin is the catalog behavior of the three second-area NPCs; all have
TownNpc faction. The initial area's monsters have Bandit faction.

The generated tutorial profile exposes:

- `npc_maps`: all map-specific spawners, normalized placements, patrol nodes,
  activation flags, motion keys, and respawn metadata for both tutorial areas.
- `npc_spawns`: all twelve first-area placements with local unit IDs, integer
  millimeter positions, yaw, faction, stance index, GUID, and `initially_active`.
  Monster records also contain validated AI inputs from `Npc`, `NpcTemplate`,
  `UnitModel`, and `UnitGroupModel`: perception, leash/reset, combat spacing, and
  run speed. Distances and speeds are converted from client centimeters to
  millimeters; move-speed rate is per mille.
- `npc_spawners`: the existing first-area spawner index, now including patrols
  and activation metadata.
- `civilians`: the existing dialogue index, containing Shard of Recollection.

`TutorialData` validates and indexes these records at startup. The initial
snapshot sends nine NPC-add notifications: eight monsters and the dialogue NPC.
The other three monster placements remain indexed but do not appear at entry.
All IDs are emulator-owned, private to the instance, and stable on reconnect;
official unit IDs, ticks, health snapshots, and player data are not imported.

The three inactive selectors correspond to the sequential hunt objectives of
quest 10000043. The Warden appears after the earlier key-object quest action in
the trace; the Scout and Herald appear later. That observation does not prove
the exact activation conditions. A later quest/combat implementation must activate
these mapped spawners from verified server events. Respawn timers and one-time-use
flags are recorded as inputs, not implemented simulation.

Patrol execution, collision-aware navigation, original AI phase reconstruction,
and the second map transition are outside this placement increment. A separate
local combat-AI model uses the exported perception and movement inputs; see
`tutorial-combat.md`. Map 10117 is mapped but is not emitted while the character
is on map 8000. No original-client visual trial of these new monster notifications
has been performed.

## Spatial verification

The verifier checks stationary NPCs within 100 mm horizontally and 250 mm
vertically, including yaw modulo 360 degrees within 0.01 degree. Moving NPCs
are checked against their authored patrol segments, allowing 500 mm horizontally
and 2,000 mm vertically. These are explicit evidence-checking allowances, not
movement-authority or collision rules. In particular, the Bound Prowler's first
observed position is over ten meters from its spawn point and differs vertically
from the authored patrol. Terrain projection could explain that vertical offset,
but its cause has not been established. Copying the snapshot as its spawn would
lose the actual placement.

From `Emulator/`:

```powershell
python tools/build_tutorial_data.py
python tools/test_build_tutorial_data.py
python tools/test_tutorial_spawns.py
python tools/verify_tutorial_mapping.py `
  out/captures/official-full-session-20261024-01/decoded-final.jsonl `
  --require-all `
  --output out/captures/official-full-session-20261024-01/tutorial-spawn-mapping.json
$env:CHRONICLES_TUTORIAL_DEFINITIONS = (Resolve-Path data/generated/tutorial/definitions.json).Path
ctest --preset release -R 'Tutorial'
Remove-Item Env:CHRONICLES_TUTORIAL_DEFINITIONS
```

The complete capture verifies all fifteen placements and seventeen NPC-add
notifications. `--require-all` also requires evidence for the initially inactive
spawners. The private output report contains static mappings, match counts,
and spatial errors, without captured runtime identities. Keep captures and
generated profiles under their existing ignored directories.

The placement checks passed on 2026-10-24: twelve Python extraction/verification
tests and four focused C++ spawn tests, including the complete generated profile,
wire encoding, reconnect stability, invalid data, and initially inactive spawners.
The Release test target built successfully. Original-client visual validation
remains pending with the coordinated live trial.
