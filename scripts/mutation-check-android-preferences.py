#!/usr/bin/env python3
"""A7 preference storage mutations on a real device, with independent Core controls."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_PREFERENCES_MUTATION'
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
    model = ROOT / 'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/AppPreferences.kt'
    codec = ROOT / 'android/data/src/main/java/com/hanxi/ghosterm/data/PreferencesCodec.kt'
    repository = ROOT / 'android/data/src/main/java/com/hanxi/ghosterm/data/PreferencesRepository.kt'
    storage = ROOT / 'android/data/src/main/java/com/hanxi/ghosterm/data/PreferencesFile.kt'
    originals = {path: path.read_bytes() for path in (model, codec, repository, storage)}
    modes = {path: path.stat().st_mode & 0o777 for path in originals}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual preference mutation')
    tests = 'PreferencesStorageTest#'
    first = tests + 'firstLoadDoesNotCreateAFileAndAllValuesSurviveRestart'
    invalid = tests + 'invalidIndividualValuesFallBackWithoutRewritingTheDocument'
    damaged = tests + 'damagedOrFutureDocumentsNeverBecomeEmptyDefaultsOnDisk'
    missing = tests + 'missingFieldsMigrateInMemoryAndDuplicateFieldsAreRejected'
    before = tests + 'prePublicationFailurePreservesCommittedValuesAndAllowsRetry'
    after = tests + 'postPublicationFailureIsUnknownUntilExplicitReload'
    stale = tests + 'externalChangesAreNotOverwrittenAndResetOnlyTouchesPreferences'
    readback = tests + 'publicationMustReadBackTheActualCandidateBeforeReportingSaved'
    choices = tests + 'finalPrototypeChoiceIdsLoadAndSaveWithoutFallback'
    rapid = tests + 'rapidEditsSerializeAgainstTheLatestCommittedSnapshot'
    denied = tests + 'accessDeniedIsNotAnAbsentFileAndDoesNotEnableReset'
    default = tests + 'firstUseDefaultsMatchTheFinalPrototypeDocument'
    unchanged = 'if (same(raw(), expected)) PreferenceWrite.Unchanged else PreferenceWrite.Unknown'
    cases = []

    def case(name, target, *edits):
        cases.append((name, target, edits))

    def edit(path, old, new, count=1):
        return path, old, new, count

    case('absent-read', first, edit(storage, 'if (!checkDirectory(false)) null else locked { raw() }', 'if (!checkDirectory(true)) null else locked { raw() }'))
    case('persisted-field', first, edit(codec, 'put("fontSize", p.fontSize)', 'put("fontSize", 12)'))
    case('invalid-value-fallback', invalid, edit(codec, '.takeIf { n -> n in 10..20 }', '.takeIf { n -> n in 10..21 }'))
    case('no-read-repair', invalid, edit(repository,
        'val decoded = bytes?.let(PreferencesCodec::decode) ?: DecodedPreferences(AppPreferences())',
        'val decoded = bytes?.let(PreferencesCodec::decode) ?: DecodedPreferences(AppPreferences()); if (decoded.issues.isNotEmpty()) storage.write(bytes, PreferencesCodec.encode(decoded.values))'))
    case('read-error-not-default', damaged, edit(repository,
        'PreferencesState.Unavailable("无法读取偏好，请重试；原文件没有被重建。", previous).also { mutable.value = it }',
        'PreferencesState.Ready(AppPreferences(), emptySet(), ++generation).also { mutable.value = it }'))
    case('future-schema', damaged, edit(codec, 'root.getInt("version") == 1', 'root.getInt("version") >= 1'))
    case('missing-field-diagnostic', missing, edit(codec, 'issues += PreferenceIssue(key, PreferenceIssueKind.MISSING)', 'Unit'))
    case('duplicate-fields', missing, edit(codec, 'require(root.toString() == text)', 'require(true)'))
    case('unchanged-not-saved', before, edit(storage, unchanged, 'if (same(raw(), expected)) PreferenceWrite.Saved else PreferenceWrite.Unknown'))
    case('unknown-not-unchanged', after, edit(storage, unchanged, 'if (same(raw(), expected)) PreferenceWrite.Unchanged else PreferenceWrite.Unchanged'))
    case('published-readback', readback, edit(storage, 'check(same(raw(), bytes)) { "偏好发布后的内容无法确认" }', 'Unit'))
    case('line-height-id', choices, edit(model, 'RELAXED("relaxed", 2.15f)', 'RELAXED("spacious", 2.15f)'))
    case('ctrl-id', choices, edit(model, 'LOCKED("locked")', 'LOCKED("lock")'))
    case('keep-both-id', choices, edit(model, 'FileConflictPolicy.KEEP_BOTH -> "rename"', 'FileConflictPolicy.KEEP_BOTH -> "keepBoth"'))
    case('stale-document', stale, edit(storage, 'if (!same(raw(), expected)) return@locked PreferenceWrite.Unknown', 'if (false && !same(raw(), expected)) return@locked PreferenceWrite.Unknown', 2))
    case('reset-defaults', stale, edit(repository, 'suspend fun reset(): PreferenceUpdate = update { AppPreferences() }', 'suspend fun reset(): PreferenceUpdate = update { it }'))
    case('serialized-owner', rapid, edit(repository, 'mutex.withLock {', 'run withLock@ {', 2))
    case('withhold-candidate', rapid, edit(repository, 'val outcome = try { storage.write(original?.copyOf(), encoded.copyOf()) }',
        'mutable.value = PreferencesState.Ready(candidate, emptySet(), generation); val outcome = try { storage.write(original?.copyOf(), encoded.copyOf()) }'))
    case('latest-commit-transform', rapid, edit(repository, 'change(ready.values)', 'change(AppPreferences())'))
    case('publish-committed', first, edit(repository, 'mutable.value = committed', 'Unit'))
    case('failed-write-keeps-committed', before, edit(repository,
        'PreferenceWrite.Unchanged -> PreferenceUpdate.Failed("偏好未保存，原设置保持不变。")',
        'PreferenceWrite.Unchanged -> { mutable.value = PreferencesState.Ready(candidate, emptySet(), generation); PreferenceUpdate.Failed("偏好未保存，原设置保持不变。") }'))
    case('strict-boolean', invalid, edit(codec, 'it as? Boolean', 'when (it) { is Boolean -> it; is String -> it.toBooleanStrictOrNull(); else -> null }'))
    case('directory-no-follow', tests + 'aSymbolicLinkDirectoryCannotRedirectStorage', edit(storage,
        'Files.readAttributes(path, BasicFileAttributes::class.java, NOFOLLOW_LINKS)', 'Files.readAttributes(path, BasicFileAttributes::class.java)'))
    case('document-no-follow', tests + 'aSymbolicLinkDocumentCannotBeReadOrReplaced',
        edit(storage, 'Files.readAttributes(path, BasicFileAttributes::class.java, NOFOLLOW_LINKS)', 'Files.readAttributes(path, BasicFileAttributes::class.java)'),
        edit(storage, 'FileChannel.open(target, READ, NOFOLLOW_LINKS)', 'FileChannel.open(target, READ)'))
    case('denied-not-absent', denied, edit(repository, 'val bytes = storage.read()', 'val bytes = try { storage.read() } catch (_: Exception) { null }'))
    case('range-before-write', tests + 'outOfRangeEditsFailBeforeAnyStorageWrite', edit(model, 'require(fontSize in 10..20)', 'require(fontSize in 0..Int.MAX_VALUE)'))
    case('strict-utf8', damaged, edit(codec, 'onMalformedInput(CodingErrorAction.REPORT)', 'onMalformedInput(CodingErrorAction.REPLACE)'))
    case('document-size', damaged, edit(codec, 'const val LIMIT = 8192', 'const val LIMIT = 16384'))
    case('parser-depth', damaged, edit(codec, 'require(depth <= 4)', 'require(depth <= 64)'))
    case('prototype-default', default, edit(model, 'val fontSize: Int = 12', 'val fontSize: Int = 14'))
    case('private-directory', first, edit(storage, 'Os.chmod(directory.path, 0x1c0)', 'Os.chmod(directory.path, 0x1ed)'))
    case('unknown-blocks-writes', after, edit(repository,
        'PreferenceWrite.Unknown -> {\n                    mutable.value = PreferencesState.Unavailable("保存结果尚未确认，请重新载入后检查。", ready.values)',
        'PreferenceWrite.Unknown -> {\n                    Unit'))
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'S{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-preferences-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('PREFERENCE RECEIPTS', work, flush=True)

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
                raise RuntimeError('Preference source restore SHA256 mismatch: ' + path.name)

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
        output = run_logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
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
            label = f'S{number:02d}-{name}'
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
    print(f'ANDROID PREFERENCE MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
