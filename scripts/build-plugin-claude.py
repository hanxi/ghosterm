#!/usr/bin/env python3
"""Build and verify the official Claude package with the locked developer toolchain."""
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
SOURCE = ROOT / 'plugins/official/ai-claude'


def run(command, env):
    subprocess.run([str(x) for x in command], cwd=ROOT, env=env, check=True, timeout=300)


def files(directory):
    return {p.relative_to(directory).as_posix(): p.read_bytes()
            for p in directory.rglob('*') if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true', help='fetch integrity-locked developer tools')
    parser.add_argument('--check', action='store_true', help='compare the rebuilt package to the existing output')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/plugins/ai-claude')
    parser.add_argument('--tools', type=Path, help='explicit preinstalled locked developer tools; skip npm installation')
    args = parser.parse_args()
    output = args.out.resolve()
    if output == SOURCE or SOURCE in output.parents or output in SOURCE.parents:
        raise SystemExit('output must be separate from Claude source')
    if output.exists() and not args.check:
        try:
            identity = json.loads((output / 'plugin.json').read_text())
            if identity.get('id') != 'org.ghosterm.ai-claude' or not (output / 'integrity.json').is_file():
                raise ValueError('unrecognized output')
        except (OSError, ValueError):
            raise SystemExit('refusing to replace a directory without the Claude package identity')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='claude-package-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        env = dict(os.environ, TMPDIR=str(work))
        tools = args.tools.resolve(strict=True) if args.tools else work / 'tools'
        if args.tools:
            for name in ('package.json', 'package-lock.json'):
                if (tools / name).read_bytes() != (ROOT / 'scripts/fixtures/plugin-sdk-tools' / name).read_bytes():
                    raise SystemExit('selected developer tools do not match the locked manifest')
            observed = subprocess.check_output(['node', '-p', 'require(process.argv[1]).version',
                str(tools / 'node_modules/typescript/package.json')], env=env, text=True).strip()
            if observed != '5.9.3': raise SystemExit('selected TypeScript version differs from lock')
        else:
            tools.mkdir()
            for name in ('package.json', 'package-lock.json'):
                shutil.copyfile(ROOT / 'scripts/fixtures/plugin-sdk-tools' / name, tools / name)
            install = ['npm', 'ci', '--prefix', tools, '--ignore-scripts', '--no-audit', '--no-fund',
                       '--cache', ROOT / 'tmp/plugin-claude-npm-cache', '--registry', 'https://registry.npmjs.org']
            if not args.fetch:
                install.append('--offline')
            run(install, env)
        emitted = work / 'emitted'
        run(['node', tools / 'node_modules/typescript/bin/tsc', '-p', SOURCE / 'tsconfig.json',
             '--noEmit', 'false', '--rewriteRelativeImportExtensions', 'true', '--rootDir', ROOT,
             '--outDir', emitted, '--noEmitOnError', 'true'], env)
        stage = work / 'package'
        stage.mkdir()
        emitted_plugin = emitted / SOURCE.relative_to(ROOT)
        for source in emitted_plugin.rglob('*.js'):
            target = stage / source.relative_to(emitted_plugin)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        for name in ('plugin.json', 'schemas/command.json'):
            target = stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / name, target)
        shutil.copyfile(SOURCE / 'PACKAGE.md', stage / 'README.md')
        (stage / 'LICENSES').mkdir()
        shutil.copyfile(ROOT / 'LICENSE', stage / 'LICENSES/MIT.txt')
        members = files(stage)
        scripts = {name for name in members if name.endswith('.js')}
        if not {'client/index.js', 'executor/index.js'} <= scripts or len(scripts) > 64:
            raise SystemExit('invalid compiled entry/module set')
        for name in scripts:
            if len(members[name]) > 131072:
                raise SystemExit(f'compiled module exceeds Runtime limit: {name}')
            if re.search(r'\bimport\s*\(', members[name].decode()):
                raise SystemExit(f'dynamic imports are not supported by the Portable Runtime: {name}')
            for reference in re.findall(r'(?:from\s+|import\s*)[\"\']([^\"\']+)[\"\']', members[name].decode()):
                if reference == '@ghosterm/plugin-sdk':
                    continue
                target = (stage / name).parent.joinpath(reference).resolve()
                if not reference.startswith('.') or not target.is_relative_to(stage) or not target.is_file():
                    raise SystemExit(f'compiled import escapes or is missing: {name} -> {reference}')
        index = {'schemaVersion': 1, 'members': [
            {'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            for name, data in sorted(members.items())]}
        (stage / 'integrity.json').write_text(json.dumps(index, ensure_ascii=False, separators=(',', ':')) + '\n')
        run(['node', ROOT / 'scripts/check-plugin-claude-package.mjs', tools, stage], env)
        env['GHOSTERM_TEST_PLUGIN_SDK_ENTRY'] = str(emitted / 'packages/plugin-sdk/portable/index.js')
        env['GHOSTERM_CLAUDE_PACKAGE_ENTRY'] = str(stage / 'executor/index.js')
        run(['node', '--test', '--experimental-loader',
             ROOT / 'scripts/plugin-sdk-test-loader.mjs', ROOT / 'scripts/test-plugin-claude-entry.mjs'], env)
        if args.check:
            if not output.is_dir() or files(output) != files(stage):
                raise SystemExit('Claude package output is missing or stale; rebuild without --check')
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            backup = work / 'previous'
            if output.exists():
                output.rename(backup)
            try:
                stage.rename(output)
            except BaseException:
                if backup.exists():
                    backup.rename(output)
                raise
        digest = hashlib.sha256((output / 'integrity.json').read_bytes()).hexdigest()
        print(f'CLAUDE PACKAGE {"CHECK" if args.check else "BUILD"} PASS: {output}; {digest}')


if __name__ == '__main__':
    main()
