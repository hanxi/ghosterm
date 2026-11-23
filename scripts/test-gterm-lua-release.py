#!/usr/bin/env python3
"""Install a signed Lua Preview and inspect its ordinary foreground command."""
import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from fixtures.plugin_release_signer import memory_signer, sha, signer_config

ROOT = Path(__file__).resolve().parents[1]
OFFICIAL = ('preview', 'table', 'diff', 'task', 'views', 'logs', 'form', 'ai-codex', 'ai-claude')


def signed_fixtures(build, work):
    policy, signer = work / 'policy.json', work / 'publisher.json'
    receipts = {}
    with memory_signer(work) as (public, endpoint):
        key = sha(base64.b64decode(public))
        policy.write_text(json.dumps(dict(schemaVersion=1, channel='fixture', keys=[
            dict(id=key, algorithm='ed25519', publicKey=public, publisher='Ghosterm',
                 pluginIds=[f'org.ghosterm.{name}' for name in OFFICIAL])], revokedKeyIds=[])))
        signer.write_text(json.dumps(signer_config(endpoint)))
        for name in OFFICIAL:
            archive = work / f'{name}.zip'
            release = work / f'release-{name}'
            build('pack', ROOT / 'plugins' / 'official' / name, '--out', archive)
            receipts[name] = json.loads(build('release', 'sign', '--artifact', archive, '--policy',
                                               policy, '--key-id', key, '--signer-json', signer, '--out', release))

    root_dir = work / 'root'
    root_dir.mkdir()
    channel = work / 'channel'
    with memory_signer(root_dir) as (root, endpoint):
        signer.write_text(json.dumps(signer_config(endpoint)))
        now = int(time.time())
        build('release', 'sign-policy', '--channel', 'fixture', '--channel-root', root,
              '--policy', policy, '--generation', '1', '--issued-at', str(now - 5),
              '--expires-at', str(now + 600), '--signer-json', signer, '--out', channel)
    for name, signed in receipts.items():
        release = work / f'release-{name}'
        verify = ('release', 'verify', '--descriptor', release / 'release.json',
                  '--signature', release / 'release.sig', '--policy', channel / 'policy.json',
                  '--channel', 'fixture', '--channel-root', root, '--policy-statement', channel / 'channel-policy.json',
                  '--policy-signature', channel / 'channel-policy.sig', '--record-state', work / 'channel.sqlite')
        artifact = release / 'plugin.zip'
        checked = json.loads(build(*verify, '--artifact', artifact))
        assert checked['packageSHA256'] == signed['packageSHA256']
        assert checked['pluginId'] == f'org.ghosterm.{name}'
        assert checked['trustSource'] == 'verified-channel-policy' and checked['publisherAuthenticated']
        tampered = work / 'tampered.zip'
        tampered.write_bytes(artifact.read_bytes() + b'changed')
        rejected = json.loads(build(*verify, '--artifact', tampered, expected=6))
        assert rejected['code'] == 'RELEASE_ARTIFACT_MISMATCH'
    print(f'LUA SIGNED RELEASES PASS: {len(receipts)} official plugins')
    return policy, work / 'release-preview', receipts['preview']


def exercise(gterm, client, work):
    env = dict(os.environ, TMPDIR=str(work), TMP=str(work), TEMP=str(work))

    def run(*args, expected=0, extra_env=env):
        result = subprocess.run(args, env=extra_env, input=b"", capture_output=True, timeout=15)
        assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
        return result.stdout

    def build(*args, expected=0):
        return run(sys.executable, '-B', ROOT / 'scripts/gterm-plugin', *args, expected=expected)

    policy, release, signed = signed_fixtures(build, work)

    host = work / 'host.json'
    host.write_text(json.dumps(dict(side='executor', sdkVersion='1.0.0', protocols=[1],
        runtimes=['lua'], capabilities=['session.rpc', 'resource.receive', 'file.readScoped'])))
    catalog, socket_dir, workspace, store = (work / name for name in ('catalog', 'ipc', 'workspace', 'store'))
    for directory in (catalog, socket_dir, workspace):
        directory.mkdir(mode=0o700)
    (workspace / 'file.bin').write_bytes(b'abc')
    digest = signed['packageSHA256']
    assert json.loads(run(gterm, 'doctor'))['lua'] == 'available'
    for retired in ('node', 'exec', 'portable-js'):
        run(gterm, 'relay', '--stdio', '--runtime', retired, '--trust-development-digest', digest,
            '--client-digest', digest, '--', workspace, 'preview', './file.bin', expected=4)
    trusted = policy.read_bytes()
    try:
        installed = json.loads(run(gterm, 'plugin', 'install', '--catalog', catalog,
            '--expect-revision', '0', '--store', store, '--artifact', release / 'plugin.zip',
            '--descriptor', release / 'release.json', '--signature', release / 'release.sig',
            '--policy', policy, '--host-json', host, '--', '--client-digest', digest,
            '--method', 'preview.open', '--root', 'workspace', workspace))
        assert installed['result'] == 'installed-and-registered' and not installed['authorizationGranted']

        launch_env = dict(env, GTERM_CATALOG=str(catalog), GTERM_ENDPOINT=str(socket_dir))
        arguments = ('preview', './file.bin')
        policy.write_bytes(trusted + b'\n')
        run(gterm, *arguments, expected=9, extra_env=launch_env)
        assert not list(socket_dir.iterdir())
        policy.write_bytes(trusted)
        with subprocess.Popen([gterm, *arguments], env=launch_env, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as frontend:
            try:
                permit = socket_dir / 'permit'
                deadline = time.monotonic() + 8
                while not (permit.is_file() and permit.stat().st_size == 56):
                    assert frontend.poll() is None, frontend.communicate()
                    assert time.monotonic() < deadline, 'foreground permit never appeared'
                    time.sleep(0.025)
                instance = permit.read_bytes()[8:24].hex()
                declaration = json.loads(run(gterm, 'relay', '--inspect', socket_dir,
                                            '--instance', instance, extra_env=launch_env))
                assert declaration['runtime'] == 'lua' and declaration['command'] == 'preview'
                assert declaration['arguments'] == ['./file.bin'] and declaration['methods'] == ['preview.open']
                assert declaration['executorDigest'] == declaration['clientDigest'] == digest
                assert not declaration['authorizationGranted']
                policy.write_bytes(trusted + b'\n')
                attached = subprocess.run([gterm, 'relay', '--attach', socket_dir, '--instance',
                    instance, '--', *arguments], input=b'', env=launch_env, capture_output=True, timeout=10)
                assert not attached.stdout and frontend.wait(timeout=5) == 9
                assert not list(socket_dir.iterdir())
            finally:
                if frontend.poll() is None:
                    frontend.terminate()
                    try:
                        frontend.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        frontend.kill()
                        frontend.wait()
        policy.write_bytes(trusted)
        with subprocess.Popen([gterm, *arguments], env=launch_env, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as frontend:
            try:
                permit = socket_dir / 'permit'
                deadline = time.monotonic() + 8
                while not (permit.is_file() and permit.stat().st_size == 56):
                    assert frontend.poll() is None, frontend.communicate()
                    assert time.monotonic() < deadline, 'foreground permit never appeared'
                    time.sleep(0.025)
                saved_permit = permit.read_bytes()
                instance = saved_permit[8:24].hex()
                wrong_instance = ('1' if instance[0] == '0' else '0') + instance[1:]
                mismatches = (
                    ('instance', wrong_instance, arguments),
                    ('command', instance, ('table', arguments[1])),
                    ('argument', instance, (arguments[0], './other.bin')),
                )
                for label, selected, invocation in mismatches:
                    rejected = subprocess.run(
                        [gterm, 'relay', '--attach', socket_dir, '--instance', selected, '--', *invocation],
                        env=launch_env, input=b'', capture_output=True, timeout=8)
                    assert rejected.returncode == 5 and not rejected.stdout, (label, rejected.stderr)
                    assert frontend.poll() is None and permit.read_bytes() == saved_permit, label
                    print(f'PASS Lua foreground rejects {label} mismatch and preserves its permit')
                left, right = socket.socketpair()
                with left, right, subprocess.Popen(
                    [gterm, 'relay', '--attach', socket_dir, '--instance', instance, '--', *arguments],
                    stdin=left, stdout=left, stderr=subprocess.PIPE, env=launch_env,
                ) as relay:
                    left.close()
                    result = subprocess.run([client, '--attached-client', ROOT / 'plugins/official/preview/main.lua',
                                             digest, work], stdin=right, stdout=right, stderr=subprocess.PIPE,
                                            env=env, timeout=15)
                    right.close()
                    assert result.returncode == 0, result.stderr
                    assert relay.wait(timeout=5) == 0, relay.stderr.read()
                assert frontend.wait(timeout=5) == 0, frontend.stderr.read()
                assert not list(socket_dir.iterdir())
            finally:
                if frontend.poll() is None:
                    frontend.terminate()
                    frontend.wait(timeout=5)
    finally:
        for directory, _, _ in os.walk(store):
            os.chmod(directory, 0o700)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('gterm', type=Path)
    parser.add_argument('client', type=Path, help='Lua Core test_lua_preview binary')
    parser.add_argument('--export-official-fixtures', type=Path,
                        help='Write nine public signed test releases to a new project tmp path')
    args = parser.parse_args()
    workspace_tmp = ROOT / 'tmp'
    export = args.export_official_fixtures
    if export is not None:
        if export.is_symlink() or export.exists():
            parser.error('Fixture export requires a new directory')
        export = export.resolve()
        if not export.is_relative_to(workspace_tmp) or not export.parent.is_dir():
            parser.error('Fixture export requires an existing project tmp parent')
    parent = Path(os.environ.get('TMPDIR', str(workspace_tmp))).resolve()
    if not parent.is_relative_to(workspace_tmp) or not parent.is_dir():
        parent = workspace_tmp
    with tempfile.TemporaryDirectory(prefix='agent-', dir=parent) as directory:
        work = Path(directory)
        exercise(args.gterm.resolve(strict=True), args.client.resolve(strict=True), work)
        if export is not None:
            stage = work / 'public-fixtures'
            stage.mkdir()
            for name in OFFICIAL:
                public_release = stage / f'official-{name}-release'
                public_release.mkdir()
                for filename in ('plugin.zip', 'release.json', 'release.sig'):
                    shutil.copyfile(work / f'release-{name}' / filename, public_release / filename)
                shutil.copyfile(work / 'policy.json', stage / f'official-{name}-policy.json')
            stage.rename(export)
    assert not Path(directory).exists()
    print('LUA SIGNED FOREGROUND PASS')


if __name__ == '__main__':
    main()
