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
            "ExecPolicyAmendment": types / "v2/ExecPolicyAmendment.ts",
            "NetworkPolicyAmendment": types / "v2/NetworkPolicyAmendment.ts",
            "NetworkPolicyRuleAction": types / "v2/NetworkPolicyRuleAction.ts",
            "UserInput": types / "v2/UserInput.ts",
            "TextElement": types / "v2/TextElement.ts",
            "ByteRange": types / "v2/ByteRange.ts",
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

        for name, expected in lock.get("lockedTypeExpressions", {}).items():
            text = re.sub(r"/\*.*?\*/", "", type_paths[name].read_text(encoding="utf-8"), flags=re.DOTALL)
            match = re.search(rf"export type {re.escape(name)}\s*=\s*(.*?);", text, re.DOTALL)
            if not match:
                raise SystemExit(f"{name} from {version} is not an exported type alias")
            actual = re.sub(r"\s+", "", re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.DOTALL))
            if actual != expected:
                raise SystemExit(f"{name} from {version} differs from the pinned stable shape")

        def normalized_alias(name):
            text = re.sub(r"/\*.*?\*/", "", type_paths[name].read_text(encoding="utf-8"), flags=re.DOTALL)
            match = re.search(rf"export type {re.escape(name)}\s*=\s*(.*?);", text, re.DOTALL)
            if not match:
                raise SystemExit(f"{name} from {version} is not an exported type alias")
            return re.sub(r"\s+", "", match.group(1))

        for name, declarations in lock.get("lockedFieldDeclarations", {}).items():
            actual = normalized_alias(name)
            missing = [declaration for declaration in declarations if declaration not in actual]
            if missing:
                raise SystemExit(f"{name} from {version} field shape mismatch: {', '.join(missing)}")

        for name, fragments in lock.get("lockedTypeFragments", {}).items():
            actual = normalized_alias(name)
            missing = [fragment for fragment in fragments if fragment not in actual]
            if missing:
                raise SystemExit(f"{name} from {version} is missing locked variants: {', '.join(missing)}")

        generated_schemas = list(schema.glob("*.schemas.json"))
        definitions = {}
        for path in generated_schemas:
            definitions.update(json.loads(path.read_text(encoding="utf-8")).get("definitions", {}))
        # TS aliases such as `Array<string>` are inlined by the official JSON
        # Schema generator and therefore have no named schema definition.
        schema_type_names = set(type_paths) - {"ExecPolicyAmendment"}
        required_definitions = schema_type_names | {"ClientRequest", "ClientNotification", "ServerRequest", "ServerNotification"}
        if not generated_schemas or not required_definitions <= set(definitions):
            missing = sorted(required_definitions - set(definitions))
            raise SystemExit("Codex app-server schema generator is missing locked definitions: " + ", ".join(missing))
        if not definitions:
            raise SystemExit("Codex app-server schema generator produced no definitions")

        def schema_require(condition, detail):
            if not condition:
                raise SystemExit(f"Codex JSON Schema from {version} shape mismatch: {detail}")

        def decision_variant(name, wrapper):
            definition = definitions.get(name, {})
            for variant in definition.get("oneOf", []):
                properties = variant.get("properties", {})
                if wrapper in properties:
                    return variant, properties[wrapper]
            schema_require(False, f"{name}.{wrapper} variant is missing")

        exec_variant, exec_payload = decision_variant(
            "CommandExecutionApprovalDecision", "acceptWithExecpolicyAmendment")
        exec_body = exec_payload.get("properties", {}).get("execpolicy_amendment", {})
        schema_require("acceptWithExecpolicyAmendment" in exec_variant.get("required", []) and
                       "execpolicy_amendment" in exec_payload.get("required", []) and
                       exec_body.get("type") == "array" and
                       exec_body.get("items", {}).get("type") == "string",
                       "acceptWithExecpolicyAmendment.execpolicy_amendment")
        network_variant, network_payload = decision_variant(
            "CommandExecutionApprovalDecision", "applyNetworkPolicyAmendment")
        network_body = network_payload.get("properties", {}).get("network_policy_amendment", {})
        schema_require("applyNetworkPolicyAmendment" in network_variant.get("required", []) and
                       "network_policy_amendment" in network_payload.get("required", []) and
                       network_body.get("$ref", "").endswith("/NetworkPolicyAmendment"),
                       "applyNetworkPolicyAmendment.network_policy_amendment")
        network_schema = definitions.get("NetworkPolicyAmendment", {})
        network_properties = network_schema.get("properties", {})
        schema_require({"host", "action"} <= set(network_schema.get("required", [])) and
                       network_properties.get("host", {}).get("type") == "string" and
                       network_properties.get("action", {}).get("$ref", "").endswith("/NetworkPolicyRuleAction") and
                       set(definitions.get("NetworkPolicyRuleAction", {}).get("enum", [])) == {"allow", "deny"},
                       "NetworkPolicyAmendment host/action")

        turn_schema = definitions.get("TurnStartParams", {})
        turn_properties = turn_schema.get("properties", {})
        schema_require({"threadId", "input"} <= set(turn_schema.get("required", [])) and
                       turn_properties.get("threadId", {}).get("type") == "string" and
                       turn_properties.get("input", {}).get("type") == "array" and
                       turn_properties.get("input", {}).get("items", {}).get("$ref", "").endswith("/UserInput"),
                       "TurnStartParams threadId/input")
        text_input = next((variant for variant in definitions.get("UserInput", {}).get("oneOf", [])
                           if variant.get("properties", {}).get("type", {}).get("enum") == ["text"]), {})
        text_properties = text_input.get("properties", {})
        schema_require(text_properties.get("text", {}).get("type") == "string" and
                       text_properties.get("text_elements", {}).get("type") == "array" and
                       text_properties.get("text_elements", {}).get("items", {}).get("$ref", "").endswith("/TextElement"),
                       "UserInput text/text_elements")
        element_properties = definitions.get("TextElement", {}).get("properties", {})
        byte_range = element_properties.get("byteRange", {})
        schema_require(byte_range.get("allOf", [{}])[0].get("$ref", "").endswith("/ByteRange") and
                       {"start", "end"} <= set(definitions.get("ByteRange", {}).get("required", [])),
                       "TextElement.byteRange")
        print("CODEX APP-SERVER PROTOCOL PASS: " + version +
              "; stable initialize/thread/turn, generated approval types, and JSON Schema payload shapes match the lock")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.stderr.write(error.stderr or str(error) + "\n")
        raise SystemExit(error.returncode or 1)
