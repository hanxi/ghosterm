#!/usr/bin/env python3
"""Real directory protocol mutations, allocator observations, Swift binding and dependency guards."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_SFTP_BROWSE_MUTATION'


def main():
    production = [ROOT / 'core/src/sftp.c', ROOT / 'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    snapshots = {p: p.read_bytes() for p in production}
    if any(MARK.encode() in text for text in snapshots.values()): raise RuntimeError('Residual browse mutation')
    logs = ROOT / 'tmp/sftp-browse-mutations'; logs.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(command, label, red=None, extra=None):
        result = subprocess.run(['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require browse; shift; gt_timeout 300 "$@"',
                                 'browse', str(ROOT), *map(str, command)], env=dict(env, **(extra or {})),
                                capture_output=True, text=True, cwd=ROOT)
        output = result.stdout + result.stderr; (logs / (label + '.log')).write_text(output)
        okay = (result.returncode == 1 and red in output) if red else (result.returncode == 0 and not re.search(r'(?m)^SKIP|Test Case .* skipped', output))
        if not okay:
            print(output, flush=True)
            raise RuntimeError(label + ': expected ' + ('specific assertion red' if red else 'green'))
        return output
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-browse-', dir=ROOT / 'tmp') as directory:
            work = Path(directory); (work / 'tmp').mkdir()
            shutil.copytree(ROOT / 'core', work / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
            build = work / 'core/build'; source = work / 'core/src/sftp.c'; clean = source.read_text()
            run(['cmake', '-S', work / 'core', '-B', build], 'configure')
            def compile_core():
                run(['cmake', '--build', build, '--clean-first', '--target', 'test_sftp_browse', 'test_api', 'test_config'], 'core-build')
            wrapper = work / 'protocol'
            wrapper.write_text('#!/bin/bash\nexec ' + shlex.join([str(ROOT / 'build/ssh-channel-toolchain/bin/python'), str(ROOT / 'scripts/test-sftp-browse.py')]) + ' "$@"\n')
            wrapper.chmod(0o700)
            def protocol(case, label, red=None, binary=None):
                result = run(['bash', ROOT / 'core/tests/scripts/ssh_e2e.sh', wrapper], label, red,
                             dict(SFTP_BROWSE_CASE=case, GHOSTERM_SFTP_BROWSE_BIN=str(binary or build / 'test_sftp_browse')))
                if red is None and 'SFTP BROWSE PROTOCOL PASS' not in result: raise RuntimeError('Browse protocol not executed')
            def control(label): run([build / 'test_config', work / 'core/tests/fixtures'], label)
            compile_core(); protocol('', 'baseline'); run([build / 'test_api'], 'api-baseline'); control('baseline-control')
            start = clean.index('int gt_sftp_browse_controlled(')
            end = clean.index('static int stat_common(', start)
            browse = clean[start:end]
            cases = [
                ('zero', '      !timeout_ms || !max_entries', '      !max_entries', 'normal'),
                ('budget', 'const int64_t deadline = mono_ms() + timeout_ms;', 'const int64_t deadline = mono_ms() + 5000;', 'readdir-timeout'),
                ('jobs', 'if (has_jobs) {', 'if (has_jobs && false) {', 'jobs'),
                ('close-status', 'if (result != 0) status = GT_ERR_IO;', 'if (result != 0) status = GT_OK;', 'closedir-deny'),
                ('handle-free', 'if (directory) libssh2_sftp_closedir(directory);', '(void)directory;', 'readdir-cancel'),
                ('abort', 'gt__ssh_abort(sftp->ssh);', '(void)sftp;', 'closedir-deny'),
                ('limit', 'const int64_t deadline = mono_ms() + timeout_ms;', 'max_entries = 65536;\n  const int64_t deadline = mono_ms() + timeout_ms;', 'limit'),
                ('absolute', "canonical[0] != '/'", 'false', 'relative-path'),
                ('name', "memchr(name, '\\0', (size_t)result) || memchr(name, '/', (size_t)result)", 'false', 'slash-name'),
                ('duplicates', '!strcmp(entries[i-1].full_path, entries[i].full_path)', 'false && !strcmp(entries[i-1].full_path, entries[i].full_path)', 'duplicate'),
            ]
            variants = []
            for name, old, new, case in cases:
                assert browse.count(old) == 1, (name, browse.count(old))
                variants.append((name, clean[:start] + browse.replace(old, new) + clean[end:], case))
            # Keep preflight cancellation intact; remove only in-flight propagation.
            split = browse.index('  SFTP_LOCK(sftp);')
            variants.append(('cancel', clean[:start] + browse[:split] + browse[split:].replace('cancelled, context', 'NULL, NULL') + clean[end:], 'readdir-cancel'))
            for name, old, new in [
                ('size', 'out->has_size = (a->flags & LIBSSH2_SFTP_ATTR_SIZE) != 0;', 'out->has_size = false;'),
                ('type', 'out->is_regular = (type == LIBSSH2_SFTP_S_IFREG);', 'out->is_regular = !out->is_dir && !out->is_symlink;')]:
                assert clean.count(old) == 1
                variants.append((name, clean.replace(old, new), 'allow'))
            for name, changed, case in variants:
                source.write_text(changed + '\n/* ' + MARK + ' */\n'); compile_core()
                protocol(case, name + '-red', 'FAIL: browse')
                control(name + '-control')
                source.write_text(clean); compile_core(); protocol(case, name + '-restored')
                assert source.read_bytes() == snapshots[production[0]]
                print('PASS browse ' + name + ': compiled target red / config green / restored green / SHA256', flush=True)

            # Compile actual upstream mutations, linked as a dylib so the probe
            # can install its supported allocator through public symbol lookup.
            archive = ROOT / 'tmp/host-ssh2/libssh2-1.11.1.tar.gz'
            expected = 'd9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7'
            assert hashlib.sha256(archive.read_bytes()).hexdigest() == expected
            with tarfile.open(archive) as package: package.extractall(work, filter='data')
            library = work / 'libssh2-1.11.1'
            for patch in sorted((ROOT / 'patches/libssh2/1.11.1').glob('*.patch')):
                result = subprocess.run(['patch', '--batch', '--forward', '-p1', '-i', str(patch)], cwd=library, capture_output=True, text=True)
                if result.returncode: raise RuntimeError(result.stdout + result.stderr)
            library_build = work / 'library-build'
            sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
            run(['cmake', '-S', library, '-B', library_build, '-DBUILD_SHARED_LIBS=ON', '-DBUILD_STATIC_LIBS=OFF',
                 '-DBUILD_EXAMPLES=OFF', '-DBUILD_TESTING=OFF', '-DENABLE_ZLIB_COMPRESSION=OFF', '-DCRYPTO_BACKEND=OpenSSL',
                 '-DCMAKE_OSX_SYSROOT=' + sdk, '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0'], 'library-configure')
            upstream = library / 'src/sftp.c'; upstream_clean = upstream.read_text(); library_probe = work / 'dependency-probe'
            def compile_library():
                run(['cmake', '--build', library_build, '-j4'], 'library-build')
                run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(library / 'include'),
                     '-I' + str(ROOT / 'core/include'), '-I' + str(ROOT / 'core/src'), ROOT / 'core/tests/test_sftp_browse.c',
                     build / 'libghosterm_core.a', library_build / 'src/libssh2.dylib', '-lpthread',
                     '-Wl,-rpath,' + str(library_build / 'src'), '-o', library_probe], 'library-link')
            compile_library()
            for case in ('raw-normal', 'raw-path', 'raw-empty'): protocol(case, 'dependency-baseline-' + case, binary=library_probe)
            for name, old, new, case in [
                ('path', 'if(link_len > data_len - 13)', 'if(0 && link_len > data_len - 13)', 'raw-path'),
                ('empty-name', 'if(filename_len == 0 || filename_len >= buffer_maxlen)', 'if(filename_len >= buffer_maxlen)', 'raw-empty')]:
                assert upstream_clean.count(old) == 1
                upstream.write_text(upstream_clean.replace(old, new)); compile_library()
                protocol(case, 'dependency-' + name + '-red', 'FAIL: dependency', binary=library_probe)
                protocol('raw-normal', 'dependency-' + name + '-control', binary=library_probe)
                upstream.write_text(upstream_clean); compile_library(); protocol(case, 'dependency-' + name + '-restored', binary=library_probe)
                assert upstream.read_text() == upstream_clean
                print('PASS browse dependency ' + name + ': compiled target red / valid path green / restored green / SHA256', flush=True)

            package = work / 'apple/GhostermCore'
            shutil.copytree(ROOT / 'apple/GhostermCore', package, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
            binding = package / 'Sources/GhostermCore/GhostermCore.swift'; swift_clean = binding.read_text()
            swift = ['xcrun', 'swift', 'test', '--package-path', package, '--scratch-path', work / 'swift-build', '--skip-build']
            fixture = work / 'swift-fixture'
            fixture.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known_hosts"\nexec ' + shlex.join(map(str, [*swift, '--filter', 'GhostermCoreTests.testSFTPBrowseEndToEnd'])) + '\n')
            fixture.chmod(0o700)
            def compile_swift(): run(['xcrun', 'swift', 'build', '--build-tests', '--package-path', package, '--scratch-path', work / 'swift-build'], 'swift-build')
            def swift_test(label, red=None):
                output = run(['bash', ROOT / 'core/tests/scripts/ssh_e2e.sh', fixture], label, red)
                if 'Executed 1 test' not in output: raise RuntimeError('Swift browse test not executed')
            compile_swift(); swift_test('swift-baseline')
            swift_start = swift_clean.index('    public func browse(')
            swift_end = swift_clean.index('    public func stat(', swift_start)
            swift_browse = swift_clean[swift_start:swift_end]
            for name, old, new, error in [
                ('cancel', 'return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled', 'return false', 'XCTAssertThrowsError failed'),
                ('budget', 'timeoutMilliseconds, maxEntries, { pointer in', '3000, maxEntries, { pointer in', 'XCTAssertThrowsError failed'),
                ('metadata', 'hasSize: metadata.hasSize,', 'hasSize: false,', 'XCTAssertTrue failed')]:
                assert swift_browse.count(old) == 1, (name, swift_browse.count(old))
                binding.write_text(swift_clean[:swift_start] + swift_browse.replace(old, new) + swift_clean[swift_end:] + '\n// ' + MARK + '\n'); compile_swift()
                swift_test('swift-' + name + '-red', error)
                run([*swift, '--filter', 'GhostermCoreTests.testConfigRoundTripIsByteIdentical'], 'swift-' + name + '-control')
                binding.write_text(swift_clean); compile_swift(); swift_test('swift-' + name + '-restored')
                assert binding.read_bytes() == snapshots[production[1]]
                print('PASS Swift browse ' + name + ': target red / config green / restored green / SHA256', flush=True)
            # No live SFTP object can be created without this capability. This
            # stub-only token verifies that the unavailable branch never touches
            # a handle or fabricates a successful empty directory.
            unavailable = work / 'unavailable'
            run(['cmake', '-S', work / 'core', '-B', unavailable, '-DGHOSTERM_WITH_LIBSSH2=OFF'], 'unavailable-configure')
            probe = work / 'unavailable.c'
            probe.write_text('#include "ghosterm.h"\n#include <stdio.h>\nint main(void) {\n int token = 0;\n char path[32] = "stale";\n gt_sftp_entry_t *entries = (void *)&token;\n size_t count = 42;\n int status = gt_sftp_browse_controlled((void *)&token, ".", path, sizeof path,\n     &entries, &count, 100, 16, NULL, NULL);\n int failed = status != GT_ERR_UNSUPPORTED || entries || count || *path;\n if(failed) puts("FAIL: unavailable browse result");\n return failed;\n}\n')
            def no_library():
                run(['cmake', '--build', unavailable, '--clean-first', '--target', 'test_api', 'test_config'], 'unavailable-build')
                run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(work / 'core/include'),
                     probe, unavailable / 'libghosterm_core.a', '-lpthread', '-o', work / 'unavailable-probe'], 'unavailable-link')
            no_library(); run([work / 'unavailable-probe'], 'unavailable-baseline')
            old = 'gt__set_error("sftp browse: libssh2 unavailable");\n  return GT_ERR_UNSUPPORTED;'
            assert clean.count(old) == 1
            source.write_text(clean.replace(old, old.replace('GT_ERR_UNSUPPORTED', 'GT_OK')) + '\n/* ' + MARK + ' */\n')
            no_library(); run([work / 'unavailable-probe'], 'unavailable-red', 'FAIL: unavailable browse result')
            run([unavailable / 'test_config', work / 'core/tests/fixtures'], 'unavailable-control')
            source.write_text(clean); no_library(); run([work / 'unavailable-probe'], 'unavailable-restored')
            assert source.read_bytes() == snapshots[production[0]]
            print('PASS browse unavailable: real archive / target red / config green / restored green / SHA256', flush=True)
    finally:
        if any(p.read_bytes() != original for p, original in snapshots.items()): raise RuntimeError('Production changed during isolated mutations')
    print('SFTP BROWSE MUTATIONS PASS: 13 Core + 2 dependency + 3 Swift + 1 unavailable groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
