"""Independently read back every output and compare its archive-capture digest."""

import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("capture", type=Path)
args = parser.parse_args()
root = args.capture.resolve()
count = total = 0
errors = []
paths = set()
for line in (root / "extraction-manifest.jsonl").open(encoding="utf-8"):
    row = json.loads(line)
    if row["status"] != "ok":
        errors.append(row)
        continue
    if row["path"] in paths:
        errors.append({"path": row["path"], "error": "duplicate manifest path"})
    paths.add(row["path"])
    path = (root / "raw" / row["path"]).resolve()
    if not path.is_relative_to(root / "raw"):
        raise ValueError("Unsafe manifest path")
    try:
        data = path.read_bytes()
        if (
            len(data) != row["size"]
            or hashlib.sha256(data).hexdigest() != row["sha256"]
        ):
            errors.append({"path": row["path"], "error": "hash/size mismatch"})
        count += 1
        total += len(data)
    except OSError as exc:
        errors.append({"path": row["path"], "error": str(exc)})
result = {"files_verified": count, "bytes_verified": total, "errors": errors}
(root / "verification.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps(result, indent=2))
raise SystemExit(bool(errors))
