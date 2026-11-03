"""Copy only schema/opcode metadata; packet captures and keys stay external."""

import argparse
import hashlib
import json
from pathlib import Path

from google.protobuf import descriptor_pb2


METADATA_FILES = ("descriptors.pb", "descriptors.normalized.pb", "opcodes.json")


def copy_metadata(source_directory, output_directory):
    manifest = {}
    for name in METADATA_FILES:
        data = (source_directory / name).read_bytes()
        (output_directory / name).write_bytes(data)
        manifest[name] = {
            "sha256": hashlib.sha256(data).hexdigest(),
            "bytes": len(data),
        }
    return manifest


def validate_normalization(output_directory):
    original = descriptor_pb2.FileDescriptorSet.FromString(
        (output_directory / "descriptors.pb").read_bytes()
    )
    normalized = descriptor_pb2.FileDescriptorSet.FromString(
        (output_directory / "descriptors.normalized.pb").read_bytes()
    )
    if len(original.file) != len(normalized.file):
        raise ValueError("Descriptor count differs")

    for before, after in zip(original.file, normalized.file):
        expected = descriptor_pb2.FileDescriptorProto()
        expected.CopyFrom(before)
        if expected.syntax == "x5msg":
            expected.syntax = "proto3"
        if expected != after:
            raise ValueError("Normalization changed more than syntax: " + before.name)

    return len(original.file)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "protocol",
    )
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    manifest = copy_metadata(args.source, args.output)
    schema_count = validate_normalization(args.output)

    summary = {
        "files": manifest,
        "schema_files": schema_count,
        "normalization": "x5msg -> proto3 only",
    }
    (args.output / "import-manifest.json").write_text(
        json.dumps(summary, indent=2), encoding="utf-8"
    )
    print(
        f"Imported {schema_count} descriptors and opcode metadata; "
        "no live capture or keys copied"
    )


if __name__ == "__main__":
    main()
