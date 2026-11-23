#!/usr/bin/env python3
"""Verify a real pinned Codex app-server through the Lua package, without a model turn."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def wait_for_thread(process):
    pending = b""
    sent = False
    deadline = time.monotonic() + 30
    fd = process.stdout.fileno()
    while time.monotonic() < deadline:
        if not select.select([fd], [], [], max(0, deadline - time.monotonic()))[0]:
            break
        data = os.read(fd, 4096)
        if not data:
            break
        pending += data
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            reply = json.loads(line)
            if reply.get("ready") and not sent:
                process.stdin.write(b"codex.thread.start\n{}\n")
                process.stdin.flush()
                sent = True
            if "result" in reply:
                result = reply["result"]
                if sent and result["kind"] == "success" and result["value"]["threadID"]:
                    return
                raise RuntimeError("Codex thread/start failed: " + json.dumps(result))
    raise RuntimeError("No confirmed Codex thread/start response")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("peer", "gterm", "package", "codex"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    peer, gterm, package, codex = (
        path.resolve(strict=True) for path in (args.peer, args.gterm, args.package, args.codex)
    )
    lock = json.loads((package / "protocol/app-server.lock.json").read_bytes())
    expected = lock["codexCliVersion"]
    digest = hashlib.sha256((package / "integrity.json").read_bytes()).hexdigest()
    with codex.open("rb") as backend:
        backend_digest = hashlib.file_digest(backend, "sha256").hexdigest()
    workspace_tmp = ROOT / "tmp"
    workspace_tmp.mkdir(exist_ok=True)
    parent = Path(os.environ.get("TMPDIR", str(workspace_tmp))).resolve()
    if not parent.is_relative_to(workspace_tmp) or not parent.is_dir():
        parent = workspace_tmp
    with tempfile.TemporaryDirectory(prefix="agent-", dir=parent) as temporary:
        root = Path(temporary)
        home, workspace = root / "home", root / "workspace"
        (home / "codex").mkdir(parents=True)
        (home / "config").mkdir()
        workspace.mkdir()
        env = dict(PATH="/usr/bin:/bin", HOME=str(home), CODEX_HOME=str(home / "codex"),
                   XDG_CONFIG_HOME=str(home / "config"), TMPDIR=temporary, TMP=temporary, TEMP=temporary)
        result = subprocess.run([codex, "--version"], check=True, env=env, capture_output=True, text=True, timeout=5)
        if result.stdout.strip() != expected:
            raise RuntimeError(f"Expected {expected}, got {result.stdout.strip()}")
        command = [peer, gterm, package, digest, workspace, codex, expected, backend_digest]
        with subprocess.Popen(command, cwd=workspace, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env) as child:
            try:
                wait_for_thread(child)
                child.stdin.close()
                if child.wait(timeout=10):
                    raise RuntimeError("Codex peer did not close cleanly")
            finally:
                if child.poll() is None:
                    child.terminate()
                    child.wait(timeout=10)
    print("REAL CODEX LUA HANDSHAKE PASS: app-server initialize and ephemeral thread/start")


if __name__ == "__main__":
    main()
