#!/usr/bin/env python3
"""Compiled official Executor over real native Session/gterm; no model request."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def compact(value): return json.dumps(value, ensure_ascii=False, separators=(',', ':'))
def units(value): return len(value.encode('utf-16-le')) // 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-claude')
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    if os.uname().sysname != 'Darwin': parser.error('requires explicit macOS no-network profile')
    build, package = args.build.resolve(), args.package.resolve()
    digest = hashlib.sha256((package / 'integrity.json').read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix='claude-native-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); node = work / 'node'
        shutil.copytree(ROOT / 'plugins/official/ai-claude/node', node)
        environment = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
        install = ['npm', 'ci', '--prefix', node, '--ignore-scripts', '--no-audit', '--no-fund', '--cache', ROOT / 'tmp/claude-npm-cache', '--registry', 'https://registry.npmjs.org']
        if not args.fetch: install.append('--offline')
        subprocess.run(list(map(str, install)), env=environment, check=True, capture_output=True, timeout=300)
        subprocess.run(['node', str(node / 'node_modules/typescript/bin/tsc'), '-p', str(node / 'tsconfig.json'), '--noEmitOnError'], env=environment, check=True, timeout=120)
        arch = 'arm64' if os.uname().machine == 'arm64' else 'x64'
        cli = node / f'node_modules/@anthropic-ai/claude-agent-sdk-darwin-{arch}/claude'
        node_binary = shutil.which('node'); node_sha = hashlib.sha256(Path(node_binary).read_bytes()).hexdigest()
        for mode in ['sdk-control-fixture', 'real-cli-no-model']:
            scope = work / mode; scope.mkdir()
            for name in ['home', 'config', 'workspace']: (scope / name).mkdir()
            env = dict(HOME=str(scope / 'home'), CLAUDE_CONFIG_DIR=str(scope / 'config'), PATH=os.environ['PATH'], TMPDIR=str(ROOT / 'tmp'),
                       CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1')
            version = subprocess.check_output([node_binary, '--version'], env=env, text=True).strip()
            profile = scope / 'profile.json'
            profile.write_text(compact(dict(version=1, cwd=str(scope / 'workspace'), executable=str(cli), cliVersion='2.1.323',
                                            sha256=hashlib.sha256(cli.read_bytes()).hexdigest(), environmentNames=list(env), tools=[])))
            entry = ROOT / 'scripts/fixtures/claude-node-sdk.mjs' if mode == 'sdk-control-fixture' else node / 'dist/cli.js'
            command = ['/usr/bin/sandbox-exec', '-p', '(version 1)(allow default)(deny network*)(deny mach-lookup (global-name "com.apple.securityd"))',
                       build / 'test_gterm_claude_peer', build / 'gterm', package, digest, node_binary, version, node_sha, entry, profile, scope / 'workspace']
            with (scope / 'stderr.log').open('w+') as errors:
                peer = subprocess.Popen(list(map(str, command)), env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors, start_new_session=True)
                pending = bytearray()
                def line(timeout=40):
                    end = time.monotonic() + timeout
                    while b'\n' not in pending:
                        if time.monotonic() >= end: raise RuntimeError('native peer observation deadline')
                        if select.select([peer.stdout], [], [], max(0, end - time.monotonic()))[0]:
                            chunk = os.read(peer.stdout.fileno(), 4096)
                            if not chunk:
                                errors.seek(0); raise RuntimeError('native peer ended:\n' + errors.read())
                            pending.extend(chunk)
                    at = pending.index(10); result = bytes(pending[:at]); del pending[:at + 1]; return result.decode('utf-8')
                def rpc(method, value):
                    data = (method + '\n' + compact(value) + '\n').encode()
                    assert len(compact(value).encode()) <= 3072
                    peer.stdin.write(data); peer.stdin.flush()
                    result = json.loads(line())
                    assert result['kind'] == 'success', result
                    return result['value']
                def call(method, value):
                    text = compact(value); opened = rpc('claude.input.open', dict(totalUnits=units(text))); assert opened['status'] == 'ok'
                    ident, offset = opened['inputID'], 0
                    for start in range(0, len(text), 128):
                        chunk = text[start:start + 128]
                        result = rpc('claude.input.append', dict(inputID=ident, offset=offset, content=chunk))
                        assert result['status'] == 'ok'; offset += units(chunk); assert result['nextOffset'] == offset
                    assert rpc('claude.input.seal', dict(inputID=ident))['status'] == 'ok'
                    receipt = rpc('claude.call', dict(method=method, inputID=ident)); assert receipt['status'] == 'ok', receipt
                    content, offset = [], 0
                    while True:
                        part = rpc('claude.response.read', dict(responseID=receipt['responseID'], offset=offset))
                        assert part['status'] == 'ok' and part['totalUnits'] == receipt['totalUnits'] and part['totalBytes'] == receipt['totalBytes']
                        assert part['offset'] == offset; content.append(part['content']); offset = part['nextOffset']
                        if part['done']: break
                        assert units(part['content']) > 0
                    content = ''.join(content); assert units(content) == receipt['totalUnits'] and len(content.encode()) == receipt['totalBytes']
                    assert rpc('claude.response.forget', dict(responseID=receipt['responseID']))['accepted']
                    return json.loads(content)
                def until(predicate):
                    for _ in range(200):
                        response = call('poll', {})
                        assert response['ok'], response
                        result = response['result']
                        if predicate(result): return result
                        time.sleep(.005)
                    raise RuntimeError('expected SDK observation missing')
                try:
                    assert line() == 'READY'
                    assert rpc('claude.status', {})['phase'] == 'new'
                    assert call('start', {})['result']['requested']
                    until(lambda result: result.get('source') == 'none' and result.get('phase') == 'ready')
                    if mode == 'sdk-control-fixture':
                        user = 'explicit no-model fixture 中文😀\n' * 500
                        assert call('send', dict(text=user))['ok']
                        approval = until(lambda result: result.get('source') == 'approval' and result.get('event', {}).get('kind') == 'requested')['event']['request']
                        assert approval['toolName'] == 'Bash' and len(approval['input']['command'].encode()) > 3072
                        ref = approval['reference']
                        assert call('approve', dict(reference=ref, allow=True))['result']['accepted']
                        assert not call('approve', dict(reference=ref, allow=True))['result']['accepted']
                        question = until(lambda result: result.get('source') == 'approval' and result.get('event', {}).get('kind') == 'requested')['event']['request']
                        assert question['kind'] == 'question'
                        answers = {item['question']: '回答😀' * 80 for item in question['questions']}
                        assert len(compact(dict(reference=question['reference'], answers=answers)).encode()) > 3072
                        assert call('answer', dict(reference=question['reference'], answers=answers))['result']['accepted']
                        audit = until(lambda result: result.get('source') == 'session' and result.get('event', {}).get('message', {}).get('subtype') == 'fixture-audit')['event']['message']
                        assert audit['approvals'] == 1 and audit['originalRequestID'] == 'fixture-审批😀'
                        assert audit['answers'] == answers and audit['userText'] == user and audit['command'] == approval['input']['command']
                    assert call('stop', {})['ok']
                    state = rpc('claude.status', {}); assert state['reaped'] and json.loads(state['exitJSON'])['code'] == 0, state
                    peer.stdin.close(); code = peer.wait(timeout=10)
                    errors.seek(0); diagnostic = errors.read()
                    assert code == 0, f'{mode}: native peer exit {code}:\n{diagnostic}'
                    assert not diagnostic, diagnostic
                    print(f'PASS compiled Claude Native/gterm bridge: {mode}; complete paged receipts / explicit stop / reap', flush=True)
                finally:
                    if peer.poll() is None:
                        peer.send_signal(signal.SIGTERM)
                        try: peer.wait(timeout=10)
                        except subprocess.TimeoutExpired: peer.kill(); peer.wait(timeout=5)
        print('CLAUDE NATIVE BRIDGE PASS: real SDK control fixture and real CLI preflight kept separate; no model execution')


if __name__ == '__main__': main()
