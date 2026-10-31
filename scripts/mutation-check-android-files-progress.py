#!/usr/bin/env python3
"""SFTP progress binding mutations; actual assertion red and same APK control."""
import hashlib
MARK='GT_ANDROID_PROGRESS_MUTATION'

def execute(root,build,test):
    native=root/'android/bridge/src/main/cpp/files_jni.c'
    kotlin=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteFiles.kt'
    originals={p:p.read_bytes() for p in (native,kotlin)}
    if any(MARK.encode() in value for value in originals.values()):raise RuntimeError('Residual progress mutation')
    create='exclusiveCreatePreservesContentOffsetAndExistingTarget'
    receive='receiveRegularFilePreservesOffsetAndOwnedDescriptor'
    cases=[
        ('delivery',native,'s->progress=*value;s->progress_present=true;','s->progress=*value;s->progress_present=false;',create,'create progress delivered'),
        ('bytes',native,'values[1]=(jlong)s->progress.bytes;','values[1]=(jlong)s->progress.bytes+1;',receive,'receive final observed bytes'),
        ('direction',kotlin,'RemoteTransferDirection.CREATE else RemoteTransferDirection.RECEIVE','RemoteTransferDirection.RECEIVE else RemoteTransferDirection.RECEIVE',create,'create direction and total'),
        ('total',kotlin,'fields[3].toULong().takeIf { fields[2]==1L }','fields[3].toULong().takeIf { false }',receive,'receive direction and total'),
    ]
    try:
        for label,path,old,new,method,message in cases:
            clean=originals[path].decode();assert clean.count(old)==1,(label,clean.count(old))
            path.write_text(clean.replace(old,new)+'\n// '+MARK+'\n')
            label='files-progress-'+label
            selection='com.hanxi.ghosterm.RemoteFilesTest#'+method
            build(label);out=test(label,selection,False)
            if message not in out:print(out,flush=True);raise RuntimeError('Wrong assertion failed: '+message)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);build(label+'-restored');test(label+'-restored',selection,True)
            assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(originals[path]).digest()
            print('PASS '+label+': compiled target assertion red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,value in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(value)
        if any(path.read_bytes()!=value for path,value in originals.items()):raise RuntimeError('Progress source restoration failed')
    print('ANDROID FILES PROGRESS MUTATIONS PASS: 4 groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_FILES_PROGRESS_MUTATE=1 with test-android-ssh.sh')
