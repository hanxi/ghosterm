#!/usr/bin/env python3
"""Verify pinned host libssh2 inputs and artifacts; no network or rebuilds."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

SOURCE_SHA = 'd9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7'
PATCH_NAMES = ('0001-sftp-eof.patch', '0002-sftp-init-cleanup.patch', '0003-sftp-name-bounds.patch')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(prefix, root):
    record = json.loads((prefix / 'receipt.json').read_text())
    expected = {name: sha(root / 'patches/libssh2/1.11.1' / name) for name in PATCH_NAMES}
    if record.get('source_sha256') != SOURCE_SHA or record.get('patches') != expected:
        raise RuntimeError('host libssh2 source/patch receipt mismatch')
    if record.get('builder') != sha(root / 'scripts/build-host-ssh2.py'):
        raise RuntimeError('host libssh2 builder receipt mismatch')
    library = 'lib/libssh2.dylib' if sys.platform == 'darwin' else 'lib/libssh2.so'
    required = (library, 'lib/libssh2.a', 'include/libssh2.h', 'include/libssh2_sftp.h',
                'include/libssh2_publickey.h', 'share/licenses/libssh2.txt')
    for name in required:
        if record.get('sha256', {}).get(name) != sha(prefix / name):
            raise RuntimeError('host libssh2 artifact mismatch: ' + name)
    crypto = Path(record['crypto']['path'])
    if record['crypto']['sha256'] != sha(crypto):
        raise RuntimeError('host libssh2 crypto dependency changed; rebuild required')
    return record


def sections(path):
    """Hash Mach-O section contents, excluding mutable load commands/signatures.

    install_name_tool changes dylib IDs/dependency paths; codesign changes
    LINKEDIT. All file-backed sections (code, constants, data) must stay equal.
    """
    data = path.read_bytes()
    if len(data) < 32 or struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise RuntimeError('host libssh2 package must contain a thin 64-bit Mach-O')
    count = struct.unpack_from('<I', data, 16)[0]
    cursor = 32
    hashes = []
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, cursor)
        if size < 8 or cursor + size > len(data):
            raise RuntimeError('invalid Mach-O load command')
        if command == 0x19:
            nsects = struct.unpack_from('<I', data, cursor + 64)[0]
            if 72 + nsects * 80 > size:
                raise RuntimeError('invalid Mach-O section table')
            for index in range(nsects):
                section = cursor + 72 + index * 80
                name, segment, address, length, offset = struct.unpack_from('<16s16sQQI', data, section)
                flags = struct.unpack_from('<I', data, section + 64)[0] & 0xff
                if flags in (1, 0xc, 0x12):  # zero-fill sections have no file bytes
                    continue
                if offset + length > len(data):
                    raise RuntimeError('invalid Mach-O section range')
                hashes.append((segment.hex(), name.hex(), length,
                               hashlib.sha256(data[offset:offset + length]).hexdigest()))
        cursor += size
    if not hashes:
        raise RuntimeError('host libssh2 package has no file-backed sections')
    return hashes


def portable_record(record):
    result = dict(record)
    result['crypto'] = {'sha256': record['crypto']['sha256']}
    return result


def stage_bundle(app, prefix, root):
    record = verify(prefix, root)
    resources = app / 'Contents/Resources'
    (resources / 'licenses').mkdir(parents=True, exist_ok=True)
    (resources / 'libssh2-receipt.json').write_text(json.dumps(portable_record(record), indent=2) + '\n')
    (resources / 'licenses/libssh2.txt').write_bytes((prefix / 'share/licenses/libssh2.txt').read_bytes())


def check_bundle(app, prefix, root):
    record = verify(prefix, root)
    bundled = app / 'Contents/Frameworks/libssh2.1.dylib'
    if sections(bundled) != sections(prefix / 'lib/libssh2.dylib'):
        raise RuntimeError('packaged libssh2 sections differ from the verified patched library')
    receipt = app / 'Contents/Resources/libssh2-receipt.json'
    if json.loads(receipt.read_text()) != portable_record(record):
        raise RuntimeError('packaged libssh2 receipt mismatch')
    if (app / 'Contents/Resources/licenses/libssh2.txt').read_bytes() != (prefix / 'share/licenses/libssh2.txt').read_bytes():
        raise RuntimeError('packaged libssh2 license mismatch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--app', type=Path)
    modes.add_argument('--stage-app', type=Path)
    args = parser.parse_args()
    if args.stage_app:
        stage_bundle(args.stage_app, args.prefix, args.root)
    elif args.app:
        check_bundle(args.app, args.prefix, args.root)
    else:
        verify(args.prefix, args.root)
    print('HOST LIBSSH2 VERIFIED')


if __name__ == '__main__':
    try:
        main()
    except (OSError, KeyError, ValueError, RuntimeError, struct.error) as error:
        print('host libssh2 verification failed: ' + str(error), file=sys.stderr)
        sys.exit(1)
