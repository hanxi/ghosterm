#!/usr/bin/env python3
"""Receive/staging ownership with same-APK controls and a delayed-close fence probe."""
import hashlib
MARK='GT_ANDROID_RECEIVE_MUTATION'
def execute(root,build,test):
    native=root/'android/bridge/src/main/cpp/files_jni.c'
    kotlin=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteFiles.kt'
    store=root/'android/data/src/main/java/com/hanxi/ghosterm/data/TransferStagingStore.kt'
    originals={p:p.read_bytes() for p in (native,kotlin,store)}
    if any(MARK.encode() in v for v in originals.values()):raise RuntimeError('Residual receive mutation')
    good='receiveRegularFilePreservesOffsetAndOwnedDescriptor'
    cancel='receiveCancellationWaitsForNativeDescriptorRelease'
    failure='receiveProtocolFailuresNeverPublishSnapshot'
    cases=[
        ('native-count',native,'u64(&p,count);length=8;','u64(&p,0);length=8;',good),
        ('offset',native,'int owned=fcntl(destination_fd,F_DUPFD_CLOEXEC,0);','int owned=fcntl(destination_fd,F_DUPFD_CLOEXEC,0);if(owned>=0)lseek(owned,0,SEEK_SET);',good),
        ('fd-close',native,'\n        free(path);if(source_fd>=0)close(source_fd);','\n        free(path);',cancel),
        ('cloexec',native,'int owned=fcntl(destination_fd,F_DUPFD_CLOEXEC,0);','int owned=fcntl(destination_fd,F_DUPFD,0);',cancel),
        ('budget',native,'expected_size,&count,budget,stopped,s','expected_size,&count,50,stopped,s',failure),
        ('kotlin-count',kotlin,'return count\n    }\n    suspend fun preview','return 0\n    }\n    suspend fun preview',good),
        ('status',kotlin,'if(status != 0 && kind!=3 && kind!=5)','if(status != 0 && kind!=3 && kind!=5 && kind!=4)',failure),
        ('frozen-size',store,'check(expected == null || size == expected)','check(true)','receiveFrozenSizeMismatchDoesNotPublishSnapshot'),
        ('release-budget',store,'if(!transferred) release(reservation)','if(!transferred) Unit',failure),
        ('cancel-store',store,'pending.first.forEach { it.cancel() }','pending.first.forEach { }','storeCloseCancelsPendingReceiveAndReleasesReservation'),
        ('cancel-delivery',store,'onCancellation = { _, value, _ -> value.close() }','onCancellation = { _, _, _ -> Unit }','receiveDeliveryCancellationClosesUnclaimedSnapshot'),
    ]
    def selection(method):return 'com.hanxi.ghosterm.RemoteFilesTest#'+method
    control='com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    try:
        for label,path,old,new,method in cases:
            clean=originals[path].decode();start,end=0,len(clean)
            if path==store and label in ('frozen-size','release-budget','cancel-delivery'):
                start=clean.index('    /** Complete remote bytes');end=clean.index('    /** Snapshot close',start)
            piece=clean[start:end];assert piece.count(old)==(2 if label=="budget" else 1),(label,piece.count(old))
            path.write_text(clean[:start]+piece.replace(old,new)+clean[end:]+'\n// '+MARK+'\n')
            label='files-receive-'+label
            build(label);test(label,selection(method),False);test(label+'-control',control,True)
            path.write_bytes(originals[path]);build(label+'-restored');test(label+'-restored',selection(method),True)
            assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(originals[path]).digest()
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
        # A controlled scheduling perturbation applies to both green and red:
        # delay closing the data fd after cancellation, without delaying the UI.
        # The green branch must retain the lease; removing the EOF wait must fail.
        delay_old='\n        free(path);if(source_fd>=0)close(source_fd);'
        delay_new='\n        free(path);if(source_fd>=0){usleep(500000);close(source_fd);}'
        clean=originals[native].decode();assert clean.count(delay_old)==1
        delayed=clean.replace(delay_old,delay_new)+'\n// '+MARK+'\n'
        fence_old='check(it.read()==-1) { "文件释放通知格式无效" }'
        clean_k=originals[kotlin].decode();assert clean_k.count(fence_old)==1
        for name,method in [('receive',cancel),('upload','cancelledSnapshotUploadKeepsLeaseUntilNativeRelease')]:
            native.write_text(delayed);build('fence-'+name+'-baseline');test('fence-'+name+'-baseline',selection(method),True)
            kotlin.write_text(clean_k.replace(fence_old,'Unit')+'\n// '+MARK+'\n')
            build('fence-'+name+'-red');test('fence-'+name+'-red',selection(method),False);test('fence-'+name+'-control',control,True)
            kotlin.write_bytes(originals[kotlin]);build('fence-'+name+'-restored');test('fence-'+name+'-restored',selection(method),True)
            native.write_bytes(originals[native]);build('fence-'+name+'-clean');test('fence-'+name+'-clean',selection(method),True)
            print('PASS fd fence '+name+': delayed close baseline green / target red / config green / restored green / SHA256',flush=True)
    finally:
        for p,data in originals.items():
            if MARK.encode() in p.read_bytes():p.write_bytes(data)
        if any(p.read_bytes()!=data for p,data in originals.items()):raise RuntimeError('Receive mutation restoration failed')
    print('ANDROID RECEIVE MUTATIONS PASS: 13 groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_FILES_RECEIVE_MUTATE=1 with test-android-ssh.sh')
