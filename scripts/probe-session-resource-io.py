#!/usr/bin/env python3
"""P0 resource ownership: a consumed Kitty temp file cannot be replayed twice."""
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
SOURCE = ROOT / "terminal/session-state/resource-io-probe.c"
MARK = "GT_P0_RESOURCE_IO_MUTATION_MARK"


def compile_probe(source, binary, library):
    extra = ["-lutil"] if sys.platform.startswith("linux") else []
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                    "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror", "-I",
                    str(ROOT / "vendor/ghostty/include"), "-I",
                    str(ROOT / "terminal/session-state"), str(source), str(library),
                    *extra, "-o", str(binary)], check=True, timeout=60)


def probe(binary, directory, case, expected=0):
    result = subprocess.run([str(binary), case, str(directory)], capture_output=True,
                            text=True, timeout=15)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f"resource-io/{case}: expected exit {expected}, got {result.returncode}")


def mutations(work, library, original):
    variants = (
        ("source file transmission invalid", "f=32,s=1,v=1,t=t,q=2;",
         "f=32,s=1,v=1,t=d,q=2;", "temporary"),
        ("raw replay never applied", "} else feed(replica,command);",
         "} else (void)replica;", "temporary"),
        ("resolved pixel corrupted", "base64(pixels,length,encoded,sizeof encoded);",
         "base64(pixels,length,encoded,sizeof encoded); encoded[0] = 'A';", "resolved"),
        ("resolved placement moved", 'normalized,sizeof normalized,"\\x1b[3;5H',
         'normalized,sizeof normalized,"\\x1b[1;1H', "resolved"),
    )
    source, binary = work / "mutated.c", work / "mutated"
    digest = hashlib.sha256(original).digest()
    text = original.decode()
    for label, old, new, target in variants:
        if text.count(old) != 1:
            raise RuntimeError(f"mutation anchor is not unique: {label}")
        try:
            # A comment after an anchor inside a C string would change the
            # fixture. Put the marker at file scope instead.
            source.write_text(text.replace(old, new, 1) + f"\n/* {MARK} */\n")
            compile_probe(source, binary, library)
            probe(binary, work, target, 1)
            probe(binary, work, "inline")
            print(f"PASS mutation/{label}: target red, control green", flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError("resource IO rollback SHA256 mismatch")
        compile_probe(source, binary, library)
        probe(binary, work, target, 3 if target == "temporary" else 0)


def interrupted(signum, frame):
    raise InterruptedError(f"signal {signum}; restoring private resource IO source")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=("inline", "temporary", "resolved"))
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    if args.case and args.mutations:
        parser.error("--case and --mutations are mutually exclusive")
    files = (SOURCE, ROOT / "terminal/session-state/probe.c")
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):
        return 2
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    (ROOT / "tmp").mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(ROOT / "tmp")
    try:
        shared = runpy.run_path(str(ROOT / "scripts/probe-session-state.py"))
        library = shared["build_engine"]()
        with tempfile.TemporaryDirectory(prefix="session-resource-io-", dir=ROOT / "tmp") as tmp:
            work = Path(tmp)
            binary = work / "probe"
            compile_probe(SOURCE, binary, library)
            for case in (args.case,) if args.case else ("inline", "temporary", "resolved"):
                probe(binary, work, case, 3 if case == "temporary" else 0)
            if args.mutations:
                mutations(work, library, SOURCE.read_bytes())
                print("PASS 4 resource IO mutations and rollback; P0 remains incomplete")
                return 0
        print("P0 INCOMPLETE: replicas need resolved resource events, not repeated filesystem IO")
        return 0 if args.case in ("inline", "resolved") else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError(f"repository source SHA256 changed: {path}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f"session-resource-io probe failed: {error}", file=sys.stderr)
        sys.exit(1)
