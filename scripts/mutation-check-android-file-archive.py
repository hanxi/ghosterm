#!/usr/bin/env python3
"""Move finished runtime results to immutable history: real UI/files/SQLite guards."""
import hashlib,os
MARK='GT_ANDROID_FILE_ARCHIVE_MUTATION'
def scope(text,marker):
    start=text.index(marker);begin=text.index('{',start);depth=1;end=begin+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
def execute(root,build,test):
    store=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksViewModel.kt'
    screen=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksScreen.kt'
    originals={p:p.read_bytes() for p in (store,screen)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual archive mutation')
    real='realCompletedTreeMovesWithoutCopyReplayOrRecordChangesAndStaleClearCannotDeleteIt'
    uncertain='uncertainFinishedTaskCanBeReviewedAndCleanedInTheSameRuntime'
    guards='activeUnsavedForeignAndLeasedTasksCannotArchiveOrClearRecords'
    mismatch='changedHeaderOrItemSnapshotRetainsCurrentResultsAndShowsReadError'
    text=originals[store].decode();piece=scope(text,'    fun archive(task:FileTask)')
    variants=[
        ('durable-phase','check(record.phase==FileJournalPhase.FINISHED)','check(true || record.phase==FileJournalPhase.FINISHED)','durableRunningRecordCannotBePresentedAsFinishedHistory'),
        ('header','check(record.intent==expected &&','check(true || record.intent==expected &&',mismatch),
        ('items','check(record.items.map { it.value }==result.items','check(true || record.items.map { it.value }==result.items',mismatch),
        ('history-result','history.add(record);','history.add(record.copy(outcome=FileCopyState.COMPLETE));',uncertain),
        ('keep-live','tasks.remove(task)','Unit',real),
        ('release','endContentRead(lease)','if(false) endContentRead(lease)',real),
        ('write-record','history.add(record);','journal.remove(request.id);history.add(record);',real),
    ]
    cases=[]
    for name,old,new,method in variants:
        assert piece.count(old)==1,name;cases.append((name,store,piece,piece.replace(old,new),method))
    removed=scope(text,'    fun remove(task: FileTask) {')
    for name,old,new,method in [('stale-clear','task !in tasks || ','',real),('leased-clear',' || contentReading','',guards)]:
        assert removed.count(old)==1,name;cases.append((name,store,removed,removed.replace(old,new),method))
    cases.append(('archive-route',screen,'onClick={ model.archive(task) }','onClick={ model.remove(task) }',real))
    selected=os.environ.get('ANDROID_FILE_ARCHIVE_CASES','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown archive mutation')
        cases=[c for c in cases if c[0] in selected]
    try:
        for label,path,old,new,method in cases:
            text=originals[path].decode();assert text.count(old)==1,(label,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');label='file-archive-'+label
            selection='com.hanxi.ghosterm.FileTaskArchiveTest#'+method
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:raise RuntimeError('Expected compiled normal assertion red')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(originals[path]).digest()
            build(label+'-restored');test(label+'-restored',selection,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(path.read_bytes()!=data for path,data in originals.items()):raise RuntimeError('Archive restoration failed')
    print(f'ANDROID FILE ARCHIVE MUTATIONS PASS: {len(cases)} groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_FILE_ARCHIVE_MUTATE=1 with test-android-ssh.sh and FileTaskArchiveTest')
