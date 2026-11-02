package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.data.FileCleanupRecord
import com.hanxi.ghosterm.data.FileCleanupState
import com.hanxi.ghosterm.domain.FileKind

private fun cleanupState(state:FileCleanupState)=when(state) {
    FileCleanupState.NOT_ATTEMPTED->"未发起删除"
    FileCleanupState.ABSENT->"本次未找到（不是删除确认）"
    FileCleanupState.CHANGED->"条目已改变，保留当前位置"
    FileCleanupState.UNKNOWN->"结果不明，不会自动重试"
    FileCleanupState.REJECTED->"删除被明确拒绝"
    FileCleanupState.REMOVED->"已收到删除成功确认"
}

@Composable
fun FileCleanupDialogs(model:FileTasksViewModel) {
    model.cleanupReview?.let { review ->
        var confirmed by remember(review.attemptId) { mutableStateOf(false) }
        AlertDialog(onDismissRequest=model::dismissCleanupReview,title={ Text("清理当前位置？") },text={
            Column(Modifier.heightIn(max=420.dp).verticalScroll(rememberScrollState()),verticalArrangement=Arrangement.spacedBy(8.dp)) {
                Text(review.entry.name,style=MaterialTheme.typography.titleMedium)
                Text(review.parent.endpoint.label,style=MaterialTheme.typography.titleSmall)
                Text(review.entry.id,modifier=Modifier.testTag("file-cleanup-review-location"))
                Text(if(review.entry.kind==FileKind.DIRECTORY) "文件夹 · 只删除空目录，包含文件时保留" else
                    "文件 · ${review.entry.size} 字节")
                Text("修改时间：${java.text.DateFormat.getDateTimeInstance().format(java.util.Date(checkNotNull(review.entry.modifiedMillis)))}")
                Text("历史位置不证明文件归属。请核对当前文件；其他程序仍可能修改它。删除不能撤销，原复制结果保持不变。")
                Row(Modifier.fillMaxWidth().heightIn(min=48.dp).toggleable(confirmed,role=Role.Checkbox,onValueChange={ confirmed=it })
                    .testTag("file-cleanup-consent"),verticalAlignment=Alignment.CenterVertically) {
                    Checkbox(checked=confirmed,onCheckedChange=null)
                    Text("我已核对，确认删除当前条目",modifier=Modifier.weight(1f))
                }
            }
        },confirmButton={ TextButton(onClick=model::confirmCleanup,
            enabled=confirmed && !model.cleanupBusy && !model.busy && !model.contentReading,
            modifier=Modifier.testTag("file-cleanup-confirm")) { Text("确认删除") } },
            dismissButton={ TextButton(onClick=model::dismissCleanupReview,modifier=Modifier.testTag("file-cleanup-keep")) { Text("保留") } })
    }
    if(model.cleanupRecordsVisible) {
        var clearing by remember { mutableStateOf<FileCleanupRecord?>(null) }
        AlertDialog(onDismissRequest=model::hideCleanupRecords,title={ Text("清理记录") },text={
            Column(verticalArrangement=Arrangement.spacedBy(8.dp)) {
                Text("记录独立的清理操作，不改写原复制任务。结果不明时请重新核对当前位置，不会自动恢复删除。")
                model.cleanupError?.let { Text(it,color=MaterialTheme.colorScheme.error) }
                if(model.cleanupBusy) LinearProgressIndicator(Modifier.fillMaxWidth())
                if(model.cleanupRecords.isEmpty() && !model.cleanupBusy) Text("还没有清理记录")
                LazyColumn(Modifier.heightIn(max=420.dp).testTag("file-cleanup-records"),verticalArrangement=Arrangement.spacedBy(12.dp)) {
                    items(model.cleanupRecords,key={ it.id }) { record ->
                        Column(verticalArrangement=Arrangement.spacedBy(4.dp)) {
                            Text(record.endpoint+" · "+record.name,style=MaterialTheme.typography.titleSmall)
                            Text(record.entryId)
                            Text(cleanupState(record.state),modifier=Modifier.testTag("file-cleanup-state:${record.id}"))
                            Text(java.text.DateFormat.getDateTimeInstance().format(java.util.Date(record.updatedMillis)),style=MaterialTheme.typography.bodySmall)
                            Text("复制项目：${record.relativePath}"+(if(record.candidate) " · 暂存位置" else " · 目标位置"),style=MaterialTheme.typography.bodySmall)
                            record.error?.let { Text(it,color=MaterialTheme.colorScheme.error) }
                            TextButton(onClick={ clearing=record },enabled=!model.cleanupBusy && !model.contentReading,
                                modifier=Modifier.testTag("file-cleanup-clear:${record.id}")) { Text("清除这条记录") }
                            HorizontalDivider()
                        }
                    }
                }
            }
        },confirmButton={ TextButton(onClick=model::hideCleanupRecords) { Text("返回任务") } },
            dismissButton={ TextButton(onClick=model::reloadCleanupRecords,enabled=!model.cleanupBusy) { Text("刷新") } })
        clearing?.let { record ->
            AlertDialog(onDismissRequest={ clearing=null },title={ Text("清除清理记录？") },text={
                Text("只移除这条记录，实际文件和原复制任务保持不变。")
            },confirmButton={ TextButton(onClick={ clearing=null;model.removeCleanupRecord(record) },
                enabled=!model.cleanupBusy && !model.contentReading,modifier=Modifier.testTag("file-cleanup-clear-confirm")) { Text("清除记录") } },
                dismissButton={ TextButton(onClick={ clearing=null }) { Text("保留记录") } })
        }
    }
}
