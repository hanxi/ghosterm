#!/usr/bin/env python3
"""P0: identical bytes on real Apple full surface / Android VT, without Client.

No production source edits. --android-build compiles only; --android SERIAL
executes on that chosen device. Requested missing dependencies fail, no SKIP.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PIN = '81598385d608dda4946093d6d6a234d204a2f225'
NDK = '28.2.13676358'
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('plugin_carrier', ROOT / 'scripts/test-plugin-bridge.py')
carrier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(carrier)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked(args, limit=120, **kwargs):
    result = carrier.timed(args, limit=limit, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError('command failed: ' + repr(result))
    return result


def fixtures(work):
    cases = []
    def add(name, content, expected='BEFOREAFTER', chunk=8192, prefix=''):
        file = name + '.bin'
        (work / file).write_bytes(content)
        cases.append(dict(name=name, file=file, expected=expected, chunk=chunk, replyPrefix=prefix))
    raw = bytes(range(256)) * 3
    add('ordinary', b'BEFOREAFTER')
    add('bridge-complete', b'BEFORE' + carrier.frame(b'probe') + b'AFTER')
    add('bridge-max-fragment', b'BEFORE' + carrier.frame(raw) + b'AFTER')
    add('bridge-byte-fragments', b'BEFORE' + carrier.frame(raw) + b'AFTER', chunk=1)
    add('unknown-dcs', b'BEFORE\x1bP+unrelated;not a bridge\x1b\\AFTER', chunk=3)
    add('unknown-osc', b'BEFORE\x1b]777;not a bridge\x1b\\AFTER', chunk=2)
    add('bridge-malformed-body', b'BEFORE' + carrier.PREFIX + b'not-hex' + carrier.ST + b'AFTER')
    add('bridge-can-abort', b'BEFORE' + carrier.PREFIX + b'0011\x18AFTER', chunk=1)
    # Demonstrate the actual generic-terminal failure mode of an incomplete
    # control string: following text is inside DCS until an explicit ST.
    add('truncated-until-st', b'BEFORE' + carrier.PREFIX + b'0011AFTER' + carrier.ST + b'RECOVER',
        expected='BEFORERECOVER', chunk=1)
    # Official OSC 5113 encoding example; this is NOT a transfer receiver.
    add('kitty-file-osc', b'BEFORE\x1b]5113;ac=send;id=test;n=c29tZWZpbGU=;sz=3;d=AQID\x1b\\AFTER')
    add('kitty-graphics-control', b'BEFORE\x1b_Ga=q,i=41,f=24,s=1,v=1;AAAA\x1b\\AFTER',
        prefix='\x1b_Gi=41;OK\x1b\\')
    (work / 'cases.json').write_text(json.dumps(cases, indent=2) + '\n')
    return cases


def mac_probe(work, cases):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise RuntimeError('real Apple Silicon full libghostty required')
    sdk = ROOT / 'build/GhosttyKit.xcframework'
    receipt = json.loads((sdk / 'terminal-engine.json').read_text())
    archive = sdk / 'macos-arm64/libghostty-internal.a'
    if receipt['engine'] != PIN or receipt['archive_sha256'] != sha(archive):
        raise RuntimeError('full engine pin/archive receipt mismatch')
    for name, digest in receipt['patches'].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError('full engine patch receipt mismatch/' + name)
    package = work / 'apple/GhostermTerminal'
    shutil.copytree(ROOT / 'apple/GhostermTerminal', package, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
    for name in ('core', 'build'):
        (work / name).symlink_to(ROOT / name, target_is_directory=True)
    probe = work / 'probe'
    (probe / 'Sources/Probe').mkdir(parents=True)
    shutil.copyfile(ROOT / 'scripts/probes/PluginNoClient.swift', probe / 'Sources/Probe/main.swift')
    (probe / 'Package.swift').write_text('''// swift-tools-version:5.9
import PackageDescription
let package = Package(name: "PluginNoClient", platforms: [.macOS(.v13)],
    dependencies: [.package(path: "../apple/GhostermTerminal")],
    targets: [.executableTarget(name: "Probe", dependencies: [
        .product(name: "GhostermTerminal", package: "GhostermTerminal")])])
''')
    environment = dict(os.environ, TMPDIR=str(work), PROBE_INPUT=str(work),
                       CLANG_MODULE_CACHE_PATH=str(work / 'clang-cache'),
                       SWIFTPM_MODULECACHE_OVERRIDE=str(work / 'swift-modules'))
    args = ['swift', 'build', '--package-path', probe, '--scratch-path', work / 'swift-build']
    for option in ('cache', 'config', 'security'):
        args += ['--' + option + '-path', work / ('swift-' + option)]
    checked(args, env=environment, limit=300)
    binary = work / 'swift-build/debug/Probe'
    result = checked([binary], env=environment, limit=90)
    lines = result.stdout.decode().splitlines()
    if any('PASS ' + case['name'] not in lines for case in cases) or f'RESULT cases={len(cases)} failures=0' not in lines:
        raise RuntimeError('full surface assertions missing: ' + repr(result))
    print(result.stdout.decode(), end='', flush=True)
    return {'engine': receipt, 'cases': len(cases), 'binarySHA256': sha(binary), 'macOS': platform.mac_ver()[0]}


def android_probe(work, cases, serial):
    prefix = ROOT / 'build/android-native/arm64-v8a'
    receipt = json.loads((prefix / 'receipt.json').read_text())
    archive = prefix / 'lib/libghostty-vt.a'
    if receipt['inputs']['ghostty'] != PIN or receipt['sha256']['libghostty-vt.a'] != sha(archive):
        raise RuntimeError('VT archive receipt mismatch')
    for name, digest in receipt['inputs']['vt_patches'].items():
        if sha(ROOT / 'patches/libghostty/android-vt-8159838' / name) != digest:
            raise RuntimeError('VT patch receipt mismatch/' + name)
    if receipt['sha256']['terminal.h'] != sha(prefix / 'include/ghostty/vt/terminal.h'):
        raise RuntimeError('VT terminal header receipt mismatch')
    sdk = Path(os.environ.get('ANDROID_HOME', ROOT / 'build/android-toolchain/sdk'))
    host = 'darwin-x86_64' if platform.system() == 'Darwin' else 'linux-x86_64'
    cc = sdk / 'ndk' / NDK / 'toolchains/llvm/prebuilt' / host / 'bin/aarch64-linux-android29-clang'
    binary = work / 'vt-probe'
    checked([cc, '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', prefix / 'include',
             ROOT / 'scripts/probes/plugin-no-client-vt.c', archive, '-lm', '-pthread', '-o', binary])
    report = {'engine': PIN, 'archiveSHA256': sha(archive), 'binarySHA256': sha(binary),
              'headerSHA256': {str(p.relative_to(prefix)): sha(p) for p in (prefix / 'include').rglob('*.h')},
              'ndk': NDK, 'minimumApi': 29, 'abi': 'arm64-v8a', 'executedCases': 0}
    if serial:
        adb = ['adb', '-s', serial]
        checked([*adb, 'get-state'])
        remote = '/data/local/tmp/ghosterm-' + work.name
        try:
            checked([*adb, 'shell', 'mkdir', remote])
            checked([*adb, 'push', binary, remote + '/probe'])
            checked([*adb, 'shell', 'chmod', '700', remote + '/probe'])
            for case in cases:
                checked([*adb, 'push', work / case['file'], remote + '/' + case['file']])
                command = [remote + '/probe', remote + '/' + case['file'], case['expected'], str(case['chunk']),
                           case['name'], case['replyPrefix']]
                result = checked([*adb, 'shell', shlex.join(command)], limit=15)
                if result.stdout.strip() != ('PASS ' + case['name']).encode() or result.stderr:
                    raise RuntimeError('VT result mismatch: ' + repr(result))
                print('PASS Android VT', case['name'], flush=True)
            report.update(executedCases=len(cases), serial=serial,
                          api=checked([*adb, 'shell', 'getprop', 'ro.build.version.sdk']).stdout.decode().strip(),
                          pageBytes=checked([*adb, 'shell', 'getconf', 'PAGESIZE']).stdout.decode().strip())
        finally:
            checked([*adb, 'shell', 'rm', '-rf', remote])
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--android-build', action='store_true')
    parser.add_argument('--android', metavar='SERIAL')
    args = parser.parse_args()
    sources = [ROOT / 'scripts/probes/PluginNoClient.swift', ROOT / 'scripts/probes/plugin-no-client-vt.c',
               Path(__file__).resolve(), ROOT / 'scripts/test-plugin-bridge.py']
    sources += [path for path in (ROOT / 'apple/GhostermTerminal').rglob('*') if path.is_file() and
                '.build' not in path.parts and '.swiftpm' not in path.parts]
    hashes = {str(path.relative_to(ROOT)): sha(path) for path in sources}
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='plugin-no-client-', dir=ROOT / 'tmp') as temp:
        work = Path(temp)
        os.environ['TMPDIR'] = str(work)
        cases = fixtures(work)
        report = {'productionBridge': False, 'inputSHA256': hashes,
                  'fixtureSHA256': {case['name']: sha(work / case['file']) for case in cases},
                  'mac': mac_probe(work, cases)}
        if args.android or args.android_build:
            report['android'] = android_probe(work, cases, args.android)
        if any(sha(ROOT / path) != digest for path, digest in hashes.items()):
            raise RuntimeError('input changed during probe')
    print(json.dumps(report, sort_keys=True), flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
