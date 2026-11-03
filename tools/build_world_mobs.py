"""Export world monster placements from the hash-verified client data.

Weighted choice, population policy and uncaptured stats are explicit local policies.
Never copy official runtime identities into the population.
"""

import argparse
import copy
import hashlib
import json
import math
import re
from pathlib import Path

from build_combat_data import OBSERVATIONS, build as build_combat
from build_gathering_data import handling_rewards
from build_quest_catalog import fragment as lenient_fragment
from build_stationary_npc_placements import parse_candidates
from build_tutorial_data import BUILD_ID, Source, write_profile
from npc_patrol_data import build_patrol, load_layer_paths, model_speeds
from tutorial_spawns import MAP_ROOT, npc_ai_definition, position_mm, wire_guid
from world_layers import (fragment, map_area_cm, placement_in_map, runtime_layer_names,
                          static_layer_directories, uses_own_static_layers,
                          uses_static_world_layers, world_directory, world_name)


FIRST_MOB_ID = (1 << 59) | 2
MAX_POPULATION = 64
PRIVATE_TUTORIAL_MAPS = {8000, 10117}


def weighted_candidate(candidates, identity):
    total = sum(candidate["weight"] for candidate in candidates)
    draw = int.from_bytes(hashlib.sha256(identity.encode()).digest()[:8], "little") % total
    for candidate in candidates:
        draw -= candidate["weight"]
        if draw < 0:
            return candidate["npc_id"]
    raise ValueError("Empty world monster candidate list")


def group_choices(selector):
    """NpcSpawner.npcGroup lists weighted NpcGroup ids ("5002404:1"), like npc candidates. Each
    spawned slot draws one group and spawns every member of it."""
    choices = []
    for part in filter(None, (entry.strip() for entry in selector["npcGroup"].split(","))):
        group_id, _, weight = part.partition(":")
        choices.append({"npc_id": group_id.strip(), "weight": int(weight or 1)})
    if any(choice["weight"] <= 0 for choice in choices):
        raise ValueError(f"Invalid NpcSpawner group weight: {selector['id']}")
    return choices


def local_stats(level):
    """CUSTOM: a playable fallback, not recovered official NPC stat formulas."""
    if not 1 <= level <= 1000:
        raise ValueError(f"Invalid monster level: {level}")
    health = 150 + level * 75
    return {"18": health, "69": health, "19": 100, "72": 100,
            "22": 10 + level * 4, "17": 10 + level * 2, "84": 1000}


def local_roaming_route(source, npc, spawn, identity):
    """CUSTOM: short idle walks for roaming-enabled monsters without an authored path."""
    if npc.get("isRoaming") != "True":
        return None

    models = {row["id"]: row for row in source.table("UnitModel")}
    groups = {row["id"]: row for row in source.table("UnitGroupModel")}
    group = groups[models[npc["FK_modelId"]]["fk_UnitGroupModelId"]]
    if float(group["walkSpeed"]) == 0:
        # Stationary/aerial models have no walking locomotion to announce.
        return None
    speeds, _ = model_speeds(source, npc, identity)
    leash_mm = int(npc["returnStartDist"]) * 10
    radius_mm = min(3000, leash_mm // 4)
    if radius_mm < 100:
        return None

    seed = hashlib.sha256(identity.encode()).digest()
    angle = int.from_bytes(seed[:2], "little") / 65536 * math.tau
    home = spawn["position"]
    nodes = []
    for index, direction in enumerate((None, angle, None, angle + 2.1)):
        point = list(home)
        if direction is not None:
            point[0] += round(math.cos(direction) * radius_mm)
            point[1] += round(math.sin(direction) * radius_mm)
        if any(not -(2 ** 31) <= coordinate < 2 ** 31 for coordinate in point):
            raise ValueError(f"Roaming position overflows WorldPos: {identity}")
        nodes.append({
            "position_mm": dict(zip("xyz", point)),
            "speed_mm_per_second": speeds["EX5MoveSpeedType_Walk"],
            "move_detail": "MoveDetail_Walk",
            "wait_ms": 2000 + seed[index] * 10,
        })
    return {
        "mode": "circle_forward",
        "nodes": nodes,
        "limits": "CUSTOM: bounded home-centred idle walks at spawn height; no navigation "
                  "or terrain projection. Only NPCs with isRoaming=True use this fallback.",
    }


def phase_attack_skills(source, template):
    relative = Path("X5/Content/Data/BuiltData/Npc/PhaseSkill") / (
        template["phaseSkill"] + ".phaseskill"
    )
    if relative.as_posix() not in source.raw_files:
        return []
    text = source.read(Path("raw") / relative).decode("utf-8-sig")
    # Authored .phaseskill files allow comments, trailing commas and bare property names.
    # Normalize those extensions while preserving quoted strings; other malformed syntax fails.
    tokens = r'"(?:\\.|[^"\\])*"|//[^\r\n]*|/\*[\s\S]*?\*/|,(?=\s*[}\]])'
    normalized = re.sub(tokens, lambda match: match[0] if match[0].startswith('"') else "", text)
    normalized = re.sub(
        r'"(?:\\.|[^"\\])*"|[A-Za-z_][A-Za-z0-9_]*(?=\s*:)',
        lambda match: match[0] if match[0].startswith('"') else json.dumps(match[0]),
        normalized,
    )
    try:
        document = json.loads(normalized)
    except json.JSONDecodeError as error:
        raise ValueError(f"Invalid phase skill {relative}: {normalized[error.pos:error.pos + 100]}") from error
    skills = set()

    def visit(nodes):
        if isinstance(nodes, dict):
            nodes = [nodes]
        for node in nodes:
            skill_id = int(node.get("skillId", 0))
            if skill_id > 0:
                skills.add(skill_id)
            visit(node.get("nextSkills", []))

    for phase in document["phases"]:
        for group in phase["phaseSkills"]:
            if group["phaseSkillType"] == "Offense":
                visit(group["skillTree"])
    return sorted(skills)


def quest_hunt_targets(source):
    """Npc ids that some quest's (Ethereal)HuntMonster objective asks the player to defeat:
    a named template (npcId), a template list (npcIds, job board hunts) or every member of a
    named NpcGroup (npcGroupId)."""
    group_members = {}
    for member in source.table("NpcGroupMember"):
        group_members.setdefault(member["FK_npcGroup"], set()).add(int(member["FK_npc"]))
    targets = set()
    for row in source.table("QuestProgress"):
        if row.get("typeCase") not in ("HuntMonster", "EtherealHuntMonster"):
            continue
        parameters = lenient_fragment(row["type"])
        npc_id = int(parameters.get("npcId", 0))
        if npc_id:
            targets.add(npc_id)
        targets.update(int(listed) for listed in parameters.get("npcIds", []))
        group_id = parameters.get("npcGroupId")
        if group_id is not None:
            targets.update(group_members.get(str(int(group_id)), set()))
    return targets


def spawn_exclusion(selector, placement, candidates, group_members, hunt_targets):
    """Returns (exclusion reason or None, whether a quest target keeps it active).

    CUSTOM: quest phases activate some inactive spawners whose monsters are HuntMonster
    objectives. Without a server-side quest phase those monsters would never appear, so such
    spawners stay present and their quests remain completable. Other inactive spawners still
    await their scripts.
    """
    quest_activated = False
    if selector["activationState"] != "True":
        spawner_npcs = {choice["npc_id"] for choice in candidates}
        spawner_npcs.update(int(member["FK_npc"]) for member in group_members)
        if not spawner_npcs & hunt_targets:
            return "inactive_requires_script", False
        quest_activated = True
    if "npcData" not in placement:
        return "client_static_or_unsupported", False
    if not placement.get("pointList", {}).get("points"):
        return "missing_points", False
    return None, quest_activated


def build(source):
    npcs = {int(row["id"]): row for row in source.table("Npc")}
    # PS2C_UnitAddNoti for a butcherable monster carries npc.is_handling_loot_candidate (captured
    # Alpha Wolf 5001029); the same rewards decide what the server lets a knife butcher.
    butcherable = {reward["npc_cms_id"] for reward in handling_rewards(source)[0]}
    selectors = {int(row["id"]): row for row in source.table("NpcSpawner")}
    hunt_targets = quest_hunt_targets(source)
    templates = {row["id"]: row for row in source.table("NpcTemplate")}
    groups = {}
    for member in source.table("NpcGroupMember"):
        groups.setdefault(member["FK_npcGroup"], []).append(member)
    stances = {row["animStanceKey"].casefold(): index
               for index, row in enumerate(source.table("AnimStanceKeyMapping"))}
    maps = []
    used_npcs = set()
    serial = 0
    for map_row in sorted(source.table("Map"), key=lambda row: int(row["id"])):
        map_id = int(map_row["id"])
        if map_id in PRIVATE_TUTORIAL_MAPS:
            continue
        world = world_name(map_row)
        layers = runtime_layer_names(map_row)
        # (directory, area): runtime layers belong to the map; always-loaded Static world
        # layers contribute only the placements inside the map's authored area. Dungeon worlds
        # keep their runtime layers below MazeDungeons.
        layer_sources = [(MAP_ROOT / world_directory(source, map_row) / "DataLayers/Runtime" / layer,
                          None)
                         for layer in layers]
        if uses_static_world_layers(map_row):
            area = map_area_cm(map_row)
            layer_sources.extend((directory, area)
                                 for directory in static_layer_directories(source, world))
        elif uses_own_static_layers(map_row):
            # Story and interaction test worlds author their monsters in Static layers; their
            # field map shows all of them.
            layer_sources.extend((directory, None)
                                 for directory in static_layer_directories(source, world))
        spawns = []
        missing = []
        excluded = {}
        seen = set()
        for directory, area in layer_sources:
            relative = directory / "npc_spawner.json"
            if relative.as_posix() not in source.raw_files:
                missing.append(directory.name)
                continue
            placement_data = json.loads(source.read(Path("raw") / relative))
            paths = load_layer_paths(source, directory)
            placements = [placement for sector in placement_data["spawnerSectors"]
                          for placement in sector["spawners"]
                          if area is None or placement_in_map(placement, area)]
            for placement in sorted(placements, key=lambda row: row["id"]):
                identity = placement["id"]
                if identity in seen:
                    continue
                seen.add(identity)
                selector = selectors.get(placement["cmsId"])
                if selector is None:
                    excluded["missing_selector"] = excluded.get("missing_selector", 0) + 1
                    continue
                candidates = parse_candidates(selector, identity)
                groups_of_spawner = group_choices(selector)
                group_members = [member for choice in groups_of_spawner
                                 for member in groups.get(choice["npc_id"], [])]
                monster_candidates = [choice for choice in candidates
                                      if npcs.get(choice["npc_id"], {}).get("behaviorType") == "Monster"]
                if not monster_candidates and not any(
                    npcs.get(int(member["FK_npc"]), {}).get("behaviorType") == "Monster"
                    for member in group_members
                ):
                    continue
                reason, quest_activated = spawn_exclusion(
                    selector, placement, candidates, group_members, hunt_targets)
                if reason:
                    excluded[reason] = excluded.get(reason, 0) + 1
                    continue
                # CUSTOM: the server has no game clock for day/night spawners, so time-gated
                # monsters stay present instead of never appearing.
                time_gated = bool(selector["startGameTime"] or selector["endGameTime"])
                population = int(selector["maxPopulation"])
                minimum = int(selector["minPopulation"])
                if not 0 <= minimum <= population <= MAX_POPULATION:
                    raise ValueError(f"Invalid world monster population: {identity}")
                delay_min = int(selector["spawnDelayMinMs"])
                delay_max = int(selector["spawnDelayMaxMs"])
                if not 0 <= delay_min <= delay_max <= 86400000:
                    raise ValueError(f"Invalid world monster respawn: {identity}")
                points = placement["pointList"]["points"]
                for slot in range(population):
                    members = [(weighted_candidate(candidates, f"{map_id}/{identity}/{slot}"), 0, 0)] \
                        if candidates else []
                    slot_group = []
                    if groups_of_spawner:
                        chosen = weighted_candidate(groups_of_spawner,
                                                    f"{map_id}/{identity}/{slot}/group")
                        slot_group = groups.get(chosen, [])
                    members.extend((int(member["FK_npc"]), float(member["posX"] or 0),
                                    float(member["posY"] or 0)) for member in slot_group)
                    for member_index, (npc_id, offset_x, offset_y) in enumerate(members):
                        npc = npcs[npc_id]
                        if npc["behaviorType"] != "Monster":
                            continue
                        point = dict(points[slot % len(points)])
                        yaw = float(point["yaw"])
                        if not math.isfinite(yaw):
                            raise ValueError(f"Invalid world monster yaw: {identity}")
                        angle = math.radians(yaw)
                        point["x"] += offset_x * math.cos(angle) - offset_y * math.sin(angle)
                        point["y"] += offset_x * math.sin(angle) + offset_y * math.cos(angle)
                        serial += 1
                        used_npcs.add(npc_id)
                        spawn = {
                            "unit_id": str(FIRST_MOB_ID + serial * 16),
                            "cms_id": npc_id, "position": position_mm(point), "yaw": yaw,
                            "faction": "EX5Faction_" + npc["faction"],
                            "spawner_guid": wire_guid(identity),
                            "anim_stance_key": stances.get(
                                placement["npcData"]["animStanceKey"].casefold(), 0),
                            "respawn_ms": delay_max,
                            "one_time_use": selector["oneTimeUse"] == "True",
                            "slot": slot, "group_member": member_index,
                        }
                        if time_gated:
                            spawn["game_time_ignored"] = True
                        if quest_activated:
                            spawn["quest_target_activation"] = True
                        if npc_id in butcherable:
                            spawn["handling_loot_candidate"] = True
                        path_id = placement["npcData"]["pathId"]
                        if path_id and path_id in paths and not offset_x and not offset_y:
                            patrol, reason = build_patrol(source, npc, placement, paths[path_id], identity)
                            if patrol:
                                spawn["patrol"] = patrol
                            else:
                                excluded[reason] = excluded.get(reason, 0) + 1
                        elif not path_id:
                            roaming = local_roaming_route(
                                source, npc, spawn, f"{map_id}/{identity}/{slot}/{member_index}"
                            )
                            if roaming:
                                spawn["roaming"] = roaming
                            elif npc.get("isRoaming") == "True":
                                excluded["roaming_unavailable"] = (
                                    excluded.get("roaming_unavailable", 0) + 1
                                )
                        spawns.append(spawn)
        if layer_sources:
            maps.append({"map_id": map_id, "instance_type": map_row["type"],
                         "spawns": spawns, "missing_layers": missing,
                         "excluded": excluded})

    observations = json.loads(OBSERVATIONS.read_text(encoding="utf-8"))
    observations = copy.deepcopy(observations)
    fallback = []
    for npc_id in sorted(used_npcs):
        npc = npcs[npc_id]
        cms = str(npc_id)
        if cms not in observations["npc_stats"]:
            observations["npc_stats"][cms] = local_stats(int(npc["level"]))
            fallback.append(npc_id)
        template = templates[npc["FK_template"]]
        authored = [int(value) for value in re.findall(r"\d+", template["FK_usableSkills"])]
        authored.extend(phase_attack_skills(source, template))
        existing = observations["npc_skills"].get(cms, [])
        observations["npc_skills"][cms] = sorted(set(existing + authored))
    # Reuse the existing action/effect extractor without modifying capture observations.
    from tempfile import TemporaryDirectory
    with TemporaryDirectory(prefix="aac-world-mobs-") as directory:
        observations_path = Path(directory) / "observations.json"
        observations_path.write_text(json.dumps(observations), encoding="utf-8")
        combat = build_combat(source, observations_path)
    return {"format_version": 1, "build_id": BUILD_ID, "maps": maps, "combat": combat,
            "fallback_stat_npcs": fallback, "sources": source.sources,
            "policy": "CUSTOM: maximum population, stable weighted slots, cyclic authored points; "
                      "respawn uses maximum delay; inactive spawners await scripts; time-gated "
                      "spawners stay present without a server game clock; static world layers "
                      "join the public field whose area holds them; uncaptured stats use the "
                      "explicit level model. AI units remain Candidate."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path, default=Path("data/generated/world-mobs/definitions.json"))
    args = parser.parse_args()
    profile = build(Source(args.source))
    write_profile(args.output, profile)
    print(json.dumps({"maps": len(profile["maps"]),
                      "spawns": sum(len(row["spawns"]) for row in profile["maps"]),
                      "fallback_stat_npcs": len(profile["fallback_stat_npcs"]),
                      "npc_types": len(profile["combat"]["npc_stats"])}))


if __name__ == "__main__":
    main()
