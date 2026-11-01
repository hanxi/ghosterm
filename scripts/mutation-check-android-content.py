#!/usr/bin/env python3
"""Durable source digests and explicit historical content observations; invoke through the SSH fixture."""
import hashlib
import os
import re

MARK = 'GT_ANDROID_CONTENT_MUTATION'

def execute(root,build,test):
    digest=root/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FileHistoryObservation.kt'
    snapshot=root/'android/data/src/main/java/com/hanxi/ghosterm/data/TransferStagingStore.kt'
    executor=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileCopyExecutor.kt'
    codec=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileJournalCodec.kt'
    inspector=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileHistoryContentInspector.kt'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksViewModel.kt'
    screen=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksScreen.kt'
    originals={p:p.read_bytes() for p in (digest,snapshot,executor,codec,inspector,model,screen)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual content mutation')
    data='FileContentTest#'
    cases=[
        ('hash-bytes',snapshot,'hash.update(buffer,0,count)','Unit',data+'realSnapshotDigestKeepsPublisherOffsetAndCapacityLease'),
        ('hash-cancel',snapshot,'ensureActive();checkpoint()','ensureActive()',data+'digestCheckpointCancellationReleasesReaderWithoutDestroyingSnapshot'),
        ('digest-before-write',executor,'result=result.copy(sourceDigest=snapshot.contentDigest { active() })','snapshot.contentDigest { active() }',data+'digestIsDurableBeforeCreationAndRetainedAcrossNewJournal'),
        ('codec-digest',codec,'val digest=if(version=="2")','val digest=if(false)',data+'digestIsDurableBeforeCreationAndRetainedAcrossNewJournal'),
        ('comparison',inspector,'if(actual==expected)','if(actual!=expected)',data+'historicalMatchMismatchAndMissingNeverPromoteUnknownResult'),
        ('no-reference',inspector,'if(expected==null) return result','if(false) return result',data+'legacyWithoutReferenceDoesNotReadOrGuess'),
        ('metadata-change',inspector,'check(after.location==parent && after.entries.singleOrNull { it.id==entry.id }==entry)','check(true)',data+'changedMetadataAndReadFailureCannotReportMatchingBytes'),
        ('unknown-error',inspector,'result(FileHistoryContentState.UNKNOWN,error=error.message','result(FileHistoryContentState.MATCH,error=error.message',data+'changedMetadataAndReadFailureCannotReportMatchingBytes'),
        ('screen-action',screen,'onContent(item,candidate)','Unit','FileJournalProductTest#explicitContentVerificationShowsMatchAndDifferenceWithoutChangingOldResult'),
        ('digest-shape',digest,'sha256.matches(Regex("[0-9a-f]{64}"))','true',data+'digestIsDurableBeforeCreationAndRetainedAcrossNewJournal'),
    ]
    old='val observed=withTimeout(60_000) { withContext(Dispatchers.IO) {\n                    work.authentication.fileHistoryConnections(record.intent.id,record.intent.destination.endpoint,::cancelInspection)'
    cases.append(('target-only',model,old,old.replace('record.intent.destination.endpoint','record.intent.source.endpoint'),
        'RemoteFileTasksProductTest#historicalContentUsesFreshDestinationAndStoredDigestAfterSourceDisappears'))
    cases.append(('chunk-progress',snapshot,'hash.update(buffer,0,count);offset+=count','hash.update(buffer,0,count);offset=size',
        data+'multiChunkBinaryDigestIncludesEveryByte'))
    owner='FileJournalProductTest#sharedContentLeaseQueuesCopiesAndRejectsAnotherReaderOrLateRelease'
    cases += [
        ('reader-single',model,'if(busy || journalLoading || contentReadOwner!=null) return null','if(busy || journalLoading) return null',owner),
        ('reader-queue',model,'if(contentReadOwner!=null) return','if(false) return',owner),
        ('reader-release',model,'if(contentReadOwner!==reference) return false','if(false) return false',owner),
    ]
    cases += [
        ('reader-vm-release',model,'work.tasks.endContentRead(readReference)','Unit','FileJournalProductTest#explicitContentVerificationShowsMatchAndDifferenceWithoutChangingOldResult'),
        ('reader-history-remove',model,'if(record !in history || journalLoading || busy || contentReadOwner!=null) return','if(record !in history || journalLoading || busy) return',owner),
    ]
    selected=os.environ.get('ANDROID_FILE_CONTENT_CASES','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown content mutation')
        cases=[c for c in cases if c[0] in selected]
    for name,path,old,new,target in cases:
        if originals[path].decode().count(old)!=(2 if name=='hash-cancel' else 1):raise RuntimeError('Ambiguous mutation '+name)
    def one(label,target,green):
        output=test(label,'com.hanxi.ghosterm.'+target,green)
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output:raise RuntimeError(output)
        valid=('OK (1 test)' in output and 'FAILURES' not in output) if green else (bool(re.search(r'Tests run: 1,\s+Failures: 1',output)) and any(t in output for t in ('java.lang.AssertionError','org.junit.ComparisonFailure')))
        if not valid:raise RuntimeError(output)
    try:
        for name,path,old,new,target in cases:
            path.write_text(originals[path].decode().replace(old,new)+'\n// '+MARK+'\n')
            label='content-'+name
            build(label);one(label,target,False)
            one(label+'-control','ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');one(label+'-restored',target,True)
            print('PASS '+label+': compiled assertion red / same APK Core green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('Content source restoration failed')
    print(f'ANDROID NAMESPACE MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_FILE_CONTENT_MUTATE=1 with scripts/test-android-ssh.sh')
