#!/usr/bin/env python3
"""Real task summary navigation, queue ownership and unknown-state UI guards."""
import hashlib,os
MARK='GT_ANDROID_TASK_DOCK_MUTATION'
def execute(root,build,test):
    dock=root/'android/app/src/main/java/com/hanxi/ghosterm/FileTaskDock.kt'
    main=root/'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    files=root/'android/app/src/main/java/com/hanxi/ghosterm/FilesScreen.kt'
    originals={p:p.read_bytes() for p in (dock,main,files)}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual task dock mutation')
    navigation='emptyHomeHidesSummaryAndFileHistoryEntryReturnsToTheSameLocation'
    real='actualCopyChangesDockFromConfirmationToFinishedThenImmutableHistory'
    small='com.hanxi.ghosterm.FileSmallWindowTest#landscapeFontTwoKeepsFileRowsAndPreviewReadable'
    copied='com.hanxi.ghosterm.FileSmallWindowTest#copiedFilesStayVisibleThroughSelectionDetailsAndConfirmedPaste'
    cases=[
        ('open',dock,'Card(onClick = model::show,','Card(onClick = {},',real),
        ('active-owner',dock,'it.active && it.phase != FileTaskPhase.QUEUED','it.active','newerQueuedRequestDoesNotHideTheTaskWaitingForConfirmation'),
        ('unknown',dock,'task?.let(::taskStatus)','task?.let { if(it.result?.state == FileCopyState.UNKNOWN) "全部复制完成" else taskStatus(it) }','unknownCurrentResultIsNeverPresentedAsSuccessfulCopy'),
        ('files',main,'showTasks=fileTasks::show','showTasks={}',navigation),
        ('home',dock,'get()=tasks.isNotEmpty() || journalLoading || journalError!=null','get()=true',navigation),
        ('header',main,'Page.HOSTS -> {\n','Page.HOSTS -> {\n                        TextButton(onClick=fileTasks::show) { Text("任务") }\n',navigation),
        ('history-open',main,'"文件任务历史",onClick=fileTasks::show','"文件任务历史",onClick={}',real),
        ('list-space',files,'heightIn(min=52.dp)','heightIn(min=500.dp)','copiedSourceAndTaskDockLeaveAVisibleFileRowAndPasteOnNarrowScreens'),
        ('active-list-space',dock,'heightIn(min = 64.dp)','heightIn(min = 500.dp)','activeSummaryAndClipboardLeaveOneActualFileRow'),
    ]
    cases += [
        ('small-window',dock,'val compact = LocalConfiguration.current.screenHeightDp < 400','val compact = false',copied),
        ('preview-scroll',files,'.consumeWindowInsets(padding).verticalScroll(rememberScrollState()).padding(16.dp)',
            '.consumeWindowInsets(padding).padding(16.dp)',small),
    ]
    compact_state='com.hanxi.ghosterm.FileSmallWindowTest#compactDockPreservesConfirmationAndUnknownWithoutStartingWork'
    cases += [
        ('compact-open',dock,'Card(onClick = model::show,','Card(onClick = { if(!compact) model.show() },',compact_state),
        ('compact-unknown',dock,'Text("文件任务 · $status",','Text("文件任务 · ${if(uncertain) "全部复制完成" else status}",',compact_state),
    ]
    cases += [
        ('compact-header',files,'if(compact) Row(Modifier.fillMaxWidth().height(48.dp).padding(horizontal=8.dp)',
            'if(false) Row(Modifier.fillMaxWidth().height(48.dp).padding(horizontal=8.dp)',copied),
        ('compact-selection',files,'if(compact && model.selecting) Row(', 'if(false) Row(',copied),
        ('clipboard-height',files,'.height(48.dp).testTag("file-clipboard-compact")','.height(220.dp).testTag("file-clipboard-compact")',copied),
        ('clipboard-details',files,'.fillMaxHeight().clickable(role=Role.Button,onClick=model::showClipboard)','.fillMaxHeight().clickable(role=Role.Button,onClick={})',copied),
        ('clipboard-paste',files,'TextButton(onClick=onPaste,enabled=','TextButton(onClick={},enabled=',copied),
        ('clipboard-clear',files,'TextButton(onClick=model::requestClipboardClear,contentPadding=','TextButton(onClick={},contentPadding=',copied),
        ('clipboard-sheet',files,'LazyColumn(Modifier.weight(1f,fill=false).testTag("file-clipboard-items"))',
            'LazyColumn(Modifier.height(8.dp).testTag("file-clipboard-items"))',copied),
    ]
    cases += [('compact-entry',files,'if(compact) Row(entryModifier.height(48.dp)','if(false) Row(entryModifier.height(48.dp)',copied)]
    cases += [('header-identity',files,
        '"${if(model.remoteScope) "远端" else "本机"} · "+',
        '""+',small)]
    cases += [('portrait-font',files,
        'val compact = LocalConfiguration.current.screenHeightDp < 400 || LocalConfiguration.current.fontScale >= 1.5f',
        'val compact = LocalConfiguration.current.screenHeightDp < 400',
        'com.hanxi.ghosterm.FileSmallWindowTest#portraitFontTwoKeepsCopiedFilesAndConfirmedPaste')]
    cases += [('compact-title',files,'!model.selecting && !compact) Text(', '!model.selecting) Text(',copied)]
    selected=os.environ.get('ANDROID_TASK_DOCK_CASES','').split(',')
    if selected!=['']:
        if set(selected)-{c[0] for c in cases}:raise RuntimeError('Unknown task dock mutation')
        cases=[c for c in cases if c[0] in selected]
    try:
        for name,path,old,new,method in cases:
            text=originals[path].decode();assert text.count(old)==1,(name,text.count(old))
            path.write_text(text.replace(old,new)+'\n// '+MARK+'\n');label='task-dock-'+name
            selection=method if method.startswith('com.') else 'com.hanxi.ghosterm.FileTaskDockTest#'+method
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
