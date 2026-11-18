#!/usr/bin/env python3
"""Offline release conformance with a disposable memory-only Ed25519 agent."""
import argparse
import base64
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DOMAIN = b'GHOSTERM-PLUGIN-RELEASE-V1\0'
sys.dont_write_bytecode = True


class BehaviorMismatch(AssertionError):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


@contextmanager
def agent(node, work):
    endpoint = work / 's'
    child = subprocess.Popen([node, ROOT / 'scripts/fixtures/plugin-release-agent.mjs', 'server', endpoint],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    try:
        assert select.select([child.stdout],[],[],5)[0], 'memory signer startup timed out'
        public = child.stdout.readline(1024).strip().decode()
        assert len(base64.b64decode(public)) == 32
        yield public, endpoint
    finally:
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill(); child.wait()
        child.stdout.close(); child.stderr.close()


def exercise(root, work, tools, node, public, endpoint, selected=None, sdk_tools=None, ai_packages=()):
    def cli(*args, status=0, code=None):
        child = subprocess.Popen([sys.executable, '-B', root / 'scripts/gterm-plugin', *map(str, args)],
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True,env=dict(os.environ, TMPDIR=str(ROOT / 'tmp')))
        try: output,diagnostics=child.communicate(timeout=90)
        finally:
            if child.poll() is None:
                os.killpg(child.pid,signal.SIGTERM)
                try: child.communicate(timeout=5)
                except subprocess.TimeoutExpired: os.killpg(child.pid,signal.SIGKILL);child.communicate()
        report = json.loads(output)
        assert b'Traceback' not in diagnostics and b'DO_NOT_LOG_SIGNER_SECRET' not in output + diagnostics
        assert not any(marker in diagnostics for marker in [b'SyntaxError',b'IndentationError',b'AddressSanitizer',b'Fatal error']),diagnostics
        if child.returncode != status or (code and report.get('code') != code):
            assert status != 0 and child.returncode in (0,2,4,6) and report.get('code') != 'TOOL_FAILED', (args,report,diagnostics.decode())
            raise BehaviorMismatch((args,child.returncode,report,status,code))
        return report
    package = work / 'p'
    shutil.copytree(ROOT / 'plugins/official/preview', package)
    # Metadata/closure validation must not activate the signed plugin.
    entry = package / 'client/index.js'
    entry.write_text(entry.read_text() + '\nthrow new Error("RELEASE_ENTRY_EXECUTED");\n')
    members = json.loads((package / 'integrity.json').read_text())
    for member in members['members']:
        data = (package / member['path']).read_bytes(); member.update(bytes=len(data), sha256=sha(data))
    (package / 'integrity.json').write_text(json.dumps(members))
    artifact = work / 'a.zip'
    cli('pack', '--package', package, '--tools', tools, '--out', artifact)
    key_id = sha(base64.b64decode(public))
    trusted = dict(schemaVersion=1, channel='fixture', keys=[dict(id=key_id, algorithm='ed25519', publicKey=public,
                   publisher='Ghosterm', pluginIds=['org.ghosterm.preview'])], revokedKeyIds=[])
    policy = work / 'policy.json'; policy.write_text(json.dumps(trusted))
    marker = work / 'signer-entered'
    config = dict(executable=str(node), sha256=sha(node.read_bytes()),
                  prefixArguments=[str(ROOT / 'scripts/fixtures/plugin-release-agent.mjs'), 'client', str(endpoint), str(marker)])
    signer = work / 'signer.json'; signer.write_text(json.dumps(config))
    bundle = work / 'r'
    sign_args = ['release','sign','--artifact',artifact,'--policy',policy,'--tools',tools,'--key-id',key_id,'--signer-json',signer]
    signed = cli(*sign_args, '--out', bundle)
    verify_args = ['release','verify','--artifact',bundle/'plugin.zip','--descriptor',bundle/'release.json',
                   '--signature',bundle/'release.sig','--policy',policy,'--tools',tools]
    def verify(status=0, code=None): return cli(*verify_args, status=status, code=code)
    def file_reject(name, data, code, status=6):
        path=bundle/name; old=path.read_bytes(); path.write_bytes(data)
        try: verify(status,code)
        finally: path.write_bytes(old)
    def policy_reject(changed, code, status=6):
        old=policy.read_bytes(); policy.write_text(json.dumps(changed))
        try: verify(status,code)
        finally: policy.write_bytes(old)
    def malformed_zip(label, build, code):
        bad=work/(label+'.zip'); build(bad)
        payload=json.loads((bundle/'release.json').read_text())
        payload['artifact'].update(bytes=bad.stat().st_size,sha256=sha(bad.read_bytes()))
        with zipfile.ZipFile(bad) as check:
            if 'integrity.json' in check.namelist(): payload['packageSHA256']=sha(check.read('integrity.json'))
        raw=json.dumps(payload,separators=(',',':')).encode()
        request=subprocess.run([node,ROOT/'scripts/fixtures/plugin-release-agent.mjs','client',endpoint],input=DOMAIN+raw,capture_output=True,check=True)
        description=work/(label+'.json');description.write_bytes(raw)
        signature=work/(label+'.sig');signature.write_bytes(request.stdout)
        cli('release','verify','--artifact',bad,'--descriptor',description,'--signature',signature,'--policy',policy,'--tools',tools,status=2,code=code)
    cases = []
    requested = {selected} if isinstance(selected, str) else selected
    def case(name, action):
        if requested is None or name in requested:
            action();cases.append(name)
    def success():
        checked=verify()
        assert checked['publisherAuthenticated'] is True and checked['authorization']=='not-granted' and checked['installation'] is False
        assert checked['artifactSHA256']==sha(artifact.read_bytes()) and checked['pluginId']=='org.ghosterm.preview'
        assert signed['kind']=='release-signed' and checked['kind']=='release-verified'
    case('success',success)
    def official_package(name, original):
        archive = work / f'official-{name}.zip'
        release = work / f'official-{name}-release'
        package_id = f'org.ghosterm.{name}'
        channel = work / f'official-{name}-policy.json'
        channel.write_text(json.dumps(dict(trusted, keys=[dict(trusted['keys'][0], pluginIds=[package_id])])))
        cli('pack', '--package', original, '--tools', tools, '--out', archive)
        args = list(sign_args)
        args[args.index('--artifact')+1] = archive
        args[args.index('--policy')+1] = channel
        cli(*args, '--out', release)
        checked = cli('release', 'verify', '--artifact', release/'plugin.zip',
                      '--descriptor', release/'release.json', '--signature', release/'release.sig',
                      '--policy', channel, '--tools', tools)
        assert checked['publisherAuthenticated'] is True and checked['authorization']=='not-granted'
        assert checked['installation'] is False and checked['pluginId']==package_id
        assert checked['artifactSHA256']==sha(archive.read_bytes())
        with zipfile.ZipFile(release/'plugin.zip') as signed_zip:
            expected = {p.relative_to(original).as_posix(): p.read_bytes()
                        for p in original.rglob('*') if p.is_file()}
            assert set(signed_zip.namelist()) == set(expected)
            assert all(signed_zip.read(name) == data for name, data in expected.items())
    case('official-preview', lambda: official_package('preview', ROOT / 'plugins/official/preview'))
    if sdk_tools is not None:
        def sample(name):
            package = work / f'sample-{name}'
            subprocess.run([sys.executable, '-B', ROOT/'scripts/build-plugin-sample.py',
                            '--source', ROOT/'plugins/official'/name, '--tools', sdk_tools,
                            '--out', package], check=True, timeout=200)
            official_package(name, package)
        for name in ('table', 'diff', 'task', 'views'):
            case(f'official-{name}', lambda name=name: sample(name))
    for name, built_package in zip(('ai-codex', 'ai-claude'), ai_packages, strict=bool(ai_packages)):
        case(f'official-{name}', lambda name=name, built_package=built_package: official_package(name, built_package))
    def deterministic():
        other=work/'r2';cli(*sign_args,'--out',other)
        assert {p.name:p.read_bytes() for p in bundle.iterdir()}=={p.name:p.read_bytes() for p in other.iterdir()}
    case('deterministic',deterministic)
    case('signature',lambda:file_reject('release.sig',bytes(64),'RELEASE_SIGNATURE'))
    case('payload',lambda:file_reject('release.json',(bundle/'release.json').read_bytes()+b' ','RELEASE_SIGNATURE'))
    def artifact_mismatch():
        other=work/'equivalent.zip'
        with zipfile.ZipFile(other,'w',compression=zipfile.ZIP_DEFLATED) as output:
            for path in sorted(package.rglob('*')):
                if path.is_file(): output.writestr(path.relative_to(package).as_posix(),path.read_bytes())
        file_reject('plugin.zip',other.read_bytes(),'RELEASE_ARTIFACT_MISMATCH')
    case('artifact',artifact_mismatch)
    case('revoked',lambda:policy_reject(dict(trusted,revokedKeyIds=[key_id]),'RELEASE_KEY_REVOKED'))
    case('channel',lambda:policy_reject(dict(trusted,channel='other'),'RELEASE_CHANNEL_MISMATCH'))
    case('scope',lambda:policy_reject(dict(trusted,keys=[dict(trusted['keys'][0],pluginIds=['org.other.plugin'])]),'RELEASE_KEY_SCOPE'))
    case('publisher',lambda:policy_reject(dict(trusted,keys=[dict(trusted['keys'][0],publisher='Other')]),'RELEASE_KEY_SCOPE'))
    case('key-id',lambda:policy_reject(dict(trusted,keys=[dict(trusted['keys'][0],id='0'*64)]),'RELEASE_KEY_ID',2))
    def policy_type():
        helper=root/'scripts/plugin-release-schemas.mjs';before=helper.read_bytes()
        helper.write_bytes(before.replace(b"valid: check('channel'",b"valid: true || check('channel'"))
        try: policy_reject(dict(trusted,schemaVersion=True),'RELEASE_POLICY',2)
        finally: helper.write_bytes(before)
    case('policy-type',policy_type)
    def duplicate():
        raw=(bundle/'release.json').read_bytes().replace(b'{',b'{"schemaVersion":1,',1)
        file_reject('release.json',raw,'DUPLICATE_OR_RESERVED_KEY',2)
    case('duplicate',duplicate)
    def signed_number_reject(raw):
        description=bundle/'release.json'; signature=bundle/'release.sig'
        before_description=description.read_bytes(); before_signature=signature.read_bytes()
        signed=subprocess.run([node,ROOT/'scripts/fixtures/plugin-release-agent.mjs','client',endpoint],
                              input=DOMAIN+raw,capture_output=True,check=True)
        description.write_bytes(raw); signature.write_bytes(signed.stdout)
        try: verify(2,'RELEASE_JSON_NUMBER')
        finally: description.write_bytes(before_description); signature.write_bytes(before_signature)
    def precise_descriptor():
        raw=(bundle/'release.json').read_bytes().replace(b'"schemaVersion":1',b'"schemaVersion":1.00000000000000000001')
        signed_number_reject(raw)
    case('precise-descriptor',precise_descriptor)
    def precise_policy():
        original=policy.read_bytes()
        policy.write_bytes(original.replace(b'"schemaVersion": 1',b'"schemaVersion": 1.00000000000000000001'))
        try: verify(2,'RELEASE_JSON_NUMBER')
        finally: policy.write_bytes(original)
    case('precise-policy',precise_policy)
    def precise_artifact():
        original=(bundle/'release.json').read_bytes()
        payload=json.loads(original)
        count=payload['artifact']['bytes']
        signed_number_reject(original.replace(f'"bytes":{count}'.encode(),f'"bytes":{count}.00000000000000000001'.encode()))
    case('precise-artifact',precise_artifact)
    def wrong_domain():
        raw=(bundle/'release.json').read_bytes()
        sig=subprocess.run([node,ROOT/'scripts/fixtures/plugin-release-agent.mjs','client',endpoint],input=raw,capture_output=True,check=True).stdout
        file_reject('release.sig',sig,'RELEASE_SIGNATURE')
    case('domain',wrong_domain)
    def identity():
        value=json.loads((bundle/'release.json').read_text());value['pluginVersion']='9.9.9'
        raw=json.dumps(value,separators=(',',':')).encode()
        sig=subprocess.run([node,ROOT/'scripts/fixtures/plugin-release-agent.mjs','client',endpoint],input=DOMAIN+raw,capture_output=True,check=True).stdout
        old=(bundle/'release.sig').read_bytes();(bundle/'release.sig').write_bytes(sig)
        try: file_reject('release.json',raw,'RELEASE_IDENTITY_MISMATCH')
        finally: (bundle/'release.sig').write_bytes(old)
    case('identity',identity)
    def boolean_version():
        value=json.loads((bundle/'release.json').read_text());value['schemaVersion']=True
        file_reject('release.json',json.dumps(value).encode(),'RELEASE_DESCRIPTOR',2)
    case('descriptor-type',boolean_version)
    def numeric_equivalence():
        value=json.loads((bundle/'release.json').read_text());value['schemaVersion']=1.0;value['artifact']['bytes']=float(value['artifact']['bytes'])
        raw=json.dumps(value,separators=(',',':')).encode()
        sig=subprocess.run([node,ROOT/'scripts/fixtures/plugin-release-agent.mjs','client',endpoint],input=DOMAIN+raw,capture_output=True,check=True).stdout
        old=(bundle/'release.sig').read_bytes();before=(bundle/'release.json').read_bytes()
        (bundle/'release.sig').write_bytes(sig);(bundle/'release.json').write_bytes(raw)
        try: verify()
        finally: (bundle/'release.sig').write_bytes(old);(bundle/'release.json').write_bytes(before)
    case('numeric-equivalence',numeric_equivalence)

    def crypto_type():
        helper=root/'scripts/plugin-release-crypto.mjs';before=helper.read_bytes()
        assert b'valid: verify(null, message, key, signature)' in before
        helper.write_bytes(before.replace(b'valid: verify(null, message, key, signature)',b'valid: 1'))
        try: verify(6,'RELEASE_SIGNATURE')
        finally: helper.write_bytes(before)
    case('crypto-type',crypto_type)
    def signer_failure(mode,code):
        before=signer.read_bytes();changed=dict(config,prefixArguments=[str(ROOT/'scripts/fixtures/plugin-release-agent.mjs'),mode])
        signer.write_text(json.dumps(changed))
        try: cli(*sign_args,'--out',work/mode,status=2,code=code)
        finally: signer.write_bytes(before)
    case('signer-output',lambda:signer_failure('noise','RELEASE_SIGNER_OUTPUT'))
    case('signer-diagnostic',lambda:signer_failure('diagnostic','RELEASE_SIGNER_FAILED'))
    def revoked_sign():
        marker.unlink(missing_ok=True);before=policy.read_bytes();policy.write_text(json.dumps(dict(trusted,revokedKeyIds=[key_id])))
        try: cli(*sign_args,'--out',work/'denied',status=6,code='RELEASE_KEY_REVOKED')
        finally: policy.write_bytes(before)
        assert not marker.exists()
    case('signer-authorization',revoked_sign)
    def zipped(path,names,compression=zipfile.ZIP_STORED):
        with zipfile.ZipFile(path,'w',compression=compression) as output:
            for name,data in names: output.writestr(name,data)
    def package_members(): return [(p.relative_to(package).as_posix(),p.read_bytes()) for p in package.rglob('*') if p.is_file()]
    case('traversal',lambda:malformed_zip('traversal',lambda p:zipped(p,[*package_members(),('../escape',b'a')]),'MEMBER_PATH'))
    case('collision',lambda:malformed_zip('collision',lambda p:zipped(p,[('a',b'a'),('A',b'b')]),'RELEASE_ZIP_MEMBER'))
    case('duplicate-member',lambda:malformed_zip('duplicate-member',lambda p:zipped(p,[('a',b'a'),('a',b'b')]),'RELEASE_ZIP_MEMBER'))
    def linked(path):
        with zipfile.ZipFile(path,'w') as output:
            for name,data in package_members():
                if name=='integrity.json':
                    index=json.loads(data);index['members'].append(dict(path='link',bytes=7,sha256=sha(b'outside')))
                    data=json.dumps(index).encode()
                output.writestr(name,data)
            info=zipfile.ZipInfo('link');info.create_system=3;info.external_attr=0o120777<<16
            output.writestr(info,b'outside')
    case('link',lambda:malformed_zip('link',linked,'RELEASE_ZIP_MEMBER'))
    def crc_bad(path):
        zipped(path,package_members());raw=bytearray(path.read_bytes())
        central=struct.unpack_from('<I',raw,len(raw)-6)[0]
        wrong=struct.unpack_from('<I',raw,14)[0]^1
        struct.pack_into('<I',raw,14,wrong);struct.pack_into('<I',raw,central+16,wrong)
        path.write_bytes(raw)
    case('crc',lambda:malformed_zip('crc',crc_bad,'RELEASE_ZIP_CONTENT'))
    def hidden(path):
        zipped(path,[('a',b'data')]);raw=path.read_bytes();path.write_bytes(b'PREFIX'+raw)
    case('hidden-prefix',lambda:malformed_zip('hidden-prefix',hidden,'RELEASE_ZIP_DIRECTORY'))
    def oversized(path):
        zipped(path,[('a',b'data')]);raw=bytearray(path.read_bytes())
        central=struct.unpack_from('<I',raw,len(raw)-6)[0]
        struct.pack_into('<I',raw,22,16777217);struct.pack_into('<I',raw,central+24,16777217)
        path.write_bytes(raw)
    case('member-limit',lambda:malformed_zip('member-limit',oversized,'RELEASE_ZIP_LAYOUT'))
    def compressed_tail(path):
        zipped(path,package_members(),zipfile.ZIP_DEFLATED);raw=bytearray(path.read_bytes())
        central=struct.unpack_from('<I',raw,len(raw)-6)[0]
        end=30+struct.unpack_from('<H',raw,26)[0]+struct.unpack_from('<I',raw,18)[0]
        raw[end:end]=b'X';central+=1
        struct.pack_into('<I',raw,18,struct.unpack_from('<I',raw,18)[0]+1)
        at=central;first=True
        for _ in range(struct.unpack_from('<H',raw,len(raw)-12)[0]):
            if first: struct.pack_into('<I',raw,at+20,struct.unpack_from('<I',raw,at+20)[0]+1)
            else: struct.pack_into('<I',raw,at+42,struct.unpack_from('<I',raw,at+42)[0]+1)
            first=False
            at+=46+sum(struct.unpack_from('<3H',raw,at+28))
        struct.pack_into('<I',raw,len(raw)-6,central);path.write_bytes(raw)
    case('compressed-tail',lambda:malformed_zip('compressed-tail',compressed_tail,'RELEASE_ZIP_STREAM'))
    def inflation(path):
        zipped(path,[('a',b'Z'*131072)],zipfile.ZIP_DEFLATED);raw=bytearray(path.read_bytes())
        central=struct.unpack_from('<I',raw,len(raw)-6)[0]
        struct.pack_into('<I',raw,22,4);struct.pack_into('<I',raw,central+24,4);path.write_bytes(raw)
    case('inflation',lambda:malformed_zip('inflation',inflation,'RELEASE_ZIP_BUDGET'))
    def deflated():
        compressed=work/'deflated.zip'
        zipped(compressed,[(p.relative_to(package).as_posix(),p.read_bytes()) for p in package.rglob('*') if p.is_file()],zipfile.ZIP_DEFLATED)
        cli(*[compressed if v==artifact else v for v in sign_args],'--out',work/'compressed')
    case('deflate',deflated)
    def output_exists():
        before={p.name:p.read_bytes() for p in bundle.iterdir()}
        cli(*sign_args,'--out',bundle,status=2,code='OUTPUT_EXISTS')
        assert before=={p.name:p.read_bytes() for p in bundle.iterdir()}
    case('output-exists',output_exists)
    def incompatible():
        host=work/'host.json';host.write_text(json.dumps(dict(side='client',sdkVersion='1.0.0',protocols=[1],runtimes=['portable-js'],capabilities=[])))
        cli(*verify_args,'--host-json',host,status=4,code='RELEASE_HOST_INCOMPATIBLE')
    case('host-incompatible',incompatible)
    def interrupted_sign(number):
        barrier=work/('held-'+str(number));before=signer.read_bytes()
        signer.write_text(json.dumps(dict(config,prefixArguments=[str(ROOT/'scripts/fixtures/plugin-release-agent.mjs'),'hold',str(barrier)])))
        destination=work/('interrupted-'+str(number))
        child=subprocess.Popen([sys.executable,'-B',root/'scripts/gterm-plugin',*map(str,[*sign_args,'--out',destination])],
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True,env=dict(os.environ,TMPDIR=str(ROOT/'tmp')))
        try:
            deadline=time.monotonic()+10
            while not barrier.exists() and child.poll() is None and time.monotonic()<deadline: time.sleep(.02)
            assert barrier.exists() and child.poll() is None,'signer did not reach owned message barrier'
            pid=int(barrier.read_text());os.kill(pid,0)
            child.send_signal(number);output,diagnostics=child.communicate(timeout=8)
            assert child.returncode==128+number and not destination.exists() and not list((root/'tmp').glob('plugin-release-*'))
            try: os.kill(pid,0)
            except ProcessLookupError: pass
            else: raise AssertionError('owned signer still alive')
            assert b'DO_NOT_LOG_SIGNER_SECRET' not in output+diagnostics
        finally:
            signer.write_bytes(before)
            if child.poll() is None:
                os.killpg(child.pid,signal.SIGTERM)
                try: child.communicate(timeout=5)
                except subprocess.TimeoutExpired: os.killpg(child.pid,signal.SIGKILL);child.communicate()
    case('interrupt-int',lambda:interrupted_sign(signal.SIGINT))
    case('interrupt-term',lambda:interrupted_sign(signal.SIGTERM))
    assert requested is None or set(cases) == requested, selected
    return cases


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools',type=Path,required=True)
    parser.add_argument('--sdk-tools', type=Path, help='also build and verify the official Portable samples')
    parser.add_argument('--ai-packages', nargs=2, type=Path, metavar=('CODEX', 'CLAUDE'),
                        help='verify signed archives of the two built official AI packages')
    parser.add_argument('--export-official-fixtures', type=Path,
                        help='write only the seven signed test releases to a new project tmp directory')
    parser.add_argument('--mutate',action='store_true')
    parser.add_argument('--receipt',type=Path,default=ROOT/'tmp/plugin-release-receipt.json')
    args=parser.parse_args()
    official = ('preview', 'table', 'diff', 'task', 'views', 'ai-codex', 'ai-claude')
    export = args.export_official_fixtures
    if export is not None:
        if not args.sdk_tools or not args.ai_packages or args.mutate:
            parser.error('--export-official-fixtures requires --sdk-tools and --ai-packages without --mutate')
        export = export.resolve()
        if not export.is_relative_to(ROOT / 'tmp') or export.exists() or args.export_official_fixtures.is_symlink():
            parser.error('official fixtures require a new directory under project tmp/')
    node=Path(shutil.which('node')).resolve(strict=True)
    inputs={str(p.relative_to(ROOT)):sha(p.read_bytes()) for directory in ['scripts','packages','plugins/official/preview','core/tests/fixtures',*(['plugins/official/'+n for n in ('table','diff','task','views')] if args.sdk_tools else [])] for p in (ROOT/directory).rglob('*')
            if p.is_file() and '__pycache__' not in p.parts}
    node_sha=sha(node.read_bytes())
    binary_inputs={str(p):sha(p.read_bytes()) for p in args.tools.resolve().rglob('*') if p.is_file()}
    if args.sdk_tools:
        binary_inputs.update({str(p):sha(p.read_bytes()) for p in args.sdk_tools.resolve().rglob('*') if p.is_file()})
    ai_packages = tuple(p.resolve(strict=True) for p in (args.ai_packages or ()))
    for name, package in zip(('ai-codex', 'ai-claude'), ai_packages, strict=bool(ai_packages)):
        if json.loads((package/'plugin.json').read_text())['id'] != f'org.ghosterm.{name}':
            parser.error(f'expected the built {name} package at {package}')
        binary_inputs.update({str(p):sha(p.read_bytes()) for p in package.rglob('*') if p.is_file()})
    binary_inputs[str(ROOT/'core/build/test_config')]=sha((ROOT/'core/build/test_config').read_bytes())
    with tempfile.TemporaryDirectory(prefix='pr-',dir=ROOT/'tmp') as temporary:
        work=Path(temporary)
        source=work/'source'
        for directory in ['scripts','packages']:
            shutil.copytree(ROOT/directory,source/directory,ignore=shutil.ignore_patterns('__pycache__'))
        (source/'tmp').mkdir()
        with agent(node,work) as (public,endpoint):
            case_root=work/'cases';case_root.mkdir()
            cases=exercise(source,case_root,args.tools.resolve(),node,public,endpoint,sdk_tools=args.sdk_tools.resolve() if args.sdk_tools else None,
                           ai_packages=ai_packages,
                           selected=frozenset('official-' + name for name in official) if export else None)
            print('RELEASE PASS: '+str(len(cases))+' cases: '+', '.join(cases),flush=True)
            recipes=[
                ('number-precision','precise-policy','json.loads(raw, parse_float=number)','json.loads(raw, parse_float=float)'),
                ('signature','signature',"result['valid'] is True, 'RELEASE_SIGNATURE', 6)", "True, 'RELEASE_SIGNATURE', 6)"),
                ('artifact','artifact',"(release['artifact']['bytes'], release['artifact']['sha256']) == (size, digest)","True"),
                ('channel','channel',"release['channel'] == trusted['channel']","True"),
                ('revoked','revoked',"release['keyId'] not in trusted['revokedKeyIds']","True"),
                ('publisher','publisher',"release['publisher'] == key['publisher'] and release['pluginId'] in key['pluginIds']","release['pluginId'] in key['pluginIds']"),
                ('scope','scope',"release['publisher'] == key['publisher'] and release['pluginId'] in key['pluginIds']","release['publisher'] == key['publisher']"),
                ('identity','identity',"all(release[name] == value for name, value in identity.items())","True"),
                ('policy-type','policy-type',"type(value['schemaVersion']) in (int, float) and value['schemaVersion'] == 1, 'RELEASE_POLICY')","value['schemaVersion'] == 1, 'RELEASE_POLICY')"),
                ('zip-member','link',"stat.S_IFMT(item.external_attr >> 16) in (0, stat.S_IFREG)","True"),
                ('zip-path','traversal',"name = api.member(item.filename)","name = item.filename"),
                ('zip-collision','collision',"name.lower() not in folded","True"),
                ('zip-crc','crc',"produced == original and checksum == crc","produced == original"),
                ('zip-limit','member-limit',"0 <= item.file_size <= MEMBER_LIMIT","0 <= item.file_size <= ARCHIVE_LIMIT"),
                ('zip-stream','compressed-tail',"not decoder.unused_data and (not decoder.eof or remaining == 0)","True"),
                ('zip-inflation','inflation',"produced <= original, 'RELEASE_ZIP_BUDGET'","True, 'RELEASE_ZIP_BUDGET'"),
                ('crypto-type','crypto-type',"result['valid'] is True, 'RELEASE_SIGNATURE', 6)","result['valid'] == True, 'RELEASE_SIGNATURE', 6)"),
            ]
            target=source/'scripts/plugin_release.py';original=target.read_bytes();groups=[];observations={}
            for label,selected,old,new in recipes:
                assert original.count(old.encode())==1,label
            for label,selected,old,new in recipes if args.mutate else []:
                target.write_bytes(original.replace(old.encode(),new.encode()))
                compile(target.read_bytes(),str(target),'exec')
                test=work/('m-'+label);test.mkdir()
                try:
                    try: exercise(source,test,args.tools.resolve(),node,public,endpoint,selected)
                    except BehaviorMismatch as error:
                        _args,actual,report,expected,code=error.args[0]
                        observations[label]=dict(actualStatus=actual,actualCode=report.get('code'),expectedStatus=expected,expectedCode=code,bytecodeCompiled=True)
                    else: raise AssertionError('mutation was not detected: '+label)
                    control=subprocess.run([ROOT/'core/build/test_config',ROOT/'core/tests/fixtures',work],capture_output=True,timeout=10)
                    assert control.returncode==0,('independent config control',control.stderr.decode())
                finally: target.write_bytes(original)
                restored=work/('r-'+label);restored.mkdir()
                exercise(source,restored,args.tools.resolve(),node,public,endpoint,selected)
                assert sha(target.read_bytes())==sha(original)
                groups.append(label);print('PASS assertion/control/restore '+label,flush=True)
        if export is not None:
            export.mkdir(mode=0o700)
            try:
                for name in official:
                    basename = 'official-' + name
                    release = basename + '-release'
                    (export / release).mkdir()
                    for filename in ('plugin.zip', 'release.json', 'release.sig'):
                        shutil.copyfile(case_root / release / filename, export / release / filename)
                    shutil.copyfile(case_root / (basename + '-policy.json'), export / (basename + '-policy.json'))
            except BaseException:
                shutil.rmtree(export)
                raise
    assert all(sha((ROOT/name).read_bytes())==value for name,value in inputs.items()),'source changed'
    assert sha(node.read_bytes())==node_sha,'Node changed'
    assert all(sha(Path(name).read_bytes())==value for name,value in binary_inputs.items()),'tool inputs changed'
    args.receipt.write_text(json.dumps(dict(schemaVersion=1,inputs=inputs,binaryInputs=binary_inputs,nodeSHA256=node_sha,cases=cases,mutations=groups,observations=observations),indent=2)+'\n')


if __name__=='__main__':
    def interrupted(number,_frame): raise SystemExit(128+number)
    for number in (signal.SIGINT,signal.SIGTERM): signal.signal(number,interrupted)
    main()
