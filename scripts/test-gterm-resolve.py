#!/usr/bin/env python3
"""Resolve actual fixed development packages through the public C registry."""
import argparse, hashlib, json, shutil, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CASES = ['unique', 'conflict', 'reverse', 'select', 'stale', 'missing-id', 'missing-command', 'prefix', 'duplicate', 'digest', 'invalid-digest', 'tampered', 'unknown', 'limit', 'usage']
def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--binary', type=Path, required=True); p.add_argument('--case', choices=CASES); p.add_argument('--disabled', action='store_true'); a = p.parse_args()
    work = Path(tempfile.mkdtemp(prefix='gterm-resolve-', dir=ROOT/'tmp'))
    try:
        def package(name, command):
            directory = work/name; directory.mkdir()
            manifest = dict(schemaVersion=1, id='org.ghosterm.'+name, pluginVersion='0.1.0', sdkApiRange='^1.0.0', bridgeProtocolRange=[1], publisher='fixture', license='MIT', commands=[dict(path=command, title='中文\x1b[31m😀', argsSchema='schemas/input.json')], executor=dict(variants=[dict(id='portable', runtime='portable-js', entry='executor/index.js', requires=['session.rpc'])]), permissions=[])
            files = {'plugin.json':json.dumps(manifest), 'README':'fixture', 'LICENSES/MIT.txt':'fixture', 'schemas/input.json':'{}', 'executor/index.js':"throw Error('MUST_NOT_EXECUTE');export function activate(){}"}
            members = []
            for name, text in files.items():
                path = directory/name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text)
                members.append(dict(path=name, bytes=len(path.read_bytes()), sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
            (directory/'integrity.json').write_text(json.dumps(dict(schemaVersion=1, members=members)))
            return [directory, hashlib.sha256((directory/'integrity.json').read_bytes()).hexdigest()]
        one = package('one', ['fixture']); two = package('two', ['fixture']); sub = package('sub', ['fixture', 'sub'])
        host = work/'host.json'; host.write_text(json.dumps(dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=['portable-js'], capabilities=['session.rpc'])))
        for case in [a.case] if a.case else CASES:
            pairs = one + two; options = []; command = 'fixture'; code = 0; kind = 'resolved'
            if case == 'unique': pairs = one
            elif case in ['conflict', 'reverse', 'prefix']: code = 8; kind = 'conflict'
            elif case == 'select': options = ['--select', 'org.ghosterm.two', two[1]]
            elif case == 'stale': options = ['--select', 'org.ghosterm.two', one[1]]; code = 9; kind = 'stale'
            elif case == 'missing-id': options = ['--select', 'org.ghosterm.missing', two[1]]; code = 9; kind = 'stale'
            elif case == 'missing-command': command = 'absent'; code = 10; kind = 'not-found'
            elif case == 'duplicate': pairs = one + one; code = 8; kind = 'conflict'
            elif case == 'digest': pairs = one + [two[0], one[1]]; code = 6; kind = 'denied'
            elif case == 'invalid-digest': pairs = [one[0], 'bad']; code = 2; kind = 'invalid'
            elif case == 'tampered': (two[0]/'README').write_text('changed'); code = 5
            elif case == 'unknown': host.write_text('{}'); code = 3; kind = 'unknown'
            elif case == 'limit': pairs = one * 33; code = 2
            elif case == 'usage': options = ['--select', 'org.ghosterm.one']; code = 2
            if case == 'reverse': pairs = two + one
            if case == 'prefix': pairs = one + sub
            argv = ['plugin', 'resolve', '--host-json', host, *options, '--', command, *pairs]
            if a.disabled: code = 4
            r = subprocess.run([str(a.binary.resolve()), *map(str, argv)], capture_output=True, timeout=15)
            if r.returncode < 0 or any(x in r.stderr for x in [b'AddressSanitizer', b'runtime error:']): raise RuntimeError(r.stderr)
            assert r.returncode == code, (case, r.returncode, code, r.stdout, r.stderr)
            if case in ['limit', 'usage'] and not a.disabled: assert not r.stdout and b'usage:' in r.stderr
            else:
                result = json.loads(r.stdout)
                assert not result['authorizationGranted'] and not result['publisherAuthenticated'], result
                if a.disabled: assert result['code'] == 'PLUGIN_JS_DISABLED'
                elif case == 'tampered': assert result['result'] != 'resolved'
                else: assert result['result'] == kind, result
                if code == 0:
                    selected = two if case == 'select' else one
                    assert result['id'] == 'org.ghosterm.'+selected[0].name and result['packageDigest'] == selected[1], result
                    assert result['command'] == 'fixture' and result['argsSchema'] == 'schemas/input.json', result
                    assert result['title'] == '中文\x1b[31m😀' and all(c < 128 for c in r.stdout) and b'\x1b' not in r.stdout, result
                else: assert 'id' not in result and 'packageDigest' not in result, result
            if case == 'tampered': (two[0]/'README').write_text('fixture')
            if case == 'unknown': host.write_text(json.dumps(dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=['portable-js'], capabilities=['session.rpc'])))
            print('PASS resolve', case)
            if a.disabled: break
    finally: shutil.rmtree(work)
if __name__ == '__main__':
    try: main()
    except AssertionError as error: print('FAIL', error); raise SystemExit(1)
