"""Build monster corpse-loot definitions from the verified local client export.

The output contains static CMS data only. No official character, account, item-instance or
session identifier is copied into the runtime profile.

Evidence for the joins below (build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa):

* ``Npc.FK_lootPackId`` selects a ``LootPack``; its ``LootCondition`` rows are independent
  groups (``rate`` percent) and each group's ``LootItem`` rows are weighted alternatives with
  a ``minAmount``..``maxAmount`` quantity. Gathering uses the same tables. All 63 official
  corpse-loot item grants in the three 2026-10-24 captures that could be joined to a killed
  NPC contained only items from that NPC's pack (no mismatches).
* ``GameVariable.LootingRange`` (centimetres) is the corpse looting range.
  ``NpcCorpseLootingDestroyMillis`` is how long a corpse with loot remains, and
  ``NpcCorpseNoneDestroyMillis`` how long an empty or fully looted corpse remains. The
  captured unit-removal notifications arrive about 10 s after the corpse is emptied.

CUSTOM (guessed, not official) behavior. The data needed to reproduce these official rules does
not exist in the client export or the captures, so this tool emits our own guesses and tags every
affected group with the rule name that the server can switch off (``custom_rules`` in the
output). With a rule switched off the server skips the tagged groups, which is the
evidence-only behavior.

* ``kill_gold``: captured corpse gold appeared for NPCs without any loot pack and always paid
  the same amount for the same NPC, which depends on the NPC template grade and level. No
  catalog table authors it, so the amount is a per-grade straight line fitted to the captured
  samples (``GOLD_SAMPLES``), rounded, and written per NPC as ``kill_gold``.
* ``first_kill_rewards``: groups filtered by ``lootingClass`` or ``FK_PlayRecord_End`` drop only
  on a character's first looted kill of that NPC.
* ``quest_gated_rows``: items with ``Item.questLooting`` (and ``QuestItem`` rows) drop only while
  the character has one of the listed quests active. Quest IDs that the client's Quest table does
  not define can never be active, so such items fall back to no gate.
* ``full_moon_stones``: ``FullMoonStone`` items are granted as ordinary stackable items.
* ``polish_step_rows``: ``LootItem.polishStep`` becomes the dropped instance's polish step.
* ``handling_loot``: ``Npc.FK_handlingLootPackId`` / ``handlingRate`` become a second pack that
  is rolled with the corpse when the rate succeeds.

Groups that use a filter or item kind that is not covered above are skipped entirely and are
listed in ``skipped_groups``, so an unsupported group never drops a wrong reward.
"""

import argparse
import json
import math
import re
from pathlib import Path

from build_tutorial_data import BUILD_ID, Source, write_profile


FORMAT_VERSION = 2
MILLIMETRES_PER_CENTIMETRE = 10
MONSTER_BEHAVIOR = "Monster"
GOLD_CURRENCY = "Gold"

# Rule names. They are the keys of the server's loot.custom_rules configuration.
RULE_KILL_GOLD = "kill_gold"
RULE_FIRST_KILL = "first_kill_rewards"
RULE_QUEST_GATED = "quest_gated_rows"
RULE_FULL_MOON_STONES = "full_moon_stones"
RULE_POLISH_STEPS = "polish_step_rows"
RULE_HANDLING = "handling_loot"

# Same storage split as the NPC shop: equipment is an item instance, the listed categories
# are one stack per CMS ID in WorldProgress.stackables, and anything else is not persisted.
INSTANCE_CATEGORY = "Equipment"
STACKABLE_CATEGORIES = (
    "Consumable", "CraftMaterial", "PolishingMaterial", "Repairer", "Etc", "CraftRecipe",
)
FULL_MOON_STONE_CATEGORY = "FullMoonStone"
QUEST_ITEM_CATEGORY = "QuestItem"

# The P2 capture shows the Cliffs prison key as an individual item with durability 1.
CLIFFS_PRISON_KEY = 120109

# Equipment drops one instance per unit; this bounds the inventory rows one corpse can grant.
MAXIMUM_INSTANCES_PER_ENTRY = 16

# Mirrors kMaxItemPolishStep in src/persistence/character_state_types.hpp.
MAXIMUM_POLISH_STEP = 15

# Item.questLooting values that mean "no quest restriction".
UNRESTRICTED_QUEST_LOOTING = ("", "()", "[]")

# LootCondition columns that restrict who can receive a group. These filters are not modelled,
# so groups using them are skipped.
UNSUPPORTED_CONDITION_FILTERS = (
    "worldLevel",
    "contributionType",
    "publicAble",
    "FK_PlayRecord_Start",
)

# LootCondition columns that this server maps onto the custom first-kill rule.
FIRST_KILL_CONDITION_FILTERS = ("lootingClass", "FK_PlayRecord_End")

# NpcTemplate.grade values that share the Boss_B gold curve. Only Boss_B kills were captured.
BOSS_GRADES = ("Boss_A", "Boss_B", "Boss_C", "Boss_S")
BOSS_REFERENCE_GRADE = "Boss_B"

# Captured corpse-gold samples: (NpcTemplate.grade, Npc.level, gold). Gold was identical for
# every repeated kill of the same NPC, so each distinct NPC contributes one sample. Source:
# the three 2026-10-24 official captures joined to the catalog; no player data is involved.
GOLD_SAMPLES = (
    ("Weak", 4, 3), ("Weak", 6, 3), ("Weak", 7, 3), ("Weak", 8, 3),
    ("Normal", 2, 6), ("Normal", 4, 6), ("Normal", 5, 7), ("Normal", 6, 7),
    ("Normal", 7, 8), ("Normal", 8, 8),
    ("Strong", 2, 9), ("Strong", 5, 11), ("Strong", 8, 13),
    ("Elite", 2, 90), ("Elite", 4, 102), ("Elite", 6, 115), ("Elite", 8, 129),
    ("Boss_B", 2, 338), ("Boss_B", 5, 405), ("Boss_B", 6, 430), ("Boss_B", 7, 457),
    ("Boss_B", 8, 485), ("Boss_B", 9, 516),
)


class UnsupportedGroup(Exception):
    """Raised for a well-formed loot group that this server intentionally does not model."""


def game_variable(source, name):
    matches = [row["value"] for row in source.table("GameVariable") if row["name"] == name]
    if len(matches) != 1:
        raise ValueError(f"Expected one GameVariable named {name}")
    return int(matches[0])


def gold_limit(source):
    matches = [row for row in source.table("Money") if row["type"] == GOLD_CURRENCY]
    if len(matches) != 1:
        raise ValueError("Expected one Gold money definition")
    return int(matches[0]["limitUserMax"])


def parse_rate_percent(text, context):
    rate = float(text)
    if not math.isfinite(rate) or not 0 < rate <= 100:
        raise ValueError(f"Invalid loot rate in {context}")
    return rate


def fit_gold_models(samples=GOLD_SAMPLES):
    """Least-squares gold = base + per_level * level for every sampled NPC grade."""
    levels_by_grade = {}
    for grade, level, gold in samples:
        levels_by_grade.setdefault(grade, []).append((level, gold))

    models = {}
    for grade, points in levels_by_grade.items():
        count = len(points)
        mean_level = sum(level for level, _ in points) / count
        mean_gold = sum(gold for _, gold in points) / count
        spread = sum((level - mean_level) ** 2 for level, _ in points)
        if spread == 0:
            raise ValueError(f"Gold samples for {grade} need at least two distinct levels")
        per_level = sum((level - mean_level) * (gold - mean_gold)
                        for level, gold in points) / spread
        models[grade] = {"base": mean_gold - per_level * mean_level, "per_level": per_level}
    for grade in BOSS_GRADES:
        models.setdefault(grade, models[BOSS_REFERENCE_GRADE])
    return models


def kill_gold(models, grade, level):
    """CUSTOM (guessed) gold of one kill, or 0 for an unsampled grade or invalid level."""
    model = models.get(grade)
    if model is None or level <= 0:
        return 0
    return max(1, math.floor(model["base"] + model["per_level"] * level + 0.5))


def quest_ids(text):
    return [int(value) for value in re.findall(r"\d+", text)]


class LootContext:
    """Catalog lookups shared by every group conversion."""

    def __init__(self, item_row_by_id, known_quest_ids):
        self.item_row_by_id = item_row_by_id
        self.known_quest_ids = known_quest_ids


def item_definition(item_row, known_quest_ids=frozenset()):
    """Return (runtime item facts, custom rules used) for one Item row.

    Raises UnsupportedGroup for item kinds that no rule covers.
    """
    category = item_row["category"]
    rules = set()
    if category == INSTANCE_CATEGORY:
        definition = {"storage": "instance", "grade": item_row["grade"],
                      "max_durability": int(item_row["maxDurability"])}
    elif category in STACKABLE_CATEGORIES:
        definition = {"storage": "stackable", "grade": item_row["grade"], "max_durability": 0}
    elif category == FULL_MOON_STONE_CATEGORY:
        definition = {"storage": "stackable", "grade": item_row["grade"], "max_durability": 0}
        rules.add(RULE_FULL_MOON_STONES)
    elif category == QUEST_ITEM_CATEGORY:
        if int(item_row["id"]) == CLIFFS_PRISON_KEY:
            definition = {"storage": "instance", "grade": item_row["grade"],
                          "max_durability": 1}
        else:
            definition = {"storage": "stackable", "grade": item_row["grade"],
                          "max_durability": 0}
        rules.add(RULE_QUEST_GATED)
    else:
        raise UnsupportedGroup(f"unsupported_category:{category}")

    if item_row["questLooting"] not in UNRESTRICTED_QUEST_LOOTING:
        rules.add(RULE_QUEST_GATED)
    # Only quests that the client's Quest table defines can ever be active, so only those
    # gate the item. An item listing no known quest has no gate (documented fallback).
    listed = quest_ids(item_row["questLooting"])
    definition["required_quests"] = sorted(quest for quest in listed if quest in known_quest_ids)
    return definition, rules


def loot_entry(context, definitions, group_id, row):
    """Convert one LootItem row into (entry, rules).

    Raises ValueError for corrupt rows and UnsupportedGroup for rows no rule covers.
    """
    minimum = int(row["minAmount"])
    maximum = int(row["maxAmount"])
    weight = int(row["dropWeight"])
    if not 0 < minimum <= maximum or weight <= 0:
        raise ValueError(f"Invalid loot amount in group {group_id}")
    if row["grade"]:
        raise UnsupportedGroup("unsupported_item_modifier")

    if row["moneyType"]:
        if row["moneyType"] != GOLD_CURRENCY or row["itemId"] or row["polishStep"]:
            raise UnsupportedGroup(f"unsupported_currency:{row['moneyType']}")
        return {"kind": "gold", "min_quantity": minimum, "max_quantity": maximum,
                "weight": weight}, set()

    item_row = context.item_row_by_id.get(row["itemId"])
    if item_row is None:
        raise UnsupportedGroup("unknown_item")
    definition, rules = item_definition(item_row, context.known_quest_ids)
    if definition["storage"] == "instance" and maximum > MAXIMUM_INSTANCES_PER_ENTRY:
        raise UnsupportedGroup("excessive_instance_quantity")

    entry = {"kind": "item", "item_cms_id": int(row["itemId"]), "min_quantity": minimum,
             "max_quantity": maximum, "weight": weight}
    if row["polishStep"]:
        polish_step = int(row["polishStep"])
        if definition["storage"] != "instance":
            raise UnsupportedGroup("polish_step_on_stackable")
        if not 0 < polish_step <= MAXIMUM_POLISH_STEP:
            raise ValueError(f"Invalid polish step in group {group_id}")
        entry["polish_step"] = polish_step
        rules.add(RULE_POLISH_STEPS)

    definitions[int(row["itemId"])] = definition
    return entry, rules


def condition_rules(condition):
    """Custom rules used by a LootCondition row; raises UnsupportedGroup for other filters."""
    for name in UNSUPPORTED_CONDITION_FILTERS:
        if condition[name]:
            raise UnsupportedGroup(f"unsupported_filter:{name}")
    if any(condition[name] for name in FIRST_KILL_CONDITION_FILTERS):
        return {RULE_FIRST_KILL}
    return set()


def loot_groups(pack_id, conditions, items_by_group, context, definitions, extra_rules=()):
    """Return (supported groups, skipped group records) for one loot pack.

    ``conditions`` are the pack's LootCondition rows; ``items_by_group`` indexes every
    LootItem row by ``lootGroupId``. Item facts of supported groups are added to
    ``definitions``. ``extra_rules`` tags every group (the handling pack uses this).
    """
    group_ids = [row["lootGroupId"] for row in conditions]
    if len(set(group_ids)) != len(group_ids):
        raise ValueError(f"Loot pack {pack_id} repeats a loot group")

    groups = []
    skipped = []
    for condition in conditions:
        group_id = condition["lootGroupId"]
        rate = parse_rate_percent(condition["rate"], f"group {group_id}")
        rows = items_by_group.get(group_id, [])
        if not rows:
            raise ValueError(f"Loot group {group_id} has no items")

        try:
            rules = condition_rules(condition) | set(extra_rules)
            group_definitions = {}
            entries = []
            for row in rows:
                entry, entry_rules = loot_entry(context, group_definitions, group_id, row)
                entries.append(entry)
                rules |= entry_rules
        except UnsupportedGroup as unsupported:
            skipped.append({"loot_pack_id": int(pack_id), "loot_group_id": int(group_id),
                            "reason": str(unsupported)})
            continue

        definitions.update(group_definitions)
        groups.append({"loot_group_id": int(group_id), "rate_percent": rate,
                       "custom_rules": sorted(rules), "entries": entries})
    return groups, skipped


def monster_rows(source):
    return [row for row in source.table("Npc") if row["behaviorType"] == MONSTER_BEHAVIOR]


def handling_rate_percent(row):
    """Npc.handlingRate as a percentage; an empty column means the pack always applies."""
    if not row["handlingRate"]:
        return 100.0
    return parse_rate_percent(row["handlingRate"], f"handling rate of NPC {row['id']}")


def build_pack_list(pack_ids, pack_rate_by_id, conditions_by_pack, items_by_group, context,
                    definitions, extra_rules=()):
    packs = []
    skipped_groups = []
    for pack_id in sorted(pack_ids):
        if str(pack_id) not in pack_rate_by_id:
            raise ValueError(f"Npc references unknown loot pack {pack_id}")
        groups, skipped = loot_groups(pack_id, conditions_by_pack.get(str(pack_id), []),
                                      items_by_group, context, definitions, extra_rules)
        skipped_groups.extend(skipped)
        if groups:
            packs.append({
                "loot_pack_id": pack_id,
                "rate_percent": parse_rate_percent(pack_rate_by_id[str(pack_id)],
                                                   f"pack {pack_id}"),
                "groups": groups,
            })
    return packs, skipped_groups


def build(source):
    item_row_by_id = {row["id"]: row for row in source.table("Item")}
    known_quest_ids = {int(row["id"]) for row in source.table("Quest")}
    context = LootContext(item_row_by_id, known_quest_ids)
    pack_rate_by_id = {row["id"]: row["rate"] for row in source.table("LootPack")}
    template_grade = {row["id"]: row["grade"] for row in source.table("NpcTemplate")}

    conditions_by_pack = {}
    for row in source.table("LootCondition"):
        conditions_by_pack.setdefault(row["FK_lootPack"], []).append(row)
    items_by_group = {}
    for row in source.table("LootItem"):
        items_by_group.setdefault(row["lootGroupId"], []).append(row)

    monsters = monster_rows(source)
    loot_pack_ids = {int(row["FK_lootPackId"]) for row in monsters if row["FK_lootPackId"]}
    handling_pack_ids = {int(row["FK_handlingLootPackId"]) for row in monsters
                         if row["FK_handlingLootPackId"]}

    definitions = {}
    packs, skipped_groups = build_pack_list(loot_pack_ids, pack_rate_by_id, conditions_by_pack,
                                            items_by_group, context, definitions)
    handling_packs, skipped_handling = build_pack_list(
        handling_pack_ids, pack_rate_by_id, conditions_by_pack, items_by_group, context,
        definitions, extra_rules=(RULE_HANDLING,))
    skipped_groups.extend(skipped_handling)

    supported_packs = {pack["loot_pack_id"] for pack in packs}
    supported_handling = {pack["loot_pack_id"] for pack in handling_packs}
    gold_models = fit_gold_models()

    npcs = []
    for row in monsters:
        npc_id = int(row["id"])
        pack_id = int(row["FK_lootPackId"]) if row["FK_lootPackId"] else 0
        pack_id = pack_id if pack_id in supported_packs else 0
        handling_id = int(row["FK_handlingLootPackId"]) if row["FK_handlingLootPackId"] else 0
        handling_id = handling_id if handling_id in supported_handling else 0
        handling_rate = handling_rate_percent(row) if handling_id else 0.0
        gold = kill_gold(gold_models, template_grade.get(row["FK_template"]), int(row["level"]))
        if pack_id or handling_id or gold:
            npcs.append([npc_id, pack_id, handling_id, handling_rate, gold])
    npcs.sort()

    return {
        "format_version": FORMAT_VERSION,
        "build_id": BUILD_ID,
        "looting_range_mm": game_variable(source, "LootingRange") * MILLIMETRES_PER_CENTIMETRE,
        "corpse_lootable_ms": game_variable(source, "NpcCorpseLootingDestroyMillis"),
        "corpse_empty_ms": game_variable(source, "NpcCorpseNoneDestroyMillis"),
        "gold_limit": gold_limit(source),
        "kill_gold_model": {
            "custom": True,
            "form": "round(base + per_level * level), at least 1",
            "samples": len(GOLD_SAMPLES),
            "grades": {grade: gold_models[grade] for grade in sorted(gold_models)},
        },
        "items": {str(item_id): definitions[item_id] for item_id in sorted(definitions)},
        "packs": packs,
        "handling_packs": handling_packs,
        "npc_fields": ["npc_cms_id", "loot_pack_id", "handling_pack_id",
                       "handling_rate_percent", "kill_gold"],
        "npcs": npcs,
        "skipped_groups": sorted(skipped_groups, key=lambda entry: (
            entry["loot_pack_id"], entry["loot_group_id"])),
        "sources": source.sources,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/loot/definitions.json"))
    args = parser.parse_args()

    profile = build(Source(args.source))
    write_profile(args.output, profile)
    print(json.dumps({
        "npcs": len(profile["npcs"]),
        "packs": len(profile["packs"]),
        "handling_packs": len(profile["handling_packs"]),
        "items": len(profile["items"]),
        "skipped_groups": len(profile["skipped_groups"]),
    }))


if __name__ == "__main__":
    main()
