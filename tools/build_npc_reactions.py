"""Export server-owned civilian threat reactions from verified client data."""

import argparse
import json
from pathlib import Path

from build_tutorial_data import BUILD_ID, SOURCE_SHA256, Source, write_profile
from build_loot_data import game_variable


def build(source):
    models = {row["id"]: row for row in source.table("UnitModel")}
    groups = {row["id"]: row for row in source.table("UnitGroupModel")}
    variables = {row["type"]: row["value"] for row in source.table("NpcX5MotionVariable")}
    motion_key = variables["Threatened"]
    motions = {}
    for row in source.table("X5Motion"):
        if row["x5MotionKey"] == motion_key and row["animStanceKey"] == "Base":
            key = row["skeletalMeshServantKey"]
            if key in motions:
                raise ValueError(f"Ambiguous threatened motion for {key}")
            motions[key] = row["gameAnimKey"]
    mapping = [row["gameAnimKey"] for row in source.table("GameAnimKeyMapping")]
    if len(mapping) != len(set(mapping)):
        raise ValueError("Ambiguous game animation key mapping")

    definitions = []
    unavailable = []
    durations = {}
    for npc in source.table("Npc"):
        if (npc["behaviorType"] != "Civilian" or npc["isReactPlayerBehavior"] != "True"
                or not int(npc["clientBehaviorTree"] or 0)):
            continue
        model = models[npc["FK_modelId"]]
        group = groups[model["fk_UnitGroupModelId"]]
        animation = motions.get(group["skeletalMeshServantKey"])
        if animation is None:
            unavailable.append({"npc_cms_id": int(npc["id"]), "reason": "no_authored_motion"})
            continue
        if animation not in mapping:
            raise ValueError(f"Missing animation mapping: {animation}")
        if animation not in durations:
            relative = Path("raw/X5/Content/Data/BuiltData/Anim/X5Motion") / (animation + ".json")
            data = json.loads(source.read(relative))
            duration = data["endTime"]
            if isinstance(duration, bool) or not isinstance(duration, int) or not 0 < duration <= 60000:
                raise ValueError(f"Invalid threatened motion duration: {animation}")
            durations[animation] = duration
        definitions.append({
            "npc_cms_id": int(npc["id"]),
            "game_anim_mapping_id": mapping.index(animation),
            "duration_ms": durations[animation],
        })
    return {
        "format_version": 1,
        "build_id": BUILD_ID,
        "source_executable_dump_sha256": SOURCE_SHA256,
        "range_mm": game_variable(source, "ThreatImpactRange") * 10,
        "npcs": sorted(definitions, key=lambda row: row["npc_cms_id"]),
        "unavailable": unavailable,
        "sources": dict(source.sources),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/npc-reactions/definitions.json"))
    args = parser.parse_args()
    document = build(Source(args.source))
    write_profile(args.output, document)
    print(json.dumps({"npcs": len(document["npcs"]), "unavailable": len(document["unavailable"]),
                      "range_mm": document["range_mm"]}))


if __name__ == "__main__":
    main()
