#!/usr/bin/env python3
"""Check the pinned Codex app-server protocol against the installed official CLI."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "plugins/official/ai-codex/protocol/app-server.lock.json"


def run(command, *, env, timeout=60):
    return subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True,
                          timeout=timeout, check=True).stdout


def methods(path):
    return set(re.findall(r'"method": "([^"]+)"', path.read_text(encoding="utf-8")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codex", default="codex", help="official Codex CLI executable")
    args = parser.parse_args()
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    version = run([args.codex, "--version"], env=None).strip()
    if version != lock["codexCliVersion"]:
        raise SystemExit(f"Codex protocol version mismatch: expected {lock['codexCliVersion']}, got {version}")

    work = Path(tempfile.mkdtemp(prefix="codex-protocol-", dir=ROOT / "tmp"))
    try:
        env = dict(os.environ)
        env["TMPDIR"] = str(ROOT / "tmp")
        types = work / "types"
        schema = work / "schema"
        run([args.codex, "app-server", "generate-ts", "--out", str(types)], env=env)
        run([args.codex, "app-server", "generate-json-schema", "--out", str(schema)], env=env)

        surfaces = {
            "clientRequests": types / "ClientRequest.ts",
            "clientNotifications": types / "ClientNotification.ts",
            "serverRequests": types / "ServerRequest.ts",
            "serverNotifications": types / "ServerNotification.ts",
        }
        for key, path in surfaces.items():
            actual = methods(path)
            missing = sorted(set(lock[key]) - actual)
            if missing:
                raise SystemExit(f"{path.name} from {version} is missing locked methods: {', '.join(missing)}")

        type_paths = {
            "InitializeParams": types / "InitializeParams.ts",
            "ThreadStartParams": types / "v2/ThreadStartParams.ts",
            "TurnStartParams": types / "v2/TurnStartParams.ts",
            "TurnInterruptParams": types / "v2/TurnInterruptParams.ts",
            "CommandExecutionRequestApprovalParams": types / "v2/CommandExecutionRequestApprovalParams.ts",
            "FileChangeRequestApprovalParams": types / "v2/FileChangeRequestApprovalParams.ts",
            "ServerRequestResolvedNotification": types / "v2/ServerRequestResolvedNotification.ts",
        }
        for name, required in lock["requiredFields"].items():
            text = type_paths[name].read_text(encoding="utf-8")
            missing = [field for field in required if not re.search(rf"\b{re.escape(field)}\s*:", text)]
            if missing:
                raise SystemExit(f"{name} from {version} is missing locked fields: {', '.join(missing)}")

        generated_schemas = list(schema.glob("*.schemas.json"))
        definitions = set()
        for path in generated_schemas:
            definitions.update(json.loads(path.read_text(encoding="utf-8")).get("definitions", {}))
        required_definitions = set(type_paths) | {"ClientRequest", "ClientNotification", "ServerRequest", "ServerNotification"}
        if not generated_schemas or not required_definitions <= definitions:
            missing = sorted(required_definitions - definitions)
            raise SystemExit("Codex app-server schema generator is missing locked definitions: " + ", ".join(missing))
        if not definitions:
            raise SystemExit("Codex app-server schema generator produced no definitions")
        print("CODEX APP-SERVER PROTOCOL PASS: " + version +
              "; stable initialize/thread/turn, stream events, and approval callbacks match the lock")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.stderr.write(error.stderr or str(error) + "\n")
        raise SystemExit(error.returncode or 1)
