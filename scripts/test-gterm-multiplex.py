#!/usr/bin/env python3
"""One native-owned gterm process, real mux/Session/Lua owners and private files.

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
    manifest = dict(schemaVersion=2, id="org.ghosterm.mux-fixture", pluginVersion="0.1.0",
        sdkApiRange="^1.0.0", bridgeProtocolRange=[1], publisher="owned-fixture", license="MIT",
        commands=[dict(path=["fixture"], title="Fixture", argsSchema="schemas/input.json")],
        executor=dict(entry="executor.lua", requires=["session.rpc"], rpc=["fixture.echo"]))
    code = """local ctx = ...
local args = ctx.arguments
local owner = tonumber(args.slot)
assert(owner and owner >= 0 and owner <= 7)
assert(args.unicode == "中文" and args.empty == "" and args.literal == "';throw Error('injected');//")
local calls = 0
return {
  ["fixture.echo"] = function(input)
    calls = calls + 1
    return {text = input.text, count = input.count, owner = owner, calls = calls}
  end,
}
"""
    files = {"manifest.json": json.dumps(manifest), "schemas/input.json": json.dumps(schema),
        "executor.lua": code, "README": "owned mux fixture", "LICENSES/MIT.txt": "owned test fixture"}
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
    parser.add_argument("--case", choices=["roundtrip", "denied", "resources", "table-projects", "cancel", "preflight", "live"])
    parser.add_argument("--mutate-table", action="store_true", help="verify project data and stale-page assertions")
    parser.add_argument("--disabled", action="store_true")
    args = parser.parse_args()
    if args.mutate_table and (args.case != "table-projects" or args.disabled):
        parser.error("--mutate-table requires --case table-projects in the Lua build")
    build = args.build.resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix="gterm-multiplex-", dir=os.environ["TMPDIR"]))
    try:
        runtime_tmp = work / "runtime"
        runtime_tmp.mkdir(mode=0o700)
        directory, digest = package(work)
        profiles = [profile(["--trust-development-digest", digest, "--client-digest", "d" * 64,
            "--method", "fixture.echo", "--", directory, "fixture", str(i), "中文", "", "';throw Error('injected');//"]) for i in range(8)]
        env = {**os.environ, "TMPDIR": str(runtime_tmp), "TMP": str(runtime_tmp), "TEMP": str(runtime_tmp)}

        def run(command, expected=0, timeout=40):
            result = subprocess.run(list(map(str, command)), input=b"", capture_output=True, env=env, timeout=timeout)
            assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
            assert not any(marker in result.stderr for marker in
                [b"AddressSanitizer", b"UndefinedBehaviorSanitizer", b"runtime error:"]), result.stderr
            return result

        def table_projects(source, label, assertion=None):
            table = work / ("table-" + label)
            run(["python3", "-B", ROOT / "scripts/gterm-plugin", "build", source, "--out", table])
            digest = hashlib.sha256((table / "integrity.json").read_bytes()).hexdigest()
            selected = [profile(["--trust-development-digest", digest, "--client-digest", "d" * 64,
                "--method", "view.table.describe", "--method", "view.table.page", "--",
                table, "table", "--project", "sample-a" if i % 2 == 0 else "sample-b"]) for i in range(8)]
            result = run([build / "test_gterm_multiplex", build / "gterm", "table-projects",
                          "org.ghosterm.table", digest, runtime_tmp, *selected], expected=int(assertion is not None))
            if assertion is not None:
                assert result.stderr.count(b"FAIL ") == 1 and assertion in result.stderr, result.stderr
            else:
                assert b"PASS gterm multiplex table-projects" in result.stdout and not list(runtime_tmp.iterdir())
                print(result.stdout.decode(), end="", flush=True)
            return digest

        if args.disabled:
            command = [build / "gterm", "relay", "--multiplex", "--connection", "a" * 32]
            for i in range(8):
                command += ["--instance", f"{i+1:032x}", str(i+1), profiles[i]]
            assert run(command, 4).stdout == b""
            assert not list(runtime_tmp.iterdir())
            print("PASS disabled multiplex: explicit unsupported, no protocol or temporary owners")
            return
        for case in [args.case] if args.case else ["roundtrip", "denied", "resources", "table-projects", "cancel", "preflight", "live"]:
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
            if case == "table-projects":
                source = ROOT / "plugins/official/table"
                baseline = table_projects(source, "baseline")
                if args.mutate_table:
                    original = (source / "executor.lua").read_bytes()
                    mutations = [
                        ("wrong-project", b"local tasks = projects[project]",
                         b'local tasks = projects["sample-a"]', b"expected_ids"),
                        ("old-page", b"input.viewId ~= description.viewId\n"
                         b"      or input.revision ~= description.revision or ", b"", b"stale.state"),
                    ]
                    for label, old, new, assertion in mutations:
                        candidate = work / label
                        shutil.copytree(source, candidate)
                        assert original.count(old) == 1, label
                        (candidate / "executor.lua").write_bytes(original.replace(old, new, 1))
                        assert table_projects(candidate, label, assertion) != baseline
                        run([build / "core/test_config", ROOT / "core/tests/fixtures"])
                        (candidate / "executor.lua").write_bytes(original)
                        assert table_projects(candidate, label + "-restored") == baseline
                        assert (source / "executor.lua").read_bytes() == original
                        print(f"PASS {label}: assertion / same Core control / restore / SHA256", flush=True)
                continue
            if case == "resources":
                preview = work / "preview"
                run(["python3", "-B", ROOT / "scripts/gterm-plugin", "build",
                     ROOT / "plugins/official/preview", "--out", preview])
                sha = hashlib.sha256((preview / "integrity.json").read_bytes()).hexdigest()
                plugin_id = "org.ghosterm.preview"
                selected = []
                for i in range(8):
                    source = work / f"workspace-{i}"
                    source.mkdir()
                    (source / "preview.md").write_text(f"# mux slot {i}\n")
                    selected.append(profile(["--trust-development-digest", sha, "--client-digest", "d" * 64,
                        "--method", "preview.open", "--root", "workspace", source,
                        "--", preview, "preview", "preview.md"]))
            result = run([build / "test_gterm_multiplex", build / "gterm", case, plugin_id, sha, runtime_tmp, *selected])
            assert b"PASS gterm multiplex" in result.stdout, result.stdout
            assert not list(runtime_tmp.iterdir()), "runtime left resource/staging owners"
            print(result.stdout.decode(), end="", flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
