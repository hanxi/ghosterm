#!/usr/bin/env python3
"""One native-owned gterm process, real mux/Session/JS owners and private files.

No user accounts, model calls, SSH service, device or foreground windows.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def profile(arguments):
    data = bytearray([1]) + len(arguments).to_bytes(2, "big")
    for argument in arguments:
        raw = str(argument).encode()
        data += len(raw).to_bytes(2, "big") + raw
    return data.hex()


def package(work):
    directory = work / "package"
    directory.mkdir()
    names = ["slot", "unicode", "empty", "literal"]
    schema = dict(type="object", properties={name: dict(type="string", maxLength=100) for name in names},
        required=names, additionalProperties=False,
        **{"x-ghosterm-cli": dict(version=1, fields={name: dict(position=i) for i, name in enumerate(names)})})
    manifest = dict(schemaVersion=1, id="org.ghosterm.mux-fixture", pluginVersion="0.1.0",
        sdkApiRange="^1.0.0", bridgeProtocolRange=[1], publisher="owned-fixture", license="MIT",
        commands=[dict(path=["fixture"], title="Fixture", argsSchema="schemas/input.json")],
        executor=dict(variants=[dict(id="portable", runtime="portable-js", entry="executor/index.js", requires=["session.rpc"])]), permissions=[])
    code = """import {object,text,integer} from '@ghosterm/plugin-sdk';
let calls=0;
export function activate(ctx){
 if(ctx.invocation.arguments[0]==='denied')throw Error('denied scope must never activate');
 const args=ctx.invocation.arguments,owner=Number(args[0]);
 if(args.length!==4||owner<0||owner>7||args[1]!=='中文'||args[2]!==''||args[3]!=="';throw Error('injected');//")throw Error('argv');
 ctx.session.register({name:'fixture.echo',input:object({text:text(40),count:integer(0,100)}),
  output:object({text:text(40),count:integer(0,100),owner:integer(0,7),calls:integer(1,10)})},
  async(v,c)=>{c.check();calls++;return {...v,owner,calls}});
 return {dispose(){}};
}
"""
    files = {"plugin.json": json.dumps(manifest), "schemas/input.json": json.dumps(schema),
        "executor/index.js": code, "README": "owned mux fixture", "LICENSES/MIT.txt": "owned test fixture"}
    members = []
    for name, value in files.items():
        p = directory / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(value)
        raw = p.read_bytes()
        members.append(dict(path=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
    integrity = directory / "integrity.json"
    integrity.write_text(json.dumps(dict(schemaVersion=1, members=members)))
    return directory, hashlib.sha256(integrity.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--case", choices=["roundtrip", "denied", "resources", "cancel", "preflight", "live"])
    parser.add_argument("--disabled", action="store_true")
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix="gterm-multiplex-", dir=ROOT / "tmp"))
    try:
        runtime_tmp = work / "runtime"
        runtime_tmp.mkdir(mode=0o700)
        directory, digest = package(work)
        profiles = [profile(["--trust-development-digest", digest, "--client-digest", "d" * 64,
            "--method", "fixture.echo", "--", directory, "fixture", str(i), "中文", "", "';throw Error('injected');//"]) for i in range(8)]
        env = {**os.environ, "TMPDIR": str(runtime_tmp)}

        def run(command, expected=0, timeout=40):
            result = subprocess.run(list(map(str, command)), input=b"", capture_output=True, env=env, timeout=timeout)
            assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
            assert not any(marker in result.stderr for marker in
                [b"AddressSanitizer", b"UndefinedBehaviorSanitizer", b"runtime error:"]), result.stderr
            return result

        if args.disabled:
            command = [build / "gterm", "relay", "--multiplex", "--connection", "a" * 32]
            for i in range(8):
                command += ["--instance", f"{i+1:032x}", str(i+1), profiles[i]]
            assert run(command, 4).stdout == b""
            assert not list(runtime_tmp.iterdir())
            print("PASS disabled multiplex: explicit unsupported, no protocol or temporary owners")
            return
        for case in [args.case] if args.case else ["roundtrip", "denied", "resources", "cancel", "preflight", "live"]:
            if case == "preflight":
                base = [build / "gterm", "relay", "--multiplex", "--connection", "a" * 32]
                group = ["--instance", "1" * 32, "1", profiles[0]]
                malformed = [([], 2), (["--instance"], 2),
                    (["--instance", "0" * 32, "1", profiles[0]], 2),
                    (["--instance", "1" * 32, "01", profiles[0]], 2),
                    (["--instance", "1" * 32, "18446744073709551616", profiles[0]], 2),
                    (["--instance", "1" * 32, "1", "010001000161ff"], 2),
                    (["--instance", "1" * 32, "1", "010001000100"], 2),
                    (["--instance", "1" * 32, "1", "0100010001ff"], 2),
                    (["--instance", "1" * 32, "1", "0100010003ff"], 2),
                    (["--instance", "1" * 32, "1", "01FF"], 2),
                    (group * 2, 2), (group + ["--instance", "2" * 32, "1", profiles[1]], 2)]
                nine = []
                for i in range(9):
                    nine += ["--instance", f"{i+1:032x}", str(i+1), profiles[i % 8]]
                malformed.append((nine, 2))
                changed = profile(["--trust-development-digest", "0" * 64, "--client-digest", "d" * 64,
                    "--method", "fixture.echo", "--", directory, "fixture", "7", "中文", "", "';throw Error('injected');//"])
                malformed.append((group + ["--instance", "2" * 32, "2", changed], 6))
                for extra, status in malformed:
                    result = run(base + extra, status)
                    assert result.stdout == b"", "preflight must not emit a Session or run a module"
                    assert not list(runtime_tmp.iterdir()), "preflight retained files"
                print(f"PASS multiplex preflight: {len(malformed)} explicit rejections", flush=True)
                continue
            if case == "live":
                result = run([build / "test_gterm_live", build / "gterm", "org.ghosterm.mux-fixture", digest, runtime_tmp, *profiles], timeout=70)
                assert b"PASS live multiplex" in result.stdout, result.stdout
                assert not list(runtime_tmp.iterdir()), "live server retained resource or staging owners"
                print(result.stdout.decode(), end="", flush=True)
                continue
            selected, sha, plugin_id = profiles, digest, "org.ghosterm.mux-fixture"
            if case == "denied":
                selected = list(profiles)
                selected[3] = profile(["--trust-development-digest", digest, "--client-digest", "d" * 64,
                    "--method", "fixture.echo", "--", directory, "fixture", "denied", "中文", "", "';throw Error('injected');//"])
            if case == "resources":
                preview = work / "preview"
                shutil.copytree(ROOT / "plugins/official/preview", preview)
                sha = hashlib.sha256((preview / "integrity.json").read_bytes()).hexdigest()
                plugin_id = "org.ghosterm.preview"
                selected = []
                for i in range(8):
                    source = work / f"workspace-{i}"
                    source.mkdir()
                    (source / "preview.md").write_text(f"# mux slot {i}\n")
                    selected.append(profile(["--trust-development-digest", sha, "--client-digest", "d" * 64,
                        "--root", "workspace", source, "--", preview, "preview", "preview.md"]))
            result = run([build / "test_gterm_multiplex", build / "gterm", case, plugin_id, sha, runtime_tmp, *selected])
            assert b"PASS gterm multiplex" in result.stdout, result.stdout
            assert not list(runtime_tmp.iterdir()), "runtime left resource/staging owners"
            print(result.stdout.decode(), end="", flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
