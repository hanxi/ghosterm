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
      ('scoped-authority',[('    const char *caps[] = {"file.readScoped", "resource.receive"};\n','    const char *caps[] = {"ui.form", "resource.receive"};\n')],'ungranted'),
      ('lease-scope',[('        if (a != GT_BRIDGE_ACCESS_ALLOWED) {\n','        if (a != GT_BRIDGE_ACCESS_ALLOWED && a != GT_BRIDGE_ACCESS_DENIED) {\n')],'scope'),
      ('waiting-distinct',[('            return a == GT_BRIDGE_ACCESS_UNKNOWN       ? GT_BRIDGE_SOURCE_WAIT\n','            return a == GT_BRIDGE_ACCESS_UNKNOWN       ? GT_BRIDGE_SOURCE_DENIED\n')],'waiting'),
      ('unsupported-distinct',[('        if (!gt__bridge_session_has_capability(session, caps[i])) {\n','        if (false && !gt__bridge_session_has_capability(session, caps[i])) {\n'),('                   : a == GT_BRIDGE_ACCESS_UNSUPPORTED ? GT_BRIDGE_SOURCE_UNSUPPORTED\n','                   : a == GT_BRIDGE_ACCESS_UNSUPPORTED ? GT_BRIDGE_SOURCE_DENIED\n')],'unsupported'),
      ('parent-path',[("        if (size == 2 && p[start] == '.' && p[start + 1] == '.') {\n            return GT_BRIDGE_SOURCE_DENIED;\n","        if (size == 2 && p[start] == '.' && p[start + 1] == '.') {\n            continue;\n")],'parent'),
      ('utf8',[('    if (!source_utf8(p, n)) {\n','    if (false && !source_utf8(p, n)) {\n')],'utf8'),
      ('component-boundary',[('        if (size > 255 || s->count == GT_BRIDGE_SOURCE_DEPTH) {\n','        if (size > 255 || s->count == GT_BRIDGE_SOURCE_DEPTH - 1) {\n')],'max-depth'),
      ('nofollow',[('                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (final ? 0 : O_DIRECTORY));\n','                        O_RDONLY | O_CLOEXEC | O_NONBLOCK | (final ? 0 : O_DIRECTORY));\n')],'swap-open'),
      ('cloexec',[('                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (final ? 0 : O_DIRECTORY));\n','                        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | (final ? 0 : O_DIRECTORY));\n')],'roundtrip'),
      ('nonblock',[('                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (final ? 0 : O_DIRECTORY));\n','                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (final ? 0 : O_DIRECTORY));\n')],'roundtrip'),
      ('source-readonly',[('                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (final ? 0 : O_DIRECTORY));\n','                        O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (final ? 0 : O_DIRECTORY));\n')],'roundtrip'),
      ('negotiated-limit',[('        s->limit = config->byte_limit < session.budgets.resource_bytes\n                       ? config->byte_limit\n                       : session.budgets.resource_bytes;\n','        s->limit = config->byte_limit;\n')],'negotiated-limit'),
      ('configured-limit',[('            } else if ((uint64_t)after.st_size > s->limit) {\n','            } else if (false && (uint64_t)after.st_size > s->limit) {\n')],'limit'),
      ('source-version',[('    if (!source_inode(a, b) || a->st_size != b->st_size || a->st_nlink != b->st_nlink ||\n        a->st_mode != b->st_mode) {\n','    if (!source_inode(a, b) || false) {\n'),('    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&\n           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&\n           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&\n           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;\n','    return true;\n'),('    return a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&\n           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;\n','    return true;\n')],'edit'),
      ('ancestor-chain',[('    for (size_t i = 0; i < s->count; i++) {\n','    for (size_t i = 0; i < 0 * s->count; i++) {\n')],'rename'),
      ('constructor-stop',[('            status = GT_BRIDGE_SOURCE_UNSUPPORTED;\n        }\n    }\n    if (status == GT_BRIDGE_SOURCE_OK) {\n','            status = GT_BRIDGE_SOURCE_UNSUPPORTED;\n        }\n    }\n    if (false && status == GT_BRIDGE_SOURCE_OK) {\n'),('    for (size_t i = 0; status == GT_BRIDGE_SOURCE_OK && i < s->count; i++) {\n        status = source_access(s);\n','    for (size_t i = 0; status == GT_BRIDGE_SOURCE_OK && i < s->count; i++) {\n        status = GT_BRIDGE_SOURCE_OK;\n')],'stop-open'),
      ('post-stat-authority',[('    return source_access(s);\n','    return GT_BRIDGE_SOURCE_OK;\n')],'stop-stat'),
      ('post-read-authority',[('        if ((status = source_access(s)) != GT_BRIDGE_SOURCE_OK) {\n','        if (false && (status = source_access(s)) != GT_BRIDGE_SOURCE_OK) {\n')],'stop-read'),
      ('post-read-version',[('        if ((status = source_check(s)) != GT_BRIDGE_SOURCE_OK) {\n','        if (false && (status = source_check(s)) != GT_BRIDGE_SOURCE_OK) {\n')],'mid-read'),
      ('actual-eof',[('            if (count) {\n','            if (false && count) {\n')],'eof-extra'),
      ('second-eof',[('    if (offset == s->info.descriptor.size) {\n        if (count) {\n','    if (offset == s->info.descriptor.size) {\n        if (false && count) {\n')],'second-eof'),
      ('short-read-count',[('            s->info.hashed += (uint64_t)count;\n','            s->info.hashed += (uint64_t)n;\n')],'short'),
      ('read-error',[('                source_fail(s, GT_BRIDGE_SOURCE_IO);\n','                source_fail(s, GT_BRIDGE_SOURCE_CHANGED);\n')],'read-error'),
      ('read-eintr',[('            if (read_error != EINTR && read_error != EAGAIN) {\n','            if (read_error != EAGAIN) {\n')],'eintr'),
      ('source-hash',[('        } else if (!gt__sha256_update(&s->hash, s->buffer, (size_t)count)) {\n','        } else if (!gt__sha256_update(&s->hash, s->buffer, 0)) {\n')],'roundtrip'),
      ('backpressure',[('        s->at += accepted;\n','        s->at = s->used;\n')],'large'),
      ('second-pass-digest',[('        status = finished == GT_BRIDGE_RESOURCE_INVALID ? GT_BRIDGE_SOURCE_CHANGED\n','        status = finished == GT_BRIDGE_RESOURCE_INVALID ? GT_BRIDGE_SOURCE_OK\n')],'digest'),
      ('receipt-required',[('if (r.confirmed) {','if (true) {')],'roundtrip'),
      ('lost-is-unknown',[('            s->info.phase = GT_BRIDGE_SOURCE_UNCERTAIN;\n            s->info.error = GT_BRIDGE_SOURCE_UNKNOWN;\n','            s->info.phase = GT_BRIDGE_SOURCE_FAILED;\n            s->info.error = GT_BRIDGE_SOURCE_IO;\n')],'lost'),
      ('cancel-preserves-failure',[('    if (s->info.phase != GT_BRIDGE_SOURCE_CANCELLING) {\n        source_fail(s, GT_BRIDGE_SOURCE_CANCELLED);\n    }\n','    source_fail(s, GT_BRIDGE_SOURCE_CANCELLED);\n')],'failure-close'),
      ('queued-failure',[('                                s->info.error == GT_BRIDGE_SOURCE_CLOSED\n                            ? GT_BRIDGE_SOURCE_STOPPED\n                            : GT_BRIDGE_SOURCE_FAILED;\n','                                s->info.error == GT_BRIDGE_SOURCE_CLOSED\n                            ? GT_BRIDGE_SOURCE_STOPPED\n                            : GT_BRIDGE_SOURCE_STOPPED;\n')],'queued-change'),
      ('close-error',[('        if (!closed) {\n            return GT_BRIDGE_SOURCE_IO;\n','        if (!closed) {\n            return GT_BRIDGE_SOURCE_OK;\n')],'close-error'),
      ('owned-root',[('        s->directory[0] = fcntl(config->root_fd, F_DUPFD_CLOEXEC, 3);\n','        s->directory[0] = config->root_fd;\n')],'close-root'),
      ('owner-release',[('    gt__secure_erase(s, sizeof(*s));\n    gt__plugin_memory_free(&a, s, sizeof(*s));\n','    gt__secure_erase(s, sizeof(*s));\n    (void)a;\n')],'allocation'),
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
