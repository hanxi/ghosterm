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
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_SETTINGS_MUTATION'
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
    # Match test-android-ssh.py: locked API 29 fixtures require system-setting
    # privilege. Do not mistake a rejected fixture command for a product failure.
    permissions = helper.run([*adb,'shell','dumpsys','package','com.android.shell']).stdout
    night_locked = 'mNightModeLocked=true' in helper.run([*adb,'shell','dumpsys','uimode']).stdout
    privileged_night = night_locked and 'android.permission.MODIFY_DAY_NIGHT_MODE: granted=true' not in permissions
    if privileged_night:
        result = helper.run([*adb,'shell','su','0','id'])
        helper.require(result)
        if 'uid=0' not in result.stdout:
            raise RuntimeError('Locked night-mode fixture requires privileged system setting access')
    base = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm'
    model, owner, screen = (base / name for name in ('SettingsViewModel.kt', 'AppSettings.kt', 'SettingsContent.kt'))
    originals = {path: path.read_bytes() for path in (model, owner, screen, base / "MainActivity.kt", base / "FilesScreen.kt", base / "GhostermTheme.kt")}
    modes = {path: path.stat().st_mode & 0o777 for path in originals}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual settings mutation')
    cases = []
    def case(name, test, path, old, new, count=1):
        cases.append((name, test, ((path, old, new, count),)))
    control = 'SettingsControllerTest#'
    page = 'SettingsScreenTest#'
    case('newer-equal-candidate', control+'olderReceiptsCannotClearANewerEqualCandidate', model,
        'if (edits[key]?.serial == value.serial)', 'if (edits[key]?.edit == value.edit)')
    case('failed-draft-retained', control+'failuresKeepCandidatesForRetryOrDiscard', model,
        'is PreferenceUpdate.Failed -> if (ticket == attempt) error = result.message',
        'is PreferenceUpdate.Failed -> { edits.clear(); if (ticket == attempt) error = result.message }')
    case('windows-patch-latest', control+'windowsMergeOnlyTheirSelectedKeys', owner,
        'operation.edits.fold(before)', 'operation.edits.fold(AppPreferences())')
    case('unknown-blocks-editing', control+'unknownResultsRequireConfirmedReloadBeforeEditing', model,
        'storage is PreferencesState.Ready && !reloading', 'storage !is PreferencesState.Loading && !reloading')
    case('failed-reload-retains-draft', control+'failedReloadRetainsTheCandidateUntilSuccessfulConfirmation', model,
        'if (result.state is PreferencesState.Ready)', 'if (true)')
    case('reset-whole-document', control+'resetRequiresConfirmationAndPublishesOneWholeTransaction', owner,
        'if (operation.reset) repository.reset()', 'if (operation.reset) repository.update { it }')
    case('discard-removes-draft', control+'failuresKeepCandidatesForRetryOrDiscard', model,
        'edits.clear(); error = null; refreshCandidate()', 'error = null; refreshCandidate()')
    case('leave-consumed-once', control+'leavingAnUnsavedOverviewRequiresDiscardAndConsumesTheEventOnce', model,
        'leaveRequested.also { leaveRequested = false }', 'leaveRequested.also { Unit }')
    case('category-back-restores-overview', control+'leavingAnUnsavedOverviewRequiresDiscardAndConsumesTheEventOnce', model,
        'if (section != null) { section = null; return true }', 'if (section != null) { return true }')
    case('older-success-keeps-newest-error', control+'queueRejectionKeepsTheNewestDraftAfterOlderWritesFinish', model,
        'if (ticket == attempt) error = null', 'error = null')
    case('one-tap-one-toggle', page+'singleSwitchTapPersistsAndRecreationKeepsCategoryAndValue', screen,
        'onValueChange=change','onValueChange={ change(!it) }')
    case('choice-saves', page+'choicesSliderAndConfirmedResetUseProductionStorage', screen,
        'model.edit(PreferenceEdit.Theme(it));choice=null', 'choice=null')
    case('confirmed-reset-action', page+'choicesSliderAndConfirmedResetUseProductionStorage', screen,
        'TextButton(model::confirmReset,', 'TextButton(model::cancelReset,')
    case('all-five-categories', page+'fiveCategoriesHaveOneBackPathAndSecurityRoutes', screen,
        'GhostermActionRow(section.title,summary,icon,Modifier.testTag("settings-section-${section.name}")) { model.open(section) }','if(section!=SettingsSection.SECURITY) GhostermActionRow(section.title,summary,icon,Modifier.testTag("settings-section-${section.name}")) { model.open(section) }')
    case('unknown-page-disabled', page+'damagedDocumentIsVisibleAndDisabledUntilReloadSucceeds', screen,
        'val enabled = model.canEdit', 'val enabled = true')
    case('slider-measured-touch-size', page+'categoryRowsRemainReachableInsideTheScreen', screen,
        'thumb = { Box(Modifier.height(48.dp)', 'thumb = { Box(Modifier.height(24.dp)')
    app = base / 'MainActivity.kt'
    appearance = page+'applicationAppearanceChangesWindowPixelsBarsAndSurvivesRecreation'
    case('actual-dark-background', appearance, base/'GhostermTheme.kt',
        'val bg=if(dark) Color(0xFF171B20)','val bg=if(dark) Color(0xFF000000)')
    case('actual-light-background', appearance, base/'GhostermTheme.kt',
        'else Color(0xFFF7F8FC)','else Color(0xFFFFFFFF)')
    case('status-icons-follow-appearance', appearance, app,
        'isAppearanceLightStatusBars = !dark', 'isAppearanceLightStatusBars = true')
    case('navigation-icons-follow-appearance', appearance, app,
        'isAppearanceLightNavigationBars = !dark', 'isAppearanceLightNavigationBars = true')
    case('system-night-mode-dark', page+'systemAppearanceTracksAndroidNightModeInBothDirections', app,
        'AppAppearance.SYSTEM -> isSystemInDarkTheme()', 'AppAppearance.SYSTEM -> false')
    case('system-night-mode-light', page+'systemAppearanceTracksAndroidNightModeInBothDirections', app,
        'AppAppearance.SYSTEM -> isSystemInDarkTheme()', 'AppAppearance.SYSTEM -> true')
    file_menu = page+'fileMenuHiddenPreferencePersistsAndSettingsUpdatesTheExistingBrowser'
    case('file-menu-hidden-action', file_menu, base / 'FilesScreen.kt',
        'onClick={ menu=false;model.toggleHidden() }', 'onClick={ menu=false }')
    case('file-menu-hidden-caption', file_menu, base / 'FilesScreen.kt',
        'if(model.showHidden) "隐藏点文件" else "显示隐藏文件"', 'if(model.showHidden) "显示隐藏文件" else "隐藏点文件"')
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'C{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-settings-mutations' / str(time.time_ns())
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
        output = run_logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r',
            *(['-e','privilegedNightMode','true'] if privileged_night else []), '-e', 'class',
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
    print(f'ANDROID SETTINGS MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
