"""Export the map travel profile: instances, dungeons, portals and map-move cutscenes.

The map definitions come from the hash-verified client export. The Cliffs entrance uses
the fixed destination observed in the official capture because its spawn table is absent
from that export; no player or session identity is included. Source chain:

* ``Map``: type (Field, Instance, Dungeon, PointOfInterest, Raid, HousingArea), parent map,
  world asset, authored area, level transform, entrance and exit condition ids.
* ``MapCondition``: the entrance and exit conditions the client checks before it asks to
  travel (static analysis: AAC_MapMove_Candidate_RequestMapTravel tests the current map's exit
  condition and the destination's entrance condition, then sends ChannelTriggerEntryReq).
* ``Maps/<world>/DataLayers/{Runtime,Static}/<layer>/level_trigger_volume.json`` for every
  world, including the MazeDungeons levels: ``channelSession`` and ``channel`` volumes name a
  destination map, ``portal`` volumes name a ``Portal`` row. The client reports entering them
  with PC2S_LevelTriggerReq and then waits for the server's map move (static analysis of the
  volume overlap handlers that build LevelTriggerReq and start the pending transition).
* ``LevelTriggerVolume``/``LevelTriggerTask``/``LevelTriggerCondition``: per-volume rules
  (cutscene, map move, doodad phase, client dialogue) and their conditions.
* ``Portal``: from map, to map and spawn id of each portal row (several dungeons reuse ids).
  Same-map dungeon rows (``GoBoss``/``GoExit``) name spawn points the export lacks; their
  arrival comes from the dungeon's own boss and exit doodad interactions (see
  ``dungeon_jump_arrivals``).
* ``MapMoveCutScene``/``MapMoveCutSceneCondition``: the cutscene MapMoveStartNoti names when
  the character travels between two maps.
* ``CutScene``: invisibility, client-only flags and end points of every cutscene. An end point
  names a map and a server spawn id; its position is the captured one, else the single doodad
  of that id placed on the end map, else the runtime arrival rule (``end_point_of``).

Condition rows are normalized for the runtime evaluator. Kinds the server can establish
(Quest, OwnItem, AdventureObject, CheckUnitLevel, PlayRecord, PartyState, MapEnter, MapLeave)
keep their parameters; every other kind is exported as ``ClientChecked`` because the client
evaluates it before it sends the request (CUSTOM: the server trusts those rows).
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

from build_quest_catalog import COMPARISONS, fragment, play_record
from tutorial_level_triggers import CAPTURED_END_POINTS
from tutorial_spawns import wire_guid


FORMAT_VERSION = 2
MAP_DATA_ROOT = "X5/Content/Data/BuiltData/Maps/"
TRIGGER_FILE = "level_trigger_volume.json"
LAYER_GROUPS = ("Runtime", "Static")
TRAVEL_KINDS = ("channelSession", "channel", "portal")
TUTORIAL_WORLD = "ArcheWorld_Tutorial"
# Same-map dungeon Portal rows: GoBoss (comment "<map>,2") and GoExit (comment "<map>,3").
DUNGEON_JUMP_PORTALS = {11: "GoBoss", 12: "GoExit"}
# Each dungeon's boss and exit doodads use MapMove interactions numbered <prefix>01 (boss room)
# and <prefix>02 (beside the exit), for example 10110101 and 10110102 for map 10111.
DUNGEON_JUMP_SUFFIXES = {"GoBoss": 1, "GoExit": 2}
QUEST_REQUIREMENTS = {
    "EX5RequirementQuestType_Acceptable": "Acceptable",
    "EX5RequirementQuestType_Progressing": "Progressing",
    "EX5RequirementQuestType_QuestComplete": "QuestComplete",
    "EX5RequirementQuestType_QuestProgress": "QuestProgress",
}
PARTY_STATES = {
    "EX5PartyStateType_NoParty": "NoParty",
    "EX5PartyStateType_LeaderOnly": "LeaderOnly",
    "EX5PartyStateType_InviteAccepted": "InviteAccepted",
}
CENTIMETRES_TO_MILLIMETRES = 10

# Official P2 capture: cutscene 1565 ends with a map handoff to this fixed
# endpoint. The client export names the endpoint but does not contain its table.
CLIFFS_GATE_CUT_SCENE = 1565
CLIFFS_CANNON_CUT_SCENE = 301006
CLIFFS_ELITE_OBJECT_CUT_SCENE = 1550
CLIFFS_GATE_MAP = 30104
CLIFFS_GATE_DOODAD = 10100002
CLIFFS_GATE_INTERACTION = 10100002
CLIFFS_GATE_QUEST = 20001051
CLIFFS_GATE_SOURCE_MAP = 9012
CLIFFS_GATE_ENDPOINT_MM = [-311380, 2987010, -622360]
CLIFFS_GATE_ENDPOINT_YAW = -100.0
CLIFFS_GATE_SPOT_PATH = (
    "X5/Content/Data/BuiltData/Maps/ArcheWorld/DataLayers/Runtime/"
    "Zone_01_Public_02/spot_object.json"
)


def world_name(asset_path):
    """Asset basename of a Map.world path such as /Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld."""
    return asset_path.rsplit("/", 1)[-1].split(".")[0]


def layer_names(map_row):
    return [name for name in map_row["runtimeDataLayerPath"].strip("[]").split(",") if name]


def area_of(map_row):
    if not map_row["area"]:
        return None
    area = fragment(map_row["area"])
    return {
        "min": [area["min"]["x"], area["min"]["y"], area["min"]["z"]],
        "max": [area["max"]["x"], area["max"]["y"], area["max"]["z"]],
    }


def translation_of(map_row):
    """Level origin of a map whose own world is streamed into ArcheWorld (dungeons)."""
    if not map_row["worldTransform"]:
        return None
    transform = fragment(map_row["worldTransform"])
    translation = transform["translation"]
    point = [translation["x"], translation["y"], translation["z"]]
    if all(abs(value) < 1e-6 for value in point):
        return None
    return point


def normalize_condition(row, type_case_key, type_key):
    """One condition row in the shape of the quest condition evaluator."""
    kind = row[type_case_key]
    parameters = fragment(row[type_key]) if row[type_key] else {}
    condition = {
        "group": int(row["group"] or 0),
        "operator": row["logicalOperator"] or "AND",
        "negated": row["negated"] == "True",
        "kind": kind,
    }
    if row["checkOnType"] != "Caster":
        condition["kind"] = "ClientChecked"
        condition["source_kind"] = kind
        return condition

    if kind == "Quest":
        requirement = QUEST_REQUIREMENTS.get(parameters.get("questRequirementType"))
        if requirement is None:
            raise ValueError(f"Unknown quest requirement: {parameters}")
        condition.update({"quest_id": int(parameters["questId"]), "requirement": requirement})
        if "progressGroup" in parameters:
            condition["progress_group"] = int(parameters["progressGroup"])
    elif kind == "OwnItem":
        condition.update({"item_id": int(parameters["itemId"]),
                          "count": int(parameters.get("count", 1))})
    elif kind == "AdventureObject":
        condition.update({"adventure_object_id": int(parameters["cmsId"]),
                          "active": bool(parameters.get("active", False))})
    elif kind == "CheckUnitLevel":
        condition.update({"min": int(parameters.get("min", 0)),
                          "max": int(parameters.get("max", 2147483647))})
    elif kind == "PartyState":
        state = PARTY_STATES.get(parameters.get("type"))
        if state is None:
            raise ValueError(f"Unknown party state: {parameters}")
        condition["state"] = state
    elif kind in ("MapEnter", "MapLeave"):
        condition["map_id"] = int(parameters["mapId"])
    else:
        condition["kind"] = "ClientChecked"
        condition["source_kind"] = kind
    return condition


def normalize_play_record(source, condition, row, type_key):
    """PlayRecord rows keep the counted action, like quest conditions do."""
    parameters = fragment(row[type_key])
    record_id = int(parameters["playRecordId"])
    comparison = COMPARISONS.get(parameters.get("comparisonType",
                                                "EX5ComparisonType_GreaterThanOrEqual"))
    if comparison is None:
        raise ValueError(f"Unknown play record comparison: {parameters}")
    condition.update({"kind": "PlayRecord", "play_record_id": record_id,
                      "count": int(parameters.get("count", 0)), "comparison": comparison,
                      "record": play_record(source, record_id)})
    condition.pop("source_kind", None)
    return condition


def conditions_by_id(source, table, type_case_key, type_key):
    grouped = {}
    for row in source.table(table):
        condition = normalize_condition(row, type_case_key, type_key)
        if row[type_case_key] == "PlayRecord" and row["checkOnType"] == "Caster":
            condition = normalize_play_record(source, condition, row, type_key)
        grouped.setdefault(row["conditionId"], []).append(condition)
    return grouped


def build_maps(source, map_conditions):
    maps = []
    for row in source.table("Map"):
        entry = {
            "map_id": int(row["id"]),
            "type": row["type"],
            "world": world_name(row["world"]),
            "parent_map_id": int(row["parentMapId"]) if row["parentMapId"] else 0,
            "origin_map_id": int(row["originId"]) if row["originId"] else 0,
            "channel_max": int(row["chMaxCount"]) if row["chMaxCount"] else 0,
            "retain_on_disconnect": row["retainOnDisconnect"] == "True",
            "entrance_conditions": map_conditions.get(row["FK_enteranceConditionId"], [])
            if row["FK_enteranceConditionId"] else [],
            "exit_conditions": map_conditions.get(row["FK_exitConditionId"], [])
            if row["FK_exitConditionId"] else [],
        }
        if row["FK_enteranceConditionId"] and row["FK_enteranceConditionId"] not in map_conditions:
            raise ValueError(f"Map {row['id']} names a missing entrance condition")
        if row["FK_exitConditionId"] and row["FK_exitConditionId"] not in map_conditions:
            raise ValueError(f"Map {row['id']} names a missing exit condition")
        area = area_of(row)
        if area is not None:
            entry["area_cm"] = area
        translation = translation_of(row)
        if translation is not None:
            entry["translation_cm"] = translation
        maps.append(entry)
    maps.sort(key=lambda item: item["map_id"])
    return maps


def trigger_files(source):
    """(world, group, layer, raw path) of every level trigger file of every world."""
    found = []
    for path in source.raw_files:
        if not path.startswith(MAP_DATA_ROOT) or not path.endswith("/" + TRIGGER_FILE):
            continue
        parts = path[len(MAP_DATA_ROOT):].split("/")
        if "DataLayers" not in parts:
            continue
        index = parts.index("DataLayers")
        if index == 0 or len(parts) != index + 4 or parts[index + 1] not in LAYER_GROUPS:
            raise ValueError(f"Unexpected trigger file location: {path}")
        found.append((parts[index - 1], parts[index + 1], parts[index + 2], path))
    return sorted(found)


def layer_listing(source):
    listing = {}
    for row in source.table("Map"):
        world = world_name(row["world"])
        for layer in layer_names(row):
            listing.setdefault((world, layer), set()).add(int(row["id"]))
    return listing


def geometry_of(volume, context):
    if "box" in volume and "sphere" not in volume:
        box = volume["box"]
        return {"shape": "box",
                "center_cm": [box["center"]["x"], box["center"]["y"], box["center"]["z"]],
                "extent_cm": [box["extent"]["x"], box["extent"]["y"], box["extent"]["z"]],
                "rotation_deg": [box["rotation"]["pitch"], box["rotation"]["yaw"],
                                 box["rotation"]["roll"]]}
    if "sphere" in volume and "box" not in volume:
        sphere = volume["sphere"]
        return {"shape": "sphere",
                "center_cm": [sphere["center"]["x"], sphere["center"]["y"],
                              sphere["center"]["z"]],
                "radius_cm": sphere["radius"]}
    raise ValueError(f"Trigger volume needs exactly one shape: {context}")


def build_tasks(source, task_id, spots):
    tasks = []
    for row in source.table("LevelTriggerTask"):
        if row["id"] != task_id:
            continue
        parameters = fragment(row["task"])
        case = row["taskCase"]
        if case == "PlayCutScene":
            tasks.append({"kind": "PlayCutScene", "cut_scene_id": int(parameters["id"])})
        elif case == "MapMove":
            position = parameters["pos"]
            tasks.append({"kind": "MapMove", "map_id": int(parameters["mapId"]),
                          "position_mm": [round(float(position[axis]) *
                                                CENTIMETRES_TO_MILLIMETRES)
                                          for axis in ("x", "y", "z")]})
        elif case == "ChangeDoodadPhase":
            task = {"kind": "ChangeDoodadPhase", "doodad_key": parameters["doodadKey"],
                    "phase": int(parameters["phaseId"])}
            spot = spots.get(parameters["doodadKey"])
            if spot is not None:
                task["unit_id"] = spot
            tasks.append(task)
        elif case == "ClientPlayDialogue":
            tasks.append({"kind": "ClientPlayDialogue", "dialogue_id": int(parameters["id"])})
        else:
            raise ValueError(f"Unknown level trigger task {task_id}: {case}")
    if not tasks:
        raise ValueError(f"Missing level trigger task: {task_id}")
    return tasks


def build_rules(source, trigger_conditions, spots):
    rules = {}
    for row in source.table("LevelTriggerVolume"):
        if row["triggerType"] not in ("Enter", "Leave"):
            raise ValueError(f"Unknown level trigger type: {row}")
        conditions = []
        if row["FK_conditionId"]:
            if row["FK_conditionId"] not in trigger_conditions:
                raise ValueError(f"Missing level trigger condition {row['FK_conditionId']}")
            conditions = trigger_conditions[row["FK_conditionId"]]
        rules.setdefault(row["id"], []).append({
            "trigger_type": row["triggerType"],
            "conditions": conditions,
            "tasks": build_tasks(source, row["FK_task"], spots),
        })
    return rules


def spot_unit_ids(source):
    """Wire unit id of every placed spot object by its placement key (spot_unit_id layout)."""
    from build_tutorial_data import spot_unit_id

    units = {}
    for path in source.raw_files:
        if not path.startswith(MAP_DATA_ROOT) or not path.endswith("/spot_object.json"):
            continue
        document = json.loads(source.read(Path("raw") / path))
        for sector in document.get("sectors", []):
            for spot in sector["spotObjects"]:
                units[spot["key"]] = spot_unit_id(sector["sectorIndex"], int(spot["id"]))
    return units


def build_volumes(source, rules):
    listing = layer_listing(source)
    volumes = []
    seen = set()
    for world, group, layer, path in trigger_files(source):
        document = json.loads(source.read(Path("raw") / path))
        for volume in document["triggerVolumes"]:
            guid = volume["id"]
            context = f"{guid} in {world}/{group}/{layer}"
            kinds = [kind for kind in TRAVEL_KINDS if kind in volume]
            if len(kinds) > 1:
                raise ValueError(f"Trigger volume has several destination blocks: {context}")
            kind = kinds[0] if kinds else None
            if kind is None and guid not in rules:
                continue
            if guid in seen:
                raise ValueError(f"Duplicate trigger GUID {context}")
            seen.add(guid)

            entry = {
                "trigger_id_wire": wire_guid(guid),
                "trigger_guid": guid,
                "world": world,
                "layer": layer,
                "maps": sorted(listing.get((world, layer), ())) if group == "Runtime" else [],
                "active": bool(volume["isActivate"]),
                "kind": kind or "rules",
                **geometry_of(volume, context),
            }
            if kind in ("channelSession", "channel"):
                entry["destination_map_id"] = int(volume[kind]["mapId"])
            if kind == "channelSession":
                entry["spawn_position_id"] = int(volume[kind]["spawnPositionId"])
            if kind == "portal":
                entry["portal_id"] = int(volume[kind]["cmsId"])
            if guid in rules:
                entry["rules"] = rules[guid]
            volumes.append(entry)
    volumes.sort(key=lambda item: (item["world"], item["layer"], item["trigger_guid"]))
    return volumes


def build_portals(source, arrivals):
    portals = []
    for row in source.table("Portal"):
        portal = {"portal_id": int(row["id"]), "from_map_id": int(row["fromMapId"]),
                  "to_map_id": int(row["toMapId"]), "spawn_id": int(row["spawnId"])}
        arrival = arrivals.get((portal["portal_id"], portal["from_map_id"]))
        if arrival is not None:
            portal.update(arrival)
        portals.append(portal)
    portals.sort(key=lambda item: (item["portal_id"], item["from_map_id"]))
    return portals


def map_move_interactions(source):
    """(interaction id, position in millimetres) of every MapMove interaction by its map."""
    moves = {}
    for row in source.table("Interaction"):
        if row["playerTaskCase"] != "MapMove":
            continue
        task = fragment(row["playerTask"])
        position = [round(float(task["pos"][axis]) * CENTIMETRES_TO_MILLIMETRES)
                    for axis in ("x", "y", "z")]
        moves.setdefault(int(task["mapId"]), []).append((int(row["id"]), position))
    return moves


def dungeon_exit_centres(portal_rows, volumes):
    """Centres (cm) of the portal volumes that leave each dungeon for another map."""
    leaving = {(row["portal_id"], row["from_map_id"]) for row in portal_rows
               if row["to_map_id"] != row["from_map_id"]}
    centres = {}
    for volume in volumes:
        if volume["kind"] != "portal":
            continue
        for map_id in volume["maps"]:
            if (volume["portal_id"], map_id) in leaving:
                centres.setdefault(map_id, []).append(volume["center_cm"])
    return centres


def planar_distance_mm(position_mm, centre_cm):
    dx = position_mm[0] - centre_cm[0] * CENTIMETRES_TO_MILLIMETRES
    dy = position_mm[1] - centre_cm[1] * CENTIMETRES_TO_MILLIMETRES
    return (dx * dx + dy * dy) ** 0.5


def dungeon_jump_arrivals(source, volumes):
    """Arrival of each same-map GoBoss/GoExit Portal row, keyed by (portal id, map id).

    The Portal rows name spawn ids (11, 12) of a server table the export lacks. The same
    dungeon's boss and exit doodads move the character with authored MapMove interactions
    numbered <prefix>01 and <prefix>02. A row is resolved only when exactly one prefix offers
    both interactions for the dungeon and the exit interaction lies nearer to the dungeon's exit
    portal volume than the boss one; otherwise it stays unresolved and the runtime refuses it.
    """
    portal_rows = [{"portal_id": int(row["id"]), "from_map_id": int(row["fromMapId"]),
                    "to_map_id": int(row["toMapId"]), "comment": row["comment"]}
                   for row in source.table("Portal")]
    moves = map_move_interactions(source)
    exits = dungeon_exit_centres(portal_rows, volumes)
    arrivals = {}
    for row in portal_rows:
        role = DUNGEON_JUMP_PORTALS.get(row["portal_id"])
        map_id = row["from_map_id"]
        if role is None or row["to_map_id"] != map_id or role not in row["comment"]:
            continue

        by_prefix = {}
        for interaction_id, position in moves.get(map_id, []):
            suffix = interaction_id % 100
            if suffix in DUNGEON_JUMP_SUFFIXES.values():
                by_prefix.setdefault(interaction_id // 100, {})[suffix] = (interaction_id,
                                                                           position)
        pairs = [pair for pair in by_prefix.values() if len(pair) == 2]
        if len(pairs) != 1 or map_id not in exits:
            continue

        pair = pairs[0]
        boss = pair[DUNGEON_JUMP_SUFFIXES["GoBoss"]]
        exit_ = pair[DUNGEON_JUMP_SUFFIXES["GoExit"]]

        def nearest_exit(position):
            return min(planar_distance_mm(position, centre) for centre in exits[map_id])

        if nearest_exit(exit_[1]) >= nearest_exit(boss[1]):
            continue
        interaction_id, position = pair[DUNGEON_JUMP_SUFFIXES[role]]
        arrivals[(row["portal_id"], map_id)] = {"arrival_mm": position,
                                                "arrival_interaction_id": interaction_id}
    return arrivals


def build_map_move_cut_scenes(source, cut_scene_conditions):
    rows = []
    for row in source.table("MapMoveCutScene"):
        condition_id = row["FK_mapMoveCutSceneConditionId"]
        if condition_id and condition_id not in cut_scene_conditions:
            raise ValueError(f"Missing map move cutscene condition {condition_id}")
        rows.append({
            "from_map_id": int(row["from"]),
            "to_map_id": int(row["to"]),
            "timing": row["timingType"],
            "cut_scene_id": int(row["FK_cutsceneId"]),
            "conditions": cut_scene_conditions.get(condition_id, []) if condition_id else [],
        })
    return rows


def doodad_placements(source):
    """(world, layer group, layer, position cm) of every placed doodad by its doodad id."""
    placements = {}
    for path in source.raw_files:
        if not path.startswith(MAP_DATA_ROOT) or not path.endswith("/spot_object.json"):
            continue
        parts = path[len(MAP_DATA_ROOT):].split("/")
        if "DataLayers" not in parts:
            continue
        index = parts.index("DataLayers")
        world, group, layer = parts[index - 1], parts[index + 1], parts[index + 2]
        document = json.loads(source.read(Path("raw") / path))
        for sector in document.get("sectors", []):
            for spot in sector["spotObjects"]:
                for kind in spot["types"]:
                    if "doodad" in kind:
                        placements.setdefault(int(kind["doodad"]["cmsId"]), []).append(
                            (world, group, layer, spot["position"]))
    return placements


def end_point_of(row, maps_by_id, placements):
    """The end point of a CutScene row, or None when it names none.

    Position sources, strongest first: the official capture; the single doodad whose id equals
    the end point id placed on the end map (its world and layers, inside its authored area);
    otherwise none, and the runtime applies the travel arrival rule (CUSTOM).
    """
    if not row["fk_EndPointMapId"]:
        return None
    map_id = int(row["fk_EndPointMapId"])
    end_point_id = int(row["endPointId"]) if row["endPointId"] else 0
    end_point = {"map_id": map_id, "end_point_id": end_point_id}

    captured = CAPTURED_END_POINTS.get((map_id, end_point_id))
    if (map_id, end_point_id) == (CLIFFS_GATE_MAP, CLIFFS_GATE_CUT_SCENE):
        captured = {"position": CLIFFS_GATE_ENDPOINT_MM, "yaw": CLIFFS_GATE_ENDPOINT_YAW}
    if captured is not None:
        end_point.update({"position_mm": captured["position"], "yaw": captured["yaw"],
                          "source": "captured"})
        return end_point

    map_row = maps_by_id.get(str(map_id))
    if map_row is None:
        raise ValueError(f"Cutscene {row['id']} ends on unknown map {map_id}")
    world = world_name(map_row["world"])
    layers = set(layer_names(map_row))
    area = area_of(map_row)
    candidates = []
    for placed_world, group, layer, position in placements.get(end_point_id, []):
        if placed_world != world or (group != "Static" and layer not in layers):
            continue
        if area is not None and not (area["min"][0] <= position["x"] <= area["max"][0]
                                     and area["min"][1] <= position["y"] <= area["max"][1]):
            continue
        candidates.append(position)
    if len(candidates) == 1:
        position = candidates[0]
        end_point.update({
            "position_mm": [round(position[axis] * CENTIMETRES_TO_MILLIMETRES)
                            for axis in ("x", "y", "z")],
            "source": "doodad",
        })
        return end_point

    end_point["source"] = "arrival_rule"
    return end_point


def build_cut_scenes(source, placements):
    """Every CutScene row: the server plays any of them for doodads, volumes and map moves."""
    maps_by_id = {row["id"]: row for row in source.table("Map")}
    cut_scenes = []
    for row in source.table("CutScene"):
        entry = {
            "cut_scene_id": int(row["id"]),
            "invisible": row["invisible"] == "True",
            "client_only": row["isClientCutscene"] == "True",
        }
        end_point = end_point_of(row, maps_by_id, placements)
        if end_point is not None:
            entry["end_point"] = end_point
        cut_scenes.append(entry)
    cut_scenes.sort(key=lambda item: item["cut_scene_id"])
    for wanted in (CLIFFS_GATE_CUT_SCENE, CLIFFS_CANNON_CUT_SCENE, CLIFFS_ELITE_OBJECT_CUT_SCENE):
        if not any(entry["cut_scene_id"] == wanted for entry in cut_scenes):
            raise ValueError(f"Captured Cliffs cutscene {wanted} is missing")
    return cut_scenes


def tutorial_unit_ids(source):
    """Wire unit ids of the doodads placed in the tutorial world."""
    from build_tutorial_data import spot_unit_id

    units = set()
    prefix = MAP_DATA_ROOT + TUTORIAL_WORLD + "/"
    for path in source.raw_files:
        if not path.startswith(prefix) or not path.endswith("/spot_object.json"):
            continue
        document = json.loads(source.read(Path("raw") / path))
        for sector in document.get("sectors", []):
            for spot in sector["spotObjects"]:
                units.add(spot_unit_id(sector["sectorIndex"], int(spot["id"])))
    return units


def check_saved_doodad_keys(volumes, tutorial_units):
    """Shared-world volume doodads are saved in WorldProgress.doodad_phases by wire unit id, the
    key the tutorial uses for its own doodads; the two must never name the same unit."""
    for volume in volumes:
        if volume["world"] == TUTORIAL_WORLD:
            continue
        for rule in volume.get("rules", []):
            for task in rule["tasks"]:
                if task["kind"] == "ChangeDoodadPhase" and task.get("unit_id") in tutorial_units:
                    raise ValueError(f"Volume doodad {task['unit_id']} shares a tutorial unit id")


def build_cliffs_gate(source):
    """Join the captured Cliffs entrance to its verified client placement and tables."""
    from build_tutorial_data import spot_unit_id

    cut_scene = source.row("CutScene", CLIFFS_GATE_CUT_SCENE)
    if int(cut_scene["fk_EndPointMapId"]) != CLIFFS_GATE_MAP:
        raise ValueError("Cliffs entrance cutscene no longer names the captured destination")

    interaction = source.row("Interaction", CLIFFS_GATE_INTERACTION)
    if (interaction["playerTaskCase"] != "Use"
            or interaction["FK_conditionId"] != str(CLIFFS_GATE_INTERACTION)):
        raise ValueError("Cliffs entrance no longer uses the captured interaction")

    conditions = [row for row in source.table("InteractionCondition")
                  if row["conditionId"] == interaction["FK_conditionId"]]
    required_quest_groups = {
        row["group"] for row in conditions
        if row["typeCase"] == "Quest"
        and fragment(row["type"]).get("questId") == CLIFFS_GATE_QUEST
        and fragment(row["type"]).get("questRequirementType")
        == "EX5RequirementQuestType_QuestComplete"
    }
    if required_quest_groups != {"1", "3"}:
        raise ValueError("Cliffs entrance quest conditions changed")

    phases = [row for row in source.table("DoodadPhase")
              if row["FK_doodad"] == str(CLIFFS_GATE_DOODAD)]
    if not phases or any(row["FK_cutScene"] != str(CLIFFS_GATE_CUT_SCENE) for row in phases):
        raise ValueError("Cliffs entrance phases no longer name the captured cutscene")

    document = json.loads(source.read(Path("raw") / CLIFFS_GATE_SPOT_PATH))
    matches = []
    for sector in document["sectors"]:
        for spot in sector["spotObjects"]:
            if any(kind.get("doodad", {}).get("cmsId") == CLIFFS_GATE_DOODAD
                   for kind in spot["types"]):
                matches.append((sector, spot))
    if len(matches) != 1:
        raise ValueError("Expected one Cliffs entrance placement")

    sector, spot = matches[0]
    position = spot["position"]
    return {
        "unit_id": spot_unit_id(sector["sectorIndex"], int(spot["id"])),
        "interaction_id": CLIFFS_GATE_INTERACTION,
        "source_map_id": CLIFFS_GATE_SOURCE_MAP,
        "position_mm": [round(position[axis] * CENTIMETRES_TO_MILLIMETRES)
                        for axis in ("x", "y", "z")],
        "required_quest_id": CLIFFS_GATE_QUEST,
        "cut_scene_id": CLIFFS_GATE_CUT_SCENE,
        "destination_map_id": CLIFFS_GATE_MAP,
        "destination_mm": CLIFFS_GATE_ENDPOINT_MM,
        "destination_yaw": CLIFFS_GATE_ENDPOINT_YAW,
    }


def build_travel(source):
    map_conditions = conditions_by_id(source, "MapCondition", "conditionTypeCase",
                                      "conditionType")
    trigger_conditions = conditions_by_id(source, "LevelTriggerCondition", "conditionTypeCase",
                                          "conditionType")
    cut_scene_conditions = conditions_by_id(source, "MapMoveCutSceneCondition", "typeCase",
                                            "type")
    rules = build_rules(source, trigger_conditions, spot_unit_ids(source))
    volumes = build_volumes(source, rules)
    check_saved_doodad_keys(volumes, tutorial_unit_ids(source))
    map_move_cut_scenes = build_map_move_cut_scenes(source, cut_scene_conditions)
    return {
        "format_version": FORMAT_VERSION,
        "maps": build_maps(source, map_conditions),
        "volumes": volumes,
        "portals": build_portals(source, dungeon_jump_arrivals(source, volumes)),
        "map_move_cut_scenes": map_move_cut_scenes,
        "cut_scenes": build_cut_scenes(source, doodad_placements(source)),
        "cliffs_gate": build_cliffs_gate(source),
    }


def summarize(document):
    kinds = {}
    for volume in document["volumes"]:
        kinds[volume["kind"]] = kinds.get(volume["kind"], 0) + 1
    gated = sum(1 for entry in document["maps"]
                if entry["entrance_conditions"] or entry["exit_conditions"])
    end_points = {}
    for scene in document["cut_scenes"]:
        if "end_point" in scene:
            source = scene["end_point"]["source"]
            end_points[source] = end_points.get(source, 0) + 1
    return {"maps": len(document["maps"]), "gated_maps": gated,
            "volumes": dict(sorted(kinds.items())), "portals": len(document["portals"]),
            "dungeon_jump_arrivals": sum(1 for portal in document["portals"]
                                         if "arrival_mm" in portal),
            "map_move_cut_scenes": len(document["map_move_cut_scenes"]),
            "cut_scenes": len(document["cut_scenes"]),
            "cut_scene_end_points": dict(sorted(end_points.items()))}


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/travel/definitions.json"))
    options = parser.parse_args(arguments)

    from build_tutorial_data import Source

    source = Source(options.source)
    document = build_travel(source)
    document["source_sha256"] = dict(sorted(source.sources.items()))
    data = (json.dumps(document, indent=1) + "\n").encode("utf-8")
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_bytes(data)
    print(json.dumps({"output": options.output.as_posix(),
                      "sha256": hashlib.sha256(data).hexdigest(), **summarize(document)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
