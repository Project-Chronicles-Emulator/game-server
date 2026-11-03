"""Report population coverage and source gaps for every candidate Demo/Release map.

The report is a private data audit, not original-client gameplay acceptance. Empty
maps may be intentional or require scripted activation; this tool never invents spawns.
"""

import argparse
import json
from collections import Counter
from pathlib import Path

from build_stationary_npc_placements import all_population_map_ids, build_map_placements
from build_tutorial_data import Source, write_profile


def audit(source_root):
    source = Source(source_root)
    maps = {int(row["id"]): row for row in source.table("Map")}
    monster_document = json.loads(
        (source_root / "world-mobs/definitions.json").read_text(encoding="utf-8")
    )
    monsters = {row["map_id"]: row for row in monster_document["maps"]}
    static_document = json.loads(
        (source_root / "static-npcs/definitions.json").read_text(encoding="utf-8")
    )
    static_counts = Counter(row["map_id"] for row in static_document["targets"])
    results = []

    for map_id in all_population_map_ids(source):
        row = maps[map_id]
        expected = build_map_placements(
            source, map_id, require_layers=False, lenient=True
        )
        path = source_root / "npc/placements" / f"map-{map_id}.json"
        installed = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
        # The CLI adds a cumulative provenance map after building each population.
        # Compare authored/runtime content independently of that bookkeeping field.
        installed.pop("source_sha256", None)
        monster_map = monsters.get(map_id, {})
        civilian_count = len(installed.get("placements", []))
        monster_count = len(monster_map.get("spawns", []))
        expected_count = len((expected or {}).get("placements", []))
        results.append({
            "map_id": map_id,
            "type": row["type"],
            "package": row["package"],
            "zone_key": row.get("FK_zoneKey", ""),
            "parent_map_id": int(row.get("parentMapId") or 0),
            "civilian_count": civilian_count,
            "monster_count": monster_count,
            "static_interaction_targets": static_counts[map_id],
            "civilian_output_matches_source": (
                installed == expected if expected_count else civilian_count == 0
            ),
            "has_any_population_definition":
                civilian_count + monster_count + static_counts[map_id] > 0,
            "civilian_exclusions": (expected or {}).get("excluded", {}),
            "invalid_civilian_placements": (expected or {}).get("invalid_placements", []),
            "missing_civilian_layers": (expected or {}).get("layers_missing", []),
            "monster_exclusions": monster_map.get("excluded", {}),
            "missing_monster_layers": monster_map.get("missing_layers", []),
        })

    return {
        "format_version": 1,
        "scope": "Candidate Demo/Release maps; excludes private tutorials and developer worlds",
        "limits": "Counts and source agreement do not establish live spawns or gameplay acceptance",
        "summary": {
            "candidate_maps": len(results),
            "maps_with_civilians": sum(row["civilian_count"] > 0 for row in results),
            "maps_with_monsters": sum(row["monster_count"] > 0 for row in results),
            "maps_with_any_population_definition": sum(
                row["has_any_population_definition"] for row in results
            ),
            "civilian_output_mismatches": sum(
                not row["civilian_output_matches_source"] for row in results
            ),
        },
        "maps": results,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("out/world-population-audit.local.json"))
    options = parser.parse_args()
    document = audit(options.source)
    write_profile(options.output, document)
    print(json.dumps(document["summary"]))
    if document["summary"]["civilian_output_mismatches"]:
        raise SystemExit("Civilian populations differ from the verified source; regenerate them")


if __name__ == "__main__":
    main()
