#!/usr/bin/env python3
"""Exercise the pinned Codex app-server handshake without starting a turn."""
import argparse
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "plugins/official/ai-codex/protocol/app-server.lock.json"


def fail(message):
    raise SystemExit(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codex", default="codex", help="pinned official Codex CLI executable")
    parser.add_argument("--timeout", type=float, default=15, help="per-message timeout in seconds")
    args = parser.parse_args()
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    executable = shutil.which(args.codex)
    if not executable:
        fail(f"Codex CLI not found: {args.codex}")
    version = subprocess.run([executable, "--version"], cwd=ROOT, capture_output=True,
                             text=True, timeout=args.timeout, check=True).stdout.strip()
    if version != lock["codexCliVersion"]:
        fail(f"Codex protocol version mismatch: expected {lock['codexCliVersion']}, got {version}")

    work = Path(tempfile.mkdtemp(prefix="codex-app-server-", dir=ROOT / "tmp"))
    process = None
    selector = selectors.DefaultSelector()
    try:
        home = work / "home"
        config = work / "config"
        cwd = work / "project"
        for directory in (home, config, cwd):
            directory.mkdir()
        env = dict(os.environ)
        for key in ("CODEX_HOME", "OPENAI_API_KEY", "CODEX_API_KEY"):
            env.pop(key, None)
        env.update({"HOME": str(home), "XDG_CONFIG_HOME": str(config), "TMPDIR": str(ROOT / "tmp")})
        process = subprocess.Popen([executable, "app-server"], cwd=cwd, env=env,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, bufsize=1)
        selector.register(process.stdout, selectors.EVENT_READ)

        def send(message):
            process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
            process.stdin.flush()

        def receive():
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                if selector.select(timeout=0.1):
                    line = process.stdout.readline()
                    if not line:
                        fail("Codex app-server closed stdout: " + process.stderr.read())
                    try:
                        message = json.loads(line)
                    except json.JSONDecodeError as error:
                        fail(f"Codex app-server wrote non-JSON stdout: {error}")
                    if not isinstance(message, dict):
                        fail(f"Codex app-server emitted a non-object message: {message!r}")
                    version_field = message.get("jsonrpc")
                    expected_field = lock["serverEnvelope"]["jsonrpcVersionField"]
                    if (expected_field == "omitted" and version_field is not None) or (
                            expected_field != "omitted" and version_field != expected_field):
                        fail(f"Codex app-server jsonrpc field mismatch: expected {expected_field!r}, got {version_field!r}")
                    if version_field not in (None, "2.0"):
                        fail(f"Codex app-server emitted an invalid JSON-RPC message: {message!r}")
                    return message
                if process.poll() is not None:
                    fail("Codex app-server exited before responding: " + process.stderr.read())
            fail("Codex app-server response timed out")

        send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "clientInfo": {"name": "ghosterm-protocol-check", "title": "Ghosterm protocol check",
                           "version": "0.0.0"}}})
        initialized = receive()
        if initialized.get("id") != 1 or "result" not in initialized:
            fail(f"Codex app-server initialize response mismatch: {initialized!r}")

        send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        send({"jsonrpc": "2.0", "id": 2, "method": "thread/start", "params": {
            "cwd": str(cwd), "approvalPolicy": "on-request", "sandbox": "read-only"}})
        response = receive()
        while response.get("id") != 2:
            response = receive()
        thread = response.get("result", {}).get("thread") if isinstance(response.get("result"), dict) else None
        if thread is None:
            fail(f"Codex app-server thread/start response mismatch: {response!r}")
        print(f"CODEX APP-SERVER LIVE HANDSHAKE PASS: {version}; initialize, initialized, thread/start; no turn/model request")
    finally:
        selector.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.stderr.write(error.stderr or str(error) + "\n")
        raise SystemExit(error.returncode or 1)
    except (OSError, subprocess.TimeoutExpired) as error:
        fail(f"Codex app-server probe failed: {error}")
