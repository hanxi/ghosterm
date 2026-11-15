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
    def cli(*arguments, status=0, diagnostic=None):
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1')
        result = subprocess.run(['python3', str(ROOT / 'scripts/gterm-plugin'), *map(str, arguments)], capture_output=True, env=env, timeout=160)
        assert result.returncode == status, (arguments, result.returncode, result.stdout.decode(), result.stderr.decode())
        report = json.loads(result.stdout)
        assert b'Traceback' not in result.stderr
        if diagnostic is not None: assert diagnostic.encode() in result.stderr, result.stderr.decode()
        return report

    project = work / 'project'
    cli('init', project, '--id', 'org.example.test', '--command', 'echo', '--publisher', 'Example')
    common = ['--project', project, '--tools', tools]
    host = dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=['portable-js'], capabilities=['session.rpc'])
    hostfile = work / 'host.json'; hostfile.write_text(json.dumps(host))
    def check(status=0):
        return cli('check', *common, '--host-json', hostfile, status=status)
    def native(contract=None, status=0, side='executor', diagnostic=None):
        return cli('test', *common, '--contract', contract or project / 'plugin.test.json', '--core-build', core, '--side', side, status=status, diagnostic=diagnostic)
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

    def execution_project(runtime):
        manifest = json.loads((project / 'plugin.json').read_text())
        config = json.loads((project / 'plugin.project.json').read_text())
        if runtime == 'node':
            manifest['executor']['variants'][0].update(id='node', runtime='node', runtimeVersionRange='^22.0.0')
            (project / 'package.json').write_text('{"type":"module","scripts":{"postinstall":"touch HOOK_RAN"}}')
            config['assets'].append('package.json')
            (project / 'executor/index.ts').write_text('import { writeFileSync } from "node:fs";\nexport const bytes = Buffer.from("typed");\nwriteFileSync(' + json.dumps(str(work / 'ENTRY_RAN')) + ', process.version);\nexport function activate() { return { dispose() {} }; }\n')
        else:
            manifest['executor']['variants'][0] = dict(id='native', runtime='exec', executable='bin/fixture', requires=['session.rpc'], os=['macos'], arch=['aarch64'])
            config['sources'].remove('executor/index.ts'); config['assets'].append('bin/fixture')
            (project / 'bin').mkdir(); shutil.copyfile('/usr/bin/true', project / 'bin/fixture')
        (project / 'plugin.json').write_text(json.dumps(manifest))
        (project / 'plugin.project.json').write_text(json.dumps(config))
        return manifest, config

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
    elif name == 'client-rpc':
        # Distinct Client bytes and response prove we did not activate Executor.
        target = project / 'client/index.ts'
        target.write_text(target.read_text().replace('return { text: input.text };', 'return { text: "client:" + input.text };'))
        contract = json.loads((project / 'plugin.test.json').read_text())
        contract['expected'] = {'text': 'client:hello'}
        alternate = work / 'client.json'; alternate.write_text(json.dumps(contract))
        report = native(alternate, side='client')
        assert report['side'] == 'client' and report['observations'][0]['side'] == 'client'
        assert report['observations'][1]['outcome']['value'] == contract['expected']
        assert report['observations'][2]['outcome']['kind'] == 'unknown'
        assert all(report['observations'][-1][key] for key in ['activation', 'budgetAdmission', 'stale', 'revocation', 'engineReleased', 'fdBalance'])
        assert native()['side'] == 'executor'
        # Check already built packages as well as TypeScript project input.
        package = work / 'package'; cli('build', *common, '--out', package)
        assert cli('test', '--package', package, '--tools', tools, '--contract', alternate, '--core-build', core, '--side', 'client')['side'] == 'client'
    elif name == 'client-contract':
        assert native(side='client')['side'] == 'client'
        contract = json.loads((project / 'plugin.test.json').read_text())
        contract['expected'] = {'text': 'a response the Client never sent'}
        alternate = work / 'wrong.json'; alternate.write_text(json.dumps(contract))
        assert native(alternate, side='client', status=2)['code'] == 'TEST_RESPONSE'
        contract['expected'] = {'text': 'hello'}; contract['errorKind'] = 'failed'
        alternate.write_text(json.dumps(contract))
        assert native(alternate, side='client', status=2)['code'] == 'TEST_ERROR_OUTCOME'
    elif name == 'client-failures':
        target = project / 'client/index.ts'; original = target.read_text()
        for text in ['export function activate() { throw new Error("Client activation fixture"); }\n', original.replace('live = false;', 'live = false; throw new Error("Client dispose fixture");')]:
            target.write_text(text)
            assert native(side='client', status=1)['kind'] == 'error'
        target.write_text(original)
        assert native(side='client')['kind'] == 'tested'
    elif name == 'client-rpc-required':
        target = project / 'plugin.json'; manifest = json.loads(target.read_text())
        manifest['client']['requires'] = []
        manifest['permissions'] = [p for p in manifest['permissions'] if p['side'] != 'client']
        target.write_text(json.dumps(manifest))
        assert native(side='client', status=1, diagnostic='TEST_REQUIRES_DECLARED_RPC')['kind'] == 'error'
    elif name == 'side-profile':
        arguments = ['test', *common, '--contract', project / 'plugin.test.json', '--core-build', core, '--host-json', hostfile]
        assert cli(*arguments, '--side', 'client', status=2)['code'] == 'TEST_HOST_SIDE'
        hostfile.write_text(json.dumps({**host, 'side': 'client'}))
        assert cli(*arguments, status=2)['code'] == 'TEST_HOST_SIDE'
        assert cli(*arguments, '--side', 'client')['side'] == 'client'
        for change, status, state in [({'capabilities': []}, 4, 'unsupported'), ({'deniedCapabilities': ['session.rpc']}, 6, 'denied'), ({'sdkVersion': 'unknown'}, 3, 'unknown')]:
            hostfile.write_text(json.dumps({**host, 'side': 'client', **change}))
            assert cli(*arguments, '--side', 'client', status=status)['compatibility'] == state
    elif name == 'paired-rpc':
        contract = project / 'plugin.pair.test.json'
        report = native(contract, side='paired')
        assert report['side'] == 'paired' and report['observations'][1]['outcome']['value'] == {'text': 'client:executor:callback:hello'}
        assert all(report['observations'][-1][k] for k in ['clientActivated', 'executorActivated', 'clientEngineReleased', 'executorEngineReleased', 'fdBalance'])
        package = work / 'package'; cli('build', *common, '--out', package)
        assert cli('test', '--package', package, '--tools', tools, '--contract', contract, '--core-build', core, '--side', 'paired')['side'] == 'paired'
        reverse = json.loads(contract.read_text()); reverse.update(method='dev.exchange', expected={'text': 'executor:callback:hello'})
        other = work / 'reverse.json'; other.write_text(json.dumps(reverse))
        assert native(other, side='paired')['kind'] == 'tested'
    elif name == 'paired-contract':
        contract = json.loads((project / 'plugin.pair.test.json').read_text()); other = work / 'invalid.json'
        for update, code in [({'methods': []}, 'TEST_PAIR_METHODS'), ({'methods': contract['methods'] * 2}, 'TEST_PAIR_DUPLICATE'), ({'method': 'dev.missing'}, 'TEST_PAIR_DIRECTION'), ({'expected': {'text': 'executor was bypassed'}}, 'TEST_RESPONSE')]:
            other.write_text(json.dumps({**contract, **update}))
            assert native(other, side='paired', status=2)['code'] == code
    elif name == 'paired-failures':
        for side in ['client', 'executor']:
            target = project / side / 'index.ts'; original = target.read_text()
            for text in ['export function activate() { throw new Error("Paired activation fixture"); }\n', original.replace('live = false;', 'live = false; throw new Error("Paired dispose fixture");')]:
                target.write_text(text)
                assert native(project / 'plugin.pair.test.json', side='paired', status=1, diagnostic='ASSERT:')['kind'] == 'error'
            target.write_text(original)
        assert native(project / 'plugin.pair.test.json', side='paired')['side'] == 'paired'
    elif name == 'paired-hosts':
        facts = {s: {**host, 'side': s} for s in ['client', 'executor']}
        # Incompatibility must be diagnosed before compiling/running a probe.
        arguments = ['test', *common, '--contract', project / 'plugin.pair.test.json', '--core-build', work / 'must-not-run', '--side', 'paired', '--host-json', hostfile]
        for invalid in [host, {'client': facts['client']}, {'client': facts['executor'], 'executor': facts['client']}]:
            hostfile.write_text(json.dumps(invalid))
            assert cli(*arguments, status=2)['code'] == 'TEST_PAIR_HOSTS'
        for side in ['client', 'executor']:
            for change, status, state in [({'capabilities': []}, 4, 'unsupported'), ({'deniedCapabilities': ['session.rpc']}, 6, 'denied'), ({'sdkVersion': 'unknown'}, 3, 'unknown')]:
                hostfile.write_text(json.dumps({**facts, side: {**facts[side], **change}}))
                report = cli(*arguments, status=status)
                assert report['compatibility'] == state and report['checkedSide'] == side
    elif name == 'paired-declarations':
        target = project / 'plugin.json'; original = json.loads(target.read_text())
        for side in ['client', 'executor']:
            manifest = json.loads(json.dumps(original))
            selected = manifest['client'] if side == 'client' else manifest['executor']['variants'][0]
            selected['requires'] = []
            manifest['permissions'] = [p for p in manifest['permissions'] if p['side'] != side]
            target.write_text(json.dumps(manifest))
            assert native(project / 'plugin.pair.test.json', side='paired', status=1, diagnostic='TEST_REQUIRES_DECLARED_RPC')['kind'] == 'error'
    elif name == 'node-project':
        execution_project('node')
        package = work / 'package'; cli('build', *common, '--out', package)
        assert b'node:fs' in (package / 'executor/index.js').read_bytes() and (package / 'client/index.js').is_file()
        assert not (work / 'ENTRY_RAN').exists() and not (project / 'HOOK_RAN').exists()
        facts = {**host, 'runtimes': ['node'], 'programs': {'node': {'state': 'available', 'version': '22.19.0'}}}
        hostfile.write_text(json.dumps(facts)); assert check()['facts']['variantId'] == 'node'
        assert cli('check', '--package', package, '--tools', tools, '--host-json', hostfile)['runtimes'] == ['portable-js', 'node']
        a, b = work / 'a.zip', work / 'b.zip'
        cli('pack', *common, '--out', a); cli('pack', '--package', package, '--tools', tools, '--out', b)
        assert a.read_bytes() == b.read_bytes() and not (work / 'ENTRY_RAN').exists()
        assert native(side='client')['side'] == 'client'
    elif name == 'node-boundaries':
        execution_project('node'); target = project / 'client/index.ts'; original = target.read_bytes()
        reject('client/index.ts', original + b'export const bad = process;\n')
        reject('client/index.ts', original + b'import "node:fs";\n')
        target = project / 'executor/index.ts'; original = target.read_bytes()
        for bad in [b'export const bad: any = 1;\n', b'export const bad: string = Buffer.from("bytes");\n', b'import "node:missing_builtin";\n', b'import "some-uninstalled-npm-package";\n', b'export const bad = document;\n', b'import "../../escape.js";\n']:
            reject('executor/index.ts', original + bad)
        reject('package.json', b'{"type":"commonjs"}')
    elif name == 'source-references':
        target = project / 'client/index.ts'; original = target.read_bytes()
        ambient = work / 'ambient.d.ts'; ambient.write_text('declare const outsideSnapshot: string;\n')
        for prefix, statement in [('/// <reference types="node" />\n', 'export const leaked = process;\n'), ('/// <reference lib="dom" />\n', 'export const leaked = document;\n'), ('/// <reference path=' + json.dumps(str(ambient)) + ' />\n', 'export const leaked = outsideSnapshot;\n')]:
            reject('client/index.ts', prefix.encode() + original + statement.encode())
    elif name == 'node-facts':
        execution_project('node')
        for fact, status, state in [(None, 3, 'unknown'), ({'state': 'missing'}, 4, 'unsupported'), ({'state': 'denied'}, 6, 'denied'), ({'state': 'available', 'version': '20.0.0'}, 4, 'unsupported'), ({'state': 'available', 'version': '22.19.0'}, 0, 'compatible')]:
            hostfile.write_text(json.dumps({**host, 'runtimes': ['node'], 'programs': {'node': fact}}))
            assert check(status)['compatibility'] == state
        args = ['test', *common, '--contract', project / 'plugin.test.json', '--core-build', work / 'must-not-run', '--host-json', hostfile]
        assert cli(*args, status=4)['reason'] == 'TEST_RUNTIME_UNSUPPORTED'
    elif name == 'exec-project':
        execution_project('exec'); hostfile.write_text(json.dumps({**host, 'runtimes': ['exec'], 'os': 'macos', 'arch': 'aarch64'}))
        assert check()['facts']['variantId'] == 'native'
        package = work / 'package'; cli('build', *common, '--host-json', hostfile, '--out', package)
        assert (package / 'bin/fixture').read_bytes() == Path('/usr/bin/true').read_bytes()
        a, b = work / 'a.zip', work / 'b.zip'
        cli('pack', *common, '--out', a); cli('pack', '--package', package, '--tools', tools, '--out', b)
        assert a.read_bytes() == b.read_bytes()
        reject('bin/fixture', b'')
        (package / 'bin/fixture').write_bytes(b'tampered')
        assert cli('check', '--package', package, '--tools', tools, '--host-json', hostfile, status=1)['kind'] == 'error'
    elif name == 'runtime-variants':
        manifest, config = execution_project('node')
        manifest['executor']['variants'].append(dict(id='native', runtime='exec', executable='bin/fixture', requires=['session.rpc'], os=['macos'], arch=['aarch64']))
        (project / 'bin').mkdir(); shutil.copyfile('/usr/bin/true', project / 'bin/fixture'); config['assets'].append('bin/fixture')
        (project / 'plugin.json').write_text(json.dumps(manifest)); (project / 'plugin.project.json').write_text(json.dumps(config))
        facts = {**host, 'runtimes': ['node', 'exec'], 'os': 'macos', 'arch': 'aarch64', 'programs': {'node': {'state': 'available', 'version': '22.19.0'}}}
        hostfile.write_text(json.dumps(facts)); assert check(1)['kind'] == 'error'
        for variant in ['node', 'native']:
            hostfile.write_text(json.dumps({**facts, 'variantId': variant})); assert check()['facts']['variantId'] == variant
        hostfile.write_text(json.dumps({**facts, 'runtimes': ['node'], 'variantId': 'native'})); assert check(4)['compatibility'] == 'unsupported'
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


CASES = ['reproducible', 'host-states', 'strict-types', 'dependencies', 'imports', 'physical-inputs', 'json-inputs', 'integrity', 'conflicts', 'publication', 'rpc', 'activation-failure', 'dispose-failure', 'rpc-required', 'services-unsupported', 'interrupt-cleanup', 'client-rpc', 'client-contract', 'client-failures', 'client-rpc-required', 'side-profile']
CASES += ['paired-rpc', 'paired-contract', 'paired-failures', 'paired-hosts', 'paired-declarations']
CASES += ['node-project', 'node-boundaries', 'node-facts', 'exec-project', 'runtime-variants']
CASES += ['source-references']
MUTATIONS = [
    ('explicit-any', 'strict-types', 'scripts/plugin-dev-worker.mjs', "demand(node.kind !== ts.SyntaxKind.AnyKeyword, 'EXPLICIT_ANY');", ''),
    ('host-denied', 'host-states', 'scripts/plugin-dev-worker.mjs', 'compatibility = error.state; reason = error.code;', "compatibility = error.state === 'denied' ? 'unknown' : error.state; reason = error.code;"),
    ('integrity', 'integrity', 'scripts/plugin-dev-worker.mjs', 'facts = inspect(manifest, integrity, observed, host, schemas);', 'facts = {};'),
    ('queued-cancel', 'rpc', 'scripts/probes/plugin-testkit.c', 'gt_ext_session_cancel_request(peer, &lease, handle) == GT_EXT_RPC_OK', 'true'),
    ('old-epoch', 'rpc', 'scripts/probes/plugin-testkit.c', 'stale.epoch ^= 1;', 'stale.epoch = lease.epoch;'),
    ('revocation', 'rpc', 'scripts/probes/plugin-testkit.c', 'gt_ext_session_revoke(peer);', '(void)peer;'),
    ('client-role', 'client-rpc', 'scripts/probes/plugin-testkit.c', '.role = role, .invocation = &invocation', '.role = GT_EXT_EXECUTOR, .invocation = &invocation'),
    ('client-declaration', 'client-rpc-required', 'scripts/plugin-dev-worker.mjs', "selected?.requires.includes('session.rpc')", "manifest.executor.variants[0].requires.includes('session.rpc')"),
    ('client-response', 'client-contract', 'scripts/gterm-plugin', "require(observations[1]['confirmed'] and observations[1]['outcome'] == dict(kind='success', value=bind(contract['expected'])), 'TEST_RESPONSE')", "require(True, 'TEST_RESPONSE')"),
    ('host-side', 'side-profile', 'scripts/gterm-plugin', "require(isinstance(host, dict) and host.get('side') == args.side, 'TEST_HOST_SIDE')", "require(True, 'TEST_HOST_SIDE')"),
    ('paired-executor-pump', 'paired-rpc', 'scripts/probes/plugin-pair-testkit.c', '    gt_ext_runtime_step(executor);', '    (void)executor;'),
    ('paired-direction', 'paired-rpc', 'scripts/probes/plugin-pair-testkit.c', 'methods[i].receiver = !strcmp(split + 1, "client") ? GT_EXT_HOST : GT_EXT_EXECUTOR;', 'methods[i].receiver = !strcmp(split + 1, "client") ? GT_EXT_EXECUTOR : GT_EXT_HOST;'),
    ('paired-executor-release', 'paired-rpc', 'scripts/probes/plugin-pair-testkit.c', '    pair_release(&executor);', '    /* missing Executor release */'),
    ('paired-executor-facts', 'paired-hosts', 'scripts/gterm-plugin', "report = worker(work, tools, package, None, host['executor'], registry, True)", "report = worker(work, tools, package, None, host['client'], registry, True)"),
    ('portable-node-types', 'node-boundaries', 'scripts/plugin-dev-worker.mjs', "types: side.runtime === 'node' ? ['node'] : []", "types: ['node']"),
    ('node-esm', 'node-boundaries', 'scripts/plugin-dev-worker.mjs', "json(path.join(directory, 'package.json')).type === 'module'", "true"),
    ('exec-empty', 'exec-project', 'scripts/plugin-dev-worker.mjs', 'catalog.get(side.executable).length > 0', 'true'),
    ('test-runtime', 'node-facts', 'scripts/plugin-dev-worker.mjs', "if (compatibility === 'compatible' && selected.runtime !== 'portable-js')", "if (false)"),
    ('source-references', 'source-references', 'scripts/plugin-dev-worker.mjs', "demand(!tree.referencedFiles.length && !tree.typeReferenceDirectives.length && !tree.libReferenceDirectives.length, 'REFERENCE_DIRECTIVE');", ''),
]


def mutations(work, tools, core):
    isolated = work / 'isolated'; isolated.mkdir(); (isolated / 'tmp').mkdir()
    for directory in ['packages/plugin-sdk', 'core/include']:
        shutil.copytree(ROOT / directory, isolated / directory)
    files = ['LICENSE', 'core/tests/test_ext_session.c', 'scripts/test-plugin-dev.py', 'scripts/gterm-plugin', 'scripts/plugin-dev-worker.mjs', 'scripts/probes/plugin-testkit.c', 'scripts/probes/plugin-pair-testkit.c', 'scripts/lib/gt-timeout.sh', 'scripts/fixtures/plugin-dev-tools/package.json', 'scripts/fixtures/plugin-dev-tools/package-lock.json']
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
    sources = {ROOT / name for name in ['scripts/gterm-plugin', 'scripts/plugin-dev-worker.mjs', 'scripts/probes/plugin-testkit.c', 'scripts/probes/plugin-pair-testkit.c', 'scripts/lib/gt-timeout.sh', 'core/tests/test_ext_session.c', 'LICENSE']}
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
