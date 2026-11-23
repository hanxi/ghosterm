#!/usr/bin/env python3
"""Exercise both Preview Lua roles over a disposable managed SSH route."""
import argparse
import json
import os
from pathlib import Path
import pwd
import runpy
import shlex
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--gterm", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    preview = ROOT / "plugins/official/preview"
    routes = runpy.run_path(str(ROOT / "scripts/test-plugin-route.py"))
    load = routes["load"]
    agent_fixture = load("preview_ssh_agent", ROOT / "scripts/test-plugin-ssh.py")
    identity_fixture = load("preview_ssh_identity", ROOT / "scripts/test-plugin-ssh-stream.py")
    project_tmp = ROOT / "tmp"
    requested = Path(os.environ.get("TMPDIR", project_tmp)).resolve()
    scratch = requested if requested.is_dir() and requested.is_relative_to(project_tmp) else project_tmp
    with tempfile.TemporaryDirectory(prefix="preview-ssh-", dir=scratch) as directory:
        work = Path(directory)
        workspace = work / "workspace"
        workspace.mkdir()
        (workspace / "file.bin").write_bytes(b"abc")
        outside = work / "outside"
        outside.mkdir()
        (outside / "file.bin").write_bytes(b"not part of the authorized workspace")
        (workspace / "linked.bin").symlink_to(outside / "file.bin")
        (workspace / "linked-dir").symlink_to(outside, target_is_directory=True)
        chain_dir = work / "chain"
        chain_dir.mkdir()
        private, public = identity_fixture.memory_identity()
        with agent_fixture.owned_agent(work, "preview-agent") as agent:
            keys = [agent_fixture.add_memory_key(agent, f"preview-hop-{i}") for i in range(2)]
            with routes["chain"](chain_dir, 2, public, keys, agent, "echo") as (ports, known, _):
                command = shlex.join(
                    [str(binary), "--executor", str(preview / "executor.lua"), str(workspace), directory]
                )
                environment = {**os.environ, "TMPDIR": directory, "TMP": directory, "TEMP": directory}
                if args.gterm:
                    package = work / "package"
                    built = subprocess.run(
                        [sys.executable, str(ROOT / "scripts/gterm-plugin"), "build",
                         str(preview), "--out", str(package)],
                        env=environment, capture_output=True, check=True, timeout=10,
                    )
                    digest = json.loads(built.stdout)["packageSHA256"]
                    environment["GHOSTERM_TEST_PREVIEW_EXECUTOR_DIGEST"] = digest
                    command = shlex.join([
                        str(args.gterm.resolve(strict=True)), "relay", "--stdio",
                        "--trust-development-digest", digest, "--client-digest", "d" * 64,
                        "--method", "preview.open", "--root", "workspace", str(workspace),
                        "--", str(package), "preview", "./file.bin",
                    ])
                for rejected in (None, "../outside/file.bin", "linked.bin", "linked-dir/file.bin"):
                    case_env = dict(environment)
                    case_env.pop("GHOSTERM_TEST_PREVIEW_REJECTED_PATH", None)
                    if rejected:
                        case_env["GHOSTERM_TEST_PREVIEW_REJECTED_PATH"] = rejected
                    result = subprocess.run(
                        [str(binary), str(preview / "main.lua"), "2", pwd.getpwuid(os.getuid()).pw_name,
                         str(known), command, *map(str, ports)],
                        env=case_env,
                        input=struct.pack(">I", len(private)) + private,
                        capture_output=True,
                        timeout=30,
                    )
                    if result.returncode:
                        sys.stderr.buffer.write(result.stderr)
                        raise SystemExit(result.returncode)
                    if rejected:
                        print(f"PASS Lua Preview rejects {rejected} with no resource receipt")
    assert not work.exists()


if __name__ == "__main__":
    main()
