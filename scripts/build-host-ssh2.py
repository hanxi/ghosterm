#!/usr/bin/env python3
"""Build pinned patched host libssh2. Source downloads require explicit --fetch."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ssh2_receipt', ROOT / 'core/cmake/verify-ssh2.py')
receipt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receipt)


def run(command, **kwargs):
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    prefix = ROOT / 'build/host-native'
    work = ROOT / 'tmp/host-ssh2'
    work.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(work))
    crypto_dir = Path(subprocess.check_output(['pkg-config', '--variable=libdir', 'libcrypto'], text=True).strip())
    crypto = (crypto_dir / ('libcrypto.3.dylib' if sys.platform == 'darwin' else 'libcrypto.so')).resolve(strict=True)
    crypto_prefix = subprocess.check_output(['pkg-config', '--variable=prefix', 'libcrypto'], text=True).strip()
    identity = {'platform': sys.platform, 'machine': platform.machine(), 'crypto': {
        'path': str(crypto), 'sha256': receipt.sha(crypto)}}
    try:
        saved = receipt.verify(prefix, ROOT)
        if all(saved.get(key) == value for key, value in identity.items()):
            print('HOST LIBSSH2 PASS (verified cache)')
            return
    except (OSError, ValueError, KeyError, RuntimeError):
        pass
    archive = work / 'libssh2-1.11.1.tar.gz'
    if not archive.exists():
        for candidate in (ROOT / 'tmp/android-native/arm64-v8a/libssh2-1.11.1.tar.gz',
                          ROOT / 'tmp/libssh2-sources/libssh2-1.11.1.tar.gz'):
            if candidate.exists() and receipt.sha(candidate) == receipt.SOURCE_SHA:
                shutil.copyfile(candidate, archive)
                break
        else:
            if not args.fetch:
                raise RuntimeError('Pinned libssh2 source missing; run python3 scripts/build-host-ssh2.py --fetch')
            partial = work / 'libssh2.download'
            urllib.request.urlretrieve('https://libssh2.org/download/libssh2-1.11.1.tar.gz', partial)
            if receipt.sha(partial) != receipt.SOURCE_SHA:
                raise RuntimeError('Downloaded libssh2 SHA256 mismatch')
            partial.replace(archive)
    if receipt.sha(archive) != receipt.SOURCE_SHA:
        raise RuntimeError('Cached libssh2 SHA256 mismatch')
    source = work / 'libssh2-1.11.1'
    if source.exists():
        shutil.rmtree(source)
    with tarfile.open(archive) as package:
        package.extractall(work, filter='data')
    for name in receipt.PATCH_NAMES:
        run(['patch', '--batch', '--forward', '-p1', '-i', ROOT / 'patches/libssh2/1.11.1' / name], cwd=source)
    prefix.mkdir(parents=True, exist_ok=True)
    # A failed rebuild must not leave a valid-looking old receipt.
    (prefix / 'receipt.json').unlink(missing_ok=True)
    build = work / 'build'
    options = []
    if sys.platform == 'darwin':
        sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
        options = ['-DCMAKE_OSX_SYSROOT=' + sdk, '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0',
                   '-DCMAKE_INSTALL_NAME_DIR=' + str(prefix / 'lib')]
    run(['cmake', '-S', source, '-B', build, '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
         '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=ON',
         '-DBUILD_STATIC_LIBS=ON', '-DBUILD_EXAMPLES=OFF', '-DBUILD_TESTING=OFF',
         '-DENABLE_ZLIB_COMPRESSION=OFF', '-DCRYPTO_BACKEND=OpenSSL',
         '-DOPENSSL_ROOT_DIR=' + crypto_prefix, *options], env=environment)
    run(['cmake', '--build', build, '-j4'], env=environment)
    run(['cmake', '--install', build], env=environment)
    licenses = prefix / 'share/licenses'
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / 'COPYING', licenses / 'libssh2.txt')
    library = 'lib/libssh2.dylib' if sys.platform == 'darwin' else 'lib/libssh2.so'
    artifacts = (library, 'lib/libssh2.a', 'include/libssh2.h', 'include/libssh2_sftp.h',
                 'include/libssh2_publickey.h', 'share/licenses/libssh2.txt')
    saved = dict(identity, source_sha256=receipt.SOURCE_SHA,
                 patches={name: receipt.sha(ROOT / 'patches/libssh2/1.11.1' / name) for name in receipt.PATCH_NAMES},
                 builder=receipt.sha(Path(__file__)), sha256={name: receipt.sha(prefix / name) for name in artifacts})
    (prefix / 'receipt.json').write_text(json.dumps(saved, indent=2) + '\n')
    receipt.verify(prefix, ROOT)
    print('HOST LIBSSH2 PASS:', prefix)


if __name__ == '__main__':
    main()
