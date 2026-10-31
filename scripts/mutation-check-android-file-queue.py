#!/usr/bin/env python3
"""FIFO file task queue: compiled targets, independent config control and restored sources."""
import hashlib
import os

MARK='GT_ANDROID_FILE_QUEUE_MUTATION'

def execute(root,build,test):
    app=root/'android/app/src/main/java/com/hanxi/ghosterm'
    tasks=app/'FileTasksViewModel.kt';screen=app/'FileTasksScreen.kt'
    originals={p:p.read_bytes() for p in (tasks,screen)}
    if any(MARK.encode() in value for value in originals.values()):raise RuntimeError('Residual queue mutation')
    prefix='com.hanxi.ghosterm.FileQueueProductTest#'
    fifo=prefix+'fifoWaitsForEachConfirmationAndKeepsIndependentDestinations'
    cancel=prefix+'cancelledWaitingRequestNeverAcquiresAndNextLiveRequestCanRun'
    failure=prefix+'failedQueuedTargetIsRecheckedAndDoesNotStrandFollowingRequest'
    limit=prefix+'pendingLimitRejectsExtraRequestAndCancelledWaitingSlotCanBeReused'
    page=prefix+'waitingPageShowsPositionAndCanCancelWithoutStartingCopy'
    cases=[
        ('queued-phase',tasks,'var phase by mutableStateOf(FileTaskPhase.QUEUED)','var phase by mutableStateOf(FileTaskPhase.SCANNING)',fifo),
        ('fifo',tasks,'tasks.lastOrNull { it.phase==FileTaskPhase.QUEUED }','tasks.firstOrNull { it.phase==FileTaskPhase.QUEUED }',fifo),
        ('scan-next',tasks,'if(task.phase!=FileTaskPhase.CONFIRMING) task.phase=FileTaskPhase.FINISHED\n                advance()',
            'if(task.phase!=FileTaskPhase.CONFIRMING) task.phase=FileTaskPhase.FINISHED',failure),
        ('execution-next',tasks,'completedState.value=execution.outcome\n                    advance()','completedState.value=execution.outcome',fifo),
        ('confirmation-cancel-next',tasks,'task.error="复制已取消，尚未写入目标"\n            advance()',
            'task.error="复制已取消，尚未写入目标"',failure),
        ('waiting-cancel',tasks,'task.phase=FileTaskPhase.FINISHED;task.error="排队任务已取消，尚未读取或写入"',
            'task.phase=FileTaskPhase.QUEUED;task.error="排队任务已取消，尚未读取或写入"',cancel),
        ('execution-slot',tasks,'if(tasks.any { it.active && it.phase!=FileTaskPhase.QUEUED }) return','if(false) return',
            prefix+'cancellingExecutionDoesNotAdvanceQueueBeforeActualConnectionRelease'),
        ('remote-slot',tasks,'if(tasks.any { it.active && it.phase!=FileTaskPhase.QUEUED }) return','if(false) return',
            'com.hanxi.ghosterm.RemoteFileTasksProductTest#queuedRemoteEndpointsWaitForTheirOwnAuthenticationAndKeepIndependentBytes'),
        ('limit',tasks,'tasks.count { it.active } >= MAX_REQUESTS','tasks.count { it.active } > MAX_REQUESTS',limit),
        ('cancelled-capacity',tasks,'tasks.count { it.active } >= MAX_REQUESTS','tasks.size >= MAX_REQUESTS',limit),
        ('position',tasks,'tasks.asReversed().filter { it.active }.indexOf(task)','tasks.asReversed().indexOf(task)',fifo),
        ('waiting-label',screen,'FileTaskPhase.QUEUED -> "排队等待"','FileTaskPhase.QUEUED -> "正在复制"',page),
        ('waiting-cancel-button',screen,'model.cancel(task)','if(task.phase!=FileTaskPhase.QUEUED) model.cancel(task)',page),
        ('decision-owner',tasks,'task.decisionOwner=FilePasteDecisions.begin(check)',
            'task.decisionOwner=tasks.lastOrNull { it!==task && it.decisionOwner!=null }?.decisionOwner ?: FilePasteDecisions.begin(check)',
            prefix+'queuedConflictsAndLateActionsNeverReusePreviousRequestsDecision'),
    ]
    selected=os.environ.get('ANDROID_FILE_QUEUE_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{case[0] for case in cases}:raise RuntimeError('Unknown file queue mutation')
        cases=[case for case in cases if case[0] in selected]
    def restore():
        for p,value in originals.items():
            if MARK.encode() in p.read_bytes():p.write_bytes(value)
        if any(hashlib.sha256(p.read_bytes()).digest()!=hashlib.sha256(value).digest() for p,value in originals.items()):
            raise RuntimeError('Queue sources changed during mutations')
    try:
        for name,path,before,after,method in cases:
            source=originals[path].decode();assert source.count(before)==1,(name,source.count(before))
            path.write_text(source.replace(before,after)+'\n// '+MARK+'\n')
            label='file-queue-'+name
            build(label);test(label,method,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(label+'-restored');test(label+'-restored',method,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    build('file-queue-final');test('file-queue-final','com.hanxi.ghosterm.FileQueueProductTest,com.hanxi.ghosterm.RemoteFileTasksProductTest#queuedRemoteEndpointsWaitForTheirOwnAuthenticationAndKeepIndependentBytes',True)
    print(f'ANDROID FILE QUEUE MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_FILE_QUEUE_MUTATE=1 with test-android-ssh.sh and FileQueueProductTest')
