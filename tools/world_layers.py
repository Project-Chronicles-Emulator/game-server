"""Shared ArcheWorld data-layer scoping for the map-based exporters.

A Map row lists only its runtime data layers. The ArcheWorld world additionally keeps
always-loaded Static data layers (villages and zone content such as 02_RuinsofGold) that the
client shows on every map of that world. Exporters assign a static-layer placement to a map
when the placement lies inside the map's authored area.
"""

import json

from tutorial_spawns import MAP_ROOT

ARCHEWORLD = "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld"
PUBLIC_PACKAGES = ("Demo", "Release")


def fragment(text):
    """Catalog object fields are stored without their enclosing braces."""
    return json.loads("{" + text + "}") if text else {}


def world_name(map_row):
    return map_row["world"].rsplit("/", 1)[-1].split(".")[0]


def world_directory(source, map_row):
    """The exported folder of a map's world below MAP_ROOT. Dungeon worlds are grouped under
    MazeDungeons (``MazeDungeons/DungeonZ02D01``); every other world has its own folder."""
    world = world_name(map_row)
    grouped = (MAP_ROOT / "MazeDungeons" / world).as_posix() + "/"
    if any(path.startswith(grouped) for path in source.raw_files):
        return f"MazeDungeons/{world}"
    return world


def runtime_layer_names(map_row):
    return [name.strip() for name in map_row["runtimeDataLayerPath"].strip("[]").split(",")
            if name.strip()]


def map_area_cm(map_row):
    """The map's authored horizontal bounds in centimetres, or None when it has none."""
    if not map_row.get("area"):
        return None
    area = fragment(map_row["area"])
    bounds = {corner: (area[corner]["x"], area[corner]["y"]) for corner in ("min", "max")}
    if any(low > high for low, high in zip(bounds["min"], bounds["max"])):
        raise ValueError(f"Inverted map area: {map_row['id']}")
    return bounds


def inside_area(point, area):
    """True when an authored point (centimetres) lies inside the map's horizontal bounds."""
    return all(area["min"][index] <= point[axis] <= area["max"][index]
               for index, axis in enumerate(("x", "y")))


def uses_static_world_layers(map_row, map_types=("Field",)):
    """Public ArcheWorld maps of the given types that show the world's Static layers."""
    return (map_row.get("world") == ARCHEWORLD
            and map_row.get("package") in PUBLIC_PACKAGES
            and map_row.get("type") in map_types
            and map_area_cm(map_row) is not None)


def uses_own_static_layers(map_row):
    """Public Field maps of other worlds (story1, Interaction1) whose world keeps its content
    in Static layers; the field shows all of it."""
    return (bool(map_row.get("world")) and map_row.get("world") != ARCHEWORLD
            and map_row.get("package") in PUBLIC_PACKAGES
            and map_row.get("type") == "Field")


def static_layer_directories(source, world, file_name="npc_spawner.json"):
    """Always-loaded layer directories holding `file_name`. Test layers are excluded."""
    prefix = (MAP_ROOT / world / "DataLayers/Static").as_posix() + "/"
    directories = set()
    for path in source.raw_files:
        if not path.startswith(prefix) or not path.endswith("/" + file_name):
            continue
        layer = path[len(prefix):].split("/", 1)[0]
        if "test" in layer.lower():
            continue
        directories.add(MAP_ROOT / world / "DataLayers/Static" / layer)
    return sorted(directories, key=lambda directory: directory.as_posix())


def placement_point(placement):
    """The first authored point of a spawner placement, or None when it has none."""
    points = placement.get("pointList", {}).get("points", [])
    return points[0] if points else None


def placement_in_map(placement, area):
    """Static-layer placements belong to a map when their first point is inside its area."""
    point = placement_point(placement)
    return point is not None and inside_area(point, area)


def map_layer_files(source, world_directory, map_row, file_name, static_map_types=()):
    """Returns (relative path, area or None) for every exported layer file a map shows.

    Runtime layers named by the Map row belong to the map whole (area None). For public
    ArcheWorld maps of `static_map_types`, the always-loaded Static layers are added with the
    map's authored area, so callers keep only the entries inside it.
    """
    files = []
    for layer in runtime_layer_names(map_row):
        relative = MAP_ROOT / world_directory / "DataLayers/Runtime" / layer / file_name
        if relative.as_posix() in source.raw_files:
            files.append((relative, None))

    if static_map_types and uses_static_world_layers(map_row, static_map_types):
        area = map_area_cm(map_row)
        for directory in static_layer_directories(source, world_name(map_row), file_name):
            files.append((directory / file_name, area))
    return files
