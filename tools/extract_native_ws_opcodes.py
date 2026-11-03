"""Recover native WebSocket IDs using the wrapper layout established in IDA.

This reuses the asset extractor's PE reader and recognizes only constant-ID
getters paired with exact UTF-16 packet-name getters. It never assigns IDs from
descriptor ordering, field numbers, or neighboring wrappers.
"""

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path


EXTRACTOR_DIRECTORY = Path(__file__).resolve().parents[2] / "ArcheAgeAssetExtractor"
sys.path.insert(0, str(EXTRACTOR_DIRECTORY))
from extract_opcodes import PE


def constant_packet_id(pe, getter_address):
    instruction_bytes = pe.read(getter_address, 8)
    if instruction_bytes[:1] == b"\xb8" and instruction_bytes[5:6] == b"\xc3":
        return struct.unpack_from("<I", instruction_bytes, 1)[0]

    if instruction_bytes[:3] == b"\x33\xc0\xc3":
        return 0

    return None


def find_packet_names(rdata, rdata_address):
    name_pattern = (
        rb"W\x00S\x00(?:(?:C\x002\x00[AF]\x00)|(?:[AF]\x002\x00C\x00))"
        rb"_\x00(?:[A-Za-z0-9_]\x00){1,100}\x00\x00"
    )
    packet_names = {}
    for match in re.finditer(name_pattern, rdata):
        packet_names[rdata_address + match.start()] = match.group()[:-2].decode("utf-16le")
    return packet_names


def find_name_getters(text, text_address, packet_names):
    name_getters = {}
    for match in re.finditer(rb"\x48\x8d\x05[\s\S]{4}\xc3", text):
        getter_address = text_address + match.start()
        displacement = struct.unpack_from("<i", match.group(), 3)[0]
        target_address = getter_address + 7 + displacement
        if target_address in packet_names:
            name_getters[getter_address] = target_address
    return name_getters


def extract_wrappers(pe):
    rdata_address, rdata_offset, rdata_size, _ = pe.sections[".rdata"][0]
    rdata = pe.data[rdata_offset : rdata_offset + rdata_size]
    text_address, text_offset, text_size, _ = pe.sections[".text"][0]
    text = pe.data[text_offset : text_offset + text_size]

    packet_names = find_packet_names(rdata, rdata_address)
    name_getters = find_name_getters(text, text_address, packet_names)
    wrappers = {}

    for offset in range(16, len(rdata) - 48, 8):
        name_getter = struct.unpack_from("<Q", rdata, offset)[0]
        if name_getter not in name_getters:
            continue

        destructor, id_getter = struct.unpack_from("<QQ", rdata, offset - 16)
        if not text_address <= destructor < text_address + text_size:
            continue

        packet_id = constant_packet_id(pe, id_getter)
        if packet_id is None or packet_id > 65535:
            continue

        name_address = name_getters[name_getter]
        packet_name = packet_names[name_address]
        previous = wrappers.get(packet_name)
        if previous is not None and previous["opcode"] != packet_id:
            raise ValueError(f"Conflicting native WebSocket IDs for {packet_name}")

        wrappers[packet_name] = {
            "packet": packet_name,
            "opcode": packet_id,
            "vtable_va": hex(rdata_address + offset - 16),
            "id_getter_va": hex(id_getter),
            "name_getter_va": hex(name_getter),
            "name_va": hex(name_address),
            "id_getter_bytes": pe.read(id_getter, 8).hex(),
        }

    unresolved = sorted(set(packet_names.values()) - set(wrappers))
    entries = sorted(
        wrappers.values(),
        key=lambda row: (row["packet"].split("_", 1)[0], row["opcode"]),
    )
    return entries, unresolved


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()

    binary_bytes = arguments.binary.read_bytes()
    entries, unresolved = extract_wrappers(PE(binary_bytes))
    result = {
        "source_sha256": hashlib.sha256(binary_bytes).hexdigest(),
        "method": (
            "IDA-established wrapper vtable: constant ID getter and exact UTF-16 packet label; "
            "no sequence inference"
        ),
        "unresolved_named_packets": unresolved,
        "entries": entries,
    }

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"native_ws_wrappers": len(entries), "unresolved": len(unresolved)}))


if __name__ == "__main__":
    main()
