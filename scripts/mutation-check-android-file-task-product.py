#!/usr/bin/env python3
"""Production paste/task/authentication mutations with real IO and same-APK control."""
import hashlib
import os

MARK='GT_ANDROID_FILE_TASK_PRODUCT_MUTATION'

def execute(root,build,test):
    app=root/'android/app/src/main/java/com/hanxi/ghosterm'
    data=root/'android/data/src/main/java/com/hanxi/ghosterm/data'
    tasks=app/'FileTasksViewModel.kt'
    files=app/'FilesViewModel.kt'
    screen=app/'FileTasksScreen.kt'
    browser=app/'FilesScreen.kt'
    main=app/'MainActivity.kt'
    auth=app/'SessionsViewModel.kt'
    local=data/'LocalFileBrowser.kt'
    access=data/'FileDestinationAccess.kt'
    originals={p:p.read_bytes() for p in (tasks,files,screen,browser,main,auth,local,access)}
    if any(MARK.encode() in text for text in originals.values()):raise RuntimeError('Residual task product mutation')
    l='com.hanxi.ghosterm.FileTasksProductTest#'
    r='com.hanxi.ghosterm.RemoteFileTasksProductTest#'
    s='com.hanxi.ghosterm.SafFileTasksProductTest#'
    copy=l+'pasteFreezesDestinationAndKeepsDefaultClipboardAfterRealCopy'
    stale=l+'explicitKeepBothNeverOverwritesAndStaleConfirmationCannotRun'
    clipboard=l+'changedSourceFailsAndFinishedTaskCannotClearNewClipboard'
    slot=l+'singleSlotRemainsOccupiedUntilCancelledAcquisitionReturns'
    config=r+'changedConfigurationAndUnsupportedOptionsCannotRedirectFrozenTask'
    cases=[
        ('timeout',tasks,'} catch(timeout: TimeoutCancellationException) {\n                task.error="任务连接超时，请重新确认后重试"\n            } catch(cancel: CancellationException)',
            '} catch(cancel: CancellationException)',l+'connectionTimeoutIsFailureRatherThanUserCancellation'),
        ('paste-button',browser,'Button(onClick=onPaste,enabled=','Button(onClick={},enabled=',copy),
        ('task-route',main,'if(fileTasks.visible) { FileTasksScreen(fileTasks);return@MaterialTheme }','if(false) { FileTasksScreen(fileTasks);return@MaterialTheme }',r+'uploadThroughPasteButtonSurvivesRotationAndKeepsBrowserAndClipboard'),
        ('destination',tasks,'FilePasteRequest.capture(snapshot,destination,label)','FilePasteRequest.capture(snapshot,FileLocation(FileEndpoint.APP),label)',copy),
        ('slot',tasks,'if(tasks.any { it.active && it.phase!=FileTaskPhase.QUEUED }) return','if(false) return',slot),
        ('cancel-slot',tasks,'} else {\n            task.phase=FileTaskPhase.CANCELLING','} else {\n            task.phase=FileTaskPhase.FINISHED',slot),
        ('remove-running',tasks,'if(task.active || task.recording) return','if(false) return',slot),
        ('confirm-route',screen,'model.confirm(task,decision.reference)','Unit',copy),
        ('confirm-ref',tasks,'checkNotNull(task.decisionOwner).confirm(ref)','checkNotNull(task.decisionOwner).confirm(task.decisions!!.reference)',stale),
        ('execute',tasks,'ensureActive();execution.execute()','ensureActive();execution.cancel();execution.execute()',copy),
        ('clipboard-default',files,'clearUsed: Boolean = false','clearUsed: Boolean = true',copy),
        ('clipboard-id',files,' && clipboard?.id==result.clipboardId','',clipboard),
        ('clipboard-result',files,' && result.mayClearClipboard','',clipboard),
        ('refresh',files,'if(visible && preview==null && !selecting && result.items.any { it.targetParent==location }) reload()','if(visible) reload()',l+'completionDoesNotClearSelectionOrPreviewInAnotherDirectory'),
        ('actual-location',screen,'(entry.documentUri ?: entry.createdId)','entry.targetName',copy),
        ('config-identity',auth,'prepared.host == identity.host && prepared.port == identity.port &&\n                            (prepared.user.isBlank() || prepared.user == identity.user)','true',config),
        ('config-issues',auth,'check(issues.isEmpty())','check(true)',config),
        ('config-exists',auth,'check(snapshot.hosts.any { it.alias == identity.alias })','check(true)',config),
        ('cancel-auth',auth,'attempt.taskCancelled?.invoke();','',r+'cancelledAuthenticationReleasesTaskAndRejectsOldCredentialResponse'),
        ('auth-role',auth,'if(role == FileTaskConnections.Role.SOURCE) "读取来源" else "写入目标"','if(role == FileTaskConnections.Role.DESTINATION) "读取来源" else "写入目标"',r+'downloadUsesSourceRoleAndLeavingFilePageKeepsTaskAlive'),
        ('source-role',tasks,'browse(FileTaskConnections.Role.SOURCE,it)','browse(FileTaskConnections.Role.DESTINATION,it)',r+'downloadUsesSourceRoleAndLeavingFilePageKeepsTaskAlive'),
        ('source-read-wait',local,'launch(Dispatchers.IO) {','Dispatchers.IO.dispatch(EmptyCoroutineContext) {',s+'cancellingSourceListingWaitsForActualProviderReturn'),
        ('unknown-label',screen,'FileCopyState.UNKNOWN -> "复制结果未知，请核对实际位置"','FileCopyState.UNKNOWN -> "全部复制完成"',s+'lostCreationReplyStaysUnknownAndShowsTheUnconfirmedResult'),
        ('target-read-wait',access,'launch(Dispatchers.IO) {','Dispatchers.IO.dispatch(EmptyCoroutineContext) {',s+'cancellingDestinationMetadataWaitsForActualProviderReturn'),
    ]
    selected=os.environ.get('ANDROID_FILE_TASK_PRODUCT_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{case[0] for case in cases}:raise RuntimeError('Unknown task product mutation')
        cases=[case for case in cases if case[0] in selected]
    def restore():
        for p,value in originals.items():
            if MARK.encode() in p.read_bytes():p.write_bytes(value)
        if any(hashlib.sha256(p.read_bytes()).digest()!=hashlib.sha256(value).digest() for p,value in originals.items()):
            raise RuntimeError('Task product restoration failed')
    try:
        for name,path,before,after,method in cases:
            text=originals[path].decode();assert text.count(before)==1,(name,text.count(before))
            path.write_text(text.replace(before,after)+'\n// '+MARK+'\n')
            label='file-task-product-'+name
            build(label);test(label,method,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(label+'-restored');test(label+'-restored',method,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    build('file-task-product-final')
    test('file-task-product-final','com.hanxi.ghosterm.FileTasksProductTest,com.hanxi.ghosterm.RemoteFileTasksProductTest,com.hanxi.ghosterm.SafFileTasksProductTest',True)
    print(f'ANDROID FILE TASK PRODUCT MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_FILE_TASK_PRODUCT_MUTATE=1 with test-android-ssh.sh and RemoteFileTasksProductTest')
