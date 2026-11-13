#!/usr/bin/env python3
"""Verify Host and app-server version locks both reject mismatches."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = "stops and reaps a Host child whose verified version differs from the protocol lock"
SERVER_TARGET = "initialize response version mismatch stops and reaps its child"
CONTROL = "PluginContext process grant launches Codex through the bounded stdio adapter"
OLD = 'if (child.verifiedVersion !== CODEX_CLI_VERSION) {'
MUTANT = 'if (false && child.verifiedVersion !== CODEX_CLI_VERSION) {'
SERVER_OLD = 'if (reportedVersion !== expectedVersion) {'
SERVER_MUTANT = 'if (false && reportedVersion !== expectedVersion) {'


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
    elif (result.returncode == 0 or "ℹ fail 1" not in output or
          "AssertionError [ERR_ASSERTION]: Missing expected rejection" not in output):
        raise RuntimeError(f"{name}: expected a normal assertion red\n{output}")
    return output


def main() -> None:
    work = Path(tempfile.mkdtemp(prefix="codex-version-mutation-", dir=ROOT / "tmp"))
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
        for name, old, mutant in ((TARGET, OLD, MUTANT),
                                  (SERVER_TARGET, SERVER_OLD, SERVER_MUTANT)):
            if old.encode() not in original:
                raise RuntimeError(f"version guard source anchor changed: {name}")
            run(work, name, expect_pass=True)
            try:
                target.write_bytes(original.replace(old.encode(), mutant.encode(), 1))
                run(work, name, expect_pass=False)
                run(work, CONTROL, expect_pass=True)
            finally:
                target.write_bytes(original)
            if digest(target) != before:
                raise RuntimeError("restored source digest mismatch")
            run(work, name, expect_pass=True)
            print(f"{name}: target red / independent control green / restored / SHA256", flush=True)
        print("CODEX VERSION MUTATION PASS")
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
