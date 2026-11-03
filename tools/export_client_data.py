"""Copy verified, static client catalogs into the emulator's runtime data folder."""

import argparse
import hashlib
import json
import os
import shutil
from pathlib import Path


CATALOG_FILES = (
    "npcs.jsonl",
    "mobs.jsonl",
    "civilians.jsonl",
    "npc-spawners.jsonl",
    "summary.json",
)
BUFFER_SIZE = 1024 * 1024


def sha256_file(path):
    digest = hashlib.sha256()

    with path.open("rb") as stream:
        while True:
            chunk = stream.read(BUFFER_SIZE)
            if not chunk:
                break

            digest.update(chunk)

    return digest.hexdigest()


def count_jsonl_records(path):
    records = 0

    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, start=1):
            if not line.strip():
                raise ValueError(f"Blank JSONL record: {path}:{line_number}")

            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError(f"Expected JSON object: {path}:{line_number}")

            records += 1

    return records


def source_files(source):
    catalog = source / "Catalog"
    summary_path = catalog / "summary.json"
    protocol_path = source / "Protocol" / "manifest.json"
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    protocol = json.loads(protocol_path.read_text(encoding="utf-8"))

    if not isinstance(summary.get("tables"), dict) or not summary["tables"]:
        raise ValueError(f"Missing catalog table summary: {summary_path}")

    files = []
    for table_name in sorted(summary["tables"]):
        if not table_name.isidentifier():
            raise ValueError(f"Invalid table name: {table_name}")

        relative_path = Path("Tables") / f"{table_name}.jsonl"
        files.append((relative_path, summary["tables"][table_name]["rows"]))

    for name in CATALOG_FILES:
        files.append((Path(name), None))

    return catalog, files, summary, protocol


def describe_source(catalog, files, summary, protocol):
    descriptions = []

    for relative_path, expected_records in files:
        path = catalog / relative_path
        if not path.is_file():
            raise FileNotFoundError(path)

        records = None
        if path.suffix == ".jsonl":
            records = count_jsonl_records(path)

        if expected_records is not None and records != expected_records:
            raise ValueError(
                f"Record count mismatch for {relative_path}: "
                f"expected {expected_records}, found {records}"
            )

        descriptions.append(
            {
                "path": relative_path.as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
                "records": records,
            }
        )

    return {
        "format_version": 1,
        "source_kind": "ArcheAge Chronicles Demo extracted static client catalog",
        "source_executable_dump_sha256": protocol["source_sha256"],
        "source_catalog_summary_sha256": sha256_file(catalog / "summary.json"),
        "table_count": len(summary["tables"]),
        "files": descriptions,
    }


def copy_atomic(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".partial")

    with source.open("rb") as input_stream, temporary.open("wb") as output_stream:
        shutil.copyfileobj(input_stream, output_stream, BUFFER_SIZE)

    os.replace(temporary, destination)


def write_text_atomic(destination, contents):
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".partial")
    temporary.write_text(contents, encoding="utf-8")
    os.replace(temporary, destination)


def verify_export(output, manifest):
    if manifest.get("format_version") != 1:
        raise ValueError("Unsupported client catalog manifest version")

    for item in manifest["files"]:
        relative_path = Path(item["path"])
        if relative_path.is_absolute() or ".." in relative_path.parts:
            raise ValueError(f"Invalid client catalog path: {relative_path}")

        path = output / relative_path
        if not path.is_file():
            raise FileNotFoundError(path)

        if path.stat().st_size != item["bytes"] or sha256_file(path) != item["sha256"]:
            raise ValueError(f"Client catalog hash mismatch: {path}")


def raw_entries(source_manifest):
    seen_paths = set()

    with source_manifest.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, start=1):
            entry = json.loads(line)
            relative_path = Path(entry["path"])
            if relative_path.is_absolute() or ".." in relative_path.parts:
                raise ValueError(f"Invalid raw client path at line {line_number}")

            if entry["status"] != "ok":
                raise ValueError(f"Unverified raw client file at line {line_number}")

            if relative_path.as_posix() in seen_paths:
                raise ValueError(f"Duplicate raw client path at line {line_number}")

            seen_paths.add(relative_path.as_posix())
            yield entry


def verify_raw_export(raw_output, manifest_path, summary_path):
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    if summary.get("format_version") != 1:
        raise ValueError("Unsupported raw client export summary version")

    if sha256_file(manifest_path) != summary["source_extraction_manifest_sha256"]:
        raise ValueError("Raw client extraction manifest hash mismatch")

    file_count = 0
    total_bytes = 0
    for entry in raw_entries(manifest_path):
        path = raw_output / entry["path"]
        if not path.is_file():
            raise FileNotFoundError(path)

        if path.stat().st_size != entry["size"] or sha256_file(path) != entry["sha256"]:
            raise ValueError(f"Raw client file hash mismatch: {path}")

        file_count += 1
        total_bytes += entry["size"]

    if file_count != summary["files"] or total_bytes != summary["bytes"]:
        raise ValueError("Raw client export count or byte total differs from its summary")

    return summary


def export_raw(source, output, summary_path):
    source_manifest = source / "extraction-manifest.jsonl"
    raw_output = output.parent / "raw"
    copied_manifest = output.parent / "raw-manifest.jsonl"
    file_count = 0
    total_bytes = 0

    for entry in raw_entries(source_manifest):
        source_path = source / "raw" / entry["path"]
        destination = raw_output / entry["path"]
        already_verified = (
            destination.is_file()
            and destination.stat().st_size == entry["size"]
            and sha256_file(destination) == entry["sha256"]
        )
        if not already_verified:
            copy_atomic(source_path, destination)
            copied_matches = (
                destination.stat().st_size == entry["size"]
                and sha256_file(destination) == entry["sha256"]
            )
            if not copied_matches:
                raise ValueError(f"Raw client file hash mismatch after copy: {destination}")

        file_count += 1
        total_bytes += entry["size"]

    verification_path = source / "verification.json"
    verification = json.loads(verification_path.read_text(encoding="utf-8"))
    readback_matches = (
        not verification["errors"]
        and verification["files_verified"] == file_count
        and verification["bytes_verified"] == total_bytes
    )
    if not readback_matches:
        raise ValueError("Source extraction readback verification does not match raw files")

    copy_atomic(source_manifest, copied_manifest)
    summary = {
        "format_version": 1,
        "source_kind": "ArcheAge Chronicles Demo verified raw client extraction",
        "source_extraction_manifest_sha256": sha256_file(source_manifest),
        "source_readback_verification_sha256": sha256_file(verification_path),
        "files": file_count,
        "bytes": total_bytes,
    }
    write_text_atomic(summary_path, json.dumps(summary, indent=2) + "\n")
    return summary


def run(source, output, lock_path, verify_only):
    if verify_only:
        manifest = json.loads(lock_path.read_text(encoding="utf-8"))
        runtime_manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        if runtime_manifest != manifest:
            raise ValueError("Runtime and tracked client catalog manifests differ")

        verify_export(output, manifest)
        return manifest

    if source is None:
        raise ValueError("--source is required when exporting client data")

    catalog, files, summary, protocol = source_files(source)
    manifest = describe_source(catalog, files, summary, protocol)

    for item in manifest["files"]:
        relative_path = Path(item["path"])
        copy_atomic(catalog / relative_path, output / relative_path)

    verify_export(output, manifest)
    manifest_text = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    write_text_atomic(output / "manifest.json", manifest_text)
    write_text_atomic(lock_path, manifest_text)
    return manifest


def main():
    emulator_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=emulator_root / "data" / "generated" / "catalog",
    )
    parser.add_argument(
        "--lock",
        type=Path,
        default=emulator_root / "data" / "catalog-manifest.json",
    )
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--include-raw", action="store_true")
    args = parser.parse_args()
    if args.verify_only and args.include_raw:
        parser.error("--verify-only and --include-raw cannot be combined")

    manifest = run(args.source, args.output, args.lock, args.verify_only)
    total_bytes = sum(item["bytes"] for item in manifest["files"])
    print(
        f"Verified {manifest['table_count']} tables, "
        f"{len(manifest['files'])} files, {total_bytes} bytes at {args.output}"
    )

    raw_summary_path = args.lock.parent / "raw-export-summary.json"
    raw_output = args.output.parent / "raw"
    copied_manifest = args.output.parent / "raw-manifest.jsonl"
    if args.verify_only and raw_summary_path.exists():
        summary = verify_raw_export(raw_output, copied_manifest, raw_summary_path)
        print(f"Verified {summary['files']} raw client files, {summary['bytes']} bytes")
    elif args.include_raw:
        if args.source is None:
            raise ValueError("--source is required to export raw client files")

        summary = export_raw(args.source, args.output, raw_summary_path)
        print(f"Exported {summary['files']} raw client files, {summary['bytes']} bytes")

    if not args.verify_only and (raw_output / "X5/Content/Data/BuiltData/Maps/ArcheWorld_Tutorial").is_dir():
        from build_tutorial_data import Source, build

        profile = build(Source(args.output.parent))
        write_text_atomic(args.output.parent / "tutorial/definitions.json",
                          json.dumps(profile, ensure_ascii=False, indent=2) + "\n")
        print("Built tutorial interaction definitions")

        from build_quest_catalog import build as build_quests

        quest_catalog = build_quests(Source(args.output.parent))
        write_text_atomic(args.output.parent / "quests/catalog.json",
                          json.dumps(quest_catalog, ensure_ascii=False, indent=2) + "\n")
        print(f"Built quest catalog with {len(quest_catalog['quests'])} quests")

        from build_contents_guides import build_guides

        guide_source = Source(args.output.parent)
        guide_profile = build_guides(guide_source)
        guide_profile["source_sha256"] = dict(sorted(guide_source.sources.items()))
        write_text_atomic(args.output.parent / "contents-guides/definitions.json",
                          json.dumps(guide_profile, ensure_ascii=False, indent=2) + "\n")
        print(f"Built contextual guide definitions with {len(guide_profile['guides'])} guides")

        from build_nuia_tears import build as build_tears

        tears = build_tears(Source(args.output.parent))
        write_text_atomic(args.output.parent / "nuia-tears/definitions.json",
                          json.dumps(tears, ensure_ascii=False, indent=2) + "\n")
        print(f"Built Nuia Tear definitions with {len(tears['placements'])} placements")

        from build_nui_statues import build as build_statues

        statues = build_statues(Source(args.output.parent))
        write_text_atomic(args.output.parent / "nui-statues/definitions.json",
                          json.dumps(statues, ensure_ascii=False, indent=2) + "\n")
        print(f"Built Nui shrine definitions with {len(statues['placements'])} placements")

        from build_map_static_npcs import build as build_static_npcs

        # Dialogue/shop targets for client-owned static NPCs; without this optional index the
        # server refuses every static NPC dialogue, including quest InteractNpc objectives.
        static_npcs = build_static_npcs(Source(args.output.parent))
        write_text_atomic(args.output.parent / "static-npcs/definitions.json",
                          json.dumps(static_npcs, ensure_ascii=False, indent=2) + "\n")
        print(f"Built static NPC interaction index with {len(static_npcs['targets'])} targets")

        from build_channel_event_data import build as build_channel_events

        channel_events = build_channel_events(Source(args.output.parent))
        write_text_atomic(args.output.parent / "channel-events/definitions.json",
                          json.dumps(channel_events, ensure_ascii=False, indent=2) + "\n")
        print(f"Built channel event definitions with "
              f"{len(channel_events['tribute_altars'])} tribute altars")

        from build_exploration_places import build as build_exploration, render_html

        exploration = build_exploration(Source(args.output.parent))
        write_text_atomic(args.output.parent / "exploration/places.json",
                          json.dumps(exploration, ensure_ascii=False, indent=2) + "\n")
        write_text_atomic(args.output.parent / "exploration/guide.html", render_html(exploration))
        print(f"Built exploration guide with {len(exploration['places'])} destinations")


if __name__ == "__main__":
    main()
