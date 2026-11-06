#!/usr/bin/env python3
"""Isolated compiled resource-message regressions: assertion red, same archive
config green, complete native and real multi-hop SSH restoration with SHA256.
"""
from pathlib import Path
import sys
import hashlib,json,os,shutil,signal,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_TRANSFER_MUTATION'
MODES='roundtrip large unknown empty partial snapshot bidirectional queued-cancel cancel receiver-cancel denied unsupported feature lost stop deferred-credit pending-window rpc-progress io native-scope source-incomplete integrity duplicate receipt-conflict snapshot-quota retired destroy-retry stop-write stop-finish bad-success possible-cancel offset overspend gap epoch instance allocation counter exit-before-receipt credit-overflow fairness source-digest peer-id limit duplicate-conflict release-io release-last-io'.split()
INPUTS=sorted({str(p.relative_to(ROOT)) for folder in ('core/src','core/include') for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')})+['core/CMakeLists.txt','core/tests/test_bridge_transfer.c','core/tests/test_bridge_rpc.c','core/tests/test_bridge_session.c','core/tests/test_bridge_ssh.c','packages/plugin-sdk/schema/bridge-v1.candidate.json','packages/plugin-sdk/schema/bridge-v1.cases.json','scripts/generate-plugin-wire.py','scripts/test-plugin-session.py','scripts/test-plugin-route.py','scripts/test-plugin-ssh.py','scripts/test-plugin-ssh-stream.py','scripts/mutation-check-plugin-transfer.py']
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-transfer-mutations-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();source=work/'source';build=work/'build';fixtures=work/'fixtures';fixtures.mkdir()
    hashes={n:digest(ROOT/n) for n in INPUTS};commands=[]
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    def run(label,args,red=False,timeout=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-transfer-mutations; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-transfer-mutations',str(ROOT),str(timeout),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=timeout+10);output=p.stdout+p.stderr
        (logs/(label+'.log')).write_text(output);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in output or any(x in output for x in ('AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer')):raise RuntimeError(label+': expected normal C assertion; '+output)
        elif p.returncode:raise RuntimeError(label+': '+output)
    def rebuild(label):
        # GNU make can rebuild .o/.a within one second without relinking tests.
        binaries=['test_bridge_transfer', 'test_config']
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        objects=[('ghosterm_core','src/extension/'+n+'.c') for n in ('transfer','session','resource','wire')]+[('test_bridge_transfer','tests/test_bridge_transfer.c')]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_bridge_transfer','test_config','-j','2'])
        link_log=(logs/(label+'-build.log')).read_text()
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in link_log:raise RuntimeError('Missing actual link: '+label+' '+marker)
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:
            if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in output:raise RuntimeError('Missing actual compilation: '+label+' '+owner)
    def suite(label):
        for mode in MODES:run(label+'-'+mode,[build/'test_bridge_transfer',mode,fixtures],timeout=20)
        run(label+'-ssh',['python3',ROOT/'scripts/test-plugin-session.py','--resources','--binary',build/'test_bridge_transfer'],timeout=280)
    cases=[
      ('feature-exact','session.c',[('    if (count != (size_t)s->config.rpc_enabled + (s->config.resource_store ? 1u : 0u)) {\n','    if (false && count != (size_t)s->config.rpc_enabled + (s->config.resource_store ? 1u : 0u)) {\n'),('            if (!s->config.resource_store) {\n','            if (false && !s->config.resource_store) {\n')],'feature'),
      ('native-authority','transfer.c',[('    if (!transfer_access(t, lease)) {\n','    if (false && !transfer_access(t, lease)) {\n')],'native-scope'),
      ('incoming-authority','transfer.c',[('        if (!transfer_access(t, &t->lease)) {\n','        if (false && !transfer_access(t, &t->lease)) {\n')],'denied'),
      ('unsupported-distinct','transfer.c',[('        if (!gt__bridge_session_has_capability(t->session, "resource.receive")) {\n','        if (false && !gt__bridge_session_has_capability(t->session, "resource.receive")) {\n')],'unsupported'),
      ('instance-envelope','session.c',[('    if (s->info.connection[0] &&\n        (strcmp(m.connection, s->info.connection) || strcmp(m.instance, s->info.instance))) {\n','    if (s->info.connection[0] && (strcmp(m.connection, s->info.connection))) {\n')],'instance'),
      ('typed-counter','transfer.c',[('    return gt__bridge_wire_counter(t->wire, gt__bridge_wire_member(t->wire, object, key), n);\n','    return gt__bridge_wire_uint(t->wire, gt__bridge_wire_member(t->wire, object, key), n);\n')],'roundtrip'),
      ('receive-credit','transfer.c',[('    if (size > transfer_limit(t) || size + 4 > t->rx_credit || t->rx_exhausted ||\n','    if (size > transfer_limit(t) || (false && size + 4 > t->rx_credit) || t->rx_exhausted ||\n')],'overspend'),
      ('credit-overflow','transfer.c',[('                n > GT_BRIDGE_TRANSFER_WINDOW - t->tx_credit) {\n','                (false && n > GT_BRIDGE_TRANSFER_WINDOW - t->tx_credit)) {\n')],'credit-overflow'),
      ('send-credit','transfer.c',[('    if (!*control && t->cost > t->tx_credit) {\n','    if (false && !*control && t->cost > t->tx_credit) {\n')],'rpc-progress'),
      ('persist-before-credit','transfer.c',[('        c->cost = (uint32_t)(size + 4);\n','        c->cost = 0;\n        t->credit_pending += (uint32_t)(size + 4);\n')],'deferred-credit'),
      ('offset','transfer.c',[('        if (!transfer_uint(t, m->payload, "offset", &offset) || offset != t->in.received ||\n','        if (!transfer_uint(t, m->payload, "offset", &offset) ||\n            (false && offset != t->in.received) ||\n')],'offset'),
      ('cumulative-limit','transfer.c',[('        if (c->size > info.budgets.fragment_bytes || c->size > limit - t->in.received) {\n','        if (false && (c->size > info.budgets.fragment_bytes || c->size > limit - t->in.received)) {\n')],'limit'),
      ('partial-write','transfer.c',[('        c->at += n;\n','        c->at = c->size;\n')],'partial'),
      ('source-eof','transfer.c',[('    if (!complete ||\n','    if ((false && !complete) ||\n')],'source-incomplete'),
      ('source-digest','transfer.c',[('    if (!valid) {\n','    if (!valid && false) {\n')],'source-digest'),
      ('success-receipt','transfer.c',[('        if (!t->out.end_committed || !transfer_uint(t, v, "byteLength", &size) ||\n','        if ((false && !t->out.end_committed) || !transfer_uint(t, v, "byteLength", &size) ||\n')],'bad-success'),
      ('immutable-receipt','transfer.c',[('        state = GT_BRIDGE_TRANSFER_UNKNOWN;\n    }\n    if (t->out.result.confirmed) {\n','        state = GT_BRIDGE_TRANSFER_UNKNOWN;\n    }\n    if (false && t->out.result.confirmed) {\n')],'receipt-conflict'),
      ('lost-unknown','transfer.c',[('            t->out.started ? GT_BRIDGE_TRANSFER_UNKNOWN : GT_BRIDGE_TRANSFER_CANCELLED;\n','            t->out.started ? GT_BRIDGE_TRANSFER_SUCCEEDED : GT_BRIDGE_TRANSFER_CANCELLED;\n')],'exit-before-receipt'),
      ('queued-cancel','transfer.c',[('    t->out.result.cancel_requested = true;\n    if (!t->out.started) {\n','    t->out.result.cancel_requested = true;\n    if (false && !t->out.started) {\n')],'queued-cancel'),
      ('reserved-control','transfer.c',[('    if (!*control && t->cost > t->tx_credit) {\n','    if ((!*control || t->prepared == TRANSFER_CANCEL_OUT) && t->cost > t->tx_credit) {\n')],'cancel'),
      ('snapshot-scope','transfer.c',[('    if (!gt__bridge_session_scope(t->session, lease)) {\n','    if (false && !gt__bridge_session_scope(t->session, lease)) {\n')],'native-scope'),
      ('retire-session','transfer.c',[('        t->cleanup_pending =\n            gt__bridge_resource_retire(t->store, t->owner, &t->lease) != GT_BRIDGE_RESOURCE_OK;\n','        t->cleanup_pending = false;\n')],'snapshot'),
      ('last-pin','resource.c',[('    if (scope->retiring && !scope->resources && !failed) {\n','    if (false && scope->retiring && !scope->resources && !failed) {\n')],'snapshot'),
      ('owner-release','transfer.c',[('        gt__plugin_memory_free(&a, t, sizeof(*t));\n','        (void)a;\n')],'roundtrip'),
      ('cleanup-retry','session.c',[('    if (cleanup != GT_OK) {\n','    if (false && cleanup != GT_OK) {\n')],'destroy-retry'),
      ('io-stop-recheck','transfer.c',[('        }\n    }\n    if (!transfer_live(t)) {\n','        }\n    }\n    if (false && !transfer_live(t)) {\n')],'stop-finish'),
      ('peer-resource-id','transfer.c',[('    return !memcmp(id, expected, 33);\n','    return true;\n')],'peer-id'),
      ('retired-generation','transfer.c',[('        if (generation <= t->rx_ordinal) {\n','        if (false && generation <= t->rx_ordinal) {\n')],'retired'),
      ('duplicate-end','transfer.c',[('                (cached->kind == RECEIPT_SUCCESS &&\n','                (false && cached->kind == RECEIPT_SUCCESS &&\n')],'duplicate-conflict'),
      ('store-publication','transfer.c',[('        }\n        if (status != GT_BRIDGE_RESOURCE_OK) {\n','        }\n        if (false && status != GT_BRIDGE_RESOURCE_OK) {\n')],'integrity'),
      ('credit-routing','session.c',[('    if (m->kind == GT_BRIDGE_CREDIT && string_equal(s, m->payload, "streamId", "resource")) {\n','    if (false && m->kind == GT_BRIDGE_CREDIT &&\n        string_equal(s, m->payload, "streamId", "resource")) {\n')],'roundtrip'),
      ('data-fairness','session.c',[('                                       (resource_control == rpc_control && s->prefer_resource));\n','                                       (resource_control == rpc_control && false));\n')],'fairness'),
      ('possible-is-not-none','transfer.c',[('            !transfer_string(t, e, "effect", effect, sizeof(effect)) || strcmp(effect, "none")) {\n','            !transfer_string(t, e, "effect", effect, sizeof(effect)) ||\n            (false && strcmp(effect, "none"))) {\n')],'possible-cancel'),
      ('sequence','transfer.c',[('        m->sequence != t->rx_sequence) {\n','        (false && m->sequence != t->rx_sequence)) {\n')],'gap'),
      ('release-error-retains-owner','transfer.c',[('    if (t->snapshot_count || failed) {\n','    if (false && (t->snapshot_count || failed)) {\n')],'release-io'),
      ('failed-last-close-scope','resource.c',[('scope->retiring && !scope->resources && !failed','scope->retiring && !scope->resources')],'release-last-io'),
    ]
    try:
        for name in ('transfer.c','session.c','resource.c'):
            if MARK.encode() in (ROOT/'core/src/extension'/name).read_bytes():print('Residual mutation: '+name,file=sys.stderr);raise SystemExit(2)
        shutil.copytree(ROOT/'core',source,ignore=shutil.ignore_patterns('build','build-*'))
        run('configure',['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');run('baseline-config',[build/'test_config',source/'tests/fixtures'])
        for label,name,changes,mode in cases:
            path=source/'src/extension'/name;original=path.read_bytes();mutated=original.decode()
            for old,new in changes:
                if old not in mutated:raise RuntimeError('Missing mutation anchor: '+label)
                mutated=mutated.replace(old,new)
            path.write_text(mutated+'\n/* '+MARK+' */\n')
            try:
                rebuild(label);run(label+'-target',[build/'test_bridge_transfer',mode,fixtures],red=True,timeout=20);run(label+'-config',[build/'test_config',source/'tests/fixtures'])
            finally:path.write_bytes(original)
            rebuild(label+'-restored');suite(label+'-restored');assert digest(path)==hashlib.sha256(original).hexdigest()
            for child in fixtures.iterdir():
                if child.is_dir():shutil.rmtree(child)
                else:child.unlink()
            print(label+': compiled C assertion red / config green / restored 47 native + 4 SSH green / SHA256',flush=True)
        for n,h in hashes.items():
            if digest(ROOT/n)!=h:raise RuntimeError('Input changed: '+n)
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],inputs_sha256=hashes,commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n');shutil.rmtree(source);shutil.rmtree(build);shutil.rmtree(fixtures)
        print(f'PLUGIN TRANSFER MUTATIONS PASS: {len(cases)} groups; logs: {logs}',flush=True)
    except BaseException as e:(logs/'failure.txt').write_text(repr(e)+'\n');raise
def stop(number,frame):raise KeyboardInterrupt(f'signal {number}')
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
