#!/usr/bin/env python3
"""SAF query state: compiled target red, same-APK Core green, restored green/SHA."""
import hashlib
import os
import signal

MARK='GT_ANDROID_SAF_QUERY_MUTATION'

def execute(root,build,test):
    base=root/'android/data/src/main/java/com/hanxi/ghosterm/data'
    paths={name:base/name for name in ['SafDocumentQuery.kt','LocalFileBrowser.kt','FileDestinationAccess.kt','FileDirectoryCreator.kt','SafFilePublisher.kt','FileResidualCleanup.kt']}
    originals={p:p.read_bytes() for p in paths.values()}
    hashes={p:hashlib.sha256(b).hexdigest() for p,b in originals.items()}
    if any(MARK.encode() in b for b in originals.values()):raise RuntimeError('Residual SAF query mutation')
    browser='com.hanxi.ghosterm.FilesSafTest#'
    publication='com.hanxi.ghosterm.SafPublicationTest#'
    directory='com.hanxi.ghosterm.SafDirectoryTest#'
    error=browser+'providerCursorErrorsRejectBothEmptyAndPartialResults'
    malformed=browser+'providerMalformedCursorStatesAreErrors'
    cleanup=publication+'cleanupRequiresCurrentWriteGrantDeleteCapabilityAndKnownMetadata'
    cases=[
        ('error','SafDocumentQuery.kt','check(error == null) {','check(true) {',error),
        ('loading','SafDocumentQuery.kt','return pending == true','return false',browser+'loadingDirectoryWaitsForNotificationWithoutPublishingEmptyOrPartialRows'),
        ('deadline','SafDocumentQuery.kt','LOADING_TIMEOUT_MS = 30_000L','LOADING_TIMEOUT_MS = 60_000L',browser+'stuckProviderTimesOutAsErrorInsteadOfEmptySuccess'),
        ('cancellation','SafDocumentQuery.kt','while (true) {\n                        signal.throwIfCanceled()','while (true) {\n                        while (signal.isCanceled) Thread.sleep(10)',browser+'leavingPendingProviderCancelsWaitAndClosesItsCursor'),
        ('cursor-close','SafDocumentQuery.kt','finally { if (!returned) cursor.close() }','finally { Unit }','com.hanxi.ghosterm.SafQueryResourceTest#partialAndErrorCursorsCloseBeforeReturningOrThrowing'),
        ('caller-owner','SafDocumentQuery.kt','returned = true','returned = false','com.hanxi.ghosterm.SafQueryResourceTest#partialAndErrorCursorsCloseBeforeReturningOrThrowing'),
        ('cleanup-state','FileResidualCleanup.kt','SafDocumentQuery.requireComplete(cursor)','Unit',cleanup),
        ('error-type','SafDocumentQuery.kt','check(error == null || error is String)','check(true)',malformed),
        ('loading-type','SafDocumentQuery.kt','check(pending == null || pending is Boolean)','check(true)',malformed),
        ('browser-info','LocalFileBrowser.kt','SafDocumentQuery.read(resolver,uri,columns,signal).use {\n            check(it.moveToFirst())','checkNotNull(resolver.query(uri,columns,null,null,null,signal)).use {\n            check(it.moveToFirst())',error),
        ('browser-list','LocalFileBrowser.kt','SafDocumentQuery.read(resolver,uri,columns,signal).use { cursor ->\n                while(cursor.moveToNext())','checkNotNull(resolver.query(uri,columns,null,null,null,signal)).use { cursor ->\n                while(cursor.moveToNext())',error),
        ('target-info','FileDestinationAccess.kt','SafDocumentQuery.read(resolver,uri,columns,signal)','checkNotNull(resolver.query(uri,columns,null,null,null,signal))',publication+'targetPreflightUsesRealGrantFlagsAndConflictMetadataWithoutCreating'),
        ('directory-info','FileDirectoryCreator.kt','SafDocumentQuery.read(resolver,uri,columns,signal)','checkNotNull(resolver.query(uri,columns,null,null,null,signal))',directory+'deniedRevokedAndUnreadableTargetsNeverCreate'),
        ('directory-list','FileDirectoryCreator.kt','SafDocumentQuery.read(resolver,uri,arrayOf(Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME),signal)','checkNotNull(resolver.query(uri,arrayOf(Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME),null,null,null,signal))',directory+'deniedRevokedAndUnreadableTargetsNeverCreate'),
        ('publisher-info','SafFilePublisher.kt','SafDocumentQuery.read(resolver,uri,columns,signal)','checkNotNull(resolver.query(uri,columns,null,null,null,signal))',publication+'unsupportedDirectoryAndFailedOrDuplicateListingDoNotCreate'),
        ('publisher-list','SafFilePublisher.kt','SafDocumentQuery.read(resolver,uri,arrayOf(Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME),signal)','checkNotNull(resolver.query(uri,arrayOf(Document.COLUMN_DOCUMENT_ID,Document.COLUMN_DISPLAY_NAME),null,null,null,signal))',publication+'unsupportedDirectoryAndFailedOrDuplicateListingDoNotCreate'),
    ]
    selected=set(filter(None,os.environ.get('ANDROID_SAF_QUERY_CASES','').split(',')))
    if selected-set(c[0] for c in cases):raise RuntimeError('Unknown SAF query cases')
    for name,filename,old,new,target in cases:
        if originals[paths[filename]].decode().count(old)!=1:raise RuntimeError('Missing/ambiguous SAF query anchor: '+name)
    handlers={s:signal.getsignal(s) for s in (signal.SIGINT,signal.SIGTERM)}
    def interrupted(signum,frame):raise KeyboardInterrupt('SAF query mutation interrupted')
    for signum in handlers:signal.signal(signum,interrupted)
    accepted=[]
    try:
        for name,filename,old,new,target in cases:
            if selected and name not in selected:continue
            path=paths[filename];label='saf-query-'+name
            path.write_text(originals[path].decode().replace(old,new)+'\n// '+MARK+'\n')
            build(label)
            output=test(label,target,False)
            if target.split('#')[1] not in output:raise RuntimeError('SAF query target not reached: '+name)
            test(label+'-control','com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings',True)
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).hexdigest()!=hashes[path]:raise RuntimeError('SAF query restore mismatch')
            build(label+'-restored');test(label+'-restored',target,True)
            accepted.append(name);print('PASS '+label+': compiled target red / same APK Core green / restored green / SHA256',flush=True)
    finally:
        for path,original in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).hexdigest()!=hashes[path]:raise RuntimeError('SAF query source restoration failed')
        for signum,handler in handlers.items():signal.signal(signum,handler)
    print('ANDROID SAF QUERY MUTATIONS PASS: '+str(len(accepted))+' groups ('+','.join(accepted)+')',flush=True)

if __name__=='__main__':raise SystemExit('Run through test-android-ssh.sh with ANDROID_SAF_QUERY_MUTATE=1')
