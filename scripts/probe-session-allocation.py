#!/usr/bin/env python3
"""Probe pinned VT error visibility and C allocator cleanup; not a total RSS cap."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "terminal/session-state/allocation-probe.c"
MARK = "GT_P0_ALLOCATION_MUTATION_MARK"


def compile_probe(source, binary, library):
    extra = ["-lutil"] if sys.platform.startswith("linux") else []
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                    "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-I",
                    str(ROOT / "vendor/ghostty/include"), "-I",
                    str(ROOT / "terminal/session-state"), str(source), str(library),
                    *extra, "-o", str(binary)], check=True, timeout=60)


def probe(binary, case, expected=0):
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=15)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f"allocation/{case}: expected exit {expected}, got {result.returncode}")


def mutations(work, library, original):
    variants = (
        ("semantic error hidden", "  return failed;", "  return false;", "failure", "budget"),
        ("budget check bypassed", "len > b->limit-b->live", "false", "budget", "failure"),
        ("decoder cleanup omitted", "  ghostty_snapshot_decoder_free(d);",
         "  (void)d;", "budget", "failure"),
        ("reset loses prior failure", "  ghostty_terminal_reset(t);",
         "  ghostty_terminal_free(t);\n"
         "  REQUIRE(ghostty_terminal_new(&alloc,&t,80,24)==GHOSTTY_SUCCESS);",
         "failure", "budget"),
    )
    source = work / "mutated.c"
    binary = work / "mutated"
    digest = hashlib.sha256(original).digest()
    text = original.decode()
    for label, old, new, target, control in variants:
        if text.count(old) != 1:
            raise RuntimeError(f"mutation anchor is not unique: {label}")
        try:
            source.write_text(text.replace(old, new + f" /* {MARK} */", 1))
            compile_probe(source, binary, library)
            probe(binary, target, 1)
            probe(binary, control)
            print(f"PASS mutation/{label}: target red, control green", flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError("allocation mutation rollback SHA256 mismatch")
        compile_probe(source, binary, library)
        probe(binary, target)


def interrupted(signum, frame):
    raise InterruptedError(f"signal {signum}; restoring private mutation source")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    files = (SOURCE, ROOT / "terminal/session-state/probe.c")
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    original = SOURCE.read_bytes()
    if any(MARK in p.read_text() for p in files):
        return 2
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    (ROOT / "tmp").mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(ROOT / "tmp")
    try:
        shared = runpy.run_path(str(ROOT / "scripts/probe-session-state.py"))
        library = shared["build_engine"]()
        with tempfile.TemporaryDirectory(prefix="session-allocation-", dir=ROOT / "tmp") as tmp:
            work = Path(tmp)
            binary = work / "probe"
            compile_probe(SOURCE, binary, library)
            probe(binary, "failure")
            probe(binary, "budget")
            if args.mutations:
                mutations(work, library, original)
                print("PASS 4 allocation mutations and rollback")
        print("P0 remains incomplete: native page mappings bypass the C allocator budget")
        return 0
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError(f"repository source SHA256 changed: {path}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f"session-allocation probe failed: {error}", file=sys.stderr)
        sys.exit(1)
