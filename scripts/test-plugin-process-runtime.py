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
RESOURCE = 'core/src/extension/process-resource.inc'
NODE_RESOURCE = 'packages/plugin-sdk/node/resources.ts'
JOBS = 'core/src/extension/process-jobs.c'
JOBS_RPC = 'core/src/extension/process-jobs-rpc.inc'
NODE_PROCESS = 'packages/plugin-sdk/node/process.ts'
PROCESS_CASES = [
    ('jobs-write-grant', JOBS, '!jobs_live(j) || s->stopping', 's->stopping', 'node:process-native'),
    ('jobs-read-grant', JOBS, 'if (!jobs_live(j)) {\n        return (gt_ext_process_io_t){GT_EXT_PROCESS_CLOSED', 'if (false && !jobs_live(j)) {\n        return (gt_ext_process_io_t){GT_EXT_PROCESS_CLOSED', 'node:process-native'),
    ('jobs-root', JOBS, 'int fd = -1;', 'int fd = gt__runtime_resources_root_fd(j->roots, 0);', 'node:process-roundtrip'),
    ('jobs-deadline', JOBS, 'now >= s->deadline', 'false', 'node:process-deadline'),
    ('jobs-first-reason', JOBS, 'if (!s->reason) {\n            s->reason = 2;', 'if (true) {\n            s->reason = 2;', 'node:process-first-reason'),
    ('jobs-output', JOBS, 'if (io.count > j->output_limit - j->output_bytes)', 'if (false && io.count > j->output_limit - j->output_bytes)', 'node:process-output'),
    ('jobs-release', JOBS, 'if (!s->info.exited) {\n        return GT_PROCESS_JOBS_WAIT;', 'if (!s->info.exited) {\n        return GT_OK;', 'node:process-native'),
    ('jobs-reference', JOBS, 's->id = j->next++;', 's->id = 1; j->next++;', 'node:process-native'),
    ('jobs-retire', JOBS, 'if (j->stopped && s->info.exited)', 'if (false && j->stopped && s->info.exited)', 'node:process-native'),
    ('jobs-environment', JOBS, 'j->environment_count = c->environment_count;', 'j->environment_count = 0;', 'node:process-roundtrip'),
    ('jobs-digest', 'core/src/extension/process-service.c', 'strcmp(actual_sha256, mappings[i].verified_sha256)', 'false', 'node:process-bad-digest'),
    ('jobs-pump', SOURCE, 'if (gt__process_jobs_step(r->jobs) != GT_OK) {\n        runtime_fail', 'if (false && gt__process_jobs_step(r->jobs) != GT_OK) {\n        runtime_fail', 'node:process-deadline'),
    ('jobs-owner', 'core/src/extension/process-rpc.c', '(jobs && !gt__process_jobs_matches(jobs, session))', 'false', 'node:process-native'),
    ('jobs-domain', JOBS_RPC, 'process_find(p->children, ref)', 'process_find(p->outgoing, ref)', 'exec:process-roundtrip'),
    ('jobs-fields', JOBS_RPC, '!process_keys(d, spawn, 8) ||', '(false && !process_keys(d, spawn, 8)) ||', 'exec:process-extra-field'),
    ('jobs-hex', JOBS_RPC, "if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))", "if (false && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))", 'exec:process-bad-hex'),
    ('node-process-fields', NODE_PROCESS, 'if (Object.keys(v).sort().join(",") !== [...names].sort().join(","))', 'if (false && Object.keys(v).sort().join(",") !== [...names].sort().join(","))', 'process-unit:fields'),
    ('node-process-reference', NODE_PROCESS, 'compareUint64(ref, this.#reference) <= 0 || this.#children.has(ref)', 'false && (compareUint64(ref, this.#reference) <= 0 || this.#children.has(ref))', 'process-unit:reuse'),
    ('node-process-bytes', NODE_PROCESS, 'v.hex.length !== count * 2', 'false', 'process-unit:bytes'),
    ('node-process-error-bytes', NODE_PROCESS, 'v.hex.length !== count * 2', '(v.state === "data" && v.hex.length !== count * 2)', 'process-unit:errorBytes'),
    ('node-process-exit', NODE_PROCESS, '(!v.exited && (code !== 0 || signal !== 0))', 'false', 'process-unit:unknownExit'),
    ('node-process-cancel', NODE_PROCESS, 'e.cancelled = true;', 'e.cancelled = false;', 'process-unit:cancellation'),
    ('node-process-release', NODE_PROCESS, 'if (r.status === "wait") return;', 'if (r.status === "wait") { e.resolve?.(); return; }', 'process-unit:release'),
]
RESOURCE_CASES = [
    ('resource-owner', 'core/src/extension/process-rpc.c', '(resources && !gt__runtime_resources_matches(resources, session))', 'false', 'core:foreign'),
    ('resource-store', 'core/src/extension/runtime-resource.c', 'r->store == session->store &&', 'true &&', 'core:foreign-store'),
    ('resource-lease', 'core/src/extension/runtime-resource.c', '!strcmp(live.token, r->lease.token)', 'true', 'core:foreign-lease'),
    ('resource-unused-owner', 'core/src/extension/runtime-resource.c', 'r->next == 1 &&', 'true &&', 'core:used-owner'),
    ('resource-fields', RESOURCE, 'if (!process_keys(d, basic, 4)) {', 'if (false && !process_keys(d, basic, 4)) {', 'core:bad-fields'),
    ('resource-base64', RESOURCE, '(padding == 2 && (v[1] & 15))', 'false', 'core:bad-base64'),
    ('resource-reference-domain', RESOURCE, 'release || opening || reading ? p->snapshots : p->publications', 'p->publications', 'core:roundtrip'),
    ('resource-reference-retire', RESOURCE, 'status == GT_RUNTIME_RESOURCE_OK && (forget || release)', 'false', 'core:reuse'),
    ('resource-confirmation', RESOURCE, 's->confirmed ? "true" : "false"', '"false"', 'node:resource-roundtrip'),
    ('resource-unknown', RESOURCE, '"unknown",\n', '"cancelled",\n', 'core:unknown'),
    ('resource-read-chunk', RESOURCE, 'uint8_t bytes[3072];', 'uint8_t bytes[3073];', 'core:roundtrip'),
    ('resource-native-pump', SOURCE, 'gt__runtime_resources_step(r->resources);', '(void)r->resources;', 'node:resource-roundtrip'),
    ('resource-after-close', 'core/src/extension/transfer.c', 'session.phase != GT_BRIDGE_SESSION_CLOSED', 'true', 'core:unknown'),
    ('resource-before-close', 'core/src/extension/transfer.c', 'session.phase != GT_BRIDGE_SESSION_CLOSED', 'false', 'core:unknown'),
    ('closed-observation', 'core/src/extension/session.c', 'if (s->released) {\n        return false;\n    }\n    if (stopped(s))', 'if (stopped(s))', 'core:roundtrip'),
    ('node-resource-fields', NODE_RESOURCE, 'Object.keys(v).sort().join(",") !== [...keys].sort().join(",")', 'Object.keys(v).length < 0 && keys.length < 0', 'unit:fields'),
    ('node-resource-reuse', NODE_RESOURCE, 'compareUint64(ref, this.#reference) <= 0', 'compareUint64(ref, this.#reference) < 0', 'unit:reference'),
    ('node-resource-retire', NODE_RESOURCE, 's === "ok" && retire', 's === "ok" && retire && false', 'unit:retirement'),
    ('node-resource-bytes', NODE_RESOURCE, 'v.hex.length > Math.min(3072,maxBytes)*2', 'false', 'unit:readLimit'),
    ('node-resource-error-bytes', NODE_RESOURCE, '(s !== "ok" && v.hex.length)', 'false', 'unit:errorBytes'),
]
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

PLUGIN = r'''import {runProcessCase} from './process-cases.mjs';
import {object,boolean} from '@ghosterm/plugin-sdk';
export async function activate(ctx) {
  const mode = process.env.GT_RUNTIME_MODE;
  const resourceMode = mode.startsWith('resource-'), processMode=mode.startsWith('process-');
  if (ctx.side !== 'executor' || !/^[0-9a-f]{32}$/.test(ctx.instanceId) ||
      typeof ctx.epoch !== 'string' || ctx.invocation.command !== 'fixture' ||
      ctx.capabilities.query('session.rpc') !== 'allowed' || !Object.isFrozen(ctx) || process.env.HOME !== undefined || process.env.NODE_OPTIONS !== undefined ||
      Object.keys(ctx).sort().join(',') !== (processMode ? 'cancellation,capabilities,epoch,instanceId,invocation,process,resources,session,side' : resourceMode ? 'cancellation,capabilities,epoch,instanceId,invocation,resources,session,side' : 'cancellation,capabilities,epoch,instanceId,invocation,session,side')) throw Error('context');
  if (mode === 'argument-boundary') {
    if (ctx.invocation.arguments.length !== 6 ||
        ['label','second','third'].some(k => ctx.invocation.decodedArguments[k] !== '\x01'.repeat(4096))) throw Error('boundary');
  } else if (ctx.invocation.arguments.length !== 2 || ctx.invocation.arguments[0] !== '--label' ||
      ctx.invocation.decodedArguments.label !== "中文 \"';$()") throw Error('arguments');
  if (mode === 'activation-reject') throw Error('fixture rejected');
  if (mode === 'activation-timeout') { process.stderr.write('activation waiting'); await new Promise(() => {}); }
  if (resourceMode) {
    const limits = {byteLimit:'262144',timeoutMs:10000};
    const publication = ctx.resources.publish({root:'workspace',relativePath:mode === 'resource-unicode' ? '展示"\\file.bin' : 'file.bin',mediaType:'application/octet-stream'},limits);
    const result = await publication.outcome;
    if (result.kind !== 'success' || result.value.size.kind !== 'known' || result.value.size.bytes !== '262144') throw Error('publication');
    let taken;
    for (;;) {
      taken = ctx.resources.take(); if (taken.status === 'ok') break;
      if (taken.status !== 'wait') throw Error('take');
      await new Promise(resolve => setTimeout(resolve,1));
    }
    const opened = ctx.resources.open(taken.ref,limits);
    if (opened.status !== 'ok') throw Error('open');
    let used=0;
    for (;;) {
      const chunk=await opened.stream.read(); if (chunk.kind !== 'success') throw Error('read');
      for (const byte of chunk.value.bytes) { if(byte !== used%251) throw Error('bytes'); used++; }
      if (chunk.value.done) break;
    }
    if (used !== 262144 || ctx.resources.release(taken.ref) !== 'ok') throw Error('release');
  }
  if(processMode) await runProcessCase(ctx,mode);
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
    p.add_argument('--processes', action='store_true', help='native/Node process service faults')
    p.add_argument('--resources', action='store_true', help='include native/Node resource faults and choose resource mutations')
    p.add_argument('--case', action='append')
    args = p.parse_args()
    if args.resources and args.processes: p.error('choose one mutation family')
    available = PROCESS_CASES if args.processes else RESOURCE_CASES if args.resources else CASES
    if args.case and (not args.mutate or set(args.case) - {c[0] for c in available}):
        p.error('--case requires --mutate and known groups')
    node, tools = args.node.resolve(strict=True), args.tools.resolve(strict=True)
    cases = [c for c in available if not args.case or c[0] in args.case]
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
            assert result.returncode == 1 and re.search(r'^(?:FAIL|ASSERT) ', result.stderr, re.M) and not any(
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
            if changed in (RESOURCE, JOBS_RPC): changed = 'core/src/extension/process-rpc.c'
            if changed:
                (build / ('CMakeFiles/ghosterm_core.dir/' + changed.removeprefix('core/') + '.o')).unlink(missing_ok=True)
            binaries = ['test_process_runtime', 'test_process_runtime_peer', 'test_process_resources', 'test_config']
            for name in [*binaries, 'libghosterm_core.a']:
                (build / name).unlink(missing_ok=True)
            output = run(label + '-build', ['cmake', '--build', build, '--target', 'test_process_runtime',
                                           'test_process_runtime_peer', 'test_process_resources', 'test_config', '-j', '4'])
            for binary in binaries:
                assert 'Linking C executable ' + binary in output, label + ': missing actual link ' + binary
            if changed:
                assert changed.removeprefix('core/') + '.o' in output and 'Linking C static library' in output

        rebuild('initial')
        os_name = {'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]
        arch = {'arm64': 'aarch64', 'aarch64': 'aarch64', 'x86_64': 'x86_64'}[platform.machine()]
        sequence = 0

        def target(label, spec, red=False):
            nonlocal sequence
            runtime, mode = spec.split(':')
            if runtime == 'process-unit':
                return run(label,[node,source/'scripts/fixtures/plugin-node-process-cases.mjs',bundle,mode],red)
            if runtime == 'unit':
                return run(label,[node,source/'scripts/fixtures/plugin-node-resource-cases.mjs',bundle,mode],red)
            if runtime == 'core':
                sandbox = Path(tempfile.mkdtemp(prefix='resource-case-',dir=source/'tmp'))
                try: return run(label,[build/'test_process_resources',mode,sandbox],red)
                finally: shutil.rmtree(sandbox)
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
            if mode.startswith('resource-'):
                variant['requires'] += ['resource.receive','file.readScoped']
            if mode.startswith('process-'):
                variant['requires'] += ['process.spawn','process.pipes']
            if runtime == 'node':
                variant.update(entry='executor/main.mjs', runtimeVersionRange='>=20.0.0 <1000.0.0')
                plugin = PLUGIN
                if mode == 'ambient':
                    foreign = fixture / 'ambient.mjs'
                    foreign.write_text("import{writeFileSync}from'node:fs';writeFileSync(" + json.dumps(str(fixture / 'ambient-ran')) + ",'executed');")
                    plugin = 'import ' + json.dumps(foreign.as_uri()) + ';\n' + plugin
                (package / 'executor/main.mjs').write_text(plugin)
                shutil.copyfile(source/'scripts/fixtures/plugin-node-process-run.mjs',package/'executor/process-cases.mjs')
            else:
                variant.update(executable='executor/main.mjs', os=[os_name], arch=[arch])
                shutil.copyfile(build / 'test_process_runtime_peer', package / 'executor/main.mjs')
            manifest = dict(schemaVersion=1, id='org.ghosterm.launch', pluginVersion='1.0.0', sdkApiRange='^1.0.0',
                            bridgeProtocolRange=[1], publisher='fixture', license='MIT',
                            commands=[dict(path=['fixture'], title='Fixture', argsSchema='schemas/input.json')],
                            executor=dict(variants=[variant]), permissions=[])
            if mode.startswith('resource-'):
                manifest['permissions'] = [dict(side='executor',capability='file.readScoped',purpose='Disposable authorized fixture',scope='instance')]
            if mode.startswith('process-'):
                manifest['permissions'] = [dict(side='executor',capability='process.spawn',purpose='Disposable local children',scope='instance',programs=['cat','env','pwd'])]
            (package / 'plugin.json').write_text(json.dumps(manifest))
            members = [dict(path=str(f.relative_to(package)), bytes=f.stat().st_size, sha256=sha(f))
                       for f in sorted(package.rglob('*')) if f.is_file()]
            (package / 'integrity.json').write_text(json.dumps(dict(schemaVersion=1, members=members)))
            facts = dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=[runtime], capabilities=variant['requires'],
                         programs=dict(node=dict(state='available', version='199.0.0' if mode == 'wrong-version' else version[1:])),
                         os=os_name, arch=arch)
            fact_file = fixture / 'facts.json'
            fact_file.write_text(json.dumps(facts))
            try:
                run(label, [build / 'test_process_runtime', mode, package, fixed_sdk, temp, node, node_sha, sdk_digest, fact_file], red)
                if red:
                    # C atexit stops/reaps its actual child. Left-over native
                    # fixture roots/stores are private to this test invocation.
                    for directory, _, _ in os.walk(temp,followlinks=False): os.chmod(directory,0o700)
                    for child in list(temp.iterdir()):
                        if child.is_dir() and not child.is_symlink(): shutil.rmtree(child)
                        else: child.unlink()
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
        baseline += ['node:resource-roundtrip', 'node:resource-unicode', 'node:resource-stop', 'node:resource-missing', 'node:resource-bad-root', 'node:resource-duplicate-root']
        baseline += ['exec:resource-roundtrip']
        baseline += ['node:'+m for m in ['process-roundtrip','process-deadline','process-output','process-cancel','process-capacity','process-stop','process-bad-digest','process-missing','process-native','process-first-reason']]
        baseline += ['exec:process-roundtrip','exec:process-extra-field','exec:process-bad-hex']
        for spec in baseline:
            target('baseline-' + spec.replace(':', '-'), spec)
            print(spec + ': PASS', flush=True)
        if args.processes:
            run('process-node-baseline',[node,source/'scripts/fixtures/plugin-node-process-cases.mjs',bundle])
        if args.resources:
            run('resource-core-baseline',['ctest','--test-dir',build,'-R','^plugin_process_resources_','--output-on-failure'])
            run('resource-node-baseline',[node,source/'scripts/fixtures/plugin-node-resource-cases.mjs',bundle])
        if args.mutate:
            for label, name, old, new, spec in cases:
                path = source / name
                original = path.read_bytes()
                try:
                    path.write_text(original.decode().replace(old, new))
                    if name.startswith('packages/'):
                        sdk_digest = sdk(label)
                    else:
                        rebuild(label, name)
                    target(label + '-red', spec, True)
                    run(label + '-control', [build / 'test_config', source / 'core/tests/fixtures'])
                finally:
                    path.write_bytes(original)
                    assert sha(path) == hashlib.sha256(original).hexdigest()
                if name.startswith('packages/'):
                    sdk_digest = sdk(label + '-restored')
                else:
                    rebuild(label + '-restored', name)
                target(label + '-restored', spec)
                print(label + ': compiled assertion red / same archive control green / restored green / SHA256', flush=True)
        for spec in ['node:roundtrip', 'exec:roundtrip']:
            target('final-' + spec.replace(':', '-'), spec)
        if args.processes:
            target('final-node-process','node:process-roundtrip')
            target('final-exec-process','exec:process-roundtrip')
            target('final-native-process','node:process-native')
            run('process-node-final',[node,source/'scripts/fixtures/plugin-node-process-cases.mjs',bundle])
        if args.resources:
            target('final-node-resource','node:resource-roundtrip')
            target('final-exec-resource','exec:resource-roundtrip')
            run('resource-core-final',['ctest','--test-dir',build,'-R','^plugin_process_resources_','--output-on-failure'])
            run('resource-node-final',[node,source/'scripts/fixtures/plugin-node-resource-cases.mjs',bundle])
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
