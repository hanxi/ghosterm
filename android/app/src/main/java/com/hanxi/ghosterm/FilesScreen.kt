package com.hanxi.ghosterm

import android.text.format.Formatter
import androidx.activity.compose.BackHandler
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.domain.*

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FilesScreen(model: FilesViewModel, pickFolder: () -> Unit, remoteHosts: List<String> = emptyList(),
    remoteError: String? = null, onPaste: () -> Unit = {}, showTasks: () -> Unit = {},
    taskBar: @Composable () -> Unit = { TextButton(onClick = showTasks) { Text("文件任务") } },
    onHosts: (() -> Unit)? = null, onTerminal: (() -> Unit)? = null, summaryVisible: Boolean = true,
    browseDeviceStorage: () -> Unit = { model.choose(FileEndpoint.DEVICE) },
    connectRemote: (String) -> Unit = {}) {
    var menu by remember { mutableStateOf(false) }
    val compact = LocalConfiguration.current.screenHeightDp < 400 || LocalConfiguration.current.fontScale >= 1.5f
    val context=LocalContext.current
    BackHandler { if(menu) menu=false else model.back() }
    val lifecycle=LocalLifecycleOwner.current.lifecycle
    DisposableEffect(model,lifecycle) {
        val observer=LifecycleEventObserver { _,event -> if(event==Lifecycle.Event.ON_STOP) model.externalPresenterStopped() }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer);model.externalPresenterStopped() }
    }
    val ready=model.externalReady
    LaunchedEffect(ready) { ready?.let {
        if(lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) model.launchExternal(it) { copy ->
            context.startActivity(externalPreviewIntent(copy))
        } else model.externalPresenterStopped()
    } }
    val pasteEnabled=!model.awaitingRemote && !model.loading && model.error==null && model.location.endpoint!=FileEndpoint.DEVICE
    val preview=model.preview
    val pathLabel=if(model.location.endpoint.remoteAlias!=null) model.location.directoryId else model.crumbs.joinToString("/") { it.label }
    Scaffold(topBar={
        GhostermTopBar(title={ Column {
            Text(if(preview!=null) preview.entry.name else if(model.selecting) "已选 ${model.selected.size} 项" else "文件",maxLines=1,overflow=TextOverflow.Ellipsis)
            if(preview==null && !model.selecting && !compact) Text(if(model.awaitingRemote) "选择远程主机" else "${if(model.remoteScope) "远端" else "本机"} · ${model.location.endpoint.label}",
                modifier=Modifier.testTag("file-endpoint-title"),style=MaterialTheme.typography.labelSmall,color=MaterialTheme.colorScheme.onSurfaceVariant,maxLines=1,overflow=TextOverflow.Ellipsis)
        } },navigationIcon={ GhostermIconButton(if(model.selecting) UiIcon.Close else UiIcon.Back,if(model.selecting) "取消选择" else "返回",onClick=model::back) },actions={
            if(preview==null) {
                if(model.selecting && !compact) TextButton(onClick={ if(model.selected.isNotEmpty() && model.selected.size==model.visibleEntries.size) model.clearChecked() else model.all() }) {
                    Text(if(model.selected.isNotEmpty() && model.selected.size==model.visibleEntries.size) "取消全选" else "全选")
                }
                else if(!model.selecting) TextButton(onClick=model::selectMode,enabled=!model.awaitingRemote && !model.loading && model.error==null) { Text("选择") }
            }
            Box {
                GhostermIconButton(UiIcon.More,"更多",onClick={ menu=true })
                DropdownMenu(expanded=menu,onDismissRequest={ menu=false }) {
                    if(preview==null) {
                        DropdownMenuItem(text={ Text("选择位置") },leadingIcon={ GhostermIcon(UiIcon.Server) },onClick={ menu=false;model.chooseLocations() })
                        if(compact && onHosts!=null) DropdownMenuItem(text={ Text("主机列表") },onClick={ menu=false;onHosts() })
                        if(compact && onTerminal!=null) DropdownMenuItem(text={ Text("回到终端") },onClick={ menu=false;onTerminal() })
                        if(compact) DropdownMenuItem(text={ Text("文件任务") },leadingIcon={ GhostermIcon(UiIcon.Clock) },
                            modifier=Modifier.testTag("files-task-history"),onClick={ menu=false;showTasks() })
                        if(compact && model.selecting) {
                            DropdownMenuItem(text={ Text("全选") },onClick={ menu=false;model.all() })

                            if(model.clipboard!=null) DropdownMenuItem(text={ Text("查看清单") },onClick={ menu=false;model.showClipboard() })
                        }
                        if(model.selecting) DropdownMenuItem(text={ Text("清除选择") },onClick={ menu=false;model.clearChecked() })
                        if(model.location.endpoint.remoteAlias!=null)
                            DropdownMenuItem(text={ Text("断开文件连接") },onClick={ menu=false;model.disconnectRemote() })
                        DropdownMenuItem(text={ Text("刷新") },onClick={ menu=false;model.reload() })
                        DropdownMenuItem(text={ Text(if(model.showHidden) "隐藏点文件" else "显示隐藏文件") },
                            enabled=model.preferencesAvailable && !model.hiddenSaving,onClick={ menu=false;model.toggleHidden() })
                    }
                    DropdownMenuItem(text={ Text("清理预览副本") },enabled=!model.externalClearing,
                        onClick={ menu=false;model.requestExternalClear() },modifier=Modifier.testTag("file-preview-cache-clear"))
                }
            }
        })
    }, bottomBar = { Column(Modifier.fillMaxWidth().navigationBarsPadding()) {
        taskBar()
        if(!compact && onHosts!=null) Row(Modifier.fillMaxWidth().heightIn(min=52.dp).padding(horizontal=12.dp),verticalAlignment=Alignment.CenterVertically) {
            GhostermIconButton(UiIcon.Server,"返回主机列表",onClick=onHosts)
            TextButton(onClick={ onTerminal?.invoke() },enabled=onTerminal!=null,modifier=Modifier.weight(1f).heightIn(min=48.dp).background(MaterialTheme.colorScheme.surfaceContainerHigh,MaterialTheme.shapes.small).testTag("files-return-terminal")) {
                GhostermIcon(UiIcon.Terminal);Spacer(Modifier.width(8.dp));Text("回到终端");Spacer(Modifier.width(8.dp));GhostermIcon(UiIcon.Arrow)
            }
            GhostermIconButton(UiIcon.Clock,"文件任务",onClick=showTasks,modifier=Modifier.testTag(if(summaryVisible) "files-task-nav" else "file-task-dock"))
        }
    } }) { padding ->
        if(preview!=null) {
            Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding).verticalScroll(rememberScrollState()).padding(16.dp)) {
                Text("只读预览 · UTF-8 文本",style=MaterialTheme.typography.labelMedium)
                if(preview.entry.kind==FileKind.FILE) {
                    TextButton(onClick=model::openExternal,
                        enabled=!model.externalPreparing && (preview.text!=null || preview.error!=null),
                        modifier=Modifier.testTag("file-open-external")) { Text(if(model.externalPreparing) "正在准备只读副本…" else "用其他应用打开") }
                    Text("只读副本最多 16 MiB，最多保留 24 小时；修改不会写回来源。",style=MaterialTheme.typography.bodySmall)
                }
                model.externalError?.let { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-external-error")) }
                model.externalCacheError?.let { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-preview-cache-error")) }
                PreviewCacheStatus(model)
                if(preview.error!=null) Text(preview.error,color=MaterialTheme.colorScheme.error,modifier=Modifier.padding(top=16.dp).testTag("file-preview-error"))
                else if(preview.text==null) LinearProgressIndicator(Modifier.fillMaxWidth().padding(top=16.dp))
                else SelectionContainer(Modifier.fillMaxWidth()) {
                    Text(preview.text, fontFamily=FontFamily.Monospace,modifier=Modifier.padding(vertical=16.dp).testTag("file-preview"))
                }
            }
        } else Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding)) {
            if(compact) Row(Modifier.fillMaxWidth().height(48.dp).padding(horizontal=8.dp), verticalAlignment=Alignment.CenterVertically) {
                FileScopeTabs(model,Modifier.width(144.dp),compact=true)
                TextButton(onClick=model::chooseLocations,enabled=!model.selecting,contentPadding=PaddingValues(horizontal=8.dp),
                    modifier=Modifier.weight(1f).testTag("file-location")) {
                    Text(if(model.awaitingRemote) "选择远程主机" else "${if(model.remoteScope) "远端" else "本机"} · "+
                        pathLabel,
                        Modifier.testTag("file-breadcrumb"),style=MaterialTheme.typography.bodySmall,maxLines=1,overflow=TextOverflow.Ellipsis)
                }
            } else {
            Row(Modifier.fillMaxWidth().heightIn(min=56.dp).padding(horizontal=12.dp),verticalAlignment=Alignment.CenterVertically) {
                FileScopeTabs(model,Modifier.weight(1f))
                GhostermIconButton(UiIcon.Server,"选择远端主机",onClick=model::chooseLocations,enabled=!model.selecting)
            }
            Row(Modifier.fillMaxWidth().heightIn(min=56.dp).padding(horizontal=12.dp),verticalAlignment=Alignment.CenterVertically) {
                GhostermIconButton(UiIcon.Back,"上一级目录",onClick=model::back,enabled=model.crumbs.size>1 && !model.selecting && !model.awaitingRemote)
                TextButton(onClick=model::chooseLocations,enabled=!model.selecting,modifier=Modifier.weight(1f).testTag("file-location")) {
                    Text(if(model.awaitingRemote) "选择远程主机" else pathLabel,
                        Modifier.fillMaxWidth().testTag("file-breadcrumb"),style=MaterialTheme.typography.bodySmall.copy(fontFamily=FontFamily.Monospace),maxLines=1,overflow=TextOverflow.Ellipsis)
                }
            }
            Row(Modifier.fillMaxWidth().heightIn(min=32.dp).padding(horizontal=18.dp),verticalAlignment=Alignment.CenterVertically) {
                Text(if(model.awaitingRemote) "尚未连接" else if(model.loading) "正在读取…" else if(model.error!=null) "读取失败" else "${model.visibleEntries.size} 个项目",
                    Modifier.weight(1f),style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                Text(if(model.selecting) "点按选择" else "点按打开 · 长按多选",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
            }
            }
            if(!model.remoteScope && !model.selecting) Column(Modifier.fillMaxWidth().padding(horizontal=16.dp)) {
                Row(Modifier.fillMaxWidth()) {
                    TextButton(onClick=browseDeviceStorage,enabled=!model.deviceAccessPending,
                        modifier=Modifier.weight(1f).testTag("file-device-storage")) { Text("浏览设备存储") }
                    TextButton(onClick=pickFolder,enabled=!model.pickerPending,
                        modifier=Modifier.weight(1f).testTag("file-add-device-folder")) { Text("授权文件夹") }
                }
                if(model.location.endpoint==FileEndpoint.APP) Text(
                    "当前为应用目录。浏览设备存储可访问 Download、Documents 等共享目录；也可单独授权文件夹。",
                    style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                if(model.location.endpoint==FileEndpoint.DEVICE) Text("可浏览、预览和复制来源；粘贴到手机请先「授权文件夹」选择目标。",
                    style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if(remoteError!=null && !model.choosingLocation)
                Text(remoteError,color=MaterialTheme.colorScheme.error,modifier=Modifier.padding(horizontal=16.dp).testTag("file-connect-error"))
            model.externalCacheError?.let { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-preview-cache-error")) }
            PreviewCacheStatus(model)
            model.preferenceError?.let { Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("file-preference-error")) }
            model.clipboardError?.let { Text(it,color=MaterialTheme.colorScheme.error,
                modifier=Modifier.padding(horizontal=16.dp).testTag("file-clipboard-error")) }
            if(model.awaitingRemote) Column(Modifier.weight(1f).fillMaxWidth().padding(16.dp).testTag("file-remote-empty"),
                horizontalAlignment=Alignment.CenterHorizontally,verticalArrangement=Arrangement.Center) {
                Text("选择远程主机",style=MaterialTheme.typography.titleMedium)
                Text(model.endpointError ?: "连接后浏览远程文件。本地目录和待粘贴清单会保留。",
                    style=MaterialTheme.typography.bodySmall)
                TextButton(onClick=model::chooseLocations,modifier=Modifier.heightIn(min=48.dp)) { Text("选择或连接主机") }
            } else {
            when {
                model.loading -> LinearProgressIndicator(Modifier.fillMaxWidth())
                model.error!=null -> Column(Modifier.padding(16.dp)) {
                    Text(model.error!!,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-list-error"))
                    if(model.location.endpoint.remoteAlias!=null)
                        TextButton(onClick={ connectRemote(model.location.endpoint.remoteAlias!!) }) { Text("重新连接") }
                    else TextButton(onClick=model::reload) { Text("重试") }
                    TextButton(onClick=model::chooseLocations) { Text("更换位置") }
                }
                model.visibleEntries.isEmpty() -> Text("此文件夹没有可见项目",Modifier.padding(24.dp).testTag("file-list-empty"))
            }
            LazyColumn(Modifier.weight(1f).fillMaxWidth().testTag("file-list")) {
                items(model.visibleEntries,key={ it.id }) { entry ->
                    val checked=entry.id in model.selected
                    val detail=when(entry.kind) {
                        FileKind.DIRECTORY -> "文件夹"
                        FileKind.LINK -> "符号链接"
                        FileKind.OTHER -> "特殊或未知类型"
                        FileKind.FILE -> entry.size?.let { if(it<=Long.MAX_VALUE.toULong()) Formatter.formatShortFileSize(context,it.toLong()) else "$it 字节" } ?: "大小未知"
                    }
                    val entryModifier=Modifier.fillMaxWidth().testTag("file-entry:${entry.name}")
                        .semantics { selected=checked }.combinedClickable(role=if(model.selecting) Role.Checkbox else Role.Button,
                            onClick={ model.tap(entry) },onLongClick={ model.longPress(entry) },onLongClickLabel="选择此项目")
                    var itemMenu by remember(entry.id) { mutableStateOf(false) }
                    if(compact) Row(entryModifier.height(48.dp).padding(horizontal=12.dp),verticalAlignment=Alignment.CenterVertically,
                        horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                        if(model.selecting) Checkbox(checked=checked,onCheckedChange=null) else GhostermIcon(if(entry.kind==FileKind.DIRECTORY) UiIcon.Folder else UiIcon.File,tint=MaterialTheme.colorScheme.primary)
                        Text(entry.name,Modifier.weight(1f),style=MaterialTheme.typography.bodyMedium,maxLines=1,overflow=TextOverflow.Ellipsis)
                        Text(detail,Modifier.widthIn(max=120.dp),style=MaterialTheme.typography.bodySmall,maxLines=1,overflow=TextOverflow.Ellipsis)
                    } else Row(entryModifier.heightIn(min=64.dp).background(if(checked) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.background)
                        .padding(start=24.dp,end=18.dp),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(14.dp)) {
                        GhostermIcon(if(entry.kind==FileKind.DIRECTORY) UiIcon.Folder else UiIcon.File,tint=MaterialTheme.colorScheme.primary)
                        Column(Modifier.weight(1f).padding(vertical=10.dp),verticalArrangement=Arrangement.spacedBy(4.dp)) {
                            Text(entry.name,style=MaterialTheme.typography.titleSmall,maxLines=2,overflow=TextOverflow.Ellipsis)
                            Text(detail,style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        if(model.selecting) Checkbox(checked=checked,onCheckedChange=null) else Box {
                            GhostermIconButton(UiIcon.More,"${entry.name} 的操作",onClick={ itemMenu=true })
                            DropdownMenu(expanded=itemMenu,onDismissRequest={ itemMenu=false }) {
                                DropdownMenuItem(text={ Text("打开") },onClick={ itemMenu=false;model.tap(entry) })
                                DropdownMenuItem(text={ Text("复制") },onClick={ itemMenu=false;model.longPress(entry);model.copySelected() })
                                DropdownMenuItem(text={ Text("选择此项") },onClick={ itemMenu=false;model.longPress(entry) })
                            }
                        }
                    }
                    HorizontalDivider(modifier=if(compact) Modifier else Modifier.padding(horizontal=18.dp),color=MaterialTheme.colorScheme.outline)
                }
            }
            }
            if(compact && model.selecting) Row(Modifier.fillMaxWidth().height(48.dp).padding(horizontal=16.dp),verticalAlignment=Alignment.CenterVertically) {
                Text("已选 ${model.selected.size} 项"+(model.clipboard?.let { " · 清单 ${it.items.size} 项" } ?: ""),Modifier.weight(1f),maxLines=1,overflow=TextOverflow.Ellipsis)
                Button(onClick=model::copySelected,enabled=model.selected.isNotEmpty() && !model.loading && model.error==null,
                    contentPadding=PaddingValues(horizontal=12.dp),modifier=Modifier.height(48.dp).testTag("file-copy")) { Text("复制 ${model.selected.size} 项",maxLines=1) }
            } else if(model.selecting) Row(Modifier.fillMaxWidth().heightIn(min=56.dp).background(MaterialTheme.colorScheme.surfaceContainerHigh)
                .padding(horizontal=12.dp),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                Text("已选 ${model.selected.size} 项",Modifier.weight(1f),style=MaterialTheme.typography.bodyMedium)
                Button(onClick=model::copySelected,enabled=model.selected.isNotEmpty() && !model.loading && model.error==null,
                    modifier=Modifier.heightIn(min=48.dp).testTag("file-copy")) { GhostermIcon(UiIcon.Copy);Spacer(Modifier.width(6.dp));Text("复制") }
                TextButton(onClick=model::clearSelection) { Text("取消") }
            }
            if(!compact && model.clipboard==null && !model.selecting && !model.awaitingRemote)
                Text("长按进入多选，也可以点顶部「选择」。",Modifier.padding(horizontal=18.dp,vertical=12.dp),
                    style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
            model.clipboard?.let { snapshot ->
                if(compact && !model.selecting) ElevatedCard(Modifier.fillMaxWidth().padding(horizontal=12.dp,vertical=4.dp).testTag("file-clipboard")) {
                    Row(Modifier.fillMaxWidth().height(48.dp).testTag("file-clipboard-compact"),verticalAlignment=Alignment.CenterVertically) {
                        Box(Modifier.weight(1f).fillMaxHeight().clickable(role=Role.Button,onClick=model::showClipboard)
                            .testTag("file-clipboard-details").padding(horizontal=8.dp),contentAlignment=Alignment.CenterStart) {
                            Text(snapshot.items.take(3).joinToString("、") { it.name }+" · ${snapshot.items.size} 项 · "+clipboardSource(snapshot)+" / "+snapshot.sourcePathLabel,
                                maxLines=1,overflow=TextOverflow.Ellipsis,modifier=Modifier.testTag("file-clipboard-names"))
                        }
                        TextButton(onClick=onPaste,enabled=pasteEnabled,contentPadding=PaddingValues(horizontal=8.dp),
                            modifier=Modifier.height(48.dp).testTag("file-paste")) { Text("粘贴",maxLines=1) }
                        TextButton(onClick=model::requestClipboardClear,contentPadding=PaddingValues(horizontal=8.dp),modifier=Modifier.height(48.dp)) { Text("清空",maxLines=1) }
                    }
                } else if(!compact) OutlinedCard(Modifier.fillMaxWidth().padding(12.dp).testTag("file-clipboard"),
                    shape=MaterialTheme.shapes.medium,border=BorderStroke(1.dp,MaterialTheme.colorScheme.outline),
                    colors=CardDefaults.elevatedCardColors(containerColor=MaterialTheme.colorScheme.surfaceContainerHigh),
                    elevation=CardDefaults.elevatedCardElevation(defaultElevation=0.dp)) {
                    Column(Modifier.padding(horizontal=12.dp,vertical=8.dp)) {
                        Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically) {
                            GhostermIcon(UiIcon.Copy,tint=MaterialTheme.colorScheme.primary);Spacer(Modifier.width(8.dp))
                            Text("待粘贴 · ${snapshot.items.size} 项",Modifier.weight(1f),style=MaterialTheme.typography.titleSmall)
                            GhostermIconButton(UiIcon.Close,"清空",onClick=model::requestClipboardClear)
                        }
                        Column(Modifier.fillMaxWidth().heightIn(min=70.dp).clickable(role=Role.Button,onClick=model::showClipboard)
                            .testTag("file-clipboard-details")) {
                            Text(snapshot.items.take(3).joinToString("、") { it.name }+if(snapshot.items.size>3) "…" else "",
                                maxLines=1,overflow=TextOverflow.Ellipsis,style=MaterialTheme.typography.titleSmall,modifier=Modifier.testTag("file-clipboard-names"))
                            Text(clipboardSource(snapshot),maxLines=1,overflow=TextOverflow.Ellipsis,
                                modifier=Modifier.testTag("file-clipboard-source"),style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                            Text(snapshot.sourcePathLabel,maxLines=1,overflow=TextOverflow.Ellipsis,
                                modifier=Modifier.testTag("file-clipboard-path"),style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                            Row(Modifier.fillMaxWidth(),horizontalArrangement=Arrangement.End,verticalAlignment=Alignment.CenterVertically) {
                                Text("查看清单",style=MaterialTheme.typography.labelSmall,color=MaterialTheme.colorScheme.primary)
                                GhostermIcon(UiIcon.Arrow,tint=MaterialTheme.colorScheme.primary,modifier=Modifier.size(16.dp))
                            }
                        }
                        if(!model.selecting) Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically) {
                            Text(if(model.awaitingRemote) "请选择远程主机" else "粘贴到当前目录",Modifier.weight(1f),style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                            Button(onClick=onPaste,enabled=pasteEnabled,
                                modifier=Modifier.heightIn(min=48.dp).testTag("file-paste")) {
                                GhostermIcon(UiIcon.Paste);Spacer(Modifier.width(8.dp));Text("粘贴")
                            }
                        }
                    }
                }
            }
        }
    }
    if(model.choosingLocation) ModalBottomSheet(dragHandle={ GhostermSheetHandle() },onDismissRequest=model::dismissLocations) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp)) {
            Text("文件位置",style=MaterialTheme.typography.titleLarge)
            model.endpointError?.let { Text(it,color=MaterialTheme.colorScheme.error) }
            remoteError?.let { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-connect-error")) }
            model.endpoints.forEach { endpoint -> TextButton(onClick={ if(endpoint==FileEndpoint.DEVICE) browseDeviceStorage() else model.choose(endpoint) },modifier=Modifier.fillMaxWidth()) { Text(endpoint.label) } }
            if(remoteHosts.isNotEmpty()) Text("远端主机",style=MaterialTheme.typography.titleMedium)
            remoteHosts.forEach { alias -> TextButton(onClick={ connectRemote(alias) },modifier=Modifier.fillMaxWidth().testTag("file-connect:$alias")) { Text("连接 $alias") } }
            OutlinedButton(onClick=pickFolder,enabled=!model.pickerPending,modifier=Modifier.fillMaxWidth()) { Text("选择设备文件夹") }
            Text("通过系统选择器授权文件夹，授权后可在此切换。Android 11 及以上不允许选择存储根目录、Download 根目录和 Android/data、Android/obb；可选择允许的子文件夹。",
                style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
    if(model.externalClearConfirm) AlertDialog(onDismissRequest=model::dismissExternalClear,
        title={ Text("清理预览副本？") },
        text={ Text("释放临时预览占用的空间，原文件会保留。已打开或正在准备的副本会保留；其他应用尚未打开的旧链接可能失效。") },
        confirmButton={ TextButton(onClick=model::confirmExternalClear,modifier=Modifier.testTag("file-preview-cache-clear-confirm")) { Text("确认清理") } },
        dismissButton={ TextButton(onClick=model::dismissExternalClear) { Text("取消") } })
    model.clipboardReplacement?.let { pending ->
        AlertDialog(onDismissRequest=model::dismissClipboardReplacement,title={ Text("替换复制清单？") },
            text={ Text("用 ${clipboardSource(pending.snapshot)} 的 ${pending.snapshot.items.size} 项替换现有清单。原文件不会改变。") },
            confirmButton={ TextButton(onClick=model::confirmClipboardReplacement) { Text("替换清单") } },
            dismissButton={ TextButton(onClick=model::dismissClipboardReplacement) { Text("保留原清单") } })
    }
    if(model.clipboardClearId!=null) AlertDialog(onDismissRequest=model::dismissClipboardClear,
        title={ Text("清空复制清单？") },text={ Text("只清空待粘贴的清单，原文件不会删除。") },
        confirmButton={ TextButton(onClick=model::confirmClipboardClear) { Text("确认清空") } },
        dismissButton={ TextButton(onClick=model::dismissClipboardClear) { Text("取消") } })
    val clipboard=model.clipboard
    if(model.clipboardDetails && clipboard!=null) ModalBottomSheet(dragHandle={ GhostermSheetHandle() },onDismissRequest=model::hideClipboard,
        sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true)) {
        Column(Modifier.fillMaxWidth().padding(16.dp)) {
            LazyColumn(Modifier.weight(1f,fill=false).testTag("file-clipboard-items")) {
                item { Text("复制清单 · ${clipboard.items.size} 项",style=MaterialTheme.typography.titleLarge) }
                item { Text(clipboardSource(clipboard),modifier=Modifier.testTag("file-clipboard-detail-source")) }
                item { Text(clipboard.sourcePathLabel,modifier=Modifier.testTag("file-clipboard-detail-path")) }
                item { Text("清单保存的是来源位置；文件内容会在粘贴时读取。",style=MaterialTheme.typography.bodySmall) }
                items(clipboard.items,key={ it.id }) { entry ->
                    ListItem(headlineContent={ Text(entry.name) },supportingContent={ Text(when(entry.kind) {
                        FileKind.DIRECTORY -> "文件夹";FileKind.LINK -> "符号链接 · 粘贴时需单独处理"
                        FileKind.OTHER -> "特殊或未知类型 · 粘贴时需单独处理";FileKind.FILE -> "文件"
                    }) },modifier=Modifier.testTag("file-clipboard-item:${entry.id}"))
                }
            }
            TextButton(onClick=model::hideClipboard,modifier=Modifier.fillMaxWidth().heightIn(min=48.dp)) { Text("返回文件") }
        }
    }
}

/** Navigation scopes share one clipboard; choosing a scope never starts a copy. */
@Composable
private fun FileScopeTabs(model: FilesViewModel, modifier: Modifier = Modifier, compact: Boolean=false) {
    Row(modifier.height(if(compact) 48.dp else 56.dp).clip(CircleShape)
        .background(MaterialTheme.colorScheme.surfaceContainerHigh)
        .selectableGroup().testTag("file-scope-tabs").padding(4.dp),
        verticalAlignment=Alignment.CenterVertically) {
        listOf(true to "远程",false to "本地").forEach { (remote,label) ->
            val active=model.remoteScope==remote
            Box(Modifier.weight(1f).fillMaxHeight().clip(CircleShape)
                .background(if(active) MaterialTheme.colorScheme.primaryContainer else androidx.compose.ui.graphics.Color.Transparent)
                .selectable(selected=active,enabled=!model.selecting,role=Role.Tab,onClick={ model.switchScope(remote) })
                .testTag(if(remote) "file-tab-remote" else "file-tab-local").padding(horizontal=4.dp),
                contentAlignment=Alignment.Center) {
                Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                    if(!compact) GhostermIcon(if(remote) UiIcon.Server else UiIcon.Home,tint=if(active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(label,style=MaterialTheme.typography.labelLarge,maxLines=1,overflow=TextOverflow.Ellipsis,
                        color=if(active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

@Composable
private fun PreviewCacheStatus(model: FilesViewModel) {
    val context=LocalContext.current
    if(model.externalClearing) Text("正在清理预览副本…",modifier=Modifier.testTag("file-preview-cache-clearing"))
    model.externalClearResult?.let { result ->
        Text("已清理 ${result.removed} 个副本，释放 ${Formatter.formatShortFileSize(context,result.bytes)}；保留 ${result.retained} 个正在使用或准备的副本。"+
            if(result.failed>0) "${result.failed} 个清理失败，请重试。" else "",
            modifier=Modifier.testTag("file-preview-cache-clear-result"),style=MaterialTheme.typography.bodySmall)
    }
    model.externalClearError?.let { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-preview-cache-clear-error")) }
}

private fun clipboardSource(snapshot: FileClipboardSnapshot): String = snapshot.source.endpoint.let { endpoint ->
    endpoint.remote?.let { "${it.alias} · ${it.user}@${it.host}:${it.port}" } ?: "本机 · ${endpoint.label}"
}

/** URI grants follow the receiving activity lifetime; no persistable or write grant. */
internal fun externalPreviewIntent(copy: ExternalPreviewCache.Ready): android.content.Intent =
    android.content.Intent(android.content.Intent.ACTION_VIEW).apply {
        setDataAndType(copy.uri,copy.mime)
        clipData=android.content.ClipData.newRawUri(copy.name,copy.uri)
        addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }
