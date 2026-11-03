"""Export the complete recovered descriptor set as lossless, readable protobuf JSON."""

import argparse
import hashlib
from pathlib import Path

from google.protobuf import descriptor_pb2, json_format

from import_protocol import validate_normalization
from protocol_schema import load_descriptor_pool


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    validate_normalization(root / "protocol")
    source = root / "protocol/descriptors.normalized.pb"
    load_descriptor_pool(source)
    descriptors = descriptor_pb2.FileDescriptorSet.FromString(source.read_bytes())
    text = json_format.MessageToJson(
        descriptors, preserving_proto_field_name=True, indent=2,
        sort_keys=True, ensure_ascii=False,
    ) + "\n"
    restored = descriptor_pb2.FileDescriptorSet()
    json_format.Parse(text, restored)
    if restored != descriptors:
        raise ValueError("Readable schema does not preserve the complete descriptor set")

    destination = root / "protocol/packet-fields.json"
    if args.check:
        if not destination.exists() or destination.read_text(encoding="utf-8") != text:
            raise SystemExit("Packet field export is stale; run tools/export_protocol_schema.py")
    else:
        destination.write_text(text, encoding="utf-8", newline="\n")

    print(f"Validated {len(descriptors.file)} schema files; all recovered fields preserved.")
    print(f"Descriptor SHA-256: {hashlib.sha256(source.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
