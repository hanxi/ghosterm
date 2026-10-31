#!/usr/bin/env python3
"""Task-owned SSH connections: real compiled device failures and independent controls."""
import hashlib
import os

MARK = 'GT_ANDROID_FILE_TASK_MUTATION'

def execute(root, build, test):
    native = root/'android/bridge/src/main/cpp/files_jni.c'
    bridge = root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteFiles.kt'
    task = root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileTaskConnections.kt'
    originals = {p:p.read_bytes() for p in (native, bridge, task)}
    if any(MARK.encode() in value for value in originals.values()):
        raise RuntimeError('Residual task connection mutation')
    close = 'closeWaitsForItsOwnOwnerAndPreservesAnotherConnection'
    identity = 'resolvedIdentityChangesAreRejectedBeforeAuthentication'
    local = 'localEndpointsNeverCreateConnectionsAndScopeCannotEscape'
    separate = 'sameEndpointStillGetsTwoOwnersAndClosedRoleIsNotReconnected'
    failed = 'secondEndpointFailureReleasesFirstAndPreservesOriginalError'
    cancelled = 'cancellationDuringAuthenticationWaitsForAllAcquiredOwners'
    cases = [
        ('close-await',bridge,'close(); closed.await()','close(); Unit',close),
        ('close-drain',bridge,'check(it.read() == -1) { "连接释放通知格式无效" }','check(true) { "连接释放通知格式无效" }',close),
        ('close-native-early',native,'int status=GT_ERR_IO;int directory_fence=-1;',
            'int status=GT_ERR_IO;int directory_fence=-1;close(s->owner_fence);s->owner_fence=-1;',close),
        ('directory-fence-order',native,
            'if(directory_fence>=0){usleep(300000);close(directory_fence);}\n    /* The connection fence in release must follow every request-local fd. */\n    release(s);',
            'release(s);\n    if(directory_fence>=0){usleep(300000);close(directory_fence);}',
            'connectionCompletionIncludesPendingDirectoryFence'),
        ('cloexec',native,'s->owner_fence=fcntl(completion_fd,F_DUPFD_CLOEXEC,0);','s->owner_fence=fcntl(completion_fd,F_DUPFD,0);',
            'completionPipeIsCloexecAndCloseWithoutWaiterDrainsIt'),
        ('constructor-reader',bridge,'completion[0].close()','Unit','constructionFailureAndFullRegistryDoNotLeakNotificationPipes'),
        ('task-await',task,'it.closeAndAwait()','Unit',failed),
        ('cancel-cleanup',task,'withContext(NonCancellable)','withContext(currentCoroutineContext())',cancelled),
        ('second-owner',task,'owned.add(session)','if(owned.isEmpty()) owned.add(session)',failed),
        ('host',task,'connection.host == identity.host','true',identity),
        ('port',task,'connection.port == identity.port','true',identity),
        ('user',task,'connection.user == identity.user','true',identity),
        ('tree',task,'endpoint.treeUri == null','true',identity),
        ('endpoint-id',task,'endpoint.id.isNotEmpty()','true',identity),
        ('endpoint-user',task,'identity.user.isNotBlank()','true',identity),
        ('endpoint-port',task,'identity.port in 1..65535','true',identity),
        ('scope-endpoint',task,'endpoints[role] == endpoint','true',local),
        ('scope-active',task,'check(active.get())','check(true)',local),
        ('scope-ready',task,'it.session.state().ready','true',separate),
        ('one-shot',task,'started.compareAndSet(false, true)','true',local),
        ('late-resolver',task,'ensureActive() // A resolver may return after cancellation.',
            'Unit // A resolver may return after cancellation.','cancellationOfLateResolverCannotAcquireAnotherOwner'),
        ('late-auth',task,'currentCoroutineContext().ensureActive()','Unit','lateSuccessfulAuthenticationCannotStartCancelledTask'),
        ('role-isolation',task,'bindings[role] = RemoteFileBinding(endpoint, session)',
            'bindings[role] = RemoteFileBinding(endpoint, owned.first())',separate),
    ]
    selected = os.environ.get('ANDROID_FILE_TASK_MUTATIONS','').split(',')
    if selected != ['']:
        if set(selected)-{c[0] for c in cases}: raise RuntimeError('Unknown task connection mutation')
        cases = [c for c in cases if c[0] in selected]
    def restore():
        for path,value in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(value)
        if any(hashlib.sha256(path.read_bytes()).digest()!=hashlib.sha256(value).digest() for path,value in originals.items()):
            raise RuntimeError('Task connection restoration failed')
    try:
        for name,path,old,new,method in cases:
            label='file-task-'+name;selection='com.hanxi.ghosterm.RemoteFileTaskTest#'+method
            baseline = dict(originals)
            if name in ('close-await','close-drain','close-native-early','task-await','cancel-cleanup'):
                text=baseline[native].decode()
                anchor='gt_sftp_close(files);gt_ssh_close(ssh);'
                assert text.count(anchor)==1
                baseline[native]=(text.replace(anchor,'usleep(300000);'+anchor)+'\n// '+MARK+'\n').encode()
                native.write_bytes(baseline[native])
                build(label+'-delayed-baseline');test(label+'-delayed-baseline',selection,True)
            if name=='directory-fence-order':
                text=baseline[native].decode()
                anchor='if(directory_fence>=0)close(directory_fence);\n    /* The connection fence in release must follow every request-local fd. */\n    release(s);'
                assert text.count(anchor)==1
                baseline[native]=(text.replace(anchor,old)+'\n// '+MARK+'\n').encode()
                native.write_bytes(baseline[native])
                build(label+'-delayed-baseline');test(label+'-delayed-baseline',selection,True)
            text=baseline[path].decode()
            assert text.count(old)==1,(name,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n')
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:
                raise RuntimeError('Expected compiled assertion failure')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(baseline[path])
            assert path.read_bytes()==baseline[path]
            build(label+'-restored');test(label+'-restored',selection,True)
            restore()
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally: restore()
    build('file-task-final');test('file-task-final','com.hanxi.ghosterm.RemoteFileTaskTest',True)
    print(f'ANDROID FILE TASK MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    raise SystemExit('Use ANDROID_FILE_TASK_MUTATE=1 with test-android-ssh.sh and RemoteFileTaskTest')
