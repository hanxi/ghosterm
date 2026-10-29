#!/usr/bin/env python3
"""Build pinned Android dependencies in tmp/; never compile against host libraries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PIN = '81598385d608dda4946093d6d6a234d204a2f225'
NDK = '28.2.13676358'
VT_PATCH = ROOT / 'patches/libghostty/android-vt-8159838/0001-link-android-libc.patch'
SOURCES = {
    'openssl-3.5.4': ('https://www.openssl.org/source/openssl-3.5.4.tar.gz',
                      '967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99'),
    'libssh2-1.11.1': ('https://libssh2.org/download/libssh2-1.11.1.tar.gz',
                       'd9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7'),
}


def run(command, **kwargs):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def source(name, work):
    url, digest = SOURCES[name]
    archive = work / (name + '.tar.gz')
    if not archive.exists():
        partial = archive.with_suffix('.download')
        urllib.request.urlretrieve(url, partial)
        partial.rename(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise RuntimeError(f'{name}: source SHA256 mismatch; remove {archive} and retry')
    path = work / name
    if not path.exists():
        with tarfile.open(archive) as handle:
            handle.extractall(work, filter='data')
    return path


def verify_vt_libc(archive, nm):
    symbols = subprocess.check_output([str(nm), '--defined-only', '--just-symbol-name', str(archive)], text=True)
    if {'calloc', 'free'} & set(symbols.splitlines()):
        raise RuntimeError('VT archive defines libc allocator stubs; Android must link system libc')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--abi', choices=['arm64-v8a'], default='arm64-v8a')
    args = parser.parse_args()
    sdk = Path(os.environ.get('ANDROID_HOME', ROOT / 'build/android-toolchain/sdk')).resolve()
    ndk = sdk / 'ndk' / NDK
    if not (ndk / 'build/cmake/android.toolchain.cmake').is_file():
        raise RuntimeError(f'Install Android NDK {NDK} under {sdk}')
    host = 'darwin-x86_64' if os.uname().sysname == 'Darwin' else 'linux-x86_64'
    toolchain = ndk / 'toolchains/llvm/prebuilt' / host
    zig = os.environ.get('ZIG') or shutil.which('zig') or str(ROOT / 'tmp/zig/zig-aarch64-macos-0.16.0/zig')
    if subprocess.check_output([zig, 'version'], text=True).strip() != '0.16.0':
        raise RuntimeError('Zig 0.16.0 required')
    vendor = ROOT / 'vendor/ghostty'
    if subprocess.check_output(['git', '-C', vendor, 'rev-parse', 'HEAD'], text=True).strip() != PIN:
        raise RuntimeError('Ghostty pin mismatch')
    run(['git', '-C', vendor, 'diff', '--quiet', 'HEAD', '--'])
    work = ROOT / 'tmp/android-native' / args.abi
    prefix = ROOT / 'build/android-native' / args.abi
    work.mkdir(parents=True, exist_ok=True)
    prefix.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(work), ANDROID_NDK_HOME=str(ndk),
                       ANDROID_NDK_ROOT=str(ndk), PATH=str(toolchain / 'bin') + os.pathsep + os.environ['PATH'],
                       ZIG_LOCAL_CACHE_DIR=str(work / 'zig-cache'), ZIG_GLOBAL_CACHE_DIR=str(ROOT / 'tmp/zig-global'))
    # Do not let Apple SDK or host pkg-config leak into the device build.
    for key in ('SDKROOT', 'CPATH', 'LIBRARY_PATH', 'PKG_CONFIG_PATH', 'CFLAGS', 'LDFLAGS'):
        environment.pop(key, None)
    signature = {'ghostty': PIN, 'ndk': NDK, 'api': 29, 'abi': args.abi, 'zig': '0.16.0',
                 'sources': SOURCES, 'vt_patch': hashlib.sha256(VT_PATCH.read_bytes()).hexdigest(), 'builder': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    receipt = prefix / 'receipt.json'
    licenses = ROOT / 'build/android-native/assets/licenses'
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / 'LICENSE', licenses / 'Ghosterm-MIT.txt')
    shutil.copyfile(vendor / 'LICENSE', licenses / 'Ghostty-MIT.txt')
    for name, filename in [('openssl-3.5.4', 'LICENSE.txt'), ('libssh2-1.11.1', 'COPYING')]:
        shutil.copyfile(source(name, work) / filename, licenses / (name + '.txt'))
    artifacts = [prefix / 'lib' / n for n in ('libcrypto.a', 'libssh2.a', 'libghostty-vt.a')]
    if receipt.exists() and all(p.exists() for p in artifacts):
        saved = json.loads(receipt.read_text())
        if saved.get('inputs') == json.loads(json.dumps(signature)) and all(
            saved.get('sha256', {}).get(p.name) == hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts):
            verify_vt_libc(prefix / 'lib/libghostty-vt.a', toolchain / 'bin/llvm-nm')
            print('Android native dependencies verified (cached)', flush=True)
            return
    openssl = source('openssl-3.5.4', work)
    run(['./Configure', 'android-arm64', '-D__ANDROID_API__=29', 'no-shared', 'no-tests',
         'no-apps', 'no-docs', 'no-module', '--openssldir=/nonexistent/ghosterm', '--prefix=' + str(prefix), '--libdir=lib'], cwd=openssl, env=environment)
    run(['make', '-j4', 'build_libs'], cwd=openssl, env=environment)
    run(['make', 'install_dev'], cwd=openssl, env=environment)
    ssh2 = source('libssh2-1.11.1', work)
    run(['cmake', '-S', ssh2, '-B', work / 'ssh2-build',
         '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build/cmake/android.toolchain.cmake'),
         '-DANDROID_ABI=' + args.abi, '-DANDROID_PLATFORM=android-29',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
         '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
         '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_EXAMPLES=OFF', '-DBUILD_TESTING=OFF',
         '-DCRYPTO_BACKEND=OpenSSL', '-DENABLE_ZLIB_COMPRESSION=OFF',
         '-DOPENSSL_INCLUDE_DIR=' + str(prefix / 'include'),
         '-DOPENSSL_CRYPTO_LIBRARY=' + str(prefix / 'lib/libcrypto.a'),
         '-DOPENSSL_SSL_LIBRARY=' + str(prefix / 'lib/libssl.a')], env=environment)
    run(['cmake', '--build', work / 'ssh2-build', '-j', '4'], env=environment)
    run(['cmake', '--install', work / 'ssh2-build'], env=environment)
    engine = work / 'ghostty'
    if engine.exists():
        shutil.rmtree(engine)
    engine.mkdir()
    archive = work / 'ghostty.tar'
    with archive.open('wb') as output:
        run(['git', '-C', vendor, 'archive', PIN], stdout=output)
    with tarfile.open(archive) as handle:
        handle.extractall(engine, filter='data')
    run(['patch', '--batch', '--forward', '-p1', '-i', VT_PATCH], cwd=engine)
    run([zig, 'build', '-Demit-lib-vt=true', '-Demit-xcframework=false', '-Demit-docs=false',
         '-Dtarget=aarch64-linux-android.29', '-Doptimize=ReleaseFast', '-Dsimd=false',
         '-p', prefix], cwd=engine, env=environment)
    verify_vt_libc(prefix / 'lib/libghostty-vt.a', toolchain / 'bin/llvm-nm')
    receipt.write_text(json.dumps({'inputs': signature, 'sha256': {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}}, indent=2) + '\n')
    print('ANDROID NATIVE PASS:', prefix)


if __name__ == '__main__':
    main()
