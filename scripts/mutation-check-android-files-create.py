#!/usr/bin/env python3
"""Native descriptor ownership and creation outcome, same-APK independent control."""
import hashlib
MARK='GT_ANDROID_CREATE_MUTATION'

def execute(root,build,test):
    native=root/'android/bridge/src/main/cpp/files_jni.c'
    kotlin=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteFiles.kt'
    originals={p:p.read_bytes() for p in (native,kotlin)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual create mutation')
    cases=[
        ('outcome',native,'if(status!=GT_OK && kind!=3)','if(status!=GT_OK)','exclusiveCreateProtocolOutcomesSurviveFailure'),
        ('fd-offset',native,'int owned=fcntl(source_fd,F_DUPFD_CLOEXEC,0);','int owned=fcntl(source_fd,F_DUPFD_CLOEXEC,0);if(owned>=0)lseek(owned,0,SEEK_SET);','exclusiveCreatePreservesContentOffsetAndExistingTarget'),
        ('fd-close',native,'\n        free(path);if(source_fd>=0)close(source_fd);','\n        free(path);','exclusiveCreateCancellationReleasesSourceDuplicate'),
        ('cloexec',native,'int owned=fcntl(source_fd,F_DUPFD_CLOEXEC,0);','int owned=fcntl(source_fd,F_DUPFD,0);','exclusiveCreateCancellationReleasesSourceDuplicate'),
        ('size',native,'source_fd,expected_size,&outcome','source_fd,expected_size * 0,&outcome','exclusiveCreatePreservesContentOffsetAndExistingTarget'),
        ('unknown',native,'u32(&p,(uint32_t)outcome.state);','u32(&p,(uint32_t)(outcome.state==GT_SFTP_CREATE_UNKNOWN?GT_SFTP_CREATE_REJECTED:outcome.state));','exclusiveCreateProtocolOutcomesSurviveFailure'),
        ('status',kotlin,'return RemoteCreationResult(status,','return RemoteCreationResult(0,','exclusiveCreateProtocolOutcomesSurviveFailure'),
        ('bytes',kotlin,'RemoteCreationState.entries[state],count)','RemoteCreationState.entries[state],0uL)','exclusiveCreatePreservesContentOffsetAndExistingTarget'),
        ('cancel',kotlin,'if(!completed) close()','if(!completed) Unit','exclusiveCreateCancellationReleasesSourceDuplicate'),
    ]
    try:
        for label,path,old,new,method in cases:
            text=originals[path].decode();assert text.count(old)==1,(label,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');label='files-create-'+label
            selection='com.hanxi.ghosterm.RemoteFilesTest#'+method
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:raise RuntimeError('Expected assertion failure')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(originals[path]).digest()
            build(label+'-restored');test(label+'-restored',selection,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(path.read_bytes()!=data for path,data in originals.items()):raise RuntimeError('Create source restoration failed')
    print('ANDROID FILES CREATE MUTATIONS PASS: 9 groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_FILES_CREATE_MUTATE=1 with test-android-ssh.sh')
