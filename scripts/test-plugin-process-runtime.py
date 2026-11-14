#!/usr/bin/env python3
"""Actual checked Node/Exec packages, native Session RPC and bounded cleanup.

Only isolated fixtures below repository tmp/ and explicitly installed Node /
locked developer tools. No account, remote server, model, UI or runtime install.
--mutate requires compiled normal assertion reds, configuration controls and
restored greens; crashes, sanitizer reports and timeouts are never target reds.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'core/src/extension/process-runtime.c'
BOOTSTRAP = 'packages/plugin-sdk/node/runtime/bootstrap.mjs'
CASES = [
    ('execution-approval', SOURCE, '!c->execution_authorized', 'false', 'exec:denied'),
    ('package-identity', SOURCE, 'info.id,', '"org.ghosterm.bad",', 'exec:wrong-identity'),
    ('sdk-identity', SOURCE, 'strcmp(digest, c->sdk_catalog_sha256)', 'false', 'node:bad-sdk'),
    ('executable-identity', SOURCE, 'strcmp(digest, r->executable_digest) || !(r->executable_identity.st_mode & 0111)', 'false || !(r->executable_identity.st_mode & 0111)', 'node:wrong-sha'),
    ('environment-injection', SOURCE, '!strncmp(text, "NODE_OPTIONS=", 13)', 'false', 'exec:bad-env'),
    ('environment-names', SOURCE, '!gt__ext_process_environment_valid(c->environment, c->environment_count)', 'false', 'exec:bad-env-name'),
    ('version-probe', SOURCE, 'strcmp(expected, r->version_output)', 'false', 'node:wrong-version'),
    ('activation-handshake', SOURCE, 'gt__process_rpc_activated(r->rpc) &&', 'true &&', 'node:activation-reject'),
    ('stdout-budget', SOURCE, 'io.count > r->stdout_limit - r->info.stdout_bytes', 'false', 'exec:stdout-limit'),
    ('stderr-budget', SOURCE, 'error_io.count > r->stderr_limit - r->info.stderr_bytes', 'false', 'node:stderr-limit'),
    ('live-destroy', SOURCE, '(*out)->info.phase != GT_PROCESS_RUNTIME_CLOSED', 'false', 'exec:stop-created'),
    ('leader-group-stop', SOURCE, 'else if (process.exited && !r->killed)', 'else if (false && process.exited && !r->killed)', 'exec:leader-exit'),
    ('stderr-eof', SOURCE, 'if (!r->stderr_eof) {', 'if (true) {', 'exec:stderr-eof'),
    ('module-scope', BOOTSTRAP, "if (!within(file, root) && !within(file, sdkRoot))", 'if (false)', 'node:ambient'),
    ('context-arguments', BOOTSTRAP, 'invocation: snapshot(config.invocation)', 'invocation: snapshot({ command: config.invocation.command, arguments: [] })', 'node:roundtrip'),
]

PLUGIN = r'''import {object,boolean} from '@ghosterm/plugin-sdk';
export async function activate(ctx) {
  const mode = process.env.GT_RUNTIME_MODE;
  if (ctx.side !== 'executor' || !/^[0-9a-f]{32}$/.test(ctx.instanceId) ||
      typeof ctx.epoch !== 'string' || ctx.invocation.command !== 'fixture' ||
      ctx.capabilities.query('session.rpc') !== 'allowed' || !Object.isFrozen(ctx) || process.env.HOME !== undefined || process.env.NODE_OPTIONS !== undefined ||
      Object.keys(ctx).sort().join(',') !== 'cancellation,capabilities,epoch,instanceId,invocation,session,side') throw Error('context');
  if (mode === 'argument-boundary') {
    if (ctx.invocation.arguments.length !== 6 ||
        ['label','second','third'].some(k => ctx.invocation.decodedArguments[k] !== '\x01'.repeat(4096))) throw Error('boundary');
  } else if (ctx.invocation.arguments.length !== 2 || ctx.invocation.arguments[0] !== '--label' ||
      ctx.invocation.decodedArguments.label !== "中文 \"';$()") throw Error('arguments');
  if (mode === 'activation-reject') throw Error('fixture rejected');
  if (mode === 'activation-timeout') { process.stderr.write('activation waiting'); await new Promise(() => {}); }
  const codec = object({peer:boolean()});
  ctx.session.register({name:'fixture.echo',input:codec,output:codec},async (v,c) => { c.check(); return v; });
  return {dispose(){}, step(){
    if (mode === 'busy') { process.stderr.write('busy\n'); while (true) {} }
    if (mode === 'stderr-limit') process.stderr.write('too much\n');
  }};
}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--node', type=Path, required=True)
    p.add_argument('--tools', type=Path, required=True)
    p.add_argument('--mutate', action='store_true')
    p.add_argument('--case', action='append')
    args = p.parse_args()
    if args.case and (not args.mutate or set(args.case) - {c[0] for c in CASES}):
        p.error('--case requires --mutate and known groups')
    node, tools = args.node.resolve(strict=True), args.tools.resolve(strict=True)
    cases = [c for c in CASES if not args.case or c[0] in args.case]
    for label, name, old, _, _ in cases:
        assert (ROOT / name).read_text().count(old) == 1, label
    work = Path(tempfile.mkdtemp(prefix='process-runtime-', dir=ROOT / 'tmp'))
    source, build, logs = work / 'source', work / 'build', work / 'logs'
    logs.mkdir()
    env = {**os.environ, 'TMPDIR': str(work), 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
    env.pop('NODE_OPTIONS', None)
    commands = []

    def run(label, command, red=False):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require process-runtime; shift; gt_timeout 180 "$@"'
        result = subprocess.run(['bash', '-c', shell, 'process-runtime', str(ROOT), *map(str, command)],
                                cwd=ROOT, env=env, capture_output=True, text=True, timeout=190)
        output = result.stdout + result.stderr
        (logs / (label + '.log')).write_text(output)
        commands.append(dict(label=label, returncode=result.returncode, assertion=red))
        if red:
            assert result.returncode == 1 and re.search(r'^FAIL ', result.stderr, re.M) and not any(
                x in output for x in ['AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:']), label + '\n' + output
        else:
            assert result.returncode == 0, label + '\n' + output
        return output

    try:
        def ignore(directory, names):
            return [name for name in names if name == '__pycache__' or
                    ((Path(directory) / name).is_dir() and (name == 'build' or name.startswith('build-')))]
        for folder in ['core', 'packages/plugin-sdk', 'scripts', 'patches/quickjs-ng']:
            shutil.copytree(ROOT / folder, source / folder, ignore=ignore)
        shutil.copyfile(ROOT / 'LICENSE', source / 'LICENSE')
        (source / 'tmp').mkdir()
        (source / 'vendor').mkdir()
        (source / 'vendor/quickjs-ng').symlink_to(ROOT / 'vendor/quickjs-ng', target_is_directory=True)
        inputs = {str(f.relative_to(source)): sha(f) for folder in ['core', 'packages/plugin-sdk',
                  'scripts', 'patches/quickjs-ng'] for f in (source / folder).rglob('*') if f.is_file()}
        for name in ['package.json', 'package-lock.json']:
            assert (tools / name).read_bytes() == (ROOT / 'scripts/fixtures/plugin-sdk-tools' / name).read_bytes()
        node_sha = sha(node)
        version = run('node-version', [node, '--version']).strip()
        bundle = source / 'tmp/sdk'

        def sdk(label):
            if bundle.exists():
                shutil.rmtree(bundle)
            output = run(label + '-sdk', [node, source / 'scripts/build-plugin-node-sdk.mjs', '--tools', tools, '--out', bundle])
            digest = re.search(r'catalog=([0-9a-f]{64})', output).group(1)
            catalog = hashlib.sha256()
            for f in sorted((f for f in bundle.rglob('*') if f.is_file()), key=lambda f: str(f.relative_to(bundle)).encode()):
                name = str(f.relative_to(bundle)).encode()
                catalog.update(len(name).to_bytes(4, 'big') + f.stat().st_size.to_bytes(8, 'big') + name + sha(f).encode())
            assert catalog.hexdigest() == digest
            run(label + '-bootstrap-syntax', [node, '--check', bundle / 'node/runtime/bootstrap.mjs'])
            return digest

        sdk_digest = sdk('initial')
        run('configure', ['cmake', '-S', source / 'core', '-B', build,
                          '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DGHOSTERM_WITH_PLUGIN_JS=ON',
                          '-DGHOSTERM_BUILD_TESTS=ON', '-DGHOSTERM_PLUGIN_SDK_DIR=' + str(bundle / 'portable'),
                          '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g',
                          '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])

        def rebuild(label, changed=None):
            if changed:
                (build / ('CMakeFiles/ghosterm_core.dir/' + changed.removeprefix('core/') + '.o')).unlink(missing_ok=True)
            for name in ['test_process_runtime', 'libghosterm_core.a']:
                (build / name).unlink(missing_ok=True)
            output = run(label + '-build', ['cmake', '--build', build, '--target', 'test_process_runtime',
                                           'test_process_runtime_peer', 'test_config', '-j', '4'])
            assert 'Linking C executable test_process_runtime' in output
            if changed:
                assert changed.removeprefix('core/') + '.o' in output and 'Linking C static library' in output

        rebuild('initial')
        os_name = {'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]
        arch = {'arm64': 'aarch64', 'aarch64': 'aarch64', 'x86_64': 'x86_64'}[platform.machine()]
        sequence = 0

        def target(label, spec, red=False):
            nonlocal sequence
            runtime, mode = spec.split(':')
            sequence += 1
            fixture = work / ('fixture-' + str(sequence))
            package, fixed_sdk, temp = fixture / 'package', fixture / 'sdk', fixture / 'temp'
            temp.mkdir(parents=True)
            shutil.copytree(bundle, fixed_sdk)
            (package / 'executor').mkdir(parents=True)
            (package / 'schemas').mkdir()
            (package / 'LICENSES').mkdir()
            (package / 'LICENSES/MIT.txt').write_text('MIT')
            (package / 'README').write_text('Disposable controller fixture\n')
            (package / 'package.json').write_text('{"type":"module"}')
            schema = {'type': 'object', 'properties': {'label': {'type': 'string', 'maxLength': 64}},
                      'required': ['label'], 'additionalProperties': False,
                      'x-ghosterm-cli': {'version': 1, 'fields': {'label': {'option': 'label', 'default': 'default'}}}}
            if mode == 'argument-boundary':
                schema['required'] = ['label', 'second', 'third']
                for key in ['label', 'second', 'third']:
                    schema['properties'][key] = {'type': 'string', 'maxLength': 4096}
                    schema['x-ghosterm-cli']['fields'][key] = {'option': key, 'default': ''}
            (package / 'schemas/input.json').write_text(json.dumps(schema))
            variant = dict(id='selected', runtime=runtime, requires=['session.rpc'])
            if runtime == 'node':
                variant.update(entry='executor/main.mjs', runtimeVersionRange='>=20.0.0 <1000.0.0')
                plugin = PLUGIN
                if mode == 'ambient':
                    foreign = fixture / 'ambient.mjs'
                    foreign.write_text("import{writeFileSync}from'node:fs';writeFileSync(" + json.dumps(str(fixture / 'ambient-ran')) + ",'executed');")
                    plugin = 'import ' + json.dumps(foreign.as_uri()) + ';\n' + plugin
                (package / 'executor/main.mjs').write_text(plugin)
            else:
                variant.update(executable='executor/main.mjs', os=[os_name], arch=[arch])
                shutil.copyfile(build / 'test_process_runtime_peer', package / 'executor/main.mjs')
            manifest = dict(schemaVersion=1, id='org.ghosterm.launch', pluginVersion='1.0.0', sdkApiRange='^1.0.0',
                            bridgeProtocolRange=[1], publisher='fixture', license='MIT',
                            commands=[dict(path=['fixture'], title='Fixture', argsSchema='schemas/input.json')],
                            executor=dict(variants=[variant]), permissions=[])
            (package / 'plugin.json').write_text(json.dumps(manifest))
            members = [dict(path=str(f.relative_to(package)), bytes=f.stat().st_size, sha256=sha(f))
                       for f in sorted(package.rglob('*')) if f.is_file()]
            (package / 'integrity.json').write_text(json.dumps(dict(schemaVersion=1, members=members)))
            facts = dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=[runtime], capabilities=['session.rpc'],
                         programs=dict(node=dict(state='available', version='199.0.0' if mode == 'wrong-version' else version[1:])),
                         os=os_name, arch=arch)
            fact_file = fixture / 'facts.json'
            fact_file.write_text(json.dumps(facts))
            try:
                run(label, [build / 'test_process_runtime', mode, package, fixed_sdk, temp, node, node_sha, sdk_digest, fact_file], red)
                assert not list(temp.iterdir()), label + ': retained stage'
                if not red:
                    assert not (fixture / 'ambient-ran').exists(), label + ': outside module executed'
            finally:
                # All actual children are stopped/reaped by the C fixture, even
                # on ordinary assertion reds. Preserve abnormal failures to audit.
                if not any(temp.iterdir()):
                    shutil.rmtree(fixture)

        baseline = ['node:' + mode for mode in ['roundtrip', 'retained', 'frozen', 'sdk-frozen', 'argument-boundary',
                    'stop-created', 'denied', 'wrong-sha', 'node-alias', 'bad-sdk', 'bad-env', 'bad-env-name', 'duplicate-env',
                    'bad-arguments', 'bad-command', 'wrong-identity', 'wrong-version', 'activation-reject',
                    'activation-timeout', 'lifetime', 'stdout-limit', 'stderr-limit', 'ambient', 'busy']]
        baseline += ['exec:' + mode for mode in ['roundtrip', 'retained', 'frozen', 'stop-created', 'denied',
                     'bad-env', 'bad-env-name', 'duplicate-env', 'bad-arguments', 'bad-command', 'wrong-identity',
                     'stubborn', 'stdout-limit', 'leader-exit', 'stderr-eof', 'stdout-eof']]
        for spec in baseline:
            target('baseline-' + spec.replace(':', '-'), spec)
            print(spec + ': PASS', flush=True)
        if args.mutate:
            for label, name, old, new, spec in cases:
                path = source / name
                original = path.read_bytes()
                try:
                    path.write_text(original.decode().replace(old, new))
                    if name == BOOTSTRAP:
                        sdk_digest = sdk(label)
                    else:
                        rebuild(label, name)
                    target(label + '-red', spec, True)
                    run(label + '-control', [build / 'test_config', source / 'core/tests/fixtures'])
                finally:
                    path.write_bytes(original)
                    assert sha(path) == hashlib.sha256(original).hexdigest()
                if name == BOOTSTRAP:
                    sdk_digest = sdk(label + '-restored')
                else:
                    rebuild(label + '-restored', name)
                target(label + '-restored', spec)
                print(label + ': compiled assertion red / same archive control green / restored green / SHA256', flush=True)
        for spec in ['node:roundtrip', 'exec:roundtrip']:
            target('final-' + spec.replace(':', '-'), spec)
        for name, digest in inputs.items():
            assert sha(source / name) == digest and sha(ROOT / name) == digest, name
        assert sha(node) == node_sha
        (logs / 'result.json').write_text(json.dumps(dict(accepted=True, baseline=baseline,
            groups=[c[0] for c in cases] if args.mutate else [], commands=commands, inputs_sha256=inputs,
            node_version=version, node_sha256=node_sha, sanitizers=['address', 'undefined'], leak_sanitizer=False), indent=2) + '\n')
        print('PROCESS RUNTIME PASS: ' + str(logs), flush=True)
    except BaseException as error:
        (logs / 'failure.txt').write_text(repr(error))
        raise


if __name__ == '__main__':
    def stop(number, frame):
        raise KeyboardInterrupt('signal ' + str(number))
    for number in [signal.SIGINT, signal.SIGTERM]:
        signal.signal(number, stop)
    main()
