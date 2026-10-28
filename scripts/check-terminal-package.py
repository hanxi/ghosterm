#!/usr/bin/env python3
"""Verify the ordinary engine receipt and absence of retired session payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def check(app, sdk):
    engine = runpy.run_path(str(ROOT / "scripts/build-terminal-engine.py"))
    engine["validate"](sdk)
    receipt = json.loads((sdk / "terminal-engine.json").read_text())
    expected = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in engine["PATCHES"]}
    if receipt.get("profile") != "host-io" or receipt.get("engine") != engine["PIN"] or receipt.get("patches") != expected:
        raise RuntimeError("ordinary engine receipt does not match current pin/patches")
    archive = sdk / "macos-arm64/libghostty-internal.a"
    if receipt.get("archive_sha256") != hashlib.sha256(archive.read_bytes()).hexdigest():
        raise RuntimeError("ordinary engine archive does not match receipt")
    retired_names = {"ghosterm-sessiond", "ghosterm-session-proxy", "session-engine.json"}
    for path in app.rglob("*"):
        if path.name in retired_names or path.name == "sessiond" or path.suffix == ".gtvc":
            raise RuntimeError("retired session payload remains in app: " + str(path.relative_to(app)))
    info = app / "Contents/Resources/BUILD-INFO.txt"
    if info.is_file() and any(x in info.read_text() for x in ("会话运行环境:", "Linux 会话安装包:", "Linux glibc 下限:")):
        raise RuntimeError("retired session metadata remains in BUILD-INFO")
    executables = [p for p in (app / "Contents/MacOS").iterdir() if p.is_file()]
    if not executables:
        raise RuntimeError("app executable missing")
    for executable in executables:
        symbols = subprocess.check_output(["nm", "-g", "--defined-only", str(executable)],
                                          stderr=subprocess.DEVNULL, text=True)
        if any(name in symbols for name in ("_gt_session_", "_ghostty_p0_")):
            raise RuntimeError("retired session symbols remain in app")
    print("TERMINAL PACKAGE PASS: current ordinary engine and no retired session runtime")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--sdk", type=Path, default=ROOT / "build/GhosttyKit.xcframework")
    args = parser.parse_args()
    check(args.app.resolve(), args.sdk.resolve())


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("terminal package failed: " + str(error), file=sys.stderr)
        sys.exit(1)
