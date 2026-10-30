#!/usr/bin/env python3
"""Known-host source records: isolated C and Swift mutations with controls."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_KNOWN_HOSTS_SNAPSHOT_MUTATION'


def run(command, log, env=None, seconds=180):
    shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require known-hosts-snapshot; shift; limit="$1"; shift; gt_timeout "$limit" "$@"'
    result = subprocess.run(['bash', '-c', shell, 'metadata', str(ROOT), str(seconds), *map(str, command)],
                            cwd=ROOT, env=env, capture_output=True, text=True)
    log.write_text(result.stdout + result.stderr)
    return result


def require(result):
    if result.returncode: raise RuntimeError(result.stdout + result.stderr)


def main():
    args = argparse.ArgumentParser()
    args.add_argument('--apple', action='store_true', help='also compile and mutate the Swift binding with Xcode')
    apple = args.parse_args().apple
    paths = [ROOT / 'core/src/known_hosts.c', ROOT / 'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    original = {p: p.read_bytes() for p in paths}
    if any(MARK.encode() in data for data in original.values()): raise RuntimeError('residual mutation marker')
    hashes = {p: hashlib.sha256(data).digest() for p, data in original.items()}
    logs = ROOT / 'tmp/known-hosts-snapshot-mutations'; logs.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix='kh-snapshot-mutations-', dir=ROOT / 'tmp') as directory:
            work = Path(directory)
            (work / 'tmp').mkdir()
            shutil.copytree(ROOT / 'core', work / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
            source = work / 'core/src/known_hosts.c'
            clean = source.read_bytes()
            build = work / 'core/build'
            require(run(['cmake', '-S', work / 'core', '-B', build, '-DCMAKE_BUILD_TYPE=Debug'], logs / 'configure.log'))

            def compile_core():
                # Force relink even on make's integer-second timestamp boundaries.
                require(run(['cmake', '--build', build, '--clean-first', '--target', 'test_known_hosts_snapshot', 'test_api'], logs / 'build.log'))

            def target(label, reason=None):
                result = run([build / 'test_known_hosts_snapshot', work / 'tmp'], logs / f'{label}.log')
                if reason is None: require(result)
                elif result.returncode != 1 or f'FAIL snapshot: {reason}' not in result.stderr:
                    raise RuntimeError('wrong mutation failure: ' + result.stdout + result.stderr)

            def control(label):
                require(run([build / 'test_api'], logs / f'{label}-control.log'))

            compile_core(); target('baseline'); control('baseline')
            cases = [
                ('line', 'entry.source_line = line;', 'entry.source_line = line + 1;', 'physical line numbers'),
                ('start', 'entry.byte_start = start;', 'entry.byte_start = start + 1;', 'original byte start'),
                ('end', 'entry.byte_end = end;', 'entry.byte_end = end - 1;', 'range includes original newline'),
                ('marker', 'fields[count++] = &out->marker;', 'fields[count++] = &out->hosts;', 'markers never folded to trust'),
                ('shape', '  return true;\n}', '  out->recognized = true; return true;\n}', 'field shape not silent skip'),
                ('hash', "out->hashed = out->hosts[0] == '|';", 'out->hashed = false;', 'hashed classified without matching'),
                ('invalid', 'if (gt__kh_split_line(snapshot->text + start, &entry))',
                 'if (gt__kh_split_line(snapshot->text + start, &entry) && entry.recognized)', 'visible records include malformed'),
                ('nul', '|| memchr(text, 0, length)', '', 'embedded NUL rejected'),
                ('limit', 'snapshot->count == 4096', 'snapshot->count == 4097', 'record limit explicit failure'),
                ('clear', 'if (out) memset(out, 0, sizeof *out);', 'if (false && out) memset(out, 0, sizeof *out);', 'end clears output'),
            ]
            for label, before, after, reason in cases:
                text = clean.decode()
                if text.count(before) != 1: raise RuntimeError('ambiguous mutation anchor: ' + label)
                try:
                    source.write_text(text.replace(before, after + ' /* ' + MARK + ' */'))
                    compile_core(); target(label, reason); control(label)
                finally:
                    source.write_bytes(clean)
                    if hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(clean).digest():
                        raise RuntimeError('copy SHA256 restoration failed')
                compile_core(); target(label + '-restored')
                print(f'PASS {label}: target red / independent API green / restored green / SHA256', flush=True)

            if apple:
                package = work / 'apple/GhostermCore'
                shutil.copytree(ROOT / 'apple/GhostermCore', package, symlinks=True,
                                ignore=shutil.ignore_patterns('.build', '.swiftpm'))
                binding = package / 'Sources/GhostermCore/GhostermCore.swift'
                clean_binding = binding.read_bytes()
                env = dict(os.environ, TMPDIR=str(work / 'tmp'))
                if 'DEVELOPER_DIR' not in env and Path('/Applications/Xcode.app/Contents/Developer').exists():
                    env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
                command = ['xcrun', 'swift', 'test', '--package-path', package, '--scratch-path', work / 'swift-build']

                def swift_build():
                    require(run(['xcrun', 'swift', 'build', '--build-tests', '--package-path', package,
                                 '--scratch-path', work / 'swift-build'], logs / 'swift-build.log', env))

                def swift_test(label, method, red=False):
                    # Xcode may spend time collecting diagnostics for an expected assertion failure.
                    result = run([*command, '--skip-build', '--filter', ('KnownHostsSnapshotTests.' if method == 'testRecordsCopyMarkersAndByteRanges' else 'GhostermCoreTests.') + method], logs / (label + '.log'), env, seconds=300)
                    if 'Executed 1 test' not in result.stdout + result.stderr:
                        raise RuntimeError('Swift test did not execute: ' + result.stdout + result.stderr)
                    if not red: require(result)
                    elif result.returncode != 1 or 'XCTAssertEqual failed' not in result.stdout + result.stderr:
                        raise RuntimeError('wrong Swift failure: ' + result.stdout + result.stderr)

                method = 'testRecordsCopyMarkersAndByteRanges'
                swift_build(); swift_test('swift-baseline', method)
                try:
                    text = clean_binding.decode(); before = 'byteEnd: Int(record.byte_end)'
                    if text.count(before) != 1: raise RuntimeError('Swift mutation anchor')
                    binding.write_text(text.replace(before, 'byteEnd: 0 /* ' + MARK + ' */'))
                    swift_build(); swift_test('swift-source-line', method, True)
                    swift_test('swift-control', 'testConfigRoundTripIsByteIdentical')
                finally:
                    binding.write_bytes(clean_binding)
                    if hashlib.sha256(binding.read_bytes()).digest() != hashes[paths[1]]:
                        raise RuntimeError('Swift copy SHA256 restoration failed')
                swift_build(); swift_test('swift-restored', method)
                print('PASS Swift known-host records: target red / roundtrip green / restored green / SHA256', flush=True)
    finally:
        if any(hashlib.sha256(p.read_bytes()).digest() != expected for p, expected in hashes.items()):
            raise RuntimeError('repository sources changed during isolated mutations')
    print(f'KNOWN HOSTS SNAPSHOT MUTATIONS PASS: {len(cases)} C groups' + (' + 1 Swift group' if apple else ''), flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
