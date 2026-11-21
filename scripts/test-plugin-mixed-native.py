#!/usr/bin/env python3
"""Real Native App mixed groups over local/direct/two-hop owned channels.

Fixtures, packages and logs stay in project tmp. SSH/signing private material
stays in memory; macOS runs offscreen and Android uses an owned no-window AVD.
No user service, account or physical device is used.
"""
import argparse
import base64
from contextlib import ExitStack
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import pwd
import re
import secrets
import shutil
import signal
import socketserver
import struct
import subprocess
import sys
import threading
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_inputs(inputs):
    changed = []
    for name, digest in sorted(inputs.items()):
        path = ROOT / name
        if not path.is_file() or sha(path) != digest:
            changed.append(name)
    if changed:
        raise RuntimeError('root inputs changed: ' + ', '.join(changed))


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--node', required=True, type=Path)
    parser.add_argument('--sdk', required=True, type=Path, help='bundle built by build-plugin-node-sdk.mjs')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--route', action='append', choices=('local', 'direct', 'jump'))
    parser.add_argument('--mutate', action='store_true', help='compiled copied-worker regression, independent control, restore and hashes')
    parser.add_argument('--project', type=Path, help='explicit owned copied-project cache with its mixed-fixture marker')
    parser.add_argument('--android-api', choices=('29', '36'), help='run the owned no-window Android backend instead of macOS')
    parser.add_argument('--android-repeat', type=int, choices=range(1, 11), default=1, metavar='1–10',
                        help='repeat each Android baseline route with the same AVD and APKs')
    parser.add_argument('--android-mutation-case', choices=('android-parent-pump', 'android-rejected-admission-generation'),
                        help='run one Android mixed-group mutation after the baseline')
    parser.add_argument('--android-revocation', action='store_true', help='exercise a signed managed Android Session revoked before consent')
    parser.add_argument('--macos-stop-disconnect', action='store_true', help='reset the owned macOS SSH route after explicit Runtime stop starts')
    parser.add_argument('--macos-group-revocation', action='store_true', help='stop a managed macOS SSH group instance from Settings after revocation')
    args = parser.parse_args()
    if args.android_mutation_case and (not args.android_api or not args.mutate or args.android_revocation):
        parser.error('--android-mutation-case requires Android mixed-group mutation mode')
    if args.android_api and 'local' in (args.route or ()):
        parser.error('Android uses direct/jump routes')
    if args.android_repeat > 1 and (not args.android_api or args.mutate or args.android_revocation):
        parser.error('--android-repeat requires an Android baseline without mutations or revocation')
    if args.android_revocation and not args.android_api:
        parser.error('--android-revocation requires --android-api')
    if args.macos_stop_disconnect and (args.android_api or 'local' in (args.route or ())):
        parser.error('--macos-stop-disconnect requires macOS direct/jump routes')
    if args.macos_group_revocation and (args.android_api or args.macos_stop_disconnect or args.mutate or 'local' in (args.route or ())):
        parser.error('--macos-group-revocation runs the dedicated macOS direct/jump baseline')
    build, node, sdk, out = args.build.resolve(strict=True), args.node.resolve(strict=True), args.sdk.resolve(strict=True), args.out.absolute()
    if not build.is_relative_to(ROOT / 'tmp') or not sdk.is_relative_to(ROOT / 'tmp') or not out.is_relative_to(ROOT / 'tmp'):
        parser.error('build, SDK and new output must be in project tmp')
    if not (sdk / 'node/runtime/bootstrap.mjs').is_file():
        parser.error('SDK must be a Node SDK bundle built by build-plugin-node-sdk.mjs')
    if out.exists() or out.is_symlink():
        parser.error('output must be a new owned directory')
    cache = (build / 'CMakeCache.txt').read_text()
    for option in ('GHOSTERM_WITH_PLUGIN_JS', 'GHOSTERM_WITH_LIBSSH2', 'GHOSTERM_WITH_RELEASE_SIGNATURE'):
        if option + ':BOOL=ON' not in cache:
            parser.error('requires explicit JS/SSH/signature-enabled gterm')
    crypto = next(Path(line.split('=', 1)[1]) for line in cache.splitlines() if line.startswith('OPENSSL_CRYPTO_LIBRARY:FILEPATH='))
    if not crypto.is_file():
        parser.error('enabled build must identify its actual OpenSSL crypto library')
    platform_inputs = ('android', 'terminal/android') if args.android_api else ('apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal')
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in (*platform_inputs,
        'core/include', 'core/src', 'core/tests', 'cli/gterm', 'packages/plugin-sdk') for p in (ROOT / folder).rglob('*')
        if p.is_file() and p.name != 'local.properties' and not any(part in ('.build', '.swiftpm', '__pycache__', 'build', '.gradle', '.kotlin', '.cxx')
            for part in p.relative_to(ROOT / folder).parts)}
    for filename in ('test-plugin-mixed-native.py', 'test-plugin-ssh.py', 'test-plugin-ssh-stream.py', 'test-plugin-route.py',
        'test-plugin-release.py', 'fixtures/plugin-dev-executor.c', 'fixtures/plugin-release-agent.mjs'):
        inputs['scripts/' + filename] = sha(ROOT / 'scripts' / filename)
    if args.android_api:
        for filename in ('lib/plugin-mixed-android.py', 'lib/tmux-fault-proxy.py', 'test-plugin-codex-patch-android.py', 'test-plugin-channel.py'):
            inputs['scripts/' + filename] = sha(ROOT / 'scripts' / filename)
    if args.macos_stop_disconnect:
        inputs['scripts/lib/tmux-fault-proxy.py'] = sha(ROOT / 'scripts/lib/tmux-fault-proxy.py')
    source = args.project.absolute() if args.project else out / ('android-project' if args.android_api else 'project')
    marker = source / '.mixed-fixture-project.json'
    if not source.is_relative_to(ROOT / 'tmp') or source.is_symlink():
        parser.error('copied project must be in project tmp')
    if source.exists() and (not marker.is_file() or json.loads(marker.read_text()) != {'root': str(ROOT)}):
        parser.error('refuse to overwrite an unmarked copied project')
    out.mkdir(parents=True, mode=0o700)
    logs = out / 'logs'; logs.mkdir()
    env = dict(os.environ, TMPDIR=str(out), DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',
        GHOSTERM_AUTOMATION_OFFSCREEN='1', GHOSTERM_CORE_TEST_PLUGIN_JS='1')
    commands = []

    def run(label, command, seconds=300, input_bytes=None, assertion=False):
        process = subprocess.Popen(list(map(str, command)), cwd=ROOT, env=env, stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            output, errors = process.communicate(input_bytes, timeout=seconds)
        except BaseException:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
            try:
                process.communicate(timeout=8)
            except subprocess.TimeoutExpired:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                process.communicate(timeout=3)
            raise
        text = (output + errors).decode('utf-8', errors='replace')
        (logs / (label + '.log')).write_text(text)
        commands.append(dict(label=label, returncode=process.returncode))
        abnormal = any(mark in text for mark in ('AddressSanitizer', 'runtime error:', 'unexpected signal', 'fatalError'))
        if assertion:
            failures = re.findall(r'^.*: error: -\[.*\] : (?:XCTAssert\w+ failed|failed - )[^\n]*', text, re.M)
            if process.returncode != 1 or abnormal or len(failures) != 1 or 'Executed 1 test, with 1 failure (0 unexpected)' not in text:
                raise RuntimeError(label + ': expected one compiled ordinary assertion\n' + text[-12000:])
        elif process.returncode or abnormal:
            raise RuntimeError(label + '\n' + text[-12000:])
        return output, text

    peer = out / 'executor'
    run('exec-build', ['xcrun', '--sdk', 'macosx', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-DGT_DEV_RESOURCE_TIMEOUT_MS=10000', '-I' + str(ROOT / 'core/src'), ROOT / 'scripts/fixtures/plugin-dev-executor.c', build / 'core/libghosterm_core.a', '-lpthread', '-lm', '-o', peer])
    package = out / 'package'; package.mkdir()
    code = """import {object,text,integer} from '@ghosterm/plugin-sdk';
export function activate(ctx){
 const codec=object({text:text(64),count:integer(0,100)});
 // A diagnostic must fit the output codec or the SDK replaces it with HANDLER_INTERRUPTED.
 const diagnostic=(kind,detail,count)=>({
  text:('publish:'+kind+':'+String(detail).replace(/[^ -~]/g,'?')).slice(0,64),count
 });
 ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{
  c.check(); if(v.text==='resource'){
   try {
    const result=await ctx.resources.publish({root:'workspace',relativePath:'file.bin',mediaType:'application/octet-stream'},
     {byteLimit:'262144',timeoutMs:10000}).outcome;
    if(result.kind!=='success')return diagnostic(result.kind,result.error?.code||result.reason||'',v.count);
    if(result.value.size.kind!=='known'||result.value.size.bytes!=='262144')return {text:'publish:invalid-size',count:v.count};
   } catch(error){return diagnostic('threw',error,v.count);}
  } c.check(); return v;
 }); return {dispose(){}};
}
"""
    requires = ['session.rpc', 'resource.receive', 'file.readScoped']
    variants = [dict(id='portable', runtime='portable-js', entry='executor/index.js', requires=requires),
        dict(id='node', runtime='node', entry='executor/index.mjs', runtimeVersionRange='>=20.0.0 <1000.0.0', requires=requires),
        dict(id='exec', runtime='exec', executable='executor/main', requires=requires,
            os=[{'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]],
            arch=[{'arm64': 'aarch64', 'aarch64': 'aarch64', 'x86_64': 'x86_64'}[platform.machine()]])]
    manifest = dict(schemaVersion=1, id='org.ghosterm.mixed-fixture', pluginVersion='0.1.0', sdkApiRange='^1.0.0',
        bridgeProtocolRange=[1], publisher='fixture', license='MIT',
        commands=[dict(path=['fixture'], title='Owned mixed-runtime fixture', argsSchema='schemas/input.json')],
        client=dict(runtime='portable-js', entry='client/index.js', requires=['session.rpc', 'resource.receive']),
        executor=dict(variants=variants), permissions=[dict(side='executor', capability='file.readScoped',
            purpose='Only the selected disposable workspace', scope='instance')])
    schema = dict(type='object', properties={}, required=[], additionalProperties=False, **{'x-ghosterm-cli': dict(version=1, fields={})})
    files = {'plugin.json': json.dumps(manifest).encode(), 'schemas/input.json': json.dumps(schema).encode(),
        'client/index.js': b'export function activate(ctx){if(ctx.resources.workspaces.length)throw Error("Client roots");return {dispose(){}}}',
        'executor/index.js': code.encode(), 'executor/index.mjs': code.encode(), 'executor/main': peer.read_bytes(),
        'README': b'Owned mixed-runtime conformance fixture; not an official business plugin', 'LICENSES/MIT.txt': (ROOT / 'LICENSE').read_bytes()}
    for name, data in files.items():
        path = package / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
    (package / 'executor/main').chmod(0o700)
    (package / 'integrity.json').write_text(json.dumps(dict(schemaVersion=1, members=[
        dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()) for name, data in sorted(files.items())])))
    digest = sha(package / 'integrity.json')
    artifact = out / 'plugin.zip'
    with zipfile.ZipFile(artifact, 'w') as archive:
        for path in sorted(package.rglob('*')):
            if path.is_file():
                info = zipfile.ZipInfo(path.relative_to(package).as_posix(), date_time=(2020, 1, 1, 0, 0, 0))
                info.create_system = 3; info.external_attr = (0o100755 if path.name == 'main' else 0o100644) << 16
                archive.writestr(info, path.read_bytes())
    host = out / 'host.json'
    host.write_text(json.dumps(dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=['portable-js'],
        capabilities=requires, variantId='portable')))
    run('package-check', [build / 'gterm', 'plugin', 'check', '--host-json', host, '--', package])
    sdk_hash = hashlib.sha256()
    for path in sorted((p for p in sdk.rglob('*') if p.is_file()), key=lambda p: p.relative_to(sdk).as_posix().encode()):
        name = path.relative_to(sdk).as_posix().encode()
        sdk_hash.update(len(name).to_bytes(4, 'big') + path.stat().st_size.to_bytes(8, 'big') + name + sha(path).encode())
    node_version = run('node-version', [node, '--version'])[0].decode().strip()[1:]
    release = load('mixed_signing', 'test-plugin-release.py')
    keys = load('mixed_keys', 'test-plugin-ssh.py')
    streams = load('mixed_streams', 'test-plugin-ssh-stream.py')
    routes = load('mixed_routes', 'test-plugin-route.py')
    with ExitStack() as stack:
        public, signer = stack.enter_context(release.agent(node, out))
        key_id = hashlib.sha256(base64.b64decode(public)).hexdigest()
        policy = out / 'policy.json'
        policy.write_text(json.dumps(dict(schemaVersion=1, channel='fixture', keys=[dict(id=key_id, algorithm='ed25519', publicKey=public,
            publisher=manifest['publisher'], pluginIds=[manifest['id']])], revokedKeyIds=[])))
        descriptor = out / 'release.json'
        descriptor.write_text(json.dumps(dict(schemaVersion=1, channel='fixture', keyId=key_id, pluginId=manifest['id'],
            pluginVersion=manifest['pluginVersion'], publisher=manifest['publisher'], packageSHA256=digest,
            artifact=dict(format='zip-v1', bytes=artifact.stat().st_size, sha256=sha(artifact)))))
        # The client subcommand returns a signature only; never write signing private material.
        signed = subprocess.run([node, ROOT / 'scripts/fixtures/plugin-release-agent.mjs', 'client', signer],
            input=release.DOMAIN + descriptor.read_bytes(), capture_output=True, check=True, timeout=5)
        signature = out / 'release.sig'; signature.write_bytes(signed.stdout)
        store, catalog, install_root = out / 'store', out / 'catalog', out / 'install-root'
        catalog.mkdir(mode=0o700); install_root.mkdir(mode=0o700); (install_root / 'file.bin').write_bytes(b'owned preflight')
        result = json.loads(run('install', [build / 'gterm', 'plugin', 'install', '--catalog', catalog, '--expect-revision', '0',
            '--store', store, '--artifact', artifact, '--descriptor', descriptor, '--signature', signature, '--policy', policy,
            '--host-json', host, '--', '--client-digest', digest, '--variant', 'portable', '--root', 'workspace', install_root,
            '--method', 'fixture.echo'])[0])
        if not result.get('installed') or not result.get('committed'):
            raise RuntimeError('fixture signed version was not installed and registered')
        installed = store / sha(descriptor)
        revocation = None
        if args.android_revocation or args.macos_group_revocation:
            authority = out / 'channel-authority'
            authority.mkdir()
            channel_root, channel_signer = stack.enter_context(release.agent(node, authority))
            now = int(time.time())
            def signed_policy(generation, data):
                statement = json.dumps(dict(schemaVersion=1, channel='fixture', generation=str(generation),
                    issuedAt=str(now - 60), expiresAt=str(now + 1800), policySHA256=hashlib.sha256(data).hexdigest())).encode()
                proof = subprocess.run([node, ROOT / 'scripts/fixtures/plugin-release-agent.mjs', 'client', channel_signer],
                    input=b'ghosterm.channel-policy.v1\0' + statement, capture_output=True, check=True, timeout=5).stdout
                return {name: base64.b64encode(value).decode() for name, value in
                        (('statement', statement), ('signature', proof), ('policy', data))}
            revoked = json.loads(policy.read_bytes())
            revoked['revokedKeyIds'] = [key_id]
            revocation = dict(root=channel_root, initial=signed_policy(1, policy.read_bytes()),
                revoked=signed_policy(2, json.dumps(revoked).encode()))
            if args.android_revocation:
                revocation.update({name: base64.b64encode(path.read_bytes()).decode() for name, path in
                    (('artifact', artifact), ('descriptor', descriptor), ('signature', signature))})
        if args.android_api:
            backend = load('mixed_android_backend', 'lib/plugin-mixed-android.py')
            report = backend.execute(args.android_api, args.route, args.mutate, build, node, sdk, out, package, installed,
                policy, sdk_hash.hexdigest(), node_version, run, env, load, inputs, source, revocation,
                mutation_case=args.android_mutation_case, repeat=args.android_repeat)
            verify_inputs(inputs)
            for path in logs.iterdir():
                if b'PRIVATE KEY-----' in path.read_bytes(): raise RuntimeError('fixture private material leaked to a log')
            receipt = dict(accepted=True, inputs=inputs, commands=commands, **report,
                binaries={str(path): sha(path) for path in (build / 'gterm', build / 'core/libghosterm_core.a', peer, node, crypto)},
                logs={path.name: sha(path) for path in logs.iterdir()},
                scope=('owned no-window Android cross-process revocation, explicit stop and release, and SSH reset after stop request'
                    if revocation else 'owned no-window Android Native mixed groups; consent/RPC/resources, signed stale-policy rejection and online/empty reuse'))
            (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
            return

        for folder in ('apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal'):
            # Refresh only fixture-source links; leave compiled-cache links alone.
            copied = source / folder
            if copied.exists():
                for link in copied.rglob('*'):
                    if link.is_symlink() and not any(part in ('.build', '.swiftpm') for part in link.relative_to(copied).parts):
                        link.unlink()
            shutil.copytree(ROOT / folder, source / folder, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'), dirs_exist_ok=True)
        shutil.copytree(ROOT / 'core/include', source / 'core/include', dirs_exist_ok=True)
        core_link = source / 'core/build'
        if core_link.is_symlink(): core_link.unlink()
        core_link.symlink_to(build / 'core', target_is_directory=True)
        for folder in ('build', 'patches'):
            link = source / folder
            if link.is_symlink(): link.unlink()
            link.symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir(exist_ok=True)
        marker.write_text(json.dumps({'root': str(ROOT)}))
        for name, digest in inputs.items():
            copied = source / name
            if name.startswith(('apple/', 'core/include/')) and (not copied.is_file() or sha(copied) != digest):
                raise RuntimeError('copied input mismatch: ' + name)
        # Crypto comes from the explicitly enabled CMake build, never a guessed library.
        command = ['xcrun', 'swift', 'test', '--package-path', source / 'apple/GhostermApp', '-Xlinker', crypto,
            '--filter', 'ExtensionMixedConnectionTests.testEightMixedExecutorsKeepResourcesAndAdmissionFailuresIndependent']
        run('compile', ['xcrun', 'swift', 'build', '--build-tests', '--package-path', source / 'apple/GhostermApp', '-Xlinker', crypto], seconds=600)
        private, client_public = streams.memory_identity()
        agent = stack.enter_context(keys.owned_agent(out, 'a'))
        host_keys = [keys.add_memory_key(agent, 'mixed-hop-' + str(i)) for i in range(2)]
        route_root = out / 'r'; route_root.mkdir()
        ports, _, _ = stack.enter_context(routes.chain(route_root, 2, client_public, host_keys, agent, 'echo'))
        faults = load('mixed_macos_ssh_faults', 'lib/tmux-fault-proxy.py') if args.macos_stop_disconnect else None
        completed = []
        def actual(route, label, assertion=False, compile=False):
            scratch = out / label; scratch.mkdir(); (scratch / 'stage').mkdir()
            for slot in range(8):
                root = scratch / ('workspace-' + str(slot)); root.mkdir()
                (root / 'file.bin').write_bytes(bytes((i + slot * 17) % 251 for i in range(262144)))
            token = secrets.token_hex(24)
            endpoint_ports = list(ports)
            proxy = None
            if faults:
                first = 0 if route == 'jump' else 1
                proxy = faults.SSHFaultProxy('127.0.0.1', ports[first], scratch)
                stack.callback(proxy.close)
                endpoint_ports[first] = proxy.port
            payload = dict(route=route, scratch=str(scratch), package=str(package), installed=str(installed / 'payload'),
                release=str(installed), policy=str(policy), policySHA256=sha(policy), ports=endpoint_ports, disconnectOnStop=bool(proxy),
                hostKeys=[key.decode() for key in host_keys], user=pwd.getpwuid(os.getuid()).pw_name, pem=private.decode(),
                gterm=str(build / 'gterm'), gtermSHA256=sha(build / 'gterm'), node=str(node), nodeVersion=node_version,
                nodeSHA256=sha(node), sdk=str(sdk), sdkSHA256=sdk_hash.hexdigest())
            if args.macos_group_revocation:
                payload.update(revocation=revocation, artifact=str(artifact), descriptor=str(descriptor), signature=str(signature))
            class Handler(socketserver.StreamRequestHandler):
                def handle(self):
                    self.connection.settimeout(5)
                    request = self.rfile.readline(128).rstrip(b'\n')
                    if proxy and request in (f'{token}:blackhole'.encode(), f'{token}:reset'.encode()):
                        with proxy.lock:
                            connected = bool(proxy.links)
                        if connected:
                            proxy.fault(request.rsplit(b':', 1)[1].decode())
                            self.wfile.write(b'OK\n')
                        return
                    if request != token.encode():
                        return
                    body = json.dumps(payload, separators=(',', ':')).encode()
                    self.wfile.write(struct.pack('>I', len(body)) + body)
            with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                thread = threading.Thread(target=server.serve_forever); thread.start()
                env.update(GHOSTERM_MIXED_PORT=str(server.server_address[1]), GHOSTERM_MIXED_TOKEN=token)
                try:
                    selected = command if compile else [command[0], *command[1:3], '--skip-build', *command[3:]]
                    _, text = run(label, selected, seconds=180, assertion=assertion)
                finally:
                    server.shutdown(); thread.join(5)
            if thread.is_alive() or (not assertion and ('Executed 1 test, with 0 failures' not in text or 'skipped' in text.lower())):
                raise RuntimeError(label + ': exact discovery/no-skip evidence missing')
        for route in args.route or (('direct', 'jump') if faults or args.macos_group_revocation else ('local', 'direct', 'jump')):
            actual(route, route)
            completed.append(route)
            print('PASS mixed Native ' + route, flush=True)
        mutations = []
        recipes = [
            ('remote-exit-unknown', 'ExtensionConnectionModel',
             'if done && state?.peerEOF != true && process?.reaped != true { issue = "连接已释放；执行端退出未确认，请检查各实例回执" }'.encode(),
             b'', 1, 'released connection must preserve unconfirmed remote exit'),
            ('local-release-error', 'ExtensionController',
             b'guard name.withCString({ unlinkat(resourceRootFD, $0, AT_REMOVEDIR) }) == 0 else {\n'
             b'                        throw ControllerError.resourceUnavailable\n'
             b'                    }',
             b'_ = name.withCString { unlinkat(resourceRootFD, $0, AT_REMOVEDIR) }', 1,
             'local cleanup failure must not report closure'),
            ('revocation-group-owner', 'AppModel', b'let grouped = extensionConnections.flatMap(\\.instances)',
             b'let grouped: [ExtensionInstanceModel] = []', 1,
             'a live group owner remains a channel stop target without its workspace pane'),
        ] if faults else [
            ('parent-pump', 'ExtensionController', b'                    _ = try self.run.parent?.multiplexer?.step()\n', b'', 2, None),
            ('rejected-admission-generation', 'ExtensionInstanceModel',
             b'            await onRejectedAdmission?()\n            try checkExtensionRunGeneration(generation)\n',
             b'            await onRejectedAdmission?()\n', 1,
             'Late rejected admission was returned after closing its original run'),
        ]
        for mutation, stem, anchor, replacement, occurrences, expected in recipes if args.mutate else []:
            filename = f'apple/GhostermApp/Sources/GhostermAppModel/{stem}.swift'
            target = source / filename; original = target.read_bytes()
            if original.count(anchor) != occurrences:
                raise RuntimeError('ambiguous macOS mixed mutation: ' + mutation)
            objects = [p for p in (source / 'apple/GhostermApp/.build').rglob('*.o')
                       if p.name in (stem + '.o', stem + '.swift.o')]
            before = {str(p): sha(p) for p in objects}
            if not before: raise RuntimeError('missing compiled worker object')
            try:
                route = completed[0] if mutation == 'rejected-admission-generation' else ('direct' if faults else 'local')
                target.write_bytes(original.replace(anchor, replacement))
                actual(route, mutation + '-red', assertion=True, compile=True)
                if expected and expected not in (logs / (mutation + '-red.log')).read_text():
                    raise RuntimeError('missing targeted assertion: ' + mutation)
                if not any(p.is_file() and sha(p) != before[str(p)] for p in objects):
                    raise RuntimeError('mutated worker did not recompile')
                _, control = run(mutation + '-control', [*command[:-2], '--filter', 'HostListTests.testReadingDoesNotMutateConfig'])
                if 'Executed 1 test, with 0 failures' not in control or 'skipped' in control.lower():
                    raise RuntimeError('independent configuration control did not pass')
            finally:
                target.write_bytes(original)
            actual(route, mutation + '-restored', compile=True)
            if sha(target) != inputs[filename]: raise RuntimeError('worker restoration mismatch')
            mutations.append(mutation)
            print('PASS ' + mutation + ': compiled ordinary assertion / independent config / restore / object / SHA256', flush=True)
    verify_inputs(inputs)
    for path in logs.iterdir():
        if b'PRIVATE KEY-----' in path.read_bytes():
            raise RuntimeError('fixture private material leaked to a log')
    receipt = dict(accepted=True, inputs=inputs, commands=commands, routes=completed, mutations=mutations,
        artifacts={str(path): sha(path) for path in (build / 'gterm', build / 'core/libghosterm_core.a', peer, node, crypto)},
        logs={path.name: sha(path) for path in logs.iterdir()},
        scope=('macOS App-owned SSH reset after explicit stop, local release failure and retry, remote exit unknown'
            if args.macos_stop_disconnect else 'macOS managed SSH group revocation and Settings stop without a workspace pane'
            if args.macos_group_revocation else 'macOS App-owned local/direct/two-hop mixed Portable/Node/Exec, per-instance consent/RPC/resources, signed stale-policy rejection and online/empty reuse'))
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    def interrupted(number, frame):
        raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
