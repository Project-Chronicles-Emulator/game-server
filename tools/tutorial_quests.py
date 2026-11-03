"""Recover bounded quest definitions for the two captured tutorial instances."""

import json
import re
from pathlib import Path

from tutorial_spawns import MAP_ROOT, wire_guid


QUESTS_BY_MAP = {
    8000: {10000040, 10000042, 10000043, 10000044, 10000045, 10000106, 90101006},
    10117: {10000046, 90100011},
}

# x5.cms.EX5CharBuildPointType values; the server's remaining_char_build_points keys.
CHAR_BUILD_POINT_TYPES = {
    "EX5CharBuildPointType_LevelUpPoint": 1,
    "EX5CharBuildPointType_WeaponPoint": 2,
}


def fragment(text):
    return json.loads("{" + text + "}") if text else {}


def map_trigger_volumes(source, map_id):
    row = source.row("Map", map_id)
    world = row["world"].rsplit("/", 1)[1].split(".")[0]
    layers = set()
    visited = set()
    current = row
    while True:
        current_id = int(current["id"])
        if current_id in visited or len(visited) >= 8:
            raise ValueError("Invalid tutorial map ancestry")
        visited.add(current_id)
        current_world = current["world"].rsplit("/", 1)[1].split(".")[0]
        if current_world != world:
            break
        layers.update(filter(None, current["runtimeDataLayerPath"].strip("[]").split(",")))
        parent_id = int(current.get("parentMapId") or 0)
        if parent_id == 0:
            break
        current = source.row("Map", parent_id)

    volumes = {}
    # Instance exit objectives may refer to a trigger in their parent map's
    # shared layer. Include only catalog-declared ancestry from this world.
    for layer in sorted(layers):
        relative = MAP_ROOT / world / "DataLayers/Runtime" / layer / "level_trigger_volume.json"
        if relative.as_posix() not in source.raw_files:
            continue
        document = json.loads(source.read(Path("raw") / relative))
        for volume in document["triggerVolumes"]:
            previous = volumes.get(volume["id"])
            if previous is not None and previous != volume:
                raise ValueError(f"Conflicting quest trigger volume: {volume['id']}")
            volumes[volume["id"]] = volume
    return volumes


def trigger_definition(volumes, parameters):
    guid = parameters["triggerVolumeId"]
    volume = volumes.get(guid)
    if volume is None:
        return None
    if parameters.get("mapLayer", 0) != 0:
        raise ValueError("Unsupported tutorial quest map layer")

    if "sphere" in volume:
        sphere = volume["sphere"]
        return {
            "guid": wire_guid(guid),
            "center": [round(sphere["center"][axis] * 10) for axis in ("x", "y", "z")],
            "radius": round(sphere["radius"] * 10),
        }

    box = volume["box"]
    return {
        "guid": wire_guid(guid),
        "center": [round(box["center"][axis] * 10) for axis in ("x", "y", "z")],
        "extent": [round(box["extent"][axis] * 10) for axis in ("x", "y", "z")],
        "yaw": box["rotation"]["yaw"],
    }


def supply_items(source, text):
    supplies = []
    for item in text.split(","):
        if not item:
            continue
        identifier, quantity = item.split(":")
        definition = source.row("Item", identifier)
        if definition["category"] != "QuestItem" or not 0 < int(quantity) <= 1000000:
            raise ValueError("Unsupported tutorial quest supply item")
        supplies.append({"item_cms_id": int(identifier), "quantity": int(quantity)})
    return supplies


def build_quest_definitions(source, map_id, fixed_rewards):
    volumes = map_trigger_volumes(source, map_id)
    quests = []
    chains = {}
    for quest_id in sorted(QUESTS_BY_MAP[map_id]):
        row = source.row("Quest", quest_id)
        if any(row.get(key) for key in (
            "requireGold", "requireItemIds", "FK_QuestCondition", "retrieveDependentItem",
        )):
            raise ValueError(f"Unsupported tutorial quest requirements: {quest_id}")

        accepts = []
        npc_ids = []
        for index in range(1, 4):
            kind = row[f"acceptConditionType{index}Case"]
            parameters = fragment(row[f"acceptConditionType{index}"])
            if kind in ("TriggerVolumeEnter", "UnexpectTriggerVolumeEnter"):
                trigger = trigger_definition(volumes, parameters)
                if trigger is not None:
                    trigger["request_field"] = (
                        "unexpect_trigger_volume_enter"
                        if kind == "UnexpectTriggerVolumeEnter" else "trigger_volume_enter"
                    )
                    accepts.append(trigger)
            elif kind == "NpcId":
                npc_ids.append(parameters["npcId"])
            elif kind not in ("", "Sequential"):
                raise ValueError(f"Unsupported quest acceptance: {quest_id}/{kind}")

        progresses = []
        for progress in source.table("QuestProgress"):
            if progress["id"] != str(quest_id):
                continue
            entry = {
                "sequence": int(progress["sequenceId"]),
                "group": int(progress["group"]),
                "kind": progress["typeCase"],
                "parameters": fragment(progress["type"]),
            }
            if entry["kind"] in ("TriggerVolumeEnter", "TriggerVolumeLeave"):
                entry["trigger"] = trigger_definition(volumes, entry["parameters"])
                if entry["trigger"] is None:
                    raise ValueError(f"Missing quest trigger: {quest_id}/{entry['sequence']}")
            elif entry["kind"] not in ("DialogueSelection", "InteractDoodad", "HuntMonster"):
                raise ValueError(f"Unsupported quest objective: {quest_id}/{entry['kind']}")
            progresses.append(entry)

        rewards = []
        build_points = []
        experience = 0
        for reward in source.table("QuestReward"):
            if reward["id"] != row["FK_QuestReward"]:
                continue
            parameters = fragment(reward["type"])
            if reward["typeCase"] == "Exp":
                experience += parameters["value"]
            elif reward["typeCase"] == "LootPack":
                rewards.extend(fixed_rewards(source, parameters["lootPackId"]))
            elif reward["typeCase"] == "Money" and parameters["moneyType"] == "EX5MoneyType_Gold":
                rewards.append({"item_cms_id": 0, "quantity": parameters["value"]})
            elif reward["typeCase"] == "CharBuildPoint":
                build_points.append({
                    "type": CHAR_BUILD_POINT_TYPES[parameters["type"]],
                    "value": parameters["value"],
                })
            else:
                raise ValueError(f"Unsupported tutorial quest reward: {quest_id}")

        quest = {
            "cms_id": quest_id,
            "accept_triggers": accepts,
            "accept_npcs": npc_ids,
            "supplies": supply_items(source, row["supplyItemIds"]),
            "auto_complete": row["type"] == "HiddenAutoComplete",
            "sequential": row["isProgressSequential"] == "True",
            "progresses": progresses,
            "experience": experience,
            "rewards": rewards,
        }
        # Only emitted when present, so profiles without build point rewards are unchanged.
        if build_points:
            quest["build_points"] = build_points
        quests.append(quest)
        if row["sequentialGroupOrder"]:
            match = re.fullmatch(r"\((\d+),(\d+)\)", row["sequentialGroupOrder"])
            if match is None:
                raise ValueError(f"Invalid tutorial quest chain: {quest_id}")
            chain, order = map(int, match.groups())
            if order in chains.setdefault(chain, {}):
                raise ValueError(f"Duplicate tutorial quest chain order: {quest_id}")
            chains[chain][order] = quest

    for chain in chains.values():
        ordered = [quest for order, quest in sorted(chain.items())]
        for previous, following in zip(ordered, ordered[1:]):
            previous["next_quest"] = following["cms_id"]
    return quests
