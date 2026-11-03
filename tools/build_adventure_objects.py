"""Export placed chest and record interactions from the verified client data.

The selected interaction groups are the shared-world requests observed in server logs.
Rewards come from authored expertise definitions, never from captured player inventories.
"""

import argparse
import json
from pathlib import Path

from build_tutorial_data import (
    BUILD_ID, SOURCE_SHA256, Source, adventure_interaction,
    fragment, spot_unit_id, write_profile,
)
from tutorial_spawns import position_mm
from build_loot_data import LootContext, build_pack_list, game_variable, gold_limit
from build_quest_catalog import BUILD_POINT_TYPES, quest_conditions
from world_layers import inside_area, map_layer_files


# Public maps that also show the objects of ArcheWorld's always-loaded Static layers (for
# example the Ruins of Gold looting spots) inside their authored area.
STATIC_LAYER_MAP_TYPES = ("Field", "HousingArea")
# Adventure object types with their own exporter and session: Nuia Tears are collected by
# proximity (nuia-tears) and Nui idols are revival shrines (nui-statues).
SEPARATELY_OWNED_TYPES = ("TearsOfNui", "NuiIdol")
# InteractionCondition kinds the shared adventure session checks with the quest condition
# evaluator, for example quest 10000125 (First Hunt) gating its hunting knife 20019119 on
# "objective group 1 in progress". Key-consuming (ConsumeItem) and channel-event
# gates are not modelled for adventure objects, so those objects stay unexported.
INTERACTION_CONDITION_KINDS = {"Quest", "OwnItem", "PlayRecord"}


def interaction_conditions(source, condition_id):
    """Return the runtime condition rows of one interaction condition id.

    Returns an empty list without a condition and None when any row is unsupported, so the
    caller never exports an object whose gate the server would only partly check.
    """
    if not condition_id:
        return []

    reasons = []
    try:
        conditions = quest_conditions(source, condition_id, reasons,
                                      table="InteractionCondition")
    except ValueError:
        # For example a PlayRecord whose record kind the quest evaluator does not count.
        return None
    if reasons or not conditions:
        return None
    if any(condition["kind"] not in INTERACTION_CONDITION_KINDS for condition in conditions):
        return None
    return conditions


def shared_adventure_object(definition, interactions, conditions_by_id=None):
    """True for an object the shared adventure session activates with a plain Use interaction.

    The chest (300) and record (305) groups were the first observed; every other Use
    interaction that activates its adventure object (for example the looting spots of group 23,
    activated with InteractUnitReq because they have no animation) follows the same activation
    and reward path. A conditional interaction qualifies only when ``conditions_by_id`` holds
    supported rows for its condition id.
    """
    if definition["type"] in SEPARATELY_OWNED_TYPES:
        return False
    interaction = interactions.get(definition["interactionGroupId"])
    if interaction is None:
        return False
    if (interaction["playerTaskCase"] != "Use"
            or interaction.get("activateAdventureObject") != "True"):
        return False

    condition_id = interaction.get("FK_conditionId")
    if not condition_id:
        return True
    return bool(conditions_by_id and conditions_by_id.get(condition_id))


def object_definition(source, definition, conditions):
    for field in (
        "requiredItemId", "requiredItemCount", "costType", "costValue",
        "FK_rewardStatId", "rewardExp", "FK_lootPackId", "charBuildPointType",
        "charBuildPoint",
    ):
        if definition[field]:
            raise ValueError(f"Unsupported adventure requirement {field}: {definition['id']}")
    rewards = []
    build_points = []
    experience = 0
    loot_pack_id = 0
    if definition["FK_expertiseRewardId"]:
        expertise = source.row("ExpertiseReward", definition["FK_expertiseRewardId"])
        parameters = fragment(expertise["type"])
        if expertise["typeCase"] == "LootPack":
            if parameters["count"] != 1:
                raise ValueError("Unsupported repeated adventure reward")
            loot_pack_id = parameters["lootPackId"]
        elif expertise["typeCase"] == "Exp":
            experience = parameters["value"]
            if not isinstance(experience, int) or experience < 0:
                raise ValueError("Invalid adventure experience")
        elif expertise["typeCase"] == "CharBuildPoint":
            point_name = BUILD_POINT_TYPES.get(parameters.get("type"))
            point_type = {"LevelUpPoint": 1, "WeaponPoint": 2}.get(point_name)
            amount = parameters.get("value")
            if point_type is None or not isinstance(amount, int) or not 0 < amount <= 1000:
                raise ValueError("Invalid adventure build-point reward")
            build_points.append({"type": point_type, "value": amount})
        else:
            raise ValueError(f"Unsupported adventure expertise reward: {expertise['typeCase']}")
    interaction_id = int(definition["interactionGroupId"])
    result = {
        "cms_id": int(definition["id"]),
        "interaction_id": interaction_id,
        "experience": experience,
        "loot_pack_id": loot_pack_id,
        "rewards": rewards,
        **adventure_interaction(source, interaction_id, allow_condition=bool(conditions)),
    }
    if conditions:
        result["interaction_conditions"] = conditions
    if build_points:
        result["build_points"] = build_points
    return result


def build(source):
    interactions = {row["id"]: row for row in source.table("Interaction")}
    # Only activating Use interactions can make an adventure object shared.
    condition_ids = {row["FK_conditionId"] for row in interactions.values()
                     if row.get("FK_conditionId") and row["playerTaskCase"] == "Use"
                     and row.get("activateAdventureObject") == "True"}
    conditions_by_id = {condition_id: interaction_conditions(source, condition_id)
                        for condition_id in sorted(condition_ids)}
    definitions = {int(row["id"]): row for row in source.table("AdventureObject")
                   if shared_adventure_object(row, interactions, conditions_by_id)}
    cached_objects = {}
    cached_layers = {}
    maps = []
    for map_row in source.table("Map"):
        map_id = int(map_row["id"])
        if map_id in (8000, 10117):
            continue
        world = map_row["world"].rsplit("/", 1)[0].removeprefix("/Game/Maps/")
        objects = {}
        layer_files = map_layer_files(source, world, map_row, "spot_object.json",
                                      STATIC_LAYER_MAP_TYPES)
        for relative, area in layer_files:
            if relative not in cached_layers:
                cached_layers[relative] = json.loads(source.read(Path("raw") / relative))
            for sector in cached_layers[relative]["sectors"]:
                for spot in sector["spotObjects"]:
                    if area is not None and not inside_area(spot["position"], area):
                        continue
                    for kind in spot["types"]:
                        cms_id = kind.get("adventureObject", {}).get("cmsId")
                        if cms_id not in definitions:
                            continue
                        if cms_id not in cached_objects:
                            row = definitions[cms_id]
                            condition_id = interactions[row["interactionGroupId"]].get(
                                "FK_conditionId")
                            conditions = conditions_by_id.get(condition_id) or []
                            try:
                                cached_objects[cms_id] = object_definition(source, row, conditions)
                            except ValueError as error:
                                # Incomplete authored references remain explicit unavailable objects;
                                # they must never be marked collected with a guessed/empty reward.
                                cached_objects[cms_id] = {
                                    "cms_id": cms_id,
                                    "interaction_id": int(definitions[cms_id]["interactionGroupId"]),
                                    "rewards": [], "loot_pack_id": 0,
                                    "unavailable_reason": str(error),
                                }
                        unit_id = spot_unit_id(sector["sectorIndex"], spot["id"])
                        placement = {
                            **cached_objects[cms_id],
                            "unit_id": unit_id,
                            "position": position_mm(spot["position"]),
                        }
                        if unit_id in objects and objects[unit_id] != placement:
                            raise ValueError(f"Conflicting adventure placement: {map_id}/{unit_id}")
                        objects[unit_id] = placement
        if objects:
            maps.append({
                "format_version": 1,
                "build_id": BUILD_ID,
                "profile_scope": "shared_adventure",
                "map_id": map_id,
                "objects": sorted(objects.values(), key=lambda row: int(row["unit_id"])),
                "quests": [], "doodads": [], "sectors": [], "utility_items": [],
                "civilians": [],
            })
    pack_ids = {row["loot_pack_id"] for row in cached_objects.values() if row["loot_pack_id"]}
    context = LootContext({row["id"]: row for row in source.table("Item")},
                          {int(row["id"]) for row in source.table("Quest")})
    conditions = {}
    for row in source.table("LootCondition"):
        conditions.setdefault(row["FK_lootPack"], []).append(row)
    items = {}
    for row in source.table("LootItem"):
        items.setdefault(row["lootGroupId"], []).append(row)
    item_definitions = {}
    packs, skipped = build_pack_list(
        pack_ids, {row["id"]: row["rate"] for row in source.table("LootPack")},
        conditions, items, context, item_definitions,
    )
    unavailable_packs = {row["loot_pack_id"] for row in skipped}
    for profile in maps:
        for placement in profile["objects"]:
            if placement["loot_pack_id"] in unavailable_packs:
                placement["unavailable_reason"] = "Authored loot pack has unresolved conditions"
                placement["loot_pack_id"] = 0
    loot = {
        "format_version": 2, "build_id": BUILD_ID,
        "looting_range_mm": game_variable(source, "LootingRange") * 10,
        "corpse_lootable_ms": game_variable(source, "NpcCorpseLootingDestroyMillis"),
        "corpse_empty_ms": game_variable(source, "NpcCorpseNoneDestroyMillis"),
        "gold_limit": gold_limit(source),
        "items": {str(key): value for key, value in item_definitions.items()},
        "packs": packs, "handling_packs": [], "npcs": [],
        "npc_fields": ["npc_cms_id", "loot_pack_id", "handling_pack_id",
                       "handling_rate_percent", "kill_gold"],
    }
    return {
        "format_version": 1,
        "source_executable_dump_sha256": SOURCE_SHA256,
        "maps": sorted(maps, key=lambda row: row["map_id"]),
        "loot": loot,
        "unresolved_loot_groups": skipped,
        "sources": dict(source.sources),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/adventure/definitions.json"))
    args = parser.parse_args()
    document = build(Source(args.source))
    write_profile(args.output, document)
    print(json.dumps({"maps": len(document["maps"]),
                      "placements": sum(len(row["objects"]) for row in document["maps"]),
                      "unavailable": [
                          {"map_id": profile["map_id"], "cms_id": row["cms_id"],
                           "reason": row["unavailable_reason"]}
                          for profile in document["maps"] for row in profile["objects"]
                          if "unavailable_reason" in row
                      ]}))


if __name__ == "__main__":
    main()
