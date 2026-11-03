"""Build and check the versioned feature reference from recovered protocol inputs."""

import argparse
import collections
import hashlib
import json
import re
from pathlib import Path

from google.protobuf import descriptor_pb2


ROOT = Path(__file__).resolve().parents[1]
COVERAGE_PATH = ROOT / "protocol/feature-coverage.json"
OBSERVATIONS_PATH = ROOT / "protocol/demo-capture-observations.json"
DOCUMENT_PATH = ROOT / "docs/protocol-feature-reference.md"
CATALOG_PATH = ROOT / "docs/protocol-packet-catalog.json"
PUBLICATION_PATH = ROOT / "docs/protocol-reference-publication.json"
PAGE_BLOCKS_PATH = ROOT / "out/protocol-reference/page-blocks.json"
STATUSES = {"implemented", "partial", "missing"}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reviewed_source_sha256(path):
    # Git checkouts may use CRLF on Windows and LF elsewhere. Source review
    # freshness follows text, rather than a checkout's line-ending policy.
    source = path.read_text(encoding="utf-8")
    return hashlib.sha256(source.encode("utf-8")).hexdigest()


def pending_source_reviews(coverage):
    return [
        path
        for path, digest in coverage["reviewed_sources"].items()
        if reviewed_source_sha256(ROOT / path) != digest
    ]


def validate_publication(publication, document_digest):
    if publication.get("document_sha256") != document_digest:
        raise ValueError("Page mirror is stale or unverified; synchronize and confirm readback")
    if not publication.get("page_id", "").startswith("page_"):
        raise ValueError("Publication record has no exact Page ID")


def schema_family(filename):
    families = [
        ("p_c2ae", "C2AE"), ("p_ae2c", "AE2C"),
        ("p_c2a", "WSC2A" if ".ws" in filename else "HTTP_C2A"),
        ("p_a2c", "WSA2C" if ".ws" in filename else "HTTP_A2C"),
        ("p_c2f", "WSC2F" if ".ws" in filename else "REST_C2F"),
        ("p_f2c", "WSF2C" if ".ws" in filename else "REST_F2C"),
        ("p_c2s", "PC2S"), ("p_s2c", "PS2C"),
    ]
    for prefix, family in families:
        if filename.startswith(prefix):
            return family
    if filename == "timesync_req.proto":
        return "PC2S"
    if filename == "timesync_res.proto":
        return "PS2C"
    return None


def field_description(field):
    if field.type_name:
        field_type = field.type_name.lstrip(".")
    else:
        field_type = descriptor_pb2.FieldDescriptorProto.Type.Name(field.type)[5:].lower()
    prefix = "repeated " if field.label == field.LABEL_REPEATED else ""
    return f"{field.number}:{field.name} ({prefix}{field_type})"


def load_schema_index():
    descriptors = descriptor_pb2.FileDescriptorSet.FromString(
        (ROOT / "protocol/descriptors.normalized.pb").read_bytes()
    )
    index = {}
    client_messages = []
    for file in descriptors.file:
        for message in file.message_type:
            full_name = f"{file.package}.{message.name}"
            entry = {
                "schema": full_name,
                "schema_file": file.name,
                "message_name": message.name,
                "fields": [field_description(field) for field in message.field],
            }
            index[full_name] = entry
            family = schema_family(file.name)
            if family:
                client_messages.append((family, entry))
    return index, client_messages


def classify(packet, schema_file, coverage):
    target = f"{packet} {schema_file}"
    for rule in coverage["classification_rules"]:
        if re.search(rule["pattern"], target, re.IGNORECASE):
            return rule["feature"]
    return "common"


def resolve_ws_schema(packet, index, client_messages):
    family, basename = packet.split("_", 1)
    candidates = [entry for candidate_family, entry in client_messages
                  if candidate_family == family and entry["message_name"] == basename]
    if len(candidates) == 1:
        return candidates[0]
    candidates = [entry for entry in index.values() if entry["message_name"] == basename]
    if len(candidates) == 1:
        return candidates[0]
    return None


def validate_capture_comparisons(observations):
    captures = {
        capture.get("key", f"C{index + 1}"): capture
        for index, capture in enumerate(observations["captures"])
    }
    if len(captures) != len(observations["captures"]):
        raise ValueError("Capture keys must be unique")

    for comparison in observations.get("comparisons", []):
        baseline_keys = comparison["baseline_keys"]
        new_key = comparison["new_capture_key"]
        if new_key in baseline_keys or len(set(baseline_keys)) != len(baseline_keys):
            raise ValueError("Comparison must use a distinct new capture and baseline")
        if any(key not in captures for key in [*baseline_keys, new_key]):
            raise ValueError("Comparison refers to an unknown capture")

        previous_counts = collections.Counter()
        for key in baseline_keys:
            previous_counts.update(captures[key]["packet_counts"])
        new_capture = captures[new_key]
        new_types = {
            name: count
            for name, count in new_capture["packet_counts"].items()
            if name not in previous_counts
        }
        if comparison["new_packet_type_counts"] != new_types:
            raise ValueError("Comparison new packet types disagree with capture counts")
        if comparison["new_records"] != new_capture["records"]:
            raise ValueError("Comparison new record count disagrees with capture")

        combined = previous_counts + collections.Counter(new_capture["packet_counts"])
        expected = {
            "records": sum(combined.values()),
            "distinct_packet_types": len(combined),
            "client_records": sum(value for name, value in combined.items()
                                  if name.startswith("PC2S_")),
            "server_records": sum(value for name, value in combined.items()
                                  if name.startswith("PS2C_")),
        }
        if any(comparison["combined"][key] != value for key, value in expected.items()):
            raise ValueError("Comparison combined totals disagree with capture counts")


def build_catalog(coverage, observations):
    index, client_messages = load_schema_index()
    packets = {}
    mapped_schemas = set()
    world = read_json(ROOT / "protocol/opcodes.json")
    websocket = read_json(ROOT / "protocol/native-ws-opcodes.json")

    def add_packet(packet, family, schema=None, opcode=None, mapping=None):
        if packet in packets:
            raise ValueError(f"Duplicate packet identity: {packet}")
        schema_file = schema["schema_file"] if schema else "unresolved"
        feature_id = classify(packet, schema_file, coverage)
        override = coverage["packet_overrides"].get(packet, {})
        state = override.get("status", "missing")
        if state not in STATUSES:
            raise ValueError(f"Invalid status for {packet}: {state}")
        custom_note = override.get("custom_note")
        if custom_note is not None:
            if not isinstance(custom_note, str) or not custom_note:
                raise ValueError(f"custom_note of {packet} must be a non-empty string")
            if state == "implemented":
                raise ValueError(f"Custom behavior cannot make {packet} implemented")
        record = {
            "packet": packet,
            "family": family,
            "feature": feature_id,
            "opcode": opcode,
            "mapping": override.get("route", mapping) or
                       "schema only; numeric ID/HTTP route not established",
            "schema": schema["schema"] if schema else None,
            "schema_file": schema_file,
            "fields": schema["fields"] if schema else [],
            "runtime_status": state,
            "runtime_note": override.get("note", "No current runtime path established."),
            "source_refs": override.get("source_refs", []),
            "capture_counts": [capture["packet_counts"].get(packet, 0)
                               for capture in observations["captures"]],
        }
        if custom_note is not None:
            # Kept apart from runtime_note: custom behavior is our own guess, not official.
            record["custom_note"] = custom_note
        packets[packet] = record
        if schema:
            mapped_schemas.add((family, schema["schema"]))

    for entry in world["entries"]:
        schema = index.get(entry["schema"])
        if schema is None:
            raise ValueError(f"World mapping has no descriptor: {entry['packet']}")
        add_packet(entry["packet"], entry["family"], schema, entry["opcode"],
                   "verified world wrapper ID")

    for entry in websocket["entries"]:
        packet = entry["packet"]
        add_packet(packet, packet.split("_", 1)[0],
                   resolve_ws_schema(packet, index, client_messages), entry["opcode"],
                   "verified native WebSocket wrapper ID; service-local namespace")

    for family, schema in client_messages:
        if (family, schema["schema"]) not in mapped_schemas:
            add_packet(f"{family}_{schema['message_name']}", family, schema)

    for name in world["unresolved_named_packets"]:
        if name not in packets:
            add_packet(name, name.split("_", 1)[0],
                       mapping="named wrapper; numeric ID and matching schema unresolved")

    for route in coverage["additional_routes"]:
        add_packet(route["packet"], route["family"], mapping=route["route"])

    unknown = set(coverage["packet_overrides"]) - set(packets)
    if unknown:
        raise ValueError(f"Status entries have no packet: {sorted(unknown)}")
    feature_ids = {feature["id"] for feature in coverage["features"]}
    if {packet["feature"] for packet in packets.values()} - feature_ids:
        raise ValueError("A classification rule refers to an unknown feature")
    for capture in observations["captures"]:
        if set(capture["packet_counts"]) - set(packets):
            raise ValueError("Captured packet names are missing from the catalog")
        if sum(capture["packet_counts"].values()) != capture["records"]:
            raise ValueError("Capture packet counts disagree with the declared record count")
    validate_capture_comparisons(observations)
    return sorted(packets.values(), key=lambda row: (row["feature"], row["family"],
                                                     row["opcode"] or 0, row["packet"]))


def validate_references(coverage):
    for feature in coverage["features"]:
        if feature["status"] not in STATUSES:
            raise ValueError(f"Invalid feature status: {feature['id']}")
        custom_behavior = feature.get("custom_behavior")
        if custom_behavior is not None:
            valid = (isinstance(custom_behavior, list) and custom_behavior
                     and all(isinstance(item, str) and item for item in custom_behavior))
            if not valid:
                raise ValueError(f"custom_behavior of {feature['id']} must list strings")
            if feature["status"] == "implemented":
                raise ValueError(f"Custom behavior cannot make {feature['id']} implemented")
        for reference in feature["sources"] + feature["validation_sources"]:
            if reference.startswith("https://github.com/Project-Chronicles-Emulator/"):
                continue
            if not (ROOT / reference).is_file():
                raise ValueError(f"Missing source reference: {reference}")


def feature_block(feature, packets, capture_keys):
    count_heading = " / ".join(capture_keys)
    lines = [f"## {feature['title']}", "", f"**Feature status: {feature['status'].upper()}.**",
             "", feature["scope"], "", "**Current implementation**", ""]
    lines.extend(f"- {item}" for item in feature["implemented"])
    if feature.get("custom_behavior"):
        lines.extend(["", "**Custom / guessed behavior (not official)**", "",
                      "Our own guesses, because the data to reproduce the official behavior does "
                      "not exist. They add no capture evidence and do not promote any status.", ""])
        lines.extend(f"- {item}" for item in feature["custom_behavior"])
    lines.extend(["", "**Missing behavior and limitations**", ""])
    lines.extend(f"- {item}" for item in feature["missing"])
    lines.extend(["", "**Evidence and verification**", "", feature["verification"], ""])
    lines.extend(f"- Source: `{reference}`." for reference in feature["sources"])
    lines.extend(f"- Validation source: `{reference}`." for reference in feature["validation_sources"])
    lines.extend(["", f"**Dependencies:** {feature['dependencies']}", "",
                  f"**Packet inventory** — counts follow capture keys {count_heading}. "
                  "Zero means absent from these world logs, not impossible or unsupported by the game.", "",
                  f"| Packet / payload schema | ID or route | Runtime | {count_heading} |",
                  "| --- | --- | --- | --- |"])
    for packet in packets:
        identity = f"`{packet['packet']}`"
        if packet["schema"]:
            identity += f"<br>`{packet['schema']}`"
        identifier = str(packet["opcode"]) if packet["opcode"] is not None else packet["mapping"]
        counts = " / ".join(map(str, packet["capture_counts"]))
        lines.append(f"| {identity} | {identifier} | {packet['runtime_status']} | {counts} |")
    lines.extend(["", "**Payload fields and packet-specific runtime limits**", ""])
    for packet in packets:
        fields = "; ".join(packet["fields"]) or (
            "Empty protobuf body." if packet["schema"] else "Payload schema unresolved or route has no protobuf body."
        )
        custom = (f" CUSTOM (guessed, not official): {packet['custom_note']}"
                  if "custom_note" in packet else "")
        lines.append(f"- **`{packet['packet']}`** — {packet['runtime_note']}{custom} "
                     f"Schema file: `{packet['schema_file']}`. Fields: {fields}")
    return "\n".join(lines)


def render(coverage, observations, packets):
    status_counts = collections.Counter(packet["runtime_status"] for packet in packets)
    mapped = sum(packet["opcode"] is not None for packet in packets)
    capture_keys = [capture.get("key", f"C{index + 1}")
                    for index, capture in enumerate(observations["captures"])]
    combined_counts = collections.Counter()
    for capture in observations["captures"]:
        combined_counts.update(capture["packet_counts"])
    total_records = sum(combined_counts.values())
    client_records = sum(count for name, count in combined_counts.items()
                         if name.startswith("PC2S_"))
    server_records = sum(count for name, count in combined_counts.items()
                         if name.startswith("PS2C_"))
    introduction = [
        "# ArcheAge Chronicles Demo — protocol and feature coverage",
        "",
        f"Reviewed: {coverage['reviewed_at']}. Client native-log build: `{coverage['build_id']}`.",
        "",
        "This is the maintained reference for every recovered client-facing message in the current "
        "descriptor set, every established world/native-WebSocket packet mapping, unresolved named "
        "wrappers, and known auth routes without request bodies. It is not proof that unknown packets "
        "or future builds do not exist. Internal/common protobuf DTOs are referenced by field type, "
        "rather than incorrectly counted as standalone network packets. REST files also contain "
        "helper DTOs; their rows explicitly retain an unverified route.",
        "",
        "## Status and evidence policy",
        "",
        "- **Implemented:** a concrete current runtime path exists for the stated bounded contract. "
        "It does not certify complete official semantics, deployment or stock-client compatibility.",
        "- **Partial:** a subset, fixture, bootstrap, tutorial-only implementation, mixed-use packet, "
        "or transport path exists; the documented remaining work is still required.",
        "- **Missing:** no current behavior-producing runtime path was established. Decoding, generated "
        "protobuf classes, extracted action data, rejecting a request, or ignoring a report do not "
        "count as implementing the feature.",
        "- Packet status is independent of feature status. An implemented message can belong to an "
        "unfinished feature. Captured official success, static schemas, source/tests and observed "
        "local-client behavior are separate evidence layers.",
        "- The 2026-11-03 tools cleanup retired Python/PowerShell test harnesses and obsolete tooling. "
        "Historical test results and commands remain dated evidence, not runnable checks in this checkout. "
        "Runtime contracts and packet/feature status labels are unchanged; C++ tests remain available.",
        "- Database distribution now contains one complete schema and its installer. Historical "
        "upgrade, rollback and maintenance SQL references describe retired workflows; the "
        "inactive schema-45 experiment is not installed. Runtime contracts remain at schema 44.",
        "- Test source references identify available checks; this documentation pass did not run "
        "the full emulator suite or a new original-client gameplay trial. Existing validation "
        "reports must retain their dates and limitations.",
        "",
        "## Coverage totals",
        "",
        f"Catalog: **{len(packets)} entries**, including **{mapped} established numeric mappings**. "
        f"Packet runtime labels: {status_counts['implemented']} implemented, "
        f"{status_counts['partial']} partial, {status_counts['missing']} missing. "
        "These are message-contract counts, not a gameplay-completion percentage.",
        "",
        "| Key | Capture | Complete frames | Distinct packet types | Observed levels |",
        "| --- | --- | ---: | ---: | --- |",
    ]
    for key, capture in zip(capture_keys, observations["captures"]):
        levels = ", ".join(map(str, capture["levels_observed"]))
        introduction.append(f"| {key} | {capture['label']} | {capture['records']:,} | "
                            f"{len(capture['packet_counts'])} | {levels} |")
    introduction.extend([
        "",
        f"The {len(capture_keys)} captures contain **{total_records:,} complete frames across "
        f"{len(combined_counts)} packet types**, with {client_records:,} client frames and "
        f"{server_records:,} server frames. Every included capture's complete frames decoded "
        "and passed semantic encode/decode round trips.",
        "",
        "**Original three-capture baseline (T/P1/P2):** 70,972 complete frames across 153 packet "
        "types, with 9,788 client frames and 61,184 server frames. Those frames passed "
        "semantic encode/decode round trips. Eight map IDs, levels 1–8, 42 completed quest IDs, "
        "173 NPC CMS types and 40 requested player/item skills were observed. Counts describe "
        "world traffic including nearby entities; they must not all be attributed to the player.",
        "",
        "**Original baseline activity:** 22 equip requests, seven polishing operations, one dismantle, two "
        "discards, 77 loot requests, 57 dialogue starts, four buys, three sales, five repairs, "
        "nine cooking results for one recipe, gathering/mining/tool actions, five perk changes, "
        "five stat-allocation requests, four self deaths/revive requests, 12 gliding starts, seven "
        "teleport starts and 16 ranking list exchanges. Merchant requests were followed by "
        "shop-reason item/money updates; requests alone are not a general success criterion.",
        "",
        "**Original baseline blind spots:** no chat sends/receives, no full party join/leave lifecycle, "
        "no leaderboard score submission, and no substantive housing/guild/mount/mail/market "
        "flows. Player skills primarily cover Wild handaxe/longbow. Empty login snapshots "
        "do not establish a working subsystem. No PC2S_MoveReq appeared; the log does contain "
        "root-motion, rollbacks, gliding and map changes. Auth and front WebSockets were not "
        "recorded by this world logger.",
        "",
        "## Transport namespaces and serialization",
        "",
        "- PC2S/PS2C: world TLS application frames. Little-endian length/opcode; client frames "
        "also contain a sequence. Native X5Packet.log adds elapsed milliseconds and a 57-byte header.",
        "- WSC2A/WSA2C and WSC2F/WSF2C: separate auth/front WebSocket namespaces; four-byte "
        "length/ID envelope, no world sequence. A repeated numeric ID in another service is not a collision.",
        "- HTTP_C2A/HTTP_A2C: native auth protobuf payloads. Only documented routes are established. "
        "REST_C2F/REST_F2C include schema-only REST shapes whose URLs remain unverified unless a "
        "row names its route. C2AE/AE2C/HTTP_C2AE are JSON agent-engine (AI chat) shapes on the "
        "client's separate agent-engine base URL; rows marked emulator-defined have no client caller.",
        "- Field listings use descriptor field numbers, exact names and scalar/message/enum types. "
        "Repeated protobuf fields can include maps; nested structures and enums remain authoritative "
        "in protocol/descriptors.normalized.pb. Field presence/defaults must not be guessed from names.",
        "- The native-log build identifier and dumped executable SHA-256 identify different artifacts. "
        "The opcode inputs record their dump provenance; revalidate matches for another client build.",
        "",
        "## Maintenance contract",
        "",
        "Read this reference and the relevant feature's source and evidence before implementation. "
        "Update protocol/feature-coverage.json and any affected packet overrides in the same change "
        "as a behavior, persistence, protocol or validation update. Update capture observations only "
        "from sanitized verified counts. Never publish raw packets, player IDs, names, chat text, "
        "credentials or access-bearing URLs.",
        "",
        "After reviewing changed sources, refresh their recorded hashes deliberately and regenerate:",
        "",
        "```powershell",
        "python tools/build_protocol_reference.py --refresh-reviewed-sources",
        "python tools/build_protocol_reference.py --check",
        "```",
        "",
        "Refreshing hashes records a human/agent review; it does not audit behavior or promote "
        "statuses automatically. Pure refactors may retain statuses but must record the preserved "
        "contract and relevant checks. Synchronize the linked Page using its current block IDs/hashes; "
        "if access is unavailable, retain the local update and explicitly report the Page as stale. "
        "Do not mark the task's documentation complete while silently leaving either copy outdated.",
        "",
        "## Feature index",
        "",
        "| Feature | Overall status | Catalog entries |",
        "| --- | --- | ---: |",
    ])
    blocks = []
    for feature in coverage["features"]:
        grouped = [packet for packet in packets if packet["feature"] == feature["id"]]
        introduction.append(f"| {feature['title']} | {feature['status']} | {len(grouped)} |")
        blocks.append(feature_block(feature, grouped, capture_keys))

    pending_reviews = pending_source_reviews(coverage)
    if pending_reviews:
        introduction.extend([
            "",
            "### Pending runtime source reviews",
            "",
            "The capture evidence below is current, but concurrent runtime edits have "
            "not completed the reference review workflow. Existing implementation labels "
            "remain bounded by their recorded evidence. Source hashes were not refreshed "
            "to certify these edits, and the strict reference check remains failing.",
            "",
            ", ".join(f"`{path}`" for path in pending_reviews) + ".",
        ])
    comparison_blocks = []
    for comparison in observations.get("comparisons", []):
        totals = comparison["combined"]
        new_types = ", ".join(
            f"`{name}` ({count})" for name, count in comparison["new_packet_type_counts"].items()
        )
        lines = [
            f"## {comparison.get('title', 'Additional capture comparison')} — "
            f"{comparison['reviewed_at']}",
            "",
            f"Capture {comparison['new_capture_key']} adds {comparison['new_records']:,} "
            f"complete frames against baseline {' / '.join(comparison['baseline_keys'])}. "
            f"New packet types: {new_types}.",
            "",
            f"Combined evidence now covers {totals['maps']} maps, "
            f"{totals['completed_quests']} completed quest IDs, {totals['npc_types']} NPC CMS "
            f"types and {totals['requested_player_item_skills']} requested player/item skills.",
        ]
        for key, label in [
            ("completed_quest_ids", "Newly completed quest IDs"),
            ("npc_cms_ids", "Newly observed NPC CMS types"),
            ("map_ids", "Newly observed map IDs"),
            ("skill_ids", "Newly requested player/item skill IDs"),
        ]:
            values = comparison["novel_static_ids"].get(key, [])
            description = ", ".join(map(str, values)) if values else "none"
            lines.extend(["", f"{label}: {description}."])
        for finding in comparison["findings"]:
            lines.extend(["", f"### {finding['title']}", "", finding["evidence"],
                          "", f"**Boundary:** {finding['limit']}"])
        for table in comparison.get("tables", []):
            lines.extend(["", f"### {table['title']}", ""])
            lines.append("| " + " | ".join(table["columns"]) + " |")
            lines.append("| " + " | ".join("---" for _ in table["columns"]) + " |")
            for row in table["rows"]:
                if len(row) != len(table["columns"]):
                    raise ValueError(f"Comparison table has inconsistent columns: {table['title']}")
                cells = [str(cell).replace("|", "\\|").replace("\n", " ") for cell in row]
                lines.append("| " + " | ".join(cells) + " |")
            if table.get("note"):
                lines.extend(["", table["note"]])
        lines.extend(["", "### Unchanged blind spots", ""])
        lines.extend(f"- {item}" for item in comparison["unchanged_blind_spots"])
        lines.extend(["", f"**Validation:** {comparison['validation']}"])
        if comparison.get("capture_recovery"):
            lines.extend(["", comparison["capture_recovery"]])
        comparison_blocks.append("\n".join(lines))
    sources = ["## Provenance and reproducibility", ""]
    for path in ["protocol/descriptors.normalized.pb", "protocol/opcodes.json",
                 "protocol/native-ws-opcodes.json", "protocol/demo-capture-observations.json",
                 "protocol/feature-coverage.json"]:
        sources.append(f"- `{path}`: SHA-256 `{sha256(ROOT / path)}`.")
    sources.extend(["", "Raw captures and decoded payloads remain under ignored out/captures/. "
                    "The committed observations contain sanitized labels, static packet names/IDs, "
                    "counts, levels, relative log offsets, file hashes and bounded comparisons. "
                    "The full machine-readable catalog is "
                    "docs/protocol-packet-catalog.json. See docs/capture-corpus.md, VALIDATION.md "
                    "and NATIVE_PROTOCOL.md for the owning evidence and validation boundaries."])
    return ["\n".join(introduction), *comparison_blocks, *blocks, "\n".join(sources)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--refresh-reviewed-sources", action="store_true")
    parser.add_argument(
        "--allow-pending-source-reviews",
        action="store_true",
        help="Render current evidence with an explicit pending-review warning; never passes --check",
    )
    args = parser.parse_args()
    if args.check and args.refresh_reviewed_sources:
        parser.error("Review refresh and read-only checking are separate operations")
    if args.allow_pending_source_reviews and (args.check or args.refresh_reviewed_sources):
        parser.error("Pending-review rendering cannot check or refresh reviewed sources")
    coverage = read_json(COVERAGE_PATH)
    validate_references(coverage)
    if args.refresh_reviewed_sources:
        for path in coverage["reviewed_sources"]:
            coverage["reviewed_sources"][path] = reviewed_source_sha256(ROOT / path)
        COVERAGE_PATH.write_text(json.dumps(coverage, indent=2) + "\n", encoding="utf-8")
    changed_sources = pending_source_reviews(coverage)
    if changed_sources:
        message = f"Review changed sources and update feature coverage: {changed_sources}"
        if not args.allow_pending_source_reviews:
            raise ValueError(message)
        print(f"WARNING: {message}. Review hashes remain unchanged; --check will fail.")
    observations = read_json(OBSERVATIONS_PATH)
    packets = build_catalog(coverage, observations)
    blocks = render(coverage, observations, packets)
    document = "\n\n".join(blocks) + "\n"
    catalog = json.dumps({"build_id": coverage["build_id"], "packets": packets}, indent=2) + "\n"
    outputs = {DOCUMENT_PATH: document, CATALOG_PATH: catalog}
    if args.check:
        outdated = [str(path.relative_to(ROOT)) for path, content in outputs.items()
                    if not path.exists() or path.read_text(encoding="utf-8") != content]
        if outdated:
            raise ValueError(f"Regenerate outdated protocol reference outputs: {outdated}")
        if not PUBLICATION_PATH.exists():
            raise ValueError("Page publication has not been recorded after confirmed readback")
        validate_publication(read_json(PUBLICATION_PATH), sha256(DOCUMENT_PATH))
        print(f"Protocol reference is current: {len(packets)} catalog entries; source hashes agree.")
        return
    for path, content in outputs.items():
        path.write_text(content, encoding="utf-8", newline="\n")
    PAGE_BLOCKS_PATH.parent.mkdir(parents=True, exist_ok=True)
    PAGE_BLOCKS_PATH.write_text(json.dumps(blocks), encoding="utf-8", newline="\n")
    print(f"Generated {len(packets)} catalog entries; {len(document.encode('utf-8'))} Markdown bytes.")


if __name__ == "__main__":
    main()
