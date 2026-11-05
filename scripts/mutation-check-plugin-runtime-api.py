#!/usr/bin/env python3
"""Public runtime: actual C/Swift links, ordinary assertions and same-library control.

Only isolated copies are mutated. SDK developer tools use the pinned offline cache.
Positive JS profile is explicit; this does not enable the App runtime entry.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_PLUGIN_RUNTIME_API_MUTATION'
API = 'core/src/extension/runtime-api.c'
SWIFT = 'apple/GhostermCore/Sources/GhostermCore/ExtensionRuntime.swift'
RPC = 'core/src/extension/rpc.c'
TRANSFER = 'core/src/extension/transfer.c'
MODES = 'invalid roundtrip reverse prototype isolation relative top-await handled stop cpu top-cpu atomics jobs memory stack reject unhandled entry missing-export native-import node-import escape dynamic-import dispose-loop activation-timeout peer-loss'.split()
OWNERS = 'outgoing-owner taken-owner untaken-peer resource-owner'.split()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core-only', action='store_true')
    parser.add_argument('--case', action='append', default=[])
    args = parser.parse_args()
    # Before mkdir/copy/build or any subprocess, check every mutation input.
    for name in [API, SWIFT, RPC, TRANSFER]:
        if MARK in (ROOT / name).read_text():
            print('residual public runtime mutation: ' + name, file=sys.stderr)
            raise SystemExit(2)
    cases = [
        ('enabled-capability', API, 'return GT_OK;\n#else', 'return GT_ERR_UNSUPPORTED;\n#else', 'test_ext_runtime', 'invalid'),
        ('constructor-output-clear', API, 'if(out)*out=NULL;', 'if(out)*out=*out;', 'test_ext_runtime', 'invalid'),
        ('info-output-clear', API, 'if(out)memset(out,0,sizeof(*out));', '(void)out;', 'test_ext_runtime', 'invalid'),
        ('module-copy-digest', API, 'memcpy(modules[i].sha256,m->sha256,65);', 'memset(modules[i].sha256,0,65);', 'test_ext_runtime', 'roundtrip'),
        ('module-copy-size', API, '.size=m->size', '.size=0', 'test_ext_runtime', 'roundtrip'),
        ('phase-active', API, 'case GT_RUNTIME_ACTIVE:return GT_EXT_RUNTIME_ACTIVE;', 'case GT_RUNTIME_ACTIVE:return GT_EXT_RUNTIME_STOPPED;', 'test_ext_runtime', 'roundtrip'),
        ('phase-failed', API, 'case GT_RUNTIME_FAILED:return GT_EXT_RUNTIME_FAILED;', 'case GT_RUNTIME_FAILED:return GT_EXT_RUNTIME_STOPPED;', 'test_ext_runtime', 'cpu'),
        ('error-cpu', API, 'case GT_RUNTIME_CPU:return GT_EXT_RUNTIME_CPU;', 'case GT_RUNTIME_CPU:return GT_EXT_RUNTIME_SCRIPT;', 'test_ext_runtime', 'cpu'),
        ('error-memory', API, 'case GT_RUNTIME_MEMORY:return GT_EXT_RUNTIME_MEMORY;', 'case GT_RUNTIME_MEMORY:return GT_EXT_RUNTIME_SCRIPT;', 'test_ext_runtime_boundary', None),
        ('module-byte-count', API, 'out->module_bytes=i.module_bytes;', 'out->module_bytes=0;', 'test_ext_runtime', 'roundtrip'),
        ('step-count', API, 'out->steps=i.steps;', 'out->steps=0;', 'test_ext_runtime', 'roundtrip'),
        ('job-count', API, 'out->jobs=i.jobs;', 'out->jobs=0;', 'test_ext_runtime', 'jobs'),
        ('dispose-error', API, 'out->dispose_error=i.dispose_error;', 'out->dispose_error=false;', 'test_ext_runtime', 'dispose-loop'),
        ('step-advances', API, 'ext_runtime_phase(gt__bridge_runtime_step(r->inner))', 'GT_EXT_RUNTIME_CREATED', 'test_ext_runtime', 'roundtrip'),
        ('stop-effect', API, 'if(r)gt__bridge_runtime_stop(r->inner);', 'if(r)(void)r->inner;', 'test_ext_runtime', 'stop'),
        ('destroy-failure-retains', API, 'if(status!=GT_OK)return status;', 'if(false && status!=GT_OK)return status;', 'test_ext_runtime', 'roundtrip'),
        ('outgoing-owner', RPC, 'if(r->out[i].used || (', 'if((false && r->out[i].used) || (', 'test_ext_runtime_ownership', 'outgoing-owner'),
        ('taken-owner', RPC, 'r->in[i].used && r->in[i].taken && !r->in[i].done', 'false && r->in[i].used && r->in[i].taken && !r->in[i].done', 'test_ext_runtime_ownership', 'taken-owner'),
        ('resource-owner', TRANSFER, 'return t && t->out.used;', 'return t && false && t->out.used;', 'test_ext_runtime_ownership', 'resource-owner'),
    ]
    swift_cases = [
        ('swift-phase', 'Info(phase: Phase(rawValue: v.phase.rawValue) ?? .failed', 'Info(phase: v.phase == GT_EXT_RUNTIME_ACTIVE ? .stopped : (Phase(rawValue: v.phase.rawValue) ?? .failed)', 'testAdoptionAndActualPluginRPC'),
        ('swift-cpu-error', 'error: Failure(rawValue: v.error.rawValue) ?? .io', 'error: v.error == GT_EXT_RUNTIME_CPU ? .script : (Failure(rawValue: v.error.rawValue) ?? .io)', 'testCPUAndImportFailureRemainDistinct'),
        ('swift-jobs', 'steps: v.steps, jobs: v.jobs', 'steps: v.steps, jobs: 0', 'testAdoptionAndActualPluginRPC'),
        ('swift-module-size', 'value.size = m.source.count', 'value.size = 0', 'testAdoptionAndActualPluginRPC'),
        ('swift-explicit-destroy', 'let status = gt_ext_runtime_destroy(&raw);', 'let status = GT_OK;', 'testExplicitCloseInertsHandle'),
        ('swift-owner-reaper', 'owner.async { value.run() }', 'owner.async { }', 'testDroppedRuntimeCleansOnOwnerQueue'),
    ]
    known = {c[0] for c in cases + swift_cases}
    if set(args.case) - known:
        parser.error('unknown case: ' + ', '.join(sorted(set(args.case) - known)))
    if args.case:
        cases = [c for c in cases if c[0] in args.case]
        swift_cases = [c for c in swift_cases if c[0] in args.case]
    if args.core_only:
        swift_cases = []
    for label, name, old, *_ in cases:
        if (ROOT / name).read_text().count(old) != 1:
            raise RuntimeError('missing/ambiguous anchor: ' + label)
    for label, old, *_ in swift_cases:
        if (ROOT / SWIFT).read_text().count(old) != 1:
            raise RuntimeError('missing/ambiguous Swift anchor: ' + label)
    inputs = []
    for folder in ['core', 'packages/plugin-sdk', 'apple/GhostermCore']:
        for p in (ROOT / folder).rglob('*'):
            rel = p.relative_to(ROOT / folder)
            if p.is_file() and not any(x.startswith('build') or x in ['.build', '.swiftpm', 'node_modules'] for x in rel.parts):
                inputs.append(p)
    inputs += [Path(__file__), ROOT / 'scripts/fixtures/plugin-sdk-tools/package.json', ROOT / 'scripts/fixtures/plugin-sdk-tools/package-lock.json']
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    work = Path(tempfile.mkdtemp(prefix='plugin-runtime-api-mutations-', dir=ROOT / 'tmp'))
    source, logs = work / 'source', work / 'logs'
    logs.mkdir()
    commands = []
    env = dict(os.environ, TMPDIR=str(work), ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')

    def run(label, cmd, budget=180, red=None):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-runtime-api; shift; gt_timeout "$@"'
        p = subprocess.run(['bash', '-c', shell, 'plugin-runtime-api', str(ROOT), str(budget), *map(str, cmd)], cwd=ROOT, env=env, capture_output=True, text=True, timeout=budget + 10)
        out = p.stdout + p.stderr
        (logs / (label + '.log')).write_text(out)
        commands.append(dict(label=label, returncode=p.returncode, assertion=red))
        poisoned = any(x in out for x in ['AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'])
        valid = p.returncode == 0
        if red == 'c':
            valid = p.returncode == 1 and p.stderr.startswith('FAIL ') and not poisoned
        elif red == 'swift':
            valid = p.returncode == 1 and re.search(r'Executed 1 test, with 1 failure \(0 unexpected\)', out) and not poisoned and 'Build complete!' in out
        if not valid:
            raise RuntimeError(label + ': unexpected result\n' + out)
        return out

    try:
        shutil.copytree(ROOT / 'core', source / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
        shutil.copytree(ROOT / 'apple/GhostermCore', source / 'apple/GhostermCore', symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        shutil.copytree(ROOT / 'packages/plugin-sdk', source / 'packages/plugin-sdk')
        (source / 'tmp').mkdir()
        (source / 'vendor').mkdir()
        (source / 'vendor/quickjs-ng').symlink_to(ROOT / 'vendor/quickjs-ng', target_is_directory=True)
        tools = work / 'tools'
        tools.mkdir()
        for name in ['package.json', 'package-lock.json']:
            shutil.copyfile(ROOT / 'scripts/fixtures/plugin-sdk-tools' / name, tools / name)
        for name, entry in json.loads((tools / 'package-lock.json').read_text())['packages'].items():
            if name and (not entry['resolved'].startswith('https://registry.npmjs.org/') or not entry['integrity'].startswith('sha512-')):
                raise RuntimeError('unpinned SDK tool: ' + name)
        npmrc = work / 'empty.npmrc'
        npmrc.touch()
        env['NPM_CONFIG_USERCONFIG'] = str(npmrc)
        run('tools', ['npm', 'ci', '--prefix', tools, '--ignore-scripts', '--no-audit', '--no-fund', '--offline', '--cache', ROOT / 'tmp/plugin-sdk-npm-cache'])
        sdk = work / 'sdk'
        run('typescript', ['node', tools / 'node_modules/typescript/bin/tsc', '-p', source / 'packages/plugin-sdk/tsconfig.json', '--outDir', sdk])
        core, build = source / 'core', source / 'core/build'
        base = ['cmake', '-S', core, '-B', build, '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DGHOSTERM_WITH_PLUGIN_JS=ON', '-DGHOSTERM_BUILD_TESTS=ON', '-DGHOSTERM_PLUGIN_SDK_DIR=' + str(sdk), '-DCMAKE_BUILD_TYPE=RelWithDebInfo']
        run('configure', [*base, '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        binaries = ['test_ext_runtime', 'test_ext_runtime_ownership', 'test_ext_runtime_boundary', 'test_runtime', 'test_runtime_boundary', 'test_config']
        objects = [('ghosterm_core', 'src/extension/' + n + '.c') for n in ['runtime-api', 'runtime', 'rpc', 'transfer']]
        objects += [(n, 'tests/' + n + '.c') for n in binaries[:-1]]

        def rebuild(label):
            for owner, name in objects:
                (build / 'CMakeFiles' / (owner + '.dir') / (name + '.o')).unlink(missing_ok=True)
            for name in ['libghosterm_core.a', *binaries]:
                (build / name).unlink(missing_ok=True)
            out = run(label + '-build', ['cmake', '--build', build, '--target', *binaries, '-j', '2'], budget=300)
            for owner, name in objects:
                assert 'Building C object CMakeFiles/' + owner + '.dir/' + name + '.o' in out, (label, name)
            for name in binaries:
                assert 'Linking C executable ' + name in out, (label, name)
            assert 'Linking C static library libghosterm_core.a' in out

        def suite(label):
            expected = {'ext_runtime_' + m for m in MODES + OWNERS} | {'ext_runtime_boundary_outbound'}
            private = {'runtime_' + m for m in MODES} | {'runtime_boundary_outbound'}
            discovered = json.loads(run(label + '-discover', ['ctest', '--test-dir', build, '--show-only=json-v1']))
            assert {t['name'] for t in discovered['tests'] if t['name'].startswith(('ext_runtime_', 'runtime_'))} == expected | private
            out = run(label, ['ctest', '--test-dir', build, '-R', '^(ext_runtime_|runtime_)', '--output-on-failure'], budget=90)
            for name in expected | private:
                assert re.search(r'Test\s+#\d+: ' + re.escape(name) + r'\s+\.+\s+Passed', out), name
            assert '100% tests passed out of 58' in out

        def target(label, binary, mode, red=False):
            cmd = [build / binary]
            if mode is not None:
                cmd.append(mode)
            run(label, [*cmd, sdk], red='c' if red else None, budget=30)

        rebuild('baseline')
        suite('baseline')
        refs = run('public-runtime-object', ['nm', '-u', build / 'CMakeFiles/test_ext_runtime.dir/tests/test_ext_runtime.c.o'])
        assert '_gt_ext_runtime_open' in refs and 'gt__bridge_runtime_' not in refs
        run('baseline-config', [build / 'test_config', core / 'tests/fixtures'])
        for label, name, old, new, binary, mode in cases:
            path = source / name
            original = path.read_bytes()
            try:
                path.write_text(original.decode().replace(old, new) + '\n/* ' + MARK + ' */\n')
                rebuild(label)
                target(label + '-target', binary, mode, True)
                run(label + '-control', [build / 'test_config', core / 'tests/fixtures'])
            finally:
                path.write_bytes(original)
                assert sha(path) == hashlib.sha256(original).hexdigest()
            rebuild(label + '-restored')
            target(label + '-restored', binary, mode)
            print(label + ': actual compile/archive/executable assertion red / same archive config green / restored / SHA256', flush=True)
        suite('final')
        if not args.core_only:
            run('swift-configure', [*base, '-DCMAKE_C_FLAGS='])
            rebuild('swift-core')
            env['GHOSTERM_CORE_TEST_PLUGIN_JS'] = '1'
            env['GHOSTERM_RUNTIME_TEST_SDK'] = str(sdk)
            package = source / 'apple/GhostermCore'

            def swift_run(label, method=None, red=False):
                output = run(label, ['swift', 'test', '--package-path', package, '--filter', 'ExtensionRuntimeTests.' + method if method else 'ExtensionRuntime'], budget=240, red='swift' if red else None)
                if not red:
                    count = 1 if method else 8
                    assert re.search(r'Executed ' + str(count) + r' tests?, with 0 failures \(0 unexpected\)', output)
                    assert 'skipped' not in output.lower()

            swift_run('swift-baseline')
            for label, old, new, method in swift_cases:
                path = source / SWIFT
                original = path.read_bytes()
                try:
                    path.write_text(original.decode().replace(old, new) + '\n// ' + MARK + '\n')
                    swift_run(label + '-target', method, True)
                    run(label + '-control', [build / 'test_config', core / 'tests/fixtures'])
                finally:
                    path.write_bytes(original)
                    assert sha(path) == hashlib.sha256(original).hexdigest()
                swift_run(label + '-restored', method)
                print(label + ': rebuilt ordinary XCTest assertion red / same archive config green / restored / SHA256', flush=True)
            swift_run('swift-final')
        for name, digest in hashes.items():
            assert sha(ROOT / name) == digest, name
        receipt = dict(accepted=True, inputs_sha256=hashes, core_groups=[c[0] for c in cases], swift_groups=[c[0] for c in swift_cases], public_cases=31, private_cases=27, swift_tests=0 if args.core_only else 8, commands=commands, c_sanitizers=['address', 'undefined'], leak_sanitizer=False, core_sha256=sha(build / 'libghosterm_core.a'), engine_sha256=sha(build / 'libghosterm_quickjs.a'), sdk_sha256={p.name: sha(p) for p in sdk.glob('*.js')}, scope='public C facade / owner admission / native Swift worker; no App activation, gateway, physical device, WAN or release')
        (logs / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
        shutil.rmtree(source)
        print('PUBLIC RUNTIME MUTATIONS PASS; logs: ' + str(logs), flush=True)
    except BaseException as error:
        (logs / 'failure.txt').write_text(repr(error) + '\n')
        raise


def stop(number, _frame):
    raise KeyboardInterrupt('signal ' + str(number))


if __name__ == '__main__':
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    main()
