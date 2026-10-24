#!/usr/bin/env python3
"""Capture chunked Kitty image state; native full surface and P0 remain pending."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-surface-clear.py'))
FULL, VT = PREVIOUS['FULL'], PREVIOUS['VT']
PATCH = ROOT / 'patches/libghostty/8159838/experimental/0018-loading-resource-snapshot.patch'
PATCHES = (*PREVIOUS['PATCHES'], PATCH)
SOURCE = ROOT / 'terminal/session-state/loading-resource-probe.c'
MARK = 'GT_P0_RESOURCE_LOADING_MUTATION_MARK'
CASES = ('direct', 'no-display', 'quiet', 'zlib', 'png', 'continuation', 'screens', 'implicit', 'frame-loading', 'limit', 'invalid', 'oom', 'static', 'animation')


def build(engine, work):
    VT['build'](engine, work, True)
    binary = work / 'loading-resource'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                 '-DGHOSTERM_P0_BUILTIN_PNG', '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'),
                 '-I', str(ROOT / 'terminal/session-state'), str(SOURCE),
                 str(work / 'out/lib/libghostty-vt.a'), *(['-lutil'] if sys.platform.startswith('linux') else []), '-o', str(binary)])
    return binary


def probe(binary, mode, expected=0):
    result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=20)
    sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f'loading/{mode}: expected exit {expected}, got {result.returncode}')


def mutations(engine, work):
    module = 'src/terminal/p0_loading_snapshot.zig'
    variants = (
        ('loading state omitted', module, 'return loading;', 'loading.destroy(alloc); return null;', 'direct', 'static'),
        ('loading bytes omitted', module, 'try loading.data.appendSlice(alloc, view.bytes);',
         'try loading.data.appendSlice(alloc, view.bytes[0..0]);', 'direct', 'static'),
        ('display command lost', module, 'loading.display = display;', 'loading.display = if (display.image_id == 0) display else null;', 'direct', 'no-display'),
        ('implicit identity lost', module, 'loading.image.metadata.implicit_id = flags & 2 != 0;', 'loading.image.metadata.implicit_id = false;', 'implicit', 'quiet'),
        ('frame loading silently flattened', module, 'loading.frame != null or ', '', 'frame-loading', 'static'),
        ('quiet flag lost', module, 'loading.quiet = try enumeration(command.Command.Quiet, r);',
         '_ = try enumeration(command.Command.Quiet, r);', 'quiet', 'direct'),
        ('response identity lost', module, 'loading.response.id = try word(r);', '_ = try word(r);', 'direct', 'static'),
        ('compression flag lost', module, 'loading.image.compression = try enumeration(command.Transmission.Compression, r);',
         '_ = try enumeration(command.Transmission.Compression, r);', 'zlib', 'direct'),
        ('invalid enum accepted', module, 'std.enums.fromInt(T, try word(r)) orelse error.InvalidData',
         'std.enums.fromInt(T, try word(r)) orelse @enumFromInt(0)', 'invalid', 'static'),
        ('partial allocation not released', module, 'errdefer loading.destroy(alloc);',
         '// missing cleanup', 'oom', 'static'),
        ('wire budget bypassed', 'src/terminal/p0_resource_snapshot.zig',
         'if (data.len > max_bytes) return error.LimitExceeded;', 'if (false) return error.LimitExceeded;', 'limit', 'static'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path
        original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique loading mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            binary = build(engine, work); probe(binary, target, 1); probe(binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('loading rollback mismatch')
        binary = build(engine, work); probe(binary, target)
    for mode in CASES: probe(binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (*PATCHES, SOURCE, *(ROOT / 'terminal/session-state' / name for name in
             ('resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-resource-loading-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            binary = build(engine, work)
            for mode in (args.case,) if args.case else CASES: probe(binary, mode)
            if args.mutations:
                mutations(engine, work); print('PASS 11 loading resource mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: native surface, frame loading/animation/pending, atomic capture, budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'loading resource probe failed: {error}; see tmp/session-resource-hooks-build.log', file=sys.stderr); sys.exit(1)
