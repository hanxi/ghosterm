#!/usr/bin/env python3
"""Check bundled prompt hooks against their canonical standalone scripts."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESOURCES = ROOT / 'apple/GhostermApp/Sources/GhostermAppModel/Resources/ShellIntegration'


def check(bundle=None):
    destination = bundle / 'ShellIntegration' if bundle else RESOURCES
    for shell in ('zsh',):
        name = 'mouse-recovery.' + shell
        if (destination / name).read_bytes() != (ROOT / 'scripts/shell-integration' / name).read_bytes():
            raise RuntimeError('Bundled prompt hook differs from scripts/shell-integration/' + name)
    for name in ('zsh/.zshenv',):
        if (destination / name).read_bytes() != (RESOURCES / name).read_bytes():
            raise RuntimeError('Bundled shell startup resource differs: ' + name)
    actual = {str(path.relative_to(destination)) for path in destination.rglob('*') if path.is_file()}
    if actual != {'mouse-recovery.zsh', 'zsh/.zshenv'}:
        raise RuntimeError('Unexpected bundled shell integration files: ' + repr(sorted(actual)))


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path)
    arguments = parser.parse_args()
    check(arguments.bundle)
    print('PASS bundled shell integration resources')
