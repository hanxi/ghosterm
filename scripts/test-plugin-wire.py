#!/usr/bin/env python3
"""C wire conformance against the shared candidate and raw-byte adversarial input.

Default is stdlib + built C binary, including in CTest. --reference additionally
compares schema instances with pinned Ajv; its npm cache is offline unless the
user explicitly supplies --fetch. Raw lexical policy cases are kept separate.
"""
from pathlib import Path
import argparse,base64,copy,hashlib,json,os,random,shutil,signal,struct,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
KINDS=['offer','request','response','event','cancel','credit','resource.begin','resource.chunk','resource.end','close','hello','probe','accept','ready']

def fixtures():
    source=json.loads((ROOT/'packages/plugin-sdk/schema/bridge-v1.cases.json').read_text());cases=[]
    def encoded(frame):return json.dumps(frame,ensure_ascii=False,separators=(',',':')).encode()
    def add(name,frame,valid=True,reason=None,reference=True):
        raw=encoded(frame) if isinstance(frame,dict) else frame
        cases.append(dict(name=name,raw=raw,valid=valid,reason=0 if valid else 5 if reason is None else reason,reference=reference,frame=frame if valid and isinstance(frame,dict) else None))
    originals={name:{**copy.deepcopy(source['base']),**copy.deepcopy(message)} for name,message in source['messages'].items()}
    for name,frame in originals.items():add('valid-'+name,frame)
    for patch in source['patches']:
        frame=copy.deepcopy(originals[patch['from']]);target=frame
        for key in patch['path'][:-1]:target=target[key]
        if patch.get('remove'):del target[patch['path'][-1]]
        else:target[patch['path'][-1]]=patch['value']
        add(patch['name'],frame,patch['valid'])
    request=originals['request'];offer=originals['offer'];chunk=originals['resource.chunk']
    def change(name,frame,path,value,valid=True,**kwargs):
        frame=copy.deepcopy(frame);target=frame
        for key in path[:-1]:target=target[key]
        target[path[-1]]=value;add(name,frame,valid,**kwargs)
    rng=random.Random(0x475457495245)
    for i,value in enumerate([0,1,2**53-1,2**53,2**63-1,2**63,2**64-1]+[rng.randrange(2**64) for _ in range(80)]):
        change('uint64-'+str(i),request,['seq'],str(value))
    for value in ['','00','01','-0','+1',' 1','1 ','1.0','1e0','18446744073709551616','99999999999999999999','١','１','1\x00']:
        change('bad-counter-'+str(len(cases)),request,['seq'],value,False)
    for i,(value,valid) in enumerate([('1.2.3-alpha.1+build.9',True),('1.2.3-0',True),('1.2.3-01',False),('01.2.3',False),('1.2.3+',False),('1.2.3-a..b',False),('1.2.3+01',True),('1.2.3-12a',True),('1.2.3-中文',False)]):
        change('semver-'+str(i),offer,['payload','pluginVersion'],value,valid)
    for i,value in enumerate(['AA==','AQ==','AAA=','AAQ=','AAAA','////','+/8=']):
        change('base64-valid-'+str(i),chunk,['payload','dataBase64'],value)
    for i,value in enumerate(['AB==','AAB=','A===','AAA','====','AA=A','AA==\n','A A=']):
        change('base64-invalid-'+str(i),chunk,['payload','dataBase64'],value,False)
    change('unicode-display-256',originals['resource.begin'],['payload','displayName'],'😀'*256)
    change('unicode-display-257',originals['resource.begin'],['payload','displayName'],'😀'*257,False)
    change('unknown-input-keys',request,['payload','input'],{'strange\x00key':True,'a':1,'nested':{'a':2},'__proto__':{'x':3}})
    change('duplicate-spelling-distinct',request,['payload','input'],{'é':1,'e\u0301':2})
    def payload(depth):
        choices=[None,True,False,rng.randrange(-100000,100000),rng.uniform(-10,10),''.join(rng.choice(['a','界','😀','\x00','\n','"','\\','\u2028']) for _ in range(20))]
        if depth:
            choices.extend([[payload(depth-1) for _ in range(rng.randrange(4))],
                            {str(i):payload(depth-1) for i in range(rng.randrange(4))}])
        return rng.choice(choices)
    for i in range(60):
        frame=copy.deepcopy(request);frame['payload']['input']=payload(3)
        keys=list(frame);rng.shuffle(keys);frame={key:frame[key] for key in keys}
        if i%2:
            add('json-tree-'+str(i),frame)
        else:
            add('json-tree-'+str(i),json.dumps(frame,ensure_ascii=True,separators=(',',':')).encode())
            cases[-1]['frame']=frame
    def raw_case(name,raw,valid=False,reason=1,frame=None):
        add(name,raw,valid,reason,False)
        if valid:cases[-1]['frame']=frame or request
    raw=encoded(request)
    raw_case('escaped-root-key',raw.replace(b'"wireVersion"',b'"wire\\u0056ersion"'),True,frame=request)
    raw_case('duplicate-root',raw[:-1]+b',"seq":"0"}',reason=3)
    raw_case('duplicate-escaped-root',raw[:-1]+b',"s\\u0065q":"0"}',reason=3)
    raw_case('duplicate-input',raw.replace('"answer":"终端"'.encode(),b'"a":1,"a":2'),reason=3)
    raw_case('duplicate-unicode',raw.replace('"answer":"终端"'.encode(),b'"\\ud83d\\ude00":1,"'+ '😀'.encode()+b'":2'),reason=3)
    for name,bad in [('overlong',b'\xc0\xaf'),('surrogate',b'\xed\xa0\x80'),('range',b'\xf4\x90\x80\x80'),('continuation',b'\x80'),('truncated',b'\xf0\x9f')]:
        raw_case('utf8-'+name,raw.replace('终端'.encode(),bad),reason=2)
    for name,bad in [('high',b'\\ud800'),('low',b'\\udc00'),('bad-pair',b'\\ud800\\u0041')]:
        raw_case('surrogate-'+name,raw.replace('终端'.encode(),bad),reason=2)
    changed=copy.deepcopy(request);changed['payload']['input']['answer']='😀\x00\n'
    raw_case('surrogate-pair-valid',encoded(changed).replace('😀'.encode(),b'\\ud83d\\ude00'),True,frame=changed)
    raw_case('raw-control',raw.replace('终端'.encode(),b'\x01'))
    for name,suffix in [('trailing',b' false'),('nul',b'\0'),('second-object',b'{}')]:raw_case(name,raw+suffix)
    raw_case('bom',b'\xef\xbb\xbf'+raw)
    raw_case('whitespace',b' \t\r\n'+raw+b' \n',True,frame=request)
    for name,token in [('leading-zero',b'01'),('no-integer',b'.1'),('no-fraction',b'1.'),('no-exponent',b'1e'),('plus-number',b'+1'),('nan',b'NaN'),('infinity',b'Infinity')]:
        raw_case('number-'+name,raw.replace(b'"wireVersion":1',b'"wireVersion":'+token))
    for name,token in [('decimal',b'1.0'),('exponent',b'1e0'),('negative-exponent',b'10e-1'),('huge-coefficient',b'1'+b'0'*100+b'e-100')]:
        raw_case('integer-'+name,raw.replace(b'"wireVersion":1',b'"wireVersion":'+token),True,frame=request)
    for name,token in [('rounded',b'1.00000000000000000001'),('huge-exponent',b'1e9999999999999999999'),('small-exponent',b'1e-9999999999999999999'),('negative',b'-1')]:
        raw_case('integer-'+name,raw.replace(b'"wireVersion":1',b'"wireVersion":'+token),reason=5)
    # Exact limits are policy outside JSON Schema, including business payloads.
    for depth,valid in [(30,True),(31,False)]:
        frame=copy.deepcopy(request);node=0
        for _ in range(depth):node=[node]
        frame['payload']['input']=node;add('depth-'+str(depth),frame,valid,reason=4,reference=False)
    for elements,valid in [(4083,True),(4084,False)]:
        change('nodes-'+str(elements),request,['payload','input'],[None]*elements,valid,reason=4,reference=False)
    for size,valid in [(65536,True),(65537,False)]:
        data=raw+b' '*(size-len(raw));raw_case('bytes-'+str(size),data,valid,reason=4,frame=request)
    for index in range(len(raw)):
        try:raw[:index].decode('utf8');reason=1
        except UnicodeDecodeError:reason=2
        raw_case('truncated-'+str(index),raw[:index],reason=reason)
    raw_case('trailing-comma',raw[:-1]+b',}')
    raw_case('invalid-escape',raw.replace('终端'.encode(),b'\\x41'))
    raw_case('bad-unicode-hex',raw.replace('终端'.encode(),b'\\u12xz'))
    for case in cases:
        if case['valid'] and case['frame'] is None:case['frame']=json.loads(case['raw'])
    assert len({case['name'] for case in cases})==len(cases)
    return cases

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,default=ROOT/'core/build/test_bridge_wire')
    parser.add_argument('--reference',action='store_true');parser.add_argument('--fetch',action='store_true')
    parser.add_argument('--case',action='append');parser.add_argument('--expect-assertion',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args();cases=fixtures()
    if args.case:
        assert set(args.case)<={case['name'] for case in cases};cases=[case for case in cases if case['name'] in args.case]
    assert not args.expect_assertion or len(cases)==1
    work=Path(tempfile.mkdtemp(prefix='plugin-wire-',dir=ROOT/'tmp'))
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    try:
        reference_count=0
        if args.reference:
            tools=work/'tools';tools.mkdir();fixture=ROOT/'scripts/fixtures/plugin-schema-tools'
            for name in ('package.json','package-lock.json'):shutil.copyfile(fixture/name,tools/name)
            (work/'npmrc').touch();env.update(NPM_CONFIG_USERCONFIG=str(work/'npmrc'),NPM_CONFIG_UPDATE_NOTIFIER='false')
            command=['npm','ci','--prefix',str(tools),'--ignore-scripts','--no-audit','--no-fund','--cache',str(ROOT/'tmp/plugin-schema-npm-cache')]
            if not args.fetch:command.append('--offline')
            subprocess.run(command,env=env,check=True,capture_output=True,timeout=60)
            vector=[dict(name=c['name'],bytes=base64.b64encode(c['raw']).decode(),valid=c['valid']) for c in cases if c['reference']]
            path=work/'reference.json';path.write_text(json.dumps(vector));reference_count=len(vector)
            p=subprocess.run(['node',str(ROOT/'scripts/probes/plugin-wire-reference.mjs'),str(ROOT/'packages/plugin-sdk/schema/bridge-v1.candidate.json'),str(path),str(tools)],env=env,capture_output=True,text=True,timeout=30)
            (work/'reference.log').write_text(p.stdout+p.stderr)
            if p.returncode:raise RuntimeError(p.stdout+p.stderr)
            print(p.stdout,end='',flush=True)
        data=bytearray()
        for case in cases:
            name=case['name'].encode();assert 0<len(name)<256
            data+=bytes([len(name)])+name+bytes([case['valid'],case['reason']])+struct.pack('>I',len(case['raw']))
            if case['valid']:
                f=case['frame'];stream=f['streamId'].encode();assert len(stream)<129
                data+=struct.pack('>QQB',int(f['epoch']),int(f['seq']),KINDS.index(f['kind']))+f['connectionId'].encode()+f['instanceId'].encode()+stream+b'\0'*(129-len(stream))
            data+=case['raw']
        p=subprocess.run([str(args.binary.resolve()),'cases'],input=bytes(data),env=env,capture_output=True,timeout=30)
        output=(p.stdout+p.stderr).decode(errors='replace');(work/'client.log').write_text(output)
        if args.expect_assertion:
            if p.returncode!=1 or 'FAIL ' not in output or any(x in output for x in ('AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer')):raise RuntimeError('Expected normal C assertion: '+output)
            print('ASSERTION RED '+cases[0]['name'],flush=True)
        elif p.returncode or 'PASS bridge_wire '+str(len(cases))+' cases' not in output:raise RuntimeError(output)
        else:print(output.splitlines()[-1],flush=True)
        (work/'result.json').write_text(json.dumps(dict(accepted=True,cases=[c['name'] for c in cases],reference_instances=reference_count,expected_assertion=args.expect_assertion,binary_sha256=hashlib.sha256(args.binary.read_bytes()).hexdigest()),indent=2)+'\n')
        print('PLUGIN WIRE PASS; logs: '+str(work),flush=True)
    finally:shutil.rmtree(work/'tools',ignore_errors=True)

if __name__=='__main__':
    def interrupted(number,frame):raise KeyboardInterrupt('signal '+str(number))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted);main()
