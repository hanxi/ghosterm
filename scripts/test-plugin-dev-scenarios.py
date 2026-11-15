#!/usr/bin/env python3
"""Actual official multi-view package through the developer CLI; no GUI or remote host."""
import argparse
import copy
import importlib.util
import signal
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def bounded(command, seconds=120):
    process = subprocess.Popen(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True, env=dict(os.environ, TMPDIR=str(ROOT/'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1'))
    try:
        output, errors = process.communicate(timeout=seconds)
    except BaseException:
        if process.poll() is None:
            process.terminate()
            try: process.communicate(timeout=8)
            except subprocess.TimeoutExpired:
                try: os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError: pass
                process.communicate()
        raise
    return subprocess.CompletedProcess(command, process.returncode, output, errors)


def run_case(name, work, tools, core, execution=None, control_file=None):
    def cli(*args, status=0, diagnostic=None):
        result = bounded(['python3', ROOT / 'scripts/gterm-plugin', *args])
        assert result.returncode == status, (result.returncode, result.stdout.decode(), result.stderr.decode())
        if diagnostic: assert diagnostic.encode() in result.stderr, result.stderr.decode()
        return json.loads(result.stdout)
    if name == 'interruptions-node':
        assert execution
        for number in [signal.SIGINT,signal.SIGTERM]:
            marker=work/('ready-'+str(number)); before=set((ROOT/'tmp').glob('dev-scenario-*'))
            command=['python3',str(Path(__file__).resolve()),'--tools',str(tools),'--core-build',str(core),'--execute','--node-executable',execution['nodeExecutable'],'--node-sdk',execution['nodeSDKDirectory'],'--case','hold-node','--control-file',str(marker)]
            process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True,env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),GHOSTERM_AUTOMATION_OFFSCREEN='1'))
            try:
                until=time.monotonic()+15
                while not marker.exists() and process.poll() is None and time.monotonic()<until: time.sleep(.005)
                assert marker.exists() and process.poll() is None,'owned Node must be live before interruption'
                pid=int(marker.read_text());os.kill(pid,0)
                owned=set((ROOT/'tmp').glob('dev-scenario-*'))-before;assert len(owned)==1
                process.send_signal(number);output,errors=process.communicate(timeout=12)
                assert process.returncode==128+number and not output and b'Traceback' not in errors,(process.returncode,output,errors)
                until=time.monotonic()+3
                while time.monotonic()<until:
                    try:os.kill(pid,0)
                    except ProcessLookupError:break
                    time.sleep(.01)
                else:raise AssertionError('owned Node survived interrupted test runner')
                assert all(not p.exists() for p in owned),'interrupted private snapshot must be removed'
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:process.communicate(timeout=12)
                    except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.communicate()
        return dict(interruptions=2)
    if name == 'rpc-only':
        project=work/'project'
        cli('init',project,'--id','org.example.scenario','--command','echo','--publisher','Example')
        contract=json.loads((project/'plugin.pair.test.json').read_text())
        contract['steps']=[dict(action='request',method='dev.exchange',input={'text':'sequential'},expected={'kind':'success','value':{'text':'executor:callback:sequential'}}),
                           dict(action='request',method='dev.callback',input={'text':'reverse'},expected={'kind':'success','value':{'text':'callback:reverse'}})]
        path=work/'contract.json';path.write_text(json.dumps(contract))
        report=cli('test','--project',project,'--tools',tools,'--core-build',core,'--side','paired','--contract',path)
        assert report['scenarioSteps']==2 and report['services']==[] and len(report['observations'])==7
        return report
    project = work / 'project'; project.mkdir()
    for part in ['client', 'executor', 'schemas']: shutil.copytree(ROOT / 'plugins/official/views' / part, project / part)
    for name_ in ['shared.ts', 'plugin.json', 'README.md']: shutil.copyfile(ROOT / 'plugins/official/views' / name_, project / name_)
    (project / 'LICENSES').mkdir(); shutil.copyfile(ROOT / 'LICENSE', project / 'LICENSES/MIT.txt')
    manifest = json.loads((project / 'plugin.json').read_text())
    config = dict(schemaVersion=1, sources=['client/index.ts', 'executor/index.ts', 'shared.ts'], assets=['schemas/demo.json', 'README.md', 'LICENSES/MIT.txt'])
    options = []
    node = name.endswith('-node')
    if node:
        variant = manifest['executor']['variants'][0]; variant.update(id='node', runtime='node', runtimeVersionRange='>=22.0.0 <1000.0.0')
        config['assets'].append('package.json'); (project / 'package.json').write_text('{"type":"module"}')
        (project / 'plugin.json').write_text(json.dumps(manifest))
        assert execution
        host = dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=['node'], capabilities=['session.rpc'], programs={'node':dict(state='available', version=execution['version'])})
        client = dict(side='client', sdkVersion='1.0.0', protocols=[1], runtimes=['portable-js'], capabilities=['session.rpc', 'ui.table', 'ui.diff', 'ui.task', 'ui.views'])
        hostfile = work / 'host.json'; hostfile.write_text(json.dumps(dict(client=client, executor=host)))
        exefile = work / 'execution.json'; exefile.write_text(json.dumps({k:v for k,v in execution.items() if k != 'version'}))
        options = ['--host-json', hostfile, '--execution-json', exefile, '--allow-process-execution']
    (project / 'plugin.project.json').write_text(json.dumps(config))
    kinds = ['table', 'diff', 'task']
    views = [dict(kind='table',viewId='alpha',methods=dict(describe='alpha.describe',read='alpha.page')),
             dict(kind='table',viewId='beta',methods=dict(describe='beta.describe',read='beta.page')),
             dict(kind='diff',viewId='changes',methods=dict(describe='changes.describe',read='changes.page')),
             dict(kind='task',viewId='progress',methods=dict(describe='work.describe',read='work.read',cancel='work.cancel'))]
    methods = [dict(name=m,receiver='executor') for v in views for m in v['methods'].values()]
    methods += [dict(name='views.reopen',receiver='executor'),dict(name='views.client.open',receiver='client')]
    def success(value): return dict(kind='success',value=value)
    def request(method, value, expected): return dict(action='request',method=method,input=value,expected=success(expected))
    def obj(properties): return dict(type='object',additionalProperties=False,required=list(properties),properties=properties)
    def task_schema(phase):
        value = obj(dict(viewId={'const':'progress'}, revision={'const':'1'}, sequence=dict(type='string',pattern='^[1-9][0-9]{0,6}$'),
                         phase={'const':phase},completed=dict(type='string',pattern='^[1-9][0-9]{0,5}$'),
                         total={'const':{'kind':'known','units':'1000000'}}, message={'const':'Counting without external effects'},
                         result={'const':{'kind':'none'} if phase=='running' else {'kind':'cancelled','effect':'none'}}))
        return obj(dict(kind={'const':'success'},value=value))
    description = dict(viewId='alpha',revision='1',title='Alpha numbers',columns=[dict(id='number',title='Number'),dict(id='value',title='Value')],total=64,pageSize=16)
    alpha = dict(viewId='alpha',revision='1',offset=16,total=64,rows=[dict(id=str(i),cells=[str(i),str(i)]) for i in range(17,33)])
    beta = dict(viewId='beta',revision='1',offset=0,total=5,rows=[dict(id=str(i),cells=[str(i),str(i*10)]) for i in range(1,6)])
    difference = dict(viewId='changes',revision='1',offset=0,total=2,beforeOffset=0,afterOffset=0,
                      rows=[dict(kind='delete',beforeLine=1,afterLine=0,text='before',ending='none'),dict(kind='insert',beforeLine=0,afterLine=1,text='after',ending='none')])
    scope = dict(viewId='progress',revision='1'); cancel = {**scope,'requestId':'cancel-1'}
    steps = [request('alpha.describe',{},description),
             request('alpha.page',dict(viewId='alpha',revision='1',offset=16,limit=16),alpha),
             request('beta.page',dict(viewId='beta',revision='1',offset=0,limit=16),beta),
             request('changes.page',dict(viewId='changes',revision='1',offset=0,limit=2),difference),
             request('work.describe',{},dict(**scope,title='Internal counter',unit='steps',cancellable=True)),
             dict(action='closeView',viewId='progress'),
             dict(action='request',method='work.read',input=scope,expectedSchema=task_schema('running')),
             request('views.reopen',dict(viewId='progress'),dict(opened=True)),
             dict(action='snapshot',views=views[1:]),
             request('work.cancel',cancel,{**cancel,'kind':'requested'}),
             dict(action='request',method='work.read',input=scope,expectedSchema=task_schema('cancelled')),
             request('work.cancel',cancel,{**cancel,'kind':'requested'}),
             request('views.reopen',dict(viewId='alpha'),dict(opened=True)),
             dict(action='snapshot',views=views)]
    contract = dict(schemaVersion=1,command='views demo',arguments=[],method='views.reopen',methods=methods,
                    input=dict(viewId='alpha'),expected=dict(opened=False),errorInput=dict(viewId='missing'),errorKind='unknown',views=views,steps=steps)
    spec = work / 'views.json'; spec.write_text(json.dumps(dict(kinds=kinds,registry=True)))
    contractfile = work / 'contract.json'; contractfile.write_text(json.dumps(contract))
    arguments = ['test','--project',project,'--tools',tools,'--core-build',core,'--side','paired','--views-json',spec,'--allow-native-views','--contract',contractfile,*options]
    if name == 'hold-node':
        assert control_file is not None and control_file.is_relative_to(ROOT/'tmp') and not control_file.exists()
        target=project/'executor/index.ts';text=target.read_text()
        needle='context.session.register(ReopenView, async (input, request) => {'
        assert text.count(needle)==1
        text='import {writeFileSync} from "node:fs";\n'+text.replace(needle,needle+'\n    if(input.viewId === "hold") { writeFileSync('+json.dumps(str(control_file))+',String(process.pid)); return await new Promise<{opened:boolean}>(() => {}); }')
        target.write_text(text);contract.pop('steps');contract.update(timeoutMs=5000,input={'viewId':'hold'})
        contractfile.write_text(json.dumps(contract))
    def invoke(status=0, diagnostic=None): return cli(*arguments,status=status,diagnostic=diagnostic)
    report = invoke()
    assert report['scenarioSteps'] == 14 and report['services'] == ['views'] and report['viewServices']==['ui.table','ui.diff','ui.task','ui.views']
    traces = report['observations']; assert len(traces) == 20 and traces[-1]['clientEngineReleased'] and traces[-1]['executorEngineReleased']
    assert len(traces[4]['snapshot']['views']) == 4 and len(traces[4]['afterClose']['views']) == 3
    assert traces[10]['viewId'] == 'progress' and len(traces[10]['afterClose']['views']) == 2
    final = traces[18]['snapshot']['views']; initial=traces[4]['snapshot']['views']
    assert {v['viewId'] for v in final} == {'alpha','beta','changes','progress'}
    for identity in ['alpha','progress']:
        assert next(v['handle'] for v in initial if v['viewId']==identity) != next(v['handle'] for v in final if v['viewId']==identity)
    assert all(t.get('staleRejected') is True for t in traces if t.get('action') in ('snapshot','closeView'))
    if name == 'contracts':
        variants = [(None,'TEST_SCENARIO_SIDE'),([], 'TEST_SCENARIO'),(steps*2,'TEST_SCENARIO'),([{'action':'unknown'}],'TEST_SCENARIO_ACTION'),
                    ([{'action':'request','method':'missing.method','input':{},'expected':success({})}],'TEST_SCENARIO_REQUEST'),
                    ([{'action':'request','method':'alpha.describe','input':[],'expected':success({})}],'TEST_SCENARIO_REQUEST'),
                    ([{'action':'closeView','viewId':'bad id'}],'TEST_SCENARIO_CLOSE'),
                    ([{'action':'snapshot','views':views*2}],'TEST_VIEW_DUPLICATE'),
                    ([request('alpha.describe', {'text':'x'*8192}, {})],'TEST_SCENARIO_INPUT_LIMIT'),
                    ([request('alpha.describe', {'text':'x'*4500}, {}) for _ in range(8)],'TEST_SCENARIO_INPUT_LIMIT')]
        for changed, code in variants:
            contractfile.write_text(json.dumps({**contract,'steps':changed}))
            assert invoke(status=2)['code'] == code
        contractfile.write_text(json.dumps({**contract,'timeoutMs':2000}))
        assert invoke(status=2)['code'] == 'TEST_SCENARIO_BUDGET'
        contractfile.write_text(json.dumps(contract))
        side = arguments.index('--side')+1; arguments[side]='client'
        assert invoke(status=2)['code'] == 'TEST_SCENARIO_SIDE'
    elif name == 'registry-guards':
        for invalid in [1, 'true', None]:
            spec.write_text(json.dumps(dict(kinds=kinds,registry=invalid)))
            assert invoke(status=2)['code'] == 'TEST_VIEWS_CONFIG'
        spec.write_text(json.dumps(dict(kinds=kinds,registry=False)))
        assert invoke(status=4)['compatibility'] == 'unsupported'
        spec.write_text(json.dumps(dict(kinds=kinds,registry=True)))
        host = {side:dict(side=side,sdkVersion='1.0.0',protocols=[1],runtimes=['portable-js'],capabilities=['session.rpc', *(['ui.table','ui.diff','ui.task','ui.views'] if side=='client' else [])]) for side in ['client','executor']}
        host['client']['deniedCapabilities']=['ui.views']
        hostfile=work/'host.json';hostfile.write_text(json.dumps(host));arguments += ['--host-json',hostfile]
        assert invoke(status=6)['compatibility'] == 'denied'
        del host['client']['deniedCapabilities']; hostfile.write_text(json.dumps(host))
        spec.write_text(json.dumps(dict(kinds=kinds,registry=False)))
        assert invoke(status=1,diagnostic='ASSERT: pair Client adoption')['kind'] == 'error'
        spec.write_text(json.dumps(dict(kinds=kinds,registry=True)))
        manifest['client']['requires'].remove('ui.views')
        manifest['permissions']=[p for p in manifest['permissions'] if p['capability']!='ui.views']
        (project/'plugin.json').write_text(json.dumps(manifest))
        assert invoke(status=1,diagnostic='TEST_REQUIRES_DECLARED_VIEW_REGISTRY')['kind']=='error'
    elif name in ('schema-guards', 'schema-guards-node'):
        marker=work/'NATIVE_ENTRY_RAN'
        if node:
            target=project/'executor/index.ts';target.write_text('import {writeFileSync} from "node:fs";\nwriteFileSync('+json.dumps(str(marker))+',"owned");\n'+target.read_text())
        for schema in [{'type':'banana'},{'$ref':'https://ghosterm.invalid/never-fetch.json'},{'$async':True,'type':'object'}]:
            changed=copy.deepcopy(steps); changed[6]['expectedSchema']=schema
            contractfile.write_text(json.dumps({**contract,'steps':changed}))
            assert invoke(status=1)['code']=='TOOL_FAILED'
            assert not marker.exists(), 'invalid schema must not start a plugin'
        contractfile.write_text(json.dumps(contract))
        assert invoke()['scenarioSteps']==14
        if node: assert marker.read_text()=='owned'
        changed=copy.deepcopy(steps); changed[10]['expectedSchema']=task_schema('running')
        contractfile.write_text(json.dumps({**contract,'steps':changed}))
        assert invoke(status=1,diagnostic='TEST_SCENARIO_SCHEMA_RESULT')['code']=='TOOL_FAILED'
        changed=copy.deepcopy(steps);changed[10]['expectedSchema']['properties']['value']['properties']['completed']={'type':'integer','minimum':0,'maximum':1000000}
        contractfile.write_text(json.dumps({**contract,'steps':changed}))
        assert invoke(status=1,diagnostic='TEST_SCENARIO_SCHEMA_RESULT')['code']=='TOOL_FAILED'
    elif name == 'typed-outcomes':
        contractfile.write_text(json.dumps({**contract,'expected':{'opened':0}}))
        assert invoke(status=2)['code']=='TEST_RESPONSE'
        changed=copy.deepcopy(steps);changed[7]['expected']['value']['opened']=1
        contractfile.write_text(json.dumps({**contract,'steps':changed}))
        assert invoke(status=2)['code']=='TEST_SCENARIO_RESPONSE'
        changed=copy.deepcopy(steps);changed[0]['expected']['value']['total']=64.0
        contractfile.write_text(json.dumps({**contract,'steps':changed}))
        assert invoke()['kind']=='tested', 'JSON numeric spelling must not change equal numeric values'
    elif name == 'trace-guards':
        changed=copy.deepcopy(steps);changed[-1]['views'][0]['viewId']='different'
        contractfile.write_text(json.dumps({**contract,'steps':changed}))
        assert invoke(status=2)['code']=='TEST_SCENARIO_VIEWS'
        changed=copy.deepcopy(steps);changed[0]['expected']['value']['title']='wrong title'
        contractfile.write_text(json.dumps({**contract,'steps':changed}))
        assert invoke(status=2)['code']=='TEST_SCENARIO_RESPONSE'
    return report


CASES=['rpc-only','typed-outcomes','official','contracts','registry-guards','schema-guards','trace-guards']
NODE_CASES=['official-node','schema-guards-node','interruptions-node']
MUTATIONS=[
    ('official-beta','official','plugins/official/views/executor/index.ts','["beta", 5, 10]','["beta", 5, 11]',None),
    ('official-cancel','official','plugins/official/views/executor/index.ts','phase = "cancelling"; sequence++','phase = "running"; sequence++','TEST_SCENARIO_SCHEMA_RESULT'),
    ('registry-bit','official','scripts/gterm-plugin',"(8 if views.get('registry') else 0)",'0','ASSERT: pair Client adoption'),
    ('registry-grant','official','scripts/probes/plugin-testkit-views.inc','i == 3 ? !kit_view_registry : !(kit_view_kinds & (1u << i))','i == 3 ? true : !(kit_view_kinds & (1u << i))','ASSERT: pair Client adoption'),
    ('scenario-direction','official','scripts/probes/plugin-testkit-scenario.inc','methods[j].receiver == GT_EXT_EXECUTOR ? host : peer','methods[j].receiver == GT_EXT_EXECUTOR ? peer : host','ASSERT: pair control request admission'),
    ('scenario-close','official','scripts/probes/plugin-testkit-scenario.inc','gt_ext_runtime_view_close(client, handle) == GT_OK','client != NULL','ASSERT: late close cannot remove a reopened view'),
    ('scenario-stale','official','scripts/probes/plugin-testkit-scenario.inc','GT_ERR_NOT_FOUND,\n                  "late close','GT_OK,\n                  "late close','ASSERT: late close cannot remove a reopened view'),
    ('scenario-types','typed-outcomes','scripts/gterm-plugin','return type(a) is type(b) and a == b','return a == b',None),
    ('scenario-async-schema','schema-guards','scripts/plugin-dev-scenarios.mjs','validators.some(validate => validate.$async)','false',None),
    ('scenario-coercion','schema-guards','scripts/plugin-dev-scenarios.mjs','coerceTypes: false','coerceTypes: true',None),
    ('scenario-exact','trace-guards','scripts/gterm-plugin',"require(json_equal(observed.get('outcome'), bind(step['expected'])), 'TEST_SCENARIO_RESPONSE')","require(True, 'TEST_SCENARIO_RESPONSE')",None),
    ('scenario-schema','schema-guards','scripts/plugin-dev-scenarios.mjs','!validate(checks[index])','false && !validate(checks[index])',None),
    ('scenario-view-snapshot','trace-guards','scripts/gterm-plugin',"sorted(actual, key=lambda s: s['viewId']) == sorted(step['views'], key=lambda s: s['viewId'])",'True',None),
    ('scenario-input-limit','contracts','scripts/gterm-plugin',"size <= 8192 and total <= 32768",'True','ASSERT: bound request input'),
]


def mutation_checks(work, tools, core, selected):
    isolated=work/'isolated'; isolated.mkdir();(isolated/'tmp').mkdir()
    for name in ['packages/plugin-sdk','core/include','plugins/official/views']:
        shutil.copytree(ROOT/name,isolated/name)
    names=['scripts/test-plugin-dev-scenarios.py','scripts/gterm-plugin','scripts/plugin-dev-worker.mjs','scripts/plugin-dev-scenarios.mjs','scripts/lib/gt-timeout.sh','core/tests/test_ext_session.c','LICENSE']
    names += [p.relative_to(ROOT).as_posix() for p in (ROOT/'scripts/probes').glob('plugin*testkit*') if p.is_file()]
    names += ['scripts/fixtures/plugin-dev-tools/package.json','scripts/fixtures/plugin-dev-tools/package-lock.json','core/src/base/allocator.h','core/src/base/sha256.h']
    for name in names:
        target=isolated/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/name,target)
    build=isolated/'tmp/core';build.mkdir()
    for name in ['CMakeCache.txt','libghosterm_core.a','libghosterm_quickjs.a']:os.link(core/name,build/name)
    for label,_case,name,old,_new,_diagnostic in selected: assert (isolated/name).read_text().count(old)==1,(label,'mutation marker')
    proof=[]
    for label,case,name,old,new,diagnostic in selected:
        target=isolated/name;before=target.read_bytes()
        command=['python3',str(isolated/'scripts/test-plugin-dev-scenarios.py'),'--tools',str(tools),'--core-build',str(build),'--case',case]
        def execute():return bounded(command,180)
        baseline=execute();assert baseline.returncode==0,(label,baseline.stderr.decode())
        target.write_bytes(before.replace(old.encode(),new.encode()))
        try:
            red=execute();data=red.stdout+red.stderr
            assert red.returncode==1 and b'AssertionError' in data and b'error: ' not in data and b'Traceback' in data,(label,data.decode())
            if diagnostic:assert diagnostic.encode() in data,(label,data.decode())
            else:assert b'ASSERT:' not in data and b'\"code\":\"TOOL_FAILED\"' not in data,(label,data.decode())
            control=subprocess.run([str(core/'test_config'),str(ROOT/'core/tests/fixtures'),str(work)],capture_output=True,timeout=10)
            assert control.returncode==0,(label,control.stderr.decode())
        finally:target.write_bytes(before)
        restored=execute();assert restored.returncode==0 and sha(target)==hashlib.sha256(before).hexdigest(),(label,restored.stderr.decode())
        proof.append(dict(case=label,targetAssertionRed=True,configurationGreen=True,restoredGreen=True,sourceSHA256=sha(target),redSHA256=hashlib.sha256(data).hexdigest()))
        print('MUTATION PASS '+label,flush=True)
    return proof


def main():
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('--tools',type=Path,required=True);parser.add_argument('--core-build',type=Path,required=True)
    parser.add_argument('--case',choices=CASES+NODE_CASES+['hold-node']);parser.add_argument('--control-file',type=Path);parser.add_argument('--execute',action='store_true');parser.add_argument('--node-executable',type=Path);parser.add_argument('--node-sdk',type=Path)
    parser.add_argument('--mutate',action='store_true');parser.add_argument('--mutation-case',action='append',choices=[m[0] for m in MUTATIONS]);parser.add_argument('--receipt',type=Path)
    args=parser.parse_args();tools=args.tools.resolve();core=args.core_build.resolve();execution=None;binary={}
    if args.execute:
        if not args.node_executable or not args.node_sdk:parser.error('--execute requires explicit node and SDK')
        node=args.node_executable.resolve(strict=True);sdk=args.node_sdk.resolve(strict=True);catalog=hashlib.sha256()
        for p in sorted((p for p in sdk.rglob('*') if p.is_file()),key=lambda p:p.relative_to(sdk).as_posix().encode()):
            name=p.relative_to(sdk).as_posix().encode();catalog.update(len(name).to_bytes(4,'big')+p.stat().st_size.to_bytes(8,'big')+name+sha(p).encode());binary[str(p)]=sha(p)
        binary[str(node)]=sha(node)
        version=subprocess.check_output([node,'--version'],text=True,timeout=5).strip().removeprefix('v')
        execution=dict(runtime='node',nodeExecutable=str(node),nodeSHA256=sha(node),nodeSDKDirectory=str(sdk),nodeSDKCatalogSHA256=catalog.hexdigest(),version=version)
    if args.case == 'hold-node' and (not args.control_file or execution is None):parser.error('internal hold requires an owned marker and explicit Node')
    if args.case != 'hold-node' and args.control_file:parser.error('marker is internal to hold')
    if args.case in NODE_CASES and execution is None:parser.error('Node cases require --execute')
    if args.mutation_case and not args.mutate:parser.error('--mutation-case requires --mutate')
    sources=[ROOT/n for n in ['scripts/test-plugin-dev-scenarios.py','scripts/gterm-plugin','scripts/plugin-dev-worker.mjs','scripts/plugin-dev-scenarios.mjs','scripts/lib/gt-timeout.sh','core/tests/test_ext_session.c','core/src/base/sha256.h','LICENSE']]
    for directory in ['core/include','packages/plugin-sdk','plugins/official/views','scripts/fixtures/plugin-dev-tools']:
        sources += [p for p in (ROOT/directory).rglob('*') if p.is_file()]
    sources += [p for p in (ROOT/'scripts/probes').glob('plugin*testkit*') if p.is_file()]
    inputs={str(p.relative_to(ROOT)):sha(p) for p in sorted(set(sources))}
    cases=[args.case] if args.case else CASES+(NODE_CASES if execution else [])
    with tempfile.TemporaryDirectory(prefix='dev-scenario-',dir=ROOT/'tmp') as d:
        work=Path(d)
        for case in cases:
            item=work/case;item.mkdir();run_case(case,item,tools,core,execution,args.control_file.resolve() if args.control_file else None);print('PASS '+case,flush=True)
        selected=[m for m in MUTATIONS if args.mutation_case is None or m[0] in args.mutation_case]
        proof=mutation_checks(work,tools,core,selected) if args.mutate else []
    assert all(sha(ROOT/name)==digest for name,digest in inputs.items()),'source changed'
    assert all(sha(Path(name))==digest for name,digest in binary.items()),'fixed tool changed'
    if args.receipt:args.receipt.write_text(json.dumps(dict(inputs=inputs,binaryInputs=binary,cases=cases,mutations=proof,coreSHA256=sha(core/'libghosterm_core.a'),quickjsSHA256=sha(core/'libghosterm_quickjs.a')),indent=2)+'\n')

if __name__=='__main__':
    def interrupted(number,_frame):raise SystemExit(128+number)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
