#!/usr/bin/env python3
"""Full Metal relative-resource import evidence; default exits 3 (P0 incomplete)."""
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
FULL = runpy.run_path(str(ROOT / 'scripts/probe-session-surface.py'))
VT = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-hooks.py'))
REL = runpy.run_path(str(ROOT / 'scripts/probe-session-relative-resources.py'))
PATCHDIR = ROOT / 'patches/libghostty/8159838/experimental'
PATCHES = (*REL['PATCHES'], PATCHDIR / '0012-surface-resource-observation.patch')
SOURCE = ROOT / 'terminal/session-state/full-surface'
MARK = 'GT_P0_FULL_RELATIVE_MUTATION_MARK'
CASES = ('static', 'relative', 'chain', 'forward', 'internal', 'import-only')


def build(engine, work):
    FULL['build'](engine, work)
    binary = work / 'surface-relative'
    frameworks = [x for name in FULL['FRAMEWORKS'] for x in ('-framework', name)]
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                 '-I', str(engine / 'include'), str(SOURCE / 'relative-probe.m'),
                 str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
                 '-lc++', *frameworks, '-o', str(binary)])
    return binary


def fixtures(engine, work):
    VT['build'](engine, work, True)
    binary = work / 'relative-fixtures'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                 '-DGHOSTERM_P0_BUILTIN_PNG', '-Wall', '-Wextra', '-Werror',
                 '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'),
                 str(SOURCE / 'relative-fixture.c'), str(work / 'out/lib/libghostty-vt.a'), '-o', str(binary)])
    FULL['run']([str(binary), str(work)])


def mutations(engine, work):
    variants = (
        ('relative offset lost', 'src/terminal/p0_resource_snapshot.zig',
         '.horizontal_offset = @bitCast(try u32get(&reader)),',
         '.horizontal_offset = zero: { _ = try u32get(&reader); break :zero 0; },', 'relative', 'static'),
        ('parent namespace lost', 'src/terminal/p0_resource_snapshot.zig',
         '.parent = .{ .image_id = x, .placement_id = .{ .tag = if (parent_tag == 0) .internal else .external, .id = y } },',
         '.parent = .{ .image_id = x, .placement_id = .{ .tag = .external, .id = y } },', 'internal', 'static'),
        ('GUI speculatively resizes replica', 'src/termio/Termio.zig',
         'if (self.replica_stream == null) try self.terminal.resize(',
         'try self.terminal.resize(', 'relative', 'import-only'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path
        original = source.read_bytes()
        text = original.decode()
        digest = hashlib.sha256(original).digest()
        if MARK in text:
            raise RuntimeError('residual full relative mutation marker')
        if text.count(old) != 1:
            raise RuntimeError(f'nonunique full relative anchor: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            binary = build(engine, work)
            FULL['probe'](binary, work, target, 1)
            FULL['probe'](binary, work, control)
            print(f'PASS mutation/{label}: full surface target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError('full relative rollback mismatch')
        binary = build(engine, work)
        FULL['probe'](binary, work, target)
    # A wrong deletion must fail even when the imported initial resource graph matches.
    delta = work / 'relative.delta'
    original = delta.read_bytes()
    digest = hashlib.sha256(original).digest()
    try:
        if original.count(b'p=9,') != 1:
            raise RuntimeError('nonunique parent deletion anchor')
        delta.write_bytes(original.replace(b'p=9,', b'p=99,', 1))
        FULL['probe'](binary, work, 'relative', 1)
        FULL['probe'](binary, work, 'static')
        print('PASS mutation/parent deletion lost: target red, control green', flush=True)
    finally:
        delta.write_bytes(original)
        if hashlib.sha256(delta.read_bytes()).digest() != digest:
            raise RuntimeError('relative deletion rollback mismatch')
    FULL['probe'](binary, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--case', choices=CASES)
    args = parser.parse_args()
    if args.mutations and args.case:
        parser.error('--case and --mutations are mutually exclusive')
    if sys.platform != 'darwin' or os.uname().machine != 'arm64':
        raise RuntimeError('full Metal relative prototype requires native Apple Silicon macOS')
    files = (*PATCHES, SOURCE / 'relative-fixture.c', SOURCE / 'relative-probe.m',
             SOURCE / 'probe.m', ROOT / 'terminal/session-state/relative-resource-probe.c')
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):
        return 2
    (ROOT / 'tmp').mkdir(exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-full-relative-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp)
            engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            fixtures(engine, work)
            binary = build(engine, work)
            FULL['probe'](binary, work, args.case or 'all')
            if args.mutations:
                mutations(engine, work)
                print('PASS 4 full relative mutations and rollback; P0 remains incomplete')
                return 0
        print('P0 INCOMPLETE: loading/animation, checkpoint binding, budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'full relative probe failed: {error}; see tmp/session-surface-build.log', file=sys.stderr)
        sys.exit(1)
