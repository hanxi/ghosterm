package com.hanxi.ghosterm

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.toggleable
import androidx.compose.ui.semantics.Role
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.data.FileResidualCleanup

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FileTasksScreen(model: FileTasksViewModel) {
    if(model.retainedVisible) { RetainedFilesScreen(model);return }
    FileCleanupDialogs(model)
    BackHandler { when {
        model.cleanupReview!=null -> model.dismissCleanupReview()
        model.cleanupRecordsVisible -> model.hideCleanupRecords()
        model.cleanupBusy -> model.cancelCleanup()
        model.originId!=null -> model.dismissOrigin()
        model.recoveryHistory!=null -> model.dismissHistoryRecovery()
        model.retryHistory!=null -> model.dismissHistoryRetry()
        model.retryTask!=null -> model.dismissRetry()
        else -> model.hide()
    } }
    model.retryTask?.let { task -> model.retryPlan(task)?.let { retry ->
        AlertDialog(onDismissRequest=model::dismissRetry,title={ Text("重试未完成项目？") },text={
            Column(verticalArrangement=Arrangement.spacedBy(8.dp)) {
                Text("重试 ${retry.retryItems} 项，保留 ${retry.retained} 项已完成或跳过的结果。")
                if(retry.blocked>0) Text("另有 ${retry.blocked} 项需核对或清理实际位置；本次不会重试。")
                Text("重新检查来源、目标与同名项目；不会沿用上次的冲突选择。")
                retry.groups.take(4).forEach { Text("目标：${it.destinationLabel}") }
                if(retry.groups.size>4) Text("共 ${retry.groups.size} 个目标目录")
            }
        },confirmButton={ TextButton(onClick=model::confirmRetry,modifier=Modifier.testTag("file-task-retry-confirm")) { Text("检查并重试") } },
            dismissButton={ TextButton(onClick=model::dismissRetry) { Text("取消") } })
    } }

    model.retryHistory?.let { record -> model.historyRetryPlan(record)?.let { retry ->
        AlertDialog(onDismissRequest=model::dismissHistoryRetry,title={ Text("重新发起未完成项目？") },text={
            Column(verticalArrangement=Arrangement.spacedBy(8.dp)) {
                Text("重新检查 ${retry.retryItems} 项；保留 ${retry.retained} 项已完成或跳过的结果。")
                if(retry.blocked>0) Text("另有 ${retry.blocked} 项结果待核对，本次不会重试。")
                Text("将创建关联原记录的新任务，重新取得授权、检查来源与冲突。原任务记录保持不变。")
                retry.groups.take(4).forEach { Text("目标：${it.destinationLabel}") }
            }
        },confirmButton={ TextButton(onClick=model::confirmHistoryRetry,enabled=retry.groups.isNotEmpty(),modifier=Modifier.testTag("file-history-retry-confirm")) { Text("重新检查并入队") } },
            dismissButton={ TextButton(onClick=model::dismissHistoryRetry) { Text("保留记录") } })
    } }
    model.recoveryHistory?.let { record ->
        AlertDialog(onDismissRequest=model::dismissHistoryRecovery,title={ Text("重新选择结果不明的来源") },text={
            Column(verticalArrangement=Arrangement.spacedBy(8.dp)) {
                Text("原任务结果保持不变，已产生的文件会保留。只对你本次勾选的来源创建新任务，再重新扫描、认证和处理同名冲突。")
                Text("选择文件夹会重新检查整个来源树，其中可能包含已经复制的文件。请先核对原目标；保留两者可能产生重复副本。")
                Text("目标：${record.intent.destinationLabel}")
                if(model.recoveryEntries.isEmpty()) Text("没有可在此重新选择的来源，请返回任务查看结果。")
                LazyColumn(Modifier.fillMaxWidth().heightIn(max=280.dp)) {
                    items(model.recoveryEntries,key={ it.id }) { entry ->
                        Row(Modifier.fillMaxWidth().heightIn(min=48.dp)
                            .testTag("file-history-recovery-select:${entry.name}")
                            .toggleable(value=entry.id in model.recoverySelection,role=Role.Checkbox,
                                onValueChange={ model.selectRecovery(entry.id,it) }),verticalAlignment=Alignment.CenterVertically) {
                            Checkbox(checked=entry.id in model.recoverySelection,onCheckedChange=null)
                            Text(entry.name,Modifier.weight(1f))
                        }
                    }
                }
            }
        },confirmButton={ TextButton(onClick=model::confirmHistoryRecovery,
            enabled=model.recoverySelection.isNotEmpty(),modifier=Modifier.testTag("file-history-recovery-confirm")) { Text("检查所选来源") } },
            dismissButton={ TextButton(onClick=model::dismissHistoryRecovery) { Text("保留原状") } })
    }
    model.originId?.let { id ->
        val old=model.history.firstOrNull { it.intent.id==id }
        val live=model.tasks.firstOrNull { it.request.id==id }
        AlertDialog(onDismissRequest=model::dismissOrigin,title={ Text("原任务") },text={ Column(verticalArrangement=Arrangement.spacedBy(8.dp)) {
            when {
                old!=null -> {
                    Text("来源：${old.intent.source.endpoint.label} / ${old.intent.sourceLabel}")
                    Text("目标：${old.intent.destination.endpoint.label} / ${old.intent.destinationLabel}")
                    Text(if(old.interrupted) "已中断 · 结果待核对" else historyStatus(old))
                    Text("原记录已保留，不受重新发起的结果影响。")
                }
                live!=null -> {
                    Text("来源：${live.request.source.endpoint.label} / ${live.request.sourcePathLabel}")
                    Text("目标：${live.request.destination.endpoint.label} / ${live.request.destinationPathLabel}")
                    Text(taskStatus(live))
                }
                else -> Text("原任务记录已清除；这条任务保留了原任务的关联。")
            }
        } },confirmButton={ TextButton(onClick=model::dismissOrigin) { Text("返回任务") } })
    }

    val list=rememberLazyListState()
    val context=LocalContext.current
    LaunchedEffect(model.tasks.firstOrNull()?.request?.id) { list.scrollToItem(0) }
    Scaffold(topBar={ TopAppBar(title={ Text("文件任务") },navigationIcon={
        TextButton(onClick=model::hide) { Text("返回") }
    },actions={ TextButton(onClick=model::showCleanupRecords,modifier=Modifier.testTag("file-cleanup-records-open")) { Text("清理记录") };TextButton(onClick=model::showRetained,enabled=!model.busy && !model.journalLoading && model.inspectingId==null,modifier=Modifier.testTag("file-retained-open")) { Text("保留暂存") } }) }) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding).testTag("file-tasks"),
            state=list,contentPadding=PaddingValues(16.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
            model.retainedReleaseError?.let { item { Text(it,color=MaterialTheme.colorScheme.error) } }
            model.notice?.let { item { Text(it,color=MaterialTheme.colorScheme.error) } }
            model.historyRetryError?.let { item { Text(it,color=MaterialTheme.colorScheme.error) } }
            model.cleanupError?.let { item { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-cleanup-error")) } }
            model.cleanupNotice?.let { item { Text(it,modifier=Modifier.testTag("file-cleanup-notice")) } }
            if(model.cleanupBusy) item { Column { LinearProgressIndicator(Modifier.fillMaxWidth());Text("正在处理，请稍候…");TextButton(onClick=model::cancelCleanup,modifier=Modifier.testTag("file-cleanup-cancel")) { Text("取消") } } }
            model.inspectionError?.let { item { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-history-inspection-error")) } }
            if(model.background.enabled && (model.background.state!=FileBackgroundState.IDLE || model.background.error!=null)) item {
                Card(Modifier.fillMaxWidth().testTag("file-background-status")) {
                    Column(Modifier.padding(12.dp),verticalArrangement=Arrangement.spacedBy(8.dp)) {
                        Text(when(model.background.state) {
                            FileBackgroundState.STARTING -> "正在启动后台传输"
                            FileBackgroundState.ACTIVE -> "后台传输已启动"
                            FileBackgroundState.STOPPING -> "后台传输正在结束"
                            FileBackgroundState.TIMED_OUT -> "后台传输已达系统时限"
                            else -> "后台传输未启动"
                        })
                        model.background.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
                        if(model.background.state==FileBackgroundState.FAILED || model.background.state==FileBackgroundState.TIMED_OUT)
                            TextButton(onClick={ model.retryBackground() },enabled=model.busy) { Text("重试后台传输") }
                        if(!model.background.notificationsVisible) {
                            Text("通知已关闭，可在任务页查看状态和取消任务。")
                            TextButton(onClick={ context.startActivity(android.content.Intent(android.provider.Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                                .putExtra(android.provider.Settings.EXTRA_APP_PACKAGE,context.packageName)) }) { Text("通知设置") }
                        }
                    }
                }
            }
            if(model.contentReading) item {
                Text("正在核验文件；新复制请求将在核验与资源释放结束后开始检查。")
            }
            if(model.journalLoading) item { Text("正在读取任务记录…") }
            model.journalError?.let { error -> item {
                Column(verticalArrangement=Arrangement.spacedBy(8.dp),modifier=Modifier.testTag("file-journal-error")) {
                    Text(error,color=MaterialTheme.colorScheme.error)
                    Text("已停止新任务。请先取消等待中的任务，再重新读取记录；原记录不会自动清空。")
                    TextButton(onClick=model::reloadHistory,enabled=!model.busy && !model.journalLoading) { Text("重新读取记录") }
                }
            } }
            if(model.tasks.isEmpty() && model.history.isEmpty() && !model.journalLoading && model.journalError==null)
                item { Text("还没有复制任务。选择文件并复制后，到目标文件夹粘贴。") }
            items(model.tasks,key={ it.request.id }) { task ->
                ElevatedCard(Modifier.fillMaxWidth().testTag("file-task:${task.request.id}")) {
                    Column(Modifier.padding(16.dp),verticalArrangement=Arrangement.spacedBy(8.dp)) {
                        Text(taskStatus(task),style=MaterialTheme.typography.titleMedium,modifier=Modifier.testTag("file-task-status"))
                        task.request.retryOf?.let { id -> TextButton(onClick={ model.showOrigin(id) },modifier=Modifier.testTag("file-task-origin")) { Text("查看原任务") } }
                        if(task.phase==FileTaskPhase.QUEUED) {
                            Text("前方还有 ${model.queuedAhead(task) ?: 0} 个任务",modifier=Modifier.testTag("file-task-queue-position"))
                            Text("尚未检查来源与目标，轮到本任务后再确认复制。",style=MaterialTheme.typography.bodySmall)
                        }
                        Text("已选 ${task.request.items.size} 项",style=MaterialTheme.typography.labelLarge)
                        var sourceDetails by remember(task.request.id) { mutableStateOf(false) }
                        TextButton(onClick={ sourceDetails=true }) { Text("查看本次来源清单") }
                        if(sourceDetails) AlertDialog(onDismissRequest={ sourceDetails=false },title={ Text("本次来源 · ${task.request.items.size} 项") },
                            text={ LazyColumn(Modifier.fillMaxWidth().heightIn(max=480.dp)) {
                                items(task.request.items,key={ it.id }) { entry ->
                                    ListItem(headlineContent={ Text(entry.name) },supportingContent={ Text(when(entry.kind) {
                                        FileKind.DIRECTORY->"文件夹";FileKind.LINK->"符号链接";FileKind.OTHER->"特殊或未知类型"
                                        FileKind.FILE->entry.size?.let { "$it 字节" } ?: "大小未知"
                                    }) })
                                }
                            } },confirmButton={ TextButton(onClick={ sourceDetails=false }) { Text("返回任务") } })
                        Text("来源：${task.request.source.endpoint.label}\n${task.request.sourcePathLabel}")
                        Text("目标：${task.request.destination.endpoint.label}\n${task.request.destinationPathLabel}",modifier=Modifier.testTag("file-task-destination"))
                        if(task.request.source.endpoint.remote!=null && task.request.destination.endpoint.remote!=null)
                            Text("经本机中转",style=MaterialTheme.typography.bodySmall)
                        if(task.phase==FileTaskPhase.SCANNING || task.phase==FileTaskPhase.CANCELLING)
                            LinearProgressIndicator(Modifier.fillMaxWidth())
                        task.error?.let { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-task-error")) }
                        task.persistenceError?.let { Text(it,color=MaterialTheme.colorScheme.error,modifier=Modifier.testTag("file-task-record-error")) }
                        val decision=task.decisions
                        task.namespaceReview?.takeIf { task.phase==FileTaskPhase.CONFIRMING }?.let { review ->
                            Text("请核对两端位置",style=MaterialTheme.typography.titleSmall,
                                modifier=Modifier.testTag("file-task-namespace-review"))
                            Text("不同主机地址或用户也可能指向同一存储。应用无法确认两端的包含关系，请确认目标不在所选目录内。")
                            Text("继续后会重新检查来源与目标，再确认复制；原文件不会覆盖。此确认只用于本次请求。")
                            OutlinedButton(onClick={ model.confirmNamespace(task,review) },
                                modifier=Modifier.fillMaxWidth().heightIn(min=48.dp).testTag("file-task-namespace-confirm")) {
                                Text("已核对位置，继续本次检查")
                            }
                        }
                        if(task.phase==FileTaskPhase.CONFIRMING && decision!=null) {
                            Text("原文件保留；同名项目不会覆盖。",style=MaterialTheme.typography.bodySmall)
                            if(task.createCapability==FileCreateCapability.UNKNOWN)
                                Text("目标写入权限尚未确认，执行时会核验。",style=MaterialTheme.typography.bodySmall)
                            if(task.namespaceAcknowledged)
                                Text("两端存储关系以本次核对为准；复制经本机中转。",style=MaterialTheme.typography.bodySmall)
                            val question=decision.pending.firstOrNull()
                            if(question!=null) {
                                var all by remember(decision.reference) { mutableStateOf(false) }
                                Text(if(question.kind==FilePasteDecisionKind.CONFLICT) "目标已有同名项目" else "此条目暂不支持复制",
                                    modifier=Modifier.testTag("file-task-question"))
                                Text(question.source.relativePath)
                                Text("来源大小：${question.source.entry.size?.let { "$it 字节" } ?: "未知"}")
                                question.existing?.let { Text("目标大小：${it.size?.let { size -> "$size 字节" } ?: "未知"}") }
                                if(question.kind==FilePasteDecisionKind.CONFLICT) Row(verticalAlignment=Alignment.CenterVertically) {
                                    Checkbox(checked=all,onCheckedChange={ all=it },modifier=Modifier.testTag("file-task-all"))
                                    Text("应用到本次其余同名项目",Modifier.weight(1f))
                                }
                                if(question.kind==FilePasteDecisionKind.CONFLICT) OutlinedButton(onClick={
                                    model.choose(task,decision.reference,question.source.relativePath,FilePasteChoice.KEEP_BOTH,all)
                                },modifier=Modifier.fillMaxWidth().testTag("file-task-keep")) { Text("保留两者") }
                                TextButton(onClick={ model.choose(task,decision.reference,question.source.relativePath,FilePasteChoice.SKIP,all) },
                                    modifier=Modifier.fillMaxWidth().testTag("file-task-skip")) { Text("跳过此项") }
                            } else Button(onClick={ model.confirm(task,decision.reference) },
                                modifier=Modifier.fillMaxWidth().heightIn(min=48.dp).testTag("file-task-confirm")) {
                                    Text(if(task.request.items.all { decision.choices[it.name]==FilePasteChoice.SKIP }) "结束（全部跳过）" else "确认复制到此目录")
                                }
                        }
                        task.result?.let { result ->
                            val skips=result.items.count { it.state==FileCopyItemState.SKIPPED }
                            Text("已完成 ${result.completed} 项 · 已跳过 $skips 项 · 已提交 ${result.committedBytes} 字节")
                            if(task.phase==FileTaskPhase.RUNNING && !task.cancelled) {
                                when(result.pause) {
                                    FileCopyPause.RUNNING -> OutlinedButton(onClick={ model.pause(task) },modifier=Modifier.testTag("file-task-pause")) { Text("暂停") }
                                    FileCopyPause.REQUESTED -> {
                                        Text("正在暂停，等待当前操作到达安全位置",Modifier.testTag("file-task-pausing"))
                                        TextButton(onClick={ model.continueTask(task) },modifier=Modifier.testTag("file-task-resume")) { Text("继续") }
                                    }
                                    FileCopyPause.PAUSED -> {
                                        Text("已暂停",Modifier.testTag("file-task-paused"))
                                        OutlinedButton(onClick={ model.continueTask(task) },modifier=Modifier.testTag("file-task-resume")) { Text("继续") }
                                    }
                                }
                            }
                            result.activity?.takeIf { result.state!=FileCopyState.COMPLETE && result.state!=FileCopyState.WITH_SKIPS }?.let { activity ->
                                FileTaskByteProgress(activity,result.state==FileCopyState.RUNNING && task.active && result.pause==FileCopyPause.RUNNING,result.work)
                            }
                            result.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
                            model.retryPlan(task)?.let { retry ->
                                if(retry.blocked>0) Text("${retry.blocked} 项需核对实际位置后再操作",modifier=Modifier.testTag("file-task-retry-blocked"))
                                if(retry.groups.isNotEmpty()) OutlinedButton(onClick={ model.requestRetry(task) },modifier=Modifier.testTag("file-task-retry")) { Text("重试未完成项目") }
                            }
                            var details by remember(task.request.id) { mutableStateOf(false) }
                            TextButton(onClick={ details=!details },modifier=Modifier.testTag("file-task-details")) { Text(if(details) "收起结果" else "查看逐项结果") }
                            if(details) AlertDialog(onDismissRequest={ details=false },title={ Text("逐项结果") },
                                text={ LazyColumn(Modifier.fillMaxWidth().heightIn(max=480.dp)) {
                                    items(result.items,key={ it.source.relativePath }) { entry ->
                                        Column(Modifier.fillMaxWidth().padding(vertical=8.dp)) {
                                            Text("${entry.source.relativePath} · ${itemStatus(entry.state)}")
                                            (entry.documentUri ?: entry.createdId)?.let { Text("实际位置：$it") }
                                            entry.targetName?.let { Text("目标名称：$it") }
                                            entry.candidatePath?.let { Text("未确认的暂存位置：$it") }
                                            entry.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
                                            if(entry.coreStatus!=null && entry.coreStatus!=0) Text("错误码：${entry.coreStatus} / ${entry.remoteStatus ?: "未知"}")
                                        }
                                        HorizontalDivider()
                                    }
                                } },confirmButton={ TextButton(onClick={ details=false }) { Text("关闭结果") } })
                        }
                        if(task.active) TextButton(onClick={ model.cancel(task) },enabled=task.phase!=FileTaskPhase.CANCELLING,
                            modifier=Modifier.fillMaxWidth().testTag("file-task-cancel")) { Text("取消任务") }
                        else {
                            OutlinedButton(onClick={ model.archive(task) },enabled=model.canArchive(task),
                                modifier=Modifier.fillMaxWidth().heightIn(min=48.dp).testTag("file-task-archive")) { Text("移到历史记录") }
                            TextButton(onClick={ model.remove(task) },enabled=!task.recording && !model.journalLoading && !model.contentReading,
                                modifier=Modifier.fillMaxWidth().testTag("file-task-remove")) { Text("清除这条记录") }
                        }
                    }
                }
            }
            items(model.history,key={ "history:${it.intent.id}" }) { record ->
                FileHistoryCard(record,onRemove={ model.removeHistory(record) },canRemove=!model.busy && !model.journalLoading && model.inspectingId==null && !model.contentReading,
                    inspection=model.inspections[record.intent.id],inspecting=model.inspectingId==record.intent.id,
                    canInspect=!model.busy && !model.journalLoading && model.inspectingId==null && !model.contentReading,
                    onInspect={ model.inspectHistory(record) },onCancel=model::cancelInspection,
                    canRetry=!model.journalLoading && model.journalError==null && model.inspectingId==null,
                    onRetry={ model.requestHistoryRetry(record) },onRecover={ model.requestHistoryRecovery(record) },onOrigin={ record.intent.retryOf?.let(model::showOrigin) },
                    content=model.contentInspections,checkingContent=model.inspectingContent,
                    identities=model.identityInspections,checkingIdentity=model.inspectingIdentity,
                    onIdentity={ item,candidate -> model.inspectIdentity(record,item,candidate) },
                    onContent={ item,candidate -> model.inspectContent(record,item,candidate) },
                    onCleanup={ item,candidate -> model.requestCleanup(record,item,candidate) })
            }
        }
    }
}

@Composable
private fun FileTaskByteProgress(activity: FileCopyActivity, running: Boolean, work: FileCopyWork?) {
    var now by remember(activity.startedMillis) { mutableLongStateOf(activity.observedMillis) }
    LaunchedEffect(activity.startedMillis,running) {
        while(running) {
            now=android.os.SystemClock.elapsedRealtime()
            kotlinx.coroutines.delay(1000)
        }
    }
    Column(Modifier.fillMaxWidth().testTag("file-task-byte-progress"),verticalArrangement=Arrangement.spacedBy(4.dp)) {
        work?.let { total ->
            Text("整体传输 ${total.bytes} / ${total.totalBytes?.let { "$it 字节" } ?: "总量未知"}（含读取与写入）",Modifier.testTag("file-task-overall-bytes"))
            if(running) {
                val seconds=total.estimatedSeconds(maxOf(now,activity.observedMillis))
                Text(seconds?.let { "整体预计还需约 $it 秒" } ?: "整体剩余时间暂无法估算",Modifier.testTag("file-task-overall-estimate"))
            }
        }
        Text(activity.relativePath,maxLines=2,overflow=androidx.compose.ui.text.style.TextOverflow.Ellipsis)
        val stage=when(activity.stage) {
            FileCopyStage.READING -> "读取来源"
            FileCopyStage.WRITING -> "写入目标"
            FileCopyStage.VERIFYING -> "核对并提交"
        }
        Text(if(running) "正在$stage" else "最后进度：$stage",Modifier.testTag("file-task-byte-stage"))
        Text("${activity.bytes} / ${activity.totalBytes?.let { "$it 字节" } ?: "总大小未知"}",Modifier.testTag("file-task-byte-count"))
        if(running) {
            val total=activity.totalBytes
            if(activity.stage==FileCopyStage.VERIFYING || total==null || total==0uL) LinearProgressIndicator(Modifier.fillMaxWidth())
            else LinearProgressIndicator(progress={ (activity.bytes.toDouble()/total.toDouble()).coerceIn(0.0,1.0).toFloat() },modifier=Modifier.fillMaxWidth())
            activity.averageBytesPerSecond(maxOf(now,activity.observedMillis))?.let { rate ->
                Text("本阶段平均 ${String.format(java.util.Locale.ROOT,"%.0f",rate)} 字节/秒",Modifier.testTag("file-task-byte-speed"))
            }
            activity.estimatedSeconds(maxOf(now,activity.observedMillis))?.let { seconds ->
                Text("本阶段预计还需约 $seconds 秒",Modifier.testTag("file-task-byte-estimate"))
            }
            if(activity.stage!=FileCopyStage.READING) Text("写入字节不代表复制完成，正在等待核验结果",style=MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun FileHistoryCard(record: FileJournalRecord,onRemove: () -> Unit,canRemove: Boolean,
    inspection: FileHistoryInspection?, inspecting: Boolean, canInspect: Boolean, onInspect: () -> Unit, onCancel: () -> Unit, canRetry: Boolean, onRetry: () -> Unit, onRecover: () -> Unit, onOrigin: () -> Unit,
    content: Map<FileHistoryContentKey,FileHistoryContentResult>,checkingContent: FileHistoryContentKey?,
    identities: Map<FileHistoryContentKey,LocalFileIdentityResult>,checkingIdentity:FileHistoryContentKey?,
    onIdentity:(FileJournalItem,Boolean)->Unit,
    onContent: (FileJournalItem,Boolean) -> Unit, onCleanup:(FileJournalItem,Boolean)->Unit) {
    val observations=remember(inspection) { inspection?.observations?.groupBy { it.relativePath }.orEmpty() }
    ElevatedCard(Modifier.fillMaxWidth().testTag("file-history:${record.intent.id}")) {
        Column(Modifier.padding(16.dp),verticalArrangement=Arrangement.spacedBy(8.dp)) {
            Text(if(record.interrupted) "已中断 · 结果待核对" else "历史任务 · ${historyStatus(record)}",
                style=MaterialTheme.typography.titleMedium,modifier=Modifier.testTag("file-history-status"))
            Text("来源：${record.intent.source.endpoint.label}\n${record.intent.sourceLabel}")
            Text("目标：${record.intent.destination.endpoint.label}\n${record.intent.destinationLabel}")
            Text("${record.intent.selected.size} 项来源 · 已记录 ${record.items.count { it.value.state==FileCopyItemState.COMPLETE }} 项完成")
            Text(if(record.interrupted) "任务未自动继续。记录后的操作可能尚未确认，请核对目标和暂存位置后再发起新的复制。"
                else "这是已保存的任务结果；未重新检查文件。",style=MaterialTheme.typography.bodySmall)
            record.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
            if(record.intent.retryOf!=null) TextButton(onClick=onOrigin,modifier=Modifier.testTag("file-history-origin")) { Text("查看原任务") }
            TextButton(onClick=onRetry,enabled=canRetry,modifier=Modifier.testTag("file-history-retry")) { Text("重新检查未完成项目") }
            if(record.interrupted || record.outcome==FileCopyState.UNKNOWN || record.items.any {
                it.checkpoint!=FileCopyCheckpoint.RESULT || it.value.state in setOf(FileCopyItemState.RUNNING,FileCopyItemState.CREATED,FileCopyItemState.UNKNOWN)
            }) TextButton(onClick=onRecover,enabled=canRetry,modifier=Modifier.testTag("file-history-recovery")) { Text("重新选择结果不明的来源") }
            TextButton(onClick=onInspect,enabled=canInspect && record.items.isNotEmpty(),modifier=Modifier.testTag("file-history-inspect")) { Text("核对当前位置") }
            if(inspecting) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
                Text("正在只读核对目标与暂存位置…")
                TextButton(onClick=onCancel,modifier=Modifier.testTag("file-history-inspect-cancel")) { Text("取消核对") }
            }
            if(record.items.isEmpty()) Text("尚未记录逐项位置，请到目标文件夹检查。",style=MaterialTheme.typography.bodySmall)
            inspection?.let { observed ->
                Text("核对时间：${java.text.DateFormat.getDateTimeInstance().format(java.util.Date(observed.observedMillis))}")
                Text("存在 ${observed.observations.count { it.presence==FileHistoryPresence.PRESENT }} · 未找到 ${observed.observations.count { it.presence==FileHistoryPresence.ABSENT }} · 无法核对 ${observed.observations.count { it.presence==FileHistoryPresence.UNKNOWN }}",
                    modifier=Modifier.testTag("file-history-inspection-summary"))
                Text("仅为当时的位置观察；存在不代表复制成功或归属本任务，未找到不代表操作从未发生。原始结果保持不变。",style=MaterialTheme.typography.bodySmall)
            }
            var details by remember(record.intent.id) { mutableStateOf(false) }
            TextButton(onClick={ details=true },modifier=Modifier.testTag("file-history-details")) { Text("查看来源与结果") }
            if(details) AlertDialog(onDismissRequest={ details=false },title={ Text("任务记录") },text={
                LazyColumn(Modifier.fillMaxWidth().heightIn(max=480.dp)) {
                    items(record.intent.selected,key={ "source:${it.id}" }) { Text("来源：${it.name}") }
                    items(record.items,key={ it.value.source.relativePath }) { item ->
                        val value=item.value
                        Column(Modifier.fillMaxWidth().padding(vertical=8.dp)) {
                            Text("${value.source.relativePath} · ${itemStatus(value.state)}")
                            if(record.interrupted && value.state in setOf(FileCopyItemState.RUNNING,FileCopyItemState.CREATED,FileCopyItemState.UNKNOWN))
                                Text("此项最终结果待核对")
                            value.targetParent?.let { Text("目标目录：${it.directoryId}") }
                            value.targetName?.let { Text("目标名称：$it") }
                            if(value.targetParent?.endpoint==FileEndpoint.APP && value.source.entry.kind==FileKind.FILE) {
                                if(value.localIdentity==null) Text("这条记录没有本机文件创建身份参考",style=MaterialTheme.typography.bodySmall)
                                else for(candidate in if(value.candidatePath!=null) listOf(false,true) else listOf(false)) {
                                    val identityKey=FileHistoryContentKey(record.intent.id,value.source.relativePath,candidate)
                                    TextButton(onClick={ onIdentity(item,candidate) },enabled=canInspect,
                                        modifier=Modifier.testTag("file-history-identity:${value.source.relativePath}:$candidate")) {
                                        Text(if(candidate) "核对暂存文件身份" else "核对目标文件身份")
                                    }
                                    if(checkingIdentity==identityKey) {
                                        Text("正在核对文件身份…")
                                        TextButton(onClick=onCancel) { Text("取消身份核对") }
                                    }
                                    identities[identityKey]?.let { observed ->
                                        Text("身份核对时间：${java.text.DateFormat.getDateTimeInstance().format(java.util.Date(observed.observedMillis))}")
                                        Text(when(observed.state) {
                                            LocalFileIdentityState.SAME->"仍是本次创建的文件"
                                            LocalFileIdentityState.DIFFERENT->"条目或位置与创建记录不同"
                                            LocalFileIdentityState.MISSING->"本次未找到该文件"
                                            LocalFileIdentityState.NO_REFERENCE->"没有本机文件身份参考"
                                            LocalFileIdentityState.UNKNOWN->"文件身份无法确认"
                                        },modifier=Modifier.testTag("file-history-identity-result"))
                                        observed.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
                                        Text("文件身份与内容分开核对；不赋予删除权，原任务结果保持不变。",style=MaterialTheme.typography.bodySmall)
                                    }
                                }
                            }
                            (value.documentUri ?: value.createdId)?.let { Text("已记录位置：$it") }
                            value.candidatePath?.let { Text("未确认的暂存位置：$it") }
                            for(candidate in listOf(false,true).filter { FileResidualCleanup.eligible(record,item,it) }) {
                                TextButton(onClick={ details=false;onCleanup(item,candidate) },enabled=canInspect,
                                    modifier=Modifier.testTag("file-cleanup-review:${value.source.relativePath}:$candidate")) {
                                    Text(if(candidate) "查看并清理暂存位置" else "查看并清理目标位置")
                                }
                            }
                            value.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
                            if(value.sourceDigest==null && value.source.entry.kind==FileKind.FILE)
                                Text("未记录来源内容摘要，无法比较历史内容。",style=MaterialTheme.typography.bodySmall)
                            if(value.sourceDigest!=null) {
                                for(candidate in listOf(false,true).filter { !it || value.candidatePath!=null }) {
                                    val key=FileHistoryContentKey(record.intent.id,value.source.relativePath,candidate)
                                    TextButton(onClick={ onContent(item,candidate) },enabled=canInspect,
                                        modifier=Modifier.testTag("file-history-content:${value.source.relativePath}:$candidate")) {
                                        Text(if(candidate) "核验暂存文件内容" else "核验目标文件内容")
                                    }
                                    if(checkingContent==key) {
                                        Text("正在读取并核验内容（最多 256 MiB）…")
                                        TextButton(onClick=onCancel) { Text("取消内容核验") }
                                    }
                                    content[key]?.let { result ->
                                        Text("内容核验时间：${java.text.DateFormat.getDateTimeInstance().format(java.util.Date(result.observedMillis))}")
                                        Text(when(result.state) {
                                            FileHistoryContentState.MATCH->"内容与复制时的来源一致"
                                            FileHistoryContentState.DIFFERENT->"内容与复制时的来源不一致"
                                            FileHistoryContentState.MISSING->"本次读取未找到该文件"
                                            FileHistoryContentState.NO_REFERENCE->"没有来源内容摘要，无法比较"
                                            FileHistoryContentState.UNKNOWN->"内容无法确认"
                                        },modifier=Modifier.testTag("file-history-content-result"))
                                        result.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
                                        Text("仅核验本次读取的字节，不证明文件归属或复制已成功；原任务结果保持不变。",style=MaterialTheme.typography.bodySmall)
                                    }
                                }
                            }
                            observations[value.source.relativePath]?.forEach { observation ->
                                Text((if(observation.candidate) "暂存位置" else "目标位置")+"："+when(observation.presence) {
                                    FileHistoryPresence.PRESENT->"存在（仅位置观察）"
                                    FileHistoryPresence.ABSENT->"本次目录读取未找到"
                                    FileHistoryPresence.UNKNOWN->"无法核对"
                                })
                                observation.entry?.let { Text("当前类型：${when(it.kind) { FileKind.FILE->"文件";FileKind.DIRECTORY->"文件夹";FileKind.LINK->"符号链接";FileKind.OTHER->"其他" }} · ${it.size?.let { size -> "$size 字节" } ?: "大小未知"}") }
                                observation.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
                            }
                        }
                        HorizontalDivider()
                    }
                }
            },confirmButton={ TextButton(onClick={ details=false }) { Text("返回任务") } })
            var clearing by remember(record.intent.id) { mutableStateOf(false) }
            TextButton(onClick={ clearing=true },enabled=canRemove,modifier=Modifier.testTag("file-history-remove")) { Text("清除这条记录") }
            if(clearing) AlertDialog(onDismissRequest={ clearing=false },title={ Text("清除任务记录？") },
                text={ Text("只移除这条历史记录。目标文件和未确认的暂存文件都会保留。") },
                confirmButton={ TextButton(onClick={ clearing=false;onRemove() }) { Text("清除记录") } },
                dismissButton={ TextButton(onClick={ clearing=false }) { Text("保留记录") } })
        }
    }
}

private fun historyStatus(record: FileJournalRecord): String = when(record.outcome) {
    FileCopyState.COMPLETE->"全部复制完成";FileCopyState.WITH_SKIPS->"完成，部分跳过"
    FileCopyState.ALL_SKIPPED->"全部跳过";FileCopyState.CANCELLED->"已取消"
    FileCopyState.UNKNOWN->"结果未知";else->if(record.cancelled) "已取消" else "未完成"
}

internal fun taskStatus(task: FileTask): String = when(task.phase) {
    FileTaskPhase.QUEUED -> "排队等待"
    FileTaskPhase.SCANNING -> "正在检查来源与目标"
    FileTaskPhase.CONFIRMING -> "确认粘贴"
    FileTaskPhase.RUNNING -> when(task.result?.pause) {
        FileCopyPause.REQUESTED -> "正在暂停"
        FileCopyPause.PAUSED -> "已暂停"
        else -> "正在复制"
    }
    FileTaskPhase.CANCELLING -> "正在取消，等待资源释放"
    FileTaskPhase.FINISHED -> when(task.result?.state) {
        FileCopyState.COMPLETE -> "全部复制完成"
        FileCopyState.WITH_SKIPS -> "复制完成，部分项目已跳过"
        FileCopyState.ALL_SKIPPED -> "全部跳过，未复制任何项目"
        FileCopyState.CANCELLED -> "复制已取消"
        FileCopyState.FAILED -> "复制未完成"
        FileCopyState.UNKNOWN -> "复制结果未知，请核对实际位置"
        else -> if(task.cancelled) "复制已取消" else "复制未完成"
    }
}
private fun itemStatus(state: FileCopyItemState): String = when(state) {
    FileCopyItemState.NOT_ATTEMPTED->"未尝试";FileCopyItemState.RUNNING->"处理中"
    FileCopyItemState.COMPLETE->"已完成";FileCopyItemState.SKIPPED->"已跳过"
    FileCopyItemState.CONFLICT->"出现同名项目";FileCopyItemState.FAILED->"失败"
    FileCopyItemState.UNKNOWN->"结果未知";FileCopyItemState.CREATED->"已创建，尚未完成核验"
}
