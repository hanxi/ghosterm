#!/usr/bin/env python3
"""Run the real Lua gterm Executor over disposable two/three-hop SSH."""
from pathlib import Path
import argparse
import importlib.util
import json
import os
import pwd
import shlex
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def package_at(directory, resources=False):
    project = directory / "project"
    project.mkdir()
    package = directory / "package"
    extra = ', "resource.receive", "file.readScoped"' if resources else ''
    (project / "main.lua").write_text(f"""local manifest = {{
  id = "org.ghosterm.fixture", version = "0.1.0", api = "1",
  publisher = "Fixture", license = "MIT",
  client = {{ requires = {{ "session.rpc" }}, rpc = {{ "fixture.ask" }} }},
  executor = {{
    entry = "executor.lua", requires = {{ "session.rpc"{extra} }},
    rpc = {{ "fixture.echo" }},
  }},
  commands = {{ {{ path = {{ "fixture" }}, title = "Fixture" }} }},
}}
return {{ fixture = function() return true end }}
""")
    publish = (
        'local resource = assert(ctx.resources.publish("workspace", "fixture.txt", "text/plain", 1024))\n'
        'assert(resource.size == 18)\n'
    ) if resources else ''
    source = """local ctx = ...
local reply = assert(ctx.rpc.call("fixture.ask", { text = "from-executor", count = 17 }, 10000))
assert(reply.text == "from-executor" and reply.count == 17)
""" + publish + """
return { ["fixture.echo"] = function(input) return input end }
"""
    (project / "executor.lua").write_text(source)
    result = subprocess.check_output(
        [sys.executable, "-B", ROOT / "scripts/gterm-plugin", "build", project, "--out", package],
        text=True, timeout=30,
    )
    return package, json.loads(result)["packageSHA256"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-binary", type=Path, required=True,
                        help="libssh2-enabled core/build/test_ext_resource")
    parser.add_argument("--gterm", type=Path, required=True, help="Lua-enabled gterm executable")
    parser.add_argument("--case", choices=("2", "3"), action="append")
    parser.add_argument("--resource", action="store_true",
                        help="publish a scoped text resource through gterm over SSH")
    args = parser.parse_args()
    host_binary, gterm = args.host_binary.resolve(), args.gterm.resolve()
    if not host_binary.is_file() or not gterm.is_file():
        raise SystemExit("both host runner and gterm must exist")
    project_tmp = ROOT / "tmp"
    requested = Path(os.environ.get("TMPDIR", project_tmp)).resolve()
    scratch = requested if requested.is_dir() and requested.is_relative_to(project_tmp) else project_tmp
    work = Path(tempfile.mkdtemp(prefix="gterm-ssh-rpc-", dir=scratch))
    streams = load("gterm_ssh_streams", ROOT / "scripts/test-plugin-ssh-stream.py")
    keys_helper = load("gterm_ssh_keys", ROOT / "scripts/test-plugin-ssh.py")
    routes = load("gterm_ssh_routes", ROOT / "scripts/test-plugin-route.py")
    private, public = streams.memory_identity()
    package, executor_digest = package_at(work, resources=args.resource)
    source_root = work / "source"
    source_root.mkdir()
    (source_root / "fixture.txt").write_text("resource over SSH\n")
    client_digest = "d" * 64
    env = {**os.environ, "TMPDIR": str(work), "TMP": str(work), "TEMP": str(work), "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
           "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
    env.pop("TMUX", None)
    env.pop("TMUX_PANE", None)
    try:
        with keys_helper.owned_agent(work, "gterm-host-agent") as agent:
            keys = [keys_helper.add_memory_key(agent, f"gterm-route-hop-{i}") for i in range(3)]
            if len({key.split()[1] for key in keys}) != 3:
                raise RuntimeError("fixture SSH host identities collided")
            for hop_count in args.case or ["2", "3"]:
                count = int(hop_count)
                folder = work / f"route-{count}"
                folder.mkdir()
                with routes.chain(folder, count, public, keys, agent, "echo") as (ports, known_hosts, _):
                    bootstrap_args = [
                        "env", "TMPDIR=" + str(work), str(gterm), "relay", "--stdio",
                        "--trust-development-digest", executor_digest,
                        "--client-digest", client_digest, "--method", "fixture.echo",
                        "--host-method", "fixture.ask",
                    ]
                    if args.resource:
                        bootstrap_args.extend(["--root", "workspace", str(source_root)])
                    bootstrap_args.extend(["--", str(package), "fixture"])
                    remote_stderr = work / "gterm-executor.stderr"
                    bootstrap = shlex.join(bootstrap_args) + " 2>" + shlex.quote(str(remote_stderr))
                    command = [str(host_binary), "gterm-resource" if args.resource else "gterm",
                               str(count), pwd.getpwuid(os.getuid()).pw_name,
                               str(known_hosts), bootstrap, client_digest, executor_digest,
                               *map(str, ports)]
                    completed = subprocess.run(command, input=struct.pack(">I", len(private)) + private,
                                               capture_output=True, timeout=45, env=env, cwd=ROOT)
                    output = (completed.stdout + completed.stderr).decode(errors="replace")
                    expected = (f"PASS gterm RPC and resource transfer over SSH ({count} hops)"
                                if args.resource else
                                f"PASS gterm bidirectional RPC over SSH ({count} hops)")
                    if completed.returncode or expected not in output:
                        if remote_stderr.exists():
                            output += "\nremote gterm stderr:\n" + remote_stderr.read_text(errors="replace")
                        raise RuntimeError(f"gterm SSH {count}-hop fixture failed ({completed.returncode}):\n{output}")
                    print(f"PASS Lua gterm {'RPC and resource transfer' if args.resource else 'bidirectional RPC'} over {count}-hop SSH", flush=True)
    finally:
        # The owner started every fixture child in its own group; context exit
        # stops them before this workspace (keys, package and logs) is removed.
        import shutil
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
