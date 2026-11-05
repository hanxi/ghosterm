#!/usr/bin/env python3
"""P0 engine comparison, isolated from production builds. Explicit --fetch only.

Compile both pinned engines with the same C host probes; optionally cross-build
Android API 29 arm64 and execute on an explicitly selected adb device.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'scripts/probes/plugin-js.c'
MARK = 'GT_PLUGIN_JS_MUTATION'
NDK = '28.2.13676358'
ENGINES = {
    'quickjs': {
        'version': '2026-06-04', 'directory': 'quickjs-2026-06-04',
        'archive': 'quickjs-2026-06-04.tar.xz',
        'url': 'https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz',
        'sha256': 'b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a',
        'upstreamDialect': 'gnu11',
        'sources': ['quickjs.c', 'dtoa.c', 'libregexp.c', 'libunicode.c', 'cutils.c'],
    },
    'quickjs-ng': {
        'version': '0.18.0', 'directory': 'quickjs-0.18.0',
        'archive': 'quickjs-ng-v0.18.0.tar.gz',
        'url': 'https://codeload.github.com/quickjs-ng/quickjs/tar.gz/refs/tags/v0.18.0',
        'sha256': '28bcd89aa3683fd2a946537e5ce3f11cc46b5cbf3383c7561aaef3df5ff9f1dd',
        'upstreamDialect': 'c11',
        'sources': ['quickjs.c', 'dtoa.c', 'libregexp.c', 'libunicode.c'],
    },
}
CASES = ('portable', 'modules', 'promise', 'job-budget', 'interrupt', 'cancel',
         'memory', 'stack', 'gc', 'isolation', 'native-io')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def timed(args, limit=120, **kwargs):
    command = ['bash', '-c', 'source "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require; shift; gt_timeout "$@"',
               '_', str(ROOT), str(limit)] + [str(x) for x in args]
    result = subprocess.run(command, capture_output=True, **kwargs)
    return result


def checked(args, **kwargs):
    result = timed(args, **kwargs)
    require(result.returncode == 0, 'command failed: ' + repr(result))
    return result


def unpack(engine, work, fetch):
    cache = ROOT / 'tmp/plugin-js-sources'
    cache.mkdir(exist_ok=True)
    archive = cache / engine['archive']
    if not archive.exists():
        require(fetch, 'missing pinned source; rerun with --fetch: ' + str(archive))
        partial = archive.with_suffix('.download')
        try:
            with urllib.request.urlopen(engine['url'], timeout=30) as response, partial.open('wb') as out:
                shutil.copyfileobj(response, out)
            require(sha(partial) == engine['sha256'], 'download SHA256 mismatch')
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    require(sha(archive) == engine['sha256'], 'cached source SHA256 mismatch')
    # Never trust an extracted cache or modify vendor: fresh verified source.
    with tarfile.open(archive) as package:
        package.extractall(work, filter='data')
    source = work / engine['directory']
    require((source / 'LICENSE').is_file(), 'upstream license missing')
    return source


def compiler(android):
    if not android:
        return shutil.which('cc')
    sdk = Path(os.environ.get('ANDROID_HOME', ROOT / 'build/android-toolchain/sdk'))
    host = 'darwin-x86_64' if platform.system() == 'Darwin' else 'linux-x86_64'
    binary = sdk / 'ndk' / NDK / 'toolchains/llvm/prebuilt' / host / 'bin/aarch64-linux-android29-clang'
    require(binary.is_file(), 'pinned Android NDK compiler missing: ' + str(binary))
    return str(binary)


def build_engine(name, source, work, android):
    cc = compiler(android)
    flags = ['-std=c11', '-O2', '-g', '-fwrapv', '-D_GNU_SOURCE',
             '-DCONFIG_VERSION="' + ENGINES[name]['version'] + '"', '-isystem', source]
    if not android:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    target = work / ('android' if android else 'host')
    target.mkdir()
    objects = []
    # Upstream remains unpatched and uses its own warning policy. Our host
    # translation unit is separately compiled with -Wall -Wextra -Werror.
    for filename in ENGINES[name]['sources']:
        obj = target / (filename + '.o')
        upstream_flags = ['-std=' + ENGINES[name]['upstreamDialect'], *flags[1:]]
        checked([cc, *upstream_flags, '-c', source / filename, '-o', obj], limit=300)
        objects.append(obj)
    return cc, flags, objects, target


def build_probe(name, input_source, cc, flags, objects, target):
    obj = target / 'probe.o'
    binary = target / 'probe'
    define = ['-DPROBE_NG'] if name == 'quickjs-ng' else []
    checked([cc, *flags, *define, '-Wall', '-Wextra', '-Werror', '-c', input_source, '-o', obj])
    checked([cc, *flags, obj, *objects, '-lm', '-pthread', '-o', binary])
    return binary


def run_case(binary, case, adb=None):
    args = [*adb, 'shell', binary, case] if adb else [binary, case]
    result = timed(args, limit=15)
    expected = ('PASS ' + case).encode()
    require(result.returncode == 0 and result.stdout.strip() == expected and not result.stderr,
            'case failed/' + case + ': ' + repr(result))


def mutations(name, original, setup):
    cc, flags, objects, target = setup
    cases = (
        ('memory', 'JS_SetMemoryLimit(o->rt, 16 * 1024 * 1024);',
         'JS_SetMemoryLimit(o->rt, 128 * 1024 * 1024);', 'memory limit failed'),
        ('job-budget', 'if (jobs++ >= budget) return 1;',
         'if (jobs++ >= budget) return 0;', 'job budget not enforced'),
        ('modules', 'return js_strdup(ctx, "plugin:/shared.js");',
         'return NULL;', 'allowed module evaluation failed'),
        ('gc', 'JS_RunGC(o.rt);', '(void)0;', 'GC did not finalize owned cycle'),
    )
    copy = target / 'probe.c'
    digest = hashlib.sha256(original).hexdigest()
    for case, old, new, message in cases:
        text = original.decode()
        require(text.count(old) == 1, 'mutation anchor drift/' + case)
        try:
            copy.write_text(text.replace(old, new) + '\n/* ' + MARK + ' */\n')
            binary = build_probe(name, copy, cc, flags, objects, target)
            result = timed([binary, case], limit=15)
            require(result.returncode == 1 and result.stderr == ('ASSERT: ' + message + '\n').encode(),
                    'mutation did not hit named assertion: ' + repr(result))
            run_case(binary, 'portable')
        finally:
            copy.write_bytes(original)
            require(sha(copy) == digest and sha(SOURCE) == digest, 'restore SHA256 mismatch')
        binary = build_probe(name, copy, cc, flags, objects, target)
        run_case(binary, case)
        print('PASS', name, 'mutation', case, 'assertion red / portable green / restored green / SHA256', flush=True)
    return len(cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--android-build', action='store_true')
    parser.add_argument('--android', metavar='SERIAL', help='also build and run both Android probes')
    args = parser.parse_args()
    original = SOURCE.read_bytes()
    require(MARK.encode() not in original, 'mutation marker found before start')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    report = {'production': False, 'host': platform.platform(), 'cases': list(CASES),
              'inputSHA256': {str(path.relative_to(ROOT)): sha(path) for path in (SOURCE, Path(__file__))}, 'engines': {}}
    with tempfile.TemporaryDirectory(prefix='plugin-js-', dir=ROOT / 'tmp') as temp:
        work = Path(temp)
        os.environ['TMPDIR'] = str(work)
        snapshot = work / 'probe-original.c'
        snapshot.write_bytes(original)
        for name, engine in ENGINES.items():
            directory = work / name
            directory.mkdir()
            source = unpack(engine, directory, args.fetch)
            setup = build_engine(name, source, directory, False)
            binary = build_probe(name, snapshot, *setup)
            for case in CASES:
                run_case(binary, case)
                print('PASS', name, 'host', case, flush=True)
            entry = {'version': engine['version'], 'sourceSHA256': engine['sha256'],
                     'upstreamDialect': engine['upstreamDialect'], 'probeDialect': 'c11',
                     'licenseSHA256': sha(source / 'LICENSE'), 'hostCases': len(CASES),
                     'hostBinaryBytes': binary.stat().st_size, 'hostBinarySHA256': sha(binary),
                     'mutations': mutations(name, original, setup) if args.mutate else 0}
            if args.android_build or args.android:
                android_setup = build_engine(name, source, directory, True)
                binary = build_probe(name, snapshot, *android_setup)
                entry['android'] = {'ndk': NDK, 'abi': 'arm64-v8a', 'minimumApi': 29,
                                    'bytes': binary.stat().st_size, 'sha256': sha(binary), 'executedCases': 0}
                if args.android:
                    adb = ['adb', '-s', args.android]
                    checked([*adb, 'get-state'])
                    # Device OS requires executable files in its shell-writable
                    # data area; this is Android /data/local/tmp, never host /tmp.
                    remote = '/data/local/tmp/ghosterm-' + work.name + '-' + name
                    try:
                        checked([*adb, 'push', binary, remote])
                        checked([*adb, 'shell', 'chmod', '700', remote])
                        for case in CASES:
                            run_case(remote, case, adb)
                            print('PASS', name, args.android, case, flush=True)
                        entry['android'].update(executedCases=len(CASES), serial=args.android,
                            api=checked([*adb, 'shell', 'getprop', 'ro.build.version.sdk']).stdout.decode().strip(),
                            pageBytes=checked([*adb, 'shell', 'getconf', 'PAGESIZE']).stdout.decode().strip())
                    finally:
                        checked([*adb, 'shell', 'rm', '-f', remote])
            report['engines'][name] = entry
        require(sha(SOURCE) == hashlib.sha256(original).hexdigest(), 'source changed during run')
    print(json.dumps(report, sort_keys=True), flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
