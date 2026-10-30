#!/usr/bin/env python3
"""Remote file product mutations against actual Activity/SSH, independent same-APK config."""
import hashlib
import os

MARK='GT_ANDROID_FILES_PRODUCT_MUTATION'
def execute(root,build,test):
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/FilesViewModel.kt'
    sessions=root/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
    source=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileBrowserRepository.kt'
    screen=root/'android/app/src/main/java/com/hanxi/ghosterm/FilesScreen.kt'
    app=root/'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    originals={p:p.read_bytes() for p in (model,sessions,source,screen,app)}
    hashes={p:hashlib.sha256(b).digest() for p,b in originals.items()}
    if any(MARK.encode() in b for b in originals.values()):raise RuntimeError('Residual file product mutation')
    real='realRemotePageAuthenticationRotationAndPreviewKeepTerminal'
    metadata='remoteMetadataPreservesUnknownAndUnsignedSizes'
    cases=[
      ('reconnect-error',screen,'if(remoteError!=null && !model.choosingLocation)','if(remoteError!=null && model.choosingLocation)','leavingRequiresExplicitReconnectWithNewEndpoint'),
      ('identity-user',app,'RemoteFileIdentity(target.alias,target.host,user,target.port)','RemoteFileIdentity(target.alias,target.host,target.user,target.port)','enteredUsernameIsKeptInImmutableFileIdentity'),
      ('unknown-label',screen,'FileKind.OTHER -> "特殊或未知类型"','FileKind.OTHER -> "普通文件"',metadata),
      ('factory',sessions,'if(attempt.forFiles) RemoteFileSession(request)','if(false && attempt.forFiles) RemoteFileSession(request)',real),
      ('canonical',source,'location.copy(directoryId=directory.path)','location',real),
      ('sorting',source,'it.kind!=FileKind.DIRECTORY','it.kind==FileKind.DIRECTORY',real),
      ('unknown-size',source,'},it.size,it.modifiedSeconds?','},it.size ?: 0uL,it.modifiedSeconds?',metadata),
      ('unknown-type',source,'else -> FileKind.OTHER','else -> FileKind.FILE',metadata),
      ('unsigned-size',source,'},it.size,it.modifiedSeconds?','},it.size?.toLong()?.coerceAtLeast(0)?.toULong(),it.modifiedSeconds?',metadata),
      ('preview',source,'return session(location.endpoint).previewText(entry.id)','return ""',real),
      ('leave',model,'clearSelection();(source as? FileBrowserRepository)?.close()','clearSelection()',real),
      ('stale-handoff',model,'if(!acceptsRemoteConnection(token))','if(false && !acceptsRemoteConnection(token))','lateAuthenticationCannotReopenOrReplaceLeftPage'),
      ('read-error',model,'error=failure.message ?: "读取文件夹失败，请重试"','error=null','protocolReadFailureIsVisibleAndCannotPretendEmpty'),
      ('remote-label',screen,'if(model.location.endpoint.remoteAlias==null) "本机" else "远端"','if(model.location.endpoint.remoteAlias==null) "本机" else "本机"',real),
    ]
    selected=os.environ.get('ANDROID_FILES_PRODUCT_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown file product mutation')
        cases=[c for c in cases if c[0] in selected]
    try:
        for label,path,old,new,method in cases:
            clean=originals[path].decode()
            if clean.count(old)!=1:raise RuntimeError('Ambiguous product mutation '+label)
            path.write_text(clean.replace(old,new)+'\n// '+MARK+'\n')
            label='files-product-'+label;target='com.hanxi.ghosterm.RemoteFilesProductTest#'+method
            build(label);test(label,target,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored');test(label+'-restored',target,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Product source restoration failed')
    print(f'ANDROID FILES PRODUCT MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_FILES_PRODUCT_MUTATE=1 ANDROID_SERIAL=... bash scripts/test-android-ssh.sh')
