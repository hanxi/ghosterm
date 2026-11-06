#!/usr/bin/env python3
"""Compiled storage/hash mutations. Normal C assertion red, independent config
green, restore all native storage cases and input digests. Workspace only tmp.
"""
from pathlib import Path
import sys
import hashlib,json,os,shutil,signal,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_RESOURCE_MUTATION'
MODES='roundtrip partial unknown pins integrity offset budget quota scope seal malformed filesystem allocation io-write io-read io-cleanup cleanup-race exhaustion hash crash shared large'.split()
INPUTS=sorted({str(p.relative_to(ROOT)) for folder in ('core/src','core/include') for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')})+['core/CMakeLists.txt','core/tests/test_bridge_resource.c','scripts/test-plugin-resource.py','scripts/mutation-check-plugin-resource.py']
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-resource-mutations-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();source=work/'source';build=work/'build';fixtures=work/'fixtures';fixtures.mkdir()
    hashes={name:digest(ROOT/name) for name in INPUTS};commands=[]
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    def run(label,args,red=False,timeout=120):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-resource-mutations; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-resource-mutations',str(ROOT),str(timeout),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=timeout+10)
        output=p.stdout+p.stderr;(logs/(label+'.log')).write_text(output);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in output or any(x in output for x in ('AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer')):raise RuntimeError(label+': expected normal C assertion; '+output)
        elif p.returncode:raise RuntimeError(label+': '+output)
    def rebuild(label):
        # GNU make can rebuild .o/.a within one second without relinking tests.
        binaries=['test_bridge_resource', 'test_config']
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        objects=[('ghosterm_core','src/extension/resource.c'),('ghosterm_core','src/base/sha256.c'),('test_bridge_resource','tests/test_bridge_resource.c')]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_bridge_resource','test_config','-j','2'])
        link_log=(logs/(label+'-build.log')).read_text()
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in link_log:raise RuntimeError('Missing actual link: '+label+' '+marker)
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:
            if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in output:raise RuntimeError('Missing actual compilation: '+label+' '+owner)
    def suite(label):
        for mode in MODES:run(label+'-'+mode,[build/'test_bridge_resource',mode,fixtures],timeout=15)
        run(label+'-hash-reference',['python3',ROOT/'scripts/test-plugin-resource.py','--binary',build/'test_bridge_resource'])
    cases=[
      ('unpublished-read','resource.c','if(item->state!=RESOURCE_PUBLISHED)return GT_BRIDGE_RESOURCE_CLOSED;if(offset>', 'if(false && item->state!=RESOURCE_PUBLISHED)return GT_BRIDGE_RESOURCE_CLOSED;if(offset>','roundtrip'),
      ('computed-digest','resource.c','if(!matched)return resource_fail','if(!matched && false)return resource_fail','integrity'),
      ('declared-digest','resource.c',' || memcmp(digest,item->descriptor.sha256,65))return resource_fail',' )return resource_fail','integrity'),
      ('known-length','resource.c','(item->descriptor.size_known && size!=item->descriptor.size)','(false && item->descriptor.size_known && size!=item->descriptor.size)','integrity'),
      ('source-complete','resource.c','if(!complete || size!=item->received','if((false && !complete) || size!=item->received','integrity'),
      ('unknown-reservation','resource.c','d->size_known?d->size:s->config.resource_bytes','d->size_known?d->size:0','unknown'),
      ('total-budget','resource.c','if(reserve>s->config.total_bytes-s->reserved ||','if((false && reserve>s->config.total_bytes-s->reserved) ||','shared'),
      ('scope-budget','resource.c',' || reserve>scope->budget-scope->reserved ||',' || (false && reserve>scope->budget-scope->reserved) ||','scope'),
      ('slot-quota','resource.c',[(' || s->resources==s->config.slots',' || (false && s->resources==s->config.slots)'),('resource_item_t *item=NULL;for(size_t i=0;i<s->config.slots;i++)','resource_item_t *item=NULL;for(size_t i=0;i<GT_BRIDGE_RESOURCE_SLOTS;i++)')],None,'quota'),
      ('snapshot-charge','resource.c','s->reserved-=item->reserved-size;scope->reserved-=item->reserved-size;item->reserved=size;','s->reserved-=item->reserved;scope->reserved-=item->reserved;item->reserved=0;','unknown'),
      ('retained-pins','resource.c','if(item->references>1){','if(false && item->references>1){','pins'),
      ('generation-highwater','resource.c','if(d->generation<=scope->last_generation)','if(false && d->generation<=scope->last_generation)','budget'),
      ('native-lease','resource.c',' && !memcmp(a->token,b->token,33)','', 'scope'),
      ('native-reference','resource.c',' && !memcmp(s->items[i].ref.token,ref->token,33)','', 'scope'),
      ('seal-staging','resource.c','if(resource_drop(s,scope,&s->items[i])!=GT_BRIDGE_RESOURCE_OK)','if(false)','seal'),
      ('seal-new-traffic','resource.c','if(scope->sealed)return GT_BRIDGE_RESOURCE_CLOSED','if(false && scope->sealed)return GT_BRIDGE_RESOURCE_CLOSED','seal'),
      ('offset','resource.c','if(offset!=item->received)','if(false && offset!=item->received)','offset'),
      ('cumulative-bytes','resource.c','if(size>item->reserved-item->received)','if(false && size>item->reserved-item->received)','offset'),
      ('partial-write-hash','resource.c','gt__sha256_update(&item->hash,data,(size_t)n)','gt__sha256_update(&item->hash,data,size)','partial'),
      ('write-zero','resource.c','if(n<=0 || (size_t)n>size)','if(n<0 || (size_t)n>size)','io-write'),
      ('write-eintr','resource.c','if(n<0 && errno==EINTR)return GT_BRIDGE_RESOURCE_WAIT;','if(n<0 && errno==EINTR)return GT_BRIDGE_RESOURCE_IO;','io-write'),
      ('read-zero','resource.c','if(n<=0 || (size_t)n>capacity)','if(n<0 || (size_t)n>capacity)','io-read'),
      ('private-directory','resource.c',' || (st.st_mode&07777)!=0700','', 'filesystem'),
      ('readonly-snapshot','resource.c','name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC','name,O_RDWR|O_NOFOLLOW|O_CLOEXEC','roundtrip'),
      ('anonymous-file','resource.c','if(unlinkat(s->config.directory_fd,name,0))','if(false && unlinkat(s->config.directory_fd,name,0))','roundtrip'),
      ('failed-cleanup-charge','resource.c',' || unlinkat(s->config.directory_fd,item->name,0))return GT_BRIDGE_RESOURCE_IO;',' || unlinkat(s->config.directory_fd,item->name,0)){item->name[0]=0;}','io-cleanup'),
      ('cleanup-identity','resource.c','st.st_dev!=item->device || st.st_ino!=item->inode || !S_ISREG(st.st_mode) || unlinkat','false || unlinkat','cleanup-race'),
      ('owner-erase','resource.c','gt__secure_erase(s,sizeof(*s));gt__plugin_memory_free(&a,s,sizeof(*s));','a.release(a.context,s,sizeof(*s));','allocation'),
      ('sha-round','sha256.c','0x428a2f98,','0x428a2f99,','hash'),
      ('sha-bit-length','sha256.c','uint64_t bits=s->bytes*8;','uint64_t bits=s->bytes;','hash'),
      ('sha-overflow','sha256.c',' || n>UINT64_MAX/8-s->bytes','', 'hash'),
      ('sha-final-state','sha256.c','s->ended=true;return true;','s->ended=false;return true;','hash')]
    try:
        for name in ('core/src/extension/resource.c','core/src/base/sha256.c'):
            if MARK.encode() in (ROOT/name).read_bytes():print('Residual mutation: '+name,file=sys.stderr);raise SystemExit(2)
        shutil.copytree(ROOT/'core',source,ignore=shutil.ignore_patterns('build','build-*'))
        run('configure',['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');run('baseline-config',[build/'test_config',source/'tests/fixtures'])
        for label,file,old,new,mode in cases:
            path=source/'src'/('base' if file=='sha256.c' else 'extension')/file;before=path.read_bytes();original=before.decode()
            mutated=original
            for anchor,replacement in old if isinstance(old,list) else [(old,new)]:
                if anchor not in mutated:raise RuntimeError('Missing mutation anchor: '+label)
                mutated=mutated.replace(anchor,replacement)
            path.write_text(mutated+'\n/* '+MARK+' */\n')
            try:
                rebuild(label);run(label+'-target',[build/'test_bridge_resource',mode,fixtures],red=True,timeout=15)
                run(label+'-config',[build/'test_config',source/'tests/fixtures'])
            finally:path.write_bytes(before)
            rebuild(label+'-restored');suite(label+'-restored');assert digest(path)==hashlib.sha256(before).hexdigest()
            for child in fixtures.iterdir():
                if child.is_dir():shutil.rmtree(child)
                else:child.unlink()
            print(label+': compiled C assertion red / config green / restored 22 storage + independent hash green / SHA256',flush=True)
        for name,h in hashes.items():
            if digest(ROOT/name)!=h:raise RuntimeError('Input changed: '+name)
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],inputs_sha256=hashes,commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        shutil.rmtree(source);shutil.rmtree(build);shutil.rmtree(fixtures)
        print(f'PLUGIN RESOURCE MUTATIONS PASS: {len(cases)} groups; logs: {logs}',flush=True)
    except BaseException as error:
        (logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(signum,frame):raise KeyboardInterrupt(f'signal {signum}')
if __name__=='__main__':
    signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
