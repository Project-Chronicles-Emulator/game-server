# Official tutorial world trace — 2026-10-24

The original Steam client recorded bidirectional world application frames in
`X5Packet.log` while the user played through the tutorial and entered the
shared area. The source build ID is `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`.
The complete raw snapshot and decoded JSONL are private files under
`out/captures/official-full-session-20261024-01/`. Do not commit or share them:
world frames can contain character and session values.

The final snapshot contained 6,224 complete frames using 115 distinct opcodes.
All 6,224 decoded and passed exact semantic frame round trips through
`chronicles-replay`. Its final 1,647 bytes were an incomplete record and were
excluded from these counts. Overlapping entry, tutorial-action, and arrival
slices are also under `out/captures/` to preserve progress if the live log is
rotated. They overlap and must not be added together as independent traffic.
The trimmed `native-session-complete.bin` contains exactly those 6,224 frames
and has SHA-256
`83657fb30e1d479361e58e7040dd45ff5e42b365226ff1ac8f734200ce548609`.

## Observed map sequence

| Order | Map ID | Evidence | Interpretation |
| --- | ---: | --- | --- |
| 1 | 8000 | Map start, load-complete request, map complete | First instanced tutorial area |
| 2 | 10117 | Same map handshake after cutscene end | Second instanced tutorial area, confirmed by the user |
| 3 | 9011 | Same map handshake after a second cutscene end | Shared area, confirmed by the user |

The first transition followed combat, a boss-killed notification, a quest
reward request, and `PC2S_CutSceneEndReq`. The second followed dialogue, an
interaction, quest progress, another `PC2S_CutSceneEndReq`, and then the map
handshake. These are observed packet orders, not proof of the server's exact
quest conditions.

The trace includes five quest-accept requests, ten quest-progress requests,
six quest-reward requests, skills, projectile updates, loot, dialogue,
interactions, and combat notifications. A separate shared-area slice has 314
complete frames, including 33 unit-add notifications. Five of those have a
`player` nub; the others are NPCs or other unit types. The full snapshot has
12 player unit-add records after entering map 9011. These are notification
counts, not a count of distinct nearby people.

No `PC2S_MoveReq` appeared in this official run despite visible movement.
The existing local emulator trial did produce that opcode. Do not assume the
official logger captures every movement transport path or derive position
persistence from this absence.

## Implementation boundary

With the generated tutorial profile loaded, map 8000 now has private
per-character instances, persistent floor pickups, trigger-based quests,
dialogue, and two animated quest objects. See
[tutorial-interactions.md](tutorial-interactions.md) for supported requests,
validation, and the remaining movement and scaling limits. Combat, cutscenes,
and map transitions remain unimplemented. The captured world frames provide
message order and payload examples; arbitrary replay does not establish correct
state or persistence. Character creation and HTTP/front authentication are
outside the scope of this world logger.

Replay a private snapshot from `Emulator/` with:

```powershell
& out/build/windows/bin/Release/chronicles-replay.exe `
  out/captures/official-full-session-20261024-01/native-session-complete.bin protocol `
  --jsonl out/captures/official-full-session-20261024-01/decoded-final.jsonl
```
