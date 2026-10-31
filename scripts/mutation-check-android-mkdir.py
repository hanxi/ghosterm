#!/usr/bin/env python3
"""Controlled remote directory request: real APK targets and independent controls."""
import hashlib
import os
MARK='GT_ANDROID_MKDIR_MUTATION'

def execute(root,build,test):
    native=root/'android/bridge/src/main/cpp/files_jni.c'
    kotlin=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteFiles.kt'
    originals={p:p.read_bytes() for p in (native,kotlin)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual directory mutation')
    real='realNestedDirectoriesAcceptSubsequentFilePublication'
    protocol='protocolStatesAndRawErrorsSurviveFailures'
    cancel='cancellationRetainsStageOutcomeUntilActualOwnerRelease'
    late='lateCancellationCannotEraseVerifiedCreation'
    invalid='invalidAndCancelledRequestsDoNotConsumeConnection'
    stale='nativeCancellationRejectsStaleRequestsAndRecycledHandles'
    fence='nativeFenceRemainsOwnedUntilCancelledConnectionActuallyCloses'
    cases=[
        ('paths',native,'files,path,destination,&outcome,budget,mkdir_stopped,s);','files,destination,path,&outcome,budget,mkdir_stopped,s);',real),
        ('budget',native,'files,path,destination,&outcome,budget,mkdir_stopped,s);','files,path,destination,&outcome,50,mkdir_stopped,s);','oneDeadlineCoversEveryProtocolStage'),
        ('outcome',native,'if(status!=GT_OK && kind!=3 && kind!=5 && kind!=6)','if(status!=GT_OK && kind!=3 && kind!=5)',protocol),
        ('remote-status',native,'u32(&p, outcome.remote_status);','u32(&p, 0);',protocol),
        ('created',native,'u32(&p, (uint32_t)outcome.state);','u32(&p, (uint32_t)(outcome.state==GT_SFTP_MKDIR_CREATED?GT_SFTP_MKDIR_UNKNOWN:outcome.state));',cancel),
        ('unknown',native,'u32(&p, (uint32_t)outcome.state);','u32(&p, (uint32_t)(outcome.state==GT_SFTP_MKDIR_UNKNOWN?GT_SFTP_MKDIR_REJECTED:outcome.state));',protocol),
        ('cancel-core',native,'budget,mkdir_stopped,s);','budget,(true?NULL:mkdir_stopped),s);',cancel),
        ('cancel-id',native,'s->request==(uint64_t)request && s->kind==6','s->kind==6',stale),
        ('cancel-generation',native,'file_session *s = registry[slot].generation==generation ? registry[slot].value : NULL;','file_session *s = generation ? registry[slot].value : NULL;',stale),
        ('fence-early',native,'if(kind==6){directory_fence=release_fd;release_fd=-1;}','if(kind==6){close(release_fd);release_fd=-1;}',fence),
        ('status',kotlin,'outcome=RemoteDirectoryCreationResult(status,RemoteDirectoryCreationState.entries[state],remote,','outcome=RemoteDirectoryCreationResult(0,RemoteDirectoryCreationState.entries[state],remote,',protocol),
        ('request-once',kotlin,'check(started.compareAndSet(false,true)) { "目录请求不能重复执行" }','check(true) { "目录请求不能重复执行" }',real),
        ('cancel-owner',kotlin,'if(active!=null) NativeFiles.cancelMkdir(active.first,active.second)','if(active!=null) worker?.cancel()',cancel),
        ('queued-cancel',kotlin,'else worker?.cancel()','else { this@RemoteFileSession.close();worker?.cancel() }','cancellingQueuedRequestDoesNotCancelCurrentOperation'),
        ('late-result',kotlin,'            cancelled=true\n            val active=binding','            cancelled=true\n            outcome=outcome.copy(state=RemoteDirectoryCreationState.UNKNOWN)\n            val active=binding',late),
        ('failed-close',kotlin,'if(status!=0) this@RemoteFileSession.close()','if(status!=0) Unit',protocol),
        ('parent',kotlin,'require(parent.startsWith(\'/\') &&','require(true || parent.startsWith(\'/\') &&',invalid),
        ('name',kotlin,"nameBytes.size in 1..255","nameBytes.size in 1..65535",invalid),
    ]
    # Keep the native fence descriptor mutation scoped to this admission path.
    text=originals[native].decode();start=text.index('JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_beginMkdir');end=text.index('JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_cancelMkdir',start)
    piece=text[start:end];assert piece.count('F_DUPFD_CLOEXEC')==1
    cases.append(('fence-cloexec',native,piece,piece.replace('F_DUPFD_CLOEXEC','F_DUPFD'),fence))
    text=originals[kotlin].decode();start=text.index('    inner class DirectoryRequest');end=text.index('    private fun encodePath(',start)
    piece=text[start:end]
    changed=piece.replace('if(cancelled) throw kotlinx.coroutines.CancellationException("目录创建已取消")','if(false && cancelled) throw kotlinx.coroutines.CancellationException("目录创建已取消")').replace('if(cancelled) owner.cancel()','if(false && cancelled) owner.cancel()')
    cases.append(('pre-cancel',kotlin,piece,changed,invalid))
    selected=os.environ.get('ANDROID_MKDIR_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown directory mutation')
        cases=[c for c in cases if c[0] in selected]
    try:
        for label,path,old,new,method in cases:
            text=originals[path].decode();assert text.count(old)==1,(label,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');label='mkdir-'+label
            selection='com.hanxi.ghosterm.RemoteDirectoryTest#'+method
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:raise RuntimeError('Expected a compiled assertion failure')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(originals[path]).digest()
            build(label+'-restored');test(label+'-restored',selection,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(path.read_bytes()!=data for path,data in originals.items()):raise RuntimeError('Directory restoration failed')
    print(f'ANDROID MKDIR MUTATIONS PASS: {len(cases)} groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_MKDIR_MUTATE=1 with test-android-ssh.sh and RemoteDirectoryTest')
