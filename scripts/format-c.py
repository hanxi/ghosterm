#!/usr/bin/env python3
"""Check project-owned, tracked C files; writing requires explicit --write."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
VERSION = "19.1.7"
ROOTS = {"core", "terminal", "apple", "android", "windows", "linux", "harmony", "web", "cli"}
EXCLUDED = {"vendor", "build", "build-debug", "build-release", "generated", "fixtures",
            ".gradle", ".cxx", "node_modules"}
# These checked-in files are owned and byte-verified by their generators.
GENERATED = {
    "core/src/extension/package-validator-code.inc",
    "core/src/extension/sdk-code.inc",
    "core/src/extension/sdk-identity.h",
    "core/src/extension/wire-schema.inc",
}


def eligible(path):
    parts = path.parts
    scope = parts[0] in ROOTS or path.as_posix().startswith(("scripts/probes/", "scripts/lib/"))
    return (scope and path.as_posix() not in GENERATED and path.suffix in {".c", ".h", ".inc"}
            and not any(part in EXCLUDED or part.startswith("cmake-build-") for part in parts))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--check", action="store_true", help="check without writing (default)")
    modes.add_argument("--write", action="store_true", help="format selected files in place")
    modes.add_argument("--list", action="store_true", help="list files without invoking clang-format")
    parser.add_argument("paths", nargs="*", help="repository-relative tracked files/directories; default: all")
    args = parser.parse_args()

    tracked = subprocess.check_output(["git", "ls-files", "-z", "--cached"], cwd=ROOT)
    candidates = []
    for name in sorted(set(os.fsdecode(p) for p in tracked.split(b"\0") if p)):
        path = Path(name)
        absolute = ROOT / path
        # Never follow symlinks, including symlinked parent directories.
        if (eligible(path) and absolute.is_file()
                and not any(p.is_symlink() for p in [absolute, *absolute.parents])
                and absolute.resolve().is_relative_to(ROOT)):
            candidates.append(path)

    selected = set()
    for name in args.paths:
        path = Path(name)
        if path.is_absolute() or ".." in path.parts:
            parser.error(f"use repository-relative paths without '..': {name}")
        matches = [p for p in candidates if p == path or path in p.parents]
        if not matches:
            parser.error(f"no eligible tracked C files: {name}")
        selected.update(matches)
    files = sorted(selected) if args.paths else candidates
    if args.list:
        for path in files:
            print(path.as_posix())
        return 0
    if not files:
        parser.error("no eligible tracked C files")

    binary = os.environ.get("CLANG_FORMAT") or shutil.which("clang-format-19") or shutil.which("clang-format")
    if not binary:
        parser.error(f"install clang-format {VERSION}; set CLANG_FORMAT to its executable path")
    version = subprocess.check_output([binary, "--version"], text=True)
    match = re.search(r"\bversion\s+(\d+\.\d+\.\d+)\b", version)
    if not match or match.group(1) != VERSION:
        parser.error(f"clang-format {VERSION} required; found: {version.strip()}")

    flags = ["-i"] if args.write else ["--dry-run", "--Werror"]
    failed = False
    for path in files:
        result = subprocess.run([binary, f"--style=file:{ROOT / '.clang-format'}",
                                 "--fallback-style=none", *flags, str(path)], cwd=ROOT)
        failed = failed or result.returncode != 0
    print(f"{'write' if args.write else 'check'}: {len(files)} files", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"format-c: {error}", file=sys.stderr)
        sys.exit(2)
