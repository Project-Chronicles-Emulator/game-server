"""Recover level-trigger tasks and cutscene follow-ups for the tutorial instances.

Level triggers come from the client's LevelTriggerVolume, LevelTriggerTask and
LevelTriggerCondition tables, joined with the volume geometry placed in the
instance's own map layers. Cutscene metadata comes from CutScene, CutSceneSequence
and CutSceneBind.

The client tables do not contain every server-side consequence. Where the official
2026-10-24 tutorial capture shows a fixed, non-personal world effect that the
tables omit (a post-cutscene teleport destination, a cutscene end-point spawn and
the boss defeat script), it is recorded below as captured official evidence and
kept separate from table-derived fields with an explicit ``evidence`` label.
"""

import json
import re

from tutorial_quests import fragment, map_trigger_volumes, trigger_definition
from tutorial_spawns import wire_guid


# Captured official behavior (build 40367e24): after PC2S_CutSceneEndReq for cutscene
# 1523 the server sent PS2C_TeleportReq to this fixed arena position. CutScene 1523
# has no end point in the client tables; the destination is a static world coordinate.
CAPTURED_CUT_SCENE_END_TELEPORTS = {
    1523: {"position": [-1393646, 706393, -698790], "yaw": 125.0},
}

# Captured official behavior: PS2C_MapMoveStartNoti spawn points used after the
# cutscenes whose CutScene row names (fk_EndPointMapId, endPointId). The client
# export does not contain the server's end-point table.
CAPTURED_END_POINTS = {
    (10117, 9999993): {"position": [-1552556, 3078387, -628450], "yaw": 80.0},
    (9011, 10000046): {"position": [-1568074, 3083414, -628027], "yaw": 6.11788845},
}

# Captured official behavior: when the Herald Knight died the server sent
# BossKilledBannerNoti, activated the deactivated arena doodad 10100017 (phase 1),
# and played cutscene 1521. No client table links these effects to the NPC.
CAPTURED_BOSS_DEFEATS = {
    5001527: {
        "cut_scene_id": 1521,
        "activate_doodad_guid": "09911850-4040-fc7c-967e-ecbdcc30d0c4",
        "activate_doodad_cms_id": 10100017,
        "activate_phase": 1,
    },
}

# Quest requirement types understood by the runtime condition evaluator.
QUEST_REQUIREMENTS = {
    "EX5RequirementQuestType_Acceptable": "Acceptable",
    "EX5RequirementQuestType_Progressing": "Progressing",
    "EX5RequirementQuestType_QuestComplete": "QuestComplete",
    "EX5RequirementQuestType_QuestProgress": "QuestProgress",
}

SERVER_NPC_BINDING = re.compile(r"(\w+):(\{[^{}]*\})")


def trigger_conditions(source, condition_id, known_quest_ids):
    """Return (conditions, supported). Unsupported condition kinds fail closed at runtime.

    A quest requirement is only evaluable when the runtime owns that quest's definition;
    "Acceptable" in particular depends on the quest's own acceptance rules.
    """
    if not condition_id:
        return [], True

    conditions = []
    supported = True
    rows = [row for row in source.table("LevelTriggerCondition")
            if row["conditionId"] == condition_id]
    if not rows:
        raise ValueError(f"Missing level trigger condition: {condition_id}")

    for row in rows:
        if row["checkOnType"] != "Caster" or row["logicalOperator"] not in ("AND", "OR"):
            supported = False
            continue
        parameters = fragment(row["conditionType"])
        requirement = QUEST_REQUIREMENTS.get(parameters.get("questRequirementType"))
        if row["conditionTypeCase"] != "Quest" or requirement is None:
            supported = False
            continue
        if int(parameters["questId"]) not in known_quest_ids:
            supported = False
            continue
        condition = {
            "group": int(row["group"]),
            "operator": row["logicalOperator"],
            "negated": row["negated"] == "True",
            "quest_id": int(parameters["questId"]),
            "requirement": requirement,
        }
        if "progressGroup" in parameters:
            condition["progress_group"] = int(parameters["progressGroup"])
        conditions.append(condition)
    return conditions, supported


def trigger_tasks(source, task_id, doodads_by_guid):
    """Return (tasks, supported) for one LevelTriggerTask identifier."""
    rows = [row for row in source.table("LevelTriggerTask") if row["id"] == task_id]
    if not rows:
        raise ValueError(f"Missing level trigger task: {task_id}")

    tasks = []
    supported = True
    for row in rows:
        parameters = fragment(row["task"])
        if row["taskCase"] == "PlayCutScene":
            tasks.append({"kind": "PlayCutScene", "cut_scene_id": int(parameters["id"])})
        elif row["taskCase"] == "ChangeDoodadPhase":
            doodad = doodads_by_guid.get(parameters["doodadKey"])
            if doodad is None:
                supported = False
                continue
            tasks.append({
                "kind": "ChangeDoodadPhase",
                "unit_id": doodad["unit_id"],
                "phase": int(parameters["phaseId"]),
            })
        else:
            # ClientPlayDialogue and MapMove have no captured server response yet.
            supported = False
    return tasks, supported


def build_level_triggers(source, map_id, doodads, known_quest_ids):
    """Level triggers whose volume is placed in this instance's map layers.

    One volume can carry several LevelTriggerVolume rows, each with its own task and
    condition set (for example a different cutscene per quest state). They are kept as
    ordered rules of a single trigger.
    """
    volumes = map_trigger_volumes(source, map_id)
    doodads_by_guid = {doodad["placement_guid"]: doodad for doodad in doodads
                       if "placement_guid" in doodad}
    triggers = {}
    for row in source.table("LevelTriggerVolume"):
        if row["id"] not in volumes:
            continue
        if row["triggerType"] not in ("Enter", "Leave"):
            raise ValueError(f"Unsupported level trigger type: {row['id']}")

        trigger = triggers.get(row["id"])
        if trigger is None:
            geometry = trigger_definition(volumes, {"triggerVolumeId": row["id"], "mapLayer": 0})
            trigger = {**geometry, "volume_id": row["id"], "trigger_type": row["triggerType"],
                       "rules": []}
            triggers[row["id"]] = trigger
        elif trigger["trigger_type"] != row["triggerType"]:
            raise ValueError(f"Level trigger mixes enter and leave rules: {row['id']}")

        tasks, tasks_supported = trigger_tasks(source, row["FK_task"], doodads_by_guid)
        conditions, conditions_supported = trigger_conditions(
            source, row["FK_conditionId"], known_quest_ids
        )
        trigger["rules"].append({
            "tasks": tasks,
            "conditions": conditions,
            "supported": tasks_supported and conditions_supported,
        })
    return [triggers[volume_id] for volume_id in sorted(triggers)]


def server_npc_bindings(source, cut_scene_id, npcs):
    """Return (bindings, supported); bound NPCs receive the binding's AI phase at the end."""
    bind_ids = {row["fk_BindId"] for row in source.table("CutSceneSequence")
                if row["id"] == str(cut_scene_id) and row["fk_BindId"]}
    npcs_by_spawner = {tuple(npc["spawner_guid"]): npc for npc in npcs
                       if "spawner_guid" in npc}
    bindings = []
    supported = True
    for bind_id in sorted(bind_ids):
        text = source.row("CutSceneBind", bind_id)["serverNpcBindingTags"]
        for tag, body in SERVER_NPC_BINDING.findall(text):
            binding = json.loads(body)
            npc = npcs_by_spawner.get(tuple(wire_guid(binding["spawnerId"])))
            if npc is None or npc["cms_id"] != binding["cmsId"]:
                supported = False
                continue
            bindings.append({
                "tag": tag,
                "unit_id": npc["unit_id"],
                "ai_phase": int(binding["targetAiCmsId"]),
            })
    return bindings, supported


def build_cut_scenes(source, cut_scene_ids, npcs):
    cut_scenes = []
    for cut_scene_id in sorted(cut_scene_ids):
        row = source.row("CutScene", cut_scene_id)
        bindings, bindings_supported = server_npc_bindings(source, cut_scene_id, npcs)
        definition = {
            "cut_scene_id": cut_scene_id,
            "invisible": row["invisible"] == "True",
            "npc_bindings": bindings,
            # Client-only cutscenes never receive a server CutScenePlayNoti.
            "supported": bindings_supported and row["isClientCutscene"] != "True",
        }
        teleport = CAPTURED_CUT_SCENE_END_TELEPORTS.get(cut_scene_id)
        if teleport is not None:
            definition["end_teleport"] = {**teleport, "evidence": "captured_official"}
        if row["fk_EndPointMapId"]:
            map_id = int(row["fk_EndPointMapId"])
            end_point_id = int(row["endPointId"])
            end_point = CAPTURED_END_POINTS.get((map_id, end_point_id))
            if end_point is None:
                # The server's end-point table is not in the client export. Without a
                # recovered spawn the runtime must refuse to start this cutscene.
                definition["supported"] = False
                definition["end_map_move"] = {"map_id": map_id, "end_point_id": end_point_id}
            else:
                definition["end_map_move"] = {
                    "map_id": map_id,
                    "end_point_id": end_point_id,
                    **end_point,
                    "evidence": "captured_official",
                }
        cut_scenes.append(definition)
    return cut_scenes


def mark_unplayable_triggers(level_triggers, cut_scenes):
    """A rule is unsupported when any cutscene it plays cannot be completed."""
    supported_cut_scenes = {cut_scene["cut_scene_id"] for cut_scene in cut_scenes
                            if cut_scene["supported"]}
    for trigger in level_triggers:
        for rule in trigger["rules"]:
            for task in rule["tasks"]:
                if (task["kind"] == "PlayCutScene"
                        and task["cut_scene_id"] not in supported_cut_scenes):
                    rule["supported"] = False


def build_boss_defeats(spots_by_guid, unit_identifier):
    """Captured boss-defeat scripts, resolved against static spot placements."""
    defeats = []
    for npc_cms_id, script in sorted(CAPTURED_BOSS_DEFEATS.items()):
        placement = spots_by_guid.get(script["activate_doodad_guid"])
        if placement is None or placement["cms_id"] != script["activate_doodad_cms_id"]:
            raise ValueError(f"Boss defeat doodad anchor no longer matches: {npc_cms_id}")
        defeats.append({
            "npc_cms_id": npc_cms_id,
            "cut_scene_id": script["cut_scene_id"],
            "doodad_activations": [{
                "unit_id": unit_identifier(placement["sector"], placement["local_id"]),
                "phase": script["activate_phase"],
            }],
            "evidence": "captured_official",
        })
    return defeats


def deactivated_doodads(source, spots, unit_identifier, wanted_guids):
    """Placed doodads that start deactivated and appear only once a script activates them."""
    doodads = []
    for sector in spots["sectors"]:
        sector_id = sector["sectorIndex"]["x"] | (sector["sectorIndex"]["y"] << 14)
        for spot in sector["spotObjects"]:
            if spot["key"] not in wanted_guids:
                continue
            for kind in spot["types"]:
                doodad = kind.get("doodad")
                if doodad is None or not doodad.get("deactivation"):
                    continue
                phases = sorted(int(row["phaseId"]) for row in source.table("DoodadPhase")
                                if row["FK_doodad"] == str(doodad["cmsId"]))
                if not phases:
                    raise ValueError(f"Deactivated doodad has no phases: {doodad['cmsId']}")
                doodads.append({
                    "unit_id": unit_identifier(sector["sectorIndex"], spot["id"]),
                    "sector_id": sector_id,
                    "local_id": spot["id"],
                    "cms_id": doodad["cmsId"],
                    "placement_guid": spot["key"],
                    "position": [round(spot["position"][axis] * 10) for axis in ("x", "y", "z")],
                    "yaw": spot["rotation"]["yaw"],
                    "initial_phase": phases[0],
                    "initially_deactivated": True,
                    "phases": phases,
                    "consumes": [],
                })
    return doodads


def spot_placements(spots):
    placements = {}
    for sector in spots["sectors"]:
        for spot in sector["spotObjects"]:
            for kind in spot["types"]:
                doodad = kind.get("doodad")
                if doodad is None:
                    continue
                placements[spot["key"]] = {
                    "sector": sector["sectorIndex"],
                    "local_id": spot["id"],
                    "cms_id": doodad["cmsId"],
                }
    return placements


def referenced_cut_scenes(level_triggers, doodads, boss_defeats):
    referenced = set()
    for trigger in level_triggers:
        for rule in trigger["rules"]:
            if not rule["supported"]:
                continue
            for task in rule["tasks"]:
                if task["kind"] == "PlayCutScene":
                    referenced.add(task["cut_scene_id"])
    for doodad in doodads:
        if "cut_scene_id" in doodad:
            referenced.add(doodad["cut_scene_id"])
    for defeat in boss_defeats:
        referenced.add(defeat["cut_scene_id"])
    return referenced
