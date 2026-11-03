"""Recover bounded skill/action definitions and sanitized tutorial observations.

The optional capture import keeps only CMS identifiers, NPC stat values, and
relative skill-event timings. It never writes character, item-instance, or
session identifiers into versioned protocol inputs.
"""

import argparse
import math
import collections
import hashlib
import json
import re
import statistics
from pathlib import Path

from tutorial_spawns import npc_ai_definition


CLASS_IDS = {"Fight": 1, "Wild": 2, "Adamant": 3, "Magic": 5}
_MISSING = object()
OBSERVATIONS = Path(__file__).resolve().parents[1] / "protocol/tutorial-combat-observations.json"
CAPTURE_TICKS_PER_MILLISECOND = 1000000
# Every placed world monster type contributes its skills; mirrors kMaxCombatSkills in
# src/world/features/combat/combat.cpp.
MAX_COMBAT_SKILLS = 4096


def fragment(text):
    if not text:
        return {}
    # Three CMS removeStatusEffect rows use bare property names.
    normalized = re.sub(r'(^|,)\s*([A-Za-z_][A-Za-z_0-9]*)\s*:', r'\1"\2":', text)
    return json.loads("{" + normalized + "}")


def capture_rows(path):
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            yield json.loads(line)


def collect_observations(capture, open_world=False):
    starts = {}
    events = collections.defaultdict(list)
    npc_stats = {}
    npc_units = {}
    npc_skills = collections.defaultdict(set)
    player_id = None
    map_id = None
    for row in capture_rows(capture):
        message = row["message"]
        packet = row["packet"]
        if packet == "PS2C_MapMoveStartNoti":
            map_id = message["map_id"]
            starts.clear()
            # Unit ids are only unique within a map visit; a carried-over id would attribute a
            # later NPC's casts to the previous NPC that used the same id.
            npc_units.clear()
        if not open_world and map_id not in (8000, 10117):
            continue
        if packet == "PS2C_MapMoveCompleteNoti":
            player_id = str(message["player_unit_id"])
        elif packet == "PS2C_UnitRemoveNoti":
            # Removed ids are recycled by the official server for other units.
            for removed in message.get("unit_ids", []):
                npc_units.pop(str(removed), None)
        elif packet == "PS2C_UnitAddNoti" and "npc" in message.get("nub", {}):
            npc = message["nub"]["npc"]
            cms_id = npc["npc_cms_id"]
            npc_units[str(message["unit_id"])] = cms_id
            stats = dict(npc.get("stats", {}))
            # HP/SP on an already visible unit are transient, not its definition.
            stats["69"] = stats.get("18", 1)
            stats["72"] = stats.get("19", 1)
            npc_stats.setdefault(str(cms_id), stats)
        elif packet == "PC2S_StartSkillReq" and player_id is not None:
            skill = message["start_skill"]
            starts[(player_id, skill["instance_id"])] = (
                skill["skill_id"], skill.get("layer_index", 0), 0, int(skill["tick"])
            )
        elif packet == "PS2C_StartSkillNoti":
            unit_id = str(message["unit_id"])
            skill = message["start_skill"]
            key = (unit_id, skill["instance_id"])
            previous = starts.get(key)
            # A local stage input can precede the echoed start. Keep its stage clock instead
            # of attributing a charged release to stage zero (observed for Thunderbolt 2249).
            if previous is None or previous[0] != skill["skill_id"] or previous[2] == 0:
                starts[key] = (
                    skill["skill_id"], skill.get("layer_index", 0), 0, int(skill["tick"])
                )
            if unit_id in npc_units:
                npc_skills[npc_units[unit_id]].add(skill["skill_id"])
        elif packet == "PC2S_ChangeSkillStageReq":
            key = (player_id, message["skill_instance_id"])
            if key in starts:
                skill, layer, _, _ = starts[key]
                starts[key] = (skill, layer, message["stage_index"], int(message["tick"]))
        elif packet in ("PC2S_ExecuteSkillEffectReq", "PC2S_ExecuteSpawnProjectileSkillEffectReq"):
            start = starts.get((player_id, message["skill_instance_id"]))
            if start is None:
                continue
            skill, layer, stage, start_tick = start
            event = message["skill_effect"]
            # The official trace uses nanoseconds; the emulator's runtime clock
            # uses 100 ns ticks. Only these normalized millisecond delays cross
            # that boundary. Capture elapsed_ms independently confirms the unit.
            delay_ms = (int(message["tick"]) - start_tick) / CAPTURE_TICKS_PER_MILLISECOND
            if 0 <= delay_ms <= 10000:
                key = (skill, layer, stage, event["effect_id"], event.get("unique_effect_id", 0))
                events[key].append(delay_ms)

    capture = Path(capture)
    native_session_path = capture.parent / "native-session.bin"
    native_session_sha256 = None
    if native_session_path.exists():
        native_session_sha256 = hashlib.sha256(native_session_path.read_bytes()).hexdigest()
    # Stable dataset label: directory name, not absolute path
    dataset_label = capture.parent.name if capture.parent.name else capture.stem

    return {
        "format_version": 1,
        "capture_path": dataset_label,
        "decoded_sha256": hashlib.sha256(capture.read_bytes()).hexdigest(),
        "native_session_sha256": native_session_sha256,
        "capture_sha256": native_session_sha256,  # matches docs/capture-corpus.md
        "npc_stats": npc_stats,
        "npc_skills": {str(cms): sorted(skills) for cms, skills in npc_skills.items()},
        "skill_events": [
            {"skill_id": skill, "layer": layer, "stage": stage, "effect_id": effect,
             "unique_id": unique, "time_ms": round(statistics.median(delays)),
             "samples": len(delays), "min_ms": round(min(delays)), "max_ms": round(max(delays))}
            for (skill, layer, stage, effect, unique), delays in sorted(events.items())
        ],
    }
def merge_observations(observation_list):
    merged = {
        "format_version": 1,
        "capture_hashes": {},
        "npc_stats": {},
        "npc_skills": collections.defaultdict(set),
        "skill_events": [],
    }
    event_map = collections.defaultdict(list)
    for idx, obs in enumerate(observation_list):
        label = obs.get("capture_path", f"capture_{idx}")
        merged["capture_hashes"][f"{label}:decoded"] = obs.get("decoded_sha256", obs.get("capture_sha256"))
        if obs.get("native_session_sha256"):
            merged["capture_hashes"][f"{label}:native_session"] = obs["native_session_sha256"]
        if obs.get("capture_sha256") and obs.get("capture_sha256") != obs.get("decoded_sha256"):
            merged["capture_hashes"][f"{label}:capture_sha256"] = obs["capture_sha256"]
        for cms_id, stats in obs.get("npc_stats", {}).items():
            if cms_id not in merged["npc_stats"]:
                merged["npc_stats"][cms_id] = stats
        for cms_id, skills in obs.get("npc_skills", {}).items():
            merged["npc_skills"][cms_id].update(skills)
        for event in obs.get("skill_events", []):
            key = (event["skill_id"], event["layer"], event["stage"],
                   event["effect_id"], event["unique_id"])
            event_map[key].extend([event["time_ms"]] * event.get("samples", 1))
    for key, delays in sorted(event_map.items()):
        skill, layer, stage, effect, unique = key
        merged["skill_events"].append({
            "skill_id": skill, "layer": layer, "stage": stage, "effect_id": effect,
            "unique_id": unique, "time_ms": round(statistics.median(delays)),
            "samples": len(delays), "min_ms": round(min(delays)), "max_ms": round(max(delays)),
        })
    merged["npc_skills"] = {cms: sorted(skills) for cms, skills in merged["npc_skills"].items()}
    return merged


def npc_skill_min_range_mm(skill, range_mm):
    """Skill.minDistance in millimeters (catalog centimeters x 10, like maxDistance)."""
    min_range_mm = round(float(skill["minDistance"] or 0) * 10)
    if min_range_mm < 0 or min_range_mm > range_mm:
        raise ValueError(
            f"Skill {skill['id']} has minDistance {skill['minDistance']} outside 0..maxDistance "
            f"{skill['maxDistance']}")
    return min_range_mm


SHAPE_KEYS = {"cylinderSectorShape": "cylinder_sector", "boxShape": "box", "sphereShape": "sphere"}


def event_shape(catalog_event):
    """Per-event hit shape of one Action skillEffects[] entry, or None when it has none.

    cylinder_sector (verified against captured hits, see combat/per-event-shapes.md): authored
    centimeters become millimeters (radii, half height, offset); angles stay degrees.
    axis_degrees is collisionDegree, the sector axis relative to the caster yaw; angle_degrees is
    the full sector width. offset_mm.x is forward along the caster yaw, y to the left (same
    rotation sense as yaw). box and sphere are exported as authored with verified false: their
    units and transform are not established, so the runtime must not use them.
    """
    for key, kind in SHAPE_KEYS.items():
        authored = catalog_event.get(key)
        if authored is None:
            continue
        if kind != "cylinder_sector":
            return {"kind": kind, "verified": False, "authored": authored}
        offset = authored.get("collisionOffset", {})
        return {
            "kind": kind,
            "verified": True,
            "outer_radius_mm": round(float(authored["outerRadius"]) * 10),
            "inner_radius_mm": round(float(authored.get("innerRadius", 0)) * 10),
            "half_height_mm": round(float(authored.get("halfHeight", 0)) * 10),
            "angle_degrees": float(authored["angle"]),
            "axis_degrees": float(authored.get("collisionDegree", 0)),
            "offset_mm": {
                "x": round(float(offset.get("x", 0)) * 10),
                "y": round(float(offset.get("y", 0)) * 10),
                "z": round(float(offset.get("z", 0)) * 10),
            },
        }
    return None


def with_event_shape(event_definition, catalog_event):
    shape = event_shape(catalog_event) if catalog_event else None
    if shape:
        event_definition["shape"] = shape
    return event_definition


ADAPTED_CHASE_TARGET = "EX5RootMotionType_AdaptedChaseTarget"
BUILT_DATA = Path("raw/X5/Content/Data/BuiltData")
POSITION_TIME_BASE = "montage_ms"
# positionDatas steps are uniform when they differ by at most this much (authored centimeters).
POSITION_STEP_TOLERANCE_CM = 0.01


def cm_to_mm(value):
    return round(float(value) * 10, 3)


def position_data_block(position_datas):
    """Montage rootMotion.positionDatas: the baked root translation per elapsed time.

    elapsed_ms is on the MONTAGE timeline, not relative to the window start: the first sample
    sits at the block's window_start_ms and the last at window_end_ms (1006 and 1313 for skill
    190100200). time_base says so explicitly; values are exactly as authored.

    Samples are exported in full (mm = authored cm x 10). uniform is true when the elapsed
    step and the per-axis step are constant within POSITION_STEP_TOLERANCE_CM, so that a
    consumer could replace the samples by count and end value; whether the client uses the
    curve as progress is not established.
    """
    samples = [
        {"elapsed_ms": data["elapsedTime"],
         "x_mm": cm_to_mm(data["pos"]["x"]),
         "y_mm": cm_to_mm(data["pos"]["y"]),
         "z_mm": cm_to_mm(data["pos"]["z"])}
        for data in position_datas
    ]
    uniform = len(position_datas) >= 2
    for previous, current in zip(position_datas, position_datas[1:]):
        first_previous, first_current = position_datas[0], position_datas[1]
        if abs((current["elapsedTime"] - previous["elapsedTime"])
               - (first_current["elapsedTime"] - first_previous["elapsedTime"])) > 1:
            uniform = False
        for axis in ("x", "y", "z"):
            step = current["pos"][axis] - previous["pos"][axis]
            first_step = first_current["pos"][axis] - first_previous["pos"][axis]
            if abs(step - first_step) > POSITION_STEP_TOLERANCE_CM:
                uniform = False
    return {"time_base": POSITION_TIME_BASE, "uniform": uniform,
            "sample_count": len(samples), "samples": samples}


def adapted_chase_block(root_motion):
    """RootMotion_AdaptedChaseTarget of a montage, as serialized, in profile units.

    Distances are authored centimeters x 10 (_mm), times milliseconds, angles degrees. No
    standoff is derived: the end distance offset + caster radius + target radius is Candidate
    and left to the consumer (see combat/lunge-data.md).
    """
    setup = root_motion["adaptedSetupData"]
    rotating = setup.get("rotatingSetupData", {})
    return {
        "type": root_motion["type"],
        "window_start_ms": root_motion["startTime"],
        "window_end_ms": root_motion["endTime"],
        "max_move_distance_mm": cm_to_mm(setup["maxMoveDistance"]),
        "offset_distance_mm": cm_to_mm(setup["offsetDistance"]),
        "offset_basis_pos_type": setup["offsetBasisPosType"],
        "offset_basis_dir_type": setup["offsetBasisDirType"],
        "offset_direction_degrees": setup["offsetDirectionDegree"],
        "look_at_dir_type": rotating.get("directionType"),
        "rotation_angular_speed_degrees": rotating.get("rotationAngularSpeed"),
        "using_scale": root_motion["usingScale"],
        "ground_move": root_motion["groundMove"],
        "ignore_collision": root_motion["ignoreCollision"],
        "end_ground_move": root_motion.get("endGroundMove", False),
        "position_data": position_data_block(root_motion.get("positionDatas", [])),
    }


def stage_adapted_chase(source, anim_id):
    """The stage's adapted-chase block from its montage, or None for any other root motion."""
    if not anim_id:
        return None
    path = BUILT_DATA / "Anim/Skill" / (anim_id + ".json")
    if not (source.root / path).exists():
        return None
    root_motion = json.loads(source.read(path)).get("rootMotion")
    if not root_motion or root_motion.get("type") != ADAPTED_CHASE_TARGET:
        return None
    return adapted_chase_block(root_motion)


def stage_player_motion(source, anim_id):
    """Authored bounds for validating a player's requested skill movement.

    Keep this independent of the NPC chase solver: player montages also use Adapted and
    AnimMovePos. The server uses their distance/window bounds without treating a packet as
    permission to teleport or to move another unit.
    """
    if not anim_id:
        return None
    path = BUILT_DATA / "Anim/Skill" / (anim_id + ".json")
    raw_path = Path(*path.parts[1:]).as_posix()
    if raw_path.casefold() not in source.raw_paths_casefolded:
        return None
    motion = json.loads(source.read(path)).get("rootMotion")
    if not motion:
        return None

    samples = position_data_block(motion.get("positionDatas", []))["samples"]
    path_length = sum(
        math.dist((first["x_mm"], first["y_mm"], first["z_mm"]),
                  (second["x_mm"], second["y_mm"], second["z_mm"]))
        for first, second in zip(samples, samples[1:])
    )
    setup = motion.get("adaptedSetupData", {})
    maximum = cm_to_mm(setup["maxMoveDistance"]) if "maxMoveDistance" in setup else path_length
    values = (maximum, path_length, motion["startTime"], motion["endTime"])
    if any(not math.isfinite(value) or value < 0 for value in values):
        raise ValueError(f"Invalid player motion bounds in {path}")
    if motion["endTime"] < motion["startTime"]:
        raise ValueError(f"Reversed player motion window in {path}")
    return {
        "type": motion["type"],
        "start_ms": motion["startTime"],
        "end_ms": motion["endTime"],
        "max_distance_mm": maximum,
        "path_length_mm": path_length,
        "ground_move": motion["groundMove"],
        "using_scale": motion["usingScale"],
    }


def npc_collision_definitions(source, npc_skills):
    """gameHitCollisionData capsule of every NPC with skills: Npc -> UnitModel -> preset file."""
    models = {row["id"]: row for row in source.table("UnitModel")}
    npcs = {row["id"]: row for row in source.table("Npc")}
    result = {}
    for npc_id in sorted(npc_skills, key=int):
        model_id = npcs[npc_id]["FK_modelId"]
        preset_id = models[model_id]["fk_CollisionPresetId"]
        path = BUILT_DATA / "UnitCollision" / f"UnitModelCollisionSet_{preset_id}.json"
        if not (source.root / path).exists():
            raise ValueError(
                f"NPC {npc_id} model {model_id} has no collision preset file {preset_id}")
        capsule = json.loads(source.read(path))["gameHitCollisionData"]["capsuleShape"]
        result[npc_id] = {
            "model_id": int(model_id),
            "collision_preset_id": int(preset_id),
            "game_hit_capsule_radius_mm": cm_to_mm(capsule["radius"]),
            "game_hit_capsule_half_height_mm": cm_to_mm(capsule["halfHeight"]),
        }
    return result


def player_collision_definitions(source):
    """Target capsule of every player model: Character -> UnitModel -> collision preset file.

    Keyed by UnitModel id. Each entry names the Character rows (race/gender) that use the model
    and the three source files it was read from. The run fails when the table has no Character
    rows or a preset file is missing: the lunge standoff (Candidate) needs a real target radius.
    """
    models = {row["id"]: row for row in source.table("UnitModel")}
    characters = source.table("Character")
    if not characters:
        raise ValueError("The Character table has no rows; no player capsule can be exported")
    by_model = {}
    for row in characters:
        by_model.setdefault(row["FK_modelId"], set()).add(f"{row['race']}/{row['gender']}")
    result = {}
    for model_id in sorted(by_model, key=int):
        preset_id = models[model_id]["fk_CollisionPresetId"]
        path = BUILT_DATA / "UnitCollision" / f"UnitModelCollisionSet_{preset_id}.json"
        if not (source.root / path).exists():
            raise ValueError(f"Player model {model_id} has no collision preset file {preset_id}")
        capsule = json.loads(source.read(path))["gameHitCollisionData"]["capsuleShape"]
        result[model_id] = {
            "characters": sorted(by_model[model_id]),
            "collision_preset_id": int(preset_id),
            "game_hit_capsule_radius_mm": cm_to_mm(capsule["radius"]),
            "game_hit_capsule_half_height_mm": cm_to_mm(capsule["halfHeight"]),
            "sources": ["catalog/Tables/Character.jsonl", "catalog/Tables/UnitModel.jsonl",
                        path.as_posix()],
        }
    return result


CATALOG_BOOLEANS = {"True": True, "False": False, "": False}


def catalog_bool(skill, column):
    value = skill[column]
    if value not in CATALOG_BOOLEANS:
        raise ValueError(f"Skill {skill['id']} has an unexpected {column} value {value!r}")
    return CATALOG_BOOLEANS[value]


def npc_skill_selection_fields(skill, range_mm):
    """Catalog Skill columns the NPC attack choice needs, exported for NPC skills only.

    min_range_mm: minDistance in millimeters (catalog cm x 10, like range_mm).
    is_basic_attack: isBasicAttack, "True" -> true, "False" or blank -> false.
    skill_type: skillType verbatim (Normal, Dodge, Guard, ...). It has the same value as the
    existing "type" field, repeated here under the name the selection rule reads.
    """
    return {
        "min_range_mm": npc_skill_min_range_mm(skill, range_mm),
        "is_basic_attack": catalog_bool(skill, "isBasicAttack"),
        "skill_type": skill["skillType"],
    }


def union_npc_skills(baseline, extractions):
    """Add the NPC skills seen in the given captures to an existing observation document.

    Only npc_skills, npc_skill_sources and capture_hashes change; npc_stats and skill_events
    stay exactly as in the baseline, because they cannot be regenerated from every capture.
    A baseline skill can never be dropped. Returns the new document and the added skills.
    """
    skills = {cms: set(values) for cms, values in baseline.get("npc_skills", {}).items()}
    sources = collections.defaultdict(lambda: collections.defaultdict(set))
    hashes = dict(baseline.get("capture_hashes", {}))
    for extraction in extractions:
        label = extraction["capture_path"]
        hashes[f"{label}:decoded"] = extraction["decoded_sha256"]
        if extraction.get("native_session_sha256"):
            hashes[f"{label}:native_session"] = extraction["native_session_sha256"]
        for cms, values in extraction["npc_skills"].items():
            skills.setdefault(cms, set()).update(values)
            for skill in values:
                sources[cms][str(skill)].add(label)

    added = {
        cms: sorted(values - set(baseline.get("npc_skills", {}).get(cms, ())))
        for cms, values in skills.items()
        if values - set(baseline.get("npc_skills", {}).get(cms, ()))
    }
    result = dict(baseline)
    result["npc_skills"] = {cms: sorted(values) for cms, values in skills.items()}
    result["npc_skill_sources"] = {
        cms: {
            skill: sorted(labels)
            for skill, labels in sorted(per_skill.items(), key=lambda item: int(item[0]))
        }
        for cms, per_skill in sources.items()
    }
    result["capture_hashes"] = hashes
    return result, added


def validate_projectile_fields(projectiles, referenced_ids):
    errors = []
    for pid in sorted(referenced_ids):
        pdata = projectiles.get(str(pid))
        if pdata is None:
            errors.append(f"projectile {pid}: missing definition")
            continue
        # parameters must be a dict (C++: definition.at("parameters"))
        params = pdata.get("parameters")
        if not isinstance(params, dict):
            errors.append(f"projectile {pid}: missing or malformed 'parameters'")
            continue
        # duration_ms must be int (not bool/float/str/null) within int32
        # (C++: definition.at("duration_ms").get<int>())
        dur = pdata.get("duration_ms", _MISSING)
        if dur is _MISSING:
            errors.append(f"projectile {pid}: missing 'duration_ms'")
        elif isinstance(dur, bool) or not isinstance(dur, int):
            errors.append(f"projectile {pid}: 'duration_ms' must be int, got {type(dur).__name__}")
        elif dur < -2147483648 or dur > 2147483647:
            errors.append(f"projectile {pid}: 'duration_ms' out of int32 range")
        # speed if present must be finite real (not bool/str/null/NaN/inf)
        # (C++: definition.at("parameters").value("speed", 0.0) as double)
        if "speed" in params:
            speed = params["speed"]
            if isinstance(speed, bool) or not isinstance(speed, (int, float)):
                errors.append(f"projectile {pid}: 'speed' must be a real number")
            elif not math.isfinite(speed):
                errors.append(f"projectile {pid}: 'speed' must be finite")
    return errors


def validate_status_effect_references(effects, status_effects):
    """Check that addStatusEffect/removeStatusEffect rows point at exported StatusEffect rows.

    Only the referential contract is proven from the catalog (every referenced id resolves).
    The meaning of rate, stackCount and level is not enforced; see
    docs/combat-effect-kind-contract.md.
    """
    errors = []
    for effect_id, effect in sorted(effects.items(), key=lambda item: int(item[0])):
        if effect["kind"] not in ("addStatusEffect", "removeStatusEffect"):
            continue
        parameters = effect["parameters"]
        if "statusEffectId" not in parameters:
            if effect["kind"] == "addStatusEffect":
                errors.append(f"effect {effect_id}: addStatusEffect has no 'statusEffectId'")
            continue
        status_effect_id = parameters["statusEffectId"]
        is_number = isinstance(status_effect_id, (int, float)) and not isinstance(
            status_effect_id, bool)
        if not is_number or not math.isfinite(status_effect_id) or (
            status_effect_id != int(status_effect_id)
        ):
            errors.append(f"effect {effect_id}: 'statusEffectId' must be an integral number")
        elif str(int(status_effect_id)) not in status_effects:
            errors.append(
                f"effect {effect_id}: statusEffectId {int(status_effect_id)} "
                "is not an exported StatusEffect row")
    return errors


def validate_immunity_references(skills, effects, immunities, shields):
    """Every stage immune window, addShield effect and Shield immunity must resolve."""
    errors = []
    for skill in skills:
        for layer in skill["layers"]:
            for stage in layer["stages"]:
                for notify in stage["notifies"]:
                    if notify.get("notifyType") != "immune":
                        continue
                    immune_id = str(notify.get("immune", {}).get("immuneCmsId", ""))
                    if immune_id not in immunities:
                        errors.append(f"skill {skill['id']}: immune window names missing "
                                      f"Immune row {immune_id!r}")
    for effect_id, effect in effects.items():
        if effect["kind"] != "addShield":
            continue
        shield_id = effect["parameters"].get("shieldId")
        if not isinstance(shield_id, (int, float)) or str(int(shield_id)) not in shields:
            errors.append(f"effect {effect_id}: shieldId {shield_id!r} is not a Shield row")
    for shield_id, shield in shields.items():
        if shield["immune_id"] and shield["immune_id"] not in immunities:
            errors.append(f"shield {shield_id}: FK_immuneId {shield['immune_id']} is missing")
    return errors


def validate_profile_limits(profile):
    required_keys = ["format_version", "skills", "effects", "projectiles",
                     "npc_stats", "npc_skills", "weapon_items", "recoveries"]
    for key in required_keys:
        if key not in profile:
            raise ValueError(f"Combat profile missing required key: {key}")
    if profile.get("format_version") != 1:
        raise ValueError("Combat profile format_version must be 1")
    if len(profile.get("skills", [])) > MAX_COMBAT_SKILLS:
        raise ValueError(f"Combat profile exceeds skills limit ({MAX_COMBAT_SKILLS})")
    if len(profile.get("effects", {})) > 8192:
        raise ValueError("Combat profile exceeds effects limit (8192)")
    if len(profile.get("projectiles", {})) > 2048:
        raise ValueError("Combat profile exceeds projectiles limit (2048)")
    if len(profile.get("npc_stats", {})) > 8192:
        raise ValueError("Combat profile exceeds npc_stats limit (8192)")


def action_layers(source, skill):
    layers = []
    for action_id in skill["actionLayerIds"].split(","):
        if not action_id:
            continue
        path = Path("raw/X5/Content/Data/BuiltData/Action") / (action_id + ".json")
        if not (source.root / path).exists():
            continue
        layers.extend(json.loads(source.read(path))["actionLayers"])
    # Some exported actions already contain the complete directional layer set.
    if not layers:
        path = Path("raw/X5/Content/Data/BuiltData/Action") / (skill["id"] + ".json")
        if (source.root / path).exists():
            layers = json.loads(source.read(path))["actionLayers"]
    return layers


def effect_definition(row, parameters):
    definition = {
        "kind": row["jsonCase"], "parameters": parameters,
        "targeting_kind": row["targetingCase"], "targeting": fragment(row["targeting"]),
        "chain": int(row["chainSkillEffect"] or 0),
    }
    # SkillEffect.overlapGroupId: a damage effect with one lands once per target per cast.
    # overlapGroupPriority is ignored (Candidate).
    if row["jsonCase"] == "damage" and row.get("overlapGroupId"):
        definition["overlap_group"] = row["overlapGroupId"]
    # SkillEffect.FK_statusEffectId: a status the effect applies to every target it reaches (for
    # example the armor-reduction debuff of a balm's interval hit).
    status_text = row.get("FK_statusEffectId", "")
    if status_text.strip().isdigit() and int(status_text) > 0:
        definition["status_effect_id"] = int(status_text)
    # Where a spawned skill object is placed (EX5TargetingPosType): areas, and projectiles whose
    # flight ends at the client's aimed point (Dragon's Roar 11130 aims at a ground marker).
    if row["jsonCase"] in ("spawnAreaOfEffect", "spawnProjectile") and row.get("targetingPurpose"):
        definition["targeting_purpose"] = fragment(row["targetingPurpose"])
    # SkillEffect.duration: how long a changeWeaponSkillSlot swap lasts. Official
    # PS2C_ChangeWeaponSkillSlotNoti expire ticks equal the effect tick plus this value.
    if row["jsonCase"] == "changeWeaponSkillSlot":
        definition["duration_ms"] = catalog_integer(row.get("duration", ""), 0)
    # SkillEffect.FK_CooldownResetSkillIds: skills whose cooldown a skillCooldownReset clears
    # (the parry counter 1127 readies pursuit 1000526).
    if row["jsonCase"] == "skillCooldownReset":
        definition["cooldown_reset_skill_ids"] = catalog_id_list(
            row.get("FK_CooldownResetSkillIds", ""))
    return definition


def catalog_integer(text, default):
    """A catalog cell holding an optional integer; blank cells yield the default."""
    stripped = text.strip()
    if not stripped:
        return default
    return int(float(stripped))


def catalog_id_list(text):
    """A comma-separated catalog id list such as "1000526" or "(1,2)"; blanks are skipped."""
    return [int(value) for value in text.strip("()[] ").split(",") if value.strip().isdigit()]


def immunity_definitions(immune_rows):
    """Every Immune row as its EX5ImmuneType names, keyed by row id.

    Stage immune notifies (immune.immuneCmsId) and Shield.FK_immuneId name these rows; the
    runtime reads Dodge, Damage and the reaction categories (Sway, Knockback, ...).
    """
    return {
        row["id"]: [name.strip() for name in row["immunes"].split(",") if name.strip()]
        for row in immune_rows
    }


def shield_definitions(shield_rows):
    """Shield rows named by addShield effects: lifetime, absorb amount and reaction immunity.

    The absorbed amount is shieldAmount plus per-mille shares of the holder's maximum and
    current health. Official grants of Shield 3006 (100 per mille of maximum health) match a
    warrior's 440 maximum health with a 44-point shield.
    """
    shields = {}
    for row in shield_rows:
        shields[row["id"]] = {
            "duration_ms": catalog_integer(row["duration"], 0),
            "amount": catalog_integer(row["shieldAmount"], 0),
            "hp_per_remain_milli": catalog_integer(row["shieldHpPerRemainMilli"], 0),
            "hp_per_total_milli": catalog_integer(row["shieldHpPerTotalMilli"], 0),
            "immune_id": row["FK_immuneId"].strip(),
        }
    return shields


def uses_captured_events(skill, capture_events):
    """Whether captured client reports replace the catalog stage events of a skill.

    Item skills always keep the catalog events: the client reports an item's consumption
    effect before the server's StartSkillNoti, so the capture normalizer drops it, and the
    consumption event must never be lost from the server-timed action.
    """
    return bool(capture_events) and skill["skillType"] != "Item"


def stage_event_definitions(skill, stage, capture_events):
    """Keep every unconditional authored event; use observed timing only for a matching event.

    A capture is a sample of executed actions, not a complete action definition. It must not
    delete uncaptured hits/spawns or inject an event into a different stage. Conditional perk
    activations stay excluded until the runtime evaluates those conditions.
    """
    observed = {}
    if uses_captured_events(skill, capture_events):
        observed = {
            (event["unique_id"], event["effect_id"]): event
            for event in capture_events
        }

    definitions = []
    for event in stage["skillEffects"]:
        activation = event.get("notifyActivation", {})
        if activation.get("notifyActivations"):
            continue
        key = (event["skillEffectUniqueId"], event["skillEffectId"])
        sample = observed.get(key)
        execute_time = sample["time_ms"] if sample else event["executeTime"]
        definitions.append(with_event_shape({
            "effect_id": event["skillEffectId"],
            "unique_id": event["skillEffectUniqueId"],
            "time_ms": execute_time,
        }, event))
    return definitions


def skill_consumes_item(skill_definition, effect_definitions):
    """Whether any stage event of an exported skill executes a useItem effect."""
    for layer in skill_definition["layers"]:
        for stage in layer["stages"]:
            for event in stage["events"]:
                effect = effect_definitions.get(str(event["effect_id"]))
                if effect is not None and effect["kind"] == "useItem":
                    return True
    return False


def item_skill_map(item_rows, exported_skills, effect_definitions):
    """Map each item CMS id to the exported Item-type skill its StartSkillReq must name.

    Items whose skill has no extracted action, or whose action never spends the item (the
    aiming, trap and totem skills of this build), are left out, so the server refuses them
    instead of granting an endlessly reusable animation.
    """
    mapping = {}
    for item in item_rows:
        skill_text = item["FK_skillId"]
        if not skill_text:
            continue
        skill = exported_skills.get(int(skill_text))
        if skill is None or skill["type"] != "Item":
            continue
        if not skill_consumes_item(skill, effect_definitions):
            continue
        mapping[item["id"]] = int(skill_text)
    return mapping


def status_stat_modifiers(maintain_effect_ids, maintain_effects, maintain_stats):
    """Resolve StatusEffect.FK_maintainEffects into catalog stat modifiers.

    Only MaintainEffect rows of type Stat carry stat values; their value is a MaintainStat
    row id. `add` is the flat catalog statAdd and `mul` the catalog statMul, both verbatim
    integers; the runtime decides which of them it applies.
    """
    modifiers = []
    for effect_id in maintain_effect_ids:
        effect = maintain_effects.get(str(effect_id))
        if effect is None or effect["maintainEffectType"] != "Stat":
            continue
        stat = maintain_stats.get(effect["maintainEffectValue"])
        if stat is None:
            raise ValueError(f"MaintainEffect {effect_id} names a missing MaintainStat row")
        modifiers.append({
            "stat": stat["statType"],
            "add": int(stat["statAdd"] or 0),
            "mul": int(stat["statMul"] or 0),
        })
    return modifiers


def recovery_definition(row):
    """One StatRecovery row: an HP or SP gain of `amount` every `interval_ms`."""
    return {
        "stat": 69 if row["type"] == "HP" else 72,
        "interval_ms": int(row["tick"] or 100),
        "amount": float(row["perTick"] or 0),
        "duration_ms": int(row["duration"] or 0),
        "use_potion_heal_rate": row["usePotionHealRate"] == "True",
        "cancel_type": row["cancelType"] or "None",
    }


# SkillObject collision sizes are catalog centimeters; the runtime works in millimeters.
CENTIMETERS_TO_MILLIMETERS = 10


def area_spawn_effect_id(row):
    """SkillObject.aoeSpawnSkillEffectId: the effect an area applies once when it appears.

    Official captures show it landing at the area's created tick with the area as caster (for
    example 160505, the lightning-burst damage of area 160402). Zero when the row has none.
    """
    text = row.get("aoeSpawnSkillEffectId", "") or ""
    return int(text) if text.strip().isdigit() else 0


def area_definition(row, conditional_effects=None):
    """One AOEIntervalHit SkillObject row: an area that hits every interval for its lifetime.

    Returns None for other skill-object kinds (projectiles, traps) and for areas without a
    usable interval effect, which have no area runtime. A conditional interval effect uses its
    ConditionalSkillEffect default branch; the conditions themselves are not evaluated
    (Candidate). An authored spawn effect is exported as spawn_effect_id.
    """
    if row["jsonCase"] != "AOEIntervalHit":
        return None
    behavior = fragment(row["json"])
    collision = fragment(row["collision"])
    interval_effect_id = int(behavior.get("intervalHitSkillEffectId", 0))
    conditional_id = int(behavior.get("intervalHitConditionalSkillEffectId", 0))
    if interval_effect_id <= 0 and conditional_id > 0 and conditional_effects:
        conditional = conditional_effects.get(str(conditional_id), {})
        default_text = conditional.get("FK_defaultSkillEffectId", "")
        interval_effect_id = int(default_text) if default_text.strip().isdigit() else 0
    interval_ms = int(behavior.get("intervalMiliSec", 0))
    duration_ms = int(row["duration"] or 0)
    if interval_effect_id <= 0 or interval_ms <= 0 or duration_ms <= 0:
        return None
    shape = row["collisionCase"]
    if shape not in ("cylinder", "box"):
        return None

    if shape == "box":
        area = {
            "duration_ms": duration_ms,
            "interval_ms": interval_ms,
            "interval_effect_id": interval_effect_id,
            "shape": "box",
            "extent_x_mm": round(float(collision.get("extentX", 0)) * CENTIMETERS_TO_MILLIMETERS),
            "extent_y_mm": round(float(collision.get("extentY", 0)) * CENTIMETERS_TO_MILLIMETERS),
            "radius_mm": 0,
            "half_height_mm": round(float(collision.get("extentZ", 0)) * CENTIMETERS_TO_MILLIMETERS),
            "max_targets": int(row["maxTargetCount"] or 0),
        }
    else:
        area = {
            "duration_ms": duration_ms,
            "interval_ms": interval_ms,
            "interval_effect_id": interval_effect_id,
            "radius_mm": round(float(collision.get("radius", 0)) * CENTIMETERS_TO_MILLIMETERS),
            "half_height_mm": round(float(collision.get("halfHeight", 0))
                                    * CENTIMETERS_TO_MILLIMETERS),
            "max_targets": int(row["maxTargetCount"] or 0),
        }

    # Only areas with an authored spawn hit carry the key, so other area rows stay unchanged.
    spawn_effect_id = area_spawn_effect_id(row)
    if spawn_effect_id > 0:
        area["spawn_effect_id"] = spawn_effect_id
    # Likewise for the hit an area applies when its lifetime ends (the lightning storm's bolts).
    despawn_effect_id = area_lifetime_despawn_effect_id(row)
    if despawn_effect_id > 0:
        area["despawn_effect_id"] = despawn_effect_id
    return area


def area_lifetime_despawn_effect_id(row):
    """SkillObject.FK_lifeTimeDespawnSkillEffect: the effect an area applies when its lifetime ends.

    Official captures of the lightning storm (area 160501, effect 160212) show every hit 1200 ms
    (the area duration) after the created tick, with the area as caster and ahead of its
    LifeTime DespawnSkillObjectNoti. Zero when the row has none.
    """
    text = row.get("FK_lifeTimeDespawnSkillEffect", "") or ""
    return int(text) if text.strip().isdigit() else 0


def skill_effect_conditions(condition_row):
    """SkillEffectCondition: up to five conditions that must all hold, as authored.

    Each becomes {"kind": conditionNCase, ...its JSON fields}; the runtime evaluates the kinds it
    knows (maxHPRatio, statusEffectStack) and treats any other kind as not satisfied.
    """
    conditions = []
    for index in range(1, 6):
        kind = (condition_row.get(f"condition{index}Case") or "").strip()
        if not kind:
            continue
        body = condition_row.get(f"condition{index}") or ""
        conditions.append({"kind": kind, **(fragment(body) if body else {})})
    return conditions


def conditional_effect_definition(conditional_row, condition_rows):
    """ConditionalSkillEffect: the first branch whose conditions hold, else the default effect.

    Branches keep catalog order (FK_conditionId1 first). The Elemental Staff stitching burn
    (3001) lists its strongest hit (target at or below 50% health and burning) first.
    """
    default_text = (conditional_row.get("FK_defaultSkillEffectId") or "").strip()
    branches = []
    for index in range(1, 6):
        condition_id = (conditional_row.get(f"FK_conditionId{index}") or "").strip()
        effect_text = (conditional_row.get(f"FK_skillEffectId{index}") or "").strip()
        if not condition_id or not effect_text.isdigit():
            continue
        condition_row = condition_rows.get(condition_id)
        if condition_row is None:
            raise ValueError(f"ConditionalSkillEffect {conditional_row['id']} names missing "
                             f"SkillEffectCondition {condition_id}")
        branches.append({"effect_id": int(effect_text),
                         "conditions": skill_effect_conditions(condition_row)})
    return {
        "default_effect_id": int(default_text) if default_text.isdigit() else 0,
        "branches": branches,
    }


def projectile_conditional_hit(row, conditional_effects, condition_rows):
    """SkillObject.FK_projectileHitConditionalSkillEffect, or None when the row has none.

    The stitching burn projectile 160560 carries no plain hit effect: its damage is this
    per-target choice, so without it the flight struck nothing.
    """
    text = (row.get("FK_projectileHitConditionalSkillEffect") or "").strip()
    if not text.isdigit():
        return None
    conditional = conditional_effects.get(text)
    if conditional is None:
        raise ValueError(f"SkillObject {row['id']} names missing ConditionalSkillEffect {text}")
    return conditional_effect_definition(conditional, condition_rows)


def conditional_effect_ids(definition):
    """Every effect a conditional definition can apply."""
    ids = [branch["effect_id"] for branch in definition["branches"]]
    if definition["default_effect_id"] > 0:
        ids.append(definition["default_effect_id"])
    return ids


def projectile_despawn_effects(row):
    """Effects a projectile SkillObject applies when its flight ends on a hit (for example the
    poison bottle's lingering cloud)."""
    effect_id = row.get("FK_hitTargetDespawnSkillEffect", "")
    return [int(effect_id)] if effect_id.strip().isdigit() and int(effect_id) > 0 else []


def projectile_lifetime_despawn_effects(row):
    """SkillObject.FK_lifeTimeDespawnSkillEffect: effects a flight applies where it ends without
    striking a unit, such as Dragon's Roar's fire area at its aimed point."""
    effect_id = row.get("FK_lifeTimeDespawnSkillEffect", "") or ""
    return [int(effect_id)] if effect_id.strip().isdigit() and int(effect_id) > 0 else []


def status_immunities(immune_id, immune_rows):
    """StatusEffect.immune names an Immune row; returns its EX5ImmuneType names."""
    if not immune_id:
        return []
    row = immune_rows.get(immune_id)
    if row is None:
        raise ValueError(f"StatusEffect immune {immune_id} names no Immune row")
    return [name.strip() for name in row["immunes"].split(",") if name.strip()]


ABNORMAL_STATUS_TYPES = ("Burning", "Freezing", "ElectricShock", "Bleeding", "Poisoning", "Stun",
                         "Shrink")


def abnormal_gauge_rules(game_variables):
    """GameVariable gauge maximum, decay and resulting status per abnormal status type."""
    values = {row["name"]: row["value"] for row in game_variables}
    rules = {"reduce_tick_ms": int(values["AbnormalStatusGaugeReduceTick"]), "types": {}}
    for name in ABNORMAL_STATUS_TYPES:
        # Shrink is the one type whose status variable carries a "Gauge" infix.
        status_key = f"AbnormalStatus{name}StatusEffectId"
        if status_key not in values:
            status_key = f"AbnormalStatus{name}GaugeStatusEffectId"
        rules["types"][name] = {
            "maximum": int(values[f"AbnormalStatus{name}GaugeMax"]),
            "reduce_per_tick": int(values[f"AbnormalStatus{name}GaugeReducePerTick"]),
            "status_effect_id": int(values[status_key]),
        }
    return rules


def item_effect_lists(item_rows, effect_rows):
    """Item.FK_skillEffectList for consumables used without a skill (PC2S_ItemUseReq).

    Lists that register a summon belong to the companion runtime and are left out.
    """
    lists = {}
    for item in item_rows:
        text = item["FK_skillEffectList"].strip("[]")
        effect_ids = [int(value) for value in text.split(",") if value.strip().isdigit()]
        if not effect_ids:
            continue
        kinds = {effect_rows[effect_id]["jsonCase"] for effect_id in effect_ids
                 if effect_id in effect_rows}
        if "registerSummonNpc" in kinds:
            continue
        missing = [effect_id for effect_id in effect_ids if effect_id not in effect_rows]
        if missing:
            raise ValueError(f"Item {item['id']} names missing skill effects {missing}")
        lists[item["id"]] = effect_ids
    return lists


def build(source, observations_path=OBSERVATIONS):
    observations = {}
    if observations_path.exists():
        observations = json.loads(observations_path.read_text(encoding="utf-8"))
    skills = {int(row["id"]): row for row in source.table("Skill")}
    effects = {int(row["id"]): row for row in source.table("SkillEffect")}
    projectiles = {int(row["id"]): row for row in source.table("SkillObject")}
    weapons = {}
    for row in source.table("Character"):
        if row["class"] in CLASS_IDS:
            for index in range(1, 7):
                weapon = row[f"weaponType{index}"]
                if weapon:
                    weapons.setdefault(weapon, set()).add(CLASS_IDS[row["class"]])

    observed = collections.defaultdict(list)
    for event in observations.get("skill_events", []):
        observed[(event["skill_id"], event["layer"], event["stage"])].append(event)
    npc_skill_ids = {skill for values in observations.get("npc_skills", {}).values()
                     for skill in values}
    definitions = []
    required_effects = set()
    missing_actions = []
    for skill_id, skill in sorted(skills.items()):
        classes = sorted(weapons.get(skill["weaponType"], set())) if (
            skill["isPlayerSkill"] == "True"
        ) else []
        if skill["weaponType"] == "None" and skill["isPlayerSkill"] == "True":
            classes = sorted(CLASS_IDS.values())
        if not classes and skill_id not in npc_skill_ids:
            continue
        raw_layers = action_layers(source, skill)
        if not raw_layers:
            missing_actions.append(skill_id)
            continue
        layers = []
        seen_layers = set()
        for layer in raw_layers:
            if layer["actionLayerIndex"] in seen_layers:
                continue
            seen_layers.add(layer["actionLayerIndex"])
            stages = []
            for stage in layer["actionStages"]:
                event_definitions = []
                capture_events = observed.get((skill_id, layer["actionLayerIndex"],
                                               stage["actionStageIndex"]))
                event_definitions = stage_event_definitions(skill, stage, capture_events)
                required_effects.update(event["effect_id"] for event in event_definitions)
                duration = max(1, stage["endTime"],
                               max((e["time_ms"] + 1 for e in event_definitions), default=0))
                stage_definition = {
                    "index": stage["actionStageIndex"], "duration_ms": duration,
                    "next_stage": stage["defaultNextStageIndex"],
                    "events": event_definitions,
                    "notifies": [notify for notify in stage["notifies"]
                                 if notify["notifyType"] in (
                                     "combo", "cancelable", "move_cancelable", "immune",
                                     "input_condition", "action_stage_condition",
                                 )]}
                if skill_id in npc_skill_ids:
                    chase = stage_adapted_chase(source, stage.get("animId"))
                    if chase:
                        stage_definition["root_motion"] = chase
                else:
                    motion = stage_player_motion(source, stage.get("animId"))
                    if motion:
                        stage_definition["player_motion"] = motion
                    rotations = [notify for notify in stage["notifies"]
                                 if notify["notifyType"] == "adapted_rotation"]
                    if rotations:
                        stage_definition["adapted_rotations"] = rotations
                stages.append(stage_definition)
            layers.append({"index": layer["actionLayerIndex"], "stages": stages})
        costs = {}
        for entry in filter(None, skill["costs"].split(",")):
            stat, amount = entry.split(":")
            costs[stat] = float(amount)
        definition = {
            "id": skill_id, "classes": classes, "npc": skill_id in npc_skill_ids,
            "weapon": skill["weaponType"], "type": skill["skillType"], "costs": costs,
            "cooldown_ms": int(skill["cooldown"] or 0),
            "cooldown_group": int(skill["cooldownGroup"] or 0),
            "range_mm": round(float(skill["maxDistance"] or 0) * 10),
            "min_range_mm": npc_skill_min_range_mm(
                skill, round(float(skill["maxDistance"] or 0) * 10)),
            "angle": float(skill["angleRange"] or 360),
            "require_target": skill["requireMainTarget"] == "True",
            "layers": layers,
        }
        if skill_id in npc_skill_ids:
            definition.update(npc_skill_selection_fields(skill, definition["range_mm"]))
        definitions.append(definition)

    status_effects = {int(row["id"]): row for row in source.table("StatusEffect")}
    maintain_effects = {row["id"]: row for row in source.table("MaintainEffect")}
    maintain_stats = {row["id"]: row for row in source.table("MaintainStat")}
    immune_rows = {row["id"]: row for row in source.table("Immune")}
    status_effect_definitions = {}
    for se_id, se_row in sorted(status_effects.items()):
        maintain_effect_ids = [int(v) for v in se_row["FK_maintainEffects"].strip("()").split(",")
                               if v.strip().isdigit()]
        status_effect_definitions[str(se_id)] = {
            "ui_type": se_row["ui_type"],
            "duration_ms": int(se_row["duration"] or 0),
            "stack_rule": se_row["stackRule"],
            "tick_ms": int(se_row["tick"] or 0),
            "tick_effect_id": int(se_row["tickSkillEffectId"] or 0),
            "add_effect_id": int(se_row["addSkillEffectId"] or 0),
            "remove_effect_ids": [
                int(se_row[f"removeSkillEffectId{i}"] or 0)
                for i in range(1, 4)
                if int(se_row[f"removeSkillEffectId{i}"] or 0) > 0
            ],
            "json_case": se_row["jsonCase"],
            "json": fragment(se_row["json"]) if se_row["json"] else {},
            "maintain_effect_ids": maintain_effect_ids,
            "stat_modifiers": status_stat_modifiers(maintain_effect_ids, maintain_effects,
                                                    maintain_stats),
            "stack_rule_value": int(se_row["stackRuleValue1"] or 0),
            "immune_types": status_immunities(se_row["immune"], immune_rows),
        }

    # Extract all projectile definitions from catalog (not just referenced ones)
    all_projectile_definitions = {}
    conditional_effect_rows = {row["id"]: row for row in source.table("ConditionalSkillEffect")}
    skill_effect_condition_rows = {row["id"]: row for row in source.table("SkillEffectCondition")}
    for proj_id, proj in sorted(projectiles.items()):
        hits = [int(value) for value in proj["projectileHitSkillEffect"].split(",")
                if value.strip().isdigit()]
        all_projectile_definitions[str(proj_id)] = {
            "parameters": fragment(proj["json"]) if proj["json"] else {},
            "duration_ms": int(proj["duration"] or 1000),
            "target_type": proj["targetType"],
            "max_target_count": int(proj["maxTargetCount"] or 0),
            "collision_case": proj["collisionCase"],
            "collision": fragment(proj["collision"]) if proj["collision"] else {},
            "hit_effects": hits,
            "despawn_effects": projectile_despawn_effects(proj),
            "lifetime_despawn_effects": projectile_lifetime_despawn_effects(proj),
            "json_case": proj["jsonCase"],
            "json": fragment(proj["json"]) if proj["json"] else {},
        }
        conditional_hit = projectile_conditional_hit(proj, conditional_effect_rows,
                                                     skill_effect_condition_rows)
        if conditional_hit:
            all_projectile_definitions[str(proj_id)]["conditional_hit"] = conditional_hit
    item_effects = item_effect_lists(source.table("Item"), effects)
    for effect_ids in item_effects.values():
        required_effects.update(effect_ids)
    # Air-bubble doodads apply these authored stamina recovery effects to their user.
    required_effects.update((11150, 11151, 11152))
    conditional_effects = conditional_effect_rows
    area_definitions = {}
    projectile_definitions = {}
    effect_definitions = {}
    while required_effects:
        effect_id = required_effects.pop()
        if str(effect_id) in effect_definitions or effect_id not in effects:
            continue
        row = effects[effect_id]
        parameters = fragment(row["json"])
        effect_definitions[str(effect_id)] = effect_definition(row, parameters)
        if row["chainSkillEffect"]:
            required_effects.add(int(row["chainSkillEffect"]))
        if row["jsonCase"] == "spawnAreaOfEffect":
            # SpawnAreaOfEffect.areaOfEffectId names a SkillObject row directly; the separate
            # AreaOfEffectSpawner table holds only one unrelated level-trap spawner.
            area_id = int(parameters["areaOfEffectId"])
            area_row = projectiles.get(area_id)
            area = area_definition(area_row, conditional_effects) if area_row else None
            if area:
                area_definitions[str(area_id)] = area
                required_effects.add(area["interval_effect_id"])
                if "spawn_effect_id" in area:
                    required_effects.add(area["spawn_effect_id"])
                if "despawn_effect_id" in area:
                    required_effects.add(area["despawn_effect_id"])
        if row["jsonCase"] == "spawnProjectile":
            projectile_id = int(parameters["projectileId"])
            projectile = projectiles.get(projectile_id)
            if projectile:
                hits = [int(value) for value in projectile["projectileHitSkillEffect"].split(",")
                        if value]
                required_effects.update(hits)
                required_effects.update(projectile_despawn_effects(projectile))
                required_effects.update(projectile_lifetime_despawn_effects(projectile))
                conditional_hit = all_projectile_definitions[str(projectile_id)].get(
                    "conditional_hit")
                if conditional_hit:
                    required_effects.update(conditional_effect_ids(conditional_hit))
                projectile_definitions[str(projectile_id)] = {
                    "duration_ms": int(projectile["duration"] or 1000),
                    "parameters": fragment(projectile["json"]), "hit_effects": hits,
                }
    # Merge referenced projectiles into the full catalog set
    for pid, pdata in projectile_definitions.items():
        if pid not in all_projectile_definitions:
            all_projectile_definitions[pid] = pdata

    # Extract NPC AI parameters for observed NPCs
    npc_ai = {}
    npc_table = {row["id"]: row for row in source.table("Npc")}
    npc_template_table = {row["id"]: row for row in source.table("NpcTemplate")}
    for cms_id in observations.get("npc_skills", {}):
        npc_row = npc_table.get(str(cms_id))
        if not npc_row:
            continue
        template = npc_template_table.get(npc_row.get("FK_template", ""))
        npc_ai[str(cms_id)] = {
            "search_circle_range": int(npc_row.get("searchCircleRange") or 0),
            "search_cone_range": int(npc_row.get("searchConeRange") or 0),
            "search_half_cone_angle": int(npc_row.get("searchHalfConeAngle") or 0),
            "search_alert_range": int(npc_row.get("searchAlertRange") or 0),
            "hearing_range": int(npc_row.get("hearingRange") or 0),
            "return_start_dist": int(npc_row.get("returnStartDist") or 0),
            "return_end_time_ms": int(npc_row.get("returnEndTime") or 0),
            "is_roaming": npc_row.get("isRoaming") == "True",
            "combat_group_type": npc_row.get("combatGroupType", ""),
            "behavior_type": npc_row.get("behaviorType", ""),
            "corpse_remain_ms": int(npc_row.get("corpseRemainMillis") or 0),
            "template_id": npc_row.get("FK_template", ""),
            "combat_suitable_dist_min": int(template.get("combatSuitableDistMin") or 0) if template else 0,
            "combat_suitable_dist_max": int(template.get("combatSuitableDistMax") or 0) if template else 0,
            "combat_option": template.get("combatOption", "") if template else "",
            "ai_id": template.get("ai", "") if template else "",
            "grade": template.get("grade", "") if template else "",
        }

    # GM-created private encounters reuse the same complete locomotion definition as
    # authored tutorial placements. Keep unsupported catalog rows explicit, not stationary
    # attackers with a made-up speed or engagement radius.
    npc_locomotion = {}
    npc_locomotion_unavailable = {}
    for cms_id in observations.get("npc_skills", {}):
        npc_row = npc_table.get(str(cms_id))
        if not npc_row or npc_row.get("behaviorType") != "Monster":
            continue
        try:
            npc_locomotion[str(cms_id)] = npc_ai_definition(source, npc_row)
        except ValueError as error:
            npc_locomotion_unavailable[str(cms_id)] = str(error)

    capture_hashes = observations.get("capture_hashes", {})
    label = observations.get("capture_path", "single")
    if "decoded_sha256" in observations:
        capture_hashes = {**capture_hashes, f"{label}:decoded": observations["decoded_sha256"]}
    if observations.get("native_session_sha256"):
        capture_hashes = {**capture_hashes, f"{label}:native_session": observations["native_session_sha256"]}

    # Validate that every skill-referenced projectile has loader-required fields
    referenced_projectiles = set()
    for eff in effect_definitions.values():
        if eff.get("kind") == "spawnProjectile":
            pid = eff.get("parameters", {}).get("projectileId")
            if pid is not None:
                referenced_projectiles.add(int(pid))
    validation_errors = validate_projectile_fields(all_projectile_definitions, referenced_projectiles)
    if validation_errors:
        raise ValueError("Projectile validation failed: " + "; ".join(validation_errors))
    status_errors = validate_status_effect_references(effect_definitions, status_effect_definitions)
    if status_errors:
        raise ValueError("Status effect validation failed: " + "; ".join(status_errors))
    # Validate profile against CombatData constructor limits
    # (src/world/features/combat/combat.cpp ~128-134)
    npc_collision = npc_collision_definitions(source, observations.get("npc_skills", {}))
    player_collision = player_collision_definitions(source)
    validate_profile_limits({
        "format_version": 1, "skills": definitions, "effects": effect_definitions,
        "projectiles": all_projectile_definitions,
        "npc_stats": observations.get("npc_stats", {}),
        "npc_skills": observations.get("npc_skills", {}),
        "weapon_items": {}, "recoveries": {},
    })
    immunities = immunity_definitions(immune_rows.values())
    shields = shield_definitions(source.table("Shield"))
    reference_errors = validate_immunity_references(definitions, effect_definitions, immunities,
                                                    shields)
    if reference_errors:
        raise ValueError("Immunity/shield validation failed: " + "; ".join(reference_errors))
    return {
        "format_version": 1, "skills": definitions, "effects": effect_definitions,
        "projectiles": all_projectile_definitions,
        "status_effects": status_effect_definitions,
        "immunities": immunities,
        "shields": shields,
        "npc_stats": observations.get("npc_stats", {}),
        "npc_skills": observations.get("npc_skills", {}),
        "npc_ai": npc_ai,
        "npc_locomotion": npc_locomotion,
        "npc_locomotion_unavailable": npc_locomotion_unavailable,
        "npc_collision": npc_collision,
        "player_collision": player_collision,
        "weapon_items": {row["id"]: row["equipmentType"] for row in source.table("Item")
                         if row["equipCategory"] == "Weapon"},
        "item_skills": item_skill_map(source.table("Item"),
                                      {definition["id"]: definition for definition in definitions},
                                      effect_definitions),
        "recoveries": {row["id"]: recovery_definition(row)
                       for row in source.table("StatRecovery") if row["type"] in ("HP", "SP")},
        "areas": area_definitions,
        "abnormal_gauges": abnormal_gauge_rules(source.table("GameVariable")),
        "item_effect_lists": item_effects,
        "missing_actions": missing_actions,
        "capture_hashes": capture_hashes,
        "observations_sha256": hashlib.sha256(observations_path.read_bytes()).hexdigest()
        if observations_path.exists() else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--captures", type=Path, nargs="*")
    parser.add_argument("--open-world", action="store_true", default=False)
    parser.add_argument("--union-npc-skills", type=Path, nargs="+", metavar="CAPTURE",
                        help="add the NPC skills of these captures to the committed observations")
    parser.add_argument("--output", type=Path, default=Path("data/generated/tutorial/combat.json"))
    parser.add_argument(
        "--update-tutorial-profile", type=Path,
        help="replace only the combat block of an existing same-build tutorial profile",
    )
    args = parser.parse_args()
    if args.captures:
        observations = merge_observations([
            collect_observations(cap, open_world=args.open_world)
            for cap in args.captures
        ])
        OBSERVATIONS.write_text(json.dumps(observations, indent=2) + "\n", encoding="utf-8")
    elif args.union_npc_skills:
        baseline = json.loads(OBSERVATIONS.read_text(encoding="utf-8"))
        extractions = [collect_observations(cap, open_world=args.open_world)
                       for cap in args.union_npc_skills]
        observations, added = union_npc_skills(baseline, extractions)
        OBSERVATIONS.write_text(json.dumps(observations, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"npc_skills_added": added}))
    elif args.capture:
        observations = collect_observations(args.capture, open_world=args.open_world)
        OBSERVATIONS.write_text(json.dumps(observations, indent=2) + "\n", encoding="utf-8")
    from build_tutorial_data import BUILD_ID, Source, write_profile
    profile = build(Source(args.source))
    write_profile(args.output, profile)
    if args.update_tutorial_profile:
        document = json.loads(args.update_tutorial_profile.read_text(encoding="utf-8"))
        if document.get("format_version") != 1 or document.get("build_id") != BUILD_ID:
            raise ValueError("Cannot replace combat in an unknown tutorial build")
        document["combat"] = profile
        write_profile(args.update_tutorial_profile, document)
    print(json.dumps({"skills": len(profile["skills"]), "effects": len(profile["effects"]),
                      "npc_stats": len(profile["npc_stats"]),
                      "npc_skills": len(profile["npc_skills"]),
                      "projectiles": len(profile["projectiles"]),
                      "status_effects": len(profile["status_effects"]),
                      "missing_actions": len(profile["missing_actions"])}))

if __name__ == "__main__":
    main()
