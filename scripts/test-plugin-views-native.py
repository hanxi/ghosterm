#!/usr/bin/env python3
"""Copied macOS Native view registry, actual owned routes and assertion mutants."""
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
MODEL = 'apple/GhostermApp/Sources/GhostermAppModel/PluginViews.swift'
CONTROL = 'HostListTests.testReadingDoesNotMutateConfig'
VIEW_TEST_COUNT = 21


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True); parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True); parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--mutation-case', action='append', help='run only named mutations after the baseline')
    parser.add_argument('--route', choices=['local', 'direct', 'jump'], action='append')
    parser.add_argument('--attach', action='store_true', help='connect an owned one-shot Portable foreground IPC instance')
    parser.add_argument('--attach-loss', action='store_true', help='terminate the owned foreground after Native view interactions')
    args = parser.parse_args(); build, package = args.build.resolve(), args.package.resolve()
    if args.attach_loss and not args.attach:
        parser.error('--attach-loss requires --attach')
    for p in [build, package, args.receipt.resolve()]: assert p.is_relative_to(ROOT / 'tmp')
    cache = (build / 'CMakeCache.txt').read_text()
    assert all(s + ':BOOL=ON' in cache for s in ['GHOSTERM_WITH_PLUGIN_JS', 'GHOSTERM_WITH_LIBSSH2'])
    inputs = {str(p.relative_to(ROOT)): sha(p) for directory in ['apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal/Sources', 'core/src', 'core/include', 'cli/gterm', 'packages/plugin-sdk', 'plugins/official/views']
              for p in (ROOT / directory).rglob('*') if p.is_file() and not any(x in ('.build', '.swiftpm') for x in p.parts)}
    for name in ['scripts/test-plugin-views-native.py', 'scripts/test-plugin-ssh.py', 'scripts/test-plugin-ssh-stream.py', 'scripts/test-plugin-route.py', 'scripts/lib/gt-timeout.sh']:
        inputs[name] = sha(ROOT / name)
    inputs['scripts/fixtures/plugin-native-ipc.py'] = sha(ROOT / 'scripts/fixtures/plugin-native-ipc.py')
    ipc = load('native_ipc', 'scripts/fixtures/plugin-native-ipc.py')
    cases = [
        ('instance', MODEL, 'snapshot.instance == instance, snapshot.epoch == epoch', 'snapshot.instance != "", snapshot.epoch == epoch', 'testWrongInstanceClearsViews'),
        ('epoch', MODEL, 'snapshot.epoch == epoch', 'snapshot.epoch != ""', 'testWrongEpochClearsViews'),
        ('same-revision', MODEL, 'guard snapshot == last else', 'guard true else', 'testSameRevisionCannotChangeBindings'),
        ('resurrection', MODEL, 'guard r.counter > highWater else', 'guard true else', 'testRemovedHandleCannotReturnAtNewRevision'),
        ('binding', MODEL, 'r.methods.isSubset(of: methods)', 'true', 'testUnknownMethodBindingRejected'),
        ('remove', MODEL, 'for old in items where !handles.contains(old.id)', 'for old in items where false && !handles.contains(old.id)', 'testReopenSameViewIDUsesFreshModel'),
        ('task-history', MODEL, 'expectedViewID: r.viewID, history: history', 'expectedViewID: r.viewID, history: nil', 'testTaskReopenKeepsUnknownCancellationIdentity'),
        ('descriptor-id', 'apple/GhostermApp/Sources/GhostermAppModel/PluginTable.swift', 'expectedViewID == nil || expectedViewID == description.viewID', 'true', 'testDescriptorMustMatchRegisteredViewID'),
        ('close-label', 'apple/GhostermApp/Sources/GhostermApp/PluginViewsView.swift',
         '"关闭视图 \\(item.registration.viewID)"', '"关闭视图"',
         'testNativeCloseControlsRouteOverlappingHandlesToTheirOwnInstance'),
    ]
    if args.mutation_case:
        if not args.mutate or set(args.mutation_case) - {case[0] for case in cases}:
            parser.error('--mutation-case requires --mutate and known case names')
        cases = [case for case in cases if case[0] in args.mutation_case]
    for label, filename, old, _new, _test in cases:
        assert (ROOT / filename).read_text().count(old) == 1, label
    with tempfile.TemporaryDirectory(prefix='mv-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'; logs = work / 'logs'; logs.mkdir()
        for directory in ['apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal']:
            shutil.copytree(ROOT / directory, source / directory, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir(); shutil.copytree(ROOT / 'core/include', source / 'core/include'); (source / 'core/build').symlink_to(build / 'core', target_is_directory=True)
        for directory in ['build', 'patches']: (source / directory).symlink_to(ROOT / directory, target_is_directory=True)
        (source / 'tmp').mkdir()
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer', GHOSTERM_AUTOMATION_OFFSCREEN='1', GHOSTERM_CORE_TEST_PLUGIN_JS='1')
        commands, groups, actual = [], [], []
        def test(label, selected, count=1, red=False):
            command = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require views-native; shift; exec "$gt_timeout_bin" "$@"', 'views-native', ROOT, 300, 'swift', 'test', '--package-path', source / 'apple/GhostermApp', '--filter', selected]
            child = subprocess.Popen(list(map(str, command)), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            try: output, errors = child.communicate(timeout=305)
            finally:
                if child.poll() is None:
                    try: os.killpg(child.pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    child.wait()
            text = (output + errors).decode(); (logs / (label + '.log')).write_text(text)
            if red:
                failures = re.findall(r'^.*: error: .* : (?:XCTAssert\w+ failed|XCTUnwrap failed)[^\n]*', text, re.M)
                assert child.returncode == 1 and len(failures) == 1 and 'Executed 1 test, with 1 failure (0 unexpected)' in text and not any(s in text for s in ['unexpected signal', 'AddressSanitizer', 'fatalError']), (label, text[-16000:])
            else:
                assert child.returncode == 0 and f'Executed {count} test' in text and 'with 0 failures' in text and 'skipped' not in text.lower(), (label, text[-16000:])
            commands.append(dict(label=label, status=child.returncode, sha256=hashlib.sha256(output + errors).hexdigest()))
            print('PASS ' + label, flush=True)
        test('baseline', 'PluginViewsTests', VIEW_TEST_COUNT)
        test('bootstrap', 'ExtensionControllerTests.testRelayBootstrapAcceptsFourUniqueImplementedViewServices')
        test('host-profile', 'ExtensionControllerTests.testClientHostProfileOnlyAdvertisesIntegratedCapabilities')
        test('legacy-table', 'PluginTableTests', 15); test('legacy-diff', 'PluginDiffTests', 15); test('legacy-task', 'PluginTaskTests', 28)
        if args.mutate:
            for label, filename, old, new, selected in cases:
                path = source / filename; before = path.read_bytes()
                objects = {str(p): sha(p) for p in (source / 'apple/GhostermApp/.build').rglob(path.stem + '.o')}
                path.write_bytes(before.replace(old.encode(), new.encode()))
                try:
                    test(label + '-assertion', 'PluginViewsTests.' + selected, red=True)
                    assert any(Path(p).is_file() and sha(Path(p)) != digest for p, digest in objects.items()), label
                    test(label + '-control', CONTROL)
                finally: path.write_bytes(before)
                test(label + '-restored', 'PluginViewsTests.' + selected)
                assert path.read_bytes() == before
                groups.append(label)
        keys = load('view_keys', 'scripts/test-plugin-ssh.py'); streams = load('view_streams', 'scripts/test-plugin-ssh-stream.py'); routes = load('view_routes', 'scripts/test-plugin-route.py')
        private, public = streams.memory_identity()
        with keys.owned_agent(work, 'agent') as agent:
            host_keys = [keys.add_memory_key(agent, 'views-hop-' + str(i)) for i in range(2)]
            route_root = work / 'r'; route_root.mkdir()
            with routes.chain(route_root, 2, public, host_keys, agent, 'echo') as (ports, _, _):
                for route in args.route or ['local', 'direct', 'jump']:
                    scratch = work / route; scratch.mkdir(); (scratch / 'stage').mkdir()
                    token = secrets.token_hex(24)
                    payload = dict(route=route, scratch=str(scratch), package=str(package), gterm=str(build / 'gterm'), ports=ports, hostKeys=[k.decode() for k in host_keys], user=pwd.getpwuid(os.getuid()).pw_name, pem=private.decode(), foregroundLoss=args.attach_loss)
                    with ExitStack() as attached:
                        frontend = attached.enter_context(ipc.foreground(build / 'gterm', package, 'views', [], env)) if args.attach else None
                        if frontend: payload.update(ipcDirectory=str(frontend.endpoint), ipcInstance=frontend.instance)
                        class Handler(socketserver.StreamRequestHandler):
                            def handle(self):
                                self.connection.settimeout(5)
                                request = self.rfile.readline(128).rstrip(b'\n').split(b' ')
                                if request[0] != token.encode() or len(request) > 2: return
                                if len(request) == 2:
                                    if frontend is None or not args.attach_loss or request[1] != b'lose-foreground': return
                                    frontend.control('pause')
                                    frontend.control('kill')
                                data = json.dumps(dict(payload, ipcObservation=frontend.observation()) if frontend else payload).encode(); self.wfile.write(struct.pack('>I', len(data)) + data)
                        with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                            thread = threading.Thread(target=server.serve_forever); thread.start()
                            env.update(GHOSTERM_VIEWS_PORT=str(server.server_address[1]), GHOSTERM_VIEWS_TOKEN=token)
                            try: test('actual-' + route, 'PluginViewsRPCAppTests.testRegisteredViewsThroughNativeAppModel')
                            finally: server.shutdown(); thread.join(5)
                        assert not thread.is_alive(); actual.append(dict(route=route, ipc=frontend.finish()) if frontend else route)
        test('restored', 'PluginViewsTests', VIEW_TEST_COUNT)
        assert all(sha(ROOT / name) == value for name, value in inputs.items())
        for file in logs.glob('*.log'): assert b'PRIVATE KEY-----' not in file.read_bytes()
        args.receipt.write_text(json.dumps(dict(foregroundAttachment=args.attach, foregroundLoss=args.attach_loss,
            inputs=inputs, commands=commands, mutations=groups, actual=actual,
            gtermSHA256=sha(build / 'gterm'), package={str(p.relative_to(package)): sha(p) for p in package.rglob('*') if p.is_file()},
            scope='macOS Native registrations/models, actual local/direct/jump and offscreen controls; no Android, OS VoiceOver, full user import flow or release'), indent=2) + '\n')


if __name__ == '__main__':
    def interrupted(number, _frame): raise SystemExit(128 + number)
    signal.signal(signal.SIGTERM, interrupted)
    main()
