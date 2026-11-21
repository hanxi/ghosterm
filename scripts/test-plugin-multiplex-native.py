#!/usr/bin/env python3
"""Copied macOS App, real eight-instance groups on owned local/direct/jump IO.

No user endpoint, model service, device, private key file or foreground window.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import secrets
import shutil
import socketserver
import struct
import subprocess
import sys
import threading
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--route", choices=["local", "direct", "jump"], action="append")
    args = parser.parse_args()
    build, work = args.build.resolve(strict=True), args.work.resolve(strict=True)
    assert work.is_relative_to(ROOT / "tmp") and work.is_dir()
    project, logs = work / "project", work / "logs"
    logs.mkdir(exist_ok=True)
    if not project.exists():
        for folder in ["apple/GhostermApp", "apple/GhostermCore", "apple/GhostermTerminal"]:
            shutil.copytree(ROOT / folder, project / folder, symlinks=True, ignore=shutil.ignore_patterns(".build", ".swiftpm"))
        (project / "core").mkdir()
        shutil.copytree(ROOT / "core/include", project / "core/include", symlinks=True)
        (project / "core/build").symlink_to(build / "core", target_is_directory=True)
        for folder in ["build", "patches"]:
            (project / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (project / "tmp").mkdir()
    env = {**os.environ, "TMPDIR": str(work), "TMP": str(work), "TEMP": str(work),
        "DEVELOPER_DIR": "/Applications/Xcode.app/Contents/Developer",
        "GHOSTERM_CORE_TEST_PLUGIN_JS": "1", "GHOSTERM_AUTOMATION_OFFSCREEN": "1"}

    def run(label, arguments, extra=None, count=None):
        with (logs / (label + ".log")).open("w") as output:
            result = subprocess.run(list(map(str, arguments)), cwd=ROOT, env={**env, **(extra or {})},
                stdout=output, stderr=subprocess.STDOUT, timeout=300)
        text = (logs / (label + ".log")).read_text()
        assert result.returncode == 0, f"{label} failed; see {logs / (label + '.log')}"
        if count is not None:
            assert f"Executed {count} test" in text and "with 0 failures" in text and "skipped" not in text.lower(), label
        print("PASS " + label, flush=True)

    app = project / "apple/GhostermApp"
    swift_options = ["--package-path", app, "--cache-path", work / "swift-cache", "--disable-keychain", "--disable-netrc"]
    run("compile", ["swift", "build", "--build-tests", *swift_options])
    package = work / "table-package"
    if not package.exists():
        run("package", ["python3", ROOT / "scripts/build-plugin-sample.py", "--source", ROOT / "plugins/official/table",
            "--tools", ROOT / "build/plugin-sdk-tools", "--out", package])
    # Verify every copied production input, not just the newly introduced files.
    inputs = {str(p.relative_to(project)): hashlib.sha256(p.read_bytes()).hexdigest()
        for folder in ["apple/GhostermApp/Sources", "apple/GhostermCore/Sources", "apple/GhostermApp/Tests"]
        for p in (project / folder).rglob("*") if p.is_file()}
    keys = load("mux_keys", "scripts/test-plugin-ssh.py")
    streams = load("mux_streams", "scripts/test-plugin-ssh-stream.py")
    routes = load("mux_routes", "scripts/test-plugin-route.py")
    private, public = streams.memory_identity()
    with keys.owned_agent(work, "agent") as agent:
        host_keys = [keys.add_memory_key(agent, "mux-hop-" + str(i)) for i in range(2)]
        route_root = Path(tempfile.mkdtemp(prefix="r-", dir=work))
        with routes.chain(route_root, 2, public, host_keys, agent, "echo") as (ports, _, _):
            for route in args.route or ["local", "direct", "jump"]:
                scratch = Path(tempfile.mkdtemp(prefix=route + "-", dir=work))
                token = secrets.token_hex(24)
                payload = dict(route=route, scratch=str(scratch), package=str(package), gterm=str(build / "gterm"),
                    ports=ports, hostKeys=[key.decode() for key in host_keys], user=pwd.getpwuid(os.getuid()).pw_name, pem=private.decode())
                class Handler(socketserver.StreamRequestHandler):
                    def handle(self):
                        self.connection.settimeout(5)
                        if self.rfile.readline(128).rstrip(b"\n") != token.encode(): return
                        data = json.dumps(payload, separators=(",", ":")).encode()
                        self.wfile.write(struct.pack(">I", len(data)) + data)
                with socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler) as server:
                    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
                    try:
                        run(route, ["swift", "test", "--skip-build", *swift_options, "--filter",
                            "ExtensionConnectionTests.testRealEightTableInstancesShareOneConnectionAndCloseIndependently"],
                            {"GHOSTERM_APP_MUX_PORT": str(server.server_address[1]), "GHOSTERM_APP_MUX_TOKEN": token}, 1)
                    finally: server.shutdown(); thread.join()
    for name, sha in inputs.items():
        assert hashlib.sha256((project / name).read_bytes()).hexdigest() == sha, "copied input changed: " + name
        assert hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == sha, "root input differs: " + name
    for path in logs.glob("*.log"):
        assert b"PRIVATE KEY-----" not in path.read_bytes()
    (work / "receipt.json").write_text(json.dumps(dict(inputs_sha256=inputs,
        logs_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in logs.glob("*.log")},
        scope="eight real App Client/Executor instances over one local/direct/jump connection; separate panes, consent and close; no full platform matrix"), indent=2) + "\n")


if __name__ == "__main__":
    main()
