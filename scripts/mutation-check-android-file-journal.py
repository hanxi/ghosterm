#!/usr/bin/env python3
"""Durable file tasks: compiled target red, same-APK config green, restored green/SHA."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import signal

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_FILE_JOURNAL_MUTATION'
spec=importlib.util.spec_from_file_location('android_helpers',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case',help='One named mutation');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL to the intended test device')
    adb=[str(ROOT/'build/android-toolchain/sdk/platform-tools/adb'),'-s',serial];require(run([*adb,'get-state']))
    data=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data'
    app=ROOT/'android/app/src/main/java/com/hanxi/ghosterm'
    journal=data/'FileTaskJournal.kt';codec=data/'FileJournalCodec.kt';executor=data/'FileCopyExecutor.kt'
    local=data/'LocalFilePublisher.kt';saf=data/'SafFilePublisher.kt';directory=data/'FileDirectoryCreator.kt'
    store=app/'FileTasksViewModel.kt';screen=app/'FileTasksScreen.kt'
    j='FileJournalTest#';p='FileJournalProductTest#'
    cases=[
        ('transaction',journal,'try { val result=block(db);db.setTransactionSuccessful();result }','try { db.setTransactionSuccessful();block(db) }',j+'failedPlanTransactionKeepsOriginalIntentAndNoPartialItems'),
        ('version',journal,'check(db.version==1)','check(true)',j+'corruptOrUnknownDatabaseIsPreservedAndNeverBecomesEmptySuccess'),
        ('preserve-invalid',journal,'catch(error: Throwable) { db.close();throw error }','catch(error: Throwable) { db.close();file.delete();throw error }',j+'corruptOrUnknownDatabaseIsPreservedAndNeverBecomesEmptySuccess'),
        ('history-limit',journal,'count(*) FROM tasks")<MAX_RECORDS','count(*) FROM tasks")<=MAX_RECORDS',j+'historyLimitDoesNotEvictOrAdmitAnUnrecordedRequest'),
        ('byte-budget',journal,'const val MAX_BYTES = 16L * 1024 * 1024','const val MAX_BYTES = 32L * 1024 * 1024',j+'totalMetadataBudgetRollsBackLargePlanBeforeAnyFileWrite'),
        ('accounting',journal,'bytes==scalar(db,"SELECT bytes FROM budget WHERE id=1")','true',j+'metadataCorruptionAndOversizePlanFailWithoutLosingIntent'),
        ('json-depth',codec,'require(depth<=16)','require(true)',j+'deeplyNestedAndNonCanonicalRecordsFailBeforeUnboundedParsing'),
        ('utf8-write',codec,'Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)',
            'Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPLACE)',j+'metadataCorruptionAndOversizePlanFailWithoutLosingIntent'),
        ('local-create',local,'beforeCreate(File(staging,candidateName).absolutePath)','Unit',j+'localIntentPrecedesCandidateAndPublicationAndRecordsActualResult'),
        ('local-publish',local,'beforePublish()','Unit',j+'failedPublicationIntentStopsRenameAndKeepsPreviousRecord'),
        ('write-error-stops',executor,'recordingError="任务记录保存失败：${error.message ?: "未知原因"}；请核对实际位置后再操作"\n                throw error','recordingError="任务记录保存失败：${error.message ?: "未知原因"}；请核对实际位置后再操作"\n                Unit',j+'failedPublicationIntentStopsRenameAndKeepsPreviousRecord'),
        ('actual-result',executor,'results[value.source.relativePath]=value;mutable.value=progress(FileCopyState.RUNNING)','Unit',j+'failedCompletionRecordPreservesActualPublishedResultWithoutRetrying'),
        ('directory-intent',executor,'report(result,FileCopyCheckpoint.DIRECTORY_CREATE)','Unit',j+'directoryCheckpointsRetainCreationAndStopBeforeChildWrites'),
        ('directory-created',directory,'outcome=Outcome(State.CREATED,parent.copy(directoryId=id),actualName=requestedName)\n                withContext(NonCancellable) { observed(outcome) }','outcome=Outcome(State.CREATED,parent.copy(directoryId=id),actualName=requestedName)',j+'directoryCheckpointsRetainCreationAndStopBeforeChildWrites'),
        ('saf-uri',saf,'outcome = outcome.copy(documentUri = created.toString())\n            withContext(NonCancellable) { observed(outcome) }','outcome = outcome.copy(documentUri = created.toString())','SafPublicationTest#journalUriIsPersistedBeforeContentWrite'),
        ('finish-record',store,'journal.finish(task.request.id,task.result,task.cancelled,task.error ?: task.persistenceError)','Unit',p+'finishedProductPersistsResultsAndNewRuntimeDoesNotReplayCopy'),
        ('load-history',store,'history.addAll(journal.load())','journal.load()',p+'reopenedQueuedIntentIsReadOnlyAndClearingHistoryNeverStartsIt'),
        ('stop-queue',store,'if(journalLoading || journalError!=null) return','if(journalLoading) return',p+'completionRecordFailureStopsAlreadyQueuedNextRequest'),
        ('remove-after-commit',store,'journal.remove(record.intent.id);history.remove(record)','history.remove(record);journal.remove(record.intent.id)',p+'failedHistoryRemovalKeepsVisibleRecordAndOriginalDatabase'),
        ('interrupted-label',screen,'"已中断 · 结果待核对"','"全部复制完成"',p+'reopenedQueuedIntentIsReadOnlyAndClearingHistoryNeverStartsIt'),
    ]
    if args.case:
        if args.case not in {c[0] for c in cases}:raise RuntimeError('Unknown case')
        cases=[c for c in cases if c[0]==args.case]
    originals={path:path.read_bytes() for path in {case[1] for case in cases}}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual journal mutation')
    work=ROOT/'tmp/android-file-journal-mutations';work.mkdir(exist_ok=True)
    def restore():
        for path,body in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(body)
        if any(hashlib.sha256(path.read_bytes()).digest()!=hashlib.sha256(body).digest() for path,body in originals.items()):raise RuntimeError('Sources changed during mutations')
    def build(label):
        result=run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (work/f'{label}-build.log').write_text(result.stdout+result.stderr);require(result)
        for name in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
            require(run([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/name]))
    def test(label,selection,green,count=1):
        result=run([*adb,'shell','am','instrument','-w','-r','-e','class',','.join('com.hanxi.ghosterm.'+n for n in selection.split(',')),
            'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],seconds=180)
        output=result.stdout+result.stderr;(work/f'{label}.log').write_text(output)
        passed=result.returncode==0 and f'OK ({count} '+('test)' if count==1 else 'tests)') in output
        if passed!=green or (not green and ('FAILURES!!!' not in output or not any(x in output for x in ('AssertionError','org.junit.ComparisonFailure')))):
            raise RuntimeError(output)
    baseline='FileJournalTest,FileJournalProductTest,SafPublicationTest#journalUriIsPersistedBeforeContentWrite'
    build('baseline');test('baseline',baseline,True,18)
    test('baseline-control','ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
    try:
        for name,path,before,after,target in cases:
            source=originals[path].decode()
            if source.count(before)!=1:raise RuntimeError(f'{name}: missing/ambiguous anchor {source.count(before)}')
            path.write_text(source.replace(before,after)+'\n// '+MARK+'\n')
            build(name);test(name+'-red',target,False)
            test(name+'-control','ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(name+'-restored');test(name+'-restored',target,True)
            print(f'PASS {name}: compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    build('final');test('final',baseline,True,18)
    print(f'ANDROID FILE JOURNAL MUTATIONS PASS: {len(cases)} groups',flush=True)


if __name__=='__main__':
    def interrupt(signum,frame):raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT,interrupt);signal.signal(signal.SIGTERM,interrupt)
    main()
