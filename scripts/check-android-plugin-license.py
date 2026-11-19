#!/usr/bin/env python3
"""Verify the QuickJS license in an Android APK against its intended build profile."""
import argparse
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
LICENSE_ENTRY = 'assets/licenses/QuickJSNG-MIT.txt'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--plugin-js', choices=('enabled', 'disabled'), required=True)
    args = parser.parse_args()
    with ZipFile(args.apk) as archive:
        count = archive.namelist().count(LICENSE_ENTRY)
        if args.plugin_js == 'disabled':
            if count:
                parser.exit(1, 'QuickJS license present in a plugin-disabled APK\n')
        else:
            if count != 1:
                parser.exit(1, f'Expected one QuickJS license in the APK, found {count}\n')
            upstream = (ROOT / 'vendor/quickjs-ng/LICENSE').read_bytes()
            if archive.read(LICENSE_ENTRY) != upstream:
                parser.exit(1, 'APK QuickJS license differs from the pinned upstream license\n')
    print(f'ANDROID APK QUICKJS LICENSE PASS ({args.plugin_js})')


if __name__ == '__main__':
    main()
