#!/usr/bin/env python3
"""Run the Lua Preview foreground relay in an owned tmux pane beside a shell."""

import argparse
from contextlib import contextmanager, nullcontext
import hashlib
import os
from pathlib import Path
import pwd
import runpy
import select
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def wait_for(condition, message):
    deadline = time.monotonic() + 8
    while not condition():
        if time.monotonic() >= deadline:
            raise RuntimeError(message)
        time.sleep(0.025)


@contextmanager
def ssh_transport(work, count):
    routes = runpy.run_path(str(ROOT / "scripts/test-plugin-route.py"))
    helpers = routes["load"]("tmux_ssh_fixture", ROOT / "scripts/test-plugin-ssh.py")
    with helpers.owned_agent(work, "agent") as agent:
        identity = helpers.add_memory_key(agent, "tmux-client")
        public_file = work / "client.pub"
        public_file.write_bytes(identity)
        keys = [helpers.add_memory_key(agent, f"tmux-host-{i}") for i in range(count)]
        server_dir = work / "ssh"
        server_dir.mkdir()
        with routes["chain"](server_dir, count, identity, keys, agent, "echo") as (ports, _, _):
            known = work / "known_hosts"
            known.write_bytes(b"".join(f"[127.0.0.1]:{port} ".encode() + key
                                      for port, key in zip(ports, keys)))
            config = work / "config"
            user = pwd.getpwuid(os.getuid()).pw_name
            settings = (f"Host *\n    HostName 127.0.0.1\n    User {user}\n"
                        f"    UserKnownHostsFile {known}\n    GlobalKnownHostsFile /dev/null\n"
                        f"    StrictHostKeyChecking yes\n    IdentityFile {public_file}\n"
                        f"    IdentityAgent {agent}\n    IdentitiesOnly yes\n    BatchMode yes\n")
            for i, port in enumerate(ports[:-1]):
                settings += f"Host hop{i}\n    Port {port}\n"
            settings += f"Host destination\n    Port {ports[-1]}\n"
            if count > 1:
                settings += "    ProxyJump " + ",".join(f"hop{i}" for i in range(count - 1)) + "\n"
            config.write_text(settings)
            yield ["/usr/bin/ssh", "-F", str(config), "-T", "destination"]


def attach_client(attach, client, source, digest, work, environment, interrupt=None):
    left, right = socket.socketpair()
    with left, right:
        relay = subprocess.Popen(attach, stdin=left, stdout=left, stderr=subprocess.PIPE, env=environment)
        left.close()
        try:
            if interrupt:
                with subprocess.Popen([client, "--attached-pending-client", digest, work],
                                      stdin=right, stdout=right, stderr=subprocess.PIPE, env=environment) as host:
                    try:
                        assert select.select([host.stderr], [], [], 10)[0], "pending RPC never reached Lua"
                        assert host.stderr.readline() == b"PENDING\n", "pending RPC did not publish its readiness resource"
                        interrupt()
                        _, error = host.communicate(timeout=10)
                        assert host.returncode == 0, error
                    finally:
                        if host.poll() is None:
                            host.kill()
                            host.wait(timeout=5)
            else:
                subprocess.run([client, "--attached-client", source, digest, work],
                               stdin=right, stdout=right, env=environment, check=True, timeout=15)
            right.close()
            if relay.wait(timeout=5):
                raise RuntimeError(relay.stderr.read().decode(errors="replace"))
        finally:
            if relay.poll() is None:
                relay.kill()
                relay.wait(timeout=5)
            relay.stderr.close()


def exercise(work, gterm, client, tmux_path, ssh_prefix=None, kill_pane=False, kill_active=False):
    environment = dict(os.environ, TERM="xterm-256color", TMPDIR=str(work), TMP=str(work), TEMP=str(work))
    environment.pop("TMUX", None)
    environment.pop("TMUX_PANE", None)
    command = [tmux_path, "-S", str(work / "t.sock"), "-f", "/dev/null"]

    def tmux(*args):
        return subprocess.run([*command, *args], cwd=work, env=environment,
                              capture_output=True, check=True, timeout=5).stdout

    def marker(pane, word):
        tmux("send-keys", "-l", "-t", pane, f"printf '%s%s\\n' LUA_FIXTURE_ {word}")
        tmux("send-keys", "-t", pane, "Enter")
        wait_for(lambda: ("LUA_FIXTURE_" + word).encode() in tmux("capture-pane", "-p", "-t", pane),
                 "ordinary terminal did not execute its own command")

    source = ROOT / "plugins/official/preview"
    if kill_active:
        fixture = work / "source"
        shutil.copytree(source, fixture)
        executor = fixture / "executor.lua"
        executor.write_text("""
local ctx = ...
return {
  ["preview.open"] = function()
    assert(ctx.resources.publish("workspace", "file.bin", "application/octet-stream", 3))
    while true do ctx.wait() end
  end,
}
""")
        source = fixture
    package, workspace, endpoint = (work / name for name in ("p", "w", "e"))
    subprocess.run([sys.executable, "-B", ROOT / "scripts/gterm-plugin", "build", source,
                    "--out", package], cwd=ROOT, env=environment, check=True, timeout=20)
    workspace.mkdir()
    endpoint.mkdir(mode=0o700)
    (workspace / "file.bin").write_bytes(b"abc")
    digest = hashlib.sha256((package / "integrity.json").read_bytes()).hexdigest()
    invocation = ("preview", "./file.bin")
    plugin = shlex.join(map(str, [gterm, "relay", "--listen", endpoint, "--trust-development-digest", digest,
                               "--client-digest", digest, "--method", "preview.open", "--root", "workspace",
                               workspace, "--", package, *invocation]))
    try:
        ordinary = tmux("new-session", "-d", "-P", "-F", "#{pane_id}", "-s", "local", "/bin/sh").decode().strip()
        marker(ordinary, "BEFORE")
        preview = tmux("split-window", "-d", "-P", "-F", "#{pane_id}", "-t", ordinary,
                       "test -t 0 && exec " + plugin).decode().strip()
        assert preview and preview != ordinary
        permit = endpoint / "permit"
        wait_for(lambda: permit.is_file() and permit.stat().st_size == 56,
                 "Preview did not publish a permit from the tmux TTY")
        marker(ordinary, "LIVE")
        instance = permit.read_bytes()[8:24].hex()
        attach = [gterm, "relay", "--attach", endpoint, "--instance", instance, "--", *invocation]
        if ssh_prefix:
            attach = [*ssh_prefix, shlex.join(map(str, attach))]
        if kill_pane:
            tmux("kill-pane", "-t", preview)
            wait_for(lambda: not any(endpoint.iterdir()), "closed tmux pane left a Preview endpoint")
            result = subprocess.run(attach, input=b"", capture_output=True, env=environment, timeout=8)
            assert result.returncode == 5 and not result.stdout, result.stderr
        else:
            interrupt = (lambda: tmux("kill-pane", "-t", preview)) if kill_active else None
            attach_client(attach, client, source / "main.lua", digest, work, environment, interrupt)
        wait_for(lambda: not any(endpoint.iterdir()), "Preview did not clean its endpoint")
        marker(ordinary, "AFTER")
        wait_for(lambda: tmux("list-panes", "-t", ordinary, "-F", "#{pane_id}").decode().strip() == ordinary,
                 "the ordinary terminal was not the only remaining pane")
    finally:
        subprocess.run([*command, "kill-server"], capture_output=True, timeout=5, env=environment)
    route = "SSH" if ssh_prefix else "local"
    scope = ("pending Lua RPC stayed unconfirmed after killing the active pane" if kill_active else
             "stale attachment refused after killing the Preview pane" if kill_pane
             else "no-TTY attachment and resource bytes")
    print(f"LUA TMUX PREVIEW PASS: {route}, {scope}, sibling shell and cleanup")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gterm", type=Path)
    parser.add_argument("client", type=Path, help="Lua-enabled Core test_lua_preview executable")
    parser.add_argument("--ssh-hops", type=int, choices=(1, 2, 3), help="attach through owned loopback SSH servers")
    fault = parser.add_mutually_exclusive_group()
    fault.add_argument("--kill-pane", action="store_true", help="close the owned plugin pane before attaching")
    fault.add_argument("--kill-active-pane", action="store_true", help="close the owned pane while its Lua RPC is pending")
    args = parser.parse_args()
    tmux = shutil.which("tmux")
    if not tmux:
        parser.error("tmux is required for this explicit test")
    tmp = ROOT / "tmp"
    parent = Path(os.environ.get("TMPDIR", tmp)).resolve()
    if not parent.is_dir() or not parent.is_relative_to(tmp):
        parser.error("TMPDIR must be an existing project temporary directory")
    with tempfile.TemporaryDirectory(prefix="mux-", dir=parent) as directory:
        work = Path(directory)
        transport = ssh_transport(work, args.ssh_hops) if args.ssh_hops else nullcontext()
        with transport as ssh_prefix:
            exercise(work, args.gterm.resolve(strict=True), args.client.resolve(strict=True), tmux, ssh_prefix,
                     args.kill_pane, args.kill_active_pane)


if __name__ == "__main__":
    main()
