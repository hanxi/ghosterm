#!/usr/bin/env python3
"""Build full libghostty with the three ordinary host patches in isolated sources."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PIN = "81598385d608dda4946093d6d6a234d204a2f225"
PATCHES = tuple(ROOT / "patches/libghostty/8159838" / name for name in (
    "0001-host-io-backend.patch", "0002-program-status.patch", "0003-semantic-prompt.patch"))


def run(command, **kwargs):
    process = subprocess.Popen([str(x) for x in command], start_new_session=True, **kwargs)
    try:
        code = process.wait(timeout=1800)
        if code:
            raise RuntimeError(f"command failed ({code}): {command[0]}")
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()


def validate(sdk):
    library = sdk / "macos-arm64/libghostty-internal.a"
    header = sdk / "macos-arm64/Headers/ghostty.h"
    symbols = subprocess.check_output(["nm", "-g", "--defined-only", str(library)],
                                      text=True, stderr=subprocess.DEVNULL)
    names = {line.split()[-1].removeprefix("_") for line in symbols.splitlines()
             if len(line.split()) >= 3 and line.split()[-2] == "T"}
    required = {"ghostty_surface_process_output", "ghostty_surface_key",
                "ghostty_surface_new", "ghostty_surface_set_color_scheme"}
    if not required <= names:
        raise RuntimeError("ordinary terminal engine entry points missing")
    if any(name.startswith("ghostty_p0_") for name in names) or "ghostty_p0_" in header.read_text():
        raise RuntimeError("retired session engine API remains in SDK")
    if any(word not in header.read_text() for word in ("host_write", "host_program_status", "host_semantic_prompt")):
        raise RuntimeError("ordinary host callbacks missing from SDK header")


def main():
    if sys.platform != "darwin" or os.uname().machine != "arm64":
        raise RuntimeError("Apple Silicon macOS builder required")
    (ROOT / "tmp").mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(ROOT / "tmp")
    os.environ.setdefault("DEVELOPER_DIR", "/Applications/Xcode.app/Contents/Developer")
    vendor = ROOT / "vendor/ghostty"
    actual = subprocess.check_output(["git", "-C", str(vendor), "rev-parse", "HEAD"], text=True).strip()
    if actual != PIN:
        raise RuntimeError("terminal engine pin mismatch")
    run(["git", "-C", vendor, "diff", "--quiet", "HEAD", "--"])
    zig = os.environ.get("ZIG") or shutil.which("zig") or str(ROOT / "tmp/zig/zig-aarch64-macos-0.16.0/zig")
    if subprocess.check_output([zig, "version"], text=True).strip() != "0.16.0":
        raise RuntimeError("Zig 0.16.0 required")
    work = ROOT / "tmp/terminal-engine"
    work.mkdir(exist_ok=True)
    engine = work / "source"
    if engine.is_symlink():
        raise RuntimeError("isolated engine must not be a symlink")
    if engine.exists():
        shutil.rmtree(engine)
    engine.mkdir()
    with (work / "source.tar").open("wb") as output:
        run(["git", "-C", vendor, "archive", "HEAD"], stdout=output)
    run(["tar", "-xf", work / "source.tar", "-C", engine])
    run(["git", "-C", engine, "init", "-q"])
    for patch in PATCHES:
        run(["git", "-C", engine, "apply", "--check", patch])
        run(["git", "-C", engine, "apply", patch])
    environment = dict(os.environ, ZIG_LOCAL_CACHE_DIR=str(ROOT / "tmp/zig-cache-terminal"),
                       ZIG_GLOBAL_CACHE_DIR=str(ROOT / "tmp/zig-global"))
    with (work / "build.log").open("w") as log:
        run([zig, "build", "-Demit-xcframework=true", "-Demit-macos-app=false",
             "-Dxcframework-target=native", "-Dsentry=false", "-Doptimize=ReleaseFast",
             "-p", work / "out"], cwd=engine, env=environment, stdout=log, stderr=subprocess.STDOUT)
    sdk = engine / "macos/GhosttyKit.xcframework"
    validate(sdk)
    headers = sdk / "macos-arm64/Headers"
    (headers / "module.modulemap").write_text('module GhosttyKit { header "ghostty.h" export * }\n')
    metadata = {"engine": PIN, "profile": "host-io", "patches": {
        str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in PATCHES},
        "archive_sha256": hashlib.sha256((sdk / "macos-arm64/libghostty-internal.a").read_bytes()).hexdigest()}
    (sdk / "terminal-engine.json").write_text(json.dumps(metadata, indent=2) + "\n")
    destination = ROOT / "build/GhosttyKit.xcframework"
    destination.parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="terminal-sdk-", dir=ROOT / "tmp") as temporary:
        staging = Path(temporary) / "GhosttyKit.xcframework"
        shutil.copytree(sdk, staging, symlinks=True)
        backup = Path(temporary) / "previous.xcframework"
        if destination.is_symlink():
            raise RuntimeError("SDK destination must not be a symlink")
        existed = destination.exists()
        if existed:
            destination.rename(backup)
        try:
            staging.rename(destination)
        except BaseException:
            if existed:
                backup.rename(destination)
            raise
    run(["git", "-C", vendor, "diff", "--quiet", "HEAD", "--"])
    print("PASS full libghostty host engine: unchanged vendor pin, 3 patches, no session/replica API", flush=True)
    return 0


if __name__ == "__main__":
    def interrupted(number, frame):
        raise KeyboardInterrupt(f"terminal engine interrupted: {number}")
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    sys.exit(main())
