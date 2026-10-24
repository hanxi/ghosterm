#!/usr/bin/env python3
"""Prove public header checks detect inverted edges, cycles and hidden dependencies."""
import hashlib
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "core/include"
MARK = "GT_HEADER_MUTATION_MARK"


def hashes(include):
    return {p.relative_to(include): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in include.rglob("*.h")}


def check(include, expected):
    result = subprocess.run([sys.executable, str(ROOT / "scripts/check-public-headers.py"),
                             "--include", str(include)], capture_output=True, text=True,
                            timeout=60)
    if (result.returncode == 0) != expected:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout + result.stderr


def interrupted(signum, frame):
    raise InterruptedError(f"signal {signum}")


def main():
    for header in INCLUDE.rglob("*.h"):
        if MARK in header.read_text():
            print("Residual mutation marker: refusing to start", file=sys.stderr)
            return 2
    start_hashes = hashes(INCLUDE)
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    (ROOT / "tmp").mkdir(exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="mutation-headers-", dir=ROOT / "tmp") as tmp:
            copy = Path(tmp) / "include"
            shutil.copytree(INCLUDE, copy)
            check(copy, True)
            mutations = (
                ("aggregate edge", "ghosterm/transport.h", '#include "ghosterm/types.h"',
                 '#include "ghosterm.h"', "subheader includes aggregate"),
                ("hidden aggregate type", "ghosterm/transport.h",
                 "typedef struct gt_transport gt_transport_t;",
                 "typedef struct gt_transport gt_transport_t;\n"
                 "gt_event_t *gt__hidden_dependency(void);", "gt_event_t"),
                ("subheader cycle", "ghosterm/types.h", "#include <stdbool.h>",
                 '#include <stdbool.h>\n#include "ghosterm/transport.h"',
                 "public include cycle"),
            )
            for label, name, old, new, reason in mutations:
                path = copy / name
                original = path.read_text()
                if original.count(old) != 1:
                    raise RuntimeError(f"missing or ambiguous mutation anchor: {label}")
                try:
                    path.write_text(original.replace(old, new + f" /* {MARK} */", 1))
                    output = check(copy, False)
                    if reason not in output:
                        raise RuntimeError(f"mutation failed for wrong reason: {label}: {output}")
                    # Same mutation, unrelated standalone config header still compiles.
                    source = Path(tmp) / "control.c"
                    source.write_text('#include "ghosterm/config.h"\n')
                    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-fsyntax-only", "-I", str(copy), str(source)],
                                   check=True, timeout=30)
                finally:
                    path.write_text(original)
                    if hashes(copy) != start_hashes:
                        raise RuntimeError("rollback SHA256 mismatch")
                check(copy, True)
                print(f"PASS {label}: target red, control green, rollback green")
    finally:
        if hashes(INCLUDE) != start_hashes:
            raise RuntimeError("repository headers changed during mutation run")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f"public header mutations failed: {error}", file=sys.stderr)
        sys.exit(1)
