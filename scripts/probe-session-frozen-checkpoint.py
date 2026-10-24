#!/usr/bin/env python3
"""Immutable native capture, deferred encoding and source lifetime; P0 incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-scratch-checkpoint.py'))
FULL, VT, PROBE = (PREVIOUS[k] for k in ('FULL', 'VT', 'PROBE'))
PATCHES = (*PREVIOUS['PATCHES'], ROOT / 'patches/libghostty/8159838/experimental/0026-immutable-checkpoint-capture.patch')
SOURCE = ROOT / 'terminal/session-state/frozen-checkpoint-probe.c'
MARK = 'GT_P0_FROZEN_CAPTURE_MUTATION_MARK'
CASES = ('ground', 'continuation', 'utf8', 'rich', 'dual', 'resources', 'loading', 'compressed', 'pair-oom', 'bulk',
         'capture-limit', 'cancel', 'wire-limit', 'freeze-oom', 'encode-oom', 'compressed-freeze-oom', 'compressed-encode-oom')


def build(engine, work):
    VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-frozen-checkpoint-build.log')
    binary = work / 'frozen-checkpoint'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'),
                 str(SOURCE), str(ROOT / 'terminal/session-state/budget-posix.c'), str(work / 'out/lib/libghostty-vt.a'),
                 *(['-lutil'] if sys.platform.startswith('linux') else []), '-lpthread', '-o', str(binary)])
    return binary


def mutations(engine, work):
    source = engine / 'src/terminal/p0_frozen_checkpoint.zig'
    variants = (
        ('resident bytes not frozen', 'const copy = page.cloneBuf(memory);', 'const copy = page.cloneBuf(memory); @memset(copy.memory, 0);', 'ground', 'capture-limit'),
        ('compressed bytes not frozen', 'const encoded = try self.alloc.dupe(u8, page.encoded);', 'const encoded = try self.alloc.dupe(u8, page.encoded); @memset(encoded, 0);', 'compressed', 'ground'),
        ('continuation omitted', 'codec.continuation.encode(continuation, &stream)', 'codec.continuation.encode(.ground, &stream)', 'continuation', 'ground'),
        ('terminal metadata omitted', 'try codec.terminal.encode(t, &stream);', '// terminal metadata omitted', 'rich', 'capture-limit'),
        ('capture copy limit bypassed', 'if (n > self.limit - self.copied) return error.LimitExceeded;', 'if (n > self.limit - self.copied and false) return error.LimitExceeded;', 'capture-limit', 'ground'),
        ('resource CRC not finalized', 'resources_codec.finalize(resources);', '// resource CRC omitted', 'resources', 'capture-limit'),
        ('combined wire limit bypassed', 'if (self.resources.len > max_wire - bytes.written().len) return error.LimitExceeded;', 'if (self.resources.len > max_wire - bytes.written().len and false) return error.LimitExceeded;', 'wire-limit', 'ground'),
        ('resident capture leaked', '.resident => |page| alloc.free(page.memory),', '.resident => |page| { _ = page; },', 'cancel', 'capture-limit'),
        ('compressed capture leaked', '.compressed => |page| alloc.free(page.encoded),', '.compressed => |page| { _ = page; },', 'compressed-freeze-oom', 'ground'),
        ('alternate screen omitted', 'for ([_]Key{ .primary, .alternate }) |key| {', 'for ([_]Key{ .primary }) |key| {', 'dual', 'ground'),
        ('alternate history omitted', 'for ([_]Key{ .primary, .alternate }) |key| {', 'for ([_]Key{ .primary }) |key| {', 'dual', 'ground'),
    )
    for label, old, new, target, control in variants:
        original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != (2 if label.startswith('alternate ') else 1): raise RuntimeError(f'nonunique frozen mutation: {label}')
        try:
            patched = (new.join(text.rsplit(old, 1)) if label == 'alternate history omitted' else text.replace(old, new, 1))
            source.write_text(patched + f'\n// {MARK}\n'); binary = build(engine, work)
            PROBE(binary, target, 1); PROBE(binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('frozen capture rollback mismatch')
        binary = build(engine, work); PROBE(binary, target)
    for mode in CASES: PROBE(binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (*PATCHES, SOURCE, *(ROOT / 'terminal/session-state' / name for name in
             ('budget-posix.c', 'budget-posix.h', 'checkpoint-budget.h', 'resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-frozen-checkpoint-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            binary = build(engine, work)
            for mode in (args.case,) if args.case else CASES: PROBE(binary, mode)
            if args.mutations:
                mutations(engine, work); print('PASS 11 frozen capture mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: session event boundary, GUI budgets, production worker/IPC and performance calibration remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'frozen checkpoint probe failed: {error}; see tmp/session-frozen-checkpoint-build.log', file=sys.stderr); sys.exit(1)
