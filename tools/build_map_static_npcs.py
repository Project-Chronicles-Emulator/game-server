"""Export trusted interaction targets for client-owned static NPCs.

These entries authorize dialogue/shops only. They must never produce UnitAddNoti:
staticNpcData actors already belong to the client's sector scene. Unit identity follows the
verified SectorUpdateNoti handler: (sector << 32) | (staticNpcId << 12) | 7.

Sources are each map's runtime data layers plus ArcheWorld's always-loaded Static layers
(villages such as Ruins of Gold), the latter limited to the map's authored area.
"""

import argparse
import json
from pathlib import Path

from build_gathering_data import sector_id
from build_stationary_npc_placements import layer_names, millimeters, parse_candidates
from build_tutorial_data import BUILD_ID, Source, write_profile
from tutorial_spawns import MAP_ROOT
from world_layers import ARCHEWORLD, inside_area, map_area_cm, static_layer_directories

# Every playable ArcheWorld map of the shipped packages: open fields, housing areas and the
# instances, points of interest and raids whose runtime layers hold their own static NPCs.
# The index only authorizes map-scoped interaction; it never spawns, so covering a map that
# a player cannot reach yet is harmless.
SUPPORTED_PACKAGES = ("Demo", "Release")
SUPPORTED_MAP_TYPES = ("HousingArea", "Field", "Instance", "PointOfInterest", "Raid")


def static_unit_id(sector, local_id):
    for axis in ("x", "y"):
        value = sector[axis]
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value < (1 << 14):
            raise ValueError("Invalid static NPC sector")
    if isinstance(local_id, bool) or not isinstance(local_id, int) or not 1 <= local_id < (1 << 20):
        raise ValueError("Invalid static NPC local ID")
    return (sector_id(sector) << 32) | (local_id << 12) | 7


def add_static_targets(document, map_row, npcs, selectors, targets, area=None):
    """Adds single-position static NPC placements, optionally limited to the map's area."""
    for sector in document["spawnerSectors"]:
        for placement in sector["spawners"]:
            static = placement.get("staticNpcData")
            if not static:
                continue
            # Only single authored positions are established. Adventure-controlled
            # activation and ambiguous placement choices remain unsupported.
            points = placement.get("pointList", {}).get("points", [])
            if len(points) != 1 or static.get("adventureCmsId", 0):
                continue
            point = points[0]
            # Static layers span the whole world; each public map owns only its own area.
            if area is not None and not inside_area(point, area):
                continue
            npc_id = placement["cmsId"]
            selector = selectors.get(npc_id)
            if selector is None:
                raise ValueError(f"Static placement has unknown NPC spawner {npc_id}")
            # The client owns these actors' day/night lifetime and script activation. A schedule
            # or an inactive start state must not erase their interaction identity once the
            # client shows them. This index authorizes exact map/template/range checks, not
            # spawning or scheduling.
            if selector.get("weatherGroup"):
                continue
            candidates = parse_candidates(selector, placement["id"])
            unique_ids = {candidate["npc_id"] for candidate in candidates}
            if len(unique_ids) != 1:
                continue
            npc_id = unique_ids.pop()
            if npc_id not in npcs:
                raise ValueError(f"Static placement has unknown NPC {npc_id}")
            unit_id = static_unit_id(sector["sectorIndex"], static["staticNpcId"])
            if unit_id in targets:
                raise ValueError(f"Duplicate static NPC identity in map {map_row['id']}")
            targets[unit_id] = {
                "map_id": int(map_row["id"]),
                "unit_id": str(unit_id),
                "npc_cms_id": npc_id,
                "position_mm": [millimeters(point[axis], axis, placement["id"])
                                for axis in ("x", "y", "z")],
            }


def build_map(source, map_row):
    npcs = {int(row["id"]): row for row in source.table("Npc")}
    selectors = {int(row["id"]): row for row in source.table("NpcSpawner") if "npc" in row}
    world = map_row["world"].rsplit("/", 1)[1].split(".")[0]
    targets = {}
    missing = []
    for layer in layer_names(map_row):
        path = MAP_ROOT / world / "DataLayers/Runtime" / layer / "npc_spawner.json"
        if path.as_posix() not in source.raw_files:
            missing.append(layer)
            continue
        document = json.loads(source.read(Path("raw") / path))
        add_static_targets(document, map_row, npcs, selectors, targets)

    area = map_area_cm(map_row)
    if area is not None:
        for directory in static_layer_directories(source, world):
            path = directory / "npc_spawner.json"
            document = json.loads(source.read(Path("raw") / path))
            add_static_targets(document, map_row, npcs, selectors, targets, area)
    return [targets[key] for key in sorted(targets)], missing


def supported_map(map_row):
    return (map_row.get("package") in SUPPORTED_PACKAGES
            and map_row.get("type") in SUPPORTED_MAP_TYPES
            and map_row.get("world") == ARCHEWORLD)


def build(source):
    targets = []
    missing = {}
    for map_row in source.table("Map"):
        if not supported_map(map_row):
            continue
        entries, missing_layers = build_map(source, map_row)
        targets.extend(entries)
        if missing_layers:
            missing[map_row["id"]] = missing_layers
    return {
        "format_version": 1,
        "build_id": BUILD_ID,
        "targets": targets,
        "missing_layers": missing,
        "sources": source.sources,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path, default=Path("data/generated/static-npcs/definitions.json"))
    arguments = parser.parse_args()
    document = build(Source(arguments.source))
    write_profile(arguments.output, document)
    print(f"Exported {len(document['targets'])} map-scoped static NPC interaction targets.")


if __name__ == "__main__":
    main()
