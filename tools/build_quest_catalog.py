"""Build the server quest catalog for every quest in the verified client export.

Each quest is normalized into the same definition shape the tutorial runtime uses
(objective groups, triggers, rewards and sequential successors) and annotated with
what the shared-world quest service can verify. Nothing here comes from official
packets: acceptance rules, objectives, conditions and rewards are all CMS tables,
trigger geometry comes from the placed level_trigger_volume layers, and memo, doodad and
NPC placements come from the spot_object and npc_spawner layers.

Requirements this client build cannot satisfy are resolved by documented CUSTOM rules:
a start that was never placed becomes GM-only (development quests) or server offered,
a step whose target was never placed is credited by the server, and a turn-in NPC that
was never placed turns the quest into an automatic completion. Any requirement the
generator still cannot model is kept in `unsupported`, so the runtime fails closed for it.
"""

import argparse
import json
import math
import re
from collections import namedtuple
from datetime import datetime, timezone
from pathlib import Path

from build_tutorial_data import Source, fixed_rewards, spot_unit_id, write_profile
from tutorial_level_triggers import QUEST_REQUIREMENTS
from tutorial_quests import fragment as strict_fragment
from tutorial_spawns import MAP_ROOT, position_mm, wire_guid


CATALOG_FORMAT_VERSION = 1
BARE_KEY = re.compile(r"(^|,)\s*([A-Za-z_]\w*)\s*:")
# Placed level geometry the catalog resolves quest starts and objectives against: trigger
# volumes by GUID, memo and doodad spot objects by CMS ID, the NPC templates spawners can place
# and the doodads that offer an interaction.
Places = namedtuple("Places", ["volumes", "memos", "doodads", "npcs", "interactable"])


def fragment(text):
    """Parse a CMS JSON fragment. A few authored rows leave their first key unquoted
    (for example ``npcId:1004022``); those are repaired only after strict parsing fails."""
    try:
        return strict_fragment(text)
    except json.JSONDecodeError:
        return json.loads("{" + BARE_KEY.sub(r'\1"\2":', text) + "}")

# Objectives the shared-world quest service can establish from server-owned state.
SERVER_OBJECTIVES = {
    "TriggerVolumeEnter",  # tracked position inside the placed volume
    "TriggerVolumeLeave",  # tracked position at the volume boundary
    "DialogueSelection",   # active dialogue with the announced NPC
    "InteractNpc",         # dialogue started with an announced NPC of this template
    "GetItem",             # owned item count
    "CharLevelUp",         # character level
    "EquipItem",           # equipped item
    "AdventureObjectStatus",  # saved adventure object state
    "HuntMonster",         # authoritative combat kill
    "InteractDoodad",      # validated doodad interaction (tutorial instances, gathering doodads)
    "InteractMemo",        # client report naming a placed memo object near the tracked position
    "CounterSuccessCrafting",  # committed crafting-station recipe
    "CounterTryCrafting",  # committed crafting-station recipe (every server craft succeeds)
    "EtherealHuntMonster",  # authoritative combat kill with a virtual item roll
    "EtherealInteractDoodad",  # validated doodad interaction with a virtual item roll
    "TimeElapsed",         # server clock since the attempt was accepted
    "TradePayment",        # committed trade-good sale at the named trading post
    "CounterUseItem",      # committed item use
    "UseItemInTriggerVolume",  # committed item use while inside the placed volume
    "CheckHp",             # server-owned player health ratio
    "AdventureLogbookPercentage",  # CUSTOM: client-reported logbook percentage of the zone
    "LevelExpGain",        # EXP granted by the server after acceptance
    "CompanyContributionGain",  # contribution credited by the server after acceptance
    "CompanyContributionTotal",  # saved company contribution
    "housingFunishing",    # saved installed housing furniture
    "AddStatusEffect",     # status effect applied to the player by server combat
    "CounterUseSkill",     # skill started by the player and accepted by server combat
    "CounterUseSkillEffect",  # skill effect executed by server combat
    "DestroyObject",       # CUSTOM: credited; destructible objects are not simulated
}

ACCEPT_KINDS = {"NpcId", "TriggerVolumeEnter", "UnexpectTriggerVolumeEnter", "Sequential",
                "QuestComplete", "CharLevelUp", "JobBoard", "InteractDoodad", "InteractMemo"}
SERVER_EVENT_OBJECTIVES = {"HuntMonster", "InteractDoodad", "EtherealHuntMonster",
                           "EtherealInteractDoodad", "CounterSuccessCrafting",
                           "CounterTryCrafting", "TradePayment", "CounterUseItem",
                           "UseItemInTriggerVolume", "AddStatusEffect", "CounterUseSkill",
                           "CounterUseSkillEffect", "DestroyObject"}
CONDITION_KINDS = {"Quest", "CheckUnitLevel", "Company", "OwnItem", "PlayRecord",
                   "AdventureObject"}
# Quests below this id are the client's development and test content (story1, Interaction1
# test maps and placeholder volumes). Without a placed start they are GM-only.
FIRST_LIVE_QUEST_ID = 1000000
# x5.cms.EX5ComparisonType names used by PlayRecord conditions.
COMPARISONS = {
    "EX5ComparisonType_Equal": "Equal",
    "EX5ComparisonType_NotEqual": "NotEqual",
    "EX5ComparisonType_GreaterThan": "GreaterThan",
    "EX5ComparisonType_GreaterThanOrEqual": "GreaterThanOrEqual",
    "EX5ComparisonType_LessThan": "LessThan",
    "EX5ComparisonType_LessThanOrEqual": "LessThanOrEqual",
}
BUILD_POINT_TYPES = {
    "EX5CharBuildPointType_LevelUpPoint": "LevelUpPoint",
    "EX5CharBuildPointType_WeaponPoint": "WeaponPoint",
}
SEQUENTIAL_ORDER = re.compile(r"(?:\((\d+),(\d+)\)|(\d+),(\d+))")
# x5.cms.EX5CompanyType values from protocol/descriptors.normalized.pb.
COMPANY_TYPES = {
    "EX5CompanyType_None": 0,
    "EX5CompanyType_Antiqus": 1,
    "EX5CompanyType_BlueSalt": 2,
    "EX5CompanyType_Fairwind": 3,
    "EX5CompanyType_Shadowhk": 4,
    "EX5CompanyType_YokedIron": 5,
}


def company_type(name):
    if name not in COMPANY_TYPES:
        raise ValueError(f"Unknown company type: {name}")
    return COMPANY_TYPES[name]


def company_condition(parameters):
    """RequirementType.Company fields in the order the client checks them
    (AAC_Requirement_CheckCompany_Candidate, dumps/ida_research/company-requirement-20261030).
    Some rows quote their numbers, so every numeric field is converted explicitly."""
    return {
        "company_type": company_type(parameters["companyId"]),
        "ignore_permit": bool(parameters.get("ignorePermit", False)),
        "level": int(parameters.get("level", 0)),
        "is_max_contribution": bool(parameters.get("isMaxContribution", False)),
        "contribution": int(parameters.get("contribution", 0)),
    }


def map_world_of(path, prefix):
    """World asset of an exported map data path. Dungeon worlds are grouped one folder deeper
    (``MazeDungeons/DungeonZ02D01/...``); every other world is the first folder."""
    parts = path[len(prefix):].split("/")
    if parts[0] == "MazeDungeons":
        return parts[1]
    return parts[0]


def all_trigger_volumes(source):
    """Every placed level-trigger volume, keyed by GUID, with the world it belongs to."""
    volumes = {}
    prefix = MAP_ROOT.as_posix() + "/"
    for path in sorted(source.raw_files):
        if not path.startswith(prefix) or not path.endswith("/level_trigger_volume.json"):
            continue
        world = map_world_of(path, prefix)
        document = json.loads(source.read(Path("raw") / path))
        for volume in document["triggerVolumes"]:
            entry = {**volume, "world": world}
            previous = volumes.get(volume["id"])
            if previous is not None and previous != entry:
                raise ValueError(f"Conflicting trigger volume placement: {volume['id']}")
            volumes[volume["id"]] = entry
    return volumes


def all_spot_objects(source, type_key):
    """Every placed spot object carrying ``type_key`` (``memoObject``, ``doodad``), keyed by its
    CMS ID. The wire unit ID of a spot is ``sector << 32 | local_id << 12 | 6``; the captured
    Words of Warning report (memo 10000127) named the Zone_01_Public_02 board."""
    placements_by_id = {}
    prefix = MAP_ROOT.as_posix() + "/"
    for path in sorted(source.raw_files):
        if not path.startswith(prefix) or not path.endswith("/spot_object.json"):
            continue
        world = map_world_of(path, prefix)
        document = json.loads(source.read(Path("raw") / path))
        for sector in document["sectors"]:
            for spot in sector["spotObjects"]:
                for kind in spot["types"]:
                    placed = kind.get(type_key)
                    if placed is None:
                        continue
                    placement = {
                        "world": world,
                        "unit_id": str(spot_unit_id(sector["sectorIndex"], spot["id"])),
                        "center": position_mm(spot["position"]),
                    }
                    placements = placements_by_id.setdefault(int(placed["cmsId"]), [])
                    if placement not in placements:
                        placements.append(placement)
    return placements_by_id


def all_memo_objects(source):
    """Every placed memo spot object (notice boards and the like), keyed by Memo CMS ID."""
    return all_spot_objects(source, "memoObject")


def placed_npcs(source):
    """NPC templates some exported npc_spawner layer can spawn: the spawner's weighted
    candidates and the members of its NpcGroups. NPC rows without a model template
    (Npc.FK_template empty, arena manager 100265) cannot be shown and are left out."""
    selectors = {row["id"]: row for row in source.table("NpcSpawner")}
    showable = {int(row["id"]) for row in source.table("Npc") if row["FK_template"]}
    group_members = {}
    for member in source.table("NpcGroupMember"):
        group_members.setdefault(member["FK_npcGroup"], set()).add(int(member["FK_npc"]))
    npcs = set()
    prefix = MAP_ROOT.as_posix() + "/"
    for path in sorted(source.raw_files):
        if not path.startswith(prefix) or not path.endswith("/npc_spawner.json"):
            continue
        document = json.loads(source.read(Path("raw") / path))
        for sector in document["spawnerSectors"]:
            for spawner in sector["spawners"]:
                selector = selectors.get(str(spawner["cmsId"]))
                if selector is None:
                    continue
                for entry in filter(None, selector["npc"].split(",")):
                    npcs.add(int(entry.partition(":")[0]))
                for entry in filter(None, selector["npcGroup"].split(",")):
                    npcs.update(group_members.get(entry.partition(":")[0].strip(), set()))
    return npcs & showable


def interactable_doodads(source):
    """Doodads offering at least one interaction: an Interaction-triggered DoodadPhaseFlow or a
    DoodadPhase interaction group with an Interaction row."""
    doodads = {int(flow["FK_doodad"]) for flow in source.table("DoodadPhaseFlow")
               if flow["triggerTypeCase"] == "Interaction"}
    groups = {row["groupId"] for row in source.table("Interaction")}
    doodads.update(int(phase["FK_doodad"]) for phase in source.table("DoodadPhase")
                   if phase["interactionGroupId"] in groups)
    return doodads


def trigger_geometry(volumes, parameters):
    """Wire GUID and millimeter geometry, or None when the volume is not placed.

    A nonzero ``mapLayer`` names the Map row whose data layer holds the volume (dungeons such as
    10201, or channel fields such as 20331); such a volume only exists on that map."""
    volume = volumes.get(parameters["triggerVolumeId"])
    if volume is None:
        return None

    geometry = {"guid": wire_guid(volume["id"]), "world": volume["world"]}
    map_layer = int(parameters.get("mapLayer", 0))
    if map_layer != 0:
        geometry["map_id"] = map_layer
    if "sphere" in volume:
        sphere = volume["sphere"]
        geometry["center"] = [round(sphere["center"][axis] * 10) for axis in ("x", "y", "z")]
        geometry["radius"] = round(sphere["radius"] * 10)
        return geometry

    box = volume["box"]
    geometry["center"] = [round(box["center"][axis] * 10) for axis in ("x", "y", "z")]
    geometry["extent"] = [round(box["extent"][axis] * 10) for axis in ("x", "y", "z")]
    geometry["yaw"] = box["rotation"]["yaw"]
    return geometry


def map_worlds(source):
    worlds = {}
    for row in source.table("Map"):
        if row["world"]:
            worlds[row["id"]] = row["world"].rsplit("/", 1)[1].split(".")[0]
    return worlds


def acceptance(row, places, reasons):
    """Acceptance rules the server can verify. Returns (rules, unplaced): ``unplaced`` is True
    when a start names a volume, memo or doodad that this client build never placed, so the
    client can never report it."""
    accepts = []
    unplaced = False
    for index in range(1, 4):
        kind = row[f"acceptConditionType{index}Case"]
        if not kind:
            continue
        parameters = fragment(row[f"acceptConditionType{index}"])
        if kind not in ACCEPT_KINDS:
            reasons.append(f"accept:{kind}")
            continue
        if kind == "NpcId":
            npc_id = int(parameters["npcId"])
            if npc_id not in places.npcs:
                # No spawner of this client build places the quest giver.
                unplaced = True
                continue
            accepts.append({"kind": kind, "npc_id": npc_id})
        elif kind in ("TriggerVolumeEnter", "UnexpectTriggerVolumeEnter"):
            geometry = trigger_geometry(places.volumes, parameters)
            if geometry is None:
                unplaced = True
                continue
            field = ("unexpect_trigger_volume_enter" if kind == "UnexpectTriggerVolumeEnter"
                     else "trigger_volume_enter")
            accepts.append({"kind": "TriggerVolumeEnter", "request_field": field,
                            "trigger": geometry})
        elif kind == "QuestComplete":
            accepts.append({"kind": kind, "quest_id": int(parameters["questId"])})
        elif kind == "CharLevelUp":
            accepts.append({"kind": kind,
                            "min_level": int(parameters.get("minLevel", 0)),
                            "max_level": int(parameters.get("maxLevel", 2147483647))})
        elif kind == "JobBoard":
            accepts.append({"kind": kind, "board_id": int(parameters["jobBoardId"])})
        elif kind == "InteractDoodad":
            doodad_id = int(parameters["doodadId"])
            placements = places.doodads.get(doodad_id)
            if not placements:
                unplaced = True
                continue
            accepts.append({"kind": kind, "doodad_id": doodad_id,
                            "target_phase": int(parameters.get("targetPhase", 0)),
                            "doodads": placements})
        elif kind == "InteractMemo":
            memo_id = int(parameters["memoId"])
            placements = places.memos.get(memo_id)
            if not placements:
                unplaced = True
                continue
            accepts.append({"kind": kind, "memo_id": memo_id, "memos": placements})
        else:
            accepts.append({"kind": "Sequential"})
    return accepts, unplaced


def offered_start(quest_id, row, progresses, zone_maps):
    """CUSTOM start for a quest whose only start was never placed in this client build.

    Development quests (ids below FIRST_LIVE_QUEST_ID) become GM-only. A live quest is offered
    by the server at the first in-world point its own objectives name: talking to an objective
    NPC, defeating an objective monster or using an objective doodad; with none of those, on
    entering a map of its journal zone (any map of its world when it names no zone)."""
    if quest_id < FIRST_LIVE_QUEST_ID:
        return {"kind": "GmOnly"}

    npc_ids = set()
    monster_ids = set()
    doodad_ids = set()
    for objective in progresses:
        parameters = objective["parameters"]
        if objective.get("auto_credit"):
            # A target this client build never places cannot anchor the start.
            continue
        if objective["kind"] in ("InteractNpc",) and "npcId" in parameters:
            npc_ids.add(int(parameters["npcId"]))
        elif objective["kind"] == "DialogueSelection" and "targetId" in parameters:
            npc_ids.add(int(parameters["targetId"]))
        elif objective["kind"] in ("HuntMonster", "EtherealHuntMonster"):
            monster_ids.update(objective.get("npc_ids", []))
            if "npcId" in parameters:
                monster_ids.add(int(parameters["npcId"]))
        elif objective["kind"] in ("InteractDoodad", "EtherealInteractDoodad"):
            doodad_ids.add(int(parameters["doodadId"]))

    zone = ""
    if row["questUiTypeCase"] in ("Local", "Saga", "Company"):
        zone = fragment(row["questUiType"]).get("zoneKey", "") if row["questUiType"] else ""
    zone = zone or row["originZoneKey"]
    return {
        "kind": "ServerOffered",
        "npc_ids": sorted(npc_ids),
        "monster_ids": sorted(monster_ids),
        "doodad_ids": sorted(doodad_ids),
        "zone_maps": sorted(zone_maps.get(zone, [])),
    }


def quest_conditions(source, condition_id, reasons, table="QuestCondition"):
    """Normalize the rows of one condition id for the runtime condition evaluator.

    QuestCondition and InteractionCondition share their columns (group, logicalOperator,
    checkOnType, negated, typeCase, type), so both use this normalization. Rows the runtime
    cannot evaluate are reported in ``reasons`` and left out.
    """
    conditions = []
    if not condition_id:
        return conditions
    for row in source.table(table):
        if row["conditionId"] != condition_id:
            continue
        parameters = fragment(row["type"])
        if row["typeCase"] not in CONDITION_KINDS or row["checkOnType"] != "Caster":
            reasons.append(f"condition:{row['typeCase']}")
            continue
        condition = {
            "group": int(row["group"]),
            "operator": row["logicalOperator"] or "AND",
            "negated": row["negated"] == "True",
            "kind": row["typeCase"],
        }
        if row["typeCase"] == "Quest":
            requirement = QUEST_REQUIREMENTS.get(parameters.get("questRequirementType"))
            if requirement is None:
                reasons.append("condition:quest_requirement")
                continue
            condition.update({"quest_id": int(parameters["questId"]), "requirement": requirement})
            if "progressGroup" in parameters:
                condition["progress_group"] = int(parameters["progressGroup"])
        elif row["typeCase"] == "Company":
            condition.update(company_condition(parameters))
        elif row["typeCase"] == "OwnItem":
            # The grade names the item's catalog grade; stacks carry no separate grade.
            condition.update({"item_id": int(parameters["itemId"]),
                              "count": int(parameters.get("count", 1))})
        elif row["typeCase"] == "PlayRecord":
            record_id = int(parameters["playRecordId"])
            comparison = COMPARISONS.get(parameters.get("comparisonType",
                                                        "EX5ComparisonType_GreaterThanOrEqual"))
            if comparison is None:
                raise ValueError(f"Unknown play record comparison: {parameters}")
            condition.update({"play_record_id": record_id,
                              "count": int(parameters.get("count", 0)),
                              "comparison": comparison,
                              "record": play_record(source, record_id)})
        elif row["typeCase"] == "AdventureObject":
            condition.update({"adventure_object_id": int(parameters["cmsId"]),
                              "active": bool(parameters.get("active", False))})
        else:
            condition.update({"min": int(parameters.get("min", 0)),
                              "max": int(parameters.get("max", 2147483647))})
        conditions.append(condition)
    return conditions


def play_record(source, record_id):
    """The action a PlayRecord counts. The server tracks HuntMonster kills; GetItem records are
    read from the character's current holdings."""
    row = source.row("PlayRecord", record_id)
    parameters = fragment(row["type"]) if row["type"] else {}
    if row["typeCase"] == "HuntMonster":
        if "npcId" in parameters:
            return {"kind": "HuntMonster", "npc_id": int(parameters["npcId"])}
        if "npcGroupId" in parameters:
            return {"kind": "HuntMonster", "npc_ids": npc_group_members(
                source, int(parameters["npcGroupId"]))}
        raise ValueError(f"Unsupported HuntMonster play record target: {record_id}")
    if row["typeCase"] == "GetItem":
        return {"kind": "GetItem", "item_id": int(parameters["itemId"])}
    raise ValueError(f"Unsupported quest play record {record_id}: {row['typeCase']}")


def npc_group_members(source, group_id):
    members = sorted({int(row["FK_npc"]) for row in source.table("NpcGroupMember")
                      if row["FK_npcGroup"] == str(group_id)})
    if not members:
        raise ValueError(f"Empty NPC group: {group_id}")
    return members


# Parameters that stay fractional; every other integral float is stored as an integer.
FRACTIONAL_PARAMETERS = {"rate", "minValue", "maxValue", "priceRatio"}


def objectives(source, progress_rows, places, reasons):
    entries = []
    for progress in progress_rows:
        parameters = fragment(progress["type"])
        for key, value in list(parameters.items()):
            if (isinstance(value, float) and key not in FRACTIONAL_PARAMETERS
                    and value.is_integer()):
                parameters[key] = int(value)
        entry = {
            "sequence": int(progress["sequenceId"]),
            "group": int(progress["group"]),
            "kind": progress["typeCase"],
            "parameters": parameters,
        }
        if entry["kind"] not in SERVER_OBJECTIVES:
            reasons.append(f"objective:{entry['kind']}")
        if entry["kind"] in ("TriggerVolumeEnter", "TriggerVolumeLeave",
                             "UseItemInTriggerVolume"):
            geometry = trigger_geometry(places.volumes, parameters)
            if geometry is None:
                # CUSTOM: this client build never placed the volume, so no client can report
                # it; the server credits the step once it becomes ready.
                entry["auto_credit"] = True
            else:
                entry["trigger"] = geometry
        elif entry["kind"] == "InteractMemo":
            placements = places.memos.get(parameters.get("memoId"))
            if placements:
                entry["memos"] = placements
            else:
                entry["auto_credit"] = True
        elif entry["kind"] in ("HuntMonster", "EtherealHuntMonster") and "npcId" not in parameters:
            # A hunt names one template (npcId), a list of templates (npcIds, job board hunts)
            # or an NpcGroup whose members all count.
            if "npcIds" in parameters:
                entry["npc_ids"] = sorted({int(npc_id) for npc_id in parameters["npcIds"]})
            else:
                entry["npc_ids"] = npc_group_members(source, parameters["npcGroupId"])
        if entry["kind"] in ("HuntMonster", "EtherealHuntMonster"):
            targets = entry.get("npc_ids") or [parameters.get("npcId")]
            if not any(target in places.npcs for target in targets):
                # CUSTOM: no spawner of this client build places the monster.
                entry["auto_credit"] = True
        elif entry["kind"] in ("InteractNpc", "DialogueSelection"):
            target = parameters.get("npcId", parameters.get("targetId"))
            if target not in places.npcs:
                # CUSTOM: the NPC to talk to is never placed (a scripted appearance).
                entry["auto_credit"] = True
        elif entry["kind"] in ("InteractDoodad", "EtherealInteractDoodad"):
            doodad_id = parameters.get("doodadId")
            if doodad_id not in places.doodads or doodad_id not in places.interactable:
                # CUSTOM: the doodad is never placed, or offers no interaction to use.
                entry["auto_credit"] = True
        elif entry["kind"] == "DestroyObject":
            # CUSTOM: destructible spot objects (DestructibleObject health, destruction and
            # respawn) are not simulated, and their destruction packets are not recovered. The
            # only quest using it is the GM-only test quest 9088, so the step is credited.
            entry["auto_credit"] = True
        entries.append(entry)
    return sorted(entries, key=lambda entry: entry["sequence"])


def quest_loot_pack(source, pack_id):
    """A randomly rolled LootPack reward: the pack rate and its groups. Gold rows are kept as
    item 0, the convention the quest reward path uses for Gold; HersanderToken rows are item 0
    with ``money_type``."""
    pack_rate = float(source.row("LootPack", pack_id)["rate"])
    if not math.isfinite(pack_rate) or not 0 < pack_rate <= 100:
        raise ValueError(f"Invalid loot pack rate: {pack_id}")
    groups = []
    for condition in source.table("LootCondition"):
        if condition["FK_lootPack"] != str(pack_id):
            continue
        rate = float(condition["rate"])
        if not math.isfinite(rate) or not 0 < rate <= 100:
            raise ValueError(f"Invalid loot condition rate in pack {pack_id}")
        items = []
        for row in source.table("LootItem"):
            if row["lootGroupId"] != condition["lootGroupId"]:
                continue
            if row["moneyType"] not in ("", "Gold", "HersanderToken"):
                raise ValueError(f"Unsupported loot currency in pack {pack_id}")
            minimum = int(row["minAmount"])
            maximum = int(row["maxAmount"])
            weight = int(row["dropWeight"] or 1)
            if not 0 < minimum <= maximum or weight <= 0:
                raise ValueError(f"Invalid loot amount in pack {pack_id}")
            entry = {"item_cms_id": 0 if row["moneyType"] else int(row["itemId"]),
                     "min_quantity": minimum, "max_quantity": maximum, "weight": weight}
            if row["moneyType"] == "HersanderToken":
                entry["money_type"] = "HersanderToken"
            items.append(entry)
        if items:
            groups.append({"rate_percent": rate, "items": items})
    if not groups:
        raise ValueError(f"Empty loot pack: {pack_id}")
    return {"loot_pack_id": int(pack_id), "pack_rate_percent": pack_rate, "groups": groups}


def rewards(source, reward_rows, reasons):
    result = {
        "items": [],
        "experience": 0,
        "company_unlocks": [],
        "company_contribution": 0,
        "loot_packs": [],
        "guild_experience": 0,
        "mates": [],
        "mate_intimacy": [],
        "build_points": [],
    }
    for reward in reward_rows:
        parameters = fragment(reward["type"])
        kind = reward["typeCase"]
        if kind == "Exp":
            result["experience"] += int(parameters["value"])
        elif kind == "Money" and parameters["moneyType"] == "EX5MoneyType_Gold":
            result["items"].append({"item_cms_id": 0, "quantity": int(parameters["value"])})
        elif kind == "CompanyLevelUnlock":
            result["company_unlocks"].append(
                {"company_type": company_type(parameters["companyType"]),
                 "level": int(parameters["level"])})
        elif kind == "CompanyContribution":
            # The row names no company: it credits the character's current company.
            result["company_contribution"] += int(parameters["value"])
        elif kind == "LootPack":
            count = int(parameters.get("count", 1))
            try:
                pack = fixed_rewards(source, parameters["lootPackId"])
            except ValueError:
                rolled = quest_loot_pack(source, parameters["lootPackId"])
                result["loot_packs"].extend([rolled] * count)
                continue
            for _ in range(count):
                result["items"].extend(pack)
        elif kind == "GuildExp":
            # Credited to the character's guild after the completion commits.
            result["guild_experience"] += int(parameters["value"])
        elif kind == "Mate":
            result["mates"].append(int(parameters["mateId"]))
        elif kind == "Intimate":
            mate_id = int(parameters["mateId"])
            # The intimacy ceiling the companion service also applies: the sum of the mate's
            # MateIntimateLevel.nextLevelIntimate thresholds.
            limit = sum(int(level["nextLevelIntimate"] or 0)
                        for level in source.table("MateIntimateLevel")
                        if level["FK_mateNpc"] == str(mate_id))
            result["mate_intimacy"].append({"mate_id": mate_id,
                                            "amount": int(parameters["amount"]),
                                            "limit": limit})
        elif kind == "CharBuildPoint":
            point_type = BUILD_POINT_TYPES.get(parameters["type"])
            if point_type is None:
                raise ValueError(f"Unknown build point reward: {parameters}")
            result["build_points"].append({"type": point_type, "value": int(parameters["value"])})
        else:
            reasons.append(f"reward:{kind}")
    return result


def supplies(source, text, reasons):
    entries = []
    for item in filter(None, text.split(",")):
        identifier, quantity = item.split(":")
        definition = source.row("Item", identifier)
        if definition["category"] != "QuestItem" or not 0 < int(quantity) <= 1000000:
            reasons.append("supply:non_quest_item")
            continue
        entries.append({"item_cms_id": int(identifier), "quantity": int(quantity)})
    return entries


def group_rows(rows, key):
    grouped = {}
    for row in rows:
        grouped.setdefault(row[key], []).append(row)
    return grouped


def dependent_items(row, variants):
    entries = []
    for variant in [row, *variants]:
        for item_id in filter(None, variant["dependentItemIds"].split(",")):
            entries.append({
                "class": variant["class"],
                "item_cms_id": int(item_id),
                "retrieve": variant["retrieveDependentItem"] == "True",
            })
    return entries


def merged_quest_rows(source):
    """One row per quest. A few quests repeat their row per character class, differing
    only in class and dependent items; those variants are kept under `class_variants`."""
    variant_fields = {"class", "dependentItemIds", "retrieveDependentItem"}
    merged = {}
    for row in source.table("Quest"):
        previous = merged.get(row["id"])
        if previous is None:
            merged[row["id"]] = (row, [])
            continue
        base, variants = previous
        differing = {key for key in row if row[key] != base[key]}
        if not differing <= variant_fields:
            raise ValueError(f"Quest rows conflict beyond class variants: {row['id']}")
        variants.append(row)
    return merged.values()


def requirement_items(text):
    entries = []
    for item in filter(None, text.split(",")):
        identifier, quantity = item.split(":")
        entries.append({"item_cms_id": int(identifier), "quantity": int(quantity)})
    return entries


def zone_maps(source):
    """Journal zone key (Zone_01, ...) -> the Map ids that belong to it."""
    zones = {}
    for row in source.table("Map"):
        if row["FK_zoneKey"]:
            zones.setdefault(row["FK_zoneKey"], []).append(int(row["id"]))
    return zones


def game_variable(source, name):
    for row in source.table("GameVariable"):
        if row["name"] == name:
            return row["value"]
    raise ValueError(f"Missing game variable: {name}")


def job_boards(source, places):
    """QuestJobBoard rows with their board doodad placements and offered quest pools.

    QuestJobBoardCategory lists, per board, one row per offered slot (`order`) and company or
    guild level; each row names a QuestPickGroup, the pool one offered quest is drawn from."""
    pools = {}
    for row in source.table("QuestPickGroup"):
        pools.setdefault(int(row["id"]), []).append(int(row["FK_questId"]))
    categories = {}
    for row in source.table("QuestJobBoardCategory"):
        categories.setdefault(int(row["id"]), []).append({
            "company_type": COMPANY_TYPES[f"EX5CompanyType_{row['company']}"],
            "is_guild": row["isGuild"] == "True",
            "company_level": int(row["companyLevel"] or 0),
            "guild_level": int(row["guildLevel"] or 0),
            "order": int(row["order"]),
            "quest_ids": sorted(pools.get(int(row["FK_questPickGroupId"]), [])),
        })

    boards = {}
    for row in source.table("QuestJobBoard"):
        doodad_id = int(row["FK_doodadId"])
        boards[row["id"]] = {
            "doodad_id": doodad_id,
            "doodads": places.doodads.get(doodad_id, []),
            "reset_cost": int(row["resetCost"] or 0),
            "slots": categories.get(int(row["FK_questJobBoardCategoryId"]), []),
        }
    return boards


def job_board_schedule(source):
    """The shared refresh schedule (GameVariable QuestJobBoard*). The start is read as UTC."""
    start = datetime.strptime(game_variable(source, "QuestJobBoardRefreshScheduleStart"),
                              "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
    interval = game_variable(source, "QuestJobBoardRefreshScheduleInterval")
    if interval != "Daily":
        raise ValueError(f"Unsupported job board refresh interval: {interval}")
    times = int(game_variable(source, "QuestJobBoardRefreshScheduleIntervalTimes"))
    if times <= 0:
        raise ValueError("Invalid job board refresh interval count")
    return {
        "pick_count": int(game_variable(source, "QuestJobBoardPickOnceCount")),
        "start_epoch_ms": int(start.timestamp() * 1000),
        "period_ms": times * 24 * 60 * 60 * 1000,
    }


def kill_play_records(source):
    records = []
    for row in source.table("PlayRecord"):
        if row["typeCase"] != "HuntMonster":
            continue
        parameters = fragment(row["type"])
        if "npcId" in parameters:
            npc_ids = [int(parameters["npcId"])]
        elif "npcGroupId" in parameters:
            npc_ids = npc_group_members(source, int(parameters["npcGroupId"]))
        else:
            raise ValueError(f"Unknown monster play record target: {row['id']}")
        for npc_id in npc_ids:
            records.append({"id": int(row["id"]), "npc_id": npc_id})
    return records


def build(source):
    places = Places(all_trigger_volumes(source), all_memo_objects(source),
                    all_spot_objects(source, "doodad"), placed_npcs(source),
                    interactable_doodads(source))
    zones = zone_maps(source)
    progress_by_quest = group_rows(source.table("QuestProgress"), "id")
    rewards_by_id = group_rows(source.table("QuestReward"), "id")
    boards = job_boards(source, places)
    quests = []
    chains = {}
    for row, variants in merged_quest_rows(source):
        quest_id = int(row["id"])
        reasons = []
        reward = rewards(
            source, rewards_by_id.get(row["FK_QuestReward"], []) if row["FK_QuestReward"] else [],
            reasons,
        )
        progresses = objectives(source, progress_by_quest.get(row["id"], []), places, reasons)

        completion = {"kind": row["questCompleteType"]}
        if row["questCompleteType"] == "Npc":
            # Several turn-in NPCs can be listed, comma separated. CUSTOM: turn-in NPCs this
            # client build never places are dropped; with none left the quest completes Auto.
            listed = [int(npc_id) for npc_id in row["questCompleteTypeParams"].split(",")
                      if npc_id]
            placed = [npc_id for npc_id in listed if npc_id in places.npcs]
            if placed:
                completion["npc_ids"] = placed
            else:
                completion = {"kind": "Auto"}
        elif row["questCompleteType"] == "JobBoard":
            board_id = row["questCompleteTypeParams"]
            if board_id not in boards:
                raise ValueError(f"Quest {quest_id} turns in at an unknown job board")
            completion["board_id"] = int(board_id)
        elif row["questCompleteType"] == "Doodad":
            doodad_id, _, phase = row["questCompleteTypeParams"].partition(",")
            completion["doodad_id"] = int(doodad_id)
            completion["target_phase"] = int(phase or 0)
            completion["doodads"] = places.doodads.get(int(doodad_id), [])
            if not completion["doodads"]:
                # CUSTOM: an unplaced turn-in doodad completes like an Auto quest.
                completion = {"kind": "Auto"}
        elif row["questCompleteType"] not in ("Auto",):
            reasons.append(f"completion:{row['questCompleteType']}")

        accepts, unplaced = acceptance(row, places, reasons)
        if unplaced and not accepts:
            accepts = [offered_start(quest_id, row, progresses, zones)]

        quest = {
            "cms_id": quest_id,
            "type": row["type"],
            "accept": accepts,
            "conditions": quest_conditions(source, row["FK_QuestCondition"], reasons),
            "completion": completion,
            "supplies": supplies(source, row["supplyItemIds"], reasons),
            # Acceptance gates (Quest_IsNotAcceptable_RequireGold/_RequireItemId): checked, not
            # consumed, because successive quests such as 30003121 and 30003123 require the
            # same quest items.
            "require_gold": int(row["requireGold"] or 0),
            "require_items": requirement_items(row["requireItemIds"]),
            "auto_complete": row["type"] == "HiddenAutoComplete",
            "sequential": row["isProgressSequential"] == "True",
            "abandonable": row["isAbandonable"] == "True",
            "progresses": progresses,
            "experience": reward["experience"],
            "rewards": reward["items"],
            # Randomly rolled LootPack rewards, rolled once at completion.
            "loot_packs": reward["loot_packs"],
            # CompanyLevelUnlock rewards: applied through the company service after the quest
            # completion commits, and reconciled from completed quests at world entry.
            "company_unlocks": reward["company_unlocks"],
            # CompanyContribution points for the character's current company, credited once
            # after the completion commits.
            "company_contribution": reward["company_contribution"],
            # GuildExp points for the character's guild, credited once after the commit.
            "guild_experience": reward["guild_experience"],
            "mates": reward["mates"],
            "mate_intimacy": reward["mate_intimacy"],
            "build_points": reward["build_points"],
            # Dependent items are informational: the service does not retrieve them on
            # completion yet (the official retrieval behavior is not captured).
            "dependent_items": dependent_items(row, variants),
            "unsupported": sorted(set(reasons)),
        }
        quests.append(quest)

        match = SEQUENTIAL_ORDER.fullmatch(row["sequentialGroupOrder"] or "")
        if match:
            chain = int(match.group(1) or match.group(3))
            order = int(match.group(2) or match.group(4))
            chains.setdefault(chain, {})[order] = quest

    for chain in chains.values():
        ordered = [quest for order, quest in sorted(chain.items())]
        for previous, following in zip(ordered, ordered[1:]):
            previous["next_quest"] = following["cms_id"]

    known = {quest["cms_id"] for quest in quests}
    for quest in quests:
        for rule in quest["accept"]:
            anchored = rule.get("npc_ids") or rule.get("monster_ids") or rule.get("doodad_ids")
            if rule["kind"] == "ServerOffered" and not anchored and quest["cms_id"] - 1 in known:
                # CUSTOM: an unanchored offered quest follows the preceding step of its series.
                rule["after_quest_id"] = quest["cms_id"] - 1
    for board in boards.values():
        for slot in board["slots"]:
            slot["quest_ids"] = [quest_id for quest_id in slot["quest_ids"] if quest_id in known]

    return {
        "format_version": CATALOG_FORMAT_VERSION,
        "build_id": "40367e24241c9481bfc07fbf9ef1919f25fb0eaa",
        "map_worlds": map_worlds(source),
        "job_boards": boards,
        "job_board_schedule": job_board_schedule(source),
        # Interaction requirements also reference these records, independently of quests.
        "kill_play_records": kill_play_records(source),
        "quests": sorted(quests, key=lambda quest: quest["cms_id"]),
        "sources": source.sources,
    }


def summary(catalog):
    supported = [quest for quest in catalog["quests"] if not quest["unsupported"]]
    # These objectives advance only from server events (kills, crafts, doodad interactions), so
    # they also depend on the event producers being present on the quest's map.
    needs_events = [quest for quest in supported
                    if any(objective["kind"] in SERVER_EVENT_OBJECTIVES
                           for objective in quest["progresses"])]
    reasons = {}
    for quest in catalog["quests"]:
        for reason in quest["unsupported"]:
            reasons[reason] = reasons.get(reason, 0) + 1
    return {
        "quests": len(catalog["quests"]),
        "fully_supported": len(supported),
        "supported_needing_shared_world_events": len(needs_events),
        "unsupported_reasons": dict(sorted(reasons.items(), key=lambda item: -item[1])),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/quests/catalog.json"))
    args = parser.parse_args()
    catalog = build(Source(args.source))
    write_profile(args.output, catalog)
    print(json.dumps(summary(catalog)))


if __name__ == "__main__":
    main()
