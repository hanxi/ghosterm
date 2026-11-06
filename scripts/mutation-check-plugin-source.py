#!/usr/bin/env python3
"""Compiled scoped-file producer faults in an isolated Core. Normal assertion
red, same-library configuration green, restored native/SSH suite and SHA256.
"""
from pathlib import Path
import sys
import hashlib,json,os,re,shutil,signal,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_SOURCE_MUTATION'
MODES=re.search(r'foreach\(mode ([^)]*)\)\s+add_test\(NAME bridge_source_\$\{mode\}',(ROOT/'core/CMakeLists.txt').read_text()).group(1).split()
INPUTS=sorted({str(p.relative_to(ROOT)) for folder in ('core/src','core/include') for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')})+['core/CMakeLists.txt','core/tests/test_bridge_source.c','core/tests/test_bridge_transfer.c','core/tests/test_bridge_rpc.c','core/tests/test_bridge_session.c','core/tests/test_bridge_ssh.c','packages/plugin-sdk/schema/bridge-v1.candidate.json','packages/plugin-sdk/schema/bridge-v1.cases.json','scripts/generate-plugin-wire.py','scripts/test-plugin-session.py','scripts/test-plugin-route.py','scripts/test-plugin-ssh.py','scripts/test-plugin-ssh-stream.py','scripts/mutation-check-plugin-source.py']
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-source-mutations-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();source=work/'source';build=work/'build';fixtures=work/'fixtures';fixtures.mkdir()
    hashes={n:digest(ROOT/n) for n in INPUTS};commands=[]
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    def run(label,args,red=False,budget=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-source-mutations; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-source-mutations',str(ROOT),str(budget),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);output=p.stdout+p.stderr
        (logs/(label+'.log')).write_text(output);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in output or any(x in output for x in ('AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer')):raise RuntimeError(label+': expected normal C assertion; '+output)
        elif p.returncode:raise RuntimeError(label+': '+output)
    def rebuild(label):
        # GNU make can rebuild .o/.a within one second without relinking tests.
        binaries=['test_bridge_source', 'test_config']
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        objects=[('ghosterm_core','src/extension/source.c'),('test_bridge_source','tests/test_bridge_source.c')]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_bridge_source','test_config','-j','2'])
        link_log=(logs/(label+'-build.log')).read_text()
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in link_log:raise RuntimeError('Missing actual link: '+label+' '+marker)
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:
            if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in output:raise RuntimeError('Missing actual compilation: '+label+' '+owner)
    def suite(label):
        for mode in MODES:run(label+'-'+mode,[build/'test_bridge_source',mode,fixtures],budget=20)
        run(label+'-ssh',['python3',ROOT/'scripts/test-plugin-session.py','--source','--binary',build/'test_bridge_source'],budget=280)
    cases=[
      ('scoped-authority',[('"file.readScoped","resource.receive"','"ui.form","resource.receive"')],'ungranted'),
      ('lease-scope',[('if(a!=GT_BRIDGE_ACCESS_ALLOWED)','if(a!=GT_BRIDGE_ACCESS_ALLOWED && a!=GT_BRIDGE_ACCESS_DENIED)')],'scope'),
      ('waiting-distinct',[('a==GT_BRIDGE_ACCESS_UNKNOWN?GT_BRIDGE_SOURCE_WAIT','a==GT_BRIDGE_ACCESS_UNKNOWN?GT_BRIDGE_SOURCE_DENIED')],'waiting'),
      ('unsupported-distinct',[('if(!gt__bridge_session_has_capability(session,caps[i]))','if(false && !gt__bridge_session_has_capability(session,caps[i]))'),('a==GT_BRIDGE_ACCESS_UNSUPPORTED?GT_BRIDGE_SOURCE_UNSUPPORTED','a==GT_BRIDGE_ACCESS_UNSUPPORTED?GT_BRIDGE_SOURCE_DENIED')],'unsupported'),
      ('parent-path',[('return GT_BRIDGE_SOURCE_DENIED;\n  if(!size)','continue;\n  if(!size)')],'parent'),
      ('utf8',[('if(!source_utf8(p,n))','if(false && !source_utf8(p,n))')],'utf8'),
      ('component-boundary',[('s->count==GT_BRIDGE_SOURCE_DEPTH','s->count==GT_BRIDGE_SOURCE_DEPTH-1')],'max-depth'),
      ('nofollow',[('O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK','O_RDONLY|O_CLOEXEC|O_NONBLOCK')],'swap-open'),
      ('cloexec',[('O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK','O_RDONLY|O_NOFOLLOW|O_NONBLOCK')],'roundtrip'),
      ('nonblock',[('O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK','O_RDONLY|O_NOFOLLOW|O_CLOEXEC')],'roundtrip'),
      ('source-readonly',[('O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK','O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK')],'roundtrip'),
      ('negotiated-limit',[('config->byte_limit<session.budgets.resource_bytes?config->byte_limit:session.budgets.resource_bytes','config->byte_limit')],'negotiated-limit'),
      ('configured-limit',[('else if((uint64_t)after.st_size>s->limit)','else if(false && (uint64_t)after.st_size>s->limit)')],'limit'),
      ('source-version',[(' || a->st_size!=b->st_size || a->st_nlink!=b->st_nlink || a->st_mode!=b->st_mode',' || false'),('return a->st_mtimespec.tv_sec==b->st_mtimespec.tv_sec && a->st_mtimespec.tv_nsec==b->st_mtimespec.tv_nsec && a->st_ctimespec.tv_sec==b->st_ctimespec.tv_sec && a->st_ctimespec.tv_nsec==b->st_ctimespec.tv_nsec;','return true;'),('return a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec && a->st_ctim.tv_sec==b->st_ctim.tv_sec && a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;','return true;')],'edit'),
      ('ancestor-chain',[('for(size_t i=0;i<s->count;i++){\n  if(fstat','for(size_t i=0;i<0*s->count;i++){\n  if(fstat')],'rename'),
      ('constructor-stop',[('if(status==GT_BRIDGE_SOURCE_OK)status=source_access(s);','if(false && status==GT_BRIDGE_SOURCE_OK)status=source_access(s);'),('status=source_access(s);if(status!=GT_BRIDGE_SOURCE_OK)break;','status=GT_BRIDGE_SOURCE_OK;if(status!=GT_BRIDGE_SOURCE_OK)break;')],'stop-open'),
      ('post-stat-authority',[('}return source_access(s);','}return GT_BRIDGE_SOURCE_OK;')],'stop-stat'),
      ('post-read-authority',[('if((status=source_access(s))!=GT_BRIDGE_SOURCE_OK){source_fail(s,status);return s->info.phase;}if(count<0)','if(false && (status=source_access(s))!=GT_BRIDGE_SOURCE_OK){source_fail(s,status);return s->info.phase;}if(count<0)')],'stop-read'),
      ('post-read-version',[('if((status=source_check(s))!=GT_BRIDGE_SOURCE_OK){source_fail(s,status);return s->info.phase;}\n  if(offset','if(false && (status=source_check(s))!=GT_BRIDGE_SOURCE_OK){source_fail(s,status);return s->info.phase;}\n  if(offset')],'mid-read'),
      ('actual-eof',[('if(count)source_fail(s,GT_BRIDGE_SOURCE_CHANGED);','if(false && count)source_fail(s,GT_BRIDGE_SOURCE_CHANGED);')],'eof-extra'),
      ('second-eof',[('if(count){source_fail(s,GT_BRIDGE_SOURCE_CHANGED);return s->info.phase;}s->send_eof=true;','if(false && count){source_fail(s,GT_BRIDGE_SOURCE_CHANGED);return s->info.phase;}s->send_eof=true;')],'second-eof'),
      ('short-read-count',[('s->info.hashed+=(uint64_t)count;','s->info.hashed+=(uint64_t)n;')],'short'),
      ('read-error',[('if(read_error!=EINTR && read_error!=EAGAIN)source_fail(s,GT_BRIDGE_SOURCE_IO);','if(read_error!=EINTR && read_error!=EAGAIN)source_fail(s,GT_BRIDGE_SOURCE_CHANGED);')],'read-error'),
      ('read-eintr',[('read_error!=EINTR && read_error!=EAGAIN','read_error!=EAGAIN')],'eintr'),
      ('source-hash',[('gt__sha256_update(&s->hash,s->buffer,(size_t)count)','gt__sha256_update(&s->hash,s->buffer,0)')],'roundtrip'),
      ('backpressure',[('s->at+=accepted;s->info.accepted+=accepted','s->at=s->used;s->info.accepted+=accepted')],'large'),
      ('second-pass-digest',[('status=finished==GT_BRIDGE_RESOURCE_INVALID?GT_BRIDGE_SOURCE_CHANGED:source_transfer_status(finished);','status=finished==GT_BRIDGE_RESOURCE_INVALID?GT_BRIDGE_SOURCE_OK:source_transfer_status(finished);')],'digest'),
      ('receipt-required',[('if(r.confirmed){','if(true){')],'roundtrip'),
      ('lost-is-unknown',[('s->info.phase=GT_BRIDGE_SOURCE_UNCERTAIN;s->info.error=GT_BRIDGE_SOURCE_UNKNOWN;','s->info.phase=GT_BRIDGE_SOURCE_FAILED;s->info.error=GT_BRIDGE_SOURCE_IO;')],'lost'),
      ('cancel-preserves-failure',[('if(s->info.phase!=GT_BRIDGE_SOURCE_CANCELLING)source_fail','source_fail')],'failure-close'),
      ('queued-failure',[('s->info.error==GT_BRIDGE_SOURCE_CLOSED?GT_BRIDGE_SOURCE_STOPPED:GT_BRIDGE_SOURCE_FAILED','s->info.error==GT_BRIDGE_SOURCE_CLOSED?GT_BRIDGE_SOURCE_STOPPED:GT_BRIDGE_SOURCE_STOPPED')],'queued-change'),
      ('close-error',[('if(!closed)return GT_BRIDGE_SOURCE_IO;','if(!closed)return GT_BRIDGE_SOURCE_OK;')],'close-error'),
      ('owned-root',[('s->directory[0]=fcntl(config->root_fd,F_DUPFD_CLOEXEC,3);','s->directory[0]=config->root_fd;')],'close-root'),
      ('owner-release',[('gt__plugin_memory_free(&a,s,sizeof(*s));return GT_BRIDGE_SOURCE_OK;','(void)a;return GT_BRIDGE_SOURCE_OK;')],'allocation'),
    ]
    try:
        if MARK.encode() in (ROOT/'core/src/extension/source.c').read_bytes():print('Residual source mutation',file=sys.stderr);raise SystemExit(2)
        for label,changes,mode in cases:
            if mode not in MODES:raise RuntimeError('Unregistered target: '+label)
            candidate=(ROOT/'core/src/extension/source.c').read_text()
            for old,new in changes:
                if old not in candidate:raise RuntimeError('Missing mutation anchor: '+label)
                candidate=candidate.replace(old,new)
        shutil.copytree(ROOT/'core',source,ignore=shutil.ignore_patterns('build','build-*'))
        run('configure',['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');run('baseline-config',[build/'test_config',source/'tests/fixtures'])
        for label,changes,mode in cases:
            path=source/'src/extension/source.c';original=path.read_bytes();mutated=original.decode()
            for old,new in changes:mutated=mutated.replace(old,new)
            path.write_text(mutated+'\n/* '+MARK+' */\n')
            try:
                rebuild(label);run(label+'-target',[build/'test_bridge_source',mode,fixtures],red=True,budget=20);run(label+'-config',[build/'test_config',source/'tests/fixtures'])
            finally:path.write_bytes(original)
            rebuild(label+'-restored');run(label+'-restored-target',[build/'test_bridge_source',mode,fixtures],budget=20);assert digest(path)==hashlib.sha256(original).hexdigest()
            for child in fixtures.iterdir():
                if child.is_dir():shutil.rmtree(child)
                else:child.unlink()
            print(label+': compiled C assertion red / config green / rebuilt target restored green / SHA256',flush=True)
        suite('final');run('final-config',[build/'test_config',source/'tests/fixtures'])
        for n,h in hashes.items():
            if digest(ROOT/n)!=h:raise RuntimeError('Input changed: '+n)
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],inputs_sha256=hashes,commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n');shutil.rmtree(source);shutil.rmtree(build);shutil.rmtree(fixtures)
        print(f'PLUGIN SOURCE MUTATIONS PASS: {len(cases)} groups; logs: {logs}',flush=True)
    except BaseException as e:(logs/'failure.txt').write_text(repr(e)+'\n');raise
def stop(number,frame):raise KeyboardInterrupt(f'signal {number}')
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
