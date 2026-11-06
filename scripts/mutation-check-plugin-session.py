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
          ('unknown-is-not-allow','    if (decision == GT_BRIDGE_DECISION_UNKNOWN) {\n        return GT_OK;\n','    if (decision == GT_BRIDGE_DECISION_UNKNOWN) {\n        decision = GT_BRIDGE_DECISION_ALLOW;\n','allow'),
          ('consent-ticket','    if (s->info.phase != GT_BRIDGE_SESSION_CONSENT || strcmp(token, s->ticket)) {\n','    if (s->info.phase != GT_BRIDGE_SESSION_CONSENT) {\n','allow'),
          ('host-nonce','        if (!string_equal(s, m.payload, "hostNonce", s->host_nonce) ||\n            !string_member(s, m.payload, "nonce", s->executor_nonce, 33) ||\n','        if (false || !string_member(s, m.payload, "nonce", s->executor_nonce, 33) ||\n','bad-nonce'),
          ('challenge','           (!challenge || string_equal(s, payload, "challenge", s->challenge));\n','           (!challenge || string_equal(s, payload, "hostNonce", s->host_nonce));\n','bad-accept'),
          ('instance-scope','    if (s->info.connection[0] &&\n        (strcmp(m.connection, s->info.connection) || strcmp(m.instance, s->info.instance))) {\n','    if (s->info.connection[0] && (strcmp(m.connection, s->info.connection))) {\n','bad-scope'),
          ('epoch','    if (m.epoch != 1) {\n','    if (m.epoch == 0) {\n','old-epoch'),
          ('sequence','    if (s->rx_exhausted || m.sequence != s->rx_sequence) {\n','    if (s->rx_exhausted || false) {\n','gap'),
          ('executor-digest','           string_equal(s, payload, "clientDigest", s->config.client_digest) &&\n           string_equal(s, payload, "executorDigest", s->config.executor_digest) &&\n','           string_equal(s, payload, "clientDigest", s->config.client_digest) && true &&\n','wrong-digest'),
          ('required-capabilities','            !string_equal(s, m.payload, "route", "direct-duplex") ||\n            !caps_equal(s, m.payload, "requires") || !read_budgets(s, m.payload, &budgets) ||\n','            !string_equal(s, m.payload, "route", "direct-duplex") || false ||\n            !read_budgets(s, m.payload, &budgets) ||\n','wrong-cap'),
          ('budget-upgrade','            !same_budgets(budgets, minimum_budgets(s->config.budgets, budgets))) {\n','            false) {\n','budget-upgrade'),
          ('ready-ack','        return queue(s, GT_BRIDGE_READY, GT_BRIDGE_SESSION_WAIT_ACK);\n','        return queue(s, GT_BRIDGE_READY, GT_BRIDGE_SESSION_ACTIVE);\n','bad-ready'),
          ('human-wait','    bool waiting = s->info.phase == GT_BRIDGE_SESSION_CONSENT ||\n                   s->info.phase == GT_BRIDGE_SESSION_WAIT_DECISION;\n','    bool waiting = false;\n','consent-time'),
          ('stop-wake','        atomic_store_explicit(&s->stopping, true, memory_order_release);\n        gt__bridge_stream_stop(s->stream);\n    }\n','        atomic_store_explicit(&s->stopping, true, memory_order_release);\n    }\n','stop'),
          ('stop-permission','    if (stopped(s) || s->info.end != GT_BRIDGE_END_NONE) {\n','    if (s->info.end != GT_BRIDGE_END_NONE) {\n','stop-before-step'),
          ('lease-token','        memcmp(lease->instance, s->info.instance, 33) || memcmp(lease->token, s->lease, 33)) {\n','        memcmp(lease->instance, s->info.instance, 33)) {\n','allow'),
          ('early-lease','    if (!s || !out || stopped(s) || s->info.end != GT_BRIDGE_END_NONE ||\n        s->info.phase != GT_BRIDGE_SESSION_ACTIVE) {\n','    if (!s || !out || stopped(s) || s->info.end != GT_BRIDGE_END_NONE) {\n','allow'),
          ('application-not-silent','    return resource\n               ? (s->resources ? gt__bridge_transfer_receive(s->resources, m, size)\n                               : GT_ERR_UNSUPPORTED)\n               : (s->rpc ? gt__bridge_rpc_receive(s->rpc, m, size, s->now) : GT_ERR_UNSUPPORTED);\n','    return resource ? (s->resources ? gt__bridge_transfer_receive(s->resources, m, size)\n                                    : GT_ERR_UNSUPPORTED)\n                    : (s->rpc ? gt__bridge_rpc_receive(s->rpc, m, size, s->now) : GT_OK);\n','active-message'),
          ('late-read','        if (!tick(s)) {\n            return s->info.phase; /* no activation from late read bytes */\n        }\n','        /* omitted read deadline recheck */\n','late-read'),
          ('late-write','        if (!tick(s)) {\n            return s->info.phase; /* time may advance during the write */\n        }\n','        /* omitted write deadline recheck */\n','late-write'),
          ('expired-effect','        const char *outcome =\n            s->info.end == GT_BRIDGE_END_DENIED\n                ? "{\\"kind\\":\\"failed\\",\\"error\\":{\\"code\\":\\"DENIED\\",\\"message\\":\\"declined\\","\n                  "\\"effect\\":\\"none\\"}}"\n            : s->info.end == GT_BRIDGE_END_EXPIRED && s->info.decision != GT_BRIDGE_DECISION_ALLOW\n                ? "{\\"kind\\":\\"failed\\",\\"error\\":{\\"code\\":\\"EXPIRED\\",\\"message\\":\\"expired\\","\n                  "\\"effect\\":\\"none\\"}}"\n                : "{\\"kind\\":\\"unknown\\",\\"reason\\":\\"bridge ended without a business completion "\n                  "receipt\\"}";\n','        const char *outcome = s->info.end == GT_BRIDGE_END_DENIED\n                                  ? "{\\"kind\\":\\"failed\\",\\"error\\":{\\"code\\":\\"DENIED\\","\n                                    "\\"message\\":\\"declined\\",\\"effect\\":\\"none\\"}}"\n                              : s->info.end == GT_BRIDGE_END_EXPIRED\n                                  ? "{\\"kind\\":\\"failed\\",\\"error\\":{\\"code\\":\\"EXPIRED\\","\n                                    "\\"message\\":\\"expired\\",\\"effect\\":\\"none\\"}}"\n                                  : "{\\"kind\\":\\"unknown\\",\\"reason\\":\\"bridge ended without a "\n                                    "business completion receipt\\"}";\n','expired-after-allow'),
          ('owner-release','    gt__plugin_memory_free(&a, s, sizeof(*s));\n','    (void)a;\n','allow'),
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
