#!/usr/bin/env python3
"""Run production Kotlin Codex assembly/lifecycle on a JVM; Android JSON/UI need a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/CodexChangeReview.kt'
CASES = (
    ('identity', 'chunk.snapshotID == reference.snapshotID', 'true', 1, 'snapshot identity must match'),
    ('offset', 'chunk.offset == nextOffset', 'true', 1, 'chunks must be contiguous'),
    ('utf16', 'chunk.nextOffset == nextOffset + chunk.content.length', 'true', 1, 'UTF16 chunk length must match'),
    ('bytes', '!chunk.done || receivedBytes + data.size == reference.totalBytes', 'true', 1, 'EOF must match actual UTF8 bytes'),
    ('scope', 'if (!isCurrent()) { cancel(); return@withTimeout }', '// mutation removed scope check', 3,
     'changed reference must discard late data'),
    ('encoding', '.onMalformedInput(CodingErrorAction.REPORT)', '.onMalformedInput(CodingErrorAction.REPLACE)', 1,
     'malformed surrogate must not become replacement UTF8 bytes'),
    ('approval-scope', '!isCurrent(reference)', 'false', 1, 'changed patch must not authorize an old review'),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cached(group, artifact, version):
    directory = Path.home() / '.gradle/caches/modules-2/files-2.1' / group / artifact / version
    found = list(directory.glob(f'*/{artifact}-{version}.jar'))
    if len(found) != 1:
        raise RuntimeError(f'Run the Android Gradle build to populate {group}:{artifact}:{version}; no tools are downloaded here')
    return found[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES])
    args = parser.parse_args()
    if args.case and not args.mutate:
        parser.error('--case requires --mutate')
    compiler = cached('org.jetbrains.kotlin', 'kotlin-compiler-embeddable', '2.2.21')
    dependencies = [cached('org.jetbrains.kotlin', 'kotlin-stdlib', '2.2.21'),
        cached('org.jetbrains.kotlin', 'kotlin-script-runtime', '2.2.21'),
        cached('org.jetbrains.kotlin', 'kotlin-reflect', '1.6.10'),
        cached('org.jetbrains.kotlin', 'kotlin-daemon-embeddable', '2.2.21'),
        cached('org.jetbrains.kotlinx', 'kotlinx-coroutines-core-jvm', '1.8.0'),
        cached('org.jetbrains', 'annotations', '13.0')]
    runtime = [dependencies[0], dependencies[-1], cached('org.jetbrains.kotlinx', 'kotlinx-coroutines-core-jvm', '1.9.0')]
    java = ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home/bin/java'
    if not java.is_file():
        raise RuntimeError('Prepared Android JDK 17 is required')
    before = sha(SOURCE)
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='android-codex-review-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        (work / 'tmp').mkdir()
        target = work / 'CodexChangeReview.kt'
        original = SOURCE.read_bytes()
        target.write_bytes(original)
        fixture = work / 'fixture.kt'
        shutil.copyfile(ROOT / 'scripts/probes/plugin-codex-review.kt', fixture)
        env = dict(os.environ, TMPDIR=str(work / 'tmp'))
        base = [str(java), '-Djava.awt.headless=true', '-Djava.io.tmpdir=' + str(work / 'tmp')]
        compile_command = base + ['-cp', os.pathsep.join(map(str, [compiler, *dependencies])),
            'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler', '-no-stdlib', '-no-reflect', '-jvm-target', '17',
            '-classpath', os.pathsep.join(map(str, runtime)), '-d', str(work / 'classes'), str(target), str(fixture)]
        execute = base + ['-cp', os.pathsep.join(map(str, [work / 'classes', *runtime])), 'com.hanxi.ghosterm.CodexReviewFixture']

        def run(command, assertion=None):
            bounded = ['bash', '-c',
                '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require android-codex-review; shift; gt_timeout 120 "$@"',
                'android-codex-review', str(ROOT), *command]
            result = subprocess.run(bounded, cwd=work, env=env, capture_output=True, text=True)
            output = result.stdout + result.stderr
            if assertion:
                valid = result.returncode == 1 and 'java.lang.AssertionError' in output and assertion in output
            else:
                valid = result.returncode == 0
            if not valid:
                raise RuntimeError(f'Unexpected result from {command}:\n{output}')
            return output

        def green():
            run(compile_command)
            output = run(execute)
            if 'ANDROID CODEX REVIEW JVM PASS' not in output:
                raise RuntimeError('JVM fixture assertions did not execute')
            return output

        print(green(), end='', flush=True)
        for name, old, new, count, assertion in CASES if args.mutate else []:
            if args.case and name not in args.case:
                continue
            if original.count(old.encode()) != count:
                raise RuntimeError(f'{name}: mutation anchor changed')
            target.write_bytes(original.replace(old.encode(), new.encode()))
            run(compile_command)
            run(execute, assertion=assertion)
            # Same compiled mutant, independent reference factory control.
            control = base + ['-cp', os.pathsep.join(map(str, [work / 'classes', *runtime])),
                              'com.hanxi.ghosterm.CodexReviewFixture', 'control']
            if 'PASS independent reference control' not in run(control):
                raise RuntimeError('Independent reference control did not execute')
            target.write_bytes(original)
            if sha(target) != before:
                raise RuntimeError('Restored source SHA256 mismatch')
            green()
            print(f'PASS {name}: Kotlin compiled assertion red / same mutant control green / restored green / SHA256', flush=True)
        (work / 'receipt.json').write_text(json.dumps({'source_sha256': before,
            'tools_sha256': {str(p): sha(p) for p in [java, compiler, *dependencies, *runtime]}}, indent=2))
    if sha(SOURCE) != before:
        raise RuntimeError('Original source changed during isolated checks')
    print('ANDROID CODEX REVIEW JVM VERIFIED; Android JSON, Compose and device behavior are separate checks', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
