#!/usr/bin/env python3
"""Isolated Go->C ABI research. Does not change ghosterm.h or App builds.

Uses the P0 TailPocket source/module pins. Go networking is linked into a C
executable; the runtime experiment includes a local DERP/STUN reference server.
Artifact sizes therefore describe this research library, not App size growth.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SELF = Path(__file__).resolve()
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('tailpocket_reference_pin', ROOT / 'scripts/test-plugin-tailpocket.py')
ref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ref)
SOURCES = [ROOT / ('scripts/probes/plugin-go-abi.' + suffix) for suffix in ('go', 'h', 'c')]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, env, cwd=None, timeout=600, expected=0):
    result = ref.bridge.timed([str(x) for x in args], cwd=cwd, env=env,
                              capture_output=True, limit=timeout)
    if result.returncode != expected:
        raise AssertionError('ABI research command failed: ' + result.stderr.decode(errors='replace')[-4000:])
    return result


def make_source(work, env, fetch):
    cache = ROOT / 'tmp/plugin-tailpocket-sources'
    cache.mkdir(exist_ok=True)
    archive = cache / ('tailpocket-' + ref.COMMIT + '.tar.gz')
    if not archive.exists():
        if not fetch: raise RuntimeError('pinned TailPocket source missing; use --fetch')
        with urllib.request.urlopen('https://codeload.github.com/jt-wang/tailpocket/tar.gz/' + ref.COMMIT,
                                    timeout=30) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != ref.ARCHIVE_SHA: raise RuntimeError('upstream SHA256 mismatch')
        archive.write_bytes(data)
    if sha(archive) != ref.ARCHIVE_SHA: raise RuntimeError('cached upstream SHA256 mismatch')
    with tarfile.open(archive) as package: package.extractall(work, filter='data')
    source = work / ('tailpocket-' + ref.COMMIT)
    originals = {str(p.relative_to(source)): sha(p) for p in source.rglob('*') if p.is_file()}
    build = work / 'module'; build.mkdir()
    (build / 'go.mod').write_text((ref.TOOLS / 'go.mod').read_text() +
                                '\nreplace github.com/jt-wang/tailpocket => ' + str(source) + '\n')
    shutil.copyfile(ref.TOOLS / 'go.sum', build / 'go.sum')
    for p in SOURCES[:2]: shutil.copyfile(p, build / p.name)
    if fetch:
        online = dict(env, GOTOOLCHAIN='go1.27.3', GOPROXY='https://proxy.golang.org', GOSUMDB='sum.golang.org')
        run(['go', 'mod', 'download'], online, cwd=build)
    tools = list(Path(env['GOMODCACHE']).glob('golang.org/toolchain@v0.0.1-go1.27.3.*/bin/go'))
    if len(tools) != 1: raise RuntimeError('pinned Go toolchain missing; use --fetch')
    go = tools[0]
    run([go, 'mod', 'verify'], env, cwd=build)
    return go, build, source, originals


def build_go(go, build, env, output, mode):
    before = {name: sha(build / name) for name in ('go.mod', 'go.sum')}
    run([go, 'build', '-mod=readonly', '-trimpath', '-ldflags=-s -w', '-buildmode=' + mode,
         '-o', output, '.'], env, cwd=build)
    assert all(sha(build / name) == digest for name, digest in before.items()), 'module lock drift'


def c_link(args, env):
    run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-g0',
         '-I', ROOT / 'scripts/probes', *args], env)


def observe(binary, env, expected):
    result = run([binary], env, timeout=90)
    assert not result.stderr, 'runtime stderr is not empty'
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    assert rows[-1]['result'] == expected
    return rows


def apple(go, build, work, env):
    developer = Path('/Applications/Xcode.app/Contents/Developer')
    if not developer.exists(): raise RuntimeError('full Xcode missing')
    results = {}
    for name, sdk, triple in [('ios-arm64', 'iphoneos', 'arm64-apple-ios15.0'),
                              ('ios-simulator-arm64', 'iphonesimulator', 'arm64-apple-ios15.0-simulator')]:
        sdkpath = run(['xcrun', '--sdk', sdk, '--show-sdk-path'], dict(env, DEVELOPER_DIR=str(developer))).stdout.decode().strip()
        clang = developer / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
        wrapper = work / (name + '-cc')
        # All paths are fixed local tool paths, no user input or secrets.
        import shlex
        wrapper.write_text('#!/bin/sh\nexec ' + shlex.join([str(clang), '-isysroot', sdkpath, '-target', triple]) + ' "$@"\n')
        wrapper.chmod(0o700)
        target_env = dict(env, GOOS='ios', GOARCH='arm64', CC=str(wrapper), CGO_CFLAGS='', CGO_LDFLAGS='')
        archive = work / (name + '.a')
        print('BUILD', name, 'c-archive', flush=True)
        build_go(go, build, target_env, archive, 'c-archive')
        caller = work / (name + '-main.c')
        caller.write_text('#include "plugin-go-abi.h"\nint main(void) { return pgabi_new() == 0; }\n')
        binary = work / (name + '-link-smoke')
        run([wrapper, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'scripts/probes',
             caller, archive, '-framework', 'CoreFoundation', '-framework', 'Security', '-lresolv', '-o', binary], env)
        results[name] = dict(archiveBytes=archive.stat().st_size, linkedBytes=binary.stat().st_size,
                             runtimeExecuted=False, sdk=Path(sdkpath).name)
    return results


def android(go, build, work, env):
    ndk = ROOT / 'build/android-toolchain/sdk/ndk/28.2.13676358'
    tools = ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin'
    cc = tools / 'aarch64-linux-android29-clang'
    if not cc.exists(): raise RuntimeError('pinned Android NDK missing')
    target_env = dict(env, GOOS='android', GOARCH='arm64', CC=str(cc), CGO_CFLAGS='',
                      CGO_LDFLAGS='-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384')
    unsupported = ref.bridge.timed([str(go), 'build', '-mod=readonly', '-buildmode=c-archive', '-o', str(work / 'unsupported.a'), '.'],
                                  env=target_env, cwd=build, capture_output=True, limit=30)
    assert unsupported.returncode != 0 and b'not supported' in unsupported.stderr, 'Android c-archive unexpectedly supported'
    library = work / 'libpgabi-android.so'
    print('BUILD android-arm64 c-shared, API 29 / NDK 28.2', flush=True)
    build_go(go, build, target_env, library, 'c-shared')
    headers = run([tools / 'llvm-readelf', '-lW', library], env).stdout.decode()
    loads = [line for line in headers.splitlines() if line.strip().startswith('LOAD')]
    assert loads and all(int(line.split()[-1], 16) >= 16384 for line in loads), 'Android ELF alignment below 16 KiB'
    caller = work / 'android-main.c'
    caller.write_text('#include "plugin-go-abi.h"\nint main(void) { return pgabi_new() == 0; }\n')
    executable = work / 'android-link-smoke'
    run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', ROOT / 'scripts/probes', caller,
         library, '-o', executable], env)
    return dict(libraryBytes=library.stat().st_size, linkedBytes=executable.stat().st_size,
                loadAlignmentBytes=[int(line.split()[-1], 16) for line in loads],
                cArchiveSupported=False, runtimeExecuted=False, ndk='28.2.13676358', api=29)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--apple', action='store_true', help='iOS device/simulator arm64 archive/link only')
    parser.add_argument('--android', action='store_true', help='NDK arm64 shared/link/alignment only')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    inputs = [SELF, *SOURCES, ref.SELF, ref.TOOLS / 'go.mod', ref.TOOLS / 'go.sum']
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    with tempfile.TemporaryDirectory(prefix='goabi-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        home = work / 'home'; home.mkdir()
        env = dict(os.environ, HOME=str(home), TMPDIR=str(work), GOTOOLCHAIN='local', GOENV='off',
                   GOPROXY='off', GOSUMDB='off', CGO_ENABLED='1', TS_DEBUG_ALWAYS_USE_DERP='true',
                   MACOSX_DEPLOYMENT_TARGET='13.0', CGO_CFLAGS='-mmacosx-version-min=13.0',
                   CGO_LDFLAGS='-mmacosx-version-min=13.0', CC='/usr/bin/clang', GOTRACEBACK='none')
        for variable, name in [('GOTMPDIR','go-tmp'),('GOCACHE','go-cache'),('GOMODCACHE','go-modules'),('GOPATH','go-path')]:
            path=ROOT / ('tmp/plugin-tailpocket-'+name); path.mkdir(exist_ok=True); env[variable]=str(path)
        for name in ('TMUX','TMUX_PANE','SSH_AUTH_SOCK','SSH_CONNECTION','SSH_CLIENT','SSH_TTY'):
            env.pop(name, None)
        go, build, source, originals = make_source(work, env, args.fetch)
        archive = work / 'libpgabi.a'
        print('BUILD macOS arm64 c-archive and C11 caller', flush=True)
        build_go(go, build, env, archive, 'c-archive')
        binary = work / 'c-caller'
        baseline = work / 'c-baseline'
        c_link([SOURCES[2], archive, '-framework', 'CoreFoundation', '-framework', 'Security', '-lresolv', '-o', binary], env)
        c_link(['-DPGABI_BASELINE', SOURCES[2], '-o', baseline], env)
        # Toolchains may write non-secret metadata under their isolated HOME.
        # Give runtime its own empty HOME so the no-key-file assertion cannot
        # confuse Xcode/Go build metadata with networking-library persistence.
        runtime_home = work / 'runtime-home'; runtime_home.mkdir()
        runtime_env = dict(env, HOME=str(runtime_home), ZDOTDIR=str(runtime_home))
        baseline_rows = observe(baseline, runtime_env, 'baseline')
        rows = observe(binary, runtime_env, 'pass')
        print('PASS C main -> Go C ABI: buffers, real paired DERP roundtrip, cancel/close from C threads, stale handles', flush=True)
        report = dict(production=False, ghostermPublicABIChanged=False, helperProcess=False,
                      upstreamCommit=ref.COMMIT, inputSHA256=hashes,
                      measurementsIncludeLocalRelay=True,
                      macOS=dict(mode='c-archive', archiveBytes=archive.stat().st_size,
                                 executableBytes=binary.stat().st_size, baselineBytes=baseline.stat().st_size,
                                 baseline=baseline_rows, observations=rows))
        if args.apple: report['appleBuilds'] = apple(go, build, work, env)
        if args.android: report['androidBuild'] = android(go, build, work, env)
        assert all(sha(source / name) == digest for name,digest in originals.items()), 'upstream source drift'
        for p in runtime_home.rglob('*'):
            assert not p.is_file(), 'runtime unexpectedly wrote HOME files'
        report['isolatedBuildHomeFiles'] = [str(p.relative_to(home)) for p in home.rglob('*') if p.is_file()]
    assert all(sha(p)==hashes[str(p.relative_to(ROOT))] for p in inputs), 'probe source drift'
    print(json.dumps(report, sort_keys=True), flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame): raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM): signal.signal(number, interrupted)
    main()
