#!/usr/bin/env python3
"""Check the linked shared client does not extract host runtime/PTY/engine code."""
import re
import subprocess
import sys

PREFIXES = ('gt__manager_', 'gt__worker_', 'gt__pty_', 'gt__creation_', 'gt__server_',
            'gt__runtime_', 'gt__data_router_', 'gt__attachment_owner_', 'gt__ssh_',
            'gt__sftp_', 'gt__handshake_', 'gt__tokens_', 'gt_transport_', 'gt_ssh_', 'gt_sftp_', 'ghostty_')


def main():
    if len(sys.argv) != 2:
        return 2
    p = subprocess.run(['nm', '-g', sys.argv[1]], capture_output=True, text=True)
    if p.returncode:
        print('nm failed: ' + p.stderr, file=sys.stderr)
        return 2
    names = {line.split()[-1].removeprefix('_') for line in p.stdout.splitlines()
             if re.search(r'\b_?(?:gt_|ghostty_)\w+\s*$', line)}
    forbidden = sorted(name for name in names if name.startswith(PREFIXES))
    if forbidden:
        print('FAIL attachment client dependency boundary: ' + ', '.join(forbidden), file=sys.stderr)
        return 1
    print('PASS attachment client linked without host runtime/PTY/engine')
    return 0


if __name__ == '__main__':
    sys.exit(main())
