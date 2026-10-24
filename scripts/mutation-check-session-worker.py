#!/usr/bin/env python3
"""Real PTY worker mutations in isolated source copies under project tmp/."""
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'core/src/session/worker.c'
original = SOURCE.read_bytes()
sha = hashlib.sha256(original).digest()
if b'MUTATION_SESSION_WORKER' in original:
    raise SystemExit(2)

def interrupted(signum, frame):
    raise KeyboardInterrupt

signal.signal(signal.SIGINT, interrupted)
signal.signal(signal.SIGTERM, interrupted)
(ROOT/'tmp').mkdir(exist_ok=True)
os.environ['TMPDIR']=str(ROOT/'tmp')
subprocess.run(['bash', str(ROOT / 'scripts/build-core.sh')], check=True,
               stdout=subprocess.DEVNULL)
with tempfile.TemporaryDirectory(prefix='session-worker-', dir=ROOT / 'tmp') as directory:
    source = Path(directory) / 'worker.c'
    binary = Path(directory) / 'probe'
    def build(text):
        source.write_text(text)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                        '-Werror', '-pthread', '-I' + str(ROOT / 'core/src'),
                        '-I' + str(ROOT / 'core/src/session'),
                        '-I' + str(ROOT / 'core/include'), str(source),
                        str(ROOT / 'core/tests/test_session_worker.c'),
                        str(ROOT / 'core/build/libghosterm_core.a'), '-o', str(binary)],
                       check=True)
    def run(mode):
        return subprocess.run([str(binary), mode], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=12).returncode
    text = original.decode()
    mutations = [
        ('worker bypasses domain','w = a.allocate(a.context,sizeof(*w))',
         'w = malloc(sizeof(*w)); /* MUTATION_SESSION_WORKER */','allocator','bytes'),
        ('payload bypasses domain','length ? w->allocator.allocate(w->allocator.context,length) : NULL',
         'length ? malloc(length) : NULL /* MUTATION_SESSION_WORKER */','allocator','bytes'),
        ('payload charge leaked','if(cmd->payload){gt__secure_erase(cmd->payload,cmd->length);w->allocator.release(w->allocator.context,cmd->payload,cmd->length);}',
         '/* MUTATION_SESSION_WORKER: payload leaked */','allocator','bytes'),
        ('join wrong object size','a.release(a.context,w,sizeof(*w));\n}',
         'a.release(a.context,w,sizeof(*w)-1); /* MUTATION_SESSION_WORKER */\n}','allocator','bytes'),
        ('reply-thread', 'if (!pthread_equal(w->owner_thread, pthread_self())) return GT_ERR_INVALID_ARG;',
         '/* MUTATION_SESSION_WORKER: foreign reply accepted */', 'reply-thread', 'bytes'),
        ('reply-window', '!w->reply_window || (length && !bytes)',
         '(length && !bytes) /* MUTATION_SESSION_WORKER */', 'reply-init', 'bytes'),
        ('reply-drop', 'result = backend->ops->write(backend, w->replies, w->replies_length);',
         '(void)backend; result = GT_OK; /* MUTATION_SESSION_WORKER */', 'replies', 'bytes'),
        ('reply-repeat', 'w->replies_length = 0;',
         '/* MUTATION_SESSION_WORKER: retain replies */', 'replies', 'bytes'),
        ('reply-write-failure', 'if (result==GT_OK) {w->replies_length = 0;}',
         'result=GT_OK; w->replies_length = 0; /* MUTATION_SESSION_WORKER */', 'reply-write-failure', 'bytes'),
        ('reply-overflow', 'return w->reply_failure = GT_ERR_PROTOCOL;',
         'return GT_OK; /* MUTATION_SESSION_WORKER */', 'reply-overflow', 'bytes'),
        ('startup-stop', '  if (stopped(w)) goto done;',
         '  /* MUTATION_SESSION_WORKER: start process after stop */', 'pressure', 'pty'),
        ('resize-state', 'gt_state_event_t event = {GT_STATE_RESIZE, size, sizeof(*size)};',
         'gt_state_size_t wrong = {80, 24, 0, 0}; /* MUTATION_SESSION_WORKER */\n'
         '          gt_state_event_t event = {GT_STATE_RESIZE, &wrong, sizeof(wrong)};', 'pty', 'bytes'),
        ('output-state', 'gt_state_event_t event = {GT_STATE_OUTPUT, output, (size_t)n};',
         'gt_state_event_t event = {GT_STATE_OUTPUT, output, 0}; /* MUTATION_SESSION_WORKER */', 'pty', 'bytes'),
        ('request-limit', 'w->outstanding == QUEUE_CAPACITY',
         'false /* MUTATION_SESSION_WORKER */', 'pressure', 'bytes'),
        ('byte-limit', 'length > INPUT_CAPACITY - w->bytes',
         'false /* MUTATION_SESSION_WORKER */', 'bytes', 'pty'),
        ('cancel', 'complete(w, &cmd, GT_ERR_CANCELLED, (gt_state_capture_t){0});',
         'complete(w, &cmd, GT_OK, (gt_state_capture_t){0}); /* MUTATION_SESSION_WORKER */', 'pressure', 'pty'),
        ('state-failure', 'owner.sequence!=previous && c->output',
         '((void)previous,true) && c->output /* MUTATION_SESSION_WORKER */', 'failure', 'bytes'),
    ]
    try:
        build(text)
        for mode in ['pty', 'pressure', 'bytes', 'failure', 'replies', 'reply-overflow', 'reply-write-failure', 'reply-thread', 'reply-init', 'allocator']:
            assert run(mode) == 0, mode
        for name, before, after, target, control in mutations:
            assert text.count(before) == 1, name
            build(text.replace(before, after))
            assert run(target) != 0, name + ': target falsely green'
            assert run(control) == 0, name + ': control failed'
            print(name + ': target red, control green', flush=True)
        build(text)
        for mode in ['pty', 'pressure', 'bytes', 'failure', 'replies', 'reply-overflow', 'reply-write-failure', 'reply-thread', 'reply-init', 'allocator']:
            assert run(mode) == 0, mode
    finally:
        source.write_bytes(original)
        assert hashlib.sha256(source.read_bytes()).digest() == sha
        assert hashlib.sha256(SOURCE.read_bytes()).digest() == sha
print('rollback SHA256 and all baselines green')
