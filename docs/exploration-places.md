# Exploration destinations

Open [the searchable exploration guide](exploration-guide.html) in a browser. It runs
entirely offline and offers search by name, region, zone key or associated map id,
category/region filters, copyable GM teleport commands, static coordinate provenance,
and optional browser-local visited checkmarks. Checkmarks do not modify the game.

## Exploring

1. Enter the main world. If necessary use `/gm tp_map 9012` first.
2. Find a village, landmark, housing area, region or trading post in the guide.
3. Copy its `/gm tp <x> <y> <z>` command into in-game chat. Coordinates are meters.
4. Use `/gm pos` to verify your position after the move.
5. Use `/gm reveal_map` to save all known fog areas, then return to character selection
   and select the same character again to reload them. See [map exploration](map-exploration.md).

Every named `Zone` catalog row is retained, including disabled, duplicate-named,
dungeon and unplaced entries. Distinct zone ids/keys remain separate even when their
English names match. A missing command means that a usable main-world coordinate
has not been established; it does not mean the named location was omitted.
Dungeon/map ids are listed as references, not as proof that an instance can be entered
or its encounter played. Teleport commands are deliberately withheld for other worlds.

## Coordinate confidence

- **Authored discovery point:** the `discovered_point.json` entry names this exact zone key.
- **Authored trade wagon parking:** a `tradingpost.json` placement identifies the trade post.
- **Approximate anchor inside 2D bounds:** an actual discovery, NPC spawn or trade parking
  point is selected nearest the center of the zone's bounding rectangle. Its original
  authored height is preserved. Rectangle overlap and unused grid cells mean this is
  not proof that the point lies in the named area's actual polygon.
- **Approximate anchor in associated map layer:** the zone's `Map.FK_zoneKey` and authored
  runtime-layer membership supply an anchor; no exact landmark match is claimed.
- **No verified 3D anchor:** no position is fabricated from a 2D boundary or map center.

These are static exports, not tested player spawn positions. NPC anchors can be crowded,
under a roof, on a different floor or inside collision. Keep `/gm pos` coordinates for a
known return point. Fog reveal and GM protection do not remove physical collision or
activate content missing from the emulator. The [local zone access helper](local-zone-unlock.md)
is a separate feature for the researched client's disabled-zone movement restriction.

The guide assumes the character remains in the `ArcheWorld` map asset. Its same-map `tp`
commands preserve that map instead of inventing instance routes. The default populated
server map is 9012; other regions can have scenery without corresponding server NPCs,
quests or encounters. Region names and associated map ids come from the matching catalog,
not older ArcheAge world maps.

## Regeneration and validation

From `Emulator/`:

```powershell
python tools/test_build_exploration_places.py
python tools/build_exploration_places.py
python tools/build_exploration_places.py --check
```

Canonical JSON is generated at `data/generated/exploration/places.json`; the portable
HTML is generated at `docs/exploration-guide.html`. Edit the generator and its HTML
template under `tools/`, then regenerate; do not edit the generated files. The full
client-data exporter also generates `exploration/places.json` and `exploration/guide.html`
beside its exported catalog, so custom export directories retain a self-contained guide.

The exporter uses the existing hash-verifying `Source` reader and records all catalog/raw
source hashes plus the researched executable SHA-256/build identity. It rejects invalid
coordinates. Synthetic tests cover exact matches, unavailable names, real-height retention,
deterministic ordering and HTML script-data escaping. A matching-export check confirms
that every named Zone id survives and every offered command has a main-world source anchor.
No original-client landing safety or traversal trial is claimed.
