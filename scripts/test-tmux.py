#!/usr/bin/env python3
"""Real optional tmux tests. Requires the claimed version, never counts SKIP."""
import argparse, os, pathlib, shutil, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parent.parent

FAKE = (ROOT / 'core/tests/fixtures/tmux-command.sh').read_text()

def run(binary, modes, fake=False):
    tmux = os.environ.get('TMUX_BINARY') or shutil.which('tmux')
    if not tmux and not fake:
        raise SystemExit('tmux fixture requires tmux >=3.2a; install it or set TMUX_BINARY')
    for mode in modes:
        with tempfile.TemporaryDirectory(prefix='tmux-test-', dir=ROOT / 'tmp') as work:
            folder = pathlib.Path(work)
            config = folder / 'tmux.conf'
            config.write_text('set -g default-shell /bin/sh\nset -g default-command \"exec /bin/sh\"\nset -g status off\nset -g mouse on\nset -s escape-time 0\nset -g assume-paste-time 0\nset -g destroy-unattached off\nset -s exit-unattached off\n')
            wrapper = folder / 'tmux'
            import shlex
            wrapper.write_text('#!/bin/sh\n' +
                ('case "$*" in *attach-session*) ' + shlex.quote(tmux) + ' -S ' + shlex.quote(str(folder / 'socket')) + ' -N kill-server ;; esac\n' if mode == 'race' else '') +
                'exec ' + shlex.quote(tmux or '/nonexistent') + ' -f ' + shlex.quote(str(config)) + ' "$@"\n')
            if mode == 'race': config.write_text(config.read_text() + 'set -s exit-empty off\n')
            if fake: wrapper.write_text(FAKE)
            wrapper.chmod(0o700)
            socket = str(folder / 'socket')
            env = dict(os.environ, TMPDIR=work, GT_TMUX_FAKE_SOCKET=socket, GT_TMUX_FAKE_MODE='good', GT_TMUX_FAKE_MARKER=str(folder / 'launched'))
            env.pop('TMUX', None)
            try:
                subprocess.run([str(binary), mode, str(wrapper), socket], env=env, check=True, timeout=60)
            finally:
                if not fake: subprocess.run([tmux, '-S', socket, '-N', 'kill-server'], env=env, capture_output=True, timeout=5)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fake', action='store_true')
    parser.add_argument('--binary', type=pathlib.Path, default=ROOT / 'core/build/test_tmux')
    parser.add_argument('modes', nargs='*', default=['lifecycle', 'names', 'identity', 'policy', 'attach', 'race', 'empty', 'owner-kill'])
    args = parser.parse_args()
    run(args.binary.resolve(), ["adversarial"] if args.fake else args.modes, args.fake)
