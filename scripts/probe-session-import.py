#!/usr/bin/env python3
"""Native empty replica -> IO-thread checkpoint import and LIVE input barrier.
This proves one engine seam, not full P0 or the native product entry point.
"""
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
SUITE=runpy.run_path(str(ROOT/'scripts/probe-session-worker-surface.py'))
FULL,VT=SUITE['FULL'],SUITE['VT']
WORK=ROOT/'tmp/surface-import-probe'
ENGINE_WORK=ROOT/'tmp/worker-surface'
SOURCE=ROOT/'terminal/session-state/full-surface'
PATCH=ROOT/'patches/libghostty/8159838/experimental/0044-empty-replica-io-import.patch'
MARK='GT_SURFACE_IMPORT_MUTATION_MARK'
CASES=('empty','plain','utf8','image','bad-canonical','bad-resources','raw','budget','deny-file','config','legacy')
ENGINE_FILES=('include/ghostty.h','src/apprt/embedded.zig','src/Surface.zig','src/termio/Options.zig','src/termio/Termio.zig','src/config/CApi.zig')

def managed(command,**kwargs):
    timeout=kwargs.pop('timeout',600);check=kwargs.pop('check',False)
    if kwargs.pop('capture_output',False):kwargs['stdout']=subprocess.PIPE;kwargs['stderr']=subprocess.PIPE
    p=subprocess.Popen([str(x) for x in command],start_new_session=True,**kwargs)
    try:stdout,stderr=p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
        p.wait();raise
    result=subprocess.CompletedProcess(command,p.returncode,stdout,stderr)
    if check:result.check_returncode()
    return result

def checked(command,**kwargs):return managed(command,check=True,**kwargs)

def compile_probe(engine):
    flags=['-Wall','-Wextra','-Werror','-I'+str(engine/'include'),'-I'+str(ROOT/'terminal/session-state'),'-I'+str(SOURCE)]
    cc=shlex.split(os.environ.get('CC','cc'))
    checked([*cc,'-std=c11','-D_POSIX_C_SOURCE=200809L',*flags,'-c',WORK/'import-fixture.c','-o',WORK/'fixture.o'])
    frameworks=[a for name in FULL['FRAMEWORKS'] for a in ('-framework',name)]
    checked([*cc,'-fobjc-arc',*flags,WORK/'import-probe.m',WORK/'fixture.o',ROOT/'terminal/session-state/budget-posix.c',
        ENGINE_WORK/'out/lib/libghostty-vt.dylib',engine/'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a',
        '-Wl,-rpath,'+str(ENGINE_WORK/'out/lib'),'-lc++','-pthread',*frameworks,'-o',WORK/'probe'])

def probe(case,red=False):
    result=managed([WORK/'probe',case],cwd=ROOT,text=True,capture_output=True,timeout=25)
    (WORK/('case-'+case+'.log')).write_text(result.stdout+result.stderr)
    if red:
        if result.returncode!=1 or 'FAIL ' not in result.stderr:raise RuntimeError('ordinary FAIL/exit 1 required/'+case+': '+result.stdout+result.stderr)
    elif result.returncode:raise RuntimeError('native baseline/control failed/'+case+': '+result.stdout+result.stderr)
    if not red:print(result.stdout,end='',flush=True)

def full_build(engine,label):
    FULL['build'](engine,ENGINE_WORK,cache_dir=ENGINE_WORK/'full-cache',log_path=ROOT/('tmp/import-'+label+'-build.log'),process_run=managed)

def mutations(engine):
    paths={n:engine/n for n in ENGINE_FILES};paths['driver']=WORK/'import-probe.m'
    originals={n:p.read_bytes() for n,p in paths.items()};digests={n:hashlib.sha256(v).digest() for n,v in originals.items()}
    termio='src/termio/Termio.zig'
    specs=[
      ('continuation replay omitted',termio,'const continuation: []const u8 = switch (decoded.continuation) { .ground => &.{}, .bytes => |b| b };','const continuation: []const u8 = &.{};','plain'),
      ('resource graph not imported',termio,'try @import("../terminal/p0_resource_snapshot.zig").decodeWithLimit(self.alloc, &decoded.terminal.?, resources, 16 * 1024 * 1024, limit);','_ = resources;','image'),
      ('duplicate import falsely acknowledged',termio,'if (!self.replica_dynamic or self.replica_import_attempted or generation == 0 or self.replica_generation != 0','if (self.replica_import_attempted) { self.renderer_state.mutex.unlock(global.io()); return true; }\n    if (!self.replica_dynamic or self.replica_import_attempted or generation == 0 or self.replica_generation != 0','plain'),
      ('empty placeholder acknowledges application',termio,'if (self.replica_dynamic and self.replica_generation == 0) return false;','if (false and self.replica_dynamic and self.replica_generation == 0) return false;','plain'),
      ('import opens input before LIVE',termio,'self.replica_input_enabled = false;','self.replica_input_enabled = true;','plain'),
      ('foreign generation enables input',termio,'if (!self.replica_dynamic or generation == 0 or generation != self.replica_generation) return false;','if (!self.replica_dynamic or generation == 0) return false;','plain'),
      ('stale input poisons current generation',termio,' or stamp.generation != self.replica_generation))','))','plain'),
      ('import loses its resource guard',termio,'entry.value.*.kitty_images.image_limits.resource_hooks = &self.replica_resources;','entry.value.*.kitty_images.image_limits.resource_hooks = null;','deny-file'),
      ('ordinary input blanket blocked','src/termio/Termio.zig','.blocked = self.replica_scheme != null or (self.replica_dynamic and !self.replica_input_enabled),','.blocked = self.replica_scheme != null or (self.replica_dynamic and !self.replica_input_enabled) or !self.replica_dynamic,','legacy'),
      ('replica config ignored','src/apprt/embedded.zig','opts.p0_replica_config orelse &app.config','&app.config','config'),
      ('replica palette ignored','src/config/CApi.zig','self.background = .{ .r = bg.r, .g = bg.g, .b = bg.b };','_ = bg;','config'),
      ('unbound config accepted','src/apprt/embedded.zig','if (opts.p0_replica_config != null and !opts.p0_replica_empty) return error.UnboundReplicaConfig;','// unbound config accepted','config'),
      # Mutate only the expected bytes AFTER the engine has consumed the real
      # checkpoint. Never alter a constant shared by both sides of the oracle.
      ('canonical comparison loses its oracle','driver','equal(fixture.initial);','((uint8_t *)fixture.initial.canonical)[0] ^= 1;\n  equal(fixture.initial);','plain'),
    ]
    def restore():
        for n,p in paths.items():
            p.write_bytes(originals[n])
            if hashlib.sha256(p.read_bytes()).digest()!=digests[n]:raise RuntimeError('native import SHA256 rollback failed/'+n)
    verified=False
    try:
        for index,(label,name,old,new,target) in enumerate(specs):
            text=originals[name].decode()
            if old not in text:raise RuntimeError('native import mutation anchor drift/'+label)
            print('BUILD native import mutation/'+label,flush=True)
            try:
                paths[name].write_text(text.replace(old,new)+'\n// '+MARK+'\n')
                if name!='driver':full_build(engine,'mutation-'+str(index))
                compile_probe(engine);probe(target,True);probe('empty')
                print('PASS native import mutation/'+label+': ordinary red, same-mutant empty surface green',flush=True)
            finally:restore()
        full_build(engine,'rollback');compile_probe(engine)
        for case in CASES:probe(case)
        print('PASS 13 native import mutations, 11 baselines, actual Metal pixels and SHA256 rollback',flush=True)
        verified=True
    finally:
        restore()
        if not verified:
            # A failed/aborted variant is not a reusable SDK or test artifact.
            (WORK/'probe').unlink(missing_ok=True)
            (engine/'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a').unlink(missing_ok=True)

def interrupted(sig,frame):
    raise RuntimeError('native import probe interrupted: '+str(sig))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--mutations',action='store_true');p.add_argument('--case',choices=CASES);args=p.parse_args()
    if args.case and args.mutations:p.error('choose --case or --mutations')
    if sys.platform!='darwin' or os.uname().machine!='arm64':raise RuntimeError('Apple Silicon macOS required')
    (ROOT/'tmp').mkdir(exist_ok=True);WORK.mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    if not os.environ.get('DEVELOPER_DIR'):os.environ['DEVELOPER_DIR']='/Applications/Xcode.app/Contents/Developer'
    files=(*SUITE['PATCHES'],*(SOURCE/n for n in ('import-fixture.c','import-fixture.h','import-probe.m')),ROOT/'terminal/session-state/budget-posix.c',ROOT/'terminal/session-state/budget-posix.h')
    originals={f:hashlib.sha256(f.read_bytes()).digest() for f in files}
    engine=ENGINE_WORK/'engine'
    markers=[*files,WORK/'import-probe.m',*(engine/n for n in ENGINE_FILES)]
    if any(x.exists() and MARK in x.read_text(errors='replace') for x in markers):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    # Existing helpers also launch build/compile tools; give them the same
    # process-group cleanup rather than leaving compiler descendants behind.
    full_globals=FULL['prepare'].__globals__;old_full=full_globals['run'];full_globals['run']=checked
    vt_globals=VT['build'].__globals__;old_vt=vt_globals['run'];vt_globals['run']=checked
    try:
        checked(['git','-C',ROOT/'vendor/ghostty','diff','--quiet','HEAD','--'])
        pin=managed(['git','-C',ROOT/'vendor/ghostty','rev-parse','HEAD'],text=True,capture_output=True,check=True).stdout.strip()
        if pin!='81598385d608dda4946093d6d6a234d204a2f225':raise RuntimeError('engine pin mismatch')
        for stale in ('engine','engine.tar'):
            path=ENGINE_WORK/stale
            if path.is_dir():shutil.rmtree(path)
            elif path.exists():path.unlink()
        engine=FULL['prepare'](ENGINE_WORK)
        for patch in SUITE['PATCHES']:
            checked(['git','-C',engine,'apply','--check',patch]);checked(['git','-C',engine,'apply',patch])
        VT['build'](engine,ENGINE_WORK,True,log_path=ROOT/'tmp/import-vt-build.log')
        full_build(engine,'baseline')
        for name in ('import-fixture.c','import-probe.m'):(WORK/name).write_bytes((SOURCE/name).read_bytes())
        compile_probe(engine)
        for case in (args.case,) if args.case else CASES:probe(case)
        if args.mutations:mutations(engine)
        print('Native IO import seam passed; full P0 and native product entry remain in progress',flush=True)
    finally:
        full_globals['run']=old_full;vt_globals['run']=old_vt
        for f,digest in originals.items():
            if hashlib.sha256(f.read_bytes()).digest()!=digest:raise RuntimeError('repository source changed during import probe/'+str(f))
    return 0
if __name__=='__main__':sys.exit(main())
