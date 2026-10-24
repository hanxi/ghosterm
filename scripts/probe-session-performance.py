#!/usr/bin/env python3
"""Measure real PTY -> VT direct/worker paths; excludes GUI, IPC and rendering."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import runpy
import shlex
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'daemon/probes/session-performance.c'
STATE = runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))
MARK = 'GT_SESSION_PERFORMANCE_MUTATION_MARK'


def run(command, **kwargs):
    if kwargs.pop('capture_output', False):
        kwargs['stdout'] = subprocess.PIPE
        kwargs['stderr'] = subprocess.PIPE
    timeout = kwargs.pop('timeout', 300)
    process = subprocess.Popen(command, start_new_session=True, **kwargs)
    try:
        out, err = process.communicate(timeout=timeout)
    except BaseException:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()
        raise
    return subprocess.CompletedProcess(command, process.returncode, out, err)


def checked_run(command, **kwargs):
    result = run(command, **kwargs)
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, command)
    return result


def compile_probe(source, binary, library):
    result = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-O2',
        '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'core/src'),
        '-I', str(ROOT / 'core/include'), '-I', str(ROOT / 'vendor/ghostty/include'),
        str(source), str(ROOT / 'core/build/libghosterm_core.a'), str(library),
        *(['-lutil'] if sys.platform.startswith('linux') else []), '-lpthread', '-o', str(binary)])
    if result.returncode:
        raise RuntimeError('performance compile failed')


def measure(binary, mode):
    result = run([str(binary), mode], cwd=ROOT, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    row = json.loads(result.stdout)
    if (row['mode'] != mode or row['bulk_bytes'] != 4*1024*1024 or
            row['latency_samples'] != 128 or not row['snapshot_equal'] or
            row['applied_bytes'] != 1+4*1024*1024+128 or
            not 900 <= row['idle_wall_ms'] < 10000 or
            not 0 <= row['idle_cpu_ms'] or
            not 0 < row['latency_p50_ms'] <= row['latency_p95_ms'] <= row['latency_p99_ms'] or
            row['bulk_ms'] <= 0 or row['peak_rss_native'] <= 0):
        raise RuntimeError('invalid measured result: '+result.stdout)
    row['throughput_mib_s'] = row['bulk_bytes']/(1024*1024)/(row['bulk_ms']/1000)
    row['idle_cpu_percent_one_core'] = row['idle_cpu_ms']/row['idle_wall_ms']*100
    row['peak_rss_bytes'] = row['peak_rss_native'] * (1 if sys.platform == 'darwin' else 1024)
    return row


def mutations(work, library):
    original = SOURCE.read_bytes()
    digest = hashlib.sha256(original).digest()
    variants = (
        ('PTY producer corrupts bytes', 'block[i]=pattern(offset+i);send_all', "block[i]=(unsigned char)'Z';send_all"),
        ('terminal application omitted', 'ghostty_terminal_vt_write(b->terminal,event->payload,event->length);', '(void)event;'),
        ('echo input corrupted', "(unsigned char)('a'+i%26));wait_received", "(unsigned char)('a'+(i+1)%26));wait_received"),
    )
    source, binary = work/'mutated.c', work/'mutated'
    try:
        for label, old, new in variants:
            text = original.decode()
            if text.count(old) != 1:
                raise RuntimeError('nonunique mutation: '+label)
            source.write_text(text.replace(old, new)+'\n/* '+MARK+' */\n')
            compile_probe(source, binary, library)
            result = run([str(binary), 'worker'], cwd=ROOT, capture_output=True, text=True)
            if result.returncode != 1 or 'FAIL performance:' not in result.stderr:
                raise RuntimeError('mutation did not reach assertion: '+label+result.stderr)
            control = run([str(binary), 'oracle'], cwd=ROOT, capture_output=True, text=True)
            if control.returncode or 'PASS independent oracle control' not in control.stdout:
                raise RuntimeError('same-mutant independent control failed: '+label)
            print('PASS mutation/'+label+': target red, same-mutant independent oracle green', flush=True)
    finally:
        source.write_bytes(original)
        if hashlib.sha256(source.read_bytes()).digest() != digest or SOURCE.read_bytes() != original:
            raise RuntimeError('performance source rollback mismatch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.rounds <= 10:
        parser.error('rounds must be 1..10')
    work = ROOT/'tmp/session-performance'
    if MARK.encode() in SOURCE.read_bytes() or (work/'mutated.c').exists() and MARK.encode() in (work/'mutated.c').read_bytes():
        return 2
    def interrupted(signum, frame):
        raise RuntimeError('performance signal '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    work.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    # Build the exact unmodified pinned VT, not a potentially mutated artifact.
    checked_run(['bash', 'scripts/build-core.sh'], cwd=ROOT)
    STATE['build_engine'].__globals__['run'] = checked_run
    library = STATE['build_engine']()
    binary = work/'performance'
    compile_probe(SOURCE, binary, library)
    rows = []
    for index in range(args.rounds):
        for mode in (('direct', 'worker') if index%2 == 0 else ('worker', 'direct')):
            row = measure(binary, mode)
            row['round'] = index+1
            rows.append(row)
            print(json.dumps(row, sort_keys=True), flush=True)
    if args.mutations:
        mutations(work, library)
        for mode in ('direct', 'worker'):
            measure(binary, mode)
    paths = (SOURCE, Path(__file__).resolve(), ROOT/'core/build/libghosterm_core.a', library, binary)
    report = {'build': {'probe': 'C11 -O2', 'core': 'Debug', 'vt': 'ReleaseFast',
                        'compiler': subprocess.check_output([*shlex.split(os.environ.get('CC', 'cc')), '--version'], text=True).splitlines()[0]},
              'scope': 'PTY -> VT only; no GUI/IPC/rendering; idle CPU excludes child',
              'platform': platform.platform(), 'pin': STATE['PIN'], 'rounds': args.rounds,
              'sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
              'rows': rows, 'mutations': 3 if args.mutations else 0,
              'note': 'RSS includes final oracle/snapshots; no production performance budget asserted'}
    (work/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    print('PASS measured direct/worker paths, raw bytes and independent VT snapshots; P0 incomplete')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
