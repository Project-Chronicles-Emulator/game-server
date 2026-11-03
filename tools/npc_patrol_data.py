"""Normalize authored civilian routes without inventing missing motion-point activities."""

import json
import math
from pathlib import Path

from tutorial_spawns import position_mm


MAXIMUM_NODES = 1024
SUPPORTED_MODES = ("LevelPath_Circle", "LevelPath_Forward", "LevelPath_NearPoint")


def table_index(source, name, key="id"):
    return {row[key]: row for row in source.table(name)}


def model_speeds(source, npc, context):
    models = table_index(source, "UnitModel")
    groups = table_index(source, "UnitGroupModel")
    try:
        model = models[npc["FK_modelId"]]
        group = groups[model["fk_UnitGroupModelId"]]
        walk = float(group["walkSpeed"]) * 10
        run = float(group["runSpeed"]) * 10
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(f"NPC patrol model speed is missing: {context}") from error
    if any(not math.isfinite(speed) or not 0 < speed <= 100000 for speed in (walk, run)):
        raise ValueError(f"Invalid NPC patrol model speed: {context}")
    return {"EX5MoveSpeedType_Walk": walk, "EX5MoveSpeedType_Run": run}, group


def arrival_activity(source, npc_group, option, stance_name, context):
    """Resolve only inline activities whose exact model/stance/motion join exists."""
    target = option.get("targetAnimStanceKey") or stance_name
    stance_names = [row["animStanceKey"] for row in source.table("AnimStanceKeyMapping")]
    if target not in stance_names:
        return {}, "unknown_stance"
    stance = stance_names.index(target)
    motion_key = option.get("x5motionKey", "")
    if not motion_key:
        return {"stance": stance}, None

    matches = [row for row in source.table("X5Motion")
               if row["x5MotionKey"] == motion_key
               and row["skeletalMeshServantKey"] == npc_group["skeletalMeshServantKey"]
               and row["animStanceKey"] == target]
    if len(matches) != 1:
        return {"stance": stance}, "unresolved_motion"
    mapping = [row["gameAnimKey"] for row in source.table("GameAnimKeyMapping")]
    key = matches[0]["gameAnimKey"]
    if key not in mapping:
        raise ValueError(f"NPC patrol animation mapping is missing: {context}/{key}")
    loops = option.get("loopSectionPlayCnt", 0)
    if isinstance(loops, bool) or not isinstance(loops, int) or not -1 <= loops <= 100000:
        raise ValueError(f"Invalid NPC patrol animation loop count: {context}")
    return {"stance": stance, "motion": {
        "game_anim_mapping_id": mapping.index(key),
        "target_anim_stance_mapping_id": stance,
        "loop_count": loops,
    }}, None


def build_patrol(source, npc, placement, path, context):
    modes = tuple(path.get(key) for key in ("roamingType", "dirType", "restartType"))
    if modes != SUPPORTED_MODES:
        return None, "unsupported_patrol_mode"
    points = path.get("nodePositions", [])
    if not 2 <= len(points) <= MAXIMUM_NODES:
        raise ValueError(f"Invalid NPC patrol node count: {context}")
    positions = [dict(zip("xyz", position_mm(point))) for point in points]
    if len({tuple(position.values()) for position in positions}) < 2:
        raise ValueError(f"NPC patrol has no movement: {context}")

    options = {}
    for option in path.get("nodeOptions", []):
        index = option["idx"]
        if (isinstance(index, bool) or not isinstance(index, int)
                or not 0 <= index < len(points) or index in options):
            raise ValueError(f"Invalid or duplicate NPC patrol node option: {context}")
        options[index] = option

    speeds, group = model_speeds(source, npc, context)
    if any(option.get("moveSpeedType", "EX5MoveSpeedType_Walk") not in speeds
           for option in options.values()):
        return None, "unsupported_patrol_speed"

    nodes = []
    unresolved = []
    speed_type = "EX5MoveSpeedType_Walk"
    stance_name = placement["npcData"].get("animStanceKey", "Base")
    for index, pos in enumerate(positions):
        option = options.get(index, {})
        speed_type = option.get("moveSpeedType", speed_type)
        if not isinstance(option.get("isStay", False), bool):
            raise ValueError(f"Invalid NPC patrol stay flag: {context}")
        wait = option.get("stayMsec", 0) if option.get("isStay", False) else 0
        if isinstance(wait, bool) or not isinstance(wait, int) or not 0 <= wait <= 3600000:
            raise ValueError(f"Invalid NPC patrol wait duration: {context}")
        node = {
            "position_mm": pos,
            "speed_mm_per_second": speeds[speed_type],
            "move_detail": "MoveDetail_Run" if speed_type.endswith("_Run") else "MoveDetail_Walk",
            "wait_ms": wait,
        }
        if wait:
            yaw = option.get("stayRotation", 0)
            if isinstance(yaw, bool) or not isinstance(yaw, (int, float)) or not math.isfinite(yaw):
                raise ValueError(f"Invalid NPC patrol wait rotation: {context}")
            node["wait_yaw"] = yaw
            activity, reason = arrival_activity(source, group, option, stance_name, context)
            node.update(activity)
            if reason:
                unresolved.append({"node_index": index, "reason": reason})
        for identifier in option.get("motionPointId", []):
            unresolved.append({"node_index": index, "motion_point_guid": identifier,
                               "reason": "motion_point_definition_not_exported"})
        nodes.append(node)

    # NearPoint startup uses the nearest authored node; subsequent joins sample the same
    # server epoch. No arbitrary straight line is inserted from a spawn through buildings.
    spawn = position_mm(placement["pointList"]["points"][0])
    first = min(range(len(nodes)), key=lambda index: sum(
        (nodes[index]["position_mm"][axis] - coordinate) ** 2
        for axis, coordinate in zip("xyz", spawn)
    ))
    nodes = nodes[first:] + nodes[:first]
    return {
        "mode": "circle_forward",
        "path_guid": path["id"],
        "authored_first_node": first,
        "nodes": nodes,
        "unresolved_activities": unresolved,
        "limits": "Authored polyline; no navmesh projection or motion-point script reconstruction. "
                  "Node speed inheritance and NearPoint startup are local Candidate policies.",
    }, None


def load_layer_paths(source, directory):
    relative = directory / "level_path.json"
    if relative.as_posix() not in source.raw_files:
        return {}
    data = json.loads(source.read(Path("raw") / relative))
    paths = {}
    for path in data["paths"]:
        identifier = path["id"]
        if identifier in paths:
            raise ValueError(f"Duplicate NPC patrol path: {relative}/{identifier}")
        paths[identifier] = path
    return paths
