#!/usr/bin/env python3
"""Real PTY/VT owner -> bounded restore stream -> full Metal surface barrier."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import shutil
import signal
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
VT_SUITE=runpy.run_path(str(ROOT/'scripts/probe-session-worker-vt.py'))
FULL,VT=(VT_SUITE[k] for k in ('FULL','VT'))
PATCHES=(*VT_SUITE['PATCHES'],ROOT/'patches/libghostty/8159838/experimental/0027-surface-host-budget.patch',
         ROOT/'patches/libghostty/8159838/experimental/0029-replica-focus-input.patch',
         ROOT/'patches/libghostty/8159838/experimental/0030-replica-scheme-barrier.patch',
         ROOT/'patches/libghostty/8159838/experimental/0031-replica-checkpoint-scheme.patch',
         ROOT/'patches/libghostty/8159838/experimental/0032-surface-thread-observation.patch',
         ROOT/'patches/libghostty/8159838/experimental/0033-surface-thread-args.patch',
         ROOT/'patches/libghostty/8159838/experimental/0034-owned-signal-stack.patch',
         ROOT/'patches/libghostty/8159838/experimental/0035-budgeted-search-thread.patch',
         ROOT/'patches/libghostty/8159838/experimental/0036-app-allocator.patch',
         ROOT/'patches/libghostty/8159838/experimental/0037-owned-app-warmups.patch',
         ROOT/'patches/libghostty/8159838/experimental/0038-history-pruned-notify.patch',
         ROOT/'patches/libghostty/8159838/experimental/0039-config-budget.patch',
         ROOT/'patches/libghostty/8159838/experimental/0040-config-environment-owner.patch',
         ROOT/'patches/libghostty/8159838/experimental/0041-global-allocator-lifecycle.patch',
         ROOT/'patches/libghostty/8159838/experimental/0042-global-allocator-observer.patch',
         ROOT/'patches/zig/0.16.0/0001-owned-io-threads.patch',
         ROOT/'patches/libghostty/8159838/experimental/0043-owned-global-io.patch',
         ROOT/'patches/libghostty/8159838/experimental/0044-empty-replica-io-import.patch',
         ROOT/'patches/libghostty/8159838/experimental/0045-empty-replica-config.patch',
         ROOT/'patches/libghostty/8159838/experimental/0046-empty-replica-input-barrier.patch',
         ROOT/'patches/libghostty/8159838/experimental/0047-ordered-replica-style.patch',
         ROOT/'patches/libghostty/8159838/experimental/0048-live-replica-config-input.patch')
SHARED=runpy.run_path(str(ROOT/'scripts/probe-session-shared-budget.py'))
DRIVER=ROOT/'daemon/probes/session-vt-owner.c'
SURFACE=ROOT/'daemon/probes/session-surface-owner.m'
HEADER=ROOT/'daemon/probes/surface-restore-probe.h'
BUDGET=ROOT/'terminal/session-state/budget-posix.c'
MARK='GT_WORKER_SURFACE_MUTATION_MARK'
CASES=('pixels','continuation','utf8','resize','resources','idle','query','query-delta','query-caps','isolation','clipboard','input','import-only','gui-default','gui-default-utf8','gui-denied','gui-pages','gui-partial','gui-growth','gui-revoked','gui-interrupt','gui-live','gui-slow','gui-race','tui-less','tui-vim')


def compile_probe(engine,work,driver,surface,budget):
    common=['-Wall','-Wextra','-Werror','-I',str(engine/'include'),'-I',str(ROOT/'core/include'),
            '-I',str(ROOT/'core/src'),'-I',str(ROOT/'terminal/session-state'),'-I',str(ROOT/'daemon/probes')]
    cc=shlex.split(os.environ.get('CC','cc'))
    obj=work/'owner.o';binary=work/'worker-surface'
    FULL['run']([*cc,'-std=c11','-D_DEFAULT_SOURCE','-DGHOSTERM_P0_FULL_SURFACE_REPLAY',
                 '-Dmain=ghosterm_worker_surface_fixture_main',*common,'-c',str(driver),'-o',str(obj)])
    # Both static archives bundle strong UBSan runtime definitions. Keep the VT
    # engine in its generated dylib, with Mach-O two-level symbol namespaces;
    # full surface remains the exact pinned native static build. No suppression
    # of duplicate symbols or modifications to the upstream build are needed.
    dylib=work/'out/lib/libghostty-vt.dylib'
    if not dylib.is_file(): raise RuntimeError('VT dynamic library missing')
    frameworks=[arg for name in FULL['FRAMEWORKS'] for arg in ('-framework',name)]
    FULL['run']([*cc,'-fobjc-arc',*common,str(surface),str(obj),str(budget),
                 str(ROOT/'terminal/session-state/checkpoint-budget.c'),
                 str(ROOT/'terminal/session-state/frozen-budget.c'),
                 str(ROOT/'core/build/libghosterm_core.a'),str(dylib),
                 str(engine/'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
                 '-Wl,-rpath,'+str(work/'out/lib'),'-lc++','-lpthread',*frameworks,'-o',str(binary)])
    return binary


def probe(binary,case,expected=0):
    # A timeout propagates as TimeoutExpired: hanging is never a pass. 'nonzero'
    # covers faults whose exit is a signal (wrong-allocator free traps).
    result=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=30,cwd=ROOT)
    sys.stdout.write(result.stdout);sys.stderr.write(result.stderr)
    if expected=='nonzero':
        if not result.returncode: raise RuntimeError(f'{case}: expected nonzero exit, got 0')
    elif result.returncode!=expected: raise RuntimeError(f'{case}: expected {expected}, got {result.returncode}')


def scheme_barrier_mutations(engine,work,driver,surface,budget):
    # Only isolated upstream files are mutated; repository patch/source hashes
    # are checked by main, including on INT/TERM.
    paths={name:engine/name for name in ('src/termio/Termio.zig','src/Surface.zig')}
    originals={name:path.read_bytes() for name,path in paths.items()}
    digests={name:hashlib.sha256(data).digest() for name,data in originals.items()}
    variants=(
        ('admission falsely publishes sequence','src/termio/Termio.zig',
         'self.replica_scheme = .{ .sequence = sequence, .dark = dark };',
         'self.replica_scheme = .{ .sequence = sequence, .dark = dark }; self.replica_sequence = sequence;'),
        ('IO application never publishes sequence','src/termio/Termio.zig',
         'self.replica_sequence = pending.sequence;','self.replica_sequence +%= 0; _ = pending;'),
        ('later event bypasses pending barrier','src/termio/Termio.zig',
         'if (self.replica_scheme != null) return false;','// pending event gate removed'),
        ('pending input stamp loses barrier','src/termio/Termio.zig',
         '.blocked = self.replica_scheme != null,', '.blocked = false,'),
        ('configuration message loses its sequence','src/Surface.zig',
         'termio_config_ptr.replica_scheme_sequence = sequence;', 'termio_config_ptr.replica_scheme_sequence = 0;'),
        ('unsequenced reload accepted by bound replica','src/termio/Termio.zig',
         'self.replica_generation != 0 or config.replica_scheme_sequence != 0',
         'config.replica_scheme_sequence != 0'),
        ('actual terminal background not updated','src/termio/Termio.zig',
         'self.terminal.colors.background.default = config.background.toTerminalRGB();',
         '// actual terminal background omitted'),
    )
    def verify(binary,barrier,expected=0):
        env=os.environ.copy()
        if barrier:env['GHOSTERM_P0_SCHEME_BARRIER']='1'
        else:env.pop('GHOSTERM_P0_SCHEME_BARRIER',None)
        result=subprocess.run([str(binary),'continuation'],env=env,capture_output=True,text=True,timeout=30,cwd=ROOT)
        if result.returncode!=expected:
            sys.stdout.write(result.stdout);sys.stderr.write(result.stderr)
            raise RuntimeError(f'scheme barrier expected {expected}, got {result.returncode}')
        if expected==0:sys.stdout.write(result.stdout)
    binary=compile_probe(engine,work,driver,surface,budget);verify(binary,True);verify(binary,False)
    try:
        for label,name,old,new in variants:
            text=originals[name].decode()
            if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique scheme barrier anchor')
            try:
                paths[name].write_text(text.replace(old,new)+'\n// '+MARK+'\n')
                FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/scheme-barrier-mutation-build.log')
                binary=compile_probe(engine,work,driver,surface,budget)
                verify(binary,True,1);verify(binary,False)
                print('PASS scheme-barrier mutation/'+label+': target red, control green',flush=True)
            finally:
                paths[name].write_bytes(originals[name])
                if hashlib.sha256(paths[name].read_bytes()).digest()!=digests[name]:raise RuntimeError('scheme barrier rollback SHA mismatch')
    finally:
        for name,path in paths.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=digests[name]:raise RuntimeError('scheme barrier rollback SHA mismatch')
    FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/scheme-barrier-rollback-build.log')
    binary=compile_probe(engine,work,driver,surface,budget);verify(binary,True);verify(binary,False)
    print('PASS 7 scheme-barrier mutations, SHA256 rollback, native baselines; IPC configuration still disabled',flush=True)


def mutations(engine,work,driver,surface,budget):
    variants=(
        ('captured frame loses rendered content',surface,
         'CGContextDrawImage(ctx,CGRectMake(0,0,w,h),image);',
         '(void)image;', 'pixels','continuation'),
        ('output consumed on UI thread',surface,'CHECK(!pthread_create(&thread,NULL,surface_consume,&restore));',
         'memset(&thread,0,sizeof(thread));(void)surface_consume(&restore);','continuation','import-only'),
        ('output not applied',surface,'CHECK(ghostty_p0_surface_event_output_for(surface,generation,event->sequence,bytes,event->length));',
         '(void)event;','continuation','idle'),
        ('cell metrics omitted',surface,'event->sequence,size.cols,size.rows,probe->cell_width,probe->cell_height)',
         'event->sequence,size.cols,size.rows,0,0)','continuation','idle'),
        ('barrier unavailable',surface,'CHECK(ghostty_p0_surface_applied_sequence_for(surface,generation,&sequence));',
         'CHECK(false && ghostty_p0_surface_applied_sequence_for(surface,generation,&sequence));','continuation','import-only'),
        ('delivery mistaken for engine application',surface,'uint64_t before=surface_applied(surface,generation,cursor);',
         'uint64_t before=event->sequence;(void)cursor;','continuation','idle'),
        ('second READY omitted',surface,'CHECK(gt__attachment_ready_live(probe->attachment,generation,applied)==GT_RESTORE_OK);',
         '/* live acknowledgment omitted */','idle','import-only'),
        ('input callback missing',surface,'opts.p0_host_write_input=surface_input;',
         'opts.p0_host_write_input=NULL; (void)surface_input;','continuation','import-only'),
        ('authority loses cell metrics',budget,'rc=ghostty_terminal_resize(t,cols,rows,width,height);',
         '(void)width;(void)height;rc=ghostty_terminal_resize(t,cols,rows,0,0);','continuation','idle'),
        ('input routed to wrong generation',surface,'if(!gt__attachment_input_allowed(context->probe->attachment,generation,sequence)) {',
         'if(!gt__attachment_input_allowed(context->probe->attachment,generation+1,sequence)) {','continuation','import-only'),
        ('input sequence lost',surface,'if(!gt__attachment_input_allowed(context->probe->attachment,generation,sequence)) {',
         'if((void)sequence,!gt__attachment_input_allowed(context->probe->attachment,generation,0)) {','continuation','import-only'),
        ('surface image bytes corrupted',surface,'const uint8_t red[]={255,0,0,255};CHECK(n==sizeof(red)',
         'rgba[0]=0;const uint8_t red[]={255,0,0,255};CHECK(n==sizeof(red)','resources','idle'),
        # -X keeps less on the primary screen: the TUI cases must fail when the
        # program never leaves it, otherwise they prove nothing beyond the
        # plain-output modes.
        ('alternate screen never entered',driver,'exec /usr/bin/less ','exec /usr/bin/less -X ',
         'tui-less','continuation'),
        # 把掉线推迟到增量放完，用例就只是在验证一次正常恢复：队列空了，
        # 剩下的全是已送达的事件，中断根本没有发生在恢复中途。
        ('interruption after the stream drained',surface,'if(interrupt)break;','if(false && interrupt)break;',
         'gui-interrupt','continuation'),
        # 权威改在 READY(S) 之前产生那个事件，它就被算进 L：恢复中途没有任何
        # 东西越过 L，用例也就不再证明“L 冻在 READY(S)”。
        ('authority emits before READY(S)',surface,
         'CHECK(gt__attachment_ready_snapshot(probe->attachment,generation,applied)==GT_RESTORE_OK);\n'
         '    /* \u6743\u5a01\u5728 READY(S) \u4e4b\u540e\u624d\u4ea7\u751f\u8fd9\u4e2a\u4e8b\u4ef6\uff1a\u5b83\u5fc5\u987b\u6392\u5728 L \u4e4b\u5916\u3002 */\n'
         '    const uint64_t live_last=live ? probe->authority_emit(probe->authority_context) : 0;',
         'const uint64_t live_last=live ? probe->authority_emit(probe->authority_context) : 0;\n'
         '    CHECK(gt__attachment_ready_snapshot(probe->attachment,generation,applied)==GT_RESTORE_OK);',
         'gui-live','continuation'),
        # 越过 L 的事件只出队不上屏，恢复出来的表面就停在 L，追不上还在跑的权威。
        ('events past L dequeued but not applied',surface,
         'cursor=surface_apply(surface,probe,&event,bytes,cursor);crossed++;','crossed++;',
         'gui-live','continuation'),
        # 慢客户端把队列撑满之后照常往下走：这一代已经作废，没有任何通向 LIVE
        # 的路径，拒绝必须来自核心，而不只是探针自己收手。
        # 抢注方改用在位那一代的号：它立刻就能从同一条流里取到东西，用例也就
        # 不再证明「抢不到的那一方什么都碰不到」。
        ('rival uses the incumbent generation',surface,
         'CHECK(gt__attachment_next(c->attachment,c->generation+1,&event,bytes,sizeof(bytes))==GT_RESTORE_STALE);',
         'CHECK(gt__attachment_next(c->attachment,c->generation,&event,bytes,sizeof(bytes))==GT_RESTORE_STALE);',
         'gui-race','continuation'),
        # detach 之后那次 OSC 9 不发，增量流里就没有宿主动作，副本「一次都没触发」
        # 也就无从谈起：这条路径必须让 isolation 红。
        ('detached host action never emitted',driver,
         '"printf \'h\\\\033]9;SECONDNOTE\\\\033\\\\\\\\\'; "','"printf \'h\'; "',
         'isolation','continuation'),
        # 同理：detach 之后那次剪贴板写不发，增量流里就没有剪贴板动作。
        ('detached clipboard write never emitted',driver,
         '"printf \'h\\\\033]52;c;U0VDT05EQ0xJUA==\\\\033\\\\\\\\\'; "','"printf \'h\'; "',
         'clipboard','continuation'),
        # 组合中的预编辑一旦当成已提交，拼音半成品就会漏进 PTY：把 composing
        # 翻成 false，那一步的「零字节」立刻变成六个字节，input 必须红。
        ('composing preedit treated as committed',surface,
         r'.text="\xe4\xb8\xad",.composing=true};',
         r'.text="\xe4\xb8\xad",.composing=false};',
         'input','continuation'),
        # 鼠标按钮编号真的来自事件而不是写死的 0：换成右键，SGR 第一个参数变 2。
        ('mouse button number not taken from the event',surface,
         'GHOSTTY_MOUSE_PRESS,GHOSTTY_MOUSE_LEFT','GHOSTTY_MOUSE_PRESS,GHOSTTY_MOUSE_RIGHT',
         'input','continuation'),
        # 焦点只在状态真变时才报：把重复的那次设成相反值，原本该是零字节的
        # 一步就冒出 CSI I。否则窗口一晃程序就被刷屏。
        ('focus reported without a state change',surface,
         r'expect_input(&input,"focus-lost","\x1b[O");' + '\n        ghostty_surface_set_focus(surface,false);',
         r'expect_input(&input,"focus-lost","\x1b[O");' + '\n        ghostty_surface_set_focus(surface,true);',
         'input','continuation'),
        ('overflowed generation continues to LIVE',surface,
         'restore->cut=cursor;restore->applied=cursor;atomic_store(&restore->done,true);return NULL;\n    }\n    if(live) {',
         '}\n    if(live) {','gui-slow','continuation'),
    )
    for label,path,old,new,target,control in variants:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(f'nonunique surface mutation: {label}')
        try:
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n')
            binary=compile_probe(engine,work,driver,surface,budget)
            probe(binary,target,1);probe(binary,control)
            print(f'PASS mutation/{label}: target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('surface rollback SHA mismatch')
    binary=compile_probe(engine,work,driver,surface,budget)
    for case in CASES:probe(binary,case)
    # The old zero-cell API delegates to the new explicit-cell variant. Its
    # owner faults, accounting, and refusal contracts must remain intact.
    shared=SHARED['build'](engine,work,budget)
    for case in SHARED['CASES']:SHARED['PROBE'](shared,case)
    SHARED['mutations'](engine,work,budget)


def engine_mutations(engine,work,driver,surface,budget):
    variants=(
        ('surface general allocator bypassed','src/apprt/embedded.zig',
         'if (opts.p0_allocator) |p| p.zig() else app.core_app.alloc,','app.core_app.alloc,','gui-denied','gui-default',1,1),
        ('snapshot page provider bypassed','src/termio/Termio.zig',
         '.p0_page_allocator = opts.replica_page_allocator,','.p0_page_allocator = null,','gui-pages','gui-default',2,1),
        ('sticky host fault ignored','src/termio/Termio.zig',
         'return if (self.replica_budget_healthy) |healthy| healthy(self.replica_budget_userdata) else true;',
         '_ = self; return true;','gui-revoked','gui-default',1,1),
        ('shared device charged to surface','src/Surface.zig',
         'const device: *rendererpkg.Device = try .init(app.alloc, device_options);',
         'const device: *rendererpkg.Device = try .init(alloc, device_options);','continuation','gui-default',1,1),
        ('incomplete provider accepted','src/apprt/embedded.zig',
         'opts.p0_page_allocator == null or','false or','gui-partial','gui-default',1,1),
        # The fallback cache grows under the renderer's surface allocator but is
        # freed by the set's own: without the owner the free traps (signal) or
        # leaves the GUI ledger unbalanced. gui-default-utf8 renders the same
        # codepoints on default allocators, so the red is about ownership.
        ('font cache ownership lost','src/font/SharedGridSet.zig',
         'grid.owner_alloc = self.alloc;','grid.owner_alloc = null;','utf8','gui-default-utf8',1,'nonzero'),
        # 0028. Without the restored headroom the replica's page is full, so
        # the detached resize splits it where the authority reflowed in place:
        # same text, different pages, no byte equality. Only a case with real
        # scrollback reaches that split, which is why the control stays green.
        ('restored page without reflow headroom','src/terminal/snapshot/page.zig',
         'if (TerminalPage.layout(cap).total_size > max_page_size) cap = exact;','cap = exact;',
         'tui-less','continuation',1,1),
        # 0029. 把副本分支改回 0004 的「整条丢掉」：?1004h 开着、焦点真的变了，
        # 字节却到不了权威。只有 input 这一刀切焦点，所以对照照样绿。
        ('replica focus report dropped again','src/termio/Termio.zig',
         'defer request.deinit();\n            try self.queueReplicaInput(request);',
         'request.deinit();',
         'input','continuation',1,1),
    )
    def rebuild():
        cache=work/'full-cache'
        if shutil.disk_usage(work).free<4*1024**3 and cache.exists():
            if cache.is_symlink() or cache.resolve().parent!=work.resolve():raise RuntimeError('unsafe cache path')
            shutil.rmtree(cache)
        FULL['build'](engine,work,cache_dir=cache,log_path=ROOT/'tmp/worker-surface-budget-mutation-build.log')
        return compile_probe(engine,work,driver,surface,budget)
    for label,relative,old,new,target,control,count,expected in variants:
        path=engine/relative;original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=count:raise RuntimeError(f'nonunique budget mutation: {label}')
        try:
            path.write_text(text.replace(old,new,1)+f'\n// {MARK}\n')
            binary=rebuild();probe(binary,target,expected);probe(binary,control)
            print(f'PASS engine mutation/{label}: target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('engine budget rollback SHA mismatch')
    binary=rebuild()
    for case in CASES:probe(binary,case)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',choices=CASES);parser.add_argument('--mutations',action='store_true')
    parser.add_argument('--scheme-barrier',action='store_true')
    args=parser.parse_args()
    if sum((bool(args.case),args.mutations,args.scheme_barrier))>1:parser.error('choose one case or mutation mode')
    if sys.platform!='darwin' or os.uname().machine!='arm64':raise RuntimeError('requires Apple Silicon macOS')
    files=(DRIVER,SURFACE,HEADER,BUDGET,ROOT/'terminal/session-state/budget-posix.h',*PATCHES,
           *(ROOT/'core/src/session'/name for name in ('worker.c','worker.h','attachment.c','attachment.h',
             'restore-stream.c','restore-stream.h','checkpoint-task.c','checkpoint-task.h','state-owner.c','state-owner.h')),
           *(ROOT/'terminal/session-state'/name for name in ('frozen-budget.c','frozen-budget.h','checkpoint-budget.c','checkpoint-budget.h')))
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,FULL['interrupted'])
    if not os.environ.get('DEVELOPER_DIR'):
        developer=Path('/Applications/Xcode.app/Contents/Developer')
        if not developer.is_dir():raise RuntimeError('full Xcode SDK is required for Metal build')
        os.environ['DEVELOPER_DIR']=str(developer)
    try:
        FULL['run'](['bash',str(ROOT/'scripts/build-core.sh')])
        runpy.run_path(str(ROOT/'scripts/probe-session-state.py'))['build_engine']()
        # Fixed work path and binary names: hosts that gate execution by path
        # kill freshly created executables, including zig's own build runner.
        # Only the sources are wiped; the content-addressed build caches stay,
        # so the engine is still re-extracted from the pin and re-patched.
        work=ROOT/'tmp/worker-surface'
        for stale in ('engine','engine.tar'):
            path=work/stale
            if path.is_dir():shutil.rmtree(path)
            elif path.exists():path.unlink()
        work.mkdir(parents=True,exist_ok=True)
        engine=FULL['prepare'](work)
        for patch in PATCHES:
            FULL['run'](['git','-C',str(engine),'apply','--check',str(patch)])
            FULL['run'](['git','-C',str(engine),'apply',str(patch)])
        VT['build'](engine,work,True,log_path=ROOT/'tmp/worker-surface-vt-build.log')
        FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/worker-surface-full-build.log')
        driver=work/DRIVER.name;driver.write_bytes(DRIVER.read_bytes())
        surface=work/SURFACE.name;surface.write_bytes(SURFACE.read_bytes())
        budget=work/BUDGET.name;budget.write_bytes(BUDGET.read_bytes())
        binary=compile_probe(engine,work,driver,surface,budget)
        if args.scheme_barrier:
            scheme_barrier_mutations(engine,work,driver,surface,budget);return 0
        for case in (args.case,) if args.case else CASES:probe(binary,case)
        if args.mutations:
            mutations(engine,work,driver,surface,budget)
            engine_mutations(engine,work,driver,surface,budget)
            print('PASS 23 full-surface, 11 shared-budget, 8 engine-budget mutations and rollback; P0 incomplete')
            return 0
        print('P0 INCOMPLETE: GUI global budget, other authority events, production manager/IPC and reconnect remain')
        return 0 if args.case else 3
    finally:
        for path,digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError(f'repository source changed: {path}')


if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,subprocess.SubprocessError,InterruptedError) as error:
        print(f'worker/surface probe failed: {error}; see tmp/worker-surface-full-build.log',file=sys.stderr)
        sys.exit(1)
