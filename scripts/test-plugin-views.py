#!/usr/bin/env python3
"""Native view service conformance and isolated assertion mutations, no UI."""
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
SOURCE = 'core/src/extension/runtime-views.inc'
CASES = [
    ('role', 'r->role != GT_EXT_HOST', 'false', 'executor'),
    ('grant', 'gt_ext_session_access(r->session, &r->lease, capability) != GT_EXT_ACCESS_ALLOWED', 'false', 'ungranted'),
    ('stop', '!r->session || atomic_load_explicit(&r->stopped, memory_order_relaxed) ||', '!r->session || false ||', 'roundtrip'),
    ('direction', 'methods[i].receiver == GT_BRIDGE_EXECUTOR', 'true', 'direction'),
    ('capacity', 'if (!entry->handle[0] && !slot)', 'if (i < 7 && !entry->handle[0] && !slot)', 'native-limit'),
    ('duplicate', 'entry->handle[0] && !strcmp(entry->id, candidate.id)', 'false && entry->handle[0] && !strcmp(entry->id, candidate.id)', 'native-duplicate'),
    ('close', 'gt__secure_erase(view, sizeof(*view));', '(void)view;', 'native-close'),
    ('selected-command', '(kinds & declared) != kinds', 'false && (kinds & declared) != kinds', 'package:client-table-views-other-command'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    args = parser.parse_args(); tools = args.tools.resolve()
    inputs = {str(p.relative_to(ROOT)): sha(p) for directory in ['core/src', 'core/include', 'core/tests', 'packages/plugin-sdk'] for p in (ROOT / directory).rglob('*') if p.is_file()}
    for name in ['core/CMakeLists.txt', 'scripts/test-plugin-views.py', 'scripts/test-plugin-views.mjs', 'scripts/check-plugin-runtime-sdk.py']:
        inputs[name] = sha(ROOT / name)
    original = (ROOT / SOURCE).read_text()
    for label, old, _new, _case in CASES:
        assert original.count(old) == 1, (label, 'ambiguous anchor')
    sdk_anchor = 'if (owner.#handles.get(spec.viewId) === handle) owner.#handles.delete(spec.viewId);'
    assert (ROOT / 'packages/plugin-sdk/portable/views.ts').read_text().count(sdk_anchor) == 1
    commands, groups = [], []
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1',
               DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',
               ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    with tempfile.TemporaryDirectory(prefix='views-proof-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'source'; build = work / 'build'; sdk = work / 'sdk'
        for directory in ['core', 'packages/plugin-sdk', 'patches/quickjs-ng', 'scripts']:
            shutil.copytree(ROOT / directory, source / directory, ignore=shutil.ignore_patterns('build', 'build-*', '__pycache__'))
        (source / 'tmp').mkdir(); (source / 'vendor').mkdir()
        (source / 'vendor/quickjs-ng').symlink_to(ROOT / 'vendor/quickjs-ng', target_is_directory=True)
        def run(label, command, red=False, native=True):
            wrapper = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-views; shift; exec "$gt_timeout_bin" "$@"'
            child = subprocess.Popen(list(map(str, ['bash', '-c', wrapper, 'plugin-views', ROOT, 240, *command])), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            try:
                output, errors = child.communicate(timeout=245)
            finally:
                if child.poll() is None:
                    try: os.killpg(child.pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    child.wait()
            text = (output + errors).decode()
            if red:
                assert child.returncode == 1 and (re.search(r'^FAIL ', text, re.M) if native else 'AssertionError' in text) and not any(marker in text for marker in ['AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:', 'error: ']), (label, text)
            else:
                assert child.returncode == 0, (label, text)
            commands.append(dict(label=label, status=child.returncode, sha256=hashlib.sha256(output + errors).hexdigest()))
            print('PASS ' + label, flush=True)
            return text
        def types(label):
            run(label, ['node', tools / 'node_modules/typescript/bin/tsc', '-p', source / 'packages/plugin-sdk/tsconfig.json', '--outDir', sdk])
        def rebuild(label):
            for file in [build / 'CMakeFiles/ghosterm_core.dir/src/extension/runtime.c.o', build / 'libghosterm_core.a', build / 'test_runtime_views', build / 'test_package_runtime', build / 'test_config']:
                file.unlink(missing_ok=True)
            text = run(label, ['cmake', '--build', build, '--target', 'test_runtime_views', 'test_package_runtime', 'test_config', '-j', '4'])
            assert 'runtime.c.o' in text and 'Linking C static library' in text and 'Linking C executable test_runtime_views' in text
        def target(label, mode, red=False):
            command = [build / 'test_package_runtime', mode.split(':')[1], source / 'tmp'] if mode.startswith('package:') else [build / 'test_runtime_views', mode, sdk]
            run(label, command, red)
        def control(label):
            run(label, [build / 'test_config', source / 'core/tests/fixtures', source / 'tmp'])
        types('types')
        run('node-baseline', ['node', source / 'scripts/test-plugin-views.mjs', sdk, tools])
        run('configure', ['cmake', '-S', source / 'core', '-B', build, '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DGHOSTERM_WITH_PLUGIN_JS=ON', '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DGHOSTERM_PLUGIN_SDK_DIR=' + str(sdk), '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline-build')
        run('native-baseline', ['ctest', '--test-dir', build, '-R', '^runtime_views_|^plugin_package_runtime_client-table-views', '--no-tests=error', '--output-on-failure'])
        path = source / SOURCE; before = path.read_bytes()
        if args.mutate:
            for label, old, new, mode in CASES:
                path.write_text(before.decode().replace(old, new))
                try:
                    rebuild(label + '-build'); target(label + '-assertion', mode, True); control(label + '-control')
                finally:
                    path.write_bytes(before)
                rebuild(label + '-restore-build'); target(label + '-restored', mode)
                assert path.read_bytes() == before
                groups.append(dict(case=label, sourceSHA256=hashlib.sha256(before).hexdigest(), objectSHA256=sha(build / 'CMakeFiles/ghosterm_core.dir/src/extension/runtime.c.o')))
            path = source / 'packages/plugin-sdk/portable/views.ts'; before = path.read_bytes()
            path.write_text(before.decode().replace(sdk_anchor, 'owner.#handles.delete(spec.viewId);'))
            try:
                types('sdk-late-close-types')
                run('sdk-late-close-assertion', ['node', source / 'scripts/test-plugin-views.mjs', sdk, tools], True, native=False)
                control('sdk-late-close-control')
            finally:
                path.write_bytes(before)
            types('sdk-late-close-restored-types')
            run('sdk-late-close-restored', ['node', source / 'scripts/test-plugin-views.mjs', sdk, tools])
            groups.append(dict(case='sdk-late-close', sourceSHA256=hashlib.sha256(before).hexdigest()))
        run('native-restored', ['ctest', '--test-dir', build, '-R', '^runtime_views_|^plugin_package_runtime_client-table-views', '--no-tests=error', '--output-on-failure'])
        assert all(sha(ROOT / name) == digest for name, digest in inputs.items())
        receipt = dict(inputs=inputs, groups=groups, commands=commands, coreSHA256=sha(build / 'libghosterm_core.a'), sanitizers=['address', 'undefined'], leakSanitizer=False, nativeCases=19, nodeChecks=44, scope='local registration service; no Native UI, SSH or device behavior')
        args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise SystemExit(128 + number)
    signal.signal(signal.SIGTERM, interrupted)
    main()
