#!/usr/bin/env python3
"""Exercise channel publication with disposable, isolated in-memory signers."""
import base64
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('release_fixture', ROOT / 'scripts/test-plugin-release.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    node = Path(shutil.which('node')).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='pcs-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        root_dir, publisher_dir = work / 'root', work / 'pub'
        root_dir.mkdir()
        publisher_dir.mkdir()
        env = dict(os.environ, TMPDIR=str(work))
        with helper.agent(node, root_dir) as (root, root_endpoint), helper.agent(node, publisher_dir) as (publisher, publisher_endpoint):
            def cli(arguments, expected=None):
                result = subprocess.run([sys.executable, '-B', ROOT / 'scripts/gterm-plugin', *map(str, arguments)],
                                        cwd=ROOT, env=env, capture_output=True, timeout=45)
                report = json.loads(result.stdout)
                if expected is None:
                    assert result.returncode == 0, (report, result.stderr)
                else:
                    assert result.returncode != 0 and report.get('code') == expected, (report, result.stderr)
                return report

            def signer_config(name, endpoint):
                path = work / name
                path.write_text(json.dumps(dict(executable=str(node), sha256=helper.sha(node.read_bytes()),
                    prefixArguments=[str(ROOT / 'scripts/fixtures/plugin-release-agent.mjs'), 'client', str(endpoint)])))
                return path

            signer = signer_config('root.json', root_endpoint)
            wrong_signer = signer_config('publisher.json', publisher_endpoint)
            policy = work / 'policy-input.json'
            policy.write_text(json.dumps(dict(schemaVersion=1, channel='fixture',
                keys=[dict(id=helper.sha(base64.b64decode(publisher)), algorithm='ed25519',
                           publicKey=publisher, publisher='Ghosterm', pluginIds=['org.ghosterm.preview'])],
                revokedKeyIds=[]), indent=2) + '\n')
            now = int(time.time())

            def sign(destination, *, generation='2', expires=now + 600, key=root, config=signer, expected=None):
                return cli(['release', 'sign-policy', '--channel', 'fixture', '--channel-root', key,
                            '--policy', policy, '--generation', generation, '--issued-at', now - 5,
                            '--expires-at', expires, '--signer-json', config, '--out', destination], expected)

            output = work / 'signed'
            signed = sign(output)
            assert signed['kind'] == 'signed-channel-policy'
            assert {p.name for p in output.iterdir()} == {'channel-policy.json', 'channel-policy.sig', 'policy.json'}
            assert (output / 'policy.json').read_bytes() == policy.read_bytes()
            def verify(bundle, expected=None):
                return cli(['release', 'verify-policy', '--channel', 'fixture', '--channel-root', root,
                            '--statement', bundle / 'channel-policy.json', '--signature', bundle / 'channel-policy.sig',
                            '--policy', bundle / 'policy.json', '--record-state', work / 'channel.sqlite'], expected)

            assert verify(output)['statementSHA256'] == signed['statementSHA256']
            sign(output, expected='OUTPUT_EXISTS')
            assert verify(output)['statementSHA256'] == signed['statementSHA256']

            rejected = work / 'rejected'
            sign(rejected, config=wrong_signer, expected='RELEASE_SIGNATURE')
            sign(rejected, key=publisher, expected='CHANNEL_ROOT_IS_PUBLISHER')
            sign(rejected, generation='02', expected='CHANNEL_POLICY_INTEGER')
            sign(rejected, expires=now, expected='CHANNEL_POLICY_FRESHNESS')
            assert not rejected.exists()
            dual_use = work / 'dual-use'
            dual_use.mkdir()
            overlap = json.loads(policy.read_bytes())
            overlap['keys'][0].update(id=helper.sha(base64.b64decode(root)), publicKey=root)
            shared_policy = json.dumps(overlap).encode()
            statement = json.loads((output / 'channel-policy.json').read_bytes())
            statement['policySHA256'] = helper.sha(shared_policy)
            raw = json.dumps(statement).encode()
            signed_overlap = subprocess.run([node, ROOT / 'scripts/fixtures/plugin-release-agent.mjs', 'client', root_endpoint],
                input=b'ghosterm.channel-policy.v1\0' + raw, capture_output=True, check=True, env=env, timeout=15).stdout
            (dual_use / 'policy.json').write_bytes(shared_policy)
            (dual_use / 'channel-policy.json').write_bytes(raw)
            (dual_use / 'channel-policy.sig').write_bytes(signed_overlap)
            verify(dual_use, expected='CHANNEL_ROOT_IS_PUBLISHER')
            old = work / 'old'
            sign(old, generation='1')
            verify(old, expected='CHANNEL_POLICY_ROLLBACK')

    print('CHANNEL POLICY SIGN PASS')


if __name__ == '__main__':
    main()
