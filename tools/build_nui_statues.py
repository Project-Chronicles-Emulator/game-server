"""Export matching-build Nui Statue/flame placements and authored action timings."""

import argparse
import json
import math
from pathlib import Path

from build_tutorial_data import BUILD_ID, SOURCE_SHA256, Source, spot_unit_id, write_profile
from tutorial_spawns import MAP_ROOT, position_mm


def interaction_timing(source, animation_id):
    """Statue rigs share execution but legitimately have different montage end times."""
    animation = source.row("InteractionAnim", animation_id)
    timings = []
    for detail in source.table("InteractionAnimDetail"):
        if detail["id"] != animation["animId"]:
            continue
        relative = Path("raw/X5/Content/Data/BuiltData/Anim/Interaction") / (
            detail["gameAnimKey"] + ".json"
        )
        document = json.loads(source.read(relative))
        events = [event for event in document["commonActStage"]["notifies"]
                  if event["notifyType"] == "interaction_execute"]
        if len(events) != 1:
            raise ValueError("Unsupported Nui Statue execute event sequence")
        execute_ms = events[0]["startTime"]
        duration_ms = document["endTime"]
        if not 0 < execute_ms < duration_ms <= 30000:
            raise ValueError("Invalid Nui Statue interaction timing")
        timings.append((execute_ms, duration_ms))

    if not timings or len({execute for execute, _ in timings}) != 1:
        raise ValueError("Nui Statue execution differs across rigs")
    return {"execute_time_ms": timings[0][0],
            "duration_ms": max(duration for _, duration in timings)}


def radius_mm(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("Invalid Nui shrine radius")
    if not math.isfinite(value) or not 0 <= value <= 100000:
        raise ValueError("Nui shrine radius is outside the export bound")
    return round(value * 10)


def placement(map_id, sector, spot, kind, cms_id, shrine):
    sphere = shrine["spawnVolume"]["spawnVolumeSphere"]
    spawn = sphere["spawnPosition"]
    yaw = spawn["yaw"]
    if not isinstance(yaw, (int, float)) or not math.isfinite(yaw):
        raise ValueError("Invalid Nui shrine spawn yaw")
    return {
        "map_id": map_id,
        "unit_id": spot_unit_id(sector["sectorIndex"], spot["id"]),
        "kind": kind,
        "cms_id": cms_id,
        "position": position_mm(spot["position"]),
        "spawn": position_mm(spawn),
        "spawn_yaw": yaw,
        "restricted_combat_radius_mm": radius_mm(shrine.get("restrictedCombatRadius", 0)),
        "restricted_npc_spawn_radius_mm": radius_mm(shrine.get("restrictedNpcSpawnRadius", 0)),
        "revive_task_id": shrine.get("reviveTaskId", 0),
    }


def build(source):
    interactions = {}
    for interaction_id, task, activation in ((10, "Use", "True"),
                                               (16, "QuickSlotAllItemSupplement", "")):
        row = source.row("Interaction", interaction_id)
        if (row["groupId"] != "10" or row["playerTaskCase"] != task
                or row["activateAdventureObject"] != activation or row["FK_conditionId"]
                or row["FK_interactionAnimId"] != str(interaction_id)):
            raise ValueError("Unsupported Nui Statue interaction join")
        interactions[str(interaction_id)] = interaction_timing(source, interaction_id)

    definitions = {int(row["id"]): row for row in source.table("AdventureObject")
                   if row["type"] == "NuiIdol"}
    for definition in definitions.values():
        if definition["interactionGroupId"] != "10":
            raise ValueError("Unsupported Nui Statue interaction group")

    # Statues also live in common world layers absent from runtimeDataLayerPath. The
    # older revival index already uses world ownership plus Map.area; use that contract
    # instead of silently omitting nine of the ten officially observed statue targets.
    layers_by_world = {}
    for name in sorted(source.raw_files):
        relative = Path(name)
        if relative.name != "spot_object.json" or not relative.is_relative_to(MAP_ROOT):
            continue
        parts = relative.relative_to(MAP_ROOT).parts
        if "DataLayers" not in parts:
            continue
        world = "/".join(parts[:parts.index("DataLayers")])
        layers_by_world.setdefault(world, []).append(relative)

    cached_layers = {}
    placements = {}
    flame_only_maps = []
    for map_row in source.table("Map"):
        map_id = int(map_row["id"])
        if map_row.get("nuiFlameOnly") == "True":
            flame_only_maps.append(map_id)
        world_path, asset = map_row["world"].removeprefix("/Game/Maps/").rsplit("/", 1)
        nested = world_path + "/" + asset.split(".")[0]
        world = nested if nested in layers_by_world else world_path
        bounds = json.loads("{" + map_row["area"] + "}")
        for relative in layers_by_world.get(world, []):
            if relative not in cached_layers:
                cached_layers[relative] = json.loads(source.read(Path("raw") / relative))
            for sector in cached_layers[relative]["sectors"]:
                for spot in sector["spotObjects"]:
                    for object_type in spot["types"]:
                        adventure = object_type.get("adventureObject", {})
                        if "nuiIdol" in adventure:
                            cms_id = adventure["cmsId"]
                            if cms_id not in definitions:
                                raise ValueError("Nui Statue placement has no catalog definition")
                            row = placement(map_id, sector, spot, "statue", cms_id,
                                            adventure["nuiIdol"])
                        elif "nuiFlameObject" in object_type:
                            row = placement(map_id, sector, spot, "flame", 0,
                                            object_type["nuiFlameObject"])
                        else:
                            continue
                        if not all(bounds["min"][axis] * 10 <= row["spawn"][index]
                                   <= bounds["max"][axis] * 10
                                   for index, axis in enumerate("xyz")):
                            continue
                        key = (map_id, row["unit_id"])
                        if key in placements and placements[key] != row:
                            raise ValueError("Conflicting Nui shrine placement")
                        placements[key] = row

    utility_groups = {}
    for row in source.table("UtilityBagFullFillCnt"):
        counts = [int(row["fullFillCnt" + str(index)]) for index in range(6)]
        if any(not 0 < count <= 1000 for count in counts):
            raise ValueError("Unsupported utility bag capacity tier")
        utility_groups[row["id"]] = counts
    utility_items = {}
    bag_options = {}
    utility_capacity = {}
    for item in source.table("Item"):
        if item.get("equipSlot") == "Bag":
            groups = json.loads(item.get("FK_UtilityBagFullFillCntOptionList") or "[]")
            if any(str(group) not in utility_groups for group in groups):
                raise ValueError("Bag references an unknown utility capacity group")
            bag_options[item["id"]] = {"level": int(item["level"]), "groups": groups}
        fill_id = item.get("FK_utilityBagFullFillCnt", "")
        if not fill_id:
            continue
        capacity = int(source.row("UtilityBagFullFillCnt", fill_id)["fullFillCnt0"])
        if not 0 < capacity <= 1000:
            raise ValueError("Unsupported utility bag refill capacity")
        utility_capacity[item["id"]] = capacity
        utility_items[item["id"]] = int(fill_id)

    blessing_levels = []
    for row in source.table("NuiBlessingLevel"):
        level = int(row["level"])
        required_tears = int(row["requiredTearsCount"])
        if level != len(blessing_levels) + 1 or not 0 < required_tears <= 100000:
            raise ValueError("Invalid Nui blessing threshold sequence")
        blessing_levels.append({"level": level, "required_tears": required_tears,
                                "max_hp_reward": int(row["maxHpReward"] or 0),
                                "max_sp_reward": int(row["maxSpReward"] or 0)})

    revive_tasks = {}
    for task in source.table("ReviveTask"):
        if task["playerTaskCase"] != "ItemSupply":
            raise ValueError("Unsupported Nui revival task")
        parameters = json.loads("{" + task["playerTask"] + "}")
        for supply in parameters["supplies"]:
            if int(supply["itemId"]) <= 0 or not 0 < int(supply["maxCount"]) <= 1000:
                raise ValueError("Invalid Nui revival supply")
        revive_tasks[task["id"]] = parameters
    for row in placements.values():
        if row["revive_task_id"] and str(row["revive_task_id"]) not in revive_tasks:
            raise ValueError("Nui flame references an unknown revival task")

    return {
        "format_version": 1,
        "build_id": BUILD_ID,
        "source_executable_dump_sha256": SOURCE_SHA256,
        "interactions": interactions,
        "utility_capacity": utility_capacity,
        "utility_groups": utility_groups,
        "utility_items": utility_items,
        "bag_options": bag_options,
        "blessing_levels": blessing_levels,
        "revive_tasks": revive_tasks,
        "flame_only_maps": sorted(flame_only_maps),
        "placements": sorted(placements.values(), key=lambda row: (
            row["map_id"], int(row["unit_id"]))),
        "sources": dict(source.sources),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/nui-statues/definitions.json"))
    arguments = parser.parse_args()
    profile = build(Source(arguments.source))
    write_profile(arguments.output, profile)
    print(json.dumps({"placements": len(profile["placements"]),
                      "statues": sum(row["kind"] == "statue" for row in profile["placements"]),
                      "maps": len({row["map_id"] for row in profile["placements"]})}))


if __name__ == "__main__":
    main()
