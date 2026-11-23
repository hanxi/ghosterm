#!/usr/bin/env python3
"""Verify Lua budget assertions in an isolated Core build."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VM = Path("src/extension/lua-vm.c")
JSON = Path("src/extension/lua-json.c")
MUTATIONS = {
    "encoding": [
        ("    lua_settop(state, top);\n    if (success) {",
         "    if (success) lua_settop(state, top);\n    if (success) {"),
    ],
    "cpu": [
        ("if (cpu_now() >= vm->deadline_ns)", "if (cpu_now() == UINT64_MAX)"),
    ],
    "memory": [
        ("new_size > vm->limit - (vm->bytes - old_size)", "vm->bytes < old_size"),
    ],
    "global": [
        ("#include <string.h>", "#include <stdlib.h>\n#include <string.h>"),
        ("gt__plugin_memory_alloc(NULL, new_size)", "malloc(new_size)"),
        ("gt__plugin_memory_free(NULL, ptr, old_size);", "free(ptr);"),
    ],
}


def main():
    parent = ROOT / "tmp"
    parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=parent, prefix="agent-") as directory:
        work = Path(directory)
        env = dict(os.environ, TMPDIR=directory, TMP=directory, TEMP=directory)
        source, build = work / "core", work / "build"
        shutil.copytree(ROOT / "core", source, ignore=shutil.ignore_patterns("build", "build-*", "__pycache__"))
        archive = work / "lua-5.5.1.tar.gz"
        supplied = os.environ.get("GHOSTERM_LUA_ARCHIVE")
        if supplied:
            archive = Path(supplied).resolve(strict=True)
        else:
            subprocess.run(["curl", "-fsSL", "https://www.lua.org/ftp/lua-5.5.1.tar.gz", "-o", archive],
                           env=env, check=True, timeout=20)

        def run(*command):
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise RuntimeError(f"{command[0]} failed\n{result.stdout}{result.stderr}")
            return result

        run("cmake", "-S", source, "-B", build, "-DGHOSTERM_WITH_LIBSSH2=OFF",
            "-DGHOSTERM_WITH_PLUGIN_LUA=ON", f"-DGHOSTERM_LUA_ARCHIVE={archive}")
        executable = build / "test_lua_budget"

        def compile_budget():
            run("cmake", "--build", build, "--target", "test_lua_budget", "--clean-first", "--parallel", "4")

        compile_budget()
        run(executable, "all")
        for mode, replacements in MUTATIONS.items():
            path = JSON if mode == "encoding" else VM
            original = (source / path).read_bytes()
            mutated = original.decode()
            for old, new in replacements:
                assert old in mutated, (mode, old)
                mutated = mutated.replace(old, new)
            try:
                (source / path).write_text(mutated)
                compile_budget()
                observed = subprocess.run([executable, mode], env=env, capture_output=True, text=True, timeout=10)
                assert observed.returncode == 1 and observed.stderr.startswith("Lua budget assertion at line "), observed
                run(executable, "control")
                print(f"{mode}: target assertion red, same binary control green", flush=True)
            finally:
                (source / path).write_bytes(original)
            compile_budget()
            run(executable, mode)
            assert hashlib.sha256((source / path).read_bytes()).digest() == hashlib.sha256(original).digest()
            assert (ROOT / "core" / path).read_bytes() == original
    assert not work.exists()
    print(f"LUA BUDGET MUTATIONS PASS: {len(MUTATIONS)}")


if __name__ == "__main__":
    main()
