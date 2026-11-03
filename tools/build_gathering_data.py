"""Build shared-world gathering definitions from the verified local client export.

The output contains static CMS and placement data only. No official character,
account, item-instance or session identifier is copied into the runtime profile.

Evidence for the joins below (build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa):

* ``PC2S_StartInteractionAnimReq.target_unit_id`` carries ``sector_id << 32`` and
  ``param.gathering.indices`` carries the placement's per-sector ``index``. The
  sector ID packs ``x | (y << 14)``, the same layout as tutorial spot unit IDs.
* ``gathering.json`` positions are centimetres stored relative to their sector:
  world = stored + (sector index - 1) * 5100 cm. Twelve captured requests were
  0.4-1.9 m from the node resolved this way, with matching gathering animations.
* ``ExpertiseReward.id`` equals ``Gathering.id``. The captured 1006 reward
  (5-7 x 70102001) matches pack 3060101, while every ``Gathering.lootpackId``
  is the placeholder pack 1 and is ignored.
* Looping montages repeat their single loop section ``loopCount - 1`` extra times.
  This reproduces every captured SP-flow duration (1646, 4989 and 6508 ms).
* Thirteen ``Gathering`` IDs have no ``ExpertiseReward`` row. The ``1306xxxx``
  ``ExpertiseReward`` family mirrors the authored gathering rewards one-to-one
  (13060101 is gathering 80's Roseberry pack, 13061101/13061102 the tree packs).
  ``INFERRED_REWARDS`` joins each unauthored ID to that family. Four joins are
  confirmed by official captures; the others are labelled catalog-name candidates.
* Doodad gathering (Roseberry Bush 74, Dark Vein 90, Rock 91, Wild Bush 58000, ...)
  is a separate path: ``StartInteractionAnimReq.param.interaction`` names the
  doodad's ``Interaction`` and ``target_unit_id`` is the spot unit
  ``sector << 32 | local_id << 12 | 6``. ``DoodadPhaseFlow`` supplies the reward
  task and phase graph. All twelve captured doodad gathering targets resolve to
  authored spot doodads 74 and 90 within 0.4-1.4 m of the request position.
* Quest doodads (those quests accept, complete or count interactions with) use the same
  ``StartInteractionAnimReq.param.interaction`` path with their own ``Interaction`` and
  animation; executing one advances the shared phase and reports the quest interaction.
"""

import argparse
import json
import math
import re
from pathlib import Path

from build_quest_catalog import fragment as lenient_fragment
from build_tutorial_data import BUILD_ID, Source, fragment, spot_unit_id, write_profile
from tutorial_spawns import MAP_ROOT, position_mm


FORMAT_VERSION = 2
ARCHEWORLD = "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld"
ARCHEWORLD_LAYER_ROOT = "ArcheWorld"
PLACEMENT_PATH = Path("raw/X5/Content/Data/BuiltData/Maps/ArcheWorld/gathering.json")
ANIMATION_ROOT = Path("raw/X5/Content/Data/BuiltData/Anim/Interaction")
SECTOR_SIZE_CM = 5100
SECTOR_AXIS_LIMIT = 16384
MILLIMETRES_PER_CENTIMETRE = 10
MAXIMUM_ANIMATION_MS = 30000
MAXIMUM_PHASE_DURATION_MS = 24 * 60 * 60 * 1000
# CUSTOM: delay before a quest doodad leaves a phase without a trigger when Doodad.respawnDelay
# is not authored.
DEFAULT_STATIC_PHASE_MS = 60000
# Only demo-package maps are reachable in this client build.
SUPPORTED_PACKAGE = "Demo"
TOOL_EQUIPMENT_TYPES = ("Hatchet", "Knife", "Pick", "Sickle")
# Interaction animations whose InteractionAnim.expertiseType names a harvesting expertise.
GATHERING_EXPERTISE_TYPES = ("Gathering", "Mining", "Logging")
UNSUPPORTED_LOOT_FILTERS = (
    "worldLevel", "contributionType", "publicAble", "lootingClass",
    "FK_PlayRecord_Start", "FK_PlayRecord_End",
)
# GameVariable rows that time the shared SP flow and tool wear. Their captured effect:
# every tool gather lowered the tool by exactly 100 durability, and every SP-flow regen
# checkpoint resumed 300 ms after the animation's end.
TOOL_DURABILITY_VARIABLE = "ToolDurabiliyDecreaseValue"
SP_REGEN_DELAY_VARIABLE = "SPStatRegenDelayMilli"

# Gathering IDs without an authored ExpertiseReward row, joined to the matching 1306xxxx
# ExpertiseReward. "capture" joins are confirmed by official ItemsUpdateNoti rewards of the
# same item and quantity range; "catalog_name" joins match the node name to the reward item
# (or to an authored sibling node with the same name) and remain Candidates.
INFERRED_REWARDS = {
    102: (13061102, "capture"),       # Young Tree: 3 official gathers granted Log 1-2.
    300: (13061611, "capture"),       # Clam: 2 official gathers granted Clam 5.
    301: (13061612, "capture"),       # Sea Mushroom Coral: official Sea Mushroom grants.
    302: (13061610, "capture"),       # Suspicious Rock: official Crab grants 3-5.
    209: (13061101, "catalog_name"),  # Mature Tree, as authored Mature Trees 200-208.
    210: (13061101, "catalog_name"),  # Mature Tree, as authored Mature Trees 200-208.
    50: (13061201, "catalog_name"),   # Wildfiber Herb -> Wild Fiber.
    51: (13061202, "catalog_name"),   # Wildfiber Bloom -> Wild Fiber and Wild Petal.
    71: (13061301, "catalog_name"),   # Bluebell -> Wild Petal, as authored flower Lily 70.
    72: (13061301, "catalog_name"),   # Spireflower -> Wild Petal.
    73: (13061301, "catalog_name"),   # Sundaisy -> Wild Petal.
    74: (13061301, "catalog_name"),   # Yarrow -> Wild Petal.
    15: (13061401, "catalog_name"),   # Rock -> Marble, as doodad Rock 91's mining reward.
}
DOODAD_TASKS = ("LootingPack", "ExpertiseReward", "NoneTask")
DESPAWNED_PHASE = 0
PHASE_RATIO = re.compile(r"^(\d+):(\d+)$")
# Corpse butchering ("handling loot"). Official captures (wavesword-mob-combat-20261026) show
# PC2S_StartInteractionAnimReq{interaction_anim_id 1000, param.handling_loot, target_unit_id =
# the corpse}, then at ExecuteInteractionReq an ItemsUpdateNoti whose items are exactly the
# corpse NPC's Npc.FK_expertiseRewardId loot pack (Alpha Wolf 5001029 -> pack 3104238: Wolf
# Molar 100 %, Primordial Echo 30 %; Wolf 5001030 -> pack 3105178: Raw Pelt 1-3). Animation 1000
# is the only InteractionAnim whose tool is a Knife, and the knife lost 100 durability per use.
HANDLING_ANIMATION_ID = 1000
HANDLING_TOOL_TYPE = "Knife"


def sector_id(sector_index):
    x = sector_index["x"]
    y = sector_index["y"]
    if not 0 <= x < SECTOR_AXIS_LIMIT or not 0 <= y < SECTOR_AXIS_LIMIT:
        raise ValueError("Gathering sector is outside the recovered sector-ID layout")
    return x | (y << 14)


def world_position_mm(sector_index, stored_position):
    """Convert a sector-relative placement (cm) into world millimetres."""
    offsets = {"x": sector_index["x"] - 1, "y": sector_index["y"] - 1, "z": 0}
    position = []
    for axis in ("x", "y", "z"):
        centimetres = stored_position[axis] + offsets[axis] * SECTOR_SIZE_CM
        position.append(round(centimetres * MILLIMETRES_PER_CENTIMETRE))
    return position


def map_areas(source, *, include_housing=False, include_release_fields=False,
              include_release_instances=False):
    """Return authored ArcheWorld bounds for the explicitly supported map types.

    Release fields use the same world coordinates as Demo fields and are reachable through
    map travel. Release channel instances and points of interest (for example 20316 and 30301)
    are ArcheWorld maps too, holding quest doodads. Callers opt in so this does not broaden
    unrelated exporters implicitly.
    """
    maps = []
    for row in source.table("Map"):
        housing_map = (include_housing and row.get("package") == "Release"
                       and row.get("type") == "HousingArea")
        release_field = (include_release_fields and row.get("package") == "Release"
                         and row.get("type") == "Field")
        release_instance = (include_release_instances and row.get("package") == "Release"
                            and row.get("type") in ("Instance", "PointOfInterest")
                            and bool(row.get("area")))
        if row["world"] != ARCHEWORLD or (
                row["package"] != SUPPORTED_PACKAGE and not housing_map and not release_field
                and not release_instance):
            continue
        if not row["area"]:
            raise ValueError(f"Demo ArcheWorld map has no area: {row['id']}")
        area = fragment(row["area"])
        bounds = {}
        for corner in ("min", "max"):
            bounds[corner] = [
                round(area[corner][axis] * MILLIMETRES_PER_CENTIMETRE) for axis in ("x", "y", "z")
            ]
        if any(low > high for low, high in zip(bounds["min"], bounds["max"])):
            raise ValueError(f"Inverted map area: {row['id']}")
        maps.append({"map_id": int(row["id"]), "area_min_mm": bounds["min"],
                     "area_max_mm": bounds["max"]})
    if not maps:
        raise ValueError("No demo ArcheWorld maps were found")
    return sorted(maps, key=lambda entry: entry["map_id"])


def inside_any_map(position, maps):
    for entry in maps:
        low = entry["area_min_mm"]
        high = entry["area_max_mm"]
        if all(low[axis] <= position[axis] <= high[axis] for axis in range(3)):
            return True
    return False


def looped_animation_timing(source, animation, earliest_across_rigs=False):
    """Return the server execute/duration times for one looping interaction montage.

    Gathering montages must time identically on every character rig. Quest doodad montages may
    differ per rig; with ``earliest_across_rigs`` the earliest execute and the longest duration
    are used, so the server never refuses an execute the client's rig timed correctly."""
    loop_count = int(animation["loopCount"])
    if not 1 <= loop_count <= 10:
        raise ValueError(f"Unsupported interaction loop count: {animation['id']}")

    details = [row for row in source.table("InteractionAnimDetail")
               if row["id"] == animation["animId"]]
    if not details:
        raise ValueError(f"Interaction animation has no montage: {animation['id']}")

    timings = set()
    for detail in details:
        document = json.loads(source.read(ANIMATION_ROOT / (detail["gameAnimKey"] + ".json")))
        events = [event for event in document["commonActStage"]["notifies"]
                  if event["notifyType"] == "interaction_execute"]
        if len(events) != 1:
            raise ValueError(f"Unsupported interaction event sequence: {animation['id']}")

        execute_ms = events[0]["startTime"]
        duration_ms = document["endTime"]
        loops = [section for section in document.get("sections", []) if section["isLoop"]]
        if len(loops) > 1:
            raise ValueError(f"Several loop sections are unsupported: {animation['id']}")
        if loops and loop_count > 1:
            loop = loops[0]
            loop_length_ms = loop["endTime"] - loop["startTime"]
            if loop_length_ms <= 0 or loop["startTime"] < execute_ms < loop["endTime"]:
                raise ValueError(f"Execute event inside a loop is unsupported: {animation['id']}")
            repeated_ms = loop_length_ms * (loop_count - 1)
            duration_ms += repeated_ms
            if execute_ms >= loop["endTime"]:
                execute_ms += repeated_ms

        maximum_ms = MAXIMUM_ANIMATION_MS
        if earliest_across_rigs and execute_ms == duration_ms:
            # Some quest montages (warp 8) execute on their last frame; the server only needs
            # the execute time and a later end.
            duration_ms += 1
            maximum_ms += 1
        if not 0 < execute_ms < duration_ms <= maximum_ms:
            raise ValueError(f"Invalid interaction timing: {animation['id']}")
        timings.add((execute_ms, duration_ms))

    if len(timings) != 1:
        if not earliest_across_rigs:
            raise ValueError(
                f"Interaction timing differs across character rigs: {animation['id']}")
        return min(execute for execute, _ in timings), max(duration for _, duration in timings)
    execute_ms, duration_ms = timings.pop()
    return execute_ms, duration_ms


def animation_definition(source, animation_id, earliest_across_rigs=False):
    animation = source.row("InteractionAnim", animation_id)
    if animation["costType"] not in ("SP", "None"):
        raise ValueError(f"Unsupported gathering cost type: {animation_id}")
    if animation["toolEquipmentType"] not in ("None", *TOOL_EQUIPMENT_TYPES):
        raise ValueError(f"Unsupported gathering tool type: {animation_id}")

    execute_ms, duration_ms = looped_animation_timing(source, animation, earliest_across_rigs)
    return {
        "interaction_anim_id": animation_id,
        "sp_cost": int(animation["costValue"] or 0) if animation["costType"] == "SP" else 0,
        "tool_equipment_type": animation["toolEquipmentType"],
        "expertise_type": animation["expertiseType"],
        "execute_time_ms": execute_ms,
        "duration_ms": duration_ms,
    }


def loot_groups(source, pack_id):
    """Return a loot pack's groups, failing closed on filters this server does not model."""
    groups = []
    for condition in source.table("LootCondition"):
        if condition["FK_lootPack"] != str(pack_id):
            continue
        if any(condition[key] for key in UNSUPPORTED_LOOT_FILTERS):
            raise ValueError(f"Unsupported loot condition filter in pack {pack_id}")
        rate = float(condition["rate"])
        if not math.isfinite(rate) or not 0 < rate <= 100:
            raise ValueError(f"Invalid loot condition rate in pack {pack_id}")

        items = []
        for row in source.table("LootItem"):
            if row["lootGroupId"] != condition["lootGroupId"]:
                continue
            if row["moneyType"] or row["grade"] or row["polishStep"]:
                raise ValueError(f"Unsupported currency/grade loot in pack {pack_id}")
            minimum = int(row["minAmount"])
            maximum = int(row["maxAmount"])
            weight = int(row["dropWeight"])
            if not 0 < minimum <= maximum or weight <= 0:
                raise ValueError(f"Invalid loot amount in pack {pack_id}")
            items.append({"item_cms_id": int(row["itemId"]), "min_quantity": minimum,
                          "max_quantity": maximum, "weight": weight})
        if not items:
            raise ValueError(f"Empty loot group in pack {pack_id}")
        groups.append({"rate_percent": rate, "items": items})
    if not groups:
        raise ValueError(f"Empty loot pack: {pack_id}")
    return groups


def loot_pack_reward(source, pack_id):
    """Resolve one loot pack into its rate and groups."""
    pack_rate = float(source.row("LootPack", pack_id)["rate"])
    if not math.isfinite(pack_rate) or not 0 < pack_rate <= 100:
        raise ValueError(f"Invalid loot pack rate: {pack_id}")
    return {
        "loot_pack_id": pack_id,
        "pack_rate_percent": pack_rate,
        "groups": loot_groups(source, pack_id),
    }


def expertise_reward(source, reward_id):
    """Resolve an ExpertiseReward ID into its single loot pack, or None when it has none."""
    rows = [row for row in source.table("ExpertiseReward") if row["id"] == str(reward_id)]
    packs = [fragment(row["type"]) for row in rows if row["typeCase"] == "LootPack"]
    experience = [fragment(row["type"])["value"] for row in rows if row["typeCase"] == "Exp"]
    if not packs:
        return None
    if len(packs) != 1 or packs[0]["count"] != 1 or len(experience) > 1:
        raise ValueError(f"Unsupported repeated expertise reward: {reward_id}")

    reward = loot_pack_reward(source, packs[0]["lootPackId"])
    # Recorded for provenance; no captured packet shows this value being granted.
    reward["expertise_experience"] = experience[0] if experience else 0
    return reward


def gathering_reward(source, gathering_id):
    """Resolve a gathering node's reward, or return a reason when none can be joined."""
    reward = expertise_reward(source, gathering_id)
    if reward is not None:
        reward["reward_source"] = "authored"
        return reward, None

    inferred = INFERRED_REWARDS.get(gathering_id)
    if inferred is None:
        return None, "No ExpertiseReward loot pack is authored for this gathering ID"

    reward_id, evidence = inferred
    reward = expertise_reward(source, reward_id)
    if reward is None:
        raise ValueError(f"Inferred gathering reward {reward_id} has no loot pack")
    reward["reward_source"] = evidence
    reward["expertise_reward_id"] = reward_id
    return reward, None


def gathering_definitions(source):
    definitions = []
    unsupported = []
    animations = {}
    for row in source.table("Gathering"):
        gathering_id = int(row["id"])
        if row["activationState"] != "True" or row["remainActivateCount"] != "-1":
            raise ValueError(f"Unsupported finite or inactive gathering node: {gathering_id}")

        minimum_delay = int(row["activateDelayMinMs"])
        maximum_delay = int(row["activateDelayMaxMs"])
        deactivate_delay = int(row["deactivateDelayMs"])
        if not 0 < minimum_delay <= maximum_delay or deactivate_delay < 0:
            raise ValueError(f"Invalid gathering respawn delay: {gathering_id}")

        animation_id = int(row["FK_interactionAnimId"])
        if animation_id not in animations:
            animations[animation_id] = animation_definition(source, animation_id)

        reward, reason = gathering_reward(source, gathering_id)
        if reward is None:
            unsupported.append({"gathering_id": gathering_id, "reason": reason})
            continue
        definitions.append({
            "gathering_id": gathering_id,
            "interaction_anim_id": animation_id,
            "deactivate_delay_ms": deactivate_delay,
            "activate_delay_min_ms": minimum_delay,
            "activate_delay_max_ms": maximum_delay,
            **reward,
        })
    return definitions, unsupported, sorted(animations.values(),
                                            key=lambda entry: entry["interaction_anim_id"])


def handling_rewards(source):
    """Butchering rewards for every monster whose ExpertiseReward authors a loot pack.

    Returns the rewards and the monsters whose reward cannot be modelled (for example a
    filtered or currency loot group); those stay explicit and are never butchered.
    """
    item_categories = {row["id"]: row["category"] for row in source.table("Item")}
    rewards = []
    unsupported = []
    for npc in source.table("Npc"):
        reward_id = npc["FK_expertiseRewardId"]
        if npc["behaviorType"] != "Monster" or not reward_id:
            continue

        npc_cms_id = int(npc["id"])
        try:
            reward = expertise_reward(source, reward_id)
        except ValueError as error:
            unsupported.append({"npc_cms_id": npc_cms_id, "reason": str(error)})
            continue
        if reward is None:
            continue

        equipment = [item["item_cms_id"] for group in reward["groups"] for item in group["items"]
                     if item_categories.get(str(item["item_cms_id"]), "Equipment") == "Equipment"]
        if equipment:
            unsupported.append({"npc_cms_id": npc_cms_id,
                                "reason": f"Butchering reward is not stackable: {equipment}"})
            continue
        rewards.append({"npc_cms_id": npc_cms_id, "expertise_reward_id": int(reward_id), **reward})
    return rewards, unsupported


def handling_animation(source):
    """The corpse butchering animation, checked against its authored knife requirement."""
    definition = animation_definition(source, HANDLING_ANIMATION_ID)
    if definition["tool_equipment_type"] != HANDLING_TOOL_TYPE:
        raise ValueError("The butchering animation no longer requires a knife")
    return definition


def tool_items(source):
    tools = {equipment_type: [] for equipment_type in TOOL_EQUIPMENT_TYPES}
    for row in source.table("Item"):
        equipment_type = row["equipmentType"]
        if equipment_type in tools and row["equipSlot"].startswith("Tool_"):
            tools[equipment_type].append(int(row["id"]))
    for equipment_type, items in tools.items():
        if not items:
            raise ValueError(f"No tool items found for {equipment_type}")
        items.sort()
    return tools


def gathering_points(source, maps, known_gathering_ids):
    document = json.loads(source.read(PLACEMENT_PATH))
    points = []
    for sector in document["gatheringSectors"]:
        sector_index = sector["sectorIndex"]
        packed_sector = sector_id(sector_index)
        seen = set()
        for point in sector["gatheringPoints"]:
            index = point["index"]
            if index in seen or not 0 <= index < 2**32:
                raise ValueError(f"Duplicate or invalid gathering index in sector {packed_sector}")
            seen.add(index)

            gathering_id = point["gatheringDetailRowId"]
            if gathering_id not in known_gathering_ids:
                raise ValueError(f"Placement references unknown gathering ID {gathering_id}")
            position = world_position_mm(sector_index, point["position"])
            if inside_any_map(position, maps):
                points.append([packed_sector, index, gathering_id, *position])
    points.sort()
    return points


def game_variable_ms(source, name):
    """Return one non-negative integer GameVariable, failing closed on missing rows."""
    rows = [row for row in source.table("GameVariable") if row["name"] == name]
    if len(rows) != 1:
        raise ValueError(f"Expected one GameVariable row named {name}")
    value = int(rows[0]["value"])
    if not 0 <= value <= MAXIMUM_ANIMATION_MS:
        raise ValueError(f"GameVariable {name} is outside the supported range")
    return value


def interaction_trigger_id(trigger):
    """DoodadPhaseFlow exports interaction triggers as '"id":N' and, in some rows, 'id:N'."""
    if trigger.startswith('"'):
        return int(fragment(trigger)["id"])
    if trigger.startswith("id:"):
        return int(trigger.removeprefix("id:"))
    raise ValueError(f"Unsupported doodad interaction trigger: {trigger}")


def next_phases(flow):
    """Return the weighted next phases of one doodad flow as [[phase_id, weight], ...]."""
    if flow["nextPhaseId"]:
        if flow["nextPhaseIdRatio"]:
            raise ValueError("A doodad flow names both a next phase and a ratio")
        return [[int(flow["nextPhaseId"]), 1]]

    choices = []
    for part in flow["nextPhaseIdRatio"].split(","):
        match = PHASE_RATIO.match(part.strip())
        if match is None:
            raise ValueError(f"Unsupported doodad phase ratio: {flow['nextPhaseIdRatio']}")
        choices.append([int(match.group(1)), int(match.group(2))])
    if not choices or any(weight <= 0 for _, weight in choices):
        raise ValueError(f"Invalid doodad phase ratio: {flow['nextPhaseIdRatio']}")
    return choices


def doodad_task_reward(source, flow):
    """Resolve a gathering flow's reward and the ItemsUpdateNoti reason it is reported with.

    Captured: LootingPack doodads (Roseberry Bush 74) report the looting reason naming the
    doodad; ExpertiseReward doodads (Dark Vein 90) report the system reason.
    """
    task = fragment(flow["taskType"])
    if flow["taskTypeCase"] == "LootingPack":
        reward = loot_pack_reward(source, task["id"])
        reward["reason"] = "looting"
        return reward
    if flow["taskTypeCase"] == "ExpertiseReward":
        reward = expertise_reward(source, task["id"])
        if reward is None:
            raise ValueError(f"Doodad expertise reward {task['id']} has no loot pack")
        reward["reason"] = "system"
        return reward
    raise ValueError(f"Unsupported doodad gathering task: {flow['taskTypeCase']}")


def gathering_interaction(source, interaction_id, group_id):
    """Return the animation of a harvesting interaction, or None for other interactions."""
    interaction = source.row("Interaction", interaction_id)
    if not interaction["FK_interactionAnimId"]:
        return None
    animation = source.row("InteractionAnim", interaction["FK_interactionAnimId"])
    if animation["expertiseType"] not in GATHERING_EXPERTISE_TYPES:
        return None
    if (interaction["groupId"] != group_id or interaction["playerTaskCase"] != "Use"
            or interaction["FK_conditionId"] or interaction["activateAdventureObject"]):
        raise ValueError(f"Unsupported doodad gathering interaction: {interaction_id}")
    return int(interaction["FK_interactionAnimId"])


def doodad_phase(source, doodad, flow, phase_rows):
    """Translate one DoodadPhaseFlow row into the runtime phase description."""
    phase_id = int(flow["phaseId"])
    phase = {"phase_id": phase_id, "next": next_phases(flow)}
    if flow["triggerTypeCase"] == "Timer":
        duration_ms = int(fragment(flow["triggerType"])["duration"])
        if not 0 < duration_ms <= MAXIMUM_PHASE_DURATION_MS or flow["taskTypeCase"] != "NoneTask":
            raise ValueError(f"Unsupported timed phase of doodad {doodad}")
        phase.update({"trigger": "timer", "duration_ms": duration_ms})
        return phase

    if flow["triggerTypeCase"] != "Interaction" or flow["interactionCount"] != "1":
        raise ValueError(f"Unsupported trigger of doodad {doodad}")
    if phase_id not in phase_rows:
        raise ValueError(f"Doodad {doodad} has a flow without a phase row")
    interaction_id = interaction_trigger_id(flow["triggerType"])
    animation_id = gathering_interaction(source, interaction_id,
                                         phase_rows[phase_id]["interactionGroupId"])
    if animation_id is None:
        raise ValueError(f"Doodad {doodad} mixes gathering with other interactions")
    phase.update({"trigger": "interaction", "interaction_id": interaction_id,
                  "interaction_anim_id": animation_id,
                  "reward": doodad_task_reward(source, flow)})
    return phase


def is_gathering_doodad(source, flows):
    """True when one interaction of the doodad is a harvesting animation with a reward."""
    interactions = {row["id"]: row for row in source.table("Interaction")}
    animations = {row["id"]: row for row in source.table("InteractionAnim")}
    for flow in flows:
        if (flow["triggerTypeCase"] != "Interaction"
                or flow["taskTypeCase"] not in ("LootingPack", "ExpertiseReward")):
            continue
        interaction = interactions.get(str(interaction_trigger_id(flow["triggerType"])))
        if interaction is None:
            continue
        animation = animations.get(interaction["FK_interactionAnimId"])
        if animation is not None and animation["expertiseType"] in GATHERING_EXPERTISE_TYPES:
            return True
    return False


def doodad_definition(source, doodad, flows, phase_rows):
    """Return a gathering doodad's validated phase graph."""
    phases = []
    for flow in sorted(flows, key=lambda row: int(row["phaseId"])):
        phases.append(doodad_phase(source, doodad, flow, phase_rows))

    phase_ids = {phase["phase_id"] for phase in phases}
    if len(phase_ids) != len(phases) or set(phase_rows) != phase_ids:
        raise ValueError(f"Doodad {doodad} needs exactly one flow per phase")
    despawns = False
    for phase in phases:
        for next_phase, _ in phase["next"]:
            if next_phase == DESPAWNED_PHASE:
                despawns = True
            elif next_phase not in phase_ids:
                raise ValueError(f"Doodad {doodad} names an unknown next phase")

    row = source.row("Doodad", doodad)
    definition = {"doodad_id": doodad, "initial_phase": min(phase_ids), "phases": phases}
    if despawns:
        minimum = int(row["respawnDelayMin"])
        maximum = int(row["respawnDelayMax"])
        if not 0 < minimum <= maximum <= MAXIMUM_PHASE_DURATION_MS:
            raise ValueError(f"Invalid respawn delay of doodad {doodad}")
        definition.update({"respawn_delay_min_ms": minimum, "respawn_delay_max_ms": maximum})
    return definition


def doodad_definitions(source):
    """Return supported gathering and quest doodads, and the ones this server refuses."""
    flows_by_doodad = {}
    for flow in source.table("DoodadPhaseFlow"):
        flows_by_doodad.setdefault(int(flow["FK_doodad"]), []).append(flow)
    phases_by_doodad = {}
    for phase in source.table("DoodadPhase"):
        phases_by_doodad.setdefault(int(phase["FK_doodad"]), {})[int(phase["phaseId"])] = phase
    # The three authored air bubbles use the shared interaction animation and a stamina effect.
    air_bubbles = {11150, 11151, 11152}
    quest_doodads = quest_doodad_ids(source) | portal_doodad_ids(source, flows_by_doodad)
    quest_doodads |= air_bubbles

    definitions = []
    unsupported = []
    # Quest doodads may have no flows at all and offer only their phases' interaction groups.
    candidates = set(flows_by_doodad) | (quest_doodads & set(phases_by_doodad))
    for doodad in sorted(candidates):
        flows = flows_by_doodad.get(doodad, [])
        try:
            if flows and is_gathering_doodad(source, flows):
                definitions.append(
                    doodad_definition(source, doodad, flows, phases_by_doodad.get(doodad, {})))
            elif doodad in quest_doodads:
                definition = quest_doodad_definition(source, doodad, flows,
                                                     phases_by_doodad.get(doodad, {}))
                if definition is not None:
                    definitions.append(definition)
        except ValueError as error:
            unsupported.append({"doodad_id": doodad, "reason": str(error)})
    return definitions, unsupported


def map_move_destination(player_task):
    """Interaction.playerTask of a MapMove interaction: the target Map row and a position in
    centimetres, converted to the millimetres world positions use."""
    task = fragment(player_task)
    position = task["pos"]
    return {"map_id": int(task["mapId"]),
            "x_mm": round(float(position["x"]) * MILLIMETRES_PER_CENTIMETRE),
            "y_mm": round(float(position["y"]) * MILLIMETRES_PER_CENTIMETRE),
            "z_mm": round(float(position["z"]) * MILLIMETRES_PER_CENTIMETRE)}


def group_interaction(source, phase_row):
    """The first Interaction of a DoodadPhase row's interactionGroupId as (id, row), or None."""
    if phase_row is None or not phase_row.get("interactionGroupId"):
        return None
    group = phase_row["interactionGroupId"]
    members = sorted((int(row["id"]), row) for row in source.table("Interaction")
                     if row["groupId"] == group)
    return members[0] if members else None


def portal_doodad_ids(source, flows_by_doodad):
    """Doodads with an interaction that moves the player to another map (portals)."""
    portals = set()
    interactions = {row["id"]: row for row in source.table("Interaction")}
    for doodad, flows in flows_by_doodad.items():
        for flow in flows:
            if flow["triggerTypeCase"] != "Interaction":
                continue
            interaction = interactions.get(str(interaction_trigger_id(flow["triggerType"])))
            if interaction is not None and interaction["playerTaskCase"] == "MapMove":
                portals.add(doodad)
    return portals


def quest_doodad_ids(source):
    """Doodads quests name: InteractDoodad acceptance, Doodad turn-ins and (Ethereal)
    InteractDoodad objectives. Some authored rows leave a key unquoted, hence the lenient
    fragment parser."""
    doodads = set()
    for row in source.table("Quest"):
        for index in range(1, 4):
            if row[f"acceptConditionType{index}Case"] == "InteractDoodad":
                parameters = lenient_fragment(row[f"acceptConditionType{index}"])
                doodads.add(int(float(parameters["doodadId"])))
        if row["questCompleteType"] == "Doodad":
            doodads.add(int(row["questCompleteTypeParams"].split(",")[0]))
    for row in source.table("QuestProgress"):
        if row["typeCase"] in ("InteractDoodad", "EtherealInteractDoodad"):
            doodads.add(int(float(lenient_fragment(row["type"])["doodadId"])))
    return doodads


def quest_doodad_definition(source, doodad, flows, phase_rows):
    """Return a quest doodad's phase graph, or None when no phase can be interacted with.

    Interaction phases keep their animation and any LootingPack/ExpertiseReward reward; other
    tasks (ChangeDoodadPhase, ExecSkillEffect) are not modelled. CUSTOM: phases without a
    trigger (NoneTrigger, NpcSpawnerState, a DoodadPhase row without a flow, an out-of-range
    timer) would freeze a shared doodad for every player after one use, so they return to the
    initial phase after Doodad.respawnDelayMin (60 s without one), like a respawn.
    """
    phases = []
    row = source.row("Doodad", doodad)
    flows_by_phase = {int(flow["phaseId"]): flow for flow in flows}
    phase_ids = set(flows_by_phase) | set(phase_rows)
    initial_phase = min(phase_ids)
    static_delay_ms = int(row["respawnDelayMin"] or 0) or DEFAULT_STATIC_PHASE_MS
    if not 0 < static_delay_ms <= MAXIMUM_PHASE_DURATION_MS:
        raise ValueError(f"Invalid static phase delay of doodad {doodad}")
    static_next = [[initial_phase, 1]]

    for phase_id in sorted(phase_ids):
        flow = flows_by_phase.get(phase_id)
        phase = {"phase_id": phase_id}
        has_next = flow is not None and bool(flow["nextPhaseId"] or flow["nextPhaseIdRatio"])
        trigger = flow["triggerTypeCase"] if flow is not None else "NoneTrigger"
        duration_ms = 0
        if trigger == "Timer" and has_next:
            duration_ms = int(fragment(flow["triggerType"])["duration"])

        if trigger == "Interaction":
            interaction_id = interaction_trigger_id(flow["triggerType"])
            interaction = source.row("Interaction", interaction_id)
            phase.update({"trigger": "interaction", "interaction_id": interaction_id,
                          "next": next_phases(flow) if has_next else [[phase_id, 1]]})
            if interaction["FK_interactionAnimId"]:
                phase["interaction_anim_id"] = int(interaction["FK_interactionAnimId"])
            else:
                # Without an animation the client uses the interaction at once
                # (PC2S_InteractUnitReq with param.interaction).
                phase["instant"] = True
            if interaction["playerTaskCase"] == "MapMove":
                phase["map_move"] = map_move_destination(interaction["playerTask"])
            if flow["taskTypeCase"] in ("LootingPack", "ExpertiseReward"):
                phase["reward"] = doodad_task_reward(source, flow)
            elif doodad in (11150, 11151, 11152) and flow["taskTypeCase"] == "ExecSkillEffect":
                effect_id = int(fragment(flow["taskType"])["id"])
                effect = source.row("SkillEffect", effect_id)
                if effect_id != doodad or effect["jsonCase"] != "statRecovery":
                    raise ValueError(f"Unsupported air bubble effect: {doodad}")
                phase["skill_effect_id"] = effect_id
        elif 0 < duration_ms <= MAXIMUM_PHASE_DURATION_MS:
            phase.update({"trigger": "timer", "duration_ms": duration_ms,
                          "next": next_phases(flow)})
        elif group_interaction(source, phase_rows.get(phase_id)) is not None:
            # A phase without a flow can still offer its DoodadPhase.interactionGroupId
            # interaction (Functional quest doodads such as 10000059); using it keeps the phase.
            interaction_id, interaction = group_interaction(source, phase_rows.get(phase_id))
            phase.update({"trigger": "interaction", "interaction_id": interaction_id,
                          "next": [[phase_id, 1]]})
            if interaction["FK_interactionAnimId"]:
                phase["interaction_anim_id"] = int(interaction["FK_interactionAnimId"])
            else:
                phase["instant"] = True
            if interaction["playerTaskCase"] == "MapMove":
                phase["map_move"] = map_move_destination(interaction["playerTask"])
        else:
            phase.update({"trigger": "timer", "duration_ms": static_delay_ms,
                          "next": static_next})
        phases.append(phase)

    if not any(phase["trigger"] == "interaction" for phase in phases):
        return None
    despawns = False
    for phase in phases:
        for next_phase, _ in phase["next"]:
            if next_phase == DESPAWNED_PHASE:
                despawns = True
            elif next_phase not in phase_ids:
                raise ValueError(f"Doodad {doodad} names an unknown next phase")

    definition = {"doodad_id": doodad, "initial_phase": initial_phase, "quest": True,
                  "phases": phases}
    if despawns:
        minimum = int(row["respawnDelayMin"] or 0) or DEFAULT_STATIC_PHASE_MS
        maximum = int(row["respawnDelayMax"] or 0) or minimum
        if not 0 < minimum <= maximum <= MAXIMUM_PHASE_DURATION_MS:
            raise ValueError(f"Invalid respawn delay of doodad {doodad}")
        definition.update({"respawn_delay_min_ms": minimum, "respawn_delay_max_ms": maximum})
    return definition


def doodad_points(source, maps, doodad_ids, world_folder=ARCHEWORLD_LAYER_ROOT, world_index=0):
    """Index authored spot doodads of one world inside its hosted map areas. Points of worlds
    other than the main one (index 0) carry their world index as a seventh field.
    Authored deactivation is an optional eighth field, with an explicit world index."""
    points = {}
    prefix = world_folder + "/"
    for name in sorted(source.raw_files):
        relative = Path(name)
        if relative.name != "spot_object.json" or not relative.is_relative_to(MAP_ROOT):
            continue
        if not relative.relative_to(MAP_ROOT).as_posix().startswith(prefix):
            continue
        document = json.loads(source.read(Path("raw") / relative))
        for sector in document["sectors"]:
            packed_sector = sector_id(sector["sectorIndex"])
            for spot in sector["spotObjects"]:
                for object_type in spot["types"]:
                    doodad = object_type.get("doodad")
                    if doodad is None or doodad["cmsId"] not in doodad_ids:
                        continue
                    # Validates the recovered unit-ID layout for this sector and local ID.
                    spot_unit_id(sector["sectorIndex"], spot["id"])
                    position = position_mm(spot["position"])
                    if not inside_any_map(position, maps):
                        continue
                    key = (packed_sector, spot["id"])
                    point = [packed_sector, spot["id"], doodad["cmsId"], *position]
                    initially_deactivated = doodad.get("deactivation", False)
                    if not isinstance(initially_deactivated, bool):
                        raise ValueError("Doodad deactivation must be a boolean")
                    if world_index or initially_deactivated:
                        point.append(world_index)
                    if initially_deactivated:
                        point.append(True)
                    if key in points and points[key] != point:
                        raise ValueError(f"Conflicting doodad spot in sector {packed_sector}")
                    points[key] = point
    return sorted(points.values())


# Hosted maps of other worlds are searched without an authored area: each world asset is its
# own coordinate space, so any position of that world belongs to the map.
UNBOUNDED_AREA_MM = 2_000_000_000


def quest_world_maps(source):
    """Maps of worlds other than ArcheWorld (dungeons, story test worlds) as
    (world folder, [map areas]) in a stable order. Their doodads count only for quests."""
    worlds = {}
    for row in source.table("Map"):
        if row["world"] == ARCHEWORLD or not row["world"]:
            continue
        if row["package"] not in ("Demo", "Release"):
            continue
        world = row["world"].rsplit("/", 1)[-1].split(".")[0]
        folder = None
        for candidate in (world, f"MazeDungeons/{world}"):
            if any(path.startswith((MAP_ROOT / candidate).as_posix() + "/")
                   for path in source.raw_files):
                folder = candidate
                break
        if folder is None:
            continue
        area = {"map_id": int(row["id"]),
                "area_min_mm": [-UNBOUNDED_AREA_MM] * 3,
                "area_max_mm": [UNBOUNDED_AREA_MM] * 3}
        worlds.setdefault(folder, []).append(area)
    return sorted(worlds.items())


def validate_reward_items(source, rewards):
    """Gathering grants stackables only, so every reward item must be a non-equipment row."""
    categories = {row["id"]: row["category"] for row in source.table("Item")}
    for reward in rewards:
        for group in reward["groups"]:
            for item in group["items"]:
                category = categories.get(str(item["item_cms_id"]))
                if category is None or category == "Equipment":
                    raise ValueError(f"Gathering reward item is not stackable: "
                                     f"{item['item_cms_id']}")


def build(source):
    maps = map_areas(source, include_release_fields=True, include_release_instances=True)
    definitions, unsupported, animations = gathering_definitions(source)
    known_ids = {entry["gathering_id"] for entry in definitions}
    known_ids.update(entry["gathering_id"] for entry in unsupported)
    points = gathering_points(source, maps, known_ids)

    doodads, unsupported_doodads = doodad_definitions(source)
    candidate_doodads = {entry["doodad_id"] for entry in doodads + unsupported_doodads}
    all_doodad_points = doodad_points(source, maps, candidate_doodads)
    # Quest doodads of dungeon and story worlds: each world gets an index and its own maps.
    quest_doodads = {entry["doodad_id"] for entry in doodads if entry.get("quest")}
    worlds = [ARCHEWORLD_LAYER_ROOT]
    for folder, world_maps in quest_world_maps(source):
        world_index = len(worlds)
        world_points = doodad_points(source, world_maps, quest_doodads, folder, world_index)
        if not world_points:
            continue
        worlds.append(folder)
        maps.extend({**area, "world": world_index} for area in world_maps)
        all_doodad_points.extend(world_points)
    placed = {point[2] for point in all_doodad_points}
    # Keep only doodads placed on a hosted map, so the profile states what players can reach.
    doodads = [entry for entry in doodads if entry["doodad_id"] in placed]
    unsupported_doodads = [entry for entry in unsupported_doodads if entry["doodad_id"] in placed]
    supported_doodads = {entry["doodad_id"] for entry in doodads}
    for entry in unsupported_doodads:
        entry["placement_count"] = sum(1 for point in all_doodad_points
                                       if point[2] == entry["doodad_id"])

    known_animations = {entry["interaction_anim_id"] for entry in animations}
    usable_doodads = []
    for doodad in doodads:
        try:
            for phase in doodad["phases"]:
                animation_id = phase.get("interaction_anim_id")
                if animation_id is not None and animation_id not in known_animations:
                    animations.append(animation_definition(
                        source, animation_id, earliest_across_rigs=doodad.get("quest", False)))
                    known_animations.add(animation_id)
        except ValueError as error:
            if not doodad.get("quest"):
                raise
            # A quest doodad whose animation cannot be timed stays unsupported, not fatal.
            unsupported_doodads.append({"doodad_id": doodad["doodad_id"], "reason": str(error)})
            continue
        usable_doodads.append(doodad)
    doodads = usable_doodads
    supported_doodads = {entry["doodad_id"] for entry in doodads}
    butchering, unsupported_butchering = handling_rewards(source)
    if butchering and HANDLING_ANIMATION_ID not in known_animations:
        animations.append(handling_animation(source))
        known_animations.add(HANDLING_ANIMATION_ID)
    animations.sort(key=lambda entry: entry["interaction_anim_id"])
    validate_reward_items(source, definitions + [
        phase["reward"] for doodad in doodads for phase in doodad["phases"] if "reward" in phase
    ] + butchering)

    return {
        "format_version": FORMAT_VERSION,
        "build_id": BUILD_ID,
        "world": ARCHEWORLD,
        "sector_size_cm": SECTOR_SIZE_CM,
        "maps": maps,
        "tool_durability_loss": game_variable_ms(source, TOOL_DURABILITY_VARIABLE),
        "sp_regen_delay_ms": game_variable_ms(source, SP_REGEN_DELAY_VARIABLE),
        "animations": animations,
        "gatherings": definitions,
        "unsupported_gatherings": unsupported,
        "tool_items": tool_items(source),
        "point_fields": ["sector_id", "index", "gathering_id", "x_mm", "y_mm", "z_mm"],
        "points": points,
        "doodads": doodads,
        "unsupported_doodads": unsupported_doodads,
        "doodad_point_fields": ["sector_id", "local_id", "doodad_id", "x_mm", "y_mm", "z_mm",
                                "world (optional, index into worlds)",
                                "initially_deactivated (optional)"],
        "worlds": worlds,
        "doodad_points": [point for point in all_doodad_points if point[2] in supported_doodads],
        "handling": {
            "interaction_anim_id": HANDLING_ANIMATION_ID,
            "rewards": butchering,
            "unsupported": unsupported_butchering,
        },
        "sources": source.sources,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/gathering/definitions.json"))
    args = parser.parse_args()

    profile = build(Source(args.source))
    write_profile(args.output, profile)
    print(json.dumps({
        "maps": len(profile["maps"]),
        "points": len(profile["points"]),
        "gatherings": len(profile["gatherings"]),
        "unsupported_gatherings": len(profile["unsupported_gatherings"]),
        "doodads": len(profile["doodads"]),
        "unsupported_doodads": len(profile["unsupported_doodads"]),
        "doodad_points": len(profile["doodad_points"]),
        "butchering_monsters": len(profile["handling"]["rewards"]),
        "unsupported_butchering": len(profile["handling"]["unsupported"]),
    }))


if __name__ == "__main__":
    main()
