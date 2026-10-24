#!/usr/bin/env python3
"""Bound checkpoint/generation prototype. Default exits 3, P0 incomplete."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import signal
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-surface-events.py'))
FULL = PREVIOUS['FULL']
PATCHES = (*PREVIOUS['PATCHES'], ROOT / 'patches/libghostty/8159838/experimental/0014-bound-checkpoint-generation.patch')
SOURCE = ROOT / 'terminal/session-state/full-surface/checkpoint-probe.m'
MARK = 'GT_P0_BOUND_CHECKPOINT_MUTATION_MARK'
CASES = ('binding', 'metadata', 'sequence', 'generation', 'zero-start', 'legacy')


def build(engine, work):
    FULL['build'](engine, work)
    binary = work / 'surface-checkpoint'
    frameworks = [x for name in FULL['FRAMEWORKS'] for x in ('-framework', name)]
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                 '-I', str(engine / 'include'), str(SOURCE),
                 str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
                 '-lc++', *frameworks, '-o', str(binary)])
    return binary


def crc32c(data):
    crc = 0xffffffff
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc ^ 0xffffffff


def fixtures(engine, work):
    PREVIOUS['fixtures'](engine, work)
    canonical = (work / 'mixed-2.canonical').read_bytes()
    resources = (work / 'mixed-2.resources').read_bytes()
    blue = bytearray(resources)
    if blue[:8] != b'GTIR0002' or struct.unpack_from('<Q', blue, 64)[0] != 1 or blue[136:140] != bytes((255, 0, 0, 255)):
        raise RuntimeError('unexpected authority resource fixture layout')
    blue[136:140] = bytes((0, 0, 255, 255))
    struct.pack_into('<I', blue, 12, crc32c(blue[16:]))
    (work / 'blue.resources').write_bytes(blue)
    for name, generation, sequence, graph in (
        ('checkpoint.digest', 81, 40, resources), ('zero.digest', 81, 0, resources),
        ('zero-generation.digest', 0, 40, resources), ('blue.digest', 81, 40, blue),
    ):
        # Independent Python SHA256 oracle, not an engine-generated expectation.
        digest = hashlib.sha256(b'GHOSTERM-P0-CHECKPOINT-1' +
                                struct.pack('<QQQQ', generation, sequence, 1024*1024, len(canonical)) +
                                canonical + struct.pack('<Q', len(graph)) + graph).digest()
        (work / name).write_bytes(digest)


def mutations(engine, work):
    embedded, termio = 'src/apprt/embedded.zig', 'src/termio/Termio.zig'
    variants = (
        ('checkpoint digest ignored', embedded,
         'if (!std.mem.eql(u8, &digest, &opts.p0_checkpoint_digest)) return error.CheckpointBindingMismatch;',
         'if (false) return error.CheckpointBindingMismatch;', 'binding', 'sequence'),
        ('event generation fence omitted', termio,
         'if (generation != self.replica_generation) return false;\n    if (self.replica_sequence ==',
         'if (generation == self.replica_generation) {}\n    if (self.replica_sequence ==', 'generation', 'legacy'),
        ('query generation fence omitted', termio,
         'if (generation != self.replica_generation) return false;\n    sequence.* = self.replica_sequence;',
         'if (generation == self.replica_generation) {}\n    sequence.* = self.replica_sequence;', 'generation', 'legacy'),
        ('checkpoint sequence reset', termio,
         '.replica_sequence = opts.replica_sequence,', '.replica_sequence = 0,', 'sequence', 'legacy'),
        ('bound sequence zero allows raw output', termio,
         'if (self.replica_generation != 0 or self.replica_sequence != 0) {',
         'if (self.replica_sequence != 0) {', 'zero-start', 'sequence'),
        ('unbound metadata silently ignored', embedded,
         '} else if (opts.p0_checkpoint_generation != 0 or opts.p0_checkpoint_sequence != 0 or !std.mem.allEqual(u8, &opts.p0_checkpoint_digest, 0)) return error.UnboundCheckpointMetadata;',
         '} else if (false) return error.UnboundCheckpointMetadata;', 'metadata', 'legacy'),
        ('zero generation accepted', embedded,
         'if (opts.p0_checkpoint_generation == 0) return error.InvalidCheckpointGeneration;',
         'if (false) return error.InvalidCheckpointGeneration;', 'metadata', 'sequence'),
        ('old event API bypasses identity', termio,
         'return self.replicaEventFor(0, sequence, event);',
         'return self.replicaEventFor(self.replica_generation, sequence, event);', 'generation', 'legacy'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path
        original = source.read_bytes()
        text = original.decode()
        digest = hashlib.sha256(original).digest()
        if MARK in text or text.count(old) != 1:
            raise RuntimeError(f'residual marker or nonunique checkpoint anchor: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            binary = build(engine, work)
            FULL['probe'](binary, work, target, 1)
            FULL['probe'](binary, work, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError('bound checkpoint rollback mismatch')
        binary = build(engine, work)
        FULL['probe'](binary, work, target)
    FULL['probe'](binary, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if args.case and args.mutations:
        parser.error('--case and --mutations are mutually exclusive')
    if sys.platform != 'darwin' or os.uname().machine != 'arm64':
        raise RuntimeError('bound checkpoint prototype requires native Apple Silicon macOS')
    files = (*PATCHES, SOURCE, ROOT / 'terminal/session-state/full-surface/event-fixture.c',
             ROOT / 'terminal/session-state/full-surface/probe.m',
             ROOT / 'terminal/session-state/resource-hooks-probe.c',
             ROOT / 'terminal/session-state/resource-io-probe.c', ROOT / 'terminal/session-state/probe.c')
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):
        return 2
    (ROOT / 'tmp').mkdir(exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-bound-checkpoint-', dir=ROOT / 'tmp') as tmp:
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
                print('PASS 8 bound checkpoint mutations and rollback; P0 remains incomplete')
                return 0
        print('P0 INCOMPLETE: atomic capture, input metadata, loading/animation and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'bound checkpoint probe failed: {error}; see tmp/session-surface-build.log', file=sys.stderr)
        sys.exit(1)
