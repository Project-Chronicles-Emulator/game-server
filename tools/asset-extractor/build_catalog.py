"""Build lossless CSV catalogs and explicit NPC/spawner/protocol joins."""

import argparse
import collections
import csv
import hashlib
import json
import re
from pathlib import Path
from google.protobuf import descriptor_pb2, json_format


def csv_rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        rows = list(reader)
        if None in reader.fieldnames or any(None in row for row in rows):
            raise ValueError("CSV has extra fields: " + str(path))
        return rows


def jsonl(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        for record in records:
            stream.write(
                json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n"
            )


def index(rows, key="id"):
    result = {}
    for row in rows:
        if row[key] in result:
            raise ValueError("Duplicate ID: " + row[key])
        result[row[key]] = row
    return result


def selector_ids(value):
    # Preserve the suffix as authored data; its probability semantics are unverified.
    result = []
    for token in value.split(","):
        if not token:
            continue
        match = re.fullmatch(r"\s*(\d+)(?::([^,]+))?\s*", token)
        if not match:
            raise ValueError("Unknown selector format: " + value)
        result.append({"id": match[1], "selector_value": match[2]})
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    root = args.capture
    content = root / "raw/X5/Content"
    cms = content / "Design/CMS"
    catalog = root / "Catalog"
    catalog.mkdir(exist_ok=True)
    summary = {
        "tables": {},
        "limits": [
            "Static demo client records include test, mannequin and unused definitions.",
            "Spawner points and areas are authored candidates, not a snapshot of live NPCs.",
            "Syntax normalization establishes metadata validity; observed packet compatibility is recorded separately in LivePackets/Decoded/decode-summary.json.",
            "Packet.csv contains logging flags, not packet opcodes.",
        ],
    }
    tables = {}
    for path in sorted(cms.glob("*.csv")):
        rows = csv_rows(path)
        tables[path.stem] = rows
        summary["tables"][path.stem] = {
            "rows": len(rows),
            "source": str(path.relative_to(root)),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        }
        jsonl(catalog / "Tables" / (path.stem + ".jsonl"), rows)
    print("Converted CMS tables:", len(tables), flush=True)

    locales = {}
    for path in sorted(cms.glob("*/L10N.csv")):
        locales[path.parent.name] = {
            row["key"]: row["sourceString"] for row in csv_rows(path)
        }
    npcs = index(tables["Npc"])
    templates = index(tables["NpcTemplate"])
    models = index(tables["UnitModel"])
    groups = collections.defaultdict(list)
    for member in tables["NpcGroupMember"]:
        groups[member["FK_npcGroup"]].append(member)
    npc_records = []
    missing = collections.Counter()
    for npc in npcs.values():
        key = f"TXT_NPC_{int(npc['id']):010d}_NAME"
        record = {
            "id": int(npc["id"]),
            "name": npc["name"],
            "level": int(npc["level"]) if npc["level"] else None,
            "behavior_type": npc["behaviorType"],
            "localization_key": key,
            "localized_names": {
                language: values[key]
                for language, values in locales.items()
                if key in values
            },
            "source": npc,
            "template": templates.get(npc["FK_template"]),
            "model": models.get(npc["FK_modelId"]),
        }
        if npc["FK_template"] and record["template"] is None:
            missing["template"] += 1
        if npc["FK_modelId"] and record["model"] is None:
            missing["model"] += 1
        npc_records.append(record)
    jsonl(catalog / "npcs.jsonl", npc_records)
    jsonl(
        catalog / "mobs.jsonl",
        (row for row in npc_records if row["behavior_type"] == "Monster"),
    )
    jsonl(
        catalog / "civilians.jsonl",
        (row for row in npc_records if row["behavior_type"] == "Civilian"),
    )
    spawner_defs = collections.defaultdict(list)
    for row in tables["NpcSpawner"]:
        spawner_defs[row["id"]].append(row)
    spawner_records = []
    spawner_files = sorted((content / "Data/BuiltData/Maps").rglob("npc_spawner.json"))
    missing_spawners = collections.Counter()
    points = 0
    for path in spawner_files:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
        relative = path.relative_to(content / "Data/BuiltData/Maps")
        for sector in data.get("spawnerSectors", []):
            for spawner in sector["spawners"]:
                definitions = spawner_defs.get(str(spawner.get("cmsId", 0)), [])
                selected = [
                    dict(
                        ref,
                        map_layer=definition["mapLayer"],
                        weather_group=definition["weatherGroup"],
                    )
                    for definition in definitions
                    for ref in selector_ids(definition["npc"])
                ]
                group_refs = [
                    dict(
                        ref,
                        map_layer=definition["mapLayer"],
                        weather_group=definition["weatherGroup"],
                    )
                    for definition in definitions
                    for ref in selector_ids(definition["npcGroup"])
                ]
                candidate_ids = {x["id"] for x in selected}
                for group in group_refs:
                    candidate_ids.update(x["FK_npc"] for x in groups[group["id"]])
                for npc_id in candidate_ids:
                    if npc_id not in npcs:
                        missing_spawners["npc_reference"] += 1
                if not definitions:
                    missing_spawners["cms_id"] += 1
                points += len(spawner.get("pointList", {}).get("points", []))
                spawner_records.append(
                    {
                        "map": relative.parts[0],
                        "source_path": str(path.relative_to(root)),
                        "sector": sector.get("sectorIndex"),
                        "source": spawner,
                        "spawner_definitions": definitions,
                        "npc_selectors": selected,
                        "group_selectors": group_refs,
                        "npc_candidates": [
                            {
                                "id": int(n),
                                "name": npcs[n]["name"],
                                "behavior_type": npcs[n]["behaviorType"],
                            }
                            for n in sorted(candidate_ids, key=int)
                            if n in npcs
                        ],
                    }
                )
    jsonl(catalog / "npc-spawners.jsonl", spawner_records)

    desc = descriptor_pb2.FileDescriptorSet.FromString(
        (root / "Protocol/descriptors.pb").read_bytes()
    )
    packet_flags = index(tables["Packet"], "name")
    opcode_path = root / "Protocol/opcodes.json"
    opcode_rows = (
        json.loads(opcode_path.read_text())["entries"] if opcode_path.exists() else []
    )
    opcode_map = {row["packet"]: row["opcode"] for row in opcode_rows}
    packets = []
    for proto in desc.file:
        if not proto.name.startswith("p_"):
            continue
        direction = proto.name.split(".")[0].removeprefix("p_")
        for msg in proto.message_type:
            label = "P" + direction.upper() + "_" + msg.name
            packets.append(
                {
                    "name": proto.package + "." + msg.name,
                    "file": proto.name,
                    "family": direction,
                    "original_syntax": proto.syntax,
                    "direction_tagged": "2" in direction,
                    "opcode": opcode_map.get(label),
                    "logging_flags": packet_flags.get(label),
                    "fields": [
                        json_format.MessageToDict(f, preserving_proto_field_name=True)
                        for f in msg.field
                    ],
                }
            )
    jsonl(root / "Protocol/packet-catalog.jsonl", packets)
    summary.update(
        {
            "npc_records": len(npc_records),
            "behavior_types": dict(
                collections.Counter(n["behavior_type"] for n in npc_records)
            ),
            "locales": sorted(locales),
            "unresolved_npc_joins": dict(missing),
            "spawner_files": len(spawner_files),
            "placed_spawners": len(spawner_records),
            "authored_spawn_points": points,
            "spawner_ids_with_variants": sum(
                len(rows) > 1 for rows in spawner_defs.values()
            ),
            "unresolved_spawner_joins": dict(missing_spawners),
            "protocol_files": len(desc.file),
            "network_top_level_messages": len(packets),
            "packet_logging_flags": len(packet_flags),
            "direction_tagged_message_types": sum(
                p["direction_tagged"] for p in packets
            ),
            "recovered_opcode_mappings": len(opcode_rows),
            "network_messages_with_logging_flags": sum(
                p["logging_flags"] is not None for p in packets
            ),
        }
    )
    (catalog / "summary.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8"
    )
    print(
        json.dumps({k: v for k, v in summary.items() if k != "tables"}, indent=2),
        flush=True,
    )


if __name__ == "__main__":
    main()
