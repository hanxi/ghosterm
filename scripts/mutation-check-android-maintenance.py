#!/usr/bin/env python3
"""Android explicit version removal, persisted intent and reference guards on an explicitly selected device.

Requires one normal assertion red, same-APK independent Core green, restored
scenario green, and unchanged SHA256. Does not open any host UI window.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_MAINTENANCE_MUTATION'
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set ANDROID_SERIAL to the explicitly intended test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    controller = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExtensionController.kt'
    stage = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseStage.kt'
    journal = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginInstallJournal.kt'
    disk = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseRemoval.kt'
    model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginInstallMaintenanceModel.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginInstallMaintenanceContent.kt'
    page_view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginInstalledVersionsContent.kt'
    originals = {p: p.read_bytes() for p in [controller, stage, journal, disk, model, view, page_view]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    owner = 'com.hanxi.ghosterm.PluginInstallJournalTest#'
    removal = 'com.hanxi.ghosterm.PluginStageRemovalTest#'
    api = 'com.hanxi.ghosterm.PluginInstallMaintenanceControllerTest#'
    draft = 'com.hanxi.ghosterm.PluginInstallMaintenanceModelTest#'
    page = 'com.hanxi.ghosterm.PluginInstallMaintenanceUITest#actualSettingsReviewsStageInventoryAndSeparatelyRetiresOnlyResolvedRecords'
    cases = [
        ('producer-schema', owner+'realStagePersistsStrongIdentityAndHoldsItsOwnLockBeforeAnyPayloadWrite', stage,
         'JSONObject().put("schema", 2)', 'JSONObject().put("schema", 1)'),
        ('producer-stage-lock', owner+'realStagePersistsStrongIdentityAndHoldsItsOwnLockBeforeAnyPayloadWrite', stage,
         'val owned = PluginInstallFiles.lock(rootFD.fd)', 'val owned = 0'),
        ('parent-identity', owner+'recordDigestNameAndParentPersistentIdentityAreAllBound', journal,
         'parent.sameObject(PluginReleaseRemoval.stamp(root))', 'true'),
        ('name-binding', owner+'recordDigestNameAndParentPersistentIdentityAreAllBound', journal,
         'value.opt("operation") == id && value.opt("name") == ".prepare-$id"', 'value.opt("operation") == id'),
        ('confirmed-identity', owner+'onlyConfirmedPersistentIdentityAllowsAbandonedClassificationAndPinnedOpen', journal,
         'val identity = record.directory\n', 'val identity: PluginReleaseRemoval.Stamp? = null\n'),
        ('reservation-busy', owner+'reservationWithoutIdentityCannotAdoptDirectoryAndBusyAbsenceStaysUnconfirmed', journal,
         'staging == null && !rootBusy', 'staging == null'),
        ('record-cas', owner+'recordDigestNameAndParentPersistentIdentityAreAllBound', journal,
         'val record = read(root, reference.recordName); demand(record.reference == reference)\n        val identity',
         'val record = read(root, reference.recordName)\n        val identity'),
        ('published-observation', owner+'publishedAndAbsentRecordsCanOnlyRetireMetadataAndNeedCurrentDigest', journal,
         'targetMatches(root, record) -> record.reference.target', 'false -> record.reference.target'),
        ('retire-resolved-only', owner+'activeOrAbandonedOrUnconfirmedDirectoriesCannotLoseTheirOwnershipRecord', journal,
         'setOf(State.ABSENT, State.PUBLISHED)', 'setOf(State.ABSENT, State.PUBLISHED, State.ABANDONED)'),
        ('record-budget', owner+'corruptRecordsAreUnknownAndCountTowardBoundedEnumeration', journal,
         'if (records.size > 64)', 'if (false)'),
        ('retire-after-tree', removal+'formatTwoPartialDeletionCanExplicitlyResumeAndRetireOnlyItsOwnRecord', disk,
         'header.stage?.let { demand(PluginInstallJournal.retire(owner, it, afterRemoval = true, sync = sync), Reason.IO) }', 'Unit'),
        ('removal-record-cas', removal+'cancellingPreviewWritesNothingAndRecordChangesCannotAuthorizeDeletion', disk,
         'PluginInstallJournal.verify(root, it, allowMissing = absent)', 'Unit'),
        ('version-readonly-isolation', removal+'mutableFilesAreAllowedOnlyForAConfirmedStageNeverAnInstalledVersion', disk,
         'before.mode and (if (writable) 63 else 191)', 'before.mode and 63'),
        ('retirement-ack', api+'resolvedRecordRetirementHasIndependentAcknowledgementAndNeverDeletesTarget', controller,
         'check(acknowledged) { "请确认只退役这条已解决的归属记录" }', 'check(true) { "请确认只退役这条已解决的归属记录" }'),
        ('model-ack', draft+'observationAndUnsafeRowsCannotRetireAnythingWithoutExplicitAck', model,
         'target != null && acknowledged', 'target != null'),
        ('model-resolved-only', draft+'observationAndUnsafeRowsCannotRetireAnythingWithoutExplicitAck', model,
         'entry.state !in setOf(PluginInstallJournal.State.PUBLISHED, PluginInstallJournal.State.ABSENT)', 'false'),
        ('model-unknown-not-empty', draft+'failureIsUnknownAndAnOldOrUnlistedReferenceCannotBeConfirmed', model,
         'issue = "安装归属记录查不出来，未当作空列表。"', 'observation = PluginInstallJournal.Observation(emptyList())'),
        ('model-listed-reference', draft+'failureIsUnknownAndAnOldOrUnlistedReferenceCannotBeConfirmed', model,
         'if (observation?.entries?.contains(entry) != true) return', 'if (false) return'),
        ('model-close-late', draft+'closingRejectsLateObservationAndNeverTurnsItIntoAnAction', model,
         'if (!closed && generation === epoch) observation = value', 'if (true) observation = value'),
        ('page-entry', page, page_view,
         'PluginInstallMaintenanceContent(maintenanceModel, removalModel,', 'if (false) PluginInstallMaintenanceContent(maintenanceModel, removalModel,'),
        ('page-retirement-ack', page, view,
         'enabled = model.canConfirm && canAct', 'enabled = canAct'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-maintenance-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    print('RECEIPTS ' + str(work), flush=True)
    for path, data in originals.items(): (work / path.name).write_bytes(data)
    def write(path, data):
        temporary = work / ('source-' + str(time.time_ns()))
        temporary.write_bytes(data); temporary.chmod(modes[path]); os.replace(temporary, path)
    def restore():
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes(): write(path, data)
            if path.read_bytes() != data: raise RuntimeError('Restore SHA mismatch: ' + str(path))
    def logged(label, command, seconds=120):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result); return output
    def build(label):
        # This also compiles the actual Core package/Controller fixture test.
        logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh', '-PghostermPluginJs=true',
            ':app:assembleDebug', ':app:assembleDebugAndroidTest'], 900)
        for relative in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
            logged(label + '-install-' + Path(relative).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs/apk' / relative])
    def test(label, method, green):
        if method.startswith('live#'):
            command = [sys.executable, '-B', ROOT / 'scripts/test-plugin-channel.py', '--rpc',
                '--android-client-runtime', '--android', serial, '--case', 'allow2', '--android-test',
                'com.hanxi.ghosterm.PluginVersionSelectionChannelTest#' + method.split('#', 1)[1]]
            if not green: command += ['--expect-platform-assertion', 'android']
            output = logged(label, command, 300)
            receipt = Path(output.rsplit('PUBLIC CHANNEL PASS; logs: ', 1)[1].strip())
            result = json.loads((receipt / 'result.json').read_text())
            raw = (receipt / 'android.log').read_text()
            if ('INSTRUMENTATION_STATUS: numtests=1' not in raw or
                    result['expected_platform_assertion'] != (None if green else 'android') or
                    (green and 'OK (1 test)' not in raw)):
                raise RuntimeError('Not accepted actual Session/Runtime assertion:\n' + raw)
            return
        output = logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', method,
            'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        good = ('OK (1 test)' in output and 'FAILURES' not in output) if green else (
            bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and
            any(t in output for t in ['java.lang.AssertionError', 'org.junit.ComparisonFailure']) and
            'Process crashed' not in output and 'INSTRUMENTATION_FAILED' not in output)
        expected = {'page-entry-target':'plugin-install-maintenance-count', 'page-retirement-ack-target':'plugin-install-retire-confirm'}.get(label)
        if not green and expected is not None: good = good and expected in output
        if not good or 'INSTRUMENTATION_STATUS: numtests=1' not in output: raise RuntimeError('Not accepted assertion result:\n' + output)
    control = 'com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings'
    results = []
    try:
        build('baseline')
        for index, target in enumerate(dict.fromkeys(c[1] for c in cases)): test('baseline-' + str(index), target, True)
        for name, target, path, before, after in cases:
            write(path, (originals[path].decode().replace(before, after) + '\n// ' + MARK + '\n').encode())
            build(name); test(name + '-target', target, False); test(name + '-control', control, True)
            restore(); build(name + '-restored'); test(name + '-restored', target, True)
            results.append(dict(case=name, compiled_assertion_red=True, same_apk_core_green=True, restored_green=True))
            (work / 'accepted-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(name + ': compiled normal assertion RED / Core GREEN / restored GREEN / SHA256', flush=True)
    finally:
        restore()
    for path, data in originals.items(): print(str(path.relative_to(ROOT)) + ' SHA256 ' + hashlib.sha256(data).hexdigest(), flush=True)
    print('ANDROID MAINTENANCE MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
