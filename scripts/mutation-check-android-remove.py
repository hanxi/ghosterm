#!/usr/bin/env python3
"""Reviewed remote removal: compiled device assertions with independent controls."""
import hashlib
import os
MARK='GT_ANDROID_REMOVE_MUTATION'

def scope(text,marker):
    start=text.index(marker);begin=text.index('{',start);depth=1;end=begin+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def execute(root,build,test):
    native=root/'android/bridge/src/main/cpp/files_jni.c'
    kotlin=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteFiles.kt'
    originals={p:p.read_bytes() for p in (native,kotlin)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual removal mutation')
    real='realFileRemovalPreservesAcknowledgementAndRejectsReplay'
    protocol='protocolResultsKeepAbsentRejectedUnknownAndDirectoryRemovalDistinct'
    cancel='cancellationAtEveryProtocolStageRetainsOutcomeAndAwaitsNativeRelease'
    stale='removalCancellationRejectsStaleRequestsAndRecycledHandles'
    fence='removalFenceRetainsDuplicateCloexecAndActualNativeLifetime'
    invalid='absenceIsAnObservationAndUnknownMetadataNeverStartsDeletion'
    cases=[]
    def add(label,path,marker,old,new,method):
        text=originals[path].decode();piece=scope(text,marker) if marker else text
        assert piece.count(old)==1,(label,piece.count(old))
        cases.append((label,path,piece,piece.replace(old,new),method))
    worker='else if(kind==7)'
    call='files,path,destination,publish_directory,expected_size,expected_mtime,&outcome,budget,mkdir_stopped,s);'
    add('paths',native,worker,call,call.replace('path,destination','destination,path'),real)
    add('mtime',native,worker,call,call.replace('expected_mtime,','expected_mtime+1,'),real)
    add('budget',native,worker,call,call.replace('&outcome,budget,','&outcome,3000,'),'totalBudgetIncludesAllMetadataRequests')
    add('cancel-core',native,worker,call,call.replace('budget,mkdir_stopped,','budget,(true?NULL:mkdir_stopped),'),cancel)
    add('raw-status',native,worker,'u32(&p,outcome.remote_status);','u32(&p,0);',protocol)
    add('unknown',native,worker,'u32(&p,(uint32_t)outcome.state);','u32(&p,(uint32_t)(outcome.state==GT_SFTP_REMOVE_UNKNOWN?GT_SFTP_REMOVE_REJECTED:outcome.state));',protocol)
    add('outcome',native,None,'if(status!=GT_OK && kind!=3 && kind!=5 && kind!=6 && kind!=7)','if(status!=GT_OK && kind!=3 && kind!=5 && kind!=6)',protocol)
    cancel_marker='JNIEXPORT void JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_cancelRemove'
    add('cancel-id',native,cancel_marker,'s->request==(uint64_t)request && s->kind==7','s->kind==7',stale)
    add('cancel-generation',native,cancel_marker,'registry[slot].generation==generation?registry[slot].value:NULL','generation?registry[slot].value:NULL',stale)
    add('fence-early',native,None,'if(kind==7){directory_fence=release_fd;release_fd=-1;}','if(kind==7){close(release_fd);release_fd=-1;}',fence)
    add('fence-cloexec',native,'JNIEXPORT jlong JNICALL Java_com_hanxi_ghosterm_bridge_NativeFiles_beginRemove','F_DUPFD_CLOEXEC','F_DUPFD',fence)
    request='    inner class RemovalRequest'
    add('status',kotlin,request,'outcome=RemoteRemovalResult(status,RemoteRemovalState.entries[state],remote,','outcome=RemoteRemovalResult(0,RemoteRemovalState.entries[state],remote,',protocol)
    add('request-once',kotlin,request,'check(started.compareAndSet(false,true))','check(true)',real)
    add('cancel-owner',kotlin,request,'if(active!=null) NativeFiles.cancelRemove(active.first,active.second)','if(active!=null) worker?.cancel()',cancel)
    add('queued-cancel',kotlin,request,'else worker?.cancel()','else { this@RemoteFileSession.close();worker?.cancel() }','queuedRemovalCancellationKeepsCurrentRequestAlive')
    add('late-result',kotlin,request,'cancelled=true\n            val active=binding','cancelled=true\n            outcome=outcome.copy(state=RemoteRemovalState.UNKNOWN)\n            val active=binding',real)
    add('parent',kotlin,'    fun prepareRemoval(',"require(parent.startsWith('/') &&","require(true || parent.startsWith('/') &&",invalid)
    add('name',kotlin,'    fun prepareRemoval(','nameBytes.size in 1..255','nameBytes.size in 1..65535',invalid)
    piece=scope(originals[kotlin].decode(),request)
    changed=piece.replace('if(cancelled) throw kotlinx.coroutines.CancellationException("文件删除已取消")','if(false && cancelled) throw kotlinx.coroutines.CancellationException("文件删除已取消")').replace('if(cancelled) owner.cancel()','if(false && cancelled) owner.cancel()')
    assert piece!=changed
    cases.append(('pre-cancel',kotlin,piece,changed,'cancelledBeforeExecutionLeavesFileAndOwnerUntouched'))
    selected=os.environ.get('ANDROID_REMOVE_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown removal mutation')
        cases=[c for c in cases if c[0] in selected]
    try:
        for label,path,old,new,method in cases:
            text=originals[path].decode();assert text.count(old)==1,(label,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');label='remove-'+label
            selection='com.hanxi.ghosterm.RemoteRemovalTest#'+method
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:raise RuntimeError('Expected a compiled assertion failure')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(originals[path]).digest()
            build(label+'-restored');test(label+'-restored',selection,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(path.read_bytes()!=data for path,data in originals.items()):raise RuntimeError('Removal restoration failed')
    print(f'ANDROID REMOVE MUTATIONS PASS: {len(cases)} groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_REMOVE_MUTATE=1 with test-android-ssh.sh and RemoteRemovalTest')
