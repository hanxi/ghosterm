#!/usr/bin/env python3
"""Verify Codex thread isolation and interrupt acknowledgement semantics."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = "conversation fails closed on turn events from a different thread"
INTERRUPT_TARGET = "conversation interrupts only its active turn and reports closed work as unknown"
CONTROL = "preserves approval ids, exact decision classes, and single response"
TURN_GUARD = "threadId !== this.threadID || !isRecord(turn)"
THREAD_GUARD = "this.threadID && notification.params.threadId !== this.threadID"


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
    work = Path(tempfile.mkdtemp(prefix="codex-conversation-mutation-", dir=ROOT / "tmp"))
    try:
        for relative in ("plugins/official/ai-codex", "packages/plugin-sdk/portable"):
            shutil.copytree(ROOT / relative, work / relative)
        (work / "scripts").mkdir()
        shutil.copyfile(ROOT / "scripts/test-plugin-codex-protocol.mjs",
                        work / "scripts/test-plugin-codex-protocol.mjs")
        (work / "tmp").mkdir()
        target = work / "plugins/official/ai-codex/protocol/conversation.ts"
        original = target.read_bytes()
        before = hashlib.sha256(original).hexdigest()
        interrupt_call = "await this.client.turnInterrupt({ threadId: this.threadID, turnId: this.activeTurnID });"
        cases = (
            (TARGET, ((TURN_GUARD, "!isRecord(turn)"), (THREAD_GUARD, "false"))),
            (INTERRUPT_TARGET, ((interrupt_call, interrupt_call + '\n    this.stateValue = "ready";'),)),
        )
        for name, replacements in cases:
            mutated = original.decode()
            for before_text, after_text in replacements:
                if mutated.count(before_text) != 1:
                    raise RuntimeError(f"Expected one source anchor for {name}")
                mutated = mutated.replace(before_text, after_text, 1)
            run(work, name, expect_pass=True)
            try:
                target.write_text(mutated)
                run(work, name, expect_pass=False)
                run(work, CONTROL, expect_pass=True)
            finally:
                target.write_bytes(original)
            if digest(target) != before:
                raise RuntimeError("restored source digest mismatch")
            run(work, name, expect_pass=True)
            print(f"CODEX CONVERSATION MUTATION PASS: {name}; target red / independent control green / restored / SHA256")
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
