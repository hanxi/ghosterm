#!/usr/bin/env python3
"""Real official Preview Client/Executor over direct or two-hop SSH into Android Host Runtime."""
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
    parser.add_argument("--gterm", type=Path, required=True, help="Lua-enabled gterm executable")
    parser.add_argument("--apk-directory", type=Path, default=ROOT / "android/app/build/outputs/apk",
                        help="APK output directory from the explicit Android build")
    parser.add_argument("--route", choices=["direct", "jump"], default="jump")
    parser.add_argument("--preview-format", choices=["markdown", "html", "html-unicode"], default="markdown")
    parser.add_argument("--large-attachment", action="store_true", help="send a 256 KiB PNG through the native resource receiver")
    parser.add_argument("--test", help="single instrumentation class#method; default matches the fixture format")
    parser.add_argument("--deny-consent", action="store_true", help="verify explicit Session denial and Executor exit")
    parser.add_argument("--tmux-foreground", action="store_true", help="attach to Preview in an owned target tmux pane")
    parser.add_argument("--kill-active-pane", action="store_true", help="close the owned tmux pane while a Lua RPC is pending")
    args = parser.parse_args()
    if args.tmux_foreground and (args.test or args.deny_consent or args.preview_format != "markdown"):
        parser.error("--tmux-foreground selects its own Markdown attachment test")
    tmux = shutil.which("tmux") if args.tmux_foreground else None
    if args.tmux_foreground and not tmux:
        parser.error("--tmux-foreground requires tmux on the owned SSH fixture host")
    if args.kill_active_pane and not args.tmux_foreground:
        parser.error("--kill-active-pane requires --tmux-foreground")
    if args.deny_consent and args.test:
        parser.error("--deny-consent selects its own instrumentation case")
    hop_count = 1 if args.route == "direct" else 2
    route_label = "direct" if hop_count == 1 else "two-hop"
    method_prefix = "realDirect" if hop_count == 1 else "realTwoHop"
    if args.tmux_foreground or args.deny_consent:
        suffix = "DirectSSH" if hop_count == 1 else "ProxyJump"
        method = "luaTmuxForegroundAttachmentOver" if args.tmux_foreground else "luaDenialDoesNotActivateOn"
        args.test = "com.hanxi.ghosterm.PluginPreviewSettingsE2ETest#" + method + suffix
    elif args.test is None:
        args.test = {
            "markdown": f"com.hanxi.ghosterm.PluginPreviewSettingsE2ETest#{method_prefix}PreviewReceiptAppearsInMainActivitySettings",
            "html": f"com.hanxi.ghosterm.PluginPreviewSettingsE2ETest#{method_prefix}HtmlAppearsInMainActivitySettings",
            "html-unicode": f"com.hanxi.ghosterm.PluginPreviewSettingsE2ETest#{method_prefix}UnicodeHtmlAppearsInMainActivitySettings",
        }[args.preview_format]
    gterm = args.gterm.resolve()
    package_source = ROOT / "plugins/official/preview"
    if not gterm.is_file():
        raise SystemExit("gterm binary is missing")
    check = subprocess.run([sys.executable, "-B", ROOT / "scripts/gterm-plugin", "check", package_source],
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

    tmp = (ROOT / "tmp").resolve()
    requested = Path(os.environ.get("TMPDIR", tmp)).resolve()
    scratch = requested if requested.is_dir() and requested.is_relative_to(tmp) else tmp
    work = Path(tempfile.mkdtemp(prefix="pv-", dir=scratch))
    tmux_prefix = [tmux, "-S", str(work / "t.sock"), "-f", "/dev/null"]
    tmux_env = {**os.environ, "TERM": "xterm-256color", "TMPDIR": str(work)}
    tmux_env.pop("TMUX", None)
    tmux_env.pop("TMUX_PANE", None)

    def tmux_command(*argv, check=True):
        return subprocess.run([*tmux_prefix, *argv], cwd=work, env=tmux_env, check=check,
                              capture_output=True, text=True, timeout=5).stdout.strip()

    def marker(pane, word):
        tmux_command("send-keys", "-l", "-t", pane, f"printf '%s%s\\n' ANDROID_SIBLING_ {word}")
        tmux_command("send-keys", "-t", pane, "Enter")
        deadline = time.monotonic() + 5
        while "ANDROID_SIBLING_" + word not in tmux_command("capture-pane", "-p", "-t", pane):
            if time.monotonic() >= deadline:
                raise RuntimeError("ordinary tmux pane did not execute its own command")
            time.sleep(0.025)
    routes = load("preview_android_routes", ROOT / "scripts/test-plugin-route.py")
    keys_helper = load("preview_android_keys", ROOT / "scripts/test-plugin-ssh.py")
    streams = load("preview_android_streams", ROOT / "scripts/test-plugin-ssh-stream.py")
    private, public = streams.memory_identity()
    package = work / "preview-package"
    if args.kill_active_pane:
        source = work / "source"
        shutil.copytree(package_source, source)
        executor = source / "executor.lua"
        original = executor.read_text()
        anchor = '["preview.open"] = preview,'
        assert original.count(anchor) == 1
        executor.write_text(original.replace(anchor, '''["preview.open"] = function(input)
    if input.path ~= "pending" then return preview(input) end
    assert(ctx.resources.publish("workspace", "pending.bin", "application/octet-stream", 3))
    while true do ctx.wait() end
  end,'''))
        package_source = source
    build = subprocess.run([sys.executable, "-B", ROOT / "scripts/gterm-plugin", "build",
                            package_source, "--out", package], cwd=ROOT, capture_output=True, text=True, timeout=30)
    if build.returncode:
        shutil.rmtree(work)
        raise SystemExit(build.stdout + build.stderr)
    integrity = (package / "integrity.json").read_bytes()
    digest = hashlib.sha256(integrity).hexdigest()
    fixture_markdown = f"# Android Preview\n\n![red marker](marker.png)\n\nAndroid Markdown over {route_label} SSH\n".encode()
    workspace = work / "workspace"
    unicode_html = args.preview_format == "html-unicode"
    markdown_dir = workspace / ("docs/cafe\u0301" if unicode_html else "docs")
    markdown_dir.mkdir(parents=True)
    if args.kill_active_pane:
        (workspace / "pending.bin").write_bytes(b"abc")
    if args.preview_format == "markdown":
        (markdown_dir / "preview.md").write_bytes(fixture_markdown)
    else:
        main_name = "页面 #?%.html" if unicode_html else "index.html"
        css_name = "主 题%.css" if unicode_html else "theme.css"
        css_link = "%E4%B8%BB%20%E9%A2%98%25.css" if unicode_html else "theme.css"
        image_link = "%E5%9B%BE%F0%9F%98%80.png" if unicode_html else "marker.png"
        (markdown_dir / main_name).write_text(
            f"<!doctype html><link rel='stylesheet' href='{css_link}'><img src='{image_link}'>"
            f"<p>Android HTML over {route_label} SSH</p><script>document.body.textContent='SCRIPT_RAN'</script>", encoding="utf-8")
        (markdown_dir / css_name).write_bytes(b"body{background:#0000ff!important;min-height:200px}")
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
    if args.large_attachment:
        padding = 256 * 1024 - len(image_bytes) - 12  # Length, type and CRC.
        image_bytes = image_bytes[:-12] + png_chunk(b"gpAd", bytes(padding)) + image_bytes[-12:]
    (markdown_dir / ("图😀.png" if unicode_html else "marker.png")).write_bytes(image_bytes)
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
    apk = args.apk_directory / "debug/app-debug.apk"
    test_apk = args.apk_directory / "androidTest/debug/app-debug-androidTest.apk"

    try:
        subprocess.run([*device, "install", "-r", str(apk)], check=True, capture_output=True, timeout=90)
        subprocess.run([*device, "install", "-r", "-t", str(test_apk)], check=True, capture_output=True, timeout=90)
        for package_id, archive in (("com.hanxi.ghosterm", apk), ("com.hanxi.ghosterm.test", test_apk)):
            installed = subprocess.run([*device, "shell", "pm", "path", package_id], check=True,
                                       capture_output=True, text=True, timeout=15).stdout.strip()
            if not installed.startswith("package:/data/app/") or "\n" in installed:
                raise RuntimeError(f"unexpected installed APK for {package_id}")
            actual = subprocess.run([*device, "shell", "sha256sum", installed.removeprefix("package:")],
                                    check=True, capture_output=True, text=True, timeout=15).stdout.split()
            if not actual or actual[0] != hashlib.sha256(archive.read_bytes()).hexdigest():
                raise RuntimeError(f"installed APK differs from {archive.name}")
        with keys_helper.owned_agent(work, "agent") as agent:
            host_keys = [keys_helper.add_memory_key(agent, f"preview-android-hop-{i}") for i in range(hop_count)]
            if len({line.split()[1] for line in host_keys}) != hop_count:
                raise RuntimeError("SSH fixture host identities collided")
            route_dir = work / "route"
            route_dir.mkdir()
            with routes.chain(route_dir, hop_count, public, host_keys, agent, "echo") as (ports, known_hosts, _):
                package_files = {
                    path.relative_to(package).as_posix(): base64.b64encode(path.read_bytes()).decode("ascii")
                    for path in sorted(package.rglob("*")) if path.is_file()
                }
                fixture = {
                    "previewFormat": args.preview_format,
                    "previewImageBytes": len(image_bytes),
                    "previewImageSHA256": hashlib.sha256(image_bytes).hexdigest(),
                    "route": {"ports": ports, "known": known_hosts.read_text()},
                    "user": pwd.getpwuid(os.getuid()).pw_name,
                    "pem": private.decode("ascii"),
                    "previewPackageFiles": package_files,
                    "previewPackageDigest": digest,
                    "previewPackageDirectory": str(package),
                    "previewWorkspace": str(workspace),
                    "previewGtermWrapper": str(remote_wrapper),
                }
                payload = b""  # Published only after the owned guest port mappings exist.

                class Handler(socketserver.StreamRequestHandler):
                    def handle(self):
                        self.request.settimeout(5)
                        request = self.rfile.readline(128).rstrip(b"\n")
                        if request == fixture_token.encode():
                            reply = payload
                        elif args.kill_active_pane and request == (fixture_token + " kill-active").encode():
                            tmux_command("kill-pane", "-t", plugin_pane)
                            reply = b'{"closed":true}'
                        else:
                            return
                        self.wfile.write(struct.pack(">I", len(reply)) + reply)

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
                    if fixture["route"]["known"].count(old_name) != 1:
                        raise RuntimeError("Missing unique first-hop known-host port")
                    fixture["route"]["known"] = fixture["route"]["known"].replace(old_name, f"[route-hop-0.invalid]:{guest_hop} ")
                    fixture["route"]["ports"] = [guest_hop, *ports[1:]]
                    if args.tmux_foreground:
                        endpoint = work / "e"
                        endpoint.mkdir(mode=0o700)
                        ordinary = tmux_command("new-session", "-d", "-P", "-F", "#{pane_id}", "-s", "preview", "/bin/sh")
                        marker(ordinary, "BEFORE")
                        command = shlex.join(map(str, [gterm, "relay", "--listen", endpoint,
                            "--trust-development-digest", digest, "--client-digest", digest, "--method", "preview.open",
                            "--root", "workspace", workspace, "--", package, "preview", "docs/preview.md",
                            "--assets", "docs/marker.png"]))
                        plugin_pane = tmux_command("split-window", "-d", "-P", "-F", "#{pane_id}", "-t", ordinary,
                                                   "test -t 0 && exec " + command)
                        assert plugin_pane and plugin_pane != ordinary
                        permit = endpoint / "permit"
                        deadline = time.monotonic() + 5
                        while not permit.is_file() or permit.stat().st_size != 56:
                            if time.monotonic() >= deadline:
                                raise RuntimeError("Lua foreground permit did not appear in tmux")
                            time.sleep(0.025)
                        marker(ordinary, "LIVE")
                        fixture.update(foregroundDirectory=str(endpoint), foregroundInstance=permit.read_bytes()[8:24].hex(),
                                       killActivePane=args.kill_active_pane)
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
                        "gterm: runtime phase=3 error=6 "
                        "session-end=6 session-error=3 cleanup=0\n"
                        "gterm: executor channel outcome unknown")
                    expected_status, expected_error = ("6", "gterm: executor denied") if args.deny_consent else ("7", expected_unknown)
                    if args.tmux_foreground:
                        expected_status, expected_error = "0", ""
                    if remote_status != expected_status or remote_error.strip() != expected_error:
                        raise RuntimeError(f"unexpected remote gterm outcome: {remote_status} {remote_error}")
                    deadline = time.monotonic() + 10
                    leftovers = [entry.name for entry in remote_tmp.iterdir()]
                    while leftovers and time.monotonic() < deadline:
                        time.sleep(0.05)
                        leftovers = [entry.name for entry in remote_tmp.iterdir()]
                    if leftovers:
                        raise RuntimeError("remote ResourceStore temp files survived process cleanup: " + str(leftovers))
                    if args.tmux_foreground:
                        deadline = time.monotonic() + 8
                        while any(endpoint.iterdir()) or tmux_command("list-panes", "-t", ordinary, "-F", "#{pane_id}") != ordinary:
                            if time.monotonic() >= deadline:
                                raise RuntimeError("Lua foreground cleanup did not leave only the sibling shell")
                            time.sleep(0.025)
                        marker(ordinary, "AFTER")
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
        scope = ("pending RPC unknown after closing the active tmux pane, sibling shell and owner cleanup"
                 if args.kill_active_pane else
                 "consent denied, no Client activation or receipt, Session release, confirmed Executor denial"
                 if args.deny_consent else
                 f"consent, {args.preview_format} explicit attachments Settings rendering, stop/release, remote cleanup")
        print(f"ANDROID OFFICIAL PREVIEW E2E PASS: API {api_level}, {abi}, device {args.android}; real {route_label} SSH, {scope}")
    finally:
        if args.tmux_foreground:
            tmux_command("kill-server", check=False)
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
