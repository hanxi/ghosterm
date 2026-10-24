#!/usr/bin/env python3
"""Real PTY worker/libVT frozen boundary and shared budget; P0 incomplete."""
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
ROOT = Path(__file__).resolve().parents[1]
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-frozen-checkpoint.py'))
FULL, VT = (PREVIOUS[k] for k in ('FULL', 'VT'))
PATCHES = (*PREVIOUS['PATCHES'], ROOT / 'patches/libghostty/8159838/experimental/0028-restore-page-capacity.patch')
SOURCE = ROOT / 'daemon/probes/session-vt-owner.c'
ADAPTER = ROOT / 'terminal/session-state/frozen-budget.c'
MARK = 'GT_WORKER_VT_MUTATION_MARK'
CASES = ('continuation', 'utf8', 'resize', 'resources', 'roles', 'query', 'cancel', 'overflow', 'idle', 'query-delta', 'query-caps', 'isolation', 'clipboard', 'input',
         'tui-less', 'tui-vim')


def compile_probe(engine, work, source, adapter):
    binary = work / 'worker-vt'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
        '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'),
        '-I', str(ROOT / 'terminal/session-state'), '-I', str(ROOT / 'core/src'),
        '-I', str(ROOT / 'core/include'), str(source), str(adapter),
        str(ROOT / 'terminal/session-state/checkpoint-budget.c'),
        str(ROOT / 'terminal/session-state/budget-posix.c'),
        str(ROOT / 'core/build/libghosterm_core.a'), str(work / 'out/lib/libghostty-vt.a'),
        *(['-lutil'] if sys.platform.startswith('linux') else []), '-lpthread', '-o', str(binary)])
    return binary


def probe(binary, case, expected=0):
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=20)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f'{case}: expected {expected}, got {result.returncode}')


def mutations(engine, work, source, adapter):
    variants = (
        ('authority events not queued', source,
         'gt_restore_rc_t rc=gt__attachment_push(f->attachment,sequence,event->kind,event->payload,event->length);',
         '(void)sequence;(void)event;gt_restore_rc_t rc=GT_RESTORE_IDLE;', 'continuation', 'idle'),
        ('replica resize skipped', source,
         'CHECK(ghostty_terminal_resize(replica,size.cols,size.rows,0,0)==GHOSTTY_SUCCESS);',
         '(void)size;', 'continuation', 'idle'),
        ('second READY omitted', source,
         'CHECK(gt__attachment_ready_live(f->attachment,generation,cut)==GT_RESTORE_OK);',
         '/* live acknowledgment omitted */', 'idle', 'roles'),
        ('detach not submitted', source, 'CHECK(gt__attachment_detach(attachment,generation)==GT_TASK_OK);',
         '/* cancellation omitted */', 'cancel', 'utf8'),
        ('query reply dropped', source, 'f->reply_status = f->sink.write(f->sink.context,bytes,length);',
         '(void)bytes; f->reply_status = GT_OK;', 'query', 'utf8'),
        ('query reply duplicated', source, 'f->reply_status = f->sink.write(f->sink.context,bytes,length);',
         'f->reply_status = f->sink.write(f->sink.context,bytes,length);\n'
         '    if(f->reply_status == GT_OK) f->reply_status = f->sink.write(f->sink.context,bytes,length);', 'query', 'utf8'),
        ('resource pixel wrong', source, r'/wAA/w==\\033', r'AAD//w==\\033', 'resources', 'utf8'),
        ('capture role alias', adapter, 'source == task', 'false', 'roles', 'utf8'),
        ('capture foreign ledger', adapter, 'source->budget != task->budget', 'false', 'roles', 'utf8'),
        ('capture admission', adapter, '!source->reserved || !task->reserved', 'false', 'roles', 'utf8'),
        ('capture quota error', adapter, 'rc = p0_budget_result(task, rc);', '/* quota result discarded */', 'roles', 'utf8'),
        ('encode wire cap', adapter, 'frozen->handle, staged.allocator, limit,',
         'frozen->handle, staged.allocator, (limit | (16u*1024u*1024u)),', 'roles', 'utf8'),
        ('source feed bypass', source, 'rc = p0_budget_feed(&f->owner,f->terminal,event->payload,event->length);',
         'rc = GHOSTTY_SUCCESS;', 'continuation', 'roles'),
        ('state resize bypass', source, 'rc = p0_budget_resize(&f->owner,f->terminal,size->cols,size->rows);',
         '(void)size; rc = GHOSTTY_SUCCESS;', 'resize', 'idle'),
        # -X keeps less on the primary screen: the TUI cases must fail when the
        # program never leaves it, otherwise they prove nothing beyond the
        # plain-output modes.
        ('alternate screen never entered', source, 'exec /usr/bin/less ', 'exec /usr/bin/less -X ',
         'tui-less', 'continuation'),
        # 回调返回 false 就是「静默不答」，引擎不会替宿主编一个 DA1 出来，
        # 复用器会停在那里一直等——这条路径必须让 query-caps 红。
        ('device attributes not answered', source,
         'out->secondary.firmware_version=10;\n    return true;',
         'out->secondary.firmware_version=10;\n    return false;', 'query-caps', 'query'),
        # 答了但答错同样不行：用例逐字校验，固件号改一位就该红。
        ('device attributes wrong firmware', source, 'out->secondary.firmware_version=10;',
         'out->secondary.firmware_version=11;', 'query-caps', 'query'),
        # 配色归宿主：回调不答，引擎不会替它编一个 DSR 出来，程序会一直等。
        ('color scheme not answered', source, 'f->scheme_calls++;*out=GHOSTTY_COLOR_SCHEME_DARK;return true;',
         'f->scheme_calls++;*out=GHOSTTY_COLOR_SCHEME_DARK;return false;', 'isolation', 'query'),
        # 答的是哪一种也归宿主：改成 light，用例逐字校验就该红。
        ('color scheme reported light', source, 'f->scheme_calls++;*out=GHOSTTY_COLOR_SCHEME_DARK;return true;',
         'f->scheme_calls++;*out=GHOSTTY_COLOR_SCHEME_LIGHT;return true;', 'isolation', 'query'),
        # 剪贴板读的内容由宿主交回，引擎只负责编码：换一个同样长度的原文，
        # 答复仍是 17 字节但逐字对不上，clipboard 必须红。
        ('clipboard read answered with other content', source, 'mime[]="text/plain",text[]="PASTE";',
         'mime[]="text/plain",text[]="PASTEX";', 'clipboard', 'query'),
    )
    for label, path, old, new, target, control in variants:
        original = path.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique mutation: {label}')
        try:
            path.write_text(text.replace(old, new) + f'\n/* {MARK} */\n')
            binary = compile_probe(engine, work, source, adapter)
            probe(binary, target, 1); probe(binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError('rollback SHA mismatch')
    binary = compile_probe(engine, work, source, adapter)
    for case in CASES: probe(binary, case)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (SOURCE, ADAPTER, ROOT / 'terminal/session-state/frozen-budget.h', *PATCHES,
             *(ROOT / 'core/src/session' / name for name in ('worker.c', 'worker.h', 'state-owner.c', 'state-owner.h', 'checkpoint-task.c', 'checkpoint-task.h', 'attachment.c', 'attachment.h', 'restore-stream.c', 'restore-stream.h')),
             *(ROOT / 'terminal/session-state' / name for name in ('budget-posix.c', 'budget-posix.h', 'checkpoint-budget.c', 'checkpoint-budget.h')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        FULL['run'](['bash', str(ROOT / 'scripts/build-core.sh')])
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        # Fixed work path and binary names: hosts that gate execution by path
        # kill freshly created executables, including zig's own build runner.
        # Only the sources are wiped; the content-addressed build caches stay,
        # so the engine is still re-extracted from the pin and re-patched.
        work = ROOT / 'tmp/worker-vt'
        for stale in ('engine', 'engine.tar'):
            path = work / stale
            if path.is_dir(): shutil.rmtree(path)
            elif path.exists(): path.unlink()
        work.mkdir(parents=True, exist_ok=True)
        engine = FULL['prepare'](work)
        for patch in PATCHES:
            FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
            FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
        VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-worker-vt-build.log')
        source=work / SOURCE.name; source.write_bytes(SOURCE.read_bytes())
        adapter=work / ADAPTER.name; adapter.write_bytes(ADAPTER.read_bytes())
        binary=compile_probe(engine,work,source,adapter)
        for case in (args.case,) if args.case else CASES: probe(binary,case)
        if args.mutations:
            mutations(engine,work,source,adapter)
            print('PASS 20 worker/libVT mutations and rollback; P0 remains incomplete')
            return 0
        print('P0 INCOMPLETE: production manager/admission, full-surface/IPC restore and GUI recovery remain')
        return 0 if args.case else 3
    finally:
        for path,digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'worker/libVT probe failed: {error}; see tmp/session-worker-vt-build.log', file=sys.stderr)
        sys.exit(1)
