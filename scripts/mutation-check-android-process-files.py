#!/usr/bin/env python3
"""Process file ownership: real multi-Activity/SSH/SAF target, same-APK config control."""
import hashlib
import os

MARK='GT_ANDROID_PROCESS_FILES_MUTATION'

def execute(root,build,test):
    app=root/'android/app/src/main/java/com/hanxi/ghosterm'
    application=app/'GhostermApplication.kt'
    runtime=app/'FileWorkRuntime.kt'
    tasks=app/'FileTasksViewModel.kt'
    files=app/'FilesViewModel.kt'
    auth=app/'SessionsViewModel.kt'
    dialog=app/'SSHConnectionDialog.kt'
    main=app/'MainActivity.kt'
    originals={p:p.read_bytes() for p in (application,runtime,tasks,files,auth,dialog,main)}
    if any(MARK.encode() in value for value in originals.values()):raise RuntimeError('Residual process file mutation')
    prefix='com.hanxi.ghosterm.ProcessFileWorkTest#'
    shared=prefix+'confirmationAndResultOutliveOriginAndAreSharedWithoutSharingPageRoute'
    clipboard=prefix+'clipboardSurvivesFinishedActivityAndRejectsOtherPagesStaleDecisions'
    background=prefix+'backgroundWithdrawsAuthenticationPresenterWithoutCancellingPendingTask'
    login=prefix+'independentTaskAuthenticationTransfersToAnotherActivityAfterOriginFinishes'
    picker=prefix+'closedPresenterReleasesPickerAndItsLateResultCannotConsumeAnotherPagesRequest'
    cases=[
        ('application-owner',application,'val fileWork by lazy { FileWorkRuntime(this) }','val fileWork get() = FileWorkRuntime(this)',shared),
        ('page-owner',tasks,'internal val work: FileWorkRuntime = (application as? GhostermApplication)?.fileWork ?: FileWorkRuntime(application)',
         'internal val work: FileWorkRuntime = FileWorkRuntime(application)',shared),
        ('clipboard-owner',files,'private val clipboardStore = work.clipboard','private val clipboardStore = FileClipboardStore()',clipboard),
        ('activity-dispose',tasks,'override fun onCleared() { work.release(presenterId);super.onCleared() }',
         'override fun onCleared() { tasks.toList().forEach(::cancel);work.release(presenterId);super.onCleared() }',shared),
        ('background-cancel',tasks,'fun paused() = work.pause(presenterId)',
         'fun paused() { tasks.toList().forEach(::cancel);work.pause(presenterId) }',background),
        ('global-slot',tasks,'if(tasks.any { it.active && it.phase!=FileTaskPhase.QUEUED }) return','if(false) return',prefix+'closedOriginDoesNotCancelScanAndCancelledGlobalSlotWaitsForActualRelease'),
        ('completion-stream',files,'result?.let { copyCompleted(it) }','Unit',prefix+'completionRefreshesEveryLiveBrowserWithoutOwningItsViewModel'),
        ('authentication-dispose',runtime,'pause(id)\n        if(authenticationOwner.isInitialized()) authentication.releaseIdentityPresenter(id)',
         'pause(id)\n        if(authenticationOwner.isInitialized()) { authentication.connecting?.let { authentication.cancelConnection(it.id) };authentication.releaseIdentityPresenter(id) }',login),
        ('picker-result',auth,'fun completeIdentityImport(uri: Uri?, presenter: UUID? = null) {\n        if(pendingIdentityPresenter != presenter) return',
         'fun completeIdentityImport(uri: Uri?, presenter: UUID? = null) {',picker),
        ('picker-release',runtime,'if(authenticationOwner.isInitialized()) authentication.releaseIdentityPresenter(id)',
         'if(authenticationOwner.isInitialized()) Unit',picker),
        ('dialog-route',main,'SSHConnectionDialog(fileTasks.authentication,fileTasks.presenterId,',
         'SSHConnectionDialog(sessions,fileTasks.presenterId,',login),
        ('background-presenter',runtime,'fun pause(id: UUID) { resumed.remove(id);if(presenter==id) presenter=resumed.lastOrNull() }',
         'fun pause(id: UUID) { }',background),
        ('picker-callback',dialog,'model.completeIdentityImport(uri,presenter)','model.completeIdentityImport(uri)',
         prefix+'realTaskIdentityPickerSurvivesActivityRecreationAndAuthenticates'),
        ('replacement-ref',files,'if(clipboard?.id!=pending.expectedId)','if(false)',clipboard),
        ('clear-ref',files,'if(clipboard?.id!=expected)','if(false)',clipboard),
    ]
    selected=os.environ.get('ANDROID_PROCESS_FILES_MUTATIONS','').split(',')
    if selected!=['']:
        if set(selected)-{case[0] for case in cases}:raise RuntimeError('Unknown process file mutation')
        cases=[case for case in cases if case[0] in selected]
    def restore():
        for path,value in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(value)
        if any(hashlib.sha256(path.read_bytes()).digest()!=hashlib.sha256(value).digest() for path,value in originals.items()):
            raise RuntimeError('Process file mutation restoration failed')
    try:
        for name,path,before,after,method in cases:
            source=originals[path].decode();assert source.count(before)==1,(name,source.count(before))
            path.write_text(source.replace(before,after)+'\n// '+MARK+'\n')
            label='process-files-'+name
            build(label);test(label,method,False)
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            restore();build(label+'-restored');test(label+'-restored',method,True)
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    build('process-files-final');test('process-files-final','com.hanxi.ghosterm.ProcessFileWorkTest',True)
    print(f'ANDROID PROCESS FILES MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':raise SystemExit('Use ANDROID_PROCESS_FILES_MUTATE=1 with ProcessFileWorkTest and test-android-ssh.sh')
