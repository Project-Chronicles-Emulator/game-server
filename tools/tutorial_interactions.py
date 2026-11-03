"""Extract tutorial door/lever states and their authored animation events.

The lever-to-gate relationship is a build-specific inference from the official
trace, not a link present in the client placement export. Resolve it by static
placement GUID and verify the expected CMS IDs before using it.
"""

import json
from pathlib import Path

from tutorial_quests import fragment
from tutorial_spawns import MAP_ROOT


LEVER_GUID = "13ef9dc7-4ef5-b51f-1db7-2fb61e66ace2"
GATE_GUID = "9d85d5cd-49e2-b136-4cab-7cbbdefff448"


def animation_timing(source, animation_id, allow_rig_variation=False):
    """Return the authored execute event and end time of one interaction animation.

    Character rigs normally share one timing and any difference fails closed. With
    ``allow_rig_variation`` the rigs must still share the animation end, and the earliest
    execute event is returned: the server cannot rely on the client's rig, so it accepts
    the earliest legitimate execution and later rigs simply execute after that deadline.
    """
    animation = source.row("InteractionAnim", animation_id)
    details = [row for row in source.table("InteractionAnimDetail")
               if row["id"] == animation["animId"]]
    timings = set()
    for detail in details:
        path = Path("raw/X5/Content/Data/BuiltData/Anim/Interaction") / (
            detail["gameAnimKey"] + ".json"
        )
        document = json.loads(source.read(path))
        events = [event for event in document["commonActStage"]["notifies"]
                  if event["notifyType"] == "interaction_execute"]
        if len(events) != 1:
            raise ValueError(f"Unsupported interaction event sequence: {animation_id}")
        execute_ms = events[0]["startTime"]
        duration_ms = document["endTime"]
        if not 0 < execute_ms < duration_ms <= 30000:
            raise ValueError(f"Invalid interaction timing: {animation_id}")
        timings.add((execute_ms, duration_ms))

    if not timings:
        raise ValueError(f"Interaction animation has no character rigs: {animation_id}")

    durations = {duration_ms for _, duration_ms in timings}
    if len(timings) != 1 and (not allow_rig_variation or len(durations) != 1):
        raise ValueError(f"Interaction timing differs across character rigs: {animation_id}")

    execute_ms = min(execute_ms for execute_ms, _ in timings)
    duration_ms = durations.pop()
    return {"execute_time_ms": execute_ms, "duration_ms": duration_ms}


def build_doodads(source, spots, quests, unit_identifier):
    placements = {}
    for sector in spots["sectors"]:
        for spot in sector["spotObjects"]:
            for kind in spot["types"]:
                cms_id = kind.get("doodad", {}).get("cmsId")
                if cms_id not in (49013, 49014, 49020):
                    continue
                placements[spot["key"]] = {
                    "unit_id": unit_identifier(sector["sectorIndex"], spot["id"]),
                    "sector_id": sector["sectorIndex"]["x"] | (sector["sectorIndex"]["y"] << 14),
                    "local_id": spot["id"],
                    "cms_id": cms_id,
                    "placement_guid": spot["key"],
                    "position": [round(spot["position"][axis] * 10) for axis in ("x", "y", "z")],
                    "yaw": spot["rotation"]["yaw"],
                    "initial_phase": 1,
                    "consumes": [],
                }

    for doodad in placements.values():
        cms_id = doodad["cms_id"]
        phases = [row for row in source.table("DoodadPhase")
                  if row["FK_doodad"] == str(cms_id)]
        if not phases or phases[0]["phaseId"] != "1":
            raise ValueError(f"Missing initial tutorial doodad phase: {cms_id}")
        doodad["phases"] = sorted(int(row["phaseId"]) for row in phases)
        if cms_id == 49014:
            continue

        flow = next(row for row in source.table("DoodadPhaseFlow")
                    if row["FK_doodad"] == str(cms_id)
                    and row["triggerTypeCase"] == "Interaction")
        interaction_id = fragment(flow["triggerType"])["id"]
        interaction = source.row("Interaction", interaction_id)
        if interaction["playerTaskCase"] != "Use":
            raise ValueError("Unsupported tutorial doodad player task")
        for condition in source.table("InteractionCondition"):
            if condition["conditionId"] != interaction["FK_conditionId"]:
                continue
            if condition["typeCase"] == "ConsumeItem":
                parameters = fragment(condition["type"])
                doodad["consumes"].append({
                    "item_cms_id": parameters["itemId"],
                    "quantity": parameters["count"],
                })
            elif condition["typeCase"] != "Quest":
                raise ValueError("Unsupported tutorial doodad condition")
        objective = next((quest["cms_id"], progress["sequence"])
                         for quest in quests for progress in quest["progresses"]
                         if progress["kind"] == "InteractDoodad"
                         and progress["parameters"]["doodadId"] == cms_id)
        animation_id = int(interaction["FK_interactionAnimId"])
        doodad.update({
            "interaction_id": interaction_id,
            "animation_id": animation_id,
            "interaction_phase": int(flow["phaseId"]),
            "completed_phase": int(flow["nextPhaseId"]),
            "quest_id": objective[0],
            "quest_sequence": objective[1],
            **animation_timing(source, animation_id),
        })
        if cms_id == 49013:
            # Trace: the door unlocks after the Prowler objective, before the
            # key reward is claimed. Opening still consumes the actual key.
            doodad["phase_prerequisite"] = {"quest_id": 10000106, "sequence": 1}

    lever = placements.get(LEVER_GUID)
    gate = placements.get(GATE_GUID)
    if not lever or lever["cms_id"] != 49020 or not gate or gate["cms_id"] != 49014:
        raise ValueError("Tutorial lever/gate placement anchors no longer match")
    if 2 not in gate["phases"]:
        raise ValueError("Tutorial linked gate has no open phase")
    lever["phase_updates"] = [{"unit_id": gate["unit_id"], "phase": 2}]
    return sorted(placements.values(), key=lambda value: int(value["unit_id"]))


def build_followup_interactions(source, quests, unit_identifier):
    """Only map 10117's authored layers belong to the second instance."""
    row = source.row("Map", 10117)
    world = row["world"].rsplit("/", 1)[1].split(".")[0]
    doodads = []
    objects = []
    sectors = set()
    for layer in row["runtimeDataLayerPath"].strip("[]").split(","):
        path = MAP_ROOT / world / "DataLayers/Runtime" / layer / "spot_object.json"
        document = json.loads(source.read(Path("raw") / path))
        for sector in document["sectors"]:
            sector_id = sector["sectorIndex"]["x"] | (sector["sectorIndex"]["y"] << 14)
            sectors.add(sector_id)
            for spot in sector["spotObjects"]:
                placement = {
                    "unit_id": unit_identifier(sector["sectorIndex"], spot["id"]),
                    "sector_id": sector_id,
                    "local_id": spot["id"],
                    "position": [round(spot["position"][axis] * 10) for axis in ("x", "y", "z")],
                    "yaw": spot["rotation"]["yaw"],
                }
                for kind in spot["types"]:
                    if "adventureObject" in kind:
                        cms_id = kind["adventureObject"]["cmsId"]
                        definition = source.row("AdventureObject", cms_id)
                        if definition["type"] != "RecordStone":
                            raise ValueError("Unsupported followup adventure object")
                        expertise = source.row(
                            "ExpertiseReward", definition["FK_expertiseRewardId"]
                        )
                        if expertise["typeCase"] != "Exp":
                            raise ValueError("Unsupported followup adventure reward")
                        interaction = source.row("Interaction", definition["interactionGroupId"])
                        if interaction["FK_conditionId"] or interaction["playerTaskCase"] != "Use":
                            raise ValueError("Unsupported followup adventure interaction")
                        animation_id = int(interaction["FK_interactionAnimId"])
                        objects.append({
                            **placement,
                            "cms_id": cms_id,
                            "interaction_id": int(interaction["id"]),
                            "animation_id": animation_id,
                            "experience": fragment(expertise["type"])["value"],
                            "rewards": [],
                            **animation_timing(source, animation_id),
                        })
                    elif kind.get("doodad", {}).get("cmsId") == 10000048:
                        cms_id = 10000048
                        flows = [flow for flow in source.table("DoodadPhaseFlow")
                                 if flow["FK_doodad"] == str(cms_id)]
                        action = next(flow for flow in flows
                                      if flow["triggerTypeCase"] == "Interaction")
                        reset = next(flow for flow in flows if flow["triggerTypeCase"] == "Timer")
                        phase = next(phase for phase in source.table("DoodadPhase")
                                     if phase["FK_doodad"] == str(cms_id)
                                     and phase["phaseId"] == action["nextPhaseId"])
                        objective = next((quest["cms_id"], progress["sequence"])
                                         for quest in quests for progress in quest["progresses"]
                                         if progress["kind"] == "InteractDoodad"
                                         and progress["parameters"]["doodadId"] == cms_id)
                        doodads.append({
                            **placement,
                            "cms_id": cms_id,
                            "initial_phase": int(action["phaseId"]),
                            "interaction_phase": int(action["phaseId"]),
                            "completed_phase": int(action["nextPhaseId"]),
                            "interaction_id": fragment(action["triggerType"])["id"],
                            "animation_id": 0,
                            "quest_id": objective[0],
                            "quest_sequence": objective[1],
                            "cut_scene_id": int(phase["FK_cutScene"]),
                            "reset_time_ms": fragment(reset["triggerType"])["duration"],
                            "consumes": [],
                        })
    if len(doodads) != 1 or {entry["cms_id"] for entry in objects} != {2010032, 2010033}:
        raise ValueError("Followup interaction placements no longer match the verified build")
    return {"doodads": doodads, "objects": objects, "sectors": sorted(sectors)}
