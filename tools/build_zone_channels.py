"""Export ArcheWorld spatial zone channel cells from the hash-checked authored zone grid.

Each authored zone whose key names exactly one ArcheWorld field, point of interest or housing
area becomes a grid of 5100 cm cells. Points of interest, villages and housing areas lie inside
their parent field's cells; the client resolves such a cell to the nested zone (it asks to
travel into the nested map on entry and back to the parent on exit), so every grid records its
map's nesting depth and the server gives the deepest grid precedence. Grids of equal depth must
not overlap. Entrance and exit conditions are exported for the server to check; the client
checks them before it asks. Instance zones keep the travel planner's own entry rules.
"""

import argparse
import json
from pathlib import Path

from build_level_trigger_volumes import write_document
from tutorial_spawns import MAP_ROOT


CELL_WIDTH_CM = 5100
WORLD = "ArcheWorld"
SUPPORTED_TYPES = {"Field", "PointOfInterest", "HousingArea"}
MAXIMUM_NESTING_DEPTH = 8


def eligible_maps_by_zone(source):
    rows_by_id = {}
    maps_by_zone = {}
    for row in source.table("Map"):
        rows_by_id[row["id"]] = row
        if (row["type"] not in SUPPORTED_TYPES or not row["FK_zoneKey"]
                or row["world"] != f"/Game/Maps/{WORLD}/{WORLD}.{WORLD}"):
            continue
        key = row["FK_zoneKey"]
        if key in maps_by_zone:
            raise ValueError(f"Ambiguous maps for zone {key}")
        maps_by_zone[key] = row
    return maps_by_zone, rows_by_id


def nesting_depth(row, rows_by_id):
    depth = 0
    parent = row["parentMapId"]
    while parent:
        depth += 1
        if depth > MAXIMUM_NESTING_DEPTH or parent not in rows_by_id:
            raise ValueError(f"Invalid map nesting for map {row['id']}")
        parent = rows_by_id[parent]["parentMapId"]
    return depth


def zone_cells(zone):
    bounds = zone["boundary"]
    values = [bounds[key] for key in ("minX", "minY", "maxX", "maxY")]
    if any(type(value) is not int or value % CELL_WIDTH_CM for value in values):
        raise ValueError(f"Unaligned spatial grid: {zone['key']}")
    min_x, min_y, max_x, max_y = (value // CELL_WIDTH_CM for value in values)
    width = max_x - min_x
    height = max_y - min_y
    mask = zone["gridUsed"]
    if width <= 0 or height <= 0 or len(mask) != width * height or set(mask) - {"0", "1"}:
        raise ValueError(f"Invalid spatial mask: {zone['key']}")
    return min_x, min_y, width, height, mask


def build_zone_channels(source):
    maps_by_zone, rows_by_id = eligible_maps_by_zone(source)
    authored = json.loads(source.read(Path("raw") / MAP_ROOT / WORLD / "zone.json"))

    grids = []
    cells_by_depth = {}
    for zone in authored["zones"]:
        row = maps_by_zone.get(zone["key"])
        if row is None:
            continue
        depth = nesting_depth(row, rows_by_id)
        min_x, min_y, width, height, mask = zone_cells(zone)
        occupied = cells_by_depth.setdefault(depth, set())
        for index, used in enumerate(mask):
            if used != "1":
                continue
            cell = (min_x + index % width, min_y + index // width)
            if cell in occupied:
                raise ValueError(f"Overlapping channel cell at depth {depth}: {cell}")
            occupied.add(cell)
        grids.append({"map_id": int(row["id"]), "zone_key": zone["key"], "type": row["type"],
                      "parent_map_id": int(row["parentMapId"] or 0), "depth": depth,
                      "gated": bool(row["FK_enteranceConditionId"] or row["FK_exitConditionId"]),
                      "min_cell_x": min_x, "min_cell_y": min_y, "width": width,
                      "height": height, "mask": mask})

    return {"format_version": 2, "world": WORLD, "cell_width_cm": CELL_WIDTH_CM,
            "grids": sorted(grids, key=lambda grid: grid["map_id"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output-dir", type=Path, default=Path("data/generated/triggers"))
    options = parser.parse_args()

    from build_tutorial_data import Source

    source = Source(options.source)
    document = build_zone_channels(source)
    document["source_sha256"] = dict(sorted(source.sources.items()))
    path = options.output_dir / "ArcheWorld-zone-channels.json"
    digest = write_document(path, document)
    print(json.dumps({"output": str(path), "sha256": digest, "grids": len(document["grids"])}))


if __name__ == "__main__":
    main()
