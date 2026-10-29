#!/usr/bin/env python3
"""Android config transactions and UI: mutated APK red, independent test green, SHA restore."""
import hashlib
import importlib.util
import os
from pathlib import Path
import signal

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_CONFIG_MUTATION'
spec = importlib.util.spec_from_file_location('native_mutations', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
run, require = helper.run, helper.require


def main():
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set ANDROID_SERIAL to a test device')
    adb = ['adb', '-s', serial]
    require(run([*adb, 'get-state']))
    repo = ROOT / 'android/data/src/main/java/com/hanxi/ghosterm/data/ConfigRepository.kt'
    domain = ROOT / 'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/Hosts.kt'
    screen = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/HostsViewModel.kt'
    native = ROOT / 'android/bridge/src/main/cpp/config_jni.c'
    core = ROOT / 'core/src/config.c'
    originals = {p: p.read_bytes() for p in (repo, domain, screen, model, native, core)}
    if any(MARK.encode() in text for text in originals.values()): raise RuntimeError('mutation marker present')
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    work = ROOT / 'tmp/android-config-mutations'; work.mkdir(exist_ok=True)
    def build():
        result = run(['bash', ROOT / 'scripts/build-android.sh'], seconds=900)
        (work / 'build.log').write_text(result.stdout + result.stderr)
        require(result)
        for apk in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            require(run([*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / apk]))
    def test(method, green):
        ui_tests = {'addEditSearchAndDiscardUseRealConfiguration', 'fieldSourcesAndSharedBlockWarningAreVisible',
                    'readOnlyHostCanInspectConfigAndReturnWithFreshOwnership',
                    'inheritanceSwitchSavesSameValueAndThenRemovesOverride',
                    'inheritanceChangesRequireDiscardAndRevertingIsClean',
                    'invalidExplicitValueKeepsDraftAndCanReturnToInheritance',
                    'allFieldsCanInheritForNewAndExistingHost', 'renameUsesRealFormAndCollisionKeepsDraft'}
        name = ('HostScreenTest' if method in ui_tests else 'ConfigRepositoryTest') + '#' + method
        result = run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                      'com.hanxi.ghosterm.' + name,
                      'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        output = result.stdout + result.stderr
        (work / (method + '.log')).write_text(output)
        passed = result.returncode == 0 and 'OK (1 test)' in output
        if passed != green or (not green and (not any(kind in output for kind in ('AssertionError', 'org.junit.ComparisonFailure')) or 'FAILURES' not in output)):
            raise RuntimeError(output)
    control = 'firstSaveAndReloadDeriveHostsFromConfig'
    cases = [
        (repo, 'throw ConfigAccess("备份失败，原配置未修改", error)', 'Unit', 'backupFailureLeavesOriginal', control),
        (repo, 'if (revision != previous.revision) throw ConfigConflict()', 'if (false && revision != previous.revision) throw ConfigConflict()', 'equalSizeExternalEditIsRejected', control),
        (repo, 'if (raw().second != revision) throw ConfigConflict()', 'if (false && raw().second != revision) throw ConfigConflict()', 'externalEditDuringBackupIsRejected', control),
        (repo, 'ops.replace(temp, target)', 'Files.deleteIfExists(target); ops.replace(temp, target)', 'replaceFailureNeverDeletesOriginal', control),
        (domain, 'if (field in inherited) {', 'if (field in inherited && field != HostField.USER) {', 'inheritedValuesAreNotWrittenBack', control),
        (repo, 'name.isNotBlank() && name.none { it in "*?![]" || it.isWhitespace() }', 'name.isNotBlank()', 'sparseEditAndBackupPreserveUnchangedBytes', control),
        (native, '.default_user = ""', '.default_user = "android_uid"', 'unspecifiedRemoteUserRemainsEmpty', control),
        (screen, 'model.edit(host)', 'model.edit()', 'addEditSearchAndDiscardUseRealConfiguration', control),
        (repo, 'if (lines.replace(renamed.toString(Charsets.UTF_8), "") != lines.replace(candidate.toString(Charsets.UTF_8), ""))',
         'if (false && lines.replace(renamed.toString(Charsets.UTF_8), "") != lines.replace(candidate.toString(Charsets.UTF_8), ""))',
         'unrelatedLineChangeIsRejected', control),
        (repo, 'throw ConfigAccess("备份失败，原配置未修改", error)',
         'Files.deleteIfExists(backup); throw ConfigAccess("备份失败，原配置未修改", error)',
         'backupNameCollisionDoesNotDeleteExistingFile', control),
        (repo, 'if (Regex("-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----").containsMatchIn(bytes.toString(Charsets.UTF_8)))',
         'if (false && Regex("-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----").containsMatchIn(bytes.toString(Charsets.UTF_8)))',
         'privateKeyContainerIsNeverSavedAsConfiguration', control),
        (core, 'elist_remove_matches(&l, first + 1, e2, e->keyword);\n        if (elist_splice(&l, first, 1, rl, rn, e->keyword, blk) != 0)',
         '/* contiguous splice regression */\n        if (elist_splice(&l, first, nfound, rl, rn, e->keyword, blk) != 0)',
         'complexDuplicateEditPreservesOtherDirectives', control),
        (core, 's->origin = n;', 's->origin = NULL;',
         'provenanceDistinguishesShadowedExplicitInheritedAndDefault', 'backupFailureLeavesOriginal'),
        (native, 'info.included ? "1" : "0"', '"0"',
         'ambiguousAndIncludedHostsAreReadOnlyBeforeEditing', control),
        (repo, 'if (host != null && host.source?.editable != true)', 'if (host != null && host.source?.editable == null)',
         'ambiguousAndIncludedHostsAreReadOnlyBeforeEditing', control),
        (screen, 'provenance?.let { Text(it.description) }', 'provenance?.let { Text("") }',
         'fieldSourcesAndSharedBlockWarningAreVisible', control),
        (screen, 'if (source.patternCount > 1)', 'if (false && source.patternCount > 1)',
         'fieldSourcesAndSharedBlockWarningAreVisible', control),
        (screen, 'enabled = !hosts.busy && hosts.canEditDraft', 'enabled = !hosts.busy',
         'readOnlyHostCanInspectConfigAndReturnWithFreshOwnership', control),
        (model, 'error = null; discard = false\n        restoreDraftOrClear()', 'error = null; discard = false\n        draft = null',
         'readOnlyHostCanInspectConfigAndReturnWithFreshOwnership', control),
        (model, 'notice = saved.warning ?: "SSH 配置已保存"; page = configReturn\n            restoreDraftOrClear()',
         'notice = saved.warning ?: "SSH 配置已保存"; page = configReturn',
         'readOnlyHostCanInspectConfigAndReturnWithFreshOwnership', control),
        (domain, 'else if (value(field) != explicit) put(field.keyword, value(field))',
         'else if (previous == null || value(field) != field.effective(previous)) put(field.keyword, value(field))',
         'sameValueCanBeExplicitAndRestoredToInheritance', control),
        (native, 'edits[i].values = has_value ? tokens[i] : NULL;',
         'tokens[i][0] = has_value ? value_storage[i] : ""; edits[i].values = tokens[i];',
         'emptyInheritedUserCanBeKeptAndRestored', control),
        (domain, 'if (HostField.USER !in inherited) require(validToken(user))',
         'require(validToken(user))', 'emptyInheritedUserCanBeKeptAndRestored', control),
        (repo, 'if (host != null && changes.isEmpty() && !renaming)', 'if (changes.isEmpty() && !renaming)',
         'newHostMayInheritEveryConnectionField', control),
        (repo, 'if (requested != null && !declared.effectiveIsExplicit)',
         'if (false && requested != null && !declared.effectiveIsExplicit)',
         'shadowedOverrideIsRejectedBeforeBackup', control),
        (domain, 'field.source(host)?.explicitValue ?: field.effective(host)', 'field.effective(host)',
         'unchangedShadowedExplicitValueIsNotRewritten', control),
        (repo, 'if (declared.explicitValue != requested)', 'if (false && declared.explicitValue != requested)',
         'editorMustActuallyRemoveRequestedDirective', control),
        (model, 'val dirty: Boolean get() = edit.isDirty(original)', 'val dirty: Boolean get() = false',
         'inheritanceChangesRequireDiscardAndRevertingIsClean', control),
        (screen, 'model.draft = draft.copy(edit = edit.withInheritance(field, it))', 'model.draft = draft',
         'inheritanceSwitchSavesSameValueAndThenRemovesOverride', control),
        (domain, 'if (HostField.USER !in inherited) require(validToken(user))',
         'if (false && HostField.USER !in inherited) require(validToken(user))',
         'invalidExplicitValueKeepsDraftAndCanReturnToInheritance', control),
        (domain, 'if (explicit != null) put(field.keyword, null)',
         'if (explicit != null && field != HostField.PORT) put(field.keyword, null)',
         'allFieldsCanInheritForNewAndExistingHost', control),
        (screen, 'val enabled = !model.busy && model.canEditDraft', 'val enabled = !model.busy',
         'readOnlyHostCanInspectConfigAndReturnWithFreshOwnership', control),
        (native, 'gt_config_rename_host(cfg, alias, new_alias)', 'gt_config_rename_host(cfg, alias, alias)',
         'renamePreservesSiblingAndCanEditFieldsInSameSave', control),
        (repo, 'if (host != null && changes.isEmpty() && !renaming)', 'if (host != null && changes.isEmpty())',
         'renameOnlyAndCaseChangeKeepBodyBytes', control),
        (repo, 'if (before != after)', 'if (false && before != after)',
         'renameCollisionAndCorruptOutputNeverCommit', control),
        (screen, 'enabled = enabled, modifier = Modifier.fillMaxWidth(),',
         'enabled = enabled && draft.original == null, modifier = Modifier.fillMaxWidth(),',
         'renameUsesRealFormAndCollisionKeepsDraft', control),
    ]
    build()
    for name in dict.fromkeys(name for case in cases for name in case[3:]): test(name, True)
    try:
        for index, (path, before, after, target, independent) in enumerate(cases, 1):
            source = originals[path].decode()
            if source.count(before) != 1: raise RuntimeError('missing or ambiguous mutation anchor: ' + before)
            path.write_text(source.replace(before, after + ' /* ' + MARK + ' */'))
            build()
            test(target, False)
            test(independent, True)
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[path]: raise RuntimeError('restore SHA mismatch')
            build(); test(target, True)
            print(f'C{index}: target red / independent green / restored green / SHA256 identical', flush=True)
    finally:
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest() != h for p, h in hashes.items()): raise RuntimeError('source changed during mutation run')
    print(f'ANDROID CONFIG MUTATIONS PASS: {len(cases)} groups')


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
