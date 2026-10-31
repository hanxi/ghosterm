#!/usr/bin/env python3
"""Real SSH checkpoint ordering and post-kill read-only history, compiled mutations."""
import hashlib

MARK='GT_ANDROID_REMOTE_JOURNAL_MUTATION'

def execute(root,build,test,crash):
    executor=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileCopyExecutor.kt'
    store=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksViewModel.kt'
    prefix='com.hanxi.ghosterm.RemoteFileJournalTest#'
    cases=[
        ('create',executor,'result=result.copy(candidatePath=candidate);report(result,FileCopyCheckpoint.REMOTE_CREATE)',
            'result=result.copy(candidatePath=candidate)',prefix+'creationIntentPrecedesRemoteWrite'),
        ('publish',executor,'report(result.copy(state=FileCopyItemState.CREATED,bytes=staged.bytesAcknowledged),FileCopyCheckpoint.PUBLISH)',
            'Unit',prefix+'publicationIntentPreservesCompleteCandidate'),
        ('directory',executor,'report(result,FileCopyCheckpoint.DIRECTORY_CREATE)','Unit',prefix+'directoryCreationRecordFailureStopsChildWrites'),
        ('restart-history',store,'history.addAll(journal.load())','journal.load()',None),
    ]
    originals={p:p.read_bytes() for p in (executor,store)}
    if any(MARK.encode() in value for value in originals.values()):raise RuntimeError('Residual remote journal mutation')
    def restore():
        for path,body in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(body)
        if any(hashlib.sha256(p.read_bytes()).digest()!=hashlib.sha256(b).digest() for p,b in originals.items()):
            raise RuntimeError('Remote journal mutation source drift')
    try:
        for name,path,before,after,method in cases:
            body=originals[path].decode();assert body.count(before)==1,(name,body.count(before))
            path.write_text(body.replace(before,after)+'\n// '+MARK+'\n')
            label='remote-journal-'+name
            build(label)
            if method:test(label,method,False)
            else:crash(label,'remote-published',False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(label+'-restored')
            if method:test(label+'-restored',method,True)
            else:crash(label+'-restored','remote-published',True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    build('remote-journal-final');test('remote-journal-final','com.hanxi.ghosterm.RemoteFileJournalTest',True)
    crash('remote-journal-crash-final')
    print('ANDROID REMOTE FILE JOURNAL MUTATIONS PASS: 4 groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_FILE_JOURNAL_REMOTE_MUTATE=1 with test-android-ssh.sh and RemoteFileJournalTest')
