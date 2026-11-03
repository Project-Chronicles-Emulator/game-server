"""Generate per-map civilian placements and authored patrols from extracted client data.

The output feeds a server-owned NPC unit registry. It lists only authored placements of NPCs
that stand still (see STATIONARY_RULE), with provenance back to the placement GUID. It carries
no official runtime unit ids, which are private and unstable, and no captured values.

Catalog chain for one placement (all files are read through the hash-checked Source):

    Map.runtimeDataLayerPath -> Maps/<World>/DataLayers/Runtime/<Layer>/npc_spawner.json
    placement.cmsId          -> NpcSpawner.id   (NpcSpawner.npc = "npcId:weight,...")
    npcId                    -> Npc.id          (behaviorType, isRoaming, interactionGroupId)
    Npc.interactionGroupId   -> Interaction.groupId (playerTaskCase Shop -> playerTask shopId)

Positions: placement centimeters times 10 give wire millimeters on every map checked in
out/agent-team-reports/prereq/spawn-coordinate-proof.md (x and y verified, z approximate).
"""

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path

from tutorial_spawns import MAP_ROOT, wire_guid
from npc_patrol_data import build_patrol, load_layer_paths
from world_layers import (map_area_cm, placement_in_map, static_layer_directories,
                          uses_own_static_layers, uses_static_world_layers, world_directory,
                          world_name)


# BuiltData placement coordinates are Unreal centimeters; the wire carries millimeters.
CENTIMETERS_TO_MILLIMETERS = 10
FORMAT_VERSION = 1
DEFAULT_MAP_ID = 9012
MAX_ABSOLUTE_CENTIMETERS = 100000000
# Private tutorial maps: their NPCs belong to the tutorial instance, not a shared population.
PRIVATE_TUTORIAL_MAPS = {8000, 10117}
# Map types whose shared population also takes ArcheWorld's Static layers inside its area.
STATIC_LAYER_MAP_TYPES = ("Field", "HousingArea")
# --all-maps covers the shipped packages and leaves out developer test worlds.
PUBLIC_MAP_PACKAGES = ("Demo", "Release")
DEVELOPER_TEST_WORLDS = {
    "Battle2", "Battle3", "Battle_Test_Map", "CG_Test_Map", "Climbing_Test", "Interaction1",
    "Sound", "Thumnail_Footage",
}

# Npc.behaviorType values that are standing NPCs. The capture proof matched every Mannequin add
# (138 of 138) and most Civilian adds (204 of 263) to the placement point within 10 mm;
# Monster adds mostly moved away (305 of 939). Civilian is therefore a Candidate.
STATIONARY_BEHAVIORS = ("Civilian", "Mannequin")

STATIONARY_RULE = (
    "Npc.behaviorType is Civilian or Mannequin for every candidate of the spawner, "
    "npcData is stationary or carries a supported authored civilian patrol. Client-owned staticNpcData "
    "placements are excluded: they use the sector static-NPC lifecycle, not UnitAddNoti."
)
POINT_RULE = (
    "Candidate: the first entry of pointList.points. The official point choice for a "
    "multi-point spawner is unknown; every spawner of map 9012 has exactly one point."
)
CANDIDATE_RULE = (
    "Candidate: the candidate with the highest weight in NpcSpawner.npc, the first listed "
    "on ties. The official weighted selection is unknown; all candidates are listed."
)
COORDINATE_RULE = (
    "x_mm and y_mm are round(placement centimeters * 10), verified against captured wire "
    "positions; z_mm uses the same scale but is only approximate (the server places units "
    "near, not exactly at, the placement height), hence z_candidate."
)
ANIM_STANCE_RULE = (
    "anim_stance_key is the row index of the authored stance key in AnimStanceKeyMapping, the "
    "index the tutorial builder sends. An authored key that differs from the table only in "
    "case (anim_stance_case_folded true) is matched case-insensitively: Candidate. An "
    "authored key with no match fails the run."
)

EXCLUSION_REASONS = (
    "client_static_npc",
    "no_candidates",
    "behavior_not_stationary",
    "npc_roaming",
    "patrol_path",
    "no_motion_data",
    "no_points",
    "unsupported_patrol_mode",
    "unsupported_patrol_speed",
)


def describe(placement, layer):
    return f"spawner {placement.get('id')} (cmsId {placement.get('cmsId')}) in layer {layer}"


def integer_text(value, context, minimum):
    try:
        number = int(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"Invalid integer {value!r}: {context}") from error
    if number < minimum:
        raise ValueError(f"Integer {number} is below {minimum}: {context}")
    return number


def millimeters(value, axis, context):
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(value)
        or abs(value) > MAX_ABSOLUTE_CENTIMETERS
    ):
        raise ValueError(f"Invalid {axis} centimeters {value!r}: {context}")
    return round(value * CENTIMETERS_TO_MILLIMETERS)


def parse_candidates(selector, context):
    candidates = []
    for entry in (part for part in selector["npc"].split(",") if part):
        npc_text, separator, weight_text = entry.partition(":")
        if not separator:
            raise ValueError(f"NpcSpawner.npc entry {entry!r} has no weight: {context}")
        npc_id = integer_text(npc_text, context, 1)
        weight = integer_text(weight_text, context, 1)
        candidates.append({"npc_id": npc_id, "weight": weight})
    return candidates


def choose_candidate(candidates):
    # max() returns the first maximal element, which implements "first listed on ties".
    return max(candidates, key=lambda candidate: candidate["weight"])


def parse_shop_id(row, context):
    try:
        task = json.loads("{" + row["playerTask"] + "}")
        shop_id = task["shopId"]
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        raise ValueError(f"Interaction {row['id']} has an unreadable shop task: {context}") from error
    if isinstance(shop_id, bool) or not isinstance(shop_id, int):
        raise ValueError(f"Interaction {row['id']} has a non-integer shopId: {context}")
    return shop_id


class Catalog:
    """Indexes the catalog tables used by the placement chain."""

    def __init__(self, source):
        self.source = source
        self.npcs = {int(row["id"]): row for row in source.table("Npc")}
        self.selectors = {int(row["id"]): row for row in source.table("NpcSpawner")}
        self.interactions = {}
        for row in source.table("Interaction"):
            self.interactions.setdefault(row["groupId"], []).append(row)
        # The tutorial builder sends AnimStanceKeyMapping row order as the wire index.
        stance_names = [row["animStanceKey"] for row in source.table("AnimStanceKeyMapping")]
        self.stances = {name: index for index, name in enumerate(stance_names)}
        self.stances_folded = {name.casefold(): index for index, name in enumerate(stance_names)}
        if len(self.stances_folded) != len(self.stances):
            raise ValueError("AnimStanceKeyMapping has keys that differ only by case")

    def selector(self, placement, context):
        selector = self.selectors.get(placement["cmsId"])
        if selector is None:
            raise ValueError(f"NpcSpawner row {placement['cmsId']} is missing: {context}")
        return selector

    def npc(self, npc_id, context):
        npc = self.npcs.get(npc_id)
        if npc is None:
            raise ValueError(f"Npc row {npc_id} is missing: {context}")
        return npc

    def stance_index(self, name, context):
        """Return (index, case_folded) for an authored stance key, or (None, False) if empty.

        Authored keys sometimes differ in case from the mapping table (for example stand_34
        against Stand_34); the match is then made case-insensitively, which is Candidate.
        """
        if not name:
            return None, False
        if name in self.stances:
            return self.stances[name], False
        folded = self.stances_folded.get(name.casefold())
        if folded is None:
            raise ValueError(f"animStanceKey {name!r} is not in AnimStanceKeyMapping: {context}")
        return folded, True

    def interactions_for(self, npc, context):
        group_id = npc["interactionGroupId"]
        if group_id in ("", "0"):
            return []
        rows = self.interactions.get(group_id)
        if not rows:
            raise ValueError(
                f"Npc {npc['id']} interactionGroupId {group_id} has no Interaction row: {context}"
            )
        entries = []
        for row in sorted(rows, key=lambda item: int(item["id"])):
            entry = {"interaction_id": int(row["id"]), "task_case": row["playerTaskCase"]}
            if row["playerTaskCase"] == "Shop":
                entry["shop_id"] = parse_shop_id(row, context)
            entries.append(entry)
        return entries


def stationary_basis(placement):
    """Return how the placement shows no patrol, or the exclusion reason."""
    if "npcData" not in placement:
        return None, "no_motion_data"
    if placement["npcData"].get("pathId"):
        return None, "patrol_path"
    return "npc_data_without_path", None


def build_placement(placement, layer, sector, catalog, exclusions, paths=None,
                    missing_path_stationary=False):
    """Builds one server-owned civilian placement, or None when it is excluded.

    `missing_path_stationary` (CUSTOM, --all-maps) keeps an NPC whose authored patrol path is
    absent from the export standing at its first point instead of failing the map.
    """
    # The client resolves these authored actors through its static-NPC manager. Announcing
    # another ordinary NPC for the same placement creates a second actor (for example Gus).
    # Check ownership before catalog selection, including records carrying both data blocks.
    if "staticNpcData" in placement:
        exclusions["client_static_npc"] += 1
        return None

    context = describe(placement, layer)
    selector = catalog.selector(placement, context)
    candidates = parse_candidates(selector, context)
    if not candidates:
        exclusions["no_candidates"] += 1
        return None

    npcs = [catalog.npc(candidate["npc_id"], context) for candidate in candidates]
    for candidate, npc in zip(candidates, npcs):
        candidate["behavior"] = npc["behaviorType"]
    if any(npc["behaviorType"] not in STATIONARY_BEHAVIORS for npc in npcs):
        exclusions["behavior_not_stationary"] += 1
        return None
    path_id = placement.get("npcData", {}).get("pathId")
    roaming_stands = False
    if any(npc["isRoaming"] == "True" for npc in npcs) and not path_id:
        if not missing_path_stationary:
            exclusions["npc_roaming"] += 1
            return None
        # CUSTOM (--all-maps): roaming without an authored path is not simulated; the NPC
        # stands at its first point so quest givers and turn-in NPCs (Ruby 1001174) remain
        # reachable.
        roaming_stands = True

    basis, reason = stationary_basis(placement)
    if reason and not path_id:
        exclusions[reason] += 1
        return None

    points = placement.get("pointList", {}).get("points", [])
    if not points:
        exclusions["no_points"] += 1
        return None
    point = points[0]
    yaw = point["yaw"]
    if isinstance(yaw, bool) or not isinstance(yaw, (int, float)) or not math.isfinite(yaw):
        raise ValueError(f"Invalid yaw {yaw!r}: {context}")

    chosen = choose_candidate(candidates)
    npc = catalog.npc(chosen["npc_id"], context)
    motion = placement["npcData"]
    stance_name = motion.get("animStanceKey", "")
    stance_index, stance_case_folded = catalog.stance_index(stance_name, context)
    result = {
        "spawner_guid": placement["id"],
        "spawner_guid_wire": wire_guid(placement["id"]),
        "spawner_cms_id": placement["cmsId"],
        "layer": layer,
        "sector": {"x": sector["x"], "y": sector["y"]},
        "npc_cms_id": chosen["npc_id"],
        "npc_template_id": integer_text(npc["FK_template"], context, 1),
        "behavior": npc["behaviorType"],
        "faction": npc["faction"],
        "interaction_group_id": integer_text(npc["interactionGroupId"] or 0, context, 0),
        "interactions": catalog.interactions_for(npc, context),
        "position_mm": {
            "x": millimeters(point["x"], "x", context),
            "y": millimeters(point["y"], "y", context),
            "z": millimeters(point["z"], "z", context),
        },
        "z_candidate": True,
        "yaw_degrees": yaw,
        "point_index": 0,
        "point_count": len(points),
        "stationary_basis": "roaming_without_path" if roaming_stands else basis,
        "anim_stance_key_name": stance_name,
        "anim_stance_key": stance_index,
        "anim_stance_case_folded": stance_case_folded,
        "candidates": candidates,
        "active": selector["activationState"] == "True",
        "one_time_use": selector["oneTimeUse"] == "True",
        "min_population": integer_text(selector["minPopulation"], context, 0),
        "max_population": integer_text(selector["maxPopulation"], context, 0),
        "spawn_delay_min_ms": integer_text(selector["spawnDelayMinMs"], context, 0),
        "spawn_delay_max_ms": integer_text(selector["spawnDelayMaxMs"], context, 0),
    }
    if path_id:
        if paths is None or path_id not in paths:
            if not missing_path_stationary:
                raise ValueError(f"NPC patrol path is missing: {context}/{path_id}")
            result["stationary_basis"] = "missing_patrol_path"
            return result
        patrol, reason = build_patrol(catalog.source, npc, placement, paths[path_id], context)
        if reason:
            if not missing_path_stationary:
                exclusions[reason] += 1
                return None
            # CUSTOM (--all-maps): a patrol this server cannot follow leaves the NPC standing at
            # its first point, so quest NPCs on it (couple 1002094) stay reachable.
            exclusions[reason] += 1
            result["stationary_basis"] = "unsupported_patrol_stands"
            return result
        result["stationary_basis"] = "authored_patrol"
        result["patrol"] = patrol
        result["position_mm"] = patrol["nodes"][0]["position_mm"]
    return result


def layer_names(map_row):
    return [name for name in map_row["runtimeDataLayerPath"].strip("[]").split(",") if name]


def build_map_placements(source, map_id, strict_layers=False, require_layers=True,
                         lenient=False):
    map_row = source.row("Map", map_id)
    world = world_name(map_row)
    catalog = Catalog(source)
    exclusions = {reason: 0 for reason in EXCLUSION_REASONS}
    placements = []
    invalid = []
    present_layers = []
    missing_layers = []
    seen_guids = set()
    total_spawners = 0

    # (layer, directory, area): runtime layers belong to the map. ArcheWorld's always-loaded
    # Static layers (villages, zone content) contribute only placements inside the map area.
    # Dungeon worlds keep their runtime layers below MazeDungeons; story worlds show all of their
    # Static layers on their field map.
    world_folder = world_directory(source, map_row)
    layer_sources = [(layer, MAP_ROOT / world_folder / "DataLayers/Runtime" / layer, None)
                     for layer in layer_names(map_row)]
    if uses_static_world_layers(map_row, STATIC_LAYER_MAP_TYPES):
        area = map_area_cm(map_row)
        layer_sources.extend(("Static/" + directory.name, directory, area)
                             for directory in static_layer_directories(source, world))
    elif uses_own_static_layers(map_row):
        layer_sources.extend(("Static/" + directory.name, directory, None)
                             for directory in static_layer_directories(source, world))

    for layer, directory, area in layer_sources:
        relative = directory / "npc_spawner.json"
        if relative.as_posix() not in source.raw_files:
            missing_layers.append(layer)
            continue
        present_layers.append(layer)
        paths = load_layer_paths(source, directory)
        data = json.loads(source.read(Path("raw") / relative))
        for sector in data["spawnerSectors"]:
            for placement in sector["spawners"]:
                if area is not None and not placement_in_map(placement, area):
                    continue
                total_spawners += 1
                guid = placement["id"]
                if guid in seen_guids:
                    raise ValueError(f"Duplicate spawner GUID {guid} in layer {layer}")
                seen_guids.add(guid)
                try:
                    built = build_placement(
                        placement, layer, sector["sectorIndex"], catalog, exclusions, paths,
                        missing_path_stationary=lenient,
                    )
                except ValueError as error:
                    if not lenient:
                        raise
                    # --all-maps keeps every valid NPC of a map whose authored data also
                    # contains broken placements; each one is recorded with its reason.
                    exclusions["invalid_authored_data"] = (
                        exclusions.get("invalid_authored_data", 0) + 1)
                    invalid.append({"spawner_guid": guid, "reason": str(error)})
                    continue
                if built:
                    placements.append(built)

    if not present_layers:
        if require_layers:
            raise ValueError(f"No npc_spawner.json layer is present for map {map_id}")
        return None
    if strict_layers and missing_layers:
        raise ValueError(f"Map {map_id} layers missing from the export: {missing_layers}")

    placements.sort(key=lambda item: (item["layer"], item["spawner_guid"]))
    profile = {
        "format_version": FORMAT_VERSION,
        "map_id": map_id,
        "map_type": map_row["type"],
        "world": world,
        "rules": {
            "stationary": STATIONARY_RULE,
            "stationary_behaviors": list(STATIONARY_BEHAVIORS),
            "point_choice": POINT_RULE,
            "candidate_choice": CANDIDATE_RULE,
            "coordinates": COORDINATE_RULE,
            "yaw": "Degrees exactly as authored in the placement.",
            "anim_stance": ANIM_STANCE_RULE,
        },
        "layers_present": present_layers,
        "layers_missing": missing_layers,
        "spawner_total": total_spawners,
        "placement_total": len(placements),
        "active_total": sum(1 for item in placements if item["active"]),
        "patrol_total": sum(1 for item in placements if "patrol" in item),
        "excluded": exclusions,
        "anim_stance_case_folded_total": sum(
            1 for item in placements if item["anim_stance_case_folded"]),
        "placements": placements,
    }
    if invalid:
        profile["invalid_placements"] = invalid
    return profile


def source_hashes(source):
    return {name: digest for name, digest in sorted(source.sources.items())}


def render(profile):
    return json.dumps(profile, indent=2, sort_keys=False) + "\n"


def write_profile(path, profile):
    path.parent.mkdir(parents=True, exist_ok=True)
    data = render(profile).encode("utf-8")
    path.write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def all_population_map_ids(source):
    """Every public map that can host a shared NPC population.

    Private tutorial maps keep their instance-owned NPCs; developer test worlds are not part
    of the playable build.
    """
    map_ids = []
    for row in source.table("Map"):
        world = world_name(row)
        if (int(row["id"]) in PRIVATE_TUTORIAL_MAPS
                or row.get("package") not in PUBLIC_MAP_PACKAGES
                or world in DEVELOPER_TEST_WORLDS):
            continue
        map_ids.append(int(row["id"]))
    return sorted(map_ids)


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--map-id", type=int, action="append", dest="map_ids")
    parser.add_argument("--all-maps", action="store_true",
                        help="export every map with placement data; maps without any are skipped")
    parser.add_argument("--output-dir", type=Path, default=Path("data/generated/npc/placements"))
    parser.add_argument("--strict-layers", action="store_true",
                        help="fail when a layer listed by the Map row has no placement file")
    options = parser.parse_args(arguments)

    from build_tutorial_data import Source

    source = Source(options.source)
    if options.all_maps:
        map_ids = all_population_map_ids(source)
    else:
        map_ids = options.map_ids or [DEFAULT_MAP_ID]
    for map_id in map_ids:
        try:
            profile = build_map_placements(source, map_id, options.strict_layers,
                                           require_layers=not options.all_maps,
                                           lenient=options.all_maps)
        except ValueError as error:
            if not options.all_maps:
                raise
            # One map's inconsistent authored data must not withhold every other map's NPCs.
            # The skip is reported, never silent.
            print(json.dumps({"map_id": map_id, "skipped": str(error)}))
            continue
        if profile is None or (options.all_maps and not profile["placements"]):
            continue
        profile["source_sha256"] = source_hashes(source)
        output = options.output_dir / f"map-{map_id}.json"
        digest = write_profile(output, profile)
        print(json.dumps({
            "map_id": map_id,
            "output": output.as_posix(),
            "sha256": digest,
            "spawners": profile["spawner_total"],
            "placements": profile["placement_total"],
            "excluded": profile["excluded"],
            "layers_missing": len(profile["layers_missing"]),
            "anim_stance_case_folded": profile["anim_stance_case_folded_total"],
        }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
