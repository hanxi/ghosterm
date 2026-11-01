#!/usr/bin/env python3
"""Batch task-control mutations: actual IO/UI assertions and same-APK Core controls."""
import hashlib
import os
import re

MARK='GT_ANDROID_TASK_CONTROLS_MUTATION'

def execute(root,build,test):
    core=root/'core/src/sftp.c'
    executor=root/'android/data/src/main/java/com/hanxi/ghosterm/data/FileCopyExecutor.kt'
    retry=root/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FileCopyRetry.kt'
    progress=root/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FileCopyProgress.kt'
    model=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksViewModel.kt'
    screen=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksScreen.kt'
    resolver=root/'android/app/src/main/java/com/hanxi/ghosterm/SSHResolver.kt'
    foreground=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTransferService.kt'
    originals={p:p.read_bytes() for p in (core,executor,retry,progress,model,screen,resolver,foreground)}
    if any(MARK.encode() in value for value in originals.values()):raise RuntimeError('Residual task-control mutation')
    local='com.hanxi.ghosterm.FileCopyExecutorTest#pausedRequestRetainsWorkAndResumesWithoutRecreatingIt'
    remote='com.hanxi.ghosterm.RemoteFilesTest#pausedReceiveKeepsDescriptorAndExcludesPausedTimeFromBudget'
    estimate='com.hanxi.ghosterm.FileCopyExecutorTest#wholeRequestEstimateKeepsUnknownAndVerificationDistinctAndExcludesPause'
    cases=[
        ('learn-unknown-work',executor,'learnSourceSize(source,snapshot.size.toULong())','Unit',
            'com.hanxi.ghosterm.SafCopyExecutorTest#sealedUnknownSourceBecomesKnownWholeRequestWork','sealed source supplies request total'),
        ('paused-notification',foreground,'current?.result?.pause ?: FileCopyPause.RUNNING','FileCopyPause.RUNNING',
            'com.hanxi.ghosterm.FileForegroundServiceTest#pausedNotificationReflectsAcknowledgementAndCanCancelItsOwner','notification reflects acknowledged pause'),
        ('system-dns',resolver,'waiting.resume(answer)','waiting.resume(emptyList())',
            'com.hanxi.ghosterm.SSHNetworkTest#systemDnsDomainConnectsToRealSSH',None),
        ('local-checkpoint',executor,'if(pauseWanted.get()) {','if(false && pauseWanted.get()) {',local,'local owner acknowledges pause before publication'),
        ('remote-checkpoint',core,'if (status != GT_OK || !control || !control->pause_requested(control->context)) return status;',
            'if (status == GT_OK) return status;\n  if (status != GT_OK || !control || !control->pause_requested(control->context)) return status;',remote,'receive owner acknowledges pause before IO completes'),
        ('active-budget',core,'*deadline += elapsed;','*deadline += 0;',remote,'receive resumes with active-time budget'),
        ('unknown-retry',retry,'val safe=setOf(FileCopyItemState.NOT_ATTEMPTED,FileCopyItemState.FAILED,FileCopyItemState.CONFLICT)',
            'val safe=setOf(FileCopyItemState.NOT_ATTEMPTED,FileCopyItemState.FAILED,FileCopyItemState.CONFLICT,FileCopyItemState.UNKNOWN,FileCopyItemState.CREATED)',
            'com.hanxi.ghosterm.FileCopyRetryTest#unknownParentAndKnownCreatedSideEffectsNeverBecomeRetryable',None),
        ('completed-retry',retry,'val safe=setOf(FileCopyItemState.NOT_ATTEMPTED,FileCopyItemState.FAILED,FileCopyItemState.CONFLICT)',
            'val safe=setOf(FileCopyItemState.NOT_ATTEMPTED,FileCopyItemState.FAILED,FileCopyItemState.CONFLICT,FileCopyItemState.COMPLETE)',
            'com.hanxi.ghosterm.FileCopyExecutorTest#retryPartialTreeCopiesOnlyUnfinishedChildIntoAcknowledgedDirectory',None),
        ('retry-button',screen,'onClick=model::confirmRetry','onClick=model::dismissRetry',
            'com.hanxi.ghosterm.FileTasksProductTest#retryRequiresConfirmationAndRechecksConflictsWithoutOverwriting','retry confirmation creates a fresh checked request'),
        ('pause-button',model,'task.execution?.pause()','Unit',
            'com.hanxi.ghosterm.FileQueueProductTest#pausePageWaitsForOwnerAcknowledgementAndRetainsTheQueueSlot',None),
        ('paused-estimate',progress,'val elapsed=nowMillis-startedMillis-pausedMillis','val elapsed=nowMillis-startedMillis',estimate,None),
        ('double-bytes',executor,'transferBytes+=bytes-previous;','transferBytes+=(bytes-previous)*2uL;',local,None),
        ('unknown-estimate',progress,'val total=totalBytes ?: return null\n        val elapsed=',
            'val total=totalBytes ?: (bytes*2uL)\n        val elapsed=',estimate,None),
    ]
    wanted=set(os.environ['ANDROID_TASK_CONTROLS_CASES'].split(',')) if os.environ.get('ANDROID_TASK_CONTROLS_CASES') else None
    if wanted is not None and wanted-{c[0] for c in cases}:raise RuntimeError('Unknown task-control case')
    def one(label,method,green,message=None):
        output=test(label,method,green)
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or (not green and not re.search(r'Tests run: 1,\s+Failures: 1',output)):
            print(output,flush=True);raise RuntimeError('Expected one named JUnit result')
        if message and message not in output:
            print(output,flush=True);raise RuntimeError('Wrong task-control assertion: '+message)
    def restore():
        for path,value in originals.items():
            if path.read_bytes()!=value:path.write_bytes(value)
        if any(hashlib.sha256(path.read_bytes()).digest()!=hashlib.sha256(value).digest() for path,value in originals.items()):
            raise RuntimeError('Task-control restore SHA256 mismatch')
    count=0
    try:
        for name,path,old,new,method,message in cases:
            if wanted is not None and name not in wanted:continue
            clean=originals[path].decode()
            expected=2 if name=='paused-estimate' else 1
            if clean.count(old)!=expected:raise RuntimeError('Missing/ambiguous task-control anchor: '+name)
            path.write_text(clean.replace(old,new)+'\n// '+MARK+'\n')
            label='task-controls-'+name
            build(label);one(label,method,False,message)
            one(label+'-control','com.hanxi.ghosterm.SSHConnectionModelTest#numericResolverPreservesIpv4AndIpv6Bytes' if name=='system-dns' else 'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(label+'-restored');one(label+'-restored',method,True)
            count+=1
            print('PASS '+label+': compiled assertion red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    print(f'ANDROID TASK CONTROLS MUTATIONS PASS: {count} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_TASK_CONTROLS_MUTATE=1 with test-android-ssh.sh')
