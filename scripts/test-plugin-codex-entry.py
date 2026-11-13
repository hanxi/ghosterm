#!/usr/bin/env python3
"""Type-check and exercise the official Codex Executor entry with an isolated fake Host."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(label, command, env=None):
    result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    if result.returncode:
        raise RuntimeError(label + " failed:\n" + result.stdout)
    print(result.stdout, end="")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="allow npm to fetch the locked test tools")
    args = parser.parse_args()
    tmp = ROOT / "tmp"
    tmp.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="codex-entry-", dir=tmp))
    try:
        tools = work / "tools"
        tools.mkdir()
        fixture = ROOT / "scripts/fixtures/plugin-sdk-tools"
        for name in ("package.json", "package-lock.json"):
            shutil.copyfile(fixture / name, tools / name)
        install = ["npm", "ci", "--prefix", str(tools), "--ignore-scripts", "--no-audit", "--no-fund",
                   "--cache", str(work / "npm-cache"), "--registry", "https://registry.npmjs.org"]
        if not args.fetch:
            install.append("--offline")
        run("locked TypeScript install", install)

        tsc = tools / "node_modules/typescript/bin/tsc"
        run("Codex Executor strict TypeScript", ["node", str(tsc), "-p",
            str(ROOT / "plugins/official/ai-codex/tsconfig.json")])
        sdk = work / "sdk"
        sdk.mkdir()
        run("portable SDK emission", ["node", str(tsc), "--target", "ES2022", "--module", "ES2022",
            "--moduleResolution", "Bundler", "--lib", "ES2022", "--strict", "--exactOptionalPropertyTypes",
            "--noEmitOnError", "--outDir", str(sdk),
            str(ROOT / "packages/plugin-sdk/portable/index.ts")])
        env = dict(os.environ)
        env["GHOSTERM_TEST_PLUGIN_SDK_ENTRY"] = str(sdk / "index.js")
        run("Codex Executor entry behavior", ["node", "--experimental-strip-types", "--experimental-loader",
            str(ROOT / "scripts/plugin-sdk-test-loader.mjs"),
            str(ROOT / "scripts/test-plugin-codex-entry.mjs")], env)
        print("CODEX EXECUTOR ENTRY TEST PASS")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except BaseException as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
