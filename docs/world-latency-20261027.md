# World latency and Shining Shore static NPC dialogue — 2026-10-27

Two reports from the hosted server (North America East) were investigated together:
the quest "Way of the Hunt" (10000134) could not be advanced by talking to Herlei, and the
in-game RTT was about 426 ms although the same route had shown about 120 ms.

## Quest 10000134: Herlei could not be talked to

The quest is accepted by a trigger volume and has one `InteractNpc` objective for NPC
template 1001600 (Herlei). The live database showed the character on map 9012 with the quest
saved and objective 1 at zero.

Herlei is a client-owned static NPC (`staticNpcData` in `Zone_01_Public_01`). The server never
announces such actors; a dialogue reaches `WorldQuestBook::start_dialogue` only when the exact
static identity is in the map-scoped static interaction index (`src/world/features/npc/map_static_npcs.cpp`).
`tools/build_map_static_npcs.py` admitted only `Release`-package maps. Maps 9011 and 9012, the
Shining Shore public channels, are `Demo`-package maps, and they are the only public maps this
client build reaches (the gathering exporter documents the same rule). The index therefore had
no 9011/9012 entries, the dialogue was refused with `Interaction_TargetNotFound`, and the
objective was never credited.

The exporter now admits Demo and Release ArcheWorld Field/HousingArea maps. Regenerating the
index gives 726 targets: 250 on 9012 and 1 on 9011 were added, and all 475 earlier targets are
byte-for-byte unchanged. Herlei resolves on 9012 as unit `20477965980667911` (sector identity,
static id 1, kind 7). The quest rules and range checks are unchanged.

## RTT: server-side queueing, not the network route

Evidence from the live server (one player online):

- The process used about 0.9 CPU core; four worker threads had similar CPU totals.
- Every client packet waited 40-175 ms between its frame being read and its processing
  finishing, including a plain `PC2S_Ping` (114 ms). Packets were processed back to back, so
  a Ping queued behind movement and skill packets waited for all of them.
- About 66 frames per second were written to the client, mostly `PS2C_MoveNoti` bursts, and
  each frame produced two `info` log lines (about 130 lines per second).

Cause: the shared world-mob tick (`CombatSession::advance_private_encounter`) runs on the
session's worker strand at least every 100 ms, on civilian patrol deadlines and twice around
every combat request. For every mob on the map (417 on 9012, 812 on 9031) the AI step copied
the NPC's `npc_skills` JSON array and re-walked skill, effect and projectile JSON to find
executable attacks and their ranges, even for idle mobs far from the player. The strand was
busy most of the time, so the player's packets, Ping included, queued behind it.

Changes:

- `CombatData` builds `npc_attacks` once at load: each NPC template's executable attacks in
  catalog order with minimum/maximum range and the ranged flag. AI ticks, attack selection and
  `usable_npc_attack` read this table. Selection order, distance bands, cooldowns, least
  recently used choice and cast-range rules are unchanged.
- World sockets set `TCP_NODELAY`, so a Pong or small gameplay frame is not held behind
  earlier unacknowledged frames waiting for the client's delayed ACK.
- The four per-frame transport traces (awaiting, frame read, write, written) moved from
  `info` to `debug`. The admin log ring held only about seven seconds of history before.

## Verification

Measured in an isolated worktree at `4bf7955` with only this change applied, so other agents'
in-progress edits did not affect the result (development desktop, Release build):

- `WorldMobs.GeneratedBusiestMapTickStaysWithinPacketBudget` (profile-gated) runs 50 world-mob
  ticks on map 9031 (812 spawns): **14.6 ms per tick before, 0.97 ms after**. The hosted VPS
  is slower per core, which is consistent with the 40-175 ms waits seen there.
- `CombatData.NpcAttackOptionsKeepCatalogOrderRangesAndSkipNonAttacks` checks catalog order,
  ranges, the ranged flag and that non-attacks and unknown skills are skipped.
- Combat, WorldMobs, NpcAi, CombatData and quest filters: 179 passed, 9 gated skips.
  All non-NativeWorld tests: 791 passed, 41 skipped. NativeWorld tests, one process each with
  the generated catalog: 74 passed, 25 skipped. `BagUnequipWithCarriedWeightAboveDefault`
  `SafeWeightHasNoRefusal` crashes exactly as it did before this change (see
  [combat-rework-20261026.md](combat-rework-20261026.md)), and
  `GatheringSnapshotPreservesWorldMonsterSpawnsAndRewards` could not load a gathering profile
  that another agent had regenerated in a newer format.
- `tools/test_build_map_static_npcs.py`: 7 passed, including the Demo/Release map filter.

These are synthetic and server-side measurements. The hosted RTT after deployment and the
Herlei dialogue in the original client have not been observed yet.

## Deployment notes

The hosted data directory had no `static-npcs` export at all, so static NPC dialogue was
disabled there even on Release maps. `tools/export_client_data.py` and
`tools/start_persistent.ps1` now generate it with the other server data. Deploying the fix
needs both the new server binary and `data/generated/static-npcs/definitions.json`; the
server loads the index at startup.

```powershell
python -m unittest discover -s tools -p "test_build_map_static_npcs.py"
python tools/build_map_static_npcs.py
$env:CHRONICLES_WORLD_MOB_PROFILE = (Resolve-Path data/generated/world-mobs/definitions.json).Path
ctest --preset release -R "WorldMobs|CombatData|NpcAi|Combat\."
```
