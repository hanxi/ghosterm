#!/usr/bin/env python3
"""Native descriptor ownership and creation outcome, same-APK independent control."""
import hashlib
import os
MARK='GT_ANDROID_PUBLISH_MUTATION'

def execute(root,build,test):
    native=root/'android/bridge/src/main/cpp/files_jni.c'
    kotlin=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteFiles.kt'
    originals={p:p.read_bytes() for p in (native,kotlin)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual create mutation')
    good='publishFilesAndConflictsPreserveOriginalBytes'
    protocol='publishProtocolPreservesRejectedUnknownAndQueryFailure'
    cancel='publishTimeoutAndCancellationReleaseOwner'
    cases=[
        ('outcome',native,'if(status!=GT_OK && kind!=3 && kind!=5 && kind!=6 && kind!=7)','if(status!=GT_OK && kind!=3 && kind!=6 && kind!=7)',protocol),
        ('paths',native,'files,path,destination,publish_directory','files,destination,path,publish_directory',good),
        ('directory',native,'files,path,destination,publish_directory','files,path,destination,(publish_directory && false)','publishDirectoryPreservesEmptyDirectory'),
        ('size',native,'publish_directory,expected_size,&outcome','publish_directory,0,&outcome',good),
        ('budget',native,'publish_directory,expected_size,&outcome,budget,stopped,s);','publish_directory,expected_size,&outcome,50,stopped,s);',cancel),
        ('remote',native,'u32(&p,outcome.remote_status);length=8;','u32(&p,0);length=8;','publishMissingSourcePreservesServerStatus'),
        ('unknown',native,'u32(&p,(uint32_t)outcome.state);u32(&p,outcome.remote_status);','u32(&p,(uint32_t)(outcome.state==GT_SFTP_PUBLISH_UNKNOWN?GT_SFTP_PUBLISH_REJECTED:outcome.state));u32(&p,outcome.remote_status);',protocol),
        ('status',kotlin,'return RemotePublicationResult(status,','return RemotePublicationResult(0,',protocol),
        ('throw-status',kotlin,'if(status != 0 && kind!=3 && kind!=5)','if(status != 0 && kind!=3)',protocol),
        ('cancel',kotlin,'if(!completed) close()','if(!completed) Unit',cancel),
    ]
    # Removal has the same argument prefix; mutate only the publication branch.
    text=originals[native].decode();start=text.index('else if(kind==5)');end=text.index('else if(kind==6)',start)
    piece=text[start:end]
    cases=[(label,path,piece,piece.replace(old,new),method) if label in ('paths','directory','size','budget','remote','unknown') else (label,path,old,new,method)
        for label,path,old,new,method in cases]
    selected=os.environ.get('ANDROID_FILES_PUBLISH_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown mutation selector')
        cases=[c for c in cases if c[0] in selected]
    try:
        for label,path,old,new,method in cases:
            text=originals[path].decode();assert text.count(old)==1,(label,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');label='files-publish-'+label
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
        if any(path.read_bytes()!=data for path,data in originals.items()):raise RuntimeError('Publish source restoration failed')
    print(f'ANDROID FILES PUBLISH MUTATIONS PASS: {len(cases)} groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_FILES_PUBLISH_MUTATE=1 with test-android-ssh.sh')
