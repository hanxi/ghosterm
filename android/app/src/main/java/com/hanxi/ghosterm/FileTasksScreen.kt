package com.hanxi.ghosterm

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.domain.*

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun FileTasksScreen(model: FileTasksViewModel) {
    BackHandler { model.hide() }
    val list=rememberLazyListState()
    LaunchedEffect(model.tasks.firstOrNull()?.request?.id) { list.scrollToItem(0) }
    Scaffold(topBar={ TopAppBar(title={ Text("文件任务") },navigationIcon={
        TextButton(onClick=model::hide) { Text("返回") }
    }) }) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding).testTag("file-tasks"),
            state=list,contentPadding=PaddingValues(16.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
            model.notice?.let { item { Text(it,color=MaterialTheme.colorScheme.error) } }
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
                        if(task.phase==FileTaskPhase.CONFIRMING && decision!=null) {
                            Text("原文件保留；同名项目不会覆盖。",style=MaterialTheme.typography.bodySmall)
                            if(task.createCapability==FileCreateCapability.UNKNOWN)
                                Text("目标写入权限尚未确认，执行时会核验。",style=MaterialTheme.typography.bodySmall)
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
                            result.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
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
                        else TextButton(onClick={ model.remove(task) },modifier=Modifier.fillMaxWidth()) { Text("清除这条记录") }
                    }
                }
            }
            items(model.history,key={ "history:${it.intent.id}" }) { record ->
                FileHistoryCard(record,onRemove={ model.removeHistory(record) },canRemove=!model.busy && !model.journalLoading)
            }
        }
    }
}

@Composable
private fun FileHistoryCard(record: FileJournalRecord,onRemove: () -> Unit,canRemove: Boolean) {
    ElevatedCard(Modifier.fillMaxWidth().testTag("file-history:${record.intent.id}")) {
        Column(Modifier.padding(16.dp),verticalArrangement=Arrangement.spacedBy(8.dp)) {
            Text(if(record.interrupted) "已中断 · 结果待核对" else "历史任务 · ${historyStatus(record)}",
                style=MaterialTheme.typography.titleMedium,modifier=Modifier.testTag("file-history-status"))
            Text("来源：${record.intent.source.endpoint.label}\n${record.intent.sourceLabel}")
            Text("目标：${record.intent.destination.endpoint.label}\n${record.intent.destinationLabel}")
            Text("${record.intent.selected.size} 项来源 · 已记录 ${record.items.count { it.value.state==FileCopyItemState.COMPLETE }} 项完成")
            Text(if(record.interrupted) "任务未自动继续。记录后的操作可能尚未确认，请核对目标和暂存位置后再发起新的复制。"
                else "这是上次运行保存的结果；未重新检查文件。",style=MaterialTheme.typography.bodySmall)
            record.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
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
                            (value.documentUri ?: value.createdId)?.let { Text("已记录位置：$it") }
                            value.candidatePath?.let { Text("未确认的暂存位置：$it") }
                            value.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
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
    FileTaskPhase.RUNNING -> "正在复制"
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
