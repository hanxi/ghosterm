#!/usr/bin/env python3
"""Real task summary navigation, queue ownership and unknown-state UI guards."""
import hashlib,os
MARK='GT_ANDROID_TASK_DOCK_MUTATION'
def execute(root,build,test):
    dock=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTaskDock.kt'
    main=root/'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    originals={p:p.read_bytes() for p in (dock,main)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual task dock mutation')
    navigation='hostHeaderOnlyHasSettingsAndBottomEntryReturnsToTheSameFileLocation'
    real='actualCopyChangesDockFromConfirmationToFinishedThenImmutableHistory'
    cases=[
        ('open',dock,'Card(onClick = model::show,','Card(onClick = {},',navigation),
        ('active-owner',dock,'it.active && it.phase != FileTaskPhase.QUEUED','it.active','newerQueuedRequestDoesNotHideTheTaskWaitingForConfirmation'),
        ('unknown',dock,'task?.let(::taskStatus)','task?.let { if(it.result?.state == FileCopyState.UNKNOWN) "全部复制完成" else taskStatus(it) }','unknownCurrentResultIsNeverPresentedAsSuccessfulCopy'),
        ('files',main,'taskBar={ FileTaskDock(fileTasks) }','taskBar={}',navigation),
        ('home',main,'                FileTaskDock(fileTasks)\n','                Unit\n',navigation),
        ('header',main,'Page.HOSTS -> {\n','Page.HOSTS -> {\n                        TextButton(onClick=fileTasks::show) { Text("任务") }\n',navigation),
        ('history-open',dock,'Card(onClick = model::show,','Card(onClick = { if(model.tasks.isNotEmpty()) model.show() },',real),
        ('list-space',dock,'heightIn(min = 64.dp)','heightIn(min = 500.dp)','copiedSourceAndTaskDockLeaveAVisibleFileRowAndPasteOnNarrowScreens'),
    ]
    selected=os.environ.get('ANDROID_TASK_DOCK_CASES','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown task dock mutation')
        cases=[c for c in cases if c[0] in selected]
    try:
        for name,path,old,new,method in cases:
            text=originals[path].decode();assert text.count(old)==1,(name,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');label='task-dock-'+name
            selection='com.hanxi.ghosterm.FileTaskDockTest#'+method
            build(label);output=test(label,selection,False)
            if 'AssertionError' not in output and 'ComparisonFailure' not in output:raise RuntimeError('Expected compiled normal assertion red')
            test(label+'-control','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True)
            path.write_bytes(originals[path]);build(label+'-restored');test(label+'-restored',selection,True)
            assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(originals[path]).digest()
            print('PASS '+label+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(path.read_bytes()!=data for path,data in originals.items()):raise RuntimeError('Task dock restoration failed')
    print(f'ANDROID TASK DOCK MUTATIONS PASS: {len(cases)} groups',flush=True)
if __name__=='__main__':raise SystemExit('Use ANDROID_TASK_DOCK_MUTATE=1 with test-android-ssh.sh and FileTaskDockTest')
