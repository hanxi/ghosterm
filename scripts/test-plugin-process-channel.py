#!/usr/bin/env python3
"""Public process transport: isolated O1 ASan/UBSan, compiled mutants and Swift.

Only disposable local executables/cwd. No shell, model, user endpoint or App UI.
All mutation edits stay in a copied Core; optional Swift tests use a second,
unsanitized enabled Core, not an archive linked against incompatible runtimes.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_PROCESS_CHANNEL_MUTATION'
sys.dont_write_bytecode = True


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--apple', action='store_true')
    parser.add_argument('--full', action='store_true', help='also build a separate Debug Lua/SSH-enabled Core and run every configured CTest')
    parser.add_argument('--lua-archive', type=Path, required=True)
    parser.add_argument('--group', action='append')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'):
        parser.error('receipt must be under project tmp')
    channel = 'core/src/extension/channel.c'
    session = 'core/src/extension/session-api.c'
    runtime = 'core/src/extension/runtime-lua.inc'
    cases = [
        ('approval', channel, '.execution_authorized = config->execution_authorized,', '.execution_authorized = true,', 'invalid'),
        ('capacity', channel, 'if (count >= GT_EXT_CHANNEL_LIMIT)', 'if (count > GT_EXT_CHANNEL_LIMIT)', 'capacity'),
        ('failed-reservation', channel, 'gt_status_t status = gt__bridge_process_open(&p, &c->stream);\n    if (status != GT_OK) {\n        channel_free(c);',
            'gt_status_t status = gt__bridge_process_open(&p, &c->stream);\n    if (status != GT_OK) {\n        (void)c;', 'invalid'),
        ('diagnostic', channel, 'memcpy(out->diagnostic, p.diagnostic, p.diagnostic_size);', 'memset(out->diagnostic, 0, p.diagnostic_size);', 'tail'),
        ('signal', channel, 'out->term_signal = p.child.term_signal;', 'out->term_signal = 0;', 'signal'),
        ('eof-exit', channel, 'out->child_exited = p.child.exited;', 'out->child_exited = p.child.exited || p.stdout_eof;', 'eof-hold'),
        ('reason', channel, 'out->reason = GT_EXT_PROCESS_TRANSPORT_STDOUT_LIMIT;', 'out->reason = GT_EXT_PROCESS_TRANSPORT_NONE;', 'stdout-limit'),
        ('session-observation', session, 'return s && out ? gt_ext_channel_process_info(s->channel, out) : GT_ERR_INVALID_ARG;',
            'return s && out ? GT_ERR_UNSUPPORTED : GT_ERR_INVALID_ARG;', 'adopt'),
        ('runtime-observation', runtime, 'return gt_ext_session_process_info(runtime ? runtime->session : NULL, out);',
            '(void)runtime;\n    return gt_ext_session_process_info(NULL, out);', 'runtime'),
    ]
    originals = {name: (ROOT / name).read_bytes() for _, name, _, _, _ in cases}
    for label, name, old, _, _ in cases:
        if MARK.encode() in originals[name] or originals[name].decode().count(old) != 1:
            raise RuntimeError('Residual/ambiguous mutation: ' + label)
    if args.group:
        if not args.mutate or set(args.group) - {case[0] for case in cases}: parser.error('unknown group or missing --mutate')
        cases = [case for case in cases if case[0] in args.group]
    files = [p for folder in ('core/src', 'core/include', 'core/tests', 'apple/GhostermCore/Sources', 'apple/GhostermCore/Tests')
             for p in (ROOT / folder).rglob('*') if p.is_file()]
    files += [ROOT / name for name in ('core/CMakeLists.txt', 'core/cmake/PluginLua.cmake',
        'apple/GhostermCore/Package.swift', 'scripts/test-plugin-process-channel.py')]
    archive = args.lua_archive.resolve(strict=True)
    archive_sha = sha(archive)
    inputs = {str(p.relative_to(ROOT)): sha(p) for p in files}
    # Full Core contains an AF_UNIX socket fixture: keep its actual path within
    # Darwin sun_path, without weakening that fixture's length guard.
    with tempfile.TemporaryDirectory(prefix='pc-', dir=os.environ['TMPDIR']) as temporary:
        work = Path(temporary); source = work / 'h'; logs = work / 'logs'; logs.mkdir()
        shutil.copytree(ROOT / 'core', source / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
        for folder in ('scripts', 'vendor', 'patches', 'build'):
            (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir()
        build = work / 'build'; owned_binaries = {str(build / 'test_process_channel_child')}
        env = {**os.environ, 'TMPDIR': str(work), 'TMP': str(work), 'TEMP': str(work), 'GHOSTERM_AUTOMATION_OFFSCREEN': '1',
            'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        libproc = ctypes.CDLL('/usr/lib/libproc.dylib') if sys.platform == 'darwin' else None

        def executable(pid):
            if libproc:
                buffer = ctypes.create_string_buffer(4096)
                return os.fsdecode(buffer.value) if libproc.proc_pidpath(pid, buffer, len(buffer)) > 0 else ''
            try: return os.readlink('/proc/' + str(pid) + '/exe')
            except OSError: return ''

        def owned_children():
            table = subprocess.check_output(['ps', '-axo', 'pid=,lstart='], text=True, timeout=5)
            return [(int(fields[0]), fields[1], executable(int(fields[0])))
                for line in table.splitlines() if len(fields := line.strip().split(None, 1)) == 2
                and executable(int(fields[0])) in owned_binaries]

        def close_children():
            for pid, started, binary in owned_children():
                current = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart='], capture_output=True, text=True, timeout=5)
                if current.stdout.strip() == started and executable(pid) == binary:
                    try: os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError: pass
            end = time.monotonic() + 2
            while owned_children():
                if time.monotonic() >= end: raise RuntimeError('Owned fixture child remains alive')
                time.sleep(.01)

        def run(label, command, seconds=120, red=False):
            shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require process-channel; shift; exec "$gt_timeout_bin" --foreground "$@"'
            p = subprocess.Popen(['bash', '-c', shell, 'process-channel', str(ROOT), str(seconds), *map(str, command)],
                cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors='backslashreplace', start_new_session=True)
            try: stdout, stderr = p.communicate(timeout=seconds + 10)
            except BaseException:
                try: os.killpg(p.pid, signal.SIGTERM)
                except ProcessLookupError: pass
                try: p.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    try: os.killpg(p.pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    p.communicate(timeout=3)
                raise
            output = stdout + stderr; (logs / (label + '.log')).write_text(output)
            if red == 'swift':
                if p.returncode != 1 or 'Executed 1 test, with 1 failure' not in output or "Test Suite 'Selected tests' failed" not in output or any(
                    token in output for token in ('AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:', 'fatal error:', 'emit-module command failed')):
                    raise RuntimeError(label + ': expected one normal XCTest failure\n' + output)
            elif red:
                fatal = {int(getattr(signal, s)) for s in ('SIGILL', 'SIGABRT', 'SIGBUS', 'SIGFPE', 'SIGSEGV', 'SIGTRAP', 'SIGSYS', 'SIGEMT') if hasattr(signal, s)}
                if p.returncode != 1 or len(re.findall(r'^FAIL process_channel ', output, re.M)) != 1 or any(
                    token in output for token in ('AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:', 'Traceback')) or any(
                    int(number) in fatal for number in re.findall(r'\bsignal=(\d+)\b', output)):
                    raise RuntimeError(label + ': expected one normal assertion\n' + output)
            elif p.returncode: raise RuntimeError(label + '\n' + output)
            close_children()
            return output

        def configure(directory, enabled=True, sanitize=True):
            command = ['cmake', '-S', source / 'core', '-B', directory, '-DGHOSTERM_WITH_LIBSSH2=OFF',
                '-DGHOSTERM_WITH_PLUGIN_LUA=' + ('ON' if enabled else 'OFF'), '-DGHOSTERM_BUILD_TESTS=ON',
                '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG',
                '-DCMAKE_C_FLAGS=' + ('-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' if sanitize else ''),
                '-DGHOSTERM_LUA_ARCHIVE=' + str(archive)]
            if sys.platform == 'darwin':
                command += ['-DCMAKE_OSX_SYSROOT=' + subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], env=env, text=True).strip(), '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0']
            return command

        def compile(label, owner=None):
            objects = ['CMakeFiles/ghosterm_core.dir/src/extension/' + name + '.o'
                for name in ('channel.c', 'session-api.c', 'runtime.c')]
            for name in [*objects, 'libghosterm_core.a', 'test_process_channel', 'test_process_channel_child', 'test_config']:
                (build / name).unlink(missing_ok=True)
            output = run(label, ['cmake', '--build', build, '--target', 'test_process_channel', 'test_process_channel_child', 'test_config', '-j2'], 300)
            if any('Building C object ' + name not in output for name in objects): raise RuntimeError('Missing actual compile: ' + label)
            return {name: sha(build / name) for name in [*objects, 'libghosterm_core.a', 'test_process_channel', 'test_process_channel_child']}

        try:
            run('configure', configure(build)); baseline = compile('baseline-build')
            output = run('baseline', ['ctest', '--test-dir', build, '-R', '^process_channel_', '--output-on-failure'])
            if '100% tests passed out of 13' not in output: raise RuntimeError('Expected all 13 public cases\n' + output)
            run('config', [build / 'test_config', ROOT / 'core/tests/fixtures'])
            results = []
            if args.mutate:
                for label, name, old, new, case in cases:
                    path = source / name
                    try:
                        path.write_text(originals[name].decode().replace(old, new, 1) + '\n/* ' + MARK + ' */\n')
                        changed = compile(label + '-build')
                        object_name = 'CMakeFiles/ghosterm_core.dir/src/extension/' + ('runtime.c' if name.endswith('.inc') else Path(name).name) + '.o'
                        if changed[object_name] == baseline[object_name] or changed['test_process_channel'] == baseline['test_process_channel']:
                            raise RuntimeError('Actual mutation object/executable unchanged: ' + label)
                        command = [build / 'test_process_channel', case, build / 'test_process_channel_child', source / 'tmp']
                        run(label + '-red', command, 20, True)
                        run(label + '-control', [build / 'test_config', ROOT / 'core/tests/fixtures'])
                    finally: path.write_bytes(originals[name])
                    compile(label + '-restore')
                    run(label + '-green', command, 20)
                    if sha(path) != hashlib.sha256(originals[name]).hexdigest(): raise RuntimeError('Restore mismatch')
                    results.append(dict(group=label, case=case, actualArtifacts=changed))
                    print('PASS ' + label + ': compiled assertion red / same archive config green / restored / SHA256', flush=True)
            run('final', ['ctest', '--test-dir', build, '-R', '^process_channel_', '--output-on-failure'])
            disabled = work / 'disabled'
            run('disabled-configure', [*configure(disabled, False), '-DGHOSTERM_WITH_PLUGIN_CHANNEL=OFF'])
            run('disabled-build', ['cmake', '--build', disabled, '--target', 'test_process_channel', 'test_process_channel_child', '-j2'], 300)
            run('disabled', ['ctest', '--test-dir', disabled, '-R', '^process_channel_disabled$', '--output-on-failure'])
            apple = False; apple_groups = []
            if args.apple:
                plain = work / 'apple-build'; owned_binaries.add(str(plain / 'test_process_channel_child'))
                run('apple-configure', configure(plain, True, False))
                run('apple-core', ['cmake', '--build', plain, '--target', 'test_process_channel_child', '-j2'], 300)
                (source / 'core/build').symlink_to(plain, target_is_directory=True)
                shutil.copytree(ROOT / 'apple/GhostermCore', source / 'apple/GhostermCore', ignore=shutil.ignore_patterns('.build', '.swiftpm'))
                env['GHOSTERM_CORE_TEST_PLUGIN_LUA'] = '1'
                env['GHOSTERM_CORE_BUILD_DIR'] = str(plain)
                env['GHOSTERM_PROCESS_CHANNEL_CHILD'] = str(plain / 'test_process_channel_child')
                output = run('apple-tests', ['swift', 'test', '--package-path', source / 'apple/GhostermCore', '--scratch-path', work / 'swift', '--filter', 'ExtensionProcessChannelTests'], 300)
                if 'Executed 6 tests, with 0 failures' not in output or 'Test skipped' in output: raise RuntimeError('Expected exact six enabled Swift cases\n' + output)
                if args.mutate and not args.group:
                    path = source / 'apple/GhostermCore/Sources/GhostermCore/ExtensionChannel.swift'
                    original = path.read_bytes()
                    mutations = [
                        ('swift-environment', 'config.environment_count = environment.count', 'config.environment_count = 0'),
                        ('swift-final', 'finalProcessObservation = try ProcessObservation(observation)', 'finalProcessObservation = nil')]
                    target = 'ExtensionProcessChannelTests.testRawArgumentsDirectoryEnvironmentAndFinalExit'
                    base = ['swift', 'test', '--package-path', source / 'apple/GhostermCore', '--scratch-path', work / 'swift']
                    for label, old, new in mutations:
                        if original.decode().count(old) != 1: raise RuntimeError('Ambiguous Swift mutation ' + label)
                        objects = sorted({p.resolve() for pattern in ('ExtensionChannel.swift.o', 'ExtensionChannel.o')
                            for p in (work / 'swift').rglob(pattern)})
                        if len(objects) != 1: raise RuntimeError('Expected one actual Swift object: ' + repr(objects))
                        before = sha(objects[0])
                        try:
                            path.write_text(original.decode().replace(old, new, 1) + '\n// ' + MARK + '\n')
                            objects[0].unlink()
                            run(label + '-red', [*base, '--filter', target], 300, 'swift')
                            if not objects[0].is_file() or sha(objects[0]) == before: raise RuntimeError('Swift object unchanged')
                            run(label + '-control', [*base, '--skip-build', '--filter', 'GhostermCoreTests.testConfigRoundTripIsByteIdentical'], 60)
                        finally: path.write_bytes(original)
                        objects[0].unlink(missing_ok=True)
                        run(label + '-green', [*base, '--filter', target], 300)
                        if sha(path) != hashlib.sha256(original).hexdigest(): raise RuntimeError('Swift restore mismatch')
                        apple_groups.append(label)
                        print('PASS ' + label + ': compiled XCTest red / same library config green / restored / SHA256', flush=True)
                    run('apple-final', [*base, '--skip-build', '--filter', 'ExtensionProcessChannelTests'], 60)
                apple = True
            full_cases = 0
            if args.full:
                full = work / 'full'
                run('full-configure', [*configure(full, True, False), '-DGHOSTERM_WITH_LIBSSH2=ON', '-DCMAKE_BUILD_TYPE=Debug'])
                run('full-build', ['cmake', '--build', full, '-j2'], 600)
                discovered = json.loads(run('full-discover', ['ctest', '--test-dir', full, '--show-only=json-v1']))['tests']
                full_cases = len(discovered)
                if not {'process_channel_runtime', 'process_channel_adopt'}.issubset({case['name'] for case in discovered}):
                    raise RuntimeError('Expected the enabled process channel cases')
                output = run('full-tests', ['ctest', '--test-dir', full, '--output-on-failure'], 900)
                if '100% tests passed out of ' + str(full_cases) not in output: raise RuntimeError('Complete enabled Core not green\n' + output)
                print('PASS complete Debug Lua/SSH-enabled Core: ' + str(full_cases) + ' CTest', flush=True)
            if any(sha(ROOT / name) != digest for name, digest in inputs.items()): raise RuntimeError('Root input changed')
            if sha(archive) != archive_sha: raise RuntimeError('Lua archive changed')
            receipt = dict(accepted=True, inputs=inputs, luaArchiveSHA256=archive_sha, cases=13, disabled=True, mutations=results, appleEnabledCases=6 if apple else 0, appleMutations=apple_groups, fullEnabledCases=full_cases,
                sanitizers=['address', 'undefined'], optimization='O1', leakSanitizer=False,
                scope='public local transport / Session / Lua Runtime; disposable child; no App enable/model/device')
            if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
            print('PROCESS CHANNEL PASS: 13 C + disabled; ' + str(len(results)) + ' mutations; Swift=' + str(apple), flush=True)
        finally:
            for name, original in originals.items(): (source / name).write_bytes(original)
            close_children()


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
