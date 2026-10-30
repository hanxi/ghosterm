#!/usr/bin/env python3
"""Install prompt-time mouse recovery for Bash or Zsh, with a byte-for-byte rc backup.

Run on the host that actually runs the shell, including the remote tmux host.
The default is a preview. --apply installs the small hook and appends one source
block; existing sessions need to reload their rc file once, or start a new shell.
This does not install a daemon, wrap ssh, modify tmux, or send terminal input.
"""
import argparse
import datetime
import os
from pathlib import Path
import stat
import tempfile

HERE = Path(__file__).resolve().parent
BEGIN = '# >>> Ghosterm automatic mouse recovery >>>'
END = '# <<< Ghosterm automatic mouse recovery <<<'


def quote(value):
    return "'" + str(value).replace("'", "'\\''") + "'"


def atomic_write(path, data, mode):
    # The repository requires all temporary files under ROOT/tmp. A cross-device
    # os.replace fails explicitly and leaves the startup file untouched.
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_root = HERE.parent / 'tmp'
    temporary_root.mkdir(exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='ghosterm-mouse-', dir=temporary_root)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def install(shell, rc, directory, apply=False):
    source = HERE / 'shell-integration' / ('mouse-recovery.' + shell)
    payload = source.read_bytes()
    destination = directory / source.name
    if rc.is_symlink() or destination.is_symlink():
        raise ValueError('symbolic-link rc/hook: pass the resolved file path explicitly')
    if rc.exists() and not rc.is_file():
        raise ValueError('startup path must be a regular file')
    before = rc.read_bytes() if rc.exists() else b''
    block = ('\n' + BEGIN + '\nsource ' + quote(destination) + '\n' + END + '\n').encode()
    if BEGIN.encode() in before or END.encode() in before:
        if before.count(BEGIN.encode()) != 1 or before.count(END.encode()) != 1:
            raise ValueError('ambiguous existing Ghosterm source block')
        start = before.index(BEGIN.encode())
        end = before.index(END.encode()) + len(END.encode())
        expected = block.strip(b'\n')
        if before[start:end] != expected:
            raise ValueError('existing source block differs; inspect it before changing destinations')
        after = before
    else:
        after = before + block
    print(('INSTALL' if apply else 'PREVIEW'), shell, 'hook:', destination, 'startup:', rc)
    if not apply:
        return
    backup = None
    if after != before and rc.exists():
        suffix = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
        backup = rc.with_name(rc.name + '.ghosterm-mouse-' + suffix + '.bak')
        # Exclusive creation never overwrites an older backup.
        with backup.open('xb') as output:
            output.write(before)
        os.chmod(backup, stat.S_IMODE(rc.stat().st_mode))
        if backup.read_bytes() != before:
            raise OSError('startup backup verification failed')
        print('BACKUP', backup)
    atomic_write(destination, payload, 0o600)
    if after != before:
        mode = stat.S_IMODE(rc.stat().st_mode) if rc.exists() else 0o600
        atomic_write(rc, after, mode)
    print('Installed. Load the startup file once in existing shells; future prompts recover automatically.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shell', choices=('bash', 'zsh'), required=True)
    parser.add_argument('--rc-file', type=Path, help='Startup file, e.g. ~/.zshrc or ~/.bashrc; required for custom ZDOTDIR/login profiles')
    parser.add_argument('--directory', type=Path, default=Path.home() / '.config/ghosterm/shell-integration')
    parser.add_argument('--apply', action='store_true', help='Install after reviewing the preview')
    args = parser.parse_args()
    rc = args.rc_file or Path.home() / ('.zshrc' if args.shell == 'zsh' else '.bashrc')
    install(args.shell, rc.expanduser().absolute(), args.directory.expanduser().absolute(), args.apply)


if __name__ == '__main__':
    main()
