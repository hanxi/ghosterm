#!/usr/bin/env python3
"""Node/Exec staging: isolated ASan/UBSan C builds, assertion reds and controls.

No plugin is executed. Only disposable trees below the repository tmp/ are
modified. --tools may reuse the locked plugin-sdk developer npm installation;
otherwise npm is offline unless --fetch is explicitly supplied.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'core/src/extension/package-stage.c'
MARK = 'GT_PACKAGE_STAGE_MUTATION'
CASES = [
    ('entropy-unsupported', 'return entropy == GT_ERR_UNSUPPORTED ? GT_STAGE_UNSUPPORTED : GT_STAGE_IO;',
     'return GT_STAGE_IO;', 'boundary:entropy-unsupported'),
    ('runtime', 'if (!info.executor || (strcmp(info.runtime, "node") && strcmp(info.runtime, "exec"))) {',
     'if (!info.executor) {', 'unsupported'),
    ('disk-limit', 'if (bytes > limit) {', 'if (false && bytes > limit) {', 'limit'),
    ('cancel-open', 'if (stop && atomic_load_explicit(stop, memory_order_relaxed)) {',
     'if (false && stop && atomic_load_explicit(stop, memory_order_relaxed)) {', 'cancel-before'),
    ('cancel-step', 'if (s->stop && atomic_load_explicit(s->stop, memory_order_relaxed)) {',
     'if (false && s->stop && atomic_load_explicit(s->stop, memory_order_relaxed)) {', 'cancel-mid'),
    ('partial-view', 'if (s->status != GT_STAGE_OK) {', 'if (false) {', 'node'),
    ('fixed-copy-offset', 'write(s->file, bytes + s->offset, n)', 'write(s->file, bytes, n)', 'frozen'),
    ('write-chunk', 'n = GT_PACKAGE_CHUNK_MAX;', 'n = size - s->offset;', 'cancel-mid'),
    ('exec-permission', '? 0500 : 0400;', '? 0400 : 0400;', 'exec'),
    ('data-permission', '? 0500 : 0400;', '? 0500 : 0600;', 'node'),
    ('directory-permission', 'fchmod(s->root, 0500)', 'fchmod(s->root, 0700)', 'node'),
    ('file-identity', 's->st_dev == e->device && s->st_ino == e->inode &&', 'true &&', 'replace-file'),
    ('root-identity', 'st.st_ino == s->root_identity.st_ino;', 'true;', 'replace-root'),
    ('cleanup-error', 'unlinkat(s->parent, s->name, AT_REMOVEDIR) && result == GT_STAGE_OK',
     'unlinkat(s->parent, s->name, AT_REMOVEDIR) && false', 'unknown-file'),
    ('write-error', 'if (written <= 0) {\n            return s->status = GT_STAGE_IO;',
     'if (written <= 0) {\n            return s->status = GT_STAGE_OK;', 'boundary:io-write'),
    ('zero-write', 'if (written <= 0) {\n            return s->status = GT_STAGE_IO;',
     'if (written <= 0) {\n            return s->status = GT_STAGE_OK;', 'boundary:zero-write'),
    ('short-write', 's->offset += (size_t)written;', 's->offset += n;', 'boundary:short-write'),
    ('interrupted-write', 'if (written < 0 && errno == EINTR) {',
     'if (false && written < 0 && errno == EINTR) {', 'boundary:interrupted-write'),
    ('close-error', 'if (changed || closed) {', 'if (changed) {\n            (void)closed;', 'boundary:close-error'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--case', action='append', help='select named mutation groups')
    args = parser.parse_args()
    if args.case and set(args.case) - {c[0] for c in CASES}:
        parser.error('unknown mutation group')
    cases = [c for c in CASES if not args.case or c[0] in args.case]
    original_source = (ROOT / SOURCE).read_text()
    assert MARK not in original_source
    for label, old, _, _ in cases:
        assert original_source.count(old) == (2 if label == 'cancel-step' else 1), label
    work = Path(tempfile.mkdtemp(prefix='package-stage-mutation-', dir=ROOT / 'tmp'))
    source, build, logs = work / 'source', work / 'build', work / 'logs'
    logs.mkdir()
    commands = []
    env = {**os.environ, 'TMPDIR': str(work), 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}

    def run(label, command, red=False):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require package-stage; shift; gt_timeout 180 "$@"'
        p = subprocess.run(['bash', '-c', shell, 'package-stage', str(ROOT), *map(str, command)],
                           cwd=ROOT, env=env, capture_output=True, text=True, timeout=190)
        output = p.stdout + p.stderr
        (logs / (label + '.log')).write_text(output)
        commands.append(dict(label=label, returncode=p.returncode, assertion=red))
        if red:
            assert p.returncode == 1 and re.search(r'^FAIL ', p.stderr, re.M) and not any(
                s in output for s in ('AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer')), label + '\n' + output
        else:
            assert p.returncode == 0, label + '\n' + output
        return output

    try:
        for folder in ('core', 'packages/plugin-sdk', 'patches/quickjs-ng', 'scripts'):
            shutil.copytree(ROOT / folder, source / folder,
                            ignore=shutil.ignore_patterns('build', 'build-*', '__pycache__'))
        (source / 'vendor').mkdir()
        (source / 'vendor/quickjs-ng').symlink_to(ROOT / 'vendor/quickjs-ng', target_is_directory=True)
        (source / 'tmp').mkdir()
        inputs = {str(p.relative_to(source)): sha(p) for folder in ('core', 'packages/plugin-sdk',
                  'patches/quickjs-ng', 'scripts') for p in (source / folder).rglob('*') if p.is_file()}
        tools = args.tools.resolve() if args.tools else work / 'tools'
        if not args.tools:
            shutil.copytree(ROOT / 'scripts/fixtures/plugin-sdk-tools', tools)
            command = ['npm', 'ci', '--prefix', tools, '--ignore-scripts', '--no-audit', '--no-fund',
                       '--cache', ROOT / 'tmp/plugin-sdk-npm-cache']
            if not args.fetch:
                command.append('--offline')
            run('tools', command)
        for name in ('package.json', 'package-lock.json'):
            assert (tools / name).read_bytes() == (ROOT / 'scripts/fixtures/plugin-sdk-tools' / name).read_bytes()
        run('sdk-generated-check', ['node', source / 'scripts/generate-plugin-runtime-sdk.mjs',
                                    '--tools', tools, '--check'])
        sdk = work / 'sdk'
        run('sdk', ['node', tools / 'node_modules/typescript/bin/tsc', '-p',
                    source / 'packages/plugin-sdk/tsconfig.json', '--outDir', sdk])
        run('configure', ['cmake', '-S', source / 'core', '-B', build,
                          '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DGHOSTERM_WITH_PLUGIN_JS=ON',
                          '-DGHOSTERM_BUILD_TESTS=ON', '-DGHOSTERM_PLUGIN_SDK_DIR=' + str(sdk),
                          '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                          '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])

        def rebuild(label):
            (build / 'CMakeFiles/ghosterm_core.dir/src/extension/package-stage.c.o').unlink(missing_ok=True)
            for target in ('test_package_stage', 'test_package_stage_boundary'):
                (build / f'CMakeFiles/{target}.dir/tests/test_package_stage.c.o').unlink(missing_ok=True)
                (build / target).unlink(missing_ok=True)
            (build / 'libghosterm_core.a').unlink(missing_ok=True)
            (build / 'test_config').unlink(missing_ok=True)
            output = run(label + '-build', ['cmake', '--build', build, '--target',
                         'test_package_stage', 'test_package_stage_boundary', 'test_config', '-j', '4'])
            assert 'src/extension/package-stage.c.o' in output and 'Linking C static library' in output
            assert 'Linking C executable test_package_stage_boundary' in output

        def target(label, mode, red=False):
            boundary = mode.startswith('boundary:')
            run(label, [build / ('test_package_stage_boundary' if boundary else 'test_package_stage'),
                        mode.removeprefix('boundary:'), source / 'tmp'], red)

        rebuild('baseline')
        run('baseline', ['ctest', '--test-dir', build, '-R', '^plugin_package_stage_', '--output-on-failure'])
        for label, old, new, mode in cases:
            path = source / SOURCE
            original = path.read_bytes()
            try:
                path.write_text(original.decode().replace(old, new) + '\n/* ' + MARK + ' */\n')
                rebuild(label)
                target(label + '-red', mode, True)
                run(label + '-control', [build / 'test_config', source / 'core/tests/fixtures'])
            finally:
                path.write_bytes(original)
                assert sha(path) == hashlib.sha256(original).hexdigest()
            rebuild(label + '-restored')
            target(label + '-restored', mode)
            print(label + ': compiled assertion red / same archive configuration green / restored green / SHA256', flush=True)
        run('final', ['ctest', '--test-dir', build, '-R', '^plugin_package_stage_', '--output-on-failure'])
        for name, digest in inputs.items():
            assert sha(source / name) == digest and sha(ROOT / name) == digest, name
        (logs / 'result.json').write_text(json.dumps(dict(accepted=True, groups=[c[0] for c in cases],
            commands=commands, inputs_sha256=inputs, sanitizers=['address', 'undefined'],
            leak_sanitizer=False), indent=2) + '\n')
        print('PACKAGE STAGE MUTATIONS PASS: ' + str(logs), flush=True)
    except BaseException as error:
        (logs / 'failure.txt').write_text(repr(error))
        raise


def stop(number, frame):
    raise KeyboardInterrupt('signal ' + str(number))


if __name__ == '__main__':
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, stop)
    main()
