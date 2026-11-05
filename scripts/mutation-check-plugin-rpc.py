#!/usr/bin/env python3
"""Compiled request/flow regressions in isolated Core copies. Targets must
fail normal C assertions; every restore runs all socket and real SSH cases.
"""
from pathlib import Path
import hashlib,json,os,shutil,signal,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_RPC_MUTATION'
MODES='roundtrip full unknown denied missing-cap remote-invalid queued-cancel queued-timeout validation scope cancel late-result lost stop duplicate conflict retired feature allocation credit pending-duplicate cancel-before-take cancel-no-credit credit-overflow overspend gap response-conflict counter select-expiry held-credit incoming-timeout'.split()
INPUTS=sorted({str(p.relative_to(ROOT)) for folder in ('core/src','core/include') for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')})+[
 'core/CMakeLists.txt','core/tests/test_bridge_rpc.c','core/tests/test_bridge_session.c','core/tests/test_bridge_ssh.c',
 'packages/plugin-sdk/schema/bridge-v1.candidate.json','packages/plugin-sdk/schema/bridge-v1.cases.json',
 'scripts/generate-plugin-wire.py','scripts/test-plugin-session.py','scripts/test-plugin-route.py','scripts/test-plugin-ssh.py',
 'scripts/test-plugin-ssh-stream.py','scripts/mutation-check-plugin-rpc.py']
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-rpc-mutations-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();source=work/'source';build=work/'build'
    hashes={name:digest(ROOT/name) for name in INPUTS};commands=[]
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    def run(label,args,red=False,timeout=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-rpc-mutations; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-rpc-mutations',str(ROOT),str(timeout),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=timeout+10)
        output=p.stdout+p.stderr;(logs/(label+'.log')).write_text(output);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in output or any(x in output for x in ('AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer')):raise RuntimeError(label+': expected normal C assertion; '+output)
        elif p.returncode:raise RuntimeError(label+': '+output)
    def rebuild(label):
        # Include-based tests must not retain an object across a fast restore.
        objects=[('ghosterm_core','src/extension/rpc.c'),('ghosterm_core','src/extension/session.c'),('test_bridge_rpc','tests/test_bridge_rpc.c')]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_bridge_rpc','test_config','-j','2'])
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:
            if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in output:raise RuntimeError('Missing actual compilation: '+label+' '+owner)
    def suite(label):
        for mode in MODES:run(label+'-'+mode,[build/'test_bridge_rpc',mode],timeout=10)
        run(label+'-ssh',['python3',ROOT/'scripts/test-plugin-session.py','--binary',build/'test_bridge_rpc','--rpc'])
    try:
        for name in ('session.c','rpc.c','rpc.h'):
            if MARK.encode() in (ROOT/'core/src/extension'/name).read_bytes():raise RuntimeError('Residual mutation: '+name)
        shutil.copytree(ROOT/'core',source,ignore=shutil.ignore_patterns('build','build-*'))
        run('configure',['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');run('baseline-control',[build/'test_config',source/'tests/fixtures'])
        cases=[('input-schema',
          'rpc.c',
          'if(!r->methods[slot->method].input(r->methods[slot->method].context,(const uint8_t *)slot->input,n))',
          'if(false)',
          'remote-invalid'),
         ('output-schema',
          'rpc.c',
          ' || !r->methods[method].output(r->methods[method].context,p,size)',
          '',
          'validation'),
         ('native-permission',
          'rpc.c',
          'if(gt__bridge_session_access(r->session,lease,r->methods[m].capability)!=GT_BRIDGE_ACCESS_ALLOWED)',
          '(void)lease; if(false)',
          'validation'),
         ('native-scope', 'session.c', ' && !memcmp(lease->token,s->lease,33);', ';', 'scope'),
         ('unsupported-distinct',
          'rpc.c',
          'if(slot->method==SIZE_MAX || '
          'r->methods[slot->method].receiver!=(uint8_t)r->role){rpc_error(slot,"UNSUPPORTED");return GT_OK;}',
          'if(slot->method==SIZE_MAX || '
          'r->methods[slot->method].receiver!=(uint8_t)r->role){rpc_error(slot,"DENIED");return GT_OK;}',
          'unknown'),
         ('missing-capability',
          'rpc.c',
          'if(!gt__bridge_session_has_capability(r->session,r->methods[slot->method].capability))',
          'if(false)',
          'missing-cap'),
         ('duplicate-dispatch',
          'rpc.c',
          'if(slot->done)slot->needs_send=true;return GT_OK;',
          'slot->done=false;slot->taken=false;return GT_OK;',
          'pending-duplicate'),
         ('duplicate-content', 'rpc.c', ' || slot->input_size!=n || memcmp(slot->input,p,n)', '', 'conflict'),
         ('retired-request', 'rpc.c', 'if(ordinal<=r->rx_ordinal)', 'if(false)', 'retired'),
         ('lost-is-unknown',
          'rpc.c',
          'rpc_local(o,GT_BRIDGE_RPC_UNKNOWN,rpc_unknown);',
          'rpc_local(o,GT_BRIDGE_RPC_SUCCEEDED,rpc_unknown);',
          'lost'),
         ('queued-id',
          'rpc.c',
          'slot->state=GT_BRIDGE_RPC_QUEUED;slot->deadline=',
          'rpc_id(r,r->tx_ordinal+1,slot->id);slot->state=GT_BRIDGE_RPC_QUEUED;slot->deadline=',
          'queued-cancel'),
         ('cancel-is-not-receipt',
          'rpc.c',
          'if(!o->id[0])rpc_local(o,GT_BRIDGE_RPC_CANCELLED,rpc_cancelled);',
          'if(true)rpc_local(o,GT_BRIDGE_RPC_CANCELLED,rpc_cancelled);',
          'cancel'),
         ('confirmed-result', 'rpc.c', 'if(slot){if(slot->confirmed){', 'if(slot){if(false){', 'response-conflict'),
         ('independent-flow-credit',
          'rpc.c',
          'unsigned flow=r->prepared==RPC_TX_REQUEST?0:1;',
          'unsigned flow=0;',
          'credit'),
         ('receive-credit', 'rpc.c', ' || size+4>r->rx_credit[flow]', '', 'overspend'),
         ('credit-overflow',
          'rpc.c',
          'if(bytes>GT_BRIDGE_RPC_WINDOW-r->tx_credit[i])',
          'if(false)',
          'credit-overflow'),
         ('cancel-priority',
          'rpc.c',
          'p->cancel_requested && !p->cancel_sent',
          'r->tx_credit[0] && p->cancel_requested && !p->cancel_sent',
          'cancel-no-credit'),
         ('queued-deadline',
          'rpc.c',
          '!o->local_terminal && now>=o->deadline',
          '!o->local_terminal && false',
          'queued-timeout'),
         ('precommit-deadline',
          'session.c',
          'if(resource || gt__bridge_rpc_current(s->rpc,s->now))',
          'if(true)',
          'select-expiry'),
         ('incoming-deadline',
          'rpc.c',
          'in->used && !in->done && now>=in->deadline',
          'in->used && !in->done && false',
          'incoming-timeout'),
         ('rpc-sequence', 'rpc.c', ' || m->sequence!=r->rx_seq[flow]', '', 'gap'),
         ('fixed-capacity', 'rpc.h', '#define GT_BRIDGE_RPC_SLOTS 64u', '#define GT_BRIDGE_RPC_SLOTS 65u', 'full'),
         ('owner-release',
          'rpc.c',
          'gt__secure_erase(r,sizeof(*r));a.release(a.context,r,sizeof(*r));',
          'gt__secure_erase(r,sizeof(*r));(void)a;',
          'roundtrip')]
        results=[]
        for label,name,old,new,mode in cases:
            path=source/'src/extension'/name;original=path.read_bytes();text=original.decode()
            if text.count(old)!=1:raise RuntimeError('Ambiguous mutation anchor: '+label)
            path.write_text(text.replace(old,new+' /* '+MARK+' */'));rebuild(label)
            run(label+'-target',[build/'test_bridge_rpc',mode],red=True,timeout=10)
            run(label+'-control',[build/'test_config',source/'tests/fixtures'])
            path.write_bytes(original)
            if digest(path)!=hashlib.sha256(original).hexdigest():raise RuntimeError('Restore mismatch')
            rebuild(label+'-restored');suite(label+'-restored');results.append(label)
            print(label+': compiled C assertion red / config green / restored 31 socket + 4 SSH cases green / SHA256',flush=True)
        if any(digest(ROOT/name)!=sha for name,sha in hashes.items()):raise RuntimeError('Worktree changed during isolated run')
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=results,inputs_sha256=hashes,commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        print('PLUGIN RPC MUTATIONS PASS: '+str(len(results))+' groups; logs: '+str(logs),flush=True)
    finally:shutil.rmtree(source,ignore_errors=True);shutil.rmtree(build,ignore_errors=True)
if __name__=='__main__':
    def interrupted(number,frame):raise KeyboardInterrupt('signal '+str(number))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted);main()
