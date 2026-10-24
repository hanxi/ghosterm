#!/usr/bin/env python3
"""In-progress animation frames and generation rebinding; P0 remains incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-animation.py'))
FULL, VT = PREVIOUS['FULL'], PREVIOUS['VT']
PATCH = ROOT / 'patches/libghostty/8159838/experimental/0021-frame-loading-checkpoint.patch'
PATCHES = (*PREVIOUS['PATCHES'], PATCH)
SOURCE = ROOT / 'terminal/session-state/frame-loading-resource-probe.c'
MARK = 'GT_P0_FRAME_LOADING_MUTATION_MARK'
CASES = ('append', 'number', 'edit', 'stale', 'tick-after', 'missing', 'blend', 'overwrite', 'background', 'png', 'zlib', 'quiet', 'continuation', 'negative-gap', 'static', 'oom',
         'bad-frame-presence', 'bad-match', 'bad-reference', 'bad-format', 'bad-medium', 'bad-both', 'bad-none', 'bad-composition', 'bad-wide', 'bad-more', 'bad-usage', 'bad-number')


def build(engine, work):
    VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-frame-loading-build.log')
    binary = work / 'frame-loading'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'), str(SOURCE),
                 str(work / 'out/lib/libghostty-vt.a'), *(['-lutil'] if sys.platform.startswith('linux') else []), '-o', str(binary)])
    return binary


def mutations(engine, work):
    loading = 'src/terminal/p0_loading_snapshot.zig'; frame = 'src/terminal/p0_frame_loading_snapshot.zig'
    variants = (
        ('valid generation not rebound', loading, 'loading.frame.?.image_generation = image.generation;',
         'loading.frame.?.image_generation = image.generation - image.generation;', 'append', 'stale'),
        ('stale generation revived', loading, 'if (loading.frame != null and view.matched) {\n        const target',
         'if (loading.frame != null) {\n        const target', 'stale', 'append'),
        ('source reference declared stale', loading, 'context.image_generation == image.generation else false',
         'context.image_generation == image.generation and false else false', 'append', 'missing'),
        ('number reference rejects zero ID', loading,
         'if (loading.image.id == 0 and (loading.frame == null or loading.image.number == 0)) return error.InvalidData;',
         'if (loading.image.id == 0) return error.InvalidData;', 'number', 'append'),
        ('background command lost', frame, 'f.background = @bitCast(try word(r));', '_ = try word(r);', 'background', 'append'),
        ('composition command lost', frame, 'f.composition_mode = try enumeration(command.CompositionMode, r);',
         '_ = try enumeration(command.CompositionMode, r);', 'overwrite', 'blend'),
        ('edit command lost', frame, 'inline for (frame_words) |field| @field(f, field) = try word(r);',
         'inline for (frame_words) |field| @field(f, field) = try word(r); f.edit_frame = 0;', 'edit', 'append'),
        ('quiet flag lost', loading, 'loading.quiet = try enumeration(command.Command.Quiet, r);',
         '_ = try enumeration(command.Command.Quiet, r);', 'quiet', 'append'),
        ('frame payload lost', loading, 'try loading.data.appendSlice(alloc, view.bytes);',
         'try loading.data.appendSlice(alloc, view.bytes[0..0]);', 'append', 'static'),
        ('matched reference not preflighted', loading, 'if (!found) return error.InvalidData;',
         'if (!found and false) return error.InvalidData;', 'bad-reference', 'bad-match'),
        ('conflicting identifiers accepted', frame,
         'if (f.transmission.image_id != 0 and f.transmission.image_number != 0) return error.InvalidData;',
         'if (false) return error.InvalidData;', 'bad-both', 'bad-none'),
        ('missing identifiers accepted', frame,
         'if (f.transmission.image_id == 0 and f.transmission.image_number == 0) return error.InvalidData;',
         'if (false) return error.InvalidData;', 'bad-none', 'bad-both'),
        ('invalid boolean accepted', frame, 'else => error.InvalidData,', 'else => false,', 'bad-more', 'bad-format'),
        ('partial loader leaked', loading, 'errdefer loading.destroy(alloc);', '// loader not released', 'oom', 'static'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique frame mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            PREVIOUS['probe'](binary, target, 1); PREVIOUS['probe'](binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('frame loading rollback mismatch')
        binary = build(engine, work); PREVIOUS['probe'](binary, target)
    for mode in CASES: PREVIOUS['probe'](binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (*PATCHES, SOURCE, *(ROOT / 'terminal/session-state' / name for name in ('resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-frame-loading-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            binary = build(engine, work)
            for mode in (args.case,) if args.case else CASES: PREVIOUS['probe'](binary, mode)
            if args.mutations:
                mutations(engine, work); print('PASS 14 frame loading mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: native frame continuation, async pending, atomic capture, total budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'frame loading probe failed: {error}; see tmp/session-frame-loading-build.log', file=sys.stderr); sys.exit(1)
