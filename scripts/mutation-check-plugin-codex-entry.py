#!/usr/bin/env python3
"""Ensure the official Codex Executor never spawns without live process grants."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "plugins/official/ai-codex/protocol/process.ts"
OLD = '''if (context.side !== "executor" ||
      context.capabilities.query("process.spawn") !== "allowed" ||
      context.capabilities.query("process.pipes") !== "allowed") {'''
MUTANT = "if (false) {"


def digest() -> str:
    return hashlib.sha256(SOURCE.read_bytes()).hexdigest()


def run_entry(*, expect_pass: bool) -> str:
    result = subprocess.run(
        [sys.executable, "-B", "scripts/test-plugin-codex-entry.py", "--fetch"],
        cwd=ROOT, env={**os.environ, "TMPDIR": str(ROOT / "tmp")},
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300,
    )
    output = result.stdout
    if expect_pass:
        if result.returncode != 0 or "CODEX EXECUTOR ENTRY TEST PASS" not in output:
            raise RuntimeError("Codex Executor entry expected green:\n" + output)
    elif (result.returncode == 0 or "AssertionError [ERR_ASSERTION]" not in output or
          "spawn attempted without Executor process grants" not in output):
        raise RuntimeError("missing process grant guard did not cause a normal assertion red:\n" + output)
    return output


def run_control() -> None:
    result = subprocess.run(
        ["node", "--experimental-strip-types", "--test",
         "--test-name-pattern=turn/start emits the stable thread input shape",
         "scripts/test-plugin-codex-protocol.mjs"],
        cwd=ROOT, env={**os.environ, "TMPDIR": str(ROOT / "tmp")},
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30,
    )
    if result.returncode != 0 or "ℹ pass 1" not in result.stdout:
        raise RuntimeError("independent Codex protocol control failed:\n" + result.stdout)


def main() -> None:
    original = SOURCE.read_bytes()
    before = hashlib.sha256(original).hexdigest()
    if OLD.encode() not in original:
        raise RuntimeError("process grant source anchor changed")
    run_entry(expect_pass=True)
    try:
        SOURCE.write_bytes(original.replace(OLD.encode(), MUTANT.encode(), 1))
        run_entry(expect_pass=False)
        run_control()
    finally:
        SOURCE.write_bytes(original)
    if digest() != before:
        raise RuntimeError("restored process source digest mismatch")
    run_entry(expect_pass=True)
    print("Codex Executor process grant: target assertion red / independent protocol control green / restored / SHA256")


if __name__ == "__main__":
    try:
        main()
    except BaseException as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
