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
    version_source = (LOCK.parent / "version.ts").read_text(encoding="utf-8")
    version_match = re.search(r'^export const CODEX_CLI_VERSION = "([^"]+)";$',
                              version_source, re.MULTILINE)
    if not version_match or version_match.group(1) != lock.get("codexCliVersion"):
        raise SystemExit("Codex runtime version constant differs from the app-server lock")
    if lock.get("serverEnvelope", {}).get("jsonrpcVersionField") not in ("omitted", "2.0"):
        raise SystemExit("app-server lock has an unsupported server jsonrpc version-field policy")
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
            "ThreadStartResponse": types / "v2/ThreadStartResponse.ts",
            "TurnStartParams": types / "v2/TurnStartParams.ts",
            "TurnInterruptParams": types / "v2/TurnInterruptParams.ts",
            "CommandExecutionRequestApprovalParams": types / "v2/CommandExecutionRequestApprovalParams.ts",
            "FileChangeRequestApprovalParams": types / "v2/FileChangeRequestApprovalParams.ts",
            "CommandExecutionRequestApprovalResponse": types / "v2/CommandExecutionRequestApprovalResponse.ts",
            "FileChangeRequestApprovalResponse": types / "v2/FileChangeRequestApprovalResponse.ts",
            "ServerRequestResolvedNotification": types / "v2/ServerRequestResolvedNotification.ts",
            "CommandExecutionApprovalDecision": types / "v2/CommandExecutionApprovalDecision.ts",
            "FileChangeApprovalDecision": types / "v2/FileChangeApprovalDecision.ts",
        }
        for name, required in lock["requiredFields"].items():
            text = type_paths[name].read_text(encoding="utf-8")
            missing = [field for field in required if not re.search(rf"\b{re.escape(field)}\s*:", text)]
            if missing:
                raise SystemExit(f"{name} from {version} is missing locked fields: {', '.join(missing)}")

        for name, required in lock["requiredDecisions"].items():
            text = type_paths[name].read_text(encoding="utf-8")
            match = re.search(rf"export type {re.escape(name)}\s*=\s*(.*?);", text, re.DOTALL)
            if not match:
                raise SystemExit(f"{name} from {version} is not an exported type alias")
            definition = match.group(1)
            actual = set(re.findall(r'"([^"]+)"', definition))
            actual.update(re.findall(r'\{\s*"([^"]+)"\s*:', definition))
            if actual != set(required):
                missing = sorted(set(required) - actual)
                unlocked = sorted(actual - set(required))
                detail = []
                if missing:
                    detail.append("missing " + ", ".join(missing))
                if unlocked:
                    detail.append("unlocked " + ", ".join(unlocked))
                raise SystemExit(f"{name} from {version} decision lock mismatch: {'; '.join(detail)}")

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
              "; stable initialize/thread/turn, stream events, approval callbacks, and exact decision unions match the lock")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.stderr.write(error.stderr or str(error) + "\n")
        raise SystemExit(error.returncode or 1)
