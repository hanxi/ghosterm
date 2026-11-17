#!/usr/bin/env python3
"""Native signed Executor install/ordinary command smoke; owned local fixtures.

Private signing keys stay in the existing ephemeral in-memory signer. No user
catalogs, hosts, terminals, model calls or GUI windows are used.
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
import platform
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


ipc = module('gterm_release_ipc', 'test-gterm-ipc.py')
release = module('gterm_release_fixture', 'test-plugin-release.py')
CASES = ['roundtrip', 'deflate', 'bad-signature', 'revoked', 'scope', 'duplicate-policy',
         'fractional', 'wrong-artifact', 'wrong-package', 'zip-path', 'zip-tail',
         'conflict', 'partial-register', 'changed-policy', 'waiting-policy', 'tampered-payload',
         'store-link', 'umask']
CASES += ['unicode-publisher', 'nfd-policy', 'base64-policy', 'node-roundtrip', 'node-deny',
          'node-active-cancel', 'exec-roundtrip', 'exec-deny', 'exec-active-cancel',
          'node-development', 'exec-development']
CASES += ['node-multi', 'exec-multi', 'profile-runtime-mismatch', 'profile-variant-mismatch', 'policy-review']


def require(value, label):
    if not value:
        raise AssertionError(label)



def policy_workflow(run, work, catalog, policy, trusted, key_id, installed_version, package, host, profile):
    catalog_file = catalog / 'executor-catalog-v1'
    original_catalog = catalog_file.read_bytes()
    original_policy = policy.read_bytes()
    def review(selected=policy):
        result = run(['plugin', 'review-policy', '--catalog', catalog, '--policy', selected])
        require(not result['authorizationGranted'] and not result['publisherAuthenticated'], 'review is not a grant')
        return result
    def apply(observation, expected=0, selected=policy):
        return run(['plugin', 'disable-denied', '--catalog', catalog, '--policy', selected,
                    '--expect-revision', observation['revision'], '--expect-policy-digest', observation['policyDigest'],
                    '--review-digest', observation['reviewDigest']], expected)
    healthy = review()
    require(healthy['entries'][0]['state'] == 'allowed' and healthy['entries'][0]['policyBindingCurrent'] and
            healthy['deniedCount'] == 0, 'current policy validates actual package')
    require(review()['reviewDigest'] == healthy['reviewDigest'], 'stable observation digest')
    unchanged = apply(healthy)
    require(unchanged['result'] == 'unchanged' and not unchanged['committed'] and not unchanged['durabilityConfirmed'] and unchanged['revision'] == '1' and
            catalog_file.read_bytes() == original_catalog, 'no denial does not rewrite catalog')
    alternate = work / 'alternate-policy.json'; alternate.write_bytes(original_policy)
    other = review(alternate)
    require(other['entries'][0]['state'] == 'other-policy' and not other['deniedCount'], 'other policy path is untouched')
    apply(other, selected=alternate)
    policy.write_bytes(original_policy + b'\n')
    apply(healthy, 9)
    changed = review()
    require(changed['entries'][0]['state'] == 'allowed' and not changed['entries'][0]['policyBindingCurrent'],
            'allowed current evidence does not silently migrate a pinned binding')
    apply(changed)
    require(catalog_file.read_bytes() == original_catalog, 'valid policy change needs explicit reregistration')
    policy.write_text('{invalid')
    invalid = review()
    require(invalid['entries'][0]['state'] == 'invalid-evidence' and not invalid['deniedCount'], 'malformed policy is not a denial')
    apply(invalid)
    policy.write_bytes(original_policy)
    installed_signature = installed_version / 'release.sig'
    original_signature = installed_signature.read_bytes()
    installed_version.chmod(0o700)
    installed_signature.unlink(); installed_signature.symlink_to(work / 'release.sig')
    missing = review()
    require(missing['entries'][0]['state'] == 'evidence-unavailable' and not missing['deniedCount'], 'unreadable signature is not a denial')
    apply(missing)
    installed_signature.unlink(); installed_signature.write_bytes(original_signature); installed_signature.chmod(0o400)
    installed_version.chmod(0o500)
    trusted['revokedKeyIds'] = [key_id]; policy.write_text(json.dumps(trusted))
    revoked = review()
    require(revoked['deniedCount'] == 1 and revoked['entries'][0]['eligibleToDisable'], 'confirmed revocation is eligible')
    # A different invalid signature remains denied, but must invalidate the exact
    # reviewed material before any catalog update.
    installed_signature.chmod(0o600)
    installed_signature.write_bytes(bytes([original_signature[0] ^ 1]) + original_signature[1:]); installed_signature.chmod(0o400)
    apply(revoked, 9)
    require(catalog_file.read_bytes() == original_catalog, 'changed evidence cannot reuse a review token')
    installed_signature.chmod(0o600); installed_signature.write_bytes(original_signature); installed_signature.chmod(0o400)
    local_root = work / 'development'; local_root.mkdir()
    local, _ = ipc.package(local_root)
    manifest = json.loads((local / 'plugin.json').read_text()); manifest['id'] = 'org.ghosterm.local-fixture'
    (local / 'plugin.json').write_text(json.dumps(manifest))
    integrity = json.loads((local / 'integrity.json').read_text())
    for item in integrity['members']:
        member = local / item['path']; item.update(bytes=member.stat().st_size, sha256=release.sha(member.read_bytes()))
    (local / 'integrity.json').write_text(json.dumps(integrity))
    run(['plugin', 'register-development', '--catalog', catalog, '--expect-revision', '1',
         '--host-json', host, '--trust-development-digest', release.sha((local / 'integrity.json').read_bytes()), '--', local, *profile])
    apply(revoked, 9)
    mixed = review()
    require(mixed['revision'] == '2' and mixed['deniedCount'] == 1 and
            {item['state'] for item in mixed['entries']} == {'development', 'denied'}, 'mixed source review')
    before = run(['plugin', 'list', '--catalog', catalog])['entries'][-1]
    applied = apply(mixed)
    require(applied['disabledCount'] == 1 and applied['committed'] and applied['durabilityConfirmed'] and applied['revision'] == '3',
            'one durable catalog transaction removes exactly confirmed denied selection')
    after = run(['plugin', 'list', '--catalog', catalog])
    require(after['entries'] == [before] and installed_version.is_dir() and package.is_dir(), 'other records and all installed bytes retained')
    apply(mixed, 9)
    final = review()
    require(not final['deniedCount'] and apply(final)['result'] == 'unchanged', 'explicit second review has no replay')
    print('PASS policy workflow: allowed/current and stale bindings, other policy, invalid/unavailable, evidence/policy/catalog CAS, mixed atomic disable', flush=True)


def exercise(build, node, sdk, case, work):
    package, digest = ipc.package(work)
    manifest = json.loads((package / 'plugin.json').read_text())
    runtime = 'node' if case == 'profile-variant-mismatch' else case.split('-')[0] if case.startswith(('node-', 'exec-')) else 'portable-js'
    version = subprocess.check_output([node, '--version'], text=True).strip()[1:]
    if runtime != 'portable-js':
        variant = dict(id='selected', runtime=runtime, requires=['session.rpc'])
        if runtime == 'node':
            require(sdk is not None, 'Node ordinary command requires explicit trusted Host SDK')
            variant.update(entry='executor/main.mjs', runtimeVersionRange='>=20.0.0 <1000.0.0')
            (package / 'executor/index.js').rename(package / 'executor/main.mjs')
        else:
            import shutil
            variant.update(executable='executor/main', os=[{'Darwin':'macos','Linux':'linux'}[platform.system()]],
                           arch=[{'arm64':'aarch64','aarch64':'aarch64','x86_64':'x86_64'}[platform.machine()]])
            (package / 'executor/index.js').unlink()
            shutil.copyfile(build / 'test_gterm_exec_peer', package / 'executor/main')
            (package / 'executor/main').chmod(0o755)
        manifest['executor']['variants'] = [variant]
        if case.endswith('-multi'):
            manifest['executor']['variants'].insert(0, dict(id='unselected', runtime='portable-js',
                entry='executor/unselected.js', requires=['session.rpc']))
            (package / 'executor/unselected.js').write_text("throw Error('UNSELECTED_VARIANT_ACTIVATED');")
    if case == 'unicode-publisher': manifest['publisher'] = '作者 café'
    if runtime != 'portable-js' or case == 'unicode-publisher':
        (package / 'plugin.json').write_text(json.dumps(manifest))
        members = [dict(path=p.relative_to(package).as_posix(), bytes=p.stat().st_size,
                   sha256=release.sha(p.read_bytes())) for p in sorted(package.rglob('*'))
                   if p.is_file() and p.name != 'integrity.json']
        (package / 'integrity.json').write_text(json.dumps(dict(schemaVersion=1, members=members)))
        digest = release.sha((package / 'integrity.json').read_bytes())
    catalog = work / 'catalog'; catalog.mkdir(mode=0o700)
    endpoint = work / 'e'; endpoint.mkdir(mode=0o700)
    store = work / 'store'
    host = work / 'host.json'
    facts = dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=[runtime], capabilities=['session.rpc'])
    if runtime == 'node': facts['programs'] = dict(node=dict(state='available', version=version))
    if runtime == 'exec': facts.update(os={'Darwin':'macos','Linux':'linux'}[platform.system()], arch={'arm64':'aarch64','aarch64':'aarch64','x86_64':'x86_64'}[platform.machine()])
    if case.endswith('-multi'): facts['variantId'] = 'selected'
    host.write_text(json.dumps(facts))
    artifact = work / 'plugin.zip'
    with zipfile.ZipFile(artifact, 'w') as archive:
        for member in sorted(package.rglob('*')):
            if member.is_file():
                name = member.relative_to(package).as_posix()
                if case == 'zip-path' and name == 'README':
                    name = '../escaped'
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | (0o755 if member.stat().st_mode & 0o111 else 0o644)) << 16
                info.compress_type = zipfile.ZIP_DEFLATED if case == 'deflate' else zipfile.ZIP_STORED
                archive.writestr(info, member.read_bytes())
    if case == 'zip-tail':
        with artifact.open('ab') as stream:
            stream.write(b'not-a-zip-member')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('GTERM_', 'GT_GTERM_'))}
    env.update(TMPDIR=str(work), GTERM_CATALOG=str(catalog), GTERM_ENDPOINT=str(endpoint),
               GHOSTERM_AUTOMATION_OFFSCREEN='1')
    binary = build / 'gterm'

    def run(arguments, expected=0):
        process = subprocess.run([str(binary), *map(str, arguments)], capture_output=True, env=env,
                                 timeout=15, preexec_fn=(lambda: os.umask(0o777)) if case == 'umask' else None)
        require(process.returncode == expected, f'{case}: expected {expected}, got {process.returncode}: {process.stdout!r} {process.stderr!r}')
        require(not any(token in process.stderr for token in [b'AddressSanitizer', b'runtime error:', b'ABNORMAL']), 'normal native result')
        return json.loads(process.stdout)

    with release.agent(node, work) as (public, signer):
        key_id = release.sha(base64.b64decode(public))
        policy = work / 'policy.json'
        trusted = dict(schemaVersion=1, channel='fixture', keys=[dict(id=key_id, algorithm='ed25519',
                       publicKey=public, publisher=manifest['publisher'], pluginIds=[manifest['id']])], revokedKeyIds=[])
        descriptor = work / 'release.json'; signature = work / 'release.sig'
        description = dict(schemaVersion=1, channel='fixture', keyId=key_id, pluginId=manifest['id'],
                           pluginVersion=manifest['pluginVersion'], publisher=manifest['publisher'], packageSHA256=digest,
                           artifact=dict(format='zip-v1', bytes=artifact.stat().st_size, sha256=release.sha(artifact.read_bytes())))
        if case == 'revoked': trusted['revokedKeyIds'] = [key_id]
        if case == 'scope': trusted['keys'][0]['pluginIds'] = ['org.ghosterm.other']
        if case == 'nfd-policy': trusted['keys'][0]['publisher'] = 'cafe\u0301'
        if case == 'base64-policy':
            alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
            trusted['keys'][0]['publicKey'] = public[:-2] + alphabet[alphabet.index(public[-2]) | 1] + '='
        if case == 'wrong-package': description['pluginVersion'] = '0.2.0'
        raw = json.dumps(description).encode()
        if case == 'fractional': raw = raw.replace(b'"schemaVersion": 1', b'"schemaVersion": 1.00000000000000000001')
        descriptor.write_bytes(raw)
        signed = subprocess.run([node, ROOT / 'scripts/fixtures/plugin-release-agent.mjs', 'client', signer],
                                input=release.DOMAIN + raw, capture_output=True, check=True, timeout=3).stdout
        if case == 'bad-signature': signed = bytes([signed[0] ^ 1]) + signed[1:]
        signature.write_bytes(signed)
        policy.write_text(json.dumps(trusted))
        if case == 'duplicate-policy': policy.write_text(policy.read_text().replace('"channel": "fixture"', '"channel":"fixture","chann\\u0065l":"fixture"'))
        if case == 'wrong-artifact':
            with artifact.open('ab') as stream: stream.write(b'changed')
        if case == 'store-link':
            elsewhere = work / 'elsewhere'; elsewhere.mkdir(mode=0o700); store.symlink_to(elsewhere, target_is_directory=True)
        profile = ['--client-digest', 'd' * 64, '--method', 'fixture.echo']
        if runtime != 'portable-js': profile += ['--runtime', runtime, '--allow-process-execution']
        if case.endswith('-multi'): profile += ['--variant', 'selected']
        if case == 'profile-variant-mismatch': profile += ['--variant', 'other']
        if case == 'profile-runtime-mismatch': profile += ['--runtime', 'exec', '--allow-process-execution']
        if runtime == 'node':
            records = hashlib.sha256()
            for member in sorted((p for p in sdk.rglob('*') if p.is_file()), key=lambda p: p.relative_to(sdk).as_posix().encode()):
                name = member.relative_to(sdk).as_posix().encode()
                records.update(len(name).to_bytes(4,'big') + member.stat().st_size.to_bytes(8,'big') + name + release.sha(member.read_bytes()).encode())
            profile += ['--node-runtime', str(node), version, release.sha(node.read_bytes()),
                        '--node-sdk', str(sdk), records.hexdigest()]
        options = ['--catalog', catalog, '--expect-revision', '0', '--store', store,
                   '--artifact', artifact, '--descriptor', descriptor, '--signature', signature,
                   '--policy', policy, '--host-json', host]
        expected = 6 if case in ('bad-signature', 'revoked', 'scope', 'wrong-artifact', 'wrong-package') else \
                   2 if case in ('duplicate-policy', 'fractional', 'zip-path', 'zip-tail', 'partial-register', 'nfd-policy', 'base64-policy') else \
                   5 if case == 'store-link' else 0
        if case in ('profile-runtime-mismatch', 'profile-variant-mismatch'): expected = 4
        development = case.endswith('-development')
        if development:
            result = run(['plugin', 'register-development', '--catalog', catalog, '--expect-revision', '0',
                          '--host-json', host, '--trust-development-digest', digest, '--', package, *profile])
            installed = package
        else:
            result = run(['plugin', 'install', *options, '--', *(profile if case != 'partial-register' else [])], expected)
        if expected and not result.get('installed'):
            require(not (catalog / 'executor-catalog-v1').exists(), 'rejection cannot register a command')
            if store.exists() and not store.is_symlink():
                require(set(p.name for p in store.iterdir()) <= {'.store.lock'}, 'normal rejection cleans owned stage and intent')
            require(not (work / 'escaped').exists(), 'archive must stay inside owned payload')
            return
        installed_version = store / release.sha(raw)
        if not development:
            installed = installed_version / 'payload'
            require(result.get('installed') is True, 'published install must be explicit even when registration fails')
            require(stat.S_IMODE(installed_version.stat().st_mode) == 0o500 and stat.S_IMODE(installed.stat().st_mode) == 0o500,
                'frozen private directories')
        for member in ([] if development else package.rglob('*')):
            if member.is_file():
                actual = installed / member.relative_to(package)
                require(actual.read_bytes() == member.read_bytes() and stat.S_IMODE(actual.stat().st_mode) == (0o500 if member.stat().st_mode & 0o111 else 0o400),
                        'exact immutable installed payload')
        if not development: require(set(p.name for p in store.iterdir()) == {'.store.lock', installed_version.name}, 'only completed version retained')
        if case in ('profile-runtime-mismatch', 'profile-variant-mismatch'):
            require(not result['committed'] and not (catalog / 'executor-catalog-v1').exists(), 'mismatched launch cannot become selectable')
            return
        if case == 'partial-register':
            require(not result['committed'] and not (catalog / 'executor-catalog-v1').exists(), 'installed and catalog commit are separate')
            run(['plugin', 'register-signed', '--catalog', catalog, '--expect-revision', '0',
                 '--policy', policy, '--host-json', host, '--', installed_version, *profile])
        else:
            require(result['committed'] and result['durabilityConfirmed'] and result['publisherAuthenticated'] != development, 'authenticated durable registration')
        listing = run(['plugin', 'list', '--catalog', catalog])
        require(listing['entries'][0]['source'] == ('explicit-development-digest' if development else 'signed-release-record') and not listing['publisherAuthenticated'], 'listing is a declaration, not current authentication')
        if case == 'policy-review':
            policy_workflow(run, work, catalog, policy, trusted, key_id, installed_version, package, host, profile)
            return
        if case == 'conflict':
            options[options.index('--expect-revision') + 1] = '1'
            run(['plugin', 'install', *options, '--', *profile], 8)
            require(set(p.name for p in store.iterdir()) == {'.store.lock', installed_version.name}, 'conflict must clean only new transaction')
            return
        if case == 'changed-policy':
            policy.write_bytes(policy.read_bytes() + b'\n')
            process = subprocess.run([binary, *ipc.CALL], capture_output=True, env=env, timeout=5)
            require(process.returncode == 9 and not list(endpoint.iterdir()), 'changed policy refuses command before listener')
            return
        if case == 'tampered-payload':
            path = installed / 'executor/index.js'; path.chmod(0o600); path.write_text(path.read_text() + '\n// changed\n')
            process = subprocess.run([binary, *ipc.CALL], capture_output=True, env=env, timeout=5)
            require(process.returncode in (2, 5, 6, 9) and not list(endpoint.iterdir()) and
                    not any(token in process.stderr for token in [b'AddressSanitizer', b'runtime error:', b'ABNORMAL']),
                    'installed payload is rechecked with a normal error')
            return
        process = subprocess.Popen([binary, *ipc.CALL], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, env=env)
        try:
            ipc.wait(lambda: (endpoint / 'permit').exists() or process.poll() is not None, 'signed ordinary frontend')
            require(process.poll() is None, 'signed command must listen')
            ipc.wait(lambda: (endpoint / 'permit').stat().st_size == 56, 'signed complete permit')
            instance = (endpoint / 'permit').read_bytes()[8:24].hex()
            if case == 'waiting-policy':
                policy.write_bytes(policy.read_bytes() + b'\n')
                subprocess.run([binary, 'relay', '--attach', endpoint, '--instance', instance, '--', *ipc.CALL],
                               input=b'', capture_output=True, env=env, timeout=6)
                require(process.wait(timeout=6) == 9, 'policy changes while waiting must prevent activation')
            else:
                mode = 'deny' if case.endswith('-deny') else 'active-cancel' if case.endswith('-active-cancel') else 'roundtrip'
                host_result = subprocess.run([build / 'test_gterm_stdio', binary, installed, digest, mode],
                    capture_output=True, env={**env, 'GT_GTERM_FIXTURE_ATTACH': str(endpoint),
                    'GT_GTERM_FIXTURE_ATTACH_INSTANCE': instance}, timeout=20)
                require(host_result.returncode == 0 and ('PASS stdio ' + mode).encode() in host_result.stdout,
                        f'actual Host Session/consent/RPC: {host_result.stdout!r} {host_result.stderr!r}')
                code = process.wait(timeout=6)
                # This fixture terminates the *attacher*. Its local 130 does
                # not prove remote cancellation; the original frontend must
                # retain the existing unknown/disconnected result (7).
                expected_frontend = 6 if mode == 'deny' else 7 if mode == 'active-cancel' else 0
                require(code == expected_frontend, f'ordinary command preserves Session outcome: exit={code} stderr={process.stderr.read()!r}')
            require(not list(endpoint.iterdir()), 'owned endpoint released')
        finally:
            ipc.terminate(process)
            process.stdout.close(); process.stderr.close()


def clean_error(function, path, info):
    # The installed directories are intentionally frozen. Adjust directories
    # of this owned fixture only; never chmod a file or follow a symlink.
    parent = Path(path) if Path(path).is_dir() and not Path(path).is_symlink() else Path(path).parent
    parent.chmod(parent.stat().st_mode | 0o700)
    function(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--node', type=Path, required=True)
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--case', choices=CASES, action='append')
    args = parser.parse_args()
    import shutil
    for case in args.case or CASES:
        directory = Path(tempfile.mkdtemp(prefix='grel-', dir=ROOT / 'tmp'))
        try:
            exercise(args.build.resolve(), args.node.resolve(), args.sdk.resolve() if args.sdk else None, case, directory)
        finally:
            shutil.rmtree(directory, onerror=clean_error)
        print('PASS signed executor', case, flush=True)


if __name__ == '__main__':
    main()
