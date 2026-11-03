# Shared-world sector streaming

`PS2C_SectorUpdateNoti` (opcode 193) tells the client which world sectors around the
player are live. The emulator now sends it on shared ArcheWorld maps the way the official
server did. Evidence is for client build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`
(executable dump SHA-256 `e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`).

**Status: partial.** The schedule, ordering, clipping and gathering/doodad contents are
implemented and covered by synthetic and TLS tests. No original-client trial of the
streamed sectors has been run.

## Evidence

| Finding | Evidence |
| --- | --- |
| A world's sector index is `floor((position_cm - OriginMap.area.min) / OriginMap.sectorWidth) + 1` per axis, packed `x \| (y << 14)`. | `OriginMap` gives ArcheWorld `area.min = (-918000, -1173000)` cm and `sectorWidth = 5100`. Intersecting all 4,502 exported spot placements with their authored `sectorIndex` pins the origin to that value within 3 cm. The bonfire at (-137216, 310387) cm is sector (154, 291), ID 4767898, as in official traffic. |
| After every map load the server sends a 5 x 5 block around the spawn sector, right after `MapMoveCompleteNoti`. | Every shared-map entry in the seven official captures: 25 updates, y ascending then x ascending. This includes reloads of the same map. |
| The block is clipped per axis to `[sector(Map.area.min) - 1, sector(Map.area.max)]`. | Map 10109 entries sent x 161-164, y 287-290 (16 updates). A map-30104 entry stopped at x 175, the sector holding `area.max.x`. |
| While the player moves, only sectors entering the block are sent. A sector that left and re-entered is sent again; there is no leave message. | 27-455 repeated sector IDs within single map visits per capture. |
| Shared-world updates list only state that differs from the authored default. | Shared-world `doodads` entries are runtime phase changes with `phase_begin_tick` (for example gathering doodad 90). Default-phase doodads such as the bonfire were never listed; most updates carry only `sector_id`. |
| The client creates a sector record only within two sectors of the player's current sector. | `AAC_Sector_Candidate_CreateRecordIfNearPlayer` (RVA 0x5F54B70) checks `abs(dx) <= 2` and `abs(dy) <= 2`, then `AAC_SectorManager_Candidate_RegisterSectorAndResolveUnit` (RVA 0x5F51530) resolves the sector unit (`sector << 32`). Records: `dumps/ida_research/sector-streaming-20261027/`. |

The handler (`AAC_PS2C_SectorUpdateNoti_Candidate_Handler`, RVA 0x61E5B10) also applies
`gathering_deactivated_indices` on receipt, so an update must carry the sector's current
depleted nodes. Otherwise it could re-enable nodes on the client.

## Runtime

`src/world/core/world_sectors.*` owns the geometry and the per-session window. The native adapter
(`refresh_sector_sight` in `src/world/session/native_world.cpp`) supplies gathering state.

1. At startup `WorldSectorGrid` reads the catalog `OriginMap` and `Map` tables. Without
   them, or without a client catalog, no sector updates are sent. Malformed rows,
   duplicate worlds or map IDs and inverted areas fail startup.
2. `MapLoadCompleteReq` on a shared map (not the private tutorial instance, which has its
   own authored sector list) arms the session's `SectorSightTracker` for the current map.
3. After every frame, the player's server-side position selects the sector. When it
   changes, the newly entered sectors are announced in window order. The first update after
   arming announces the whole clipped block. Updates are never sent before the map load
   completes, because the client would drop them while its player stands elsewhere.
4. A position on another map ends the visit until the next map load.
5. Each update includes the sector's depleted gathering placements
   (`gathering_deactivated_indices`) and changed gathering doodads (`doodads`). The
   map-load `ChangeGatheringStateNoti` snapshot is sent only when no sector grid is loaded.

Sector updates are queued after the frame's world response, so at map load they follow
`MapMoveCompleteNoti`, as in the official order. They precede the NPC sight additions of
the same batch.

## Limits

- No original-client trial has observed the streamed sectors.
- The caller-side origin subtraction for `AAC_Sector_Candidate_IndexFromPositionCm` was
  not traced; the origin rule rests on the spot data and official sector IDs.
- The lower-bound clipping rule (one sector below the area minimum) is reproduced from two
  maps; its server-side reason is unknown.
- Level triggers, user objects and server-spawned static NPCs (fields 4-6) are not sent.
  No capture contained `spawned_static_npcs`.
- What exactly the client spawns when a sector record is created remains unresolved.
- Interest is per connection. Other players' doodad changes reach this client through the
  existing gathering broadcasts, not through new sector updates.

## Verification

```powershell
cmake --build --preset release --target chronicles-tests
$env:CHRONICLES_CLIENT_CATALOG = (Resolve-Path data/generated/catalog).Path
ctest --preset release -R 'WorldSectors.*|NativeWorld.SharedMapLoadAnnounces.*'
```

`WorldSectors.*` (10 cases) checks packing, the origin rule against the bonfire, the exact
official map-9012 burst order, both clipping cases, tracker arming, movement, re-entry and
map changes, malformed catalog rows, wire encoding and, with the generated catalog, the
real `OriginMap`/`Map` tables. `NativeWorld.SharedMapLoadAnnouncesTheSectorWindowAndThenEnteringSectors`
runs a real TLS session: 25 updates after `MapMoveCompleteNoti` in official order, then the
5-sector column entered by one move east.
