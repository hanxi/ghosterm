#!/usr/bin/env python3
"""P0 global allocator ownership/lifecycle. Shares worker-surface paths: run alone."""
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
SURFACE = runpy.run_path(str(ROOT / 'scripts/probe-session-worker-surface.py'))
FULL, VT, PATCHES = (SURFACE[k] for k in ('FULL', 'VT', 'PATCHES'))
SOURCE = ROOT / 'daemon/probes/session-global-api-test.c'
MARK = 'GT_GLOBAL_OWNER_MUTATION_MARK'
CASES = ('cold', 'valid', 'deny-first', 'deny-second', 'capacity', 'legacy')


def interrupted(signum, frame):
    raise RuntimeError(f'signal {signum}; restoring isolated global sources')


def run(command, *, check=True, timeout=600, **kwargs):
    # Includes the actual Zig build runner, compiler and native linker.
    process = subprocess.Popen(command, start_new_session=True, **kwargs)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except BaseException:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise
    result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    if check and result.returncode:
        raise subprocess.CalledProcessError(result.returncode, command, stdout, stderr)
    return result


def build(engine, work, evidence, label):
    FULL['build'](engine, work, cache_dir=work / 'full-cache',
                  log_path=evidence / (label + '-build.log'), process_run=run)
    binary = evidence / 'global-api-test'
    frameworks = [arg for name in FULL['FRAMEWORKS'] for arg in ('-framework', name)]
    command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'),
               str(SOURCE), str(ROOT / 'terminal/session-state/gui-budget.c'),
               str(ROOT / 'terminal/session-state/budget-posix.c'),
               str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
               str(work / 'out/lib/libghostty-vt.dylib'), '-Wl,-rpath,' + str(work / 'out/lib'),
               '-lc++', *frameworks, '-o', str(binary)]
    run(command)
    return binary


def probe(binary, evidence, case, expected=0):
    result = run([str(binary), case, str(evidence)], check=False, timeout=30,
                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    output = result.stdout
    if (result.returncode != expected or
            (expected == 1 and 'FAIL line ' not in output) or
            (expected == 0 and 'PASS global-owner/' + case + ':' not in output)):
        raise RuntimeError(f'{case}: expected assertion rc={expected}, got {result.returncode}: {output}')
    return output


def mutations(engine, work, evidence):
    paths = [engine / 'src/global.zig', engine / 'src/main_c.zig']
    originals = {p: p.read_bytes() for p in paths}
    digests = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('residual global mutation marker')
    specs=(
     ('selected allocator ignored',paths[0],'self.alloc = host_allocator orelse if (self.gpa) |*value|','self.alloc = if (self.gpa) |*value|','valid','legacy'),
     ('initialization failure reported success',paths[1],'p0_global_phase.store(5, .release);\n        std.log.err("failed to initialize ghostty error={}", .{err});\n        return 1;','p0_global_phase.store(5, .release);\n        std.log.err("failed to initialize ghostty error={}", .{err});\n        return 0;','deny-first','valid'),
     ('global teardown omitted',paths[1],'    global.deinit();','    // global teardown omitted','valid','deny-first'),
     ('owned initialization marked legacy',paths[1],'p0_global_phase.store(if (host_allocator != null) 3 else 2, .release);','p0_global_phase.store(2, .release);','valid','legacy'),
     ('duplicate initialization allowed',paths[1],'    if (p0_global_phase.cmpxchgStrong(0, 1, .acq_rel, .acquire) != null) return 1;','    _ = p0_global_phase.cmpxchgStrong(0, 1, .acq_rel, .acquire);','valid','cold'),
     ('null host accepted',paths[1],'    const selected = host orelse return 1;','    const selected = host orelse return initC(argc, argv, null);','valid','cold'),
     ('failed initialization allows retry',paths[1],'p0_global_phase.store(5, .release);','p0_global_phase.store(0, .release);','deny-first','valid'),
     ('repeated teardown pretends success',paths[1],'    if (p0_global_phase.cmpxchgStrong(3, 4, .acq_rel, .acquire) != null) return false;','    if (p0_global_phase.load(.acquire) == 4) return true;\n    if (p0_global_phase.cmpxchgStrong(3, 4, .acq_rel, .acquire) != null) return false;','valid','legacy'),
     ('retired global reopens',paths[1],'    global.deinit();','    global.deinit();\n    p0_global_phase.store(0, .release);','valid','legacy'),
    )
    try:
        for index, (label, path, old, new, target, control) in enumerate(specs):
            text = originals[path].decode()
            if text.count(old) != 1:
                raise RuntimeError(label + ': nonunique global mutation anchor')
            path.write_text(text.replace(old, new) + '\n// ' + MARK + '\n')
            binary = build(engine, work, evidence, 'mutation-' + str(index))
            output = probe(binary, evidence, target, 1) + probe(binary, evidence, control)
            (evidence / f'mutation-{index}-run.log').write_text(output)
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest() != digests[path]:
                raise RuntimeError('global SHA256 rollback mismatch')
            print('PASS global-owner mutation/' + label + ': target red/control green', flush=True)
    finally:
        for path, data in originals.items():
            path.write_bytes(data)
        for path, digest in digests.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError('global SHA256 final rollback mismatch')
    binary = build(engine, work, evidence, 'rollback')
    for case in CASES:
        probe(binary, evidence, case)
    print('PASS 9 global-owner mutations, SHA256 rollback and six baselines', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        raise RuntimeError('current global-owner evidence requires macOS; no silent skip')
    files = (*PATCHES, SOURCE, ROOT / 'scripts/prepare-session-io.py', ROOT / 'terminal/session-state/gui-budget.c',
             ROOT / 'terminal/session-state/budget-posix.c', Path(__file__).resolve())
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    work = ROOT / 'tmp/worker-surface'
    prior = [work / 'engine/src/global.zig', work / 'engine/src/main_c.zig']
    if any(MARK in p.read_text() for p in (*PATCHES, SOURCE, *prior) if p.exists()):
        return 2
    evidence = ROOT / 'tmp/global-owner'
    evidence.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT / 'tmp')
    os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    os.environ['ZIG'] = (os.environ.get('ZIG') or shutil.which('zig') or
                         str(ROOT / 'tmp/zig/zig-aarch64-macos-0.16.0/zig'))
    version = run([os.environ['ZIG'], 'version'], stdout=subprocess.PIPE, text=True).stdout.strip()
    if version != '0.16.0':
        raise RuntimeError('Zig 0.16.0 required')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    # The inherited helpers also use this owned process runner.
    FULL['prepare'].__globals__['run'] = run
    FULL['build'].__globals__['run'] = run
    VT['build'].__globals__['run'] = run
    try:
        for name in ('engine', 'engine.tar'):
            path = work / name
            if path.is_dir():
                shutil.rmtree(path)
            elif path.exists():
                path.unlink()
        engine = FULL['prepare'](work)
        for patch in PATCHES:
            run(['git', '-C', str(engine), 'apply', '--check', str(patch)])
            run(['git', '-C', str(engine), 'apply', str(patch)])
        VT['build'](engine, work, True, log_path=evidence / 'vt-build.log')
        binary = build(engine, work, evidence, 'baseline')
        for case in CASES:
            print(probe(binary, evidence, case), end='', flush=True)
        if args.mutations:
            mutations(engine, work, evidence)
        print('PASS global-owner: explicit allocator and process-scoped lifecycle; P0 incomplete', flush=True)
        return 0
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError('repository input changed: ' + str(path))


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print('global-owner probe failed: ' + str(error), file=sys.stderr)
        sys.exit(1)
