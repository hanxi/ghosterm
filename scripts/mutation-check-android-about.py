#!/usr/bin/env python3
"""A7 settings controller and native page mutations on a real device, with independent Core controls."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_ABOUT_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the preference test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    base = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm'
    model = base / 'AboutViewModel.kt'
    screen = base / 'AboutContent.kt'
    app = base / 'MainActivity.kt'
    gradle = ROOT / 'android/app/build.gradle.kts'
    originals = {path: path.read_bytes() for path in (model, screen, app, gradle)}
    modes = {path: path.stat().st_mode & 0o777 for path in originals}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual settings mutation')
    cases = []
    def case(name, test, path, old, new, count=1):
        cases.append((name, test, ((path, old, new, count),)))
    product = 'AboutProductTest#actualBuildAndPackagedLicensesHaveOneBackPathAndSurviveRecreation'
    malformed = 'AboutModelTest#missingMalformedAndUnknownBuildInfoNeverBecomeClean'
    source = 'AboutSourceActionTest#projectSourceButtonDispatchesOnlyTheExplicitProjectIntent'
    source_error = 'AboutSourceActionTest#unavailableBrowserShowsAnErrorAndTheProjectAddress'
    case('actual-commit-row', product, screen, 'Text(info.commit ?: "无法确认提交",', 'Text("无法确认提交",')
    case('dirty-is-not-clean', malformed, model, '"dirty" -> "包含未提交的修改"', '"dirty" -> "工作区干净"')
    cases.append(('missing-state-never-defaults-clean', malformed, ((model,
        'require(value.keys().asSequence().toSet() == setOf("schema", "git_commit", "source_state"))',
        'require(value.keys().asSequence().toSet().all { it in setOf("schema", "git_commit", "source_state") })', 1), (model,
        'val state = value.get("source_state")', 'val state = value.optString("source_state", "clean")', 1))))
    case('no-commit-is-not-clean', malformed, model, 'require(commit != null || state == "unknown")', 'Unit')
    case('license-read-error-is-visible', 'AboutModelTest#failedReadsRemainVisibleAndCanBeRetried', model,
        'catch (_: Exception) { AboutDocument.Failed }', 'catch (_: Exception) { AboutDocument.Ready("") }')
    cases.append(('old-license-cannot-replace-new-page', 'AboutModelTest#aLateLicenseReadCannotReplaceTheNewPageOrReopenAfterBack', ((model,
        'documentJob?.cancel()', 'Unit', 2), (model,
        'if (ticket == current && selected == license) document = result', 'document = result', 1))))
    case('license-back-stays-in-about', product, app,
        'if (hosts.page == Page.ABOUT && about.back()) return', 'if (false) return')
    case('license-file-matches-component', product, model,
        'GHOSTERM("Ghosterm · MIT", "Ghosterm-MIT.txt")', 'GHOSTERM("Ghosterm · MIT", "openssl-3.5.4.txt")')
    case('explicit-source-button-route', source, screen,
        '{ model.openSource(context) }','{ Unit }')
    case('source-launch-error-visible', source_error, model,
        'catch (_: android.content.ActivityNotFoundException) { sourceUnavailable() }',
        'catch (_: android.content.ActivityNotFoundException) { }')
    case('packaged-commit-is-this-checkout', product, gradle,
        'val commit = sourceCommit?.let { "\\\"$it\\\"" } ?: "null"',
        'val commit = "\\\"'+'b'*40+'\\\""')
    case('packaged-state-is-not-forced-clean', product, gradle,
        'val commit = sourceCommit?.let', 'val sourceState = "clean"; val commit = sourceCommit?.let')
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'C{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-about-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('SETTINGS RECEIPTS', work, flush=True)

    def write_source(path, data):
        temporary = work / ('source-' + str(time.time_ns()) + '.kt')
        temporary.write_bytes(data)
        temporary.chmod(modes[path])
        os.replace(temporary, path)

    def restore():
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                write_source(path, data)
            if path.read_bytes() != data:
                raise RuntimeError('Settings source restore SHA256 mismatch: ' + path.name)

    def run_logged(label, command, seconds=120):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result)
        return output

    def build(label):
        run_logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh'], 900)
        for name in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            run_logged(label + '-install-' + Path(name).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / name])

    def test(label, target, green):
        output = run_logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'aboutExpectedCommit', subprocess.check_output(['git','rev-parse','HEAD'], cwd=ROOT, text=True).strip(), '-e', 'aboutExpectedState', ('dirty' if subprocess.check_output(['git','status','--porcelain','--untracked-files=normal'], cwd=ROOT, text=True).strip() else 'clean'), '-e', 'class',
            'com.hanxi.ghosterm.' + target, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        if green:
            good = 'OK (1 test)' in output and 'FAILURES' not in output
        else:
            good = bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and any(
                kind in output for kind in ('java.lang.AssertionError', 'org.junit.ComparisonFailure'))
            good = good and 'Fixture:' not in output
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not good:
            raise RuntimeError(output)

    results = []
    try:
        build('baseline')
        for index, target in enumerate(dict.fromkeys(cases[number - 1][1] for number in selected)):
            test('baseline-' + str(index), target, True)
        for number in selected:
            name, target, edits = cases[number - 1]
            variants = {}
            for path, old, new, count in edits:
                variants[path] = variants.get(path, originals[path].decode()).replace(old, new)
            for path, source in variants.items():
                write_source(path, (source + '\n// ' + MARK + '\n').encode())
            label = f'C{number:02d}-{name}'
            build(label)
            test(label + '-target', target, False)
            test(label + '-control', 'ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            restore()
            build(label + '-restored')
            test(label + '-restored', target, True)
            results.append(dict(case=number, name=name, compiled_assertion_red=True, same_apk_core_green=True, restored_green=True))
            (work / 'accepted-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(label + ': compiled single assertion red / same APK Core green / restored green / SHA256', flush=True)
    finally:
        restore()
        helper.require(helper.run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm']))
    print(f'ANDROID ABOUT MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
