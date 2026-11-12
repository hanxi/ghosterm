#!/usr/bin/env python3
"""Generate the deterministic integrity index for the official preview package."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "plugins/official/preview"


def expected_bytes():
    members = []
    for path in sorted(p for p in PACKAGE.rglob("*") if p.is_file() and p.name != "integrity.json"):
        relative = path.relative_to(PACKAGE).as_posix()
        data = path.read_bytes()
        members.append({"path": relative, "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
    return (json.dumps({"schemaVersion": 1, "members": members},
                       ensure_ascii=False, separators=(",", ":")) + "\n").encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    target = PACKAGE / "integrity.json"
    expected = expected_bytes()
    if args.check:
        if not target.is_file() or target.read_bytes() != expected:
            raise SystemExit("preview package integrity is stale; run python3 scripts/build-plugin-preview.py")
    else:
        target.write_bytes(expected)
    digest = hashlib.sha256(expected).hexdigest()
    print(f"PREVIEW PACKAGE {'CHECK' if args.check else 'BUILT'} PASS: {digest}")


if __name__ == "__main__":
    main()
