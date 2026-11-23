#!/usr/bin/env python3
"""Check that Lua startup arguments can exceed the RPC body limit."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DECODER = Path("src/extension/package-arguments-lua.inc")
ANCHOR = b"char json[GT_PLUGIN_ARGUMENT_JSON_MAX + 1];"
MUTATION = b"char json[GT_EXT_RPC_BODY_MAX + 1];"


def main():
    parent = ROOT / "tmp"
    parent.mkdir(exist_ok=True)
    original = (ROOT / "core" / DECODER).read_bytes()
    assert original.count(ANCHOR) == 1
    digest = hashlib.sha256(original).hexdigest()

    with tempfile.TemporaryDirectory(dir=parent, prefix="agent-") as directory:
        work = Path(directory)
        env = dict(os.environ, TMPDIR=directory, TMP=directory, TEMP=directory)
        source, build, package = (work / name for name in ("core", "build", "diff"))
        decoder = source / DECODER
        shutil.copytree(ROOT / "core", source, ignore=shutil.ignore_patterns("build", "build-*", "__pycache__"))
        archive = os.environ.get("GHOSTERM_LUA_ARCHIVE")
        if archive:
            archive = Path(archive).resolve(strict=True)
        else:
            archive = work / "lua.tar.gz"
            subprocess.run(["curl", "-fsSL", "--retry", "2", "https://www.lua.org/ftp/lua-5.5.1.tar.gz",
                            "-o", archive], env=env, check=True, timeout=30)
        probe = work / "probe"

        def run(*command, expected=0):
            result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=90)
            if result.returncode != expected:
                raise RuntimeError(result.stdout + result.stderr)
            return result

        def compile_probe():
            run("cmake", "--build", build, "--target", "ghosterm_core", "test_config", "--parallel", "4")
            run("cc", "-I" + str(source / "include"), ROOT / "scripts/probes/plugin-diff.c",
                build / "libghosterm_core.a", build / "libghosterm_lua.a", "-lm", "-pthread", "-o", probe)

        def green():
            run(sys.executable, "-B", ROOT / "scripts/test-plugin-diff.py",
                "--probe", probe, "--package", package)

        run("cmake", "-S", source, "-B", build, "-DGHOSTERM_WITH_LIBSSH2=OFF",
            "-DGHOSTERM_WITH_PLUGIN_LUA=ON", f"-DGHOSTERM_LUA_ARCHIVE={archive}")
        run(sys.executable, "-B", ROOT / "scripts/gterm-plugin", "build",
            ROOT / "plugins/official/diff", "--out", package)
        compile_probe()
        green()
        try:
            decoder.write_bytes(original.replace(ANCHOR, MUTATION))
            compile_probe()
            result = run(probe, package, "executor", "wide", expected=1)
            assert result.stderr.splitlines() == ["ASSERT: actual diff Runtime adoption"], result.stderr
            run(probe, package, "executor", "change")
            run(build / "test_config", ROOT / "core/tests/fixtures", work)
        finally:
            decoder.write_bytes(original)
        compile_probe()
        green()
        assert hashlib.sha256(decoder.read_bytes()).hexdigest() == digest
        assert hashlib.sha256((ROOT / "core" / DECODER).read_bytes()).hexdigest() == digest
    assert not work.exists()
    print("LUA ARGUMENT LIMIT MUTATION PASS: wide input red, narrow and configuration controls green, restored")


if __name__ == "__main__":
    main()
