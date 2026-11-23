#!/usr/bin/env python3
"""Actual CLI subprocesses against owned physical packages; never activates the plugin."""
import argparse, hashlib, json, os, shutil, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CASES = ['compatible', 'unknown', 'unsupported', 'integrity', 'host-nul', 'host-limit', 'host-link', 'host-fifo', 'directory-link', 'usage', 'no-activation', 'doctor']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--case', choices=CASES)
    parser.add_argument('--disabled', action='store_true')
    args = parser.parse_args()
    project_tmp = ROOT / 'tmp'
    scratch = Path(os.environ.get('TMPDIR', project_tmp)).resolve()
    if not scratch.is_dir() or not scratch.is_relative_to(project_tmp):
        scratch = project_tmp
    work = Path(tempfile.mkdtemp(prefix='gterm-test-', dir=scratch))
    try:
        package = work / 'package'; package.mkdir()
        host = work / 'host.json'
        host.write_text(json.dumps(dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=['lua'], capabilities=['session.rpc'])))
        manifest = dict(schemaVersion=2, id='org.ghosterm.fixture', pluginVersion='0.1.0', sdkApiRange='^1.0.0', bridgeProtocolRange=[1], publisher='测试\x1b[31m😀', license='MIT',
            commands=[dict(path=['fixture'], title='Fixture 中文😀', argsSchema='schemas/input.json')],
            executor=dict(entry='executor.lua', requires=['session.rpc']))
        files = {'manifest.json': json.dumps(manifest), 'README': 'fixture', 'LICENSES/MIT.txt': 'fixture', 'executor.lua': "error('MUST_NOT_EXECUTE')", 'schemas/input.json': '{}'}
        members = []
        for name, data in files.items():
            p = package / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(data)
            members.append(dict(path=name, bytes=len(p.read_bytes()), sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
        (package / 'integrity.json').write_text(json.dumps(dict(schemaVersion=1, members=members)))
        def run(argv, code):
            p = subprocess.run([str(args.binary.resolve()), *map(str, argv)], capture_output=True, timeout=15)
            assert p.returncode == code, (p.returncode, code, p.stdout, p.stderr)
            assert not any(x in p.stderr for x in [b'AddressSanitizer', b'runtime error:']), p.stderr
            return p
        check = ['plugin', 'check', '--host-json', host, '--', package]
        if args.disabled:
            assert json.loads(run(['doctor'], 4).stdout)['lua'] == 'unsupported'
            assert json.loads(run(check, 4).stdout)['code'] == 'PLUGIN_RUNTIME_DISABLED'
            print('PASS disabled'); return
        for case in [args.case] if args.case else CASES:
            argv = list(check); code = 0
            if case == 'unknown': argv = ['plugin', 'check', '--', package]; code = 3
            if case == 'unsupported':
                h = json.loads(host.read_text()); h['runtimes'] = []; (work / 'unsupported.json').write_text(json.dumps(h)); argv[3] = work / 'unsupported.json'; code = 4
            if case == 'integrity': (package / 'README').write_text('tampered'); code = 5
            if case.startswith('host-'):
                alternate = work / case
                if case == 'host-nul': alternate.write_bytes(host.read_bytes() + b'\0ignored')
                if case == 'host-limit': alternate.write_bytes(b' ' * 65537)
                if case == 'host-link': alternate.symlink_to(host)
                if case == 'host-fifo': os.mkfifo(alternate)
                argv[3] = alternate; code = 2
            if case == 'directory-link':
                link = work / 'link'; link.symlink_to(package, target_is_directory=True); argv[-1] = link; code = 5
            if case == 'usage': argv = ['plugin', 'check', str(package)]; code = 2
            if case == 'no-activation': argv = ['fixture', '$(touch NEVER)']; code = 4
            if case == 'doctor': argv = ['doctor']
            p = run(argv, code)
            if case == 'usage': assert b'usage:' in p.stderr
            else:
                if case == 'compatible': assert all(c < 128 for c in p.stdout) and b'\x1b' not in p.stdout, p.stdout
                result = json.loads(p.stdout)
                if case == 'compatible':
                    assert result['result'] == 'compatible' and result['publisher'] == manifest['publisher'], result
                    assert result['commands'] == [dict(path='fixture', title='Fixture 中文😀', argsSchema='schemas/input.json')], result
                    assert result['packageDigest'] == hashlib.sha256((package / 'integrity.json').read_bytes()).hexdigest(), result
                    assert not result['authorizationGranted'] and not result['publisherAuthenticated'], result
                    assert all(c < 128 for c in p.stdout) and b'\x1b' not in p.stdout, p.stdout
                elif case == 'unknown': assert result['result'] == 'unknown', result
                elif case == 'unsupported': assert result['result'] == 'unsupported', result
                elif case == 'no-activation': assert result['code'] == 'COMMAND_ACTIVATION_UNAVAILABLE', result
                elif case == 'doctor': assert result == dict(lua='available', commandActivation='explicit-catalog-stdio-or-ipc', localIpc='foreground-one-shot'), result
                else: assert result['result'] != 'compatible', result
            if case == 'integrity': (package / 'README').write_text(files['README'])
            print('PASS', case)
    except AssertionError as error:
        print('FAIL', error); raise SystemExit(1)
    finally:
        shutil.rmtree(work)

if __name__ == '__main__': main()
