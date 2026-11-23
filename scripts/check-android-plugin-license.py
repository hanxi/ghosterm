#!/usr/bin/env python3
"""Verify an Android APK runtime license against its build profile."""
import argparse
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
LUA_ENTRY = 'assets/licenses/Lua-MIT.txt'
LUA_UPSTREAM = ROOT / 'core/cmake/Lua-MIT.txt'
RETIRED_ENTRY = 'assets/licenses/QuickJSNG-MIT.txt'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--runtime', choices=('disabled', 'lua'), required=True)
    args = parser.parse_args()
    runtime = args.runtime
    with ZipFile(args.apk) as archive:
        names = archive.namelist()
        if RETIRED_ENTRY in names:
            parser.exit(1, f'Unexpected retired runtime license in the APK: {RETIRED_ENTRY}\n')
        expected = int(runtime == 'lua')
        count = names.count(LUA_ENTRY)
        if count != expected:
            parser.exit(1, f'Expected {expected} Lua licenses in the APK, found {count}\n')
        if expected and archive.read(LUA_ENTRY) != LUA_UPSTREAM.read_bytes():
            parser.exit(1, 'APK Lua license differs from the pinned upstream license\n')
    print(f'ANDROID APK PLUGIN LICENSE PASS ({runtime})')


if __name__ == '__main__':
    main()
