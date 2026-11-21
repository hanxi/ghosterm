#!/usr/bin/env python3
"""Copied macOS task models/views, real local/direct/jump Executor and mutations.

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
MODEL = 'apple/GhostermApp/Sources/GhostermAppModel/PluginTask.swift'
APP = 'apple/GhostermApp/Sources/GhostermAppModel/ExtensionInstanceModel.swift'
PANE = 'apple/GhostermApp/Sources/GhostermApp/ExtensionRunView.swift'
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
    parser.add_argument('--mutation-case', action='append', help='run only named model or view mutations after the baseline')
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--attach', action='store_true', help='connect an owned one-shot Portable foreground IPC instance')
    parser.add_argument('--route', action='append', choices=('local', 'direct', 'jump'))
    parser.add_argument('--scenario', action='append', choices=('complete', 'empty', 'cancel', 'unknown'))
    parser.add_argument('--repeat', type=int, default=1, help='repeat each owned route/scenario to inspect lifecycle races')
    args = parser.parse_args()
    if not 1 <= args.repeat <= 16: parser.error('--repeat must be 1..16')
    build, package = args.build.resolve(strict=True), args.package.resolve(strict=True)
    for path in (build, package, args.receipt.resolve()):
        if not path.is_relative_to(ROOT / 'tmp'): parser.error('use owned project tmp builds, package and receipt')
    cache = (build / 'CMakeCache.txt').read_text()
    if any(option + ':BOOL=ON' not in cache for option in ('GHOSTERM_WITH_PLUGIN_JS', 'GHOSTERM_WITH_LIBSSH2')):
        parser.error('build explicit JS/SSH-on gterm first')
    inputs = {str(p.relative_to(ROOT)): sha(p) for directory in ('apple/GhostermApp/Sources', 'apple/GhostermApp/Tests',
        'apple/GhostermCore', 'apple/GhostermTerminal/Sources', 'core/include', 'cli/gterm', 'plugins/official/task')
        for p in (ROOT / directory).rglob('*') if p.is_file() and not any(part in ('.build', '.swiftpm') for part in p.parts)}
    for name in ('scripts/test-plugin-task-native.py', 'scripts/test-plugin-task.py', 'scripts/test-plugin-route.py', 'scripts/test-plugin-ssh.py',
        'scripts/test-plugin-ssh-stream.py', 'scripts/lib/gt-timeout.sh'):
        inputs[name] = sha(ROOT / name)
    inputs['scripts/fixtures/plugin-native-ipc.py'] = sha(ROOT / 'scripts/fixtures/plugin-native-ipc.py')
    ipc = load('native_ipc', 'scripts/fixtures/plugin-native-ipc.py')
    if any(b'GT_TASK_NATIVE_MUTATION' in (ROOT / n).read_bytes() for n in inputs if n.endswith('.swift')):
        raise RuntimeError('leftover Native task mutation marker')
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='nd-', dir=args.receipt.resolve().parent) as temporary:
        work = Path(temporary); source = work / 'h'; logs = work / 'logs'; logs.mkdir()
        for folder in ('apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal'):
            shutil.copytree(ROOT / folder, source / folder, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir(); shutil.copytree(ROOT / 'core/include', source / 'core/include')
        (source / 'core/build').symlink_to(build / 'core', target_is_directory=True)
        for folder in ('build', 'patches'): (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir()
        env = {**os.environ, 'TMPDIR': temporary, 'TMP': temporary, 'TEMP': temporary,
            'DEVELOPER_DIR': '/Applications/Xcode.app/Contents/Developer',
            'GHOSTERM_AUTOMATION_OFFSCREEN': '1', 'GHOSTERM_CORE_TEST_PLUGIN_JS': '1'}
        commands = []

        def run(label, command, red=False, seconds=300):
            bounded = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require task-native; shift; exec "$gt_timeout_bin" "$@"',
                'task-native', ROOT, seconds, *command]
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
        def test(label, selected='PluginTaskTests', count=28, red=False):
            text = run(label, ['swift', 'test', '--package-path', app, '--cache-path', args.receipt.resolve().parent / 'swift-cache',
                '--disable-keychain', '--disable-netrc', '--filter', selected], red)
            if not red and (f'Executed {count} test' not in text or 'with 0 failures' not in text or 'skipped' in text.lower()):
                raise RuntimeError(label + ': discovery/no-skip mismatch\n' + text)
        test('baseline')
        test('legacy-table', 'PluginTableTests', 16)
        test('legacy-diff', 'PluginDiffTests', 15)
        cases = [
            ('view', MODEL, 'try V.identifier(value["viewId"]) == d.viewID', 'try V.identifier(value["viewId"]) != ""', 'testSnapshotViewScope'),
            ('revision', MODEL, 'try V.revision(value["revision"]) == d.revision', 'try V.revision(value["revision"]) != ""', 'testSnapshotRevisionScope'),
            ('sequence', MODEL, 'UInt64(p.sequence)! >= UInt64(old.sequence)!', 'true', 'testSequenceCannotGoBack'),
            ('same-sequence', MODEL, 'p.sequence != old.sequence || p == old', 'true', 'testSameSequenceCannotChange'),
            ('progress', MODEL, 'UInt64(p.completed)! >= UInt64(old.completed)!', 'true', 'testProgressCannotGoBack'),
            ('total', MODEL, 'old.total == nil || p.total == old.total', 'true', 'testKnownTotalCannotChange'),
            ('terminal', MODEL, '!old.phase.terminal || p == old', 'true', 'testTerminalCannotBeRewritten'),
            ('cancel-identity', MODEL, 'try V.identifier(v["requestId"]) == requestID', 'try V.identifier(v["requestId"]) != ""', 'testCancelAckIdentityDoesNotAllowReplay'),
            ('cancel-no-replay', MODEL, 'cancelRequestID == nil', 'true', 'testCancelNeverRepeatsAfterUnknown'),
            ('late-reply', MODEL, 'generation == token && available && !Task.isCancelled', '!Task.isCancelled', 'testLateReplyCannotChangeInvalidatedTask'),
            ('caller-cancelled', MODEL, '                if cancellation == .pending {', '                if false && cancellation == .pending {', 'testCancelledCallerKeepsActionUnconfirmed'),
            ('visible-refresh', 'apple/GhostermApp/Sources/GhostermApp/PluginTaskView.swift', 'if model.canAutoRefresh { await model.load() }', 'if false { await model.load() }', 'testVisibleViewRefreshesThenStopsOnUncertainty'),
            ('announcement-title', 'apple/GhostermApp/Sources/GhostermApp/PluginTaskView.swift',
             'return cancellationMessage.map { "\\(title)，\\($0)" }', 'return cancellationMessage',
             'testCancellationAnnouncementIdentifiesTheTask'),
            ('cancel-receipt', 'apple/GhostermApp/Sources/GhostermApp/PluginTaskView.swift',
             '"取消请求回执未确认；任务已结束"', '"取消结果不明；不会重发取消请求"',
             'testUnknownCancellationOnlyAllowsExplicitReads'),
        ]
        if args.mutation_case:
            if not args.mutate or set(args.mutation_case) - {case[0] for case in cases}:
                parser.error('--mutation-case requires --mutate and known case names')
            cases = [case for case in cases if case[0] in args.mutation_case]
        groups = []
        for label, filename, old, new, target in cases if args.mutate else []:
            path = source / filename; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError('ambiguous mutation anchor: ' + label)
            before = {str(p): sha(p) for p in (app / '.build').rglob(path.stem + '.o')}
            try:
                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_TASK_NATIVE_MUTATION\n')
                test(label + '-red', 'PluginTaskTests.' + target, red=True)
                if not any(p.is_file() and sha(p) != digest for p, digest in ((Path(n), h) for n, h in before.items())):
                    raise RuntimeError('Native object unchanged: ' + label)
                test(label + '-control', CONTROL, 1)
            finally: path.write_bytes(original)
            test(label + '-restored', 'PluginTaskTests.' + target, 1)
            if sha(path) != inputs[filename]: raise RuntimeError('source restoration mismatch')
            groups.append(label); print('PASS ' + label + ': ordinary assertion / independent config / restore / object / SHA256', flush=True)

        keys = load('task_keys', 'scripts/test-plugin-ssh.py'); streams = load('task_streams', 'scripts/test-plugin-ssh-stream.py')
        routes = load('task_routes', 'scripts/test-plugin-route.py')
        private, public = streams.memory_identity(); records = []
        scenarios = {'complete': 10000, 'empty': 0, 'cancel': 1000000}
        if args.attach: scenarios['unknown'] = 1000000
        with keys.owned_agent(work, 'agent') as agent:
            host_keys = [keys.add_memory_key(agent, 'task-hop-' + str(i)) for i in range(2)]
            route_root = work / 'r'; route_root.mkdir()
            with routes.chain(route_root, 2, public, host_keys, agent, 'echo') as (ports, _, _):
                def actual(label, route, scenario, red=False):
                    limit = scenarios[scenario]
                    scratch = work / label; scratch.mkdir(); (scratch / 'stage').mkdir()
                    token = secrets.token_hex(24)
                    payload = dict(route=route, scenario=scenario, limit=limit, scratch=str(scratch), package=str(package), gterm=str(build / 'gterm'),
                        ports=ports, hostKeys=[p.decode() for p in host_keys], user=pwd.getpwuid(os.getuid()).pw_name, pem=private.decode())
                    with ExitStack() as attached:
                        frontend = attached.enter_context(ipc.foreground(build / 'gterm', package, 'task', ['--limit', str(limit)], env)) if args.attach else None
                        if frontend: payload.update(ipcDirectory=str(frontend.endpoint), ipcInstance=frontend.instance)
                        class Handler(socketserver.StreamRequestHandler):
                            def handle(self):
                                self.connection.settimeout(5)
                                request = self.rfile.readline(128).rstrip(b'\n').split(b' ')
                                if request[0] != token.encode() or len(request) > 2: return
                                if len(request) == 2:
                                    if frontend is None: return
                                    frontend.control(request[1].decode('ascii'))
                                data = json.dumps(dict(payload, ipcObservation=frontend.observation()) if frontend else payload, separators=(',', ':')).encode()
                                self.wfile.write(struct.pack('>I', len(data)) + data)
                        with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                            thread = threading.Thread(target=server.serve_forever); thread.start()
                            env.update(GHOSTERM_TASK_PORT=str(server.server_address[1]), GHOSTERM_TASK_TOKEN=token)
                            try: test(label, 'PluginTaskRPCAppTests.testTaskThroughNativeAppModel', 1, red)
                            finally: server.shutdown(); thread.join(5)
                        if thread.is_alive(): raise RuntimeError('fixture service did not stop')
                        if not red: records.append(dict(route=route, scenario=scenario, limit=limit))
                        if frontend and not red: records[-1]['ipc'] = frontend.finish()
                if args.scenario and set(args.scenario) - set(scenarios): parser.error('unknown requires --attach')
                for route in args.route or ('local', 'direct', 'jump'):
                    for scenario in args.scenario or scenarios:
                        for repetition in range(args.repeat):
                            actual(route + '-' + scenario + '-' + str(repetition), route, scenario)
                            print('PASS actual Native ' + route + ' task ' + scenario + ' repeat=' + str(repetition), flush=True)
                native = [
                    ('native-declaration', APP, 'if !run.requiredCapabilities.contains("ui.views"), extensionViews.contains("task"),', 'if false,', 'local'),
                    ('native-methods', APP, 'Array(Set(sessionMethods + PluginTaskModel.methods)).sorted()', 'methods', 'local'),
                    ('native-outcome', APP, 'command: command, returnTerminalOutcome: true)', 'command: command, returnTerminalOutcome: false)', 'local'),
                    ('native-render', PANE, 'if let task = model.extensionTask {', 'if let task = model.extensionTask, false {', 'local'),
                ]
                for label, filename, old, new, route in native if args.mutate and not args.mutation_case else []:
                    path = source / filename; original = path.read_bytes()
                    if original.count(old.encode()) != 1: raise RuntimeError('Native anchor: ' + label)
                    try:
                        path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_TASK_NATIVE_MUTATION\n')
                        actual(label + '-red', route, 'complete', True)
                        test(label + '-control', CONTROL, 1)
                    finally: path.write_bytes(original)
                    actual(label + '-restored', route, 'complete')
                    if sha(path) != inputs[filename]: raise RuntimeError('Native restoration mismatch')
                    groups.append(label); print('PASS ' + label + ': actual Native assertion / config / restore / SHA256', flush=True)
        test('final')
        if any(sha(ROOT / n) != h for n, h in inputs.items()): raise RuntimeError('root inputs changed')
        for file in work.rglob('*'):
            if file.is_file() and file.suffix in ('.log', '.json', '.conf', '.pub'):
                if b'PRIVATE KEY-----' in file.read_bytes(): raise RuntimeError('private fixture leaked to an artifact')
        record = dict(accepted=True, foregroundAttachment=args.attach, inputs=inputs, commands=commands, mutations=groups, actual=records,
            binary=sha(build / 'gterm'), core=sha(build / 'core/libghosterm_core.a'),
            package={str(p.relative_to(package)): sha(p) for p in package.rglob('*') if p.is_file()},
            scope='macOS Native task models, process-local AX/pixels and actual local/direct/jump Core/QuickJS; owned endpoints only; no Android, user device, VoiceOver OS interaction or release')
        args.receipt.write_text(json.dumps(record, indent=2) + '\n')
        print('NATIVE TASK PASS: 28 Native tests / 16 legacy table + 15 diff tests / ' + str(len(records)) + ' actual scenarios / ' + str(len(groups)) + ' mutations', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
