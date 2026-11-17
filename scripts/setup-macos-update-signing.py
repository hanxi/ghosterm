#!/usr/bin/env python3
"""Create/reuse a Keychain update key; optionally configure GitHub Actions.

Private material stays in Keychain and inherited pipes; never in files/logs.
Only --configure-github writes repository settings. No release is published.
"""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configure-github', action='store_true')
    parser.add_argument('--repo', default='hanxi/ghosterm')
    parser.add_argument('--account', default='ghosterm-updates-v1')
    args = parser.parse_args()
    (ROOT / 'tmp').mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
    helper = ROOT / 'build/tools/ghosterm-update-key'
    helper.parent.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'scripts/macos-update-key.swift'
    # A stable executable owns the Keychain item it creates, so configuring a
    # freshly generated key requires no second application's Keychain access.
    if not helper.exists() or helper.stat().st_mtime < source.stat().st_mtime:
        subprocess.run(['swiftc', str(source), '-o', str(helper)], check=True, env=env)
    read_fd, write_fd = os.pipe()
    try:
        result = subprocess.run([str(helper), args.account, str(write_fd if args.configure_github else -1)],
                                pass_fds=(write_fd,), text=True, stdout=subprocess.PIPE, check=True)
    finally:
        os.close(write_fd)
    try:
        private = os.read(read_fd, 1024).decode('ascii')
    finally:
        os.close(read_fd)
    public = result.stdout.strip()
    if not args.configure_github:
        print('Public key:', public)
        print('To configure Actions, re-run with --configure-github.')
        return
    if not private:
        raise RuntimeError('No private update key received; Actions was not changed')
    subprocess.run(['gh', 'secret', 'set', 'MACOS_UPDATE_PRIVATE_KEY', '--repo', args.repo],
                   input=private, text=True, check=True)
    subprocess.run(['gh', 'variable', 'set', 'MACOS_UPDATE_PUBLIC_KEY', '--repo', args.repo],
                   input=public, text=True, check=True)
    print('Configured update signing for', args.repo, '(no publication triggered)')


if __name__ == '__main__': main()
