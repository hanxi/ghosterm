#!/usr/bin/env python3
"""Exercise channel publication with disposable, isolated in-memory signers."""
import base64
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from fixtures.plugin_release_signer import memory_signer, sha, sign as sign_bytes, signer_config

ROOT = Path(__file__).resolve().parents[1]


def main():
    sys.dont_write_bytecode = True
    project_tmp = (ROOT / 'tmp').resolve()
    requested_tmp = Path(os.environ.get('TMPDIR', project_tmp)).resolve()
    work_tmp = requested_tmp if requested_tmp.is_dir() and requested_tmp.is_relative_to(project_tmp) else project_tmp
    with tempfile.TemporaryDirectory(prefix='pcs-', dir=work_tmp) as temporary:
        work = Path(temporary)
        root_dir, publisher_dir = work / 'root', work / 'pub'
        root_dir.mkdir()
        publisher_dir.mkdir()
        env = dict(os.environ, TMPDIR=temporary, TMP=temporary, TEMP=temporary)
        with memory_signer(root_dir) as (root, root_endpoint), memory_signer(publisher_dir) as (publisher, publisher_endpoint):
            def cli(arguments, expected=None):
                result = subprocess.run([sys.executable, '-B', ROOT / 'scripts/gterm-plugin', *map(str, arguments)],
                                        cwd=ROOT, env=env, capture_output=True, timeout=45)
                report = json.loads(result.stdout)
                if expected is None:
                    assert result.returncode == 0, (report, result.stderr)
                elif isinstance(expected, tuple):
                    outcome = report.get('code') if result.returncode else None
                    assert outcome in expected, (report, result.stderr)
                else:
                    assert result.returncode != 0 and report.get('code') == expected, (report, result.stderr)
                return report

            signer = work / 'root.json'
            signer.write_text(json.dumps(signer_config(root_endpoint)))
            wrong_signer = work / 'publisher.json'
            wrong_signer.write_text(json.dumps(signer_config(publisher_endpoint)))
            policy = work / 'policy-input.json'
            policy.write_text(json.dumps(dict(schemaVersion=1, channel='fixture',
                keys=[dict(id=sha(base64.b64decode(publisher)), algorithm='ed25519',
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
            overlap['keys'][0].update(id=sha(base64.b64decode(root)), publicKey=root)
            shared_policy = json.dumps(overlap).encode()
            statement = json.loads((output / 'channel-policy.json').read_bytes())
            statement['policySHA256'] = sha(shared_policy)
            raw = json.dumps(statement).encode()
            signed_overlap = sign_bytes(root_endpoint, b'ghosterm.channel-policy.v1\0' + raw)
            (dual_use / 'policy.json').write_bytes(shared_policy)
            (dual_use / 'channel-policy.json').write_bytes(raw)
            (dual_use / 'channel-policy.sig').write_bytes(signed_overlap)
            verify(dual_use, expected='CHANNEL_ROOT_IS_PUBLISHER')
            old = work / 'old'
            sign(old, generation='1')
            verify(old, expected='CHANNEL_POLICY_ROLLBACK')

            revoked = json.loads(policy.read_bytes())
            revoked['revokedKeyIds'] = [revoked['keys'][0]['id']]
            policy.write_text(json.dumps(revoked))
            fork = work / 'fork'
            sign(fork)
            verify(fork, expected='CHANNEL_POLICY_FORK')
            assert verify(output)['statementSHA256'] == signed['statementSHA256']
            next_generation = work / 'revocation'
            signed_revocation = sign(next_generation, generation='3')
            receipt = verify(next_generation)
            assert receipt['generation'] == '3'
            assert receipt['statementSHA256'] == signed_revocation['statementSHA256']
            verify(output, expected='CHANNEL_POLICY_ROLLBACK')
            assert verify(next_generation) == receipt

            first, second = work / 'concurrent-a', work / 'concurrent-b'
            sign(first, generation='4')
            sign(second, generation='4', expires=now + 601)
            with ThreadPoolExecutor(max_workers=2) as writers:
                pending = [writers.submit(verify, candidate, (None, 'CHANNEL_POLICY_FORK'))
                           for candidate in (first, second)]
                outcomes = [future.result() for future in pending]
            assert sum(outcome.get('code') == 'CHANNEL_POLICY_FORK' for outcome in outcomes) == 1, outcomes
            for candidate, outcome in zip((first, second), outcomes):
                if outcome.get('code') == 'CHANNEL_POLICY_FORK':
                    verify(candidate, expected='CHANNEL_POLICY_FORK')
                else:
                    assert verify(candidate) == outcome

    print('CHANNEL POLICY SIGN PASS')


if __name__ == '__main__':
    main()
