#!/usr/bin/env python3
"""Actual remote tmux assertions, same-APK Core control and SHA restoration."""
import hashlib
import os
import re
import signal
MARK='GT_ANDROID_TMUX_MUTATION'
def execute(root,build,test):
    core=root/'core/src/tmux/tmux.c'
    native=root/'android/bridge/src/main/cpp/tmux_session.c'
    session=root/'android/bridge/src/main/cpp/session_jni.c'
    bridge=root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/RemoteTmux.kt'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/TmuxViewModel.kt'
    sessions=root/'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
    dialog=root/'android/app/src/main/java/com/hanxi/ghosterm/SSHConnectionDialog.kt'
    originals={p:p.read_bytes() for p in (core,native,session,bridge,model,sessions,dialog)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual Android tmux mutation')
    remote='com.hanxi.ghosterm.RemoteTmuxTest#'
    ui='com.hanxi.ghosterm.TmuxProductTest#'
    unknown='RemoteTmuxResult.UNKNOWN -> error=label+"结果无法确认，请刷新实际会话后再操作；不会自动重试。"+result.error'
    cases=[
      ('login-path',core,'if (ssh) rc = remote_login(ssh, command, &b, &r);',
       'if (ssh) rc = gt__command_ssh(ssh, command, &b, &r);',remote+'remoteLoginFindsTmuxOutsideExecPathAndUsesTheAbsoluteCandidate'),
      ('execution-endpoint',core,'rc = validate(&execution, target, &b);','rc = execution.ssh ? GT_OK : GT_ERR_INVALID_ARG;',remote+'executionNamespaceIsValidatedBeforeAttachingADifferentKeysEnvironment'),
      ('pinned-address',bridge,'return connectionFor(target,connection.timeoutMillis).openTmuxTarget(handle,target.snapshot,target.index).also { snapshot=0 }','return connection.openTmuxTarget(handle,target.snapshot,target.index).also { snapshot=0 }',remote+'attachUsesTheAcknowledgedManagementAddressInsteadOfANewCandidate'),
      ('native-epoch',native,'expected==s->snapshot && index>=0','expected<=s->snapshot && index>=0',remote+'nativeRefreshEpochRejectsAnOldTargetEvenIfTheCoreIdentityStillExists'),
      ('ui-token-epoch',bridge,'snapshot==entry.snapshot && snapshot>0','entry.snapshot>0',remote+'createRenameAndTerminateUseActualIdentitiesAndInvalidateOldSelections'),
      ('auth-scope',bridge,'prompt.owner==handle && prompt.scope===this','prompt.owner==handle',remote+'equalNumericAuthenticationIdentityCannotBypassTheOwnersScope'),
      ('secret-clear',bridge,'finally { first.fill(0);second.fill(0) }','finally { Unit }',remote+'anotherOwnersCredentialPromptIsRejectedAndSecretIsCleared'),
      ('cancel-owner',native,'return atomic_load(&s->stop) || (attach_stop && atomic_load(attach_stop));','return false && (atomic_load(&s->stop) || (attach_stop && atomic_load(attach_stop)));',remote+'cancellationWaitsForNativeOwnerRelease'),
      ('ordinary-shell-substitution',session,'s->transport=gaj_tmux_attach(s->tmux_target,s->ssh,&shell,&s->stop,&opened,detail,sizeof(detail));','s->transport=gaj_ssh_open_shell(s->remote,s->ssh,&shell,&s->stop,&opened);',remote+'closedClientAndManagementConnectionPreserveTheInnerTaskPid'),
      ('unknown-as-rejected',bridge,'RemoteTmuxResult.entries[outcome]','RemoteTmuxResult.entries[if(outcome==2)1 else outcome]',remote+'mutationRefusalIsDifferentFromAnUnacknowledgedSuccessfulCreate'),
      ('unknown-ui',model,unknown,'RemoteTmuxResult.UNKNOWN -> error=label+"请求已拒绝。"+result.error',ui+'unknownCreateIsVisibleAndReconnectionDoesNotReplayIt'),
      ('unknown-auto-retry',model,unknown,'RemoteTmuxResult.UNKNOWN -> { error=label+"结果无法确认，请刷新实际会话后再操作；不会自动重试。"+result.error;action(remote);Unit }',ui+'unknownCreateIsVisibleAndReconnectionDoesNotReplayIt'),
      ('default-entry',sessions,'it.state.connected && it.backgroundSession==null','it.state.connected',ui+'normalHostEntryCreatesAnOrdinaryTerminalInsteadOfReusingABackgroundClient'),
      ('ordinary-picker',dialog,'it.remoteAlias == alias && it.state.connected && it.backgroundSession==null','it.remoteAlias == alias && it.state.connected',ui+'normalHostEntryCreatesAnOrdinaryTerminalInsteadOfReusingABackgroundClient'),
    ]
    wanted=set(os.environ['ANDROID_TMUX_CASES'].split(',')) if os.environ.get('ANDROID_TMUX_CASES') else None
    if wanted is not None and wanted-{c[0] for c in cases}:raise RuntimeError('Unknown Android tmux case')
    def one(label,method,green):
        output=test(label,method,green)
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or (not green and not re.search(r'Tests run: 1,\s+Failures: 1',output)):
            print(output,flush=True);raise RuntimeError('Expected one named tmux JUnit assertion')
    def restore():
        for p,data in originals.items():
            if p.read_bytes()!=data:p.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=hashlib.sha256(data).digest() for p,data in originals.items()):raise RuntimeError('Tmux restore SHA256 mismatch')
    def interrupted(number,frame):raise KeyboardInterrupt('Tmux mutation interrupted')
    handlers={number:signal.signal(number,interrupted) for number in (signal.SIGINT,signal.SIGTERM)}
    count=0
    try:
        for name,path,old,new,method in cases:
            if wanted is not None and name not in wanted:continue
            clean=originals[path].decode()
            if clean.count(old)!=1:raise RuntimeError('Missing/ambiguous tmux anchor: '+name)
            path.write_text(clean.replace(old,new)+'\n// '+MARK+'\n')
            label='tmux-'+name
            build(label);one(label,method,False)
            one(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(label+'-restored');one(label+'-restored',method,True)
            count+=1;print('PASS '+label+': compiled assertion red / same APK Core green / restored green / SHA256',flush=True)
    finally:
        restore()
        for number,handler in handlers.items():signal.signal(number,handler)
    print('ANDROID TMUX MUTATIONS PASS:',count,'groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_TMUX_MUTATE=1 with test-android-ssh.sh')
