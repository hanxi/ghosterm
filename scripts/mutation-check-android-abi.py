#!/usr/bin/env python3
"""Isolated ABI mutation checks: targeted red, independent green, restored SHA256."""
import hashlib
import json
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARKER = 'GT_ANDROID_ABI_MUTATION'


def main():
    paths = [ROOT / 'core/include', ROOT / 'android/bridge', ROOT / 'terminal/android/native']
    originals = [p for parent in paths for p in parent.rglob('*') if p.suffix in ('.c', '.h', '.json') and 'build' not in p.parts]
    hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in originals}
    if any(MARKER.encode() in p.read_bytes() for p in originals):
        raise RuntimeError('mutation marker present before start')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='android-abi-', dir=ROOT / 'tmp') as directory:
        work = Path(directory)
        for path in paths:
            shutil.copytree(path, work / path.relative_to(ROOT), ignore=shutil.ignore_patterns('build', '.cxx', '.gradle'))
        def check(expected):
            result = subprocess.run(['python3', ROOT / 'scripts/check-abi-android.py', '--root', work], capture_output=True, text=True, check=False)
            if (result.returncode == 0) != (expected is None) or (expected and expected not in result.stderr):
                raise RuntimeError(result.stdout + result.stderr)
        check(None)
        source = work / 'android/bridge/src/main/cpp/core_jni.c'
        allow = work / 'android/bridge/deferred-abi.json'
        header = work / 'core/include/ghosterm.h'
        cases = [
            (source, lambda s: s + '\nvoid probe(void) { gt_nonexistent(); }\n', 'undeclared JNI references'),
            (allow, lambda s: json.dumps(dict(json.loads(s), gt_version='intentional stale entry')), 'stale deferred bindings'),
            (header, lambda s: s + '\nvoid gt_future_api(void);\n', 'unregistered bindings'),
        ]
        for index, (path, mutate, error) in enumerate(cases, 1):
            saved = path.read_bytes()
            try:
                path.write_text(mutate(saved.decode()))
                check(error)
                # Same mutation: public headers still compile independently of the JNI reference inventory.
                control = subprocess.run(['cc', '-std=c11', '-Werror', '-fsyntax-only', '-x', 'c',
                                          '-I', str(work / 'core/include'), '-'],
                                         input='#include \"ghosterm.h\"\nint main(void) { return GT_OK; }\n',
                                         capture_output=True, text=True)
                if control.returncode:
                    raise RuntimeError(control.stdout + control.stderr)
            finally:
                path.write_bytes(saved)
                if hashlib.sha256(path.read_bytes()).digest() != hashlib.sha256(saved).digest():
                    raise RuntimeError('restore hash mismatch')
            check(None)
            print(f'A{index}: target red / independent green / restored green')
    if any(hashlib.sha256(p.read_bytes()).hexdigest() != h for p, h in hashes.items()):
        raise RuntimeError('repository changed during isolated mutations')
    print('ANDROID ABI MUTATIONS PASS: 3 groups')


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    main()
