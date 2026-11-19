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
import shlex
from pathlib import Path
import shutil
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
CASES += ['node-multi', 'exec-multi', 'profile-runtime-mismatch', 'profile-variant-mismatch', 'policy-review', 'version-maintenance', 'version-reference', 'version-active', 'node-version-active', 'exec-version-active']
CASES += ['residue-maintenance', 'residue-reference', 'residue-damaged-intent']
CASES += ['node-same-multi', 'exec-same-multi']
CASES += ['policy-subset']
POLICY_SYNC_CASES = {'policy-file-sync': 'file-sync', 'policy-directory-sync': 'directory-sync'}
FAULT_CASES = ['version-resume', 'residue-resume', *POLICY_SYNC_CASES]


def require(value, label):
    if not value:
        raise AssertionError(label)


def policy_subset(run, work, package, catalog, policy, trusted, key_id, description,
                  install_options, profile, node, signer, fault):
    other_id = 'org.ghosterm.second'
    second = work / 'second-package'
    shutil.copytree(package, second)
    manifest = json.loads((second / 'plugin.json').read_text())
    manifest['id'] = other_id
    (second / 'plugin.json').write_text(json.dumps(manifest))
    integrity_path = second / 'integrity.json'
    integrity = json.loads(integrity_path.read_text())
    for item in integrity['members']:
        data = (second / item['path']).read_bytes()
        item.update(bytes=len(data), sha256=release.sha(data))
    integrity_path.write_text(json.dumps(integrity))
    artifact = work / 'second.zip'
    with zipfile.ZipFile(artifact, 'w') as archive:
        for member in sorted(second.rglob('*')):
            if member.is_file():
                archive.write(member, member.relative_to(second))
    descriptor = work / 'second.json'
    signature = work / 'second.sig'
    raw = json.dumps(dict(description, pluginId=other_id, packageSHA256=release.sha(integrity_path.read_bytes()),
                          artifact=dict(format='zip-v1', bytes=artifact.stat().st_size,
                                        sha256=release.sha(artifact.read_bytes())))).encode()
    descriptor.write_bytes(raw)
    signature.write_bytes(subprocess.run(
        [node, ROOT / 'scripts/fixtures/plugin-release-agent.mjs', 'client', signer],
        input=release.DOMAIN + raw, capture_output=True, check=True, timeout=3).stdout)
    options = list(install_options)
    for flag, value in [('--expect-revision', '1'), ('--artifact', artifact),
                        ('--descriptor', descriptor), ('--signature', signature)]:
        options[options.index(flag) + 1] = value
    run(['plugin', 'install', *options, '--', *profile])
    review_command = ['plugin', 'review-policy', '--catalog', catalog, '--policy', policy]
    allowed = run(review_command)
    trusted['revokedKeyIds'] = [key_id]

    def apply(review, ids, expected=0, catalog_io=None):
        return run(['plugin', 'disable-denied', '--catalog', catalog, '--policy', policy,
                    '--expect-revision', review['revision'], '--expect-policy-digest', review['policyDigest'],
                    '--review-digest', review['reviewDigest'],
                    *(option for selected in ids for option in ('--select', selected))], expected, catalog_io)

    original = (catalog / 'executor-catalog-v1').read_bytes()
    require(apply(allowed, [other_id], 2)['code'] == 'POLICY_SELECTION', 'cannot select an allowed record')
    policy.write_text(json.dumps(trusted))
    revoked = run(review_command)
    require(revoked['deniedCount'] == 2, 'both real signed records revoked')
    for ids in ([other_id, 'org.ghosterm.missing'], [other_id, other_id]):
        require(apply(revoked, ids, 2)['code'] == 'POLICY_SELECTION', 'invalid selection fails as a unit')
    require((catalog / 'executor-catalog-v1').read_bytes() == original, 'rejected selection leaves bytes intact')
    if fault:
        failed = apply(revoked, [other_id], 5, fault)
        require(failed['code'] == 'POLICY_COMMIT' and not failed['durabilityConfirmed'] and
                failed['committed'] == (fault == 'directory-sync'), 'commit visibility and durability are distinct')
        require(not any(p.name.startswith('.executor-catalog-') for p in catalog.iterdir()),
                'failed publication must clean the transaction file')
        if fault == 'file-sync':
            require((catalog / 'executor-catalog-v1').read_bytes() == original, 'pre-publication failure preserves bytes')
            require(run(review_command)['reviewDigest'] == revoked['reviewDigest'], 'fresh observation matches old state')
    if fault != 'directory-sync':
        applied = apply(revoked, [other_id])
        require(applied['disabledCount'] == 1 and applied['revision'] == '3', 'only selected denial removed')
    remaining = run(review_command)
    require(remaining['revision'] == '3' and remaining['deniedCount'] == 1 and
            remaining['entries'][0]['id'] == description['pluginId'],
            'unselected denied record preserved')
    apply(revoked, [description['pluginId']], 9)
    require(apply(remaining, [description['pluginId']])['disabledCount'] == 1, 'fresh review permits second selection')



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
    empty_catalog = work / 'empty-review-catalog'; empty_catalog.mkdir(mode=0o700)
    for candidate in (catalog, empty_catalog):
        invalid = run(['plugin', 'review-policy', '--catalog', candidate, '--policy', policy], 2)
        require(invalid['code'] == 'POLICY_SNAPSHOT', 'reject malformed policy independently of catalog contents')
    invalid_apply = apply(dict(healthy, policyDigest=release.sha(policy.read_bytes())), 2)
    require(invalid_apply['code'] == 'POLICY_SNAPSHOT' and not invalid_apply['committed'],
            'confirming with the digest of malformed bytes cannot authorize a catalog change')
    require(catalog_file.read_bytes() == original_catalog, 'malformed policy does not change the catalog')
    policy.write_bytes(original_policy)
    empty = run(['plugin', 'review-policy', '--catalog', empty_catalog, '--policy', policy])
    require(empty['entries'] == [] and empty['deniedCount'] == 0, 'valid policy permits a read-only empty review')
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



def version_workflow(run, build, node, signer, work, catalog, store, endpoint, env,
                     installed_version, package, digest, host, profile, policy, description, case):
    version = installed_version.name
    def observe():
        return run(['plugin','review-installed','--catalog',catalog,'--store',store,'--version',version])
    def remove(observation, expected=0):
        return run(['plugin','remove-installed','--catalog',catalog,'--store',store,'--version',version,
                    '--expect-revision',observation['revision'],'--review-digest',observation['reviewDigest']], expected)
    def listing(): return run(['plugin','versions','--catalog',catalog,'--store',store])
    def unregister():
        run(['plugin','unregister','--catalog',catalog,'--expect-revision','1',
             '--id',description['pluginId'],'--expect-digest',digest])
    initial = observe()
    require(initial['catalogReferences'] == 1 and not initial['removable'] and not initial['publisherAuthenticated'], 'selected catalog reference blocks removal')
    require(observe()['reviewDigest'] == initial['reviewDigest'], 'read-only complete inventory is stable')
    require({p['path'] for p in initial['entries']} == {str(p.relative_to(installed_version)) for p in installed_version.rglob('*')}, 'full inventory has every current entry')
    remove(initial,8)
    require(not list(store.glob('.remove-*')), 'selected version cannot start retirement')
    if case in ('version-reference','version-active'):
        binary = build/'gterm'
        front = subprocess.Popen([binary,*ipc.CALL],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
        owner = None
        try:
            ipc.wait(lambda:(endpoint/'permit').exists() or front.poll() is not None,'owned old-version listener')
            require(front.poll() is None,'old selected invocation still alive')
            ipc.wait(lambda:(endpoint/'permit').stat().st_size==56,'complete old-version permit')
            if case=='version-active':
                ready, release_marker = work/'active-ready', work/'active-release'
                instance=(endpoint/'permit').read_bytes()[8:24].hex()
                owner=subprocess.Popen([build/'test_gterm_stdio',binary,installed_version/'payload',digest,'active-hold'],
                    stdout=subprocess.PIPE,stderr=subprocess.PIPE,env={**env,'GT_GTERM_FIXTURE_ATTACH':str(endpoint),
                    'GT_GTERM_FIXTURE_ATTACH_INSTANCE':instance,'GT_GTERM_FIXTURE_READY':str(ready),'GT_GTERM_FIXTURE_RELEASE':str(release_marker)})
                ipc.wait(lambda:ready.exists() or owner.poll() is not None,'confirmed real RPC before hold')
                require(ready.exists() and owner.poll() is None,'actual Session/RPC is active before upgrade')
            # A real second package/version is signed and installed while the
            # first pending/active instance keeps its old fixed invocation.
            import shutil
            upgraded=work/'upgrade-package';shutil.copytree(package,upgraded)
            metadata=json.loads((upgraded/'plugin.json').read_text());metadata['pluginVersion']='0.2.0'
            (upgraded/'plugin.json').write_text(json.dumps(metadata))
            integrity=json.loads((upgraded/'integrity.json').read_text())
            for item in integrity['members']:
                raw=(upgraded/item['path']).read_bytes();item.update(bytes=len(raw),sha256=release.sha(raw))
            (upgraded/'integrity.json').write_text(json.dumps(integrity));new_digest=release.sha((upgraded/'integrity.json').read_bytes())
            artifact=work/'upgrade.zip'
            with zipfile.ZipFile(artifact,'w',compression=zipfile.ZIP_STORED) as archive:
                for member in sorted(p for p in upgraded.rglob('*') if p.is_file()):
                    archive.write(member,member.relative_to(upgraded).as_posix())
            new_description={**description,'pluginVersion':'0.2.0','packageSHA256':new_digest,
                'artifact':dict(format='zip-v1',bytes=artifact.stat().st_size,sha256=release.sha(artifact.read_bytes()))}
            descriptor=work/'upgrade.json';descriptor.write_text(json.dumps(new_description));raw=descriptor.read_bytes()
            signature=work/'upgrade.sig';signature.write_bytes(subprocess.run([node,ROOT/'scripts/fixtures/plugin-release-agent.mjs','client',signer],
                input=release.DOMAIN+raw,capture_output=True,check=True,timeout=3).stdout)
            run(['plugin','install','--catalog',catalog,'--expect-revision','1','--expect-digest',digest,'--store',store,
                '--artifact',artifact,'--descriptor',descriptor,'--signature',signature,'--policy',policy,'--host-json',host,'--',*profile])
            active=observe()
            require(active['inUse'] and not active['catalogReferences'] and not active['removable'],'old pending/active version survives a new selected package')
            row=next(p for p in listing()['entries'] if p.get('name')==version)
            require(row['inUse'] and row['catalogReferences']==0,'version list observes actual cross-process reference')
            remove(active,11)
            foreign_catalog=work/'other-catalog';foreign_catalog.mkdir(mode=0o700)
            foreign=run(['plugin','review-installed','--catalog',foreign_catalog,'--store',store,'--version',version])
            require(foreign['inUse'] and not foreign['catalogReferences'],'cross-catalog observer sees the same held directory reference')
            run(['plugin','remove-installed','--catalog',foreign_catalog,'--store',store,'--version',version,
                 '--expect-revision',foreign['revision'],'--review-digest',foreign['reviewDigest']],11)
            if owner:
                release_marker.write_text('release')
                out,err=owner.communicate(timeout=10)
                require(owner.returncode==0 and b'PASS stdio active-hold' in out,f'owned active Host release: {out!r} {err!r}')
                require(front.wait(timeout=6)==0,'old active Runtime finishes after selection replacement')
            else:
                front.terminate();require(front.wait(timeout=6)==130,'owned pending invocation explicitly cancels')
            remove(active,9)
            fresh=observe();require(fresh['removable'] and not fresh['inUse'],'last old-version reference released')
            require(remove(fresh)['removed'],'explicit old-version removal after owner exit')
            current=store/release.sha(raw)
            require(current.is_dir() and run(['plugin','list','--catalog',catalog])['entries'][0]['version']=='0.2.0','new selected version remains intact')
        finally:
            if owner:
                ipc.terminate(owner);owner.stdout.close();owner.stderr.close()
            ipc.terminate(front);front.stdout.close();front.stderr.close()
        return
    unregister();remove(initial,9)
    fresh=observe();require(fresh['removable'],'unregistered inactive version can be proposed')
    if case=='version-resume':
        marker=work/'removal-paused'
        args=[build/'gterm','plugin','remove-installed','--catalog',catalog,'--store',store,'--version',version,
              '--expect-revision',fresh['revision'],'--review-digest',fresh['reviewDigest']]
        operation=subprocess.Popen(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.PIPE,
            env={**env,'GTERM_TEST_STORE_PAUSE':str(marker)})
        try:
            ipc.wait(lambda:marker.exists() or operation.poll() is not None,'test-only post-unlink checkpoint')
            require(marker.exists() and operation.poll() is None,'explicit fault build must pause after a real durable unlink')
            operation.kill();require(operation.wait(timeout=5)==-9,'actual owned SIGKILL')
        finally:
            ipc.terminate(operation);operation.stdout.close();operation.stderr.close()
        quarantine=store/('.remove-'+version)
        require(quarantine.is_dir() and not installed_version.exists() and not (quarantine/'release.sig').exists(),'published name quarantined; one actual entry removed')
        remove(fresh,9)
        # A durable pending removal blocks reinstalling the same descriptor.
        run(['plugin','install','--catalog',catalog,'--expect-revision','2','--store',store,
             '--artifact',work/'plugin.zip','--descriptor',work/'release.json','--signature',work/'release.sig',
             '--policy',policy,'--host-json',host,'--',*profile],11)
        require(quarantine.is_dir() and not installed_version.exists(),'reinstall cannot bypass pending removal')
        resumed=observe();require(resumed['pendingRemoval'] and resumed['quarantined'],'fresh explicit observation after process death')
        extra=quarantine/'new-owned-file';extra.write_text('new explicit review required');extra.chmod(0o400)
        remove(resumed,9)
        final=observe();require(any(p['path']=='new-owned-file' for p in final['entries']),'new current content is fully visible before new confirmation')
        require(remove(final)['removed'] and not list(store.glob('.remove-*')),'explicit continuation removes exact newly reviewed owned tree and intent')
        return
    outside=work/'outside';outside.write_text('outside unchanged');outside.chmod(0o600)
    leaf=installed_version/'payload/README';original=leaf.read_bytes();parent=leaf.parent
    parent.chmod(0o700);leaf.unlink();leaf.symlink_to(outside)
    run(['plugin','review-installed','--catalog',catalog,'--store',store,'--version',version],6)
    require(outside.read_text()=='outside unchanged' and stat.S_IMODE(outside.stat().st_mode)==0o600,'no symlink traversal or chmod outside selected tree')
    leaf.unlink();leaf.write_bytes(original);leaf.chmod(0o400);parent.chmod(0o500)
    hardlink=work/'hardlink';os.link(leaf,hardlink)
    run(['plugin','review-installed','--catalog',catalog,'--store',store,'--version',version],6)
    require(hardlink.read_bytes()==original and stat.S_IMODE(hardlink.stat().st_mode)==0o400,'hardlink guard never chmods regular files')
    hardlink.unlink();remove(fresh,9)
    final=observe()
    other=store/'unknown';other.mkdir(mode=0o700);(other/'keep').write_text('keep')
    result=remove(final)
    require(result['removed'] and not result['intentRetained'] and result['durabilityConfirmed'] and not installed_version.exists(),'complete confirmed removal')
    require((other/'keep').read_text()=='keep' and outside.read_text()=='outside unchanged','unrelated selected-store entries and external files retained')
    require({p.get('name') for p in listing()['entries']}=={'unknown'},'version list retains unknown entries without adoption')
    run(['plugin','review-installed','--catalog',catalog,'--store',store,'--version',version],10)


def residue_workflow(run, build, work, catalog, store, env, installed_version, case, digest, host, profile, description):
    import fcntl
    name = '.prepare-' + 'a' * 32
    stage = store / name
    stage.mkdir(mode=0o700)
    nested = stage / 'payload'; nested.mkdir(mode=0o700)
    leaf = nested / 'cafe\u0301-文件'
    leaf.write_bytes(b'partial installation'); leaf.chmod(0o600)
    intent = store / ('.intent-' + 'a' * 32)
    intent.write_text(name); intent.chmod(0o400)
    original_catalog = (catalog / 'executor-catalog-v1').read_bytes()
    def observe(selected=name, expected=0):
        return run(['plugin','review-residue','--catalog',catalog,'--store',store,'--name',selected], expected)
    def remove(review, expected=0):
        return run(['plugin','remove-residue','--catalog',catalog,'--store',store,'--name',review['entryName'],
                    '--expect-revision',review['revision'],'--review-digest',review['reviewDigest']],expected)
    if case == 'residue-damaged-intent':
        damaged = store / ('.remove-' + installed_version.name + '.intent')
        damaged.write_bytes(b'incomplete pre-rename intent'); damaged.chmod(0o600)
        registered = observe(damaged.name)
        require(registered['preservedVersion'] == installed_version.name and
                registered['catalogReferences'] == 1 and not registered['removable'] and
                any(p['path'] == installed_version.name+'/payload/plugin.json' for p in registered['entries']),
                'intent review binds complete associated version and registration')
        remove(registered,8)
        run(['plugin','unregister','--catalog',catalog,'--expect-revision','1',
             '--id',description['pluginId'],'--expect-digest',digest])
        remove(registered,9)
        fd = os.open(installed_version, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
            active = observe(damaged.name)
            require(active['inUse'] and not active['removable'],'associated version lock prevents clearing its intent')
            remove(active,11)
        finally:
            os.close(fd)
        remove(active,9)
        reviewed = observe(damaged.name)
        actual = installed_version/'payload/README'
        original = actual.read_bytes()
        actual.chmod(0o600); actual.write_bytes(original+b'changed'); actual.chmod(0o400)
        remove(reviewed,9)
        reviewed = observe(damaged.name)
        before = {str(p.relative_to(installed_version)):release.sha(p.read_bytes())
                  for p in installed_version.rglob('*') if p.is_file()}
        require(remove(reviewed)['removed'] and not damaged.exists() and installed_version.is_dir(),
                'explicit repair clears only the damaged intent')
        after = {str(p.relative_to(installed_version)):release.sha(p.read_bytes())
                 for p in installed_version.rglob('*') if p.is_file()}
        require(before == after,'associated version contents are preserved exactly')
        actual.chmod(0o600); actual.write_bytes(original); actual.chmod(0o400)
        require(remove(observe())['removed'] and remove(observe(intent.name))['removed'],'separate fixture residue cleanup')
        print('PASS damaged intent: complete preserved version, catalog and live lock guards, stale associated bytes, explicit intent-only deletion',flush=True)
        return
    if case == 'residue-reference':
        import shutil
        leaf.unlink()
        shutil.copytree(installed_version/'payload', nested, dirs_exist_ok=True)
        run(['plugin','register-development','--catalog',catalog,'--expect-revision','1',
             '--expect-digest',digest,'--host-json',host,'--trust-development-digest',digest,
             '--',nested,*profile])
        selected = (catalog/'executor-catalog-v1').read_bytes()
        referenced = observe()
        require(referenced['catalogReferences'] == 1 and not referenced['removable'],
                'real registered development selection protects a residue path')
        remove(referenced,8)
        require((catalog/'executor-catalog-v1').read_bytes() == selected and nested.is_dir(),
                'denied maintenance preserves actual selection and files')
        run(['plugin','unregister','--catalog',catalog,'--expect-revision','2',
             '--id',description['pluginId'],'--expect-digest',digest])
        remove(referenced,9)
        require(remove(observe())['removed'] and remove(observe(intent.name))['removed'],
                'unregister and fresh review explicitly release the protected residue')
        require(installed_version.is_dir(),'installed package retained after residue selection changes')
        print('PASS residue reference: actual registered package, conflict, catalog CAS, explicit unregister and fresh removal',flush=True)
        return
    fresh = observe()
    require(fresh['entryName'] == name and not fresh['ownershipKnown'] and fresh['removable'] and
            fresh['rootKind'] == 'directory' and fresh['fileBytes'] == len(leaf.read_bytes()) and
            [p['path'] for p in fresh['entries']] == ['payload', 'payload/' + leaf.name] and
            fresh['entries'][1]['sha256'] == release.sha(leaf.read_bytes()), 'complete byte-exact unknown-stage inventory')
    require(observe()['reviewDigest'] == fresh['reviewDigest'], 'stable residue review')
    observe(intent.name, 8)
    if case == 'residue-resume':
        marker = work / 'residue-paused'
        args = [build/'gterm','plugin','remove-residue','--catalog',catalog,'--store',store,'--name',name,
                '--expect-revision',fresh['revision'],'--review-digest',fresh['reviewDigest']]
        operation = subprocess.Popen(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                    env={**env,'GTERM_TEST_STORE_PAUSE':str(marker)})
        try:
            ipc.wait(lambda:marker.exists() or operation.poll() is not None,'residue durable unlink checkpoint')
            require(marker.exists() and operation.poll() is None,'explicit fault build paused after deleting an actual file')
            operation.kill(); require(operation.wait(timeout=5) == -9,'actual residue-cleaner SIGKILL')
        finally:
            ipc.terminate(operation); operation.stdout.close(); operation.stderr.close()
        require(stage.is_dir() and nested.is_dir() and not leaf.exists() and intent.exists(),'partial cleanup retained for explicit review')
        remove(fresh,9)
        final = observe()
        require([p['path'] for p in final['entries']] == ['payload'],'fresh remaining inventory after death')
        require(remove(final)['removed'] and not stage.exists() and intent.exists(),'explicit continuation only removes reviewed remainder')
    else:
        leaf.write_bytes(b'changed contents'); remove(fresh,9)
        fresh = observe()
        extra = stage / 'new-file'; extra.write_bytes(b'new'); extra.chmod(0o600)
        remove(fresh,9); extra.unlink()
        outside = work / 'outside-residue'; outside.write_bytes(b'keep'); outside.chmod(0o600)
        link = stage / 'link'; link.symlink_to(outside)
        observe(expected=6); link.unlink()
        os.link(outside, link); observe(expected=6); link.unlink()
        require(outside.read_bytes() == b'keep' and stat.S_IMODE(outside.stat().st_mode) == 0o600,'external link bytes and permissions retained')
        # A real separate directory description holds the same cooperative lock
        # used by pending and active CLI version owners.
        fd = os.open(stage, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
            active = observe()
            require(active['inUse'] and not active['removable'],'actual held residue lock is visible')
            remove(active,11)
        finally:
            os.close(fd)
        remove(active,9)
        require(remove(observe())['removed'] and not stage.exists() and intent.exists(),'only confirmed stage is removed')
        # A partially written / corrupt delete intent cannot enable maintenance
        # to bypass an existing installed slot or quarantined directory.
        protected = store / ('.remove-' + installed_version.name + '.intent')
        protected.write_bytes(b'corrupt'); protected.chmod(0o600)
        protected_review = observe(protected.name)
        require(protected_review['catalogReferences'] == 1 and not protected_review['removable'],
                'associated version registration blocks intent deletion')
        remove(protected_review,8)
        require(installed_version.is_dir() and protected.read_bytes() == b'corrupt','existing installed slot blocks damaged-intent deletion')
        protected.unlink()
        orphan_name = '.remove-' + 'c' * 64
        orphan = store / orphan_name; orphan.mkdir(mode=0o500)
        damaged = store / (orphan_name + '.intent'); damaged.write_bytes(b'incomplete'); damaged.chmod(0o600)
        observe(damaged.name,8)
        require(remove(observe(orphan_name))['removed'] and damaged.exists(),'orphan quarantine removed independently of damaged intent')
        damaged_review = observe(damaged.name)
        require(damaged_review['rootKind'] == 'file' and damaged_review['rootSHA256'] == release.sha(b'incomplete'),'damaged intent hashes actual bytes without adopting its claims')
        damaged.write_bytes(b'changed'); remove(damaged_review,9)
        require(remove(observe(damaged.name))['removed'] and not damaged.exists(),'new explicit damaged-intent confirmation')
        for invalid in ['.store.lock', installed_version.name, '../outside-residue', '.prepare-'+'z'*32]:
            observe(invalid,2)
    intent_review = observe(intent.name)
    require(intent_review['rootSHA256'] == release.sha(name.encode()),'orphan install intent has a separate complete review')
    require(remove(intent_review)['removed'] and not intent.exists(),'explicit orphan intent deletion')
    observe(expected=10)
    require((catalog/'executor-catalog-v1').read_bytes() == original_catalog and installed_version.is_dir(),
            'maintenance leaves catalog and installed version unchanged')
    print('PASS residue workflow: complete inventory, stale confirmations, references, typed cleanup and explicit continuation',flush=True)


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
            if case.endswith('-same-multi'):
                unselected = dict(variant, id='unselected')
                if runtime == 'node':
                    unselected['entry'] = 'executor/unselected.mjs'
                    (package / unselected['entry']).write_text("throw Error('UNSELECTED_VARIANT_ACTIVATED');")
                else:
                    unselected['executable'] = 'executor/unselected'
                    rejected_program = package / unselected['executable']
                    rejected_program.write_text('#!/bin/sh\nexit 83\n')
                    rejected_program.chmod(0o755)
                manifest['executor']['variants'].insert(0, unselected)
            else:
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

    def run(arguments, expected=0, catalog_io=None):
        child_env = dict(env, GTERM_TEST_CATALOG_IO=catalog_io) if catalog_io else env
        process = subprocess.run([str(binary), *map(str, arguments)], capture_output=True, env=child_env,
                                 timeout=15, preexec_fn=(lambda: os.umask(0o777)) if case == 'umask' else None)
        require(process.returncode == expected, f'{case}: expected {expected}, got {process.returncode}: {process.stdout!r} {process.stderr!r}')
        require(not any(token in process.stderr for token in [b'AddressSanitizer', b'runtime error:', b'ABNORMAL']), 'normal native result')
        return json.loads(process.stdout)

    with release.agent(node, work) as (public, signer):
        key_id = release.sha(base64.b64decode(public))
        policy = work / 'policy.json'
        trusted = dict(schemaVersion=1, channel='fixture', keys=[dict(id=key_id, algorithm='ed25519',
                       publicKey=public, publisher=manifest['publisher'], pluginIds=[manifest['id']])], revokedKeyIds=[])
        if case == 'policy-subset' or case in POLICY_SYNC_CASES:
            trusted['keys'][0]['pluginIds'].append('org.ghosterm.second')
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
        if case.startswith('residue-'):
            residue_workflow(run, build, work, catalog, store, env, installed_version, case,
                             digest, host, profile, description)
            return
        if case.startswith('version-') or '-version-' in case:
            version_workflow(run, build, node, signer, work, catalog, store, endpoint, env,
                installed_version, package, digest, host, profile, policy, description, case[case.index('version-'):])
            return
        if case == 'policy-review':
            policy_workflow(run, work, catalog, policy, trusted, key_id, installed_version, package, host, profile)
            return
        if case == 'policy-subset' or case in POLICY_SYNC_CASES:
            policy_subset(run, work, package, catalog, policy, trusted, key_id, description,
                          options, profile, node, signer, POLICY_SYNC_CASES.get(case))
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
        if case.endswith('-same-multi'):
            unpinned = profile.copy()
            selected_at = unpinned.index('--variant')
            del unpinned[selected_at:selected_at + 2]
            rejected = subprocess.run([binary, 'relay', '--stdio', '--trust-development-digest', digest,
                '--release-evidence', installed_version, policy, release.sha(policy.read_bytes()),
                *unpinned, '--', installed, *ipc.CALL], input=b'', capture_output=True, env=env, timeout=5)
            require(rejected.returncode == 5 and b'VARIANT_SELECTION_REQUIRED' in rejected.stderr and
                not any(token in rejected.stderr for token in [b'AddressSanitizer', b'runtime error:', b'ABNORMAL']) and
                not list(endpoint.iterdir()), 'same-runtime variants need an explicit choice before activation')
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


def install_official_candidates(build, fixtures, work):
    env = {key: value for key, value in os.environ.items() if not key.startswith(('GTERM_', 'GT_GTERM_'))}
    env['TMPDIR'] = str(work)
    names = ('preview', 'table', 'diff', 'task', 'views', 'ai-codex', 'ai-claude')
    host = work / 'host.json'
    host.write_text(json.dumps(dict(side='executor', sdkVersion='1.0.0', protocols=[1],
        runtimes=['portable-js'], capabilities=['resource.receive', 'file.readScoped', 'session.rpc',
                                                'process.spawn', 'process.pipes'])))
    for name in names:
        bundle = fixtures / f'official-{name}-release'
        artifact, descriptor, signature = (bundle / member for member in ('plugin.zip', 'release.json', 'release.sig'))
        policy = work / f'{name}-policy.json'
        shutil.copyfile(fixtures / f'official-{name}-policy.json', policy)
        catalog, store = work / f'{name}-catalog', work / f'{name}-store'
        catalog.mkdir(mode=0o700)
        store.mkdir(mode=0o700)
        raw = descriptor.read_bytes()
        require(json.loads(raw)['pluginId'] == f'org.ghosterm.{name}', f'{name}: signed fixture identity')
        result = subprocess.run([build / 'gterm', 'plugin', 'install', '--catalog', catalog,
            '--expect-revision', '0', '--store', store, '--artifact', artifact, '--descriptor', descriptor,
            '--signature', signature, '--policy', policy, '--host-json', host, '--'],
            env=env, capture_output=True, timeout=15)
        report = json.loads(result.stdout)
        require(result.returncode == 2 and report.get('installed') is True and report.get('committed') is False,
                f'{name}: installation without registration: {report}, {result.stderr!r}')
        payload = store / release.sha(raw) / 'payload'
        with zipfile.ZipFile(artifact) as archive:
            expected = {member: archive.read(member) for member in archive.namelist()}
        observed = {path.relative_to(payload).as_posix(): path.read_bytes()
                    for path in payload.rglob('*') if path.is_file()}
        require(observed == expected, f'{name}: installed payload must match signed archive')
        require(not (catalog / 'executor-catalog-v1').exists(), f'{name}: unrequested executor registration')
        profile = ['--client-digest', release.sha(expected['integrity.json'])]
        if name == 'table':
            profile.extend(['--method', 'view.table.describe', '--method', 'view.table.page'])
        if name == 'task':
            profile.extend(['--method', 'view.task.describe', '--method', 'view.task.read'])
        if name == 'diff':
            profile.extend(['--method', 'view.diff.describe', '--method', 'view.diff.page'])
        if name == 'views':
            profile.extend(['--method', 'beta.describe', '--method', 'beta.page',
                            '--method', 'views.reopen', '--host-method', 'views.client.open'])
        if name == 'ai-claude':
            profile.extend(['--method', 'claude.status'])
        if name == 'ai-codex':
            profile.extend(['--method', 'codex.thread.start'])
        workspace = work / f'{name}-workspace'
        if name == 'ai-codex':
            workspace = work / 'codex-patch-signed' / 'workspace'
            workspace.parent.mkdir(mode=0o700)
        if name in ('preview', 'ai-codex', 'ai-claude'):
            workspace.mkdir(mode=0o700)
            profile.extend(['--root', 'workspace', workspace])
        if name in ('ai-codex', 'ai-claude'):
            program = 'codex' if name == 'ai-codex' else 'claude-node'
            executable = Path('/usr/bin/false').resolve(strict=True)
            version = 'fixture-inert'
            if name == 'ai-codex':
                (workspace / 'fixture.json').write_text(json.dumps({'fixture': 'ghosterm-patch-rpc', 'version': 1}))
                executable = work / 'codex-app-server-fixture'
                fixture = ROOT / 'scripts/probes/codex-patch-server.py'
                executable.write_text('#!/bin/sh\nexec ' + shlex.join([sys.executable, '-B', str(fixture)]) + ' "$@"\n')
                executable.chmod(0o700)
                lock = ROOT / 'plugins/official/ai-codex/protocol/app-server.lock.json'
                version = json.loads(lock.read_text())['codexCliVersion']
            profile.extend(['--process-program', program, executable, version, release.sha(executable.read_bytes())])
        registration = subprocess.run([build / 'gterm', 'plugin', 'register-signed', '--catalog', catalog,
            '--expect-revision', '0', '--policy', policy, '--host-json', host, '--', payload.parent,
            *profile],
            env=env, capture_output=True, timeout=15)
        registered = json.loads(registration.stdout)
        require(registration.returncode == 0 and registered.get('committed') is True
                and registered.get('publisherAuthenticated') is True,
                f'{name}: explicit signed registration: {registered}, {registration.stderr!r}')
        manifest = json.loads(expected['plugin.json'])
        command = manifest['commands'][0]['path']
        arguments = ['site/index.md', '--assets', r'site/theme\,dark.css'] if name == 'preview' else []
        if name == 'table':
            arguments = ['--count', '17']
        if name == 'task':
            arguments = ['--limit', '17']
        if name == 'diff':
            arguments = ['--before', '"old"', '--after', '"new"']
        invocation = [*command, *arguments]
        endpoint = work / f'{name}-ipc'
        endpoint.mkdir(mode=0o700)
        launch_env = dict(env, GTERM_CATALOG=str(catalog), GTERM_ENDPOINT=str(endpoint))
        trusted = policy.read_bytes()
        policy.write_bytes(trusted + b'\n')
        refused = subprocess.run([build / 'gterm', *invocation], env=launch_env,
                                 capture_output=True, timeout=10)
        require(refused.returncode == 9 and not list(endpoint.iterdir()),
                f'{name}: changed trust must refuse before listening: {refused.stderr!r}')
        policy.write_bytes(trusted)
        with subprocess.Popen([build / 'gterm', *invocation], env=launch_env, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as frontend:
            try:
                permit = endpoint / 'permit'
                ipc.wait(lambda: (permit.exists() and permit.stat().st_size == 56) or frontend.poll() is not None,
                         f'{name}: signed ordinary command')
                if frontend.poll() is not None:
                    output, diagnostics = frontend.communicate()
                    raise AssertionError(f'{name}: ordinary command exited {frontend.returncode}: {output!r} {diagnostics!r}')
                instance = permit.read_bytes()[8:24].hex()
                inspection = subprocess.run([build / 'gterm', 'relay', '--inspect', endpoint, '--instance', instance],
                    input=b'', capture_output=True, env=launch_env, timeout=10)
                require(inspection.returncode == 0, f'{name}: signed foreground inspection: {inspection.stderr!r}')
                declaration = json.loads(inspection.stdout)
                digest = release.sha(expected['integrity.json'])
                require(declaration['instance'] == instance and declaration['command'] == ' '.join(command)
                        and declaration['arguments'] == arguments and declaration['executorDigest'] == digest
                        and declaration['clientDigest'] == digest and declaration['authorizationGranted'] is False,
                        f'{name}: inspection must bind the instance and declaration without granting authority')
                require(frontend.poll() is None and permit.exists(),
                        f'{name}: inspection must leave the ordinary command available for attachment')
                policy.write_bytes(trusted + b'\n')
                attached = subprocess.run([build / 'gterm', 'relay', '--attach', endpoint, '--instance', instance,
                    '--', ' '.join(command), *arguments], input=b'', capture_output=True, env=launch_env, timeout=10)
                status = frontend.wait(timeout=6)
                diagnostics = frontend.stderr.read()
                require(status == 9 and not attached.stdout,
                        f'{name}: changed trust must reject attachment: frontend={status}, '
                        f'attacher={attached.returncode}, {diagnostics!r} {attached.stdout!r} {attached.stderr!r}')
                require(not list(endpoint.iterdir()), f'{name}: rejected attachment must clean the IPC endpoint')
            finally:
                ipc.terminate(frontend)
        if name in ('preview', 'table', 'task', 'diff', 'views', 'ai-claude', 'ai-codex'):
            host_arguments = [{'task': 'task-read', 'table': 'table-describe', 'diff': 'diff-page', 'views': 'views-beta',
                               'ai-claude': 'claude-status', 'ai-codex': 'codex-thread'}.get(name)]
            if name == 'preview':
                site = workspace / 'site'
                site.mkdir()
                (site / 'index.md').write_text('# Preview runtime fixture\n')
                (site / 'theme,dark.css').write_text('body { color: rgb(1, 2, 3); }\n')
                resource_store = work / 'preview-host-resources'
                resource_store.mkdir(mode=0o700)
                host_arguments = ['preview', workspace, resource_store]
            policy.write_bytes(trusted)
            with subprocess.Popen([build / 'gterm', *invocation], env=launch_env, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) as frontend:
                try:
                    ipc.wait(lambda: (permit.exists() and permit.stat().st_size == 56) or frontend.poll() is not None,
                             f'{name}: ready for signed Host')
                    require(frontend.poll() is None, f'{name}: signed command must await Host')
                    instance = permit.read_bytes()[8:24].hex()
                    host_env = dict(launch_env, GT_GTERM_FIXTURE_ATTACH=str(endpoint),
                                    GT_GTERM_FIXTURE_ATTACH_INSTANCE=instance, GT_GTERM_FIXTURE_CLIENT_DIGEST=digest,
                                    GT_GTERM_FIXTURE_PLUGIN_VERSION=manifest['pluginVersion'],
                                    GT_GTERM_FIXTURE_PLUGIN_ID=manifest['id'])
                    result = subprocess.run([build / 'test_gterm_stdio', build / 'gterm', payload, digest,
                        *host_arguments], env=host_env, capture_output=True, timeout=30)
                    require(result.returncode == 0 and f'PASS stdio {host_arguments[0]}'.encode() in result.stdout,
                            f'{name}: signed Host session failed: {result.stdout!r} {result.stderr!r}')
                    output, diagnostics = frontend.communicate(timeout=6)
                    require(frontend.returncode == 0,
                            f'{name}: ordinary command must finish cleanly after Host revocation: '
                            f'exit={frontend.returncode}, {output!r} {diagnostics!r}')
                finally:
                    ipc.terminate(frontend)
            require(not list(endpoint.iterdir()), f'{name}: signed session must clean its IPC endpoint')
            print('PASS signed Host consent and runtime', name, flush=True)
        print('PASS official gterm install, register and dispatch', name, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--node', type=Path)
    parser.add_argument('--sdk', type=Path)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--case', choices=CASES + FAULT_CASES, action='append')
    selection.add_argument('--official-release-directory', type=Path,
                           help='install the seven fixtures exported by test-plugin-release.py')
    args = parser.parse_args()
    if args.official_release_directory:
        fixtures = args.official_release_directory.resolve(strict=True)
        if not fixtures.is_relative_to(ROOT / 'tmp') or not fixtures.is_dir():
            parser.error('official release fixtures must be a directory under project tmp/')
        with tempfile.TemporaryDirectory(prefix='grel-official-', dir=ROOT / 'tmp') as directory:
            install_official_candidates(args.build.resolve(), fixtures, Path(directory))
        return
    if not args.node:
        parser.error('--node is required for generated release fixtures')
    for case in args.case or CASES:
        directory = Path(tempfile.mkdtemp(prefix='grel-', dir=ROOT / 'tmp'))
        try:
            exercise(args.build.resolve(), args.node.resolve(), args.sdk.resolve() if args.sdk else None, case, directory)
        finally:
            shutil.rmtree(directory, onerror=clean_error)
        print('PASS signed executor', case, flush=True)


if __name__ == '__main__':
    main()
