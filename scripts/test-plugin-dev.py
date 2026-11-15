#!/usr/bin/env python3
"""Developer CLI conformance and isolated assertion mutations; no app windows."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree(directory):
    return {p.relative_to(directory).as_posix(): p.read_bytes() for p in directory.rglob('*') if p.is_file()}


def run_case(name, work, tools, core):
    def cli(*arguments, status=0):
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1')
        result = subprocess.run(['python3', str(ROOT / 'scripts/gterm-plugin'), *map(str, arguments)], capture_output=True, env=env, timeout=160)
        assert result.returncode == status, (arguments, result.returncode, result.stdout.decode(), result.stderr.decode())
        report = json.loads(result.stdout)
        assert b'Traceback' not in result.stderr
        return report

    project = work / 'project'
    cli('init', project, '--id', 'org.example.test', '--command', 'echo', '--publisher', 'Example')
    common = ['--project', project, '--tools', tools]
    host = dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=['portable-js'], capabilities=['session.rpc'])
    hostfile = work / 'host.json'; hostfile.write_text(json.dumps(host))
    def check(status=0):
        return cli('check', *common, '--host-json', hostfile, status=status)
    def native(contract=None, status=0):
        return cli('test', *common, '--contract', contract or project / 'plugin.test.json', '--core-build', core, status=status)
    def reject(suffix, data, code=None, status=1):
        target = project / suffix; before = target.read_bytes() if target.exists() else None
        target.write_bytes(data)
        try:
            report = check(status=status)
            assert report['kind'] == 'error'
            if code: assert report['code'] == code
        finally:
            if before is None: target.unlink()
            else: target.write_bytes(before)

    if name == 'reproducible':
        # A hostile package lifecycle script is inert input, never a build hook.
        (project / 'package.json').write_text(json.dumps({'scripts': {'postinstall': 'touch HOOK_RAN'}}))
        before = tree(project)
        first, second = work / 'first', work / 'second'
        built = cli('build', *common, '--out', first)
        cli('build', *common, '--out', second)
        assert built['compatibility'] == 'unknown' and not built['installation']
        assert tree(first) == tree(second) and tree(project) == before
        a, b = work / 'a.zip', work / 'b.zip'
        cli('pack', *common, '--out', a); cli('pack', '--package', first, '--tools', tools, '--out', b)
        assert a.read_bytes() == b.read_bytes()
        with zipfile.ZipFile(a) as archive:
            assert archive.namelist() == sorted(tree(first))
            assert all(i.date_time == (1980, 1, 1, 0, 0, 0) and i.external_attr >> 16 == 0o100644 for i in archive.infolist())
            assert archive.read('LICENSES/MIT.txt') == (ROOT / 'LICENSE').read_bytes()
            catalog = json.loads(archive.read('integrity.json'))['members']
            assert {m['path'] for m in catalog} == set(tree(first)) - {'integrity.json'}
            assert all(m['bytes'] == len(archive.read(m['path'])) and m['sha256'] == hashlib.sha256(archive.read(m['path'])).hexdigest() for m in catalog)
    elif name == 'host-states':
        assert cli('check', *common, status=3)['compatibility'] == 'unknown'
        assert check()['compatibility'] == 'compatible'
        for changes, status, state in [({'capabilities': []}, 4, 'unsupported'), ({'deniedCapabilities': ['session.rpc']}, 6, 'denied'), ({'sdkVersion': '9.0.0'}, 4, 'unsupported'), ({'protocols': []}, 4, 'unsupported'), ({'runtimes': []}, 4, 'unsupported'), ({'sdkVersion': 'unknown'}, 3, 'unknown')]:
            hostfile.write_text(json.dumps({**host, **changes}))
            report = check(status)
            assert report['compatibility'] == state and report['authorization'] == 'not-granted' and not report['publisherAuthenticated']
            for action in ['build', 'pack']:
                output = work / (action + '-' + str(status))
                assert cli(action, *common, '--host-json', hostfile, '--out', output, status=status)['compatibility'] == state
                assert not output.exists()
    elif name == 'strict-types':
        target = project / 'executor/index.ts'; base = target.read_bytes()
        for statement in [b'export const bypass: any = 1;\n', b'export function bad(x) { return x; }\n', b'export const bad = document;\n', b'export const bad = process;\n', b'// @ts-nocheck\n']:
            reject('executor/index.ts', base + statement)
    elif name == 'dependencies':
        target = project / 'plugin.json'; manifest = json.loads(target.read_text())
        manifest['executor']['variants'][0]['dependencies'] = [dict(program='git', versionPolicy='range', versionRange='^2.0.0')]
        target.write_text(json.dumps(manifest))
        for fact, status, state in [(None, 3, 'unknown'), ({'state': 'missing'}, 4, 'unsupported'), ({'state': 'denied'}, 6, 'denied'), ({'state': 'available', 'version': '1.0.0'}, 4, 'unsupported'), ({'state': 'available', 'version': '2.4.0'}, 0, 'compatible')]:
            hostfile.write_text(json.dumps({**host, 'programs': {'git': fact}}))
            assert check(status)['compatibility'] == state
    elif name == 'imports':
        target = project / 'executor/index.ts'; base = target.read_bytes()
        for statement in [b'import "node:fs";\n', b'import "./missing.js";\n', b'import "../../escape.js";\n', b'export const bad = import("./missing.js");\n', b'export const bad = require("fs");\n']:
            reject('executor/index.ts', statement + base)
        # Static relative closure, including SDK value imports, actually compiles.
        (project / 'executor/helper.ts').write_text('export const greeting = "hello";\n')
        target.write_bytes(b'import { greeting } from "./helper.js";\nexport const sample = greeting;\n' + base)
        assert check()['compatibility'] == 'compatible'
    elif name == 'physical-inputs':
        target = project / 'outside'; target.symlink_to(ROOT / 'LICENSE')
        assert check(2)['kind'] == 'error'; target.unlink()
        target.symlink_to(ROOT / 'packages', target_is_directory=True)
        assert check(2)['code'] == 'NONPHYSICAL_DIRECTORY'; target.unlink()
        os.mkfifo(target)
        assert check(2)['code'] == 'NONREGULAR_MEMBER'; target.unlink()
        hostfile.unlink(); os.mkfifo(hostfile)
        assert check(2)['code'] == 'JSON_FILE'
    elif name == 'json-inputs':
        for data, code in [(b'{"a":1,"a":2}', 'DUPLICATE_OR_RESERVED_KEY'), (b'{"a":"\\ud800"}', None), (b'{"a":1e999}', 'JSON_NUMBER'), (b'[' * 2000 + b']' * 2000, 'JSON_DEPTH'), (b'{"constructor":1}', 'DUPLICATE_OR_RESERVED_KEY')]:
            reject('extra.json', data, code, status=2)
    elif name == 'integrity':
        package = work / 'package'; cli('build', *common, '--out', package)
        member = package / 'README.md'; member.write_bytes(member.read_bytes() + b'tampered')
        assert cli('check', '--package', package, '--tools', tools, '--host-json', hostfile, status=1)['kind'] == 'error'
        (package / 'unindexed').write_text('not indexed')
        assert cli('pack', '--package', package, '--tools', tools, '--out', work / 'bad.zip', status=1)['kind'] == 'error'
        assert not (work / 'bad.zip').exists()
    elif name == 'conflicts':
        package = work / 'package'; cli('build', *common, '--out', package)
        assert cli('check', *common, '--registry-package', package, '--host-json', hostfile, status=1)['kind'] == 'error'
    elif name == 'publication':
        empty = work / 'empty'; empty.mkdir()
        assert cli('build', *common, '--out', empty, status=2)['code'] == 'OUTPUT_EXISTS'
        assert not list(empty.iterdir())
        file = work / 'file'; file.write_text('retain')
        assert cli('pack', *common, '--out', file, status=2)['code'] == 'OUTPUT_EXISTS' and file.read_text() == 'retain'
        assert cli('build', *common, '--out', project / 'out', status=2)['code'] == 'OUTPUT_SOURCE_OVERLAP'
        assert cli('init', project, '--id', 'org.example.test', '--command', 'echo', '--publisher', 'Example', status=2)['code'] == 'OUTPUT_EXISTS'
    elif name == 'rpc':
        report = native()
        assert report['kind'] == 'tested' and report['observations'][2]['outcome']['kind'] == 'unknown'
        contract = json.loads((project / 'plugin.test.json').read_text())
        contract['input'] = contract['expected'] = {'text': 'quoted "${epoch}"; ${instanceId}'}
        alternate = work / 'contract.json'; alternate.write_text(json.dumps(contract))
        assert native(alternate)['observations'][1]['outcome']['value'] == contract['expected']
    elif name == 'activation-failure':
        (project / 'executor/index.ts').write_text('export function activate() { throw new Error("fixture activation failure"); }\n')
        assert native(status=1)['kind'] == 'error'
    elif name == 'dispose-failure':
        target = project / 'executor/index.ts'
        target.write_text(target.read_text().replace('live = false;', 'live = false; throw new Error("fixture dispose failure");'))
        assert native(status=1)['kind'] == 'error'
    elif name == 'rpc-required':
        target = project / 'plugin.json'; manifest = json.loads(target.read_text())
        manifest['executor']['variants'][0]['requires'] = []
        manifest['permissions'] = [p for p in manifest['permissions'] if p['side'] != 'executor']
        target.write_text(json.dumps(manifest))
        assert native(status=1)['kind'] == 'error'
    elif name == 'services-unsupported':
        target = project / 'plugin.json'; manifest = json.loads(target.read_text())
        manifest['executor']['variants'][0]['requires'].append('ui.task')
        manifest['permissions'].append(dict(side='executor', capability='ui.task', purpose='Test explicit service rejection.', scope='instance'))
        target.write_text(json.dumps(manifest))
        report = native(status=4)
        assert report['compatibility'] == 'unsupported' and report['reason'] == 'CAPABILITY_UNSUPPORTED'
    elif name == 'interrupt-cleanup':
        isolated = work / 'interrupted'; (isolated / 'tmp').mkdir(parents=True)
        for filename in ['scripts/gterm-plugin', 'scripts/lib/gt-timeout.sh', 'scripts/fixtures/plugin-dev-tools/package.json', 'scripts/fixtures/plugin-dev-tools/package-lock.json']:
            target = isolated / filename; target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(ROOT / filename, target)
        # A held compiler worker is an owned fixture, never plugin code.
        (isolated / 'scripts/plugin-dev-worker.mjs').write_text('import fs from "node:fs"; import path from "node:path"; fs.writeFileSync(path.join(path.dirname(process.argv[2]), "ready"), String(process.pid)); setInterval(() => {}, 1000);\n')
        process = subprocess.Popen(['python3', str(isolated / 'scripts/gterm-plugin'), 'build', *map(str, common), '--out', str(work / 'unused')], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            until = time.monotonic() + 5
            markers = []
            while process.poll() is None and time.monotonic() < until:
                markers = list((isolated / 'tmp').glob('plugin-dev-*/ready'))
                if markers: break
                time.sleep(0.02)
            assert markers and process.poll() is None, 'worker must be live before interruption'
            child = int(markers[0].read_text())
            process.terminate(); stdout, stderr = process.communicate(timeout=5)
            assert process.returncode == 143 and not stdout and b'Traceback' not in stderr
            assert not list((isolated / 'tmp').iterdir()), 'interrupted snapshot must be removed'
            until = time.monotonic() + 2
            while time.monotonic() < until:
                try: os.kill(child, 0)
                except ProcessLookupError: break
                time.sleep(0.02)
            else: raise AssertionError('interrupted worker survived')
        finally:
            if process.poll() is None:
                process.terminate(); process.communicate(timeout=5)
    else:
        raise AssertionError(name)


CASES = ['reproducible', 'host-states', 'strict-types', 'dependencies', 'imports', 'physical-inputs', 'json-inputs', 'integrity', 'conflicts', 'publication', 'rpc', 'activation-failure', 'dispose-failure', 'rpc-required', 'services-unsupported', 'interrupt-cleanup']
MUTATIONS = [
    ('explicit-any', 'strict-types', 'scripts/plugin-dev-worker.mjs', "demand(node.kind !== ts.SyntaxKind.AnyKeyword, 'EXPLICIT_ANY');", ''),
    ('host-denied', 'host-states', 'scripts/plugin-dev-worker.mjs', 'compatibility = error.state; reason = error.code;', "compatibility = error.state === 'denied' ? 'unknown' : error.state; reason = error.code;"),
    ('integrity', 'integrity', 'scripts/plugin-dev-worker.mjs', 'facts = inspect(manifest, integrity, observed, host, schemas);', 'facts = {};'),
    ('queued-cancel', 'rpc', 'scripts/probes/plugin-testkit.c', 'gt_ext_session_cancel_request(pair.h, &lease, handle) == GT_EXT_RPC_OK', 'true'),
    ('old-epoch', 'rpc', 'scripts/probes/plugin-testkit.c', 'stale.epoch ^= 1;', 'stale.epoch = lease.epoch;'),
    ('revocation', 'rpc', 'scripts/probes/plugin-testkit.c', 'gt_ext_session_revoke(pair.h);', '(void)pair.h;'),
]


def mutations(work, tools, core):
    isolated = work / 'isolated'; isolated.mkdir(); (isolated / 'tmp').mkdir()
    for directory in ['packages/plugin-sdk', 'core/include']:
        shutil.copytree(ROOT / directory, isolated / directory)
    files = ['LICENSE', 'core/tests/test_ext_session.c', 'scripts/test-plugin-dev.py', 'scripts/gterm-plugin', 'scripts/plugin-dev-worker.mjs', 'scripts/probes/plugin-testkit.c', 'scripts/lib/gt-timeout.sh', 'scripts/fixtures/plugin-dev-tools/package.json', 'scripts/fixtures/plugin-dev-tools/package-lock.json']
    for name in files:
        target = isolated / name; target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(ROOT / name, target)
    build = isolated / 'tmp/core'; build.mkdir()
    for name in ['libghosterm_core.a', 'libghosterm_quickjs.a', 'CMakeCache.txt']:
        os.link(core / name, build / name)
    evidence = []
    for label, _case, filename, original, _changed in MUTATIONS:
        assert (isolated / filename).read_text().count(original) == 1, (label, 'ambiguous mutation anchor')
    for label, case, filename, original, changed in MUTATIONS:
        target = isolated / filename; before = target.read_bytes(); text = before.decode()
        assert text.count(original) == 1, (label, 'ambiguous mutation anchor')
        def probe():
            return subprocess.run(['python3', str(isolated / 'scripts/test-plugin-dev.py'), '--tools', str(tools), '--core-build', str(build), '--case', case], capture_output=True, timeout=200)
        baseline = probe(); assert baseline.returncode == 0, (label, baseline.stderr.decode())
        target.write_text(text.replace(original, changed))
        try:
            red = probe()
            diagnostics = red.stdout + red.stderr
            assert red.returncode == 1 and b'AssertionError' in diagnostics and b'error: ' not in diagnostics, (label, diagnostics.decode())
            if filename.endswith('.c'):
                assert b'ASSERT:' in diagnostics, (label, 'requires a compiled native assertion red', diagnostics.decode())
            else:
                assert b'TOOL_FAILED' not in diagnostics, (label, diagnostics.decode())
            control = subprocess.run([str(core / 'test_config'), str(ROOT / 'core/tests/fixtures'), str(work)], capture_output=True, timeout=10)
            assert control.returncode == 0, control.stderr.decode()
        finally:
            target.write_bytes(before)
        restored = probe(); assert restored.returncode == 0 and target.read_bytes() == before, (label, restored.stderr.decode())
        evidence.append(dict(case=label, targetAssertionRed=True, configurationGreen=True, restoredGreen=True, sourceSHA256=hashlib.sha256(before).hexdigest(), redSHA256=hashlib.sha256(diagnostics).hexdigest()))
        print('MUTATION PASS ' + label, flush=True)
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True); parser.add_argument('--core-build', type=Path, required=True)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutate', action='store_true'); parser.add_argument('--receipt', type=Path)
    args = parser.parse_args(); core, tools = args.core_build.resolve(), args.tools.resolve()
    sources = {ROOT / name for name in ['scripts/gterm-plugin', 'scripts/plugin-dev-worker.mjs', 'scripts/probes/plugin-testkit.c', 'scripts/lib/gt-timeout.sh', 'core/tests/test_ext_session.c', 'LICENSE']}
    sources.add(Path(__file__).resolve())
    for directory in ['scripts/fixtures/plugin-dev-tools', 'packages/plugin-sdk', 'core/include']:
        sources.update(p for p in (ROOT / directory).rglob('*') if p.is_file())
    inputs = {str(p.relative_to(ROOT)): digest(p) for p in sorted(sources)}
    with tempfile.TemporaryDirectory(prefix='plugin-dev-test-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        for case in [args.case] if args.case else CASES:
            directory = work / case; directory.mkdir(); run_case(case, directory, tools, core)
            print('PASS ' + case, flush=True)
        proof = mutations(work, tools, core) if args.mutate else []
    assert all(digest(ROOT / name) == value for name, value in inputs.items()), 'inputs changed during verification'
    if args.receipt:
        args.receipt.write_text(json.dumps(dict(inputs=inputs, cases=[args.case] if args.case else CASES, mutations=proof, coreSHA256=digest(core / 'libghosterm_core.a'), quickjsSHA256=digest(core / 'libghosterm_quickjs.a')), indent=2) + '\n')


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise SystemExit(128 + number)
    signal.signal(signal.SIGTERM, interrupted)
    main()
