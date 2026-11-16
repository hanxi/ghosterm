#!/usr/bin/env python3
"""Real official Preview Client/Executor over two-hop SSH into Android Host Runtime."""
from contextlib import ExitStack
from pathlib import Path
import argparse
import base64
import hashlib
import importlib.util
import json
import os
import pwd
import secrets
import shutil
import shlex
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android", required=True, help="explicit test device or headless emulator serial")
    parser.add_argument("--gterm", type=Path, required=True, help="JS-enabled gterm executable")
    parser.add_argument("--test", default="com.hanxi.ghosterm.ExtensionChannelTest#controllerReceivesOfficialPreviewOverTwoHopSSH",
                        help="single instrumentation class#method (defaults to the isolated Controller E2E)")
    args = parser.parse_args()
    gterm = args.gterm.resolve()
    package_source = ROOT / "plugins/official/preview"
    if not gterm.is_file():
        raise SystemExit("gterm binary is missing")
    check = subprocess.run([sys.executable, ROOT / "scripts/build-plugin-preview.py", "--check"],
                           cwd=ROOT, capture_output=True, text=True, timeout=30)
    if check.returncode:
        raise SystemExit(check.stdout + check.stderr)

    work_device = ROOT / "build/android-toolchain/sdk/platform-tools/adb"
    device = [str(work_device), "-s", args.android]
    api_level = subprocess.run([*device, "shell", "getprop", "ro.build.version.sdk"],
                               check=True, capture_output=True, text=True, timeout=15).stdout.strip()
    abi = subprocess.run([*device, "shell", "getprop", "ro.product.cpu.abi"],
                         check=True, capture_output=True, text=True, timeout=15).stdout.strip()
    if not api_level.isdigit() or not abi:
        raise RuntimeError(f"could not identify explicit Android test device {args.android}: API={api_level!r}, ABI={abi!r}")

    work = Path(tempfile.mkdtemp(prefix="plugin-preview-android-", dir=ROOT / "tmp"))
    routes = load("preview_android_routes", ROOT / "scripts/test-plugin-route.py")
    keys_helper = load("preview_android_keys", ROOT / "scripts/test-plugin-ssh.py")
    streams = load("preview_android_streams", ROOT / "scripts/test-plugin-ssh-stream.py")
    private, public = streams.memory_identity()
    package = work / "preview-package"
    shutil.copytree(package_source, package)
    integrity = (package / "integrity.json").read_bytes()
    digest = hashlib.sha256(integrity).hexdigest()
    fixture_markdown = b"# Android Preview\n\n![red marker](marker.png)\n\nAndroid Markdown over two-hop SSH\n"
    workspace = work / "workspace"
    markdown_dir = workspace / "docs"
    markdown_dir.mkdir(parents=True)
    (markdown_dir / "preview.md").write_bytes(fixture_markdown)
    # Small deterministic RGBA PNG, generated in the isolated fixture.
    width = height = 32
    scanline = b"\x00" + bytes((255, 0, 0, 255)) * width
    raw = scanline * height
    def png_chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)
    image_bytes = (b"\x89PNG\r\n\x1a\n" +
                   png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                   png_chunk(b"IDAT", zlib.compress(raw)) + png_chunk(b"IEND", b""))
    (markdown_dir / "marker.png").write_bytes(image_bytes)
    remote_tmp = work / "remote-tmp"
    remote_tmp.mkdir()
    remote_stderr = work / "remote-gterm.stderr"
    remote_exit = work / "remote-gterm.exit"
    remote_wrapper = work / "gterm-preview-wrapper"
    remote_wrapper.write_text("#!/bin/sh\n" + shlex.quote(str(gterm)) + " \"$@\" 2>" +
                              shlex.quote(str(remote_stderr)) + "\nstatus=$?\nprintf '%s' \"$status\" > " +
                              shlex.quote(str(remote_exit)) + "\nexit \"$status\"\n")
    remote_wrapper.chmod(0o700)
    fixture_token = secrets.token_hex(24)
    apk = ROOT / "android/app/build/outputs/apk/debug/app-debug.apk"
    test_apk = ROOT / "android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk"

    try:
        subprocess.run([*device, "install", "-r", str(apk)], check=True, capture_output=True, timeout=90)
        subprocess.run([*device, "install", "-r", "-t", str(test_apk)], check=True, capture_output=True, timeout=90)
        with keys_helper.owned_agent(work, "preview-android-host-agent") as agent:
            host_keys = [keys_helper.add_memory_key(agent, f"preview-android-hop-{i}") for i in range(2)]
            if len({line.split()[1] for line in host_keys}) != 2:
                raise RuntimeError("two-hop SSH fixture host identities collided")
            route_dir = work / "route-two"
            route_dir.mkdir()
            with routes.chain(route_dir, 2, public, host_keys, agent, "echo") as (ports, known_hosts, _):
                package_files = {
                    path.relative_to(package).as_posix(): base64.b64encode(path.read_bytes()).decode("ascii")
                    for path in sorted(package.rglob("*")) if path.is_file()
                }
                fixture = {
                    "two": {"ports": ports, "known": known_hosts.read_text()},
                    "user": pwd.getpwuid(os.getuid()).pw_name,
                    "pem": private.decode("ascii"),
                    "previewPackageFiles": package_files,
                    "previewPackageDigest": digest,
                    "previewPackageDirectory": str(package),
                    "previewWorkspace": str(workspace),
                    "previewRemoteTmp": str(remote_tmp),
                    "previewRemoteStderr": str(remote_stderr),
                    "previewRemoteExit": str(remote_exit),
                    "gtermBinary": str(gterm),
                    "previewGtermWrapper": str(remote_wrapper),
                }
                payload = b""  # Published only after the owned guest port mappings exist.

                class Handler(socketserver.StreamRequestHandler):
                    def handle(self):
                        self.request.settimeout(5)
                        if self.rfile.readline(128).rstrip(b"\n") != fixture_token.encode():
                            return
                        self.wfile.write(struct.pack(">I", len(payload)) + payload)

                server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
                server.daemon_threads = True
                server_thread = threading.Thread(target=server.serve_forever, name="preview-fixture-server")
                server_thread.start()
                owned_guest_ports = []
                def reverse_guest(host_port):
                    # Do not replace an existing mapping or use adb's tcp:0 alias.
                    # Retry only a reported bind collision, before instrumentation.
                    for _ in range(16):
                        guest_port = 20000 + secrets.randbelow(20000)
                        result = subprocess.run([*device, "reverse", "--no-rebind", f"tcp:{guest_port}", f"tcp:{host_port}"],
                                                capture_output=True, text=True, timeout=15)
                        detail = result.stdout + result.stderr
                        if result.returncode:
                            if "cannot bind listener: Address already in use" in detail or "cannot rebind existing socket" in detail:
                                continue
                            raise RuntimeError("reverse fixture: " + detail)
                        owned_guest_ports.append(guest_port)
                        return guest_port
                    raise RuntimeError("No guest fixture port after 16 bind collisions")
                try:
                    fixture_port = reverse_guest(server.server_address[1])
                    guest_hop = reverse_guest(ports[0])
                    old_name = f"[route-hop-0.invalid]:{ports[0]} "
                    if fixture["two"]["known"].count(old_name) != 1:
                        raise RuntimeError("Missing unique first-hop known-host port")
                    fixture["two"]["known"] = fixture["two"]["known"].replace(old_name, f"[route-hop-0.invalid]:{guest_hop} ")
                    fixture["two"]["ports"] = [guest_hop, *ports[1:]]
                    payload = json.dumps(fixture, separators=(",", ":")).encode()
                    if len(payload) > 1048576:
                        raise RuntimeError("Android fixture payload exceeds its explicit one MiB bound")
                    output = subprocess.run(
                        [*device, "shell", "am", "instrument", "-w", "-r",
                         "-e", "class", args.test,
                         "-e", "fixturePort", str(fixture_port), "-e", "fixtureToken", fixture_token,
                         "com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner"],
                        cwd=ROOT, capture_output=True, text=True, timeout=240)
                    transcript = output.stdout + output.stderr
                    print(transcript, end="", flush=True)
                    if "INSTRUMENTATION_STATUS_CODE: -4" in transcript or "assumption failed:" in transcript:
                        raise RuntimeError("Android Preview instrumentation was skipped instead of executed")
                    if output.returncode or "OK (1 test)" not in transcript or "FAILURES!!!" in transcript or "INSTRUMENTATION_FAILED" in transcript:
                        remote_error = remote_stderr.read_text(errors="replace") if remote_stderr.exists() else ""
                        remote_status = remote_exit.read_text(errors="replace") if remote_exit.exists() else "not-exited"
                        raise RuntimeError(f"Android Preview instrumentation failed ({output.returncode}); remote exit={remote_status}; stderr={remote_error}")
                    deadline = time.monotonic() + 15
                    while not remote_exit.exists() and time.monotonic() < deadline:
                        time.sleep(0.05)
                    if not remote_exit.exists():
                        raise RuntimeError("remote gterm did not exit after Android closed the Client Runtime")
                    remote_status = remote_exit.read_text()
                    remote_error = remote_stderr.read_text(errors="replace") if remote_stderr.exists() else ""
                    expected_unknown = (
                        "gterm: runtime phase=3 error=6 execution-error=0 child-exit=0 "
                        "child-signal=0 session-end=6 session-error=3 cleanup=0\n"
                        "gterm: executor channel outcome unknown")
                    if remote_status != "7" or remote_error.strip() != expected_unknown:
                        raise RuntimeError("remote gterm did not preserve the unknown outcome after Host closed its long-lived Session: " +
                                           remote_status + " " + remote_error)
                    deadline = time.monotonic() + 10
                    leftovers = [entry.name for entry in remote_tmp.iterdir()]
                    while leftovers and time.monotonic() < deadline:
                        time.sleep(0.05)
                        leftovers = [entry.name for entry in remote_tmp.iterdir()]
                    if leftovers:
                        raise RuntimeError("remote ResourceStore temp files survived process cleanup: " + str(leftovers))
                finally:
                    for port in owned_guest_ports:
                        subprocess.run([*device, "reverse", "--remove", f"tcp:{port}"],
                                       capture_output=True, timeout=10)
                    server.shutdown()
                    server.server_close()
                    server_thread.join(5)
                    if server_thread.is_alive():
                        raise RuntimeError("fixture server thread did not stop")
        for path in work.rglob("*"):
            if path.is_file():
                data = path.read_bytes()
                if b"PRIVATE KEY-----" in data or b"openssh-key-v1\0" in data:
                    raise RuntimeError("temporary fixture serialized private-key material")
        print(f"ANDROID OFFICIAL PREVIEW E2E PASS: API {api_level}, {abi}, device {args.android}; real two-hop SSH, consent, Markdown+PNG Settings rendering, stop/release, remote cleanup")
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
