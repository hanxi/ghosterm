#!/usr/bin/env python3
"""Compile a Portable sample using locked dev tools; never install or execute it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    source, tools, output = (p.resolve() for p in (args.source, args.tools, args.out))
    if not source.is_relative_to(ROOT / 'plugins') or not (output.is_relative_to(ROOT / 'tmp') or output.is_relative_to(ROOT / 'build/plugins')):
        parser.error('use repository plugin source and owned tmp/build/plugins output')
    if output.exists() and not args.check: parser.error('output must be new; --check only compares existing output')
    for name in ('package.json', 'package-lock.json'):
        if (tools / name).read_bytes() != (ROOT / 'scripts/fixtures/plugin-sdk-tools' / name).read_bytes():
            parser.error('locked SDK developer tools required')
    manifest = json.loads((source / 'plugin.json').read_text())
    env = {**os.environ, 'TMPDIR': str(ROOT / 'tmp')}
    version = subprocess.check_output(['node', str(tools / 'node_modules/typescript/bin/tsc'), '--version'], env=env, text=True, timeout=10).strip()
    if version != 'Version 5.9.3': raise ValueError('TypeScript 5.9.3 is required')
    with tempfile.TemporaryDirectory(prefix='sample-build-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); stage = work / 'package'; stage.mkdir()
        subprocess.run(['node', str(tools / 'node_modules/typescript/bin/tsc'), '-p', str(source / 'tsconfig.json'),
            '--noEmit', 'false', '--rootDir', str(ROOT), '--outDir', str(work / 'emitted'), '--noEmitOnError', 'true'],
            env=env, cwd=ROOT, check=True, timeout=180)
        emitted = work / 'emitted' / source.relative_to(ROOT)
        for file in emitted.rglob('*.js'):
            target = stage / file.relative_to(emitted); target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(file, target)
        for relative in ['plugin.json', 'README.md', *[c['argsSchema'] for c in manifest['commands']]]:
            origin = (source / relative).resolve()
            if not origin.is_relative_to(source): raise ValueError('manifest member escaped source')
            target = stage / relative; target.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(origin, target)
        (stage / 'LICENSES').mkdir(); shutil.copyfile(ROOT / 'LICENSE', stage / 'LICENSES/MIT.txt')
        members = {}
        for file in sorted(stage.rglob('*')):
            if not file.is_file(): continue
            data = file.read_bytes(); name = file.relative_to(stage).as_posix()
            if file.suffix == '.js':
                if len(data) > 131072 or re.search(r'\bimport\s*\(', data.decode()): raise ValueError('unsupported script size/import')
                for reference in re.findall(r'(?:from\s+|import\s*)[\"\']([^\"\']+)[\"\']', data.decode()):
                    if reference == '@ghosterm/plugin-sdk': continue
                    target = (file.parent / reference).resolve()
                    if not reference.startswith('.') or not target.is_relative_to(stage) or not target.is_file(): raise ValueError('unsupported import: ' + reference)
            members[name] = {'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        if len(members) > 64: raise ValueError('too many sample members')
        (stage / 'integrity.json').write_text(json.dumps({'schemaVersion': 1, 'members': list(members.values())}, separators=(',', ':')) + '\n')
        def contents(path): return {p.relative_to(path).as_posix(): p.read_bytes() for p in path.rglob('*') if p.is_file()}
        if args.check:
            if not output.is_dir() or contents(stage) != contents(output): raise ValueError('sample output differs')
        else:
            output.parent.mkdir(parents=True, exist_ok=True); stage.rename(output)
        print('PORTABLE SAMPLE ' + ('CHECK' if args.check else 'BUILD') + ' PASS: ' + manifest['id'])


if __name__ == '__main__': main()
