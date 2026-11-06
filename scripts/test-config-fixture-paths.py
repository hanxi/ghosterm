#!/usr/bin/env python3
"""Exercise the actual configuration oracle with long and literal path inputs."""
import argparse
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'core/build/test_config')
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix='config-path-', dir=ROOT / 'tmp'))
    try:
        target = work
        while len(str(target)) < 600:
            target /= 'long-fixture-' + 'x' * 70
        target.mkdir(parents=True)
        shutil.copytree(ROOT / 'core/tests/fixtures', target / 'fixtures')
        scratch = work / 'path;false'; scratch.mkdir()
        results = []
        for label, argv, expected in [
            ('long-literal', [target / 'fixtures', scratch], 0),
            ('missing-temp', [ROOT / 'core/tests/fixtures', work / 'missing'], 2),
        ]:
            p = subprocess.run([str(args.binary.resolve()), *map(str, argv)], capture_output=True, text=True, timeout=60)
            assert p.returncode == expected, (label, p.returncode, p.stdout, p.stderr)
            if label == 'long-literal':
                assert '322 passed, 0 failed' in p.stdout, p.stdout
                assert not list(scratch.iterdir()), 'test fixture leaked'
            else:
                assert 'mkdtemp' in p.stderr, p.stderr
            results.append(dict(case=label, returncode=p.returncode, output=p.stdout + p.stderr))
        print(json.dumps(results, ensure_ascii=False, indent=2))
    finally:
        shutil.rmtree(work)

if __name__ == '__main__':
    main()
