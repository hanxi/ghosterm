#!/usr/bin/env python3
"""Run the real QuickJS gterm Executor over disposable two/three-hop SSH."""
from pathlib import Path
import argparse
import hashlib
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


def package_at(directory):
    package = directory / "package"
    package.mkdir()
    manifest = {
        "schemaVersion": 1,
        "id": "org.ghosterm.fixture",
        "pluginVersion": "0.1.0",
        "sdkApiRange": "^1.0.0",
        "bridgeProtocolRange": [1],
        "publisher": "fixture",
        "license": "MIT",
        "commands": [{"path": ["fixture"], "title": "Fixture", "argsSchema": "schemas/input.json"}],
        "executor": {"variants": [{"id": "portable", "runtime": "portable-js",
                                      "entry": "executor/index.js", "requires": ["session.rpc"]}]},
        "permissions": [],
    }
    schema = {"type": "object", "properties": {}, "required": [], "additionalProperties": False}
    source = (
        "import {object,text,integer} from '@ghosterm/plugin-sdk';"
        "export async function activate(ctx){"
        "if(ctx.invocation.command!=='fixture'||ctx.invocation.arguments.length!==0)throw Error('invocation');"
        "const codec=object({text:text(40),count:integer(0,100)});"
        "ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{c.check();return v});"
        "const call=ctx.session.request({name:'fixture.ask',input:codec,output:codec},"
        "{text:'from-executor',count:17},10000);const reply=await call.outcome;"
        "if(reply.kind!=='success'||reply.value.text!=='from-executor'||reply.value.count!==17)"
        "throw Error('host RPC');return {dispose(){}}}"
    )
    files = {
        "plugin.json": json.dumps(manifest, separators=(",", ":")),
        "README.md": "Disposable SSH integration fixture.\n",
        "LICENSES/MIT.txt": "MIT\n",
        "schemas/input.json": json.dumps(schema, separators=(",", ":")),
        "executor/index.js": source,
    }
    members = []
    for name, content in files.items():
        path = package / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        data = path.read_bytes()
        members.append({"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    integrity = json.dumps({"schemaVersion": 1, "members": members}, separators=(",", ":")).encode()
    (package / "integrity.json").write_bytes(integrity)
    return package, hashlib.sha256(integrity).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-binary", type=Path, required=True,
                        help="libssh2-enabled core/build/test_ext_rpc")
    parser.add_argument("--gterm", type=Path, required=True, help="QuickJS-enabled gterm executable")
    parser.add_argument("--case", choices=("2", "3"), action="append")
    args = parser.parse_args()
    host_binary, gterm = args.host_binary.resolve(), args.gterm.resolve()
    if not host_binary.is_file() or not gterm.is_file():
        raise SystemExit("both host runner and gterm must exist")
    work = Path(tempfile.mkdtemp(prefix="gterm-ssh-rpc-", dir=ROOT / "tmp"))
    streams = load("gterm_ssh_streams", ROOT / "scripts/test-plugin-ssh-stream.py")
    keys_helper = load("gterm_ssh_keys", ROOT / "scripts/test-plugin-ssh.py")
    routes = load("gterm_ssh_routes", ROOT / "scripts/test-plugin-route.py")
    private, public = streams.memory_identity()
    package, executor_digest = package_at(work)
    client_digest = "d" * 64
    env = {**os.environ, "TMPDIR": str(work), "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
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
                    bootstrap = shlex.join([
                        str(gterm), "relay", "--stdio", "--trust-development-digest", executor_digest,
                        "--client-digest", client_digest, "--method", "fixture.echo",
                        "--host-method", "fixture.ask", "--", str(package), "fixture",
                    ])
                    command = [str(host_binary), "gterm", str(count), pwd.getpwuid(os.getuid()).pw_name,
                               str(known_hosts), bootstrap, client_digest, executor_digest,
                               *map(str, ports)]
                    completed = subprocess.run(command, input=struct.pack(">I", len(private)) + private,
                                               capture_output=True, timeout=45, env=env, cwd=ROOT)
                    output = (completed.stdout + completed.stderr).decode(errors="replace")
                    if completed.returncode or f"PASS gterm bidirectional RPC over SSH ({count} hops)" not in output:
                        raise RuntimeError(f"gterm SSH {count}-hop fixture failed ({completed.returncode}):\n{output}")
                    print(f"PASS QuickJS gterm bidirectional RPC over {count}-hop SSH", flush=True)
    finally:
        # The owner started every fixture child in its own group; context exit
        # stops them before this workspace (keys, package and logs) is removed.
        import shutil
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
