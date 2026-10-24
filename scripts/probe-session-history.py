#!/usr/bin/env python3
"""P0 automatic history-pruning evidence. Not production session metadata/UI."""
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
PATCHES=SURFACE['PATCHES']
SOURCE=ROOT/'terminal/session-state/history-pruned-probe.c'
MARK='GT_HISTORY_PRUNED_MUTATION_MARK'
CASES=('bytes','lines','both','no-prune','clear','same-chunk','lower','import','unregister')

class Cancelled(RuntimeError):pass

def interrupted(signum,frame):
    raise Cancelled(f'signal {signum}; restoring isolated history tree')

def run(command,*,cwd=ROOT,log=None,expected=0,timeout=600):
    # Every child group belongs to this verifier; cancelling also reaps compilers.
    with (log.open('w') if log else open(os.devnull,'w')) as stream:
        p=subprocess.Popen(command,cwd=cwd,stdout=stream if log else subprocess.PIPE,
                           stderr=subprocess.STDOUT,text=True,start_new_session=True)
        try:
            output,_=p.communicate(timeout=timeout)
        except BaseException:
            if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
            p.communicate();raise
    if output:sys.stdout.write(output)
    if p.returncode!=expected:
        raise RuntimeError(f'{command[0]}: expected {expected}, got {p.returncode}; log={log}')
    return output or ''

def build_vt(engine,work,zig):
    run([zig,'build','-Demit-lib-vt=true','-Demit-xcframework=false',
         '-Doptimize=ReleaseFast','-p',str(work/'out')],cwd=engine,log=work/'vt-build.log')
    binary=work/'history-pruned-probe'
    run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
         '-I',str(engine/'include'),str(work/SOURCE.name),str(work/'out/lib/libghostty-vt.a'),
         '-o',str(binary)],log=work/'cc-build.log')
    return binary

def build_unit(engine,work,zig):
    # Reuse the upstream VT test module, but expose a compile-only install step.
    # Keeping compilation separate prevents a compiler error counting as test red.
    run([zig,'build','p0-history-unit-build','-Demit-lib-vt=true',
         '-Demit-xcframework=false','-Doptimize=Debug','-Dtest-filter=p0-history-pruned',
         '-p',str(work/'unit-out')],cwd=engine,log=work/'unit-build.log')
    binary=work/'unit-out/bin/history-pruned-unit'
    if not binary.is_file():raise RuntimeError('compiled history unit executable missing')
    return binary

def probe(binary,case,expected=0):
    return run([str(binary),case],expected=expected,timeout=15)

def unit(binary,expected=0):
    output=run([str(binary)],expected=expected,timeout=30)
    # Verify the intended tests actually ran, including on a negative run.
    for name in ('byte-line-reset','policy-clear','clone-isolation'):
        if 'p0-history-pruned '+name not in output:raise RuntimeError('unit selection omitted '+name)
    return output

def mutations(engine,work,zig):
    page=engine/'src/terminal/PageList.zig';capi=engine/'src/terminal/c/terminal.zig'
    specs=(
        ('byte pruning notification omitted',page,
         'if (self.p0_history_pruned) |notify| notify(self.p0_history_context);',
         '// byte notification omitted','bytes','lines',False),
        ('line pruning notification omitted',page,
         'if (pagelist.p0_history_pruned) |notify| notify(pagelist.p0_history_context);',
         '// line notification omitted','lines','bytes',False),
        ('observer registration ignored',capi,'pages.p0_history_pruned = observer;',
         'pages.p0_history_pruned = null;','both','no-prune',False),
        ('reset falsely reports pruning',page,'pub fn reset(self: *PageList) void {',
         'pub fn reset(self: *PageList) void {\n    if (self.p0_history_pruned) |notify| notify(self.p0_history_context);',
         'no-prune','bytes',False),
        ('reset loses observer',page,'pub fn reset(self: *PageList) void {',
         'pub fn reset(self: *PageList) void {\n    self.p0_history_pruned = null; self.p0_history_context = null;',
         'same-chunk','bytes',False),
        ('clone inherits source observer',page,
         '        .p0_page_allocator = self.p0_page_allocator,',
         '        .p0_page_allocator = self.p0_page_allocator,\n        .p0_history_context = self.p0_history_context,\n        .p0_history_pruned = self.p0_history_pruned,',
         'unit','bytes',True),
        ('observer unregister ignored',capi,
         '    pages.p0_history_context = if (observer != null) context else null;\n    pages.p0_history_pruned = observer;',
         '    if (observer != null) { pages.p0_history_context = context; pages.p0_history_pruned = observer; }',
         'unregister','both',False),
        ('restored history lacks page headroom',engine/'src/terminal/snapshot/page.zig',
         'cap.rows = @max(cap.rows, std_cap.rows);',
         '_ = std_cap;cap.rows = exact.rows;',
         'import','bytes',False),
    )
    originals={p:p.read_bytes() for _,p,*_ in specs}
    digests={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK in b.decode() for b in originals.values()):raise RuntimeError('history mutation marker at start')
    for label,path,old,new,target,control,unit_target in specs:
        text=originals[path].decode()
        if text.count(old)!=1:raise RuntimeError(label+': nonunique history mutation anchor')
        try:
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            binary=build_vt(engine,work,zig)
            if unit_target:
                output=unit(build_unit(engine,work,zig),1)
                if 'clone-isolation...expected 0, found ' not in output or 'FAIL (TestExpectedEqual)' not in output:raise RuntimeError('clone mutation failed outside intended test')
            else:probe(binary,target,1)
            probe(binary,control)
            print('PASS history-pruned mutation/'+label+': target red, control green',flush=True)
        finally:
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest()!=digests[path]:raise RuntimeError('history SHA256 rollback mismatch')
    binary=build_vt(engine,work,zig)
    for case in CASES:probe(binary,case)
    unit(build_unit(engine,work,zig))
    print('PASS 8 history-pruned mutations, actual eviction, independent controls and SHA256 rollback',flush=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--mutations',action='store_true');args=parser.parse_args()
    if sys.platform!='darwin':raise RuntimeError('current history evidence requires macOS; no silent skip')
    files=(*PATCHES,SOURCE,Path(__file__).resolve());hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in (*PATCHES,SOURCE)):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    work=ROOT/'tmp/history-pruned';work.mkdir(parents=True,exist_ok=True)
    os.environ['TMPDIR']=str(ROOT/'tmp');os.environ.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    os.environ['ZIG_LOCAL_CACHE_DIR']=str(work/'cache');os.environ['ZIG_GLOBAL_CACHE_DIR']=str(ROOT/'tmp/zig-global')
    zig=os.environ.get('ZIG') or shutil.which('zig') or str(ROOT/'tmp/zig/zig-aarch64-macos-0.16.0/zig')
    if subprocess.check_output([zig,'version'],text=True).strip()!='0.16.0':raise RuntimeError('Zig 0.16.0 required')
    try:
        for name in ('engine','engine.tar'):
            p=work/name
            if p.is_dir():shutil.rmtree(p)
            elif p.exists():p.unlink()
        engine=SURFACE['FULL']['prepare'](work)
        for patch in PATCHES:
            run(['git','-C',str(engine),'apply','--check',str(patch)])
            run(['git','-C',str(engine),'apply',str(patch)])
        (work/SOURCE.name).write_bytes(SOURCE.read_bytes())
        p=engine/'build.zig';text=p.read_text();anchor='        const mod_vt_test_run = b.addRunArtifact(mod_vt_test);'
        if text.count(anchor)!=1:raise RuntimeError('nonunique upstream test build anchor')
        text=text.replace(anchor,'''        const p0_history_install = b.addInstallArtifact(mod_vt_test, .{ .dest_sub_path = "history-pruned-unit" });
        b.step("p0-history-unit-build", "Compile isolated history tests").dependOn(&p0_history_install.step);
'''+anchor);p.write_text(text)
        binary=build_vt(engine,work,zig)
        for case in CASES:probe(binary,case)
        unit(build_unit(engine,work,zig))
        if args.mutations:mutations(engine,work,zig)
        print('PASS history-pruned: 9 C cases and source/clone/reset tests; session metadata/UI not connected',flush=True)
        return 0
    finally:
        for p,digest in hashes.items():
            if hashlib.sha256(p.read_bytes()).digest()!=digest:raise RuntimeError('repository input changed: '+str(p))

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,subprocess.SubprocessError) as error:
        print('history probe failed: '+str(error),file=sys.stderr);sys.exit(1)
