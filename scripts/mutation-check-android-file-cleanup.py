#!/usr/bin/env python3
"""Explicit maintenance: real file/provider/SSH/UI assertions and same-APK controls."""
import hashlib,os
MARK='GT_ANDROID_FILE_CLEANUP_MUTATION'

def execute(root,build,test):
    cleaner=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileResidualCleanup.kt'
    journal=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileCleanupJournal.kt'
    dialog=root/'android/app/src/main/java/com/hanxi/ghosterm/FileCleanupDialogs.kt'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksViewModel.kt'
    screen=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksScreen.kt'
    originals={p:p.read_bytes() for p in (cleaner,journal,dialog,model,screen)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual cleanup mutation')
    local='com.hanxi.ghosterm.FileCleanupDataTest#'
    product='com.hanxi.ghosterm.FileCleanupProductTest#'
    saf='com.hanxi.ghosterm.SafPublicationTest#'
    cases=[
        ('consent',dialog,'mutableStateOf(false)','mutableStateOf(true)',product+'keepAndFreshConfirmationDefaultUncheckedThenDeletionLeavesOriginalCopyRecord'),
        ('local-identity',cleaner,'if(stamp(file.fileDescriptor)!=review.local)','if(false && stamp(file.fileDescriptor)!=review.local)',local+'metadataChangeAndIdenticalByteReplacementArePreserved'),
        ('once',cleaner,'review.owner==owner && review.once.compareAndSet(false,true)','review.owner==owner',local+'reviewNeverDeletesAndExplicitRemovalIsDurableOneShotAndDoesNotRewriteCopy'),
        ('quota',journal,'existing.size<MAX_RECORDS','true || existing.size<MAX_RECORDS',local+'fullOrUnknownVersionJournalPreventsDeletionAndPreservesRecords'),
        ('record-owner',journal,'check(record.copy(state=previous.state','check(true || record.copy(state=previous.state',local+'durableUnknownIntentNeverReplaysAndResultsCannotBeReassignedOrOverwritten'),
        ('remote-ack',cleaner,'RemoteRemovalState.REMOVED->FileCleanupState.REMOVED','RemoteRemovalState.REMOVED->FileCleanupState.ABSENT','com.hanxi.ghosterm.RemoteRemovalTest#reviewedHistoryCleanupUsesDedicatedEndpointAndPersistsNewOperationOnly'),
        ('saf-ack',cleaner,'if(acknowledged) observed(FileCleanupState.REMOVED,0)','if(acknowledged) observed(FileCleanupState.UNKNOWN,3)',saf+'explicitCleanupAcknowledgementAndLostReplyAreDurableAndNeverReplayed'),
        ('saf-write-grant',cleaner,'it.isReadPermission && it.isWritePermission','it.isReadPermission',saf+'cleanupRequiresCurrentWriteGrantDeleteCapabilityAndKnownMetadata'),
        ('known-metadata',cleaner,'check(entry.modifiedMillis!=null &&','check(true || entry.modifiedMillis!=null &&',saf+'cleanupRequiresCurrentWriteGrantDeleteCapabilityAndKnownMetadata'),
        ('early-delete',cleaner,'        Review(owner,FileHistoryContentKey','        File(home,entry.id).delete()\n        Review(owner,FileHistoryContentKey',local+'reviewNeverDeletesAndExplicitRemovalIsDurableOneShotAndDoesNotRewriteCopy'),
        ('error-visible',screen,'model.cleanupError?.let { item { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-cleanup-error")) } }','model.cleanupError?.let { }',product+'staleMetadataAndRemovedHistoryCannotDeleteCurrentTarget'),
    ]
    text=originals[model].decode();start=text.index('    fun confirmCleanup()');end=text.index('    fun removeCleanupRecord(',start)
    piece=text[start:end];changed=piece.replace('if(record==null)','if(false && record==null)').replace('record.intent.id','review.key.requestId')
    cases.append(('stale-history',model,piece,changed,product+'staleMetadataAndRemovedHistoryCannotDeleteCurrentTarget'))
    # Move the journal intent after the provider reply. The held provider lets the
    # assertion observe the missing durable intent without an unrelated final-write exception.
    text=originals[cleaner].decode()
    changed=text.replace('journal.begin(result) // Must commit before the first deletion side effect.',
        'if(false) journal.begin(result) // '+MARK).replace('journal.finish(result)',
        'journal.begin(result.copy(state=FileCleanupState.UNKNOWN,status=null,remoteStatus=null,error=null));journal.finish(result)')
    cases.append(('intent-before-delete',cleaner,text,changed,saf+'cleanupIntentCommitsBeforeProviderSideEffectAndWaitsForActualReply'))
    selected=os.environ.get('ANDROID_FILE_CLEANUP_CASES','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown cleanup mutation')
        cases=[c for c in cases if c[0] in selected]
    try:
        for label,path,old,new,selection in cases:
            text=originals[path].decode();assert text.count(old)==1,(label,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');label='file-cleanup-'+label
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:raise RuntimeError('Expected compiled normal assertion red')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(originals[path]).digest()
            build(label+'-restored');test(label+'-restored',selection,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(path.read_bytes()!=data for path,data in originals.items()):raise RuntimeError('Cleanup restoration failed')
    print(f'ANDROID FILE CLEANUP MUTATIONS PASS: {len(cases)} groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_FILE_CLEANUP_MUTATE=1 with test-android-ssh.sh')
