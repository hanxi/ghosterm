#!/usr/bin/env python3
"""Channel-stage cancellation and remaining budget, with same-APK config control."""
import hashlib
import os

MARK='GT_ANDROID_SSH_CHANNEL_MUTATION'

def execute(root,build,test):
    native=root/'android/bridge/src/main/cpp/ssh_session.c'
    session=root/'android/bridge/src/main/cpp/session_jni.c'
    core=root/'core/src/ssh.c'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
    originals={p:p.read_bytes() for p in (native,session,core,model)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual channel mutation')
    cases=[]
    for stage,method in [('channel','cancelChannelOpenReleasesOwner'),('pty','cancelPtyRequestReleasesOwner'),('shell','cancelShellRequestReleasesOwner')]:
        cases.append(('cancel-'+stage,native,'return atomic_load((atomic_bool *)context);','return atomic_load((atomic_bool *)context) && false;',method))
    for stage,method in [('channel','channelOpenUsesRemainingConnectionBudget'),('pty','ptyRequestUsesRemainingConnectionBudget'),('shell','shellRequestUsesRemainingConnectionBudget')]:
        cases.append(('budget-'+stage,native,'(uint32_t)(remaining<5000?remaining:5000)','(uint32_t)(remaining<5000?5000:remaining)',method))
    cases += [
        ('timeout-status',core,'if (gt__command_now() >= b->deadline)\n    return GT_ERR_IO;','if (gt__command_now() >= b->deadline)\n    return GT_ERR_CANCELLED;','shellRequestUsesRemainingConnectionBudget'),
        ('pty-refused',session,'opened==GT_ERR_UNSUPPORTED?"服务器拒绝远端 PTY/shell"','false?"服务器拒绝远端 PTY/shell"','ptyRefusalDoesNotFallBackToShell'),
        ('shell-refused',session,'opened==GT_ERR_UNSUPPORTED?"服务器拒绝远端 PTY/shell"','false?"服务器拒绝远端 PTY/shell"','shellRefusalIsExplicit'),
        ('channel-refused',session,'opened==GT_ERR_IO?"打开远端 PTY/shell 失败或超时"','false?"打开远端 PTY/shell 失败或超时"','channelRefusalIsIoAndSendsNoPty'),
        ('pty-disconnected',core,'*status = rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED ? GT_ERR_UNSUPPORTED : GT_ERR_IO;\n      gt__set_error("SSH: server refused PTY");','*status = GT_ERR_UNSUPPORTED;\n      gt__set_error("SSH: server refused PTY");','ptyDisconnectIsNotRefusal'),
        ('shell-disconnected',core,'*status = rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED ? GT_ERR_UNSUPPORTED : GT_ERR_IO;\n    gt__set_error("SSH: server refused exec/shell");','*status = GT_ERR_UNSUPPORTED;\n    gt__set_error("SSH: server refused exec/shell");','shellDisconnectIsNotRefusal'),
        ('product-cancel',model,'attempt.job?.cancel(); attempt.connection?.close();','/* cancellation omitted */','productCancellationDropsLateShellAndNewRequestIsIndependent'),
    ]
    selected=os.environ.get('ANDROID_SSH_CHANNEL_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown channel selection')
        cases=[c for c in cases if c[0] in selected]
    try:
        for label,path,old,new,method in cases:
            source=originals[path].decode()
            if source.count(old)!=1:raise RuntimeError('Ambiguous mutation '+label)
            path.write_text(source.replace(old,new)+'\n// '+MARK+'\n')
            label='ssh-channel-'+label
            selection='com.hanxi.ghosterm.SSHChannelTest#'+method
            build(label);test(label,selection,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[path]:raise RuntimeError('Restore mismatch')
            build(label+'-restored');test(label+'-restored',selection,True)
            print('PASS '+label+': compiled assertion red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Channel restoration failed')
    print(f'ANDROID SSH CHANNEL MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    raise SystemExit('Use ANDROID_SSH_CHANNEL_MUTATE=1 with scripts/test-android-ssh.sh')
