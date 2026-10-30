#!/usr/bin/env python3
"""Dedicated file owner/JNI lifecycle, with compiled same-APK config controls."""
import hashlib
import os

MARK='GT_ANDROID_FILES_OWNER_MUTATION'

def execute(root,build,test):
    native=root/'android/bridge/src/main/cpp/files_jni.c'
    kotlin=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteFiles.kt'
    terminal=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/NativeSession.kt'
    core=root/'core/src/ssh.c'
    originals={p:p.read_bytes() for p in (native,kotlin,terminal,core)}
    hashes={p:hashlib.sha256(b).digest() for p,b in originals.items()}
    if any(MARK.encode() in b for b in originals.values()):raise RuntimeError('Residual file owner mutation')
    cases=[
        ('close-abort',native,'if(ssh)(void)gt_ssh_abort(ssh);','if(ssh && false)(void)gt_ssh_abort(ssh);','idleCloseAbortsBeforeWaitingForPeer'),
        ('socket-shutdown',core,'do { result = shutdown(ssh->sock, SHUT_RDWR); } while (result < 0 && errno == EINTR);','do { result = 0; } while (result < 0 && errno == EINTR);','idleCloseAbortsBeforeWaitingForPeer'),
        ('request-budget',native,'s->budget=(uint32_t)budget;','s->budget=5000;','directoryRequestUsesItsOwnBudget'),
        ('dispose-stop',native,'registry[slot].value=NULL;atomic_store(&s->stop,true);','registry[slot].value=NULL;','directoryCancellationReleasesOwner'),
        ('cancel-directory',kotlin,'if(!completed) close()','if(!completed) Unit','directoryCancellationReleasesOwner'),
        ('cancel-preview',kotlin,'if(!completed) close()','if(!completed) Unit','previewCancellationReleasesOwner'),
        ('preview-limit',native,'#define PREVIEW_LIMIT (256 * 1024)','#define PREVIEW_LIMIT (512 * 1024)','oversizedAndLinkedPreviewFailWithoutPublishing'),
        ('metadata-size',native,'(st->has_size?8u:0u)','8u','unknownMetadataAndInvalidNamesRemainExplicit'),
        ('metadata-type',native,'(st->is_regular?4u:0u)','4u','unknownMetadataAndInvalidNamesRemainExplicit'),
        ('preview-data',kotlin,'bytes.copyOfRange(8, bytes.size)','bytes.copyOfRange(8, if(kind==2) 8 else bytes.size)','realDirectoryAndPreviewPreserveMetadataAndBytes'),
        ('raw-nul',kotlin,'check(bytes.none { it == 0.toByte() }) { "此文件不是可预览的 UTF-8 文本" }','check(true) { "此文件不是可预览的 UTF-8 文本" }','realDirectoryAndPreviewPreserveMetadataAndBytes'),
        ('utf8',kotlin,'.onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)\n        .decode(ByteBuffer.wrap(bytes))','.onMalformedInput(CodingErrorAction.REPLACE).onUnmappableCharacter(CodingErrorAction.REPLACE)\n        .decode(ByteBuffer.wrap(bytes))','unknownMetadataAndInvalidNamesRemainExplicit'),
        ('file-auth-scope',kotlin,'prompt.scope === this','true','authenticationScopesAndStalePromptsCannotCrossConnections'),
        ('terminal-auth-scope',terminal,'prompt.scope == null','true','authenticationScopesAndStalePromptsCannotCrossConnections'),
        ('secret-erase',kotlin,'finally { first.fill(0); second.fill(0) }','finally { /* erasure omitted */ }','authenticationScopesAndStalePromptsCannotCrossConnections'),
        ('result-id',native,'if(s->request!=(uint64_t)request || !request)fail(env,"文件请求已失效");','if(!request)fail(env,"文件请求已失效");','requestIdsAndRecycledHandlesRejectStaleCalls'),
        ('generation',native,'file_session *s=registry[slot].generation==generation?registry[slot].value:NULL;','file_session *s=(generation?registry[slot].value:NULL);','requestIdsAndRecycledHandlesRejectStaleCalls'),
        ('path-nul',kotlin,"path.isNotEmpty() && '\\u0000' !in path && timeoutMillis",'path.isNotEmpty() && timeoutMillis','malformedPathsAreRejectedWithoutBreakingConnection'),
    ]
    selected=os.environ.get('ANDROID_FILES_OWNER_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown file owner mutation')
        cases=[c for c in cases if c[0] in selected]
    try:
        for label,path,old,new,method in cases:
            text=originals[path].decode()
            # borrow checks stale generations; dispose has an independent guard.
            count=2 if label=='generation' else 1
            if text.count(old)!=count:raise RuntimeError('Ambiguous mutation '+label+': '+str(text.count(old)))
            path.write_text(text.replace(old,new,1)+'\n// '+MARK+'\n')
            label='files-owner-'+label;selection='com.hanxi.ghosterm.RemoteFilesTest#'+method
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:raise RuntimeError('Target was not an assertion failure')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('Restore hash mismatch')
            build(label+'-restored');test(label+'-restored',selection,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('File owner source restoration failed')
    print(f'ANDROID FILES OWNER MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_FILES_OWNER_MUTATE=1 with scripts/test-android-ssh.sh')
