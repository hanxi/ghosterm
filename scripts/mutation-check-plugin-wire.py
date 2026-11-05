#!/usr/bin/env python3
"""Isolated compiled wire mutations; genuine C assertions, config controls,
full restoration and input digests. No crash or timeout counts as a red.
"""
from pathlib import Path
import sys
import hashlib,json,os,shutil,signal,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_PLUGIN_WIRE_MUTATION'
INPUTS=['core/src/extension/wire.c','core/src/extension/wire.h','core/src/extension/wire-schema.inc',
        'core/src/extension/frame.h','core/src/base/secure-random.h','core/tests/test_bridge_wire.c','core/CMakeLists.txt',
        'packages/plugin-sdk/schema/bridge-v1.candidate.json','packages/plugin-sdk/schema/bridge-v1.cases.json',
        'scripts/generate-plugin-wire.py','scripts/test-plugin-wire.py','scripts/probes/plugin-wire-reference.mjs',
        'scripts/fixtures/plugin-schema-tools/package.json','scripts/fixtures/plugin-schema-tools/package-lock.json',
        'scripts/mutation-check-plugin-wire.py']
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-wire-mutations-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir()
    source=work/'source';build=work/'build';hashes={name:digest(ROOT/name) for name in INPUTS};commands=[]
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    def run(label,args,timeout=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-wire-mutations; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-wire-mutations',str(ROOT),str(timeout),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=timeout+10)
        output=p.stdout+p.stderr;(logs/(label+'.log')).write_text(output);commands.append(dict(label=label,returncode=p.returncode))
        if p.returncode:raise RuntimeError(label+': '+output)
        return output
    def rebuild(label):
        # GNU make can rebuild .o/.a within one second without relinking tests.
        binaries=['test_bridge_wire', 'test_config']
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        objects=[('ghosterm_core','src/extension/wire.c'),('test_bridge_wire','tests/test_bridge_wire.c')]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_bridge_wire','test_config','-j','2'])
        link_log=(logs/(label+'-build.log')).read_text()
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in link_log:raise RuntimeError('Missing actual link: '+label+' '+marker)
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:
            if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in output:raise RuntimeError('Missing actual compilation: '+label+' '+owner)
    def suite(label,mode=None,red=False):
        args=['python3',ROOT/'scripts/test-plugin-wire.py','--binary',build/'test_bridge_wire']
        if mode:args+=['--case',mode]
        if red:args+=['--expect-assertion']
        output=run(label,args)
        if red and ('ASSERTION RED '+mode not in output or any(x in output for x in ('AddressSanitizer','runtime error:'))):raise RuntimeError('Missing normal C assertion: '+label)
    def control(label):run(label+'-control',[build/'test_config',source/'tests/fixtures'])
    try:
        for name in ('core/src/extension/wire.c','core/src/extension/wire.h'):
            if MARK.encode() in (ROOT/name).read_bytes():print('Residual mutation: '+name,file=sys.stderr);raise SystemExit(2)
        run('generated',['python3',ROOT/'scripts/generate-plugin-wire.py','--check'])
        shutil.copytree(ROOT/'core',source,ignore=shutil.ignore_patterns('build','build-*'))
        run('configure',['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');control('baseline')
        cases=[
          ('decoded-duplicates','wire.c',[('return fail(d,GT_BRIDGE_WIRE_DUPLICATE);','return true;')],'duplicate-escaped-root'),
          ('key-scope','wire.c',[('UINT64_C(14695981039346656037)^n->parent','UINT64_C(14695981039346656037)'),('o->parent==n->parent && o->key_size','o->parent && n->parent && o->key_size')],'valid-response-success'),
          ('utf8-surrogate','wire.c',[('cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)','cp<minimum || cp>0x10ffff')],'utf8-surrogate'),
          ('lone-surrogate','wire.c',[('else if(cp>=0xdc00 && cp<=0xdfff)','else if(cp>0xdc00 && cp<=0xdfff)')],'surrogate-low'),
          ('byte-limit','wire.c',[('if(size>GT_BRIDGE_FRAME_MAX)','if(false)')],'bytes-65537'),
          ('node-limit','wire.h',[('#define GT_BRIDGE_WIRE_NODES_MAX 4096u','#define GT_BRIDGE_WIRE_NODES_MAX 4097u')],'nodes-4084'),
          ('depth-limit','wire.h',[('#define GT_BRIDGE_WIRE_DEPTH_MAX 32u','#define GT_BRIDGE_WIRE_DEPTH_MAX 33u')],'depth-31'),
          ('counter-overflow','wire.c',[('if(value>(UINT64_MAX-v)/10)return false;','/* unchecked overflow */')],'counter-overflow'),
          ('exact-integer','wire.c',[('if(shift<0 && (uint64_t)(-shift)>digits-1-last)return false;','(void)last; /* rounded fractional protocol value */')],'integer-rounded'),
          ('schema-bypass','wire.c',[('if(!wire_schema_0(d,1))','if(false && !wire_schema_0(d,1))')],'unknown-root-field'),
          ('base64-padbits','wire.c',[('if(padding==2 && (n<4 || (sextet(s[n-3])&15)))return false;','if(padding==2 && n<4)return false;')],'base64-invalid-0'),
          ('semver-prerelease','wire.c',[('numeric && at-start>1','numeric && at-start>2')],'semver-2'),
          ('failed-output','wire.c',[('if(out)memset(out,0,sizeof(*out));if(!d)','(void)out;if(!d)')],'duplicate-root'),
          ('failed-view','wire.c',[('d->allocator=a;d->input=bytes;','d->allocator=a;d->valid=true;d->input=bytes;')],'duplicate-root'),
          ('wiped-release','wire.c',[('gt_allocator_t a=d->allocator;gt__secure_erase(d,sizeof(*d));a.release(a.context,d,sizeof(*d));','gt_allocator_t a=d->allocator;a.release(a.context,d,sizeof(*d));')],'valid-request'),
          ('allocator-release','wire.c',[('gt_allocator_t a=d->allocator;gt__secure_erase(d,sizeof(*d));a.release(a.context,d,sizeof(*d));','gt_allocator_t a=d->allocator;gt__secure_erase(d,sizeof(*d));(void)a;')],'valid-request'),
        ]
        results=[]
        for label,name,changes,mode in cases:
            path=source/'src/extension'/name;original=path.read_bytes();text=original.decode()
            for old,new in changes:
                if text.count(old)!=1:raise RuntimeError('Ambiguous mutation anchor: '+label)
                text=text.replace(old,new+' /* '+MARK+' */')
            path.write_text(text);rebuild(label);suite(label+'-target',mode,True);control(label)
            path.write_bytes(original)
            if digest(path)!=hashlib.sha256(original).hexdigest():raise RuntimeError('Restore mismatch: '+label)
            rebuild(label+'-restored');suite(label+'-restored')
            run(label+'-generated',['python3',ROOT/'scripts/generate-plugin-wire.py','--check','--output',source/'src/extension/wire-schema.inc'])
            results.append(label);print(label+': compiled C assertion red / config green / full wire restored green / SHA256',flush=True)
        if any(digest(ROOT/name)!=sha for name,sha in hashes.items()):raise RuntimeError('Worktree changed during isolated mutations')
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=results,inputs_sha256=hashes,commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        print('PLUGIN WIRE MUTATIONS PASS: '+str(len(results))+' groups; logs: '+str(logs),flush=True)
    finally:shutil.rmtree(source,ignore_errors=True);shutil.rmtree(build,ignore_errors=True)
if __name__=='__main__':
    def interrupted(number,frame):raise KeyboardInterrupt('signal '+str(number))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted);main()
