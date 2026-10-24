#!/usr/bin/env python3
"""Build experimental native resource hooks; this is not the production ABI."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "patches/libghostty/8159838/experimental/0005-resource-io-hooks.patch"
PNG_PATCH = ROOT / "patches/libghostty/8159838/experimental/0006-builtin-png.patch"
SOURCE = ROOT / "terminal/session-state/resource-hooks-probe.c"
MARK = "GT_P0_RESOURCE_HOOK_MUTATION_MARK"
CASES = ("success", "source-failure", "observe-reject", "resolve-mismatch", "duplicate", "file")
PNG_CASES = ("png", "png-invalid", "png-oom")


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=300, **kwargs)


def prepare(work, builtin_png=False):
    engine = work / "engine"
    engine.mkdir()
    archive = work / "source.tar"
    with archive.open("wb") as output:
        run(["git", "-C", str(ROOT / "vendor/ghostty"), "archive", "HEAD"], stdout=output)
    run(["tar", "-xf", str(archive), "-C", str(engine)])
    run(["git", "-C", str(engine), "init", "-q"])
    run(["git", "-C", str(engine), "apply", "--check", str(PATCH)])
    run(["git", "-C", str(engine), "apply", str(PATCH)])
    if builtin_png:
        run(["git", "-C", str(engine), "apply", "--check", str(PNG_PATCH)])
        run(["git", "-C", str(engine), "apply", str(PNG_PATCH)])
    return engine


def build(engine, work, builtin_png=False, *, log_path=None):
    zig = (os.environ.get("ZIG") or shutil.which("zig") or
           str(ROOT / "tmp/zig/zig-aarch64-macos-0.16.0/zig"))
    env = dict(os.environ,
               ZIG_LOCAL_CACHE_DIR=str(ROOT / "tmp/zig-cache-p0-resource"),
               ZIG_GLOBAL_CACHE_DIR=str(ROOT / "tmp/zig-global"))
    log = Path(log_path) if log_path is not None else ROOT / "tmp/session-resource-hooks-build.log"
    with log.open("w") as output:
        run([zig, "build", "-Demit-lib-vt=true", "-Demit-xcframework=false",
             "-Doptimize=ReleaseFast", "-p", str(work / "out")], cwd=engine, env=env,
            stdout=output, stderr=subprocess.STDOUT)
    binary = work / "probe"
    extra = ["-lutil"] if sys.platform.startswith("linux") else []
    flags = ["-DGHOSTERM_P0_BUILTIN_PNG"] if builtin_png else []
    run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-D_DEFAULT_SOURCE",
         "-Wall", "-Wextra", "-Werror", *flags, "-I", str(engine / "include"), "-I",
         str(ROOT / "terminal/session-state"), str(SOURCE),
         str(work / "out/lib/libghostty-vt.a"), *extra, "-o", str(binary)])
    return binary


def probe(binary, work, case, expected=0):
    result = subprocess.run([str(binary), case, str(work)], capture_output=True,
                            text=True, timeout=15)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f"resource-hooks/{case}: expected exit {expected}, got {result.returncode}")


def mutations(engine, work, builtin_png=False):
    source = engine / "src/terminal/kitty/graphics_image.zig"
    original = source.read_bytes()
    text = original.decode()
    variants = (
        ("successful resource event dropped",
         "if (!observe(hooks.userdata, medium, cmd.data.ptr, cmd.data.len, 0, result.data.items.ptr, result.data.items.len)) return hooks.fail();",
         "if (false) return hooks.fail();", "success", "source-failure"),
        ("resolved resource bytes dropped",
         "result.addData(alloc, if (bytes) |p| p[0..len] else &.{})",
         "result.addData(alloc, &.{})", "success", "source-failure"),
        ("callback rejection hidden", "if (self.failed) |value| value.* = true;",
         "if (self.failed) |value| { _ = value; }", "observe-reject", "source-failure"),
        ("source failure event dropped",
         "if (!observe(hooks.userdata, medium, cmd.data.ptr, cmd.data.len, 1, null, 0)) return hooks.fail();",
         "if (false) return hooks.fail();", "source-failure", "success"),
    )
    if MARK in text:
        raise RuntimeError("residual resource hook mutation marker")
    digest = hashlib.sha256(original).digest()
    for label, old, new, target, control in variants:
        if text.count(old) != 1:
            raise RuntimeError(f"mutation anchor is not unique: {label}")
        try:
            source.write_text(text.replace(old, new, 1) + f"\n// {MARK}\n")
            binary = build(engine, work, builtin_png)
            probe(binary, work, target, 1)
            probe(binary, work, control)
            print(f"PASS mutation/{label}: target red, control green", flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError("resource hook rollback SHA256 mismatch")
        binary = build(engine, work, builtin_png)
        probe(binary, work, target)
    for case in CASES:
        probe(binary, work, case)


def png_mutations(engine, work):
    source = engine / "src/terminal/c/sys.zig"
    original = source.read_bytes()
    text = original.decode()
    digest = hashlib.sha256(original).digest()
    variants = (
        ("real PNG decoding disabled",
         "terminal_sys.decodePngWuffs(lib.alloc.default(allocator), ptr[0..len])",
         "terminal_sys.decodePngWuffs(lib.alloc.default(allocator), ptr[0..@min(len, 0)])",
         "png", "png-invalid"),
        ("failed PNG leaves stale output", "out.* = std.mem.zeroes(Image);",
         "out.width = 0;", "png-invalid", "png"),
        ("PNG bypasses caller allocator",
         "const image = terminal_sys.decodePngWuffs(lib.alloc.default(allocator), ptr[0..len])",
         "_ = allocator;\n    const image = terminal_sys.decodePngWuffs(lib.alloc.default(null), ptr[0..len])",
         "png-oom", "png"),
    )
    for label, old, new, target, control in variants:
        if text.count(old) != 1:
            raise RuntimeError(f"PNG mutation anchor is not unique: {label}")
        try:
            source.write_text(text.replace(old, new, 1) + f"\n// {MARK}\n")
            binary = build(engine, work, True)
            probe(binary, work, target, 1)
            probe(binary, work, control)
            print(f"PASS mutation/{label}: target red, control green", flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError("PNG rollback SHA256 mismatch")
        binary = build(engine, work, True)
        probe(binary, work, target)
    for case in PNG_CASES:
        probe(binary, work, case)


def interrupted(signum, frame):
    raise InterruptedError(f"signal {signum}; restoring private resource hook source")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=CASES + PNG_CASES)
    parser.add_argument("--mutations", action="store_true")
    parser.add_argument("--builtin-png", action="store_true")
    parser.add_argument("--png-mutations", action="store_true")
    args = parser.parse_args()
    if sum(bool(x) for x in (args.case, args.mutations, args.png_mutations)) > 1:
        parser.error("--case, --mutations and --png-mutations are mutually exclusive")
    if args.png_mutations:
        args.builtin_png = True
    if args.case in PNG_CASES and not args.builtin_png:
        parser.error("PNG cases require --builtin-png")
    files = [PATCH, SOURCE, ROOT / "terminal/session-state/resource-io-probe.c",
             ROOT / "terminal/session-state/probe.c"]
    if args.builtin_png:
        files.append(PNG_PATCH)
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):
        return 2
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    (ROOT / "tmp").mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(ROOT / "tmp")
    try:
        shared = runpy.run_path(str(ROOT / "scripts/probe-session-state.py"))
        shared["build_engine"]()  # Verify pristine pin, Zig and original VT build.
        with tempfile.TemporaryDirectory(prefix="session-resource-hooks-", dir=ROOT / "tmp") as tmp:
            work = Path(tmp)
            engine = prepare(work, args.builtin_png)
            binary = build(engine, work, args.builtin_png)
            cases = CASES + (PNG_CASES if args.builtin_png else ())
            for case in (args.case,) if args.case else cases:
                probe(binary, work, case)
            probe(binary, work, "inline")
            probe(binary, work, "temporary", 3)  # No hooks preserves original behavior.
            if args.mutations:
                mutations(engine, work, args.builtin_png)
                if args.builtin_png:
                    png_mutations(engine, work)
                print(f"PASS {7 if args.builtin_png else 4} native resource/PNG mutations and rollback; P0 remains incomplete")
                return 0
            if args.png_mutations:
                png_mutations(engine, work)
                print("PASS 3 PNG mutations and rollback; P0 remains incomplete")
                return 0
        print("P0 INCOMPLETE: experimental hooks lack snapshot resources, full surface wiring and ordered IPC")
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError(f"repository source SHA256 changed: {path}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f"session-resource-hooks probe failed: {error}; see tmp/session-resource-hooks-build.log",
              file=sys.stderr)
        sys.exit(1)
