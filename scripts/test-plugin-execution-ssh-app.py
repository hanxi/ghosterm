#!/usr/bin/env python3
"""Native App -> owned loopback SSH or macOS local pipes -> actual Node/native Exec.

Keys exist only in the fixture service memory and ssh-agent. No accounts/models,
no foreground windows, no user endpoints; temporary executables stay in project tmp.
"""
import argparse
from contextlib import ExitStack, nullcontext
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import pwd
import secrets
import shlex
import shutil
import signal
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def alive(pid):
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False


def identity(pid):
    p = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=,command='], capture_output=True, text=True, timeout=5)
    return p.stdout.strip() if p.returncode == 0 else None


def execute(args, serial=None):
    local_mode = getattr(args, "local", False)
    topology, stop_terminal = getattr(args, 'tmux', None), getattr(args, 'stop_tmux', False)
    attaching = getattr(args, 'attach', False)
    kill_front = getattr(args, 'kill_front_pane', False)
    build, node, sdk = args.build.resolve(strict=True), args.node.resolve(strict=True), args.sdk.resolve(strict=True)
    for p in (build / 'gterm', build / 'test_gterm_exec_peer', build / 'core/libghosterm_core.a'):
        if not p.is_file(): raise RuntimeError('Build the JS/SSH-enabled CLI fixture: ' + str(p))
    cache = (build / 'CMakeCache.txt').read_text()
    if 'GHOSTERM_WITH_LIBSSH2:BOOL=ON' not in cache or 'GHOSTERM_WITH_PLUGIN_JS:BOOL=ON' not in cache:
        raise RuntimeError('Explicit JS/SSH-enabled build required')
    hashes = {str(p.relative_to(ROOT)): sha(p) for directory in ['core/src', 'core/include', 'core/tests',
        'apple/GhostermApp/Sources', 'apple/GhostermApp/Tests', 'apple/GhostermCore/Sources', 'apple/GhostermCore/Tests',
        'android/app/src', 'android/bridge/src', 'packages/plugin-sdk/node']
        for p in (ROOT / directory).rglob('*') if p.is_file()}
    binary_hashes = {str(p): sha(p) for p in [build / 'gterm', build / 'test_gterm_exec_peer',
        build / 'core/libghosterm_core.a', node]}
    for name in ['scripts/test-plugin-execution-ssh-app.py', 'scripts/fixtures/plugin-execution-app.mjs',
        'scripts/fixtures/plugin-execution-fault.py', 'scripts/fixtures/plugin-execution-supervisor.py',
        'scripts/fixtures/plugin-execution-tmux.py',
        'scripts/fixtures/plugin-execution-foreground.py',
        'scripts/fixtures/plugin-foreground-tty.py',
        'cli/gterm/main.c', 'cli/gterm/stdio.c', 'cli/gterm/ipc.c', 'cli/gterm/stdio.h', 'cli/gterm/ipc.h',
        'scripts/test-plugin-route.py', 'scripts/test-plugin-ssh.py', 'scripts/test-plugin-ssh-stream.py',
        'scripts/test-plugin-codex-patch-android.py', 'scripts/fixtures/plugin-execution-process.mjs']:
        hashes[name] = sha(ROOT / name)
    binary_hashes.update({str(p): sha(p) for p in sdk.rglob('*') if p.is_file()})
    if topology:
        terminal_tools = [Path('/usr/bin/ssh').resolve(strict=True), Path(sys.executable).resolve(strict=True)]
        tmux_path = shutil.which('tmux')
        if not tmux_path: raise RuntimeError('Explicit tmux conformance requires an installed tmux')
        terminal_tools.append(Path(tmux_path).resolve(strict=True))
        binary_hashes.update({str(path): sha(path) for path in terminal_tools})
    utilities = {'cat': Path('/bin/cat').resolve(strict=True), 'env': Path('/usr/bin/env').resolve(strict=True),
                 'pwd': Path('/bin/pwd').resolve(strict=True)}
    binary_hashes.update({str(p): sha(p) for p in utilities.values()})
    version = subprocess.check_output([node, '--version'], text=True).strip()[1:]
    catalog = hashlib.sha256()
    for p in sorted((p for p in sdk.rglob('*') if p.is_file()), key=lambda p: str(p.relative_to(sdk)).encode()):
        name = str(p.relative_to(sdk)).encode(); catalog.update(len(name).to_bytes(4, 'big') + p.stat().st_size.to_bytes(8, 'big') + name + sha(p).encode())
    sdk_digest = catalog.hexdigest()
    runtime_types = ('node', 'exec')
    route_names = ('local',) if local_mode else ('direct', 'jump')
    cases = [f'{r}:{route}:{mode}' for r in runtime_types for route in route_names
             for mode in ('allow', 'deny', 'no-approval', 'pending-cancel', 'pending-loss',
                          'resource', 'resource-root', 'resource-parent', 'resource-link')]
    cases += [f'{r}:{route}:{mode}' for r in runtime_types for route in route_names
              for mode in ('process', 'process-root', 'process-program', 'process-cancel', 'process-stderr',
                           'process-environment', 'process-cwd', 'process-stop', 'process-loss')]
    cases += [f'{r}:{route}:{mode}' for r in runtime_types for route in route_names
              for mode in ('client-resource', 'client-root', 'client-parent', 'client-link', 'client-no-approval', 'client-undeclared', 'client-stop', 'client-loss', 'client-cancel', 'client-directory-link', 'client-directory-file')]
    attach_modes = ('allow','deny','no-approval','pending-cancel','pending-loss','client-resource','process','client-stop','client-loss','attach-instance','attach-command','attach-argument')
    if attaching: cases += [f'{r}:{route}:{mode}' for r in runtime_types for route in route_names for mode in attach_modes if mode.startswith('attach-')]
    tmux_modes = ('allow', 'client-resource', 'process') if stop_terminal else ('allow', 'deny', 'no-approval',
        'client-resource', 'process', 'pending-cancel', 'pending-loss', 'client-stop', 'client-loss')
    pane_loss_modes = ('pending-loss', 'process-loss', 'client-loss')
    if local_mode: cases += [f'{r}:local:{m}' for r in runtime_types for m in ('local-no-approval','local-digest','local-cwd')]
    selected = args.case or ([case for case in cases if case.split(':')[2] in pane_loss_modes] if kill_front else [case for case in cases if case.split(':')[2] in attach_modes] if attaching else [case for case in cases if case.split(':')[2] in tmux_modes] if topology else cases)
    if kill_front and (not attaching or not topology or any(case.split(':')[2] not in pane_loss_modes for case in selected)):
        raise RuntimeError('Pane fault requires explicit tmux attachment and pending / process / client loss')
    if attaching and (stop_terminal or any(case.split(':')[2] not in (attach_modes + (('process-loss',) if kill_front else ())) for case in selected)):raise RuntimeError('Unsupported foreground attachment mode')
    if stop_terminal and (not topology or any(case.split(':')[2] not in tmux_modes for case in selected)):
        raise RuntimeError('Explicit terminal stop needs tmux and one of allow / client-resource / process')
    if set(selected) - set(cases): raise RuntimeError('Unknown execution case')
    routes = load('execution_routes', 'scripts/test-plugin-route.py')
    keys = load('execution_keys', 'scripts/test-plugin-ssh.py')
    streams = load('execution_streams', 'scripts/test-plugin-ssh-stream.py')
    private, public = (b'', b'') if local_mode else streams.memory_identity()
    fault_module = load('execution_fault', 'scripts/fixtures/plugin-execution-fault.py')
    terminal_module = load('execution_tmux', 'scripts/fixtures/plugin-execution-tmux.py') if topology else None
    foreground_module = load('execution_foreground','scripts/fixtures/plugin-execution-foreground.py') if attaching else None
    records = []
    with tempfile.TemporaryDirectory(prefix='xs-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); logs = work / 'logs'; logs.mkdir()
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',
            GHOSTERM_CORE_TEST_PLUGIN_JS='1', GHOSTERM_AUTOMATION_OFFSCREEN='1',
            JAVA_HOME=str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'),
            ANDROID_HOME=str(ROOT / 'build/android-toolchain/sdk'), GRADLE_USER_HOME=str(ROOT / 'build/android-toolchain/gradle-home'))
        def run(label, command, extra=None, seconds=600):
            p = subprocess.run(list(map(str, command)), cwd=ROOT, env={**env, **(extra or {})}, capture_output=True, text=True, timeout=seconds)
            output = p.stdout + p.stderr; (logs / (label + '.log')).write_text(output)
            if p.returncode:
                diagnostics = [(str(f.relative_to(work)), f.read_text(errors='replace'))
                    for name in ('stderr', 'resource-result', 'resource-receipt', 'resource-error', 'process-proof') for f in work.glob('*/o/' + name)]
                raise RuntimeError(label + '\n' + output + '\nREMOTE STDERR: ' + repr(diagnostics))
            return output
        platform_binaries = {}
        if args.macos:
            source = args.project.resolve(strict=True) if args.project else work / 'h'
            if not args.project:
                for folder in ['apple/GhostermCore', 'apple/GhostermTerminal', 'apple/GhostermApp']:
                    shutil.copytree(ROOT / folder, source / folder, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
                (source / 'core').mkdir(); shutil.copytree(ROOT / 'core/include', source / 'core/include', symlinks=True)
                (source / 'core/build').symlink_to(build / 'core', target_is_directory=True)
                for folder in ['build', 'patches']: (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
                (source / 'tmp').mkdir()
            run('compile', ['swift', 'build', '--build-tests', '--package-path', source / 'apple/GhostermApp'])
            tests = list((source / 'apple/GhostermApp/.build').rglob('GhostermAppTests.xctest/Contents/MacOS/GhostermAppTests'))
            if not tests: raise RuntimeError('Actual XCTest binary missing')
            platform_binaries = {str(p.relative_to(source)): sha(p) for p in tests}
        else:
            source = args.project.resolve(strict=True) if args.project else ROOT
            adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial]
            run('compile', [ROOT / 'android/gradlew', '-p', source / 'android', '-PghostermPluginJs=true',
                ':app:assembleDebug', ':app:assembleDebugAndroidTest'])
            for name in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
                apk = source / 'android/app/build/outputs/apk' / name
                platform_binaries[str(apk.relative_to(source))] = sha(apk)
                run('install-' + apk.name, [*adb, 'install', '-r', '-t', apk])
                package = 'com.hanxi.ghosterm.test' if 'androidTest/' in name else 'com.hanxi.ghosterm'
                installed = run('path-' + apk.name, [*adb, 'shell', 'pm', 'path', package]).strip().removeprefix('package:')
                if run('sha-' + apk.name, [*adb, 'shell', 'sha256sum', installed]).split()[0] != sha(apk): raise RuntimeError('Installed APK mismatch')
        with (nullcontext(None) if local_mode else keys.owned_agent(work, 'agent')) as agent:
            if topology:
                added = subprocess.run(['ssh-add', '-'], input=private, capture_output=True,
                    env={**env, 'SSH_AUTH_SOCK': str(agent), 'SSH_ASKPASS_REQUIRE': 'never'}, timeout=10)
                if added.returncode: raise RuntimeError('Owned terminal client key could not be admitted to memory agent')
            host_keys = [] if local_mode else [keys.add_memory_key(agent, 'execution-hop-' + str(i)) for i in range(2)]
            route_root = work / 'r'; route_root.mkdir()
            with (nullcontext(([0, 0], None, None)) if local_mode else routes.chain(route_root, 2, public, host_keys, agent, 'echo', exec_hops=(0,) if topology == 'nested' else ())) as (ports, _, _), ExitStack() as terminals:
                for index, case in enumerate(selected):
                    runtime, route, mode = case.split(':')
                    client_case = mode.startswith('client-')
                    resource_case = mode.startswith('resource') or client_case
                    process_case = mode.startswith('process')
                    root = work / str(index); root.mkdir()
                    package, observation, stage, home, workspace = [root / p for p in ('k', 'o', 't', 'h', 'w')]
                    for folder in [package, observation, stage, home, workspace]: folder.mkdir(mode=0o700)
                    terminal = None
                    if topology:
                        terminal_root = root / 'z'; terminal_root.mkdir()
                        terminal = terminals.enter_context(terminal_module.Terminal(terminal_root, topology, ports,
                            public, host_keys, agent, keys.bridge, pwd.getpwuid(os.getuid()).pw_name))
                    resource_bytes = bytes(i % 251 for i in range(262144))
                    (workspace / 'file.bin').write_bytes(resource_bytes)
                    (root / 'outside.bin').write_bytes(b'UNAUTHORIZED disposable fixture')
                    (workspace / 'link.bin').symlink_to(root / 'outside.bin')
                    fields = ['first', 'empty', 'literal']
                    schema = dict(type='object', properties={p: dict(type='string', maxLength=256) for p in fields}, required=fields,
                        additionalProperties=False, **{'x-ghosterm-cli': dict(version=1, fields={p: dict(position=i) for i, p in enumerate(fields)})})
                    variant = dict(id='selected', runtime=runtime, requires=['session.rpc'])
                    if resource_case: variant['requires'] += ['resource.receive', 'file.readScoped']
                    if process_case: variant['requires'] += ['process.spawn', 'process.pipes']
                    if runtime == 'node': variant.update(entry='executor/index.mjs', runtimeVersionRange='>=20.0.0 <1000.0.0')
                    else: variant.update(executable='executor/main', os=[{'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]],
                        arch=[{'arm64': 'aarch64', 'aarch64': 'aarch64', 'x86_64': 'x86_64'}[platform.machine()]])
                    manifest = dict(schemaVersion=1, id='org.ghosterm.execution', pluginVersion='0.1.0', sdkApiRange='^1.0.0',
                        bridgeProtocolRange=[1], publisher='fixture', license='MIT',
                        commands=[dict(path=['fixture'], title='Fixture', argsSchema='schemas/input.json')],
                        client=dict(runtime='portable-js', entry='client/index.js', requires=['session.rpc']),
                        executor=dict(variants=[variant]), permissions=[])
                    if resource_case:
                        if mode != 'client-undeclared': manifest['client']['requires'] += ['resource.receive']
                        manifest['permissions'] = [dict(side='executor', capability='file.readScoped',
                            purpose='Owned disposable workspace fixture', scope='instance')]
                    if process_case:
                        manifest['permissions'] = [dict(side='executor', capability='process.spawn',
                            purpose='Owned mapped utility fixture', scope='instance', programs=['cat'])]
                    files = {'plugin.json': json.dumps(manifest).encode(), 'schemas/input.json': json.dumps(schema).encode(),
                        'client/index.js': b'export function activate(ctx){if(ctx.resources && ctx.resources.workspaces.length) throw Error("Unexpected Client file grant");return {dispose(){}}}', 'README': b'Owned execution fixture',
                        'LICENSES/MIT.txt': (ROOT / 'LICENSE').read_bytes(),
                        ('executor/index.mjs' if runtime == 'node' else 'executor/main'):
                            (ROOT / 'scripts/fixtures/plugin-execution-app.mjs').read_bytes() if runtime == 'node' else (build / 'test_gterm_exec_peer').read_bytes()}
                    if runtime == 'node': files['executor/process.mjs'] = (ROOT / 'scripts/fixtures/plugin-execution-process.mjs').read_bytes()
                    for name, content in files.items():
                        p = package / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(content)
                    members = [dict(path=name, bytes=len(content), sha256=hashlib.sha256(content).hexdigest()) for name, content in sorted(files.items())]
                    (package / 'integrity.json').write_text(json.dumps(dict(schemaVersion=1, members=members)))
                    digest = sha(package / 'integrity.json')
                    wrapper = root / 'g'
                    wrapper.write_text('#!/bin/sh\nexec ' + shlex.join([sys.executable, str(ROOT / 'scripts/fixtures/plugin-execution-supervisor.py'),
                        str(build / 'gterm'), str(observation), str(home)]) + ' "$@"\n'); wrapper.chmod(0o700)
                    scratch = root / 'c'; scratch.mkdir()
                    payload = dict(runtime=runtime, route=route, mode=mode, ports=ports, tmuxTopology=topology, foregroundTmux=attaching and bool(topology), stopTerminal=stop_terminal, user=pwd.getpwuid(os.getuid()).pw_name,
                        pem=private.decode('ascii'), packageFiles={str(p.relative_to(package)): base64.b64encode(p.read_bytes()).decode()
                        for p in package.rglob('*') if p.is_file()}, packageDigest=digest, packageDirectory=str(package),
                        localTransport=local_mode, localDigest=sha(build / "gterm"), localWorkingDirectory=str(workspace), processStatePath=str(observation / "local-state"),
                        wrapper=str(build / "gterm") if local_mode else str(wrapper), temporary=str(stage), scratch=str(scratch), node=str(node), nodeVersion=version,
                        nodeDigest=sha(node), sdk=str(sdk), sdkDigest=sdk_digest,
                        workspace=str(workspace), resourceDigest=hashlib.sha256(resource_bytes).hexdigest())
                    utility = utilities['env' if mode == 'process-environment' else 'pwd' if mode == 'process-cwd' else 'cat']
                    selected_environment = dict(HOME=os.environ.get('HOME', '') if local_mode and not attaching else str(home),
                        PATH=os.environ.get('PATH', '') if local_mode and not attaching else '/usr/bin:/bin', TMPDIR=str(stage),
                        GT_EXECUTION_APP_FIXTURE=str(observation), GT_SELECTED='approved')
                    expected_output = ('\n'.join(f'{name}={value}' for name, value in sorted(selected_environment.items())) + '\n').encode() \
                        if mode == 'process-environment' else (str(workspace) + '\n').encode() if mode == 'process-cwd' else resource_bytes
                    payload.update(processExecutable=str(utility), processDigest=sha(utility), processSignal=int(signal.SIGTERM),
                        processOutputDigest=hashlib.sha256(expected_output).hexdigest(), processOutputSize=len(expected_output))
                    foreground = None
                    if attaching:
                        endpoint = root/'i';endpoint.mkdir(mode=0o700)
                        command=[wrapper,'relay','--listen',endpoint,'--runtime',runtime,'--allow-process-execution']
                        if runtime=='node':command += ['--node-runtime',node,version,sha(node),'--node-sdk',sdk,sdk_digest]
                        command += ['--trust-development-digest',digest,'--client-digest',digest,'--method','fixture.echo','--process-environment','GT_EXECUTION_APP_FIXTURE']
                        if resource_case or process_case:command += ['--root','workspace',workspace]
                        if process_case:command += ['--process-program','cat',utility,'fixture',sha(utility)]
                        command += ['--',package,'different' if mode=='attach-command' else 'fixture','different' if mode=='attach-argument' else '中文😀','',"literal'$(owned)"]
                        foreground=terminals.enter_context(foreground_module.Foreground(command,{**env,'TMPDIR':str(stage)},observation,endpoint,identity,terminal))
                        instance=foreground.instance
                        if mode=='attach-instance':instance=('0' if instance[0]!='0' else '1')+instance[1:]
                        payload.update(ipcDirectory=str(endpoint),ipcInstance=instance)
                        attach_observation=observation/'attach';attach_observation.mkdir()
                        wrapper.write_text('#!/bin/sh\nexec '+shlex.join([sys.executable,str(ROOT/'scripts/fixtures/plugin-execution-supervisor.py'),str(build/'gterm'),str(attach_observation),str(home)])+' "$@"\n')
                    token = secrets.token_hex(24); owned_pids = {}; local_fault = {}
                    def observe():
                        native_marker = attach_observation / 'gterm-pid' if attaching else observation / 'gterm-pid'
                        if local_mode and not native_marker.exists():
                            table = subprocess.check_output(['ps', '-axo', 'pid=,command='], text=True, timeout=5)
                            found = [int(f[0]) for line in table.splitlines() if len(f := line.strip().split(None, 1)) == 2
                                and f[1].startswith(str(build / 'gterm') + ' ')
                                and (('--attach ' + str(endpoint)) in f[1] if attaching else str(package) in f[1])]
                            if len(found) > 1: raise RuntimeError('Ambiguous owned local process')
                            if found: native_marker.write_text(str(found[0]))
                        answer = {'wrapperStarted': (observation / 'gterm-pid').exists(), 'activated': (observation / 'activated').exists(),
                            'handler': (observation / 'handler').exists(), 'entered': (observation / 'entered').exists()}
                        if local_fault: answer['ownedFault'] = dict(local_fault)
                        if foreground is not None:
                            answer['ipcListening']=foreground.process.poll() is None and (endpoint/'permit').exists()
                            marker=attach_observation/'gterm-pid';answer['attachStarted']=marker.exists()
                            if marker.exists():
                                pid=int(marker.read_text());observed=identity(pid)
                                if observed and pid not in owned_pids:
                                    if str(endpoint) not in observed or '--attach' not in observed:raise RuntimeError('Owned attacher identity mismatch')
                                    owned_pids[pid]=observed
                            if (attach_observation/'exit').exists():answer['attachStatus']=json.loads((attach_observation/'exit').read_text())['status']
                        for file in ('gterm-pid', 'entered', 'activated'):
                            if (observation / file).exists():
                                pid = int((observation / file).read_text())
                                answer[{'entered': 'entry-pid', 'activated': 'runtime-pid'}.get(file, file)] = pid
                                observed = identity(pid)
                                if observed and pid not in owned_pids:
                                    expected = str(package) if file == 'gterm-pid' else str(stage)
                                    if expected not in observed: raise RuntimeError('Fixture PID does not match its owned command')
                                    owned_pids[pid] = observed
                        if (observation / 'tty').exists(): answer['tty'] = json.loads((observation / 'tty').read_text())
                        if (observation / 'runtime-tty').exists(): answer['runtimeTTY'] = json.loads((observation / 'runtime-tty').read_text())
                        if (observation / 'environment').exists(): answer['environment'] = (observation / 'environment').read_text()
                        if (observation / 'resource-receipt').exists(): answer['resourceReceipt'] = (observation / 'resource-receipt').read_text()
                        if (observation / 'resource-error').exists(): answer['resourceError'] = (observation / 'resource-error').read_text()
                        for filename, key in [('client-received', 'clientReceived')]:
                            if (observation / filename).exists(): answer[key] = (observation / filename).read_text()
                        if (observation / 'process-proof').exists(): answer['processProof'] = json.loads((observation / 'process-proof').read_text())
                        if process_case:
                            parent = answer.get('gterm-pid')
                            answer['businessParentVerified'] = parent in owned_pids and identity(parent) == owned_pids[parent]
                            if answer['businessParentVerified']:
                                table = subprocess.check_output(['ps', '-axo', 'pid=,ppid=,comm='], text=True, timeout=5)
                                owners = {parent, answer.get('runtime-pid')}
                                candidates = []
                                for line in table.splitlines():
                                    fields = line.split(None, 2)
                                    if len(fields) == 3 and int(fields[1]) in owners:
                                        candidates.append(line.strip())
                                        pid = int(fields[0]); observed = identity(pid)
                                        marker = 'ghosterm-business-' + str(answer.get('runtime-pid'))
                                        if observed and marker in observed:
                                            owned_pids.setdefault(pid, observed); business_pids.add(pid)
                                answer['businessCandidates'] = candidates
                            answer['businessPids'] = sorted(business_pids)
                            answer['businessLivePids'] = sorted(pid for pid in business_pids if identity(pid) == owned_pids[pid])
                        if (observation / 'local-state').exists():
                            answer['localProcess'] = json.loads((observation / 'local-state').read_text())
                            if attaching:
                                answer['attachObservedLive'] = answer['attachStarted']
                                answer['attachStarted'] = True  # Native snapshot proves spawn even for a fast rejection.
                        if (observation / 'exit').exists():
                            answer.update(json.loads((observation / 'exit').read_text()))
                            answer['statusOwner'] = 'foreground' if attaching else 'transport'
                        return answer
                    class Handler(socketserver.StreamRequestHandler):
                        def handle(self):
                            self.connection.settimeout(5)
                            if self.rfile.readline(128).rstrip(b'\n') != token.encode(): return
                            action = self.rfile.readline(32).strip()
                            if action == b'fixture': result = payload
                            elif action in (b'audit', b'kill'):
                                result = observe()
                                if action == b'kill':
                                    if kill_front:terminal.kill_foreground()
                                    elif local_mode:
                                        marker = attach_observation / 'gterm-pid' if attaching else observation / 'gterm-pid'
                                        pid = int(marker.read_text())
                                        if pid not in owned_pids or identity(pid) != owned_pids[pid]: raise RuntimeError('Owned local fault target no longer matches')
                                        os.kill(pid, signal.SIGTERM)
                                        local_fault.update(pid=pid, signal=int(signal.SIGTERM), kind='local-transport-termination')
                                        result['ownedFault'] = dict(local_fault)
                                    elif fault is not None:fault.disconnect()
                                    else:raise RuntimeError('No owned network fault wire')
                            elif action.startswith(b'tmux-') and terminal is not None: result = terminal.action(action.decode('ascii'))
                            elif action == b'stop-front' and foreground is not None: foreground.stop(); result=observe()
                            else: raise RuntimeError('Unknown fixture action')
                            data = json.dumps(result, separators=(',', ':')).encode()
                            if len(data) > 1048576: raise RuntimeError('Fixture response limit')
                            self.wfile.write(struct.pack('>I', len(data)) + data)
                    mapped = []
                    business_pids = set()
                    fault = None
                    if mode in ('pending-loss', 'process-loss', 'client-loss') and not kill_front and not local_mode:
                        fault = fault_module.FaultWire(ports[0] if route == 'jump' else ports[1])
                        fault.__enter__()
                        client_ports = list(ports); client_ports[0 if route == 'jump' else 1] = fault.port
                        payload['ports'] = client_ports
                    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                        server.daemon_threads = True; thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
                        try:
                            port = server.server_address[1]
                            if args.macos:
                                output = run(str(index), ['swift', 'test', '--skip-build', '--package-path', source / 'apple/GhostermApp',
                                    '--filter', 'ExecutorSSHAppTests.testRemoteExecutionThroughAppModel'],
                                    {'GHOSTERM_EXECUTION_PORT': str(port), 'GHOSTERM_EXECUTION_TOKEN': token, 'GT_EXECUTION_APP_FIXTURE': str(observation), 'GT_SELECTED': 'approved'}, 180)
                                if 'Executed 1 test, with 0 failures' not in output or 'skipped' in output.lower(): raise RuntimeError('Expected one executed Swift test')
                            else:
                                ephemeral = run('device-ephemeral-range', [*adb, 'shell', 'cat', '/proc/sys/net/ipv4/ip_local_port_range']).split()
                                if len(ephemeral) != 2 or not all(value.isdigit() for value in ephemeral):
                                    raise RuntimeError('Device ephemeral port range unavailable')
                                low, high = map(int, ephemeral)
                                if not 1024 <= low <= high <= 65535: raise RuntimeError('Invalid device ephemeral port range')
                                start, end = (10000, min(low, 32768)) if low > 10000 else (high + 1, 65536)
                                if start >= end: raise RuntimeError('No device listener range outside ephemeral connections')
                                def reverse(host_port):
                                    # Host-chosen numbers are not reservations on the
                                    # device. Pick outside its client ephemeral range,
                                    # use no-rebind, and verify the actual mapping.
                                    # tcp:0 is avoided: pinned adb 37.0.1 aborted in
                                    # handle_packet during two such owned probes.
                                    for attempt in range(32):
                                        device_port = start + secrets.randbelow(end - start)
                                        command = [*adb, 'reverse', '--no-rebind', f'tcp:{device_port}', f'tcp:{host_port}']
                                        result = subprocess.run(list(map(str, command)), cwd=ROOT, env=env,
                                            capture_output=True, text=True, timeout=15)
                                        output = result.stdout + result.stderr
                                        (logs / f'reverse-{index}-{attempt}.log').write_text(output)
                                        if result.returncode:
                                            if 'Address already in use' in output or 'cannot rebind existing socket' in output: continue
                                            raise RuntimeError('ADB reverse failed: ' + output)
                                        mapped.append(device_port)
                                        actual = run('reverse-list', [*adb, 'reverse', '--list'])
                                        if not any(line.split()[-2:] == [f'tcp:{device_port}', f'tcp:{host_port}'] for line in actual.splitlines()):
                                            raise RuntimeError('ADB mapping did not match the owned host endpoint')
                                        return device_port
                                    raise RuntimeError('Could not reserve a device listener without rebinding')
                                fixture_port = reverse(port)
                                first_hop = 0 if route == 'jump' else 1
                                payload['ports'] = list(payload['ports'])
                                payload['ports'][first_hop] = reverse(payload['ports'][first_hop])
                                output = run(str(index), [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                                    'com.hanxi.ghosterm.ExecutorSSHAppTest#remoteExecutionThroughViewModelAndConsentUI',
                                    '-e', 'fixturePort', fixture_port, '-e', 'fixtureToken', token,
                                    'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], seconds=180)
                                if 'OK (1 test)' not in output or 'INSTRUMENTATION_STATUS: numtests=1\n' not in output or any(
                                    mark in output for mark in ['INSTRUMENTATION_FAILED', 'assumption failed:', 'INSTRUMENTATION_STATUS_CODE: -4']):
                                    remote = (observation / 'stderr').read_text(errors='replace') if (observation / 'stderr').exists() else ''
                                    raise RuntimeError(case + '\n' + output + '\nOWNED AUDIT: ' + repr(observe()) + '\nREMOTE STDERR: ' + remote)
                            deadline = time.monotonic() + 15
                            while time.monotonic() < deadline:
                                audit = observe()
                                if not any(identity(pid) == expected for pid, expected in owned_pids.items()) and not list(stage.iterdir()): break
                                time.sleep(.05)
                            else: raise RuntimeError(case + ': remote process/staging retained: ' + repr(dict(
                                audit=observe(), livePids=[pid for pid,expected in owned_pids.items() if identity(pid)==expected],
                                staging=sorted(str(path.relative_to(stage)) for path in stage.rglob('*')))) + '\n' +
                                ((observation / 'stderr').read_text(errors='replace') if (observation / 'stderr').exists() else 'Native local transport: no supervisor stderr file'))
                            if local_mode and mode not in ('local-no-approval', 'local-digest', 'local-cwd', 'no-approval', 'client-no-approval', 'client-undeclared'):
                                if not audit.get('localProcess', {}).get('reaped'): raise RuntimeError('Local child not reaped in Native observation: ' + repr(audit))
                            if 'status' not in audit:
                                audit['exitObservation'] = 'native-process-snapshot' if 'localProcess' in audit else 'not-started' if mode in (
                                    'no-approval', 'client-no-approval', 'client-undeclared', 'local-no-approval', 'local-digest', 'local-cwd') else 'unknown'
                            if foreground is not None:foreground.close()
                            terminal_info = None
                            if terminal is not None:
                                terminal_info = terminal.finish(); terminal.close(); terminal_info["cleanupComplete"] = terminal.stopped
                            records.append(dict(case=case, audit=audit, terminal=terminal_info)); print('PASS ' + case + ' ' + json.dumps(audit), flush=True)
                        finally:
                            primary = sys.exc_info()[0] is not None
                            cleanup_errors = []
                            def cleanup(action):
                                try: action()
                                except Exception as failure: cleanup_errors.append(str(failure))
                            for p in mapped:
                                cleanup(lambda p=p: run('reverse-remove', [*adb, 'reverse', '--remove', f'tcp:{p}'], seconds=15))
                            # A lost ADB server must not skip the fixture thread,
                            # fault wire, or observed-child cleanup, or mask the
                            # original failure with a secondary removal error.
                            cleanup(server.shutdown); thread.join(timeout=5); cleanup(server.server_close)
                            if thread.is_alive(): cleanup_errors.append('Owned fixture service did not stop')
                            if fault is not None: cleanup(lambda: fault.__exit__(None, None, None))
                            cleanup(observe)
                            if foreground is not None:cleanup(foreground.close)
                            for pid, expected in owned_pids.items():
                                if identity(pid) == expected:
                                    # Only this fixture's observed child IDs; never the caller/SSHD group.
                                    try: os.kill(pid, signal.SIGKILL)
                                    except ProcessLookupError: continue
                                    for _ in range(100):
                                        if not alive(pid): break
                                        time.sleep(.01)
                            if cleanup_errors:
                                message = 'Owned fixture cleanup: ' + repr(cleanup_errors)
                                if primary: print(message, file=sys.stderr, flush=True)
                                else: raise RuntimeError(message)
                    for p in package.rglob('*'):
                        if p.is_file() and (b'PRIVATE KEY-----' in p.read_bytes() or b'openssh-key-v1\0' in p.read_bytes()): raise RuntimeError('Fixture key serialized')
        if any(sha(source / p) != value for p, value in platform_binaries.items()): raise RuntimeError('Actual platform binaries changed during conformance')
        if any(sha(ROOT / p) != value for p, value in hashes.items()) or any(sha(Path(p)) != value for p, value in binary_hashes.items()):
            raise RuntimeError('Inputs changed during conformance')
        receipt = dict(accepted=True, platform='macos' if args.macos else 'android', cases=records, inputs=hashes,
            binaries=binary_hashes, platformBinaries=platform_binaries, nodeVersion=version, sdkCatalog=sdk_digest, tmuxTopology=topology, terminalStopped=stop_terminal, foregroundAttachment=attaching, foregroundPaneKilled=kill_front,
            scope=('macOS Native local pipes; no SSH connection or credentials; actual gterm and Node/Exec' if local_mode else
                'owned loopback direct/ProxyJump; actual Native App and Node/Exec; separate SSH PTY/tmux when selected') + '; no model')
        if args.receipt:
            if not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): raise RuntimeError('Use project tmp for receipt')
            args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
        print(('EXECUTION LOCAL APP PASS: ' if local_mode else 'EXECUTION SSH APP PASS: ') + str(len(records)) + ' actual cases', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--macos', action='store_true'); target.add_argument('--android'); target.add_argument('--headless-api', choices=['29', '36'])
    parser.add_argument('--build', type=Path, required=True); parser.add_argument('--node', type=Path, required=True)
    parser.add_argument('--project', type=Path, help='explicit isolated source project under this repository tmp')
    parser.add_argument('--tmux', choices=['target', 'nested'], help='owned ordinary SSH PTYs, distinct from plugin exec')
    parser.add_argument('--stop-tmux', action='store_true', help='stop owned ordinary terminal before further plugin requests')
    parser.add_argument('--attach', action='store_true', help='explicitly connect an owned foreground IPC instance through SSH')
    parser.add_argument('--kill-front-pane', action='store_true', help='close only the owned foreground pane during an in-flight operation')
    parser.add_argument('--local', action='store_true', help='macOS Native local process transport; no SSH listener or credentials')
    parser.add_argument('--sdk', type=Path, required=True); parser.add_argument('--case', action='append'); parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if args.local and (not args.macos or args.tmux): parser.error('--local currently requires macOS without tmux')
    if args.project and not args.project.resolve().is_relative_to(ROOT / 'tmp'): parser.error('--project must be an owned project tmp directory')
    if args.headless_api:
        helper = load('execution_owned_headless', 'scripts/test-plugin-codex-patch-android.py')
        with helper.headless(args.headless_api) as serial: execute(args, serial)
    else: execute(args, args.android)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
