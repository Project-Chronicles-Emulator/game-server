"""Recover tutorial NPC placements without importing official runtime identities."""

import json
import math
import uuid
from pathlib import Path


MAP_ROOT = Path("X5/Content/Data/BuiltData/Maps")
AXES = ("x", "y", "z")
# Candidate: only the placement position_mm x10 conversion is proven (see position_mm). Applying the
# same x10 to NPC AI distances and to UnitGroupModel.runSpeed is an unproven assumption: the catalog
# unit of those fields has no capture or catalog evidence.
CENTIMETERS_TO_MILLIMETERS = 10
# Candidate: a neutral constant with no catalog or capture evidence; it is not a measured rate.
NEUTRAL_MOVE_SPEED_RATE = 1000


def integer_field(row, key, minimum=0):
    try:
        value = int(row[key] or 0)
    except (TypeError, ValueError) as error:
        raise ValueError(f"Invalid NPC AI definition field: {key}") from error
    if value < minimum:
        raise ValueError(f"Invalid NPC AI definition field: {key}")
    return value


def distance_field_mm(row, key, minimum=0):
    return integer_field(row, key, minimum) * CENTIMETERS_TO_MILLIMETERS


def npc_ai_definition(source, npc):
    # Candidate rule: every unit conversion below is unproven. Distances are the catalog value x10
    # (CENTIMETERS_TO_MILLIMETERS), run_speed_mm_per_second is UnitGroupModel.runSpeed x10 (captured
    # NPC speeds did not confirm that factor), the half cone angle is taken as degrees, and
    # move_speed_rate is a neutral constant. reset_delay_ms is returnEndTime without scaling. This
    # data must not be used to claim movement authority or NPC locomotion fidelity.
    template = source.row("NpcTemplate", npc["FK_template"])
    model = source.row("UnitModel", npc["FK_modelId"])
    group = source.row("UnitGroupModel", model["fk_UnitGroupModelId"])

    combat_min = distance_field_mm(template, "combatSuitableDistMin")
    combat_max = distance_field_mm(template, "combatSuitableDistMax", minimum=1)
    if combat_min > combat_max:
        raise ValueError("Invalid NPC AI definition combat distance range")

    try:
        half_cone_angle = float(npc["searchHalfConeAngle"])
    except (TypeError, ValueError) as error:
        raise ValueError("Invalid NPC AI definition field: searchHalfConeAngle") from error
    cone_range_mm = distance_field_mm(npc, "searchConeRange")
    if (
        not math.isfinite(half_cone_angle)
        or not 0 <= half_cone_angle <= 180
        or (cone_range_mm > 0 and half_cone_angle == 0)
    ):
        raise ValueError("Invalid NPC AI definition field: searchHalfConeAngle")

    return {
        "search_circle_range_mm": distance_field_mm(npc, "searchCircleRange"),
        "search_cone_range_mm": cone_range_mm,
        "search_half_cone_angle": half_cone_angle,
        "hearing_range_mm": distance_field_mm(npc, "hearingRange"),
        "leash_range_mm": distance_field_mm(npc, "returnStartDist", minimum=1),
        "reset_delay_ms": integer_field(npc, "returnEndTime"),
        "combat_distance_min_mm": combat_min,
        "combat_distance_max_mm": combat_max,
        "run_speed_mm_per_second": float(
            distance_field_mm(group, "runSpeed", minimum=1)
        ),
        "move_speed_rate": NEUTRAL_MOVE_SPEED_RATE,
    }


def wire_guid(text):
    raw = uuid.UUID(text).bytes_le
    return [str(int.from_bytes(raw[:8], "little")), str(int.from_bytes(raw[8:], "little"))]


def position_mm(point):
    values = [point[axis] for axis in AXES]
    if any(
        isinstance(value, bool) or not isinstance(value, (int, float))
        or not math.isfinite(value) or abs(value) > 100000000
        for value in values
    ):
        raise ValueError("Invalid tutorial NPC position in centimeters")

    # BuiltData uses Unreal centimeters; the wire uses integer millimeters.
    # Keep authored coordinates, rather than snapshots of a moving NPC.
    return [int(value * 10) for value in values]


def build_spawn_map(source, map_id):
    map_row = source.row("Map", map_id)
    world = map_row["world"].rsplit("/", 1)[1].split(".")[0]
    layers = map_row["runtimeDataLayerPath"].strip("[]").split(",")
    npcs = {int(row["id"]): row for row in source.table("Npc")}
    selectors = {int(row["id"]): row for row in source.table("NpcSpawner")}
    stances = {
        row["animStanceKey"]: index
        for index, row in enumerate(source.table("AnimStanceKeyMapping"))
    }
    spawners = []
    seen_guids = set()

    for layer in layers:
        directory = MAP_ROOT / world / "DataLayers/Runtime" / layer
        relative = directory / "npc_spawner.json"
        if relative.as_posix() not in source.raw_files:
            continue

        path_file = directory / "level_path.json"
        paths = {}
        if path_file.as_posix() in source.raw_files:
            path_data = json.loads(source.read(Path("raw") / path_file))
            paths = {path["id"]: path for path in path_data["paths"]}

        placement_data = json.loads(source.read(Path("raw") / relative))
        for sector in placement_data["spawnerSectors"]:
            for placement in sector["spawners"]:
                guid = placement["id"]
                if guid in seen_guids:
                    raise ValueError(f"Duplicate tutorial spawner GUID: {guid}")
                seen_guids.add(guid)

                selector = selectors[placement["cmsId"]]
                choices = [choice for choice in selector["npc"].split(",") if choice]
                if len(choices) != 1 or selector["npcGroup"]:
                    raise ValueError(f"Unsupported tutorial NPC selector: {guid}")
                npc_id, weight = choices[0].split(":")
                if (int(weight) <= 0 or selector["minPopulation"] != "1"
                        or selector["maxPopulation"] != "1"):
                    raise ValueError(f"Unsupported tutorial NPC population: {guid}")

                npc = npcs[int(npc_id)]
                if npc["behaviorType"] not in ("Civilian", "Mannequin", "Monster"):
                    raise ValueError(f"Unsupported tutorial NPC behavior: {npc_id}")
                candidate = {
                    "cms_id": int(npc_id),
                    "name": npc["name"],
                    "behavior": npc["behaviorType"],
                    "level": int(npc["level"]),
                    "template_id": int(npc["FK_template"]),
                    "loot_pack_id": int(npc["FK_lootPackId"] or 0),
                    "faction": "EX5Faction_" + npc["faction"],
                }
                if npc["behaviorType"] == "Monster":
                    candidate["ai"] = npc_ai_definition(source, npc)
                points = placement.get("pointList", {}).get("points", [])
                if len(points) != 1:
                    raise ValueError(f"Unsupported tutorial NPC placement count: {guid}")
                point = points[0]
                position_mm(point)
                if not math.isfinite(point["yaw"]):
                    raise ValueError(f"Invalid tutorial NPC yaw: {guid}")

                npc_data = placement["npcData"]
                path_id = npc_data["pathId"]
                patrol = None
                if path_id:
                    if path_id not in paths or len(paths[path_id]["nodePositions"]) < 2:
                        raise ValueError(f"Tutorial NPC patrol is missing: {guid}/{path_id}")
                    path = paths[path_id]
                    patrol = {
                        "guid": path_id,
                        "roaming_type": path["roamingType"],
                        "direction": path["dirType"],
                        "restart_type": path["restartType"],
                        "points_mm": [position_mm(node) for node in path["nodePositions"]],
                    }

                spawners.append({
                    "spawner_guid": wire_guid(guid),
                    "spawner_cms_id": placement["cmsId"],
                    "sector": sector["sectorIndex"],
                    "layer": layer,
                    "candidates": [candidate],
                    "points": points,
                    "patrol": patrol,
                    "anim_stance_key": stances[npc_data["animStanceKey"]],
                    "motion_keys": npc_data["x5motionKeys"],
                    "active": selector["activationState"] == "True",
                    "one_time_use": selector["oneTimeUse"] == "True",
                    "spawn_delay_min_ms": int(selector["spawnDelayMinMs"]),
                    "spawn_delay_max_ms": int(selector["spawnDelayMaxMs"]),
                })

    if not spawners:
        raise ValueError(f"No tutorial NPC placements found for map {map_id}")

    spawns = []
    counts = {1: 0, 2: 0}
    for spawner in spawners:
        candidate = spawner["candidates"][0]
        behavior = candidate["behavior"]
        # Preserve the established dialogue NPC namespace; monsters use a separate
        # local namespace. These IDs are private to each character's map instance.
        namespace = 2 if behavior == "Monster" else 1
        counts[namespace] += 1
        unit_id = (namespace << 60) | (counts[namespace] << 12) | 2
        point = spawner["points"][0]
        spawns.append({
            **candidate,
            "unit_id": str(unit_id),
            "position": position_mm(point),
            "yaw": point["yaw"],
            "spawner_guid": spawner["spawner_guid"],
            "anim_stance_key": spawner["anim_stance_key"],
            "initially_active": spawner["active"],
        })

    return {"map_id": map_id, "spawners": spawners, "spawns": spawns}
