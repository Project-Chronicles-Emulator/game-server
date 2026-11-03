"""Build bounded tutorial definitions from the verified local client export.

This output contains static CMS/placement data only. Official character, account,
item-instance, and session identifiers are never copied into the runtime profile.
"""

import argparse
import hashlib
import json
import os
import tempfile
from pathlib import Path

from tutorial_spawns import build_spawn_map, wire_guid
from build_combat_data import build as build_combat
from tutorial_quests import build_quest_definitions
from tutorial_interactions import animation_timing, build_doodads, build_followup_interactions
from tutorial_level_triggers import (
    CAPTURED_BOSS_DEFEATS,
    build_boss_defeats,
    build_cut_scenes,
    build_level_triggers,
    deactivated_doodads,
    mark_unplayable_triggers,
    referenced_cut_scenes,
    spot_placements,
)


BUILD_ID = "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"
SOURCE_SHA256 = "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6"
QUEST_IDS = {10000040, 10000042, 10000043, 10000044, 10000045, 10000106, 90101006}
MAP_PATH = Path("X5/Content/Data/BuiltData/Maps/ArcheWorld_Tutorial/DataLayers/Runtime/Z01_Tutorial")

# Same storage split as corpse loot and the NPC shop: equipment is one item instance per unit,
# while every other tutorial reward stays one WorldProgress stack per CMS ID.
INSTANCE_ITEM_CATEGORY = "Equipment"
# Bounds the item rows one adventure object can grant in a single storage commit.
MAXIMUM_INSTANCE_REWARD_QUANTITY = 16


def fragment(text):
    return json.loads("{" + text + "}") if text else {}


def spot_unit_id(sector, local_id):
    if not 0 <= sector["x"] < 16384 or not 0 <= sector["y"] < 16384:
        raise ValueError("Spot sector is outside the recovered unit-ID layout")
    if not 0 < local_id < 1048576:
        raise ValueError("Spot local ID is outside the recovered unit-ID layout")

    sector_id = sector["x"] | (sector["y"] << 14)
    return str((sector_id << 32) | (local_id << 12) | 6)


class Source:
    def __init__(self, root):
        self.root = root
        self.sources = {}
        manifest = json.loads((root / "catalog/manifest.json").read_text(encoding="utf-8"))
        if manifest["source_executable_dump_sha256"] != SOURCE_SHA256:
            raise ValueError("Tutorial export belongs to an unverified client build")
        self.catalog_files = {entry["path"]: entry for entry in manifest["files"]}
        raw_entries = (root / "raw-manifest.jsonl").read_text(encoding="utf-8").splitlines()
        self.raw_files = {entry["path"]: entry for entry in map(json.loads, raw_entries)}
        self.raw_paths_casefolded = {}
        for path in self.raw_files:
            folded = path.casefold()
            if folded in self.raw_paths_casefolded:
                raise ValueError(f"Ambiguous raw manifest path casing: {path}")
            self.raw_paths_casefolded[folded] = path
        self.tables = {}

    def read(self, relative):
        if relative.parts[0] == "raw":
            # Some authored animation references differ only by letter case from the archive
            # entry. Resolve to the manifest's spelling before reading and hashing, including
            # on case-sensitive build hosts; never bypass manifest verification.
            key = Path(*relative.parts[1:]).as_posix()
            if key not in self.raw_files:
                canonical = self.raw_paths_casefolded.get(key.casefold())
                if canonical is None:
                    raise ValueError(f"Raw source is absent from the verified manifest: {key}")
                relative = Path("raw") / canonical
        data = (self.root / relative).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if relative.parts[0] == "raw":
            expected = self.raw_files[Path(*relative.parts[1:]).as_posix()]
            if expected["status"] != "ok" or digest != expected["sha256"]:
                raise ValueError(f"Raw tutorial source hash mismatch: {relative}")
        self.sources[relative.as_posix()] = digest
        return data

    def table(self, name):
        if name not in self.tables:
            relative = Path("Tables") / (name + ".jsonl")
            data = self.read(Path("catalog") / relative)
            expected = self.catalog_files[relative.as_posix()]
            if hashlib.sha256(data).hexdigest() != expected["sha256"]:
                raise ValueError(f"Catalog hash mismatch: {name}")
            self.tables[name] = [json.loads(line) for line in data.decode("utf-8").splitlines()]
        return self.tables[name]

    def row(self, name, value):
        matches = [row for row in self.table(name) if row.get("id") == str(value)]
        if len(matches) != 1:
            raise ValueError(f"Expected one {name} definition for {value}")
        return matches[0]


def fixed_rewards(source, pack_id):
    """Only the deterministic tutorial loot subset is supported here."""
    if source.row("LootPack", pack_id)["rate"] != "100":
        raise ValueError("Tutorial loot pack is not deterministic")

    rewards = []
    for condition in source.table("LootCondition"):
        if condition["FK_lootPack"] != str(pack_id):
            continue
        if condition["rate"] != "100" or any(condition[key] for key in (
            "worldLevel", "contributionType", "lootingClass", "FK_PlayRecord_Start",
            "FK_PlayRecord_End",
        )):
            raise ValueError("Unsupported tutorial loot condition")
        group = [row for row in source.table("LootItem")
                 if row["lootGroupId"] == condition["lootGroupId"]]
        if len(group) != 1 or group[0]["minAmount"] != group[0]["maxAmount"]:
            raise ValueError("Unsupported random tutorial loot group")
        item = group[0]
        if item["moneyType"] not in ("", "Gold"):
            raise ValueError("Unsupported tutorial currency")
        rewards.append({
            "item_cms_id": int(item["itemId"] or 0),
            "quantity": int(item["minAmount"]),
        })
    if not rewards:
        raise ValueError(f"Empty tutorial loot pack: {pack_id}")
    return rewards


def potion_definitions(source, objects):
    """Recover the drink animation's consumption event for tutorial potions."""
    item_ids = {reward["item_cms_id"] for obj in objects for reward in obj["rewards"]}
    definitions = []
    for item_id in sorted(item_ids - {0}):
        item = source.row("Item", item_id)
        if item["category"] != "Consumable" or item["FK_skillId"] != "1000":
            continue

        skill = source.row("Skill", item["FK_skillId"])
        if skill["skillType"] != "Item" or skill["cooldown"] != "0":
            raise ValueError("Unsupported tutorial potion skill")
        action_path = Path("raw/X5/Content/Data/BuiltData/Action") / (
            skill["actionLayerIds"] + ".json"
        )
        action = json.loads(source.read(action_path))
        layers = action["actionLayers"]
        if len(layers) != 1 or len(layers[0]["actionStages"]) != 1:
            raise ValueError("Unsupported multi-stage potion action")
        stage = layers[0]["actionStages"][0]
        effects = stage["skillEffects"]
        if len(effects) != 1:
            raise ValueError("Unsupported potion effect sequence")
        event = effects[0]
        effect = source.row("SkillEffect", event["skillEffectId"])
        if effect["jsonCase"] != "useItem" or event["conditionalSkillEffects"]:
            raise ValueError("Unsupported potion consumption event")

        definitions.append({
            "item_cms_id": item_id,
            "skill_id": int(skill["id"]),
            "effect_id": int(effect["id"]),
            "execute_time_ms": event["executeTime"],
            "duration_ms": stage["endTime"],
        })
    return definitions


def mark_instance_rewards(source, rewards):
    """Describe equipment rewards as item instances with their catalog grade and durability."""
    for reward in rewards:
        if reward["item_cms_id"] == 0:
            continue

        item = source.row("Item", reward["item_cms_id"])
        if item["category"] != INSTANCE_ITEM_CATEGORY:
            continue

        if not 0 < reward["quantity"] <= MAXIMUM_INSTANCE_REWARD_QUANTITY:
            raise ValueError("Tutorial equipment reward quantity exceeds the instance limit")
        max_durability = int(item["maxDurability"] or 0)
        if max_durability <= 0 or not item["grade"]:
            raise ValueError(f"Tutorial equipment reward lacks grade or durability: {item['id']}")
        reward["instance"] = {"grade": item["grade"], "max_durability": max_durability}
    return rewards


def adventure_interaction(source, interaction_group_id, allow_condition=False):
    """Return the animated-use fields of an adventure object's interaction, if it has one.

    Objects without an interaction animation are activated with InteractUnitReq. Animated
    objects (for example the treasure chest, interaction 300 / animation 100) use the
    StartInteractionAnim/ExecuteInteraction/EndInteractionAnim sequence instead.
    The tutorial has no condition evaluator; the shared adventure exporter exports the
    interaction's conditions itself and passes ``allow_condition``.
    """
    interaction = source.row("Interaction", interaction_group_id)
    if ((interaction["FK_conditionId"] and not allow_condition)
            or interaction["playerTaskCase"] != "Use"):
        raise ValueError(f"Unsupported tutorial adventure interaction: {interaction_group_id}")
    if not interaction["FK_interactionAnimId"]:
        return {}

    animation_id = int(interaction["FK_interactionAnimId"])
    # The chest-opening montage's execute notify differs by up to 86 ms between rigs.
    return {
        "animation_id": animation_id,
        **animation_timing(source, animation_id, allow_rig_variation=True),
    }


def build(source):
    spots = json.loads(source.read(Path("raw") / MAP_PATH / "spot_object.json"))
    volumes = json.loads(source.read(Path("raw") / MAP_PATH / "level_trigger_volume.json"))
    objects = []
    for sector in spots["sectors"]:
        for spot in sector["spotObjects"]:
            for kind in spot["types"]:
                if "adventureObject" not in kind:
                    continue
                cms_id = kind["adventureObject"]["cmsId"]
                definition = source.row("AdventureObject", cms_id)
                if definition["type"] != "LootingObject":
                    continue
                expertise = source.row("ExpertiseReward", definition["FK_expertiseRewardId"])
                if expertise["typeCase"] != "LootPack":
                    raise ValueError("Unsupported tutorial adventure reward")
                pack = fragment(expertise["type"])
                if pack["count"] != 1:
                    raise ValueError("Unsupported repeated tutorial loot pack")
                interaction_group_id = int(definition["interactionGroupId"])
                rewards = fixed_rewards(source, pack["lootPackId"])
                objects.append({
                    "unit_id": spot_unit_id(sector["sectorIndex"], spot["id"]),
                    "sector_id": sector["sectorIndex"]["x"] | (sector["sectorIndex"]["y"] << 14),
                    "local_id": spot["id"],
                    "cms_id": cms_id,
                    "interaction_id": interaction_group_id,
                    "position": [round(spot["position"][axis] * 10) for axis in ("x", "y", "z")],
                    "rewards": mark_instance_rewards(source, rewards),
                    **adventure_interaction(source, interaction_group_id),
                })

    quests = build_quest_definitions(source, 8000, fixed_rewards)

    npc_maps = [build_spawn_map(source, map_id) for map_id in (8000, 10117)]
    initial_npcs = npc_maps[0]
    civilians = [npc for npc in initial_npcs["spawns"] if npc["behavior"] == "Civilian"]

    doodads = build_doodads(source, spots, quests, spot_unit_id)
    boss_defeats = build_boss_defeats(spot_placements(spots), spot_unit_id)
    scripted_doodad_guids = {script["activate_doodad_guid"]
                             for script in CAPTURED_BOSS_DEFEATS.values()}
    doodads.extend(deactivated_doodads(source, spots, spot_unit_id, scripted_doodad_guids))
    doodads.sort(key=lambda value: int(value["unit_id"]))
    level_triggers = build_level_triggers(
        source, 8000, doodads, {quest["cms_id"] for quest in quests},
    )
    cut_scenes = build_cut_scenes(
        source, referenced_cut_scenes(level_triggers, doodads, boss_defeats),
        initial_npcs["spawns"],
    )
    mark_unplayable_triggers(level_triggers, cut_scenes)

    followup_quests = build_quest_definitions(source, 10117, fixed_rewards)
    followup_interactions = build_followup_interactions(source, followup_quests, spot_unit_id)
    followup_triggers = build_level_triggers(
        source, 10117, followup_interactions["doodads"],
        {quest["cms_id"] for quest in followup_quests},
    )
    followup_cut_scenes = build_cut_scenes(
        source, referenced_cut_scenes(followup_triggers, followup_interactions["doodads"], []),
        npc_maps[1]["spawns"],
    )
    mark_unplayable_triggers(followup_triggers, followup_cut_scenes)
    utility_items = [int(row["id"]) for row in source.table("Item")
                     if row["FK_utilityBagFullFillCnt"]]
    potions = potion_definitions(source, objects)
    followup = {
        "format_version": 1,
        "build_id": BUILD_ID,
        "map_id": 10117,
        "quests": followup_quests,
        "npc_spawners": npc_maps[1]["spawners"],
        "npc_spawns": npc_maps[1]["spawns"],
        "civilians": [npc for npc in npc_maps[1]["spawns"] if npc["behavior"] == "Civilian"],
        "utility_items": utility_items,
        "potions": potions,
        "level_triggers": followup_triggers,
        "cut_scenes": followup_cut_scenes,
        **followup_interactions,
    }
    return {
        "format_version": 1, "build_id": BUILD_ID, "map_id": 8000,
        "objects": sorted(objects, key=lambda value: value["cms_id"]),
        "quests": quests, "doodads": doodads,
        "level_triggers": level_triggers,
        "cut_scenes": cut_scenes,
        "boss_defeats": boss_defeats,
        "followup_profiles": [followup],
        "npc_spawners": initial_npcs["spawners"],
        "npc_spawns": initial_npcs["spawns"],
        "npc_maps": npc_maps,
        "civilians": civilians,
        "potions": potions,
        "sectors": sorted({sector["sectorIndex"]["x"] | (sector["sectorIndex"]["y"] << 14)
                           for sector in spots["sectors"]}),
        "utility_items": utility_items,
        "combat": build_combat(source),
        "sources": source.sources,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/tutorial/definitions.json"))
    args = parser.parse_args()
    profile = build(Source(args.source))
    write_profile(args.output, profile)
    print(json.dumps({"objects": len(profile["objects"]), "quests": len(profile["quests"]),
                      "npc_spawners": len(profile["npc_spawners"])}))


def write_profile(output, profile):
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=output.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(json.dumps(profile, ensure_ascii=False, indent=2) + "\n")
        os.replace(temporary, output)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


if __name__ == "__main__":
    main()
