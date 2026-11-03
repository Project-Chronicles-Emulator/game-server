"""Build crafting-station (processing/cooking) definitions from the verified client export.

The output contains static CMS and placement data only. No official character,
account, item-instance or session identifier is copied into the runtime profile.

Evidence for the joins below (build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa):

* A craft is ``PC2S_StartInteractionAnimReq`` with ``param.interaction.execute_craft``.
  Its ``target_unit_id`` is a spot doodad: ``(sector_id << 32) | (local_id << 12) | 6``.
  All nine captured crafts resolved to placements of doodad 20251, whose phase-1
  ``DoodadPhase.interactionGroupId`` is interaction 10220 (Craft, animation 60001).
* The station's ``Doodad.propertyType`` names ``ItemCraftPack`` IDs; recipe 60190101
  is in pack 3000001. ``ItemCraft`` matched every capture: 9 x 70102001 and 125 Gold
  became 5 x 40101101.
* The execute request followed the start by 2,937 ms; the 60001 montage's single
  ``interaction_execute`` notify is at 2,933 ms.
* Housing crafting stations (Item category HousingBuilding with ``FK_doodadId`` 85001-85005)
  are Craft doodads with their own interaction groups and packs but no map placement. They
  are exported as stations; the server resolves their placement from the housing lot.
"""

import argparse
import glob
import json
from pathlib import Path

from build_gathering_data import looped_animation_timing, map_areas, sector_id
from build_housing_data import housing_doodad_ids
from build_tutorial_data import BUILD_ID, Source, fragment, spot_unit_id, write_profile


MAP_ROOT = Path("raw/X5/Content/Data/BuiltData/Maps/ArcheWorld/DataLayers")
CRAFT_TASK = "Craft"
GOLD_COST = "Gold"
MAXIMUM_MATERIALS = 5
MILLIMETRES_PER_CENTIMETRE = 10
# Must match the categories npc_shop.cpp stores as WorldProgress stacks.
STACKABLE_CATEGORIES = ("Consumable", "CraftMaterial", "Food", "PolishingMaterial",
                        "Repairer", "Etc", "HousingBuildingDrawing")
INSTANCE_CATEGORY = "Equipment"
CRAFT_AMOUNT_VARIABLE = "MaxNumOfCraftAmount"


def item_storage(item):
    if item["category"] in STACKABLE_CATEGORIES:
        return "stackable"
    if item["category"] == INSTANCE_CATEGORY:
        return "instance"
    if item["category"] in ("HousingBuilding", "HousingObject"):
        return "housing"
    return None


def craft_interactions(source):
    """Return Craft interactions keyed by group, failing closed on conditional rows."""
    interactions = {}
    for row in source.table("Interaction"):
        if row["playerTaskCase"] != CRAFT_TASK:
            continue
        if not row["FK_interactionAnimId"] or row["FK_conditionId"]:
            # Trade-good crafting has no animation; conditional rows need condition checks.
            continue

        task = fragment(row["playerTask"])
        expertise = task.get("expertiseType", "")
        if not expertise.startswith("EX5ExpertiseType_"):
            raise ValueError(f"Craft interaction without expertise type: {row['id']}")
        group_id = int(row["groupId"])
        interactions.setdefault(group_id, []).append({
            "interaction_id": int(row["id"]),
            "group_id": group_id,
            "expertise_type": expertise.removeprefix("EX5ExpertiseType_"),
            "tier_limit": int(task.get("tierLimit", 0)),
            "interaction_anim_id": int(row["FK_interactionAnimId"]),
        })
    return interactions


def station_groups(source, doodad):
    """Interaction groups offered by a doodad itself or by any of its phases."""
    groups = set()
    if doodad["interactionGroupId"]:
        groups.add(int(doodad["interactionGroupId"]))
    for phase in source.table("DoodadPhase"):
        if phase["FK_doodad"] == doodad["id"] and phase["interactionGroupId"]:
            groups.add(int(phase["interactionGroupId"]))
    return groups


def pack_crafts(source):
    packs = {}
    for row in source.table("ItemCraftPack"):
        packs.setdefault(int(row["id"]), []).append(int(row["FK_itemCraft"]))
    return packs


def craft_stations(source, interactions):
    """Doodads whose Craft property lists recipe packs and whose group has a Craft row."""
    packs = pack_crafts(source)
    stations = []
    for doodad in source.table("Doodad"):
        if doodad["propertyTypeCase"] != CRAFT_TASK:
            continue

        craft_groups = sorted(group for group in station_groups(source, doodad)
                              if group in interactions)
        if not craft_groups:
            continue
        if len(craft_groups) != 1:
            raise ValueError(f"Craft station has several craft groups: {doodad['id']}")

        pack_text = fragment(doodad["propertyType"]).get("packIds", "")
        pack_ids = [int(value) for value in str(pack_text).split(",") if value.strip()]
        craft_ids = set()
        for pack_id in pack_ids:
            if pack_id not in packs:
                raise ValueError(f"Craft station references unknown pack {pack_id}")
            craft_ids.update(packs[pack_id])
        if not craft_ids:
            raise ValueError(f"Craft station has no recipes: {doodad['id']}")

        stations.append({
            "doodad_cms_id": int(doodad["id"]),
            "interaction_group_id": craft_groups[0],
            "craft_ids": sorted(craft_ids),
        })
    return sorted(stations, key=lambda entry: entry["doodad_cms_id"])


def craft_definition(row, items):
    """Return (definition, None) or (None, reason) for one ItemCraft row."""
    craft_id = int(row["craftId"])
    if row["autoRegistration"] not in ("True", "False"):
        raise ValueError(f"Craft {craft_id} has an invalid registration flag")
    if row["companyPositionLevel"]:
        return None, "Company position requirements are not evaluated"
    if row["costType"] != GOLD_COST:
        return None, "Only Gold craft costs are persisted"

    product = items.get(row["FK_item"])
    if product is None:
        raise ValueError(f"Craft {craft_id} produces an unknown item")
    product_storage = item_storage(product)
    if product_storage is None:
        return None, f"Product category {product['category']} has no emulator storage"

    materials = []
    for index in range(1, MAXIMUM_MATERIALS + 1):
        material_id = row[f"materialId0{index}"]
        if not material_id:
            continue
        material = items.get(material_id)
        if material is None:
            raise ValueError(f"Craft {craft_id} consumes an unknown item")
        if item_storage(material) != "stackable":
            return None, f"Material category {material['category']} is not a stackable"
        count = int(row[f"materialCnt0{index}"])
        if count <= 0:
            raise ValueError(f"Craft {craft_id} has a non-positive material count")
        materials.append({"item_cms_id": int(material_id), "count": count})
    if not materials:
        raise ValueError(f"Craft {craft_id} has no materials")
    if len({material["item_cms_id"] for material in materials}) != len(materials):
        raise ValueError(f"Craft {craft_id} repeats a material")

    product_count = int(row["itemCount"])
    gold_cost = int(row["costValue"] or 0)
    if product_count <= 0 or gold_cost < 0:
        raise ValueError(f"Craft {craft_id} has an invalid product count or cost")

    definition = {
        "item_craft_id": craft_id,
        "requires_learning": row["autoRegistration"] == "False",
        "product_item_cms_id": int(row["FK_item"]),
        "product_count": product_count,
        "product_storage": product_storage,
        "expertise_type": row["expertiseType"],
        "proper_expertise_level": row["properExpertiseLevel"],
        "craft_group": row["craftGroup"],
        "gold_cost": gold_cost,
        "materials": materials,
    }
    if product_storage == "instance":
        durability = int(product["maxDurability"] or 0)
        if durability <= 0:
            return None, "Equipment product has no maximum durability"
        definition["product_max_durability"] = durability
        definition["product_grade"] = product["grade"]
    return definition, None


def crafts(source):
    items = {row["id"]: row for row in source.table("Item")}
    definitions = []
    unsupported = []
    for row in source.table("ItemCraft"):
        definition, reason = craft_definition(row, items)
        if definition is None:
            unsupported.append({"item_craft_id": int(row["craftId"]), "reason": reason})
        else:
            definitions.append(definition)
    definitions.sort(key=lambda entry: entry["item_craft_id"])
    unsupported.sort(key=lambda entry: entry["item_craft_id"])
    return definitions, unsupported


def craft_animations(source, interactions):
    animation_ids = sorted({entry["interaction_anim_id"]
                            for rows in interactions.values() for entry in rows})
    animations = []
    for animation_id in animation_ids:
        animation = source.row("InteractionAnim", animation_id)
        if animation["costType"] != "None" or animation["toolEquipmentType"] != "None":
            raise ValueError(f"Craft animation has an unsupported cost or tool: {animation_id}")
        execute_ms, duration_ms = looped_animation_timing(source, animation)
        animations.append({"interaction_anim_id": animation_id, "execute_time_ms": execute_ms,
                           "duration_ms": duration_ms})
    return animations


def inside_map(position, area):
    return all(area["area_min_mm"][axis] <= position[axis] <= area["area_max_mm"][axis]
               for axis in range(3))


def station_placements(source, station_ids, maps):
    """Spot doodads of craft stations that lie inside at least one demo map area."""
    placements = []
    root = source.root / MAP_ROOT
    for absolute in sorted(glob.glob(str(root / "**" / "spot_object.json"), recursive=True)):
        relative = Path(absolute).relative_to(source.root)
        document = json.loads(source.read(relative))
        for sector in document.get("sectors", []):
            sector_index = sector["sectorIndex"]
            sector_id(sector_index)
            for spot in sector.get("spotObjects", []):
                for spot_type in spot.get("types", []):
                    doodad = spot_type.get("doodad")
                    if not doodad or doodad.get("cmsId") not in station_ids:
                        continue
                    position = [round(spot["position"][axis] * MILLIMETRES_PER_CENTIMETRE)
                                for axis in ("x", "y", "z")]
                    if not any(inside_map(position, area) for area in maps):
                        continue
                    unit_id = spot_unit_id(sector_index, spot["id"])
                    placements.append([unit_id, doodad["cmsId"], *position])
    placements.sort(key=lambda entry: int(entry[0]))
    unit_ids = [entry[0] for entry in placements]
    if len(unit_ids) != len(set(unit_ids)):
        raise ValueError("Duplicate craft station unit ID")
    return placements


def reachable_stations(source, all_stations, placements):
    """Stations a player can use: map placements and the stations a housing lot can hold.

    Housing stations (the Kitchen and the other HousingBuilding stations 85001-85005, and
    building add-ons) have no map placement; players place them on their lot at runtime.
    Other unplaced doodads are unreachable and are neither validated nor exported.
    """
    placed_ids = {placement[1] for placement in placements}
    lot_ids = set(housing_doodad_ids(source))
    return [entry for entry in all_stations
            if entry["doodad_cms_id"] in placed_ids or entry["doodad_cms_id"] in lot_ids]


def maximum_craft_amount(source):
    rows = [row for row in source.table("GameVariable") if row["name"] == CRAFT_AMOUNT_VARIABLE]
    if len(rows) != 1 or int(rows[0]["value"]) <= 0:
        raise ValueError("Missing or invalid MaxNumOfCraftAmount")
    return int(rows[0]["value"])


def expertise_by_craft_id(definitions):
    return {entry["item_craft_id"]: entry["expertise_type"] for entry in definitions}


def station_expertise_problem(station, rows, expertise_by_craft):
    """Why a station's recipes disagree with its craft interaction's expertise, or None."""
    expertise = {row["expertise_type"] for row in rows}
    if len(expertise) != 1:
        return "mixes craft expertise types"
    for craft_id in station["craft_ids"]:
        recipe_expertise = expertise_by_craft.get(craft_id)
        if recipe_expertise is not None and recipe_expertise not in expertise:
            return f"offers craft {craft_id} of another expertise"
    return None


def validate_station_expertise(stations, station_interactions, definitions):
    """Every supported recipe at a station must share its craft interaction's expertise."""
    expertise_by_craft = expertise_by_craft_id(definitions)
    for station in stations:
        rows = station_interactions[station["interaction_group_id"]]
        problem = station_expertise_problem(station, rows, expertise_by_craft)
        if problem:
            raise ValueError(f"Station {station['doodad_cms_id']} {problem}")


def omit_inconsistent_lot_stations(stations, placements, interactions, definitions):
    """Split off lot-only stations whose recipes disagree with their interaction's expertise.

    Map stations stay strictly validated by validate_station_expertise. A lot-only station
    that fails the same check (building add-on 169990, the Crafting Workbench, pairs the Alchemy
    interaction with a cooking pack) is omitted, so the server refuses it, and reported.
    """
    placed_ids = {placement[1] for placement in placements}
    expertise_by_craft = expertise_by_craft_id(definitions)
    kept = []
    omitted = []
    for station in stations:
        problem = None
        if station["doodad_cms_id"] not in placed_ids:
            rows = interactions[station["interaction_group_id"]]
            problem = station_expertise_problem(station, rows, expertise_by_craft)
        if problem:
            omitted.append({"doodad_cms_id": station["doodad_cms_id"], "reason": problem})
        else:
            kept.append(station)
    return kept, omitted


def build(source):
    maps = map_areas(source, include_housing=True, include_release_fields=True)
    interactions = craft_interactions(source)
    all_stations = craft_stations(source, interactions)
    placements = station_placements(source, {entry["doodad_cms_id"] for entry in all_stations},
                                    maps)
    if not placements:
        raise ValueError("No craft station placement lies inside a demo map")

    stations = reachable_stations(source, all_stations, placements)

    definitions, unsupported = crafts(source)
    known = {entry["item_craft_id"] for entry in definitions}
    known.update(entry["item_craft_id"] for entry in unsupported)
    for station in stations:
        missing = [craft_id for craft_id in station["craft_ids"] if craft_id not in known]
        if missing:
            raise ValueError(f"Station {station['doodad_cms_id']} lists unknown crafts {missing}")
    stations, unsupported_stations = omit_inconsistent_lot_stations(
        stations, placements, interactions, definitions)

    used_groups = {station["interaction_group_id"] for station in stations}
    station_interactions = {group: interactions[group] for group in used_groups}
    interaction_rows = sorted((entry for rows in station_interactions.values() for entry in rows),
                              key=lambda entry: entry["interaction_id"])
    validate_station_expertise(stations, station_interactions, definitions)

    return {
        "format_version": 1,
        "build_id": BUILD_ID,
        "maximum_craft_amount": maximum_craft_amount(source),
        "maps": maps,
        "animations": craft_animations(source, station_interactions),
        "interactions": interaction_rows,
        "stations": stations,
        "unsupported_stations": unsupported_stations,
        "crafts": definitions,
        "unsupported_crafts": unsupported,
        "placement_fields": ["unit_id", "doodad_cms_id", "x_mm", "y_mm", "z_mm"],
        "placements": placements,
        "sources": source.sources,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/processing/definitions.json"))
    args = parser.parse_args()

    profile = build(Source(args.source))
    write_profile(args.output, profile)
    print(json.dumps({
        "maps": len(profile["maps"]),
        "stations": len(profile["stations"]),
        "unsupported_stations": len(profile["unsupported_stations"]),
        "placements": len(profile["placements"]),
        "crafts": len(profile["crafts"]),
        "unsupported_crafts": len(profile["unsupported_crafts"]),
    }))


if __name__ == "__main__":
    main()
