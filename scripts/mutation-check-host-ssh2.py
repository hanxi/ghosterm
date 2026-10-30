#!/usr/bin/env python3
"""Mutate host SSH dependency/packaging guards using real library artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import re
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'core/cmake/verify-ssh2.py'
CONFIGURE = ROOT / 'core/cmake/HostSSH2.cmake'
PREFIX = ROOT / 'build/host-native'
MARK = 'GT_HOST_SSH2_MUTATION'


def load(text):
    scope = {'__name__': 'isolated_ssh2_verifier', '__file__': str(SOURCE)}
    exec(compile(text, str(SOURCE), 'exec'), scope)
    return scope


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    args = parser.parse_args()
    clean = SOURCE.read_text()
    configure_clean = CONFIGURE.read_bytes()
    if MARK in clean or MARK.encode() in configure_clean:
        raise RuntimeError('Residual host dependency mutation')
    digest = hashlib.sha256(SOURCE.read_bytes()).hexdigest()
    baseline = load(clean)
    baseline['verify'](PREFIX, ROOT)
    baseline['check_bundle'](args.app, PREFIX, ROOT)
    with tempfile.TemporaryDirectory(prefix='host-ssh2-guards-', dir=ROOT / 'tmp') as directory:
        work = Path(directory)
        prefix = work / 'prefix'
        shutil.copytree(PREFIX, prefix, symlinks=True)
        app = work / 'Probe.app'
        (app / 'Contents/Frameworks').mkdir(parents=True)
        shutil.copyfile(args.app / 'Contents/Frameworks/libssh2.1.dylib', app / 'Contents/Frameworks/libssh2.1.dylib')
        baseline['stage_bundle'](app, prefix, ROOT)
        all_files = [p for p in prefix.rglob('*') if p.is_file() and not p.is_symlink()]
        all_files += [p for p in app.rglob('*') if p.is_file()]
        snapshots = {p: p.read_bytes() for p in all_files}
        def restore():
            for path, data in snapshots.items(): path.write_bytes(data)
        def damage(kind):
            manifest = prefix / 'receipt.json'
            record = json.loads(manifest.read_text())
            if kind in ('source', 'patch', 'builder', 'crypto'):
                if kind == 'source': record['source_sha256'] = '0' * 64
                if kind == 'patch': record['patches'] = {}
                if kind == 'builder': record['builder'] = '0' * 64
                if kind == 'crypto': record['crypto']['sha256'] = '0' * 64
                manifest.write_text(json.dumps(record))
            elif kind in ('header', 'archive'):
                path = (prefix / ('include/libssh2.h' if kind == 'header' else 'lib/libssh2.a')).resolve()
                assert path.is_relative_to(prefix)
                path.write_bytes(path.read_bytes() + b'broken')
            elif kind == 'sections':
                # Alter actual compiled code, keeping the Mach-O/load commands intact.
                path = app / 'Contents/Frameworks/libssh2.1.dylib'
                layout = subprocess.check_output(['otool', '-l', str(path)], text=True)
                match = re.search(r'sectname __text\s+segname __TEXT.*?\boffset (\d+)', layout, re.S)
                if not match:
                    raise RuntimeError('Compiled library has no executable section')
                data = bytearray(path.read_bytes())
                data[int(match.group(1)) + 4] ^= 1
                path.write_bytes(data)
            elif kind == 'receipt':
                (app / 'Contents/Resources/libssh2-receipt.json').write_text('{}')
            elif kind == 'license':
                (app / 'Contents/Resources/licenses/libssh2.txt').write_text('missing license')
            elif kind == 'magic':
                path = app / 'Contents/Frameworks/libssh2.1.dylib'
                data = bytearray(path.read_bytes()); data[0] = 0; path.write_bytes(data)
        def rejected(scope, kind):
            restore(); damage(kind)
            try:
                # Exercise the actual configuration-time verifier for prefix
                # damage. A stale bundle receipt would otherwise reject the
                # mutation even when the targeted prefix guard was removed.
                if kind in ('source', 'patch', 'builder', 'crypto', 'header', 'archive'):
                    scope['verify'](prefix, ROOT)
                else:
                    scope['check_bundle'](app, prefix, ROOT)
            except (RuntimeError, ValueError):
                return
            raise AssertionError('guard accepted damaged ' + kind)
        cases = [
            ('source', "record.get('source_sha256') != SOURCE_SHA", 'False', ['source'], 'patch'),
            ('patch', "record.get('patches') != expected", 'False', ['patch'], 'header'),
            ('builder', "record.get('builder') != sha(root / 'scripts/build-host-ssh2.py')", 'False', ['builder'], 'source'),
            ('artifact', "record.get('sha256', {}).get(name) != sha(prefix / name)", 'False', ['header', 'archive'], 'source'),
            ('crypto', "record['crypto']['sha256'] != sha(crypto)", 'False', ['crypto'], 'patch'),
            ('sections', "sections(bundled) != sections(prefix / 'lib/libssh2.dylib')", 'False', ['sections'], 'receipt'),
            ('receipt', 'json.loads(receipt.read_text()) != portable_record(record)', 'False', ['receipt'], 'sections'),
            ('license', "(app / 'Contents/Resources/licenses/libssh2.txt').read_bytes() != (prefix / 'share/licenses/libssh2.txt').read_bytes()", 'False', ['license'], 'sections'),
            ('magic', "struct.unpack_from('<I', data)[0] != 0xfeedfacf", 'False', ['magic'], 'receipt'),
        ]
        for name, old, new, targets, control in cases:
            assert clean.count(old) == 1, name
            mutant = load(clean.replace(old, new) + '\n# ' + MARK + '\n')
            for target in targets:
                rejected(baseline, target)
                try:
                    rejected(mutant, target)
                except AssertionError:
                    pass
                else:
                    raise RuntimeError('Mutation did not expose missing guard: ' + name)
                rejected(baseline, target)
            rejected(mutant, control)
            restore(); mutant['check_bundle'](app, prefix, ROOT)
            assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == digest
            print('PASS host dependency ' + name + ': compiled target red / independent green / restored green / SHA256', flush=True)
        # Bundle metadata must not carry build-machine filesystem paths.
        old = "result['crypto'] = {'sha256': record['crypto']['sha256']}"
        assert clean.count(old) == 1
        mutant = load(clean.replace(old, "result['crypto'] = dict(record['crypto'])"))
        def private_path_absent(scope):
            restore(); scope['stage_bundle'](app, prefix, ROOT)
            data = json.loads((app / 'Contents/Resources/libssh2-receipt.json').read_text())
            assert 'path' not in data['crypto'], 'bundle leaked build dependency path'
        private_path_absent(baseline)
        try:
            private_path_absent(mutant)
        except AssertionError:
            pass
        else:
            raise RuntimeError('Portable receipt mutation did not turn red')
        rejected(mutant, 'header'); private_path_absent(baseline)
        # Test the actual CMake consumer too: verifying a receipt in isolation
        # is insufficient if configure ignores the verifier's failure.
        restore()
        core = work / 'core'
        shutil.copytree(ROOT / 'core', core, ignore=shutil.ignore_patterns('build', 'build-*'))
        configure = core / 'cmake/HostSSH2.cmake'
        def configure_result(label, target=''):
            command = ['cmake', '-S', str(core), '-B', str(work / label),
                       '-DGHOSTERM_BUILD_TESTS=OFF', '-DGHOSTERM_HOST_SSH2_PREFIX=' + str(prefix)]
            if target:
                command.append('-DGHOSTERM_SSH2_TARGET=' + target)
            result = subprocess.run(['bash', '-c',
                '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require host-ssh2; shift; gt_timeout 60 "$@"',
                'host-ssh2', str(ROOT), *command], capture_output=True, text=True,
                env=dict(os.environ, TMPDIR=str(ROOT / 'tmp')))
            return result.returncode, result.stdout + result.stderr
        def configured(label):
            code, output = configure_result(label)
            if code or 'verified patched libssh2 1.11.1' not in output:
                print(output, flush=True)
                raise RuntimeError('Valid CMake dependency failed: ' + label)
        def configure_rejected(label, target='', message='source/patch receipt mismatch'):
            code, output = configure_result(label, target)
            if code == 0 and 'verified patched libssh2 1.11.1' in output:
                raise AssertionError('CMake accepted the damaged dependency: ' + label)
            if code != 1 or message not in ' '.join(output.split()):
                print(output, flush=True)
                raise RuntimeError('Unexpected CMake failure: ' + label)
        configured('configure-valid')
        damage('source'); configure_rejected('configure-rejected')
        original = configure_clean.decode()
        old = 'if(NOT _ssh2_status EQUAL 0)'
        assert original.count(old) == 1
        configure.write_text(original.replace(old, 'if(FALSE)') + '\n# ' + MARK + '\n')
        try:
            configure_rejected('configure-mutated')
        except AssertionError:
            pass
        else:
            raise RuntimeError('CMake mutation did not turn target red')
        configure_rejected('configure-control', 'missing_ssh_dependency',
                           'GHOSTERM_SSH2_TARGET must name an existing target')
        restore(); configured('configure-mutated-valid')
        configure.write_bytes(configure_clean)
        damage('source'); configure_rejected('configure-restored')
        restore(); configured('configure-restored-valid')
        assert CONFIGURE.read_bytes() == configure_clean
        print('PASS host dependency CMake: target red / independent green / restored green / SHA256', flush=True)
    assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == digest
    print('HOST SSH2 MUTATIONS PASS: 11 groups')


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
