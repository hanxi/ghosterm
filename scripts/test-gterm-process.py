#!/usr/bin/env python3
"""Actual public HOST Session -> gterm -> pinned Node/Exec, owned local fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--node', type=Path, required=True)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--case', action='append')
    a = p.parse_args()
    build, node, sdk = a.build.resolve(), a.node.resolve(strict=True), a.sdk.resolve(strict=True)
    records = hashlib.sha256()
    for f in sorted((f for f in sdk.rglob('*') if f.is_file()), key=lambda f: str(f.relative_to(sdk)).encode()):
        name = str(f.relative_to(sdk)).encode()
        records.update(len(name).to_bytes(4, 'big') + f.stat().st_size.to_bytes(8, 'big') + name + sha(f).encode())
    sdk_digest = records.hexdigest()
    version = subprocess.check_output([str(node), '--version'], text=True).strip()[1:]
    work = Path(tempfile.mkdtemp(prefix='gterm-process-', dir=ROOT / 'tmp'))
    runtime_tmp = work / 'runtime-tmp'
    runtime_tmp.mkdir(mode=0o700)
    workspace = work / 'workspace'
    workspace.mkdir(mode=0o700)
    env = {**os.environ, 'TMPDIR': str(runtime_tmp), 'GT_GTERM_TEST_SECRET': 'must-not-inherit',
           'GT_GTERM_FIXTURE_NODE': str(node), 'GT_GTERM_FIXTURE_VERSION': version,
           'GT_GTERM_FIXTURE_NODE_SHA': sha(node), 'GT_GTERM_FIXTURE_SDK': str(sdk),
           'GT_GTERM_FIXTURE_SDK_SHA': sdk_digest}
    cases = ['node-roundtrip', 'node-process', 'node-deny', 'node-cancel', 'node-active-cancel',
             'node-lost', 'node-pending-loss', 'node-no-approval', 'node-source-digest', 'node-bad-sha', 'node-bad-sdk',
             'node-bad-version', 'node-activation-error', 'exec-roundtrip', 'exec-deny',
             'exec-cancel', 'exec-active-cancel', 'exec-lost', 'exec-no-approval', 'exec-unsupported', 'arguments']
    if a.case:
        assert not set(a.case) - set(cases), 'unknown case'
        cases = a.case
    try:
        for case in cases:
            if case == 'arguments':
                for extra, code in [(['--runtime','node'], 6),
                    (['--runtime','node','--allow-process-execution'],4),
                    (['--runtime','exec','--allow-process-execution','--allow-process-execution'],2),
                    (['--runtime','portable-js','--allow-process-execution'],2),
                    (['--runtime','unavailable','--allow-process-execution'],4)]:
                    r = subprocess.run([str(build/'gterm'),'relay','--stdio',*extra,
                        '--trust-development-digest','a'*64,'--client-digest','b'*64,'--','missing','fixture'],
                        input=b'',capture_output=True,timeout=20,env=env)
                    assert r.returncode == code and r.stdout == b'', (extra,r.returncode,r.stderr)
                assert not list(runtime_tmp.iterdir()), 'argument rejection retained files'
                print(case+': PASS',flush=True)
                continue
            runtime, suffix = case.split('-', 1)
            local = dict(env, GT_GTERM_FIXTURE_RUNTIME=runtime)
            mode = {'process':'process-config', 'no-approval':'denied-launch',
                    'bad-sha':'runtime-failure','bad-sdk':'runtime-failure',
                    'bad-version':'runtime-failure','activation-error':'runtime-failure','source-digest':'digest','unsupported':'runtime-unsupported'}.get(suffix,suffix)
            marker = work / (case+'.activated')
            handler = work / (case+'.handler')
            if suffix == 'pending-loss': local['GT_GTERM_FIXTURE_HANDLER']=str(handler)
            package = work / case
            package.mkdir()
            variant = dict(id='selected', runtime=runtime, requires=['session.rpc'])
            if suffix == 'process':
                variant['requires'] += ['process.spawn','process.pipes']
            if runtime == 'node':
                variant.update(entry='executor/main.mjs',runtimeVersionRange='>=20.0.0 <1000.0.0')
            else:
                variant.update(executable='executor/main',os=[{'Darwin':'macos','Linux':'linux'}[platform.system()]],
                    arch=[{'arm64':'aarch64','aarch64':'aarch64','x86_64':'x86_64'}[platform.machine()]])
            manifest = dict(schemaVersion=1,id='org.ghosterm.fixture',pluginVersion='0.1.0',sdkApiRange='^1.0.0',
                bridgeProtocolRange=[1],publisher='fixture',license='MIT',
                commands=[dict(path=['fixture'],title='Fixture',argsSchema='schemas/input.json')],
                executor=dict(variants=[variant]),permissions=[])
            if suffix == 'process':
                manifest['permissions']=[dict(side='executor',capability='process.spawn',programs=['cat'],
                    purpose='Disposable local cat',scope='instance')]
            fields = ['first','empty','literal']
            schema = dict(type='object',properties={n:dict(type='string',maxLength=100) for n in fields},
                required=fields,additionalProperties=False)
            schema['x-ghosterm-cli']=dict(version=1,fields={n:dict(position=i) for i,n in enumerate(fields)})
            files = {'plugin.json':json.dumps(manifest),'README':'Disposable runtime fixture',
                     'LICENSES/MIT.txt':'MIT','schemas/input.json':json.dumps(schema)}
            if runtime == 'node':
                plugin = "import{object,text,integer}from'@ghosterm/plugin-sdk';import{writeFileSync}from'node:fs';\n"
                plugin += 'writeFileSync('+json.dumps(str(marker))+",'activated');\n"
                plugin += "export async function activate(ctx){if(process.env.GT_GTERM_TEST_SECRET!==undefined)throw Error('ambient env');"
                plugin += "if(process.env.TMPDIR!=="+json.dumps(str(runtime_tmp))+")throw Error('selected env');"
                plugin += "if(ctx.invocation.command!=='fixture'||ctx.invocation.arguments[0]!=='中文'||ctx.invocation.arguments[1]!==''||ctx.invocation.decodedArguments.empty!=='')throw Error('argv');"
                if suffix == 'activation-error':
                    plugin += "throw Error('fixture activation');"
                if suffix == 'process':
                    plugin += """if(!ctx.process||ctx.invocation.workspaceHandle.id!=='workspace')throw Error('process');
const c=await ctx.process.spawn({program:'cat',argv:['cat'],cwd:ctx.invocation.workspaceHandle,envPolicy:'selected-user-environment',cancellation:ctx.cancellation});
if(c.verifiedVersion!=='fixture-1'||c.writeStdin(Uint8Array.of(0,10,255,128)).state!=='data')throw Error('write');c.closeStdin();let out=[];
for(;;){const r=c.readStdout(16384);if(r.state==='data')out.push(...r.bytes);else if(r.state==='eof')break;else if(r.state!=='wait')throw Error('read');await new Promise(r=>setTimeout(r,1));}
if(out.join(',')!=='0,10,255,128')throw Error('binary');for(;;){const p=c.poll();if(p.state==='exited'){if(p.code!==0)throw Error('exit');break;}await new Promise(r=>setTimeout(r,1));}await c.release();
"""
                plugin += "const codec=object({text:text(40),count:integer(0,100)});ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{c.check();"
                plugin += ('writeFileSync('+json.dumps(str(handler))+",'started');return await new Promise(()=>{});") if suffix == 'pending-loss' else 'return v;'
                plugin += '});return{dispose(){}};}'
                files['executor/main.mjs']=plugin
            for name, text in files.items():
                f=package/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text)
            if runtime == 'exec':
                f=package/'executor/main';f.parent.mkdir();shutil.copyfile(build/'test_gterm_exec_peer',f)
            members=[dict(path=str(f.relative_to(package)),bytes=f.stat().st_size,sha256=sha(f))
                     for f in sorted(package.rglob('*')) if f.is_file()]
            (package/'integrity.json').write_text(json.dumps(dict(schemaVersion=1,members=members)))
            if suffix == 'unsupported': local['GT_GTERM_FIXTURE_UNSUPPORTED']='1'
            if suffix == 'no-approval': local['GT_GTERM_FIXTURE_NO_APPROVAL']='1'
            if suffix == 'bad-sha': local['GT_GTERM_FIXTURE_NODE_SHA']='a'*64
            if suffix == 'bad-sdk': local['GT_GTERM_FIXTURE_SDK_SHA']='a'*64
            if suffix == 'bad-version': local['GT_GTERM_FIXTURE_VERSION']='199.0.0'
            command=[str(build/'test_gterm_stdio'),str(build/'gterm'),str(package),sha(package/'integrity.json'),mode]
            if suffix == 'process': command += [str(workspace),'/bin/cat','fixture-1',sha(Path('/bin/cat'))]
            r=subprocess.run(command,capture_output=True,text=True,env=local,timeout=40)
            assert r.returncode==0 and 'PASS stdio '+mode in r.stdout and not any(
                s in r.stderr for s in ['ABNORMAL','AddressSanitizer','runtime error:']), (case,r.returncode,r.stdout,r.stderr)
            if runtime == 'node':
                expected = suffix not in ['deny','cancel','no-approval','source-digest','bad-sha','bad-sdk','bad-version']
                assert marker.exists()==expected,(case,'module executed before approval or identity check')
            assert not list(runtime_tmp.iterdir()),(case,'retained staging/store')
            print(case+': PASS',flush=True)
    finally:
        for directory, dirs, _ in os.walk(work,followlinks=False):
            os.chmod(directory,0o700)
            dirs[:]=[n for n in dirs if not (Path(directory)/n).is_symlink()]
        shutil.rmtree(work)


if __name__=='__main__':
    try: main()
    except AssertionError as error:
        print('FAIL gterm process: '+str(error),file=__import__('sys').stderr)
        raise SystemExit(1)
