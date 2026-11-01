#!/usr/bin/env python3
"""Read the APK's source receipt; missing/unknown state is never assumed clean."""
import argparse
import json
from pathlib import Path
import re
from zipfile import ZipFile


def distinct_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate metadata key')
        result[key] = value
    return result


def read_metadata(apk):
    with ZipFile(apk) as archive:
        if archive.namelist().count('assets/ghosterm-build.json') != 1:
            raise ValueError('expected exactly one APK source receipt')
        value = json.loads(archive.read('assets/ghosterm-build.json'), object_pairs_hook=distinct_object)
    if not isinstance(value, dict) or set(value) != {'schema', 'git_commit', 'source_state'}:
        raise ValueError('metadata schema key set')
    if type(value['schema']) is not int or value['schema'] != 1:
        raise ValueError('metadata schema version')
    commit = value['git_commit']
    if commit is not None and (not isinstance(commit, str) or not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', commit)):
        raise ValueError('invalid source commit')
    if value['source_state'] not in ('clean', 'dirty', 'unknown'):
        raise ValueError('invalid source state')
    if commit is None and value['source_state'] != 'unknown':
        raise ValueError('missing commit cannot identify a clean or dirty source tree')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--expect-commit', required=True)
    parser.add_argument('--expect-state', required=True, choices=('clean', 'dirty', 'unknown'))
    args = parser.parse_args()
    value = read_metadata(args.apk)
    if value['git_commit'] != args.expect_commit:
        raise ValueError('APK source commit mismatch')
    if value['source_state'] != args.expect_state:
        raise ValueError('APK source_state mismatch')
    print('ANDROID APK SOURCE PASS:', value['git_commit'], value['source_state'])


if __name__ == '__main__':
    main()
