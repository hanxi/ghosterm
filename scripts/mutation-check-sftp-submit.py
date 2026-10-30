#!/usr/bin/env python3
"""Prove job submission never reads a worker-freed object using real SFTP/ASan."""
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_SFTP_SUBMIT_MUTATION'


def main():
    source = ROOT / 'core/src/sftp.c'
    original = source.read_bytes()
    if MARK.encode() in original: raise RuntimeError('Residual submit mutation')
    logs = ROOT / 'tmp/sftp-submit-mutations'; logs.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), ASAN_OPTIONS='detect_leaks=0')
    def run(args, label, red=False):
        result = subprocess.run(['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require sftp-submit; shift; gt_timeout 180 "$@"',
                                 'sftp-submit', str(ROOT), *map(str, args)], cwd=ROOT, env=env, capture_output=True, text=True)
        output = result.stdout + result.stderr
        (logs / (label + '.log')).write_text(output)
        okay = (result.returncode != 0 and 'AddressSanitizer: heap-use-after-free' in output and 'submit_job' in output) if red else result.returncode == 0 and 'SKIP' not in output
        if not okay:
            print(output, flush=True)
            raise RuntimeError(label + ': expected ' + ('specific ASan red' if red else 'green'))
        return output
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-submit-', dir=ROOT / 'tmp') as directory:
            work = Path(directory)
            shutil.copytree(ROOT / 'core', work / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
            build = work / 'build'; changed = work / 'core/src/sftp.c'
            run(['cmake', '-S', work / 'core', '-B', build], 'configure')
            def compile_probe():
                run(['cmake', '--build', build, '--clean-first', '--target', 'test_sftp_submit', 'test_api'], 'build')
            def check(label, red=False):
                output = run(['bash', ROOT / 'core/tests/scripts/ssh_e2e.sh', build / 'test_sftp_submit'], label, red)
                if not red and 'SFTP SUBMIT PASS' not in output: raise RuntimeError('Submit probe did not execute')
            compile_probe(); check('baseline')
            text = original.decode(); old = '  return job_id;'
            assert text.count(old) == 1
            changed.write_text(text.replace(old, '  (void)job_id; return j->job_id; /* ' + MARK + ' */'))
            compile_probe(); check('red', True)
            run([build / 'test_api'], 'api-control')
            changed.write_bytes(original); compile_probe(); check('restored')
            assert hashlib.sha256(changed.read_bytes()).digest() == hashlib.sha256(original).digest()
    finally:
        if source.read_bytes() != original: raise RuntimeError('Production changed during submit mutation')
    print('SFTP SUBMIT MUTATION PASS: actual worker free / ASan target red / API green / restored green / SHA256', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
