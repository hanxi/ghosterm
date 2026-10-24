#!/usr/bin/env python3
"""Private owner boundary: isolated source mutations, target red/control green."""
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'core/src/session/state-owner.c'
original = SOURCE.read_bytes()
sha = hashlib.sha256(original).digest()
if b'MUTATION_STATE_OWNER' in original:
    raise SystemExit(2)

def interrupted(signum, frame):
    raise KeyboardInterrupt

signal.signal(signal.SIGINT, interrupted)
signal.signal(signal.SIGTERM, interrupted)
(ROOT / 'tmp').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='state-owner-', dir=ROOT / 'tmp') as directory:
    work = Path(directory)
    source = work / 'state-owner.c'
    binary = work / 'probe'
    def build(text):
        source.write_text(text)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                        '-Werror', '-pthread', '-I' + str(ROOT / 'core/src'),
                        '-I' + str(ROOT / 'core/src/session'),
                        '-I' + str(ROOT / 'core/include'), str(source),
                        str(ROOT / 'core/tests/test_state_owner.c'), '-o', str(binary)],
                       check=True)
    def run(control=False):
        return subprocess.run([str(binary)] + (['control'] if control else []),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=10).returncode
    text = original.decode()
    mutations = [
        ('publish', 'owner->sequence++; /* STATE_OWNER_PUBLISH */',
         '/* MUTATION_STATE_OWNER: no publication */'),
        ('sticky', 'owner->failure = result;',
         '/* MUTATION_STATE_OWNER: ignore failed state */'),
        ('thread', '!pthread_equal(owner->thread, pthread_self())',
         '(false /* MUTATION_STATE_OWNER */)'),
        ('boundary', 'capture->sequence = owner->sequence;',
         'capture->sequence = 0; /* MUTATION_STATE_OWNER */'),
    ]
    try:
        build(text)
        assert run() == 0 and run(True) == 0
        for name, before, after in mutations:
            assert text.count(before) == 1, name
            build(text.replace(before, after))
            assert run() != 0, name + ': target falsely green'
            assert run(True) == 0, name + ': control failed'
            print(name + ': target red, control green', flush=True)
        build(text)
        assert run() == 0 and run(True) == 0
    finally:
        source.write_bytes(original)
        assert hashlib.sha256(source.read_bytes()).digest() == sha
        assert hashlib.sha256(SOURCE.read_bytes()).digest() == sha
print('rollback SHA256 and baseline green')
