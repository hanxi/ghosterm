#!/usr/bin/env python3
"""Encoding-time input metadata through queues. Default exit 3, P0 incomplete."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import re
import shutil
import shlex
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-surface-checkpoint.py'))
FULL = PREVIOUS['FULL']
PATCHES = (*PREVIOUS['PATCHES'], ROOT / 'patches/libghostty/8159838/experimental/0015-stamped-replica-input.patch')
SOURCE = ROOT / 'terminal/session-state/full-surface/input-stamp-probe.m'
MARK = 'GT_P0_INPUT_STAMP_MUTATION_MARK'
CASES = ('paste', 'allocated', 'key', 'sequence', 'cursor', 'linefeed', 'mouse', 'reply-source',
         'missing-callback', 'readonly', 'unstamped', 'wrong-generation', 'future', 'behind', 'legacy')


def prune_own_cache(work, *, min_free_bytes=6 * 1024**3):
    # Every child process is awaited before reaching this point. Touch only this
    # suite's private full-engine artifacts, preserving compiler/dependency data.
    if shutil.disk_usage(work).free >= min_free_bytes:
        return
    objects = work / 'full-cache/o'
    if not objects.is_dir():
        return
    artifact = re.compile(r'^(?:\d+-)?libghostty(?:-internal(?:-fat)?)?\.a$')
    removed = 0
    for directory in objects.iterdir():
        if directory.is_symlink() or not directory.is_dir():
            continue
        if any(p.is_file() and artifact.fullmatch(p.name) for p in directory.iterdir()):
            if directory.resolve().parent != objects.resolve():
                raise RuntimeError('unexpected private cache path')
            shutil.rmtree(directory)
            removed += 1
    if removed:
        # Zig's custom archive steps retain input hashes for generated files.
        # Invalidate their manifests together with outputs to avoid dangling hits.
        manifests = work / 'full-cache/h'
        if manifests.is_symlink():
            raise RuntimeError('unexpected private cache manifest path')
        if manifests.is_dir():
            shutil.rmtree(manifests)
        print(f'Reclaimed {removed} private full-engine artifact directories and manifests', flush=True)


def build(engine, work):
    prune_own_cache(work)
    FULL['build'](engine, work, cache_dir=work / 'full-cache')
    binary = work / 'surface-input'
    frameworks = [x for name in FULL['FRAMEWORKS'] for x in ('-framework', name)]
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                 '-I', str(engine / 'include'), str(SOURCE),
                 str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
                 '-lc++', *frameworks, '-o', str(binary)])
    return binary


def fixtures(engine, work):
    PREVIOUS['fixtures'](engine, work)


def unit_check(engine, work, pattern, expected=0, reason=None):
    prune_own_cache(work)
    zig = os.environ.get('ZIG') or shutil.which('zig') or str(ROOT / 'tmp/zig/zig-aarch64-macos-0.16.0/zig')
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'),
               ZIG_LOCAL_CACHE_DIR=str(work / 'full-cache'),
               ZIG_GLOBAL_CACHE_DIR=str(ROOT / 'tmp/zig-global'))
    result = subprocess.run([zig, 'build', 'test', '-Demit-xcframework=true', '-Demit-macos-app=false',
                             '-Dxcframework-target=native', '-Dsentry=false', '-Dtest-filter=' + pattern,
                             '--summary', 'all'], cwd=engine, env=env, capture_output=True, text=True, timeout=600)
    output = result.stdout + result.stderr
    (work / 'input-unit-check.log').write_text(output)
    if result.returncode != expected or (reason and reason not in output):
        sys.stderr.write(output[:2500] + "\n...\n" + output[-2000:])
        raise RuntimeError(f'input unit/{pattern}: expected {expected} and {reason}, got {result.returncode}')
    if expected == 0 and not re.search(r'\b[1-9]\d*(?:/[1-9]\d*)? (?:tests? )?passed\b', output):
        sys.stderr.write(output[:2500] + "\n...\n" + output[-2000:])
        raise RuntimeError('input unit filter ran no positive test set')
    print(f'PASS input unit/{pattern}: expected exit {expected}', flush=True)


def mutations(engine, work):
    surface, termio, host = 'src/Surface.zig', 'src/termio/Termio.zig', 'src/termio/Host.zig'
    variants = (
        ('generation corrupted at callback', host,
         'callback(self.callbacks.userdata, stamp.generation, stamp.sequence, data.ptr, data.len);',
         'callback(self.callbacks.userdata, stamp.generation + 1, stamp.sequence, data.ptr, data.len);', 'key', 'legacy'),
        ('stamp rewritten at drain', termio,
         'const stamp = request.stamp;',
         'var stamp = request.stamp; stamp.sequence = self.replica_sequence;', 'key', 'cursor'),
        ('linefeed re-read at drain', termio,
         'const stamp = request.stamp;',
         'var stamp = request.stamp; stamp.linefeed = self.terminal.modes.get(.linefeed);', 'paste', 'key'),
        ('paste fragments re-stamped', surface,
         '.data = try termio.Message.WriteReq.init(self.alloc, vec), .stamp = stamp',
         '.data = try termio.Message.WriteReq.init(self.alloc, vec), .stamp = latest: { self.renderer_state.mutex.lockUncancelable(global.io()); defer self.renderer_state.mutex.unlock(global.io()); var next = stamp; next.sequence = self.io.replica_sequence; break :latest next; }', 'paste', 'key'),
        ('delayed keys re-stamped', surface,
         '.flush => for (self.keyboard.sequence_queued.items) |write_req| {\n            self.queueEncodedInput(write_req, .unlocked);',
         '.flush => for (self.keyboard.sequence_queued.items) |write_req| {\n            var replay = write_req; self.renderer_state.mutex.lockUncancelable(global.io()); replay.stamp = self.io.replicaInputStampLocked(); self.renderer_state.mutex.unlock(global.io()); self.queueEncodedInput(replay, .unlocked);', 'sequence', 'key'),
        ('allocated input dropped', 'src/termio/Thread.zig',
         'try io.queueReplicaInput(v);', 'if (v.data != .alloc) try io.queueReplicaInput(v);', 'allocated', 'key'),
        ('raw user input not stamped', surface,
         'if (self.io.replica_generation != 0) {\n        const data: ?termio.Message.WriteReq',
         'if (false) {\n        const data: ?termio.Message.WriteReq', 'cursor', 'key'),
        ('missing callback does not invalidate', termio,
         'self.backend != .host or self.backend.host.callbacks.write_input == null)',
         'self.backend != .host or false)', 'missing-callback', 'key'),
        ('readonly input runs hook', surface,
         'if (self.readonly) { request.deinit(); return; }',
         'if (false) { request.deinit(); return; }', 'readonly', 'key'),
        ('wrong generation sent', termio,
         'stamp.generation != self.replica_generation or stamp.sequence <',
         'false or stamp.sequence <', 'wrong-generation', 'cursor'),
        ('future sequence sent', termio,
         'stamp.sequence > self.replica_sequence or', 'false or', 'future', 'cursor'),
        ('pre-checkpoint sequence sent', termio,
         'stamp.sequence < self.replica_start_sequence or', 'false or', 'behind', 'cursor'),
        ('unstamped input sent', termio,
         'if (self.replica_generation != 0) {\n        self.renderer_state.mutex.lockUncancelable(global.io());\n        if (self.replica_stream) |*stream| stream.handler.semantic_failure = true;',
         'if (false) {\n        self.renderer_state.mutex.lockUncancelable(global.io());\n        if (self.replica_stream) |*stream| stream.handler.semantic_failure = true;', 'unstamped', 'cursor'),
        ('parser reply classified as user input', surface,
         '.report_title => |style| report_title: {\n            if (self.io.replica_generation != 0) return error.ReplicaReplyForbidden;',
         '.report_title => |style| report_title: {\n            if (false) return error.ReplicaReplyForbidden;', 'reply-source', 'cursor'),
        ('linefeed chunk loses sequence', host,
         'callback(self.callbacks.userdata, stamp.generation, stamp.sequence, &buf, n);',
         'callback(self.callbacks.userdata, stamp.generation, stamp.sequence + 1, &buf, n);', 'linefeed', 'key'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path
        original = source.read_bytes()
        text = original.decode()
        digest = hashlib.sha256(original).digest()
        if MARK in text or text.count(old) != 1:
            raise RuntimeError(f'residual marker or nonunique input anchor: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            binary = build(engine, work)
            FULL['probe'](binary, work, target, 1)
            FULL['probe'](binary, work, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError('input stamp rollback mismatch')
        binary = build(engine, work)
        FULL['probe'](binary, work, target)
    FULL['probe'](binary, work)
    source = engine / 'src/termio/message.zig'
    original = source.read_bytes()
    text = original.decode()
    digest = hashlib.sha256(original).digest()
    for label, old, new, pattern, reason in (
        ('message size budget', '@as(usize, 72), @sizeOf(Message)', '@as(usize, 79), @sizeOf(Message)',
         'termio.message', 'expected 79, found 72'),
        ('readonly allocation cleanup', '.write_replica => |v| v.deinit(),', '.write_replica => {},',
         'queueIo frees stamped', 'leaked'),
    ):
        if text.count(old) != 1:
            raise RuntimeError(f'nonunique input unit anchor: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            unit_check(engine, work, pattern, 1, reason)
            if label == 'message size budget':
                binary = build(engine, work)
                FULL['probe'](binary, work, 'key')
            else:
                unit_check(engine, work, 'termio.message')
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError('input unit rollback mismatch')
        unit_check(engine, work, pattern)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if args.case and args.mutations:
        parser.error('--case and --mutations are mutually exclusive')
    if sys.platform != 'darwin' or os.uname().machine != 'arm64':
        raise RuntimeError('input stamp prototype requires native Apple Silicon macOS')
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
        with tempfile.TemporaryDirectory(prefix='session-input-stamp-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp)
            engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            fixtures(engine, work)
            binary = build(engine, work)
            FULL['probe'](binary, work, args.case or 'all')
            if args.mutations:
                unit_check(engine, work, 'termio.message')
                unit_check(engine, work, 'queueIo frees')
                mutations(engine, work)
                print('PASS 17 input stamp mutations and rollback; P0 remains incomplete')
                return 0
        print('P0 INCOMPLETE: worker validation, atomic capture, loading/animation and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'input stamp probe failed: {error}; see tmp/session-surface-build.log', file=sys.stderr)
        sys.exit(1)
