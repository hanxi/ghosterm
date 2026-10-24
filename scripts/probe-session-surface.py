#!/usr/bin/env python3
"""Experimental macOS full-surface restoration. Exit 3 means P0 is incomplete."""
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
VENDOR = ROOT / "vendor/ghostty"
PATCHDIR = ROOT / "patches/libghostty/8159838"
PATCH = PATCHDIR / "experimental/0004-replica-surface.patch"
SOURCE = ROOT / "terminal/session-state/full-surface"
MARK = "GT_P0_SURFACE_MUTATION_MARK"
FRAMEWORKS = ("CoreFoundation Foundation AppKit Metal MetalKit QuartzCore CoreText "
              "CoreGraphics IOKit Carbon UniformTypeIdentifiers UserNotifications "
              "CoreServices Security IOSurface").split()


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=600, **kwargs)


def prepare(work):
    engine = work / "engine"
    # parents=True：干净检出（CI、新克隆）里 work 还不存在，CI 实测
    # FileNotFoundError: tmp/worker-surface/engine。仍不允许 engine 已存在——
    # 引擎副本必须是从 vendor 归档重建的，不接受半个工作树。
    engine.mkdir(parents=True)
    archive = work / "engine.tar"
    with archive.open("wb") as output:
        run(["git", "-C", str(VENDOR), "archive", "HEAD"], stdout=output)
    run(["tar", "-xf", str(archive), "-C", str(engine)])
    run(["git", "-C", str(engine), "init", "-q"])
    run([sys.executable, str(ROOT / "scripts/prepare-session-io.py"), str(engine)])
    for patch in (*sorted(PATCHDIR.glob("*.patch")), PATCH):
        run(["git", "-C", str(engine), "apply", "--check", str(patch)])
        run(["git", "-C", str(engine), "apply", str(patch)])
    return engine


def build(engine, work, *, cache_dir=None, log_path=None, process_run=None):
    zig = (os.environ.get("ZIG") or shutil.which("zig") or
           str(ROOT / "tmp/zig/zig-aarch64-macos-0.16.0/zig"))
    env = dict(os.environ,
               ZIG_LOCAL_CACHE_DIR=str(cache_dir if cache_dir is not None else ROOT / "tmp/zig-cache-p0-surface"),
               ZIG_GLOBAL_CACHE_DIR=str(ROOT / "tmp/zig-global"))
    log = Path(log_path) if log_path is not None else ROOT / "tmp/session-surface-build.log"
    command = [zig, "build", "-Demit-xcframework=true", "-Demit-macos-app=false",
               "-Dxcframework-target=native", "-Dsentry=false", "-p", str(work / "out")]
    # Zig writes helper executables (build runner, translate-c) into its cache
    # and runs them; hosts that gate execution kill such freshly created files.
    # They stay in the cache once written, so a killed build is retried. Any
    # other build failure is raised on the first attempt.
    for attempt in range(3):
        with log.open("w") as output:
            result = (process_run or subprocess.run)(command, cwd=engine, env=env, stdout=output,
                                                     stderr=subprocess.STDOUT, timeout=1800, check=False)
        if not result.returncode:
            break
        if attempt == 2 or "signal KILL" not in log.read_text(errors="replace"):
            raise subprocess.CalledProcessError(result.returncode, command)
        print(f"zig build helper killed by host; retrying ({attempt + 1}/2)", flush=True)
    library = engine / "macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a"
    if not library.is_file():
        raise RuntimeError("experimental build did not produce a native arm64 library")
    binary = work / "surface-probe"
    frameworks = [arg for name in FRAMEWORKS for arg in ("-framework", name)]
    run([*shlex.split(os.environ.get("CC", "cc")), "-fobjc-arc", "-Wall",
         "-Wextra", "-Werror", "-I", str(engine / "include"), str(SOURCE / "probe.m"),
         str(library), "-lc++", *frameworks, "-o", str(binary)])
    return binary


def probe(binary, fixtures, case="all", expected=0):
    result = subprocess.run([str(binary), str(fixtures), case], timeout=30,
                            capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f"surface/{case}: expected exit {expected}, got {result.returncode}")


def mutations(engine, work, fixtures):
    source = engine / "src/termio/Termio.zig"
    original = source.read_bytes()
    text = original.decode()
    # Reply re-enabled at its source, without depending on asynchronous mailbox
    # wakeups. The input-only control uses the same mutated full library.
    reply_handler = """.handler = handler: {
                var h: @import("../terminal/stream_terminal.zig").Handler = .init(&self.terminal);
                h.effects.write_pty = struct {
                    fn reply(h_: *@import("../terminal/stream_terminal.zig").Handler, bytes: []const u8) void {
                        const owner: *Termio = @alignCast(@fieldParentPtr("terminal", h_.terminal));
                        const host = owner.backend.host;
                        host.callbacks.write(host.callbacks.userdata, bytes.ptr, bytes.len);
                    }
                }.reply;
                break :handler h;
            },"""
    variants = (
        ("continuation lost", "self.replica_stream.?.nextSlice(continuation);",
         "self.replica_stream.?.nextSlice(&.{});", "state", "input"),
        ("replica replies again", ".handler = .init(&self.terminal),",
         reply_handler, "effects", "input"),
        ("user input discarded", "try self.backend.queueWrite(self.alloc, td, data, enabled);",
         "_ = enabled;", "input", "state"),
    )
    if MARK in text:
        raise RuntimeError("residual mutation marker; refusing to start")
    for label, old, new, target, control in variants:
        if text.count(old) != 1:
            raise RuntimeError(f"mutation anchor is not unique: {label}")
        try:
            source.write_text(text.replace(old, new + f" // {MARK}", 1))
            binary = build(engine, work)
            probe(binary, fixtures, target, 1)
            probe(binary, fixtures, control)
            print(f"PASS mutation/{label}: target red, control green", flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(original).digest():
                raise RuntimeError("experimental source rollback SHA256 mismatch")
        binary = build(engine, work)
        probe(binary, fixtures, target)
    probe(binary, fixtures)


def fixture_mutation(binary, work, fixtures, library):
    originals = {"fixture.c": (SOURCE / "fixture.c").read_bytes(),
                 "vim-fixture.h": (SOURCE / "vim-fixture.h").read_bytes(),
                 "tmux-fixture.h": (SOURCE / "tmux-fixture.h").read_bytes()}
    source = work / "fixture-mutated.c"
    header = work / "vim-fixture.h"
    producer = work / "fixture-producer-mutated"

    def generate(expected=0):
        run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-D_DEFAULT_SOURCE",
             "-Wall", "-Wextra", "-Werror", "-I", str(VENDOR / "include"),
             "-I", str(ROOT / "terminal/session-state"), str(source), str(library),
             "-o", str(producer)])
        result = subprocess.run([str(producer), str(fixtures)], timeout=30,
                                capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode != expected:
            raise RuntimeError(f"fixture generation: expected exit {expected}, got {result.returncode}")

    variants = (
        ("real PTY delta lost", "fixture.c",
         'save_bytes(dir, "pty.delta", delta, sizeof delta);',
         'save_bytes(dir, "pty.delta", delta, 0);', "pty", False),
        ("vim delta lost", "vim-fixture.h",
         'save_bytes(dir,"vim.delta",delta.data,delta.len);',
         'save_bytes(dir,"vim.delta",delta.data,0);', "vim", False),
        ("vim edit missing", "vim-fixture.h", '"iGHOSTERM_VIM_REAL\\x1b"',
         '"iGHOSTERM_VIM_WRONG\\x1b"', "vim", True),
        ("tmux delta lost", "tmux-fixture.h",
         'save_bytes(dir,"tmux.delta",delta.data,delta.len);',
         'save_bytes(dir,"tmux.delta",delta.data,0);', "tmux", False),
        ("tmux server not cleaned", "tmux-fixture.h", "close(fd);cleanup_tmux();",
         "close(fd);", "tmux", True),
    )
    paths = {"fixture.c": source, "vim-fixture.h": header,
             "tmux-fixture.h": work / "tmux-fixture.h"}
    for label, name, old, new, target, generator_fails in variants:
        text = originals[name].decode()
        if text.count(old) != 1:
            raise RuntimeError(f"fixture mutation anchor is not unique: {label}")
        for key, data in originals.items():
            paths[key].write_bytes(data)
        try:
            paths[name].write_text(text.replace(old, new + f" /* {MARK} */", 1))
            generate(1 if generator_fails else 0)
            if not generator_fails:
                probe(binary, fixtures, target, 1)
            probe(binary, fixtures, "input")
            print(f"PASS mutation/{label}: target red, control green", flush=True)
        finally:
            for key, data in originals.items():
                paths[key].write_bytes(data)
                if hashlib.sha256(paths[key].read_bytes()).digest() != hashlib.sha256(data).digest():
                    raise RuntimeError(f"fixture rollback SHA256 mismatch: {key}")
        generate()
        probe(binary, fixtures, target)


def interrupted(signum, frame):
    raise InterruptedError(f"signal {signum}; restoring private experimental tree")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=("state", "effects", "input", "pty", "vim", "tmux"))
    parser.add_argument("--mutations", action="store_true")
    parser.add_argument("--fixture-mutations", action="store_true")
    args = parser.parse_args()
    if sum(bool(x) for x in (args.case, args.mutations, args.fixture_mutations)) > 1:
        parser.error("--case, --mutations and --fixture-mutations are mutually exclusive")
    if sys.platform != "darwin" or os.uname().machine != "arm64":
        raise RuntimeError("this experimental full Metal probe requires macOS arm64")
    tmux = os.environ.get("GHOSTERM_P0_TMUX") or shutil.which("tmux")
    if not tmux:
        raise RuntimeError("real tmux fixture requires tmux (or GHOSTERM_P0_TMUX)")
    os.environ["GHOSTERM_P0_TMUX"] = str(Path(tmux).resolve())
    files = [PATCH, SOURCE / "fixture.c", SOURCE / "vim-fixture.h", SOURCE / "tmux-fixture.h", SOURCE / "probe.m",
             ROOT / "terminal/session-state/probe.c"]
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):
        return 2
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    (ROOT / "tmp").mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(ROOT / "tmp")
    try:
        # Shared VT runner verifies the exact pin, clean vendor and Zig 0.16.0,
        # then incrementally rebuilds the fixture producer's original VT library.
        shared = runpy.run_path(str(ROOT / "scripts/probe-session-state.py"))
        library = shared["build_engine"]()
        with tempfile.TemporaryDirectory(prefix="session-surface-", dir=ROOT / "tmp") as tmp:
            work = Path(tmp)
            producer = work / "fixture-producer"
            run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-D_DEFAULT_SOURCE",
                 "-Wall", "-Wextra", "-Werror", "-I", str(VENDOR / "include"),
                 "-I", str(ROOT / "terminal/session-state"), str(SOURCE / "fixture.c"),
                 str(library), "-o", str(producer)])
            fixtures = work / "fixtures"
            fixtures.mkdir()
            run([str(producer), str(fixtures)])
            engine = prepare(work)
            binary = build(engine, work)
            probe(binary, fixtures, args.case or "all")
            if args.mutations:
                mutations(engine, work, fixtures)
                fixture_mutation(binary, work, fixtures, library)
                print("PASS 8 surface mutations and rollback; P0 remains incomplete")
                return 0
            if args.fixture_mutations:
                fixture_mutation(binary, work, fixtures, library)
                print("PASS 5 fixture mutations and rollback; P0 remains incomplete")
                return 0
            print("P0 INCOMPLETE: image resources, decoded-memory budget, replica events and live barrier remain")
            return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError(f"repository source SHA256 changed: {path}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f"session-surface probe failed: {error}; see tmp/session-surface-build.log",
              file=sys.stderr)
        sys.exit(1)
