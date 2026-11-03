"""Build the housing doodad phase profile (gardens and interactive decorations).

The output contains static CMS data only: no character, account, item-instance or session
identifier is copied into the runtime profile.

Scope (client build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa): every doodad a housing lot can
hold (HousingObject/HousingBuilding Item.FK_doodadId, HousingPresetDoodad.FK_doodad and
HousingModelPartAddOn.doodadCmsId) whose DoodadPhaseFlow rows include an Interaction trigger.
In this build that is the three garden plots (160066, 160068, 160069), decoration 161124 and the
preset farm object 180005.

Evidence and joins:

* ``DoodadPhaseFlow`` is the phase graph. A phase may hold several transitions: several
  Interaction triggers (a garden plot is planted by one interaction per seed) and a Timer
  trigger (growth, rest and decay). ``nextPhaseId`` or the weighted ``nextPhaseIdRatio``
  chooses the next phase.
* The client starts these interactions like doodad gathering:
  ``StartInteractionAnimReq{param.interaction.interaction_id, target_unit_id}`` and, for
  animations without ``noExecuteNotify``, ``ExecuteInteractionReq`` at the montage's execute
  event. ``AAC_PS2C_ChangeDoodadPhaseNoti_Handle`` (0x1461D3910) applies a phase change to a
  placed housing doodad named by its housing doodad unit ID.
* ``InteractionCondition`` rows of these interactions are ConsumeItem (seeds, 5 per planting)
  and OwnItem (decoration 161124's quest trophies). Both are single-group AND rows checked on
  the caster.
* Rewards: ExpertiseReward tasks name one loot pack plus expertise experience; LootingPack tasks
  name a pack directly. Both reuse the gathering exporter's loot joins.
* ExecSkillEffect tasks (withering, skill effect 12000008) are recorded but not applied.
* Tools are not checked: captured doodad gathering never required or wore the authored tool
  (the gathering runtime checks tools only for placements and butchering), so the Sickle that
  garden harvesting animations name is not enforced either (Candidate).

Transitions this server cannot honour (unsupported trigger, condition or task, or an
interaction with no next phase) are listed under ``unsupported_transitions`` and omitted; the
remaining transitions of the same phase stay usable.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from build_gathering_data import (  # noqa: E402
    animation_definition,
    game_variable_ms,
    interaction_trigger_id,
    loot_pack_reward,
    next_phases,
    validate_reward_items,
    SP_REGEN_DELAY_VARIABLE,
)
from build_quest_catalog import fragment as lenient_fragment  # noqa: E402
from build_tutorial_data import BUILD_ID, Source, fragment, write_profile  # noqa: E402

FORMAT_VERSION = 1
# Garden phases last up to five days (432,000,000 ms, ripe crops before they compost).
MAXIMUM_PHASE_DURATION_MS = 30 * 24 * 60 * 60 * 1000
SUPPORTED_CONDITIONS = ("ConsumeItem", "OwnItem")
REWARD_TASKS = ("ExpertiseReward", "LootingPack")


class UnsupportedTransition(ValueError):
    """A single DoodadPhaseFlow row the runtime cannot honour."""


def housing_doodad_ids(source):
    """Doodad IDs a housing lot can hold, with where they come from."""
    origins = {}
    for item in source.table("Item"):
        if item["category"] in ("HousingObject", "HousingBuilding") and item["FK_doodadId"]:
            origins.setdefault(int(item["FK_doodadId"]), "item " + item["id"])
    for preset in source.table("HousingPresetDoodad"):
        origins.setdefault(int(preset["FK_doodad"]), "preset")
    for add_on in source.table("HousingModelPartAddOn"):
        origins.setdefault(int(add_on["doodadCmsId"]), "building add-on")
    return origins


def interaction_conditions(source, condition_id):
    """Return the caster conditions of one interaction as runtime entries."""
    if not condition_id:
        return []

    conditions = []
    for row in source.table("InteractionCondition"):
        if row["conditionId"] != condition_id:
            continue
        if row["typeCase"] not in SUPPORTED_CONDITIONS:
            raise UnsupportedTransition(f"Condition {condition_id} uses {row['typeCase']}")
        if (row["group"] != "1" or row["logicalOperator"] != "AND" or row["negated"]
                or row["checkOnType"] != "Caster"):
            raise UnsupportedTransition(f"Condition {condition_id} is not a caster AND row")
        parameters = fragment(row["type"])
        count = int(parameters["count"])
        if count <= 0:
            raise UnsupportedTransition(f"Condition {condition_id} has a non-positive count")
        conditions.append({
            "kind": "consume" if row["typeCase"] == "ConsumeItem" else "own",
            "item_cms_id": int(parameters["itemId"]),
            "count": count,
        })
    if not conditions:
        raise UnsupportedTransition(f"Condition {condition_id} has no rows")
    return conditions


def expertise_reward(source, reward_id):
    """Resolve an ExpertiseReward into its single loot pack and expertise experience.

    Garden rows include an unquoted key (reward 160010: ``lootPackId:1600662,"count":1``), so
    rows are read with the quest exporter's lenient fragment parser. Returns None when the
    reward names no loot pack.
    """
    rows = [row for row in source.table("ExpertiseReward") if row["id"] == str(reward_id)]
    packs = [lenient_fragment(row["type"]) for row in rows if row["typeCase"] == "LootPack"]
    experience = [lenient_fragment(row["type"])["value"] for row in rows
                  if row["typeCase"] == "Exp"]
    if not packs:
        return None
    if len(packs) != 1 or int(packs[0]["count"]) != 1 or len(experience) > 1:
        raise UnsupportedTransition(f"Unsupported repeated expertise reward: {reward_id}")

    reward = loot_pack_reward(source, int(packs[0]["lootPackId"]))
    # Recorded for provenance; housing does not grant expertise experience.
    reward["expertise_experience"] = int(experience[0]) if experience else 0
    return reward


def transition_reward(source, flow):
    """The reward a transition grants, or None for tasks without one."""
    task_case = flow["taskTypeCase"]
    if task_case in ("NoneTask", "ExecSkillEffect"):
        return None
    if task_case not in REWARD_TASKS:
        raise UnsupportedTransition(f"Task {task_case} is not supported")

    task = fragment(flow["taskType"])
    if task_case == "LootingPack":
        reward = loot_pack_reward(source, task["id"])
        reward["reason"] = "looting"
        return reward
    reward = expertise_reward(source, task["id"])
    if reward is None:
        raise UnsupportedTransition(f"Expertise reward {task['id']} has no loot pack")
    reward["reason"] = "system"
    return reward


def interaction_transition(source, flow, animations):
    """Translate an Interaction-triggered flow row."""
    if flow["interactionCount"] != "1":
        raise UnsupportedTransition("Interaction counts other than 1 are not supported")
    if not flow["nextPhaseId"] and not flow["nextPhaseIdRatio"]:
        raise UnsupportedTransition("The interaction names no next phase")

    interaction_id = interaction_trigger_id(flow["triggerType"])
    interaction = source.row("Interaction", interaction_id)
    if interaction["playerTaskCase"] != "Use" or not interaction["FK_interactionAnimId"]:
        raise UnsupportedTransition(f"Interaction {interaction_id} is not an animated Use")
    animation_id = int(interaction["FK_interactionAnimId"])
    if animation_id not in animations:
        try:
            # Garden montages (planting 160199, harvesting 160002) time differently per character
            # rig; the earliest execute and longest duration never refuse a valid rig.
            animations[animation_id] = animation_definition(source, animation_id,
                                                            earliest_across_rigs=True)
        except ValueError as error:
            raise UnsupportedTransition(f"Animation {animation_id}: {error}") from error

    transition = {
        "trigger": "interaction",
        "interaction_id": interaction_id,
        "interaction_anim_id": animation_id,
        "conditions": interaction_conditions(source, interaction["FK_conditionId"]),
        "next": next_phases(flow),
    }
    reward = transition_reward(source, flow)
    if reward is not None:
        transition["reward"] = reward
    if flow["taskTypeCase"] == "ExecSkillEffect":
        transition["skill_effect_id"] = int(fragment(flow["taskType"])["id"])
    return transition


def timer_transition(source, flow):
    """Translate a Timer-triggered flow row."""
    trigger = fragment(flow["triggerType"])
    duration_ms = int(trigger["duration"])
    if not 0 < duration_ms <= MAXIMUM_PHASE_DURATION_MS:
        raise UnsupportedTransition(f"Timer duration {duration_ms} ms is out of range")
    if flow["taskTypeCase"] not in ("NoneTask", "ExecSkillEffect"):
        raise UnsupportedTransition(f"Timer task {flow['taskTypeCase']} is not supported")

    transition = {"trigger": "timer", "duration_ms": duration_ms, "next": next_phases(flow)}
    if flow["taskTypeCase"] == "ExecSkillEffect":
        transition["skill_effect_id"] = int(fragment(flow["taskType"])["id"])
    return transition


def doodad_definition(source, doodad_id, flows, animations, unsupported):
    """Build one doodad's phase graph; returns None when it has no usable interaction."""
    phases = {}
    for flow in flows:
        phase_id = int(flow["phaseId"])
        try:
            if flow["triggerTypeCase"] == "Interaction":
                transition = interaction_transition(source, flow, animations)
            elif flow["triggerTypeCase"] == "Timer":
                transition = timer_transition(source, flow)
            else:
                raise UnsupportedTransition(f"Trigger {flow['triggerTypeCase']} is not supported")
        except UnsupportedTransition as error:
            unsupported.append({"doodad_id": doodad_id, "phase_id": phase_id,
                                "trigger": flow["triggerType"], "reason": str(error)})
            continue
        phases.setdefault(phase_id, []).append(transition)

    if not any(transition["trigger"] == "interaction"
               for transitions in phases.values() for transition in transitions):
        return None
    return {
        "doodad_id": doodad_id,
        "phases": [{"phase_id": phase_id, "transitions": transitions}
                   for phase_id, transitions in sorted(phases.items())],
    }


def build(source):
    origins = housing_doodad_ids(source)
    flows_by_doodad = {}
    for flow in source.table("DoodadPhaseFlow"):
        doodad_id = int(flow["FK_doodad"])
        if doodad_id in origins:
            flows_by_doodad.setdefault(doodad_id, []).append(flow)

    animations = {}
    unsupported = []
    doodads = []
    for doodad_id, flows in sorted(flows_by_doodad.items()):
        if not any(flow["triggerTypeCase"] == "Interaction" for flow in flows):
            continue
        definition = doodad_definition(source, doodad_id, flows, animations, unsupported)
        if definition is not None:
            definition["origin"] = origins[doodad_id]
            doodads.append(definition)

    rewards = [transition["reward"] for doodad in doodads for phase in doodad["phases"]
               for transition in phase["transitions"] if "reward" in transition]
    validate_reward_items(source, rewards)

    return {
        "format_version": FORMAT_VERSION,
        "build_id": BUILD_ID,
        "sp_regen_delay_ms": game_variable_ms(source, SP_REGEN_DELAY_VARIABLE),
        "animations": [animations[key] for key in sorted(animations)],
        "doodads": doodads,
        "unsupported_transitions": unsupported,
        "sources": source.sources,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/housing/doodads.json"))
    args = parser.parse_args()

    profile = build(Source(args.source))
    write_profile(args.output, profile)
    print(json.dumps({
        "doodads": len(profile["doodads"]),
        "animations": len(profile["animations"]),
        "unsupported_transitions": len(profile["unsupported_transitions"]),
    }))


if __name__ == "__main__":
    main()
