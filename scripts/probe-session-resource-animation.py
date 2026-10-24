#!/usr/bin/env python3
"""Animation checkpoint, sole clock owner and allocation recovery; P0 incomplete."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import signal
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-loading-validation.py'))
FULL, VT = PREVIOUS['FULL'], PREVIOUS['VT']
PATCH = ROOT / 'patches/libghostty/8159838/experimental/0020-authority-animation-checkpoint.patch'
PATCHES = (*PREVIOUS['PATCHES'], PATCH)
SOURCE = ROOT / 'terminal/session-state/animation-resource-probe.c'
MARK = 'GT_P0_RESOURCE_ANIMATION_MUTATION_MARK'
CASES = ('running', 'stopped', 'loading', 'finite', 'gapless', 'root-only', 'unplaced', 'continuation', 'loop-max', 'oom', 'static',
         'invalid-count', 'invalid-state', 'invalid-index', 'invalid-frame-size', 'invalid-clock', 'invalid-loop-wide')


def build(engine, work):
    VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-animation-resource-build.log')
    binary = work / 'animation-resource'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'), str(SOURCE),
                 str(work / 'out/lib/libghostty-vt.a'), *(['-lutil'] if sys.platform.startswith('linux') else []), '-o', str(binary)])
    return binary


def probe(binary, mode, expected=0):
    result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=25)
    sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
    if result.returncode != expected: raise RuntimeError(f'animation/{mode}: expected {expected}, got {result.returncode}')


def mutations(engine, work):
    module = 'src/terminal/p0_animation_snapshot.zig'
    variants = (
        ('clock does not advance', 'src/terminal/p0_animation_tick.zig',
         'const delay = t.screens.active.kitty_images.animationTick(t.io(), now_ms);', 'const delay: ?u64 = null;', 'running', 'stopped'),
        ('clock moves backward', 'src/terminal/p0_animation_tick.zig',
         'if (now_ms < previous) return error.InvalidClock;', 'if (now_ms < previous and false) return error.InvalidClock;', 'running', 'static'),
        ('animation omitted from import', module, 'return anim;', 'anim.deinit(alloc); alloc.destroy(anim); return null;', 'running', 'static'),
        ('current frame lost', module, '.current_index = try word(r)',
         '.current_index = current: { _ = try word(r); break :current 0; }', 'stopped', 'running'),
        ('frame pixels corrupted', module, 'const data = try alloc.dupe(u8, f.bytes);',
         'const data = try alloc.dupe(u8, f.bytes); @memset(data, 0);', 'running', 'root-only'),
        ('frame quota omitted', 'src/terminal/kitty/graphics_storage.zig',
         'const new_len = img.storageSize();', 'const new_len = img.data.len();', 'running', 'root-only'),
        ('partial frames leaked', module, 'anim.deinit(alloc);\n        alloc.destroy(anim);',
         'alloc.destroy(anim);', 'oom', 'static'),
        ('infinite loop counter wraps', 'src/terminal/kitty/graphics_storage.zig',
         'anim.current_loop +|= 1;', 'anim.current_loop += 1;', 'loop-max', 'running'),
        ('frame count unbounded', module, 'if (count > max_frames) return error.LimitExceeded;',
         'if (false) return error.LimitExceeded;', 'invalid-count', 'invalid-state'),
        ('invalid playback state accepted', module,
         'std.enums.fromInt(Animation.State, try word(r)) orelse return error.InvalidData',
         'std.enums.fromInt(Animation.State, try word(r)) orelse .stopped', 'invalid-state', 'invalid-count'),
        ('invalid frame shape accepted', module, 'if (len != rgba_len) return error.InvalidData;',
         '_ = rgba_len;', 'invalid-frame-size', 'static'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique animation mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            probe(binary, target, 1); probe(binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('animation rollback mismatch')
        binary = build(engine, work); probe(binary, target)
    for mode in CASES: probe(binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (*PATCHES, SOURCE, *(ROOT / 'terminal/session-state' / name for name in
             ('resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-animation-resource-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            binary = build(engine, work)
            for mode in (args.case,) if args.case else CASES: probe(binary, mode)
            if args.mutations:
                mutations(engine, work); print('PASS 11 animation mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: native clock gate, frame loading/pending, atomic capture, total budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'animation probe failed: {error}; see tmp/session-animation-resource-build.log', file=sys.stderr); sys.exit(1)
