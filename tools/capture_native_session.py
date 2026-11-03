"""Capture a bounded, bidirectional slice of the client's native packet log.

The raw slice can contain character and session data. Keep its output under out/.
Only packet metadata is printed or written to the index.
"""

import argparse
import collections
import hashlib
import json
import struct
import time
from datetime import datetime, timezone
from pathlib import Path


HEADER_BYTES = 57
MAX_FRAME_BYTES = 65535
DEFAULT_LOG = Path.home() / "AppData/Local/X5/Saved/Logs/X5Packet.log"


def read_snapshot(path: Path, maximum_bytes: int) -> bytes:
    with path.open("rb") as stream:
        data = stream.read(maximum_bytes + 1)

    if len(data) > maximum_bytes:
        raise ValueError("Native packet log exceeds --max-mb")

    validate_header(data)
    return data


def validate_header(data: bytes) -> None:
    if len(data) < HEADER_BYTES or data[40] != 0:
        raise ValueError("Native packet log has an incomplete or invalid header")

    if any(character not in b"0123456789abcdef" for character in data[:40]):
        raise ValueError("Native packet log build ID is invalid")


def complete_records(data: bytes):
    """Yield (start, end, elapsed_ms, opcode) for complete native records."""
    validate_header(data)
    position = HEADER_BYTES

    while len(data) - position >= 8:
        elapsed_ms, frame_length, opcode = struct.unpack_from("<IHH", data, position)
        if frame_length < 4 or frame_length > MAX_FRAME_BYTES:
            raise ValueError(f"Invalid frame length at offset {position}")

        record_end = position + 4 + frame_length
        if record_end > len(data):
            break

        yield position, record_end, elapsed_ms, opcode
        position = record_end


def complete_end(data: bytes) -> int:
    end = HEADER_BYTES
    for _, record_end, _, _ in complete_records(data):
        end = record_end
    return end


def opcode_map(path: Path) -> dict[int, dict]:
    document = json.loads(path.read_text(encoding="utf-8"))
    result = {}

    for entry in document["entries"]:
        opcode = int(entry["opcode"])
        if opcode in result:
            raise ValueError(f"Duplicate opcode {opcode}")
        result[opcode] = entry

    return result


def capture(source: Path, output: Path, label: str, seconds: float,
            maximum_bytes: int, opcodes: dict[int, dict]) -> dict:
    if output.exists():
        raise ValueError("Output directory already exists; use a new directory")

    initial = read_snapshot(source, maximum_bytes)
    initial_end = complete_end(initial)
    started_at = datetime.now(timezone.utc).isoformat()
    output.mkdir(parents=True)
    print(f"Capture armed: {label}; use the game now for {seconds:g} seconds", flush=True)

    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        time.sleep(min(0.25, max(0, deadline - time.monotonic())))

    final = read_snapshot(source, maximum_bytes)
    if len(final) < len(initial) or final[:len(initial)] != initial:
        raise ValueError("Native packet log rotated or changed during capture")

    final_end = complete_end(final)
    sliced_log = final[:HEADER_BYTES] + final[initial_end:final_end]
    counts = collections.Counter()
    index = []

    for start, end, elapsed_ms, opcode in complete_records(sliced_log):
        mapping = opcodes.get(opcode)
        direction = mapping["family"] if mapping else "unknown"
        packet = mapping["packet"] if mapping else "unknown"
        counts[direction] += 1
        index.append({
            "offset": start,
            "elapsed_ms": elapsed_ms,
            "direction": direction,
            "opcode": opcode,
            "packet": packet,
            "frame_bytes": end - start - 4,
        })

    capture_path = output / "native-session.bin"
    capture_path.write_bytes(sliced_log)
    with (output / "packet-index.jsonl").open("w", encoding="utf-8") as stream:
        for record in index:
            stream.write(json.dumps(record, separators=(",", ":")) + "\n")

    summary = {
        "label": label,
        "source": str(source),
        "started_at_utc": started_at,
        "ended_at_utc": datetime.now(timezone.utc).isoformat(),
        "build_id": final[:40].decode("ascii"),
        "source_start_bytes": len(initial),
        "source_final_bytes": len(final),
        "source_complete_start_offset": initial_end,
        "source_complete_final_offset": final_end,
        "incomplete_tail_bytes": len(final) - final_end,
        "records": len(index),
        "directions": dict(counts),
        "capture_sha256": hashlib.sha256(sliced_log).hexdigest(),
        "capture_file": capture_path.name,
        "privacy": "Raw application packets may contain private character/session values; keep under ignored out/.",
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--opcodes", type=Path,
                        default=Path(__file__).resolve().parents[1] / "protocol/opcodes.json")
    parser.add_argument("--label", required=True)
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--max-mb", type=int, default=64)
    args = parser.parse_args()

    if not 0 < args.seconds <= 600:
        parser.error("--seconds must be greater than zero and at most 600")
    if not 1 <= args.max_mb <= 512:
        parser.error("--max-mb must be between 1 and 512")
    if not args.label.strip() or len(args.label) > 80:
        parser.error("--label must contain 1 to 80 characters")

    summary = capture(args.source, args.output, args.label, args.seconds,
                      args.max_mb * 1024 * 1024, opcode_map(args.opcodes))
    print(json.dumps({key: summary[key] for key in ("label", "records", "directions",
                                                "incomplete_tail_bytes", "capture_file")},
                     indent=2), flush=True)


if __name__ == "__main__":
    main()
