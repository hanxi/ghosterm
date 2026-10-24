#!/usr/bin/env python3
"""Build and inspect the pinned VT snapshot API. Exit 3 means P0 is incomplete."""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "terminal/session-state/probe.c"
VENDOR = ROOT / "vendor/ghostty"
PIN = "81598385d608dda4946093d6d6a234d204a2f225"
MARK = "GT_P0_MUTATION_MARK"
CASES = ("continuation", "effects", "corruption", "pty", "resources")


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=300, **kwargs)


def build_engine():
    actual = subprocess.check_output(
        ["git", "-C", str(VENDOR), "rev-parse", "HEAD"], text=True).strip()
    if actual != PIN:
        raise RuntimeError(f"P0 probe requires pin {PIN}; found {actual}")
    run(["git", "-C", str(VENDOR), "diff", "--quiet", "HEAD", "--"])
    zig = os.environ.get("ZIG") or shutil.which("zig")
    if not zig:
        zig = str(ROOT / "tmp/zig/zig-aarch64-macos-0.16.0/zig")
    version = subprocess.check_output([zig, "version"], text=True).strip()
    if version != "0.16.0":
        raise RuntimeError(f"Zig 0.16.0 required; found {version}")
    env = dict(os.environ,
               ZIG_LOCAL_CACHE_DIR=str(ROOT / "tmp/zig-cache-vt"),
               ZIG_GLOBAL_CACHE_DIR=str(ROOT / "tmp/zig-global"))
    prefix = ROOT / "tmp/ghostty-vt"
    run([zig, "build", "-Demit-lib-vt=true", "-Demit-xcframework=false",
         "-Doptimize=ReleaseFast", "-p", str(prefix)], cwd=VENDOR, env=env)
    return prefix / "lib/libghostty-vt.a"


def compile_probe(source, binary, library):
    platform_flags = ["-lutil"] if sys.platform.startswith("linux") else []
    run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall",
         "-D_DEFAULT_SOURCE", "-Wextra", "-Werror", "-I", str(VENDOR / "include"),
         str(source), str(library), *platform_flags, "-o", str(binary)])


def probe(binary, case, expected=0):
    result = subprocess.run([str(binary), case], timeout=15, capture_output=True,
                            text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f"{case}: expected exit {expected}, got {result.returncode}")


def mutations(work, library, original):
    # Mutate a private copy. The repository source is never overwritten.
    # Each target fails while a different test under the SAME mutation passes.
    mutations = (
        ("continuation suffix lost", "feed(copy, cases[i].suffix);",
         "(void)copy;", "continuation", 1, "effects"),
        ("replica emits a duplicate response",
         "GHOSTTY_TERMINAL_OPT_WRITE_PTY,\n      NULL)",
         "GHOSTTY_TERMINAL_OPT_WRITE_PTY,\n      (const void *)on_reply)",
         "effects", 1, "continuation"),
        ("decoder corruption error swallowed",
         "result = ghostty_snapshot_decoder_decode(decoder, terminal);",
         "result = ghostty_snapshot_decoder_decode(decoder, terminal);\n"
         "  if (result == GHOSTTY_INVALID_VALUE) result = GHOSTTY_SUCCESS;",
         "corruption", 1, "effects"),
        ("source image fixture missing", "a=T,i=7,f=32,s=1,v=1,q=2;",
         "a=T,i=8,f=32,s=1,v=1,q=2;", "resources", 1, "effects"),
        ("PTY replica drops replayed delta",
         "ghostty_terminal_vt_write(replica, delta, sizeof delta);",
         "(void)delta;", "pty", 1, "effects"),
        ("PTY replica duplicates replies",
         'fd_write_all(fd, (const uint8_t *)"a", 1);',
         'enable_replies(replica, &replies);\n'
         '  fd_write_all(fd, (const uint8_t *)"a", 1);',
         "pty", 1, "corruption"),
        ("PTY resize not forwarded", "REQUIRE(ioctl(fd, TIOCSWINSZ, &size) == 0);",
         "(void)size;", "pty", 1, "effects"),
    )
    copy = work / "mutated.c"
    binary = work / "mutated"
    for label, old, new, target, code, control in mutations:
        if original.count(old) != 1:
            raise RuntimeError(f"mutation anchor is not unique: {label}")
        try:
            copy.write_text(original.replace(old, new + f" /* {MARK} */", 1))
            compile_probe(copy, binary, library)
            probe(binary, target, code)
            probe(binary, control)
            print(f"PASS mutation/{label}: target red, control green", flush=True)
        finally:
            copy.write_text(original)
            if copy.read_bytes() != SOURCE.read_bytes():
                raise RuntimeError("mutation rollback SHA input differs from source")
        compile_probe(copy, binary, library)
        probe(binary, target, 3 if target == "resources" else 0)


def interrupted(signum, frame):
    raise InterruptedError(f"signal {signum}; restoring private mutation copy")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=CASES)
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    if args.case and args.mutations:
        parser.error("--case and --mutations are mutually exclusive")
    original_bytes = SOURCE.read_bytes()
    original = original_bytes.decode()
    if MARK in original:
        print("Residual mutation marker: refusing to start", file=sys.stderr)
        return 2
    start_hash = hashlib.sha256(original_bytes).hexdigest()
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    (ROOT / "tmp").mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(ROOT / "tmp")
    try:
        library = build_engine()
        with tempfile.TemporaryDirectory(prefix="session-state-", dir=ROOT / "tmp") as tmp:
            work = Path(tmp)
            binary = work / "probe"
            compile_probe(SOURCE, binary, library)
            for case in (args.case,) if args.case else CASES:
                probe(binary, case, 3 if case == "resources" else 0)
            if args.mutations:
                mutations(work, library, original)
                print("PASS 7 mutations and rollback; P0 remains incomplete")
                return 0
            if args.case and args.case != "resources":
                return 0
            print("P0 INCOMPLETE: image resources lost; full surface import unverified")
            return 3
    finally:
        if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != start_hash:
            raise RuntimeError("repository source SHA256 changed during probe")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f"session-state probe failed: {error}", file=sys.stderr)
        sys.exit(1)
