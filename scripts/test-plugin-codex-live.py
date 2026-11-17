#!/usr/bin/env python3
"""Opt-in real model through Native Client Runtime/gterm/official Codex package.

Uses the existing selected CLI account; never copies credentials or changes HOME
or CODEX_HOME. Output receipts contain only scope, methods, counts and test data.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shlex
import shutil
import signal
import subprocess
import tempfile
import time
import tomllib
import uuid

ROOT = Path(__file__).resolve().parents[1]


def require(value, code):
    if not value:
        raise AssertionError(code)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def backend_arguments(workspace, scratch, mode):
    # Read configuration as data solely to disable named MCP servers. Values,
    # account information and credentials are neither copied nor printed.
    names = set()
    home = Path(os.environ.get('CODEX_HOME', str(Path.home() / '.codex')))
    paths = [home / 'config.toml']
    paths += [p / '.codex/config.toml' for p in [workspace, *workspace.parents]]
    def collect(value):
        if not isinstance(value, dict): return
        servers = value.get('mcp_servers')
        if isinstance(servers, dict): names.update(servers)
        for child in value.values():
            if isinstance(child, dict): collect(child)
    for path in paths:
        if path.is_file():
            require(path.stat().st_size <= 1048576, 'CONFIGURATION_BUDGET')
            collect(tomllib.loads(path.read_text()))
    servers = '{' + ','.join(json.dumps(name, ensure_ascii=False) + '={enabled=false}' for name in sorted(names)) + '}'
    values = [
        'mcp_servers=' + servers,
        'features={apps=false,plugins=false,hooks=false,multi_agent=false,code_mode_host=false,plugin_discovery_prefetch=false}',
        'history.persistence="none"', 'analytics.enabled=false', 'web_search="disabled"',
        'log_dir=' + json.dumps(str(scratch / 'logs')), 'model_reasoning_effort="low"',
    ]
    if mode == 'file':
        values += ['sandbox_mode="workspace-write"', 'approval_policy="on-request"', 'approvals_reviewer="user"',
            'sandbox_workspace_write={writable_roots=[],network_access=false,exclude_tmpdir_env_var=true,exclude_slash_tmp=true}']
    require(all(len(value.encode()) <= 4096 for value in values), 'CONFIGURATION_ARGUMENT_BUDGET')
    # One argv entry per override fits the existing 16-entry process prefix
    # budget without expanding the production capability contract.
    require(len(values) <= 16, 'PROCESS_PREFIX_BUDGET')
    return ['--config=' + value for value in values]


def run(args, scratch):
    workspace = scratch / 'workspace'; workspace.mkdir(mode=0o700)
    token = 'GHOSTERM_LIVE_' + uuid.uuid4().hex
    expected_path = workspace / 'probe.txt'
    if args.mode == 'file':
        target = scratch / 'approval-target'; target.mkdir(mode=0o700)
        expected_path = target / 'probe.txt'
    package = args.package.resolve(strict=True)
    codex = Path(shutil.which(args.codex) or args.codex).resolve(strict=True)
    version = json.loads((ROOT / 'plugins/official/ai-codex/protocol/app-server.lock.json').read_text())['codexCliVersion']
    environment = {**os.environ, 'TMPDIR': str(scratch), 'GHOSTERM_AUTOMATION_OFFSCREEN': '1'}
    observed = subprocess.run([codex, '--version'], capture_output=True, text=True, env=environment, timeout=10)
    require(observed.returncode == 0 and observed.stdout.strip() == version, 'PINNED_CLI_VERSION')
    authenticated = subprocess.run([codex, 'login', 'status'], capture_output=True, env=environment, timeout=15)
    require(authenticated.returncode == 0, 'SELECTED_CLI_AUTHENTICATION_UNAVAILABLE')
    if args.mode == 'file': environment['GHOSTERM_CODEX_LIVE_FILE_SCOPE'] = '1'
    prefix = backend_arguments(workspace, scratch, args.mode)
    # Validate overrides before the real app-server; never print startup data.
    validation = subprocess.run([codex, 'app-server', '--strict-config', '--print-startup-config', *prefix],
                                capture_output=True, env=environment, timeout=15)
    require(validation.returncode == 0, 'EXPLICIT_BACKEND_CONFIGURATION')
    command = [args.build / 'test_gterm_codex_peer', args.build / 'gterm', package,
               sha(package / 'integrity.json'), workspace, codex, version, sha(codex), *prefix]
    events, methods = [], {}
    event_sequence = 0
    stdout = bytearray()
    total = 0
    with (scratch / 'native-stderr.log').open('w+b') as errors:
        peer = subprocess.Popen(list(map(str, command)), cwd=workspace, env=environment,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, start_new_session=True)
        def line(timeout=40, idle_ok=False):
            nonlocal total, event_sequence
            end = time.monotonic() + timeout
            while b'\n' not in stdout:
                if time.monotonic() >= end and idle_ok: return {}
                require(time.monotonic() < end, 'NATIVE_OBSERVATION_DEADLINE')
                if select.select([peer.stdout], [], [], max(0, end - time.monotonic()))[0]:
                    data = os.read(peer.stdout.fileno(), 8192)
                    require(data, 'NATIVE_OWNER_ENDED')
                    total += len(data); require(total <= 8388608 and errors.tell() <= 2097152, 'NATIVE_OUTPUT_BUDGET')
                    stdout.extend(data)
            at = stdout.index(10); raw = bytes(stdout[:at]); del stdout[:at + 1]
            value = json.loads(raw)
            if 'event' in value:
                event = json.loads(value['event']['event'])
                require(isinstance(event.get('sequence'), int) and event['sequence'] > event_sequence, 'EVENT_SEQUENCE')
                event_sequence = event['sequence']; events.append(event)
                name = event.get('notification', {}).get('method', event.get('kind', 'unknown'))
                methods[name] = methods.get(name, 0) + 1
            return value
        def rpc(method, value):
            raw = json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode()
            require(len(raw) <= 3072, 'RPC_BODY_BUDGET')
            peer.stdin.write(method.encode() + b'\n' + raw + b'\n'); peer.stdin.flush()
            end = time.monotonic() + 40
            while time.monotonic() < end:
                reply = line(max(.01, end - time.monotonic()))
                if 'result' in reply:
                    result = reply['result']; require(result.get('kind') == 'success', 'RPC_' + method + '_' + str(result.get('kind')))
                    return result['value']
            raise AssertionError('RPC_OBSERVATION_DEADLINE')
        try:
            require(line().get('ready') is True, 'NATIVE_READY')
            thread = rpc('codex.thread.start', {})['threadID']
            policy = None; policy_deadline = time.monotonic() + 10
            while policy is None and time.monotonic() < policy_deadline:
                for event in events:
                    notice = event.get('notification', {})
                    if notice.get('method') == 'ghosterm/threadConfigured': policy = notice['params']
                if policy is None: line(1, idle_ok=True)
            expected_sandbox = 'workspaceWrite' if args.mode == 'file' else 'readOnly'
            require(policy and policy.get('sandboxType') == expected_sandbox and policy.get('approvalsReviewer') == 'user' and
                    policy.get('approvalPolicy') == 'on-request', 'OBSERVED_BACKEND_POLICY_BEFORE_MODEL')
            prompt = ('Reply with exactly ' + token + '. Do not call any tools, inspect files or use a subagent.') if args.mode == 'chat' else (
                'Use apply_patch to add exactly one file at ' + str(expected_path) + ', containing exactly ' + token +
                ' followed by one newline. This isolated target is deliberately outside the writable current workspace, so request file-change approval. '
                'The user authorizes only this exact proposed file. Do not use shell commands, read other files, create other files or use subagents. Then reply with exactly ' + token + '.')
            if args.mode == 'command':
                command = "/usr/bin/printf '%s\\n' '" + token + "' > probe.txt"
                prompt = ('Use the command execution tool to run exactly this command in the current workspace: ' + command +
                    '. The base sandbox is read-only: request approval for this exact command using sandbox_permissions=require_escalated. '
                    'Do not run other commands or use other tools. After this command succeeds reply with exactly ' + token + '.')
            turn = rpc('codex.turn.send', {'text': prompt})['turnID']
            end = time.monotonic() + 180
            cursor = approvals = 0
            documents = {}
            command_items = {}
            complete = False
            text = ''
            while time.monotonic() < end and not complete:
                if cursor >= len(events): line(min(1, max(.01, end - time.monotonic())), idle_ok=True)
                while cursor < len(events):
                    event = events[cursor]; cursor += 1
                    kind = event.get('kind')
                    if kind == 'notification':
                        notice = event['notification']; params = notice.get('params') or {}
                        method = notice['method']
                        item = params.get('item') or {}
                        if method == 'item/started' and item.get('type') == 'commandExecution':
                            command_items[item.get('id')] = item
                        if method == 'ghosterm/threadConfigured':
                            require(params.get('sandboxType') == expected_sandbox and params.get('approvalsReviewer') == 'user' and
                                    params.get('approvalPolicy') == 'on-request', 'OBSERVED_BACKEND_POLICY')
                        if method == 'item/agentMessage/delta':
                            require(params.get('threadId') == thread and params.get('turnId') == turn, 'DELTA_CORRELATION')
                            text += params.get('delta', '')
                        if method == 'item/completed' and params.get('item', {}).get('type') == 'agentMessage':
                            require(params.get('threadId') == thread and params.get('turnId') == turn, 'MESSAGE_CORRELATION')
                            text += params['item'].get('text', '')
                        marker = params.get('_ghosterm') or params.get('item', {}).get('_ghosterm') or {}
                        if marker.get('snapshotID'): documents[params.get('itemId') or params.get('item', {}).get('id')] = marker['snapshotID']
                        if method == 'turn/completed':
                            require(params.get('threadId') == thread and params['turn']['id'] == turn, 'TURN_CORRELATION')
                            require(params['turn']['status'] == 'completed', 'MODEL_TURN_' + str(params['turn']['status']))
                            complete = True
                    elif kind == 'approval-request':
                        request = event['request']; params = request['params']
                        require(params.get('threadId') == thread and params.get('turnId') == turn, 'APPROVAL_CORRELATION')
                        decision, snapshot = 'decline', ''
                        if args.mode == 'command' and request['method'] == 'item/commandExecution/requestApproval':
                            observed = command_items.get(params.get('itemId'), {})
                            cwd = params.get('cwd') or observed.get('cwd')
                            raw = params.get('command') or observed.get('command', '')
                            parsed = shlex.split(raw)
                            if len(parsed) == 3 and parsed[0] in ('/bin/sh','/bin/bash','/bin/zsh') and parsed[1] in ('-c','-lc'):
                                parsed = shlex.split(parsed[2])
                            require(cwd and Path(cwd).resolve() == workspace and
                                parsed == ['/usr/bin/printf', '%s\\n', token, '>', 'probe.txt'], 'OWNED_COMMAND_SCOPE')
                            decision = 'accept'; approvals += 1
                        if args.mode == 'file' and request['method'] == 'item/fileChange/requestApproval':
                            snapshot = documents.get(params.get('itemId'), '')
                            require(snapshot, 'PROPOSED_CHANGE_NOT_AVAILABLE')
                            parts, offset = [], 0
                            while True:
                                part = rpc('codex.change.read', {'snapshotID': snapshot, 'offset': offset})
                                require(part['status'] == 'ok' and part['offset'] == offset and part['snapshotID'] == snapshot, 'COMPLETE_CHANGE_READ')
                                parts.append(part['content']); offset = part['nextOffset']
                                if part['done']: break
                                require(part['content'], 'COMPLETE_CHANGE_PROGRESS')
                            document = json.loads(''.join(parts))
                            require(document['threadId'] == thread and document['turnId'] == turn and document['itemId'] == params['itemId'], 'CHANGE_CORRELATION')
                            changes = document['changes']; require(len(changes) == 1, 'OWNED_CHANGE_COUNT')
                            change = changes[0]
                            require((workspace / change['path']).resolve() == expected_path and change['kind']['type'] == 'add' and
                                    token in change['diff'], 'OWNED_CHANGE_SCOPE')
                            require(not params.get('grantRoot') or Path(params['grantRoot']).resolve() == expected_path.parent,
                                    'OWNED_FILE_GRANT_ROOT')
                            decision = 'accept'; approvals += 1
                        request_id = request['id']
                        request_key = ('s:' if isinstance(request_id, str) else 'n:') + str(request_id)
                        rpc('codex.approval.respond', {'requestID': request_key, 'decisionJSON': json.dumps(decision), 'snapshotID': snapshot})
                    elif kind in ('protocol-error', 'closed', 'outcome-unknown'):
                        if kind == 'protocol-error':
                            # Only the adapter's own fixed diagnostic is retained.
                            message = event.get('message', '')
                            if re.fullmatch(r'[A-Za-z0-9 /_.:-]{1,512}', message):
                                methods['adapterDiagnostic:' + message] = 1
                        raise AssertionError('MODEL_' + kind)
            require(complete and token in text, 'CORRELATED_MODEL_REPLY')
            if args.mode in ('file','command'):
                require(approvals > 0 and expected_path.is_file() and not expected_path.is_symlink() and
                        expected_path.read_text() == token + '\n', 'APPROVED_ACTUAL_FILE_CONTENT')
            else:
                require(approvals == 0 and not expected_path.exists(), 'CHAT_WITHOUT_TOOLS')
            peer.stdin.close()
            while peer.poll() is None:
                value = line(15)
                if value.get('closed'): break
            require(peer.wait(timeout=15) == 0, 'NATIVE_EXPLICIT_STOP_AND_REAP')
            return dict(backend='codex', realModel=True, mode=args.mode, modelTurnCompleted=True,
                        correlatedReply=True, approvals=approvals, eventMethods=methods,
                        credentialFilesCopied=False, ephemeral=True, nativeOwnerReaped=True)
        except BaseException:
            errors.flush(); errors.seek(0)
            diagnostic = errors.read(2097152).decode('utf-8', errors='replace')
            safe = '\n'.join(line for line in diagnostic.splitlines() if line.startswith(('FAIL codex-peer:', 'gterm:', 'Runtime phase ')))
            if args.receipt and args.receipt.absolute().parent.resolve().is_relative_to(ROOT / 'tmp'):
                args.receipt.with_suffix('.native-error.log').write_text(safe + '\n')
                summary = dict(complete=locals().get('complete'), approvals=locals().get('approvals'),
                    replyBytes=len(locals().get('text', '').encode()), expectedFilePresent=expected_path.is_file(), eventMethods=methods)
                args.receipt.with_suffix('.failure.json').write_text(json.dumps(summary,indent=2)+'\n')
            raise
        finally:
            if peer.poll() is None:
                peer.terminate()
                try: peer.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    peer.kill(); peer.wait(timeout=5)
            peer.stdout.close()
            if not peer.stdin.closed: peer.stdin.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--live', action='store_true', help='explicitly use the selected authenticated account for one real model turn')
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-codex')
    parser.add_argument('--codex', default='codex')
    parser.add_argument('--mode', choices=['chat', 'file', 'command'], default='chat')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if not args.live: parser.error('real account use requires --live; no automatic model test')
    args.build = args.build.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='codex-live-', dir=ROOT / 'tmp') as directory:
        result = run(args, Path(directory))
    if args.receipt:
        receipt = args.receipt.absolute()
        require(receipt.parent.resolve().is_relative_to(ROOT / 'tmp'), 'RECEIPT_MUST_BE_PROJECT_TMP')
        receipt.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()
