#!/usr/bin/env python3
"""P0 headless PTY authority and fresh Metal GUI processes; not production IPC."""
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
SURFACE=runpy.run_path(str(ROOT/'scripts/probe-session-worker-surface.py'))
FULL,VT,PATCHES=(SURFACE[k] for k in ('FULL','VT','PATCHES'))
PROBES=ROOT/'daemon/probes'
SOURCES=tuple(PROBES/name for name in ('session-global-owner.h','session-global-io-test.c','session-global-owner-test.c','session-config-owner.h','session-config-api-test.c','session-process-owner.c','session-process-client.inc','session-appkit-host.inc','session-main-stack.h','session-main-stack-test.c','session-scheme-owner.c',
    'session-process-wire.c','session-process-wire.h','session-history-wire.c','session-history-wire.h','session-history-wire-test.c','session-result-wire.c','session-result-wire.h','session-result-wire-test.c','session-command-wire.c','session-command-wire.h','session-command-wire-test.c','session-summary-wire.c','session-summary-wire.h','session-summary-wire-test.c','session-summary-queue-test.c','session-surface-owner.m',
    'session-vt-owner.c','surface-restore-probe.h','session-process-wire-test.c'))+tuple(
    ROOT/'terminal/session-state'/name for name in ('gui-budget.c','gui-budget.h','gui-budget-probe.c','history-pruned-probe.c'))
MARK='GT_P0_PROCESS_MUTATION_MARK'
COMMAND_CASES=('valid','unknown','capacity','magic','flags','reserved','running-result','hidden-exit','hidden-time','length','nul','truncated','trailing')
SUMMARY_CASES=('valid','capacity','bounds','empty','magic','count','state','kind','progress','flags','field-length','nul','truncated','trailing','duplicate')
GUI_CASES=('runtime-allocator','threads','valid','bytes','slots','race','wire','lifetime','rollback','invalid','runtime','metadata','combined','combined-race')
WIRE_CASES=('slow-send','slow-receive','valid','blobs','invalid-send','magic','version','flags','kind','length','truncated','blob-kind','blob-length')

def build_identity(engine,work):
    # Hash actual linked artifacts, rather than trusting a human version string.
    # Length-prefix every record so file boundaries cannot alias concatenation.
    digest=hashlib.sha256(b'GHOSTERM-P0-ENGINE-BUILD-1')
    records=[('pin',subprocess.check_output(['git','-C',str(ROOT/'vendor/ghostty'),'rev-parse','HEAD']).strip()),
             ('target',b'macos-arm64'),('snapshot-contract',b'process-history-restore-1')]
    records.extend((f'patch/{i}',patch.read_bytes()) for i,patch in enumerate(PATCHES))
    for name,path in (
        ('vt',work/'out/lib/libghostty-vt.dylib'),
        ('full',engine/'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a')):
        records.append((name,hashlib.sha256(path.read_bytes()).digest()))
    for name,value in records:
        label=name.encode();digest.update(len(label).to_bytes(8,'big'));digest.update(label)
        digest.update(len(value).to_bytes(8,'big'));digest.update(value)
    value=digest.digest()
    (work/'session-process-build.h').write_text(
        '/* Generated from pinned patches and actual linked engine artifacts. */\n'
        'static const unsigned char process_engine_build[32]={'+','.join(str(b) for b in value)+'};\n')
    (work/'process-engine-build.txt').write_text(value.hex()+'\n')


def build(engine,work):
    build_identity(engine,work)
    common=['-Wall','-Wextra','-Werror','-I',str(engine/'include'),'-I',str(ROOT/'core/include'),
            '-I',str(ROOT/'core/src'),'-I',str(ROOT/'terminal/session-state'),'-I',str(PROBES)]
    cc=shlex.split(os.environ.get('CC','cc'))
    native=['-Wl,-rpath,'+str(work/'out/lib'),str(work/'out/lib/libghostty-vt.dylib'),
            str(ROOT/'core/build/libghosterm_core.a'),'-lpthread']
    objects=[]
    for name,path in (('history',work/'session-history-wire.c'),
                      ('wire',work/'session-process-wire.c'),
                      ('summary',work/'session-summary-wire.c'),
                      ('command',work/'session-command-wire.c'),
                      ('result',work/'session-result-wire.c'),
                      ('gui-budget',work/'gui-budget.c'),
                      ('budget',ROOT/'terminal/session-state/budget-posix.c'),
                      ('checkpoint',ROOT/'terminal/session-state/checkpoint-budget.c'),
                      ('frozen',ROOT/'terminal/session-state/frozen-budget.c')):
        obj=work/f'process-{name}.o'
        FULL['run']([*cc,'-std=c11','-D_DEFAULT_SOURCE',*common,'-c',str(path),'-o',str(obj)])
        objects.append(str(obj))
    server,client=(work/name for name in ('process-server','process-client'))
    FULL['run']([*cc,'-std=c11','-D_DEFAULT_SOURCE',*common,str(work/'session-process-owner.c'),
                 *objects,*native,'-o',str(server)])
    frameworks=[arg for name in FULL['FRAMEWORKS'] for arg in ('-framework',name)]
    fixture=work/'process-fixture.o'
    FULL['run']([*cc,'-std=c11','-D_DEFAULT_SOURCE','-DGHOSTERM_P0_FULL_SURFACE_REPLAY',
                 '-Dmain=ghosterm_worker_surface_fixture_main',*common,'-c',str(work/'session-vt-owner.c'),'-o',str(fixture)])
    # The include comes from work, so a client mutation never edits repository.
    FULL['run']([*cc,'-fobjc-arc','-DGHOSTERM_P0_PROCESS_CLIENT',*common,
                 str(work/'session-surface-owner.m'),str(fixture),*objects,*native,
                 str(engine/'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
                 '-lc++',*frameworks,'-o',str(client)])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-config-api-test.c'),*objects,*native,str(engine/'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),'-lc++',*frameworks,'-o',str(work/'process-config-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-global-owner-test.c'),*objects,*native,str(engine/'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),'-lc++',*frameworks,'-o',str(work/'process-global-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-global-io-test.c'),*objects,*native,str(engine/'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),'-lc++',*frameworks,'-o',str(work/'process-global-io-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-history-wire-test.c'),str(work/'session-history-wire.c'),str(work/'session-process-wire.c'),'-o',str(work/'process-history-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-process-wire-test.c'),
                 str(work/'session-process-wire.c'),'-o',str(work/'process-wire-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'gui-budget-probe.c'),
                 str(work/'process-gui-budget.o'),str(work/'process-budget.o'),*native,'-o',str(work/'process-gui-budget-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-summary-wire-test.c'),
                 str(work/'session-summary-wire.c'),str(ROOT/'core/src/session/program-status.c'),
                 '-o',str(work/'process-summary-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-summary-queue-test.c'),
                 str(work/'session-summary-wire.c'),str(work/'session-command-wire.c'),str(work/'session-process-wire.c'),
                 str(ROOT/'core/build/libghosterm_core.a'),'-pthread','-o',str(work/'process-summary-queue-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-command-wire-test.c'),
                 str(work/'session-command-wire.c'),'-o',str(work/'process-command-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-result-wire-test.c'),
                 str(work/'session-result-wire.c'),'-o',str(work/'process-result-test')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-scheme-owner.c'),str(work/'process-budget.o'),*native,'-o',str(work/'process-scheme-owner')])
    FULL['run']([*cc,'-std=c11',*common,str(work/'session-main-stack-test.c'),'-pthread','-o',str(work/'process-main-stack-test')])
    return server,client

def raw_probe(server,client,case,expected=0):
    # Reap the complete process group on timeout; GUI inherits the authority's
    # group. An observation timeout is a failure, never an implicit pass.
    process=subprocess.Popen([str(server),str(client),case],cwd=ROOT,stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:
        output,_=process.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid,signal.SIGKILL);output,_=process.communicate()
        sys.stdout.write(output);raise RuntimeError(f'{case}: process group timed out')
    finally:
        if process.poll() is None:
            os.killpg(process.pid,signal.SIGKILL);process.wait()
    sys.stdout.write(output)
    if process.returncode!=expected:raise RuntimeError(f'{case}: expected {expected}, got {process.returncode}')

def cleanup_tmux_socket(socket,identity):
    if os.path.lexists(socket):
        import socket as socket_module,stat
        info=socket.lstat()
        if identity!=(info.st_dev,info.st_ino) or not stat.S_ISSOCK(info.st_mode) or info.st_uid!=os.getuid():raise RuntimeError('tmux cleanup endpoint identity changed')
        with socket_module.socket(socket_module.AF_UNIX,socket_module.SOCK_STREAM) as test:
            test.settimeout(.2)
            try:test.connect(str(socket))
            except ConnectionRefusedError:pass
            else:raise RuntimeError('tmux endpoint still connectable after reaping owned server')
        after=socket.lstat()
        if identity!=(after.st_dev,after.st_ino):raise RuntimeError('tmux endpoint replaced during cleanup')
        socket.unlink()


def probe(server,client,case,expected=0):
    if case in ('less','vim'):
        if not (Path('/usr/bin')/case).is_file():raise RuntimeError('required native TUI executable missing: '+case)
        path=ROOT/'tmp'/f'process-tui-{os.getpid()}.txt';keys=('GHOSTERM_P0_TUI_FILE','TMUX','TMUX_PANE');previous={key:os.environ.get(key) for key in keys};created=False
        try:
            with path.open('x') as stream:
                created=True
                for i in range(1,201):stream.write(('FIRST' if i==1 else 'LAST' if i==200 else '')+f'LINE{i:03d}\n')
            os.environ['GHOSTERM_P0_TUI_FILE']=str(path);os.environ.pop('TMUX',None);os.environ.pop('TMUX_PANE',None)
            return raw_probe(server,client,case,expected)
        finally:
            if created:path.unlink()
            for key,value in previous.items():
                if value is None:os.environ.pop(key,None)
                else:os.environ[key]=value
    if case!='tmux':return raw_probe(server,client,case,expected)
    binary=shutil.which('tmux')
    if not binary:raise RuntimeError('tmux case requires an installed tmux; no silent skip')
    root=ROOT/'tmp';socket=root/f'process-tmux-{os.getpid()}.sock';config=root/f'process-tmux-{os.getpid()}.conf'
    if os.path.lexists(socket) or os.path.lexists(config):raise RuntimeError('tmux fixture path already exists')
    previous={k:os.environ.get(k) for k in ('GHOSTERM_P0_TMUX_SOCKET','GHOSTERM_P0_TMUX_BIN','TMUX','TMUX_PANE')}
    owned=None;identity=None;created=False
    try:
        with config.open('x') as f:created=True;f.write('set -s exit-empty off\nset -s exit-unattached off\nset -g status off\nset -g set-titles on\nset -g set-titles-string "Ghosterm tmux"\nset-window-option -g automatic-rename off\nset-window-option -g allow-rename off\n')
        os.environ['GHOSTERM_P0_TMUX_SOCKET']=str(socket);os.environ['GHOSTERM_P0_TMUX_BIN']=binary
        os.environ.pop('TMUX',None);os.environ.pop('TMUX_PANE',None)
        with (root/'process-tmux-server.log').open('w') as log:
            owned=subprocess.Popen([binary,'-D','-S',str(socket),'-f',str(config)],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=log,start_new_session=True)
            import time
            end=time.monotonic()+4
            while not socket.exists():
                if owned.poll() is not None or time.monotonic()>=end:raise RuntimeError('owned tmux server failed to start; see tmp/process-tmux-server.log')
                time.sleep(.01)
            import stat
            info=socket.lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid!=os.getuid():raise RuntimeError('unexpected owned tmux endpoint type/owner')
            identity=(info.st_dev,info.st_ino)
            subprocess.run([binary,'-N','-S',str(socket),'show-options','-gv','status'],stdout=subprocess.DEVNULL,stderr=log,timeout=3,check=True)
            print(subprocess.check_output([binary,'-V'],text=True).strip()+': owned foreground server in repository tmp',flush=True)
            raw_probe(server,client,case,expected)
            if owned.poll() is not None:raise RuntimeError('tmux workload server died before explicit cleanup')
    finally:
        try:
            if owned is not None and owned.poll() is None:
                try:subprocess.run([binary,'-N','-S',str(socket),'kill-server'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=3,check=True)
                finally:
                    try:owned.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        owned.terminate()
                        try:owned.wait(timeout=3)
                        except subprocess.TimeoutExpired:owned.kill();owned.wait(timeout=3)
            if owned is not None and owned.poll() is None:owned.kill();owned.wait(timeout=3)
        finally:
            if created:config.unlink(missing_ok=True)
            for k,v in previous.items():
                if v is None:os.environ.pop(k,None)
                else:os.environ[k]=v
        cleanup_tmux_socket(socket,identity)
    print('PASS tmux fixture: owned server reaped, socket/config removed',flush=True)


def manager_mutations(engine,work):
    server=work/'session-process-owner.c'
    original=server.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
    if MARK in text:raise RuntimeError('manager composition marker before baseline')
    variants=(
        ('normal registry escapes domain','gt__manager_open(1,&queue_allocator)','gt__manager_open(1,NULL)','roundtrip','start-error'),
        ('startup registry escapes domain','gt__manager_open(1,&boot_allocator)','gt__manager_open(1,NULL)','start-error','roundtrip'),
        ('normal registry allows second worker','gt__manager_open(1,&queue_allocator)','gt__manager_open(2,&queue_allocator)','roundtrip','start-error'),
        ('startup registry allows second worker','gt__manager_open(1,&boot_allocator)','gt__manager_open(2,&boot_allocator)','start-error','roundtrip'),
        ('GUI disconnect terminates managed session','process_running(&s);advance(&s);process_running(&s);','CHECK(gt__manager_terminate(s.manager,s.session)==GT_MANAGER_OK);process_running(&s);advance(&s);process_running(&s);','kill','roundtrip'),
        ('managed worker reap omitted','CHECK(gt__manager_reap(s->manager,s->session)==GT_MANAGER_OK);','/* managed reap omitted */','roundtrip','legacy'),
        ('worker context pin never released','s->managed_disposed=true;','s->managed_disposed=false;','roundtrip','legacy'),
        ('registry close omitted','CHECK(gt__manager_close(s->manager)==GT_MANAGER_OK);s->manager=NULL;s->session=0;','s->manager=NULL;s->session=0;','roundtrip','legacy'),
        ('retained record never forgotten','CHECK(s->managed_disposed && gt__manager_forget(s->manager,s->session)==GT_MANAGER_OK);','CHECK(s->managed_disposed);','roundtrip','legacy'),
        ('managed terminal result invented','*result=info.result;return true;','*result=(gt_worker_result_t){0};return true;','roundtrip','legacy'),
    )
    for label,old,new,target,control in variants:
        if text.count(old)!=1:raise RuntimeError('manager composition nonunique anchor/'+label)
    def managed_run(args,**kwargs):
        timeout=kwargs.pop('timeout',600);check=kwargs.pop('check',True)
        process=subprocess.Popen(args,start_new_session=True,**kwargs)
        try:out,err=process.communicate(timeout=timeout)
        except BaseException:
            if process.poll() is None:
                try:os.killpg(process.pid,signal.SIGKILL)
                except ProcessLookupError:pass
            process.wait();raise
        result=subprocess.CompletedProcess(args,process.returncode,out,err)
        if check and result.returncode:raise subprocess.CalledProcessError(result.returncode,args,output=out,stderr=err)
        return result
    def reject(a,b,case):
        result=managed_run([str(a),str(b),case],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30,check=False)
        print(result.stdout,end='',flush=True)
        if result.returncode!=1 or 'FAIL ' not in result.stdout:raise RuntimeError('manager composition missed assertion/'+case)
    def interrupted(signum,frame):raise RuntimeError('manager composition interrupted/'+str(signum))
    handlers={sig:signal.signal(sig,interrupted) for sig in (signal.SIGINT,signal.SIGTERM)}
    old_run=FULL['run'];FULL['run']=managed_run
    try:
        a,b=build(engine,work)
        for case in ['roundtrip','start-error','kill']:probe(a,b,case)
        for label,old,new,target,control in variants:
            try:
                server.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n')
                a,b=build(engine,work);reject(a,b,target)
                if control=='legacy':legacy(b)
                else:probe(a,b,control)
                print('PASS manager composition mutation/'+label+': target assertion red, same-mutant control green',flush=True)
            finally:
                server.write_bytes(original)
                if hashlib.sha256(server.read_bytes()).digest()!=digest:raise RuntimeError('manager composition SHA256 rollback')
        a,b=build(engine,work)
        for case in ['roundtrip','start-error','kill']:probe(a,b,case)
        legacy(b)
        print('PASS 10 manager composition mutations, source SHA256 rollback and native baselines',flush=True)
    finally:
        server.write_bytes(original)
        FULL['run']=old_run
        for sig,handler in handlers.items():signal.signal(sig,handler)
        if hashlib.sha256(server.read_bytes()).digest()!=digest:raise RuntimeError('manager final SHA256 rollback')


def global_io_mutations(engine,work):
    import re
    stdio=engine/'src/lib/p0_threaded_io.zig';global_=engine/'src/global.zig';registry=engine/'src/apprt/p0_io_threads.zig'
    api=engine/'src/main_c.zig';entry=engine/'src/apprt/p0_thread.zig'
    helper=work/'session-global-owner.h';client=work/'session-process-client.inc'
    originals={p:p.read_bytes() for p in (stdio,global_,registry,api,entry,helper,client)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('global IO mutation marker before baseline')
    variants=[]
    unowned='''
fn p0UnownedSpawn(t: *Threaded, comptime function: anytype, args: anytype) !P0Handle {
    return .{ .legacy = try std.Thread.spawn(.{ .stack_size = t.stack_size }, function, args) };
}
'''
    text=originals[stdio].decode()
    for name,target,control in (('async','1','0'),('concurrent','0','1'),('groupAsync','3','2'),('groupConcurrent','2','3')):
        start=text.index('\nfn '+name+'(')+1
        following=re.search(r'\nfn [A-Za-z_][A-Za-z_0-9]*\(',text[start+1:])
        if following is None:raise RuntimeError('missing following Threaded function')
        end=start+1+following.start();old=text[start:end]
        if old.count('p0Spawn(t, worker, .{t})')!=1:raise RuntimeError('nonunique owned spawn/'+name)
        new=old.replace('p0Spawn(t, worker, .{t})','p0UnownedSpawn(t, worker, .{t})')+unowned
        variants.append((name+' bypasses owned spawn',stdio,old,new,('unit',target),('unit',control)))
    variants.extend((
        ('async limit missing',global_,'.async_limit = if (host_allocator != null) .limited(P0Threads.limit) else null,','.async_limit = if (host_allocator != null) .unlimited else null,',('unit','1'),('unit','0')),
        ('concurrent limit missing',global_,'.concurrent_limit = if (host_allocator != null) .limited(P0Threads.limit) else .unlimited,','.concurrent_limit = .unlimited,',('unit','0'),('unit','1')),
        ('entry frame escapes host',registry,'const thread = Owned.spawn(allocator, config, Wrapper.run,','_ = allocator;const thread = Owned.spawn(std.heap.c_allocator, config, Wrapper.run,',('unit','0'),('unit','idle')),
        ('global never joins owned entries',global_,'    self.io_threads.join();','    // join omitted',('unit','0'),('unit','idle')),
        ('join counter without actual wait',registry,'thread.?.join();self.joined += 1;','_ = thread;self.joined += 1;',('unit','join'),('unit','idle')),
        ('failed spawn consumes slot',registry,'_ = self.failures.fetchAdd(1, .acq_rel);return err;','self.count += 1;_ = self.failures.fetchAdd(1, .acq_rel);return err;',('unit','deny'),('unit','0')),
        ('failed spawn not reported',registry,'_ = self.failures.fetchAdd(1, .acq_rel);return err;','return err;',('unit','deny'),('unit','0')),
        ('entry observer skipped',registry,'if (owner.observe) |observe| observe(owner.context, @intCast(index), 0);','// entry observer skipped',('unit','0'),('unit','idle')),
        ('exit observer skipped',registry,'if (owner.observe) |observe| observe(owner.context, @intCast(index), 1);','// exit observer skipped',('unit','0'),('unit','idle')),
        ('signal memory freed while registered',entry,'defer signal.restore() catch unreachable;','// signal restore omitted',('unit','0'),('unit','idle')),
        ('allocator observer unrelated',api,'out.* = @intFromPtr(allocator.ptr);return true;','_ = allocator;out.* = @intFromPtr(&p0_io_retired);return true;',('unit','0'),('unit','idle')),
        ('retired IO state unavailable',api,'if (phase == 4 and p0_io_retired.load(.acquire))','if (false and phase == 4 and p0_io_retired.load(.acquire))',('unit','0'),('legacy','continuation')),
        ('native IO stack doubles',global_,'.stack_size = P0Threads.stack_size,','.stack_size = 2 * P0Threads.stack_size,',('unit','0'),('unit','idle')),
        ('GUI native IO admission omitted',helper,'if(!native || p0_gui_reserve(pool,native,&o->threads)!=P0_GUI_OK) {(void)p0_global_close(o);return false;}','if(!native) {(void)p0_global_close(o);return false;}',('native','roundtrip'),('unit','idle')),
        ('GUI native IO lease leaked',helper,'if(p0_gui_unreserve(o->threads)!=P0_GUI_OK)return false;','if(false && p0_gui_unreserve(o->threads)!=P0_GUI_OK)return false;',('native','roundtrip'),('unit','idle')),
        ('total omits native IO capacity',client,'+c.global_reserve+c.global_io_reserve,64*MIB','+c.global_reserve,64*MIB',('native','roundtrip'),('legacy','continuation')),
        ('GUI never exercises actual IO',client,'CHECK(ghostty_p0_global_io_probe(0));','// actual IO skipped',('native','roundtrip'),('legacy','continuation')),
        ('native IO guards uncharged',helper,'size_t per=stack+2*(size_t)page;','size_t per=stack;',('native','roundtrip'),('unit','idle')),
        ('native lease dropped before global teardown',helper,'if(o->initialized) {','if(o->initialized) {\n        if(o->threads){(void)p0_gui_unreserve(o->threads);o->threads=NULL;}',('native','roundtrip'),('unit','idle')),
        ('observer reassigned while workers live',stdio,'if (registry.count != 0) return false;','// live observer replacement accepted',('unit','0'),('unit','idle')),
        ('failure statistics hidden',registry,'.failures = self.failures.load(.acquire),','.failures = 0,',('unit','deny'),('unit','0')),
        ('idle barrier refuses empty pool',stdio,'return t.busy_count == 0;','return false;',('unit','0'),('unit','idle')),
        ('probe pretends work succeeded',api,'if (p0_global_phase.load(.acquire) != 3 or kind > 4) return false;','if (p0_global_phase.load(.acquire) != 3 or kind > 4) return false;\n    if(kind == 0)return true;',('unit','0'),('unit','1')),
    ))
    for label,path,old,new,target,control in variants:
        if originals[path].decode().count(old)!=1:raise RuntimeError(label+': nonunique IO mutation anchor')
    if len(variants)!=27:raise RuntimeError('global IO mutation count drift')
    def managed_run(args,**kwargs):
        timeout=kwargs.pop('timeout',600);check=kwargs.pop('check',True)
        process=subprocess.Popen(args,start_new_session=True,**kwargs)
        try:out,err=process.communicate(timeout=timeout)
        except BaseException:
            if process.poll() is None:
                try:os.killpg(process.pid,signal.SIGKILL)
                except ProcessLookupError:pass
            process.wait();raise
        result=subprocess.CompletedProcess(args,process.returncode,out,err)
        if check and result.returncode:raise subprocess.CalledProcessError(result.returncode,args,output=out,stderr=err)
        return result
    old_run=FULL['run'];globals_=FULL['build'].__globals__;old_build_run=globals_['run']
    FULL['run']=managed_run;globals_['run']=managed_run
    def invoke(a,b,case,reject=False):
        kind,name=case
        command=[str(work/'process-global-io-test'),name,str(ROOT/'tmp')] if kind=='unit' else [str(b),name] if kind=='legacy' else [str(a),str(b),name]
        result=managed_run(command,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30,check=False)
        if reject:
            marker='FAIL line ' if kind=='unit' else 'FAIL '
            if result.returncode!=1 or marker not in result.stdout:raise RuntimeError('IO mutation did not fail its assertion: '+result.stdout)
        elif result.returncode:raise RuntimeError('IO control failed: '+result.stdout)
        if kind!='unit':print(result.stdout,end='',flush=True)
    try:
        FULL['build'](engine,work,cache_dir=work/'full-cache',process_run=managed_run,log_path=ROOT/'tmp/global-io-mutation-baseline.log')
        a,b=build(engine,work);global_io_tests(work);probe(a,b,'roundtrip');probe(a,b,'scheme')
        for label,path,old,new,target,control in variants:
            try:
                path.write_text(originals[path].decode().replace(old,new)+'\n// '+MARK+'\n')
                FULL['build'](engine,work,cache_dir=work/'full-cache',process_run=managed_run,log_path=ROOT/'tmp/global-io-mutation-build.log')
                a,b=build(engine,work);invoke(a,b,target,True);invoke(a,b,control)
                print('PASS global IO mutation/'+label+': target red, control green',flush=True)
            finally:
                path.write_bytes(originals[path])
                if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('global IO SHA256 rollback mismatch')
        FULL['build'](engine,work,cache_dir=work/'full-cache',process_run=managed_run,log_path=ROOT/'tmp/global-io-mutation-rollback.log')
        a,b=build(engine,work);global_io_tests(work);probe(a,b,'roundtrip');probe(a,b,'scheme')
        print('PASS 27 global IO mutations, assertion rejection, compiler groups, SHA256 rollback and native baselines',flush=True)
    finally:
        for path,data in originals.items():
            path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('global IO final SHA256 rollback mismatch')
        FULL['run']=old_run;globals_['run']=old_build_run


def global_owner_mutations(engine,work):
    api=engine/'src/main_c.zig';helper=work/'session-global-owner.h'
    client=work/'session-process-client.inc';entry=work/'session-surface-owner.m'
    variants=(
        ('observer hides owned allocator',api,'return @intFromPtr(global.alloc().ptr);','return 0;',('unit','valid'),('unit','invalid')),
        ('observer reports unrelated identity',api,'return @intFromPtr(global.alloc().ptr);','return @intFromPtr(&p0_global_phase);',('unit','valid'),('unit','invalid')),
        ('observer reports retired allocator',api,'    if (p0_global_phase.load(.acquire) != 3) return 0;','    // retired phase guard omitted',('unit','valid'),('unit','invalid')),
        ('null owner open pretends success',helper,'if(!o || o->initialized || o->reservation || o->threads)return false;','if(!o)return true; if(o->initialized || o->reservation || o->threads)return false;',('unit','invalid'),('unit','valid')),
        ('null owner close pretends success',helper,'if(!o)return false;','if(!o)return true;',('unit','invalid'),('unit','valid')),
        ('failed init retains admission',helper,'if(ghostty_p0_init(0,NULL,allocator)!=0) {(void)p0_global_close(o);return false;}','if(ghostty_p0_init(0,NULL,allocator)!=0) {return false;}',('unit','capacity'),('unit','invalid')),
        ('failed init publishes ownership',helper,'if(ghostty_p0_init(0,NULL,allocator)!=0) {(void)p0_global_close(o);return false;}','if(ghostty_p0_init(0,NULL,allocator)!=0) { /* ignored initialization failure */ }',('unit','capacity'),('unit','invalid')),
        ('successful init loses ownership',helper,'o->initialized=true;','/* ownership omitted */',('unit','valid'),('unit','invalid')),
        ('owner omits global teardown',helper,'if(o->initialized) {','if(false && o->initialized) {',('unit','valid'),('unit','capacity')),
        ('BUSY lease lost',helper,'if(p0_gui_unreserve(o->reservation)!=P0_GUI_OK)return false;','(void)p0_gui_unreserve(o->reservation);',('unit','busy'),('unit','valid')),
        ('GUI initializes before pool admission',entry,'if(!process){CHECK(ghostty_init(0,NULL)==0);surface_config=ghostty_config_new();','CHECK(ghostty_init(0,NULL)==0);if(!process){surface_config=ghostty_config_new();',('native','roundtrip'),('legacy','continuation')),
        ('GUI bypasses global lease',client,'CHECK(p0_global_open(c.pool,c.global_reserve,&c.global_owner));','CHECK(ghostty_init(0,NULL)==0);',('native','roundtrip'),('legacy','continuation')),
        ('GUI never closes global owner',client,'CHECK(p0_global_close(&c->global_owner));','/* global close omitted */',('native','roundtrip'),('legacy','continuation')),
        ('total omits global capacity',client,'+2*c.config_reserve+c.global_reserve+c.global_io_reserve,64*MIB','+2*c.config_reserve+c.global_io_reserve,64*MIB',('native','roundtrip'),('legacy','continuation')),
        ('capacity contract exercise omitted',client,'process_global_capacity(&c);','if(false)process_global_capacity(&c);',('native','roundtrip'),('legacy','continuation')),
    )
    originals={p:p.read_bytes() for p in (api,helper,client,entry)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('global owner mutation marker before baseline')
    for label,path,old,new,target,control in variants:
        if originals[path].decode().count(old)!=1:raise RuntimeError(label+': nonunique global owner mutation anchor')
    def managed_run(args,**kwargs):
        timeout=kwargs.pop('timeout',600);check=kwargs.pop('check',True)
        process=subprocess.Popen(args,start_new_session=True,**kwargs)
        try:out,err=process.communicate(timeout=timeout)
        except BaseException:
            if process.poll() is None:
                try:os.killpg(process.pid,signal.SIGKILL)
                except ProcessLookupError:pass
            process.wait();raise
        result=subprocess.CompletedProcess(args,process.returncode,out,err)
        if check and result.returncode:raise subprocess.CalledProcessError(result.returncode,args,output=out,stderr=err)
        return result
    old_run=FULL['run'];globals_=FULL['build'].__globals__;old_build_run=globals_['run']
    FULL['run']=managed_run;globals_['run']=managed_run
    def invoke(a,b,case,reject=False):
        kind,name=case
        if kind=='native':
            if not reject:probe(a,b,name);return
            result=managed_run([str(a),str(b),name],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30,check=False)
            print(result.stdout,end='',flush=True)
            if result.returncode!=1 or 'FAIL ' not in result.stdout:raise RuntimeError('global native mutation did not fail its assertion: '+result.stdout)
            return
        if kind=='legacy':
            result=managed_run([str(b),name],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=30)
            sys.stdout.write(result.stdout);sys.stderr.write(result.stderr);return
        result=managed_run([str(work/'process-global-test'),name,str(ROOT/'tmp')],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=20,check=False)
        if reject:
            if result.returncode!=1 or 'FAIL line ' not in result.stderr:raise RuntimeError('global owner unit did not fail its assertion: '+result.stderr)
        elif result.returncode or 'PASS global-lease/'+name+':' not in result.stdout:raise RuntimeError('global owner baseline failed: '+result.stdout+result.stderr)
    try:
        FULL['build'](engine,work,cache_dir=work/'full-cache',process_run=managed_run,log_path=ROOT/'tmp/global-integration-mutation-baseline.log')
        a,b=build(engine,work);global_owner_tests(work);probe(a,b,'roundtrip');probe(a,b,'scheme')
        for label,path,old,new,target,control in variants:
            try:
                path.write_text(originals[path].decode().replace(old,new)+'\n// '+MARK+'\n')
                FULL['build'](engine,work,cache_dir=work/'full-cache',process_run=managed_run,log_path=ROOT/'tmp/global-integration-mutation-build.log')
                a,b=build(engine,work)
                invoke(a,b,target,True);invoke(a,b,control)
                print('PASS global owner mutation/'+label+': target red, control green',flush=True)
            finally:
                path.write_bytes(originals[path])
                if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('global owner SHA256 rollback mismatch')
        FULL['build'](engine,work,cache_dir=work/'full-cache',process_run=managed_run,log_path=ROOT/'tmp/global-integration-mutation-rollback.log')
        a,b=build(engine,work);global_owner_tests(work);probe(a,b,'roundtrip');probe(a,b,'scheme')
        print('PASS 15 global owner mutations, assertion rejection, compiler groups, SHA256 rollback and native baselines',flush=True)
    finally:
        for path,data in originals.items():
            path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('global owner final SHA256 rollback mismatch')
        FULL['run']=old_run;globals_['run']=old_build_run


def config_mutations(engine,work):
    api=engine/'src/config/CApi.zig';config=engine/'src/config/Config.zig'
    client=work/'session-process-client.inc';helper=work/'session-config-owner.h'
    variants=(
        ('constructor ignores host',api,'return configNew(allocator.zig());','_ = allocator; return configNew(global.alloc());',('api','valid','valid'),('api','valid','null')),
        ('clone ignores destination',api,'return configClone(self, allocator.zig());','_ = allocator; return configClone(self, global.alloc());',('api','valid','valid'),('api','valid','null')),
        ('ordinary clone escapes owner',api,'return configClone(self, configOwner(self));','return configClone(self, global.alloc());',('api','valid','public-clone'),('api','valid','valid')),
        ('free omits owned storage',api,'v.deinit();\n        alloc.destroy(v);','_ = alloc;',('api','valid','valid'),('api','valid','null')),
        ('file staging escapes budget',config,'const content = try file_reader.interface.allocRemaining(alloc, .limited(max_bytes + 1));\n    defer alloc.free(content);','const content = try file_reader.interface.allocRemaining(global.alloc(), .limited(max_bytes + 1));\n    defer global.alloc().free(content);',('api','exact-bytes','load-capacity'),('api','valid','valid')),
        ('line bound omitted',config,'if (line.len > cli.args.LineIterator.MAX_LINE_SIZE - 2) return error.ConfigLineTooLong;','_ = line;',('api','long-line','invalid'),('api','valid','valid')),
        ('byte bound doubled',config,'const max_bytes = 1024 * 1024;','const max_bytes = 2 * 1024 * 1024;',('api','oversized','invalid'),('api','valid','valid')),
        ('exact byte limit rejected',config,'.limited(max_bytes + 1)','.limited(max_bytes)',('api','exact-bytes','valid'),('api','oversized','invalid')),
        ('BOM counted against first line',config,'const text = if (std.mem.startsWith(u8, content, "\\xef\\xbb\\xbf")) content[3..] else content;','const text = content;',('api','edge-bom','edge'),('api','edge','edge')),
        ('read failure reported successful',api,'self.loadFileStrict(configOwner(self), path_slice) catch return false;','self.loadFileStrict(configOwner(self), path_slice) catch return true;',('api','missing','invalid'),('api','valid','valid')),
        ('load diagnostics ignored',api,'self.loadFileStrict(configOwner(self), path_slice) catch return false;\n    return self._diagnostics.empty();','self.loadFileStrict(configOwner(self), path_slice) catch return false;\n    return true;',('api','invalid-key','invalid'),('api','valid','valid')),
        ('finalize diagnostics ignored',api,'self.finalize() catch return false;\n    return self._diagnostics.empty();','self.finalize() catch return false;\n    return true;',('api','invalid-key','finalize-invalid'),('api','valid','valid')),
        ('partial constructor object leaked',api,'result.* = Config.default(alloc) catch {\n        alloc.destroy(result);\n        return null;','result.* = Config.default(alloc) catch {\n        // object leaked\n        return null;',('api','valid','ctor-second'),('api','valid','valid')),
        ('constructor error pretends success',api,'result.* = Config.default(alloc) catch {\n        alloc.destroy(result);\n        return null;','result.* = Config.default(alloc) catch {\n        return result;',('api','valid','ctor-second'),('api','valid','valid')),
        ('failed constructor lease leaked',helper,'if(!o->value || !p0_gui_reserved_healthy(o->reservation)){(void)p0_config_close(o);return false;}','if(!o->value || !p0_gui_reserved_healthy(o->reservation)){return false;}',('native','roundtrip',''),('legacy','continuation','')),
        ('invalid load transaction retained',helper,'if(!ghostty_p0_config_load_file(o->value,path) || !p0_gui_reserved_healthy(o->reservation)){(void)p0_config_close(o);return false;}','if(!ghostty_p0_config_load_file(o->value,path) || !p0_gui_reserved_healthy(o->reservation)){return false;}',('native','roundtrip',''),('legacy','continuation','')),
        ('root config not closed',client,'CHECK(p0_config_close(&c->config_owner));surface_config=NULL;','surface_config=NULL;',('native','roundtrip',''),('legacy','continuation','')),
        ('native failure exercise skipped',client,'process_config_failure(&c);','if(false)process_config_failure(&c);',('native','roundtrip',''),('legacy','continuation','')),
        ('scheme lease not closed',client,'CHECK(p0_config_close(&config));CHECK(p0_gui_pool_read(c->pool).committed==committed);','CHECK(p0_gui_pool_read(c->pool).committed==committed);',('native','scheme',''),('native','roundtrip','')),
        ('scheme config never loaded',client,'CHECK(p0_config_load(&config,path) && p0_config_finish(&config));','CHECK(p0_config_finish(&config));',('native','scheme',''),('native','roundtrip','')),
        ('root bypasses admission',client,'CHECK(p0_config_open(c.pool,c.config_reserve,&c.config_owner) && p0_config_finish(&c.config_owner));','c.config_owner.value=ghostty_config_new();CHECK(c.config_owner.value && ghostty_p0_config_finalize(c.config_owner.value));',('native','roundtrip',''),('legacy','continuation','')),
        ('missing host falls back globally',api,'const allocator = host orelse return null;\n    return configNew(allocator.zig());','const allocator = host orelse return configNew(global.alloc());\n    return configNew(allocator.zig());',('api','valid','null'),('legacy','continuation','')),
        ('missing clone source returns config',api,'const self = ptr orelse return null;\n    const allocator = host orelse return null;','const self = ptr orelse return configNew(global.alloc());\n    const allocator = host orelse return null;',('api','valid','null'),('legacy','continuation','')),
        ('missing clone destination falls back',api,'const allocator = host orelse return null;\n    return configClone(self, allocator.zig());','const allocator = host orelse return configClone(self, global.alloc());\n    return configClone(self, allocator.zig());',('api','valid','valid'),('legacy','continuation','')),
        ('null load reported successful',api,'export fn ghostty_p0_config_load_file(ptr: ?*Config, path: ?[*:0]const u8) bool {\n    const self = ptr orelse return false;','export fn ghostty_p0_config_load_file(ptr: ?*Config, path: ?[*:0]const u8) bool {\n    const self = ptr orelse return true;',('api','valid','null'),('legacy','continuation','')),
        ('missing path reported successful',api,'const source = path orelse return false;','const source = path orelse return true;',('api','valid','valid'),('legacy','continuation','')),
        ('relative path reported successful',api,'if (!std.fs.path.isAbsolute(path_slice)) return false;','if (!std.fs.path.isAbsolute(path_slice)) return true;',('api','valid','valid'),('legacy','continuation','')),
        ('null finalize reported successful',api,'export fn ghostty_p0_config_finalize(ptr: ?*Config) bool {\n    const self = ptr orelse return false;','export fn ghostty_p0_config_finalize(ptr: ?*Config) bool {\n    const self = ptr orelse return true;',('api','valid','null'),('legacy','continuation','')),
    )
    originals={p:p.read_bytes() for p in (api,config,client,helper)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('config mutation marker before baseline')
    for label,path,old,new,target,control in variants:
        if originals[path].decode().count(old)!=1:raise RuntimeError(label+': nonunique config mutation anchor')
    # Own compiler groups so cancellation reaps descendants before SHA rollback.
    def managed_run(args,**kwargs):
        timeout=kwargs.pop('timeout',600);check=kwargs.pop('check',True)
        process=subprocess.Popen(args,start_new_session=True,**kwargs)
        try:out,err=process.communicate(timeout=timeout)
        except BaseException:
            if process.poll() is None:
                try:os.killpg(process.pid,signal.SIGKILL)
                except ProcessLookupError:pass
            process.wait();raise
        result=subprocess.CompletedProcess(args,process.returncode,out,err)
        if check and result.returncode:raise subprocess.CalledProcessError(result.returncode,args,output=out,stderr=err)
        return result
    old_run=FULL['run'];globals_=FULL['build'].__globals__;old_build_run=globals_['run']
    FULL['run']=managed_run;globals_['run']=managed_run
    def invoke(a,b,case,reject=False):
        kind,name,mode=case
        if kind=='native':probe(a,b,name,1 if reject else 0);return
        if kind=='legacy':
            result=managed_run([str(b),name],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=30)
            sys.stdout.write(result.stdout);sys.stderr.write(result.stderr);return
        directory=work/'config-fixtures';path=directory if name=='directory' else directory/(name+'.conf')
        result=managed_run([str(work/'process-config-test'),str(path),mode],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=20,check=False)
        if reject:
            if result.returncode!=1 or 'FAIL line ' not in result.stderr:raise RuntimeError('config unit did not reject with its assertion: '+result.stderr)
        elif result.returncode:raise RuntimeError('config unit baseline failed: '+result.stderr)
    try:
        FULL['build'](engine,work,cache_dir=work/'full-cache',process_run=managed_run,log_path=ROOT/'tmp/config-mutation-baseline.log')
        a,b=build(engine,work);config_cases(work);probe(a,b,'roundtrip');probe(a,b,'scheme')
        for label,path,old,new,target,control in variants:
            try:
                path.write_text(originals[path].decode().replace(old,new)+'\n// '+MARK+'\n')
                FULL['build'](engine,work,cache_dir=work/'full-cache',process_run=managed_run,log_path=ROOT/'tmp/config-mutation-engine.log')
                a,b=build(engine,work)
                invoke(a,b,target,True);invoke(a,b,control)
                print('PASS config mutation/'+label+': target red, control green',flush=True)
            finally:
                path.write_bytes(originals[path])
                if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('config SHA256 rollback mismatch')
        FULL['build'](engine,work,cache_dir=work/'full-cache',process_run=managed_run,log_path=ROOT/'tmp/config-mutation-rollback.log')
        a,b=build(engine,work);config_cases(work);probe(a,b,'roundtrip');probe(a,b,'scheme')
        print('PASS 28 config mutations, asserted unit rejection, compiler groups, SHA256 rollback and native baselines',flush=True)
    finally:
        for path,data in originals.items():
            path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('config SHA256 rollback mismatch')
        FULL['run']=old_run;globals_['run']=old_build_run


def config_cases(work):
    directory=work/'config-fixtures';directory.mkdir(exist_ok=True)
    valid='background = #abcdef\ntitle = owned config allocation\n'
    edge='title = '+'x'*4086+'\nbackground = #abcdef\n'
    exact=valid+'#\n'*((1024*1024-len(valid))//2)+'#'*((1024*1024-len(valid))%2)
    values={'valid':valid,'bom':'\ufeff'+valid,'no-newline':valid.rstrip('\n'),'empty':'','bom-empty':'\ufeff',
            'edge':edge,'edge-bom':'\ufeff'+edge,'exact-bytes':exact,'long-line':'x'*100000+'\n'+valid,
            'oversized':'#\n'*(1024*1024//2+1),'invalid-key':'not-a-config-key = value\n'}
    for name,text in values.items():(directory/(name+'.conf')).write_text(text)
    def invoke(name,mode):
        path=directory if name=='directory' else directory/(name+'.conf')
        FULL['run']([str(work/'process-config-test'),str(path),mode])
    for name in values:
        mode='default' if name in ('empty','bom-empty') else 'edge' if name in ('edge','edge-bom') else 'invalid' if name in ('long-line','oversized','invalid-key') else 'valid'
        invoke(name,mode)
    for name in ('directory','missing'):invoke(name,'invalid')
    for name,mode in (('valid','capacity'),('exact-bytes','load-capacity'),('invalid-key','finalize-invalid'),('valid','null'),('valid','public-clone'),('valid','ctor-second')):invoke(name,mode)
    print('PASS 19 config API baselines: bounds/BOM/empty/errors, actual capacity denial, independent clone and owner reclamation',flush=True)


def global_io_tests(work):
    for case in ('0','1','2','3','4','deny','reuse','join','idle'):
        result=subprocess.run([str(work/'process-global-io-test'),case,str(ROOT/'tmp')],capture_output=True,text=True,timeout=20)
        if result.returncode or 'PASS global-io/'+case+':' not in result.stdout:
            raise RuntimeError('global IO/'+case+': '+result.stdout+result.stderr)
        print(result.stdout,end='',flush=True)
    print('PASS 9 global IO baselines: all four dispatch paths, cancellation, denied frame retry, reuse, actual join wait and idle',flush=True)


def global_owner_tests(work):
    for case in ('invalid','valid','admission','capacity','busy'):
        result=subprocess.run([str(work/'process-global-test'),case,str(ROOT/'tmp')],capture_output=True,text=True,timeout=30)
        if result.returncode or 'PASS global-lease/'+case+':' not in result.stdout:
            raise RuntimeError('global lease/'+case+': '+result.stdout+result.stderr)
        print(result.stdout,end='',flush=True)
    print('PASS 5 global-owner lease baselines: identity, capacity denial, admission retry and BUSY reclamation',flush=True)


def wire_cases(work):
    global_io_tests(work)
    global_owner_tests(work)
    config_cases(work)
    for case in ('valid','invalid'):FULL['run']([str(work/'process-history-test'),case])
    print('PASS history-wire: literal tri-state, bounds and transactional rejection',flush=True)
    FULL['run']([str(work/'process-main-stack-test'),'all'])
    for case in ('valid','capacity','magic','generation','flags','kind','status','sequence','exit','reserved','length'):FULL['run']([str(work/'process-result-test'),case])
    print('PASS 11 result-wire baselines',flush=True)
    for case in COMMAND_CASES:FULL['run']([str(work/'process-command-test'),case])
    print('PASS 13 command-wire baselines',flush=True)
    for case in ('largest','pressure'):FULL['run']([str(work/'process-summary-queue-test'),case])
    print('PASS 2 maximal summary queue baselines',flush=True)
    for case in SUMMARY_CASES:FULL['run']([str(work/'process-summary-test'),case])
    print('PASS 15 summary-wire baselines',flush=True)
    for case in GUI_CASES:FULL['run']([str(work/'process-gui-budget-test'),case])
    print(f'PASS {len(GUI_CASES)} shared GUI budget baselines',flush=True)
    for case in WIRE_CASES:
        FULL['run']([str(work/'process-wire-test'),case])
    print('PASS 13 process-wire baselines: literal oracle, split I/O, bounds and rejects',flush=True)


def legacy(client,case='continuation'):
    result=subprocess.run([str(client),case],cwd=ROOT,capture_output=True,text=True,timeout=30)
    sys.stdout.write(result.stdout);sys.stderr.write(result.stderr)
    if result.returncode:raise RuntimeError(f'legacy/{case}: failed {result.returncode}')


def gui_mutations(engine,work):
    path=work/'gui-budget.c';original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
    variants=(
        ('unreserved growth headroom',
         'p0_budget_scope_admit(scopes[i],&p->budget,limits[i])',
         '(p0_budget_scope_init(scopes[i],&p->budget,limits[i]),true)','valid','invalid'),
        ('surface count limit ignored','if(p->surfaces>=p->limits.surfaces)',
         'if(false && p->surfaces>=p->limits.surfaces)','slots','valid'),
        ('failed admission scope leaked','for(unsigned j=0;j<=i;j++)',
         'for(unsigned j=0;j<i;j++)','rollback','valid'),
        ('partial admission promises retained','for(unsigned j=0;j<=i;j++)',
         'for(unsigned j=i;j<=i;j++)','rollback','valid'),
        ('failed admission consumes a surface slot',
         'for(unsigned j=0;j<=i;j++)(void)p0_budget_scope_close(scopes[j]);\n            metadata_free(p,b,sizeof(*b));pthread_mutex_unlock(&p->admission);return P0_GUI_LIMIT;',
         'for(unsigned j=0;j<=i;j++)(void)p0_budget_scope_close(scopes[j]);\n            p->surfaces++;metadata_free(p,b,sizeof(*b));pthread_mutex_unlock(&p->admission);return P0_GUI_LIMIT;','rollback','valid'),
        ('faulted empty lease can never release',
         'if(!p0_gui_idle(b))','if(!p0_gui_healthy(b) || !p0_gui_idle(b))','wire','valid'),
        ('live lease released too early','if(!p0_gui_idle(b))','if(false && !p0_gui_idle(b))','lifetime','valid'),
        ('released surface slot retained','p->surfaces--;','/* count retained */','valid','invalid'),
        ('wire fault omitted from surface health',
         'p0_budget_result(&b->wire,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS','true','wire','valid'),
        ('snapshot bytes bypass admitted provider',
         'return a->vtable->alloc(a->ctx,n,0,0);','(void)a;return malloc(n);','wire','valid'),
        ('wire usage read from wrong role','u.by_kind[P0_BUDGET_SCRATCH]',
         'u.by_kind[P0_BUDGET_GENERAL]','wire','valid'),
        ('receive buffers omitted from idle check',
         'for(unsigned i=0;i<3;i++) {\n        p0_budget_usage u=',
         'for(unsigned i=0;i<2;i++) {\n        p0_budget_usage u=','wire','valid'),
        ('live pool not reported busy',
         'if(p->surfaces || p->reservations) {pthread_mutex_unlock(&p->admission);return P0_GUI_BUSY;}',
         'if(false && p->surfaces) {pthread_mutex_unlock(&p->admission);return P0_GUI_BUSY;}','lifetime','valid'),
        ('overflow policy accepted',
         'limits.general>SIZE_MAX-limits.pages || limits.general+limits.pages>SIZE_MAX-limits.wire',
         'false','invalid','valid'),
    )
    for label,old,new,target,control in variants:
        if MARK in text or text.count(old)!=1:raise RuntimeError(f'nonunique GUI mutation: {label}')
        try:
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n')
            a,b=build(engine,work)
            result=subprocess.run([str(work/'process-gui-budget-test'),target],stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL,timeout=10)
            if not result.returncode:raise RuntimeError(label+': falsely green')
            FULL['run']([str(work/'process-gui-budget-test'),control])
            if label=='unreserved growth headroom':
                race=subprocess.run([str(work/'process-gui-budget-test'),'race'],stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL,timeout=10)
                if not race.returncode:raise RuntimeError('race falsely green without reservations')
            if label in ('snapshot bytes bypass admitted provider','wire fault omitted from surface health'):
                probe(a,b,'roundtrip',1);legacy(b)
            print(f'PASS GUI mutation/{label}: target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('GUI rollback SHA mismatch')


def summary_mutations(work):
    path=work/'session-summary-wire.c';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('summary magic accepted','memcmp(p,"GPS1",4)','false','magic'),
        ('summary flags accepted','r.state>4 || p[offset+3]','r.state>4','flags'),
        ('summary duplicate id accepted',' || gt__program_status_count(s)!=(size_t)i+1','','duplicate'),
        ('summary trailing bytes accepted','if(offset!=n)goto fail;','/* trailing bytes accepted */','trailing'),
        ('summary embedded NUL accepted',' || memchr(p+offset,0,length)','','nul'),
        ('summary buffer capacity ignored','need>capacity','false','capacity'),
        ('summary record limit ignored','count>PSW_RECORDS ||','false ||','bounds'),
        ('summary message silently lost','memcpy(p+offset,fields[j],n);','memset(p+offset,0,n);','valid'),
    )
    for label,old,new,target in variants:
        if MARK in text or text.count(old)!=1:raise RuntimeError('nonunique '+label)
        try:
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n')
            FULL['run']([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                         '-I',str(PROBES),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/include'),
                         str(path),str(work/'session-summary-wire-test.c'),str(ROOT/'core/src/session/program-status.c'),
                         '-o',str(work/'process-summary-test')])
            result=subprocess.run([str(work/'process-summary-test'),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)
            if not result.returncode:raise RuntimeError(label+': falsely green')
            FULL['run']([str(work/'process-summary-test'),'empty' if target=='valid' else 'valid'])
            print('PASS '+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('summary rollback SHA mismatch')


def summary_queue_mutations(work):
    # Previous codec mutation restores source bytes, but its last binary is
    # still intentionally broken. Rebuild the independent control first.
    FULL['run']([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                 '-I',str(work),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/include'),
                 str(work/'session-summary-wire-test.c'),str(work/'session-summary-wire.c'),
                 str(ROOT/'core/src/session/program-status.c'),'-o',str(work/'process-summary-test')])
    specs=(
        ('old 64KiB queue cannot admit maximal pair',work/'session-summary-queue-test.c',
         'gt__attachment_open(pool,PSW_RESTORE_BYTES,4)','gt__attachment_open(pool,65536,4)','largest','codec'),
        ('summary pressure leaves attachment authorized',ROOT/'core/src/session/attachment.c',
         'if(a->stream.phase==GT_RESTORE_FAILED && a->active)',
         'if(false && a->stream.phase==GT_RESTORE_FAILED && a->active)','pressure','largest'),
        ('summary pressure retains queued payload',ROOT/'core/src/session/restore-stream.c',
         's->phase=GT_RESTORE_FAILED;s->failure=rc;clear(s);return rc;',
         's->phase=GT_RESTORE_FAILED;s->failure=rc;return rc;','pressure','largest'),
    )
    for label,path,old,new,target,control in specs:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError('nonunique '+label)
        changed=work/('mutation-'+path.name)
        try:
            changed.write_text(text.replace(old,new)+f'\n/* {MARK} */\n')
            queue_source=changed if path.name=='session-summary-queue-test.c' else work/'session-summary-queue-test.c'
            extra=[] if queue_source==changed else [str(changed)]
            FULL['run']([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                         '-I',str(work),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/session'),'-I',str(ROOT/'core/include'),
                         str(queue_source),str(work/'session-summary-wire.c'),str(work/'session-command-wire.c'),str(work/'session-process-wire.c'),
                         *extra,str(ROOT/'core/build/libghosterm_core.a'),'-pthread','-o',str(work/'process-summary-queue-test')])
            result=subprocess.run([str(work/'process-summary-queue-test'),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)
            if not result.returncode:raise RuntimeError(label+': falsely green')
            if control=='codec':FULL['run']([str(work/'process-summary-test'),'valid'])
            else:FULL['run']([str(work/'process-summary-queue-test'),control])
            print('PASS '+label+': target red, control green',flush=True)
        finally:
            changed.write_bytes(original)
            if hashlib.sha256(changed.read_bytes()).digest()!=digest or hashlib.sha256(path.read_bytes()).digest()!=digest:
                raise RuntimeError('summary queue rollback SHA mismatch')


def command_wire_mutations(work):
    path=work/'session-command-wire.c';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('command magic accepted','memcmp(p,"GCS1",4)','false','magic','valid'),
        ('unknown command flags accepted','(p[4]&~7u)','false','flags','valid'),
        ('command reserved byte accepted',' || p[7]','','reserved','valid'),
        ('command unknown exit becomes success','next.has_exit_code=(p[4]&2)!=0;',
         'next.has_exit_code=true;','unknown','valid'),
        ('command negative exit discarded',':-1-(int32_t)(UINT32_MAX-code)',':0','valid','unknown'),
        ('command embedded NUL accepted','if(memchr(p+CSW_HEADER,0,command+error))return false;',
         '/* embedded NUL accepted */','nul','valid'),
        ('command trailing bytes accepted',' || command+error!=n-CSW_HEADER','','trailing','valid'),
        ('command encode capacity ignored','CSW_HEADER+command+error>capacity','((void)capacity,false)','capacity','valid'),
    )
    for label,old,new,target,control in variants:
        if MARK in text or text.count(old)!=1:raise RuntimeError('nonunique '+label)
        try:
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n')
            FULL['run']([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                         '-I',str(work),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/include'),str(path),
                         str(work/'session-command-wire-test.c'),'-o',str(work/'process-command-test')])
            result=subprocess.run([str(work/'process-command-test'),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)
            if not result.returncode:raise RuntimeError(label+': falsely green')
            FULL['run']([str(work/'process-command-test'),control])
            print('PASS '+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('command codec rollback SHA mismatch')


def tui_mutations(engine,work):
    server=work/'session-process-owner.c';client=work/'session-process-client.inc'
    variants=(
        ('less never moves to last page while detached',server,'const char *keys=s->tui==1?"G":"iHELLO";','const char *keys=s->tui==1?"g":"iHELLO";','less','vim'),
        ('vim detached input never enters insert mode',server,'const char *keys=s->tui==1?"G":"iHELLO";','const char *keys=s->tui==1?"G":"HELLO";','vim','less'),
        ('restored less receives wrong navigation',client,'.text=c.tui==1?"g":"USER"','.text=c.tui==1?"G":"USER"','less','vim'),
        ('restored vim receives wrong text',client,'.text=c.tui==1?"g":"USER"','.text=c.tui==1?"g":"FAIL"','vim','less'),
        ('TUI view capture loses rendered content',client,'CGContextDrawImage(tui_context,CGRectMake(0,0,w,h),image);','(void)tui_context;','less','tmux'),
        ('TUI authority loses alternate screen',server,'if(event->kind==GT_STATE_OUTPUT)rc=p0_budget_feed(&s->source,s->terminal,event->payload,event->length);','if(event->kind==GT_STATE_OUTPUT){rc=p0_budget_feed(&s->source,s->terminal,event->payload,event->length);if(s->tui && rc==GHOSTTY_SUCCESS)rc=p0_budget_feed(&s->source,s->terminal,(const uint8_t *)"\x1b[?1049l",8);}','vim','tmux'),
        ('acknowledged TUI input never reaches application',server,'CHECK(process_submit(&s,GT_WORKER_INPUT,frame.bytes+8,frame.length-8,NULL,NULL));','if(!s.tui)CHECK(process_submit(&s,GT_WORKER_INPUT,frame.bytes+8,frame.length-8,NULL,NULL));','less','tmux'),
        ('unobserved vim command invented as success',client,'CHECK(csw_decode(command_wire,(size_t)command_length,&c->command));','CHECK(csw_decode(command_wire,(size_t)command_length,&c->command));if(c->tui==2)c->command.has_exit_code=true;','vim','less'),
    )
    originals={p:p.read_bytes() for p in (server,client)};digests={p:hashlib.sha256(v).digest() for p,v in originals.items()}
    if any(MARK.encode() in v for v in originals.values()):raise RuntimeError('TUI mutation marker before baseline')
    for label,path,old,new,target,control in variants:
        if originals[path].decode().count(old)!=1:raise RuntimeError(label+': nonunique TUI anchor')
    a,b=build(engine,work)
    for case in ('less','vim','roundtrip'):probe(a,b,case)
    try:
        for label,path,old,new,target,control in variants:
            try:
                path.write_text(originals[path].decode().replace(old,new)+'\n/* '+MARK+' */\n');a,b=build(engine,work)
                probe(a,b,target,1);probe(a,b,control)
                print('PASS real-TUI mutation/'+label+': target red, control green',flush=True)
            finally:
                path.write_bytes(originals[path])
                if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('TUI SHA rollback mismatch')
    finally:
        for path,value in originals.items():
            path.write_bytes(value)
            if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('TUI SHA rollback mismatch')
    a,b=build(engine,work)
    for case in ('less','vim','roundtrip'):probe(a,b,case)
    print('PASS 8 real-TUI mutations, SHA256 rollback, native less/vim/roundtrip baselines',flush=True)


def tmux_cleanup_case(cleanup,work,case):
    import socket as socket_module
    path=work/'tmux-cleanup-case.sock'
    if os.path.lexists(path):raise RuntimeError('cleanup test fixture already exists')
    first=socket_module.socket(socket_module.AF_UNIX,socket_module.SOCK_STREAM);second=None
    try:
        first.bind(str(path));info=path.lstat();identity=(info.st_dev,info.st_ino)
        if case=='file':
            path.unlink();path.write_bytes(b'keep');current=path.lstat();identity=(current.st_dev,current.st_ino)
        elif case=='replacement':
            path.unlink();second=socket_module.socket(socket_module.AF_UNIX,socket_module.SOCK_STREAM);second.bind(str(path))
            assert identity!=(path.lstat().st_dev,path.lstat().st_ino)
        elif case=='live':first.listen(1)
        else:assert case=='closed';first.close()
        if case=='closed':cleanup(path,identity);assert not os.path.lexists(path)
        else:
            rejected=False
            try:cleanup(path,identity)
            except (RuntimeError,OSError):rejected=True
            assert rejected and os.path.lexists(path),'unsafe endpoint removal'
            if case=='file':assert path.read_bytes()==b'keep'
    finally:
        first.close()
        if second is not None:second.close()
        path.unlink(missing_ok=True)


def tmux_mutations(engine,work):
    import inspect
    for case in ('closed','file','replacement','live'):tmux_cleanup_case(cleanup_tmux_socket,work,case)
    print('PASS 4 tmux cleanup baselines: owned stale, file, replaced inode, live endpoint',flush=True)
    function=inspect.getsource(cleanup_tmux_socket);module=work/'tmux-cleanup-mutant.py'
    original=('import os\n'+function).encode();module.write_bytes(original);digest=hashlib.sha256(original).digest()
    runner=work/'tmux-cleanup-runner.py'
    runner.write_text('import runpy,sys\nfrom pathlib import Path\ns=runpy.run_path('+repr(str(ROOT/'scripts/probe-session-process.py'))+')\nf=runpy.run_path(sys.argv[1])["cleanup_tmux_socket"]\ns["tmux_cleanup_case"](f,Path(sys.argv[1]).parent,sys.argv[2])\n')
    variants=(
        ('unchecked removal deletes ordinary file','if os.path.lexists(socket):','if os.path.lexists(socket):\n        socket.unlink();return','file'),
        ('endpoint identity fences missing','identity!=','False and identity!=','replacement'),
        ('live endpoint removed',"else:raise RuntimeError('tmux endpoint still connectable after reaping owned server')",'else:pass','live'),
    )
    try:
        for label,old,new,target in variants:
            text=original.decode();count=text.count(old)
            if MARK in text or count!=(2 if target=='replacement' else 1):raise RuntimeError('nonunique cleanup mutation '+label)
            module.write_text(text.replace(old,new)+'\n# '+MARK+'\n')
            rc=subprocess.run([sys.executable,str(runner),str(module),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10).returncode
            if rc!=1:raise RuntimeError(label+': expected explicit unsafe-removal failure, got '+str(rc))
            FULL['run']([sys.executable,str(runner),str(module),'closed'])
            print('PASS tmux cleanup mutation/'+label+': target red, control green',flush=True)
            module.write_bytes(original)
            if hashlib.sha256(module.read_bytes()).digest()!=digest:raise RuntimeError('cleanup SHA rollback mismatch')
    finally:
        module.write_bytes(original)
        if hashlib.sha256(module.read_bytes()).digest()!=digest:raise RuntimeError('cleanup SHA rollback mismatch')
    for case in ('closed','file','replacement','live'):tmux_cleanup_case(cleanup_tmux_socket,work,case)
    server=work/'session-process-owner.c';client=work/'session-process-client.inc'
    variants=(
        ('pane query expected wrong cursor',server,'    "answer=$(dd bs=1 count=7 2>/dev/null); [ \\"$answer\\" = \\"$(printf \'\\\\033[1;12R\')\\" ] || exit 51; "','    "answer=$(dd bs=1 count=7 2>/dev/null); [ \\"$answer\\" = \\"$(printf \'\\\\033[1;13R\')\\" ] || exit 51; "'),
        ('pane paste expectation corrupted',server,'    "[ \\"$answer\\" = \\"$(printf \'\\\\033[200~USER\\\\033[201~\')\\" ] || exit 52; wait_size \\"32 92\\"; "','    "[ \\"$answer\\" = \\"$(printf \'\\\\033[200~FAIL\\\\033[201~\')\\" ] || exit 52; wait_size \\"32 92\\"; "'),
        ('tmux backend resize silently skipped',server,'return b->real->ops->resize(b->real,c,r,x,y);','return b->owner->tmux?GT_OK:b->real->ops->resize(b->real,c,r,x,y);'),
        ('unknown command fabricated as successful zero',client,'CHECK(csw_decode(command_wire,(size_t)command_length,&c->command));','CHECK(csw_decode(command_wire,(size_t)command_length,&c->command));if(c->tmux)c->command.has_exit_code=true;'),
        ('authority loses alternate screen',server,'if(event->kind==GT_STATE_OUTPUT)rc=p0_budget_feed(&s->source,s->terminal,event->payload,event->length);','if(event->kind==GT_STATE_OUTPUT){rc=p0_budget_feed(&s->source,s->terminal,event->payload,event->length);if(s->tmux && rc==GHOSTTY_SUCCESS)rc=p0_budget_feed(&s->source,s->terminal,(const uint8_t *)"\\x1b[?1049l",8);}'),
        ('detached pane input never delivered',server,'  CHECK(process_submit(s,GT_WORKER_INPUT,"go\\n",3,NULL,NULL));\n  wait_output(s,"VERIFIED");','  if(!s->tmux)CHECK(process_submit(s,GT_WORKER_INPUT,"go\\n",3,NULL,NULL));\n  wait_output(s,"VERIFIED");'),
    )
    originals={p:p.read_bytes() for p in (server,client)};digests={p:hashlib.sha256(v).digest() for p,v in originals.items()}
    if any(MARK.encode() in v for v in originals.values()):raise RuntimeError('tmux marker before baseline')
    a,b=build(engine,work);probe(a,b,'tmux');probe(a,b,'roundtrip')
    try:
        for label,path,old,new in variants:
            text=originals[path].decode()
            if text.count(old)!=1:raise RuntimeError(label+': nonunique tmux anchor '+str(text.count(old)))
            try:
                path.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');a,b=build(engine,work)
                probe(a,b,'tmux',1);probe(a,b,'roundtrip')
                print('PASS process-tmux mutation/'+label+': target red, control green',flush=True)
            finally:
                path.write_bytes(originals[path])
                if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('tmux source SHA rollback mismatch')
    finally:
        for path,value in originals.items():
            path.write_bytes(value)
            if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('tmux source SHA rollback mismatch')
    a,b=build(engine,work);probe(a,b,'tmux');probe(a,b,'roundtrip')
    print('PASS 9 tmux mutations, four cleanup baselines, SHA256 rollback and native baselines',flush=True)


def scheme_pixel_mutations(engine,work):
    path=work/'session-process-client.inc';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    draw='CGContextDrawImage(scheme_context,CGRectMake(0,0,width,height),image);'
    variants=(
        ('frame capture loses actual content',draw,'(void)scheme_context;'),
        ('capture reuses initial bitmap',
         '[view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];\n    CGImageRef image=rep.CGImage;',
         '[view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];\n    static NSBitmapImageRep *frozen; if(!frozen)frozen=rep;rep=frozen;\n    CGImageRef image=rep.CGImage;'),
        ('light frame retains old color pixels',draw,
         draw+'if(!dark)for(size_t i=0;i<256;i++){rgba[4*i]=0x12;rgba[4*i+1]=0x34;rgba[4*i+2]=0x56;rgba[4*i+3]=255;}'),
    )
    if MARK in text:raise RuntimeError('pixel mutation marker before baseline')
    a,b=build(engine,work);probe(a,b,'scheme');probe(a,b,'roundtrip')
    try:
        for label,old,new in variants:
            if text.count(old)!=1:raise RuntimeError(label+': nonunique pixel anchor')
            try:
                path.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');a,b=build(engine,work)
                probe(a,b,'scheme',1);probe(a,b,'roundtrip')
                print('PASS scheme-pixel mutation/'+label+': target red, control green',flush=True)
            finally:
                path.write_bytes(original)
                if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('pixel SHA rollback mismatch')
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('pixel SHA rollback mismatch')
    renderer=engine/'src/renderer/generic.zig';original=renderer.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    if MARK in text:raise RuntimeError('renderer marker before mutation')
    anchor='                try self.terminal_state.beginUpdate(\n                    self.alloc,\n                    state.terminal,\n                );'
    if text.count(anchor)!=1:raise RuntimeError('nonunique render-state update anchor')
    stale=anchor+'\n                if (self.terminal_state.colors.background.eql(.{ .r = 0xab, .g = 0xcd, .b = 0xef })) self.terminal_state.colors.background = .{ .r = 0x12, .g = 0x34, .b = 0x56 };'
    try:
        renderer.write_text(text.replace(anchor,stale)+'\n// '+MARK+'\n')
        FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/scheme-pixel-renderer-mutation.log')
        a,b=build(engine,work);probe(a,b,'scheme',1);probe(a,b,'roundtrip')
        print('PASS scheme-pixel engine mutation/renderer keeps old background with healthy IO state: target red, control green',flush=True)
    finally:
        renderer.write_bytes(original)
        if hashlib.sha256(renderer.read_bytes()).digest()!=digest:raise RuntimeError('renderer SHA rollback mismatch')
    FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/scheme-pixel-renderer-rollback.log')
    a,b=build(engine,work);probe(a,b,'scheme');probe(a,b,'roundtrip')
    print('PASS 4 scheme-pixel mutations, SHA256 rollback, native scheme/roundtrip baselines',flush=True)


def scheme_mutations(engine,work):
    server=work/'session-process-owner.c';client=work/'session-process-client.inc';wire=work/'session-process-wire.c'
    variants=(
        ('external scheme omitted from frozen capture',server,'c->dark=s->dark;','c->dark=0;'),
        ('live scheme transmitted instead of frozen scheme',server,',scheme,checkpoint.dark,s.history_policy,checkpoint.history_state);',',scheme,s.dark,s.history_policy,checkpoint.history_state);'),
        ('checkpoint constructor scheme flag lost',client,'opts.p0_color_scheme_set=true;','opts.p0_color_scheme_set=false;'),
        ('snapshot scheme outside boolean domain',server,'pw_put64(metadata+216,dark);','pw_put64(metadata+216,dark+2);'),
        ('delta scheme outside boolean domain',server,'pw_put64(data,value.dark);','pw_put64(data,2);'),
        ('scheme delta mislabeled as resize',server,'pw_send(io,PW_SCHEME,e.sequence,data,sizeof(data))','pw_send(io,PW_RESIZE,e.sequence,data,sizeof(data))'),
        ('authority query ignores ordered scheme',server,'s->dark?GHOSTTY_COLOR_SCHEME_DARK:GHOSTTY_COLOR_SCHEME_LIGHT','GHOSTTY_COLOR_SCHEME_LIGHT'),
        ('authority default colors not applied',server,'->dark;rc=process_color_defaults(s);','->dark;rc=GHOSTTY_SUCCESS;'),
        ('GUI profile uses wrong background',client,'dark?"123456":"abcdef"','dark?"abcdef":"abcdef"'),
        ('GUI main does not service restore configuration',client,
         'while(!atomic_load(&c.live) && [end timeIntervalSinceNow]>0){process_scheme_main(&c);process_history_ui(&c);surface_spin(0.01);}',
         'while(!atomic_load(&c.live) && [end timeIntervalSinceNow]>0){surface_spin(0.01);}'),
        ('codec rejects supported scheme frame',wire,'kind>PW_HISTORY','kind>PW_INPUT_REJECTED'),
    )
    paths=(server,client,wire);originals={p:p.read_bytes() for p in paths};digests={p:hashlib.sha256(v).digest() for p,v in originals.items()}
    if any(MARK.encode() in v for v in originals.values()):raise RuntimeError('scheme mutation marker before baseline')
    a,b=build(engine,work);probe(a,b,'scheme');probe(a,b,'roundtrip')
    try:
        for label,path,old,new in variants:
            text=originals[path].decode()
            if text.count(old)!=1:raise RuntimeError(label+': nonunique scheme anchor')
            try:
                path.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');a,b=build(engine,work)
                probe(a,b,'scheme',1);probe(a,b,'roundtrip')
                print('PASS process-scheme mutation/'+label+': target red, control green',flush=True)
            finally:
                path.write_bytes(originals[path])
                if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('scheme rollback SHA mismatch')
    finally:
        for path,data in originals.items():
            path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('scheme rollback SHA mismatch')
    engine_paths=(engine/'src/Surface.zig',engine/'src/apprt/embedded.zig')
    engine_variants=(
        ('constructor IO scheme falls back to app state',engine_paths[0],
         'if (rt_surface.replicaColorScheme() != null) host_config.conditional_state = initial_conditional_state;',
         '// authority IO scheme omitted'),
        ('bound scheme digest not validated',engine_paths[1],
         'if (!std.mem.eql(u8, &digest, &opts.p0_checkpoint_digest)) return error.CheckpointBindingMismatch;',
         'if (!opts.p0_color_scheme_set and !std.mem.eql(u8, &digest, &opts.p0_checkpoint_digest)) return error.CheckpointBindingMismatch;'),
    )
    originals={p:p.read_bytes() for p in engine_paths};digests={p:hashlib.sha256(v).digest() for p,v in originals.items()}
    if any(MARK.encode() in v for v in originals.values()):raise RuntimeError('scheme engine marker before baseline')
    try:
        for label,path,old,new in engine_variants:
            text=originals[path].decode()
            if text.count(old)!=1:raise RuntimeError(label+': nonunique scheme engine anchor')
            try:
                path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
                FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/process-scheme-engine-mutation.log')
                a,b=build(engine,work);probe(a,b,'scheme',1);probe(a,b,'roundtrip')
                print('PASS process-scheme engine mutation/'+label+': target red, control green',flush=True)
            finally:
                path.write_bytes(originals[path])
                if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('scheme engine SHA mismatch')
    finally:
        for path,data in originals.items():
            path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('scheme engine SHA mismatch')
    FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/process-scheme-rollback-build.log')
    a,b=build(engine,work);probe(a,b,'scheme');probe(a,b,'roundtrip')
    print('PASS 13 process-scheme mutations, SHA256 rollback, native scheme/roundtrip baselines',flush=True)


def scheme_owner_mutations(engine,work):
    path=work/'session-scheme-owner.c';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    a,b=build(engine,work);FULL['run']([str(work/'process-scheme-owner')])
    variants=(
        ('real query ignores updated configuration','f->dark?GHOSTTY_COLOR_SCHEME_DARK:GHOSTTY_COLOR_SCHEME_LIGHT','GHOSTTY_COLOR_SCHEME_LIGHT'),
        ('color capability not declared','.color_scheme_supported=true','.color_scheme_supported=false'),
        ('real color callback missing','(const void *)color','(false?(const void *)color:NULL)'),
        ('external configuration missing from capture','*copy=f->dark;','*copy=0;'),
    )
    try:
        for label,old,new in variants:
            if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique scheme owner anchor')
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            rc=subprocess.run([str(work/'process-scheme-owner')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=12).returncode
            if rc!=1:raise RuntimeError(label+': expected explicit failure, got '+str(rc))
            probe(a,b,'roundtrip')
            print('PASS scheme-owner mutation/'+label+': target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('scheme owner rollback SHA mismatch')
    a,b=build(engine,work);FULL['run']([str(work/'process-scheme-owner')]);probe(a,b,'roundtrip')
    print('PASS 4 scheme-owner mutations, SHA256 rollback and native baselines green',flush=True)


def thread_args_tests(engine,work,case=None,expected=0):
    zig=os.environ.get('ZIG') or shutil.which('zig') or str(ROOT/'tmp/zig/zig-aarch64-macos-0.16.0/zig')
    binary=work/'thread-args-unit-test'
    command=[zig,'test',str(engine/'src/apprt/p0_thread.zig'),'-lc','--test-no-exec','-femit-bin='+str(binary),
             '--cache-dir',str(ROOT/'tmp/thread-args-unit-cache'),'--global-cache-dir',str(ROOT/'tmp/zig-global')]
    if case:command.extend(('--test-filter',case))
    FULL['run'](command)
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
    sys.stdout.write(result.stdout);sys.stderr.write(result.stderr)
    if (expected=='nonzero' and result.returncode==0) or (expected!='nonzero' and result.returncode!=expected):raise RuntimeError(f'thread args unit {case}: expected {expected}, got {result.returncode}')
    return result


def warmup_tests(engine,work,case=None,expected=0):
    zig=os.environ.get('ZIG') or shutil.which('zig') or str(ROOT/'tmp/zig/zig-aarch64-macos-0.16.0/zig')
    binary=work/'warmup-unit-test'
    command=[zig,'test',str(engine/'src/apprt/p0_warmup.zig'),'-lc','--test-no-exec','-femit-bin='+str(binary),
             '--cache-dir',str(ROOT/'tmp/warmup-unit-cache'),'--global-cache-dir',str(ROOT/'tmp/zig-global')]
    if case:command.extend(('--test-filter',case))
    FULL['run'](command)
    result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
    sys.stdout.write(result.stdout);sys.stderr.write(result.stderr)
    if result.returncode!=expected:raise RuntimeError(f'warmup unit {case}: expected {expected}, got {result.returncode}')


def warmup_mutations(engine,work):
    path=engine/'src/apprt/p0_warmup.zig'
    unit=(
        ('work omitted','                    function();','                    if (false) function();','warmups run','second warmup'),
        ('failed second startup not joined','        errdefer self.join();','        // no error join','second warmup','warmups run'),
        ('join skipped','                value.join();','                _ = value;','join waits','warmups run'),
    )
    for label,old,new,target,control in unit:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique warmup unit anchor')
        try:
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');warmup_tests(engine,work,target,1)
            if label=='join skipped':thread_args_tests(engine,work,'successful entry')
            else:warmup_tests(engine,work,control)
            print('PASS warmup mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('warmup unit SHA rollback mismatch')
    specs=(
        ('standard warmup bypasses allocator',engine/'src/apprt/p0_warmup.zig','try thread.spawn(allocator, .{}, Job.run, .{ context, observer })','blk: { _ = allocator; break :blk .{ .standard = try std.Thread.spawn(.{}, Job.run, .{ context, observer }) }; }'),
        ('provided warmup allocator ignored',engine/'src/apprt/embedded.zig','const thread_allocator = if (opts.p0_thread_allocator) |a| a.zig() else alloc;','const thread_allocator = alloc;'),
        ('actual warmup stack exceeds capacity',engine/'src/apprt/p0_warmup.zig','thread.spawn(allocator, .{}, Job.run','thread.spawn(allocator, .{ .stack_size = 2 * std.Thread.SpawnConfig.default_stack_size }, Job.run'),
        ('thread options accepted without App budget',engine/'src/apprt/embedded.zig','if (opts.p0_allocator == null and (opts.p0_thread_allocator != null or opts.p0_warmup_observer != null)) return error.MissingAppBudget;','// missing prerequisite ignored'),
        ('work completion not reported',engine/'src/apprt/p0_warmup.zig','if (observe) |callback| callback(c, i, 1);','// completion missing'),
        ('App destroy omits owned joins',engine/'src/App.zig','    self.p0_warmups.join();','    // App warmups not joined'),
        ('warmup capacity halved',work/'session-process-client.inc','c.warmup_reserve=2*(c.engine_stack+2*(size_t)page);','c.warmup_reserve=c.engine_stack+2*(size_t)page;'),
    )
    for label,path,old,new in specs:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique warmup engine anchor')
        try:
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            # Rebuild even for a C-only mutation: the previous engine binary
            # may still contain its reverted mutation after source rollback.
            FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/warmup-engine-mutation.log')
            a,b=build(engine,work);probe(a,b,'roundtrip',1);legacy(b)
            print('PASS warmup mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('warmup engine SHA rollback mismatch')
    FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/warmup-engine-rollback.log')
    a,b=build(engine,work);warmup_tests(engine,work);thread_args_tests(engine,work);wire_cases(work)
    for case in ('roundtrip','start-error','search'):probe(a,b,case)
    print('PASS 10 warmup mutations, actual work/stacks, transactional startup, joins and SHA256 rollback',flush=True)


def app_budget_mutations(engine,work):
    specs=(
        ('runtime allocator absent','gui-budget.c','return r?p0_budget_allocator(&r->scope,P0_BUDGET_GENERAL):NULL;','(void)r;return NULL;','runtime-allocator'),
        ('runtime idle ignores live allocations','gui-budget.c','return !u.live && u.mapped==u.unmapped;','(void)u;return true;','runtime-allocator'),
        ('runtime fault hidden','gui-budget.c','return r && p0_budget_result(&r->scope,GHOSTTY_SUCCESS)==GHOSTTY_SUCCESS;','return r!=NULL;','runtime-allocator'),
        ('busy runtime release returns generic error','gui-budget.c','if(!p0_gui_reserved_idle(r)){pthread_mutex_unlock(&p->admission);return P0_GUI_BUSY;}','if(!p0_gui_reserved_idle(r)){pthread_mutex_unlock(&p->admission);return P0_GUI_ERROR;}','runtime-allocator'),
        ('runtime scope not closed','gui-budget.c','if(!p0_budget_scope_close(&r->scope)){','if(false){','runtime-allocator'),
        ('app capacity omitted from total','session-process-client.inc','+c.metadata_limit+c.app_reserve+c.warmup_reserve+2*c.engine_thread_reserve+2*c.config_reserve+c.global_reserve+c.global_io_reserve,64*MIB','+c.metadata_limit+c.warmup_reserve+2*c.engine_thread_reserve+2*c.config_reserve+c.global_reserve+c.global_io_reserve,64*MIB','native'),
    )
    for label,name,old,new,target in specs:
        path=work/name;original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique app budget anchor')
        try:
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            if target=='native':probe(a,b,'roundtrip',1);legacy(b)
            else:
                rc=subprocess.run([str(work/'process-gui-budget-test'),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10).returncode
                if not rc:raise RuntimeError(label+': falsely green')
                FULL['run']([str(work/'process-gui-budget-test'),'valid'])
            print('PASS app budget mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('app budget SHA rollback mismatch')
    path=engine/'src/apprt/embedded.zig';text=path.read_text()
    start=text.index('    fn app_new_(');end=text.index('    /// Update the focused state',start)
    segment=text[start:end]
    variants=(
        ('CoreApp bypasses host allocator','try CoreApp.createUnwarmed(alloc) else try CoreApp.create(alloc);','try CoreApp.createUnwarmed(global.alloc()) else try CoreApp.create(alloc);'),
        ('runtime App bypasses host allocator',segment,segment.replace('alloc.create(App)','global.alloc().create(App)').replace('errdefer alloc.destroy(app);','errdefer global.alloc().destroy(app);').replace('core_app.alloc.destroy(v);','global.alloc().destroy(v);')),
        ('configuration clone bypasses host allocator','var config_clone = try config.clone(alloc);','var config_clone = try config.clone(blk: { _ = alloc; break :blk global.alloc(); });'),
        ('runtime App allocation leaked','        core_app.alloc.destroy(v);\n        core_app.destroy();','        // runtime App object leaked\n        core_app.destroy();'),
    )
    for label,old,new in variants:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique app allocator anchor')
        try:
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/app-engine-mutation.log')
            a,b=build(engine,work);probe(a,b,'roundtrip',1);legacy(b)
            print('PASS app budget mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('app engine SHA rollback mismatch')
    FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/app-engine-rollback.log')
    a,b=build(engine,work);thread_args_tests(engine,work);wire_cases(work)
    for case in ('roundtrip','start-error','search'):probe(a,b,case)
    print('PASS 10 app-budget mutations, failed creates/shared pool/rollback and native baselines',flush=True)


def search_mutations(engine,work):
    path=work/'session-process-client.inc';original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
    old='c.engine_thread_reserve=3*(c.engine_stack+2*(size_t)page);'
    if MARK in text or text.count(old)!=1:raise RuntimeError('nonunique third-slot capacity anchor')
    try:
        path.write_text(text.replace(old,'c.engine_thread_reserve=2*(c.engine_stack+2*(size_t)page);')+f'\n/* {MARK} */\n')
        a,b=build(engine,work);probe(a,b,'search',1);legacy(b)
        print('PASS search mutation/third thread capacity not promised: target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('search client SHA rollback mismatch')
    variants=(
        ('search bypasses thread allocator',engine/'src/Surface.zig',
         'try @import("apprt/p0_thread.zig").spawn(thread_allocator, .{}, terminal.search.Thread.threadMain, .{&s.state})',
         'blk: { _ = thread_allocator; break :blk .{ .standard = try std.Thread.spawn(.{}, terminal.search.Thread.threadMain, .{&s.state}) }; }'),
        ('failed search leaves stale slot',engine/'src/Surface.zig',
         's.state.deinit();\n                    self.search = null;',
         's.state.deinit();\n                    // slot not cleared'),
        ('failed search state leaks',engine/'src/Surface.zig',
         's.state.deinit();\n                    self.search = null;',
         '// state leaked\n                    self.search = null;'),
        ('search needle ignored',engine/'src/Surface.zig',
         '.{ .change_needle = try .init(\n                    self.alloc,\n                    text,',
         '.{ .change_needle = try .init(\n                    self.alloc,\n                    "__ghosterm_missing_probe__",'),
        ('end search abandons live thread',engine/'src/Surface.zig',
         'if (self.search) |*s| {\n                s.deinit();\n                self.search = null;',
         'if (self.search) |*s| {\n                _ = s;\n                self.search = null;'),
        ('search role returns IO handle',engine/'src/apprt/embedded.zig',
         '3 => if (s.core_surface.search) |*search| search.thread else return false,',
         '3 => if (s.core_surface.search != null) s.core_surface.io_thr else return false,'),
    )
    for label,path,old,new in variants:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique search source anchor')
        try:
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/search-engine-mutation.log')
            a,b=build(engine,work);probe(a,b,'search',1);probe(a,b,'roundtrip')
            print('PASS search mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('search engine SHA rollback mismatch')
    FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/search-engine-rollback.log')
    a,b=build(engine,work);thread_args_tests(engine,work);wire_cases(work);probe(a,b,'search');probe(a,b,'roundtrip')
    print('PASS 7 search mutations, actual matches/threads, SHA256 rollback and native baselines',flush=True)


def signal_stack_mutations(engine,work):
    path=engine/'src/apprt/p0_thread.zig'
    specs=(
        ('configured size silently halved','pub const signal_stack_size: usize = @intCast(std.options.signal_stack_size orelse 0);','pub const signal_stack_size: usize = @intCast((std.options.signal_stack_size orelse 0) / 2);','signal stack setup',1),
        ('owned stack replaced by TLS','signal.install(&frame.signal_stack) catch unreachable;','std.Thread.maybeAttachSignalStack();','signal stack setup',1),
        ('registered stack freed before restore','defer signal.restore() catch unreachable;','// restore omitted','signal stack setup',1),
        ('active predecessor disabled','if (!self.active) return;','if (!self.active) return;self.previous.flags |= std.c.SS.DISABLE;','active predecessor',1),
        ('Darwin disabled size not normalized','if ((self.previous.flags & std.c.SS.DISABLE) != 0) self.previous.size = @max(self.previous.size, std.c.MINSIGSTKSZ);','// normalization omitted','signal stack setup','nonzero'),
    )
    for label,old,new,target,expected in specs:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique signal stack anchor')
        try:
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            result=thread_args_tests(engine,work,target,expected)
            if expected=='nonzero' and 'SizeTooSmall' not in result.stderr:raise RuntimeError('normalization failed for unrelated reason')
            thread_args_tests(engine,work,'allocation denial')
            print('PASS signal stack mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('signal stack unit SHA rollback mismatch')
    original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
    anchor='signal_stack: [signal_stack_size]u8 align(16)'
    if MARK in text or text.count(anchor)!=1:raise RuntimeError('nonunique signal buffer anchor')
    try:
        path.write_text(text.replace(anchor,'signal_stack: [signal_stack_size / 2]u8 align(16)')+'\n// '+MARK+'\n')
        FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/signal-stack-engine-mutation.log')
        a,b=build(engine,work);probe(a,b,'roundtrip',1);legacy(b)
        print('PASS signal stack mutation/native buffer smaller than configured size: target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('signal stack engine SHA rollback mismatch')
    FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/signal-stack-engine-rollback.log')
    a,b=build(engine,work);thread_args_tests(engine,work);wire_cases(work);probe(a,b,'roundtrip');probe(a,b,'start-error')
    print('PASS 6 signal-stack mutations, kernel registration, allocation charge, SHA256 rollback and native baselines',flush=True)


def thread_args_mutations(engine,work):
    path=engine/'src/apprt/p0_thread.zig'
    specs=(
        ('entry frame never released','defer owner.destroy(frame);','_ = owner;','successful entry','allocation denial'),
        ('startup error leaks allocated frame','errdefer owner_allocator.destroy(frame);','// rollback omitted','startup error','successful entry'),
        ('signal stack setup omitted','signal.install(&frame.signal_stack) catch unreachable;','{}','signal stack setup','allocation denial'),
    )
    for label,old,new,target,control in specs:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique thread args unit anchor')
        try:
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            thread_args_tests(engine,work,target,1);thread_args_tests(engine,work,control)
            print('PASS thread args mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('thread args unit SHA rollback mismatch')
    variants=(
        ('entry frames bypass host allocator',engine/'src/apprt/p0_thread.zig','const owner_allocator = allocator;','const owner_allocator = blk: { _ = allocator; break :blk std.heap.c_allocator; };'),
        ('provided thread allocator ignored',engine/'src/apprt/embedded.zig','return if (self.p0_thread_allocator) |p| p.zig() else null;','_ = self;return null;'),
        ('default thread allocator uses page domain',engine/'src/Surface.zig','rt_surface.replicaThreadAllocator() orelse alloc','rt_surface.replicaThreadAllocator() orelse (rt_surface.replicaPageAllocator() orelse alloc)'),
        ('thread-only allocator accepted without budget',engine/'src/apprt/embedded.zig',' or opts.p0_thread_allocator != null;',';'),
        ('allocation failure falls back to libc',engine/'src/apprt/p0_thread.zig','const owner_allocator = allocator;\n    const frame = try owner_allocator.create(Frame);','var owner_allocator = allocator;\n    const frame = owner_allocator.create(Frame) catch blk: { owner_allocator = std.heap.c_allocator; break :blk try owner_allocator.create(Frame); };'),
    )
    for label,path,old,new in variants:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique thread args engine anchor')
        try:
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/thread-args-engine-mutation.log')
            a,b=build(engine,work);probe(a,b,'roundtrip',1);legacy(b)
            print('PASS thread args mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('thread args engine SHA rollback mismatch')
    FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/thread-args-engine-rollback.log')
    a,b=build(engine,work);thread_args_tests(engine,work);wire_cases(work);probe(a,b,'roundtrip');probe(a,b,'start-error')
    print('PASS 8 thread-args mutations, unit/native controls, SHA256 rollback and native baselines',flush=True)


def engine_thread_mutations(engine,work):
    specs=(
        ('thread capacity not reserved','gui-budget.c','p0_budget_scope_admit(&b->threads,&p->budget,p->limits.threads)','(p0_budget_scope_init(&b->threads,&p->budget,p->limits.threads),true)','threads'),
        ('failed thread admission keeps data promises','gui-budget.c','for(unsigned j=0;j<3;j++)(void)p0_budget_scope_close(scopes[j]);','/* data promises retained */','threads'),
        ('failed thread scope left open','gui-budget.c','(void)p0_budget_scope_close(&b->threads);','/* thread scope retained */','threads'),
        ('released thread promise retained','gui-budget.c','if(p->limits.threads && !p0_budget_scope_close(&b->threads))','if(false)','threads'),
        ('thread capacity overflow accepted','gui-budget.c','limits.general+limits.pages+limits.wire>SIZE_MAX-limits.threads','false','threads'),
        ('native thread guards not charged','session-process-client.inc','c.engine_thread_reserve=3*(c.engine_stack+2*(size_t)page);','c.engine_thread_reserve=3*c.engine_stack;','native'),
        ('only one engine thread charged','session-process-client.inc','c.engine_thread_reserve=3*(c.engine_stack+2*(size_t)page);','c.engine_thread_reserve=c.engine_stack+2*(size_t)page;','native'),
        ('pool omits engine thread capacity','session-process-client.inc','+2*c.engine_thread_reserve+2*c.config_reserve+c.global_reserve+c.global_io_reserve,64*MIB','+2*c.config_reserve+c.global_reserve+c.global_io_reserve,64*MIB','native'),
        ('native capacity observer reports zero','gui-budget.c','return b && b->pool->limits.threads?p0_budget_scope_read(&b->threads).committed:0;','(void)b;return 0;','native'),
    )
    for label,name,old,new,target in specs:
        path=work/name;original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique engine thread anchor')
        try:
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            if target=='native':probe(a,b,'roundtrip',1);legacy(b)
            else:
                rc=subprocess.run([str(work/'process-gui-budget-test'),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10).returncode
                if not rc:raise RuntimeError(label+': falsely green')
                FULL['run']([str(work/'process-gui-budget-test'),'valid'])
            print('PASS engine thread mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('engine thread budget SHA rollback mismatch')
    variants=(
        ('renderer returned for IO thread',engine/'src/apprt/embedded.zig','2 => s.core_surface.io_thr,','2 => s.core_surface.renderer_thr,'),
        ('real renderer stack exceeds promise',engine/'src/Surface.zig','spawn(thread_allocator, .{}, rendererpkg.Thread.threadMain','spawn(thread_allocator, .{ .stack_size = 2 * std.Thread.SpawnConfig.default_stack_size }, rendererpkg.Thread.threadMain'),
    )
    for label,path,old,new in variants:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique engine thread source')
        try:
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/engine-thread-engine-mutation.log')
            a,b=build(engine,work);probe(a,b,'roundtrip',1);legacy(b)
            print('PASS engine thread mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('engine thread engine SHA rollback mismatch')
    FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/engine-thread-engine-rollback.log')
    a,b=build(engine,work);wire_cases(work);probe(a,b,'roundtrip');probe(a,b,'start-error')
    print('PASS 11 engine-thread mutations, actual pthread observations, SHA256 rollback and native baselines',flush=True)


def main_stack_mutations(engine,work):
    specs=(
        ('owner reservation omitted','session-process-owner.c','CHECK(p0_budget_scope_admit(&s.main_stack,&s.budget,s.main_reserve));','p0_budget_scope_init(&s.main_stack,&s.budget,s.main_reserve);','native',2),
        ('owner release omitted','session-process-owner.c','CHECK(p0_budget_scope_close(&s.main_stack));','/* main release omitted */','native',2),
        ('owner guard not charged','session-process-owner.c','s.main_reserve=main_stack.capacity;','s.main_reserve=main_stack.stack;','native',2),
        ('owner global capacity omits main stack','session-process-owner.c','256*MIB+s.main_reserve','256*MIB','native',1),
        ('GUI reservation omitted','session-process-client.inc','CHECK(p0_gui_reserve(c.pool,c.main_reserve,&c.main_reservation)==P0_GUI_OK);','/* main GUI reservation omitted */','native',1),
        ('GUI release omitted','session-process-client.inc','CHECK(p0_gui_unreserve(c->main_reservation)==P0_GUI_OK);','/* main GUI release omitted */','native',1),
        ('GUI guard not charged','session-process-client.inc','c.main_reserve=main_stack.capacity;','c.main_reserve=main_stack.stack;','native',1),
        ('GUI pool capacity omits main stack','session-process-client.inc','const p0_gui_limits limits={320*MIB+c.main_reserve+','const p0_gui_limits limits={320*MIB+','native',1),
        ('measurement drops guard capacity','session-main-stack.h','stack+(size_t)guard};return true;','stack};return true;','measure',1),
        ('admission ignores committed bytes','session-main-stack.h','committed==actual.capacity','((void)committed,true)','admission',1),
        ('worker thread mistaken for main','session-main-stack.h','if(!pthread_main_np())return false;','/* main thread restriction omitted */','thread',1),
    )
    for label,name,old,new,target,count in specs:
        path=work/name;original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=count:raise RuntimeError(label+': invalid main stack anchor')
        try:
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            if target=='native':probe(a,b,'roundtrip',1);legacy(b)
            else:
                rc=subprocess.run([str(work/'process-main-stack-test'),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10).returncode
                if not rc:raise RuntimeError(label+': falsely green')
                FULL['run']([str(work/'process-main-stack-test'),'measure' if target!='measure' else 'thread'])
            print('PASS main stack mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('main stack SHA rollback mismatch')
    a,b=build(engine,work);FULL['run']([str(work/'process-main-stack-test'),'all'])
    for case in ('roundtrip','start-error','state-oom','backend-oom'):probe(a,b,case)
    print('PASS 11 main-stack mutations, SHA256 rollback, measurement/thread and four native baselines green',flush=True)


def native_ime_mutations(engine,work):
    path=work/'session-appkit-host.inc';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('interpretKeyEvents omitted','[self interpretKeyEvents:@[event]];','/* interpreter omitted */'),
        ('native preedit omitted','if(!_interpreting)[self syncPreedit];','/* preedit omitted */'),
        ('native composition leaks bytes','self.marked=text;','self.marked=text;ghostty_surface_text(_terminal,text.UTF8String,strlen(text.UTF8String));'),
        ('native cancellation keeps marked text','self.marked=@"";','/* marked text retained */'),
        ('native commit uses paste','ghostty_input_key_s key={.action=GHOSTTY_ACTION_PRESS,.mods=GHOSTTY_MODS_NONE,.text=text.UTF8String};\n    ghostty_surface_key(_terminal,key);','ghostty_surface_text(_terminal,text.UTF8String,strlen(text.UTF8String));'),
        ('native commit fails to unmark','[self unmarkText];_handled=YES;','_handled=YES;'),
        ('candidate screen y conversion omitted','NSHeight(self.bounds)-y','y'),
        ('candidate anchor one cell too low','NSHeight(self.bounds)-y','NSHeight(self.bounds)-y-h'),
        ('readonly keyDown admitted','- (void)keyDown:(NSEvent *)event {\n    if(!_inputEnabled || !_terminal)','- (void)keyDown:(NSEvent *)event {\n    if(!_terminal)'),
        ('readonly setMarkedText admitted','- (void)setMarkedText:(id)value selectedRange:(NSRange)selection replacementRange:(NSRange)replacement {\n    (void)replacement;if(!_inputEnabled || !_terminal)','- (void)setMarkedText:(id)value selectedRange:(NSRange)selection replacementRange:(NSRange)replacement {\n    (void)replacement;if(!_terminal)'),
        ('readonly insertText admitted','- (void)insertText:(id)value replacementRange:(NSRange)replacement {\n    (void)replacement;if(!_inputEnabled || !_terminal)','- (void)insertText:(id)value replacementRange:(NSRange)replacement {\n    (void)replacement;if(!_terminal)'),
    )
    try:
        if MARK in text:raise RuntimeError('native IME mutation marker present')
        for label,old,new in variants:
            if text.count(old)!=1:raise RuntimeError(label+': nonunique native IME anchor')
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            probe(a,b,'native-ime',1);probe(a,b,'ime')
            print('PASS native IME mutation/'+label+': target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('native IME rollback SHA mismatch')
    a,b=build(engine,work);probe(a,b,'native-ime');probe(a,b,'ime')
    print('PASS 11 native IME mutations, SHA256 rollback and native baselines green',flush=True)


def native_pointer_mutations(engine,work):
    path=work/'session-appkit-host.inc';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('readonly mouse admitted','if(!_inputEnabled || !_terminal){_blockedEvents++;return;}\n    [self.window makeFirstResponder:self];','if(!_terminal){_blockedEvents++;return;}\n    [self.window makeFirstResponder:self];'),
        ('mouse y conversion omitted','NSHeight(self.bounds)-point.y','point.y'),
        ('native release duplicated','GHOSTTY_MOUSE_RELEASE','GHOSTTY_MOUSE_PRESS'),
        ('native press omitted','ghostty_surface_mouse_button(_terminal,GHOSTTY_MOUSE_PRESS,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);','/* press omitted */'),
        ('click sends responder to window','[self.window makeFirstResponder:self];','[self.window makeFirstResponder:self.window];'),
    )
    try:
        if MARK in text:raise RuntimeError('native pointer mutation marker present')
        for label,old,new in variants:
            expected=1
            if text.count(old)!=expected:raise RuntimeError(label+': nonunique native pointer anchor')
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            probe(a,b,'native-pointer',1);probe(a,b,'pointer')
            print('PASS native pointer mutation/'+label+': target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('native pointer rollback SHA mismatch')
    a,b=build(engine,work);probe(a,b,'native-pointer');probe(a,b,'pointer')
    print('PASS 5 native pointer mutations, SHA256 rollback and native baselines green',flush=True)


def pointer_mutations(engine,work):
    path=work/'session-process-client.inc';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('mouse press omitted','ghostty_surface_mouse_button(c->surface,GHOSTTY_MOUSE_PRESS,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);','/* press omitted */'),
        ('mouse release duplicated','ghostty_surface_mouse_button(c->surface,GHOSTTY_MOUSE_RELEASE,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);','ghostty_surface_mouse_button(c->surface,GHOSTTY_MOUSE_PRESS,GHOSTTY_MOUSE_LEFT,GHOSTTY_MODS_NONE);'),
        ('focus gain omitted','ghostty_surface_set_focus(c->surface,true);process_pointer_expect(c,reports,24);','process_pointer_expect(c,reports,24);'),
        ('repeated focus accidentally toggles','ghostty_surface_set_focus(c->surface,false);process_pointer_expect(c,reports,21);\n    ghostty_surface_set_focus(c->surface,false);','ghostty_surface_set_focus(c->surface,false);process_pointer_expect(c,reports,21);\n    ghostty_surface_set_focus(c->surface,true);'),
    )
    try:
        for label,old,new in variants:
            if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique pointer anchor')
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            probe(a,b,'pointer',1);probe(a,b,'roundtrip')
            print('PASS process mutation/'+label+': target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('pointer rollback SHA mismatch')
    a,b=build(engine,work);probe(a,b,'pointer');probe(a,b,'roundtrip')
    print('PASS 4 pointer mutations, SHA256 rollback and native baselines green',flush=True)


def backend_oom_mutations(engine,work):
    path=work/'session-process-owner.c';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('known zero routed as unknown',
         'if(backend_oom)CHECK(gt__attachment_end(s.attachment,s.generation,0)==GT_RESTORE_WAIT);',
         'if(backend_oom)CHECK(gt__attachment_end_unknown(s.attachment,s.generation)==GT_RESTORE_OK);'),
        ('initialized startup state not disposed',
         'if(!s->state_ready || !s->backend_started)', 'if(!s->state_ready)'),
        ('PTY rejection replaced by fake failure',
         'gt_io_backend_t *backend=gt__pty_open_with_options(&startup_options,&startup_allocator,&s->thread_options);',
         'gt_io_backend_t *backend=((void)startup_options,(void)startup_allocator,NULL);'),
        ('known zero wire flag lost',
         'CHECK(pw_send(&io,PW_RESULT,0,bytes,length));',
         'if(backend_oom)bytes[7]=0;CHECK(pw_send(&io,PW_RESULT,0,bytes,length));'),
    )
    try:
        for label,old,new in variants:
            if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique backend OOM anchor')
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            probe(a,b,'backend-oom',1);probe(a,b,'state-oom')
            print('PASS process mutation/'+label+': target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('backend OOM rollback SHA mismatch')
    a,b=build(engine,work);probe(a,b,'backend-oom');probe(a,b,'state-oom');probe(a,b,'roundtrip')
    print('PASS 4 backend OOM mutations, SHA256 rollback and native baselines green',flush=True)


def state_oom_mutations(engine,work):
    path=work/'session-process-owner.c';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('diagnostic released to wrong domain','scope_release((s->state_oom||s->backend_oom)?&s->diagnostic:&s->source,p,n)','scope_release(&s->source,p,n)'),
        ('partial summary leaked','gt__program_status_close(s->summary);s->summary=NULL;return;','s->summary=NULL;return;'),
        ('diagnostic allocates from faulted source','scope_allocate((s->state_oom||s->backend_oom)?&s->diagnostic:&s->source,n)','scope_allocate(&s->source,n)'),
        ('partial factory context discarded','  *out=s;\n  CHECK(p0_budget_scope_read(&s->source).live>before);','  (void)out;\n  CHECK(p0_budget_scope_read(&s->source).live>before);'),
        ('real VT failure replaced by fake factory failure','return open_state(context,sink,state);','*state=NULL;return GT_ERR_IO;'),
    )
    try:
        for label,old,new in variants:
            if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique state OOM anchor')
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            probe(a,b,'state-oom',1);probe(a,b,'start-error')
            print('PASS process mutation/'+label+': target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('state OOM rollback SHA mismatch')
    a,b=build(engine,work);probe(a,b,'state-oom');probe(a,b,'start-error');probe(a,b,'roundtrip')
    print('PASS 5 state OOM mutations, SHA256 rollback and native baselines green',flush=True)


def combined_mutations(engine,work):
    path=work/'gui-budget.c';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('mixed admission drops metadata headroom',
         'limits.metadata && !p0_budget_scope_admit(&p->metadata_capacity,&p->budget,limits.metadata)', 'false','combined-race'),
        ('mixed runtime admission unreserved',
         'p0_budget_scope_admit(&r->scope,&p->budget,bytes)',
         '(p0_budget_scope_init(&r->scope,&p->budget,bytes),true)','combined-race'),
        ('mixed surface admission unreserved',
         'p0_budget_scope_admit(scopes[i],&p->budget,limits[i])',
         '(p0_budget_scope_init(scopes[i],&p->budget,limits[i]),true)','combined-race'),
        ('mixed runtime rejection leaks descriptor',
         '(void)p0_budget_scope_close(&r->scope);metadata_free(p,r,sizeof(*r));',
         '(void)p0_budget_scope_close(&r->scope);','combined-race'),
        ('mixed surface rejection leaks descriptor',
         'for(unsigned j=0;j<=i;j++)(void)p0_budget_scope_close(scopes[j]);\n            metadata_free(p,b,sizeof(*b));pthread_mutex_unlock(&p->admission);return P0_GUI_LIMIT;',
         'for(unsigned j=0;j<=i;j++)(void)p0_budget_scope_close(scopes[j]);\n            pthread_mutex_unlock(&p->admission);return P0_GUI_LIMIT;','combined-race'),
        ('metadata headroom not reserved','limits.metadata && !p0_budget_scope_admit(&p->metadata_capacity,&p->budget,limits.metadata)', 'false','combined'),
        ('metadata bound accepts uncapped libc','(limits.metadata && !allocator) || ','','combined'),
        ('metadata promise leaked','p->limits.metadata && !p0_budget_scope_close(&p->metadata_capacity)', 'false','combined'),
        ('failed metadata admit falsely succeeds','pthread_mutex_destroy(&p->admission);a.release(a.context,p,sizeof(*p));return P0_GUI_LIMIT;','pthread_mutex_destroy(&p->admission);a.release(a.context,p,sizeof(*p));return P0_GUI_OK;','combined'),
    )
    try:
        for label,old,new,target in variants:
            if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique combined anchor')
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            rc=subprocess.run([str(work/'process-gui-budget-test'),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10).returncode
            if not rc:raise RuntimeError(label+': falsely green')
            FULL['run']([str(work/'process-gui-budget-test'),'invalid' if label=='mixed surface admission unreserved' else 'valid'])
            print('PASS combined mutation/'+label+': target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('combined rollback SHA mismatch')
    a,b=build(engine,work);wire_cases(work);probe(a,b,'start-error');probe(a,b,'roundtrip')
    print('PASS 9 combined mutations, SHA256 rollback and native baselines green',flush=True)


def metadata_mutations(engine,work):
    path=work/'gui-budget.c';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('pool bypasses metadata domain','a.allocate(a.context,sizeof(*p))','malloc(sizeof(*p))'),
        ('lease bypasses metadata domain','p->allocator.allocate(p->allocator.context,n)','((void)p,malloc(n))'),
        ('lease release uses wrong size','p->allocator.release(p->allocator.context,v,n)','p->allocator.release(p->allocator.context,v,n-1)'),
        ('failed runtime admit leaks metadata','(void)p0_budget_scope_close(&r->scope);metadata_free(p,r,sizeof(*r));','(void)p0_budget_scope_close(&r->scope);'),
        ('pool release uses wrong size','a.release(a.context,p,sizeof(*p));return P0_GUI_OK','a.release(a.context,p,sizeof(*p)-1);return P0_GUI_OK'),
    )
    try:
        for label,old,new in variants:
            if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique metadata anchor')
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            rc=subprocess.run([str(work/'process-gui-budget-test'),'metadata'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10).returncode
            if not rc:raise RuntimeError(label+': falsely green')
            FULL['run']([str(work/'process-gui-budget-test'),'valid'])
            print('PASS metadata mutation/'+label+': target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('metadata rollback SHA mismatch')
    a,b=build(engine,work);wire_cases(work);probe(a,b,'start-error');probe(a,b,'roundtrip')
    print('PASS 5 metadata mutations, SHA256 rollback and native baselines green',flush=True)


def ime_mutations(engine,work):
    path=work/'session-process-client.inc';original=path.read_bytes();text=original.decode();digest=hashlib.sha256(original).digest()
    variants=(
        ('IME preedit omitted','ghostty_surface_preedit(c->surface,"中",3);','/* preedit omitted */'),
        ('IME composition prematurely committed','.keycode=0,.text="中",.composing=true', '.keycode=0,.text="中",.composing=false'),
        ('IME clear omitted','ghostty_surface_preedit(c->surface,NULL,0);','/* clear omitted */'),
        ('IME committed via paste','ghostty_surface_key(c->surface,commit);','(void)commit;ghostty_surface_text(c->surface,"中文",6);'),
    )
    try:
        for label,old,new in variants:
            if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique IME anchor')
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            probe(a,b,'ime',1);probe(a,b,'roundtrip')
            print('PASS process mutation/'+label+': target red, control green',flush=True)
    finally:
        path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('IME rollback SHA mismatch')
    a,b=build(engine,work);probe(a,b,'ime');probe(a,b,'roundtrip')
    print('PASS 4 IME mutations, SHA256 rollback and native baselines green',flush=True)


def runtime_mutations(engine,work):
    source=work/'gui-budget.c';client=work/'session-process-client.inc'
    variants=(
        ('runtime bypasses shared admission',source,
         'p0_budget_scope_admit(&r->scope,&p->budget,bytes)',
         '(p0_budget_scope_init(&r->scope,&p->budget,bytes),true)','runtime','valid'),
        ('runtime live lease not counted',source,'p->reservations++;','/* counter omitted */','runtime','valid'),
        ('runtime promise never released',source,
         'if(!p0_budget_scope_close(&r->scope))','if(false && !p0_budget_scope_close(&r->scope))','runtime','valid'),
        ('runtime failed admit scope leaked',source,
         '(void)p0_budget_scope_close(&r->scope);metadata_free(p,r,sizeof(*r));','metadata_free(p,r,sizeof(*r));','runtime','valid'),
        ('runtime failed admit leaves stale output',source,
         '    if(out)*out=NULL;\n    if(!p || !bytes || !out)',
         '    if(!p || !bytes || !out)','runtime','valid'),
        ('GUI IO stack defaults used',client,
         'gt__thread_create(&thread,process_consume,&c,&io_options)',
         '((void)io_options,pthread_create(&thread,NULL,process_consume,&c))','start-error','roundtrip'),
    )
    for label,path,old,new,target,control in variants:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique runtime anchor')
        try:
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n');a,b=build(engine,work)
            if path==source:
                rc=subprocess.run([str(work/'process-gui-budget-test'),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10).returncode
                if not rc:raise RuntimeError(label+': falsely green')
                FULL['run']([str(work/'process-gui-budget-test'),control])
            else:probe(a,b,target,1);legacy(b)
            print('PASS runtime mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('runtime rollback SHA mismatch')
    a,b=build(engine,work);wire_cases(work);probe(a,b,'start-error');probe(a,b,'roundtrip')
    print('PASS 6 runtime mutations, SHA256 rollback and native baselines green',flush=True)


def startup_mutations(engine,work):
    server=work/'session-process-owner.c'
    original=server.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
    variants=(
        ('startup peer cleanup omitted',
         'CHECK(!gt__attachment_current(s.attachment,s.generation));gt__peer_close(peer);',
         'CHECK(!gt__attachment_current(s.attachment,s.generation)); /* close omitted */'),
        ('startup stack reservation leaked',
         'p0_budget_scope_read(&s.source).live==controller_live && p0_budget_scope_close(&s.worker_stack)',
         'p0_budget_scope_read(&s.source).live==controller_live && true'),
        ('startup worker bypasses domain',
         'gt__manager_create(s.manager,&config,&boot_allocator,&s.thread_options,process_dispose,NULL,&s.session)',
         'gt__manager_create(s.manager,&config,NULL,NULL,process_dispose,NULL,&s.session)'),
        ('startup executor bypasses domain',
         'gt__tasks_open_with_options(1,&boot_allocator,&s.thread_options)',
         'gt__tasks_open(1)'),
        ('startup controller bypasses domain',
         'gt__attachment_open_with_allocator(s.tasks,64,4,&boot_allocator)',
         'gt__attachment_open(s.tasks,64,4)'),
        ('startup peer bypasses domain',
         'gt__peer_adopt_with_allocator(s.attachment,s.generation,fd,&boot_peer_allocator,&peer)',
         '((void)boot_peer_allocator,gt__peer_adopt(s.attachment,s.generation,fd,&peer))'),
        ('startup worker stack unreserved',
         'CHECK(p0_budget_scope_admit(&s.worker_stack, &s.budget, s.thread_reserve));',
         'p0_budget_scope_init(&s.worker_stack,&s.budget,0);'),
        ('startup unknown latch omitted',
         'CHECK(gt__attachment_end_unknown(s.attachment,s.generation)==GT_RESTORE_OK);',
         '(void)s;'),
        ('startup result misframed as snapshot',
         'pw_send(&io,PW_RESULT,0,bytes,length)',
         'pw_send(&io,PW_SNAPSHOT,0,bytes,length)'),
        ('startup unknown replaced by known zero',
         'CHECK(rsw_encode(s.generation,&result,bytes,sizeof(bytes),&length));',
         'CHECK(rsw_encode(s.generation,&result,bytes,sizeof(bytes),&length));bytes[7]=1;'),
    )
    try:
        for label,old,new in variants:
            if MARK in text or text.count(old)!=1:raise RuntimeError(label+': nonunique anchor')
            server.write_text(text.replace(old,new)+f'\n/* {MARK} */\n')
            a,b=build(engine,work);probe(a,b,'start-error',1);probe(a,b,'roundtrip')
            print('PASS process mutation/'+label+': target red, control green',flush=True)
    finally:
        server.write_bytes(original)
        if hashlib.sha256(server.read_bytes()).digest()!=digest:raise RuntimeError('startup rollback SHA mismatch')
    a,b=build(engine,work);probe(a,b,'start-error');probe(a,b,'roundtrip')
    print('PASS 10 startup mutations, SHA256 rollback and both baselines green',flush=True)


def mutations(engine,work):
    tui_mutations(engine,work)
    tmux_mutations(engine,work)
    scheme_pixel_mutations(engine,work)
    scheme_mutations(engine,work)
    scheme_owner_mutations(engine,work)
    pointer_mutations(engine,work)
    backend_oom_mutations(engine,work)
    state_oom_mutations(engine,work)
    combined_mutations(engine,work)
    metadata_mutations(engine,work)
    ime_mutations(engine,work)
    runtime_mutations(engine,work)
    startup_mutations(engine,work)
    server=work/'session-process-owner.c';client=work/'session-process-client.inc';wire=work/'session-process-wire.c'
    variants=(
        ('late query missing on reply error',server,
         'const char query[]="\\x1b[5n";CHECK(n>=sizeof(query)-1);memcpy(p,query,sizeof(query)-1);return sizeof(query)-1;',
         'return GT_TRANSPORT_READ_ERROR;','reply-error','read-error'),
        ('failed reply applied event discarded',server,
         'server *s=context;CHECK(sequence==s->events);\n  gt_restore_rc_t rc;',
         'server *s=context;CHECK(sequence==s->events);if(s->reply_error && atomic_load(&s->inject_read_error))return;\n  gt_restore_rc_t rc;',
         'reply-error','read-error'),
        ('invalid state notification not wired',server,
         '.state_invalidated=state_invalidated,','.state_invalidated=(false?state_invalidated:NULL),', 'apply-error','read-error'),
        ('invalid state final snapshot exported',server,
         'if(!s->invalid_state) {','if(true) {','apply-error','read-error'),
        ('partial apply falsely succeeds',server,
         'if(poison)return GT_ERR_IO;', 'if(poison)return GT_OK;', 'apply-error','read-error'),
        ('recovery end latch omitted',server,
         'CHECK(gt__attachment_end(s->attachment,s->generation,result.sequence)==GT_RESTORE_WAIT);',
         '/* recovery latch omitted */','recovery-error','roundtrip'),
        ('recovery final sequence corrupted',server,
         'gt__attachment_end(s->attachment,s->generation,result.sequence)',
         'gt__attachment_end(s->attachment,s->generation,result.sequence+1)','recovery-error','roundtrip'),
        ('controller never fenced on end',server,
         'CHECK(gt__attachment_end(s.attachment,s.generation,s.worker_result.sequence)==GT_RESTORE_OK);',
         '/* end fence omitted */','roundtrip','legacy'),
        ('inflight input falsely acknowledged',server,
         '((uint64_t)GT_ERR_CANCELLED<<32)|PW_INPUT_ENDED',
         '((uint64_t)GT_OK<<32)|PW_INPUT_ENDED','roundtrip','legacy'),
        ('terminal result never sent',server,
         'CHECK(pw_send(&io,PW_RESULT,cursor,result_bytes,result_length));',
         '/* end omitted */','roundtrip','legacy'),
        ('terminal result generation corrupted',server,
         'rsw_encode(s.generation,&s.worker_result,result_bytes,sizeof(result_bytes),&result_length)',
         'rsw_encode(s.generation+1,&s.worker_result,result_bytes,sizeof(result_bytes),&result_length)','roundtrip','legacy'),
        ('duplicate terminal result sent',server,
         'CHECK(pw_send(&io,PW_RESULT,cursor,result_bytes,result_length));',
         'CHECK(pw_send(&io,PW_RESULT,cursor,result_bytes,result_length));CHECK(pw_send(&io,PW_RESULT,cursor,result_bytes,result_length));','roundtrip','legacy'),
        ('output follows terminal result',server,
         'CHECK(pw_send(&io,PW_RESULT,cursor,result_bytes,result_length));',
         'CHECK(pw_send(&io,PW_RESULT,cursor,result_bytes,result_length));CHECK(pw_send(&io,PW_OUTPUT,cursor+1,"!",1));','roundtrip','legacy'),
        ('ended input admitted',client,
         'if(atomic_load(&c->ended))',
         'if(false && atomic_load(&c->ended))','roundtrip','legacy'),
        ('ended window title not updated',client,
         'window.title=ended_title;',
         'window.title=@"";','roundtrip','legacy'),
        ('read failure collapsed into EOF',server,
         'if(atomic_load(&b->owner->inject_read_error))return GT_TRANSPORT_READ_ERROR;',
         'if(atomic_load(&b->owner->inject_read_error))return GT_TRANSPORT_READ_EOF;','read-error','roundtrip'),
        ('worker terminal result never collected',server,
         'CHECK(process_result(&s,&s.worker_result));',
         '/* terminal report omitted */','roundtrip','legacy'),
        ('waiter stack defaults silently used',server,
         'gt__pty_open_with_options(&options,&allocator,&s->thread_options)',
         'gt__pty_open_with_options(&options,&allocator,NULL)','roundtrip','legacy'),
        ('waiter stack capacity not admitted',server,
         'CHECK(p0_budget_scope_admit(&s.waiter_stack,&s.budget,s.thread_reserve));',
         'p0_budget_scope_init(&s.waiter_stack,&s.budget,0);','roundtrip','legacy'),
        ('PTY backend escapes source budget',server,
         'gt__pty_open_with_options(&options,&allocator,&s->thread_options)',
         'gt__pty_open_with_options(&options,((void)allocator,NULL),&s->thread_options)','roundtrip','legacy'),
        ('worker stack defaults silently used',server,
         'gt__manager_create(s.manager,&config,&queue_allocator,&s.thread_options,process_dispose,NULL,&s.session)',
         'gt__manager_create(s.manager,&config,&queue_allocator,NULL,process_dispose,NULL,&s.session)','roundtrip','legacy'),
        ('encoder stack defaults silently used',server,
         'gt__tasks_open_with_options(1,&queue_allocator,&s.thread_options)',
         'gt__tasks_open_with_options(1,&queue_allocator,NULL)','roundtrip','legacy'),
        ('worker stack capacity not admitted',server,
         'CHECK(p0_budget_scope_admit(&s.worker_stack,&s.budget,s.thread_reserve));',
         'p0_budget_scope_init(&s.worker_stack,&s.budget,0);','roundtrip','legacy'),
        ('encoder stack capacity not admitted',server,
         'CHECK(p0_budget_scope_admit(&s.encoder_stack,&s.budget,s.thread_reserve));',
         'p0_budget_scope_init(&s.encoder_stack,&s.budget,0);','roundtrip','legacy'),
        ('IPC peer escapes source budget',server,
         'gt__peer_adopt_with_allocator(s.attachment,s.generation,fd,&peer_allocator,&peer)',
         'gt__peer_adopt_with_allocator(s.attachment,s.generation,fd,((void)peer_allocator,NULL),&peer)','roundtrip','legacy'),
        ('checkpoint pool escapes source budget',server,
         'gt__tasks_open_with_options(1,&queue_allocator,&s.thread_options)',
         'gt__tasks_open_with_options(1,NULL,&s.thread_options)','roundtrip','legacy'),
        ('worker escapes source budget',server,
         'gt__manager_create(s.manager,&config,&queue_allocator,&s.thread_options,process_dispose,NULL,&s.session)',
         'gt__manager_create(s.manager,&config,NULL,&s.thread_options,process_dispose,NULL,&s.session)','roundtrip','legacy'),
        ('restore queue escapes source budget',server,
         'gt__attachment_open_with_allocator(s.tasks,PSW_RESTORE_BYTES+(s.history_policy?HSW_EVENT_PREFIX:0),64,&queue_allocator)',
         'gt__attachment_open_with_allocator(s.tasks,PSW_RESTORE_BYTES+(s.history_policy?HSW_EVENT_PREFIX:0),64,((void)queue_allocator,NULL))','roundtrip','legacy'),
        ('authority summary escapes source budget',server,
         'gt__program_status_open_with_allocator(32,65536,&allocator)',
         'gt__program_status_open_with_allocator(32,65536,((void)allocator,NULL))','roundtrip','legacy'),
        ('snapshot summary remains in source budget',server,
         'gt__program_status_copy_with_allocator(s->summary,&allocator,&c->summary)',
         '((void)allocator,gt__program_status_copy(s->summary,&c->summary))','roundtrip','legacy'),
        ('authority summary not released',server,
         'gt__program_status_close(s.summary);',
         '/* owner summary leaked */','roundtrip','legacy'),
        ('decoded summary lease retained on shutdown',client,
         'gt__program_status_close(c.summary);c.summary=NULL;',
         'c.summary=NULL;','roundtrip','legacy'),
        ('decoded summary escapes GUI budget',client,
         'psw_decode_with_allocator(bytes,n,&allocator,out)',
         'psw_decode_with_allocator(bytes,n,((void)allocator,NULL),out)','roundtrip','legacy'),
        ('semantic error text discarded',server,
         '.command=strings[0],.error=strings[1]',
         '.command=strings[0],.error=""','roundtrip','legacy'),
        ('live unknown-exit error omitted by shell',server,
         r'\\033]133;D;err=unknown exit failure\\007FOURTH',
         r'\\033]133;D\\007FOURTH','roundtrip','legacy'),
        ('semantic program cleanup not routed',server,
         'size_t removed=gt__program_status_semantic(s->summary,e.kind);',
         'size_t removed=0;','roundtrip','legacy'),
        ('input-start incorrectly routed as prompt-start',server,
         'gt__program_status_semantic(s->summary,e.kind)',
         'gt__program_status_semantic(s->summary,e.kind==2?1:e.kind)','roundtrip','legacy'),
        ('command-only change never marked dirty',server,
         'else if(e.kind==3 || e.kind==4)s->command_dirty=true;',
         'else if(e.kind==3 || e.kind==4)(void)0;','roundtrip','legacy'),
        ('live command digest corrupted',server,
         'CC_SHA256(command,(CC_LONG)command_length,meta+56);',
         'CC_SHA256(command,(CC_LONG)command_length,meta+56);meta[56]^=1;','roundtrip','legacy'),
        ('command snapshot digest corrupted',server,
         'CC_SHA256(command_wire,(CC_LONG)command_length,metadata+184);',
         'CC_SHA256(command_wire,(CC_LONG)command_length,metadata+184);metadata[184]^=1;',
         'roundtrip','legacy'),
        ('client command snapshot not decoded',client,
         'CHECK(csw_decode(command_wire,(size_t)command_length,&c->command));',
         '/* command snapshot omitted */','roundtrip','legacy'),
        ('semantic prompt callback missing',server,
         'CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_SEMANTIC_PROMPT,(const void *)semantic_prompt)==GHOSTTY_SUCCESS);',
         '(void)semantic_prompt;', 'roundtrip','legacy'),
        ('shell exit code omitted from OSC 133',server,
         r'\\033]133;D;7;err=detached failure\\007',r'\\033]133;D;err=detached failure\\007','roundtrip','legacy'),
        ('command captured as empty',server,
         'c->command=s->command;', 'memset(&c->command,0,sizeof(c->command));','roundtrip','legacy'),
        ('program update never bundled with output',server,
         'if(s->summary_dirty || s->command_dirty) {','if(false && (s->summary_dirty || s->command_dirty)) {','roundtrip','legacy'),
        ('live summary generation corrupted',server,
         'pw_put64(meta,s->generation);','pw_put64(meta,s->generation+1);','roundtrip','legacy'),
        ('live summary digest corrupted',server,
         'CC_SHA256(summary,(CC_LONG)summary_length,meta+16);',
         'CC_SHA256(summary,(CC_LONG)summary_length,meta+16);meta[16]^=1;','roundtrip','legacy'),
        ('summary wire digest corrupted',server,
         'CC_SHA256(summary_wire,(CC_LONG)summary_length,metadata+144);',
         'CC_SHA256(summary_wire,(CC_LONG)summary_length,metadata+144);metadata[144]^=1;',
         'roundtrip','legacy'),
        ('client summary import omitted',client,
         'CHECK(process_decode_summary(c,summary_wire,(size_t)summary_length,&summary));',
         '/* summary not decoded */','roundtrip','legacy'),
        ('program status callback never installed',server,
         'CHECK(ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_PROGRAM_STATUS,(const void *)program_status)==GHOSTTY_SUCCESS);',
         '(void)program_status;', 'roundtrip','legacy'),
        ('detached program status not emitted',server,
         r'\\033]7501;id=build:app=ghosterm:state=blocked:kind=permission:msg=QXBwcm92ZT8=\\007',
         '', 'roundtrip','legacy'),
        ('program summary capture omitted',server,
         'CHECK(gt__program_status_copy_with_allocator(s->summary,&allocator,&c->summary)==GT_STATUS_OK);',
         '(void)allocator; /* summary not captured */','roundtrip','legacy'),
        ('initial title lost before first GUI snapshot',server,
         r'\\033]2;Ghosterm initial\\007\\033]7;file:///ghosterm/initial\\007',
         r'\\033]7;file:///ghosterm/initial\\007', 'kill','roundtrip'),
        ('initial cwd lost before first GUI snapshot',server,
         r'\\033]7;file:///ghosterm/initial\\007',
         '', 'kill','roundtrip'),
        ('detached title update omitted',server,
         r'\\033]2;Ghosterm detached\\007',
         '', 'roundtrip','legacy'),
        ('detached cwd update omitted',server,
         r'\\033]7;file:///ghosterm/detached\\007',
         '', 'roundtrip','legacy'),
        ('history workload omitted before capture',server,
         'if(s->history) {\n    CHECK(snprintf(workload,sizeof(workload),',
         'if(false && s->history) {\n    CHECK(snprintf(workload,sizeof(workload),','history','roundtrip'),
        ('foreign engine build received before snapshot',server,
         'memcpy(metadata+104,process_engine_build,sizeof(process_engine_build));',
         'memcpy(metadata+104,process_engine_build,sizeof(process_engine_build));metadata[104]^=1;',
         'roundtrip','legacy'),
        ('post-L event not transmitted while waiting for READY(L)',server,
         'cursor=drain(&s,&io,cursor);CHECK(cursor>cut);',
         'CHECK(cursor>cut);','crossing','roundtrip'),
        ('client stops reading events while waiting for LIVE confirmation',client,
         'if(frame.kind==PW_LIVE_OK)break;',
         'CHECK(frame.kind==PW_LIVE_OK);break;','crossing','roundtrip'),
        ('canonical digest corrupted before import',server,
         'CC_SHA256(c->canonical,(CC_LONG)c->canonical_len,metadata+40);',
         'CC_SHA256(c->canonical,(CC_LONG)c->canonical_len,metadata+40);metadata[40]^=1;',
         'roundtrip','legacy'),
        ('resource digest corrupted before import',server,
         'CC_SHA256(c->resources,(CC_LONG)c->resources_len,metadata+72);',
         'CC_SHA256(c->resources,(CC_LONG)c->resources_len,metadata+72);metadata[72]^=1;',
         'roundtrip','legacy'),
        ('shared-pool native exercise omitted',client,
         'process_admission_probe(&c,opts);','if(false)process_admission_probe(&c,opts);','roundtrip','legacy'),
        ('second surface reuses incumbent engine',client,
         'ghostty_surface_t surface=ghostty_surface_new(surface_app,&opts);CHECK(surface);',
         'ghostty_surface_t surface=c->surface;CHECK(surface);','roundtrip','legacy'),
        ('received checkpoint buffer leaked',client,
         'p0_gui_buffer_free(owner->budget,(void *)c->canonical,c->canonical_len);',
         '/* wire canonical leaked */','roundtrip','legacy'),
        ('client exits with TERM instead of actual SIGKILL',server,
         'CHECK(!kill(child,SIGKILL));','CHECK(!kill(child,SIGTERM));','kill','roundtrip'),
        ('dead connection never observed by owner',server,
         'gt_peer_result_t closed=gt__peer_receive(peer,&byte,1,1000);',
         'gt_peer_result_t closed={GT_PEER_CLOSED,0,0};(void)byte;','kill','roundtrip'),
        ('replacement reuses dead generation',server,
         'if(round==2 && killed)CHECK(checkpoint.sequence>old_sequence && s.generation>old_generation);',
         'if(round==2 && killed)s.generation=old_generation;','kill','roundtrip'),
        ('authority never advances while GUI is absent',server,
         'advance(&s);process_running(&s);printf("PASS process/gui SIGKILL:',
         'printf("PASS process/gui SIGKILL:','kill','roundtrip'),
        ('snapshot delta resize not applied',client,
         'CHECK(ghostty_p0_surface_event_resize_for(c->surface,c->generation,frame->sequence,(uint16_t)cols,(uint16_t)rows,c->cell_width,c->cell_height));',
         '(void)cols;(void)rows;','kill','legacy'),
        ('LIVE output dequeued but not applied',client,
         'CHECK(ghostty_p0_surface_event_output_for(c->surface,c->generation,frame->sequence,frame->bytes,frame->length));',
         '/* engine application omitted */','kill','legacy'),
        ('encoded input loses generation',client,
         'pw_put64(bytes,c->input_generation);memcpy(bytes+8,c->input,n);','pw_put64(bytes,c->input_generation+1);memcpy(bytes+8,c->input,n);','kill','legacy'),
        ('encoded input loses applied sequence',client,
         'pw_send(&c->io,PW_INPUT,c->input_sequence,bytes,n+8)',
         'pw_send(&c->io,PW_INPUT,0,bytes,n+8)','kill','legacy'),
        ('LIVE frame never reaches captured pixels',client,
         'CHECK(ctx);CGContextDrawImage(ctx,CGRectMake(0,0,width,height),image);',
         'CHECK(ctx);','roundtrip','legacy'),
        ('query reply duplicated in authority',server,
         's->replies++;s->reply_bytes+=n;CHECK(s->sink.write(s->sink.context,bytes,n)==GT_OK);',
         's->replies++;s->reply_bytes+=n;CHECK(s->sink.write(s->sink.context,bytes,n)==GT_OK && s->sink.write(s->sink.context,bytes,n)==GT_OK);',
         'roundtrip','legacy'),
    )
    for label,path,old,new,target,control in variants:
        original=path.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
        if MARK in text or text.count(old)!=1:raise RuntimeError(f'nonunique process mutation: {label}')
        try:
            path.write_text(text.replace(old,new)+f'\n/* {MARK} */\n')
            a,b=build(engine,work);probe(a,b,target,1)
            if control=='legacy':legacy(b)
            else:probe(a,b,control)
            print(f'PASS process mutation/{label}: target red, control green',flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError('process mutation rollback SHA mismatch')
    wire_variants=(
        ('blob send restarts deadline for every frame',
         'send_until(io,kind,0,p,count,end)',
         'send_until(io,kind,0,p,count,((void)end,now()+5))','slow-send','blobs'),
        ('blob receive restarts deadline for every frame',
         'receive_until(io,&frame,end)',
         'receive_until(io,&frame,((void)end,now()+5))','slow-receive','blobs'),
        ('magic accepted','memcmp(h,"GTP0",4)','false','magic','valid'),
        ('version accepted','h[5]!=1','false','version','valid'),
        ('unknown flags accepted','get32(h+8)','false','flags','valid'),
        ('unknown kind accepted','h[7]>PW_HISTORY','false','kind','valid'),
        ('oversize body read before bound','if(n>PW_CHUNK)return false;',
         '/* frame cap omitted */','length','valid'),
        ('wrong blob kind accepted','frame.kind!=kind','((void)kind,false)','blob-kind','valid'),
        ('blob overwrites caller buffer','frame.length>n','false','blob-length','valid'),
        ('encoded whole frame claimed after short I/O','p+=(size_t)count;n-=(size_t)count;',
         'p+=n;n=0;','valid','invalid-send'),
        ('truncated payload accepted','if(!exact(io,out->bytes,n,false,end))return false;',
         '(void)exact(io,out->bytes,n,false,end);','truncated','valid'),
    )
    original=wire.read_bytes();digest=hashlib.sha256(original).digest();text=original.decode()
    for label,old,new,target,control in wire_variants:
        if MARK in text or text.count(old)!=1:raise RuntimeError(f'nonunique wire mutation: {label}')
        try:
            wire.write_text(text.replace(old,new)+f'\n/* {MARK} */\n')
            # Compile only the experimental codec/test; engine stays pinned.
            FULL['run']([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                         '-I',str(PROBES),str(wire),str(work/'session-process-wire-test.c'),'-o',str(work/'process-wire-test')])
            result=subprocess.run([str(work/'process-wire-test'),target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=12)
            if not result.returncode:raise RuntimeError(label+': falsely green')
            FULL['run']([str(work/'process-wire-test'),control])
            print(f'PASS wire mutation/{label}: target red, control green',flush=True)
        finally:
            wire.write_bytes(original)
            if hashlib.sha256(wire.read_bytes()).digest()!=digest:raise RuntimeError('wire rollback SHA mismatch')
    warmup_mutations(engine,work)
    app_budget_mutations(engine,work)
    search_mutations(engine,work)
    signal_stack_mutations(engine,work)
    thread_args_mutations(engine,work)
    engine_thread_mutations(engine,work)
    main_stack_mutations(engine,work)
    native_ime_mutations(engine,work)
    native_pointer_mutations(engine,work)
    gui_mutations(engine,work)
    summary_mutations(work)
    summary_queue_mutations(work)
    command_wire_mutations(work)
    FULL['run']([sys.executable,str(ROOT/'scripts/mutation-check-result-wire.py')])
    FULL['run']([sys.executable,str(ROOT/'scripts/probe-session-history.py'),'--mutations'])
    a,b=build(engine,work);wire_cases(work)
    history_state_mutations(engine,work)
    config_mutations(engine,work)
    global_owner_mutations(engine,work)
    global_io_mutations(engine,work)
    manager_mutations(engine,work)
    for case in ('roundtrip','kill','crossing','history','read-error','recovery-error','apply-error','reply-error','start-error','ime','state-oom','backend-oom','pointer','scheme','tmux','less','vim','native-pointer','native-ime','search','history-limit'):probe(a,b,case)
    for case in SURFACE['CASES']:legacy(b,case)
    print('PASS 103 process + 11 wire + 33 GUI-budget + 8 summary-wire + 3 summary-queue + 8 command-wire + 11 result-wire + 4 scheme-owner + 13 process-scheme + 4 scheme-pixel + 9 tmux + 8 real-TUI + 5 native-pointer + 11 native-IME + 11 main-stack + 11 engine-thread + 8 thread-args + 6 signal-stack + 7 search + 10 app-budget + 10 warmup + 8 history-pruned + 18 history-state + 28 config + 15 global-owner + 27 global-IO + 10 manager-composition mutations, SHA256 rollback, 26 legacy surface baselines; P0 incomplete',flush=True)


def history_state_mutations(engine,work):
    codec=work/'session-history-wire.c';owner=work/'session-process-owner.c';client=work/'session-process-client.inc'
    variants=(
        ('codec reserved header',codec,'if(memcmp(p,header,sizeof(header)))return false;','(void)header;','invalid','valid'),
        ('codec narrowed state',codec,'uint64_t state=pw_get64(p+24);','uint64_t state=(unsigned)pw_get64(p+24);','invalid','valid'),
        ('codec decoder policy',codec,'if(!valid(s))return false;','/* invalid policy accepted */','invalid','valid'),
        ('codec encoder policy',codec,'n!=HSW_BYTES || !valid(s)','n!=HSW_BYTES','invalid','valid'),
        ('codec decoder length',codec,'!data || !out || n!=HSW_BYTES','((void)n,!data || !out)','invalid','valid'),
        ('codec encoder length',codec,'!out || n!=HSW_BYTES || !valid(s)','((void)n,!out || !valid(s))','invalid','valid'),
        ('codec swapped endian field',codec,'pw_put64(data+8,s.bytes);','pw_put64(data+8,s.lines);','valid','invalid'),
        ('codec failure modifies output',codec,'if(memcmp(p,header,sizeof(header)))return false;','if(memcmp(p,header,sizeof(header))){out->state=0;return false;}','invalid','valid'),
        ('owner prune notification',owner,'server *s=context;atomic_store(&s->history_state,HSW_TRUNCATED);','server *s=context;(void)s;','history-limit','roundtrip'),
        ('owner byte policy not applied',owner,'const size_t history_bytes=MIB,history_lines=100;','const size_t history_bytes=64*MIB,history_lines=100;','history-limit','roundtrip'),
        ('owner policy not applied',owner,'const size_t history_bytes=MIB,history_lines=100;','const size_t history_bytes=MIB,history_lines=100000;','history-limit','roundtrip'),
        ('frozen state replaced by live',owner,'scheme,checkpoint.dark,s.history_policy,checkpoint.history_state','scheme,checkpoint.dark,s.history_policy,history_state(&s)','history-limit','roundtrip'),
        ('queued state stale complete',owner,'hsw_encode(history_state(s),wrapped+8,HSW_BYTES)','hsw_encode((hsw_state){MIB,100,HSW_COMPLETE},wrapped+8,HSW_BYTES)','history-limit','roundtrip'),
        ('wrong history generation',owner,'pw_put64(data,g);CHECK(hsw_encode(history,data+8,HSW_BYTES))','pw_put64(data,g+1);CHECK(hsw_encode(history,data+8,HSW_BYTES))','history-limit','roundtrip'),
        ('wrong history sequence',owner,'pw_send(io,PW_HISTORY,e.sequence,data,sizeof(data))','pw_send(io,PW_HISTORY,e.sequence+1,data,sizeof(data))','history-limit','roundtrip'),
        ('unknown UI folded',client,'state==HSW_UNKNOWN?@"History status unknown"','state==HSW_UNKNOWN?@""','history-limit','roundtrip'),
        ('truncated UI hidden',client,'state==HSW_TRUNCATED?@"History truncated"','state==HSW_TRUNCATED?@""','history-limit','roundtrip'),
        ('live UI state not published',client,'if(!snapshot)atomic_store(&c->history_display,next.state);','if(false && !snapshot)atomic_store(&c->history_display,next.state);','history-limit','roundtrip'),
    )
    originals={p:p.read_bytes() for p in (codec,owner,client)}
    digests={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK in data.decode() for data in originals.values()):raise RuntimeError('history-state marker before baseline')
    for label,path,old,new,target,control in variants:
        if originals[path].decode().count(old)!=1:raise RuntimeError(label+': nonunique anchor')
    a,b=build(engine,work);wire_cases(work);probe(a,b,'history-limit');probe(a,b,'roundtrip')
    try:
        for label,path,old,new,target,control in variants:
            try:
                path.write_text(originals[path].decode().replace(old,new)+'\n/* '+MARK+' */\n')
                a,b=build(engine,work) # compile errors never count as target rejection
                if path==codec:
                    result=subprocess.run([str(work/'process-history-test'),target],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=12)
                    if result.returncode==0 or 'Assertion failed' not in result.stdout:raise RuntimeError(label+': missing assertion rejection: '+result.stdout)
                    FULL['run']([str(work/'process-history-test'),control])
                else:
                    probe(a,b,target,1);probe(a,b,control)
                print('PASS history-state mutation/'+label+': target red, control green',flush=True)
            finally:
                path.write_bytes(originals[path])
                if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('history-state SHA rollback mismatch')
    finally:
        for path,data in originals.items():
            path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('history-state SHA rollback mismatch')
    a,b=build(engine,work);wire_cases(work);probe(a,b,'history-limit');probe(a,b,'roundtrip')
    print('PASS 18 history-state mutations, SHA256 rollback and native history-limit/roundtrip baselines',flush=True)


def copy_sources(work):
    for source in SOURCES:(work/source.name).write_bytes(source.read_bytes())

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',choices=('history-limit','kill','roundtrip','crossing','history','read-error','recovery-error','apply-error','reply-error','start-error','ime','state-oom','backend-oom','pointer','scheme','tmux','less','vim','native-pointer','native-ime','search'));parser.add_argument('--history-state-mutations',action='store_true');parser.add_argument('--config-mutations',action='store_true');parser.add_argument('--global-owner-mutations',action='store_true');parser.add_argument('--global-io-mutations',action='store_true');parser.add_argument('--manager-mutations',action='store_true');parser.add_argument('--mutations',action='store_true');parser.add_argument('--startup-mutations',action='store_true');parser.add_argument('--runtime-mutations',action='store_true');parser.add_argument('--ime-mutations',action='store_true');parser.add_argument('--metadata-mutations',action='store_true');parser.add_argument('--combined-mutations',action='store_true');parser.add_argument('--state-oom-mutations',action='store_true');parser.add_argument('--backend-oom-mutations',action='store_true');parser.add_argument('--pointer-mutations',action='store_true');parser.add_argument('--native-pointer-mutations',action='store_true');parser.add_argument('--native-ime-mutations',action='store_true');parser.add_argument('--main-stack-mutations',action='store_true');parser.add_argument('--engine-thread-mutations',action='store_true');parser.add_argument('--thread-args-mutations',action='store_true');parser.add_argument('--signal-stack-mutations',action='store_true');parser.add_argument('--search-mutations',action='store_true');parser.add_argument('--app-budget-mutations',action='store_true');parser.add_argument('--warmup-mutations',action='store_true');parser.add_argument('--scheme-owner-mutations',action='store_true');parser.add_argument('--scheme-mutations',action='store_true');parser.add_argument('--scheme-pixel-mutations',action='store_true');parser.add_argument('--tmux-mutations',action='store_true');parser.add_argument('--tui-mutations',action='store_true');args=parser.parse_args()
    if sum((bool(args.case),args.manager_mutations,args.global_io_mutations,args.global_owner_mutations,args.config_mutations,args.history_state_mutations,args.mutations,args.startup_mutations,args.runtime_mutations,args.ime_mutations,args.metadata_mutations,args.combined_mutations,args.state_oom_mutations,args.backend_oom_mutations,args.pointer_mutations,args.native_pointer_mutations,args.native_ime_mutations,args.main_stack_mutations,args.engine_thread_mutations,args.thread_args_mutations,args.signal_stack_mutations,args.search_mutations,args.app_budget_mutations,args.warmup_mutations,args.scheme_owner_mutations,args.scheme_mutations,args.scheme_pixel_mutations,args.tmux_mutations,args.tui_mutations))>1:parser.error('choose only one case or mutation mode')
    if sys.platform!='darwin' or os.uname().machine!='arm64':raise RuntimeError('requires Apple Silicon macOS and Xcode')
    files=(*SOURCES,*PATCHES,ROOT/'core/src/session/manager.c',ROOT/'core/src/session/manager.h',ROOT/'scripts/prepare-session-io.py',ROOT/'core/src/session/attachment.c',ROOT/'core/src/session/restore-stream.c',ROOT/'scripts/probe-session-history.py')
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    if args.manager_mutations or args.global_io_mutations or args.global_owner_mutations or args.mutations:
        prior=ROOT/'tmp/worker-surface'
        prior_files=(prior/'session-process-owner.c',prior/'engine/src/main_c.zig',prior/'engine/src/global.zig',prior/'engine/src/lib/p0_threaded_io.zig',prior/'engine/src/apprt/p0_io_threads.zig',prior/'engine/src/apprt/p0_thread.zig',prior/'session-global-owner.h',prior/'session-process-client.inc',prior/'session-surface-owner.m')
        if any(MARK in p.read_text() for p in prior_files if p.exists()):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,FULL['interrupted'])
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    os.environ.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    try:
        FULL['run'](['bash',str(ROOT/'scripts/build-core.sh')])
        runpy.run_path(str(ROOT/'scripts/probe-session-state.py'))['build_engine']()
        # Share the established fixed path/cache; never run this concurrently
        # with worker-surface probes. Sources always re-extracted from the pin.
        work=ROOT/'tmp/worker-surface';work.mkdir(parents=True,exist_ok=True)
        for name in ('engine','engine.tar'):
            path=work/name
            if path.is_dir():shutil.rmtree(path)
            elif path.exists():path.unlink()
        engine=FULL['prepare'](work)
        for patch in PATCHES:
            FULL['run'](['git','-C',str(engine),'apply','--check',str(patch)])
            FULL['run'](['git','-C',str(engine),'apply',str(patch)])
        VT['build'](engine,work,True,log_path=ROOT/'tmp/process-vt-build.log')
        FULL['build'](engine,work,cache_dir=work/'full-cache',log_path=ROOT/'tmp/process-full-build.log')
        copy_sources(work);server,client=build(engine,work)
        thread_args_tests(engine,work)
        wire_cases(work)
        if args.manager_mutations:
            manager_mutations(engine,work);return 0
        if args.global_io_mutations:
            global_io_mutations(engine,work);return 0
        if args.global_owner_mutations:
            global_owner_mutations(engine,work);return 0
        if args.config_mutations:
            config_mutations(engine,work);return 0
        if args.history_state_mutations:
            history_state_mutations(engine,work);return 0
        if args.tui_mutations:
            tui_mutations(engine,work);return 0
        if args.tmux_mutations:
            tmux_mutations(engine,work);return 0
        if args.scheme_pixel_mutations:
            scheme_pixel_mutations(engine,work);return 0
        if args.scheme_mutations:
            scheme_mutations(engine,work);return 0
        if args.scheme_owner_mutations:
            scheme_owner_mutations(engine,work);return 0
        for case in (args.case,) if args.case else ('search','roundtrip') if args.search_mutations else ('roundtrip','start-error') if (args.engine_thread_mutations or args.thread_args_mutations or args.signal_stack_mutations or args.app_budget_mutations or args.warmup_mutations) else ('roundtrip','start-error','state-oom','backend-oom') if args.main_stack_mutations else ('native-ime','ime') if args.native_ime_mutations else ('native-pointer','pointer') if args.native_pointer_mutations else ('pointer','roundtrip') if args.pointer_mutations else ('backend-oom','state-oom') if args.backend_oom_mutations else ('state-oom','start-error') if args.state_oom_mutations else ('ime','roundtrip') if args.ime_mutations else ('start-error','roundtrip') if (args.startup_mutations or args.runtime_mutations or args.metadata_mutations or args.combined_mutations) else ('roundtrip','kill','crossing','history','read-error','recovery-error','apply-error','reply-error','start-error','ime','state-oom','backend-oom','pointer','scheme','tmux','less','vim','native-pointer','native-ime','search','history-limit'):probe(server,client,case)
        if args.warmup_mutations:
            warmup_mutations(engine,work);return 0
        if args.app_budget_mutations:
            app_budget_mutations(engine,work);return 0
        if args.search_mutations:
            search_mutations(engine,work);return 0
        if args.signal_stack_mutations:
            signal_stack_mutations(engine,work);return 0
        if args.thread_args_mutations:
            thread_args_mutations(engine,work);return 0
        if args.engine_thread_mutations:
            engine_thread_mutations(engine,work);return 0
        if args.main_stack_mutations:
            main_stack_mutations(engine,work);return 0
        if args.native_ime_mutations:
            native_ime_mutations(engine,work);return 0
        if args.native_pointer_mutations:
            native_pointer_mutations(engine,work);return 0
        if args.pointer_mutations:
            pointer_mutations(engine,work);return 0
        if args.backend_oom_mutations:
            backend_oom_mutations(engine,work);return 0
        if args.state_oom_mutations:
            state_oom_mutations(engine,work);return 0
        if args.combined_mutations:
            combined_mutations(engine,work);return 0
        if args.metadata_mutations:
            metadata_mutations(engine,work);return 0
        if args.ime_mutations:
            ime_mutations(engine,work);return 0
        if args.runtime_mutations:
            runtime_mutations(engine,work);return 0
        if args.startup_mutations:
            startup_mutations(engine,work);return 0
        if args.mutations:
            mutations(engine,work);return 0
        print('P0 INCOMPLETE: inherited socketpair only; production listener/manager/auth/writer and complete GUI resource accounting absent')
        return 0 if args.case else 3
    finally:
        for path,digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest()!=digest:raise RuntimeError(f'repository source changed: {path}')

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,subprocess.SubprocessError,InterruptedError) as error:
        print(f'process probe failed: {error}',file=sys.stderr);sys.exit(1)
