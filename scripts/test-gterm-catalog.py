#!/usr/bin/env python3
"""Persistent development catalog and ordinary argv; owned local fixtures only.

Uses the existing actual C HOST for Session decisions/RPC/resources. No user
catalog, external SSH target, credentials, model or foreground windows.
"""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('catalog_ipc', ROOT / 'scripts/test-gterm-ipc.py')
ipc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ipc)
CASES = ['persist', 'roundtrip', 'deny', 'cancel', 'preview', 'conflict', 'prefix',
         'select', 'stale-select', 'missing-select', 'stale-revision', 'replace',
         'stale-replace', 'duplicate', 'unregister', 'tamper', 'digest-change',
         'unrelated-tamper', 'invalid-arguments', 'no-endpoint', 'unknown',
         'directory-mode', 'directory-link', 'data-mode', 'data-link', 'data-hardlink',
         'data-fifo', 'lock-link', 'lock-hardlink', 'busy', 'truncated', 'trailing',
         'profile-overflow', 'relative-root', 'injected-trust', 'bad-client',
         'unknown-host', 'namespace', 'profile-node-approval']
CASES += ['multi-token', 'identity', 'catalog-size', 'profile-bytes', 'string-limit',
          'revision-zero', 'magic', 'bad-utf8', 'bad-digest', 'umask', 'explicit-command',
          'wrong-command', 'invalid-prefix-package', 'cwd-preview']
FAULTS = ['file-sync', 'directory-sync', 'directory-close']


def require(value, message):
    if not value:
        raise AssertionError(message)


def rehash(package):
    p = package / 'integrity.json'
    data = json.loads(p.read_text())
    for entry in data['members']:
        raw = (package / entry['path']).read_bytes()
        entry.update(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
    p.write_text(json.dumps(data))
    return hashlib.sha256(p.read_bytes()).hexdigest()


def execute(build, case, work):
    binary = build / 'gterm'
    catalog = work / 'catalog'; catalog.mkdir(mode=0o700)
    endpoint = work / 'endpoint'; endpoint.mkdir(mode=0o700)
    parent = work / 'one'; parent.mkdir()
    package, digest = ipc.package(parent)
    host = work / 'host.json'
    facts = dict(side='executor', sdkVersion='1.0.0', protocols=[1],
                 runtimes=['portable-js'], capabilities=['session.rpc'])
    host.write_text(json.dumps(facts))
    env = {k: v for k, v in os.environ.items() if not k.startswith(('GTERM_', 'GT_GTERM_'))}
    env.update(TMPDIR=str(ROOT / 'tmp'), GTERM_CATALOG=str(catalog), GTERM_ENDPOINT=str(endpoint))
    profile = ['--client-digest', 'd' * 64, '--method', 'fixture.echo']
    data_file = catalog / 'executor-catalog-v1'

    def run(args, expected=0, cwd=None):
        result = subprocess.run([binary, *map(str, args)], capture_output=True, env=env, timeout=15,
                                preexec_fn=(lambda: os.umask(0o777)) if case == 'umask' else None, cwd=cwd)
        if result.returncode < 0 or any(s in result.stderr for s in
                (b'AddressSanitizer', b'runtime error:', b'UndefinedBehaviorSanitizer')):
            raise RuntimeError((result.returncode, result.stderr))
        require(result.returncode == expected,
                f'{case}: expected exit {expected}, got {result.returncode}: {result.stdout!r} {result.stderr!r}')
        value = json.loads(result.stdout)
        require(value['authorizationGranted'] is False and value['publisherAuthenticated'] is False,
                'catalog operations must not grant Session permission or publisher identity')
        return value

    def register(rev='0', directory=package, sha=digest, extra=(), launch=None, expected=0, cwd=None):
        return run(['plugin', 'register-development', '--catalog', catalog,
                    '--expect-revision', rev, '--host-json', host,
                    '--trust-development-digest', sha, *extra, '--', directory,
                    *(profile if launch is None else launch)], expected, cwd=cwd)

    def changed_package(name='two', command=None, same_id=False):
        parent = work / name; parent.mkdir()
        other, _ = ipc.package(parent)
        m = json.loads((other / 'plugin.json').read_text())
        if not same_id:
            m['id'] = 'org.ghosterm.' + name
        m['pluginVersion'] = '0.2.0'
        if command:
            m['commands'][0]['path'] = command
        (other / 'plugin.json').write_text(json.dumps(m))
        return other, rehash(other)

    def frontend(mode='roundtrip', args=ipc.CALL, selection=(), chosen=package, sha=digest, fixture_extra=(), canonical='fixture', cwd=None):
        master, slave = pty.openpty()
        process = subprocess.Popen([binary, *selection, *args], stdin=slave, stdout=slave,
                                   stderr=subprocess.PIPE, env=env, cwd=cwd)
        try:
            ipc.wait(lambda: (endpoint / 'permit').exists() or process.poll() is not None,
                     'ordinary command endpoint')
            require(process.poll() is None, 'ordinary command must select catalog bytes and listen')
            ipc.wait(lambda: (endpoint / 'permit').stat().st_size == 56, 'ordinary command permit')
            instance = (endpoint / 'permit').read_bytes()[8:24].hex()
            if mode == 'cancel':
                process.terminate()
                require(process.wait(timeout=6) == 130, 'ordinary cancellation must reach IPC cleanup')
            else:
                result = subprocess.run([build / 'test_gterm_stdio', binary, chosen, sha, mode, *fixture_extra],
                    capture_output=True, env={**env, 'GT_GTERM_FIXTURE_ATTACH': str(endpoint),
                    'GT_GTERM_FIXTURE_ATTACH_INSTANCE': instance,
                    'GT_GTERM_FIXTURE_COMMAND': canonical,
                    'GT_GTERM_FIXTURE_PLUGIN_ID': json.loads((chosen / 'plugin.json').read_text())['id'],
                    'GT_GTERM_FIXTURE_PLUGIN_VERSION': json.loads((chosen / 'plugin.json').read_text())['pluginVersion']}, timeout=25)
                if result.returncode < 0 or b'ABNORMAL' in result.stderr or b'AddressSanitizer' in result.stderr:
                    raise RuntimeError((result.returncode, result.stderr))
                require(result.returncode == 0 and ('PASS stdio ' + mode).encode() in result.stdout,
                        'ordinary invocation must complete actual HOST consent/RPC: ' + repr((result.stdout, result.stderr)))
                require(process.wait(timeout=8) == (6 if mode == 'deny' else 0),
                        'foreground result must preserve actual consent outcome')
            require(not list(endpoint.iterdir()), 'ordinary endpoint must release its owned entries')
            require(not select.select([master], [], [], .03)[0], 'plugin bytes must stay outside the terminal')
        finally:
            ipc.terminate(process)
            process.stderr.close(); os.close(slave); os.close(master)

    def frame(entries, rev=1):
        def field(value):
            raw = value if isinstance(value, bytes) else value.encode()
            return len(raw).to_bytes(4, 'big') + raw
        records = []
        for entry in entries:
            ident = entry.get('id', 'org.ghosterm.fixture')
            args = entry.get('profile', profile)
            records.append(b''.join(field(s) for s in [ident, '0.1.0', digest, str(package),
                           entry.get('host', host.read_text())]) + len(args).to_bytes(4, 'big') +
                           b''.join(field(s) for s in args))
        return b'GTCAT01\0' + rev.to_bytes(8, 'big') + len(records).to_bytes(4, 'big') + b''.join(records)

    if case == 'bad-digest':
        register(sha='0' * 64, expected=6)
        require(not data_file.exists(), 'source approval must match the captured package digest'); return
    if case == 'invalid-prefix-package':
        m = json.loads((package / 'plugin.json').read_text())
        m['commands'].append({**m['commands'][0], 'path': ['fixture', 'sub']})
        (package / 'plugin.json').write_text(json.dumps(m))
        register(sha=rehash(package), expected=5)
        require(not data_file.exists(), 'an invalid intra-package prefix must remain rejected'); return
    if case in ('explicit-command', 'wrong-command'):
        register()
        other, _ = changed_package(command=['fixture', 'sub'])
        code = other / 'executor/index.js'; code.write_text(code.read_text().replace("command!=='fixture'", "command!=='fixture sub'"))
        sha = rehash(other); register('1', directory=other, sha=sha)
        selection = ['--select', 'org.ghosterm.two', sha, '--command', 'fixture sub' if case == 'explicit-command' else 'fixture']
        if case == 'wrong-command':
            env.pop('GTERM_ENDPOINT'); run([*selection, 'fixture', 'sub', *ipc.CALL[1:]], 10)
        else:
            frontend(args=['fixture', 'sub', *ipc.CALL[1:]], sha=sha, canonical='fixture sub', chosen=other, selection=selection)
        return
    if case == 'multi-token':
        m = json.loads((package / 'plugin.json').read_text()); m['commands'][0]['path'] = ['fixture', 'sub']
        (package / 'plugin.json').write_text(json.dumps(m))
        code = package / 'executor/index.js'; code.write_text(code.read_text().replace("command!=='fixture'", "command!=='fixture sub'"))
        sha = rehash(package); register(sha=sha)
        frontend(args=['fixture', 'sub', *ipc.CALL[1:]], sha=sha, canonical='fixture sub'); return
    if case in ('relative-root', 'injected-trust', 'bad-client', 'profile-node-approval'):
        launch = list(profile)
        if case == 'relative-root': launch += ['--root', 'workspace', '.']
        elif case == 'injected-trust': launch += ['--trust-development-digest', digest]
        elif case == 'bad-client': launch[1] = 'bad'
        else: launch += ['--runtime', 'node']
        register(launch=launch, expected=6 if case == 'profile-node-approval' else 2)
        require(not data_file.exists(), 'invalid startup profile must not commit a catalog')
        return
    if case == 'unknown-host':
        host.write_text('{}'); register(expected=3)
        require(not data_file.exists(), 'unknown compatibility must not persist approval'); return
    if case == 'namespace':
        m = json.loads((package / 'plugin.json').read_text()); m['commands'][0]['path'] = ['relay']
        (package / 'plugin.json').write_text(json.dumps(m))
        register(sha=rehash(package), expected=5); return
    if case in ('preview', 'cwd-preview'):
        package = ROOT / 'plugins/official/preview'
        digest = hashlib.sha256((package / 'integrity.json').read_bytes()).hexdigest()
        workspace = work / 'workspace'; site = workspace / 'site'; site.mkdir(parents=True)
        (site / 'index.md').write_text('# Preview runtime fixture\n')
        (site / 'theme,dark.css').write_text('body { color: rgb(1, 2, 3); }\n')
        store = work / 'store'; store.mkdir(mode=0o700)
        facts['capabilities'] = ['session.rpc', 'resource.receive', 'file.readScoped']
        host.write_text(json.dumps(facts))
        registration = work / 'registration'
        if case == 'cwd-preview':
            (registration / 'site').mkdir(parents=True)
            (registration / 'site/index.md').write_text('different registration cwd')
        register(directory=package, sha=digest,
                 launch=['--client-digest', 'd' * 64, '--root', 'workspace',
                         '@cwd' if case == 'cwd-preview' else str(workspace)],
                 cwd=registration if case == 'cwd-preview' else None)
        frontend('preview', ['preview', 'site/index.md', '--assets', 'site/theme\\,dark.css'],
                 chosen=package, sha=digest, fixture_extra=[workspace, store],
                 cwd=workspace if case == 'cwd-preview' else None); return

    result = register()
    require(result['result'] == 'registered' and result['revision'] == '1' and result['committed'],
            'explicit package trust must persist exactly one catalog revision')
    original = data_file.read_bytes()
    if case in ('persist', 'umask'):
        listed = run(['plugin', 'list', '--catalog', catalog])
        require(listed['revision'] == '1' and len(listed['entries']) == 1 and
                listed['entries'][0]['packageDigest'] == digest,
                'a separate process must read the exact persisted package selection')
        require((data_file.stat().st_mode & 0o777) == 0o600 and
                set(p.name for p in catalog.iterdir()) == {'executor-catalog-v1', 'executor-catalog.lock'},
                'catalog publication must retain private files without temporary siblings')
    elif case in ('roundtrip', 'deny', 'cancel'):
        if case == 'deny':
            code = package / 'executor/index.js'; code.write_text("throw Error('MUST_NOT_ACTIVATE');" + code.read_text())
            new_digest = rehash(package)
            register('1', sha=new_digest, extra=['--expect-digest', digest])
            frontend('deny', sha=new_digest)
        else: frontend(case)
    elif case in ('conflict', 'select', 'prefix', 'stale-select', 'missing-select', 'unrelated-tamper'):
        other, other_digest = changed_package(command=['fixture', 'sub'] if case == 'prefix' else None)
        register('1', directory=other, sha=other_digest)
        if case in ('conflict', 'prefix'):
            env.pop('GTERM_ENDPOINT')
            result = run(ipc.CALL, 8)
            require(result['code'] == 'COMMAND_RESOLVE', 'command overlap must require explicit selection')
        elif case == 'select':
            frontend(selection=['--select', 'org.ghosterm.two', other_digest], chosen=other, sha=other_digest)
        elif case in ('stale-select', 'missing-select'):
            env.pop('GTERM_ENDPOINT')
            run(['--select', 'org.ghosterm.missing' if case == 'missing-select' else 'org.ghosterm.two', digest, *ipc.CALL], 9)
        else:
            (other / 'README').write_text('changed')
            run(['--select', 'org.ghosterm.fixture', digest, *ipc.CALL], 5)
    elif case == 'stale-revision':
        other, other_digest = changed_package()
        register(directory=other, sha=other_digest, expected=9)
        require(data_file.read_bytes() == original, 'stale revision must not overwrite the persisted catalog')
    elif case in ('replace', 'stale-replace', 'duplicate'):
        other, new_digest = changed_package(same_id=True)
        register('1', directory=other, sha=new_digest,
                 extra=[] if case == 'duplicate' else ['--expect-digest', '0' * 64 if case == 'stale-replace' else digest],
                 expected=8 if case == 'duplicate' else 9 if case == 'stale-replace' else 0)
        if case == 'replace':
            listed = run(['plugin', 'list', '--catalog', catalog])
            require(listed['revision'] == '2' and len(listed['entries']) == 1 and
                    listed['entries'][0]['packageDigest'] == new_digest,
                    'replacement must atomically publish the explicitly selected new bytes')
            run(['--select', 'org.ghosterm.fixture', digest, *ipc.CALL], 9)
        else: require(data_file.read_bytes() == original, 'implicit or stale replacement must preserve the old catalog')
    elif case == 'unregister':
        run(['plugin', 'unregister', '--catalog', catalog, '--expect-revision', '1',
             '--id', 'org.ghosterm.fixture', '--expect-digest', digest])
        listed = run(['plugin', 'list', '--catalog', catalog])
        require(listed['revision'] == '2' and listed['entries'] == [], 'removal must persist without deleting the development directory')
        run(ipc.CALL, 10); require((package / 'README').is_file(), 'unregister must retain user package files')
    elif case in ('tamper', 'digest-change'):
        (package / 'README').write_text('changed')
        if case == 'digest-change': rehash(package); env.pop('GTERM_ENDPOINT')
        run(ipc.CALL, 6 if case == 'digest-change' else 5)
        require(not list(endpoint.iterdir()), 'changed package must fail before creating an IPC endpoint')
    elif case == 'invalid-arguments':
        env.pop('GTERM_ENDPOINT')
        run(['fixture', '--unknown'], 2)
        require(not list(endpoint.iterdir()), 'invalid business argv must fail before endpoint creation')
    elif case == 'no-endpoint':
        env.pop('GTERM_ENDPOINT'); run(ipc.CALL, 4)
    elif case == 'unknown': run(['missing-command'], 10)
    elif case in ('directory-mode', 'directory-link'):
        if case == 'directory-mode': catalog.chmod(0o755)
        else:
            alias = work / 'alias'; alias.symlink_to(catalog, target_is_directory=True); env['GTERM_CATALOG'] = str(alias)
        run(['plugin', 'list', '--catalog', env['GTERM_CATALOG']], 5)
    elif case.startswith(('data-', 'lock-')):
        target = data_file if case.startswith('data-') else catalog / 'executor-catalog.lock'
        if case.endswith('mode'): target.chmod(0o644)
        elif case.endswith('fifo'): target.unlink(); os.mkfifo(target, 0o600)
        elif case.endswith('hardlink'): os.link(target, work / 'retained-link')
        else:
            held = work / 'retained'; target.rename(held); target.symlink_to(held)
        run(['plugin', 'list', '--catalog', catalog], 5)
    elif case == 'busy':
        with (catalog / 'executor-catalog.lock').open('r+b') as lock:
            fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            run(ipc.CALL, 11)
        require(data_file.read_bytes() == original, 'busy catalog must preserve its current data')
    elif case in ('truncated', 'trailing', 'profile-overflow'):
        damaged = original[:-1] if case == 'truncated' else original + b'x'
        if case == 'profile-overflow':
            damaged = bytearray(original); at = 20
            for _ in range(5):
                length = int.from_bytes(damaged[at:at+4], 'big'); at += 4 + length
            damaged[at:at+4] = (0xffffffff).to_bytes(4, 'big')
        data_file.write_bytes(damaged); run(['plugin', 'list', '--catalog', catalog], 5)
        require(data_file.read_bytes() == damaged, 'malformed catalog must not be repaired or partially accepted')
    elif case in ('identity', 'catalog-size', 'profile-bytes', 'string-limit', 'revision-zero', 'magic', 'bad-utf8'):
        entries = [{}]; rev = 1
        if case == 'identity': entries = [dict(id='org.ghosterm.other')]
        elif case == 'catalog-size': entries = [dict(id='org.ghosterm.entry' + str(i), host=' ' * 40000 + '{}') for i in range(32)]
        elif case == 'profile-bytes': entries = [dict(profile=['x' * 4096] * 9)]
        elif case == 'string-limit': entries = [dict(profile=['x' * 4097])]
        elif case == 'revision-zero': rev = 0
        elif case == 'bad-utf8': entries = [dict(id=b'org.ghosterm.fixtur\xc0\x80')]
        damaged = frame(entries, rev)
        if case == 'magic': damaged = b'UNKNOWN\0' + damaged[8:]
        data_file.write_bytes(damaged)
        if case == 'identity':
            env.pop('GTERM_ENDPOINT'); run(ipc.CALL, 9)
        else: run(['plugin', 'list', '--catalog', catalog], 5)
        require(data_file.read_bytes() == damaged, 'invalid catalog must remain untouched')
    elif case in FAULTS:
        env['GTERM_TEST_CATALOG_IO'] = case
        if case == 'directory-close':
            run(['plugin', 'list', '--catalog', catalog], 5)
            require(data_file.read_bytes() == original, 'failed close must not report a successful observation')
        else:
            other, sha = changed_package()
            result = register('1', directory=other, sha=sha, expected=5)
            require(result['committed'] == (case == 'directory-sync') and not result['durabilityConfirmed'],
                    'sync failure must distinguish uncommitted bytes from uncertain durability')
            env.pop('GTERM_TEST_CATALOG_IO')
            if case == 'file-sync': require(data_file.read_bytes() == original, 'failed file sync must preserve the old catalog')
            else: require(run(['plugin', 'list', '--catalog', catalog])['revision'] == '2', 'published data must remain observable after directory sync failure')
            require(not any(p.name.startswith('.executor-catalog-') for p in catalog.iterdir()), 'failed publication must clean only its own temporary file')
    else: raise RuntimeError('unknown case ' + case)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--case', choices=CASES + FAULTS, action='append')
    parser.add_argument('--faults', action='store_true', help='include three cases requiring the explicit IO fault build')
    args = parser.parse_args()
    for case in args.case or (CASES + FAULTS if args.faults else CASES):
        with tempfile.TemporaryDirectory(prefix='gterm-catalog-', dir=ROOT / 'tmp') as temporary:
            execute(args.build.resolve(), case, Path(temporary))
        print('PASS catalog ' + case, flush=True)


if __name__ == '__main__':
    try:
        main()
    except AssertionError as failure:
        print('ASSERTION: ' + str(failure), flush=True)
        raise SystemExit(1)
