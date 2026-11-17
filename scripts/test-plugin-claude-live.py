#!/usr/bin/env python3
"""Opt-in live Claude SDK turn over the official Portable/Core/gterm bridge.

The selected owner profile uses existing authentication, empty setting sources,
and only explicitly selected tools. Keys and account data never enter receipts.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def require(value, code):
    if not value: raise AssertionError(code)


def compact(value): return json.dumps(value, ensure_ascii=False, separators=(',', ':'))
def units(value): return len(value.encode('utf-16-le')) // 2
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, scratch):
    workspace = scratch / 'workspace'; workspace.mkdir(mode=0o700)
    token = 'GHOSTERM_LIVE_' + uuid.uuid4().hex
    expected_path = workspace / 'probe.txt'
    node = Path(shutil.which(args.node) or args.node).resolve(strict=True)
    component = args.component.resolve(strict=True)
    cli = component / ('node_modules/@anthropic-ai/claude-agent-sdk-darwin-' + ('arm64' if os.uname().machine == 'arm64' else 'x64') + '/claude')
    require(cli.is_file() and (component / 'dist/cli.js').is_file(), 'LOCKED_NODE_COMPONENT_UNAVAILABLE')
    environment = {**os.environ, 'TMPDIR': str(scratch), 'GHOSTERM_AUTOMATION_OFFSCREEN': '1',
        'CLAUDE_CONFIG_DIR': os.environ.get('CLAUDE_CONFIG_DIR', str(Path.home() / '.claude')),
        'CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC': '1'}
    require(environment.get('HOME') == os.environ.get('HOME'), 'HOME_MUST_NOT_CHANGE')
    selected = ['HOME', 'CLAUDE_CONFIG_DIR', 'PATH', 'TMPDIR', 'CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC']
    auth_names = []
    allowed = ['ANTHROPIC_API_KEY','ANTHROPIC_AUTH_TOKEN','ANTHROPIC_BASE_URL','ANTHROPIC_MODEL',
               'ANTHROPIC_DEFAULT_SONNET_MODEL','ANTHROPIC_DEFAULT_OPUS_MODEL','ANTHROPIC_DEFAULT_HAIKU_MODEL','CLAUDE_CODE_OAUTH_TOKEN']
    if args.auth_settings:
        fd = os.open(args.auth_settings, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            import stat
            before = os.fstat(fd)
            require(stat.S_ISREG(before.st_mode) and before.st_uid == os.geteuid() and before.st_nlink == 1 and
                    not before.st_mode & 0o022 and before.st_size <= 1048576, 'SELECTED_AUTH_SETTINGS_FILE')
            raw = os.read(fd, 1048577); after = os.fstat(fd)
            require(len(raw) == before.st_size and (before.st_ino,before.st_size,before.st_mtime_ns,before.st_ctime_ns) ==
                    (after.st_ino,after.st_size,after.st_mtime_ns,after.st_ctime_ns), 'AUTH_SETTINGS_CHANGED')
        finally: os.close(fd)
        settings = json.loads(raw).get('env', {})
        for name in allowed:
            if name in settings:
                value = settings[name]
                require(isinstance(value,str) and '\0' not in value and len(value.encode()) <= 4096, 'SELECTED_AUTH_ENVIRONMENT_VALUE')
                environment[name] = value
    for name in allowed:
        if name in environment:
            value = environment[name]
            require('\0' not in value and len(value.encode()) <= 4096, 'SELECTED_AUTH_ENVIRONMENT_VALUE')
            selected.append(name); auth_names.append(name)
    # This Native fixture deliberately uses the selected CLI account, not an
    # inferred API key or a copied credential file.
    authenticated = subprocess.run([cli, 'auth', 'status', '--json'], capture_output=True, env=environment, timeout=15)
    require(authenticated.returncode == 0 and json.loads(authenticated.stdout).get('loggedIn') is True, 'SELECTED_CLI_AUTHENTICATION_UNAVAILABLE')
    version = subprocess.check_output([cli, '--version'], env=environment, text=True, timeout=15).strip()
    require(version.startswith('2.1.323 '), 'LOCKED_CLAUDE_CLI_VERSION')
    node_version = subprocess.check_output([node, '--version'], env=environment, text=True, timeout=10).strip()
    profile = scratch / 'profile.json'
    profile.write_text(compact(dict(version=1, cwd=str(workspace), executable=str(cli), cliVersion='2.1.323',
        sha256=sha(cli), environmentNames=selected, tools=['Write'] if args.mode == 'file' else [])))
    profile.chmod(0o600)
    package = args.package.resolve(strict=True)
    command = [args.build / 'test_gterm_claude_peer', args.build / 'gterm', package,
        sha(package / 'integrity.json'), node, node_version, sha(node), component / 'dist/cli.js', profile, workspace, *auth_names]
    output, method_counts = bytearray(), {}
    total = approvals = 0
    with (scratch / 'native-stderr.log').open('w+b') as errors:
        peer = subprocess.Popen(list(map(str, command)), cwd=workspace, env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, start_new_session=True)
        def line(timeout=40):
            nonlocal total
            end = time.monotonic() + timeout
            while b'\n' not in output:
                require(time.monotonic() < end, 'NATIVE_OBSERVATION_DEADLINE')
                if select.select([peer.stdout], [], [], max(0, end - time.monotonic()))[0]:
                    data = os.read(peer.stdout.fileno(), 8192)
                    require(data, 'NATIVE_OWNER_ENDED')
                    total += len(data); require(total <= 16777216 and errors.tell() <= 2097152, 'NATIVE_OUTPUT_BUDGET')
                    output.extend(data)
            at = output.index(10); value = bytes(output[:at]); del output[:at + 1]
            return value.decode('utf-8')
        def rpc(method, value):
            data = compact(value).encode(); require(len(data) <= 3072, 'RPC_BODY_BUDGET')
            peer.stdin.write(method.encode() + b'\n' + data + b'\n'); peer.stdin.flush()
            result = json.loads(line()); require(result.get('kind') == 'success', 'RPC_' + method + '_' + str(result.get('kind')))
            return result['value']
        def call(method, value):
            data = compact(value)
            opened = rpc('claude.input.open', dict(totalUnits=units(data))); require(opened['status'] == 'ok', 'INPUT_RESERVATION')
            ident, offset = opened['inputID'], 0
            for start in range(0, len(data), 128):
                part = data[start:start+128]
                written = rpc('claude.input.append', dict(inputID=ident, offset=offset, content=part))
                offset += units(part); require(written['status'] == 'ok' and written['nextOffset'] == offset, 'INPUT_APPEND')
            require(rpc('claude.input.seal', dict(inputID=ident))['status'] == 'ok', 'INPUT_SEAL')
            receipt = rpc('claude.call', dict(method=method, inputID=ident)); require(receipt['status'] == 'ok', 'CALL_' + method + '_' + receipt['status'])
            parts, offset = [], 0
            while True:
                part = rpc('claude.response.read', dict(responseID=receipt['responseID'], offset=offset))
                require(part['status'] == 'ok' and part['offset'] == offset and part['totalUnits'] == receipt['totalUnits'] and
                    part['totalBytes'] == receipt['totalBytes'], 'COMPLETE_RESPONSE_IDENTITY')
                parts.append(part['content']); offset = part['nextOffset']
                if part['done']: break
                require(part['content'], 'COMPLETE_RESPONSE_PROGRESS')
            data = ''.join(parts)
            require(units(data) == receipt['totalUnits'] and len(data.encode()) == receipt['totalBytes'], 'COMPLETE_RESPONSE_SIZE')
            require(rpc('claude.response.forget', dict(responseID=receipt['responseID']))['accepted'], 'RESPONSE_RELEASE')
            return json.loads(data)
        try:
            require(line() == 'READY', 'NATIVE_READY')
            require(call('start', {})['ok'], 'SDK_START')
            end = time.monotonic() + 45
            ready = False
            while time.monotonic() < end and not ready:
                reply = call('poll', {}); require(reply['ok'], 'SDK_INITIALIZATION')
                value = reply['result']
                if value.get('source') == 'none':
                    require(value.get('phase') not in ('failed', 'lost', 'closed'), 'SDK_INITIALIZATION_' + value.get('phase', 'unknown'))
                    ready = value.get('phase') == 'ready'
                time.sleep(.01)
            require(ready, 'SDK_READY_DEADLINE')
            prompt = ('Reply with exactly ' + token + '. Do not use tools or inspect files.') if args.mode == 'chat' else (
                'Use the Write tool to create exactly one file at ' + str(expected_path) + ' containing exactly ' + token +
                ' followed by one newline. Request permission as needed. Do not read or modify other files or use other tools. Then reply with exactly ' + token + '.')
            sent = call('send', dict(text=prompt)); require(sent['ok'], 'SDK_SEND')
            message_id = sent['result']['messageID']
            complete = False; end = time.monotonic() + 180
            last_sequence = 0
            while time.monotonic() < end and not complete:
                polled = call('poll', dict(maxEvents=16)); require(polled['ok'], 'SDK_POLL')
                batch = polled['result']; require(batch['source'] == 'batch', 'SDK_BATCH')
                for item in batch['events']:
                    event = item['event']
                    if item['source'] == 'approval':
                        if event.get('kind') != 'requested': continue
                        request = event['request']; allowed = False
                        if args.mode == 'file' and request.get('toolName') == 'Write':
                            proposal = request['input']
                            allowed = set(proposal) == {'file_path', 'content'} and Path(proposal['file_path']).resolve() == expected_path and proposal['content'] == token + '\n'
                        decision = call('approve', dict(reference=request['reference'], allow=allowed))
                        require(decision['ok'] and decision['result']['accepted'], 'CURRENT_APPROVAL_DECISION')
                        require(not call('approve', dict(reference=request['reference'], allow=allowed))['result']['accepted'], 'DUPLICATE_APPROVAL_REJECTED')
                        if allowed: approvals += 1
                    else:
                        require(event['sequence'] > last_sequence, 'SDK_EVENT_SEQUENCE'); last_sequence = event['sequence']
                        require(event['kind'] != 'closed', 'SDK_EARLY_CLOSE')
                        if event['kind'] != 'sdk-message': continue
                        message = event['message']; kind = message.get('type', 'unknown')
                        method_counts[kind] = method_counts.get(kind, 0) + 1
                        if kind == 'result':
                            require(message.get('user_message_uuid') == message_id, 'MODEL_RESULT_CORRELATION')
                            require(message.get('subtype') == 'success' and message.get('is_error') is False, 'MODEL_RESULT_STATUS')
                            require(token in message.get('result', ''), 'MODEL_REPLY_TOKEN')
                            complete = True
                require(batch.get('phase') not in ('failed', 'lost', 'closed'), 'MODEL_SESSION_' + batch.get('phase', 'unknown'))
                if not complete: time.sleep(.02)
            require(complete, 'MODEL_COMPLETION_DEADLINE')
            if args.mode == 'file':
                require(approvals > 0 and expected_path.is_file() and not expected_path.is_symlink() and
                    expected_path.read_text() == token + '\n', 'APPROVED_ACTUAL_FILE_CONTENT')
            else: require(approvals == 0 and not expected_path.exists(), 'CHAT_WITHOUT_TOOLS')
            require(call('stop', {})['ok'], 'EXPLICIT_SDK_STOP')
            status = rpc('claude.status', {})
            require(status['reaped'] and json.loads(status['exitJSON'])['code'] == 0, 'SDK_CHILD_REAP')
            peer.stdin.close(); require(peer.wait(timeout=15) == 0, 'NATIVE_OWNER_REAP')
            return dict(backend='claude', realModel=True, mode=args.mode, modelTurnCompleted=True, correlatedReply=True,
                approvals=approvals, eventTypes=method_counts, sdkVersion='0.3.323', cliVersion='2.1.323',
                credentialFilesCopied=False, nativeOwnerReaped=True)
        except BaseException:
            errors.flush(); errors.seek(0)
            diagnostic = errors.read(2097152).decode('utf-8', errors='replace')
            safe = '\n'.join(line for line in diagnostic.splitlines() if line.startswith(('FAIL claude-peer:', 'gterm:')))
            if args.receipt and args.receipt.absolute().parent.resolve().is_relative_to(ROOT / 'tmp'):
                args.receipt.with_suffix('.native-error.log').write_text(safe + '\n')
                args.receipt.with_suffix('.failure.json').write_text(json.dumps(dict(approvals=approvals,eventTypes=method_counts),indent=2)+'\n')
            raise
        finally:
            if peer.poll() is None:
                peer.terminate()
                try: peer.wait(timeout=15)
                except subprocess.TimeoutExpired: peer.kill(); peer.wait(timeout=5)
            peer.stdout.close()
            if not peer.stdin.closed: peer.stdin.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--live', action='store_true', help='explicitly use the selected authenticated account for one real model turn')
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--component', type=Path, default=ROOT / 'build/claude-node-runtime')
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-claude')
    parser.add_argument('--node', default='node')
    parser.add_argument('--auth-settings', type=Path, help='explicit selected user settings file; select only provider auth/model env in memory, never inherit hooks/MCP/plugins')
    parser.add_argument('--mode', choices=['chat', 'file'], default='chat')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if not args.live: parser.error('real account use requires --live; no automatic model test')
    if os.uname().sysname != 'Darwin': parser.error('this selected native CLI profile requires macOS')
    args.build = args.build.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='claude-live-', dir=ROOT / 'tmp') as directory:
        result = run(args, Path(directory))
    if args.receipt:
        require(args.receipt.absolute().parent.resolve().is_relative_to(ROOT / 'tmp'), 'RECEIPT_MUST_BE_PROJECT_TMP')
        args.receipt.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__': main()
