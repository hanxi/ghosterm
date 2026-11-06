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
      ('unpublished-read','resource.c','    if (!s || !received || !data || !capacity || capacity > GT_BRIDGE_RESOURCE_CHUNK_MAX) {\n        return GT_BRIDGE_RESOURCE_INVALID;\n    }\n    if (!resource_scope(s, owner, lease)) {\n        return GT_BRIDGE_RESOURCE_DENIED;\n    }\n    resource_item_t *item = resource_item(s, owner, ref);\n    if (!item) {\n        return GT_BRIDGE_RESOURCE_NOT_FOUND;\n    }\n    if (item->state != RESOURCE_PUBLISHED) {\n', '    if (!s || !received || !data || !capacity || capacity > GT_BRIDGE_RESOURCE_CHUNK_MAX) {\n        return GT_BRIDGE_RESOURCE_INVALID;\n    }\n    if (!resource_scope(s, owner, lease)) {\n        return GT_BRIDGE_RESOURCE_DENIED;\n    }\n    resource_item_t *item = resource_item(s, owner, ref);\n    if (!item) {\n        return GT_BRIDGE_RESOURCE_NOT_FOUND;\n    }\n    if (false && item->state != RESOURCE_PUBLISHED) {\n','roundtrip'),
      ('computed-digest','resource.c','    if (!matched) {\n','    if (!matched && false) {\n','integrity'),
      ('declared-digest','resource.c','        (item->descriptor.size_known && size != item->descriptor.size) || !hex_string(digest, 64) ||\n        memcmp(digest, item->descriptor.sha256, 65)) {\n','        (item->descriptor.size_known && size != item->descriptor.size) || !hex_string(digest, 64)) {\n','integrity'),
      ('known-length','resource.c','        (item->descriptor.size_known && size != item->descriptor.size) || !hex_string(digest, 64) ||\n        memcmp(digest, item->descriptor.sha256, 65)) {\n','        (false && item->descriptor.size_known && size != item->descriptor.size) ||\n        !hex_string(digest, 64) || memcmp(digest, item->descriptor.sha256, 65)) {\n','integrity'),
      ('source-complete','resource.c','    if (!complete || size != item->received ||\n','    if ((false && !complete) || size != item->received ||\n','integrity'),
      ('unknown-reservation','resource.c','    uint64_t reserve = d->size_known ? d->size : s->config.resource_bytes;\n','    uint64_t reserve = d->size_known ? d->size : 0;\n','unknown'),
      ('total-budget','resource.c','    if (reserve > s->config.total_bytes - s->reserved ||\n','    if ((false && reserve > s->config.total_bytes - s->reserved) ||\n','shared'),
      ('scope-budget','resource.c','        reserve > scope->budget - scope->reserved || s->resources == s->config.slots) {\n','        (false && reserve > scope->budget - scope->reserved) || s->resources == s->config.slots) {\n','scope'),
      ('slot-quota','resource.c',[('        reserve > scope->budget - scope->reserved || s->resources == s->config.slots) {\n','        reserve > scope->budget - scope->reserved || (false && s->resources == s->config.slots)) {\n'),('    resource_item_t *item = NULL;\n    for (size_t i = 0; i < s->config.slots; i++) {\n','    resource_item_t *item = NULL;\n    for (size_t i = 0; i < GT_BRIDGE_RESOURCE_SLOTS; i++) {\n')],None,'quota'),
      ('snapshot-charge','resource.c','    s->reserved -= item->reserved - size;\n    scope->reserved -= item->reserved - size;\n    item->reserved = size;\n','    s->reserved -= item->reserved;\n    scope->reserved -= item->reserved;\n    item->reserved = 0;\n','unknown'),
      ('retained-pins','resource.c','    if (item->references > 1) {\n','    if (false && item->references > 1) {\n','pins'),
      ('generation-highwater','resource.c','    if (d->generation <= scope->last_generation) {\n','    if (false && d->generation <= scope->last_generation) {\n','budget'),
      ('native-lease','resource.c','           !memcmp(a->instance, b->instance, 33) && !memcmp(a->token, b->token, 33);\n','           !memcmp(a->instance, b->instance, 33);\n', 'scope'),
      ('native-reference','resource.c','            s->items[i].ref.handle == ref->handle &&\n            !memcmp(s->items[i].ref.token, ref->token, 33)) {\n','            s->items[i].ref.handle == ref->handle) {\n', 'scope'),
      ('seal-staging','resource.c','            if (resource_drop(s, scope, &s->items[i]) != GT_BRIDGE_RESOURCE_OK) {\n','            if (false) {\n','seal'),
      ('seal-new-traffic','resource.c','    if (scope->sealed) {\n','    if (false && scope->sealed) {\n','seal'),
      ('offset','resource.c','    if (offset != item->received) {\n','    if (false && offset != item->received) {\n','offset'),
      ('cumulative-bytes','resource.c','    if (size > item->reserved - item->received) {\n','    if (false && size > item->reserved - item->received) {\n','offset'),
      ('partial-write-hash','resource.c','    if (!gt__sha256_update(&item->hash, data, (size_t)n)) {\n','    if (!gt__sha256_update(&item->hash, data, size)) {\n','partial'),
      ('write-zero','resource.c','    if (n <= 0 || (size_t)n > size) {\n','    if (n < 0 || (size_t)n > size) {\n','io-write'),
      ('write-eintr','resource.c','    if (n < 0 && errno == EINTR) {\n        return GT_BRIDGE_RESOURCE_WAIT;\n','    if (n < 0 && errno == EINTR) {\n        return GT_BRIDGE_RESOURCE_IO;\n','io-write'),
      ('read-zero','resource.c','    if (n <= 0 || (size_t)n > capacity) {\n','    if (n < 0 || (size_t)n > capacity) {\n','io-read'),
      ('private-directory','resource.c','    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 07777) != 0700) {\n','    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {\n', 'filesystem'),
      ('readonly-snapshot','resource.c','        item->reader = openat(s->config.directory_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);\n','        item->reader = openat(s->config.directory_fd, name, O_RDWR | O_NOFOLLOW | O_CLOEXEC);\n','roundtrip'),
      ('anonymous-file','resource.c','        if (unlinkat(s->config.directory_fd, name, 0)) {\n','        if (false && unlinkat(s->config.directory_fd, name, 0)) {\n','roundtrip'),
      ('failed-cleanup-charge','resource.c','                   unlinkat(s->config.directory_fd, item->name, 0)) {\n            return GT_BRIDGE_RESOURCE_IO;\n','                   unlinkat(s->config.directory_fd, item->name, 0)) {\n            item->name[0] = 0;\n','io-cleanup'),
      ('cleanup-identity','resource.c','        } else if (st.st_dev != item->device || st.st_ino != item->inode || !S_ISREG(st.st_mode) ||\n                   unlinkat(s->config.directory_fd, item->name, 0)) {\n','        } else if (false || unlinkat(s->config.directory_fd, item->name, 0)) {\n','cleanup-race'),
      ('owner-erase','resource.c','        gt__secure_erase(s, sizeof(*s));\n        gt__plugin_memory_free(&a, s, sizeof(*s));\n','        a.release(a.context, s, sizeof(*s));\n','allocation'),
      ('sha-round','sha256.c','0x428a2f98,','0x428a2f99,','hash'),
      ('sha-bit-length','sha256.c','    uint64_t bits = s->bytes * 8;\n','    uint64_t bits = s->bytes;\n','hash'),
      ('sha-overflow','sha256.c','    if (!s || s->ended || (!data && n) || n > UINT64_MAX / 8 - s->bytes) {\n','    if (!s || s->ended || (!data && n)) {\n', 'hash'),
      ('sha-final-state','sha256.c','    s->ended = true;\n','    s->ended = false;\n','hash')]
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
