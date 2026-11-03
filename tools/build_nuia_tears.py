"""Export TearsOfNui placements and deterministic rewards from verified client data."""

import argparse
import json
from pathlib import Path

from build_tutorial_data import BUILD_ID, SOURCE_SHA256, Source, fixed_rewards, fragment
from build_tutorial_data import spot_unit_id, write_profile
from tutorial_spawns import position_mm
from world_layers import inside_area, map_layer_files


TEAR_ITEM_ID = 2000
# Public maps that also show the tears of ArcheWorld's always-loaded Static layers (for
# example the Ruins of Gold tears) inside their authored area.
STATIC_LAYER_MAP_TYPES = ("Field", "HousingArea")


def tear_rewards(source, definition):
    expertise = source.row("ExpertiseReward", definition["FK_expertiseRewardId"])
    if expertise["typeCase"] != "LootPack":
        raise ValueError("Unsupported Nuia Tear expertise reward")

    pack = fragment(expertise["type"])
    if pack["count"] != 1:
        raise ValueError("Unsupported repeated Nuia Tear reward")

    rewards = fixed_rewards(source, pack["lootPackId"])
    if len(rewards) != 1 or rewards[0]["item_cms_id"] != TEAR_ITEM_ID:
        raise ValueError("Nuia Tear reward is not the recovered tear item")
    if rewards[0]["quantity"] not in (1, 5, 10):
        raise ValueError("Unsupported Nuia Tear quantity")
    return rewards[0]["quantity"]


def build(source):
    if source.row("Item", TEAR_ITEM_ID)["category"] != "TearsOfNui":
        raise ValueError("Nuia Tear item category mismatch")

    definitions = {int(row["id"]): row for row in source.table("AdventureObject")
                   if row["type"] == "TearsOfNui"}
    cached_spots = {}
    placements = {}
    for map_row in source.table("Map"):
        world = map_row["world"].rsplit("/", 1)[0].removeprefix("/Game/Maps/")
        layer_files = map_layer_files(source, world, map_row, "spot_object.json",
                                      STATIC_LAYER_MAP_TYPES)
        for relative, area in layer_files:
            if relative not in cached_spots:
                cached_spots[relative] = json.loads(source.read(Path("raw") / relative))

            for sector in cached_spots[relative]["sectors"]:
                for spot in sector["spotObjects"]:
                    if area is not None and not inside_area(spot["position"], area):
                        continue
                    for kind in spot["types"]:
                        cms_id = kind.get("adventureObject", {}).get("cmsId")
                        if cms_id not in definitions:
                            continue
                        definition = definitions[cms_id]
                        if any(definition[field] for field in (
                            "interactionGroupId", "requiredItemId", "requiredItemCount",
                            "costType", "costValue", "FK_rewardStatId", "rewardExp",
                            "FK_lootPackId", "charBuildPointType", "charBuildPoint",
                        )) or definition["FK_worldLevel"] != "1":
                            raise ValueError(f"Unsupported Nuia Tear conditions: {cms_id}")

                        placement = {
                            "map_id": int(map_row["id"]),
                            "unit_id": spot_unit_id(sector["sectorIndex"], spot["id"]),
                            "cms_id": cms_id,
                            "position": position_mm(spot["position"]),
                            "quantity": tear_rewards(source, definition),
                        }
                        key = (placement["map_id"], placement["unit_id"])
                        if key in placements and placements[key] != placement:
                            raise ValueError(f"Conflicting Nuia Tear placement: {key}")
                        placements[key] = placement

    return {
        "format_version": 1,
        "build_id": BUILD_ID,
        "source_executable_dump_sha256": SOURCE_SHA256,
        "placements": sorted(placements.values(), key=lambda row: (
            row["map_id"], int(row["unit_id"]),
        )),
        "sources": dict(source.sources),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/nuia-tears/definitions.json"))
    args = parser.parse_args()
    profile = build(Source(args.source))
    write_profile(args.output, profile)
    print(json.dumps({"placements": len(profile["placements"]),
                      "maps": len({row["map_id"] for row in profile["placements"]})}))


if __name__ == "__main__":
    main()
