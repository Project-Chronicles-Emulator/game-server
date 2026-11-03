"""Export the verified client guide rules and their placed trigger volumes."""

import argparse
import json
from pathlib import Path

from build_tutorial_data import BUILD_ID, Source
from tutorial_quests import fragment, map_trigger_volumes, trigger_definition


QUEST_REQUIREMENTS = {
    "EX5RequirementQuestType_Acceptable": "Acceptable",
    "EX5RequirementQuestType_Progressing": "Progressing",
    "EX5RequirementQuestType_QuestComplete": "QuestComplete",
    "EX5RequirementQuestType_QuestProgress": "QuestProgress",
}


def normalize_condition(row):
    """Retain unsupported conditions so evaluation can treat them as unknown."""
    condition = {
        "group": int(row["group"]),
        "operator": row["logicalOperator"],
        "negated": row["negated"] == "True",
        "kind": row["typeCase"],
    }
    if row["checkOnType"] != "Caster" or condition["operator"] not in ("AND", "OR"):
        condition["kind"] = "Unsupported"
        return condition

    parameters = fragment(row["type"])
    kind = condition["kind"]
    if kind in ("TriggerVolumeEnter", "TriggerVolumeLeave"):
        condition["volume_id"] = parameters["triggerVolumeId"]
    elif kind == "Quest":
        requirement = QUEST_REQUIREMENTS.get(parameters.get("questRequirementType"))
        if requirement is None:
            condition["kind"] = "Unsupported"
        else:
            condition.update({"quest_id": int(parameters["questId"]),
                              "requirement": requirement})
            if "progressGroup" in parameters:
                condition["progress_group"] = int(parameters["progressGroup"])
    elif kind == "OwnItem":
        condition.update({"item_id": int(parameters["itemId"]),
                          "count": int(parameters.get("count", 1)),
                          "grade": parameters.get("grade", "")})
    elif kind == "Interaction":
        condition["interaction_id"] = int(parameters["interactionId"])
    elif kind == "CutSceneEnd":
        condition["cut_scene_id"] = int(parameters["cutSceneId"])
    elif kind == "CurrentWeaponType":
        if parameters.get("actionType") != "EX5WeaponActionType_Activation":
            condition["kind"] = "Unsupported"
        else:
            condition["weapon_type"] = parameters["equipmentType"].removeprefix(
                "EX5EquipmentType_")
    else:
        condition["kind"] = "Unsupported"
    return condition


def build_guides(source):
    conditions = {}
    for row in source.table("ContentsGuideCondition"):
        conditions.setdefault(row["conditionId"], []).append(normalize_condition(row))

    guides = []
    volume_ids = set()
    for row in source.table("ContentsGuide"):
        if row["Order"] != "1" or row["Type"] not in ("Text", "ControlTip"):
            continue
        start_id = row["FK_StartCondition"]
        end_id = row["FK_EndCondition"]
        if start_id and start_id not in conditions:
            raise ValueError(f"Missing guide start condition: {start_id}")
        if end_id and end_id not in conditions:
            raise ValueError(f"Missing guide end condition: {end_id}")
        start = conditions.get(start_id, [])
        if not start:
            continue
        end = conditions.get(end_id, [])
        for condition in start + end:
            if "volume_id" in condition:
                volume_ids.add(condition["volume_id"])
        guides.append({"id": int(row["Id"]), "type": row["Type"],
                       "start": start, "end": end})

    maps = []
    for row in source.table("Map"):
        map_id = int(row["id"])
        volumes = map_trigger_volumes(source, map_id)
        selected = []
        for volume_id in sorted(volume_ids.intersection(volumes)):
            geometry = trigger_definition(volumes, {"triggerVolumeId": volume_id})
            if geometry is not None:
                selected.append({"id": volume_id, **geometry})
        if selected:
            maps.append({"map_id": map_id, "volumes": selected})

    return {"format_version": 1, "build_id": BUILD_ID, "guides": guides, "maps": maps}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/contents-guides/definitions.json"))
    options = parser.parse_args()
    source = Source(options.source)
    document = build_guides(source)
    document["source_sha256"] = dict(sorted(source.sources.items()))
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"Exported {len(document['guides'])} guides on {len(document['maps'])} maps")


if __name__ == "__main__":
    main()
