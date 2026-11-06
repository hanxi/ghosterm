#!/usr/bin/env python3
"""Compiled common-session regressions in isolated Core copies. Targets must
fail normal C assertions; every restore runs all socket and real SSH cases.
"""
from pathlib import Path
import sys
import hashlib,json,os,shutil,signal,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_SESSION_MUTATION'
MODES='allow deny late-deny consent-time expired clock lost revoke stop stop-before-step allocation bad-nonce bad-scope old-epoch gap replay wrong-digest wrong-cap budget-upgrade bad-accept bad-ready truncated early-message active-message late-read late-write expired-after-allow'.split()
INPUTS=sorted({str(p.relative_to(ROOT)) for folder in ('core/src','core/include') for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')})+[
 'core/CMakeLists.txt','core/tests/test_bridge_session.c','core/tests/test_bridge_ssh.c',
 'packages/plugin-sdk/schema/bridge-v1.candidate.json','packages/plugin-sdk/schema/bridge-v1.cases.json',
 'scripts/generate-plugin-wire.py','scripts/test-plugin-session.py','scripts/test-plugin-route.py','scripts/test-plugin-ssh.py',
 'scripts/test-plugin-ssh-stream.py','scripts/mutation-check-plugin-session.py']
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-session-mutations-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();source=work/'source';build=work/'build'
    hashes={name:digest(ROOT/name) for name in INPUTS};commands=[]
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    def run(label,args,red=False,timeout=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-session-mutations; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-session-mutations',str(ROOT),str(timeout),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=timeout+10)
        output=p.stdout+p.stderr;(logs/(label+'.log')).write_text(output);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in output or any(x in output for x in ('AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer')):raise RuntimeError(label+': expected normal C assertion; '+output)
        elif p.returncode:raise RuntimeError(label+': '+output)
    def rebuild(label):
        # GNU make can rebuild .o/.a within one second without relinking tests.
        binaries=['test_bridge_session', 'test_config']
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        objects=[('ghosterm_core','src/extension/session.c'),('test_bridge_session','tests/test_bridge_session.c')]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_bridge_session','test_config','-j','2'])
        link_log=(logs/(label+'-build.log')).read_text()
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in link_log:raise RuntimeError('Missing actual link: '+label+' '+marker)
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:
            if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in output:raise RuntimeError('Missing actual compilation: '+label+' '+owner)
    def suite(label):
        for mode in MODES:run(label+'-'+mode,[build/'test_bridge_session',mode],timeout=10)
        run(label+'-ssh',['python3',ROOT/'scripts/test-plugin-session.py','--binary',build/'test_bridge_session'])
    try:
        if MARK.encode() in (ROOT/'core/src/extension/session.c').read_bytes():print('Residual mutation',file=sys.stderr);raise SystemExit(2)
        shutil.copytree(ROOT/'core',source,ignore=shutil.ignore_patterns('build','build-*'))
        run('configure',['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');run('baseline-control',[build/'test_config',source/'tests/fixtures'])
        cases=[
          ('unknown-is-not-allow','if(decision==GT_BRIDGE_DECISION_UNKNOWN)return GT_OK;','if(decision==GT_BRIDGE_DECISION_UNKNOWN)decision=GT_BRIDGE_DECISION_ALLOW;','allow'),
          ('consent-ticket','s->info.phase!=GT_BRIDGE_SESSION_CONSENT || strcmp(token,s->ticket)','s->info.phase!=GT_BRIDGE_SESSION_CONSENT','allow'),
          ('host-nonce','!string_equal(s,m.payload,"hostNonce",s->host_nonce) || !string_member','false || !string_member','bad-nonce'),
          ('challenge','(!challenge || string_equal(s,payload,"challenge",s->challenge))','(!challenge || string_equal(s,payload,"hostNonce",s->host_nonce))','bad-accept'),
          ('instance-scope','strcmp(m.connection,s->info.connection) || strcmp(m.instance,s->info.instance)','strcmp(m.connection,s->info.connection)','bad-scope'),
          ('epoch','m.epoch!=1','m.epoch==0','old-epoch'),
          ('sequence','m.sequence!=s->rx_sequence','false','gap'),
          ('executor-digest','string_equal(s,payload,"executorDigest",s->config.executor_digest)','true','wrong-digest'),
          ('required-capabilities','!caps_equal(s,m.payload,"requires")','false','wrong-cap'),
          ('budget-upgrade','!same_budgets(budgets,minimum_budgets(s->config.budgets,budgets))','false','budget-upgrade'),
          ('ready-ack','queue(s,GT_BRIDGE_READY,GT_BRIDGE_SESSION_WAIT_ACK)','queue(s,GT_BRIDGE_READY,GT_BRIDGE_SESSION_ACTIVE)','bad-ready'),
          ('human-wait','bool waiting=s->info.phase==GT_BRIDGE_SESSION_CONSENT || s->info.phase==GT_BRIDGE_SESSION_WAIT_DECISION;','bool waiting=false;','consent-time'),
          ('stop-wake','atomic_store_explicit(&s->stopping,true,memory_order_release);gt__bridge_stream_stop(s->stream);','atomic_store_explicit(&s->stopping,true,memory_order_release);','stop'),
          ('stop-permission','if(stopped(s) || s->info.end!=GT_BRIDGE_END_NONE)return GT_BRIDGE_ACCESS_REVOKED;','if(s->info.end!=GT_BRIDGE_END_NONE)return GT_BRIDGE_ACCESS_REVOKED;','stop-before-step'),
          ('lease-token',' || memcmp(lease->token,s->lease,33)','','allow'),
          ('early-lease',' || s->info.phase!=GT_BRIDGE_SESSION_ACTIVE)return false;',' )return false;','allow'),
          ('application-not-silent',':(s->rpc?gt__bridge_rpc_receive(s->rpc,m,size,s->now):GT_ERR_UNSUPPORTED);',':(s->rpc?gt__bridge_rpc_receive(s->rpc,m,size,s->now):GT_OK);','active-message'),
          ('late-read','if(!tick(s))return s->info.phase; /* no activation from late read bytes */','/* omitted read deadline recheck */','late-read'),
          ('late-write','if(!tick(s))return s->info.phase; /* time may advance during the write */','/* omitted write deadline recheck */','late-write'),
          ('expired-effect','s->info.end==GT_BRIDGE_END_EXPIRED && s->info.decision!=GT_BRIDGE_DECISION_ALLOW?','s->info.end==GT_BRIDGE_END_EXPIRED?','expired-after-allow'),
          ('owner-release','gt_allocator_t a=s->allocator;gt__secure_erase(s,sizeof(*s));gt__plugin_memory_free(&a,s,sizeof(*s));','gt_allocator_t a=s->allocator;gt__secure_erase(s,sizeof(*s));(void)a;','allow'),
        ]
        path=source/'src/extension/session.c';original=path.read_bytes();results=[]
        for label,old,new,mode in cases:
            text=original.decode()
            if text.count(old)!=1:raise RuntimeError('Ambiguous mutation anchor: '+label)
            path.write_text(text.replace(old,new+' /* '+MARK+' */'));rebuild(label)
            run(label+'-target',[build/'test_bridge_session',mode],red=True,timeout=10)
            run(label+'-control',[build/'test_config',source/'tests/fixtures'])
            path.write_bytes(original)
            if digest(path)!=hashlib.sha256(original).hexdigest():raise RuntimeError('Restore mismatch')
            rebuild(label+'-restored');suite(label+'-restored');results.append(label)
            print(label+': compiled C assertion red / config green / restored 27 socket + 4 SSH cases green / SHA256',flush=True)
        if any(digest(ROOT/name)!=sha for name,sha in hashes.items()):raise RuntimeError('Worktree changed during isolated run')
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=results,inputs_sha256=hashes,commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        print('PLUGIN SESSION MUTATIONS PASS: '+str(len(results))+' groups; logs: '+str(logs),flush=True)
    finally:shutil.rmtree(source,ignore_errors=True);shutil.rmtree(build,ignore_errors=True)
if __name__=='__main__':
    def interrupted(number,frame):raise KeyboardInterrupt('signal '+str(number))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted);main()
