#!/usr/bin/env python3
"""Verify Codex stop reports the Host child exit after reap."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = "runs the production process helper against the pinned real server"
CONTROL = "PluginContext process integration rejects non-Executor or missing live grants"
OLD = "        exit = child.poll();\n"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(root: Path, name: str, *, expect_pass: bool) -> str:
    result = subprocess.run(
        ["node", "--experimental-strip-types", "--test", f"--test-name-pattern={name}",
         "scripts/test-plugin-codex-protocol.mjs"],
        cwd=root,
        env={**os.environ, "TMPDIR": str(root / "tmp")},
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    output = result.stdout + result.stderr
    if expect_pass:
        if result.returncode != 0 or "ℹ pass 1" not in output:
            raise RuntimeError(f"{name}: expected one passing assertion\n{output}")
    elif result.returncode == 0 or "ℹ fail 1" not in output or "AssertionError" not in output:
        raise RuntimeError(f"{name}: expected a normal assertion red\n{output}")
    return output


def main() -> None:
    work = Path(tempfile.mkdtemp(prefix="codex-reap-mutation-", dir=ROOT / "tmp"))
    try:
        for relative in ("plugins/official/ai-codex", "packages/plugin-sdk/portable"):
            shutil.copytree(ROOT / relative, work / relative)
        (work / "scripts").mkdir()
        shutil.copyfile(ROOT / "scripts/test-plugin-codex-protocol.mjs",
                        work / "scripts/test-plugin-codex-protocol.mjs")
        (work / "tmp").mkdir()
        target = work / "plugins/official/ai-codex/protocol/process.ts"
        original = target.read_bytes()
        before = hashlib.sha256(original).hexdigest()
        if OLD.encode() not in original:
            raise RuntimeError("post-reap exit poll source anchor changed")

        run(work, TARGET, expect_pass=True)
        try:
            target.write_bytes(original.replace(OLD.encode(), b"", 1))
            run(work, TARGET, expect_pass=False)
            run(work, CONTROL, expect_pass=True)
        finally:
            target.write_bytes(original)
        if digest(target) != before:
            raise RuntimeError("restored source digest mismatch")
        run(work, TARGET, expect_pass=True)
        print("CODEX REAP MUTATION PASS: target red / independent control green / restored / SHA256")
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
