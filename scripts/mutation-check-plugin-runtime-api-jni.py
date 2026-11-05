#!/usr/bin/env python3
"""Runtime JNI/Kotlin mutations on an explicitly selected disposable device.

Every variant recompiles/relinks JNI, installs both exact APKs, observes one
ordinary JUnit assertion and an independent same-APK Core control, then restores.
Only owned changes are restored; concurrent edits are preserved and reported.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_PLUGIN_RUNTIME_API_JNI_MUTATION'
PATHS = {'cpp': ROOT / 'android/bridge/src/main/cpp/extension_jni.c', 'kotlin': ROOT / 'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/ExtensionRuntime.kt'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--case', action='append', default=[])
    args = parser.parse_args()
    originals = {key: path.read_bytes() for key, path in PATHS.items()}
    if any(MARK.encode() in value for value in originals.values()):
        print('residual runtime JNI mutation', file=sys.stderr)
        raise SystemExit(2)
    cases = [
        ('jni-phase', [('cpp', 'values[10]={i.phase,i.error', 'values[10]={(i.phase==GT_EXT_RUNTIME_ACTIVE?GT_EXT_RUNTIME_STOPPED:i.phase),i.error')], 'adoptionAndActualPluginRPC'),
        ('jni-cpu-error', [('cpp', 'values[10]={i.phase,i.error', 'values[10]={i.phase,(i.error==GT_EXT_RUNTIME_CPU?GT_EXT_RUNTIME_SCRIPT:i.error)')], 'cpuAndImportFailuresRemainDistinct'),
        ('jni-job-count', [('cpp', '(jlong)i.steps,(jlong)i.jobs,(jlong)i.module_bytes', '(jlong)i.steps,0,(jlong)i.module_bytes')], 'adoptionAndActualPluginRPC'),
        ('jni-stop', [('cpp', 'if(s){gt_ext_runtime_stop(s->runtime);extension_unpin(s);}', 'if(s){if(false)gt_ext_runtime_stop(s->runtime);extension_unpin(s);}')], 'stopFromMainAndOtherWorkerRejected'),
        ('jni-successor-generation', [('cpp', 'status=exclusive?gt_ext_runtime_open(&s->session,&c,&s->runtime):GT_ERR_IO;jlong values[2]={status,0};pthread_mutex_lock(&extension_lock);if(status==GT_OK){s->id=successor;', 'status=exclusive?gt_ext_runtime_open(&s->session,&c,&s->runtime):GT_ERR_IO;jlong values[2]={status,0};pthread_mutex_lock(&extension_lock);if(status==GT_OK){(void)successor;')], 'generationalIdTransfer'),
        ('jni-kotlin-worker-guard', [
            ('cpp', 'if(s && owner && !pthread_equal(s->owner,pthread_self())){pthread_mutex_unlock(&extension_lock);if(!quiet)extension_error(e,"运行时必须在原 IO worker 调用");', 'if(s && false && owner && !pthread_equal(s->owner,pthread_self())){pthread_mutex_unlock(&extension_lock);if(!quiet)extension_error(e,"运行时必须在原 IO worker 调用");'),
            ('kotlin', 'check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper() && handle != 0L)', 'check(handle != 0L)')], 'stopFromMainAndOtherWorkerRejected'),
        ('kotlin-cpu-error', [('kotlin', 'Failure.entries[v[1].toInt()]', 'if (v[1] == 3L) Failure.SCRIPT else Failure.entries[v[1].toInt()]')], 'cpuAndImportFailuresRemainDistinct'),
    ]
    known = {c[0] for c in cases}
    if set(args.case) - known:
        parser.error('unknown case: ' + ', '.join(sorted(set(args.case) - known)))
    if args.case:
        cases = [c for c in cases if c[0] in args.case]
    for label, changes, _ in cases:
        for key, old, _ in changes:
            if originals[key].decode().count(old) != 1:
                raise RuntimeError('missing/ambiguous anchor: ' + label + ' ' + key)
    inputs = []
    for folder in ['core/src', 'core/include', 'android/bridge/src/main', 'android/app/src/pluginRuntimeTest', 'packages/plugin-sdk']:
        inputs += [p for p in (ROOT / folder).rglob('*') if p.is_file()]
    inputs += [Path(__file__), ROOT / 'core/CMakeLists.txt', ROOT / 'android/app/build.gradle.kts', ROOT / 'android/bridge/build.gradle.kts', ROOT / 'android/app/src/androidTest/java/com/hanxi/ghosterm/ExtensionRuntimeAvailabilityTest.kt']
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs}
    work = Path(tempfile.mkdtemp(prefix='plugin-runtime-api-jni-mutations-', dir=ROOT / 'tmp'))
    logs = work / 'logs'
    logs.mkdir()
    for key, value in originals.items():
        (work / (PATHS[key].name + '.original')).write_bytes(value)
    env = dict(os.environ, TMPDIR=str(work), JAVA_HOME=str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'), ZIG=str(ROOT / 'build/android-toolchain/zig-aarch64-macos-0.16.0/zig'))
    adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', args.device]
    apk = ROOT / 'android/app/build/outputs/apk/debug/app-debug.apk'
    tests = ROOT / 'android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk'
    commands, owned = [], {}

    def run(label, cmd, budget=240):
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-runtime-jni; shift; gt_timeout "$@"'
        p = subprocess.run(['bash', '-c', shell, 'plugin-runtime-jni', str(ROOT), str(budget), *map(str, cmd)], cwd=ROOT, env=env, capture_output=True, text=True, timeout=budget + 10)
        output = p.stdout + p.stderr
        (logs / (label + '.log')).write_text(output)
        commands.append(dict(label=label, returncode=p.returncode))
        if p.returncode:
            raise RuntimeError(label + '\n' + output)
        return output

    def gradle(label):
        return run(label, ['bash', ROOT / 'scripts/build-android.sh', '-PghostermPluginJs=true', '-PghostermRuntimeSdk=' + str(work / 'assets'), ':app:assembleDebug', ':app:assembleDebugAndroidTest'], budget=600)

    def installed(label, package, artifact):
        run(label + '-install', [*adb, 'install', '-r', artifact], budget=60)
        path = run(label + '-path', [*adb, 'shell', 'pm', 'path', package], budget=20).strip()
        assert path.startswith('package:') and '\n' not in path
        digest = run(label + '-installed-sha', [*adb, 'shell', 'sha256sum', path[len('package:'):]], budget=20).split()[0]
        assert digest == sha(artifact), label + ' installed bytes differ'
        return digest

    def instrumentation(label, selection, count, red=False):
        output = run(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', selection, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], budget=120)
        if red:
            if not ('FAILURES!!!' in output and re.search(r'Tests run: 1,\s+Failures: 1', output) and ('java.lang.AssertionError' in output or 'junit.framework.AssertionFailedError' in output)):
                raise RuntimeError(label + ': expected ordinary single assertion\n' + output)
            if any(s in output for s in ['Process crashed', 'INSTRUMENTATION_FAILED', 'Test run failed', 'Fatal signal', 'java.lang.IllegalStateException:']):
                raise RuntimeError(label + ': not an ordinary assertion\n' + output)
        elif 'OK (' + str(count) + ' test' not in output or 'FAILURES' in output or 'INSTRUMENTATION_STATUS_CODE: -3' in output:
            raise RuntimeError(label + ': discovery/result mismatch\n' + output)

    try:
        assert run('device-state', [*adb, 'get-state'], budget=15).strip() == 'device'
        props = {key: run('prop-' + key, [*adb, 'shell', 'getprop', key], budget=15).strip() for key in ['ro.build.version.sdk', 'ro.product.cpu.abi', 'ro.kernel.qemu']}
        page = run('page-size', [*adb, 'shell', 'getconf', 'PAGESIZE'], budget=15).strip()
        run('reverse-list', [*adb, 'reverse', '--list'], budget=15)
        tools = work / 'tools'
        tools.mkdir()
        for name in ['package.json', 'package-lock.json']:
            (tools / name).write_bytes((ROOT / 'scripts/fixtures/plugin-sdk-tools' / name).read_bytes())
        npmrc = work / 'empty.npmrc'
        npmrc.touch()
        env['NPM_CONFIG_USERCONFIG'] = str(npmrc)
        run('tools', ['npm', 'ci', '--prefix', tools, '--ignore-scripts', '--no-audit', '--no-fund', '--offline', '--cache', ROOT / 'tmp/plugin-sdk-npm-cache'])
        sdk = work / 'assets/runtime-sdk'
        run('sdk', ['node', tools / 'node_modules/typescript/bin/tsc', '-p', ROOT / 'packages/plugin-sdk/tsconfig.json', '--outDir', sdk])
        gradle('initial-apks')
        candidates = [p.parent for p in (ROOT / 'android/bridge/.cxx/Debug').glob('*/arm64-v8a/CMakeCache.txt') if 'GHOSTERM_WITH_PLUGIN_JS:BOOL=ON\n' in p.read_text()]
        if len(candidates) != 1:
            raise RuntimeError('expected one explicit enabled JNI build: ' + repr(candidates))
        ndk = candidates[0]
        shared = ROOT / 'android/bridge/build/intermediates/cxx/Debug' / ndk.parent.name / 'obj/arm64-v8a/libghosterm_android.so'
        obj = ndk / 'CMakeFiles/ghosterm_android.dir/extension_jni.c.o'

        def rebuild(label, kotlin=False):
            obj.unlink(missing_ok=True)
            shared.unlink(missing_ok=True)
            output = run(label + '-native', ['cmake', '--build', ndk, '--target', 'ghosterm_android', '--verbose'])
            assert obj.is_file() and 'extension_jni.c' in output and ' -c ' in output, label
            assert shared.is_file() and ' -shared ' in output and '-o ' in output, label
            output = gradle(label + '-apks')
            if kotlin:
                line = next((s for s in output.splitlines() if s.startswith('> Task :bridge:compileDebugKotlin')), '')
                assert line and not any(s in line for s in ['UP-TO-DATE', 'FROM-CACHE']), label + ' Kotlin not compiled'
            app_sha = installed(label + '-app', 'com.hanxi.ghosterm', apk)
            test_sha = installed(label + '-tests', 'com.hanxi.ghosterm.test', tests)
            with zipfile.ZipFile(apk) as z:
                library = hashlib.sha256(z.read('lib/arm64-v8a/libghosterm_android.so')).hexdigest()
                assert z.read('assets/licenses/QuickJSNG-MIT.txt') == (ROOT / 'vendor/quickjs-ng/LICENSE').read_bytes()
            with zipfile.ZipFile(tests) as z:
                for p in sdk.glob('*.js'):
                    assert z.read('assets/runtime-sdk/' + p.name) == p.read_bytes()
            merged = ROOT / 'android/app/build/intermediates/merged_native_libs/debug/mergeDebugNativeLibs/out/lib/arm64-v8a/libghosterm_android.so'
            stripped = ROOT / 'android/app/build/intermediates/stripped_native_libs/debug/stripDebugDebugSymbols/out/lib/arm64-v8a/libghosterm_android.so'
            assert sha(merged) == sha(shared), label + ' merged JNI differs from linked input'
            assert library == sha(stripped), label + ' packaged JNI differs from stripped input'
            (logs / (label + '-artifact.json')).write_text(json.dumps(dict(cpp_sha256=sha(PATHS['cpp']), kotlin_sha256=sha(PATHS['kotlin']), object_sha256=sha(obj), installed_apk_sha256=app_sha, installed_test_apk_sha256=test_sha, linked_jni_sha256=sha(shared), packaged_jni_sha256=library), indent=2) + '\n')

        all_tests = 'com.hanxi.ghosterm.ExtensionRuntimeTest,com.hanxi.ghosterm.ExtensionRuntimeAvailabilityTest'
        rebuild('baseline')
        instrumentation('baseline-tests', all_tests, 8)
        for label, changes, method in cases:
            try:
                for key, old, new in changes:
                    owned[key] = (originals[key].decode().replace(old, new) + '\n/* ' + MARK + ' */\n').encode()
                    PATHS[key].write_bytes(owned[key])
                rebuild(label, any(k == 'kotlin' for k, _, _ in changes))
                instrumentation(label + '-target', 'com.hanxi.ghosterm.ExtensionRuntimeTest#' + method, 1, True)
                variant = sha(apk)
                instrumentation(label + '-control', 'com.hanxi.ghosterm.NativeSmokeTest', 3)
                assert sha(apk) == variant
            finally:
                for key, _, _ in changes:
                    if PATHS[key].read_bytes() not in (originals[key], owned.get(key)):
                        raise RuntimeError('concurrent edit; backup retained: ' + key)
                    PATHS[key].write_bytes(originals[key])
                    assert sha(PATHS[key]) == hashlib.sha256(originals[key]).hexdigest()
            rebuild(label + '-restored', any(k == 'kotlin' for k, _, _ in changes))
            instrumentation(label + '-restored-target', 'com.hanxi.ghosterm.ExtensionRuntimeTest#' + method, 1)
            print(label + ': actual JNI/Kotlin compile/shared link / JUnit assertion red / same APK Core green / restored / SHA256', flush=True)
        instrumentation('final-tests', all_tests, 8)
        for name, digest in hashes.items():
            assert sha(ROOT / name) == digest, name
        (logs / 'result.json').write_text(json.dumps(dict(accepted=True, groups=[c[0] for c in cases], device=args.device, properties=props, page_size=page, inputs_sha256=hashes, commands=commands, app_sha256=sha(apk), test_apk_sha256=sha(tests), sdk_sha256={p.name: sha(p) for p in sdk.glob('*.js')}, scope='native worker runtime / real JS RPC / API-selected simulator; no App activation UI, SSH, WAN, physical device, release or gateway'), indent=2) + '\n')
        print('PUBLIC RUNTIME JNI MUTATIONS PASS; logs: ' + str(logs), flush=True)
    except BaseException as error:
        (logs / 'failure.txt').write_text(repr(error) + '\n')
        raise
    finally:
        for key, path in PATHS.items():
            if path.read_bytes() != originals[key]:
                if path.read_bytes() != owned.get(key):
                    raise RuntimeError('concurrent edit; backup retained: ' + key)
                path.write_bytes(originals[key])
            assert sha(path) == hashlib.sha256(originals[key]).hexdigest()


def stop(number, _frame):
    raise KeyboardInterrupt('signal ' + str(number))


if __name__ == '__main__':
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    main()
