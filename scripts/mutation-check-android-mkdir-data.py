#!/usr/bin/env python3
"""Unified directory adapter: compiled real SSH/device faults and same-APK controls."""
import hashlib,os
MARK='GT_ANDROID_MKDIR_DATA_MUTATION'

def execute(root,build,test):
    data=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileDirectoryCreator.kt'
    binding=root/'android/data/src/main/java/com/hanxi/ghosterm/data/RemoteFileBinding.kt'
    bridge=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteFiles.kt'
    native=root/'android/bridge/src/main/cpp/files_jni.c'
    originals={p:p.read_bytes() for p in (data,binding,bridge,native)}
    if any(MARK.encode() in value for value in originals.values()):raise RuntimeError('Residual directory data mutation')
    real='unifiedCreationKeepsFrozenLocationsAndSupportsNestedFilePublication'
    identity='peerAndEndpointMismatchesAreRejectedBeforeAnyCreation'
    states='eachRemoteStateAndRawStatusSurvivesTheDataBoundary'
    cancelled='cancellationWaitsForFinalNativeOutcomeAndPreservesCreatedLocation'
    cases=[
        ('host',binding,'identity.host == session.peer.host','true',identity),
        ('port',binding,'identity.port == session.peer.port','true',identity),
        ('user',binding,'identity.user == session.peer.user','true',identity),
        ('tree',binding,'endpoint.treeUri == null','true',identity),
        ('endpoint',binding,'parent.endpoint == endpoint','true',identity),
        ('ready',binding,'session.state().ready','true',identity),
        ('peer-host',bridge,'RemoteFilePeer(connection.host, connection.port, connection.user)','RemoteFilePeer("wrong.example", connection.port, connection.user)',identity),
        ('peer-port',bridge,'RemoteFilePeer(connection.host, connection.port, connection.user)','RemoteFilePeer(connection.host, 1, connection.user)',identity),
        ('peer-user',bridge,'RemoteFilePeer(connection.host, connection.port, connection.user)','RemoteFilePeer(connection.host, connection.port, "wrong-user")',identity),
        ('parent',binding,'session.prepareDirectory(parent.directoryId, name, timeoutMillis)','session.prepareDirectory(parent.directoryId+"/folder", name, timeoutMillis)',real),
        ('budget',binding,'session.prepareDirectory(parent.directoryId, name, timeoutMillis)','session.prepareDirectory(parent.directoryId, name, 5000)','remoteBudgetIsForwardedAndQueuedCancellationIsIsolated'),
        ('unknown',data,'RemoteDirectoryCreationState.UNKNOWN -> State.UNKNOWN','RemoteDirectoryCreationState.UNKNOWN -> State.REJECTED',states),
        ('rejected',data,'RemoteDirectoryCreationState.REJECTED -> State.REJECTED','RemoteDirectoryCreationState.REJECTED -> State.UNKNOWN',states),
        ('created',data,'RemoteDirectoryCreationState.CREATED -> State.CREATED','RemoteDirectoryCreationState.CREATED -> State.NOT_ATTEMPTED',states),
        ('complete',data,'RemoteDirectoryCreationState.COMPLETE -> State.COMPLETE','RemoteDirectoryCreationState.COMPLETE -> State.CREATED',states),
        ('core-status',data,'coreStatus=result.status','coreStatus=0',states),
        ('remote-status',data,'remoteStatus=result.remoteStatus','remoteStatus=0u',states),
        ('location',data,'parent.copy(directoryId=request.path)','parent.copy(directoryId=parent.directoryId)',real),
        ('name',data,'actualName=if(created) requestedName else null','actualName=if(created) "wrong-name" else null',real),
        ('frozen-binding',data,'private suspend fun remoteDirectory() {','private suspend fun remoteDirectory() { remote?.invoke(parent.endpoint)?.prepareDirectory(parent,requestedName,5000)','bindingIsCapturedOnceAndBrowserClosureCannotRetargetTheRequest'),
    ]
    clean=originals[data].decode()
    start=clean.index('        fun cancel() {');end=clean.index('        suspend fun awaitClosed()',start)
    piece=clean[start:end];changed=piece.replace('remoteRequest?.cancel()','Unit').replace('worker?.cancel()','Unit')
    cases.append(('cancel',data,piece,changed,cancelled))
    # Delay actual native resource release, not a fabricated Kotlin completion.
    # These two groups use compiled production code with only the fence timing changed.
    cases.extend([
        ('await-close',data,'withContext(NonCancellable) { request.awaitClosed() }','Unit',cancelled),
        ('late-success',data,'if(outcome.state!=State.COMPLETE)','if(true)','cancellationAfterNativeAcknowledgementKeepsVerifiedResult'),
    ])
    selected=os.environ.get('ANDROID_MKDIR_DATA_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown data mutation')
        cases=[c for c in cases if c[0] in selected]
    def restore():
        for path,value in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(value)
        if any(path.read_bytes()!=value for path,value in originals.items()):raise RuntimeError('Directory data restoration failed')
    try:
        for name,path,old,new,method in cases:
            label='mkdir-data-'+name;selection='com.hanxi.ghosterm.RemoteDirectoryDataTest#'+method
            if name in ('await-close','late-success'):
                text=originals[native].decode()
                if name=='await-close':
                    text=text.replace('gt_sftp_close(files);gt_ssh_close(ssh);','if(directory_fence>=0)usleep(300000);gt_sftp_close(files);gt_ssh_close(ssh);')
                else:
                    text=text.replace('if(directory_fence>=0){close(directory_fence);directory_fence=-1;}','if(directory_fence>=0){usleep(300000);close(directory_fence);directory_fence=-1;}')
                assert text!=originals[native].decode()
                native.write_text(text+'\n// '+MARK+'\n')
                build(label+'-delayed-baseline');test(label+'-delayed-baseline',selection,True)
            text=originals[path].decode();assert text.count(old)==1,(name,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:raise RuntimeError('Expected compiled assertion failure')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(originals[path]).digest()
            build(label+'-restored');test(label+'-restored',selection,True)
            restore()
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    build('mkdir-data-final');test('mkdir-data-final','com.hanxi.ghosterm.RemoteDirectoryDataTest',True)
    print(f'ANDROID MKDIR DATA MUTATIONS PASS: {len(cases)} groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_MKDIR_DATA_MUTATE=1 with test-android-ssh.sh and RemoteDirectoryDataTest')
