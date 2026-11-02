#!/usr/bin/env python3
"""Reject build-host paths embedded anywhere in the distributable APK."""
import argparse
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def build_path_entries(apk):
    patterns = {
        b'/Users/': 'developer-home',
        b'/home/runner/work/': 'github-runner',
        b'/runner/_work/': 'runner-workspace',
        b'/__w/': 'container-workspace',
        b'/github/workspace/': 'container-workspace',
        str(ROOT).encode(): 'current-workspace',
    }
    overlap = max(map(len, patterns)) - 1
    findings = []
    with zipfile.ZipFile(apk) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            found = {label for pattern, label in patterns.items() if pattern in entry.filename.encode()}
            with archive.open(entry) as source:
                tail = b''
                while block := source.read(1024 * 1024):
                    block = tail + block
                    found.update(label for pattern, label in patterns.items() if pattern in block)
                    tail = block[-overlap:]
            if found:
                findings.append((entry.filename, sorted(found)))
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    args = parser.parse_args()
    findings = build_path_entries(args.apk)
    if findings:
        details = '; '.join(name + ' (' + ', '.join(labels) + ')' for name, labels in findings)
        raise SystemExit('Android APK embeds build-host paths: ' + details)
    print('ANDROID APK BUILD PATHS PASS')


if __name__ == '__main__':
    main()
