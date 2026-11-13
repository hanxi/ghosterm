#!/usr/bin/env python3
"""Native HOST -> gterm -> authorized Node -> real pinned CLI, no model request."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--binary', type=Path, help='explicit alternative gterm executable for isolated mutations')
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    if os.uname().sysname != 'Darwin': parser.error('requires the macOS no-network test profile')
    build = args.build.resolve()
    binary = args.binary.resolve() if args.binary else build / 'gterm'
    with tempfile.TemporaryDirectory(prefix='gterm-node-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        node = work / 'node'
        shutil.copytree(ROOT / 'plugins/official/ai-claude/node', node)
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
        install = ['npm', 'ci', '--prefix', node, '--ignore-scripts', '--no-audit', '--no-fund',
                   '--cache', ROOT / 'tmp/claude-npm-cache', '--registry', 'https://registry.npmjs.org']
        if not args.fetch: install.append('--offline')
        subprocess.run(list(map(str, install)), env=env, check=True, timeout=300, capture_output=True)
        subprocess.run(['node', str(node / 'node_modules/typescript/bin/tsc'), '-p', str(node / 'tsconfig.json'), '--noEmitOnError'],
                       env=env, check=True, timeout=120)
        machine = 'arm64' if os.uname().machine == 'arm64' else 'x64'
        cli = node / f'node_modules/@anthropic-ai/claude-agent-sdk-darwin-{machine}/claude'
        package = work / 'package'; package.mkdir()
        manifest = dict(schemaVersion=1, id='org.ghosterm.fixture', pluginVersion='0.1.0', sdkApiRange='^1.0.0',
                        bridgeProtocolRange=[1], publisher='fixture', license='MIT',
                        commands=[dict(path=['fixture'], title='Fixture', argsSchema='schemas/input.json')],
                        executor=dict(variants=[dict(id='portable', runtime='portable-js', entry='executor/index.js',
                                                    requires=['session.rpc', 'process.spawn', 'process.pipes'])]),
                        permissions=[dict(side='executor', capability='process.spawn', programs=['cat'],
                                          purpose='Run the explicitly mapped Node fixture.', scope='instance')])
        names = ['first', 'empty', 'literal']
        schema = dict(type='object', properties={name: dict(type='string', maxLength=100) for name in names},
                      required=names, additionalProperties=False)
        schema['x-ghosterm-cli'] = dict(version=1, fields={name: dict(position=i) for i, name in enumerate(names)})
        files = {'plugin.json': json.dumps(manifest), 'schemas/input.json': json.dumps(schema),
                 'README': 'Owned no-model gterm Node conformance fixture.\n', 'LICENSES/MIT.txt': (ROOT / 'LICENSE').read_text(),
                 'executor/index.js': (ROOT / 'scripts/fixtures/gterm-node/executor.js').read_text()}
        members = []
        for name, text in files.items():
            target = package / name; target.parent.mkdir(parents=True, exist_ok=True); target.write_text(text)
            data = target.read_bytes(); members.append(dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        index = package / 'integrity.json'; index.write_text(json.dumps(dict(schemaVersion=1, members=members)))
        digest = hashlib.sha256(index.read_bytes()).hexdigest()
        for name in ['home', 'config', 'workspace']: (work / name).mkdir()
        environment = {key: str(work / name) for key, name in [('HOME', 'home'), ('CLAUDE_CONFIG_DIR', 'config')]}
        environment.update(PATH=os.environ['PATH'], TMPDIR=str(ROOT / 'tmp'), CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1',
                           GT_GTERM_NODE_EXTRA='explicitly-selected-fixture')
        profile = work / 'profile.json'
        profile.write_text(json.dumps(dict(version=1, cwd=str(work / 'workspace'), executable=str(cli), cliVersion='2.1.323',
                                          sha256=hashlib.sha256(cli.read_bytes()).hexdigest(),
                                          environmentNames=list(environment), tools=[])))
        node_binary = shutil.which('node')
        version = subprocess.check_output([node_binary, '--version'], text=True, env=environment).strip()
        node_digest = hashlib.sha256(Path(node_binary).read_bytes()).hexdigest()
        entry = work / 'node-entry.mjs'
        entry.write_text('if(process.env.GT_GTERM_NODE_EXTRA!=="explicitly-selected-fixture"||'
                         'Object.hasOwn(process.env,"GT_GTERM_NODE_PARENT_SECRET"))throw Error("fixture environment isolation");'
                         'await import(' + json.dumps((node / 'dist/cli.js').as_uri()) + ');\n')
        environment['GT_GTERM_NODE_PARENT_SECRET'] = 'unselected-fixture-value'
        command = ['/usr/bin/sandbox-exec', '-p', '(version 1)(allow default)(deny network*)(deny mach-lookup (global-name "com.apple.securityd"))',
                   build / 'test_gterm_stdio', binary, package, digest, 'process-prefix', work / 'workspace',
                   node_binary, version, node_digest, entry, profile]
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, env=environment, timeout=60)
        if result.returncode or 'PASS stdio process-prefix' not in result.stdout:
            raise RuntimeError(f'gterm Node integration failed ({result.returncode}):\n{result.stdout}\n{result.stderr}')
        print(result.stdout, end='')
        base = [binary, 'relay', '--stdio', '--trust-development-digest', digest, '--client-digest', 'd' * 64,
                '--root', 'workspace', work / 'workspace', '--process-program', 'cat', node_binary, version, node_digest]
        negatives = [(['--process-argument', 'unmapped', 'value'], 2, 'fixed argument requires an earlier mapping'),
                     (['--process-argument', 'cat', 'value'] * 17, 2, 'fixed argument requires an earlier mapping'),
                     (['--process-environment', 'BAD-NAME'], 2, 'invalid selected environment name'),
                     (['--process-environment', 'GT_GTERM_MISSING_SELECTED_ENV'], 4, 'explicitly selected process environment is unavailable'),
                     (['--process-environment', 'HOME'], 2, 'duplicate selected process environment name')]
        for options, code, message in negatives:
            result = subprocess.run(list(map(str, [*base, *options, '--', package, 'fixture'])), input=b'',
                                    capture_output=True, env=environment, timeout=15)
            if result.returncode != code or message.encode() not in result.stderr or result.stdout:
                raise RuntimeError(f'gterm launch policy rejection failed: {code} / {message}; actual {result.returncode}')
        print('PASS gterm launch policy: five explicit input/environment rejections')
        print('GTERM NODE PASS: native Session consent / fixed Host arguments / real CLI initialize / stop / reap; no model request')


if __name__ == '__main__':
    main()
