#!/usr/bin/env python3
"""Copied macOS diff models/views, real local/direct/jump Executor and mutations.

Only owned loopback SSH, memory keys, process-local AX and offscreen windows.
No user account, endpoint, file tree, foreground UI or model service is used.
"""
import argparse
from contextlib import ExitStack
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import secrets
import shutil
import signal
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
MODEL = 'apple/GhostermApp/Sources/GhostermAppModel/PluginDiff.swift'
APP = 'apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift'
PANE = 'apple/GhostermApp/Sources/GhostermApp/ExtensionsPane.swift'
CONTROL = 'HostListTests.testReadingDoesNotMutateConfig'


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    build, package = args.build.resolve(strict=True), args.package.resolve(strict=True)
    for path in (build, package, args.receipt.resolve()):
        if not path.is_relative_to(ROOT / 'tmp'): parser.error('use owned project tmp builds, package and receipt')
    cache = (build / 'CMakeCache.txt').read_text()
    if any(option + ':BOOL=ON' not in cache for option in ('GHOSTERM_WITH_PLUGIN_JS', 'GHOSTERM_WITH_LIBSSH2')):
        parser.error('build explicit JS/SSH-on gterm first')
    inputs = {str(p.relative_to(ROOT)): sha(p) for directory in ('apple/GhostermApp/Sources', 'apple/GhostermApp/Tests',
        'apple/GhostermCore', 'apple/GhostermTerminal/Sources', 'core/include', 'cli/gterm', 'plugins/official/diff')
        for p in (ROOT / directory).rglob('*') if p.is_file() and not any(part in ('.build', '.swiftpm') for part in p.parts)}
    for name in ('scripts/test-plugin-diff-native.py', 'scripts/test-plugin-diff.py', 'scripts/test-plugin-route.py', 'scripts/test-plugin-ssh.py',
        'scripts/test-plugin-ssh-stream.py', 'scripts/lib/gt-timeout.sh'):
        inputs[name] = sha(ROOT / name)
    if any(b'GT_DIFF_NATIVE_MUTATION' in (ROOT / n).read_bytes() for n in inputs if n.endswith('.swift')):
        raise RuntimeError('leftover Native diff mutation marker')
    with tempfile.TemporaryDirectory(prefix='nd-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'; logs = work / 'logs'; logs.mkdir()
        for folder in ('apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal'):
            shutil.copytree(ROOT / folder, source / folder, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir(); shutil.copytree(ROOT / 'core/include', source / 'core/include')
        (source / 'core/build').symlink_to(build / 'core', target_is_directory=True)
        for folder in ('build', 'patches'): (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir()
        env = {**os.environ, 'TMPDIR': str(ROOT / 'tmp'), 'DEVELOPER_DIR': '/Applications/Xcode.app/Contents/Developer',
            'GHOSTERM_AUTOMATION_OFFSCREEN': '1', 'GHOSTERM_CORE_TEST_PLUGIN_JS': '1'}
        commands = []

        def run(label, command, red=False, seconds=300):
            bounded = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require diff-native; shift; exec "$gt_timeout_bin" "$@"',
                'diff-native', ROOT, seconds, *command]
            p = subprocess.Popen(list(map(str, bounded)), cwd=ROOT, env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, start_new_session=True)
            try: out, err = p.communicate(timeout=seconds)
            except BaseException:
                if p.poll() is None:
                    try: os.killpg(p.pid, signal.SIGTERM)
                    except ProcessLookupError: pass
                try: p.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    if p.poll() is None:
                        try: os.killpg(p.pid, signal.SIGKILL)
                        except ProcessLookupError: pass
                    p.communicate(timeout=3)
                raise
            text = out + err; (logs / (label + '.log')).write_text(text)
            commands.append(dict(label=label, returncode=p.returncode))
            if red:
                failures = re.findall(r'^.*: error: .* : (?:XCTAssert\w+ failed|XCTUnwrap failed)[^\n]*', text, re.M)
                if p.returncode != 1 or len(failures) != 1 or 'Executed 1 test, with 1 failure (0 unexpected)' not in text or any(t in text for t in ('unexpected signal', 'AddressSanitizer', 'runtime error:', 'fatalError')):
                    raise RuntimeError(label + ': expected one compiled ordinary assertion\n' + text)
            elif p.returncode: raise RuntimeError(label + '\n' + text)
            return text

        app = source / 'apple/GhostermApp'
        def test(label, selected='PluginDiffTests', count=15, red=False):
            text = run(label, ['swift', 'test', '--package-path', app, '--filter', selected], red)
            if not red and (f'Executed {count} test' not in text or 'with 0 failures' not in text or 'skipped' in text.lower()):
                raise RuntimeError(label + ': discovery/no-skip mismatch\n' + text)
        test('baseline')
        test('legacy-table', 'PluginTableTests', 15)
        cases = [
            ('view', MODEL, 'try V.identifier(value["viewId"]) == d.viewID', 'try V.identifier(value["viewId"]) != ""', 'testPageBindsViewRevisionOffsetAndTotal'),
            ('revision', MODEL, 'try V.revision(value["revision"]) == d.revision', 'try V.revision(value["revision"]) != ""', 'testPageBindsViewRevisionOffsetAndTotal'),
            ('page-complete', MODEL, 'rows.count == min(d.pageSize, d.total - offset)', 'true', 'testPageRequiresCompleteRowsAndExactCursor'),
            ('prefix-before', MODEL, 'try V.integer(value["beforeOffset"], range: 0...128) == prefix.before', 'try V.integer(value["beforeOffset"], range: 0...128) >= 0', 'testPageRequiresCompleteRowsAndExactCursor'),
            ('prefix-after', MODEL, 'try V.integer(value["afterOffset"], range: 0...128) == prefix.after', 'try V.integer(value["afterOffset"], range: 0...128) >= 0', 'testPageRequiresCompleteRowsAndExactCursor'),
            ('coordinates', MODEL, 'old == (kind == .insert ? 0 : before)', 'true', 'testRowsRequireExactCoordinatesKindsAndEndings'),
            ('final-newline', MODEL, 'ending != .none ||', 'true ||', 'testNoFinalNewlineOnlyOnLastSourceLine'),
            ('late-reply', MODEL, 'generation == token && available && !Task.isCancelled', '!Task.isCancelled', 'testLatePageCannotReopenInvalidatedView'),
        ]
        groups = []
        for label, filename, old, new, target in cases if args.mutate else []:
            path = source / filename; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError('ambiguous mutation anchor: ' + label)
            before = {str(p): sha(p) for p in (app / '.build').rglob(path.stem + '.o')}
            try:
                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_DIFF_NATIVE_MUTATION\n')
                test(label + '-red', 'PluginDiffTests.' + target, red=True)
                if not any(p.is_file() and sha(p) != digest for p, digest in ((Path(n), h) for n, h in before.items())):
                    raise RuntimeError('Native object unchanged: ' + label)
                test(label + '-control', CONTROL, 1)
            finally: path.write_bytes(original)
            test(label + '-restored', 'PluginDiffTests.' + target, 1)
            if sha(path) != inputs[filename]: raise RuntimeError('source restoration mismatch')
            groups.append(label); print('PASS ' + label + ': ordinary assertion / independent config / restore / object / SHA256', flush=True)

        keys = load('diff_keys', 'scripts/test-plugin-ssh.py'); streams = load('diff_streams', 'scripts/test-plugin-ssh-stream.py')
        routes = load('diff_routes', 'scripts/test-plugin-route.py')
        private, public = streams.memory_identity(); records = []
        scenarios = load('diff_values', 'scripts/test-plugin-diff.py').SCENARIOS
        with keys.owned_agent(work, 'agent') as agent:
            host_keys = [keys.add_memory_key(agent, 'diff-hop-' + str(i)) for i in range(2)]
            route_root = work / 'r'; route_root.mkdir()
            with routes.chain(route_root, 2, public, host_keys, agent, 'echo') as (ports, _, _):
                def actual(label, route, scenario, red=False):
                    before, after, total = scenarios[scenario]
                    scratch = work / label; scratch.mkdir(); (scratch / 'stage').mkdir()
                    token = secrets.token_hex(24)
                    payload = dict(route=route, total=total, before=json.dumps(before, ensure_ascii=False), after=json.dumps(after, ensure_ascii=False), scratch=str(scratch), package=str(package), gterm=str(build / 'gterm'),
                        ports=ports, hostKeys=[p.decode() for p in host_keys], user=pwd.getpwuid(os.getuid()).pw_name, pem=private.decode())
                    class Handler(socketserver.StreamRequestHandler):
                        def handle(self):
                            self.connection.settimeout(5)
                            if self.rfile.readline(128).rstrip(b'\n') != token.encode(): return
                            data = json.dumps(payload, separators=(',', ':')).encode()
                            self.wfile.write(struct.pack('>I', len(data)) + data)
                    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                        thread = threading.Thread(target=server.serve_forever); thread.start()
                        env.update(GHOSTERM_DIFF_PORT=str(server.server_address[1]), GHOSTERM_DIFF_TOKEN=token)
                        try: test(label, 'PluginDiffRPCAppTests.testDiffThroughNativeAppModel', 1, red)
                        finally: server.shutdown(); thread.join(5)
                    if thread.is_alive(): raise RuntimeError('fixture service did not stop')
                    if not red: records.append(dict(route=route, scenario=scenario, total=total))
                for route in ('local', 'direct', 'jump'):
                    for scenario in ('change', 'empty', 'unicode'):
                        actual(route + '-' + scenario, route, scenario)
                        print('PASS actual Native ' + route + ' diff ' + scenario, flush=True)
                for scenario in ('wide', 'interleaved'):
                    actual('local-' + scenario, 'local', scenario)
                    print('PASS actual Native local diff ' + scenario, flush=True)
                native = [
                    ('native-declaration', APP, 'if extensionViews.contains("diff"),', 'if false,', 'local'),
                    ('native-methods', APP, 'Array(Set(sessionMethods + PluginDiffModel.methods)).sorted()', 'methods', 'local'),
                    ('native-render', PANE, 'if let diff = model.extensionDiff {', 'if let diff = model.extensionDiff, false {', 'local'),
                ]
                for label, filename, old, new, route in native if args.mutate else []:
                    path = source / filename; original = path.read_bytes()
                    if original.count(old.encode()) != 1: raise RuntimeError('Native anchor: ' + label)
                    try:
                        path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_DIFF_NATIVE_MUTATION\n')
                        actual(label + '-red', route, 'change', True)
                        test(label + '-control', CONTROL, 1)
                    finally: path.write_bytes(original)
                    actual(label + '-restored', route, 'change')
                    if sha(path) != inputs[filename]: raise RuntimeError('Native restoration mismatch')
                    groups.append(label); print('PASS ' + label + ': actual Native assertion / config / restore / SHA256', flush=True)
        test('final')
        if any(sha(ROOT / n) != h for n, h in inputs.items()): raise RuntimeError('root inputs changed')
        for file in work.rglob('*'):
            if file.is_file() and file.suffix in ('.log', '.json', '.conf', '.pub'):
                if b'PRIVATE KEY-----' in file.read_bytes(): raise RuntimeError('private fixture leaked to an artifact')
        record = dict(accepted=True, inputs=inputs, commands=commands, mutations=groups, actual=records,
            binary=sha(build / 'gterm'), core=sha(build / 'core/libghosterm_core.a'),
            package={str(p.relative_to(package)): sha(p) for p in package.rglob('*') if p.is_file()},
            scope='macOS Native diff models, process-local AX/pixels and actual local/direct/jump Core/QuickJS; owned endpoints only; no Android, user device, VoiceOver OS interaction or release')
        args.receipt.write_text(json.dumps(record, indent=2) + '\n')
        print('NATIVE DIFF PASS: 15 Native tests / 15 legacy table tests / 11 actual routes / ' + str(len(groups)) + ' mutations', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
